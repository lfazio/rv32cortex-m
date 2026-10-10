/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board_con.c - USART3 on the Nucleo-F746ZG, polled, at HSI 16 MHz.
 *
 * USART3 on PD8/PD9 is the ST-LINK's virtual COM port (UM1974, Table 9),
 * the same console the emulator firmware uses. Bare registers rather than
 * the HAL: the harness is a few lines of start-up and a putc, and a HAL
 * would be the largest thing on the board by far.
 *
 * 460800 baud from a 16 MHz clock is BRR 35, 0.8% fast -- inside what a
 * UART tolerates, and four times quicker than 115200 for output that runs
 * to hundreds of kilobytes.
 */

#include "diff.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC_AHB1ENR REG(0x40023830u)
#define RCC_APB1ENR REG(0x40023840u)
#define GPIOD_MODER REG(0x40020C00u)
#define GPIOD_OSPEEDR REG(0x40020C08u)
#define GPIOD_AFRH REG(0x40020C24u)
#define USART3_CR1 REG(0x40004800u)
#define USART3_BRR REG(0x4000480Cu)
#define USART3_ISR REG(0x4000481Cu)
#define USART3_TDR REG(0x40004828u)

void board_init(void)
{
    RCC_AHB1ENR |= 1u << 3;  /* GPIOD */
    RCC_APB1ENR |= 1u << 18; /* USART3 */
    (void)RCC_APB1ENR;       /* let the enable land before using it */

    /* PD8, PD9: alternate function 7. */
    GPIOD_MODER = (GPIOD_MODER & ~(0xFu << 16)) | (0xAu << 16);
    GPIOD_OSPEEDR |= 0xFu << 16;
    GPIOD_AFRH = (GPIOD_AFRH & ~0xFFu) | 0x77u;

    USART3_CR1 = 0u;
    USART3_BRR = 35u;
    USART3_CR1 = (1u << 3) | (1u << 0); /* TE, UE */
}

void diff_putc(char c)
{
    while ((USART3_ISR & (1u << 7)) == 0u) { /* TXE */
    }
    USART3_TDR = (uint8_t)c;
}

static void putx(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        diff_putc("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        diff_putc(*s++);
    }
}

/*
 * A fault on the board is a result too: the line says which exception
 * and where, and the comparison reports it against whatever the emulator
 * did. Then stop -- there is no sensible way to resume a case.
 */
void board_fault(const uint32_t *frame, uint32_t ipsr)
{
    puts_("\nDIFF-FAULT exc ");
    putx(ipsr);
    puts_(" pc ");
    putx(frame[6]);
    puts_(" cfsr ");
    putx(REG(0xE000ED28u));
    puts_(" hfsr ");
    putx(REG(0xE000ED2Cu));
    puts_(" mmfar ");
    putx(REG(0xE000ED34u));
    puts_(" bfar ");
    putx(REG(0xE000ED38u));
    puts_("\nDIFF-END\n");
    for (;;) {
    }
}

void diff_stop(void)
{
    for (;;) {
    }
}
