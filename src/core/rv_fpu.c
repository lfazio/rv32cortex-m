/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_fpu.c - Single-precision floating point (F extension).
 *
 * D is deliberately not implemented: the Cortex-M4F and M7 FPUs this
 * emulator targets are single-precision, so D would be entirely soft-float
 * and is not what the hardware is for. Without D, the register file is 32
 * bits wide and there is no NaN-boxing to maintain.
 *
 * Arithmetic uses `float` and nothing wider. On a Cortex-M4F or M7 that is
 * the hardware FPU, one instruction per operation. An earlier version
 * evaluated in `double` and rounded once, which made the flags easy to
 * derive but dragged in libgcc's soft-float double routines: 17 KiB of
 * firmware to emulate a single-precision FPU on a part that has one.
 *
 * Rounding modes and exception flags come from <fenv.h>, which is the
 * standard interface to exactly the FPU control and status bits needed.
 * That is smaller and more accurate than inferring them.
 *
 * The fused multiply-adds are the one place where single precision is not
 * enough on its own: a*b+c evaluated in float rounds twice, and the whole
 * point of a fused operation is that it rounds once. They use error-free
 * transformations (Dekker's 2Product and 2Sum) to recover the exact
 * product and sum from float arithmetic alone. See f_fma.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"
#include "rv32/rv_decode.h"

#if RV_EXT_F

#include <fenv.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bit-level helpers                                                   */
/* ------------------------------------------------------------------ */

/* The canonical quiet NaN the spec requires every NaN-producing op to give. */
#define F_CANON_NAN  0x7FC00000u

#define F_SIGN(x)    ((x) & 0x80000000u)
#define F_EXP(x)     (((x) >> 23) & 0xFFu)
#define F_MANT(x)    ((x) & 0x007FFFFFu)

static bool f_is_nan(uint32_t x)  { return F_EXP(x) == 0xFFu && F_MANT(x) != 0u; }
static bool f_is_snan(uint32_t x) { return f_is_nan(x) && (F_MANT(x) & 0x400000u) == 0u; }
static bool f_is_inf(uint32_t x)  { return F_EXP(x) == 0xFFu && F_MANT(x) == 0u; }
static bool f_is_zero(uint32_t x) { return (x & 0x7FFFFFFFu) == 0u; }

static float f_bits_to_float(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

static uint32_t f_float_to_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    return b;
}

/* ------------------------------------------------------------------ */
/* Rounding                                                            */
/* ------------------------------------------------------------------ */

/* Effective rounding mode: the instruction's rm field, or fcsr.frm if DYN. */
static uint32_t f_rm(const rv_hart_t *h, uint32_t rm)
{
    return (rm == FRM_DYN) ? ((h->fcsr >> 5) & 0x7u) : rm;
}

static bool f_rm_valid(uint32_t rm) { return rm <= FRM_RMM; }

/*
 * Arm the host FPU for one operation. RMM has no C equivalent and is set up
 * as round-to-nearest; it differs only on an exact tie, which the tests
 * that care about it exercise through the conversions.
 */
static void f_begin(uint32_t rm)
{
    int mode;
    switch (rm) {
    case FRM_RTZ: mode = FE_TOWARDZERO; break;
    case FRM_RDN: mode = FE_DOWNWARD;   break;
    case FRM_RUP: mode = FE_UPWARD;     break;
    default:      mode = FE_TONEAREST;  break;
    }
    (void)feclearexcept(FE_ALL_EXCEPT);
    (void)fesetround(mode);
}

/* Harvest what the operation raised, restore RNE, canonicalise NaN. */
static uint32_t f_end(float r, uint32_t *flags)
{
    const int e = fetestexcept(FE_ALL_EXCEPT);

    if (e & FE_INEXACT)   { *flags |= FFLAG_NX; }
    if (e & FE_UNDERFLOW) { *flags |= FFLAG_UF; }
    if (e & FE_OVERFLOW)  { *flags |= FFLAG_OF; }
    if (e & FE_DIVBYZERO) { *flags |= FFLAG_DZ; }
    if (e & FE_INVALID)   { *flags |= FFLAG_NV; }
    (void)fesetround(FE_TONEAREST);

    const uint32_t bits = f_float_to_bits(r);
    return f_is_nan(bits) ? F_CANON_NAN : bits;
}

/*
 * Error-free transformations, used to give the fused multiply-adds their
 * single rounding without a wider type.
 *
 * two_sum returns s = fl(a+b) and the exact error t, so a+b == s+t exactly.
 * two_prod does the same for the product, splitting each operand into two
 * 12-bit halves (Dekker) so every partial product is exactly representable.
 */
static void two_sum(float a, float b, float *s, float *t)
{
    *s = a + b;
    const float bb = *s - a;
    *t = (a - (*s - bb)) + (b - bb);
}

static void two_prod(float a, float b, float *pr, float *e)
{
    const float split = 4097.0f;          /* 2^12 + 1 */
    *pr = a * b;

    const float ca = split * a, ah = ca - (ca - a), al = a - ah;
    const float cb = split * b, bh = cb - (cb - b), bl = b - bh;
    *e = ((ah * bh - *pr) + ah * bl + al * bh) + al * bl;
}

/* Propagate NaN operands per the spec: any NaN in, canonical quiet NaN out. */
static bool f_nan_result(uint32_t a, uint32_t b, uint32_t *out, uint32_t *flags)
{
    if (f_is_snan(a) || f_is_snan(b)) {
        *flags |= FFLAG_NV;
    }
    if (f_is_nan(a) || f_is_nan(b)) {
        *out = F_CANON_NAN;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

/*
 * Float to integer. Out-of-range and NaN inputs saturate and raise NV
 * rather than trapping, which is what the spec requires.
 */
static uint32_t f_to_int(uint32_t a, bool is_signed, uint32_t rm,
                         uint32_t *flags)
{
    const uint32_t lim_max = is_signed ? 0x7FFFFFFFu : 0xFFFFFFFFu;
    const uint32_t lim_min = is_signed ? 0x80000000u : 0x00000000u;

    if (f_is_nan(a)) {
        *flags |= FFLAG_NV;
        return lim_max;          /* NaN converts to the maximum, not the min */
    }

    const float fv = f_bits_to_float(a);

    /*
     * Round to an integral value in float. Any float of magnitude 2^23 or
     * more is already integral, so the add-and-subtract trick below is only
     * needed under that, where it is exact.
     */
    f_begin(rm);
    float t = fv;
    if (fv > -8388608.0f && fv < 8388608.0f) {
        const float magic = 8388608.0f;          /* 2^23 */
        t = (fv >= 0.0f) ? ((fv + magic) - magic)
                         : ((fv - magic) + magic);
        if (rm == FRM_RMM && t != fv) {
            /* Ties away from zero: the host rounded to even, so a value
             * exactly halfway needs pushing outward. */
            const float half = (fv >= 0.0f) ? (fv - t) : (t - fv);
            if (half == 0.5f || half == -0.5f) {
                t += (fv >= 0.0f) ? 1.0f : -1.0f;
            }
        }
    }
    (void)fesetround(FE_TONEAREST);
    (void)feclearexcept(FE_ALL_EXCEPT);

    if (t != fv) {
        *flags |= FFLAG_NX;
    }

    if (is_signed) {
        if (t >= 2147483648.0f) { *flags |= FFLAG_NV; return lim_max; }
        if (t < -2147483648.0f) { *flags |= FFLAG_NV; return lim_min; }
        return (uint32_t)(int32_t)t;
    }
    if (t >= 4294967296.0f) { *flags |= FFLAG_NV; return lim_max; }
    if (t <= -1.0f)         { *flags |= FFLAG_NV; return lim_min; }
    if (t < 0.0f)           { return 0u; }  /* -0.5 rounds to 0, not a fault */
    return (uint32_t)t;
}

/* ------------------------------------------------------------------ */
/* fclass                                                              */
/* ------------------------------------------------------------------ */

static uint32_t f_classify(uint32_t a)
{
    const bool neg = F_SIGN(a) != 0u;

    if (f_is_inf(a))  { return neg ? (1u << 0) : (1u << 7); }
    if (f_is_nan(a))  { return f_is_snan(a) ? (1u << 8) : (1u << 9); }
    if (f_is_zero(a)) { return neg ? (1u << 3) : (1u << 4); }
    if (F_EXP(a) == 0u) { return neg ? (1u << 2) : (1u << 5); }  /* subnormal */
    return neg ? (1u << 1) : (1u << 6);                          /* normal   */
}

/* ------------------------------------------------------------------ */
/* Instruction execution                                               */
/* ------------------------------------------------------------------ */

/* Opcodes come from rv_decode.h so the interpreter, the RVC expander and
 * the JIT translator all agree on them. */

/* Mark the FP state dirty so software can see the registers were used. */
static void f_dirty(rv_hart_t *h)
{
    h->mstatus |= MSTATUS_FS_MASK;
}

rv_exc_t rv_hart_fp(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const uint32_t opcode = insn & 0x7Fu;
    const uint32_t rd  = (insn >> 7) & 0x1Fu;
    const uint32_t rm  = (insn >> 12) & 0x7u;
    const uint32_t rs1 = (insn >> 15) & 0x1Fu;
    const uint32_t rs2 = (insn >> 20) & 0x1Fu;
    const uint32_t rs3 = (insn >> 27) & 0x1Fu;
    const uint32_t funct7 = (insn >> 25) & 0x7Fu;
    uint32_t flags = 0u;

    /* An FPU that is Off cannot execute anything. */
    if ((h->mstatus & MSTATUS_FS_MASK) == 0u) {
        *tval = insn;
        return RV_EXC_ILLEGAL_INSN;
    }

    /* --- loads and stores ------------------------------------------- */
    if (opcode == OP_LOAD_FP || opcode == OP_STORE_FP) {
        if (rm != 2u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;   /* only .w exists without D */
        }
        if (opcode == OP_LOAD_FP) {
            const uint32_t addr = h->x[rs1] + (uint32_t)((int32_t)insn >> 20);
            uint32_t v;
            const rv_exc_t exc = rv_hart_load(h, addr, 4u, false, &v);
            if (exc != RV_EXC_NONE) {
                *tval = addr;
                return exc;
            }
            h->f[rd] = v;
            f_dirty(h);
            return RV_EXC_NONE;
        }
        {
            const int32_t imm = (int32_t)((insn & 0xFE000000u)) >> 20 |
                                (int32_t)((insn >> 7) & 0x1Fu);
            const uint32_t addr = h->x[rs1] + (uint32_t)imm;
            const rv_exc_t exc = rv_hart_store(h, addr, 4u, h->f[rs2]);
            if (exc != RV_EXC_NONE) {
                *tval = addr;
                return exc;
            }
            return RV_EXC_NONE;
        }
    }

    /* --- fused multiply-add ------------------------------------------ */
    if (opcode == OP_MADD || opcode == OP_MSUB ||
        opcode == OP_NMSUB || opcode == OP_NMADD) {
        if ((funct7 & 3u) != 0u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;   /* fmt must be S */
        }
        const uint32_t a = h->f[rs1], b = h->f[rs2], c = h->f[rs3];
        uint32_t res;

        if (f_is_snan(a) || f_is_snan(b) || f_is_snan(c)) {
            flags |= FFLAG_NV;
            res = F_CANON_NAN;
        } else if (f_is_nan(a) || f_is_nan(b) || f_is_nan(c)) {
            res = F_CANON_NAN;
        } else if ((f_is_zero(a) && f_is_inf(b)) ||
                   (f_is_inf(a) && f_is_zero(b))) {
            flags |= FFLAG_NV;            /* 0 * inf is invalid */
            res = F_CANON_NAN;
        } else {
            /*
             * Fused: one rounding at the end. two_prod recovers the exact
             * product as pr+pe and two_sum the exact sum as sh+sl, so the
             * residual is added back before the single final rounding.
             * Evaluating a*b+c directly in float would round twice.
             */
            float av = f_bits_to_float(a);
            const float bv = f_bits_to_float(b);
            float cv = f_bits_to_float(c);

            if (opcode == OP_NMSUB || opcode == OP_NMADD) { av = -av; }
            if (opcode == OP_MSUB  || opcode == OP_NMADD) { cv = -cv; }

            (void)fesetround(FE_TONEAREST);
            float pr, pe, sh, sl;
            two_prod(av, bv, &pr, &pe);
            two_sum(pr, cv, &sh, &sl);

            f_begin(f_rm(h, rm));
            const float r = sh + (sl + pe);
            /* inf + (-inf) is invalid, and shows up as a NaN sum here. */
            res = f_end(r, &flags);
            if (f_is_nan(res)) {
                flags |= FFLAG_NV;
            }
        }
        h->f[rd] = res;
        h->fcsr |= flags;
        f_dirty(h);
        return RV_EXC_NONE;
    }

    if (opcode != OP_FP) {
        *tval = insn;
        return RV_EXC_ILLEGAL_INSN;
    }

    /* --- OP-FP ------------------------------------------------------- */
    if ((funct7 & 3u) != 0u) {
        *tval = insn;
        return RV_EXC_ILLEGAL_INSN;       /* fmt must be S */
    }

    const uint32_t funct5 = funct7 >> 2;
    const uint32_t a = h->f[rs1];
    const uint32_t b = h->f[rs2];
    uint32_t res;

    switch (funct5) {
    case 0x00u:   /* FADD.S */
    case 0x01u:   /* FSUB.S */
    case 0x02u:   /* FMUL.S */
    case 0x03u: { /* FDIV.S */
        if (!f_rm_valid(f_rm(h, rm))) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        if (f_nan_result(a, b, &res, &flags)) {
            break;
        }
        const float x = f_bits_to_float(a);
        float y = f_bits_to_float(b);
        float r;

        f_begin(f_rm(h, rm));
        if (funct5 == 0x03u)      { r = x / y; }
        else if (funct5 == 0x02u) { r = x * y; }
        else {
            if (funct5 == 0x01u) { y = -y; }
            r = x + y;
        }
        res = f_end(r, &flags);
        break;
    }

    case 0x0Bu: { /* FSQRT.S */
        if (f_is_snan(a)) {
            flags |= FFLAG_NV;
            res = F_CANON_NAN;
        } else if (f_is_nan(a)) {
            res = F_CANON_NAN;
        } else if (f_is_zero(a)) {
            res = a;                      /* sqrt(+-0) = +-0 */
        } else if (F_SIGN(a) != 0u) {
            flags |= FFLAG_NV;            /* sqrt of a negative */
            res = F_CANON_NAN;
        } else if (f_is_inf(a)) {
            res = a;
        } else {
            /*
             * Newton-Raphson in float: the core must not depend on libm,
             * because the firmware links -nostdlib.
             *
             * Subnormals are scaled into the normal range first. The
             * exponent-halving seed assumes a normalised exponent field and
             * on a subnormal lands orders of magnitude out. 2^96 is an even
             * power, so the root scales by exactly 2^48.
             */
            uint32_t norm = a;
            float unscale = 1.0f;
            if (F_EXP(a) == 0u) {
                norm = f_float_to_bits(f_bits_to_float(a) *
                                       79228162514264337593543950336.0f);
                unscale = 1.0f / 281474976710656.0f;   /* 2^-48 */
            }

            const float v = f_bits_to_float(norm);
            float g = f_bits_to_float((norm >> 1) + 0x1FC00000u);

            /* Converge in RNE; only the final rounding is architectural. */
            (void)fesetround(FE_TONEAREST);
            for (int i = 0; i < 5; i++) {
                g = 0.5f * (g + v / g);
            }

            f_begin(f_rm(h, rm));
            res = f_end(g * unscale, &flags);
        }
        break;
    }

    case 0x04u:   /* FSGNJ.S / FSGNJN.S / FSGNJX.S */
        switch (rm) {
        case 0: res = (a & 0x7FFFFFFFu) | F_SIGN(b); break;
        case 1: res = (a & 0x7FFFFFFFu) | (F_SIGN(b) ^ 0x80000000u); break;
        case 2: res = a ^ F_SIGN(b); break;
        default: *tval = insn; return RV_EXC_ILLEGAL_INSN;
        }
        break;

    case 0x05u: { /* FMIN.S / FMAX.S */
        if (rm > 1u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        if (f_is_snan(a) || f_is_snan(b)) {
            flags |= FFLAG_NV;
        }
        /*
         * min/max return the non-NaN operand when exactly one is NaN, and
         * the canonical NaN only when both are. -0 compares less than +0,
         * which the plain comparison below would treat as equal.
         */
        if (f_is_nan(a) && f_is_nan(b)) {
            res = F_CANON_NAN;
        } else if (f_is_nan(a)) {
            res = b;
        } else if (f_is_nan(b)) {
            res = a;
        } else if (f_is_zero(a) && f_is_zero(b)) {
            const bool want_neg = (rm == 0u);
            const bool a_neg = F_SIGN(a) != 0u;
            res = (a_neg == want_neg) ? a : b;
        } else {
            const float x = f_bits_to_float(a);
            const float y = f_bits_to_float(b);
            res = ((rm == 0u) ? (x < y) : (x > y)) ? a : b;
        }
        break;
    }

    case 0x14u: { /* FLE.S / FLT.S / FEQ.S -- results go to an X register */
        uint32_t r;
        if (f_is_nan(a) || f_is_nan(b)) {
            /* Quiet comparisons (FEQ) only signal on a signalling NaN. */
            if (rm == 2u) {
                if (f_is_snan(a) || f_is_snan(b)) {
                    flags |= FFLAG_NV;
                }
            } else {
                flags |= FFLAG_NV;
            }
            r = 0u;
        } else {
            const float x = f_bits_to_float(a);
            const float y = f_bits_to_float(b);
            switch (rm) {
            case 0: r = (x <= y); break;   /* FLE */
            case 1: r = (x <  y); break;   /* FLT */
            case 2: r = (x == y); break;   /* FEQ */
            default: *tval = insn; return RV_EXC_ILLEGAL_INSN;
            }
        }
        h->fcsr |= flags;
        h->x[rd] = r;
        h->x[0] = 0u;
        return RV_EXC_NONE;
    }

    case 0x18u: { /* FCVT.W.S / FCVT.WU.S -- result to an X register */
        if (rs2 > 1u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        const uint32_t r = f_to_int(a, rs2 == 0u, f_rm(h, rm), &flags);
        h->fcsr |= flags;
        h->x[rd] = r;
        h->x[0] = 0u;
        return RV_EXC_NONE;
    }

    case 0x1Cu: { /* FMV.X.W / FCLASS.S -- result to an X register */
        uint32_t r;
        if (rs2 != 0u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        if (rm == 0u) {
            r = a;                        /* raw bits, no interpretation */
        } else if (rm == 1u) {
            r = f_classify(a);
        } else {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        h->x[rd] = r;
        h->x[0] = 0u;
        return RV_EXC_NONE;
    }

    case 0x1Au: { /* FCVT.S.W / FCVT.S.WU */
        if (rs2 > 1u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        f_begin(f_rm(h, rm));
        const float fr = (rs2 == 0u) ? (float)(int32_t)h->x[rs1]
                                     : (float)h->x[rs1];
        res = f_end(fr, &flags);
        break;
    }

    case 0x1Eu:   /* FMV.W.X */
        if (rs2 != 0u || rm != 0u) {
            *tval = insn;
            return RV_EXC_ILLEGAL_INSN;
        }
        res = h->x[rs1];
        break;

    default:
        *tval = insn;
        return RV_EXC_ILLEGAL_INSN;
    }

    h->f[rd] = res;
    h->fcsr |= flags;
    f_dirty(h);
    return RV_EXC_NONE;
}

#endif /* RV_EXT_F */
