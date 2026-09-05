/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pairstats.c - what the pair histogram needs to know about RV32.
 *
 * The histogram itself is in src/emu/emu_pairstats.c and knows nothing
 * about any instruction set; this is the other half, and it is the only
 * half that could ever have been RISC-V. Everything here is field
 * extraction from an encoding.
 */

#include "rv32/rv_pairstats.h"

#if EMU_PAIR_STATS

#include <stdio.h>

/*
 * A pair is keyed on a compact "kind" per instruction rather than on the
 * raw encoding: what a fusion rule matches on is the operation, not the
 * registers. 12 bits is opcode[6:2] plus funct3 plus a bit for funct7's
 * bit 30, which is enough to separate ADD from SUB and SRLI from SRAI.
 */
static uint32_t rv_kind(uint64_t insn)
{
    const uint32_t i = (uint32_t)insn;

    return ((i >> 2) & 0x1Fu) | (((i >> 12) & 0x7u) << 5) |
           (((i >> 30) & 0x1u) << 8);
}

/* The opcode a kind came from, which is all the classifiers need. */
static uint32_t kind_op(uint32_t k)
{
    return ((k & 0x1Fu) << 2) | 3u;
}

static uint32_t rv_rd(uint64_t insn)
{
    const uint32_t i = (uint32_t)insn;

    switch (i & 0x7Fu) {
    case 0x23: /* STORE    */
    case 0x63: /* BRANCH   */
    case 0x27: /* STORE-FP */
        return 0u;
    default:
        return (i >> 7) & 0x1Fu;
    }
}

/*
 * Read, not merely encoded. LUI, AUIPC and JAL have an rs1 field and do
 * not read it, and counting that as a dependence would report pairs no
 * translator could fuse.
 */
static uint32_t rv_rs1(uint64_t insn)
{
    const uint32_t i = (uint32_t)insn;

    switch (i & 0x7Fu) {
    case 0x37:
    case 0x17:
    case 0x6F: /* LUI, AUIPC, JAL */
        return EMU_PAIR_NO_REG;
    default:
        return (i >> 15) & 0x1Fu;
    }
}

static uint32_t rv_rs2(uint64_t insn)
{
    const uint32_t i = (uint32_t)insn;

    switch (i & 0x7Fu) {
    case 0x33:
    case 0x23:
    case 0x63:
    case 0x2F: /* OP, STORE, BRANCH, AMO */
        return (i >> 20) & 0x1Fu;
    default:
        return EMU_PAIR_NO_REG;
    }
}

static bool rv_kind_is_mem(uint32_t k)
{
    const uint32_t op = kind_op(k);

    return op == 0x03u || op == 0x23u; /* LOAD, STORE */
}

static bool rv_kind_is_alu(uint32_t k)
{
    const uint32_t op = kind_op(k);

    return op == 0x13u || op == 0x33u; /* OP-IMM, OP */
}

