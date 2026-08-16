/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hello.c - Smallest useful guest: prove the console and the timer work.
 *
 * On the STM32 build the UART writes reach the ST-LINK virtual COM port,
 * so this is what you run first after flashing to confirm the whole path
 * (ARM firmware -> emulator -> guest -> virtual UART -> USART2) is alive.
 * It runs on the host runner too, which is why nothing here claims to
 * know what the host is.
 */

#include <stdint.h>

#define UART_THR   (*(volatile uint8_t *)0x10000000u)
#define MTIME_LO   (*(volatile uint32_t *)0x0200BFF8u)

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4) {
        UART_THR = (uint8_t)hex[(v >> i) & 0xFu];
    }
}

/*
 * The ISA this core reports, from misa, as letters.
 *
 * The banner used to be the literal "hello from RV32 on Cortex-M", and
 * both halves of that were wrong to assert. A guest cannot know its
 * host -- run under the host runner it prints on x86-64 -- and a
 * hand-written "RV32" cannot track what the core was built with, which
 * is the drift the firmware's own banner comment warns about and
 * avoids by taking ops->desc instead of spelling it out. Here misa is
 * the equivalent: it is the architecture's own statement of what is
 * implemented, and reading it is the whole point of a first-run guest.
 *
 * MXL in bits 31:30 gives the width, and bits 25:0 are A..Z.
 */
static void put_isa(uint32_t misa)
{
    /* MXL 1/2/3 is 32/64/128 bits -- a *width*, not a digit. Written as
     * one character it printed "RV22". */
    static const char *const mxl[4] = { "?", "32", "64", "128" };

    puts_("RV");
    puts_(mxl[(misa >> 30) & 3u]);
    for (unsigned i = 0; i < 26u; i++) {
        if ((misa >> i) & 1u) {
            UART_THR = (uint8_t)('A' + i);
        }
    }
}

int main(void)
{
    uint32_t misa;
    __asm__ volatile ("csrr %0, misa" : "=r"(misa));

    puts_("hello from ");
    put_isa(misa);
    puts_(", emulated\n");

    puts_("misa    ");
    puthex(misa);

    puts_("\nmtime   ");
    puthex(MTIME_LO);

    puts_("\nmhartid ");
    uint32_t id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    puthex(id);
    UART_THR = '\n';

    return 0;
}
