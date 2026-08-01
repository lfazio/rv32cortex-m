/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stm32drv.c - STM32 peripheral drivers written in the *guest*.
 *
 * This is the point of the identity-mapped passthrough window: the
 * emulator contains no GPIO driver and no USART driver, and knows nothing
 * about either peripheral. Everything below is ordinary RISC-V code
 * poking the addresses printed in RM0390, and the emulator simply forwards
 * the loads and stores to the real silicon.
 *
 * The consequence is that porting the emulator to another MCU does not
 * mean porting the drivers. The ARM-side firmware needs a clock setup, a
 * linker script and a region table; the peripheral knowledge lives here,
 * in guest code, where it can be reused across every host that maps the
 * same silicon.
 *
 * Target: Nucleo-F446RE. LD2 is on PA5, and USART2 (PA2/PA3) is wired to
 * the ST-LINK virtual COM port.
 *
 * Registers taken from RM0390 rev 6:
 *   RCC     section 6.3    (0x4002_3800)
 *   GPIO    section 8.4    (0x4002_0000)
 *   USART   section 26.6   (0x4000_4400)
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Register map                                                        */
/* ------------------------------------------------------------------ */

#define REG(a) (*(volatile uint32_t *)(a))

/* RCC */
#define RCC_BASE        0x40023800u
#define RCC_AHB1ENR     REG(RCC_BASE + 0x30u)
#define RCC_APB1ENR     REG(RCC_BASE + 0x40u)
#define RCC_AHB1ENR_GPIOAEN (1u << 0)
#define RCC_APB1ENR_USART2EN (1u << 17)

/* GPIOA */
#define GPIOA_BASE      0x40020000u
#define GPIOA_MODER     REG(GPIOA_BASE + 0x00u)
#define GPIOA_OSPEEDR   REG(GPIOA_BASE + 0x08u)
#define GPIOA_PUPDR     REG(GPIOA_BASE + 0x0Cu)
#define GPIOA_IDR       REG(GPIOA_BASE + 0x10u)
#define GPIOA_BSRR      REG(GPIOA_BASE + 0x18u)
#define GPIOA_AFRL      REG(GPIOA_BASE + 0x20u)

/* USART2 */
#define USART2_BASE     0x40004400u
#define USART2_SR       REG(USART2_BASE + 0x00u)
#define USART2_DR       REG(USART2_BASE + 0x04u)
#define USART2_BRR      REG(USART2_BASE + 0x08u)
#define USART2_CR1      REG(USART2_BASE + 0x0Cu)

#define USART_SR_TXE    (1u << 7)
#define USART_CR1_UE    (1u << 13)
#define USART_CR1_TE    (1u << 3)
#define USART_CR1_RE    (1u << 2)

/* The emulator's own virtual console, used to narrate what the guest is
 * doing so the two paths can be told apart in the output. */
#define VIRT_UART_THR   (*(volatile uint8_t *)0x10000000u)

/* ------------------------------------------------------------------ */
/* Virtual console (portable across every host)                        */
/* ------------------------------------------------------------------ */

static void vputs(const char *s)
{
    while (*s != '\0') {
        VIRT_UART_THR = (uint8_t)*s++;
    }
}

static void vputhex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    vputs("0x");
    for (int i = 28; i >= 0; i -= 4) {
        VIRT_UART_THR = (uint8_t)hex[(v >> i) & 0xFu];
    }
}

/* ------------------------------------------------------------------ */
/* Guest-side GPIO driver                                              */
/* ------------------------------------------------------------------ */

static void led_init(void)
{
    /* Ungate GPIOA. This is why RCC's enable registers are writable in the
     * passthrough policy: a guest driver must own its own clock gate. */
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    /* PA5 to general-purpose output (MODER[11:10] = 01). */
    uint32_t moder = GPIOA_MODER;
    moder &= ~(3u << (5u * 2u));
    moder |= (1u << (5u * 2u));
    GPIOA_MODER = moder;
}

static void led_set(int on)
{
    /* BSRR: low half sets, high half resets. Atomic, no read-modify-write. */
    GPIOA_BSRR = on ? (1u << 5) : (1u << (5 + 16));
}

/* ------------------------------------------------------------------ */
/* Guest-side USART driver                                             */
/* ------------------------------------------------------------------ */

/*
 * APB1 runs at 45 MHz (180 MHz HCLK / 4). For 115200 baud with
 * oversampling by 16: USARTDIV = 45e6 / (16 * 115200) = 24.4140625,
 * so mantissa 24, fraction round(0.4140625 * 16) = 7  ->  BRR = 0x187.
 */
#define USART2_BRR_115200_AT_45MHZ 0x187u

static void uart_init(void)
{
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC_APB1ENR |= RCC_APB1ENR_USART2EN;

    /* PA2/PA3 to alternate function mode (MODER = 10), AF7 = USART2. */
    uint32_t moder = GPIOA_MODER;
    moder &= ~((3u << (2u * 2u)) | (3u << (3u * 2u)));
    moder |= (2u << (2u * 2u)) | (2u << (3u * 2u));
    GPIOA_MODER = moder;

    uint32_t afrl = GPIOA_AFRL;
    afrl &= ~((0xFu << (2u * 4u)) | (0xFu << (3u * 4u)));
    afrl |= (7u << (2u * 4u)) | (7u << (3u * 4u));
    GPIOA_AFRL = afrl;

    USART2_BRR = USART2_BRR_115200_AT_45MHZ;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void uart_putc(char c)
{
    while ((USART2_SR & USART_SR_TXE) == 0u) {
        /* wait for the transmit register to drain */
    }
    USART2_DR = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

/* ------------------------------------------------------------------ */

static void delay(uint32_t loops)
{
    for (volatile uint32_t i = 0; i < loops; i++) {
    }
}

int main(void)
{
    vputs("guest: driving STM32 peripherals directly\n");

    /*
     * Read a register before touching anything, to show the passthrough is
     * really reaching silicon: the firmware already enabled GPIOA and
     * USART2 for its own console, so these bits are set.
     */
    vputs("guest: RCC_AHB1ENR = ");
    vputhex(RCC_AHB1ENR);
    vputs("\nguest: RCC_APB1ENR = ");
    vputhex(RCC_APB1ENR);
    vputs("\n");

    led_init();
    uart_init();

    /* This line is written by a UART driver running inside the emulated
     * RISC-V core, byte by byte, straight to the real USART2. */
    uart_puts("\n[guest USART2 driver] hello from RV32 via real silicon\n");

    for (int i = 0; i < 6; i++) {
        led_set(i & 1);
        uart_puts((i & 1) ? "[guest] LD2 on\n" : "[guest] LD2 off\n");
        delay(200000);
    }

    led_set(0);
    uart_puts("[guest] done\n");
    vputs("guest: finished\n");
    return 0;
}
