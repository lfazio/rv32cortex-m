/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_con.c - the harness's console when it runs as an emulator guest:
 * the NS16550 transmit register every guest here writes to.
 */

#include "diff.h"

void diff_putc(char c)
{
    *(volatile unsigned char *)0x10000000u = (unsigned char)c;
}

/* BKPT is this emulator's "the guest is finished". */
void diff_stop(void)
{
    for (;;) {
        __asm__ volatile("bkpt #0");
    }
}
