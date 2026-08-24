/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_pairstats.c - what the pair histogram needs to know about G4MH.
 *
 * The histogram is in src/emu/emu_pairstats.c and is frontend-agnostic;
 * this is the RH850 half. It exists because the generic version was worth
 * nothing until a second frontend used it -- a capability that depends on
 * nobody exercising it is not a capability -- and because this is the
 * frontend where the measurement is *most* useful: there is no reference
 * model here, so "what does this guest actually execute" cannot be
 * answered by running something else and comparing.
 *
 * **What this is precise about and what it is not.** The first halfword's
 * layout is exact and comes from the manual (see g4mh_decode.h): reg2 in
 * bits 15:11, the six-bit opcode in 10:5, reg1 in 4:0. So the *kind* of
 * an instruction and its register numbers are right.
 *
 * Which operand is read and which is written is where an approximation
 * lives, and it is stated rather than hidden: RH850's two-operand forms
 * are `op reg1, reg2` with reg2 both a source and the destination, so
 * reg2 is reported as read *and* written for them. That is the truth for
 * ADD/SUB/AND/OR and the rest of format I, and it is what makes a
 * dependent pair detectable at all. Where it is wrong is the
 * three-operand forms, which write reg3 in a second halfword this table
 * does not read -- those are reported as writing reg2 when they do not,
 * which over-counts dependence on reg2.
 *
 * That bound is worth stating because the number this histogram exists to
 * produce is a *share*, and a share computed from an over-counting rule
 * is an upper bound rather than a measurement. Read it as one.
 */

#include "g4mh/g4mh_pairstats.h"

#if EMU_PAIR_STATS

#include "g4mh/g4mh_decode.h"

#include <stdio.h>

/*
 * The kind, and getting it right is most of this file.
 *
 * RH850 overlays three opcode widths on the same halfword, and keying on
 * the wrong one produces a histogram that looks perfectly reasonable and
 * is wrong:
 *
 *   formats I/II/VI   op6, bits 10:5
 *   formats III/IV    op4, bits 10:7 -- the low two bits of op6 are
 *                     *displacement*, so keying on op6 splits one SLD.B
 *                     into four buckets and hides it
 *   the 0x3F escape   op6 plus a sub-opcode in the second halfword,
 *                     without which every 32-bit arithmetic operation
 *                     histograms as a single bucket
 *
 * The first version of this file keyed everything on op6 and named
 * 0x12/0x13/0x17 wrongly besides. It was caught by comparing against the
 * disassembler on a real guest: `add` and `cmp` are the second and fourth
 * most executed instructions and appeared *nowhere* in the top pairs,
 * while a fictional `sldb` led them. A histogram whose buckets cannot
 * represent the thing being looked for reads exactly like a histogram of
 * something that is not there.
 */
#define KIND_FMT4 (1u << 12) /* keyed on op4, not op6 */
#define KIND_EXT (1u << 13) /* the 0x3F escape       */

/* Format III/IV occupy op4 0x06..0x0B, i.e. op6 0x18..0x2F. */
static bool is_fmt34(uint32_t op6)
{
    const uint32_t op4 = op6 >> 2;

    return op4 >= 0x06u && op4 <= 0x0Bu;
}

static uint32_t g4mh_kind(uint64_t insn)
{
    const uint32_t w0 = (uint32_t)insn & 0xFFFFu;
    const uint32_t op = g4mh_op6(w0);

    if (is_fmt34(w0 ? op : op)) {
        const uint32_t op4 = g4mh_op4(w0);

        if (op4 == 0x0Bu) {
            /* The condition is part of the operation: a fusion rule that
             * matched "a branch" and not "this branch" would be wrong. */
            return KIND_FMT4 | (op4 << 5) | (w0 & 0xFu);
        }
        if (op4 == 0x0Au) {
            /* SLD.W and SST.W share op4 and differ in bit 0. */
            return KIND_FMT4 | (op4 << 5) | (w0 & 1u);
        }
        return KIND_FMT4 | (op4 << 5);
    }

    if (op == 0x3Fu) {
        const uint32_t w1 = (uint32_t)(insn >> 16) & 0xFFFFu;

        return KIND_EXT | op | ((w1 & 0x1Fu) << 6);
    }
    return op;
}

static uint32_t kind_op6(uint32_t k)
{
    return ((k & (KIND_FMT4 | KIND_EXT)) != 0u) ? 0xFFu : (k & 0x3Fu);
}

static uint32_t kind_op4(uint32_t k)
{
    return ((k & KIND_FMT4) != 0u) ? ((k >> 5) & 0xFu) : 0xFFu;
}

/*
 * Memory: the disp16 forms at op6 0x38..0x3B, and the short EP-relative
 * forms at op4 0x06..0x0A. Missing the second set would have hidden most
 * of a compiled guest's accesses -- CC-RH parks the frame in EP and
 * reaches its fields through exactly these.
 */
static bool g4mh_kind_is_mem(uint32_t k)
{
    const uint32_t op = kind_op6(k);
    const uint32_t op4 = kind_op4(k);

    return (op >= 0x38u && op <= 0x3Bu) || (op4 >= 0x06u && op4 <= 0x0Au);
}

/* Format I register-register arithmetic, format II's imm5 forms, and the
 * imm16 forms: the operations that compute an address. */
static bool g4mh_kind_is_alu(uint32_t k)
{
    const uint32_t op = kind_op6(k);

    return op <= 0x17u || (op >= 0x30u && op <= 0x37u);
}

