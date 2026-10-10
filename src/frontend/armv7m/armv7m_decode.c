/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_decode.c - the field rules, and the decoder built on them.
 *
 * Organised as the ARM ARM's chapters A5 and A6 are, one function per
 * encoding table -- the same shape as armv7m_interp.c, deliberately, so
 * the two can be read side by side. See armv7m_decode.h for why there are
 * two and what keeps them honest.
 */

#include "armv7m/armv7m_decode.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

const char *armv7m_op_name(uint32_t op)
{
    static const char *const k_names[ARMV7M_OP_COUNT] = {
#define X(n) #n,
        ARMV7M_OPS(X)
#undef X
    };

    return (op < ARMV7M_OP_COUNT) ? k_names[op] : "?";
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

typedef armv7m_insn_t I;

static void set(I *d, uint32_t op)
{
    d->op = (uint16_t)op;
}

/* The sixteen data-processing operations, by their four-bit selector. */
static const uint16_t k_dp[16] = {
    ARMV7M_OP_AND,   ARMV7M_OP_BIC,   ARMV7M_OP_ORR, ARMV7M_OP_ORN,
    ARMV7M_OP_EOR,   ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF,
    ARMV7M_OP_ADD,   ARMV7M_OP_UNDEF, ARMV7M_OP_ADC, ARMV7M_OP_SBC,
    ARMV7M_OP_UNDEF, ARMV7M_OP_SUB,   ARMV7M_OP_RSB, ARMV7M_OP_UNDEF,
};

/*
 * Resolve the data-processing aliases: S with rd == pc is TST, TEQ, CMN
 * or CMP, and rn == pc turns ORR into MOV and ORN into MVN.
 */
static void dp_finish(I *d, uint32_t sel, uint32_t rd, uint32_t rn, bool s)
{
    uint32_t op = k_dp[sel];

    d->rd = (uint8_t)rd;
    d->rn = (uint8_t)rn;
    d->setflags = s ? ARMV7M_SF_YES : ARMV7M_SF_NO;
    if (s && rd == 15u) {
        switch (sel) {
        case 0u:
            op = ARMV7M_OP_TST;
            break;
        case 4u:
            op = ARMV7M_OP_TEQ;
            break;
        case 8u:
            op = ARMV7M_OP_CMN;
            break;
        case 13u:
            op = ARMV7M_OP_CMP;
            break;
        default:
            op = ARMV7M_OP_UNDEF;
            break;
        }
        d->rd = ARMV7M_NOREG;
    } else if (rn == 15u && sel == 2u) {
        op = ARMV7M_OP_MOV;
        d->rn = ARMV7M_NOREG;
    } else if (rn == 15u && sel == 3u) {
        op = ARMV7M_OP_MVN;
        d->rn = ARMV7M_NOREG;
    }
    set(d, op);
}

/* ------------------------------------------------------------------ */
/* 16-bit                                                              */
/* ------------------------------------------------------------------ */

static void d16(uint16_t w, I *d)
{
    const uint32_t op6 = w >> 10;

    d->setflags = ARMV7M_SF_NO;
    if ((op6 >> 4) == 0u) { /* shift, add, subtract, move, compare */
        const uint32_t op = (w >> 9) & 31u;

        d->setflags = ARMV7M_SF_NOT_IN_IT;
        if (op < 12u) {
            uint32_t t;
            uint32_t n;

            armv7m_decode_imm_shift(op >> 2, (w >> 6) & 31u, &t, &n);
            d->rd = w & 7u;
            d->rn = (w >> 3) & 7u;
            if (t == ARMV7M_SH_LSL && n == 0u) {
                set(d, ARMV7M_OP_MOV); /* MOVS rd, rm */
                d->rm = d->rn;
                d->rn = ARMV7M_NOREG;
                d->opnd = ARMV7M_OPND_REG;
                return;
            }
            set(d, ARMV7M_OP_LSL + t);
            d->opnd = ARMV7M_OPND_IMM;
            d->imm = n;
            return;
        }
        if (op < 16u) {
            set(d, (op & 1u) ? ARMV7M_OP_SUB : ARMV7M_OP_ADD);
            d->rd = w & 7u;
            d->rn = (w >> 3) & 7u;
            if (op >= 14u) {
                d->opnd = ARMV7M_OPND_IMM;
                d->imm = (w >> 6) & 7u;
            } else {
                d->opnd = ARMV7M_OPND_REG;
                d->rm = (w >> 6) & 7u;
            }
            return;
        }
        {
            static const uint16_t k[4] = {ARMV7M_OP_MOV, ARMV7M_OP_CMP,
                                          ARMV7M_OP_ADD, ARMV7M_OP_SUB};
            const uint32_t sel = (op >> 2) & 3u;
            const uint32_t r = (w >> 8) & 7u;

            set(d, k[sel]);
            d->opnd = ARMV7M_OPND_IMM;
            d->imm = w & 0xFFu;
            if (sel == 0u) {
                d->rd = (uint8_t)r;
            } else if (sel == 1u) {
                d->rn = (uint8_t)r;
                d->setflags = ARMV7M_SF_YES;
            } else {
                d->rd = (uint8_t)r;
                d->rn = (uint8_t)r;
            }
            return;
        }
    }
    if (op6 == 0x10u) { /* data processing */
        static const uint16_t k[16] = {
            ARMV7M_OP_AND, ARMV7M_OP_EOR, ARMV7M_OP_LSL, ARMV7M_OP_LSR,
            ARMV7M_OP_ASR, ARMV7M_OP_ADC, ARMV7M_OP_SBC, ARMV7M_OP_ROR,
            ARMV7M_OP_TST, ARMV7M_OP_RSB, ARMV7M_OP_CMP, ARMV7M_OP_CMN,
            ARMV7M_OP_ORR, ARMV7M_OP_MUL, ARMV7M_OP_BIC, ARMV7M_OP_MVN,
        };
        const uint32_t op = (w >> 6) & 15u;
        const uint32_t rm = (w >> 3) & 7u;
        const uint32_t rdn = w & 7u;

        set(d, k[op]);
        d->setflags = ARMV7M_SF_NOT_IN_IT;
        d->rd = (uint8_t)rdn;
        d->rn = (uint8_t)rdn;
        d->rm = (uint8_t)rm;
        d->opnd = ARMV7M_OPND_REG;
        switch (op) {
        case 2u:
        case 3u:
        case 4u:
        case 7u:
            d->opnd = ARMV7M_OPND_REGSHIFT;
            break;
        case 8u:
        case 10u:
        case 11u:
            d->rd = ARMV7M_NOREG;
            d->setflags = ARMV7M_SF_YES;
            break;
        case 9u: /* RSB rd, rm, #0 */
            d->rn = (uint8_t)rm;
            d->rm = ARMV7M_NOREG;
            d->opnd = ARMV7M_OPND_IMM;
            d->imm = 0u;
            break;
        case 13u: /* MUL rd, rm, rd */
            d->rn = (uint8_t)rm;
            d->rm = (uint8_t)rdn;
            break;
        case 15u:
            d->rn = ARMV7M_NOREG;
            break;
        default:
            break;
        }
        return;
    }
    if (op6 == 0x11u) { /* special data, branch and exchange */
        const uint32_t op = (w >> 6) & 15u;
        const uint32_t rm = (w >> 3) & 15u;
        const uint32_t rd = (w & 7u) | ((w >> 4) & 8u);

        d->rm = (uint8_t)rm;
        d->opnd = ARMV7M_OPND_REG;
        switch (op >> 2) {
        case 0u:
            set(d, ARMV7M_OP_ADD);
            d->rd = (uint8_t)rd;
            d->rn = (uint8_t)rd;
            return;
        case 1u:
            set(d, ARMV7M_OP_CMP);
            d->rn = (uint8_t)rd;
            d->setflags = ARMV7M_SF_YES;
            return;
        case 2u:
            set(d, ARMV7M_OP_MOV);
            d->rd = (uint8_t)rd;
            return;
        default:
            if ((w & 7u) != 0u) {
                return;
            }
            set(d, (op & 2u) ? ARMV7M_OP_BLX : ARMV7M_OP_BX);
            d->opnd = ARMV7M_OPND_NONE;
            return;
        }
    }
    if ((op6 >> 1) == 0x09u) { /* LDR (literal) */
        set(d, ARMV7M_OP_LDR);
        d->rd = (w >> 8) & 7u;
        d->rn = 15u;
        d->addr = ARMV7M_ADDR_LITERAL;
        d->imm = (w & 0xFFu) * 4u;
        d->index = 1u;
        d->add = 1u;
        d->size = 4u;
        return;
    }
    if ((op6 >> 2) == 0x05u) { /* load/store, register offset */
        static const uint16_t k[8] = {
            ARMV7M_OP_STR, ARMV7M_OP_STRH, ARMV7M_OP_STRB, ARMV7M_OP_LDRSB,
            ARMV7M_OP_LDR, ARMV7M_OP_LDRH, ARMV7M_OP_LDRB, ARMV7M_OP_LDRSH,
        };
        static const uint8_t k_size[8] = {4u, 2u, 1u, 1u, 4u, 2u, 1u, 2u};
        const uint32_t opb = (w >> 9) & 7u;

        set(d, k[opb]);
        d->rd = w & 7u;
        d->rn = (w >> 3) & 7u;
        d->rm = (w >> 6) & 7u;
        d->addr = ARMV7M_ADDR_REG;
        d->index = 1u;
        d->add = 1u;
        d->size = k_size[opb];
        d->sext = (opb == 3u) || (opb == 7u);
        return;
    }
    if ((op6 >> 3) == 0x03u || (op6 >> 2) == 0x08u) { /* imm5 */
        const uint32_t opa = w >> 12;
        const bool load = ((w >> 11) & 1u) != 0u;
        const uint32_t size = (opa == 6u) ? 4u : ((opa == 7u) ? 1u : 2u);

        set(d, load ? ((size == 4u) ? ARMV7M_OP_LDR
                                    : ((size == 1u) ? ARMV7M_OP_LDRB : ARMV7M_OP_LDRH))
                    : ((size == 4u) ? ARMV7M_OP_STR
                                    : ((size == 1u) ? ARMV7M_OP_STRB : ARMV7M_OP_STRH)));
        d->rd = w & 7u;
        d->rn = (w >> 3) & 7u;
        d->addr = ARMV7M_ADDR_IMM;
        d->imm = ((w >> 6) & 31u) * size;
        d->index = 1u;
        d->add = 1u;
        d->size = (uint8_t)size;
        return;
    }
    if ((op6 >> 2) == 0x09u) { /* sp-relative */
        set(d, ((w >> 11) & 1u) ? ARMV7M_OP_LDR : ARMV7M_OP_STR);
        d->rd = (w >> 8) & 7u;
        d->rn = 13u;
        d->addr = ARMV7M_ADDR_IMM;
        d->imm = (w & 0xFFu) * 4u;
        d->index = 1u;
        d->add = 1u;
        d->size = 4u;
        return;
    }
    if ((op6 >> 1) == 0x14u) { /* ADR */
        set(d, ARMV7M_OP_ADR);
        d->rd = (w >> 8) & 7u;
        d->imm = (w & 0xFFu) * 4u;
        d->add = 1u;
        return;
    }
    if ((op6 >> 1) == 0x15u) { /* ADD rd, sp, #imm */
        set(d, ARMV7M_OP_ADD);
        d->rd = (w >> 8) & 7u;
        d->rn = 13u;
        d->opnd = ARMV7M_OPND_IMM;
        d->imm = (w & 0xFFu) * 4u;
        return;
    }
    if ((op6 >> 2) == 0x0Bu) { /* miscellaneous */
        const uint32_t op = (w >> 5) & 0x7Fu;

        if (op == 0x33u) {
            set(d, ((w >> 4) & 1u) ? ARMV7M_OP_CPSID : ARMV7M_OP_CPSIE);
            d->imm = w & 3u;
            return;
        }
        if ((op >> 3) == 0u) {
            set(d, (w & 0x80u) ? ARMV7M_OP_SUB : ARMV7M_OP_ADD);
            d->rd = 13u;
            d->rn = 13u;
            d->opnd = ARMV7M_OPND_IMM;
            d->imm = (w & 0x7Fu) * 4u;
            return;
        }
        if ((w & 0x0500u) == 0x0100u) {
            set(d, ((w >> 11) & 1u) ? ARMV7M_OP_CBNZ : ARMV7M_OP_CBZ);
            d->rn = w & 7u;
            d->imm = (((w >> 3) & 31u) << 1) | (((w >> 9) & 1u) << 6);
            return;
        }
        if ((w & 0xFF00u) == 0xB200u) {
            static const uint16_t k[4] = {ARMV7M_OP_SXTH, ARMV7M_OP_SXTB,
                                          ARMV7M_OP_UXTH, ARMV7M_OP_UXTB};

            set(d, k[(w >> 6) & 3u]);
            d->rd = w & 7u;
            d->rm = (w >> 3) & 7u;
            return;
        }
        if ((w & 0xFF00u) == 0xBA00u) {
            static const uint16_t k[4] = {ARMV7M_OP_REV, ARMV7M_OP_REV16,
                                          ARMV7M_OP_UNDEF, ARMV7M_OP_REVSH};

            set(d, k[(w >> 6) & 3u]);
            d->rd = w & 7u;
            d->rm = (w >> 3) & 7u;
            return;
        }
        if ((w & 0x0600u) == 0x0400u) {
            const bool pop = ((w >> 11) & 1u) != 0u;

            d->imm = w & 0xFFu;
            if ((w & 0x0100u) != 0u) {
                d->imm |= pop ? (1u << 15) : (1u << 14);
            }
            if (d->imm == 0u) {
                return;
            }
            set(d, pop ? ARMV7M_OP_POP : ARMV7M_OP_PUSH);
            d->rn = 13u;
            d->wback = 1u;
            return;
        }
        if ((w & 0xFF00u) == 0xBE00u) {
            set(d, ARMV7M_OP_BKPT);
            d->imm = w & 0xFFu;
            return;
        }
        if ((w & 0xFF00u) == 0xBF00u) {
            static const uint16_t k[5] = {ARMV7M_OP_NOP, ARMV7M_OP_YIELD,
                                          ARMV7M_OP_WFE, ARMV7M_OP_WFI,
                                          ARMV7M_OP_SEV};
            const uint32_t mask = w & 15u;
            const uint32_t first = (w >> 4) & 15u;

            if (mask != 0u) {
                if (first == 15u ||
                    (first == 14u && (mask & 7u) != 0u && (mask & 0xEu) != 0xAu)) {
                    return;
                }
                set(d, ARMV7M_OP_IT);
                d->cond = (uint8_t)first;
                d->imm = mask;
                return;
            }
            set(d, (first < 5u) ? k[first] : ARMV7M_OP_NOP);
            d->imm = first;
            return;
        }
        return;
    }
    if ((op6 >> 1) == 0x18u || (op6 >> 1) == 0x19u) { /* STM / LDM */
        const bool load = ((w >> 11) & 1u) != 0u;
        const uint32_t rn = (w >> 8) & 7u;

        d->imm = w & 0xFFu;
        if (d->imm == 0u) {
            return;
        }
        set(d, load ? ARMV7M_OP_LDM : ARMV7M_OP_STM);
        d->rn = (uint8_t)rn;
        d->wback = (!load || (d->imm & (1u << rn)) == 0u) ? 1u : 0u;
        return;
    }
    if ((op6 >> 2) == 0x0Du) { /* B<cond>, UDF, SVC */
        const uint32_t cond = (w >> 8) & 15u;

        if (cond == 15u) {
            set(d, ARMV7M_OP_SVC);
            d->imm = w & 0xFFu;
            return;
        }
        if (cond == 14u) {
            set(d, ARMV7M_OP_UDF);
            d->imm = w & 0xFFu;
            return;
        }
        set(d, ARMV7M_OP_BCC);
        d->cond = (uint8_t)cond;
        d->imm = (uint32_t)((int32_t)(int8_t)(w & 0xFFu) * 2);
        return;
    }
    if ((op6 >> 1) == 0x1Cu) { /* B */
        int32_t off = (int32_t)((uint32_t)(w & 0x7FFu) << 1);

        if ((off & 0x800) != 0) {
            off -= 0x1000;
        }
        set(d, ARMV7M_OP_B);
        d->imm = (uint32_t)off;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 32-bit                                                              */
/* ------------------------------------------------------------------ */

/*
 * **A should-be bit that is not, is UNDEFINED here.** The manual writes
 * such bits as (0) and (1) and calls getting them wrong UNPREDICTABLE,
 * which is permission for the core to do anything. What a Cortex-M7
 * does is decode them strictly and raise UNDEFINSTR, for every one the
 * board could be asked about -- and likewise for RdLo == RdHi on a long
 * multiply and for a D register a sixteen-register bank does not have.
 *
 * That was not guessed and could not have been read: it came from
 * running encodings, rather than instructions, on the board
 * (tests/armv7m-diff/rawgen.py). Each check below is one such answer.
 */

static void d32_dp_plain(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op = (w0 >> 4) & 31u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rd = (w1 >> 8) & 15u;
    const uint32_t imm3 = (w1 >> 12) & 7u;
    const uint32_t imm2 = (w1 >> 6) & 3u;
    const uint32_t i = (w0 >> 10) & 1u;
    const uint32_t imm12 = (i << 11) | (imm3 << 8) | (w1 & 0xFFu);
    const uint32_t lsb = (imm3 << 2) | imm2;
    const uint32_t field = w1 & 31u;

    d->rd = (uint8_t)rd;
    d->rn = (uint8_t)rn;
    switch (op) {
    case 0x00u:
    case 0x0Au:
        if (rn == 15u) {
            set(d, ARMV7M_OP_ADR);
            d->rn = ARMV7M_NOREG;
            d->add = (op == 0u) ? 1u : 0u;
            d->imm = imm12;
            d->wide = 1u;
            return;
        }
        set(d, (op == 0u) ? ARMV7M_OP_ADD : ARMV7M_OP_SUB);
        d->opnd = ARMV7M_OPND_IMM;
        d->imm = imm12;
        d->imm2 = 1u; /* the plain 12-bit form: ADDW, SUBW */
        return;
    case 0x04u:
    case 0x0Cu:
        set(d, (op == 4u) ? ARMV7M_OP_MOVW : ARMV7M_OP_MOVT);
        d->rn = ARMV7M_NOREG;
        d->imm = ((uint32_t)(w0 & 15u) << 12) | imm12;
        return;
    default:
        break;
    }
    if (i != 0u) {
        return;
    }
    switch (op) {
    case 0x10u:
    case 0x12u:
    case 0x18u:
    case 0x1Au: {
        const bool uns = (op & 8u) != 0u;
        const bool asr = (op & 2u) != 0u;

        if ((w1 & 0x0020u) != 0u || (asr && lsb == 0u && (w1 & 0x0010u) != 0u)) {
            return; /* (0) */
        }
        if (asr && lsb == 0u) {
            set(d, uns ? ARMV7M_OP_USAT16 : ARMV7M_OP_SSAT16);
            d->imm = uns ? (field & 15u) : ((field & 15u) + 1u);
            return;
        }
        set(d, uns ? ARMV7M_OP_USAT : ARMV7M_OP_SSAT);
        d->imm = uns ? field : field + 1u;
        d->shift_t = asr ? ARMV7M_SH_ASR : ARMV7M_SH_LSL;
        d->shift_n = (uint8_t)lsb;
        return;
    }
    case 0x14u:
    case 0x1Cu:
        if (lsb + field + 1u > 32u || (w1 & 0x0020u) != 0u) {
            return;
        }
        set(d, (op == 0x14u) ? ARMV7M_OP_SBFX : ARMV7M_OP_UBFX);
        d->imm = lsb;
        d->imm2 = field + 1u;
        return;
    case 0x16u:
        if (field < lsb || (w1 & 0x0020u) != 0u) {
            return;
        }
        set(d, (rn == 15u) ? ARMV7M_OP_BFC : ARMV7M_OP_BFI);
        if (rn == 15u) {
            d->rn = ARMV7M_NOREG;
        }
        d->imm = lsb;
        d->imm2 = field - lsb + 1u;
        return;
    default:
        return;
    }
}

static void d32_dp_reg(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t sel = (w0 >> 5) & 15u;
    const uint32_t imm5 = (((uint32_t)(w1 >> 12) & 7u) << 2) | ((w1 >> 6) & 3u);
    uint32_t t;
    uint32_t n;

    if ((w1 & 0x8000u) != 0u) {
        return; /* (0) */
    }
    d->rm = w1 & 15u;
    if (sel == 6u) {
        const bool tb = ((w1 >> 5) & 1u) != 0u;

        if (((w0 >> 4) & 1u) != 0u || ((w1 >> 4) & 1u) != 0u) {
            return; /* PKH has no S, and its T bit is zero */
        }

        set(d, tb ? ARMV7M_OP_PKHTB : ARMV7M_OP_PKHBT);
        d->rd = (w1 >> 8) & 15u;
        d->rn = w0 & 15u;
        d->shift_t = tb ? ARMV7M_SH_ASR : ARMV7M_SH_LSL;
        d->shift_n = (uint8_t)((tb && imm5 == 0u) ? 32u : imm5);
        return;
    }
    armv7m_decode_imm_shift((w1 >> 4) & 3u, imm5, &t, &n);
    d->opnd = ARMV7M_OPND_REG;
    d->shift_t = (uint8_t)t;
    d->shift_n = (uint8_t)n;
    d->wide = 1u;
    dp_finish(d, sel, (w1 >> 8) & 15u, w0 & 15u, ((w0 >> 4) & 1u) != 0u);
    /*
     * MOV with a shift is the shift instruction: `mov.w rd, rm, lsl #n`
     * and `lsl.w rd, rm, #n` are one encoding, and the second is the
     * canonical spelling.
     */
    if (d->op == ARMV7M_OP_MOV && !(t == ARMV7M_SH_LSL && n == 0u)) {
        set(d, ARMV7M_OP_LSL + t);
        d->rn = d->rm;
        d->rm = ARMV7M_NOREG;
        d->opnd = (t == ARMV7M_SH_RRX) ? ARMV7M_OPND_NONE : ARMV7M_OPND_IMM;
        d->imm = n;
    }
}

static void d32_dp_register(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op1 = (w0 >> 4) & 15u;
    const uint32_t op2 = (w1 >> 4) & 15u;
    const uint32_t rn = w0 & 15u;

    if ((w1 & 0xF000u) != 0xF000u) {
        return;
    }
    d->rd = (w1 >> 8) & 15u;
    d->rn = (uint8_t)rn;
    d->rm = w1 & 15u;
    if ((op1 >> 3) == 0u && op2 == 0u) {
        set(d, ARMV7M_OP_LSL + ((op1 >> 1) & 3u));
        d->opnd = ARMV7M_OPND_REGSHIFT;
        d->setflags = (op1 & 1u) ? ARMV7M_SF_YES : ARMV7M_SF_NO;
        d->wide = 1u;
        return;
    }
    if ((op1 >> 3) == 0u && (op2 & 8u) != 0u) {
        static const uint16_t k[6][2] = {
            {ARMV7M_OP_SXTAH, ARMV7M_OP_SXTH},
            {ARMV7M_OP_UXTAH, ARMV7M_OP_UXTH},
            {ARMV7M_OP_SXTAB16, ARMV7M_OP_SXTB16},
            {ARMV7M_OP_UXTAB16, ARMV7M_OP_UXTB16},
            {ARMV7M_OP_SXTAB, ARMV7M_OP_SXTB},
            {ARMV7M_OP_UXTAB, ARMV7M_OP_UXTB},
        };

        if (op1 > 5u || (w1 & 0x0040u) != 0u) {
            return;
        }
        set(d, k[op1][rn == 15u ? 1 : 0]);
        if (rn == 15u) {
            d->rn = ARMV7M_NOREG;
        }
        d->imm = ((w1 >> 4) & 3u) * 8u;
        /* Only the four plain byte and halfword extends have a 16-bit form. */
        d->wide = (rn == 15u && op1 != 2u && op1 != 3u) ? 1u : 0u;
        return;
    }
    if ((op1 >> 3) == 1u && (op2 >> 3) == 0u) {
        const uint32_t o = op1 & 7u;

        if ((op2 & 3u) == 3u || o == 3u || o == 7u) {
            return;
        }
        set(d, ARMV7M_OP_PARALLEL);
        d->imm = (((op2 >> 2) & 1u) << 8) | ((op2 & 3u) << 4) | o;
        return;
    }
    if ((op1 >> 2) == 2u && (op2 >> 2) == 2u) {
        static const uint16_t k[16] = {
            ARMV7M_OP_QADD, ARMV7M_OP_QDADD, ARMV7M_OP_QSUB, ARMV7M_OP_QDSUB,
            ARMV7M_OP_REV,  ARMV7M_OP_REV16, ARMV7M_OP_RBIT, ARMV7M_OP_REVSH,
            ARMV7M_OP_SEL,  ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF,
            ARMV7M_OP_CLZ,  ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF, ARMV7M_OP_UNDEF,
        };
        const uint32_t sel = ((op1 & 3u) << 2) | (op2 & 3u);

        set(d, k[sel]);
        d->wide = (sel == 4u || sel == 5u || sel == 7u) ? 1u : 0u;
        if (sel >= 4u && sel != 8u) {
            d->rn = ARMV7M_NOREG; /* one operand: rm, encoded twice */
        }
        return;
    }
}

static void d32_mul(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op1 = (w0 >> 4) & 7u;
    const uint32_t op2 = (w1 >> 4) & 3u;
    const uint32_t ra = (w1 >> 12) & 15u;
    const bool acc = ra != 15u;

    if ((w1 & 0x00C0u) != 0u) {
        return;
    }
    d->rd = (w1 >> 8) & 15u;
    d->rn = w0 & 15u;
    d->rm = w1 & 15u;
    d->ra = acc ? (uint8_t)ra : ARMV7M_NOREG;
    switch (op1) {
    case 0u:
        if (op2 == 0u) {
            set(d, acc ? ARMV7M_OP_MLA : ARMV7M_OP_MUL);
            d->wide = 1u;
        } else if (op2 == 1u && acc) {
            set(d, ARMV7M_OP_MLS);
        }
        return;
    case 1u:
        set(d, (acc ? ARMV7M_OP_SMLABB : ARMV7M_OP_SMULBB) + op2);
        return;
    case 2u:
        if (op2 <= 1u) {
            set(d, (acc ? ARMV7M_OP_SMLAD : ARMV7M_OP_SMUAD) + op2);
        }
        return;
    case 3u:
        if (op2 <= 1u) {
            set(d, (acc ? ARMV7M_OP_SMLAWB : ARMV7M_OP_SMULWB) + op2);
        }
        return;
    case 4u:
        if (op2 <= 1u) {
            set(d, (acc ? ARMV7M_OP_SMLSD : ARMV7M_OP_SMUSD) + op2);
        }
        return;
    case 5u:
        if (op2 <= 1u) {
            set(d, (acc ? ARMV7M_OP_SMMLA : ARMV7M_OP_SMMUL) + op2);
        }
        return;
    case 6u:
        if (op2 <= 1u && acc) {
            set(d, ARMV7M_OP_SMMLS + op2);
        }
        return;
    default:
        if (op2 == 0u) {
            set(d, acc ? ARMV7M_OP_USADA8 : ARMV7M_OP_USAD8);
        }
        return;
    }
}

static void d32_mull(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op1 = (w0 >> 4) & 7u;
    const uint32_t op2 = (w1 >> 4) & 15u;

    d->rd = (w1 >> 12) & 15u; /* RdLo */
    d->ra = (w1 >> 8) & 15u;  /* RdHi */
    d->rn = w0 & 15u;
    d->rm = w1 & 15u;
    if (op1 != 1u && op1 != 3u && d->rd == d->ra) {
        return; /* a 64-bit result needs two registers */
    }
    switch (op1) {
    case 0u:
        if (op2 == 0u) {
            set(d, ARMV7M_OP_SMULL);
        }
        return;
    case 2u:
        if (op2 == 0u) {
            set(d, ARMV7M_OP_UMULL);
        }
        return;
    case 1u:
    case 3u:
        if (op2 == 15u && d->rd == 15u) {
            set(d, (op1 == 1u) ? ARMV7M_OP_SDIV : ARMV7M_OP_UDIV);
            d->rd = d->ra;
            d->ra = ARMV7M_NOREG;
        }
        return;
    case 4u:
        if (op2 == 0u) {
            set(d, ARMV7M_OP_SMLAL);
        } else if ((op2 & 0xCu) == 0x8u) {
            set(d, ARMV7M_OP_SMLALBB + (op2 & 3u));
        } else if ((op2 & 0xEu) == 0xCu) {
            set(d, ARMV7M_OP_SMLALD + (op2 & 1u));
        }
        return;
    case 5u:
        if ((op2 & 0xEu) == 0xCu) {
            set(d, ARMV7M_OP_SMLSLD + (op2 & 1u));
        }
        return;
    case 6u:
        if (op2 == 0u) {
            set(d, ARMV7M_OP_UMLAL);
        } else if (op2 == 6u) {
            set(d, ARMV7M_OP_UMAAL);
        }
        return;
    default:
        return;
    }
}

static void d32_ldst(uint16_t w0, uint16_t w1, I *d)
{
    static const uint16_t k_ld[3][2] = {{ARMV7M_OP_LDRB, ARMV7M_OP_LDRSB},
                                        {ARMV7M_OP_LDRH, ARMV7M_OP_LDRSH},
                                        {ARMV7M_OP_LDR, ARMV7M_OP_UNDEF}};
    static const uint16_t k_st[3] = {ARMV7M_OP_STRB, ARMV7M_OP_STRH,
                                     ARMV7M_OP_STR};
    const uint32_t sz = (w0 >> 5) & 3u;
    const bool load = ((w0 >> 4) & 1u) != 0u;
    const bool sext = ((w0 >> 8) & 1u) != 0u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rt = (w1 >> 12) & 15u;
    bool unpriv = false;

    if (sz == 3u || (!load && sext) || (load && sz == 2u && sext)) {
        return;
    }
    d->rd = (uint8_t)rt;
    d->rn = (uint8_t)rn;
    d->size = (uint8_t)(1u << sz);
    d->sext = sext ? 1u : 0u;
    d->index = 1u;
    d->add = 1u;
    d->wide = 1u;
    if (rn == 15u) {
        if (!load) {
            return;
        }
        d->addr = ARMV7M_ADDR_LITERAL;
        d->add = (w0 >> 7) & 1u;
        d->imm = w1 & 0xFFFu;
    } else if (((w0 >> 7) & 1u) != 0u) {
        d->addr = ARMV7M_ADDR_IMM;
        d->imm = w1 & 0xFFFu;
    } else if ((w1 & 0x0800u) != 0u) {
        const uint32_t puw = (w1 >> 8) & 7u;

        d->addr = ARMV7M_ADDR_IMM;
        d->imm = w1 & 0xFFu;
        d->index = (puw >> 2) & 1u;
        d->add = (puw >> 1) & 1u;
        d->wback = puw & 1u;
        if (!d->index && !d->wback) {
            return;
        }
        unpriv = puw == 6u;
    } else if ((w1 & 0x0FC0u) == 0u) {
        d->addr = ARMV7M_ADDR_REG;
        d->rm = w1 & 15u;
        d->shift_n = (w1 >> 4) & 3u;
    } else {
        return;
    }
    if (load && rt == 15u && sz < 2u) {
        /* Rt == pc on a narrow load: a preload hint. */
        set(d, sext ? ARMV7M_OP_PLI : ARMV7M_OP_PLD);
        d->rd = ARMV7M_NOREG;
        return;
    }
    set(d, load ? k_ld[sz][sext ? 1 : 0] : k_st[sz]);
    if (unpriv) {
        /* The unprivileged forms sit in the same order, one block on. */
        static const uint16_t k_t[8][2] = {
            {ARMV7M_OP_LDR, ARMV7M_OP_LDRT},     {ARMV7M_OP_LDRB, ARMV7M_OP_LDRBT},
            {ARMV7M_OP_LDRH, ARMV7M_OP_LDRHT},   {ARMV7M_OP_LDRSB, ARMV7M_OP_LDRSBT},
            {ARMV7M_OP_LDRSH, ARMV7M_OP_LDRSHT}, {ARMV7M_OP_STR, ARMV7M_OP_STRT},
            {ARMV7M_OP_STRB, ARMV7M_OP_STRBT},   {ARMV7M_OP_STRH, ARMV7M_OP_STRHT},
        };

        for (uint32_t i = 0u; i < 8u; i++) {
            if (d->op == k_t[i][0]) {
                set(d, k_t[i][1]);
                break;
            }
        }
    }
}

static void d32_dual_excl(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op1 = (w0 >> 7) & 3u;
    const uint32_t op2 = (w0 >> 4) & 3u;
    const uint32_t op3 = (w1 >> 4) & 15u;

    d->rn = w0 & 15u;
    d->rd = (w1 >> 12) & 15u;
    d->ra = (w1 >> 8) & 15u;
    d->imm = (w1 & 0xFFu) * 4u;
    if ((op1 & 2u) != 0u || (op2 & 2u) != 0u) { /* LDRD / STRD */
        if ((op2 & 1u) != 0u && d->rd == d->ra) {
            return; /* two words into one register */
        }
        set(d, (op2 & 1u) ? ARMV7M_OP_LDRD : ARMV7M_OP_STRD);
        d->addr = (d->rn == 15u) ? ARMV7M_ADDR_LITERAL : ARMV7M_ADDR_IMM;
        d->index = (w0 >> 8) & 1u;
        d->add = (w0 >> 7) & 1u;
        d->wback = (w0 >> 5) & 1u;
        d->size = 4u;
        return;
    }
    if (op1 == 0u) {
        if (op2 == 1u) {
            if (d->ra != 15u) {
                return; /* (1)(1)(1)(1) */
            }
            set(d, ARMV7M_OP_LDREX);
            d->ra = ARMV7M_NOREG;
        } else {
            /* STREX rd, rt, [rn, #imm]: the status register is Rd. */
            set(d, ARMV7M_OP_STREX);
            d->rm = d->rd;
            d->rd = d->ra;
            d->ra = ARMV7M_NOREG;
        }
        d->size = 4u;
        return;
    }
    d->imm = 0u;
    if (op2 == 1u && (op3 == 0u || op3 == 1u)) {
        if ((w1 & 0xFF00u) != 0xF000u) {
            return; /* (1)(1)(1)(1) (0)(0)(0)(0) */
        }
        set(d, (op3 == 0u) ? ARMV7M_OP_TBB : ARMV7M_OP_TBH);
        d->rm = w1 & 15u;
        d->rd = ARMV7M_NOREG;
        d->ra = ARMV7M_NOREG;
        return;
    }
    if (op3 == 4u || op3 == 5u) {
        d->size = (op3 == 4u) ? 1u : 2u;
        if (d->ra != 15u || (op2 == 1u && (w1 & 15u) != 15u)) {
            return; /* (1)(1)(1)(1), and again in place of Rd on a load */
        }
        if (op2 == 1u) {
            set(d, (op3 == 4u) ? ARMV7M_OP_LDREXB : ARMV7M_OP_LDREXH);
            d->ra = ARMV7M_NOREG;
        } else if (op2 == 0u) {
            set(d, (op3 == 4u) ? ARMV7M_OP_STREXB : ARMV7M_OP_STREXH);
            d->rm = d->rd;
            d->rd = w1 & 15u;
            d->ra = ARMV7M_NOREG;
        }
    }
}

static void d32_branch_misc(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op = (w0 >> 4) & 0x7Fu;
    const uint32_t op1 = (w1 >> 12) & 7u;
    const uint32_t s = (w0 >> 10) & 1u;
    const uint32_t j1 = (w1 >> 13) & 1u;
    const uint32_t j2 = (w1 >> 11) & 1u;

    if ((op1 & 5u) == 0u) {
        if ((op & 0x38u) != 0x38u) {
            int32_t off = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) |
                                    ((uint32_t)(w0 & 0x3Fu) << 12) |
                                    ((uint32_t)(w1 & 0x7FFu) << 1));

            if (s != 0u) {
                off -= 0x200000;
            }
            set(d, ARMV7M_OP_BCC);
            d->cond = (w0 >> 6) & 15u;
            d->imm = (uint32_t)off;
            d->wide = 1u;
            return;
        }
        if ((op & 0x7Eu) == 0x38u) {
            if (op != 0x38u || (w1 & 0x2300u) != 0u || ((w1 >> 10) & 3u) == 0u ||
                !armv7m_sysm_valid(w1 & 0xFFu)) {
                return;
            }
            set(d, ARMV7M_OP_MSR);
            d->rn = w0 & 15u;
            d->imm = (w1 >> 10) & 3u;
            d->imm2 = w1 & 0xFFu;
            return;
        }
        if (op == 0x3Au) {
            static const uint16_t k[5] = {ARMV7M_OP_NOP, ARMV7M_OP_YIELD,
                                          ARMV7M_OP_WFE, ARMV7M_OP_WFI,
                                          ARMV7M_OP_SEV};
            const uint32_t h = w1 & 0xFFu;

            /*
             * The one group the M7 does *not* decode strictly: a hint
             * with its (1)s or its (0)s wrong still executes, and only
             * bits 10:8 -- A and R profile's CPS -- make it something
             * else.
             */
            if (((w1 >> 8) & 7u) != 0u) {
                return;
            }
            set(d, (h < 5u) ? k[h] : ARMV7M_OP_NOP);
            d->imm = h;
            d->wide = 1u;
            return;
        }
        if (op == 0x3Bu) {
            if ((w0 & 15u) != 15u || (w1 & 0x2F00u) != 0x0F00u) {
                return;
            }
            switch ((w1 >> 4) & 15u) {
            case 2u:
                if ((w1 & 15u) == 15u) {
                    set(d, ARMV7M_OP_CLREX);
                }
                return;
            case 4u:
                set(d, ARMV7M_OP_DSB);
                break;
            case 5u:
                set(d, ARMV7M_OP_DMB);
                break;
            case 6u:
                set(d, ARMV7M_OP_ISB);
                break;
            default:
                return;
            }
            d->imm = w1 & 15u;
            return;
        }
        if ((op & 0x7Eu) == 0x3Eu) {
            const uint32_t rd = (w1 >> 8) & 15u;

            if (rd == 13u || rd == 15u || op != 0x3Eu || (w0 & 15u) != 15u ||
                (w1 & 0x2000u) != 0u || !armv7m_sysm_valid(w1 & 0xFFu)) {
                return;
            }
            set(d, ARMV7M_OP_MRS);
            d->rd = (uint8_t)rd;
            d->imm2 = w1 & 0xFFu;
            return;
        }
        return;
    }
    if ((op1 & 5u) == 1u || (op1 & 5u) == 5u) {
        const uint32_t i1 = (~(j1 ^ s)) & 1u;
        const uint32_t i2 = (~(j2 ^ s)) & 1u;
        int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                ((uint32_t)(w0 & 0x3FFu) << 12) |
                                ((uint32_t)(w1 & 0x7FFu) << 1));

        if (s != 0u) {
            off -= 0x2000000;
        }
        set(d, (op1 & 4u) ? ARMV7M_OP_BL : ARMV7M_OP_B);
        d->imm = (uint32_t)off;
        d->wide = 1u;
        return;
    }
    if (op1 == 2u && op == 0x7Fu) {
        set(d, ARMV7M_OP_UDF);
        d->imm = ((uint32_t)(w0 & 15u) << 12) | (w1 & 0xFFFu);
        d->wide = 1u;
    }
}

