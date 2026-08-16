/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_fpu.c - RH850 G4MH single-precision floating point.
 *
 * The arithmetic is Berkeley SoftFloat, the same library the RISC-V
 * frontend uses and for the same reason: an FP unit built on the host's
 * own instructions is a second implementation of semantics that has to
 * be exact, and the two disagree exactly where the architecture is
 * fussiest. That lesson was paid for on the RV32 side -- see CLAUDE.md
 * -- and there is no reason to buy it twice.
 *
 * What this file owns, and SoftFloat does not, is everything RH850 spells
 * differently: FPSR's rounding-mode encoding, its three exception bit
 * groups, the CC bits, and the sixteen comparison conditions.
 *
 * Scope: single **and** double precision, and the 64-bit-integer
 * (`.L`/`.UL`) conversions. Unlike the RV32 frontend, which is F and not
 * D -- the difference is not a policy, it is that RH850's doubles live
 * in general-register *pairs* rather than in a separate file, so the
 * only new machinery is reading and writing two registers instead of
 * one.
 *
 * The `.D` sub-opcodes are the `.S` ones with **bit 4 set**: 0x460
 * ADDF.S is 0x470 ADDF.D, 0x448 is 0x458, 0x420 CMPF.S is 0x430. That
 * regularity came off CC-RH and is recorded as a fact about the
 * encodings that exist, *not* as a rule for generating new ones -- the
 * fused multiply-adds have no double form at all (CC-RH rejects
 * `fmaf.d`), so 0x4F0 is not FMAF.D and is not decoded.
 *
 * Still declined: the half-precision conversions at reg1 0x02/0x03 of
 * sub 0x442, whose storage format has no other use here.
 *
 * Double precision costs the F746 firmware **9,680 bytes** of SoftFloat
 * -- 129,560 to 139,240 of text -- which is why the f64 entry points in
 * CMakeLists.txt are added only when this frontend's FPU is on.
 *
 * There is no reference model for any of this. RV32 has riscv-arch-test
 * and Sail to disagree with; G4MH has hand-written tests and a PDF. Read
 * every result here as verified only as far as tests/unit/test_g4mh.c
 * reaches.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_types.h"

#if G4MH_EXT_FPU

#include "softfloat.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Bit-level helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * The quiet NaN produced whenever an operation has to invent one. The
 * manual's per-instruction result tables say "Q-NaN" for every NaN input
 * and for the invalid cases (infinity minus infinity, and so on) without
 * describing payload propagation, so a single default is what is
 * implemented -- which is also what the RISCV specialization of SoftFloat
 * does. **Payload propagation is therefore unverified against hardware**;
 * if a guest is ever seen to depend on it, this is the line to revisit.
 */
#define G4MH_QNAN       0x7FC00000u

#define F_EXP(x)        (((x) >> 23) & 0xFFu)
#define F_MANT(x)       ((x) & 0x007FFFFFu)

static bool f_is_nan(uint32_t x)  { return F_EXP(x) == 0xFFu && F_MANT(x) != 0u; }
static bool f_is_snan(uint32_t x) { return f_is_nan(x) && (F_MANT(x) & 0x00400000u) == 0u; }
static bool f_is_zero(uint32_t x) { return (x & 0x7FFFFFFFu) == 0u; }
static bool f_is_neg(uint32_t x)  { return (x & 0x80000000u) != 0u; }

static float32_t f_v(uint32_t bits) { float32_t f; f.v = bits; return f; }

/* ------------------------------------------------------------------ */
/* Double precision, and the register pairs it lives in                */
/* ------------------------------------------------------------------ */

/*
 * A double occupies two general registers: the **lower** 32 bits in rN
 * and the higher in rN+1, with N even. Same convention as LD.DW, and
 * checked the same way -- CC-RH aligns an odd operand down and warns,
 * and the manual's opcode diagrams draw the field's low bit as a
 * hardwired 0 rather than as part of the register number.
 *
 * Masking rather than declining, because that is what the architecture
 * says happens ("bit 0 of the register number is ignored") and because
 * no assembler can produce the case, so RIE would be a rule invented
 * here.
 */
#define D_QNAN          UINT64_C(0x7FF8000000000000)

#define D_EXP(x)        (((x) >> 52) & 0x7FFu)
#define D_MANT(x)       ((x) & UINT64_C(0x000FFFFFFFFFFFFF))

static bool d_is_nan(uint64_t x)  { return D_EXP(x) == 0x7FFu && D_MANT(x) != 0u; }
static bool d_is_snan(uint64_t x)
{
    return d_is_nan(x) && (D_MANT(x) & UINT64_C(0x0008000000000000)) == 0u;
}
static bool d_is_subnormal(uint64_t x)
{
    return D_EXP(x) == 0u && D_MANT(x) != 0u;
}

static float64_t d_v(uint64_t bits) { float64_t f; f.v = bits; return f; }

