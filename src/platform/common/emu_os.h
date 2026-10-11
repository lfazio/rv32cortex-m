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
 * against. On an SMP-capable FreeRTOS host the same round runs the cores
 * in parallel; what that needs from the shared devices is in docs/os.md.
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

/* One line: how much of each task's stack the run used. */
void emu_os_report(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_OS_H */
