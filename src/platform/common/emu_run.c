/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_run.c - the guest execution loop, shared.
 *
 * Run the guest in slices, advance its clock, service whatever the
 * platform has running beside it, and say why it stopped. Both firmware
 * runners had this and the F446's was a subset of the F746's -- the
 * difference being a gdb stub that runs the guest itself and an image
 * that may arrive mid-run.
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
 * on it exactly.
 */

#include "emu_board.h"
#include "emu_console.h"
#include "emu_run.h"

#if EMU_NET
#  include "emu_net.h"
#endif

emu_run_outcome_t emu_run_guest(emu_core_t *core, const emu_cpu_ops_t *ops,
                                const emu_run_env_t *env,
                                uint64_t *retired_total)
{
    emu_cpu_status_t st;

    for (;;) {
        uint32_t retired = 0;
        emu_run_reason_t why;

#if EMU_NET
        /*
         * With a debugger attached the *stub* drives the guest: it owns
         * stepping and breakpoints, and running the core here as well
         * would execute instructions the debugger believes are still
         * ahead of it.
         */
        if (emu_net_gdb_attached()) {
            why = (emu_run_reason_t)emu_net_gdb_run(env->slice, &retired);
        } else
#endif
        {
            why = emu_core_run(core, env->slice, &retired);
        }
        *retired_total += retired;

#if EMU_NET
        /*
         * The stack advances only when called, so this is its entire
         * schedule -- once per slice, finer than any timeout lwIP keeps.
         */
        emu_net_poll();

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
#endif

        if (env->max_insn != 0u &&
            *retired_total >= (uint64_t)env->max_insn) {
            return EMU_RUN_OUTCOME_CAPPED;
        }

        ops->set_time(core->cpu, emu_board_time_now());

        if (why == EMU_RUN_HALTED) {
            return EMU_RUN_OUTCOME_HALTED;
        }
        if (why == EMU_RUN_WFI) {
            /*
             * A guest in WFI is waiting for an interrupt. If anything can
             * still deliver one, keep going and let it arrive; if nothing
             * can, the guest is parked for ever and saying so is more
             * useful than spinning.
             */
            emu_core_status(core, &st);
            if (st.wakeable) {
                continue;
            }
            emu_console_puts("\nguest parked with no interrupts enabled\n");
            return EMU_RUN_OUTCOME_HALTED;
        }
    }
}
