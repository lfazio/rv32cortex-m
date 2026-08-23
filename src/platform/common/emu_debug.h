/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_debug.h - serving gdb, and the transport a platform serves it over.
 *
 * Three layers, and this header is the middle one:
 *
 *   emu_gdb.c        the RSP protocol and the register map, in emucore.
 *                    Knows nothing about a socket or a UART.
 *   emu_debug.c      *this*: whether there is a target description at
 *                    all, starting the stub, saying where to connect,
 *                    and handing run control to the loop. The same on
 *                    every platform, and it was written out twice.
 *   board_gdb_*      the transport. A TCP socket on a host, the IP stack
 *                    over a serial line on a board.
 *
 * The middle layer is small and was still worth having: both copies did
 * the same four things in the same order, and they had already drifted
 * into saying them differently -- "gdb    frontend has no target
 * description" against "gdb: no target description for frontend rv32",
 * which is two greps for one condition.
 *
 * **A frontend without a description is not an error.** gdb's `g` packet
 * is a fixed per-architecture concatenation it never asks about, so
 * serving the wrong layout gives an `info registers` that is entirely
 * wrong and entirely plausible -- worse than none. A platform that cannot
 * serve gdb is still a platform that runs guests, so this says so and
 * carries on.
 */
#ifndef EMU_PLATFORM_DEBUG_H
#define EMU_PLATFORM_DEBUG_H

#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"

#include "board_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The transport is board_gdb_* in board_api.h. */

/* ------------------------------------------------------------------ */
/* What the runner calls                                               */
/* ------------------------------------------------------------------ */

/*
 * Ask the frontend for its register layout, start the platform's
 * transport, and say where to connect. Called once the cores exist and
 * before the guest runs, so a debugger that connects immediately finds it
 * at its reset vector rather than somewhere arbitrary.
 */
void emu_debug_start(emu_system_t *sys, const emu_cpu_ops_t *ops);

/*
 * Service the stub between slices.
 *
 * Reached through this rather than by the run loop calling board_gdb_poll
 * itself, for the same reason emu_debug_run exists: the loop asks the
 * *debug* layer to do its debug work, and which transport that turns into
 * -- nothing on a board, a listening socket on a host -- is one layer
 * further down and no business of the loop's.
 */
void emu_debug_poll(void);

/*
 * One scheduling round, with a debugger in the picture.
 *
 * With one attached the *stub* drives the guest: it owns stepping and
 * breakpoints, and stepping the cores here as well would execute
 * instructions the debugger believes are still ahead of it. With none, an
 * ordinary round. `all_idle` is set only in the second case -- a run
 * under a debugger does not end because the guest is parked, it ends when
 * the person says so.
 *
 * The run loop calls this instead of carrying gdb_attached and gdb_run as
 * hooks. It had them because the two platforms named their stubs
 * differently; they answer to board_gdb_* now, so the loop needs no
 * per-platform anything.
 */
uint32_t emu_debug_run(emu_system_t *sys, uint32_t budget, uint32_t *retired,
                       bool *all_idle);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_DEBUG_H */
