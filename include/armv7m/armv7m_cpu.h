/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_cpu.h - ARMv7E-M as a *guest*.
 *
 * The fourth frontend, and the one with a motive the others did not
 * have: **it is how the Thumb-2 backend becomes testable without
 * hardware.** That backend can only be validated by flashing a board
 * and reading a UART -- three of the JIT bugs this project has found
 * were live for months because no host suite could reach them. With a
 * Cortex-M guest, an x86 host can run the firmware that contains the
 * Thumb-2 emitter, and the emitted code executes inside this
 * interpreter.
 *
 * **Named armv7m, not thumb2, and the prefix matters.** `src/backend/
 * thumb2/` owns every `t2_` symbol in the tree -- t2_add, t2_emit32,
 * t2_patch_branch -- and those are an *encoder*. This is a decoder.
 * Sharing a prefix between the thing that writes instructions and the
 * thing that reads them is how `rv_disasm` came to be mistaken for a
 * decoder twice, once printing confident nonsense for G4MH and once
 * reporting zero floating-point instructions in a hard-float build.
 *
 * Scope, stated up front because a frontend that overstates itself is
 * worse than one that is small: this is the integer core. No FPU, no
 * MPU, no exclusive monitor, no sleep modes. The NVIC is a stub. What
 * exists is enough to fetch, decode and execute, and to *report* what
 * it cannot do rather than skip it -- which is the lesson four frontends
 * have now taught, most recently that "unimplemented raises an
 * exception" is a claim about the guest, not about the emulator.
 */
#ifndef ARMV7M_CPU_H
#define ARMV7M_CPU_H

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARMV7M_NREGS 16u /* r0-r12, sp, lr, pc */
#define ARMV7M_SP 13u
#define ARMV7M_LR 14u
#define ARMV7M_PC 15u

/* APSR, in the bit positions the architecture puts them. */
#define ARMV7M_N (1u << 31)
#define ARMV7M_Z (1u << 30)
#define ARMV7M_C (1u << 29)
#define ARMV7M_V (1u << 28)

/* EPSR.T -- Thumb state. Clearing it is a UsageFault, not a mode switch:
 * an ARMv7-M core has no ARM state to switch to. */
#define ARMV7M_T (1u << 24)

/*
 * ITSTATE, and it lives **in xpsr at the two places the architecture
 * puts it** rather than in a field of its own: IT[1:0] in bits 26:25 and
 * IT[7:2] in bits 15:10.
 *
 * Split across two fields is awkward to read and is still the right
 * place to keep it. A separate field would be a second description of
 * one piece of state, and an MRS, an MSR or an exception entry -- which
 * stacks xPSR whole, ITSTATE included -- would then have to remember to
 * compose it. This tree has the rule written down from the G4MH INTC,
 * where `eic[]` and `imr[]` were two stores of one architectural bit and
 * the second was written by the guest and never read.
 *
 * The composed value is firstcond:mask. ITSTATE[7:4] is the condition
 * the *current* instruction runs under, and the low five bits shift left
 * after each one -- so each mask bit becomes the condition's low bit in
 * turn, which is how ITT and ITE need no separate decoding.
 */
#define ARMV7M_IT_LO_MASK (3u << 25)
#define ARMV7M_IT_HI_MASK (0x3Fu << 10)

static inline uint32_t armv7m_it_get(uint32_t xpsr)
{
    return ((xpsr >> 25) & 3u) | (((xpsr >> 10) & 0x3Fu) << 2);
}

static inline uint32_t armv7m_it_put(uint32_t xpsr, uint32_t it)
{
    return (xpsr & ~(ARMV7M_IT_LO_MASK | ARMV7M_IT_HI_MASK)) |
           ((it & 3u) << 25) | (((it >> 2) & 0x3Fu) << 10);
}

/*
 * In an IT block exactly when the low four bits are non-zero -- the
 * architecture's own test, and not "the condition field is non-zero":
 * EQ is condition 0b0000, so a block led by `IT EQ` would read as no
 * block at all.
 */
static inline bool armv7m_in_it(uint32_t xpsr)
{
    return (armv7m_it_get(xpsr) & 0x0Fu) != 0u;
}

