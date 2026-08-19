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

void emu_console_puthex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    emu_console_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        emu_console_putc((uint8_t)hex[(v >> i) & 0xFu]);
    }
}

void emu_console_putu(uint32_t v)
{
    char tmp[10];
    int n = 0;

    if (v == 0u) {
        emu_console_putc('0');
        return;
    }
    while (v != 0u && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n-- > 0) {
        emu_console_putc((uint8_t)tmp[n]);
    }
}

/* emu_print_fn onto the console, for the frontend's own state dump. */
static void console_out(void *ctx, const char *s)
{
    (void)ctx;
    emu_console_puts(s);
}

void emu_report_state(emu_cpu_t *cpu, const emu_cpu_ops_t *ops)
{
    emu_console_puts("\n-- guest state --");
    ops->dump(cpu, console_out, NULL);
}