static void rv_kind_name(uint32_t k, char *buf, unsigned n)
{
    const uint32_t op = kind_op(k);
    const uint32_t f3 = (k >> 5) & 7u;
    const uint32_t f7 = (k >> 8) & 1u;
    const char *s = NULL;

    switch (op) {
    case 0x37:
        s = "lui";
        break;
    case 0x17:
        s = "auipc";
        break;
    case 0x6F:
        s = "jal";
        break;
    case 0x67:
        s = "jalr";
        break;
    case 0x0F:
        s = "fence";
        break;
    case 0x73:
        s = "system";
        break;
    case 0x2F:
        s = "amo";
        break;
    case 0x63: {
        static const char *const b3[8] = {"beq", "bne", "?",    "?",
                                          "blt", "bge", "bltu", "bgeu"};
        s = b3[f3];
        break;
    }
    case 0x03: {
        static const char *const l3[8] = {"lb",  "lh",  "lw", "?",
                                          "lbu", "lhu", "?",  "?"};
        s = l3[f3];
        break;
    }
    case 0x23: {
        static const char *const s3[8] = {"sb", "sh", "sw", "?",
                                          "?",  "?",  "?",  "?"};
        s = s3[f3];
        break;
    }
    case 0x13: {
        static const char *const i3[8] = {"addi", "slli", "slti", "sltiu",
                                          "xori", "srli", "ori",  "andi"};
        s = (f3 == 5u && f7) ? "srai" : i3[f3];
        break;
    }
    case 0x33: {
        static const char *const r3[8] = {"add", "sll", "slt", "sltu",
                                          "xor", "srl", "or",  "and"};
        static const char *const m3[8] = {"mul",    "mulh",   "mulhsu",
                                          "mulhu",  "div",    "divu",
                                          "rem",    "remu"};
        static const char *const zbb5[8] = {"?", "?", "?", "?",
                                            "min", "minu", "max", "maxu"};
        static const char *const zba[8] = {"?",     "sh1add", "?", "sh2add",
                                           "?",     "sh3add", "?", "?"};

        /*
         * **funct7 is not a boolean.** This tested `f7` for truth and
         * called anything non-zero `sub` or `sra`, so every M-extension
         * instruction printed as its base-integer neighbour: `mul` as
         * `add`, `div` as `srl`. Asked which guests execute multiplies,
         * the histogram answered "none" for all four -- a perfect null
         * result, and this file's own rule says to suspect the
         * instrument first.
         *
         * The slot is shared four ways on this build: funct7 0x00 is
         * base, 0x01 is M, 0x20 is sub/sra, and Zba/Zbb take 0x10 and
         * 0x05. Enumerate it rather than testing a bit.
         */
        if (f7 == 0x01u) {
            s = m3[f3];
        } else if (f7 == 0x20u) {
            s = (f3 == 0u) ? "sub" : (f3 == 5u) ? "sra" : "?";
        } else if (f7 == 0x05u) {
            s = zbb5[f3];
        } else if (f7 == 0x10u) {
            s = zba[f3];
        } else if (f7 == 0x30u) {
            s = (f3 == 1u) ? "rol" : (f3 == 5u) ? "ror" : "?";
        } else if (f7 == 0x00u) {
            s = r3[f3];
        }
        break;
    }
    /*
     * **OP-FP and the fused multiply-adds, by name.**
     *
     * Without these the histogram reports the most frequent pair on a
     * float workload as "op53.f7 op53.f7", which is the instrument
     * failing to represent the thing being looked for -- the same shape
     * as the disassembler that reported zero floating point in a
     * hard-float build. Whetstone's top pair is 5.61% of all adjacent
     * pairs and it is two OP-FP instructions with a dependency; naming
     * them is what says *which* two.
     */
    case 0x53: {
        switch (f7 >> 2) {
        case 0x00: s = "fadd.s";  break;
        case 0x01: s = "fsub.s";  break;
        case 0x02: s = "fmul.s";  break;
        case 0x03: s = "fdiv.s";  break;
        case 0x0B: s = "fsqrt.s"; break;
        case 0x04: s = (f3 == 0u) ? "fsgnj.s"
                     : (f3 == 1u) ? "fsgnjn.s" : "fsgnjx.s"; break;
        case 0x05: s = (f3 == 0u) ? "fmin.s" : "fmax.s"; break;
        case 0x14: s = (f3 == 0u) ? "fle.s"
                     : (f3 == 1u) ? "flt.s" : "feq.s"; break;
        case 0x18: s = "fcvt.w.s";  break;
        case 0x1A: s = "fcvt.s.w";  break;
        case 0x1C: s = (f3 == 0u) ? "fmv.x.w" : "fclass.s"; break;
        case 0x1E: s = "fmv.w.x";   break;
        case 0x08: s = "fcvt.s.d";  break;
        default:   s = "op-fp";     break;
        }
        break;
    }
    case 0x43: s = "fmadd.s";  break;
    case 0x47: s = "fmsub.s";  break;
    case 0x4B: s = "fnmsub.s"; break;
    case 0x4F: s = "fnmadd.s"; break;
    case 0x07: {
        static const char *const l3[8] = {"?", "?", "flw", "fld",
                                          "?", "?", "?",   "?"};
        s = l3[f3];
        break;
    }
    case 0x27: {
        static const char *const s3[8] = {"?", "?", "fsw", "fsd",
                                          "?", "?", "?",   "?"};
        s = s3[f3];
        break;
    }

    default:
        break;
    }

    if (s != NULL) {
        snprintf(buf, n, "%s", s);
    } else {
        snprintf(buf, n, "op%02x.f%u", (unsigned)op, (unsigned)f3);
    }
}

const emu_pair_ops_t rv_pair_ops = {
    rv_kind,      rv_rd,          rv_rs1,         rv_rs2,
    rv_kind_name, rv_kind_is_mem, rv_kind_is_alu,
};

#endif /* EMU_PAIR_STATS */
