/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_clock.h - the time base, as a benchmark's clock.
 *
 * crt0.S enables it on the processor clock, where in this emulator a
 * tick is an instruction: a figure derived from it belongs to the guest
 * binary rather than to how fast the emulator ran, and the interpreter
 * and translated code read the same number -- as they must. The same
 * caveat the ARMv7-M guests carry for SysTick.
 */
#ifndef PPC_CLOCK_H
#define PPC_CLOCK_H

static __inline__ unsigned int ppc_clock_now(void)
{
    unsigned int v;

    __asm__ volatile("mfspr %0, 268" : "=r"(v)); /* TBL */
    return v;
}

/* Decrementer interrupts taken so far; see crt0.S. */
extern volatile unsigned int ppc_dec_ticks;

#endif /* PPC_CLOCK_H */
