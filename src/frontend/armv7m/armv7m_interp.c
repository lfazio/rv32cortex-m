/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_interp.c - the ARMv7E-M interpreter.
 *
 * **What is here is a fraction of the instruction set, and every
 * encoding that is not decodes to a reported fault rather than to a
 * skip.** That order is deliberate and is the one lesson four frontends
 * in this tree have each had to learn the hard way: an unimplemented
 * encoding that quietly advances the pc produces a guest which keeps
 * running and keeps printing plausible output, and the cost of finding
 * it later has been measured in sessions, not hours.
 *
 * So `armv7m_fault` records the pc and the halfword and stops the core.
 * A guest that hits one says exactly which encoding it was and where,
 * which is what turns "the output is wrong" into an objdump line.
 */

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_bus.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Faults                                                              */
/* ------------------------------------------------------------------ */

/*
 * Stop, and remember why.
 *
 * A real ARMv7-M takes a UsageFault here and vectors through VTOR. That
 * is the next step and not this one: vectoring to a handler a bare guest
 * has not installed lands on whatever is at that address, which is the
 * silent-wrong-answer shape this file exists to avoid. Halting is honest
 * while the frontend is incomplete, and the fault fields say what a
 * handler would have been told.
 */
static void armv7m_fault(armv7m_cpu_t *c, uint32_t pc, uint32_t insn)
{
    c->faulted = true;
    c->fault_pc = pc;
    c->fault_insn = insn;
    c->state = EMU_STATE_HALTED;
}

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

static void set_nz(armv7m_cpu_t *c, uint32_t v)
{
    c->xpsr &= ~(ARMV7M_N | ARMV7M_Z);
    if (v == 0u) {
        c->xpsr |= ARMV7M_Z;
    }
    if ((v & 0x80000000u) != 0u) {
        c->xpsr |= ARMV7M_N;
    }
}

/*
 * The carry and overflow of an addition, which is also how subtraction
 * is defined: a - b is a + ~b + 1, and the flags fall out of the same
 * arithmetic. Writing subtraction any other way is how a borrow ends up
 * inverted on one of the two paths.
 */
static void set_add_flags(armv7m_cpu_t *c, uint32_t a, uint32_t b,
                          uint32_t carry_in, uint32_t res)
{
    const uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)carry_in;

    c->xpsr &= ~(ARMV7M_C | ARMV7M_V);
    if ((wide >> 32) != 0u) {
        c->xpsr |= ARMV7M_C;
    }
    /* Overflow: the operands agreed in sign and the result did not. */
    if ((((a ^ res) & (b ^ res)) & 0x80000000u) != 0u) {
        c->xpsr |= ARMV7M_V;
    }
    set_nz(c, res);
}

