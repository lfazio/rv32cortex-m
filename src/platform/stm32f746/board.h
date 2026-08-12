/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.h - What changes when the board does.
 *
 * Everything behind this interface is a fact about the silicon and the
 * PCB: which oscillator, which PLL dividers, how many flash wait states,
 * which USART the ST-LINK's virtual COM port is wired to, and whether the
 * core has caches to turn on. Nothing here knows the emulator exists.
 *
 * The split exists because those facts are the entire difference between
 * the two supported boards, and they were previously interleaved with the
 * emulator glue in main.c -- so porting meant reading 800 lines to find
 * the forty that mattered. A Nucleo-144 putting its VCP on USART3 rather
 * than USART2 is exactly the kind of detail that hides there and produces
 * a board that runs and says nothing.
 */
#ifndef RV32_BOARD_H
#define RV32_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bring the part up: caches, clock tree, console. Called first, before
 * anything reads the clock or prints.
 */
void board_init(void);

/* Human-readable part and clock, for the banner. */
const char *board_name(void);
uint32_t    board_clock_hz(void);

/* Blocking console write; the guest's UART and the firmware share it. */
void board_console_putc(uint8_t c);

/* Non-blocking console read, or -1 when nothing is waiting. */
int board_console_getc(void);

/*
 * Move reception into an interrupt that fills a ring, which
 * board_console_getc() then drains. Idempotent, and one way: nothing
 * turns it back off.
 *
 * This is not an optimisation. Polling the receive register is fine for a
 * human at a terminal and cannot work for a protocol -- the USART holds
 * one byte and the caller reaches it once per guest slice -- so anything
 * that has to receive a framed stream must call this first.
 */
void board_console_rx_irq_enable(void);

/*
 * Bytes lost since reception began, whether to a full ring or to the
 * USART's own overrun. Nonzero means the wire outran the run loop, which
 * is a fact about the guest's slice length rather than about the link,
 * and is worth reporting before blaming the other end.
 */
uint32_t board_console_rx_overruns(void);

/*
 * A free-running cycle counter at the core clock. This is the emulator's
 * only time base: guest mtime is derived from it, so a counter that does
 * not run stops every guest timer interrupt.
 */
uint32_t board_cycles(void);

/* ------------------------------------------------------------------ */
/* The guest-image arena in flash                                      */
/* ------------------------------------------------------------------ */

/*
 * Spare flash a guest image can be programmed into at run time, so a new
 * image can be uploaded without reflashing the firmware.
 *
 * Why flash and not RAM: the largest architecture tests need ~345 KiB,
 * of which ~140 is read-only. Only the writable remainder has to be in
 * SRAM, and that is what makes them fit in the 276 KiB the guest gets.
 * Staging the read-only half in RAM instead would put it back over the
 * limit -- the flash backing is load-bearing, not an optimisation.
 *
 * The arena is append-only and erased only when the next image will not
 * fit. A sector erase on this part stalls every flash fetch for seconds
 * and costs one of ten thousand cycles; doing it per upload would mean
 * 274 erases per suite run, which is about thirty runs before the sector
 * wears out. Packing images end to end makes that roughly fifteen times
 * better, and the erase becomes rare enough that its stall stops
 * mattering.
 */
uint32_t board_flash_arena_base(void);
uint32_t board_flash_arena_size(void);

/*
 * Reserve `len` bytes and return where they will live, erasing the arena
 * first if the request does not fit in what is left. Returns 0 when the
 * arena cannot hold `len` at all.
 */
uint32_t board_flash_arena_alloc(uint32_t len);

/*
 * Program into the arena. `addr` must be within a range returned by
 * board_flash_arena_alloc, and writes must be sequential and word
 * aligned in length except for the last.
 *
 * Runs from ITCM, so it keeps executing while the bank is busy.
 */
bool board_flash_write(uint32_t addr, const void *data, uint32_t len);

#endif /* RV32_BOARD_H */
