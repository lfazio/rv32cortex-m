/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_fpu.c - RH850 G4MH floating point: the scalar instructions.
 *
 * What one element does -- SoftFloat, and the RH850's rules for NaNs,
 * subnormals and flushing around it -- is g4mh_fp.c's, shared with the
 * FXU, whose four ways follow the same rules. This file decodes the
 * scalar instructions, reads and writes their registers, and commits
 * what an element raised into FPSR.
 *
 * Single **and** double precision, the 64-bit-integer (`.L`/`.UL`)
 * conversions and the half-precision ones. RH850's doubles live in
 * general-register *pairs* rather than in a separate file, so double
 * precision is reading and writing two registers instead of one.
 *
 * The `.D` sub-opcodes are the `.S` ones with **bit 4 set**: 0x460
 * ADDF.S is 0x470 ADDF.D, 0x448 is 0x458, 0x420 CMPF.S is 0x430. That
 * regularity came off CC-RH and is recorded as a fact about the
 * encodings that exist, *not* as a rule for generating new ones -- the
 * fused multiply-adds have no double form at all (CC-RH rejects
 * `fmaf.d`), so 0x4F0 is not FMAF.D and is not decoded.
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

#include "g4mh_fp.h"

#if G4MH_EXT_FPU

/* ------------------------------------------------------------------ */
/* Registers                                                           */
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
static uint64_t d_get(const g4mh_cpu_t *c, uint32_t reg)
{
    const uint32_t n = reg & ~1u;
    return (uint64_t)c->r[n] | ((uint64_t)c->r[n + 1u] << 32);
}

/*
 * Writing r0 is discarded as everywhere else, but rN+1 is still written
 * when N is 0 -- r1 is a real register and the architecture stores the
 * high half there.
 */
static void d_set(g4mh_cpu_t *c, uint32_t reg, uint64_t v)
{
    const uint32_t n = reg & ~1u;

    if (n != 0u) {
        c->r[n] = (uint32_t)v;
    }
    c->r[n + 1u] = (uint32_t)(v >> 32);
}

static void w_set(g4mh_cpu_t *c, uint32_t reg, uint32_t v)
{
    if (reg != 0u) {
        c->r[reg] = v;
    }
}

/* ------------------------------------------------------------------ */
/* FPSR                                                                */
/* ------------------------------------------------------------------ */

/*
 * Retire one instruction's flags into FPSR, and say whether FPE is
 * taken -- in which case the caller writes nothing: "the operation
 * result is not stored, and only the cause bits are affected".
 *
 * The cause bits change **only** when an exception is taken, and then
 * hold everything detected, enabled or not: "the cause bits are
 * rewritten by a floating-point instruction ... where the floating-point
 * operation exception occurred", "the previous results are held until
 * the next enabled exception occurs" (R01UH0923EJ0130 3.2.3.4,
 * 3.2.6.1(7)). This rewrote them on every instruction, so a handler
 * reading XC after one more FP instruction saw that one's. E is always
 * an exception and has no enable or preservation bit. Otherwise the
 * preservation bits accumulate. IF accumulates either way (3.2.6.1(8)
 * caution 4).
 */
static bool fpsr_commit(g4mh_cpu_t *c, const g4mh_fp_t *e)
{
    uint32_t fpsr = c->sr[0][G4MH_SR_FPSR];
    const uint32_t enabled = (fpsr & G4MH_FPSR_XE_MASK) >> G4MH_FPSR_XE_SHIFT;

    if (e->flushed) {
        fpsr |= G4MH_FPSR_IF;
    }
    if ((e->flags & G4MH_FPX_E) != 0u || (e->flags & enabled) != 0u) {
        fpsr &= ~G4MH_FPSR_XC_MASK;
        fpsr |= (e->flags << G4MH_FPSR_XC_SHIFT) & G4MH_FPSR_XC_MASK;
        c->sr[0][G4MH_SR_FPSR] = fpsr;
        return true;
    }
    fpsr |= (e->flags << G4MH_FPSR_XP_SHIFT) & G4MH_FPSR_XP_MASK;
    c->sr[0][G4MH_SR_FPSR] = fpsr;
    return false;
}

/* ------------------------------------------------------------------ */
/* The instruction interface                                           */
/* ------------------------------------------------------------------ */

