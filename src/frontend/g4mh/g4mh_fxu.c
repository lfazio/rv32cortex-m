/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_fxu.c - the G4MH extended floating-point unit (FXU): four ways of
 * single precision on 32 128-bit registers, wr0-wr31.
 *
 * R01US0209EJ0220 section 2.5 for the instructions, R01UH0923EJ0130
 * 3.2.3.5 and 3.2.6.2 for the registers and the exceptions. On the U2B6
 * only PE0 has one: PID bit 11, and PSW.CU1 fixed to 0 on the other PEs,
 * so there every instruction here is UCPOP 81H.
 *
 * Each way is one g4mh_fp.c element, the FPU's -- the manual states the
 * FXU's rules for NaNs, subnormals and flushing in the same words as the
 * FPU's, so they are the same code. What is the FXU's own is the shape
 * of the operands (the R, X, ADDSUB and N forms pair ways differently)
 * and how the four ways' flags are kept: each way's in FXXC or FXXP, and
 * their OR in FXSR. One exception for the instruction if any way raised
 * an enabled one or E, and then no way's result is written.
 *
 * Every encoding here was held against CC-RH (scripts/g4mh-check-
 * encodings.sh, 81 forms) before it was written: the bit diagrams were
 * read from the manual and agreed with the assembler throughout,
 * SHFLV.W4's immediate split (bit 11 in w1, bits 10:0 in w2) included.
 *
 * Unverified, because no RH850 is attached: anything g4mh_fp.c lists,
 * and that FXXC is rewritten whole by an instruction that takes FXE (the
 * manual says each way's cause "is set"), and that an undefined encoding
 * in the FXU's ranges is UCPOP before RIE while CU1 is clear, as the
 * FPU's are.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_types.h"

#include "g4mh_fp.h"

#include <string.h>

#if G4MH_EXT_FXU

#define SIGN32 0x80000000u

static bool is_nan32(uint32_t x)
{
    return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0u;
}

/*
 * The four ways' flags into FXSR, FXXC and FXXP; true if FXE is taken.
 *
 * As the FPU's (g4mh_fpu.c fpsr_commit): the cause bits change only on
 * an exception and then hold everything detected, enabled or not, and
 * the preservation bits accumulate otherwise -- here per way as well,
 * "the logical OR ... of all operation ways" in FXSR (3.2.6.2(5)).
 */
static bool fxsr_commit(g4mh_cpu_t *c, const g4mh_fp_t e[4])
{
    uint32_t fxsr = c->sr[G4MH_SELID_FXU][G4MH_SR_FXSR];
    const uint32_t enabled = (fxsr & G4MH_FPSR_XE_MASK) >> G4MH_FPSR_XE_SHIFT;
    uint32_t any = 0u;
    uint32_t per_way = 0u;
    bool flushed = false;

    for (unsigned n = 0u; n < 4u; n++) {
        any |= e[n].flags;
        per_way |= (e[n].flags & 0x3Fu) << (8u * n);
        flushed = flushed || e[n].flushed;
    }
    if (flushed) {
        fxsr |= G4MH_FPSR_IF;
    }
    if ((any & G4MH_FPX_E) != 0u || (any & enabled) != 0u) {
        fxsr &= ~G4MH_FPSR_XC_MASK;
        fxsr |= (any << G4MH_FPSR_XC_SHIFT) & G4MH_FPSR_XC_MASK;
        c->sr[G4MH_SELID_FXU][G4MH_SR_FXSR] = fxsr;
        c->sr[G4MH_SELID_FXU][G4MH_SR_FXXC] = per_way;
        return true;
    }
    fxsr |= any & G4MH_FPSR_XP_MASK;
    c->sr[G4MH_SELID_FXU][G4MH_SR_FXSR] = fxsr;
    c->sr[G4MH_SELID_FXU][G4MH_SR_FXXP] |= per_way & G4MH_FXXP_WRITABLE;
    return false;
}

/* ------------------------------------------------------------------ */
/* Format M: the 32-bit instructions                                   */
/* ------------------------------------------------------------------ */

/*
 * The one-operand group, sub-opcode 0x5A0, with the operation in the
 * wreg1 field (R01US0209EJ0220 2.5.4): 0x00-0x07 ROUNDF, TRNCF, CEILF and
 * FLOORF, signed and unsigned by bit 0; 0x08-0x0D the CVTFs; 0x10-0x14
 * ABSF, NEGF, SQRTF, RECIPF, RSQRTF; 0x18-0x1B FLPV by imm2; MOVV 0x1E;
 * TRFSRV 0x1F, with imm3 in the wreg3 field.
 */
static g4mh_exc_t unary(g4mh_cpu_t *c, uint32_t s5, uint32_t reg2,
                        g4mh_fp_t e[4], uint32_t r[4])
{
    const uint32_t *b = c->wr[reg2];

    if (s5 < 0x0Au) {
        /* To an integer: the rounding is s5 >> 1 (G4MH_CVT_RN, RZ, RP,
         * RM, and CVTF's current mode at 4), the unsigned form bit 0. */
        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = (uint32_t)g4mh_fp32_to_int(&e[n], b[n], s5 >> 1,
                                              (s5 & 1u) != 0u, false);
        }
        return G4MH_EXC_NONE;
    }
    switch (s5) {
    case 0x0Au: /* CVTF.WS4  */
    case 0x0Bu: /* CVTF.UWS4 */
        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = g4mh_int_to_fp32(&e[n], b[n], s5 == 0x0Bu, false);
        }
        return G4MH_EXC_NONE;
    case 0x0Cu: /* CVTF.HS4: h0, h2, h4, h6 -- each way's low half */
        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = g4mh_fp16_to_fp32(&e[n], b[n] & 0xFFFFu);
        }
        return G4MH_EXC_NONE;
    case 0x0Du: /* CVTF.SH4: zero-extended into each way */
        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = g4mh_fp32_to_fp16(&e[n], b[n]);
        }
        return G4MH_EXC_NONE;
    case 0x10u: /* ABSF.S4: the sign alone, a NaN included; no flags */
    case 0x11u: /* NEGF.S4 */
        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = (s5 == 0x10u) ? (b[n] & ~SIGN32) : (b[n] ^ SIGN32);
        }
        return G4MH_EXC_NONE;
    case 0x12u:
    case 0x13u:
    case 0x14u: {
        static const unsigned op[3] = {G4MH_FOP_SQRT, G4MH_FOP_RECIP,
                                       G4MH_FOP_RSQRT};

        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = g4mh_fp32_op(&e[n], op[s5 - 0x12u], b[n], 0u, 0u);
        }
        return G4MH_EXC_NONE;
    }
    case 0x18u:
    case 0x19u:
    case 0x1Au:
    case 0x1Bu: {
        /*
         * FLPV.S4: imm2 bit 0 swaps each pair of ways, and bit 1 says
         * which way of the pair is negated -- the even one (w2, w0) for
         * 0 and 1, the odd one for 2 and 3. A sign flip, no flags.
         */
        const uint32_t imm2 = s5 & 3u;
        const uint32_t neg_odd = (imm2 & 2u) != 0u;

        for (unsigned n = 0u; n < 4u; n++) {
            const uint32_t v = b[(imm2 & 1u) ? (n ^ 1u) : n];

            r[n] = ((n & 1u) == neg_odd) ? (v ^ SIGN32) : v;
        }
        return G4MH_EXC_NONE;
    }
    case 0x1Eu: /* MOVV.W4 */
        memcpy(r, b, 16u);
        return G4MH_EXC_NONE;
    default:
        return G4MH_EXC_RIE;
    }
}

