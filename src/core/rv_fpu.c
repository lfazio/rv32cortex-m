/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_fpu.c - Single-precision floating point (F extension).
 *
 * D is deliberately not implemented: the Cortex-M4F and M7 FPUs this
 * emulator targets are single-precision, so D would be entirely soft-float
 * and is not what the hardware is for. Without D, the register file is 32
 * bits wide and there is no NaN-boxing to maintain.
 *
 * Arithmetic is evaluated in `double` and then rounded once to `float`.
 * That is not a shortcut -- it is what makes the flags computable. `double`
 * has more than twice the significand of `float`, so a single-precision
 * add, subtract or multiply is *exact* in double, and division and square
 * root round benignly. Comparing the double result against the rounded
 * float result therefore says precisely whether the operation was inexact,
 * overflowed or underflowed, which is otherwise the hardest part of an FPU
 * to get right without a soft-float library.
 *
 * On the target this means double arithmetic runs soft-float via libgcc.
 * That is slow, and acceptable: guest floating point is a correctness
 * feature here, not a performance one.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#if RV_EXT_F

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

static uint32_t f_round_sticky(double d, uint32_t rm, bool sticky,
                               uint32_t *flags);

/* Effective rounding mode: the instruction's rm field, or fcsr.frm if DYN. */
static uint32_t f_rm(const rv_hart_t *h, uint32_t rm)
{
    return (rm == FRM_DYN) ? ((h->fcsr >> 5) & 0x7u) : rm;
}

/* One ULP away from `bits` in the direction of `up`, staying finite-safe. */
static uint32_t f_next(uint32_t bits, bool up)
{
    const bool neg = F_SIGN(bits) != 0u;
    if (f_is_zero(bits)) {
        /* Smallest subnormal with the requested sign. */
        return up ? 0x00000001u : 0x80000001u;
    }
    /* Magnitude grows away from zero, shrinks toward it. */
    if (up != neg) {
        return bits + 1u;
    }
    return bits - 1u;
}

/*
 * Round a double to single precision under `rm`, accumulating flags.
 *
 * The host rounds to nearest-even when converting, which is the RNE case
 * directly. The directed modes are then reached by testing whether the
 * conversion moved the value and, if so, stepping one ULP the right way.
 */
static uint32_t f_round(double d, uint32_t rm, uint32_t *flags)
{
    return f_round_sticky(d, rm, false, flags);
}

