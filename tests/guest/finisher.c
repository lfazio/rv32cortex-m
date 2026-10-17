/* SPDX-License-Identifier: Apache-2.0 */
/*
 * finisher.c - the test finisher ends the run (include/emu/emu_finisher.h).
 *
 * Built three ways, by FINISH: a pass (0x5555), a failure with a code in
 * the high half (0x3333), and a reset (0x7777). Each prints one line,
 * writes the register, and then prints a FAIL that only a run that kept
 * going can show -- so the test's pattern requires the line *and* the
 * stop it expects right after it, with nothing in between.
 */

#define UART (*(volatile unsigned char *)0x10000000u)
#define FINISHER (*(volatile unsigned int *)0x00100000u)

#ifndef FINISH
#error "FINISH: the value to write"
#endif

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART = (unsigned char)*s++;
    }
}

int main(void)
{
    puts_("FINISHER-WRITE\n");
    FINISHER = FINISH;
    puts_("FAIL still running after the finisher write\n");
    for (;;) {
    }
}
