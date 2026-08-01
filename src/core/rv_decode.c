/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_decode.c - RVC (compressed) instruction expansion.
 *
 * Every 16-bit instruction has an exactly equivalent 32-bit encoding, so
 * rather than duplicating the execute logic we expand and fall through to
 * the normal 32-bit path. That costs a handful of shifts per compressed
 * instruction and keeps one implementation of each operation, which is the
 * right trade for a core this size.
 *
 * Reference: Unprivileged ISA spec, "C" Standard Extension.
 */

#include "rv32/rv_decode.h"

#if RV_EXT_C

/* ------------------------------------------------------------------ */
/* 32-bit instruction encoders                                         */
/* ------------------------------------------------------------------ */

static uint32_t enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1,
                      uint32_t f3, uint32_t rd, uint32_t op)
{
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

static uint32_t enc_i(uint32_t imm, uint32_t rs1,
                      uint32_t f3, uint32_t rd, uint32_t op)
{
    return ((imm & 0xFFFu) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

static uint32_t enc_s(uint32_t imm, uint32_t rs2, uint32_t rs1,
                      uint32_t f3, uint32_t op)
{
    return ((imm & 0xFE0u) << 20) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | ((imm & 0x1Fu) << 7) | op;
}

static uint32_t enc_b(uint32_t imm, uint32_t rs2, uint32_t rs1,
                      uint32_t f3, uint32_t op)
{
    return ((imm & 0x1000u) << 19) | ((imm & 0x7E0u) << 20) |
           (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((imm & 0x1Eu) << 7) | ((imm & 0x800u) >> 4) | op;
}

static uint32_t enc_u(uint32_t imm20, uint32_t rd, uint32_t op)
{
    return ((imm20 & 0xFFFFFu) << 12) | (rd << 7) | op;
}

static uint32_t enc_j(uint32_t imm, uint32_t rd, uint32_t op)
{
    return ((imm & 0x100000u) << 11) | ((imm & 0x7FEu) << 20) |
           ((imm & 0x800u) << 9) | (imm & 0xFF000u) | (rd << 7) | op;
}

/* ------------------------------------------------------------------ */
/* Compressed field accessors                                          */
/* ------------------------------------------------------------------ */

static RV_ALWAYS_INLINE uint32_t c_rd(uint16_t c)   { return (c >> 7) & 0x1Fu; }
static RV_ALWAYS_INLINE uint32_t c_rs2(uint16_t c)  { return (c >> 2) & 0x1Fu; }
/* 3-bit register fields select x8..x15. */
static RV_ALWAYS_INLINE uint32_t c_rdp(uint16_t c)  { return ((c >> 7) & 0x7u) + 8u; }
static RV_ALWAYS_INLINE uint32_t c_rs2p(uint16_t c) { return ((c >> 2) & 0x7u) + 8u; }

#define ILLEGAL 0u

/* ------------------------------------------------------------------ */
/* Quadrant 0                                                          */
/* ------------------------------------------------------------------ */

static uint32_t expand_q0(uint16_t c)
{
    const uint32_t f3 = (c >> 13) & 0x7u;

    switch (f3) {
    case 0: { /* C.ADDI4SPN -> addi rd', x2, nzuimm */
        uint32_t imm = (((uint32_t)c >> 7) & 0x30u)    /* nzuimm[5:4] */
                     | (((uint32_t)c >> 1) & 0x3C0u)   /* nzuimm[9:6] */
                     | (((uint32_t)c >> 4) & 0x4u)     /* nzuimm[2]   */
                     | (((uint32_t)c >> 2) & 0x8u);    /* nzuimm[3]   */
        if (imm == 0u) {
            return ILLEGAL;   /* the all-zero encoding is reserved */
        }
        /*
         * CIW format: the destination is rd' in bits [4:2], not the [9:7]
         * field the CL/CS/CB formats use for rs1'.
         */
        return enc_i(imm, 2u, 0u, c_rs2p(c), OP_IMM);
    }

    case 2: { /* C.LW -> lw rd', off(rs1') */
        uint32_t off = (((uint32_t)c >> 7) & 0x38u)    /* off[5:3] */
                     | (((uint32_t)c >> 4) & 0x4u)     /* off[2]   */
                     | (((uint32_t)c << 1) & 0x40u);   /* off[6]   */
        return enc_i(off, c_rdp(c), 2u, c_rs2p(c), OP_LOAD);
    }

    case 6: { /* C.SW -> sw rs2', off(rs1') */
        uint32_t off = (((uint32_t)c >> 7) & 0x38u)
                     | (((uint32_t)c >> 4) & 0x4u)
                     | (((uint32_t)c << 1) & 0x40u);
        return enc_s(off, c_rs2p(c), c_rdp(c), 2u, OP_STORE);
    }

    /* 1/5 are FLD/FSD, 3/7 are FLW/FSW: no F or D on this core. */
    default:
        return ILLEGAL;
    }
}

/* ------------------------------------------------------------------ */
/* Quadrant 1                                                          */
/* ------------------------------------------------------------------ */

static uint32_t expand_q1(uint16_t c)
{
    const uint32_t f3 = (c >> 13) & 0x7u;

    /* CI-format 6-bit signed immediate, shared by several encodings. */
    const uint32_t ci6 = (((uint32_t)c >> 7) & 0x20u) | (((uint32_t)c >> 2) & 0x1Fu);

    switch (f3) {
    case 0: /* C.ADDI -> addi rd, rd, imm (rd==0 is C.NOP) */
        return enc_i((uint32_t)rv_sext(ci6, 6), c_rd(c), 0u, c_rd(c), OP_IMM);

    case 1: { /* C.JAL -> jal x1, off  (RV32 only) */
        uint32_t off = (((uint32_t)c >> 1) & 0x800u)   /* off[11]  */
                     | (((uint32_t)c >> 7) & 0x10u)    /* off[4]   */
                     | (((uint32_t)c >> 1) & 0x300u)   /* off[9:8] */
                     | (((uint32_t)c << 2) & 0x400u)   /* off[10]  */
                     | (((uint32_t)c >> 1) & 0x40u)    /* off[6]   */
                     | (((uint32_t)c << 1) & 0x80u)    /* off[7]   */
                     | (((uint32_t)c >> 2) & 0xEu)     /* off[3:1] */
                     | (((uint32_t)c << 3) & 0x20u);   /* off[5]   */
        return enc_j((uint32_t)rv_sext(off, 12), 1u, OP_JAL);
    }

    case 2: /* C.LI -> addi rd, x0, imm.  rd==0 is a HINT and expands to a
             * write of x0, which the execute path discards: exactly the
             * "HINTs execute as NOP" behaviour the spec requires. */
        return enc_i((uint32_t)rv_sext(ci6, 6), 0u, 0u, c_rd(c), OP_IMM);

    case 3: {
        if (c_rd(c) == 2u) {  /* C.ADDI16SP -> addi x2, x2, nzimm */
            uint32_t imm = (((uint32_t)c >> 3) & 0x200u)  /* nzimm[9]   */
                         | (((uint32_t)c >> 2) & 0x10u)   /* nzimm[4]   */
                         | (((uint32_t)c << 1) & 0x40u)   /* nzimm[6]   */
                         | (((uint32_t)c << 4) & 0x180u)  /* nzimm[8:7] */
                         | (((uint32_t)c << 3) & 0x20u);  /* nzimm[5]   */
            if (imm == 0u) {
                return ILLEGAL;
            }
            return enc_i((uint32_t)rv_sext(imm, 10), 2u, 0u, 2u, OP_IMM);
        }
        /* C.LUI -> lui rd, nzimm[17:12].  nzimm==0 is reserved; rd==0 is a
         * HINT, which expands to a discarded write of x0. */
        if (ci6 == 0u) {
            return ILLEGAL;
        }
        return enc_u((uint32_t)rv_sext(ci6, 6) & 0xFFFFFu, c_rd(c), OP_LUI);
    }

    case 4: { /* MISC-ALU */
        const uint32_t sel = (c >> 10) & 0x3u;
        const uint32_t shamt = ci6;

        if (sel == 0u || sel == 1u) {          /* C.SRLI / C.SRAI */
            if (shamt & 0x20u) {
                return ILLEGAL;                /* shamt[5] set: RV64 only */
            }
            /* shamt==0 is a HINT; it expands to a shift by zero, which is
             * architecturally a no-op write of the same value. */
            return enc_i((sel == 1u ? 0x400u : 0u) | shamt,
                         c_rdp(c), 5u, c_rdp(c), OP_IMM);
        }
        if (sel == 2u) {                        /* C.ANDI */
            return enc_i((uint32_t)rv_sext(shamt, 6), c_rdp(c), 7u,
                         c_rdp(c), OP_IMM);
        }

        /* sel == 3: register-register ops */
        if (c & 0x1000u) {
            return ILLEGAL;                     /* C.SUBW/C.ADDW: RV64 only */
        }
        switch ((c >> 5) & 0x3u) {
        case 0: return enc_r(0x20u, c_rs2p(c), c_rdp(c), 0u, c_rdp(c), OP_OP); /* SUB */
        case 1: return enc_r(0x00u, c_rs2p(c), c_rdp(c), 4u, c_rdp(c), OP_OP); /* XOR */
        case 2: return enc_r(0x00u, c_rs2p(c), c_rdp(c), 6u, c_rdp(c), OP_OP); /* OR  */
        default:return enc_r(0x00u, c_rs2p(c), c_rdp(c), 7u, c_rdp(c), OP_OP); /* AND */
        }
    }

    case 5: { /* C.J -> jal x0, off */
        uint32_t off = (((uint32_t)c >> 1) & 0x800u)
                     | (((uint32_t)c >> 7) & 0x10u)
                     | (((uint32_t)c >> 1) & 0x300u)
                     | (((uint32_t)c << 2) & 0x400u)
                     | (((uint32_t)c >> 1) & 0x40u)
                     | (((uint32_t)c << 1) & 0x80u)
                     | (((uint32_t)c >> 2) & 0xEu)
                     | (((uint32_t)c << 3) & 0x20u);
        return enc_j((uint32_t)rv_sext(off, 12), 0u, OP_JAL);
    }

    case 6:      /* C.BEQZ */
    case 7: {    /* C.BNEZ */
        uint32_t off = (((uint32_t)c >> 4) & 0x100u)  /* off[8]   */
                     | (((uint32_t)c >> 7) & 0x18u)   /* off[4:3] */
                     | (((uint32_t)c << 1) & 0xC0u)   /* off[7:6] */
                     | (((uint32_t)c >> 2) & 0x6u)    /* off[2:1] */
                     | (((uint32_t)c << 3) & 0x20u);  /* off[5]   */
        return enc_b((uint32_t)rv_sext(off, 9), 0u, c_rdp(c),
                     (f3 == 6u) ? 0u : 1u, OP_BRANCH);
    }

    default:
        return ILLEGAL;
    }
}

/* ------------------------------------------------------------------ */
/* Quadrant 2                                                          */
/* ------------------------------------------------------------------ */

static uint32_t expand_q2(uint16_t c)
{
    const uint32_t f3 = (c >> 13) & 0x7u;
    const uint32_t rd = c_rd(c);
    const uint32_t rs2 = c_rs2(c);

    switch (f3) {
    case 0: { /* C.SLLI -> slli rd, rd, shamt */
        uint32_t shamt = (((uint32_t)c >> 7) & 0x20u) | (((uint32_t)c >> 2) & 0x1Fu);
        if (shamt & 0x20u) {
            return ILLEGAL;   /* shamt[5] set: RV64 only */
        }
        /* rd==0 and shamt==0 are HINTs; both expand to harmless no-ops. */
        return enc_i(shamt, rd, 1u, rd, OP_IMM);
    }

    case 2: { /* C.LWSP -> lw rd, off(x2) */
        uint32_t off = (((uint32_t)c >> 7) & 0x20u)    /* off[5]   */
                     | (((uint32_t)c >> 2) & 0x1Cu)    /* off[4:2] */
                     | (((uint32_t)c << 4) & 0xC0u);   /* off[7:6] */
        if (rd == 0u) {
            return ILLEGAL;   /* rd==0 is reserved */
        }
        return enc_i(off, 2u, 2u, rd, OP_LOAD);
    }

    case 4: {
        if ((c & 0x1000u) == 0u) {
            if (rs2 == 0u) {                 /* C.JR -> jalr x0, 0(rs1) */
                if (rd == 0u) {
                    return ILLEGAL;
                }
                return enc_i(0u, rd, 0u, 0u, OP_JALR);
            }
            /* C.MV -> add rd, x0, rs2.  rd==0 is a HINT. */
            return enc_r(0u, rs2, 0u, 0u, rd, OP_OP);
        }
        if (rs2 == 0u) {
            if (rd == 0u) {                  /* C.EBREAK */
                return enc_i(1u, 0u, 0u, 0u, OP_SYSTEM);
            }
            /* C.JALR -> jalr x1, 0(rs1) */
            return enc_i(0u, rd, 0u, 1u, OP_JALR);
        }
        /* C.ADD -> add rd, rd, rs2.  rd==0 is a HINT. */
        return enc_r(0u, rs2, rd, 0u, rd, OP_OP);
    }

    case 6: { /* C.SWSP -> sw rs2, off(x2) */
        uint32_t off = (((uint32_t)c >> 7) & 0x3Cu)    /* off[5:2] */
                     | (((uint32_t)c >> 1) & 0xC0u);   /* off[7:6] */
        return enc_s(off, rs2, 2u, 2u, OP_STORE);
    }

    /* 1/5 are FLDSP/FSDSP, 3/7 are FLWSP/FSWSP. */
    default:
        return ILLEGAL;
    }
}

/* ------------------------------------------------------------------ */

uint32_t rv_decode_expand_c(uint16_t c)
{
    switch (c & 0x3u) {
    case 0: return expand_q0(c);
    case 1: return expand_q1(c);
    case 2: return expand_q2(c);
    default: return ILLEGAL;   /* 0b11 is not a compressed instruction */
    }
}

#endif /* RV_EXT_C */