/* Whether an op4 form writes reg2 (a load) or reads it (a store). */
static bool fmt4_is_load(uint32_t w0)
{
    const uint32_t op4 = g4mh_op4(w0);

    if (op4 == 0x06u || op4 == 0x08u) {
        return true; /* SLD.B, SLD.H */
    }
    if (op4 == 0x0Au) {
        return (w0 & 1u) == 0u; /* SLD.W, else SST.W */
    }
    return false; /* SST.B, SST.H, Bcond */
}

static uint32_t g4mh_rd(uint64_t insn)
{
    const uint32_t w0 = (uint32_t)insn & 0xFFFFu;
    const uint32_t op = g4mh_op6(w0);

    if (is_fmt34(op)) {
        return fmt4_is_load(w0) ? g4mh_reg2(w0) : 0u;
    }
    if (op == 0x3Au || op == 0x3Bu) {
        return 0u; /* ST.B/ST.H/ST.W disp16 */
    }
    return g4mh_reg2(w0);
}

/*
 * reg1 is the first source wherever it is a register at all. The imm5
 * forms overlay it, and reporting an immediate as a register would
 * manufacture dependences that no translator could act on.
 */
static uint32_t g4mh_rs1(uint64_t insn)
{
    const uint32_t w0 = (uint32_t)insn & 0xFFFFu;
    const uint32_t op = g4mh_op6(w0);

    if (is_fmt34(op)) {
        return EMU_PAIR_NO_REG; /* addressed through EP */
    }
    if (op >= 0x10u && op <= 0x17u) {
        return EMU_PAIR_NO_REG; /* imm5 in the reg1 field */
    }
    return g4mh_reg1(w0);
}

/*
 * reg2 is read as well as written by the two-operand forms, which is most
 * of this instruction set -- `add r1, r2` means r2 += r1. That is what
 * makes a dependent pair detectable at all.
 *
 * Where it over-counts is the three-operand forms behind the 0x3F escape,
 * which write reg3 in a halfword this table does not read; those are
 * reported as reading and writing reg2 when they write elsewhere. The
 * dependent-pair share is therefore an **upper bound**, and is worth
 * reading as one rather than as a measurement.
 */
static uint32_t g4mh_rs2(uint64_t insn)
{
    const uint32_t w0 = (uint32_t)insn & 0xFFFFu;
    const uint32_t op = g4mh_op6(w0);

    if (is_fmt34(op)) {
        return fmt4_is_load(w0) ? EMU_PAIR_NO_REG : g4mh_reg2(w0);
    }
    if (op <= 0x17u || (op >= 0x38u && op <= 0x3Bu)) {
        return g4mh_reg2(w0);
    }
    return EMU_PAIR_NO_REG;
}

static void g4mh_kind_name(uint32_t k, char *buf, unsigned n)
{
    /* Format I, then format II: taken from the interpreter's own dispatch
     * rather than from memory, which is how the first version got three
     * of them wrong. */
    static const char *const fmt12[0x18] = {
        "mov",    "not",  "divh", "jmp",  "satsubr", "satsub",
        "satadd", "mulh", "or",   "xor",  "and",     "tst",
        "subr",   "sub",  "add",  "cmp",  "mov5",    "satadd5",
        "add5",   "cmp5", "shr5", "sar5", "shl5",    "mulh5",
    };
    static const char *const cond[16] = {
        "bv",  "bl",  "bz",  "bnh", "bn", "br",  "blt", "ble",
        "bnv", "bnl", "bnz", "bh",  "bp", "bsa", "bge", "bgt",
    };

    if ((k & KIND_FMT4) != 0u) {
        const uint32_t op4 = (k >> 5) & 0xFu;

        switch (op4) {
        case 0x06:
            snprintf(buf, n, "sld.b");
            return;
        case 0x07:
            snprintf(buf, n, "sst.b");
            return;
        case 0x08:
            snprintf(buf, n, "sld.h");
            return;
        case 0x09:
            snprintf(buf, n, "sst.h");
            return;
        case 0x0A:
            snprintf(buf, n, "%s", (k & 1u) ? "sst.w" : "sld.w");
            return;
        case 0x0B:
            snprintf(buf, n, "%s", cond[k & 0xFu]);
            return;
        default:
            snprintf(buf, n, "op4.%x", (unsigned)op4);
            return;
        }
    }
    if ((k & KIND_EXT) != 0u) {
        snprintf(buf, n, "ext%02x", (unsigned)((k >> 6) & 0x1Fu));
        return;
    }

    const uint32_t op = k & 0x3Fu;

    if (op < 0x18u) {
        snprintf(buf, n, "%s", fmt12[op]);
        return;
    }
    switch (op) {
    case 0x30:
        snprintf(buf, n, "addi");
        return;
    case 0x31:
        snprintf(buf, n, "movea");
        return;
    case 0x32:
        snprintf(buf, n, "movhi");
        return;
    case 0x33:
        snprintf(buf, n, "satsubi");
        return;
    case 0x34:
        snprintf(buf, n, "ori");
        return;
    case 0x35:
        snprintf(buf, n, "xori");
        return;
    case 0x36:
        snprintf(buf, n, "andi");
        return;
    case 0x37:
        snprintf(buf, n, "mulhi");
        return;
    case 0x38:
        snprintf(buf, n, "ld.b");
        return;
    case 0x39:
        snprintf(buf, n, "ld.hw");
        return;
    case 0x3A:
        snprintf(buf, n, "st.b");
        return;
    case 0x3B:
        snprintf(buf, n, "st.hw");
        return;
    default:
        snprintf(buf, n, "op%02x", (unsigned)op);
        return;
    }
}

const emu_pair_ops_t g4mh_pair_ops = {
    g4mh_kind,      g4mh_rd,          g4mh_rs1,         g4mh_rs2,
    g4mh_kind_name, g4mh_kind_is_mem, g4mh_kind_is_alu,
};

#endif /* EMU_PAIR_STATS */
