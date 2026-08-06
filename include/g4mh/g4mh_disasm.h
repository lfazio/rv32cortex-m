/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_disasm.h - Instruction disassembly, for tracing and the monitor.
 */
#ifndef G4MH_G4MH_DISASM_H
#define G4MH_G4MH_DISASM_H

#include "emu/emu_types.h"

#include "g4mh_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Format `insn` into buf. `insn` holds the first halfword in bits[15:0]
 * and the second, if the instruction has one, in bits[31:16] -- which is
 * what the trace hook is handed. `pc` resolves pc-relative branch and jump
 * targets. Always NUL terminates. Returns the number of characters
 * written, excluding the NUL.
 *
 * A 48-bit instruction cannot be rendered in full from 32 bits of
 * encoding, so its third halfword prints as an ellipsis rather than as a
 * wrong constant.
 */
size_t g4mh_disasm(char *buf, size_t buflen, uint32_t pc, uint32_t insn);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_DISASM_H */
