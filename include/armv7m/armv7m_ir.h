/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_ir.h - The ARMv7-M frontend's IR lowering.
 *
 * What a host backend needs from this frontend: how to turn guest
 * instructions into IR, and where the guest keeps its registers and pc.
 * Nothing here names a host.
 */
#ifndef ARMV7M_IR_H
#define ARMV7M_IR_H

#include "emu/emu_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lower a run of guest instructions starting at `pc` into `b`, and
 * return how many were folded in -- 0 if nothing at `pc` could be
 * lowered, which is ordinary rather than an error.
 */
uint32_t armv7m_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

/* Where this guest's state lives, for emu_ir_lower and emu_ir_interp. */
extern const emu_ir_target_t armv7m_ir_target;

/* Everything a host's jit.c needs from this frontend. */
extern const emu_ir_frontend_t armv7m_ir_frontend;

/*
 * How each instruction a block contains was handled, since translation
 * began: lowered to IR, or handed to the interpreter by a helper call.
 * `declined` counts the instructions that *ended* a block because
 * neither was possible.
 *
 * Reported because the three are indistinguishable from outside. A
 * helper call is a translation as far as the framework's own counters
 * go, so a translator that lowered nothing and called the interpreter
 * for everything would show perfect coverage and be an interpreter with
 * extra steps.
 */
typedef struct armv7m_ir_stats {
    uint64_t native;
    uint64_t helper;
    uint64_t declined;
    uint32_t helper_by_op[512]; /* indexed by armv7m_op_t */
    uint32_t declined_by_op[512];
} armv7m_ir_stats_t;

const armv7m_ir_stats_t *armv7m_ir_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_IR_H */
