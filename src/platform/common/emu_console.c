/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_console.c - see emu_console.h for why this is not per-platform.
 */

#include "emu_console.h"

#include "board.h"

#if EMU_NET
#include "emu_net.h"
#endif

#include <stdarg.h>
#include <stdio.h>

/*
 * One byte out, and one in. **The only place that knows where the
 * console is.**
 *
 * Two things decide that and both belong here rather than in a platform:
 * the board supplies the wire through board_console_putc, and EMU_NET
 * decides whether the wire is still a console at all. After
 * emu_net_init() succeeds the UART carries IP and everything printed
 * goes to a ring a telnet client drains -- which is the same rule on
 * every platform, and was written out twice because it lived in each
 * runner.
 *
 * The branch is a load and a test per character, which is nothing: the
 * console is written by human-readable output and by the guest's virtual
 * UART, neither of which is on any measured hot path.
 */
void emu_console_putc(uint8_t c)
{
#if EMU_NET
    if (emu_net_active()) {
        emu_net_console_putc(c);
        return;
    }
#endif
    board_console_putc(c);
}

int emu_console_getchar(void)
{
#if EMU_NET
    if (emu_net_active()) {
        return emu_net_console_getc();
    }
#endif
    return board_console_getc();
}

void emu_console_puts(const char *s)
{
    while (*s != '\0') {
        emu_console_putchar((uint8_t)*s++);
    }
}

const char *emu_u64_str(char *buf, unsigned long long v)
{
    char tmp[21];
    unsigned i = 0u;
    unsigned j = 0u;

    if (v == 0ull) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
    while (v != 0ull && i < sizeof(tmp)) {
        tmp[i++] = (char)('0' + (unsigned)(v % 10ull));
        v /= 10ull;
    }
    while (i > 0u) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}

void emu_console_printf(const char *fmt, ...)
{
    /*
     * 192 bytes: the longest thing printed through here is a stats line,
     * and a fixed buffer makes the worst case visible rather than
     * depending on what a caller happens to pass. It is on the stack
     * because this is called from the run loop and from fatal paths, and
     * a static would make two of those reentrant against each other.
     */
    char buf[192];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emu_console_puts(buf);
}

/*
 * A byte of *guest* output.
 *
 * The LF-to-CRLF expansion is a property of a terminal on a serial line,
 * not of a console, so a platform asks for it: EMU_CONSOLE_CRLF. Doing it
 * unconditionally would put a \r into every line of every guest's output
 * on a host's stdout, which two test suites compare and one figure script
 * parses.
 */
void emu_console_putchar(uint8_t c)
{
#if EMU_CONSOLE_CRLF
    if (c == '\n') {
        emu_console_putc('\r');
    }
#endif
    emu_console_putc(c);
}

/*
 * The emulated NS16550's transmit side. Same byte, wrapped for the
 * callback signature emu_uart_init wants -- the guest's console and the
 * firmware's are one wire, which is what makes a guest's output and a
 * panic message interleave in the order they happened.
 */
void emu_console_uart_tx(void *ctx, uint8_t c)
{
    (void)ctx;
    emu_console_putchar(c);
}
