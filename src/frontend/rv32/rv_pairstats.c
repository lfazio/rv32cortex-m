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
 * registers. opcode[6:2] and funct3, and then *the whole of funct7* where
 * funct7 is part of the opcode -- OP, OP-FP, AMO and OP-IMM's two shift
 * slots -- plus the rs2 field where that is too (Zbb's one-source forms,
 * and the conversions). Nowhere else: in every other format those bits
 * are an immediate, and keying on them would make ADDI 1 and ADDI 2 two
 * kinds.
 *
 * **This kept one bit of funct7, bit 30, from the day it was written**,
 * enough to tell ADD from SUB. Two later fixes to the *names* -- the M
 * extension, then OP-FP -- compared that bit against 0x01, 0x05, 0x20
 * and `>> 2` of it against the FP function codes, and so never took
 * effect: `mul` was still printed as `add`, `sub` became `mul`, and
 * every OP-FP instruction was `fadd.s`. A fix to how a value is read
 * has to be checked against how the value is made.
 */
static uint32_t rv_kind(uint64_t insn)
{
    const uint32_t i = (uint32_t)insn;
    const uint32_t op = i & 0x7Fu;
    const uint32_t f3 = (i >> 12) & 0x7u;
    uint32_t k = ((i >> 2) & 0x1Fu);

    /* LUI, AUIPC and JAL have no funct3: those bits are immediate, and
     * keying on them split `auipc` into eight kinds. */
    if (op != 0x37u && op != 0x17u && op != 0x6Fu) {
        k |= f3 << 5;
    }
    if (op == 0x33u || op == 0x53u || op == 0x2Fu ||
        (op == 0x13u && (f3 == 1u || f3 == 5u))) {
        k |= (i >> 25) << 8;
    }
    if (op == 0x53u || (op == 0x13u && (f3 == 1u || f3 == 5u))) {
        k |= ((i >> 20) & 0x1Fu) << 15;
    }
    return k;
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
    const uint32_t f7 = (k >> 8) & 0x7Fu;
    const uint32_t rs2 = (k >> 15) & 0x1Fu;
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
        static const char *const unary[8] = {"clz",    "ctz",    "cpop", NULL,
                                             "sext.b", "sext.h", NULL,   NULL};

        if (f3 == 1u) {
            s = (f7 == 0x00u)               ? "slli"
                : (f7 == 0x14u)             ? "bseti"
                : (f7 == 0x24u)             ? "bclri"
                : (f7 == 0x34u)             ? "binvi"
                : (f7 == 0x30u && rs2 < 8u) ? unary[rs2]
                                            : NULL;
        } else if (f3 == 5u) {
            s = (f7 == 0x00u)                 ? "srli"
                : (f7 == 0x20u)               ? "srai"
                : (f7 == 0x30u)               ? "rori"
                : (f7 == 0x24u)               ? "bexti"
                : (f7 == 0x14u && rs2 == 7u)  ? "orc.b"
                : (f7 == 0x34u && rs2 == 24u) ? "rev8"
                                              : NULL;
        } else {
            s = i3[f3];
        }
        break;
    }
    case 0x33: {
        static const char *const r3[8] = {"add", "sll", "slt", "sltu",
                                          "xor", "srl", "or",  "and"};
        static const char *const m3[8] = {"mul",    "mulh",   "mulhsu",
                                          "mulhu",  "div",    "divu",
                                          "rem",    "remu"};
        static const char *const zbb5[8] = {"?",   "clmul", "clmulr", "clmulh",
                                            "min", "minu",  "max",    "maxu"};
        /* funct3 2, 4 and 6; this had them at 1, 3 and 5. */
        static const char *const zba[8] = {"?",      "?", "sh1add", "?",
                                           "sh2add", "?", "sh3add", "?"};

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
            static const char *const alt[8] = {"sub",  "?",   "?",   "?",
                                               "xnor", "sra", "orn", "andn"};
            s = alt[f3];
        } else if (f7 == 0x04u) {
            s = (f3 == 4u) ? "zext.h" : "?";
        } else if (f7 == 0x14u) {
            s = (f3 == 1u) ? "bset" : "?";
        } else if (f7 == 0x24u) {
            s = (f3 == 1u) ? "bclr" : (f3 == 5u) ? "bext" : "?";
        } else if (f7 == 0x34u) {
            s = (f3 == 1u) ? "binv" : "?";
        } else if (f7 == 0x07u) {
            s = (f3 == 5u) ? "czero.eqz" : (f3 == 7u) ? "czero.nez" : "?";
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

/* ------------------------------------------------------------------ */
/* Executed instructions, by extension                                 */
/* ------------------------------------------------------------------ */

enum {
    X_BASE,
    X_M,
    X_A,
    X_FD,
    X_ZICSR,
    X_ZICNTR,
    X_ZIHPM,
    X_ZIFENCEI,
    X_ZBA,
    X_ZBB,
    X_ZBC,
    X_ZBS,
    X_ZICOND,
    X_ZICBOM,
    X_ZICBOP,
    X_ZICBOZ,
    X_ZIHINTNTL,
    X_ZIHINTPAUSE,
    X_ZACAS,
    X_ZALASR,
    X_ZAWRS,
    X_COUNT
};

static const char *const k_ext_name[X_COUNT] = {
    "base", "M", "A", "F/D", "Zicsr", "Zicntr", "Zihpm", "Zifencei",
    "Zba", "Zbb", "Zbc", "Zbs", "Zicond", "Zicbom", "Zicbop", "Zicboz",
    "Zihintntl", "Zihintpause", "Zacas", "Zalasr", "Zawrs",
};

static uint64_t g_ext_count[X_COUNT];
static uint64_t g_ext_compressed;

/* Is this CSR number one of the 29 programmable counters or its event? */
static bool is_hpm_csr(uint32_t csr)
{
    const uint32_t n = csr & 0x1Fu;
    const uint32_t bank = csr & ~0x1Fu;

    return n >= 3u && (bank == 0xB00u || bank == 0xB80u || bank == 0x320u ||
                       bank == 0xC00u || bank == 0xC80u);
}

/*
 * The interpreter's decode, read off it a second time -- which is what
 * an instrument has to be, and why it can be wrong independently. It is
 * checked the only way that means anything: the guest self-test executes
 * a known number of each, and those numbers are in docs/frontend/rv32.md.
 *
 * It classifies encodings, not outcomes: an instruction that traps is
 * counted where it would have belonged.
 */
static unsigned ext_of(uint32_t i)
{
    const uint32_t op = i & 0x7Fu;
    const uint32_t f3 = (i >> 12) & 7u;
    const uint32_t f7 = i >> 25;
    const uint32_t rd = (i >> 7) & 0x1Fu;
    const uint32_t rs1 = (i >> 15) & 0x1Fu;
    const uint32_t rs2 = (i >> 20) & 0x1Fu;

    switch (op) {
    case 0x33u: /* OP */
        switch (f7) {
        case 0x01u:
            return X_M;
        case 0x20u:
            return (f3 == 4u || f3 == 6u || f3 == 7u) ? X_ZBB : X_BASE;
        case 0x05u:
            return (f3 >= 4u) ? X_ZBB : X_ZBC;
        case 0x30u:
        case 0x04u:
            return X_ZBB;
        case 0x10u:
            return X_ZBA;
        case 0x14u:
        case 0x24u:
        case 0x34u:
            return X_ZBS;
        case 0x07u:
            return X_ZICOND;
        default:
            return (f7 == 0u && f3 == 0u && rd == 0u && rs1 == 0u &&
                    rs2 >= 2u && rs2 <= 5u)
                       ? X_ZIHINTNTL
                       : X_BASE;
        }

    case 0x13u: /* OP-IMM */
        if (f3 == 1u) {
            return (f7 == 0x30u) ? X_ZBB
                   : (f7 == 0x14u || f7 == 0x24u || f7 == 0x34u) ? X_ZBS
                                                                 : X_BASE;
        }
        if (f3 == 5u) {
            /* rori, rev8 and orc.b are Zbb; bexti is Zbs. */
            return (f7 == 0x30u || f7 == 0x34u || f7 == 0x14u) ? X_ZBB
                   : (f7 == 0x24u)                             ? X_ZBS
                                                               : X_BASE;
        }
        if (f3 == 6u && rd == 0u && rs2 <= 3u && rs2 != 2u) {
            return X_ZICBOP; /* ori x0: the low five bits pick the kind */
        }
        return X_BASE;

    case 0x0Fu: /* MISC-MEM */
        if (f3 == 1u) {
            return X_ZIFENCEI;
        }
        if (f3 == 2u) {
            return ((i >> 20) == 4u) ? X_ZICBOZ : X_ZICBOM;
        }
        return (i == 0x0100000Fu) ? X_ZIHINTPAUSE : X_BASE;

    case 0x2Fu: /* AMO */
        switch (f7 >> 2) {
        case 0x05u:
            return X_ZACAS;
        case 0x06u:
        case 0x07u:
            return X_ZALASR;
        default:
            return X_A;
        }

    case 0x73u: /* SYSTEM */
        if (f3 == 0u) {
            const uint32_t imm = i >> 20;

            return (imm == 0x00Du || imm == 0x01Du) ? X_ZAWRS : X_BASE;
        }
        if (is_hpm_csr(i >> 20)) {
            return X_ZIHPM;
        }
        {
            const uint32_t csr = i >> 20;
            const uint32_t bank = csr & 0xF00u;
            const uint32_t n = csr & 0x7Fu; /* the high halves are +0x80 */

            /* cycle, time, instret and their machine and high forms. */
            return ((bank == 0xC00u || bank == 0xB00u) && n <= 2u)
                       ? X_ZICNTR
                       : X_ZICSR;
        }

    case 0x07u: /* LOAD-FP  */
    case 0x27u: /* STORE-FP */
    case 0x43u: /* the fused multiply-adds */
    case 0x47u:
    case 0x4Bu:
    case 0x4Fu:
    case 0x53u: /* OP-FP    */
        return X_FD;

    default:
        return X_BASE;
    }
}

/*
 * The four bit-manipulation extensions, one bucket per instruction.
 *
 * "Zbb is 5,623 of CoreMark's interpreted instructions" says which
 * extension to lower and nothing about which *instruction*, and this
 * tree has been wrong at exactly that resolution before: SYSTEM was
 * 90.8% of a Linux boot's declined blocks and one counter read was
 * 99.2% of SYSTEM. A lowering is chosen per instruction -- two IR
 * operations for sh2add, a call for clmul -- so the count has to be per
 * instruction too.
 *
 * A second reading of the encodings, like ext_of above, and so able to
 * be wrong on its own. B_OTHER is the bucket that says so: anything
 * ext_of called B that this does not name lands there, and it is
 * printed. The self-test's figures are in docs/frontend/rv32.md.
 */
enum {
    B_SH1ADD,
    B_SH2ADD,
    B_SH3ADD,
    B_ANDN,
    B_ORN,
    B_XNOR,
    B_CLZ,
    B_CTZ,
    B_CPOP,
    B_MIN,
    B_MINU,
    B_MAX,
    B_MAXU,
    B_SEXTB,
    B_SEXTH,
    B_ZEXTH,
    B_ROL,
    B_ROR,
    B_RORI,
    B_ORCB,
    B_REV8,
    B_CLMUL,
    B_CLMULR,
    B_CLMULH,
    B_BSET,
    B_BCLR,
    B_BINV,
    B_BEXT,
    B_BSETI,
    B_BCLRI,
    B_BINVI,
    B_BEXTI,
    B_OTHER,
    B_COUNT
};

static const char *const k_b_name[B_COUNT] = {
    "sh1add", "sh2add", "sh3add", "andn",  "orn",       "xnor",  "clz",
    "ctz",    "cpop",   "min",    "minu",  "max",       "maxu",  "sext.b",
    "sext.h", "zext.h", "rol",    "ror",   "rori",      "orc.b", "rev8",
    "clmul",  "clmulr", "clmulh", "bset",  "bclr",      "binv",  "bext",
    "bseti",  "bclri",  "binvi",  "bexti", "(unnamed)",
};

static uint64_t g_b_count[B_COUNT];

static unsigned b_of(uint32_t i)
{
    const uint32_t f3 = (i >> 12) & 7u;
    const uint32_t f7 = i >> 25;
    const uint32_t rs2 = (i >> 20) & 0x1Fu;

    if ((i & 0x7Fu) == 0x33u) { /* OP */
        switch (f7) {
        case 0x10u:
            return (f3 == 2u)   ? B_SH1ADD
                   : (f3 == 4u) ? B_SH2ADD
                   : (f3 == 6u) ? B_SH3ADD
                                : B_OTHER;
        case 0x20u:
            return (f3 == 7u)   ? B_ANDN
                   : (f3 == 6u) ? B_ORN
                   : (f3 == 4u) ? B_XNOR
                                : B_OTHER;
        case 0x05u: {
            static const uint8_t k[8] = {B_OTHER, B_CLMUL, B_CLMULR, B_CLMULH,
                                         B_MIN,   B_MINU,  B_MAX,    B_MAXU};
            return k[f3];
        }
        case 0x30u:
            return (f3 == 1u) ? B_ROL : (f3 == 5u) ? B_ROR : B_OTHER;
        case 0x04u:
            return (f3 == 4u && rs2 == 0u) ? B_ZEXTH : B_OTHER;
        case 0x14u:
            return (f3 == 1u) ? B_BSET : B_OTHER;
        case 0x24u:
            return (f3 == 1u) ? B_BCLR : (f3 == 5u) ? B_BEXT : B_OTHER;
        case 0x34u:
            return (f3 == 1u) ? B_BINV : B_OTHER;
        default:
            return B_OTHER;
        }
    }
    /* OP-IMM: ext_of sends nothing else here. */
    if (f3 == 1u) {
        switch (f7) {
        case 0x30u: {
            static const uint8_t k[8] = {B_CLZ,   B_CTZ,   B_CPOP,  B_OTHER,
                                         B_SEXTB, B_SEXTH, B_OTHER, B_OTHER};
            return (rs2 < 8u) ? k[rs2] : B_OTHER;
        }
        case 0x14u:
            return B_BSETI;
        case 0x24u:
            return B_BCLRI;
        default:
            return (f7 == 0x34u) ? B_BINVI : B_OTHER;
        }
    }
    switch (f7) {
    case 0x30u:
        return B_RORI;
    case 0x14u:
        return (rs2 == 7u) ? B_ORCB : B_OTHER;
    case 0x34u:
        return (rs2 == 24u) ? B_REV8 : B_OTHER;
    default:
        return (f7 == 0x24u) ? B_BEXTI : B_OTHER;
    }
}

void rv_ext_note(uint32_t insn, unsigned len)
{
    const unsigned x = ext_of(insn);

    g_ext_count[x]++;
    if (x >= X_ZBA && x <= X_ZBS) {
        g_b_count[b_of(insn)]++;
    }
    if (len == 2u) {
        g_ext_compressed++;
    }
}

void rv_ext_report(emu_print_fn out, void *ctx)
{
    char line[96];
    uint64_t total = 0u;

    for (unsigned x = 0; x < X_COUNT; x++) {
        total += g_ext_count[x];
    }
    if (total == 0u) {
        return; /* nothing was interpreted: say nothing rather than zeros */
    }
    out(ctx, "\n-- executed, by extension (interpreted instructions) --\n");
    for (unsigned x = 0; x < X_COUNT; x++) {
        if (g_ext_count[x] != 0u) {
            (void)snprintf(line, sizeof(line), "  %-12s %12llu\n",
                           k_ext_name[x], (unsigned long long)g_ext_count[x]);
            out(ctx, line);
        }
    }
    (void)snprintf(line, sizeof(line),
                   "  %-12s %12llu\n  %-12s %12llu  (16-bit encodings)\n",
                   "total", (unsigned long long)total, "C",
                   (unsigned long long)g_ext_compressed);
    out(ctx, line);

    bool any = false;

    for (unsigned k = 0; k < B_COUNT; k++) {
        if (g_b_count[k] == 0u) {
            continue;
        }
        if (!any) {
            out(ctx, "\n-- Zba/Zbb/Zbc/Zbs, by instruction --\n");
            any = true;
        }
        (void)snprintf(line, sizeof(line), "  %-12s %12llu\n", k_b_name[k],
                       (unsigned long long)g_b_count[k]);
        out(ctx, line);
    }
}

#endif /* EMU_PAIR_STATS */
