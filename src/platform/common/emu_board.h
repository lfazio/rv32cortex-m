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
bool emu_start_guest(emu_core_t *core, emu_bus_t *bus, struct emu_uart *uart,
                     struct emu_guest_exit *exit_state);

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
/* The guest-image arena in flash -- optional                           */
/* ------------------------------------------------------------------ */

/*
 * Spare storage a guest image can be programmed into at run time, so a
 * new guest arrives over TFTP or through gdb's `load` instead of over
 * SWD.
 *
 * **A board without one returns 0 from board_flash_arena_size(), and
 * that is the whole of how it declines.** It used to be an #if plus
 * three stub callbacks per board; a run-time size is better here for the
 * reason emu_print_jit_stats takes the same shape -- the capability
 * macro that gated the old arrangement was read in a file that did not
 * include what defined it, which #if quietly treats as 0. A constant
 * zero folds the branch away for a board that has no arena, so the cost
 * is nothing and the failure mode is a wrong answer rather than silence.
 *
 * The arena is append-only and erased only when the next image will not
 * fit. A sector erase stalls flash fetch for seconds and costs one of
 * ten thousand cycles, so erasing per upload would be 274 erases per
 * suite run -- about thirty runs before the sector wears out. Packing
 * images end to end is roughly fifteen times better.
 */
uint32_t board_flash_arena_base(void);
uint32_t board_flash_arena_size(void);

/*
 * Where the next image will be programmed, erasing first if the arena
 * has never been erased since reset. Returns 0 on failure.
 *
 * No length, because TFTP does not carry one: a transfer ends when a
 * short block arrives, so the size is known only once the whole image is
 * written. So writes run until they hit the end and *fail*, and the
 * caller erases and retries -- one wasted transfer per erase cycle
 * against fifteen times the flash wear.
 */
uint32_t board_flash_arena_begin(void);

/* Accept `len` bytes at the address begin() returned, so the next image
 * starts after them. Not called when a transfer fails, which is what
 * makes a failed upload leave no trace. */
void board_flash_arena_commit(uint32_t len);

/* Erase unconditionally and restart from the base. */
bool board_flash_arena_reset(void);

/*
 * Program into the arena. Writes must be sequential and word aligned in
 * length except for the last -- which the TFTP path satisfies for free
 * with its 512-byte blocks and gdb does not, so the runner carries the
 * 1-3 byte remainder between calls.
 */
bool board_flash_write(uint32_t addr, const void *data, uint32_t len);

/* The HAL's error code from the last board_flash_write: a refused
 * program and a full arena are different problems with different
 * recoveries. */
uint32_t board_flash_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_BOARD_H */
