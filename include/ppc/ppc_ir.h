/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_ir.h - the e200z7 frontend's IR lowering.
 *
 * What a host backend needs from this frontend: how to turn guest
 * instructions into IR, and where the guest keeps its registers and pc.
 * Nothing here names a host.
 */
#ifndef PPC_IR_H
#define PPC_IR_H

#include "emu/emu_ir.h"

#include "ppc/ppc_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lower a run of guest instructions starting at `pc` into `b`, and
 * return how many were folded in -- 0 if nothing at `pc` could be
 * lowered, which is ordinary rather than an error.
 */
uint32_t ppc_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

/* Where this guest's state lives, for emu_ir_lower and emu_ir_interp. */
extern const emu_ir_target_t ppc_ir_target;

/* Everything a host's jit.c needs from this frontend. */
extern const emu_ir_frontend_t ppc_ir_frontend;

/*
 * How each instruction a block contains was handled: lowered to IR, or
 * handed to the interpreter's ppc_exec by a call from inside the block.
 * `declined` counts the instructions that *ended* a block because
 * neither would do.
 *
 * Reported because the three are indistinguishable from outside. A
 * helper call is a translation as far as the framework's counters go,
 * so a translator that lowered nothing and called the interpreter for
 * everything would show perfect coverage and be an interpreter with
 * extra steps. Indexed by ppc_sem_t.
 */
typedef struct ppc_ir_stats {
    uint64_t native;
    uint64_t helper;
    uint64_t declined;
    uint32_t helper_by_sem[PPC_S_COUNT];
    uint32_t declined_by_sem[PPC_S_COUNT];
} ppc_ir_stats_t;

const ppc_ir_stats_t *ppc_ir_get_stats(void);

/* The name of a semantic, for that report. */
const char *ppc_sem_name(uint32_t sem);

#ifdef __cplusplus
}
#endif

#endif /* PPC_IR_H */
