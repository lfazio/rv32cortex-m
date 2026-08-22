/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_console.c - see emu_console.h for why this is not per-platform.
 */

#include "emu_console.h"

#include <stdarg.h>
#include <stdio.h>

void emu_console_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            emu_console_putc('\r');   /* terminals expect CRLF */
        }
        emu_console_putc((uint8_t)*s++);
    }
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

void emu_console_putb(uint8_t c)
{
    if (c == '\n') {
        emu_console_putc('\r');
    }
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
    emu_console_putb(c);
}

