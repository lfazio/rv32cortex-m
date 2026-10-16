/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_os.h - the emulator under FreeRTOS: a task per emulated core.
 *
 * Every platform runs the runner as FreeRTOS tasks -- the POSIX port on a
 * development machine, the Cortex-M ports on the boards. main() starts
 * the scheduler with the runner as its first task, and that task is
 * also emulated core 0. Each further core of a multicore guest gets a
 * task of its own, created the first time a round needs it.
 *
 * **The cores meet at every quantum boundary.** A round releases cores
 * 1..N-1, runs core 0 on the calling task, and waits for all of them;
 * guest time, the platform's polling, uploads, the instruction cap and
 * the idle rule are all decided between rounds, exactly as the
 * single-threaded round-robin decided them. The scheduler is cooperative
 * (FreeRTOSConfig.h), so on a single-core host the round runs core 0,
 * 1, 2 in that order every time -- the interleaving the round-robin
 * produced, and what the multicore tests' quantum invariance is checked
 * against. On a multicore host (EMU_OS_SMP) the same round runs the
 * cores in parallel, one FreeRTOS core each, and --serial gives the
 * ordered round back; what parallelism needs from the rest of the
 * emulator is in docs/os.md.
 */
#ifndef EMU_PLATFORM_OS_H
#define EMU_PLATFORM_OS_H

#include "emu/emu_cpu.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*emu_os_body_fn)(int argc, char **argv);

/*
 * Start the scheduler with `body` as its first task. Does not return on a
 * board. On the host the process exits with body's status once it
 * returns -- the POSIX port's scheduler owns the main thread until then.
 */
int emu_os_start(emu_os_body_fn body, int argc, char **argv);

/*
 * One round of the system: emu_system_step's contract, with every core
 * past the first on its own task. A system of one core runs inline, with
 * no task switch at all.
 */
uint32_t emu_os_system_step(emu_system_t *sys, uint32_t quantum,
                            bool *all_idle);

/*
 * Run every round on the calling task, core 0 then 1 then 2: the
 * single-threaded round-robin, reproducible to the instruction. Off by
 * default; it only changes anything on an SMP host, where a round
 * otherwise runs its cores in parallel.
 */
void emu_os_set_serial(bool serial);

/*
 * Will a round of `ncores` cores run them at once? Only under an SMP
 * kernel, with more than one core and fewer than the build has tasks for,
 * and not --serial -- emu_os_system_step's own conditions.
 */
bool emu_os_parallel(unsigned ncores);

/*
 * The IP stack's own task. Every call into lwIP is made there: `fn(arg)`
 * runs on it and the caller waits until it returns.
 *
 * **Waits, so the stack and the guest still never run at once.** The
 * stack reads and writes guest state -- gdb's memory and registers, an
 * uploaded image into the flash the guest executes from -- and each of
 * those is safe only between slices, which is where the run loop hands
 * over. What moving it buys is a stack of its own and a task a future
 * device (USB) can wake, not concurrency.
 *
 * The task is created on the first call. A call made from the task
 * itself runs inline. Only in a build with EMU_NET.
 */
void emu_os_net_call(void (*fn)(void *), void *arg);

/* The run starts: what emu_os_report counts is from here. */
void emu_os_run_begin(void);

/* One line: how much of each task's stack the run used. */
void emu_os_report(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_OS_H */
