/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_pairstats.c - what the pair histogram needs to know about Thumb-2.
 *
 * A thin table over armv7m_decode, which already says what an encoding is
 * and which register plays which part -- so unlike G4MH's, this file has
 * no field extraction of its own to get wrong.
 *
 * **Register numbers are biased by one.** The generic histogram reads a
 * destination of 0 as "writes nothing", which is RISC-V's x0 and is right
 * there. On ARM r0 is the busiest register in the machine -- the first
 * argument and the return value -- and reporting it as "none" would drop
 * every dependence through it. The histogram only ever compares these
 * numbers with each other, so r0 is 1 and nothing else changes.
 *
 * **What it approximates, stated rather than hidden.** The interface has
 * one destination and two sources:
 *
 *   - flags are not registers here. `cmp` then `bne` is the most common
 *     dependent pair in any compiled ARM program and is reported as
 *     independent. That one is not a store-and-reload of a guest
 *     register, which is what the dependent share is used to estimate, so
 *     leaving it out keeps that estimate honest -- but it does mean this
 *     histogram cannot answer "how often does a flag-setter feed a
 *     branch", and should not be read as though it could.
 *   - an instruction with three sources reports two: a store reports its
 *     base and index (or its base and the value, with no index), MLA its
 *     two multiplicands. Writeback to a base register is not reported as
 *     a destination when the instruction also loads one.
 *   - a register list is not expanded: PUSH, POP, LDM and STM report the
 *     base only.
 *
 * All three *under*-count dependence, so the share is a lower bound --
 * the opposite direction from G4MH's table, which over-counts.
 */

#include "armv7m/armv7m_pairstats.h"

#if EMU_PAIR_STATS

#include "armv7m/armv7m_decode.h"

#include <stdio.h>

/*
 * The kind: the operation, the shape of its second operand, how it finds
 * its address, and -- for a conditional branch -- the condition, because
 * a rule that matched "a branch" and not "this branch" would be wrong.
 *
 *   9:0    operation
 *   11:10  ARMV7M_OPND_*
 *   13:12  ARMV7M_ADDR_*
 *   14     the register operand is shifted
 *   15     flags are written
 *   19:16  condition
 */
static void decode(uint64_t insn, armv7m_insn_t *d)
{
    armv7m_decode((uint16_t)(insn & 0xFFFFu), (uint16_t)((insn >> 16) & 0xFFFFu),
                  d);
}

static bool is_mem_op(uint32_t op)
{
    return (op >= ARMV7M_OP_LDR && op <= ARMV7M_OP_POP) ||
           (op >= ARMV7M_OP_VLDR && op <= ARMV7M_OP_VPOP);
}

static bool is_store_op(uint32_t op)
{
    return op == ARMV7M_OP_STR || op == ARMV7M_OP_STRB || op == ARMV7M_OP_STRH ||
           op == ARMV7M_OP_STRT || op == ARMV7M_OP_STRBT ||
           op == ARMV7M_OP_STRHT || op == ARMV7M_OP_STRD;
}

static bool is_fp_op(uint32_t op)
{
    return op >= ARMV7M_OP_VADD;
}

static uint32_t armv7m_kind(uint64_t insn)
{
    armv7m_insn_t d;
    uint32_t k;

    decode(insn, &d);
    k = d.op | ((uint32_t)d.opnd << 10) | ((uint32_t)d.addr << 12);
    if (d.opnd == ARMV7M_OPND_REG &&
        !(d.shift_t == ARMV7M_SH_LSL && d.shift_n == 0u)) {
        k |= 1u << 14;
    }
    /* SF_NOT_IN_IT counts as setting them: outside an IT block it does,
     * and that is where nearly every such instruction executes. */
    if (d.setflags != ARMV7M_SF_NO) {
        k |= 1u << 15;
    }
    if (d.op == ARMV7M_OP_BCC) {
        k |= (uint32_t)d.cond << 16;
    }
    return k;
}

static uint32_t biased(uint32_t r)
{
    return (r < 16u) ? r + 1u : EMU_PAIR_NO_REG;
}

static uint32_t armv7m_rd(uint64_t insn)
{
    armv7m_insn_t d;

    decode(insn, &d);
    if (is_fp_op(d.op)) {
        /* Only the moves and the conversions to a core register write one. */
        const bool to_core = (d.op == ARMV7M_OP_VMOV_CORE ||
                              d.op == ARMV7M_OP_VMOV_SCALAR ||
                              d.op == ARMV7M_OP_VMOV_CORE2) &&
                             d.imm != 0u;

        return (to_core || d.op == ARMV7M_OP_VMRS) && d.rd < 15u ? d.rd + 1u : 0u;
    }
    if (is_store_op(d.op) || d.op == ARMV7M_OP_PLD || d.op == ARMV7M_OP_PLI) {
        return (d.wback && d.rn < 16u) ? d.rn + 1u : 0u;
    }
    switch (d.op) {
    case ARMV7M_OP_PUSH:
    case ARMV7M_OP_POP:
    case ARMV7M_OP_LDM:
    case ARMV7M_OP_LDMDB:
    case ARMV7M_OP_STM:
    case ARMV7M_OP_STMDB:
        return (d.wback && d.rn < 16u) ? d.rn + 1u : 0u;
    case ARMV7M_OP_BL:
    case ARMV7M_OP_BLX:
        return 14u + 1u;
    case ARMV7M_OP_B:
    case ARMV7M_OP_BCC:
    case ARMV7M_OP_BX:
    case ARMV7M_OP_CBZ:
    case ARMV7M_OP_CBNZ:
    case ARMV7M_OP_TBB:
    case ARMV7M_OP_TBH:
    case ARMV7M_OP_MSR:
        return 0u;
    default:
        return (d.rd < 16u) ? d.rd + 1u : 0u;
    }
}

