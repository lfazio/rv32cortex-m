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
 * `putu` and `puthex` are gone rather than shared. They existed because
 * there was no printf, so a stats line cost one call per field and per
 * literal, and every fixed-point ratio was hand-assembled into three --
 * `x100 / 100`, a literal '.', then the tens and units of `x100 % 100`.
 * That was correct, and `%u.%02u` says it once.
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

#include <stdbool.h>

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

/*
 * A byte of guest input, or -1 when none has arrived. Supplied by the
 * platform for the same reason as putc: it is a UART on a board, stdin or
 * a telnet connection on a host, and the guest's virtual UART reads
 * whichever this is.
 */
int emu_console_getchar(void);

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

/*
 * Dump the guest's architectural state through the frontend.
 *
 * Which registers exist and what the trap cause means is the *frontend's*
 * knowledge, not a platform's -- this used to be a copy of it here, kept
 * in step with the core's by hand and with the other platform's by hand
 * again.
 */
void emu_report_state(emu_cpu_t *cpu, const emu_cpu_ops_t *ops);

/* Every core's, headed by index when there is more than one. */
void emu_report_states(emu_system_t *sys);

#if EMU_ENABLE_TRACE
/*
 * The instruction trace, installed by emu_session_start on every core.
 * `skip` and `count` bound it: a full trace of a million instructions is
 * not readable and not what anyone wants -- the question is always what
 * happened around some point.
 */
void emu_trace_configure(uint64_t skip, uint64_t count);
void emu_trace_insn(emu_cpu_t *cpu, uint32_t pc, uint64_t insn, unsigned len,
                    void *user);
#endif

/* A byte of guest output: LF becomes CRLF, as for emu_console_puts. */
void emu_console_putchar(uint8_t c);

/* The same, shaped for emu_uart_init's transmit callback. */
void emu_console_uart_tx(void *ctx, uint8_t c);

/*
 * What the shared syscall handler needs from the board.
 *
 * The bus to read a guest buffer through, the core to halt, and where to
 * record the exit status -- the platform owns all three and passes this
 * as the `user` pointer emu_syscall_fn already carries.
 */
typedef struct emu_guest_exit {
    uint32_t code;
    bool exited;
} emu_guest_exit_t;

typedef struct emu_syscall_ctx {
    struct emu_bus *bus;
    struct emu_core *core;
    emu_guest_exit_t *exit;
} emu_syscall_ctx_t;

/* newlib's write(64) and exit(93); anything else is declined so the
 * frontend takes its architectural trap. */
bool emu_guest_syscall(emu_cpu_t *cpu, emu_syscall_t *sc, void *user);

/*
 * The run summary and the framework's JIT statistics, in emu_stats.c.
 * Both are frontend- and platform-independent; emu_print_jit_stats
 * returns false when this build has no JIT, so a caller can skip its own
 * frontend-specific additions.
 */
void emu_print_run_summary(uint64_t retired, uint32_t host_cycles);
bool emu_print_jit_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_CONSOLE_H */