/*
 * The two- and three-operand group: reg3 <- reg2 OP reg1, way by way,
 * except where the form pairs them otherwise. `x` is the wreg2 operand,
 * `y` the wreg1 one, so a quiet NaN's priority (wreg2, wreg1, wreg3) and
 * MAXF's and MINF's choice between zeros come out of the element.
 *
 *   plain   x = B[n],      y = A[n]
 *   R       x = S[n | 1],  y = S[n & ~1], S = B for odd n, A for even:
 *           w3 = B3 . B2, w2 = A3 . A2, w1 = B1 . B0, w0 = A1 . A0
 *   X       x = B[n],      y = A[n ^ 1]
 *
 * ADDSUB adds in the odd ways and subtracts in the even, SUBADD the
 * other way round; N negates each result that is not a NaN, as FNMAF.
 */
enum { FORM_PLAIN, FORM_R, FORM_X };

static bool three_op(uint32_t sub, unsigned *form, unsigned *op_odd,
                     unsigned *op_even, bool *neg, bool *maxmin, bool *max)
{
    *form = FORM_PLAIN;
    *neg = false;
    *maxmin = false;
    *max = false;
    if (sub >= 0x5D0u && sub <= 0x5DEu && (sub & 1u) == 0u) {
        const bool subadd = (sub & 2u) != 0u;

        *form = (sub & 4u) ? FORM_X : FORM_PLAIN;
        *neg = (sub & 8u) != 0u;
        *op_odd = subadd ? G4MH_FOP_SUB : G4MH_FOP_ADD;
        *op_even = subadd ? G4MH_FOP_ADD : G4MH_FOP_SUB;
        return true;
    }
    switch (sub) {
    case 0x5B4u: /* ADDRF.S4 */
    case 0x5B6u: /* SUBRF.S4 */
    case 0x5B8u: /* MULRF.S4 */
    case 0x5BAu: /* MAXRF.S4 */
    case 0x5BCu: /* MINRF.S4 */
        *form = FORM_R;
        sub -= 0x10u;
        break;
    case 0x5C4u: /* ADDXF.S4 */
    case 0x5C6u: /* SUBXF.S4 */
    case 0x5C8u: /* MULXF.S4 */
        *form = FORM_X;
        sub -= 0x20u;
        break;
    default:
        break;
    }
    switch (sub) {
    case 0x5A4u:
        *op_odd = G4MH_FOP_ADD;
        break;
    case 0x5A6u:
        *op_odd = G4MH_FOP_SUB;
        break;
    case 0x5A8u:
        *op_odd = G4MH_FOP_MUL;
        break;
    case 0x5AEu:
        if (*form != FORM_PLAIN) {
            return false; /* there is no DIVRF or DIVXF */
        }
        *op_odd = G4MH_FOP_DIV;
        break;
    case 0x5AAu:
    case 0x5ACu:
        if (*form == FORM_X) {
            return false;
        }
        *maxmin = true;
        *max = (sub == 0x5AAu);
        *op_odd = 0u;
        break;
    case 0x4C0u: /* FMAF.S4 */
    case 0x4C2u: /* FMSF.S4 */
    case 0x4C4u: /* FNMAF.S4 */
    case 0x4C6u: { /* FNMSF.S4 */
        static const unsigned fma[4] = {G4MH_FOP_FMA, G4MH_FOP_FMS,
                                        G4MH_FOP_FNMA, G4MH_FOP_FNMS};

        *op_odd = fma[(sub >> 1) & 3u];
        break;
    }
    default:
        return false;
    }
    *op_even = *op_odd;
    return true;
}

