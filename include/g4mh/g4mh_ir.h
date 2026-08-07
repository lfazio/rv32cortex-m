/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ir.h - The G4MH frontend's IR lowering.
 *
 * What a host backend needs from this frontend: how to turn guest
 * instructions into IR, and where the guest keeps its registers, flags
 * and pc. Nothing here names a host.
 */
#ifndef G4MH_IR_H
#define G4MH_IR_H

#include "emu/emu_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lower a run of guest instructions starting at `pc` into `b`, and
 * return how many were folded in -- 0 if nothing at `pc` could be
 * lowered, which is ordinary and frequent rather than an error.
 *
 * The block is reset first, so `b` may be reused across calls.
 */
uint32_t g4mh_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

/* Where this guest's state lives, for emu_ir_lower and emu_ir_interp. */
extern const emu_ir_target_t g4mh_ir_target;

/* Everything a host's jit.c needs from this frontend. */
extern const emu_ir_frontend_t g4mh_ir_frontend;

#ifdef __cplusplus
}
#endif

#endif /* G4MH_IR_H */
