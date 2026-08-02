/* SPDX-License-Identifier: Apache-2.0 */
/*
 * irqtest.c - a real peripheral interrupt, taken by guest code.
 *
 * This is the end-to-end check for the one thing the passthrough window
 * cannot carry. A guest driver reaches TIM6 by using the address RM0390
 * gives it, but when TIM6 raises an interrupt the ARM NVIC vectors into the
 * *emulator*, not into the guest. The path under test is what happens next:
 *
 *   TIM6 update event
 *     -> NVIC vectors to TIM6_DAC_IRQHandler in the firmware
 *     -> the firmware masks the line and calls rv_aplic_raise
 *     -> APLIC sets pending, and MEIP reaches the hart
 *     -> the guest traps, reads topi, clears TIM6's own flag through the
 *        passthrough window, then claims
 *     -> claiming clears pending, which unmasks the line again
 *
 * The masking is not an optimisation. Nothing on the emulator's side can
 * service TIM6 -- only this guest can, and it does not run until the
 * firmware's handler returns -- so an unmasked level-triggered source would
 * re-enter the handler forever with the guest never making progress.
 *
 * Which is why the interesting assertion is not that *an* interrupt
 * arrives, but that a hundred of them do: each one requires the mask and
 * unmask to round-trip correctly. A bridge that masked and never unmasked
 * would deliver exactly one.
 *
 * Separate from isatest because it is only meaningful on the board. On the
 * host runner the peripheral window is plain memory, so TIM6 never counts
 * and never fires; the test detects that and reports it rather than hanging.
 */

#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#define csr_read(name) ({                               \
    uint32_t v_;                                        \
    __asm__ volatile ("csrr %0, " name : "=r"(v_));     \
    v_; })
#define csr_write(name, val) \
    __asm__ volatile ("csrw " name ", %0" :: "r"((uint32_t)(val)))
#define csr_set(name, val) \
    __asm__ volatile ("csrs " name ", %0" :: "r"((uint32_t)(val)))

/* ---- APLIC (AIA 20250312), direct delivery ------------------------- */

#define APLIC_BASE          0x0C000000u
#define APLIC_R(off)        (*(volatile uint32_t *)(APLIC_BASE + (off)))
#define APLIC_DOMAINCFG     APLIC_R(0x0000u)
#define APLIC_SOURCECFG(i)  APLIC_R(0x0004u + 4u * ((i) - 1u))
#define APLIC_SETIENUM      APLIC_R(0x1EDCu)
#define APLIC_TARGET(i)     APLIC_R(0x3004u + 4u * ((i) - 1u))
#define APLIC_IDELIVERY     APLIC_R(0x4000u)
#define APLIC_TOPI          APLIC_R(0x4018u)
#define APLIC_CLAIMI        APLIC_R(0x401Cu)

#define APLIC_SM_EDGE_RISE  4u

/* The source the firmware binds TIM6 to; see RV_IRQ_SRC_TIM6. */
#define SRC_TIM6            1u

/* ---- STM32F446 registers, verbatim from RM0390 --------------------- */

#define RCC_APB1ENR (*(volatile uint32_t *)0x40023840u)
#define RCC_TIM6EN  (1u << 4)

#define TIM6_BASE   0x40001000u
#define TIM6_CR1    (*(volatile uint32_t *)(TIM6_BASE + 0x00u))
#define TIM6_DIER   (*(volatile uint32_t *)(TIM6_BASE + 0x0Cu))
#define TIM6_SR     (*(volatile uint32_t *)(TIM6_BASE + 0x10u))
#define TIM6_EGR    (*(volatile uint32_t *)(TIM6_BASE + 0x14u))
#define TIM6_CNT    (*(volatile uint32_t *)(TIM6_BASE + 0x24u))
#define TIM6_PSC    (*(volatile uint32_t *)(TIM6_BASE + 0x28u))
#define TIM6_ARR    (*(volatile uint32_t *)(TIM6_BASE + 0x2Cu))

#define TIM6_CEN    (1u << 0)
#define TIM6_UIE    (1u << 0)
#define TIM6_UIF    (1u << 0)
#define TIM6_UG     (1u << 0)

/* ---- console ------------------------------------------------------- */

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void putu(uint32_t v)
{
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        UART_THR = (uint8_t)tmp[--n];
    }
}

