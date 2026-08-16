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
 * Format `insn` into buf: halfwords in ascending order from bit 0, and
 * `len` the width in bytes -- 2, 4, 6 or 8. `pc` resolves pc-relative
 * branch and jump targets. Always NUL terminates. Returns the number of
 * characters written, excluding the NUL.
 *
 * `len` *is* derivable from the value on this ISA -- g4mh_insn_len and
 * g4mh_insn_is_48/is_64 do exactly that -- so it is not information
 * this function lacks. It is a **second opinion**: the caller ran those
 * same functions to fetch the instruction, and a disagreement means the
 * caller and the decoder have diverged, which is the defect this
 * frontend keeps recording. A mismatch prints `.short` with both
 * numbers rather than a plausible mnemonic.
 *
 * What the wider value bought is the rendering: a 48-bit form used to
 * print its third halfword as an ellipsis, and a 64-bit one could not
 * be told from it at all.
 */
size_t g4mh_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                   unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_DISASM_H */
