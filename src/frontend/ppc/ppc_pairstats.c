/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_pairstats.c - what the pair histogram needs to know about the e200.
 *
 * A table over ppc_decode, as ARMv7-M's is over its decoder: the decoder
 * already maps every encoding -- Book E, VLE's 32-bit forms and its
 * 16-bit ones -- onto one semantic class and the architecture's register
 * fields, with the four-bit se_ registers expanded. So the roles below
 * are per *class*, and read off what ppc_exec does with each field, not
 * off the operand order the assembler writes: `or rA, rS, rB` writes rA
 * and reads the rd field, and se_or, whose two operands are one register
 * and another, decodes to the same fields.
 *
 * **Register numbers are biased by one**, for the reason ARMv7-M's are:
 * the histogram reads a destination of 0 as "writes nothing", and r0 is
 * an ordinary register here -- except as a base, where `ra0` says it
 * reads as zero, and then it is reported as no source at all.
 *
 * **What it approximates:**
 *
 *   - CR fields, LR, CTR and XER are not registers here, so `cmpw` then
 *     `bc` -- the commonest dependent pair in compiled PowerPC -- reads as
 *     independent, as `cmp` then `bne` does on ARMv7-M. The dependent
 *     share is used to estimate guest-register round trips, which those
 *     are not.
 *   - three sources report two: a store reports its base and its index,
 *     or its base and its value when there is no index; rlwimi and the
 *     fused multiply-adds lose their accumulator.
 *   - a load with update writes rA as well as rD and reports rD; the
 *     multiple loads and stores report their first register and base.
 *
 * All of these under-count dependence, so the share is a lower bound.
 *
 * The kind is the mnemonic, plus what the mnemonic table leaves to bits
 * of the encoding: Rc, OE, LK and AA -- and, for a conditional branch,
 * BO and which bit of the CR field it tests, because a rule matching "a
 * branch" and not "this branch" would be wrong.
 */

#include "ppc/ppc_pairstats.h"

#if EMU_PAIR_STATS

#include "ppc/ppc_decode.h"

#include <stdio.h>
#include <string.h>

/*
 *   8:0    mnemonic (ppc_mn_t; 343 of them)
 *   9      Rc
 *   10     OE
 *   11     LK
 *   12     AA
 *   17:13  BO, conditional branches only
 *   19:18  BI mod 4: lt, gt, eq or so
 */
#define K_RC (1u << 9)
#define K_OE (1u << 10)
#define K_LK (1u << 11)
#define K_AA (1u << 12)

static void decode(uint64_t insn, ppc_insn_t *d)
{
    ppc_decode((uint32_t)insn, (unsigned)((insn >> 32) & 0xFFu),
               ((insn >> 40) & 1u) != 0u, d);
}

static uint32_t pair_kind(uint64_t insn)
{
    ppc_insn_t d;
    uint32_t k;

    decode(insn, &d);
    k = d.id & 0x1FFu;
    k |= d.rc ? K_RC : 0u;
    k |= d.oe ? K_OE : 0u;
    k |= d.lk ? K_LK : 0u;
    k |= d.aa ? K_AA : 0u;
    if (d.sem == PPC_S_BC) {
        k |= ((uint32_t)(d.bo & 0x1Fu) << 13) | ((uint32_t)(d.bi & 3u) << 18);
    }
    return k;
}

static uint32_t biased(uint32_t r)
{
    return (r < 32u) ? r + 1u : EMU_PAIR_NO_REG;
}

/* A base register, or none when rA = 0 means the value 0. */
static uint32_t base(const ppc_insn_t *d)
{
    return (d->ra0 && d->ra == 0u) ? EMU_PAIR_NO_REG : biased(d->ra);
}

static bool fmt_compares_reg(const ppc_insn_t *d)
{
    const uint32_t f = ppc_mn_format(d->id);

    return f == PPC_F_A_B || f == PPC_F_CRF_A_B;
}

