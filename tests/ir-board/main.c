/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - fuzz-ir's corpus through the Thumb-2 lowering, on the board.
 *
 * tests/fuzz/fuzz_ir.c builds a block from each input, runs the IR
 * interpreter on it as built and as optimised, lowers the optimised one
 * and calls it, and compares all three. On the host the lowering is
 * x86-64's; compiled here with EMU_IR_BOARD the same file lowers to
 * Thumb-2 -- which no host suite reaches -- and this calls it once an
 * input, over a corpus linked into the image (corpus.S). A difference is
 * printed whole, as on the host, counted, and the next input runs.
 *
 * Output, at 460800 on the ST-LINK's virtual COM port:
 *
 *   IR-BOARD-START n            the inputs in the image
 *   fuzz-ir: ...                each difference, as fuzz-ir prints it
 *   IR-BOARD-FAULT case ...     a block that faulted: the run stops
 *   IR-BOARD inputs ... differ  the totals, then fuzz-ir's own stats
 *   IR-BOARD-END
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* USART3 on PD8/PD9 at 460800 from HSI 16 MHz (BRR 35), as the diff
 * harness sets it up (tests/armv7m-diff/board_con.c). */
static void uart_init(void)
{
    RCC_AHB1ENR |= 1u << 3; /* GPIOD */
    RCC_APB1ENR |= 1u << 18; /* USART3 */
    (void)RCC_APB1ENR;
    GPIOD_MODER = (GPIOD_MODER & ~(0xFu << 16)) | (0xAu << 16);
    GPIOD_OSPEEDR |= 0xFu << 16;
    GPIOD_AFRH = (GPIOD_AFRH & ~0xFFu) | 0x77u;
    USART3_CR1 = 0u;
    USART3_BRR = 35u;
    USART3_CR1 = (1u << 3) | (1u << 0); /* TE, UE */
}

static void uart_putc(char c)
{
    while ((USART3_ISR & (1u << 7)) == 0u) { /* TXE */
    }
    USART3_TDR = (uint8_t)c;
}

/* picolibc's stdio, onto the UART: fuzz_ir.c prints with fprintf. */
static int stdio_put(char c, FILE *f)
{
    (void)f;
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc(c);
    return (unsigned char)c;
}

static FILE g_uart = FDEV_SETUP_STREAM(stdio_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdin = &g_uart;
FILE *const stdout = &g_uart;
FILE *const stderr = &g_uart;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
void fuzz_ir_stats(void);

/* The corpus, packed by pack.py and linked by corpus.S. */
extern const uint8_t ir_corpus[];

static volatile uint32_t g_case;
static uint32_t g_differ;

void fuzz_ir_board_differ(void);
void fuzz_ir_board_differ(void)
{
    g_differ++;
    printf("IR-BOARD-DIFFER case %lu\n", (unsigned long)g_case);
}

/* From start.S, for every fault vector. The run cannot go on: say where. */
void ir_board_fault(const uint32_t *frame, uint32_t ipsr);
void ir_board_fault(const uint32_t *frame, uint32_t ipsr)
{
    printf("\nIR-BOARD-FAULT case %lu exc %lu pc %08lx lr %08lx cfsr %08lx "
           "hfsr %08lx\n",
           (unsigned long)g_case, (unsigned long)ipsr,
           (unsigned long)frame[6], (unsigned long)frame[5],
           (unsigned long)REG(0xE000ED28u), (unsigned long)REG(0xE000ED2Cu));
    printf("IR-BOARD-END\n");
    for (;;) {
    }
}

int main(void)
{
    const uint8_t *p = ir_corpus;
    uint32_t n = 0u;

    uart_init();
    for (const uint8_t *q = p;;) {
        const uint32_t len = (uint32_t)q[0] | ((uint32_t)q[1] << 8);

        if (len == 0u) {
            break;
        }
        q += 2u + len;
        n++;
    }
    printf("\nIR-BOARD-START %lu\n", (unsigned long)n);
    for (g_case = 0u;; g_case++) {
        const uint32_t len = (uint32_t)p[0] | ((uint32_t)p[1] << 8);

        if (len == 0u) {
            break;
        }
        (void)LLVMFuzzerTestOneInput(p + 2u, len);
        p += 2u + len;
    }
    printf("IR-BOARD inputs %lu differ %lu\n", (unsigned long)g_case,
           (unsigned long)g_differ);
    fuzz_ir_stats();
    printf("IR-BOARD-END\n");
    for (;;) {
    }
}
