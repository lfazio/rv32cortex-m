/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hello.c - Smallest useful guest: prove the console and the timer work.
 *
 * On the STM32 build the UART writes reach the ST-LINK virtual COM port,
 * so this is what you run first after flashing to confirm the whole path
 * (ARM firmware -> emulator -> guest -> virtual UART -> USART2) is alive.
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

int main(void)
{
    puts_("hello from RV32 on Cortex-M\n");

    uint32_t misa;
    __asm__ volatile ("csrr %0, misa" : "=r"(misa));
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