EMU_NOINLINE g4mh_exc_t g4mh_fxu_exec(g4mh_cpu_t *c, uint32_t sub,
                                      uint32_t reg1, uint32_t reg2,
                                      uint32_t reg3)
{
    g4mh_fp_t e[4];
    uint32_t r[4];

    if ((c->psw & G4MH_PSW_CU1) == 0u) {
        return G4MH_EXC_UCPOP + 1u;
    }
    for (unsigned n = 0u; n < 4u; n++) {
        g4mh_fp_begin(&e[n], c->sr[G4MH_SELID_FXU][G4MH_SR_FXSR]);
    }

    if (sub == 0x5A0u) {
        if (reg1 == 0x1Fu) {
            /*
             * TRFSRV.W4 imm3, wreg2: PSW.Z from the ways being non-zero
             * -- one of them by imm3 0-3, all of them at 4, any at 5.
             * imm3 sits in the wreg3 field, whose top two bits are 0.
             */
            const uint32_t *b = c->wr[reg2];
            bool z;

            if (reg3 > 5u) {
                return G4MH_EXC_RIE;
            }
            if (reg3 < 4u) {
                z = b[reg3] != 0u;
            } else if (reg3 == 4u) {
                z = b[0] != 0u && b[1] != 0u && b[2] != 0u && b[3] != 0u;
            } else {
                z = b[0] != 0u || b[1] != 0u || b[2] != 0u || b[3] != 0u;
            }
            c->psw = (c->psw & ~G4MH_PSW_Z) | (z ? G4MH_PSW_Z : 0u);
            return G4MH_EXC_NONE;
        }
        {
            const g4mh_exc_t x = unary(c, reg1, reg2, e, r);

            if (x != G4MH_EXC_NONE) {
                return x;
            }
        }
    } else if ((sub & 0x7E1u) == 0x580u) {
        /*
         * CMPF.S4 fcond, wreg1, wreg2, wreg3: fcond in the sub-opcode's
         * bits 4:1 -- unlike CMPF.S, whose fcond is the reg3 field --
         * and an all-ones or all-zeros mask per way into wreg3.
         */
        const uint32_t fcond = (sub >> 1) & 0xFu;

        for (unsigned n = 0u; n < 4u; n++) {
            r[n] = g4mh_fp32_cmp(&e[n], fcond, c->wr[reg2][n], c->wr[reg1][n])
                       ? 0xFFFFFFFFu
                       : 0u;
        }
    } else {
        const uint32_t *a = c->wr[reg1];
        const uint32_t *b = c->wr[reg2];
        const uint32_t *acc = c->wr[reg3];
        unsigned form, op_odd, op_even;
        bool neg, mm, max;

        if (!three_op(sub, &form, &op_odd, &op_even, &neg, &mm, &max)) {
            return G4MH_EXC_RIE;
        }
        for (unsigned n = 0u; n < 4u; n++) {
            const uint32_t *s = (n & 1u) ? b : a;
            const uint32_t x = (form == FORM_R) ? s[n | 1u] : b[n];
            const uint32_t y = (form == FORM_R)   ? s[n & ~1u]
                               : (form == FORM_X) ? a[n ^ 1u]
                                                  : a[n];

            if (mm) {
                r[n] = g4mh_fp32_maxmin(&e[n], max, x, y);
            } else {
                r[n] = g4mh_fp32_op(&e[n], (n & 1u) ? op_odd : op_even, x, y,
                                    acc[n]);
                if (neg && !is_nan32(r[n])) {
                    r[n] ^= SIGN32;
                }
            }
        }
    }

    if (fxsr_commit(c, e)) {
        return G4MH_EXC_FXE;
    }
    memcpy(c->wr[reg3], r, 16u);
    return G4MH_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* The 48-bit forms                                                    */
/* ------------------------------------------------------------------ */

/*
 * w0 = 00000 111101 R (reg1, or wreg1), w1 = wreg3 kkkkkk 11101, and w2
 * the displacement or a third and fourth register. k = w1[10:5]:
 *
 *   00iiii LDV.W imm4     0100ii STV.W imm2    010100 LDV.QW
 *   010101 STV.QW         0110ii LDV.DW imm2   01110i STV.DW imm1
 *   011110 LDVZ.H4        011111 STVZ.H4       110000 CMOVF.W4
 *   11010I SHFLV.W4 (imm12 bit 11)
 *
 * The displacement is disp16 with its low bits fixed to zero by the
 * access size -- two for a word, three for a double-word, four for a
 * quad-word -- the same shape as the disp23 forms' required-zero bits,
 * and the same answer when they are not: RIE, the bits being opcode.
 */
EMU_NOINLINE g4mh_exc_t g4mh_fxu_exec48(g4mh_cpu_t *c, uint32_t w0,
                                        uint32_t w1, uint32_t w2)
{
    const uint32_t r1 = w0 & 0x1Fu;
    const uint32_t w3r = (w1 >> 11) & 0x1Fu;
    const uint32_t k = (w1 >> 5) & 0x3Fu;
    uint32_t *const v = c->wr[w3r];
    uint32_t lo, hi;
    g4mh_exc_t x;
    unsigned zero_bits;

    if ((c->psw & G4MH_PSW_CU1) == 0u) {
        return G4MH_EXC_UCPOP + 1u;
    }

    if (k == 0x30u || (k & 0x3Eu) == 0x34u) {
        const uint32_t *a = c->wr[r1];
        const uint32_t *b = c->wr[(w2 >> 11) & 0x1Fu];
        uint32_t r[4];

        if (k == 0x30u) {
            /* CMOVF.W4 wreg4, wreg1, wreg2, wreg3: wreg1's way where
             * wreg4's is non-zero. w2 bits 10:5 are ignored, "an RIE
             * exception does not occur in such cases". */
            const uint32_t *m = c->wr[w2 & 0x1Fu];

            for (unsigned n = 0u; n < 4u; n++) {
                r[n] = (m[n] != 0u) ? a[n] : b[n];
            }
        } else {
            /* SHFLV.W4 imm12: three bits a way, 7-4 picking wreg2's
             * w3-w0 and 3-0 wreg1's. */
            const uint32_t imm12 = ((k & 1u) << 11) | (w2 & 0x7FFu);

            for (unsigned n = 0u; n < 4u; n++) {
                const uint32_t sel = (imm12 >> (3u * n)) & 7u;

                r[n] = (sel & 4u) ? b[sel & 3u] : a[sel];
            }
        }
        memcpy(v, r, 16u);
        return G4MH_EXC_NONE;
    }

    zero_bits = ((k & 0x30u) == 0u || (k & 0x3Cu) == 0x10u) ? 2u
                : ((k & 0x3Eu) == 0x14u)                    ? 4u
                : (k >= 0x18u && k <= 0x1Fu)                ? 3u
                                                            : 0u;
    if (zero_bits == 0u) {
        return G4MH_EXC_RIE;
    }
    if ((w2 & ((1u << zero_bits) - 1u)) != 0u) {
        return G4MH_EXC_RIE;
    }
    {
        const uint32_t addr = c->r[r1] + (uint32_t)emu_sext(w2, 16);

        if ((k & 0x30u) == 0u) { /* LDV.W */
            x = g4mh_load(c, addr, 4u, false, &lo);
            if (x == G4MH_EXC_NONE) {
                for (unsigned n = 0u; n < 4u; n++) {
                    if ((k >> n) & 1u) {
                        v[n] = lo;
                    }
                }
            }
            return x;
        }
        if ((k & 0x3Cu) == 0x10u) { /* STV.W */
            return g4mh_store(c, addr, 4u, v[k & 3u]);
        }
        if (k == 0x14u) { /* LDV.QW */
            uint32_t q[4];

            x = g4mh_load_qw(c, addr, q);
            if (x == G4MH_EXC_NONE) {
                memcpy(v, q, 16u);
            }
            return x;
        }
        if (k == 0x15u) { /* STV.QW */
            return g4mh_store_qw(c, addr, v);
        }
        if ((k & 0x3Cu) == 0x18u) { /* LDV.DW: dw1 by imm2 bit 1, dw0 by 0 */
            x = g4mh_load_dw(c, addr, &lo, &hi);
            if (x == G4MH_EXC_NONE) {
                if (k & 2u) {
                    v[2] = lo;
                    v[3] = hi;
                }
                if (k & 1u) {
                    v[0] = lo;
                    v[1] = hi;
                }
            }
            return x;
        }
        if ((k & 0x3Eu) == 0x1Cu) { /* STV.DW: dw1 or dw0 by imm1 */
            return (k & 1u) ? g4mh_store_dw(c, addr, v[2], v[3])
                            : g4mh_store_dw(c, addr, v[0], v[1]);
        }
        if (k == 0x1Eu) { /* LDVZ.H4: four halves, each zero-extended */
            x = g4mh_load_dw(c, addr, &lo, &hi);
            if (x == G4MH_EXC_NONE) {
                v[0] = lo & 0xFFFFu;
                v[1] = lo >> 16;
                v[2] = hi & 0xFFFFu;
                v[3] = hi >> 16;
            }
            return x;
        }
        /* STVZ.H4: h6 || h4 || h2 || h0, each way's low half */
        return g4mh_store_dw(c, addr, (v[0] & 0xFFFFu) | (v[1] << 16),
                             (v[2] & 0xFFFFu) | (v[3] << 16));
    }
}

#endif /* G4MH_EXT_FXU */
