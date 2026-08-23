/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_run.c - the guest execution loop, shared by the firmware and the
 * host runner.
 *
 * Run the guest in slices, advance its clock, service whatever the
 * platform has running beside it, and say why it stopped.
 *
 * Why slices at all: the emulator has other work -- an IP stack that
 * advances only when called, a debugger that may want to interrupt -- and
 * a guest running flat out would never yield. The slice is the whole
 * schedule.
 *
 * **Compare the running total against the cap, never a budget counted
 * down to zero.** A block backend may retire *more* than the slice it was
 * given, because it can only stop between blocks, so a remaining-budget
 * subtraction goes below zero and wraps -- and the guest then runs on
 * correctly while the cap silently stops existing. That was reachable
 * only by the two riscv-tests that depend on the cap to terminate, and
 * was invisible for the life of the project because the interpreter lands
 * on it exactly. It is written once here now, rather than twice.
 */

#include "emu_run.h"
#include "emu_debug.h"
#include "emu_board.h"

emu_run_outcome_t emu_run_system(emu_system_t *sys, const emu_run_env_t *env,
                                 uint64_t *retired_total)
{
    for (;;) {
        uint32_t did = 0u;
        uint32_t slice = env->slice;
        bool     all_idle = false;

        /*
         * Clamp the slice to what is left of the cap, so a run stops near
         * it rather than a whole slice past. It is a bound on overshoot,
         * not the termination test -- that is the `>=` below, and
         * confusing the two is the defect this file's header records.
         */
        if (env->max_insn != 0u) {
            const uint64_t left = (uint64_t)env->max_insn - *retired_total;

            if ((uint64_t)slice > left) {
                slice = (uint32_t)left;
            }
        }

        (void)emu_debug_run(sys, slice, &did, &all_idle);
        *retired_total += did;

        /*
         * The platform's own work, then the debugger's. Two calls
         * because they are two questions: an IP stack that must be
         * pumped, and a stub that may have a client waiting.
         */
        emu_board_poll();
        emu_debug_poll();

        /*
         * An image can arrive at any point, and the caller has to rebuild
         * the address space around it -- which is why this is a return
         * rather than something handled here. A reload that only the run
         * loop could see would never happen: a harness uploads *between*
         * runs, when the loop has already exited.
         */
        if (env->take_upload != NULL && env->take_upload()) {
            return EMU_RUN_OUTCOME_RELOAD;
        }

        if (env->max_insn != 0u &&
            *retired_total >= (uint64_t)env->max_insn) {
            return EMU_RUN_OUTCOME_CAPPED;
        }


        if (env->advance_time != NULL) {
            env->advance_time(*retired_total, did);
        }

        /*
         * Idle is the end of the run, and it covers both ways a guest can
         * be finished: halted, or parked in WFI with nothing left that
         * could deliver an interrupt. emu_system_step decides it per core
         * and reports the conjunction, so a multicore guest ends when the
         * last live core does rather than when the first stops.
         */
        if (all_idle) {
            return EMU_RUN_OUTCOME_HALTED;
        }
    }
}
