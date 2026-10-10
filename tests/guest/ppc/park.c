/* SPDX-License-Identifier: Apache-2.0 */
/*
 * park.c - a wait that nothing may end.
 *
 * `wait` stops the core until an *enabled* interrupt arrives (manual
 * 3.12). The decrementer is ticking, so with MSR[EE] set the wait ends
 * at the next tick -- sys.c checks that. With it clear nothing can end
 * it, and that half cannot be checked from inside a program that goes
 * on to report: the report is the thing that must not happen.
 *
 * So this prints, masks interrupts, waits, and prints again; a run that
 * shows the second line took a masked interrupt or never waited. The
 * emulator ends the run when the only core is parked with nothing able
 * to wake it, which is also what stops this from needing a timeout.
 */
#define UART (*(volatile unsigned char *)0x10000000u)

static void ps_(const char *s)
{
    while (*s) {
        UART = (unsigned char)*s++;
    }
}

int main(void)
{
    ps_("PARK-BEFORE\n");
    __asm__ volatile("wrteei 0");
    __asm__ volatile(".long 0x7C00007C"); /* wait */
    ps_("PARK-AFTER\n");
    return 0;
}
