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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

/*
 * The core this firmware runs on, for the banner: "Cortex-M4",
 * "Cortex-M7", "Cortex-M55". A string rather than a macro because the
 * runner prints it and nothing branches on it -- the moment something
 * does, that belongs in one of the hooks below instead.
 */
extern const char *const emu_board_core_name;

/* ------------------------------------------------------------------ */
/* The guest image                                                     */
/* ------------------------------------------------------------------ */

/*
 * The image in force, as variables rather than constants.
 *
 * A board that can take an upload moves these when one arrives; a board
 * that cannot points them at the linked-in image once and never touches
 * them again. The runner reads them and does not care which -- which is
 * what lets the address space be rebuilt identically in both cases.
 *
 * There is no read-only *boundary* any more. The guest links .text and
 * .rodata into flash and .data into RAM, so the platform serves one
 * region as each and never has to know where one ends -- which is what
 * removed the two-piece upload.
 */
extern const uint8_t *emu_board_img;
extern uint32_t       emu_board_img_size;

/*
 * Where the guest's RAM is and how much of it there is.
 *
 * Variables, not constants, and not only for symmetry with the image
 * extents: on a board that carves guest RAM out of whatever the link
 * left over, the size is a *difference of two linker symbols*, which C
 * will not accept in a static initialiser however constant it is at run
 * time. The board assigns both before building the address space.
 */
extern uint8_t *emu_board_ram;
extern uint32_t emu_board_ram_size;

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

/*
 * Add this board's own regions to the bus, after the runner has added
 * the guest image and RAM and before the frontend adds its devices.
 *
 * This is where the passthrough windows go -- the identity-mapped
 * peripheral space that lets a guest driver reach real hardware, which is
 * the entire point of this emulator and is necessarily per-part: the
 * windows differ, and so does which of them a guest may write.
 */
bool emu_board_add_regions(emu_bus_t *bus);

/*
 * Route a real interrupt line to the guest.
 *
 * `unmask` is handed to the frontend, which calls it when the guest
 * enables a source; `init` enables at the NVIC whatever lines this board
 * bridges. Both are per-board because the set of bridged lines is, and
 * because IRQn_Type is a device enumeration.
 */
/*
 * Guest cache maintenance onto this platform's, or NULL where there is
 * none to do. Handed to the core so a guest's cache-block operations
 * reach the lines that actually back the guest block -- which on a host
 * is nothing, because the guest's memory is a malloc'd buffer the host's
 * own cache is already coherent with.
 */
void emu_board_irqs_init(void);
void emu_board_irq_unmask(void *ctx, uint32_t source);

/*
 * Report a real interrupt line to the guest, from the board's ISR.
 *
 * The other direction of the pair above, and supplied by the runner
 * because the core is the runner's. A board's handler masks the line at
 * the NVIC and calls this; the guest's driver runs later and clears the
 * pending bit, which is what reaches emu_board_irq_unmask.
 */
void emu_raise_irq(uint32_t source, bool level);

/*
 * Build the guest's address space: the four shared regions, then this
 * board's own through emu_board_add_regions. In emu_address_space.c.
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
                     struct emu_uart *uart,
                     struct emu_guest_exit *exit_state);

/*
 * Host cycles, for the performance figure -- *not* guest time.
 *
 * These are two different clocks and conflating them is a number that
 * looks measured and is not: emu_board_time_now runs at the rate the
 * guest's timer expects (1 MHz here) and this one at whatever the part
 * executes at, so reporting the first as the second gave "ratio 2.01
 * host cycles per guest instruction" for a board that really spends 429.
 *
 * A platform with no cycle counter worth quoting returns 0, which
 * suppresses the ratio rather than printing a meaningless one.
 */
uint32_t emu_board_host_cycles(void);

/*
 * Rebuild the address space around a new image and restart. What a
 * platform's upload path calls once it has the bytes; in emu_main.c,
 * because the buses and the session configuration are the runner's.
 */
bool emu_main_reload(void);

/* The system, for a platform that must reach it -- the gdb stub and the
 * interrupt bridge do. */
emu_system_t *emu_main_system(void);

/*
 * Guest time, in the units the frontend's timer expects. Per-board
 * because it comes from a cycle counter whose rate is the part's.
 */
uint64_t emu_board_time_now(void);

/*
 * The board's own start-up, after board_init() and before anything uses
 * the guest's memory. Where emu_board_ram and emu_board_ram_size are set,
 * because on both existing boards they are a difference of two linker
 * symbols and only the board's own file can name them.
 */
void emu_board_init(void);

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
 * On return, emu_board_img/_size and emu_board_ram/_size must be set.
 * False means stop, with *status as the process's exit code where there
 * is a process to exit.
 */
struct emu_session_cfg;
struct emu_run_env;
bool emu_board_startup(int argc, char **argv, int *status,
                       struct emu_session_cfg *cfg, struct emu_run_env *env);

/*
 * Start a debugger, if this platform serves one. Called once the cores
 * exist and before the guest runs, so a debugger that connects
 * immediately finds it at its reset vector.
 */
void emu_board_debug_start(emu_system_t *sys, const emu_cpu_ops_t *ops);


/*
 * The runner cannot continue, and has already said why.
 *
 * A host returns and lets the shell see a status. A board has nowhere to
 * return *to* -- and, worse, by this point it may have given its console
 * to the network, so the message explaining the failure is sitting in a
 * ring that only a telnet client can drain. Halting with interrupts
 * masked writes that reason into memory nobody can reach: the board
 * answers no ping, no telnet and no TFTP, and presents as a dead link
 * rather than as a firmware that knows exactly what went wrong. So it
 * keeps servicing the stack instead, for ever.
 *
 * Never returns on a board. On a host it returns and the runner exits.
 */
void emu_board_fatal(int *status);

/*
 * The run is over. *Termination*, the second half a platform cannot
 * share: a runner returns an exit status a suite reads, and a board has
 * nowhere to go and parks serving its link.
 *
 * True means run again -- an image arrived while parked, which is the
 * normal way a board is used by a harness, because a harness uploads
 * *between* runs when the run loop has already exited. False means stop,
 * with *status as the exit code.
 */
bool emu_board_after_run(const struct emu_guest_exit *exit, bool capped,
                         int *status);

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
