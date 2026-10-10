/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_fpu.h - the e200z7's embedded floating-point unit, scalar half.
 *
 * EFPU2 in mode 0, the only mode this core has: single precision in the
 * low word of a GPR, no infinities, NaNs or denormals produced, and a
 * default result for every input that is one (manual chapter 5 and
 * tables 5-2 to 5-5).
 */
#ifndef PPC_FPU_H
#define PPC_FPU_H

#include "ppc/ppc_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execute one scalar EFPU instruction, `sem` being its PPC_S_EFS*
 * semantics. Writes rD (or CR field `crf` for a compare) and SPEFSCR.
 *
 * Returns PPC_EXC_NONE; PPC_IVOR_FP_DATA when an enabled invalid,
 * divide-by-zero, overflow or underflow suppressed the instruction --
 * nothing written but SPEFSCR, and the interrupt names this
 * instruction; or PPC_IVOR_FP_ROUND when an enabled inexact result was
 * written *truncated*, and the interrupt names the next instruction.
 * The caller raises either with ESR[SPE].
 */
uint32_t ppc_fpu_exec(ppc_cpu_t *c, uint32_t sem, uint32_t rd, uint32_t ra,
                      uint32_t rb, uint32_t crf);

#ifdef __cplusplus
}
#endif

#endif /* PPC_FPU_H */