static unsigned g_checks, g_failures;

static void check(const char *name, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        puts_("FAIL ");
        puts_(name);
        puts_(" got ");
        putu(got);
        puts_(" want ");
        putu(want);
        puts_("\n");
    }
}

/* ---- interrupt handling -------------------------------------------- */

static volatile uint32_t g_irqs;
static volatile uint32_t g_bad_source;
static volatile uint32_t g_spurious;

/*
 * The order here is the whole point.
 *
 * topi reports the source without clearing anything, so the device can be
 * serviced first; claimi clears the pending bit, and clearing it is what
 * tells the firmware the guest is done and the NVIC line may be unmasked.
 * Claiming *before* clearing TIM6's flag would unmask a line whose source
 * is still asserted, and the firmware would take a second, pointless
 * interrupt for every real one.
 */
__attribute__((interrupt("machine"), aligned(4), used))
static void trap_handler(void)
{
    const uint32_t cause = csr_read("mcause");

    if ((cause & 0x80000000u) == 0u || (cause & 0xFFu) != 11u) {
        /* Nothing else should trap here; record it and let mret retry. */
        g_spurious++;
        return;
    }

    const uint32_t topi = APLIC_TOPI;
    const uint32_t source = topi >> 16;

    if (source == SRC_TIM6) {
        TIM6_SR = 0u;               /* clear UIF, at the peripheral */
        g_irqs++;
    } else {
        g_bad_source = topi;
    }

    (void)APLIC_CLAIMI;             /* clears pending, unmasks the line */
}

int main(void)
{
    puts_("IRQTEST-START\n");

    csr_write("mtvec", (uint32_t)&trap_handler);

    /* Ungate TIM6, through the passthrough window like any driver would. */
    RCC_APB1ENR |= RCC_TIM6EN;
    (void)RCC_APB1ENR;

    /*
     * APB1 runs at 45 MHz and its timers at twice that, so 9000 x 10 ticks
     * of a 90 MHz clock is one millisecond.
     */
    TIM6_PSC = 8999u;
    TIM6_ARR = 9u;
    TIM6_EGR = TIM6_UG;             /* load PSC and ARR now */
    TIM6_SR = 0u;                   /* UG set UIF; do not start pending */

    /*
     * Is there a timer here at all? On the host runner this window is
     * ordinary memory: CNT never moves, and waiting for an interrupt that
     * cannot arrive would hang.
     */
    TIM6_CR1 = TIM6_CEN;
    uint32_t spins = 0u;
    while (TIM6_CNT == 0u && spins < 200000u) {
        spins++;
    }
    if (TIM6_CNT == 0u) {
        puts_("no timer behind the window: not the target, skipping\n");
        puts_("IRQTEST-END\n");
        return 0;
    }

    /* Route TIM6 to the hart: edge-triggered, enabled, delivery on. */
    APLIC_SOURCECFG(SRC_TIM6) = APLIC_SM_EDGE_RISE;
    APLIC_TARGET(SRC_TIM6) = 1u;
    APLIC_SETIENUM = SRC_TIM6;
    APLIC_IDELIVERY = 1u;
    APLIC_DOMAINCFG = 0x100u;       /* IE */

    TIM6_DIER = TIM6_UIE;
    csr_set("mie", 1u << 11);       /* MEIE */
    csr_set("mstatus", 1u << 3);    /* MIE */

    /*
     * A hundred of them. One would only prove the line was unmasked at
     * reset; each further one requires the mask-and-unmask handshake to
     * have completed correctly.
     */
    const uint32_t want = 100u;
    uint32_t guard = 0u;
    while (g_irqs < want && guard < 50000000u) {
        guard++;
    }

    TIM6_CR1 = 0u;
    TIM6_DIER = 0u;

    check("irqs-delivered", g_irqs >= want, 1u);
    check("source-correct", g_bad_source, 0u);
    check("no-spurious-traps", g_spurious, 0u);
    /* The flag really was cleared by the guest, not by the firmware. */
    check("uif-clear", TIM6_SR & TIM6_UIF, 0u);

    puts_("interrupts ");
    putu(g_irqs);
    puts_("\nchecks ");
    putu(g_checks);
    puts_(" failures ");
    putu(g_failures);
    puts_("\nIRQTEST-END\n");
    return 0;
}