/* The coprocessor space: FPv5, single precision. */
static void d32_fp(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t coproc = (w1 >> 8) & 15u;
    const bool t = ((w0 >> 12) & 1u) != 0u;
    const uint32_t dbit = (w0 >> 6) & 1u;
    const uint32_t vd = (w1 >> 12) & 15u;
    const uint32_t vn = w0 & 15u;
    const uint32_t vm = w1 & 15u;
    const uint32_t nbit = (w1 >> 7) & 1u;
    const uint32_t mbit = (w1 >> 5) & 1u;
    const bool dbl = ((w1 >> 8) & 1u) != 0u;

    if ((w0 & 0x0F00u) == 0x0F00u || (w0 & 0x0FE0u) == 0x0C00u) {
        /*
         * Not coprocessor instructions at all, whatever bits 11:8 of the
         * second halfword say: the Advanced SIMD data-processing space,
         * which M-profile leaves empty, and the row of the load/store
         * table with P, U, D and W all clear.
         */
        return;
    }
    if ((coproc & 0xEu) != 0xAu) {
        /*
         * No coprocessor but the FPU exists, and naming one that does not
         * is NOCP rather than UNDEFINSTR -- a different fault, with a
         * different status bit, which a handler that emulates a missing
         * coprocessor tells apart.
         */
        set(d, ARMV7M_OP_NOCP);
        return;
    }
    d->rd = (uint8_t)((vd << 1) | dbit);
    d->rn = (uint8_t)((vn << 1) | nbit);
    d->rm = (uint8_t)((vm << 1) | mbit);
    if ((w0 & 0xEF00u) == 0xEE00u && (w1 & 0x10u) == 0u) { /* data processing */
        const uint32_t o1 = (((w0 >> 7) & 1u) << 2) | ((w0 >> 4) & 3u);
        const uint32_t opc2 = w0 & 15u;
        const uint32_t opc3 = (w1 >> 6) & 3u;
        const bool op = (opc3 & 1u) != 0u;

        if (dbl) {
            return;
        }
        if (t) {
            if ((o1 & 4u) == 0u) {
                if (!op) {
                    set(d, ARMV7M_OP_VSEL);
                    d->cond = (w0 >> 4) & 3u;
                }
            } else if (o1 == 4u) {
                set(d, op ? ARMV7M_OP_VMINNM : ARMV7M_OP_VMAXNM);
            } else if (o1 == 7u && (opc2 & 0xCu) == 0x8u && op) {
                /*
                 * Bit 7 is written 0 and is not looked at: the M7 rounds
                 * the same either way. The opposite of every other
                 * should-be bit here, and equally the board's answer.
                 */
                set(d, ARMV7M_OP_VRINTA + (opc2 & 3u));
            } else if (o1 == 7u && (opc2 & 0xCu) == 0xCu && op) {
                set(d, ARMV7M_OP_VCVTA + (opc2 & 3u));
                d->imm = (opc3 >> 1) & 1u; /* signed */
            }
            return;
        }
        switch (o1) {
        case 0u:
            set(d, op ? ARMV7M_OP_VMLS : ARMV7M_OP_VMLA);
            return;
        case 1u:
            set(d, op ? ARMV7M_OP_VNMLA : ARMV7M_OP_VNMLS);
            return;
        case 2u:
            set(d, op ? ARMV7M_OP_VNMUL : ARMV7M_OP_VMUL);
            return;
        case 3u:
            set(d, op ? ARMV7M_OP_VSUB : ARMV7M_OP_VADD);
            return;
        case 4u:
            if (!op) {
                set(d, ARMV7M_OP_VDIV);
            }
            return;
        case 5u:
            set(d, op ? ARMV7M_OP_VFNMA : ARMV7M_OP_VFNMS);
            return;
        case 6u:
            set(d, op ? ARMV7M_OP_VFMS : ARMV7M_OP_VFMA);
            return;
        default:
            break;
        }
        d->rn = ARMV7M_NOREG;
        if (!op) {
            if ((w1 & 0x00A0u) != 0u) {
                return; /* (0) 0 (0) 0 */
            }
            set(d, ARMV7M_OP_VMOVI);
            d->rm = ARMV7M_NOREG;
            d->imm = armv7m_vfp_expand_imm(((uint32_t)(w0 & 15u) << 4) | (w1 & 15u));
            return;
        }
        switch (opc2) {
        case 0x0u:
            set(d, (opc3 == 3u) ? ARMV7M_OP_VABS : ARMV7M_OP_VMOV);
            return;
        case 0x1u:
            set(d, (opc3 == 3u) ? ARMV7M_OP_VSQRT : ARMV7M_OP_VNEG);
            return;
        case 0x2u:
        case 0x3u:
            set(d, nbit ? ARMV7M_OP_VCVTT : ARMV7M_OP_VCVTB);
            d->imm = opc2 & 1u; /* 1: to half */
            return;
        case 0x4u:
            set(d, nbit ? ARMV7M_OP_VCMPE : ARMV7M_OP_VCMP);
            return;
        case 0x5u:
            if ((w1 & 0x2Fu) == 0u) {
                set(d, nbit ? ARMV7M_OP_VCMPEZ : ARMV7M_OP_VCMPZ);
                d->rm = ARMV7M_NOREG;
            }
            return;
        case 0x6u:
            set(d, nbit ? ARMV7M_OP_VRINTZ : ARMV7M_OP_VRINTR);
            return;
        case 0x7u:
            if (opc3 == 1u) {
                set(d, ARMV7M_OP_VRINTX);
            }
            return;
        case 0x8u:
            set(d, nbit ? ARMV7M_OP_VCVT_FROMS : ARMV7M_OP_VCVT_FROMU);
            return;
        case 0xAu:
        case 0xBu:
        case 0xEu:
        case 0xFu: {
            const uint32_t size = nbit ? 32u : 16u;
            const uint32_t imm5 = ((w1 & 15u) << 1) | mbit;

            if (imm5 > size) {
                return;
            }
            set(d, ARMV7M_OP_VCVT_FIX);
            d->rm = ARMV7M_NOREG;
            d->imm = size - imm5;
            d->imm2 = (((opc2 >> 2) & 1u) << 2) | ((opc2 & 1u) << 1) | (nbit ? 1u : 0u);
            return;
        }
        case 0xCu:
            set(d, nbit ? ARMV7M_OP_VCVT_TOU : ARMV7M_OP_VCVTR_TOU);
            return;
        case 0xDu:
            set(d, nbit ? ARMV7M_OP_VCVT_TOS : ARMV7M_OP_VCVTR_TOS);
            return;
        default:
            return;
        }
    }
    if ((w0 & 0xFF00u) == 0xEE00u) { /* 32-bit transfers */
        const uint32_t a = (w0 >> 5) & 7u;
        const bool l = ((w0 >> 4) & 1u) != 0u;

        d->rd = vd; /* Rt */
        d->rm = ARMV7M_NOREG;
        d->imm = l ? 1u : 0u; /* 1: to the core register */
        if ((w1 & 0x006Fu) != 0u) {
            return; /* everything below the coprocessor number but N and 1 */
        }
        if (!dbl && a == 0u) {
            set(d, ARMV7M_OP_VMOV_CORE);
            return;
        }
        if (!dbl && a == 7u && vn == 1u && nbit == 0u) {
            set(d, l ? ARMV7M_OP_VMRS : ARMV7M_OP_VMSR);
            d->rn = ARMV7M_NOREG;
            return;
        }
        if (dbl && (a & 6u) == 0u && nbit == 0u) {
            /* N is the top bit of a D register this bank does not reach. */
            set(d, ARMV7M_OP_VMOV_SCALAR);
            d->rn = (uint8_t)((nbit << 4) | vn); /* D register */
            d->imm2 = a & 1u;
        }
        return;
    }
    if ((w0 & 0xFFE0u) == 0xEC40u) { /* 64-bit transfers */
        if ((w1 & 0xD0u) != 0x10u) {
            return;
        }
        if ((dbl && mbit != 0u) || (!dbl && vm == 15u && mbit != 0u)) {
            return; /* d16 and up, or a pair starting at s31 */
        }
        if (((w0 >> 4) & 1u) != 0u && vd == vn) {
            return; /* two halves into one core register */
        }
        set(d, ARMV7M_OP_VMOV_CORE2);
        d->rd = vd;
        d->ra = (uint8_t)vn;
        d->rn = ARMV7M_NOREG;
        d->rm = dbl ? (uint8_t)(2u * ((mbit << 4) | vm)) : (uint8_t)((vm << 1) | mbit);
        d->fp_dbl = dbl ? 1u : 0u;
        d->imm = (w0 >> 4) & 1u;
        return;
    }
    if ((w0 & 0xFE00u) == 0xEC00u) { /* loads and stores */
        const uint32_t opcode = (w0 >> 4) & 31u;
        const bool p = (opcode & 16u) != 0u;
        const bool u = (opcode & 8u) != 0u;
        const bool w = (opcode & 2u) != 0u;
        const bool load = (opcode & 1u) != 0u;
        const uint32_t imm8 = w1 & 0xFFu;

        if ((opcode & 0x1Au) == 0u || (dbl && dbit != 0u)) {
            return; /* unallocated, or d16 and up */
        }
        d->rd = dbl ? (uint8_t)(2u * ((dbit << 4) | vd)) : (uint8_t)((vd << 1) | dbit);
        d->rn = (uint8_t)vn;
        d->rm = ARMV7M_NOREG;
        d->fp_dbl = dbl ? 1u : 0u;
        d->add = u ? 1u : 0u;
        d->wback = w ? 1u : 0u;
        if (p && !w) {
            set(d, load ? ARMV7M_OP_VLDR : ARMV7M_OP_VSTR);
            d->addr = (vn == 15u) ? ARMV7M_ADDR_LITERAL : ARMV7M_ADDR_IMM;
            d->index = 1u;
            d->imm = imm8 * 4u;
            return;
        }
        if ((p == u && w) || imm8 == 0u || vn == 15u) {
            return;
        }
        d->imm = imm8 & (dbl ? 0xFEu : 0xFFu); /* words */
        if (d->imm == 0u || d->rd + d->imm > 32u) {
            return; /* an empty list, or one that runs off the bank */
        }
        if (vn == 13u && w && p && !u && !load) {
            set(d, ARMV7M_OP_VPUSH);
        } else if (vn == 13u && w && !p && u && load) {
            set(d, ARMV7M_OP_VPOP);
        } else if (p) {
            set(d, load ? ARMV7M_OP_VLDMDB : ARMV7M_OP_VSTMDB);
        } else {
            set(d, load ? ARMV7M_OP_VLDM : ARMV7M_OP_VSTM);
        }
    }
}