static uint32_t pair_rd(uint64_t insn)
{
    ppc_insn_t d;

    decode(insn, &d);
    switch ((ppc_sem_t)d.sem) {
    /* rD = f(...) */
    case PPC_S_ADD:
    case PPC_S_ADDC:
    case PPC_S_ADDE:
    case PPC_S_ADDME:
    case PPC_S_ADDZE:
    case PPC_S_SUBF:
    case PPC_S_SUBFC:
    case PPC_S_SUBFE:
    case PPC_S_SUBFME:
    case PPC_S_SUBFZE:
    case PPC_S_NEG:
    case PPC_S_MULLW:
    case PPC_S_MULHW:
    case PPC_S_MULHWU:
    case PPC_S_DIVW:
    case PPC_S_DIVWU:
    case PPC_S_LI:
    case PPC_S_ADDI:
    case PPC_S_ADDIC:
    case PPC_S_SUBFIC:
    case PPC_S_MULLI:
    case PPC_S_MR:
    case PPC_S_MFCR:
    case PPC_S_ISEL:
    case PPC_S_LOAD:
    case PPC_S_LMW:
    case PPC_S_LMV:
    case PPC_S_LARX:
    case PPC_S_MFMSR:
    case PPC_S_MFSPR:
        return biased(d.rd);
    /* rA = f(rS, ...) */
    case PPC_S_AND:
    case PPC_S_ANDC:
    case PPC_S_OR:
    case PPC_S_ORC:
    case PPC_S_XOR:
    case PPC_S_NAND:
    case PPC_S_NOR:
    case PPC_S_EQV:
    case PPC_S_ANDI:
    case PPC_S_ORI:
    case PPC_S_XORI:
    case PPC_S_EXTSB:
    case PPC_S_EXTSH:
    case PPC_S_EXTZB:
    case PPC_S_EXTZH:
    case PPC_S_CNTLZW:
    case PPC_S_SLW:
    case PPC_S_SRW:
    case PPC_S_SRAW:
    case PPC_S_SRAWI:
    case PPC_S_RLWINM:
    case PPC_S_RLWIMI:
    case PPC_S_RLWNM:
        return biased(d.ra);
    /* A store writes a register only by updating its base. */
    case PPC_S_STORE:
        return d.upd ? biased(d.ra) : 0u;
    default:
        /* The EFPU's results go to rD, its compares to a CR field. */
        if (d.sem >= PPC_S_EFSADD && d.sem <= PPC_S_EFSCTH &&
            !(d.sem >= PPC_S_EFSCMPGT && d.sem <= PPC_S_EFSTSTEQ)) {
            return biased(d.rd);
        }
        return 0u;
    }
}

static uint32_t pair_rs1(uint64_t insn)
{
    ppc_insn_t d;

    decode(insn, &d);
    switch ((ppc_sem_t)d.sem) {
    case PPC_S_ADDI:
    case PPC_S_ISEL:
    case PPC_S_LOAD:
    case PPC_S_STORE:
    case PPC_S_LMW:
    case PPC_S_STMW:
    case PPC_S_LMV:
    case PPC_S_STMV:
    case PPC_S_LARX:
    case PPC_S_STCX:
    case PPC_S_DCBZ:
    case PPC_S_TLBSX:
    case PPC_S_TLBIVAX:
        return base(&d);
    /* rS, in the rd field */
    case PPC_S_AND:
    case PPC_S_ANDC:
    case PPC_S_OR:
    case PPC_S_ORC:
    case PPC_S_XOR:
    case PPC_S_NAND:
    case PPC_S_NOR:
    case PPC_S_EQV:
    case PPC_S_ANDI:
    case PPC_S_ORI:
    case PPC_S_XORI:
    case PPC_S_EXTSB:
    case PPC_S_EXTSH:
    case PPC_S_EXTZB:
    case PPC_S_EXTZH:
    case PPC_S_CNTLZW:
    case PPC_S_SLW:
    case PPC_S_SRW:
    case PPC_S_SRAW:
    case PPC_S_SRAWI:
    case PPC_S_RLWINM:
    case PPC_S_RLWIMI:
    case PPC_S_RLWNM:
    case PPC_S_MTCRF:
    case PPC_S_MTMSR:
    case PPC_S_MTSPR:
        return biased(d.rd);
    case PPC_S_WRTEE:
        /* wrtee reads rS; wrteei takes its bit from the encoding. */
        return (ppc_mn_format(d.id) == PPC_F_S) ? biased(d.rd)
                                                : EMU_PAIR_NO_REG;
    case PPC_S_ADD:
    case PPC_S_ADDC:
    case PPC_S_ADDE:
    case PPC_S_ADDME:
    case PPC_S_ADDZE:
    case PPC_S_SUBF:
    case PPC_S_SUBFC:
    case PPC_S_SUBFE:
    case PPC_S_SUBFME:
    case PPC_S_SUBFZE:
    case PPC_S_NEG:
    case PPC_S_MULLW:
    case PPC_S_MULHW:
    case PPC_S_MULHWU:
    case PPC_S_DIVW:
    case PPC_S_DIVWU:
    case PPC_S_ADDIC:
    case PPC_S_SUBFIC:
    case PPC_S_MULLI:
    case PPC_S_MR:
    case PPC_S_BTST:
    case PPC_S_CMP:
    case PPC_S_CMPL:
    case PPC_S_CMPH:
    case PPC_S_CMPHL:
    case PPC_S_TW:
        return biased(d.ra);
    default:
        if (d.sem >= PPC_S_EFSADD && d.sem <= PPC_S_EFSCTH) {
            /* Whatever the format names first after rD. */
            const uint32_t f = ppc_mn_format(d.id);

            return (f == PPC_F_D_B) ? biased(d.rb) : biased(d.ra);
        }
        return EMU_PAIR_NO_REG;
    }
}

