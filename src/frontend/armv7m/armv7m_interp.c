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
static void armv7m_fault(armv7m_cpu_t *c, uint32_t pc, uint16_t insn)
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

    /* LDR/STR (immediate, word): 0110 L imm5 rn rt */
    if ((insn >> 13) == 3u && ((insn >> 12) & 1u) == 0u) {
        const uint32_t load = (insn >> 11) & 1u;
        const uint32_t addr =
            c->r[(insn >> 3) & 7u] + (((insn >> 6) & 31u) * 4u);
        const uint32_t rt = insn & 7u;

        return load ? ld32(c, addr, &c->r[rt]) : st32(c, addr, c->r[rt]);
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

        /*
         * 32-bit encodings are the whole second half of this
         * instruction set and none of them is implemented yet. Faulting
         * with the halfword recorded is the point: it names the
         * encoding to implement next rather than leaving a guest to
         * wander.
         */
        if (armv7m_is_32bit(hw)) {
            armv7m_fault(c, pc, hw);
            break;
        }

        c->r[ARMV7M_PC] = pc + 2u;
        if (!exec16(c, hw, pc)) {
            armv7m_fault(c, pc, hw);
            break;
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