static uint32_t armv7m_rs1(uint64_t insn)
{
    armv7m_insn_t d;

    decode(insn, &d);
    if (is_fp_op(d.op)) {
        /* The base of an FP load or store; the core register of a move in. */
        if (d.op >= ARMV7M_OP_VLDR) {
            return biased(d.rn);
        }
        if ((d.op == ARMV7M_OP_VMOV_CORE || d.op == ARMV7M_OP_VMOV_SCALAR ||
             d.op == ARMV7M_OP_VMOV_CORE2 || d.op == ARMV7M_OP_VMSR) &&
            d.imm == 0u) {
            return biased(d.rd);
        }
        return EMU_PAIR_NO_REG;
    }
    switch (d.op) {
    case ARMV7M_OP_BX:
    case ARMV7M_OP_BLX:
        return biased(d.rm);
    case ARMV7M_OP_MOVT:
        return biased(d.rd); /* keeps the low half of what it writes */
    default:
        /*
         * The literal forms name the pc as their base, which is not a
         * value any earlier instruction computed.
         */
        if (d.addr == ARMV7M_ADDR_LITERAL || d.rn == 15u) {
            return EMU_PAIR_NO_REG;
        }
        /* A one-operand form keeps its source in rm: MOV, MVN, the
         * extends, REV, CLZ. */
        return biased((d.rn != ARMV7M_NOREG) ? d.rn : d.rm);
    }
}

static uint32_t armv7m_rs2(uint64_t insn)
{
    armv7m_insn_t d;

    decode(insn, &d);
    if (is_fp_op(d.op)) {
        return (d.op == ARMV7M_OP_VMOV_CORE2 && d.imm == 0u) ? biased(d.ra)
                                                               : EMU_PAIR_NO_REG;
    }
    if (d.op == ARMV7M_OP_BX || d.op == ARMV7M_OP_BLX || d.rn == ARMV7M_NOREG) {
        return EMU_PAIR_NO_REG; /* rm was reported as the first source */
    }
    if (d.rm != ARMV7M_NOREG) {
        return biased(d.rm);
    }
    /* No index: the second thing a store reads is the value. */
    return is_store_op(d.op) ? biased(d.rd) : EMU_PAIR_NO_REG;
}

static void armv7m_kind_name(uint32_t k, char *buf, unsigned n)
{
    static const char *const k_cond[16] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                           "vs", "vc", "hi", "ls", "ge", "lt",
                                           "gt", "le", "al", "nv"};
    static const char *const k_addr[4] = {"", "[i]", "[r]", "[pc]"};
    const uint32_t op = k & 0x3FFu;
    const uint32_t opnd = (k >> 10) & 3u;
    char name[16];
    unsigned i = 0u;

    for (const char *p = armv7m_op_name(op); *p != '\0' && i + 1u < sizeof(name);
         p++) {
        name[i++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    }
    name[i] = '\0';
    if (op == ARMV7M_OP_BCC) {
        (void)snprintf(buf, n, "b%s", k_cond[(k >> 16) & 15u]);
        return;
    }
    /* The compares always set flags; an "s" on them says nothing. */
    const bool cmp = op == ARMV7M_OP_TST || op == ARMV7M_OP_TEQ ||
                     op == ARMV7M_OP_CMN || op == ARMV7M_OP_CMP;

    (void)snprintf(buf, n, "%s%s%s%s%s", name,
                   ((k & (1u << 15)) && !cmp) ? "s" : "",
                   (opnd == ARMV7M_OPND_IMM)        ? "#"
                   : (opnd == ARMV7M_OPND_REGSHIFT) ? "<r"
                   : (k & (1u << 14))               ? "<<"
                                                    : "",
                   k_addr[(k >> 12) & 3u], "");
}

static bool armv7m_kind_is_mem(uint32_t k)
{
    return is_mem_op(k & 0x3FFu);
}

/* What can compute an address: the data-processing group and ADR. */
static bool armv7m_kind_is_alu(uint32_t k)
{
    const uint32_t op = k & 0x3FFu;

    return op >= ARMV7M_OP_AND && op <= ARMV7M_OP_ADR;
}

const emu_pair_ops_t armv7m_pair_ops = {
    .kind = armv7m_kind,
    .rd = armv7m_rd,
    .rs1 = armv7m_rs1,
    .rs2 = armv7m_rs2,
    .kind_name = armv7m_kind_name,
    .kind_is_mem = armv7m_kind_is_mem,
    .kind_is_alu = armv7m_kind_is_alu,
};

#endif /* EMU_PAIR_STATS */
