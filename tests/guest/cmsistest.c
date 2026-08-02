/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cmsistest.c - the CMSIS-Core shim, driving real hardware.
 *
 * irqtest takes the same TIM6 interrupt by writing APLIC registers itself.
 * This one takes it the way a ported vendor driver would: NVIC_SetPriority,
 * NVIC_EnableIRQ, a named handler, __enable_irq. Nothing here mentions the
 * APLIC, which is the point -- if the shim is right, driver code does not
 * have to know it moved architecture.
 *
 * The one line a port gains is NVIC_SetHandler: Cortex-M binds a handler
 * through the vector table, and there is no vector table here.
 */

#include "cmsis_rv32.h"

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#define TIM6_DAC_IRQn  ((IRQn_Type)54)

#define RCC_APB1ENR (*(__IO uint32_t *)0x40023840u)
#define RCC_TIM6EN  (1u << 4)
#define TIM6_BASE   0x40001000u
#define TIM6_CR1    (*(__IO uint32_t *)(TIM6_BASE + 0x00u))
#define TIM6_DIER   (*(__IO uint32_t *)(TIM6_BASE + 0x0Cu))
#define TIM6_SR     (*(__IO uint32_t *)(TIM6_BASE + 0x10u))
#define TIM6_EGR    (*(__IO uint32_t *)(TIM6_BASE + 0x14u))
#define TIM6_CNT    (*(__IO uint32_t *)(TIM6_BASE + 0x24u))
#define TIM6_PSC    (*(__IO uint32_t *)(TIM6_BASE + 0x28u))
#define TIM6_ARR    (*(__IO uint32_t *)(TIM6_BASE + 0x2Cu))

static void puts_(const char *s) { while (*s) { UART_THR = (uint8_t)*s++; } }
static void putu(uint32_t v)
{
    char t[10]; unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) { UART_THR = (uint8_t)t[--n]; }
}

static unsigned g_checks, g_failures;
static void check(const char *name, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        puts_("FAIL "); puts_(name);
        puts_(" got "); putu(got);
        puts_(" want "); putu(want); puts_("\n");
    }
}

static volatile uint32_t g_ticks;

/* Named exactly as it would be on Cortex-M. */
static void TIM6_DAC_IRQHandler(void)
{
    TIM6_SR = 0u;
    g_ticks++;
}

int main(void)
{
    puts_("CMSISTEST-START\n");
    cmsis_rv32_init();

    /* The intrinsics, before anything depends on them. */
    __disable_irq();
    check("primask-set", __get_PRIMASK(), 1u);
    __enable_irq();
    check("primask-clear", __get_PRIMASK(), 0u);
    __disable_irq();
    __DSB(); __ISB(); __DMB(); __NOP();

    check("clz", __CLZ(1u), 31u);
    check("clz-zero", __CLZ(0u), 32u);
    check("rev", __REV(0x11223344u), 0x44332211u);
    check("rbit", __RBIT(0x00000001u), 0x80000000u);

    /* The cycle counter advances. */
    const uint32_t c0 = cmsis_rv32_cycles();
    for (volatile int i = 0; i < 100; i++) { }
    check("cyccnt-advances", cmsis_rv32_cycles() > c0, 1u);

    /* Priority round-trips through the APLIC's IPRIO. */
    NVIC_SetPriority(TIM6_DAC_IRQn, 3u);
    check("priority", NVIC_GetPriority(TIM6_DAC_IRQn), 3u);

    /* Pending set and cleared through the CMSIS API alone. */
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    NVIC_SetPendingIRQ(TIM6_DAC_IRQn);
    check("pending-set", NVIC_GetPendingIRQ(TIM6_DAC_IRQn), 1u);
    NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    check("pending-clear", NVIC_GetPendingIRQ(TIM6_DAC_IRQn), 0u);
    NVIC_DisableIRQ(TIM6_DAC_IRQn);

    /* Now the real thing. */
    RCC_APB1ENR |= RCC_TIM6EN;
    (void)RCC_APB1ENR;
    TIM6_PSC = 8999u;
    TIM6_ARR = 9u;
    TIM6_EGR = 1u;
    TIM6_SR = 0u;
    TIM6_CR1 = 1u;

    uint32_t spins = 0u;
    while (TIM6_CNT == 0u && spins < 200000u) { spins++; }
    if (TIM6_CNT == 0u) {
        puts_("no timer behind the window: not the target, skipping\n");
        puts_("checks "); putu(g_checks);
        puts_(" failures "); putu(g_failures);
        puts_("\nCMSISTEST-END\n");
        return 0;
    }

    NVIC_SetHandler(TIM6_DAC_IRQn, TIM6_DAC_IRQHandler);
    NVIC_SetPriority(TIM6_DAC_IRQn, 1u);
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6_DIER = 1u;
    __enable_irq();

    uint32_t guard = 0u;
    while (g_ticks < 100u && guard < 50000000u) { guard++; }

    TIM6_CR1 = 0u;
    TIM6_DIER = 0u;
    NVIC_DisableIRQ(TIM6_DAC_IRQn);

    check("irqs-via-cmsis", g_ticks >= 100u, 1u);
    check("uif-clear", TIM6_SR & 1u, 0u);

    puts_("interrupts "); putu(g_ticks);
    puts_("\nchecks "); putu(g_checks);
    puts_(" failures "); putu(g_failures);
    puts_("\nCMSISTEST-END\n");
    return 0;
}
