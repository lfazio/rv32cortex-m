/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_disasm.h - Instruction disassembly, for tracing and the monitor.
 */
#ifndef RV32_RV_DISASM_H
#define RV32_RV_DISASM_H

#include "rv_types.h"
#include "rv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Format `insn` (already expanded from RVC if it was compressed) into buf.
 * `pc` is used to resolve pc-relative branch and jump targets. Always NUL
 * terminates. Returns the number of characters written, excluding the NUL.
 *
 * `insn` is 64 bits and `len` is present because emu_cpu_ops_t.disasm is
 * -- RV32 has no encoding wider than 4 bytes, so the high half is always
 * zero and the length is always derivable from the low two bits. Both
 * are taken rather than a narrower signature so the frontend's function
 * can be the ops entry directly, with no shim to keep in step.
 */
size_t rv_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                 unsigned len);

/* ABI register name ("ra", "sp", "a0", ...) for 0..31. */
const char *rv_reg_name(unsigned r);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_DISASM_H */