static uint32_t f_round_sticky(double d, uint32_t rm, bool sticky,
                               uint32_t *flags)
{
    float r = (float)d;
    uint32_t bits = f_float_to_bits(r);

    if ((double)r == d) {
        /*
         * The double value converts exactly, but the caller may already
         * know the double computation itself rounded -- see the 2Sum in
         * the add/sub path. Addition of two floats is *not* always exact
         * in double: aligning 2^127 with 2^-149 needs far more than 53
         * bits of significand, so the intermediate can round and the
         * result still look exact here.
         */
        if (sticky) {
            *flags |= FFLAG_NX;
        }
        return bits;
    }

    (void)sticky;
    /*
     * Inexact from here on, whichever way it is rounded.
     *
     * Overflow is decided here, from the round-to-nearest result, and not
     * after the directed-mode adjustment below. IEEE raises overflow when
     * the exact result exceeds the largest finite value, even in the modes
     * that then deliver that largest finite value rather than infinity --
     * so testing for infinity after the adjustment misses exactly those
     * cases and reports NX where NX|OF is required.
     */
    const bool overflow = f_is_inf(bits);
    const bool too_big = ((double)r > d);

    switch (rm) {
    case FRM_RTZ:
        /* Toward zero: undo a rounding that increased the magnitude. */
        if ((too_big && d > 0.0) || (!too_big && d < 0.0)) {
            if (!f_is_inf(bits)) {
                bits = f_next(bits, d < 0.0);
            } else {
                /* Overflow to infinity becomes the largest finite value. */
                bits = F_SIGN(bits) | 0x7F7FFFFFu;
            }
        }
        break;
    case FRM_RDN:
        if (too_big) {
            bits = f_is_inf(bits) ? (F_SIGN(bits) | 0x7F7FFFFFu)
                                  : f_next(bits, false);
        }
        break;
    case FRM_RUP:
        if (!too_big) {
            bits = f_is_inf(bits) ? (F_SIGN(bits) | 0x7F7FFFFFu)
                                  : f_next(bits, true);
        }
        break;
    case FRM_RMM:
        /*
         * Ties away from zero. Only a tie differs from RNE, and a tie is
         * exactly halfway, so re-round by comparing against the midpoint.
         */
        {
            const double lo = (double)f_bits_to_float(f_next(bits, false));
            const double hi = (double)f_bits_to_float(f_next(bits, true));
            if (d - lo == hi - d) {
                /* d is a tie; take the larger magnitude. */
                const uint32_t away = f_next(bits, d > 0.0);
                if ((away & 0x7FFFFFFFu) > (bits & 0x7FFFFFFFu)) {
                    bits = away;
                }
            }
        }
        break;
    default:
        break;    /* FRM_RNE: the host conversion already did it */
    }

    *flags |= FFLAG_NX;

    if (overflow) {
        *flags |= FFLAG_OF;
    } else if (F_EXP(bits) == 0u) {
        /* Subnormal or zero from a non-zero value: tiny and inexact. */
        *flags |= FFLAG_UF;
    }
    return bits;
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

    double d = (double)f_bits_to_float(a);

    /* Round to an integral value under the active mode. */
    double t;
    switch (rm) {
    case FRM_RTZ: t = (d < 0.0) ? -(double)(uint64_t)(-d) : (double)(uint64_t)d; break;
    case FRM_RDN: t = (double)(int64_t)d; if (t > d) { t -= 1.0; } break;
    case FRM_RUP: t = (double)(int64_t)d; if (t < d) { t += 1.0; } break;
    case FRM_RMM: t = (d < 0.0) ? -(double)(int64_t)(-d + 0.5)
                                : (double)(int64_t)(d + 0.5); break;
    default: {   /* RNE */
        t = (double)(int64_t)d;
        const double diff = d - t;
        if (diff > 0.5 || (diff == 0.5 && ((int64_t)t & 1) != 0)) {
            t += 1.0;
        } else if (diff < -0.5 || (diff == -0.5 && ((int64_t)t & 1) != 0)) {
            t -= 1.0;
        }
        break;
    }
    }

    if (t != d) {
        *flags |= FFLAG_NX;
    }

    if (is_signed) {
        if (t >= 2147483648.0)  { *flags |= FFLAG_NV; return lim_max; }
        if (t < -2147483648.0)  { *flags |= FFLAG_NV; return lim_min; }
        return (uint32_t)(int32_t)t;
    }
    if (t >= 4294967296.0) { *flags |= FFLAG_NV; return lim_max; }
    if (t <= -1.0)         { *flags |= FFLAG_NV; return lim_min; }
    if (t < 0.0)           { return 0u; }   /* -0.5 rounds to 0, not a fault */
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

#define OP_LOAD_FP   0x07u
#define OP_STORE_FP  0x27u
#define OP_FP        0x53u
#define OP_MADD      0x43u
#define OP_MSUB      0x47u
#define OP_NMSUB     0x4Bu
#define OP_NMADD     0x4Fu

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
             * The product is exact in double, so this is a true fused
             * multiply-add: one rounding, at the end.
             */
            /*
             * The four forms differ in which term is negated, not in
             * whether the whole result is:
             *
             *   fmadd    p + c        fnmsub   -p + c
             *   fmsub    p - c        fnmadd   -p - c
             *
             * Negating the finished sum instead gives -(p+c) for fnmsub,
             * which is -p-c: right magnitude, wrong sign, and it only shows
             * up once the operands make the two disagree.
             */
            double p = (double)f_bits_to_float(a) * (double)f_bits_to_float(b);
            if (opcode == OP_NMSUB || opcode == OP_NMADD) {
                p = -p;
            }
            double addend = (double)f_bits_to_float(c);
            if (opcode == OP_MSUB || opcode == OP_NMADD) {
                addend = -addend;
            }
            const double r = p + addend;
            /* inf + (-inf) is invalid, and shows up as a NaN sum here. */
            if (r != r) {
                flags |= FFLAG_NV;
                res = F_CANON_NAN;
            } else {
                res = f_round(r, f_rm(h, rm), &flags);
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
        if (f_nan_result(a, b, &res, &flags)) {
            break;
        }
        const double x = (double)f_bits_to_float(a);
        double y = (double)f_bits_to_float(b);
        double r;

        if (funct5 == 0x03u) {
            if (f_is_zero(b)) {
                if (f_is_zero(a)) {
                    flags |= FFLAG_NV;    /* 0/0 */
                    res = F_CANON_NAN;
                } else {
                    flags |= FFLAG_DZ;
                    res = (F_SIGN(a) ^ F_SIGN(b)) | 0x7F800000u;
                }
                break;
            }
            if (f_is_inf(a) && f_is_inf(b)) {
                flags |= FFLAG_NV;        /* inf/inf */
                res = F_CANON_NAN;
                break;
            }
            r = x / y;
        } else if (funct5 == 0x02u) {
            if ((f_is_zero(a) && f_is_inf(b)) || (f_is_inf(a) && f_is_zero(b))) {
                flags |= FFLAG_NV;
                res = F_CANON_NAN;
                break;
            }
            r = x * y;
        } else {
            if (funct5 == 0x01u) {
                y = -y;
            }
            if (f_is_inf(a) && f_is_inf(b) &&
                (F_SIGN(a) != F_SIGN(f_float_to_bits((float)y)))) {
                flags |= FFLAG_NV;        /* inf + (-inf) */
                res = F_CANON_NAN;
                break;
            }
            r = x + y;
            /*
             * 2Sum recovers the exact rounding error of the double
             * addition. A non-zero error means the intermediate was
             * inexact, which the conversion below cannot see.
             */
            const double bb = r - x;
            const double err = (x - (r - bb)) + (y - bb);
            res = f_round_sticky(r, f_rm(h, rm), err != 0.0, &flags);
            break;
        }
        res = f_round(r, f_rm(h, rm), &flags);
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
             * Newton-Raphson on the double value. Written out rather than
             * calling sqrt() because the core must not depend on libm: the
             * firmware links -nostdlib. Double has enough headroom that
             * four iterations from a decent seed converge to well within a
             * single-precision ulp.
             */
            /*
             * Scale subnormals into the normal range before seeding. The
             * exponent-halving bit trick assumes a normalised exponent
             * field, and on a subnormal it produces a seed several orders
             * of magnitude out, which Newton then converges towards the
             * wrong value entirely.
             *
             * Scaling by 2^96 (even, so the square root scales by 2^48)
             * lifts every subnormal well clear of the boundary.
             */
            uint32_t norm = a;
            double unscale = 1.0;
            if (F_EXP(a) == 0u) {
                norm = f_float_to_bits((float)((double)f_bits_to_float(a) * 79228162514264337593543950336.0));
                unscale = 1.0 / 281474976710656.0;   /* 2^-48 */
            }

            const double v = (double)f_bits_to_float(norm);
            double g = (double)f_bits_to_float((norm >> 1) + 0x1FC00000u);
            for (int i = 0; i < 6; i++) {
                g = 0.5 * (g + v / g);
            }
            res = f_round(g * unscale, f_rm(h, rm), &flags);
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
        const double d = (rs2 == 0u) ? (double)(int32_t)h->x[rs1]
                                     : (double)h->x[rs1];
        res = f_round(d, f_rm(h, rm), &flags);
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
