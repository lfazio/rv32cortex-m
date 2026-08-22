/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_diag.c - diagnostics that every runner prints identically.
 *
 * A file for one function, and it earns it: there were two copies of
 * emu_jit_diff_report and they had already drifted. The firmware printed
 * twelve divergences in one format and the host printed twenty in
 * another, so a report from a board could not be diffed against a report
 * from a host -- for a debugging aid whose entire value is comparing two
 * runs against each other.
 *
 * It prints through emu_console_printf, which every platform supplies:
 * the firmware's is in emu_console.c and goes to the UART or the telnet
 * ring, the host's goes to stderr *and* the ring. That is the only thing
 * the two runners genuinely disagree about here, and it is the platform's
 * to decide -- which is why this file takes a printf rather than a sink.
 */

#include "emu_console.h"

#ifdef EMU_JIT_DIFF
/*
 * A block whose compiled code disagreed with the IR interpreter.
 *
 * `off` is a byte offset into the guest state, so the register file
 * starts at zero and the number is the register times its width. Only the
 * first few are printed: everything after the first divergence is
 * downstream of the same bug, and a UART at 921600 is not a debugger.
 */
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got);

void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got)
{
    static unsigned reported;

    if (reported++ >= 12u) {
        return;
    }
    emu_console_printf("jit-diff pc 0x%08x +%u want 0x%08x got 0x%08x\n",
                       (unsigned)pc, (unsigned)off, (unsigned)want,
                       (unsigned)got);
}
#endif