static bool cond_holds(const armv7m_cpu_t *c, uint32_t cond)
{
    const bool n = (c->xpsr & ARMV7M_N) != 0u;
    const bool z = (c->xpsr & ARMV7M_Z) != 0u;
    const bool v = (c->xpsr & ARMV7M_V) != 0u;
    const bool cy = (c->xpsr & ARMV7M_C) != 0u;

    switch (cond >> 1) {
    case 0u:
        return (cond & 1u) ? !z : z; /* EQ / NE */
    case 1u:
        return (cond & 1u) ? !cy : cy; /* CS / CC */
    case 2u:
        return (cond & 1u) ? !n : n; /* MI / PL */
    case 3u:
        return (cond & 1u) ? !v : v; /* VS / VC */
    case 4u:
        return (cond & 1u) ? (!cy || z) : (cy && !z); /* HI / LS */
    case 5u:
        return (cond & 1u) ? (n != v) : (n == v); /* GE / LT */
    case 6u:
        return (cond & 1u) ? (z || (n != v)) : (!z && (n == v)); /* GT / LE */
    default:
        return true; /* AL; 0b1111 is not a condition in Thumb */
    }
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static bool ld32(armv7m_cpu_t *c, uint32_t addr, uint32_t *out)
{
    return emu_bus_read(c->bus, addr, 4u, out) == EMU_FAULT_NONE;
}

static bool st32(armv7m_cpu_t *c, uint32_t addr, uint32_t v)
{
    return emu_bus_write(c->bus, addr, 4u, v) == EMU_FAULT_NONE;
}

/*
 * Sized accesses. `size` is 1, 2 or 4 and `sext` says whether a narrow
 * load sign-extends -- LDRSB and LDRSH against LDRB and LDRH.
 */
static bool ldn(armv7m_cpu_t *c, uint32_t addr, uint32_t size, bool sext,
                uint32_t *out)
{
    uint32_t v = 0u;

    if (emu_bus_read(c->bus, addr, size, &v) != EMU_FAULT_NONE) {
        return false;
    }
    if (sext) {
        if (size == 1u) {
            v = (uint32_t)(int32_t)(int8_t)v;
        } else if (size == 2u) {
            v = (uint32_t)(int32_t)(int16_t)v;
        }
    }
    *out = v;
    return true;
}

static bool stn(armv7m_cpu_t *c, uint32_t addr, uint32_t size, uint32_t v)
{
    return emu_bus_write(c->bus, addr, size, v) == EMU_FAULT_NONE;
}

/* ------------------------------------------------------------------ */
/* Immediates and shifts                                              */
/* ------------------------------------------------------------------ */

/*
 * ThumbExpandImm: the 12-bit "modified immediate" the wide
 * data-processing forms carry, as i:imm3:imm8.
 *
 * **This is the most error-prone decode in the instruction set**, and
 * it is worth writing out rather than paraphrasing. With bits 11:10
 * clear the value is one of four byte patterns selected by bits 9:8;
 * otherwise it is `1:imm7` rotated right by the whole top five bits.
 * The second case is easy to write as a shift and be wrong for every
 * rotation that wraps.
 */
static uint32_t expand_imm12(uint32_t imm12)
{
    const uint32_t imm8 = imm12 & 0xFFu;

    if ((imm12 & 0xC00u) == 0u) {
        switch ((imm12 >> 8) & 3u) {
        case 0u:
            return imm8;
        case 1u:
            return (imm8 << 16) | imm8;
        case 2u:
            return (imm8 << 24) | (imm8 << 8);
        default:
            return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
        }
    }
    {
        const uint32_t v = 0x80u | (imm12 & 0x7Fu);
        const uint32_t rot = (imm12 >> 7) & 31u;

        return (v >> rot) | (v << (32u - rot));
    }
}

/*
 * The shift a wide data-processing instruction applies to its second
 * operand, and the one a 16-bit shift-immediate applies.
 *
 * **imm5 == 0 does not mean "no shift" for three of the four types.**
 * LSR #0 is LSR #32, ASR #0 is ASR #32 and ROR #0 is RRX -- the same
 * trap the Thumb-2 *backend* in this tree was caught by twice, once
 * where a RISC-V shift by zero came out as a shift by 32. Only LSL
 * reads zero as an identity.
 */
static uint32_t do_shift(uint32_t type, uint32_t amount, uint32_t v)
{
    switch (type) {
    case 0u: /* LSL */
        return (amount == 0u) ? v : (v << amount);
    case 1u: /* LSR; 0 means 32 */
        return (amount == 0u) ? 0u : (v >> amount);
    case 2u: /* ASR; 0 means 32 */
        return (amount == 0u) ? (uint32_t)((int32_t)v >> 31)
                              : (uint32_t)((int32_t)v >> amount);
    default: /* ROR, and 0 is RRX -- which needs the carry, so the
              * caller must not hand a zero amount here. */
        return (amount == 0u) ? v : ((v >> amount) | (v << (32u - amount)));
    }
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

/*
 * One 16-bit instruction.
 *
 * The subset is chosen to be what a first guest needs to prove the
 * frontend runs at all -- move, add, subtract, compare, load, store,
 * branch -- rather than what is easiest. Everything else faults with
 * its encoding recorded.
 *
 * **pc reads as the address of this instruction plus four.** Not plus
 * two: the architecture defines pc-relative reads against a pipeline
 * that fetched two halfwords ahead, and every literal load and
 * pc-relative address in compiled Thumb code depends on it. Getting it
 * wrong shifts every constant a guest loads by two bytes, which reads
 * as data corruption rather than as a decode bug.
 */
static bool exec16(armv7m_cpu_t *c, uint16_t insn, uint32_t pc)
{
    const uint32_t pc4 = pc + 4u;

    /*
     * Shift (immediate), add, subtract: bits 15:13 == 0b000.
     *
     * **Three bits, not two.** `insn >> 14` also matches 0b001, which is
     * the mov/cmp/add/sub-immediate group below -- so `movs r0, #16`
     * (0x2010) decoded as `lsls r0, r2, #0` and left r0 holding r2.
     * Nothing faulted; the guest simply stored to address 0 and the
     * frontend reported a bus error two instructions later.
     *
     * This is the e_bc defect from the PowerPC frontend in a new place:
     * a field read one bit too short aliases one group of instructions
     * onto another, and the result is a wrong answer rather than an
     * unrecognised encoding.
     */
    if ((insn >> 13) == 0u) {
        const uint32_t op = (insn >> 11) & 3u;
        const uint32_t imm5 = (insn >> 6) & 31u;
        const uint32_t rm = (insn >> 3) & 7u;
        const uint32_t rd = insn & 7u;

        switch (op) {
        case 0u: /* LSL (immediate) */
            c->r[rd] = c->r[rm] << imm5;
            set_nz(c, c->r[rd]);
            return true;
        case 1u: /* LSR (immediate); a shift of 0 means 32 */
            c->r[rd] = (imm5 == 0u) ? 0u : (c->r[rm] >> imm5);
            set_nz(c, c->r[rd]);
            return true;
        case 2u: /* ASR (immediate); likewise */
            c->r[rd] = (imm5 == 0u) ? (uint32_t)((int32_t)c->r[rm] >> 31)
                                    : (uint32_t)((int32_t)c->r[rm] >> imm5);
            set_nz(c, c->r[rd]);
            return true;
        default:
            break; /* 0b0001100..0b0001111: ADD/SUB register/immediate */
        }

        const uint32_t sub = (insn >> 9) & 1u;
        const uint32_t imm3_form = (insn >> 10) & 1u;
        const uint32_t rn = (insn >> 3) & 7u;
        const uint32_t operand =
            imm3_form ? ((insn >> 6) & 7u) : c->r[(insn >> 6) & 7u];
        const uint32_t a = c->r[rn];
        const uint32_t b = sub ? ~operand : operand;
        const uint32_t res = a + b + (sub ? 1u : 0u);

        c->r[insn & 7u] = res;
        set_add_flags(c, a, b, sub ? 1u : 0u, res);
        return true;
    }

    /* MOV/CMP/ADD/SUB immediate: 001xxx */
    if ((insn >> 13) == 1u) {
        const uint32_t op = (insn >> 11) & 3u;
        const uint32_t rd = (insn >> 8) & 7u;
        const uint32_t imm8 = insn & 0xFFu;

        switch (op) {
        case 0u: /* MOV */
            c->r[rd] = imm8;
            set_nz(c, imm8);
            return true;
        case 1u: { /* CMP */
            const uint32_t b = ~imm8;
            const uint32_t res = c->r[rd] + b + 1u;

            set_add_flags(c, c->r[rd], b, 1u, res);
            return true;
        }
        case 2u: { /* ADD */
            const uint32_t res = c->r[rd] + imm8;

            set_add_flags(c, c->r[rd], imm8, 0u, res);
            c->r[rd] = res;
            return true;
        }
        default: { /* SUB */
            const uint32_t b = ~imm8;
            const uint32_t res = c->r[rd] + b + 1u;

            set_add_flags(c, c->r[rd], b, 1u, res);
            c->r[rd] = res;
            return true;
        }
        }
    }

    /* LDR (literal): 01001 rt imm8 -- the pc-relative literal pool */
    if ((insn >> 11) == 0x09u) {
        const uint32_t rt = (insn >> 8) & 7u;
        const uint32_t addr = (pc4 & ~3u) + ((insn & 0xFFu) * 4u);

        return ld32(c, addr, &c->r[rt]);
    }

    /*
     * The register-operand ALU group: 0100 00xx xxxx xxxx.
     *
     * Sixteen operations in one slot, selected by bits 9:6, and the
     * low-register pair in bits 5:3 and 2:0. Several of them write no
     * destination (TST, CMP, CMN) and one writes the flags from a
     * negation (RSB #0), so the destination is decided per operation
     * rather than once.
     */
    if ((insn >> 10) == 0x10u) {
        const uint32_t op = (insn >> 6) & 15u;
        const uint32_t rm = (insn >> 3) & 7u;
        const uint32_t rd = insn & 7u;
        const uint32_t a = c->r[rd];
        const uint32_t b = c->r[rm];

        switch (op) {
        case 0u: /* AND */
            c->r[rd] = a & b;
            set_nz(c, c->r[rd]);
            return true;
        case 1u: /* EOR */
            c->r[rd] = a ^ b;
            set_nz(c, c->r[rd]);
            return true;
        case 2u: /* LSL (register) */
            c->r[rd] = ((b & 0xFFu) >= 32u) ? 0u : (a << (b & 0xFFu));
            set_nz(c, c->r[rd]);
            return true;
        case 3u: /* LSR (register) */
            c->r[rd] = ((b & 0xFFu) >= 32u) ? 0u : (a >> (b & 0xFFu));
            set_nz(c, c->r[rd]);
            return true;
        case 4u: /* ASR (register) */
            c->r[rd] = ((b & 0xFFu) >= 32u)
                           ? (uint32_t)((int32_t)a >> 31)
                           : (uint32_t)((int32_t)a >> (b & 0xFFu));
            set_nz(c, c->r[rd]);
            return true;
        case 5u: { /* ADC */
            const uint32_t ci = (c->xpsr & ARMV7M_C) ? 1u : 0u;
            const uint32_t res = a + b + ci;

            set_add_flags(c, a, b, ci, res);
            c->r[rd] = res;
            return true;
        }
        case 6u: { /* SBC */
            const uint32_t ci = (c->xpsr & ARMV7M_C) ? 1u : 0u;
            const uint32_t nb = ~b;
            const uint32_t res = a + nb + ci;

            set_add_flags(c, a, nb, ci, res);
            c->r[rd] = res;
            return true;
        }
        case 7u: /* ROR (register) */
            if ((b & 31u) != 0u) {
                c->r[rd] = do_shift(3u, b & 31u, a);
            }
            set_nz(c, c->r[rd]);
            return true;
        case 8u: /* TST */
            set_nz(c, a & b);
            return true;
        case 9u: { /* RSB rd, rm, #0 -- the negation */
            const uint32_t na = ~b;
            const uint32_t res = na + 1u;

            set_add_flags(c, na, 0u, 1u, res);
            c->r[rd] = res;
            return true;
        }
        case 10u: { /* CMP */
            const uint32_t nb = ~b;
            const uint32_t res = a + nb + 1u;

            set_add_flags(c, a, nb, 1u, res);
            return true;
        }
        case 11u: { /* CMN */
            const uint32_t res = a + b;

            set_add_flags(c, a, b, 0u, res);
            return true;
        }
        case 12u: /* ORR */
            c->r[rd] = a | b;
            set_nz(c, c->r[rd]);
            return true;
        case 13u: /* MUL */
            c->r[rd] = a * b;
            set_nz(c, c->r[rd]);
            return true;
        case 14u: /* BIC */
            c->r[rd] = a & ~b;
            set_nz(c, c->r[rd]);
            return true;
        default: /* MVN */
            c->r[rd] = ~b;
            set_nz(c, c->r[rd]);
            return true;
        }
    }

    /*
     * The high-register forms: 0100 01xx. ADD, CMP and MOV with either
     * operand allowed to be any of the sixteen registers -- which is how
     * `bx lr` and `mov r8, r0` are spelled, and how a branch through a
     * register happens at all.
     */
    if ((insn >> 10) == 0x11u) {
        const uint32_t op = (insn >> 8) & 3u;
        const uint32_t rm = (insn >> 3) & 15u;
        const uint32_t rd = (uint32_t)((insn & 7u) | ((insn >> 4) & 8u));

        switch (op) {
        case 0u: /* ADD (register, high) -- no flags */
            c->r[rd] += (rm == ARMV7M_PC) ? pc4 : c->r[rm];
            return true;
        case 1u: { /* CMP (register, high) */
            const uint32_t b = ~c->r[rm];
            const uint32_t res = c->r[rd] + b + 1u;

            set_add_flags(c, c->r[rd], b, 1u, res);
            return true;
        }
        case 2u: /* MOV (register, high) -- no flags */
            c->r[rd] = (rm == ARMV7M_PC) ? pc4 : c->r[rm];
            return true;
        default: {
            /*
             * BX and BLX. The link register takes the address of the
             * *next* instruction with the Thumb bit set, which is what
             * lets the matching BX return to it.
             */
            const uint32_t target = c->r[rm];

            if (((insn >> 7) & 1u) != 0u) { /* BLX */
                c->r[ARMV7M_LR] = (pc + 2u) | 1u;
            }
            c->r[ARMV7M_PC] = target & ~1u;
            return true;
        }
        }
    }

    /* LDR/STR (immediate): 011 B L imm5 rn rt, word or byte */
    if ((insn >> 13) == 3u) {
        const bool byte = ((insn >> 12) & 1u) != 0u;
        const uint32_t load = (insn >> 11) & 1u;
        const uint32_t off = ((insn >> 6) & 31u) * (byte ? 1u : 4u);
        const uint32_t addr = c->r[(insn >> 3) & 7u] + off;
        const uint32_t rt = insn & 7u;
        const uint32_t size = byte ? 1u : 4u;

        return load ? ldn(c, addr, size, false, &c->r[rt])
                    : stn(c, addr, size, c->r[rt]);
    }

    /* LDRH/STRH (immediate): 1000 L imm5 rn rt */
    if ((insn >> 12) == 8u) {
        const uint32_t load = (insn >> 11) & 1u;
        const uint32_t addr =
            c->r[(insn >> 3) & 7u] + (((insn >> 6) & 31u) * 2u);
        const uint32_t rt = insn & 7u;

        return load ? ldn(c, addr, 2u, false, &c->r[rt])
                    : stn(c, addr, 2u, c->r[rt]);
    }

    /* LDR/STR (sp-relative): 1001 L rt imm8 -- how locals are reached */
    if ((insn >> 12) == 9u) {
        const uint32_t load = (insn >> 11) & 1u;
        const uint32_t rt = (insn >> 8) & 7u;
        const uint32_t addr = c->r[ARMV7M_SP] + ((insn & 0xFFu) * 4u);

        return load ? ld32(c, addr, &c->r[rt]) : st32(c, addr, c->r[rt]);
    }

    /* ADR and ADD (sp plus immediate): 1010 S rd imm8 */
    if ((insn >> 12) == 0x0Au) {
        const uint32_t rd = (insn >> 8) & 7u;
        const uint32_t imm = (insn & 0xFFu) * 4u;

        c->r[rd] = (((insn >> 11) & 1u) != 0u) ? (c->r[ARMV7M_SP] + imm)
                                               : ((pc4 & ~3u) + imm);
        return true;
    }

    /* ADD/SUB sp, #imm7*4: 1011 0000 S imm7 */
    if ((insn >> 8) == 0xB0u) {
        const uint32_t imm = (insn & 0x7Fu) * 4u;

        if (((insn >> 7) & 1u) != 0u) {
            c->r[ARMV7M_SP] -= imm;
        } else {
            c->r[ARMV7M_SP] += imm;
        }
        return true;
    }

    /*
     * PUSH and POP: 1011 L10 R register_list.
     *
     * **POP with pc in the list is a return**, which is why this cannot
     * be a loop over registers that ignores r15: the Thumb bit comes off
     * the popped value exactly as it does for BX.
     */
    if ((insn >> 12) == 0x0Bu && ((insn >> 9) & 3u) == 2u) {
        const bool pop = ((insn >> 11) & 1u) != 0u;
        const uint32_t extra = ((insn >> 8) & 1u) != 0u;
        uint32_t sp = c->r[ARMV7M_SP];

        if (pop) {
            for (uint32_t i = 0u; i < 8u; i++) {
                if ((insn & (1u << i)) != 0u) {
                    if (!ld32(c, sp, &c->r[i])) {
                        return false;
                    }
                    sp += 4u;
                }
            }
            if (extra) { /* pc */
                uint32_t t = 0u;

                if (!ld32(c, sp, &t)) {
                    return false;
                }
                sp += 4u;
                c->r[ARMV7M_PC] = t & ~1u;
            }
        } else {
            /* Pushed highest-first, so the lowest register ends up at
             * the lowest address -- the order POP undoes. */
            if (extra) { /* lr */
                sp -= 4u;
                if (!st32(c, sp, c->r[ARMV7M_LR])) {
                    return false;
                }
            }
            for (int32_t i = 7; i >= 0; i--) {
                if ((insn & (1u << (uint32_t)i)) != 0u) {
                    sp -= 4u;
                    if (!st32(c, sp, c->r[(uint32_t)i])) {
                        return false;
                    }
                }
            }
        }
        c->r[ARMV7M_SP] = sp;
        return true;
    }

    /* CBZ/CBNZ: 1011 o0i1 imm5 rn -- a compare and branch in one */
    if ((insn >> 12) == 0x0Bu && ((insn >> 8) & 5u) == 1u) {
        const uint32_t rn = insn & 7u;
        const bool nonzero = ((insn >> 11) & 1u) != 0u;
        const uint32_t imm =
            (((insn >> 3) & 31u) * 2u) | (((insn >> 9) & 1u) << 6);

        if ((c->r[rn] != 0u) == nonzero) {
            c->r[ARMV7M_PC] = pc4 + imm;
        }
        return true;
    }

    /*
     * The hint space: 1011 1111 opA opB. NOP, YIELD, WFE, WFI and SEV
     * all retire with no effect here -- there is no second core to yield
     * to and no interrupt to wait for yet.
     *
     * **IT is in this slot and is deliberately not implemented.** It
     * makes the next four instructions conditional, which means a
     * decoder that is not stateless; faulting is honest until that is
     * built, and silently treating it as a NOP would execute the
     * instructions it guards unconditionally -- a wrong answer, not a
     * missing feature.
     */
    if ((insn >> 8) == 0xBFu) {
        return (insn & 0x00FFu) == 0u ||
               ((insn & 0x000Fu) == 0u && (insn & 0x00F0u) <= 0x0050u);
    }

    /* Conditional branch and the supervisor calls: 1101 cond imm8 */
    if ((insn >> 12) == 0x0Du) {
        const uint32_t cond = (insn >> 8) & 15u;

        /*
         * 0b1110 is a permanently-undefined encoding and 0b1111 is SVC.
         * Neither is a condition, and treating them as one is the
         * classic Thumb decode slip -- `cond == 15` is exactly the
         * awkward input this project's own rule says to test.
         */
        if (cond == 14u || cond == 15u) {
            return false;
        }
        if (cond_holds(c, cond)) {
            const int32_t off = (int32_t)(int8_t)(insn & 0xFFu) * 2;

            c->r[ARMV7M_PC] = (uint32_t)((int32_t)pc4 + off);
        }
        return true;
    }

    /* Unconditional branch: 11100 imm11 */
    if ((insn >> 11) == 0x1Cu) {
        int32_t off = (int32_t)((uint32_t)(insn & 0x7FFu) << 1);

        if ((off & 0x800) != 0) {
            off |= (int32_t)0xFFFFF000; /* sign extend from 12 bits */
        }
        c->r[ARMV7M_PC] = (uint32_t)((int32_t)pc4 + off);
        return true;
    }

    /*
     * BKPT: 1011 1110 imm8. The guest's way of saying it is finished,
     * and how a test ends without needing a semihosting call.
     */
    if ((insn >> 8) == 0xBEu) {
        c->state = EMU_STATE_HALTED;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* 32-bit encodings                                                    */
/* ------------------------------------------------------------------ */

/*
 * The wide data-processing result, shared by the immediate and register
 * forms: they differ only in where the second operand comes from.
 *
 * `setflags` is the S bit, and the architecture spends several encodings
 * on "S set with rd == pc" meaning something else entirely (CMP, TST,
 * and the compare forms) -- which the callers resolve before arriving
 * here.
 */
static bool dp_apply(armv7m_cpu_t *c, uint32_t op, uint32_t rd, uint32_t rn,
                     uint32_t b, bool setflags)
{
    const uint32_t a = c->r[rn];
    uint32_t res;

    switch (op) {
    case 0u: /* AND */
        res = a & b;
        break;
    case 1u: /* BIC */
        res = a & ~b;
        break;
    case 2u: /* ORR, and MOV when rn is pc */
        res = (rn == ARMV7M_PC) ? b : (a | b);
        break;
    case 3u: /* ORN, and MVN when rn is pc */
        res = (rn == ARMV7M_PC) ? ~b : (a | ~b);
        break;
    case 4u: /* EOR */
        res = a ^ b;
        break;
    case 8u: /* ADD */
        res = a + b;
        if (setflags) {
            set_add_flags(c, a, b, 0u, res);
        }
        if (rd != ARMV7M_PC) {
            c->r[rd] = res;
        }
        return rd != ARMV7M_PC;
    case 10u: { /* ADC */
        const uint32_t ci = (c->xpsr & ARMV7M_C) ? 1u : 0u;

        res = a + b + ci;
        if (setflags) {
            set_add_flags(c, a, b, ci, res);
        }
        c->r[rd] = res;
        return true;
    }
    case 11u: { /* SBC */
        const uint32_t ci = (c->xpsr & ARMV7M_C) ? 1u : 0u;
        const uint32_t nb = ~b;

        res = a + nb + ci;
        if (setflags) {
            set_add_flags(c, a, nb, ci, res);
        }
        c->r[rd] = res;
        return true;
    }
    case 13u: { /* SUB */
        const uint32_t nb = ~b;

        res = a + nb + 1u;
        if (setflags) {
            set_add_flags(c, a, nb, 1u, res);
        }
        if (rd != ARMV7M_PC) {
            c->r[rd] = res;
        }
        return rd != ARMV7M_PC;
    }
    case 14u: { /* RSB */
        const uint32_t na = ~a;

        res = na + b + 1u;
        if (setflags) {
            set_add_flags(c, na, b, 1u, res);
        }
        c->r[rd] = res;
        return true;
    }
    default:
        return false;
    }

    /* The logical group falls through to here: N and Z only. */
    if (setflags) {
        set_nz(c, res);
    }
    c->r[rd] = res;
    return true;
}

/*
 * One 32-bit instruction, as the pair of halfwords the architecture
 * describes it by rather than as a single word -- which is how the
 * manual is organised and how the field positions stay readable.
 */
static bool exec32(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t pc4 = pc + 4u;
    const uint32_t op1 = (uint32_t)((w0 >> 11) & 3u);

    /* ---- branches and the data-processing immediates: 11110 ---- */
    if (op1 == 2u) {
        /* B/BL/BLX: w1's bit 15 set */
        if ((w1 & 0x8000u) != 0u) {
            const uint32_t s = (uint32_t)((w0 >> 10) & 1u);
            const uint32_t typ = (uint32_t)((w1 >> 12) & 5u);

            if (typ == 5u || typ == 4u) { /* BL (T1) and BLX */
                const uint32_t j1 = (uint32_t)((w1 >> 13) & 1u);
                const uint32_t j2 = (uint32_t)((w1 >> 11) & 1u);
                const uint32_t i1 = 1u - (j1 ^ s);
                const uint32_t i2 = 1u - (j2 ^ s);
                int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                        ((uint32_t)(w0 & 0x3FFu) << 12) |
                                        ((uint32_t)(w1 & 0x7FFu) << 1));

                if (s != 0u) {
                    off |= (int32_t)0xFE000000;
                }
                c->r[ARMV7M_LR] = pc4 | 1u;
                c->r[ARMV7M_PC] = (uint32_t)((int32_t)pc4 + off);
                return true;
            }
            if (typ == 1u) { /* B (T4), unconditional */
                const uint32_t j1 = (uint32_t)((w1 >> 13) & 1u);
                const uint32_t j2 = (uint32_t)((w1 >> 11) & 1u);
                const uint32_t i1 = 1u - (j1 ^ s);
                const uint32_t i2 = 1u - (j2 ^ s);
                int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                        ((uint32_t)(w0 & 0x3FFu) << 12) |
                                        ((uint32_t)(w1 & 0x7FFu) << 1));

                if (s != 0u) {
                    off |= (int32_t)0xFE000000;
                }
                c->r[ARMV7M_PC] = (uint32_t)((int32_t)pc4 + off);
                return true;
            }
            if (typ == 0u) { /* B (T3), conditional */
                const uint32_t cond = (uint32_t)((w0 >> 6) & 15u);
                const uint32_t j1 = (uint32_t)((w1 >> 13) & 1u);
                const uint32_t j2 = (uint32_t)((w1 >> 11) & 1u);
                int32_t off = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) |
                                        ((uint32_t)(w0 & 0x3Fu) << 12) |
                                        ((uint32_t)(w1 & 0x7FFu) << 1));

                if (cond >= 14u) {
                    return false; /* not conditions */
                }
                if (s != 0u) {
                    off |= (int32_t)0xFFE00000;
                }
                if (cond_holds(c, cond)) {
                    c->r[ARMV7M_PC] = (uint32_t)((int32_t)pc4 + off);
                }
                return true;
            }
            return false;
        }

        /*
         * MOVW and MOVT: the only way to build an arbitrary 32-bit
         * constant without a literal pool.
         *
         * Bits 9:4 of w0 select them -- 0b100100 and 0b101100. The first
         * version masked with 0x7B, which does not contain bit 2, so the
         * comparison against 0x24 could never be true and **neither
         * instruction decoded at all**. The compiler said so
         * (-Wtautological-compare); nothing else would have, because the
         * fall-through is a reported fault and the first C guest happened
         * to use the modified-immediate form instead.
         */
        if (((w0 >> 4) & 0x3Fu) == 0x24u || ((w0 >> 4) & 0x3Fu) == 0x2Cu) {
            const uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t imm16 = (uint32_t)((w0 & 0xFu) << 12) |
                                   (uint32_t)(((w0 >> 10) & 1u) << 11) |
                                   (uint32_t)(((w1 >> 12) & 7u) << 8) |
                                   (uint32_t)(w1 & 0xFFu);

            if (((w0 >> 7) & 1u) != 0u) { /* MOVT: the top half only */
                c->r[rd] = (c->r[rd] & 0xFFFFu) | (imm16 << 16);
            } else {
                c->r[rd] = imm16;
            }
            return true;
        }

        /*
         * DSB, DMB and ISB: 1111 0011 1011. They retire with no effect
         * -- one core, one thread, so there is no reordering for them to
         * prevent, which is the same reasoning recorded for G4MH's SYNC
         * family. The exception there applies here too: an
         * instruction-sync has to discard translations, and that matters
         * only once this frontend has a JIT.
         */
        if ((w0 & 0xFFF0u) == 0xF3B0u) {
            return true;
        }

        /*
         * Data processing (modified immediate).
         *
         * **Bit 9 alone, not bits 10:9.** Bit 10 is the immediate's `i`
         * field, so testing both rejected every such instruction with
         * the high bit of its immediate set -- half of them. The first C
         * guest's `mov.w r3, #0x10000000` has i == 0 and so worked,
         * which is exactly the kind of accident that leaves a decode bug
         * in place.
         */
        if (((w0 >> 9) & 1u) == 0u) {
            const uint32_t op = (uint32_t)((w0 >> 5) & 15u);
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const bool setflags = ((w0 >> 4) & 1u) != 0u;
            uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t imm12 = (uint32_t)(((w0 >> 10) & 1u) << 11) |
                                   (uint32_t)(((w1 >> 12) & 7u) << 8) |
                                   (uint32_t)(w1 & 0xFFu);
            const uint32_t b = expand_imm12(imm12);

            /*
             * **S with rd == pc is a different instruction**, not a
             * write to the pc: TST for AND, TEQ for EOR, CMN for ADD
             * and CMP for SUB. Treating them as the arithmetic form
             * would write a result into r15 and branch somewhere
             * computed, which is the worst available failure.
             */
            if (setflags && rd == ARMV7M_PC) {
                switch (op) {
                case 0u:
                    set_nz(c, c->r[rn] & b);
                    return true;
                case 4u:
                    set_nz(c, c->r[rn] ^ b);
                    return true;
                case 8u: {
                    const uint32_t res = c->r[rn] + b;

                    set_add_flags(c, c->r[rn], b, 0u, res);
                    return true;
                }
                case 13u: {
                    const uint32_t nb = ~b;
                    const uint32_t res = c->r[rn] + nb + 1u;

                    set_add_flags(c, c->r[rn], nb, 1u, res);
                    return true;
                }
                default:
                    return false;
                }
            }
            return dp_apply(c, op, rd, rn, b, setflags);
        }
        return false;
    }

    /*
     * Everything else, dispatched on exact w0 patterns rather than on
     * op1.
     *
     * **The first version gated this on `op1 == 3` and three of the
     * handlers inside it were unreachable.** A 32-bit Thumb-2
     * instruction has bits 15:11 of 0b11101, 0b11110 or 0b11111 -- so
     * `(w0 >> 11) & 3` is 1, 2 or 3, and the whole 0xE8xx-0xEFxx range
     * (load/store multiple, and data processing on a shifted register)
     * is op1 == 1. `add.w r5, r5, r5` reported as an unimplemented
     * encoding while its handler sat a few lines below, in a block the
     * decoder could not enter.
     *
     * The patterns below are exact and mutually exclusive, so the gate
     * bought nothing even when it was right.
     */
    {
        /* Data processing (shifted register): 1110101x */
        if ((w0 & 0xFE00u) == 0xEA00u) {
            const uint32_t op = (uint32_t)((w0 >> 5) & 15u);
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const bool setflags = ((w0 >> 4) & 1u) != 0u;
            const uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t rm = (uint32_t)(w1 & 15u);
            const uint32_t typ = (uint32_t)((w1 >> 4) & 3u);
            const uint32_t amount =
                (uint32_t)(((w1 >> 12) & 7u) << 2) | (uint32_t)((w1 >> 6) & 3u);
            const uint32_t b = do_shift(typ, amount, c->r[rm]);

            if (setflags && rd == ARMV7M_PC) {
                switch (op) {
                case 0u:
                    set_nz(c, c->r[rn] & b);
                    return true;
                case 4u:
                    set_nz(c, c->r[rn] ^ b);
                    return true;
                case 8u: {
                    const uint32_t res = c->r[rn] + b;

                    set_add_flags(c, c->r[rn], b, 0u, res);
                    return true;
                }
                case 13u: {
                    const uint32_t nb = ~b;
                    const uint32_t res = c->r[rn] + nb + 1u;

                    set_add_flags(c, c->r[rn], nb, 1u, res);
                    return true;
                }
                default:
                    return false;
                }
            }
            return dp_apply(c, op, rd, rn, b, setflags);
        }

        /*
         * Load and store, single, with every addressing mode in one
         * place: 1111 100x xxxx. Size and signedness come from w0, and
         * the mode from w1 -- T3 is a 12-bit positive offset, T4 is an
         * 8-bit one with explicit index/add/writeback bits.
         */
        if ((w0 & 0xFE00u) == 0xF800u) {
            const uint32_t sz = (uint32_t)((w0 >> 5) & 3u);
            const bool load = ((w0 >> 4) & 1u) != 0u;
            const bool sext = ((w0 >> 8) & 1u) != 0u;
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const uint32_t rt = (uint32_t)((w1 >> 12) & 15u);
            const uint32_t size = (sz == 0u) ? 1u : ((sz == 1u) ? 2u : 4u);
            bool index = true;
            bool add = true;
            bool wback = false;
            uint32_t off;

            if (sz > 2u) {
                return false;
            }
            if (((w0 >> 7) & 1u) != 0u) { /* T3: imm12, always add */
                off = (uint32_t)(w1 & 0xFFFu);
            } else if ((w1 & 0x0800u) != 0u) { /* T4: imm8 with P/U/W */
                off = (uint32_t)(w1 & 0xFFu);
                index = ((w1 >> 10) & 1u) != 0u;
                add = ((w1 >> 9) & 1u) != 0u;
                wback = ((w1 >> 8) & 1u) != 0u;
            } else if ((w1 & 0x0FC0u) == 0u) { /* register offset */
                const uint32_t rm = (uint32_t)(w1 & 15u);

                off = c->r[rm] << ((w1 >> 4) & 3u);
            } else {
                return false;
            }

            const uint32_t base = (rn == ARMV7M_PC) ? (pc4 & ~3u) : c->r[rn];
            const uint32_t ea = index ? (add ? base + off : base - off) : base;

            if (load) {
                uint32_t v = 0u;

                if (!ldn(c, ea, size, sext, &v)) {
                    return false;
                }
                /* The writeback happens whether or not rt == rn, and the
                 * loaded value wins -- so write the base first. */
                if (wback && rn != ARMV7M_PC) {
                    c->r[rn] = add ? base + off : base - off;
                }
                c->r[rt] = v;
            } else {
                if (!stn(c, ea, size, c->r[rt])) {
                    return false;
                }
                if (wback && rn != ARMV7M_PC) {
                    c->r[rn] = add ? base + off : base - off;
                }
            }
            return true;
        }

        /*
         * Shift by register: 1111 1010 0ttS Rn | 1111 Rd 0000 Rm.
         *
         * The wide forms of LSL, LSR, ASR and ROR, which is what a
         * compiler emits for any shift by a variable amount once the
         * operands are not all low registers. **The amount is the low
         * eight bits of Rm, not five** -- a shift by 32 or more yields
         * zero rather than wrapping, which is the opposite of what
         * masking to 31 would give.
         */
        if ((w0 & 0xFF80u) == 0xFA00u && (w1 & 0xF0F0u) == 0xF000u) {
            const uint32_t typ = (uint32_t)((w0 >> 5) & 3u);
            const bool setflags = ((w0 >> 4) & 1u) != 0u;
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t rm = (uint32_t)(w1 & 15u);
            const uint32_t amount = c->r[rm] & 0xFFu;
            const uint32_t v = c->r[rn];
            uint32_t res;

            if (typ == 3u) { /* ROR, by the low five bits */
                res = do_shift(3u, amount & 31u, v);
            } else if (amount >= 32u) {
                res = (typ == 2u) ? (uint32_t)((int32_t)v >> 31) : 0u;
            } else if (amount == 0u) {
                res = v;
            } else {
                res = do_shift(typ, amount, v);
            }
            c->r[rd] = res;
            if (setflags) {
                set_nz(c, res);
            }
            return true;
        }

        /* Multiply: MUL and MLA/MLS share 1111 1011 0000 */
        if ((w0 & 0xFFF0u) == 0xFB00u) {
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const uint32_t ra = (uint32_t)((w1 >> 12) & 15u);
            const uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t rm = (uint32_t)(w1 & 15u);
            const uint32_t prod = c->r[rn] * c->r[rm];

            if (((w1 >> 4) & 15u) == 0u) {
                c->r[rd] = (ra == ARMV7M_PC) ? prod : (c->r[ra] + prod);
                return true;
            }
            if (((w1 >> 4) & 15u) == 1u) { /* MLS */
                c->r[rd] = c->r[ra] - prod;
                return true;
            }
            return false;
        }

        /* SDIV and UDIV: 1111 1011 1x01 */
        if ((w0 & 0xFFD0u) == 0xFB90u && ((w1 >> 4) & 15u) == 15u) {
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const uint32_t rd = (uint32_t)((w1 >> 8) & 15u);
            const uint32_t rm = (uint32_t)(w1 & 15u);
            const uint32_t a = c->r[rn];
            const uint32_t b = c->r[rm];

            /*
             * **ARM returns zero for a division by zero**, and that is
             * the architecture's answer rather than an approximation --
             * the same fact the IR's EMU_IR_DIV* precondition exists
             * because of, from the other side.
             */
            if (b == 0u) {
                c->r[rd] = 0u;
            } else if (((w0 >> 5) & 1u) != 0u) { /* UDIV */
                c->r[rd] = a / b;
            } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
                c->r[rd] = 0x80000000u;
            } else {
                c->r[rd] = (uint32_t)((int32_t)a / (int32_t)b);
            }
            return true;
        }

        /*
         * LDM and STM, which is how a wide PUSH/POP is spelled:
         * 1110 100x x0x1 for LDMIA/STMDB and friends.
         */
        if ((w0 & 0xFE40u) == 0xE800u) {
            const bool load = ((w0 >> 4) & 1u) != 0u;
            const bool before = ((w0 >> 8) & 1u) != 0u; /* DB form */
            const bool wback = ((w0 >> 5) & 1u) != 0u;
            const uint32_t rn = (uint32_t)(w0 & 15u);
            const uint32_t list = w1;
            uint32_t count = 0u;

            for (uint32_t i = 0u; i < 16u; i++) {
                if ((list & (1u << i)) != 0u) {
                    count++;
                }
            }

            uint32_t addr = before ? (c->r[rn] - count * 4u) : c->r[rn];

            for (uint32_t i = 0u; i < 16u; i++) {
                if ((list & (1u << i)) == 0u) {
                    continue;
                }
                if (load) {
                    uint32_t v = 0u;

                    if (!ld32(c, addr, &v)) {
                        return false;
                    }
                    if (i == ARMV7M_PC) {
                        c->r[ARMV7M_PC] = v & ~1u;
                    } else {
                        c->r[i] = v;
                    }
                } else if (!st32(c, addr, c->r[i])) {
                    return false;
                }
                addr += 4u;
            }
            if (wback) {
                c->r[rn] =
                    before ? (c->r[rn] - count * 4u) : (c->r[rn] + count * 4u);
            }
            return true;
        }

        return false;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

emu_run_reason_t armv7m_run(armv7m_cpu_t *c, uint32_t budget, uint32_t *retired)
{
    uint32_t done = 0u;

    while (done < budget) {
        if (c->state != EMU_STATE_RUNNING) {
            break;
        }

        const uint32_t pc = c->r[ARMV7M_PC];
        uint32_t lo = 0u;

        if (emu_bus_read(c->bus, pc, 2u, &lo) != EMU_FAULT_NONE) {
            armv7m_fault(c, pc, 0u);
            break;
        }

        const uint16_t hw = (uint16_t)lo;

        if (armv7m_is_32bit(hw)) {
            uint32_t hi = 0u;

            /*
             * The second halfword, and it gets its own read because it
             * can be in a different page from the first -- which is the
             * straddle case the RV32 frontend had to learn about
             * separately. Nothing maps pages here yet, but a fetch that
             * assumes both halves are reachable together is a bug
             * waiting for the first MPU.
             */
            if (emu_bus_read(c->bus, pc + 2u, 2u, &hi) != EMU_FAULT_NONE) {
                armv7m_fault(c, pc, hw);
                break;
            }

            c->r[ARMV7M_PC] = pc + 4u;
            if (!exec32(c, hw, (uint16_t)hi, pc)) {
                armv7m_fault(c, pc, ((uint32_t)hw << 16) | (hi & 0xFFFFu));
                break;
            }
        } else {
            c->r[ARMV7M_PC] = pc + 2u;
            if (!exec16(c, hw, pc)) {
                armv7m_fault(c, pc, hw);
                break;
            }
        }

        /* r15 is a register like any other, and writing it is a branch --
         * but x0-style hardwiring does not exist here, so nothing needs
         * undoing. */
        done++;
        c->retired++;
    }

    if (retired != NULL) {
        *retired = done;
    }
    if (c->state == EMU_STATE_HALTED) {
        return EMU_RUN_HALTED;
    }
    return EMU_RUN_BUDGET;
}
