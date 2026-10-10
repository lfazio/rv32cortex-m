/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_disasm.h - Instruction disassembly, for tracing and the monitor.
 */
#ifndef ARMV7M_DISASM_H
#define ARMV7M_DISASM_H

#include <stddef.h>
#include <stdint.h>

/* Build the disassembler (useful for tracing; costs flash). */
#ifndef ARMV7M_ENABLE_DISASM
#define ARMV7M_ENABLE_DISASM 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Format one instruction into buf, in the unified assembler syntax, and
 * always NUL terminate. Returns the characters written, excluding the
 * NUL. `insn` holds the first halfword in bits 15:0 and the second in
 * 31:16; `len` is 2 or 4 and is a second opinion on the length, as it is
 * for G4MH -- a disagreement prints `.inst` with both numbers.
 *
 * **The text is meant to be assembled again**, and that is how it is
 * tested: tests/armv7m-diff/disasm-check.py feeds what this prints back
 * through `arm-none-eabi-as` and requires the encoding it started from.
 * So it prints a width qualifier wherever two encodings share a
 * spelling, `addw` rather than `add` for the plain 12-bit immediate, and
 * `.inst` -- not a guess -- for anything the decoder does not know.
 *
 * `itstate` is ITSTATE as it stands *for this instruction*: zero outside
 * an IT block. It matters more than a condition suffix: the 16-bit
 * data-processing forms set the flags outside an IT block and leave them
 * alone inside one, so the same two bytes are `adds` or `addeq`, and the
 * encoding cannot say which.
 */
size_t armv7m_disasm_it(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                        unsigned len, uint32_t itstate);

/*
 * The same, for emu_cpu_ops_t.disasm, which has no way to pass ITSTATE.
 *
 * It follows IT blocks itself: an `it` at pc arms the conditions for the
 * instructions at the addresses that follow, and each call that arrives
 * at the expected address consumes one. A call from anywhere else -- an
 * exception taken inside the block, a caller that skips -- drops the
 * state, so the worst case is an instruction printed without its
 * condition, never one printed with somebody else's.
 *
 * That state is static: one stream at a time. This frontend has one
 * core.
 */
size_t armv7m_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                     unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_DISASM_H */