/* reg1 of the one-operand groups, 0x44E and 0x45E. */
static bool unary_op(uint32_t reg1, unsigned *op)
{
    static const unsigned ops[3] = {G4MH_FOP_SQRT, G4MH_FOP_RECIP,
                                    G4MH_FOP_RSQRT};

    if (reg1 > 2u) {
        return false;
    }
    *op = ops[reg1];
    return true;
}

/* The two-operand groups, keyed on the .S sub-opcode. */
static bool binary_op(uint32_t sub, unsigned *op)
{
    switch (sub) {
    case 0x460u:
        *op = G4MH_FOP_ADD;
        return true;
    case 0x462u:
        *op = G4MH_FOP_SUB;
        return true;
    case 0x464u:
        *op = G4MH_FOP_MUL;
        return true;
    case 0x46Eu:
        *op = G4MH_FOP_DIV;
        return true;
    default:
        return false;
    }
}

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
 *
 * Operand order throughout: reg3 <- reg2 OP reg1, so the *second*
 * register field is the left operand. Getting this backwards is
 * invisible in add and multiply and wrong in subtract and divide, which
 * is the usual way an operand-order bug ships.
 */
g4mh_exc_t g4mh_fpu_exec(g4mh_cpu_t *c, uint32_t sub, uint32_t reg1,
                         uint32_t reg2, uint32_t reg3)
{
#if G4MH_EXT_FXU
    /* The FXU's 32-bit forms share this sub-opcode range and are gated
     * by CU1, not CU0. */
    if (g4mh_fxu_owns(sub)) {
        return g4mh_fxu_exec(c, sub, reg1, reg2, reg3);
    }
#endif
    /*
     * PSW.CU0 gates the whole coprocessor. A part without the FPU option
     * -- or a guest that has not enabled it -- gets a coprocessor-unusable
     * exception, which is what the hardware does and is distinguishable
     * from "this emulator does not implement it" (RIE).
     */
    if ((c->psw & G4MH_PSW_CU0) == 0u) {
        return G4MH_EXC_UCPOP;
    }

    const uint32_t a = c->r[reg1];
    const uint32_t b = c->r[reg2];
    const uint64_t da = d_get(c, reg1);
    const uint64_t db = d_get(c, reg2);
    g4mh_fp_t e;
    unsigned op;
    uint32_t res = 0u;
    uint64_t res64 = 0u;
    bool wide = false; /* the destination is a register pair */

    /*
     * TRFSR shares CMOVF.S's encoding and is told apart by all three
     * register fields being zero -- the same shape as CALLT hiding in
     * MOV imm5. Tested first, because CMOVF.S would otherwise consume it
     * and write r0, which discards the write and looks like it worked.
     */
    if ((sub & 0x7F1u) == 0x400u && reg1 == 0u && reg2 == 0u && reg3 == 0u) {
        const uint32_t cc = (c->sr[0][G4MH_SR_FPSR] >> G4MH_FPSR_CC_SHIFT);
        const uint32_t sel = (sub >> 1) & 7u;

        /* PSW.Z takes the selected CC bit -- the point of the
         * instruction, which is to get a comparison result out of FPSR
         * and into a place the integer conditional branches can see. */
        c->psw =
            (c->psw & ~G4MH_PSW_Z) | (((cc >> sel) & 1u) ? G4MH_PSW_Z : 0u);
        return G4MH_EXC_NONE;
    }

    g4mh_fp_begin(&e, c->sr[0][G4MH_SR_FPSR]);

    switch (sub) {
    /* ---- sign manipulation: no arithmetic, no flags, no flushing ---- */
    case 0x448u: /* ABSF.S / NEGF.S */
    case 0x458u: { /* ABSF.D / NEGF.D */
        /*
         * Pure sign operations, a NaN included ("the value obtained by
         * the operation on the sign bit alone", table 3.128 note 3), and
         * "a subnormal input will not be flushed even if FS is 1".
         */
        const bool d = (sub == 0x458u);
        const uint64_t s = d ? (UINT64_C(1) << 63) : 0x80000000u;
        const uint64_t v = d ? db : b;

        if (reg1 > 1u) {
            return G4MH_EXC_RIE;
        }
        if (d) {
            d_set(c, reg3, (reg1 == 0u) ? (v & ~s) : (v ^ s));
        } else {
            w_set(c, reg3, (uint32_t)((reg1 == 0u) ? (v & ~s) : (v ^ s)));
        }
        return G4MH_EXC_NONE;
    }

    /* ---- one operand, reg1 selects: SQRTF / RECIPF / RSQRTF ---- */
    case 0x44Eu:
        if (!unary_op(reg1, &op)) {
            return G4MH_EXC_RIE;
        }
        res = g4mh_fp32_op(&e, op, b, 0u, 0u);
        break;
    case 0x45Eu:
        if (!unary_op(reg1, &op)) {
            return G4MH_EXC_RIE;
        }
        res64 = g4mh_fp64_op(&e, op, db, 0u);
        wide = true;
        break;

    /* ---- two operands ---- */
    case 0x460u: /* ADDF.S */
    case 0x462u: /* SUBF.S */
    case 0x464u: /* MULF.S */
    case 0x46Eu: /* DIVF.S */
        (void)binary_op(sub, &op);
        res = g4mh_fp32_op(&e, op, b, a, 0u);
        break;
    case 0x470u: /* ADDF.D */
    case 0x472u: /* SUBF.D */
    case 0x474u: /* MULF.D */
    case 0x47Eu: /* DIVF.D */
        (void)binary_op(sub & ~0x10u, &op);
        res64 = g4mh_fp64_op(&e, op, db, da);
        wide = true;
        break;
    case 0x468u: /* MAXF.S */
    case 0x46Au: /* MINF.S */
        res = g4mh_fp32_maxmin(&e, sub == 0x468u, b, a);
        break;
    case 0x478u: /* MAXF.D */
    case 0x47Au: /* MINF.D */
        res64 = g4mh_fp64_maxmin(&e, sub == 0x478u, db, da);
        wide = true;
        break;

    /* ---- fused multiply-add: reg3 is read as well as written ---- */
    case 0x4E0u: /* FMAF.S   r3 = r2*r1 + r3    */
    case 0x4E2u: /* FMSF.S   r3 = r2*r1 - r3    */
    case 0x4E4u: /* FNMAF.S  r3 = -(r2*r1 + r3) */
    case 0x4E6u: { /* FNMSF.S  r3 = -(r2*r1 - r3) */
        static const unsigned fma[4] = {G4MH_FOP_FMA, G4MH_FOP_FMS,
                                        G4MH_FOP_FNMA, G4MH_FOP_FNMS};

        res = g4mh_fp32_op(&e, fma[(sub >> 1) & 3u], b, a, c->r[reg3]);
        break;
    }

    /*
     * ---- float to integer ----
     *
     * reg1 is the opcode extension *and* carries the signedness in bit 4:
     * 0x0n is the signed form and 0x1n the unsigned one, with n the
     * rounding -- 0 ROUNDF, 1 TRNCF, 2 CEILF, 3 FLOORF, 4 CVTF -- which
     * is G4MH_CVT_* by value. 0x440 and 0x450 put the result in one
     * register, 0x444 and 0x454 in a pair (.L/.UL); 0x45x read a double.
     */
    case 0x440u:
    case 0x444u:
    case 0x450u:
    case 0x454u: {
        const bool to64 = (sub & 4u) != 0u;
        const bool uns = (reg1 & 0x10u) != 0u;

        if ((reg1 & 0x0Fu) > G4MH_CVT_CUR || (reg1 & ~0x1Fu) != 0u) {
            return G4MH_EXC_RIE;
        }
        res64 = (sub & 0x10u)
                    ? g4mh_fp64_to_int(&e, db, reg1 & 0x0Fu, uns, to64)
                    : g4mh_fp32_to_int(&e, b, reg1 & 0x0Fu, uns, to64);
        res = (uint32_t)res64;
        wide = to64;
        break;
    }

    /*
     * ---- to single, from an integer or a half ----
     *
     * reg1: 0x00 word, 0x10 unsigned word, 0x01 long, 0x11 unsigned long,
     * 0x02 half (CVTF.HS, the low 16 bits of reg2), and 0x03 single to
     * half (CVTF.SH, zero-extended) -- the last two off CC-RH:
     *   cvtf.hs r6, r10  E2374254    cvtf.sh r6, r10  E3374254
     */
    case 0x442u:
        switch (reg1) {
        case 0x00u:
        case 0x10u:
        case 0x01u:
        case 0x11u:
            res = g4mh_int_to_fp32(&e, (reg1 & 1u) ? db : b, reg1 >= 0x10u,
                                   (reg1 & 1u) != 0u);
            break;
        case 0x02u:
            res = g4mh_fp16_to_fp32(&e, b & 0xFFFFu);
            break;
        case 0x03u:
            res = g4mh_fp32_to_fp16(&e, b);
            break;
        default:
            return G4MH_EXC_RIE;
        }
        break;

    /* ---- to double, from an integer or a single; and double to single ---- */
    case 0x452u:
        switch (reg1) {
        case 0x00u:
        case 0x10u:
        case 0x01u:
        case 0x11u:
            res64 = g4mh_int_to_fp64(&e, (reg1 & 1u) ? db : b, reg1 >= 0x10u,
                                     (reg1 & 1u) != 0u);
            wide = true;
            break;
        case 0x02u: /* CVTF.SD */
            res64 = g4mh_fp32_to_fp64(&e, b);
            wide = true;
            break;
        case 0x03u: /* CVTF.DS: the one member whose result is 32 bits */
            res = g4mh_fp64_to_fp32(&e, db);
            break;
        default:
            return G4MH_EXC_RIE;
        }
        break;

    default: {
        /*
         * CMPF and CMOVF, single and double, carry a CC bit number in
         * bits 3..1 of `sub` -- the "fff" field -- and the double forms
         * sit at the single ones with bit 4 set, as the arithmetic does.
         */
        const uint32_t fcbit = (sub >> 1) & 7u;
        const uint32_t ccbit = 1u << (G4MH_FPSR_CC_SHIFT + fcbit);

        if ((sub & 0x7E1u) == 0x420u) { /* CMPF.S, CMPF.D */
            /*
             * fcond is the *reg3 field*; fcbit is the `fff` bits of the
             * sub-opcode. That is the opposite of the obvious reading --
             * reg3 is the destination in every other instruction here,
             * and CMPF has no destination register -- and this file
             * shipped it the wrong way round. Settled against CC-RH:
             *
             *   cmpf.s 0x4, r6, r7, 3   E7372624  sub=0x426 reg3=4
             *   cmpf.s 0xC, r6, r7, 5   E7372A64  sub=0x42A reg3=12
             *
             * 0xC needs four bits, so fcond cannot be the three-bit
             * `fff`, and fcbit tracks `fff` across both.
             *
             * The CC bit is written only when no exception is taken:
             * "if invalid operation exceptions are enabled, the
             * comparison result is not set".
             */
            const bool r = (sub & 0x10u)
                               ? g4mh_fp64_cmp(&e, reg3 & 0xFu, db, da)
                               : g4mh_fp32_cmp(&e, reg3 & 0xFu, b, a);

            if (fpsr_commit(c, &e)) {
                return G4MH_EXC_FPP;
            }
            c->sr[0][G4MH_SR_FPSR] =
                (c->sr[0][G4MH_SR_FPSR] & ~ccbit) | (r ? ccbit : 0u);
            return G4MH_EXC_NONE;
        }
        if ((sub & 0x7E1u) == 0x400u) { /* CMOVF.S, CMOVF.D */
            /*
             * reg3 <- CC ? reg1 : reg2, and that direction is the whole
             * content of the instruction. It shipped inverted; settled
             * against CC-RH's code for `return (p < q) ? 11 : 22;`,
             *
             *   cmpf.s 0x4, r2, r5      ; reg2=r2=p, reg1=r5=q
             *   movea 22, r0, r2 ; mov 11, r5
             *   cmovf.s 0, r5, r2, r10  ; reg1=r5=11, reg2=r2=22
             *
             * where a set CC0 (1.0 < 2.0) wants 11, which is reg1.
             * A bit move: no flags, no flushing.
             */
            const bool taken = (c->sr[0][G4MH_SR_FPSR] & ccbit) != 0u;

            if (sub & 0x10u) {
                d_set(c, reg3, taken ? da : db);
            } else {
                w_set(c, reg3, taken ? a : b);
            }
            return G4MH_EXC_NONE;
        }
        return G4MH_EXC_RIE;
    }
    }

    /*
     * The destination is written only when no exception is taken; the
     * architecture leaves it untouched on one, and writing first would
     * need an undo that nothing else here has.
     */
    if (fpsr_commit(c, &e)) {
        return G4MH_EXC_FPP;
    }
    if (wide) {
        d_set(c, reg3, res64);
    } else {
        w_set(c, reg3, res);
    }
    return G4MH_EXC_NONE;
}

#endif /* G4MH_EXT_FPU */
