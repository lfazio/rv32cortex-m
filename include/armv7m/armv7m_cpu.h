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

static inline uint32_t armv7m_insn_len(uint16_t hw)
{
    return armv7m_is_32bit(hw) ? 4u : 2u;
}

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_CPU_H */