static uint64_t d_get(const g4mh_cpu_t *c, uint32_t reg)
{
    const uint32_t n = reg & ~1u;
    return (uint64_t)c->r[n] | ((uint64_t)c->r[n + 1u] << 32);
}

/* ------------------------------------------------------------------ */
/* FPSR                                                                */
/* ------------------------------------------------------------------ */

static uint32_t fpsr_get(const g4mh_cpu_t *c)
{
    return c->sr[0][G4MH_SR_FPSR];
}

static void fpsr_set(g4mh_cpu_t *c, uint32_t v)
{
    c->sr[0][G4MH_SR_FPSR] = v;
}

/*
 * FPSR.RM to SoftFloat's rounding mode.
 *
 * These do *not* share an encoding, unlike RISC-V's frm, which is why
 * this table exists rather than a cast. RH850 orders them RN/RZ/RP/RM;
 * SoftFloat numbers them near_even/minMag/min/max, so RP is SoftFloat's
 * `max` and RM its `min` -- swapping those two is a rounding error only
 * visible on directed-rounding tests, which is precisely the kind of
 * silent wrongness this project keeps writing tests for.
 */
static uint_fast8_t sf_round(uint32_t rm)
{
    switch (rm & 3u) {
    case G4MH_RM_RZ: return softfloat_round_minMag;
    case G4MH_RM_RP: return softfloat_round_max;
    case G4MH_RM_RM: return softfloat_round_min;
    default:         return softfloat_round_near_even;
    }
}

/* Arm SoftFloat for one operation, from the guest's current FPSR. */
static void f_begin(const g4mh_cpu_t *c)
{
    softfloat_roundingMode =
        sf_round((fpsr_get(c) & G4MH_FPSR_RM_MASK) >> G4MH_FPSR_RM_SHIFT);
    softfloat_exceptionFlags = 0u;
}

/*
 * SoftFloat's flag bits, in RH850's order.
 *
 * SoftFloat is inexact/underflow/overflow/infinite/invalid at 1/2/4/8/16
 * and RH850's groups are inexact/underflow/overflow/divide-zero/invalid
 * at the same positions, so this is the identity -- but stated as a
 * conversion rather than assumed, because the two libraries agreeing is a
 * property of this pairing and not a law.
 */
static uint32_t f_flags(void)
{
    const uint_fast8_t f = softfloat_exceptionFlags;
    uint32_t out = 0u;

    if (f & softfloat_flag_inexact)   { out |= G4MH_FPX_I; }
    if (f & softfloat_flag_underflow) { out |= G4MH_FPX_U; }
    if (f & softfloat_flag_overflow)  { out |= G4MH_FPX_O; }
    if (f & softfloat_flag_infinite)  { out |= G4MH_FPX_Z; }
    if (f & softfloat_flag_invalid)   { out |= G4MH_FPX_V; }
    return out;
}

/*
 * Retire one operation's exception flags into FPSR, and say whether the
 * guest asked to be told.
 *
 * Three groups, and they mean different things. The *cause* bits (XC)
 * report what this instruction raised and are overwritten each time. The
 * *preservation* bits (XP) accumulate and are sticky, which is what a
 * guest polls when it runs with exceptions disabled. The *enable* bits
 * (XE) are the guest's mask: a raised flag with its enable set takes an
 * FPP exception instead of being recorded.
 *
 * Returns true when an exception must be taken, in which case the caller
 * must not write the destination register -- the architecture leaves it
 * untouched, and computing into it first and rolling back later is how
 * that gets quietly wrong.
 */
static bool f_end(g4mh_cpu_t *c, uint32_t raised)
{
    uint32_t fpsr = fpsr_get(c);
    const uint32_t enabled = (fpsr & G4MH_FPSR_XE_MASK) >> G4MH_FPSR_XE_SHIFT;

    fpsr &= ~G4MH_FPSR_XC_MASK;
    fpsr |= (raised << G4MH_FPSR_XC_SHIFT) & G4MH_FPSR_XC_MASK;

    if ((raised & enabled) != 0u) {
        /*
         * Enabled: the cause bits say why, the preservation bits are
         * *not* updated -- they exist to record what was ignored, and an
         * exception is the opposite of ignoring it.
         */
        fpsr_set(c, fpsr);
        return true;
    }

    fpsr |= (raised << G4MH_FPSR_XP_SHIFT) & G4MH_FPSR_XP_MASK;
    fpsr_set(c, fpsr);
    return false;
}

/* ------------------------------------------------------------------ */
/* Flushing subnormals                                                 */
/* ------------------------------------------------------------------ */

/*
 * FPSR.FS, which is set out of reset, so this is the *default* path and
 * not an exotic one.
 *
 * A subnormal input is flushed to a zero of the same sign, and the fact
 * that it happened is accumulated in FPSR.IF. With FS clear a subnormal
 * input would instead raise the unimplemented-operation exception (E),
 * which is what a real part does when it declines to handle them in
 * hardware; that is modelled too, because a guest that clears FS and
 * expects E is testing exactly this.
 */
