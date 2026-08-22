/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.h - the host's "board", so the IP stack can run here too.
 *
 * src/net/ reaches its platform through this header and nothing else --
 * a console byte in and out, a cycle counter, two activity LEDs. That is
 * the whole dependency, and it is small enough that a development
 * machine can satisfy it, which is the entire reason this file exists.
 *
 * **Why bother.** Every network defect this project has had needed a
 * board, a flash cycle and a UART to find: the TFTP session that wedged
 * until reset, the flash write that failed on a pbuf chain boundary, the
 * SLIP line that came up at five data bits. None of them could be
 * stepped through, none could be run under a sanitiser, and each cost a
 * reflash per hypothesis. The same lwIP, the same PPPoS, the same telnet
 * and TFTP servers built for the host are debuggable with the tools that
 * already exist.
 *
 * The wire is a pseudo-terminal. The board's is the ST-LINK's virtual
 * COM port; here it is a pty whose slave name the runner prints, and
 * pppd attaches to that exactly as it attaches to /dev/ttyACM0. Nothing
 * in src/net/ can tell the difference, which is the property being
 * tested.
 *
 * **Where the host deliberately differs**, because pretending otherwise
 * would make the runner unusable rather than faithful: the board has one
 * wire and must therefore give its console away to get a network, and it
 * pays for that by having no channel to report its own failure on. A
 * host has a terminal *and* a pty, so the runner's own diagnostics stay
 * on stderr. Guest output still goes to the telnet ring, so that path is
 * exercised; only the firmware's inability to speak is not reproduced.
 */
#ifndef EMU_HOST_BOARD_H
#define EMU_HOST_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Open the pty and put it in raw mode. `slave_out`/`n` receive the name
 * to hand to pppd. False if no pty could be had, which is not fatal to
 * the runner -- it simply has no network.
 *
 * `dev` names an existing device to use instead, for a caller that has
 * already arranged one end of a link.
 */
bool board_console_open(const char *dev, char *slave_out, unsigned n);

/* The device the stack is talking over, for diagnostics. Empty when the
 * console has not been opened. */
const char *board_name(void);

/*
 * One byte out, one byte in or -1 if nothing has arrived.
 *
 * Non-blocking on purpose: sio_tryread's contract is "whatever is here
 * now", and the run loop reaches it between guest slices. A blocking
 * read would stop the guest whenever the link went quiet, which is most
 * of the time.
 */
void board_console_putc(uint8_t c);
int  board_console_getc(void);

/*
 * A no-op here. The board needs it because its USART holds exactly one
 * byte and the run loop cannot get back in time at 921600 baud; a pty
 * has a kernel buffer, so there is nothing to arm.
 */
void board_console_rx_irq_enable(void);

/* Bytes the link dropped. Always zero here, and kept so the stack's own
 * reporting compiles unchanged -- a pty does not overrun. */
uint32_t board_console_rx_overruns(void);

/*
 * Microsecond-resolution monotonic time, scaled to look like a cycle
 * counter at board_clock_hz(). sys_now() divides one by the other, so
 * only their ratio matters -- but it has to *advance in real time*,
 * because every lwIP timeout is measured against it. Deriving it from
 * instructions retired, as the guest's own clock is, would make the
 * stack's notion of a second depend on how fast the emulator happened to
 * be running.
 */
uint32_t board_cycles(void);
uint32_t board_clock_hz(void);

typedef enum {
    BOARD_LED_RX,
    BOARD_LED_TX
} board_led_t;

/* Counted rather than lit: on a board these say at a glance whether
 * frames are moving, and the same question is worth answering here. */
void     board_led_toggle(board_led_t led);
uint32_t board_led_count(board_led_t led);

#endif /* EMU_HOST_BOARD_H */
