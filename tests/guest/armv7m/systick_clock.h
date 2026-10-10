/* SPDX-License-Identifier: Apache-2.0 */
/*
 * systick_clock.h - a monotonic clock for an ARMv7-M guest.
 *
 * An ARMv7-M guest has no CLINT to read the time from. It has what every
 * Cortex-M has: SysTick, a 24-bit down-counter that interrupts when it
 * reaches zero. This adds up the periods in the handler and reads the
 * counter's own position for the remainder.
 *
 * In this emulator SysTick counts instructions, so a tick is an
 * instruction and a figure derived from it is a property of the guest
 * binary, not of how fast the emulator ran -- the same caveat the RV32
 * benchmarks carry for mtime on a host. Interpreted and translated runs
 * read the same number, as they must.
 *
 * **It makes a benchmark a test too.** The interrupt lands in the middle
 * of whatever the benchmark is doing, thousands of times a run, and the
 * benchmark's own checksum has to come out the same; a translating
 * backend that lost an interrupt, or took one with the flags in the
 * wrong representation, shows up here on real compiled code.
 *
 * Include it in exactly one file of an image: it defines the handler.
 * Written in C89 because Dhrystone's image is.
 */
#ifndef ARMV7M_SYSTICK_CLOCK_H
#define ARMV7M_SYSTICK_CLOCK_H

#define SYST_CSR (*(volatile unsigned int *)0xE000E010u)
#define SYST_RVR (*(volatile unsigned int *)0xE000E014u)
#define SYST_CVR (*(volatile unsigned int *)0xE000E018u)
#define SYST_PERIOD 10000u

static volatile unsigned int g_systick_ticks;
static unsigned int g_systick_started;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
    g_systick_ticks += SYST_PERIOD;
}

static void systick_clock_start(void)
{
    if (g_systick_started != 0u) {
        return;
    }
    g_systick_started = 1u;
    SYST_RVR = SYST_PERIOD - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 7u; /* ENABLE, TICKINT, the processor clock */
}

static unsigned int systick_clock_now(void)
{
    unsigned int t;
    unsigned int v;

    systick_clock_start();
    /* Read both until the handler has not run in between. */
    do {
        t = g_systick_ticks;
        v = SYST_CVR;
    } while (t != g_systick_ticks);
    return t + (SYST_PERIOD - 1u - v);
}

#endif /* ARMV7M_SYSTICK_CLOCK_H */