static uint32_t pair_rs2(uint64_t insn)
{
    ppc_insn_t d;

    decode(insn, &d);
    switch ((ppc_sem_t)d.sem) {
    case PPC_S_ADD:
    case PPC_S_ADDC:
    case PPC_S_ADDE:
    case PPC_S_SUBF:
    case PPC_S_SUBFC:
    case PPC_S_SUBFE:
    case PPC_S_MULLW:
    case PPC_S_MULHW:
    case PPC_S_MULHWU:
    case PPC_S_DIVW:
    case PPC_S_DIVWU:
    case PPC_S_AND:
    case PPC_S_ANDC:
    case PPC_S_OR:
    case PPC_S_ORC:
    case PPC_S_XOR:
    case PPC_S_NAND:
    case PPC_S_NOR:
    case PPC_S_EQV:
    case PPC_S_SLW:
    case PPC_S_SRW:
    case PPC_S_SRAW:
    case PPC_S_RLWNM:
    case PPC_S_ISEL:
        return biased(d.rb);
    case PPC_S_RLWIMI:
        return biased(d.ra); /* keeps the bits outside the mask */
    case PPC_S_CMP:
    case PPC_S_CMPL:
    case PPC_S_CMPH:
    case PPC_S_CMPHL:
        return fmt_compares_reg(&d) ? biased(d.rb) : EMU_PAIR_NO_REG;
    case PPC_S_TW:
        return (d.id == PPC_M_TWI) ? EMU_PAIR_NO_REG : biased(d.rb);
    case PPC_S_LOAD:
    case PPC_S_LARX:
    case PPC_S_DCBZ:
    case PPC_S_TLBSX:
    case PPC_S_TLBIVAX:
        return d.idx ? biased(d.rb) : EMU_PAIR_NO_REG;
    case PPC_S_STORE:
    case PPC_S_STCX:
        /* The index if there is one, else the value stored. */
        return d.idx ? biased(d.rb) : biased(d.rd);
    default:
        if (d.sem >= PPC_S_EFSADD && d.sem <= PPC_S_EFSCTH) {
            const uint32_t f = ppc_mn_format(d.id);

            return (f == PPC_F_D_A_B || f == PPC_F_CRF_A_B) ? biased(d.rb)
                                                            : EMU_PAIR_NO_REG;
        }
        return EMU_PAIR_NO_REG;
    }
}

static void pair_kind_name(uint32_t k, char *buf, unsigned n)
{
    static const char *const k_bit[4] = {"lt", "gt", "eq", "so"};
    const char *const name = ppc_mn_name(k & 0x1FFu);
    const size_t len = strlen(name);
    const bool dotted = len > 0u && name[len - 1u] == '.';
    const bool ell = len > 0u && name[len - 1u] == 'l';

    /*
     * The suffixes the assembler spells, in its order: l, a, o, '.'.
     * The VLE table names some record forms with their dot, and the
     * se_ link forms with their l, already.
     */
    if (ppc_mn_sem(k & 0x1FFu) == PPC_S_BC) {
        (void)snprintf(buf, n, "%s%s%s bo=%u %s", name,
                       ((k & K_LK) && !ell) ? "l" : "", (k & K_AA) ? "a" : "",
                       (k >> 13) & 0x1Fu, k_bit[(k >> 18) & 3u]);
        return;
    }
    (void)snprintf(buf, n, "%s%s%s%s%s", name, ((k & K_LK) && !ell) ? "l" : "",
                   (k & K_AA) ? "a" : "", (k & K_OE) ? "o" : "",
                   ((k & K_RC) && !dotted) ? "." : "");
}

static bool pair_kind_is_mem(uint32_t k)
{
    switch ((ppc_sem_t)ppc_mn_sem(k & 0x1FFu)) {
    case PPC_S_LOAD:
    case PPC_S_STORE:
    case PPC_S_LMW:
    case PPC_S_STMW:
    case PPC_S_LARX:
    case PPC_S_STCX:
    case PPC_S_LMV:
    case PPC_S_STMV:
    case PPC_S_DCBZ:
        return true;
    default:
        return false;
    }
}

/* What can compute an address: the arithmetic, logical and shift classes. */
static bool pair_kind_is_alu(uint32_t k)
{
    const uint32_t s = ppc_mn_sem(k & 0x1FFu);

    return s >= PPC_S_ADD && s <= PPC_S_RLWNM;
}

const emu_pair_ops_t ppc_pair_ops = {
    .kind = pair_kind,
    .rd = pair_rd,
    .rs1 = pair_rs1,
    .rs2 = pair_rs2,
    .kind_name = pair_kind_name,
    .kind_is_mem = pair_kind_is_mem,
    .kind_is_alu = pair_kind_is_alu,
};

#endif /* EMU_PAIR_STATS */
