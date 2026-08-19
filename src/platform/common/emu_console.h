/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_console.h - the firmware's diagnostic output, shared by platforms.
 *
 * Everything above a single byte-out function is identical on every
 * board, and was duplicated: `console_puts`, `console_putu`,
 * `console_puthex`, `console_out` and `report_state` were byte-for-byte
 * the same in the F446 and F746 runners, and a third platform would have
 * made three copies of a thing with no board in it.
 *
 * The split is where the board actually enters: `emu_console_putc` is
 * supplied by the platform, because only it knows whether the byte goes
 * to a UART or -- once the network stack has taken the wire -- to a ring
 * a telnet client drains.
 *
 * **Nothing here is RISC-V.** These used to be `rv_console_putc` and
 * friends, which was already wrong when the G4MH frontend arrived and
 * became wronger with PowerPC: a console has no instruction set. The
 * names follow the same rule as EMU_GUEST_ARCH_*, that a name says what
 * a thing is about.
 */
#ifndef EMU_PLATFORM_CONSOLE_H
#define EMU_PLATFORM_CONSOLE_H

#include "emu/emu_cpu.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One byte out. **Supplied by the platform**, not by this module.
 *
 * Not static and not inline: the network stack calls it by name to write
 * the two lines it prints before taking the wire, and the linker is what
 * catches a platform that forgets to define it.
 */
void emu_console_putc(uint8_t c);

/* A string, with LF expanded to CRLF because terminals expect it. */
void emu_console_puts(const char *s);

/*
 * Formatted, via picolibc's vsnprintf into a fixed buffer.
 *
 * The alternative was what this replaced: a call per field and per
 * literal, with any fractional value printed digit by digit because
 * there is no float. Truncation is silent, which is the right trade for
 * a diagnostic -- a stats line that loses its tail beats one that cannot
 * be printed at all.
 */
void emu_console_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Unsigned decimal and 0x-prefixed hex, for the few places a bare number
 * is clearer than a format string. */
void emu_console_putu(uint32_t v);
void emu_console_puthex(uint32_t v);

/*
 * Dump the guest's architectural state through the frontend.
 *
 * Which registers exist and what the trap cause means is the *frontend's*
 * knowledge, not a platform's -- this used to be a copy of it here, kept
 * in step with the core's by hand and with the other platform's by hand
 * again.
 */
void emu_report_state(emu_cpu_t *cpu, const emu_cpu_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_CONSOLE_H */
