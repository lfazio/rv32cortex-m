/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_ir.h - The RV32 frontend's IR lowering.
 *
 * What a host backend needs from this frontend: how to turn guest
 * instructions into IR, and where the guest keeps its registers and pc.
 * Nothing here names a host.
 */
#ifndef RV32_RV_IR_H
#define RV32_RV_IR_H

#include "emu/emu_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lower a run of instructions starting at `pc` into `b`, returning how
 * many were folded in -- 0 if nothing at `pc` could be lowered, which is
 * ordinary rather than an error. The block is reset first.
 */
uint32_t rv_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

/*
 * Where this guest's state lives. Every flag_bit is zero: RISC-V has no
 * condition flags, which is what makes the shared dead-flag pass free
 * for it.
 */
extern const emu_ir_target_t rv_ir_target;

/* Everything a host's jit.c needs from this frontend. */
extern const emu_ir_frontend_t rv_ir_frontend;

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_IR_H */
