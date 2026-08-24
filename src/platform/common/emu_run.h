/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_run.h - the shared guest execution loop.
 *
 * One loop for the firmware and the host runner. What differs between
 * them is four things, and each is a hook rather than an #if: what else
 * has to be serviced between slices, who owns run control when a debugger
 * is attached, where guest time comes from, and whether a new image can
 * arrive mid-run.
 *
 * It drives an emu_system_t rather than a core, because the host already
 * schedules several and a firmware is a system of one -- and the idle
 * rule that ends a run is the same either way: a core that is halted,
 * held at reset, or parked in WFI with nothing that could wake it is not
 * live, and when none is live the run is over.
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
     * What used to be here: `poll`, `gdb_attached` and `gdb_run`.
     *
     * They existed because the two platforms named the same three things
     * differently -- emu_net_gdb_run against host_gdb_run -- so the loop
     * could not call either by name. They answer to board_poll() and
     * board_gdb_* now, and a hook whose only purpose was to paper over a
     * naming difference is a hook that should not exist.
     */

    /*
     * Advance the guest's clock, given what the round retired.
     *
     * Two genuinely different answers, which is why it is a hook. The
     * firmware reads a real cycle counter, because its guest drives real
     * peripherals and its timer interrupts have to bear some relation to
     * the wall clock. The host derives time from instructions retired, so
     * that two runs of the same guest produce the same trace -- there is
     * no wall clock worth tracking there, and determinism is what the
     * architecture suite compares against.
     *
     * **`sys` is passed, not fetched.** Both implementations need the
     * cores, and both used to reach back into the runner's main() for
     * them through emu_main_system() -- a board file calling upward, for
     * a value the run loop is holding as it makes the call.
     */
    void (*advance_time)(emu_system_t *sys, uint64_t retired_total,
                         uint32_t did);

    /*
     * True when an uploaded image is waiting. NULL on a platform that
     * cannot be reloaded -- which is not the same as a function that
     * always returns false, because the difference is visible here as one
     * branch instead of a call.
     */
    bool (*take_upload)(void);
} emu_run_env_t;

/*
 * Run until the guest stops, the cap is reached, or an image arrives.
 * `retired_total` is added to, not assigned, so a caller that restarts
 * around a reload keeps its running total.
 */
emu_run_outcome_t emu_run_system(emu_system_t *sys, const emu_run_env_t *env,
                                 uint64_t *retired_total);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_RUN_H */
