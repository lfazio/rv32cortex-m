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
 * Where the next image will be programmed, erasing first if the arena
 * has never been erased since reset. Returns 0 on failure.
 *
 * There is no length here because TFTP does not carry one: a transfer
 * ends when a short block arrives, so the size is only known once the
 * whole image has been written. Reserving a worst case up front would
 * cost most of the packing this arena exists for -- 256 KiB reserved out
 * of 768 is three images per erase, against the fifteen or so that
 * packing tightly gives.
 *
 * So writes run until they hit the end and *fail*, and the caller erases
 * and retries. One transfer is wasted per erase cycle, which is a far
 * better trade than fifteen times the flash wear.
 */
uint32_t board_flash_arena_begin(void);

/* Accept `len` bytes at the address begin() returned, so the next image
 * starts after them. Not called when a transfer fails, which is what
 * makes a failed upload leave no trace. */
void board_flash_arena_commit(uint32_t len);

/* Erase unconditionally and restart from the base. */
bool board_flash_arena_reset(void);

/*
 * Program into the arena. `addr` must be within a range returned by
 * board_flash_arena_alloc, and writes must be sequential and word
 * aligned in length except for the last.
 *
 * Runs from ITCM, so it keeps executing while the bank is busy.
 */
bool board_flash_write(uint32_t addr, const void *data, uint32_t len);

/* ------------------------------------------------------------------ */
/* Link activity                                                       */
/* ------------------------------------------------------------------ */

/*
 * The board's user LEDs, as link-activity indicators.
 *
 * Worth having because the SLIP link has no other outward sign of life.
 * Once the UART carries IP the board is silent by design, so "nothing is
 * happening" and "the wire is dead" look identical from the desk -- and
 * this session lost a day to exactly that: a link that was mis-framed at
 * the host end, with a board that was transmitting perfectly and no way
 * to see it without a debug probe.
 *
 * Green for received frames, blue for transmitted. Toggled rather than
 * pulsed, because a pulse needs a timer to end it and a toggle needs
 * nothing: at these rates it reads as a flicker under traffic and a
 * steady state when idle, which is the whole question being asked.
 */
typedef enum {
    BOARD_LED_RX,           /* LD1, green,  PB0  */
    BOARD_LED_TX            /* LD2, blue,   PB7  */
} board_led_t;

void board_led_toggle(board_led_t led);

#endif /* RV32_BOARD_H */