static void d32(uint16_t w0, uint16_t w1, I *d)
{
    const uint32_t op1 = (w0 >> 11) & 3u;
    const uint32_t op2 = (w0 >> 4) & 0x7Fu;

    if (op1 == 1u) {
        if ((op2 & 0x64u) == 0x00u) { /* LDM / STM */
            const uint32_t op = (w0 >> 7) & 3u;
            const bool load = ((w0 >> 4) & 1u) != 0u;
            const uint32_t rn = w0 & 15u;

            if ((op != 1u && op != 2u) || (w1 & (1u << 13)) != 0u ||
                (!load && (w1 & (1u << 15)) != 0u) || rn == 15u) {
                return;
            }
            d->rn = (uint8_t)rn;
            d->imm = w1;
            d->wback = (w0 >> 5) & 1u;
            d->wide = 1u;
            if (rn == 13u && d->wback && ((op == 1u && load) || (op == 2u && !load))) {
                set(d, load ? ARMV7M_OP_POP : ARMV7M_OP_PUSH);
            } else if (op == 2u) {
                set(d, load ? ARMV7M_OP_LDMDB : ARMV7M_OP_STMDB);
            } else {
                set(d, load ? ARMV7M_OP_LDM : ARMV7M_OP_STM);
            }
            return;
        }
        if ((op2 & 0x64u) == 0x04u) {
            d32_dual_excl(w0, w1, d);
            return;
        }
        if ((op2 & 0x60u) == 0x20u) {
            d32_dp_reg(w0, w1, d);
            return;
        }
        d32_fp(w0, w1, d);
        return;
    }
    if (op1 == 2u) {
        if ((w1 & 0x8000u) != 0u) {
            d32_branch_misc(w0, w1, d);
            return;
        }
        if ((op2 & 0x20u) == 0u) { /* modified immediate */
            const uint32_t imm12 = ((uint32_t)((w0 >> 10) & 1u) << 11) |
                                   ((uint32_t)((w1 >> 12) & 7u) << 8) | (w1 & 0xFFu);
            uint32_t cy = 2u;
            uint32_t tmp;

            d->imm = armv7m_expand_imm_c(imm12, 2u, &tmp);
            cy = tmp;
            d->imm_carry = (uint8_t)cy;
            d->opnd = ARMV7M_OPND_IMM;
            d->wide = 1u;
            dp_finish(d, (w0 >> 5) & 15u, (w1 >> 8) & 15u, w0 & 15u,
                      ((w0 >> 4) & 1u) != 0u);
            return;
        }
        d32_dp_plain(w0, w1, d);
        return;
    }
    if ((op2 & 0x71u) == 0x00u || (op2 & 0x67u) == 0x01u ||
        (op2 & 0x67u) == 0x03u || (op2 & 0x67u) == 0x05u) {
        d32_ldst(w0, w1, d);
        return;
    }
    if ((op2 & 0x70u) == 0x20u) {
        d32_dp_register(w0, w1, d);
        return;
    }
    if ((op2 & 0x78u) == 0x30u) {
        d32_mul(w0, w1, d);
        return;
    }
    if ((op2 & 0x78u) == 0x38u) {
        d32_mull(w0, w1, d);
        return;
    }
    if ((op2 & 0x40u) != 0u) {
        d32_fp(w0, w1, d);
    }
}

void armv7m_decode(uint16_t w0, uint16_t w1, armv7m_insn_t *out)
{
    memset(out, 0, sizeof(*out));
    out->op = ARMV7M_OP_UNDEF;
    out->rd = ARMV7M_NOREG;
    out->rn = ARMV7M_NOREG;
    out->rm = ARMV7M_NOREG;
    out->ra = ARMV7M_NOREG;
    out->imm_carry = 2u;
    if (armv7m_is_32bit(w0)) {
        out->len = 4u;
        d32(w0, w1, out);
    } else {
        out->len = 2u;
        d16(w0, out);
    }
}
