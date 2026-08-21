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
     * The platform's own work, once per slice. An IP stack advances only
     * when called, so this is its entire schedule -- finer than any
     * timeout lwIP keeps. NULL when there is nothing beside the guest.
     */
    void (*poll)(void);

    /*
     * Run control while a debugger is attached. With one, the *stub*
     * drives the guest: it owns stepping and breakpoints, and running the
     * cores here as well would execute instructions the debugger believes
     * are still ahead of it. Both NULL in a build with no stub.
     */
    bool     (*gdb_attached)(void);
    uint32_t (*gdb_run)(uint32_t budget, uint32_t *retired);

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
     */
    void (*advance_time)(uint64_t retired_total, uint32_t did);

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
