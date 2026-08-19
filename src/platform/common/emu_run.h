/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_run.h - the shared guest execution loop.
 */
#ifndef EMU_PLATFORM_RUN_H
#define EMU_PLATFORM_RUN_H

#include "emu/emu_cpu.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMU_RUN_OUTCOME_HALTED = 0,  /* the guest stopped, or parked for ever */
    EMU_RUN_OUTCOME_CAPPED,      /* the instruction cap was reached       */
    EMU_RUN_OUTCOME_RELOAD,      /* a new image arrived; rebuild and rerun*/
} emu_run_outcome_t;

typedef struct emu_run_env {
    uint32_t slice;      /* guest instructions between returns to us */
    uint32_t max_insn;   /* 0 = no cap */

    /*
     * True when an uploaded image is waiting. NULL on a board that
     * cannot be reloaded -- which is not the same as a function that
     * always returns false, because the difference is visible here as
     * one branch instead of a call.
     */
    bool (*take_upload)(void);
} emu_run_env_t;

emu_run_outcome_t emu_run_guest(emu_core_t *core, const emu_cpu_ops_t *ops,
                                const emu_run_env_t *env,
                                uint64_t *retired_total);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_RUN_H */
