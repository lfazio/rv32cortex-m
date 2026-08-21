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


/* The guest-image arena is declared in emu_board.h -- it is part of
 * what a board owes the shared runner, not of this board's own API. */

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
