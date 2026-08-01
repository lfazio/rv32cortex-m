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
 */
size_t rv_disasm(char *buf, size_t buflen, uint32_t pc, uint32_t insn);

/* ABI register name ("ra", "sp", "a0", ...) for 0..31. */
const char *rv_reg_name(unsigned r);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_DISASM_H */
