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

/*
 * Park until an interrupt, and stop for good.
 *
 * Here rather than __WFI() in the runner so that emu_main.c needs no
 * CMSIS header, and because the right way to wait is a property of the
 * part: on a core with no sleep instruction, or one where sleeping
 * gates a clock something else depends on, this is where that is
 * decided.
 */
void board_idle(void);
void board_fatal_halt(void);


/*
 * Link activity, for the network transport.
 *
 * **This board has one user LED where the F746ZG has three**, so the two
 * events share it: LD2 on PA5 toggles for a received frame and for a
 * transmitted one alike. That loses the direction, and keeps the
 * question the LED is actually asked -- is anything moving on the wire
 * -- which is what makes a silent link distinguishable from a dead one
 * without a debug probe.
 *
 * The enumerators exist on both boards so the stack needs no #if. A
 * platform with no LED at all would implement the toggle as nothing,
 * which is a worse diagnostic and still not a build problem.
 */
typedef enum {
    BOARD_LED_RX,
    BOARD_LED_TX
} board_led_t;

void board_led_toggle(board_led_t led);

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

#endif /* RV32_BOARD_H */
