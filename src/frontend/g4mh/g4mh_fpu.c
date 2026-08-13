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
 * Scope: **single precision only**. Every `.S` operation in the manual's
 * Section 2.4.4 is here; every `.D` one is not, and neither are the
 * 64-bit-integer (`.L`/`.UL`) conversions, whose results land in a
 * register *pair* and so need a wider destination than a general
 * register. Those encodings keep raising RIE, which is the correct
 * report for something unimplemented and is what makes the gap visible
 * rather than silently wrong. This mirrors the RV32 frontend, which
 * implements F and not D.
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
         * n selecting the rounding. 0x444 is the 64-bit (.L) destination
         * group, which is not implemented -- its result needs a register
         * pair -- so it declines rather than truncating silently.
         */
        if (sub == 0x444) {
            return G4MH_EXC_RIE;                /* .L / .UL, see above  */
        }

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
        if (is_unsigned) {
            res = (uint32_t)f32_to_ui32(f_v(b), rm, true);
        } else {
            res = (uint32_t)f32_to_i32(f_v(b), rm, true);
        }
        raised = f_flags();
        wrote_int = true;
        break;
    }

    /* ---- integer to float ---- */
    case 0x442: {
        /*
         * reg1 selects the source width and signedness the same way.
         * 0x00 word to single, 0x10 unsigned word to single; the 0x01 and
         * 0x11 forms are the 64-bit sources and are declined with the
         * rest of the .L group. 0x02/0x03 are the half-precision
         * conversions, which this frontend does not implement either --
         * the storage format has no other use here and adding it would
         * mean a second rounding path for one instruction pair.
         */
        f_begin(c);
        if (reg1 == 0x00u) {
            res = i32_to_f32((int32_t)b).v;
        } else if (reg1 == 0x10u) {
            res = ui32_to_f32(b).v;
        } else {
            return G4MH_EXC_RIE;
        }
        raised = f_flags();
        break;
    }

    default:
        /* CMPF.S and CMOVF.S carry a condition in bits 3..1 of `sub`. */
        if ((sub & 0x7F1u) == 0x420u) {         /* CMPF.S               */
            /*
             * fcond[2:0] is the `fff` field of the sub-opcode. fcond[3]
             * -- the "raise invalid when unordered" half of Table 2.10 --
             * is taken from bit 3 of the reg3 field, which is the only
             * spare bit in the encoding: reg3 carries fcbit, and fcbit
             * names one of eight CC bits and so needs three.
             *
             * **That bit position is inferred, not read off the manual.**
             * The [Opcode] diagram draws the field as `0FFFF` without
             * naming its parts. Getting it wrong changes only whether a
             * *quiet* NaN comparison raises invalid -- the relation and
             * the CC bit written are unaffected -- so it fails safe, but
             * it is unverified and is the first thing to check if a guest
             * disagrees about fflags after a comparison.
             */
            const uint32_t cond = ((sub >> 1) & 7u) | ((reg3 & 8u) ? 8u : 0u);
            const uint32_t fcbit = reg3 & 7u;
            bool invalid = false;
            const bool r = f_compare(cond, a, b, &invalid);
            uint32_t fpsr = fpsr_get(c);

            /*
             * The result goes to one of the eight CC bits, named by the
             * low three bits of reg3 -- not to a general register. That
             * is what makes CMPF and CMOVF a pair rather than a compare
             * and a branch.
             */
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

            /* No arithmetic, so no flags and no flushing: a bit move. */
            if (reg3 != 0u) {
                c->r[reg3] = taken ? b : a;
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
    if (reg3 != 0u) {
        c->r[reg3] = res;
    }
    (void)wrote_int;
    return G4MH_EXC_NONE;
}

#endif /* G4MH_EXT_FPU */
