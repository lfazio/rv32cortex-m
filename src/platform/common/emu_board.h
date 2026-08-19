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
 * `ro` is where the read-only half ends. It is a *placement* figure, not
 * a correctness one: the guest initialises its own .data now, so this
 * only tells the board where to stop serving flash and start serving
 * RAM. On a part with enough RAM to map the whole image writable, it may
 * be zero.
 */
extern const uint8_t *emu_board_img;
extern uint32_t       emu_board_img_size;
extern uint32_t       emu_board_img_ro;

/* Where the guest's RAM is and how much of it there is. */
extern uint8_t *const emu_board_ram;
extern const uint32_t emu_board_ram_size;

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
void emu_board_irqs_init(void);
void emu_board_irq_unmask(void *ctx, uint32_t source);

/*
 * Guest time, in the units the frontend's timer expects. Per-board
 * because it comes from a cycle counter whose rate is the part's.
 */
uint64_t emu_board_time_now(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_BOARD_H */
