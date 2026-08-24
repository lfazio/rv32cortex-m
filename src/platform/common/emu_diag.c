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

void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want, uint32_t got)
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

/* emu_print_fn onto the console, for the frontend's own state dump. */
static void console_out(void *ctx, const char *s)
{
    (void)ctx;
    emu_console_puts(s);
}

void emu_report_state(emu_cpu_t *cpu, const emu_cpu_ops_t *ops)
{
    /*
     * A frontend without a dump is a frontend that has not written one
     * yet -- the PowerPC one has not -- and calling through the NULL was
     * a latent crash for exactly as long as nobody asked it to. The host
     * only dumped under --dump and nobody passed it for that frontend;
     * moving the dump into the shared runner asked it on the first run.
     */
    if (ops->dump == NULL) {
        emu_console_printf("\n-- guest state -- (%s has no dump)\n", ops->name);
        return;
    }
    emu_console_puts("\n-- guest state --");
    ops->dump(cpu, console_out, NULL);
}

/*
 * Every core's state, headed by which core it is when there is more than
 * one.
 *
 * The header matters more than it looks: with several cores the dumps are
 * otherwise four identical-looking blocks, and the whole reason to read
 * them is to find the one that differs. The host printed the header and
 * the board printed core 0 only -- so a multicore guest on hardware
 * reported a third of its state and said nothing about the rest.
 */
void emu_report_states(emu_system_t *sys)
{
    for (unsigned i = 0; i < sys->ncores; i++) {
        if (sys->ncores > 1u) {
            emu_console_printf("\n--- core %u ---", i);
        }
        emu_report_state(sys->core[i].cpu, sys->core[i].ops);
    }
}

/* ------------------------------------------------------------------ */
/* Instruction trace                                                   */
/* ------------------------------------------------------------------ */

/*
 * One instruction: where, what, and the first few registers after it.
 *
 * **Shared, and the board did not have one at all.** This lived in the
 * host runner, which meant the one tool that answers "what did the guest
 * actually execute" was unavailable on the platform where a guest is
 * hardest to observe. CLAUDE.md records a trace build being what found
 * the G4MH RIE defect in a single run after three sessions of bisecting
 * expressions -- and that was on the host, with a guest that also ran
 * there. A guest that only misbehaves on hardware had nothing.
 *
 * Registers by index rather than by name because the frontend decides
 * what they are called, and a divergence hunt wants the same columns on
 * every line.
 */
#if EMU_ENABLE_TRACE
static uint64_t g_skip;
static uint64_t g_count = 64u;

void emu_trace_configure(uint64_t skip, uint64_t count)
{
    g_skip = skip;
    g_count = count;
}

void emu_trace_insn(emu_cpu_t *cpu, uint32_t pc, uint64_t insn, unsigned len,
                    void *user)
{
    const emu_cpu_ops_t *const ops = (const emu_cpu_ops_t *)user;
    emu_cpu_status_t st;

    ops->status(cpu, &st);
    if (st.retired < g_skip || st.retired >= g_skip + g_count) {
        return;
    }

    char buf[64];

    buf[0] = '\0';
    if (ops->disasm != NULL) {
        ops->disasm(buf, sizeof(buf), pc, insn, len);
    }

    /*
     * The encoding, only as wide as it is. Printing a fixed eight digits
     * pads a 16-bit instruction with four zeros that look like part of
     * it -- which on an ISA where a shared opcode holds two widths is
     * exactly the thing the reader is trying to tell apart.
     */
    emu_console_printf("%8u %08x  %0*llx%*s  %-28s", (unsigned)st.retired,
                       (unsigned)pc, (int)(len * 2u), (unsigned long long)insn,
                       (int)(16u - len * 2u), "", buf);

    for (unsigned r = 1; r < 8u && r < ops->nregs; r++) {
        emu_console_printf(" %s=%08x", ops->reg_name(r),
                           (unsigned)ops->reg_read(cpu, r));
    }
    emu_console_puts("\n");
}
#endif /* EMU_ENABLE_TRACE */
