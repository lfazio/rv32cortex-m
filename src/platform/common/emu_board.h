/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_board.h - what a platform owes the shared runner.
 *
 * The runner in emu_main.c is the same on every board: bring the part up,
 * build a guest address space, open a core, hand it the console and the
 * cache ops, run it in slices, report. None of that is about a part. What
 * *is* about a part is small and is listed here.
 *
 * This exists because the two firmware runners were 724 and 1546 lines of
 * mostly the same sequence, and a third board would have made three
 * copies of it. That is the shape this project has been bitten by
 * repeatedly -- g4mh_ir.c's duplicate interrupt check has needed the same
 * fix three times -- and a thousand-line duplicate is the same trap with
 * more room in it.
 *
 * The division is *not* "what varies between the F446 and the F746",
 * which would bake two boards into an interface meant for three. It is
 * "what only a board can answer".
 */
#ifndef EMU_PLATFORM_BOARD_H
#define EMU_PLATFORM_BOARD_H

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_types.h"

#include "board_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Report a real interrupt line to the guest, from the board's ISR.
 *
 * The other direction of the pair above, and supplied by the runner
 * because the core is the runner's. A board's handler masks the line at
 * the NVIC and calls this; the guest's driver runs later and clears the
 * pending bit, which is what reaches board_irq_unmask.
 */
void emu_raise_irq(uint32_t source, bool level);

/*
 * Build the guest's address space: the four shared regions, then this
 * board's own from board_regions(). In emu_address_space.c.
 *
 * The bus is re-initialised, so the frontend's devices have to be added
 * again afterwards by the caller.
 */
struct emu_uart;
bool emu_build_address_space(emu_bus_t *bus, struct emu_uart *uart);

/*
 * Bring a guest up: address space, the frontend's devices, cleared RAM
 * and exit state, reset and boot. In emu_address_space.c. An upload
 * repeats all of it, which is why it is one function.
 */
struct emu_guest_exit;
struct emu_session_cfg;
bool emu_start_guest(emu_system_t *sys, const struct emu_session_cfg *cfg,
                     struct emu_uart *uart, struct emu_guest_exit *exit_state);

/*
 * Rebuild the address space around a new image and restart. In emu_main.c,
 * because the buses and the session configuration are the runner's.
 *
 * Called by emu_image.c once an upload is complete -- common to common,
 * which is the direction that makes it fine. It used to be called by both
 * boards, which is what made emu_board.h a header a platform reached
 * *upward* through; nothing does that any more.
 */
bool emu_main_reload(void);

/* ------------------------------------------------------------------ */
/* The two ends of a run                                               */
/* ------------------------------------------------------------------ */

/*
 * Everything before a guest can be brought up: bring the part up, obtain
 * an image, say hello -- and state how this platform wants the run done.
 *
 * This is *acquisition*, the first of the two halves emu_session.h says a
 * platform cannot share. A board brings up its clocks and peripherals and
 * has its image linked in; a runner parses argv and reads the file it
 * names. `argc`/`argv` are what a hosted platform gets and a bare-metal
 * one ignores -- passing them costs a board two unused parameters and
 * saves the runner from having a different entry point.
 *
 * **The two structs, rather than a hook each.** What a platform decides
 * about a run -- which backend, where the image goes, what to poll
 * between slices, who owns run control with a debugger attached -- is
 * already described by emu_session_cfg_t and emu_run_env_t, and adding a
 * `emu_board_want_jit`, a `emu_board_load_addr` and ten more beside them
 * would be an interface designed by listing differences. The runner fills
 * in what it owns (the buses, the UART, the syscall handler) before
 * calling this; the platform fills in the rest.
 *
 * On return, board_img/_size and board_ram/_size must be set.
 * False means stop, with *status as the process's exit code where there
 * is a process to exit.
 */
struct emu_session_cfg;
struct emu_run_env;

/* The gdb stub is emu_debug.h: a platform supplies board_gdb_*, and
 * emu_debug_start does the rest. */

/*
 * **Two namespaces, and they are layers rather than a mixture.**
 *
 * `emu_board_*` is what the runner calls: the contract in this header,
 * the same on every platform, and the only thing emu_main.c knows about.
 * `board_*` is a platform's own API -- its UART, its cycle counter, its
 * flash -- and lives in that platform's board.h.
 *
 * The rule is one direction: an emu_board_* function is *implemented in
 * terms of* board_* calls, normalising whatever the part does into what
 * the contract promises. Nothing here declares a board_* function, and
 * this header did declare seven of them -- the flash arena, which moved
 * to the STM32s' board.h when the upload path stopped being shared.
 * Mixing the two makes "what every platform must provide" and "what this
 * part happens to have" the same list, and a third platform then has to
 * read both to find out which half it owes.
 */

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_BOARD_H */