static bool f_is_subnormal(uint32_t x)
{
    return F_EXP(x) == 0u && F_MANT(x) != 0u;
}

static uint32_t f_flush_in(g4mh_cpu_t *c, uint32_t x, bool *flushed)
{
    if (!f_is_subnormal(x)) {
        return x;
    }
    if ((fpsr_get(c) & G4MH_FPSR_FS) == 0u) {
        return x;                       /* caller raises E */
    }
    *flushed = true;
    return x & 0x80000000u;             /* zero, same sign */
}

/* The same for a double. Split from the bit helpers above only because
 * fpsr_get is declared between them. */
static uint64_t d_flush_in(g4mh_cpu_t *c, uint64_t x, bool *flushed)
{
    if (!d_is_subnormal(x)) {
        return x;
    }
    if ((fpsr_get(c) & G4MH_FPSR_FS) == 0u) {
        return x;                       /* caller raises E */
    }
    *flushed = true;
    return x & UINT64_C(0x8000000000000000);   /* zero, same sign */
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

/*
 * The sixteen comparison conditions, from Table 2.10.
 *
 * fcond is a *bitmask*, not an enumeration, and writing it as one is what
 * makes all sixteen fall out of three tests:
 *
 *   CCn <- (fcond[2] & less) | (fcond[1] & equal) | (fcond[0] & unordered)
 *
 * with fcond[3] asking for an invalid-operation exception when the
 * operands are unordered. A switch over sixteen mnemonics is the same
 * function written sixteen times, and this file had one -- it got LT
 * backwards, because the relation is `reg2 < reg1` and the enumeration
 * hid which operand was which.
 *
 * That ordering is the thing to keep: the manual's pseudocode is
 * `result.less <- reg2 < reg1`, matching every other FP instruction here,
 * where reg3 <- reg2 OP reg1. Comparing the operands the other way round
 * is invisible on equality and wrong on every ordered relation.
 */
static bool f_compare(uint32_t fcond, uint32_t reg1_v, uint32_t reg2_v,
                      bool *invalid)
{
    const bool unordered = f_is_nan(reg1_v) || f_is_nan(reg2_v);
    bool less = false;
    bool equal = false;

    /*
     * A signalling NaN always raises invalid. A quiet one raises it only
     * when fcond[3] is set, which is the entire difference between the
     * two halves of Table 2.10.
     */
    *invalid = f_is_snan(reg1_v) || f_is_snan(reg2_v) ||
               (unordered && (fcond & 8u) != 0u);

    if (!unordered) {
        less  = f32_lt_quiet(f_v(reg2_v), f_v(reg1_v));
        equal = f32_eq(f_v(reg2_v), f_v(reg1_v));
    }

    return (((fcond >> 2) & 1u) && less) ||
           (((fcond >> 1) & 1u) && equal) ||
           (((fcond >> 0) & 1u) && unordered);
}

/*
 * The same sixteen conditions on a double. Written out rather than
 * shared with a width parameter, because sharing would mean passing
 * both operands as uint64 and reconstructing the float32 case -- and
 * the one thing that must not drift between the two is the *operand
 * order*, which is `reg2 < reg1` and which this file has already had
 * backwards once.
 */
static bool d_compare(uint32_t fcond, uint64_t reg1_v, uint64_t reg2_v,
                      bool *invalid)
{
    const bool unordered = d_is_nan(reg1_v) || d_is_nan(reg2_v);
    bool less = false;
    bool equal = false;

    *invalid = d_is_snan(reg1_v) || d_is_snan(reg2_v) ||
               (unordered && (fcond & 8u) != 0u);

    if (!unordered) {
        less  = f64_lt_quiet(d_v(reg2_v), d_v(reg1_v));
        equal = f64_eq(d_v(reg2_v), d_v(reg1_v));
    }

    return (((fcond >> 2) & 1u) && less) ||
           (((fcond >> 1) & 1u) && equal) ||
           (((fcond >> 0) & 1u) && unordered);
}

/* ------------------------------------------------------------------ */
/* The instruction interface                                           */
/* ------------------------------------------------------------------ */

/*
 * `sub` is instruction bits 26..16 -- the same value the interpreter's
 * Format X switch keys on -- and reg1/reg2/reg3 are the three register
 * fields. Returns the exception to take, or G4MH_EXC_NONE.
 *
 * Note that reg1 is an *opcode extension* for the one-operand group:
 * sub 0x448 is ABSF.S at reg1 == 0 and NEGF.S at reg1 == 1, and 0x44E is
 * SQRTF/RECIPF/RSQRTF at 0/1/2. This is the trap CLAUDE.md records for
 * the integer set -- a register field reused as an opcode -- and it is
 * why every case below tests reg1 rather than assuming it is a source.
 */
g4mh_exc_t g4mh_fpu_exec(g4mh_cpu_t *c, uint32_t sub, uint32_t reg1,
                         uint32_t reg2, uint32_t reg3)
{
    /*
     * PSW.CU0 gates the whole coprocessor. A part without the FPU option
     * -- or a guest that has not enabled it -- gets a coprocessor-unusable
     * exception, which is what the hardware does and is distinguishable
     * from "this emulator does not implement it" (RIE).
     */
    if ((c->psw & G4MH_PSW_CU0) == 0u) {
        return G4MH_EXC_UCPOP;
    }

    uint32_t a = c->r[reg1];
    uint32_t b = c->r[reg2];
    uint32_t res;
    uint32_t raised = 0u;
    bool flushed = false;
    bool wrote_int = false;             /* result is an integer, not a float */

    /*
     * The double-precision operands, and a 64-bit result.
     *
     * Read unconditionally beside the 32-bit ones: reg1 is an opcode
     * extension in several groups, so reading it as a pair is harmless
     * where it is not an operand, and the alternative is a fetch inside
     * every double case. `wide` says the destination is a register pair
     * -- set by the case, honoured once at the tail, so there is exactly
     * one place a result is written.
     */
    uint64_t da = d_get(c, reg1);
    uint64_t db = d_get(c, reg2);
    uint64_t res64 = 0u;
    bool wide = false;

    /*
     * TRFSR shares CMOVF.S's encoding and is told apart by all three
     * register fields being zero -- the same shape as CALLT hiding in
     * MOV imm5. Tested first, because CMOVF.S would otherwise consume it
     * and write r0, which discards the write and looks like it worked.
     */
    if ((sub & 0x7F1u) == 0x400u && reg1 == 0u && reg2 == 0u && reg3 == 0u) {
        const uint32_t cc = (fpsr_get(c) >> G4MH_FPSR_CC_SHIFT) & 0xFFu;
        const uint32_t sel = (sub >> 1) & 7u;

        /* PSW.Z takes the selected CC bit -- the point of the
         * instruction, which is to get a comparison result out of FPSR
         * and into a place the integer conditional branches can see. */
        c->psw = (c->psw & ~G4MH_PSW_Z) |
                 (((cc >> sel) & 1u) ? G4MH_PSW_Z : 0u);
        return G4MH_EXC_NONE;
    }

    switch (sub) {
    /* ------------------------------------------------------------- */
    /* Double precision: the single-precision sub-opcode with bit 4   */
    /* set. 0x460 ADDF.S is 0x470 ADDF.D, 0x448 is 0x458, and so on.  */
    /*                                                                */
    /* That regularity came off CC-RH rather than off the manual, and */
    /* is stated here as a *fact about the encodings* and not as a    */
    /* rule to derive new ones from: the FMA family has no .D form at */
    /* all -- CC-RH rejects `fmaf.d` -- so 0x4F0 is not FMAF.D and is */
    /* not decoded.                                                   */
    /* ------------------------------------------------------------- */

    case 0x458:                                 /* ABSF.D / NEGF.D      */
        /* No arithmetic, so no flags -- and no flushing either: "a
         * subnormal input will not be flushed even if FS is 1", which
         * the manual says of these two specifically. */
        if (reg1 == 0u) {
            res64 = db & UINT64_C(0x7FFFFFFFFFFFFFFF);
        } else if (reg1 == 1u) {
            res64 = db ^ UINT64_C(0x8000000000000000);
        } else {
            return G4MH_EXC_RIE;
        }
        wide = true;
        break;

    case 0x45E: {                               /* SQRTF/RECIPF/RSQRTF.D */
        db = d_flush_in(c, db, &flushed);
        if (d_is_subnormal(db)) { return G4MH_EXC_FPP; }

        f_begin(c);
        switch (reg1) {
        case 0u:
            res64 = f64_sqrt(d_v(db)).v;
            break;
        case 1u:
            /* 1/x, and as a division rather than an approximation: the
             * manual specifies the result exactly, and an approximation
             * would be a second rounding path for one instruction. */
            res64 = f64_div(d_v(UINT64_C(0x3FF0000000000000)), d_v(db)).v;
            break;
        case 2u:
            res64 = f64_div(d_v(UINT64_C(0x3FF0000000000000)),
                            f64_sqrt(d_v(db))).v;
            break;
        default:
            return G4MH_EXC_RIE;
        }
        raised = f_flags();
        wide = true;
        break;
    }

    case 0x470:                                 /* ADDF.D reg1,reg2,reg3 */
    case 0x472:                                 /* SUBF.D               */
    case 0x474:                                 /* MULF.D               */
    case 0x47E:                                 /* DIVF.D               */
    case 0x478:                                 /* MAXF.D               */
    case 0x47A: {                               /* MINF.D               */
        da = d_flush_in(c, da, &flushed);
        db = d_flush_in(c, db, &flushed);
        if (d_is_subnormal(da) || d_is_subnormal(db)) {
            return G4MH_EXC_FPP;
        }

        f_begin(c);
        switch (sub) {
        case 0x470: res64 = f64_add(d_v(db), d_v(da)).v; break;
        case 0x472: res64 = f64_sub(d_v(db), d_v(da)).v; break;
        case 0x474: res64 = f64_mul(d_v(db), d_v(da)).v; break;
        case 0x47E: res64 = f64_div(d_v(db), d_v(da)).v; break;
        default: {
            /*
             * MAXF/MINF. IEEE minNum/maxNum: a quiet NaN operand is
             * *ignored* rather than propagated, and only a signalling
             * one raises invalid. SoftFloat has no f64_min, so this is
             * written out -- and written the same way as the single
             * form beside it, which is the only defence against the two
             * disagreeing.
             */
            const bool want_max = (sub == 0x478);
            const bool na = d_is_nan(da), nb = d_is_nan(db);

            softfloat_exceptionFlags = 0u;
            if (d_is_snan(da) || d_is_snan(db)) {
                softfloat_exceptionFlags |= softfloat_flag_invalid;
            }
            if (na && nb) {
                res64 = D_QNAN;
            } else if (na) {
                res64 = db;
            } else if (nb) {
                res64 = da;
            } else {
                const bool a_lt = f64_lt_quiet(d_v(da), d_v(db));
                res64 = (a_lt != want_max) ? da : db;
            }
            break;
        }
        }
        raised = f_flags();
        wide = true;
        break;
    }

    /* ---- double to integer, one rounding mode per reg1 ---- */
    case 0x450:                                 /* .DW / .DUW           */
    case 0x454: {                               /* .DL / .DUL           */
        const bool is_unsigned = (reg1 & 0x10u) != 0u;
        const bool to64 = (sub == 0x454u);
        uint_fast8_t rm;

        switch (reg1 & 0x0Fu) {
        case 0u: rm = softfloat_round_near_even; break;
        case 1u: rm = softfloat_round_minMag;    break;
        case 2u: rm = softfloat_round_max;       break;
        case 3u: rm = softfloat_round_min;       break;
        case 4u: rm = sf_round((fpsr_get(c) & G4MH_FPSR_RM_MASK) >>
                               G4MH_FPSR_RM_SHIFT);
                 break;
        default: return G4MH_EXC_RIE;
        }

        db = d_flush_in(c, db, &flushed);
        if (d_is_subnormal(db)) { return G4MH_EXC_FPP; }

        softfloat_exceptionFlags = 0u;
        if (to64) {
            res64 = is_unsigned ? (uint64_t)f64_to_ui64(d_v(db), rm, true)
                                : (uint64_t)f64_to_i64(d_v(db), rm, true);
            wide = true;
        } else {
            res = is_unsigned ? (uint32_t)f64_to_ui32(d_v(db), rm, true)
                              : (uint32_t)f64_to_i32(d_v(db), rm, true);
        }
        raised = f_flags();
        wrote_int = true;
        break;
    }

    /*
     * The conversions whose *destination* is a double, plus the two
     * that change precision. reg1 is the whole selector, with bit 4 the
     * unsigned form of an integer source -- the same shape as the
     * float-to-integer groups.
     */
    case 0x452: {
        f_begin(c);
        switch (reg1) {
        case 0x00u: res64 = i32_to_f64((int32_t)b).v;    break;  /* W  */
        case 0x10u: res64 = ui32_to_f64(b).v;            break;  /* UW */
        case 0x01u: res64 = i64_to_f64((int64_t)db).v;   break;  /* L  */
        case 0x11u: res64 = ui64_to_f64(db).v;           break;  /* UL */
        case 0x02u:                                              /* S  */
            b = f_flush_in(c, b, &flushed);
            if (f_is_subnormal(b)) { return G4MH_EXC_FPP; }
            res64 = f32_to_f64(f_v(b)).v;
            break;
        case 0x03u: {                                            /* D->S */
            /* The one member of this group whose result is 32 bits. */
            db = d_flush_in(c, db, &flushed);
            if (d_is_subnormal(db)) { return G4MH_EXC_FPP; }
            res = f64_to_f32(d_v(db)).v;
            raised = f_flags();
            goto narrow_done;
        }
        default: return G4MH_EXC_RIE;
        }
        raised = f_flags();
        wide = true;
    narrow_done:
        break;
    }

    /* ---- one-operand group, reg1 selects the operation ---- */
    case 0x448:                                 /* ABSF.S / NEGF.S      */
        if (reg1 == 0u) {
            res = b & 0x7FFFFFFFu;
        } else if (reg1 == 1u) {
            res = b ^ 0x80000000u;
        } else {
            return G4MH_EXC_RIE;
        }
        /*
         * Neither rounds, and neither is defined to raise anything: the
         * manual gives them no exception list beyond the signalling-NaN
         * case, and they are pure sign manipulation. So no f_begin/f_end
         * and, deliberately, no flushing of a subnormal input -- the
         * manual says so explicitly for both ("A subnormal input will not
         * be flushed even if the FS bit is 1").
         */
        c->r[reg3] = (reg3 == 0u) ? 0u : res;
        return G4MH_EXC_NONE;

    case 0x44E: {                               /* SQRTF/RECIPF/RSQRTF  */
        b = f_flush_in(c, b, &flushed);
        if (f_is_subnormal(b)) { return G4MH_EXC_FPP; }

        f_begin(c);
        if (reg1 == 0u) {                       /* SQRTF.S              */
            res = f32_sqrt(f_v(b)).v;
        } else if (reg1 == 1u) {                /* RECIPF.S             */
            res = f32_div(f_v(0x3F800000u), f_v(b)).v;
        } else if (reg1 == 2u) {                /* RSQRTF.S             */
            res = f32_div(f_v(0x3F800000u), f32_sqrt(f_v(b))).v;
        } else {
            return G4MH_EXC_RIE;
        }
        raised = f_flags();
        break;
    }

    /* ---- two-operand arithmetic ---- */
    case 0x460:                                 /* ADDF.S reg1,reg2,reg3 */
    case 0x462:                                 /* SUBF.S               */
    case 0x464:                                 /* MULF.S               */
    case 0x46E:                                 /* DIVF.S               */
    case 0x468:                                 /* MAXF.S               */
    case 0x46A: {                               /* MINF.S               */
        a = f_flush_in(c, a, &flushed);
        b = f_flush_in(c, b, &flushed);
        if (f_is_subnormal(a) || f_is_subnormal(b)) { return G4MH_EXC_FPP; }

        f_begin(c);
        switch (sub) {
        /*
         * reg3 <- reg2 OP reg1, so the *second* register field is the
         * left operand. Getting this backwards is invisible in add and
         * multiply and wrong in subtract and divide, which is the usual
         * way an operand-order bug ships.
         */
        case 0x460: res = f32_add(f_v(b), f_v(a)).v; break;
        case 0x462: res = f32_sub(f_v(b), f_v(a)).v; break;
        case 0x464: res = f32_mul(f_v(b), f_v(a)).v; break;
        case 0x46E: res = f32_div(f_v(b), f_v(a)).v; break;
        default: {
            /*
             * MAXF/MINF. SoftFloat has no f32_max, and the architecture's
             * rule is not simply "the larger": a signalling NaN raises
             * invalid, a quiet NaN operand gives the *other* operand, and
             * two NaNs give a quiet NaN. Zeros of opposite sign compare
             * equal, and the manual makes MAXF(+0,-0) = +0, which a plain
             * f32_lt cannot express.
             */
            const bool want_max = (sub == 0x468);
            if (f_is_snan(a) || f_is_snan(b)) {
                softfloat_exceptionFlags |= softfloat_flag_invalid;
                res = G4MH_QNAN;
            } else if (f_is_nan(a) && f_is_nan(b)) {
                res = G4MH_QNAN;
            } else if (f_is_nan(a)) {
                res = b;
            } else if (f_is_nan(b)) {
                res = a;
            } else if (f_is_zero(a) && f_is_zero(b)) {
                const bool neg = f_is_neg(a) && f_is_neg(b);
                res = want_max ? (neg ? 0x80000000u : 0u)
                               : ((f_is_neg(a) || f_is_neg(b)) ? 0x80000000u : 0u);
            } else {
                const bool a_lt = f32_lt_quiet(f_v(a), f_v(b));
                res = (a_lt == want_max) ? b : a;
            }
            break;
        }
        }
        raised = f_flags();
        break;
    }

    /* ---- fused multiply-add family ---- */
    case 0x4E0:                                 /* FMAF.S   r3 += r2*r1 */
    case 0x4E2:                                 /* FMSF.S   r3 = r2*r1-r3 */
    case 0x4E4:                                 /* FNMAF.S  r3 = -(r2*r1+r3) */
    case 0x4E6: {                               /* FNMSF.S  r3 = -(r2*r1-r3) */
        uint32_t acc = c->r[reg3];

        a = f_flush_in(c, a, &flushed);
        b = f_flush_in(c, b, &flushed);
        acc = f_flush_in(c, acc, &flushed);
        if (f_is_subnormal(a) || f_is_subnormal(b) || f_is_subnormal(acc)) {
            return G4MH_EXC_FPP;
        }

        /*
         * f32_mulAdd rounds once, which is the whole point of a fused
         * operation and the reason this is not a multiply followed by an
         * add. The negating variants negate the *addend* or the result,
         * not the product, and the sign of a NaN is not meaningful -- so
         * the negation is applied to the SoftFloat result rather than to
         * an operand, except for FMSF where the architecture subtracts
         * the accumulator and that is spelled as adding its negation.
         */
        f_begin(c);
        switch (sub) {
        case 0x4E0: res = f32_mulAdd(f_v(b), f_v(a), f_v(acc)).v; break;
        case 0x4E2: res = f32_mulAdd(f_v(b), f_v(a),
                                     f_v(acc ^ 0x80000000u)).v; break;
        case 0x4E4: res = f32_mulAdd(f_v(b), f_v(a), f_v(acc)).v;
                    if (!f_is_nan(res)) { res ^= 0x80000000u; }
                    break;
        default:    res = f32_mulAdd(f_v(b), f_v(a),
                                     f_v(acc ^ 0x80000000u)).v;
                    if (!f_is_nan(res)) { res ^= 0x80000000u; }
                    break;
        }
        raised = f_flags();
        break;
    }

    /* ---- float to integer, one rounding mode per opcode ---- */
    case 0x440:                                 /* CVTF/TRNCF/CEILF/... */
    case 0x444: {
        /*
         * reg1 is the opcode extension *and* carries the signedness in
         * bit 4: 0x0n is the signed form and 0x1n the unsigned one, with
         * n selecting the rounding. 0x440 puts the result in one
         * register and 0x444 in a pair -- the .L/.UL group -- and the
         * reg1 encoding is identical across both, and across the
         * double-source groups at 0x450/0x454.
         */
        const bool to64 = (sub == 0x444u);
        const bool is_unsigned = (reg1 & 0x10u) != 0u;
        uint_fast8_t rm;

        switch (reg1 & 0x0Fu) {
        case 0u: rm = softfloat_round_near_even; break;  /* ROUNDF.SW  */
        case 1u: rm = softfloat_round_minMag;    break;  /* TRNCF.SW   */
        case 2u: rm = softfloat_round_max;       break;  /* CEILF.SW   */
        case 3u: rm = softfloat_round_min;       break;  /* FLOORF.SW  */
        case 4u: rm = sf_round((fpsr_get(c) & G4MH_FPSR_RM_MASK) >>
                               G4MH_FPSR_RM_SHIFT);      /* CVTF.SW    */
                 break;
        default: return G4MH_EXC_RIE;
        }

        b = f_flush_in(c, b, &flushed);
        if (f_is_subnormal(b)) { return G4MH_EXC_FPP; }

        softfloat_exceptionFlags = 0u;
        if (to64) {
            res64 = is_unsigned ? (uint64_t)f32_to_ui64(f_v(b), rm, true)
                                : (uint64_t)f32_to_i64(f_v(b), rm, true);
            wide = true;
        } else {
            res = is_unsigned ? (uint32_t)f32_to_ui32(f_v(b), rm, true)
                              : (uint32_t)f32_to_i32(f_v(b), rm, true);
        }
        raised = f_flags();
        wrote_int = true;
        break;
    }

    /* ---- integer to float ---- */
    case 0x442: {
        /*
         * reg1 selects the source width and signedness the same way:
         * 0x00 word, 0x10 unsigned word, 0x01 long, 0x11 unsigned long,
         * all to single.
         */
        f_begin(c);
        switch (reg1) {
        case 0x00u: res = i32_to_f32((int32_t)b).v;  break;      /* W  */
        case 0x10u: res = ui32_to_f32(b).v;          break;      /* UW */
        case 0x01u: res = i64_to_f32((int64_t)db).v; break;      /* L  */
        case 0x11u: res = ui64_to_f32(db).v;         break;      /* UL */
        default:
            /*
             * 0x02/0x03 are the half-precision conversions, which this
             * frontend does not implement: the storage format has no
             * other use here and adding it would mean a second rounding
             * path for one instruction pair.
             */
            return G4MH_EXC_RIE;
        }
        raised = f_flags();
        break;
    }

    default:
        /*
         * CMPF.D and CMOVF.D, which sit at the single forms' sub-opcode
         * with bit 4 set exactly as the arithmetic does: 0x420 -> 0x430
         * and 0x400 -> 0x410. Placed before the single-precision tests
         * below because the masks differ only in that bit, and a reader
         * checking the order should not have to work out which wins.
         */
        if ((sub & 0x7F1u) == 0x430u) {         /* CMPF.D               */
            const uint32_t cond = reg3 & 0xFu;
            const uint32_t fcbit = (sub >> 1) & 7u;
            bool invalid = false;
            const bool r = d_compare(cond, da, db, &invalid);
            uint32_t fpsr = fpsr_get(c);

            fpsr &= ~(1u << (G4MH_FPSR_CC_SHIFT + fcbit));
            fpsr |= (r ? 1u : 0u) << (G4MH_FPSR_CC_SHIFT + fcbit);
            fpsr_set(c, fpsr);

            return f_end(c, invalid ? G4MH_FPX_V : 0u) ? G4MH_EXC_FPP
                                                       : G4MH_EXC_NONE;
        }
        if ((sub & 0x7F1u) == 0x410u) {         /* CMOVF.D              */
            const uint32_t fcbit = (sub >> 1) & 7u;
            const bool taken =
                ((fpsr_get(c) >> (G4MH_FPSR_CC_SHIFT + fcbit)) & 1u) != 0u;
            const uint64_t v = taken ? da : db;
            const uint32_t rd = reg3 & ~1u;

            /* A pair move, and no arithmetic: no flags, no flushing. */
            if (rd != 0u) {
                c->r[rd] = (uint32_t)v;
            }
            c->r[rd + 1u] = (uint32_t)(v >> 32);
            return G4MH_EXC_NONE;
        }

        /* CMPF.S and CMOVF.S carry a condition in bits 3..1 of `sub`. */
        if ((sub & 0x7F1u) == 0x420u) {         /* CMPF.S               */
            /*
             * fcond is the *reg3 field*; fcbit is the `fff` bits of the
             * sub-opcode. That is the opposite of the obvious reading --
             * reg3 is the destination in every other instruction here,
             * and CMPF has no destination register -- and this file
             * shipped it the wrong way round.
             *
             * Settled against the vendor assembler rather than the
             * manual, whose [Opcode] diagram draws the field as `0FFFF`
             * and names neither part:
             *
             *   cmpf.s 0x4, r6, r7, 3   E7372624  sub=0x426 reg3=4
             *   cmpf.s 0xC, r6, r7, 5   E7372A64  sub=0x42A reg3=12
             *
             * The second is what makes it certain: 0xC needs four bits,
             * so fcond cannot be the three-bit `fff`, and fcbit tracks
             * `fff` across both. It also answers the question the
             * previous comment here left open -- fcond[3], the
             * raise-invalid-when-unordered bit, is simply bit 3 of the
             * same field.
             *
             * CMOVF.S and TRFSR are *not* like this: there fcbit is
             * `fff` and reg3 is the destination, which is why they were
             * already right and only this one was wrong.
             */
            const uint32_t cond = reg3 & 0xFu;
            const uint32_t fcbit = (sub >> 1) & 7u;
            bool invalid = false;
            const bool r = f_compare(cond, a, b, &invalid);
            uint32_t fpsr = fpsr_get(c);

            fpsr &= ~(1u << (G4MH_FPSR_CC_SHIFT + fcbit));
            fpsr |= (r ? 1u : 0u) << (G4MH_FPSR_CC_SHIFT + fcbit);
            fpsr_set(c, fpsr);

            return f_end(c, invalid ? G4MH_FPX_V : 0u) ? G4MH_EXC_FPP
                                                       : G4MH_EXC_NONE;
        }
        if ((sub & 0x7F1u) == 0x400u) {         /* CMOVF.S              */
            const uint32_t fcbit = (sub >> 1) & 7u;
            const bool taken =
                ((fpsr_get(c) >> (G4MH_FPSR_CC_SHIFT + fcbit)) & 1u) != 0u;

            /*
             * reg3 <- CC ? reg1 : reg2, and that direction is the whole
             * content of the instruction.
             *
             * It shipped inverted. Settled against CC-RH, which is
             * unambiguous because it emits the pair together:
             *
             *   p = 1.0f at sp+4, q = 2.0f at sp+0
             *   ld.w 4[r3], r2 ; ld.w 0[r3], r5
             *   cmpf.s 0x4, r2, r5      ; reg2=r2=p, reg1=r5=q
             *   movea 22, r0, r2 ; mov 11, r5
             *   cmovf.s 0, r5, r2, r10  ; reg1=r5=11, reg2=r2=22
             *
             * for `return (p < q) ? 11 : 22;`. fcond 4 is OLT and the
             * relation is reg2 < reg1, so 1.0 < 2.0 sets CC0 -- and the
             * answer the compiler wants for a set CC0 is 11, which is
             * reg1. Taking reg2 instead returned 22 for `1.0 < 2.0`.
             *
             * No arithmetic, so no flags and no flushing: a bit move.
             */
            if (reg3 != 0u) {
                c->r[reg3] = taken ? a : b;
            }
            return G4MH_EXC_NONE;
        }
        return G4MH_EXC_RIE;
    }

    /*
     * A flushed input is recorded whether or not anything else happened,
     * and IF is sticky.
     */
    if (flushed) {
        fpsr_set(c, fpsr_get(c) | G4MH_FPSR_IF);
    }

    /*
     * Order matters: the destination is written only when no exception is
     * taken. The architecture leaves it untouched on an enabled
     * exception, and writing first would need an undo that nothing else
     * here has.
     */
    if (f_end(c, raised)) {
        return G4MH_EXC_FPP;
    }
    if (wide) {
        /*
         * A register pair: the **lower** 32 bits in rN and the higher in
         * rN+1, N forced even. Writing r0 is discarded as everywhere
         * else, but rN+1 is still written when N is 0 -- r1 is a real
         * register and the architecture stores the high half there.
         */
        const uint32_t rd = reg3 & ~1u;

        if (rd != 0u) {
            c->r[rd] = (uint32_t)res64;
        }
        c->r[rd + 1u] = (uint32_t)(res64 >> 32);
    } else if (reg3 != 0u) {
        c->r[reg3] = res;
    }
    (void)wrote_int;
    return G4MH_EXC_NONE;
}

#endif /* G4MH_EXT_FPU */
