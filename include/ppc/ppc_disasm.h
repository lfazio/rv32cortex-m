/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_disasm.h - e200z7 disassembly, for tracing and the monitor.
 */
#ifndef PPC_DISASM_H
#define PPC_DISASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Build the disassembler (useful for tracing; costs flash). */
#ifndef PPC_ENABLE_DISASM
#define PPC_ENABLE_DISASM 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Format one instruction, in the syntax binutils assembles with
 * -mregnames, and always NUL terminate. Returns the characters written.
 *
 * `insn` is the first halfword in bits 31:16 and the second in 15:0, the
 * order it sits in memory -- which is the opposite of the other
 * frontends' trace convention, and is what this frontend's trace hook
 * has always passed. `len` is 2 or 4.
 *
 * **What it prints is meant to be assembled again**: tests/ppc-check
 * feeds the output back through the assembler and requires the same
 * instruction, which is the test of both this and ppc_decode. Branch
 * targets are absolute addresses; the checker rewrites them.
 */
size_t ppc_disasm_mode(char *buf, size_t buflen, uint32_t pc, uint32_t insn,
                       unsigned len, bool vle);

/* For emu_cpu_ops_t.disasm: VLE, the mode this core runs. */
size_t ppc_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                  unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* PPC_DISASM_H */