typedef struct armv7m_cpu {
    /*
     * Hot state first, as rv_hart.h, g4mh_cpu.h and ppc_cpu.h all do.
     * `emu_cpu_t` is opaque and a frontend simply casts its own struct
     * to it -- there is nothing to embed.
     *
     * **pc is r[15], not a separate field.** In this architecture it is
     * a general register that happens to be the program counter, and
     * every instruction that writes it -- BX, POP with pc in the list,
     * an ordinary MOV -- is a branch. Splitting it would mean every
     * lowering remembering to check whether its destination was 15,
     * which is the kind of rule a decoder forgets on exactly one slot.
     */
    uint32_t r[ARMV7M_NREGS];
    uint32_t xpsr;

    emu_bus_t *bus;
    uint32_t coreid;
    emu_state_t state;

    /* Where the vector table lives. VTOR, which resets to 0. */
    uint32_t vtor;

    /* ---- the exception model ---- */

    /*
     * PRIMASK, and nothing else of the three.
     *
     * FAULTMASK and BASEPRI exist and are not implemented: FAULTMASK
     * needs HardFault escalation to mean anything, and BASEPRI needs
     * priorities to be compared rather than merely stored. Modelling
     * either badly would make an interrupt arrive when the guest had
     * disabled it, which is the shape of bug that looks like the
     * *handler* misbehaving.
     */
    uint32_t primask;

    /*
     * The interrupt controller. 32 external sources, which covers the
     * SysTick-plus-a-UART machines this frontend is for -- a real part
     * has up to 240 and the arrays would simply be longer.
     *
     * `pending` is the latch and `enabled` the mask, held apart because
     * the architecture does: a disabled source still latches, and
     * enabling it later delivers. Collapsing them loses an interrupt
     * that arrived while masked, which presents as a device that
     * sometimes does not answer.
     */
    uint32_t irq_pending;
    uint32_t irq_enabled;
    uint32_t irq_active;

    /*
     * SysTick, the one timer every Cortex-M has in the same place. The
     * 24-bit counter counts *down*, reloads from RVR, and sets COUNTFLAG
     * on the wrap -- a bit that clears when read, which is the detail a
     * polling guest depends on.
     */
    uint32_t systick_ctrl;
    uint32_t systick_reload;
    uint32_t systick_value;
    bool systick_countflag;

    /*
     * True while an exception handler is running, so a return can be
     * told from an ordinary branch.
     *
     * **Derived from the magic LR value, not from a counter.** ARMv7-M
     * has no "return from interrupt" instruction: a handler returns by
     * branching to an address of the form 0xFFFFFFFx, and the core
     * recognises *that* rather than tracking depth. A counter would
     * disagree with the architecture the moment a handler branched to a
     * tail-called function, which is what compilers do.
     */
    uint32_t nest;

    uint64_t retired;

    /*
     * The last fault, kept so a run that stops can say *why* rather than
     * only that it stopped.
     *
     * This exists from the first commit on purpose. Four frontends have
     * now recorded the same lesson -- a trap only reports if something
     * catches it -- and in three of them the cost was days of bisecting
     * a guest that produced plausible output while silently skipping
     * instructions.
     */
    uint32_t fault_pc;
    /*
     * The whole encoding, w0 in the high half -- the order objdump
     * prints a wide instruction in, so the number can be searched for
     * in a disassembly without rearranging it.
     */
    uint32_t fault_insn;
    bool faulted;
} armv7m_cpu_t;

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * Is this halfword the first of a 32-bit instruction?
 *
 * ARMv7-M encodes the answer in bits 15:11 of the first halfword: the
 * three values 0b11101, 0b11110 and 0b11111 introduce a 32-bit
 * encoding, everything else is a complete 16-bit one.
 *
 * **One copy of this rule, and a property test over all 65536
 * halfwords.** G4MH spelled its length rule twice -- once as "op6 below
 * 0x30 is 16 bits" and once in `g4mh_is_16bit` -- and the two came
 * apart on exactly one slot, which produced an *infinite loop* rather
 * than a wrong answer in an implementation that had never executed.
 * There is no second spelling here and the test asserts there cannot
 * be one.
 */
static inline bool armv7m_is_32bit(uint16_t hw)
{
    const uint16_t top = (uint16_t)(hw >> 11);

    return top == 0x1Du || top == 0x1Eu || top == 0x1Fu;
}

/*
 * The SysTick and NVIC register block, at the addresses every Cortex-M
 * puts them. Fixed by the architecture rather than by a part, which is
 * why they are here and not in a platform header.
 */
#define ARMV7M_SCS_BASE 0xE000E000u
#define ARMV7M_SCS_SIZE 0x1000u

/* Exception numbers: 15 is SysTick, and external IRQ n is 16 + n. */
#define ARMV7M_EXC_SYSTICK 15u
#define ARMV7M_EXC_EXTERNAL 16u

/*
 * The EXC_RETURN values a handler branches to. Bit 2 chooses which
 * stack was used; this frontend has only the main one, so the other
 * values are reported rather than guessed at.
 */
#define ARMV7M_EXC_RETURN_MASK 0xFFFFFFF0u

extern const struct emu_dev_ops armv7m_scs_ops;

static inline uint32_t armv7m_insn_len(uint16_t hw)
{
    return armv7m_is_32bit(hw) ? 4u : 2u;
}

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_CPU_H */
