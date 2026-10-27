/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mmiobench.c - the driver-shaped workload, for the ARMv7-M frontend.
 *
 * The same four kernels as the RV32 guest (tests/guest/mmiobench.c), each
 * against a peripheral register and against a word of guest RAM with the
 * same machine code -- the RAM form is the control -- and the same
 * correctness checks, run before the JIT inlines the peripheral window
 * and again after:
 *
 *   read   load a status register, the pattern behind every poll loop
 *   write  store to a command register
 *   rmw    read-modify-write, the pattern behind every bit twiddle
 *   poll   read, test a bit, branch -- a real driver's wait loop
 *
 * GPIOB on the Nucleo-F746ZG, as for RV32: BSRR and ODR writes leave its
 * pins in their reset modes, and IDR reads are harmless.
 *
 * **The clock is TIM2, which the guest runs itself.** An ARMv7-M guest
 * has no wall clock here -- SysTick counts instructions, so every kernel
 * would read the same -- but it has the board's peripherals, and a free
 * running 32-bit timer in the passthrough window is one. Nothing in the
 * firmware uses it. On the host the window is simulated memory and the
 * count stays at zero: the host run is a correctness check.
 *
 * A refused store raises a precise BusFault, which escalates to
 * HardFault; the handler records CFSR and BFAR and steps over the store,
 * so each check asks for the cause and the address, not only that some
 * fault arrived.
 */

#include <stdint.h>

#define UART (*(volatile uint8_t *)0x10000000u)

#define SCB_CFSR (*(volatile uint32_t *)0xE000ED28u)
#define SCB_BFAR (*(volatile uint32_t *)0xE000ED38u)
#define CFSR_PRECISERR (1u << 9)
#define CFSR_BFARVALID (1u << 15)

/* STM32F7 peripheral addresses, from RM0385. */
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830u)
#define RCC_APB1ENR (*(volatile uint32_t *)0x40023840u)
#define RCC_GPIOBEN (1u << 1)
#define RCC_TIM2EN (1u << 0)

#define TIM2_CR1 (*(volatile uint32_t *)0x40000000u)
#define TIM2_EGR (*(volatile uint32_t *)0x40000014u)
#define TIM2_CNT (*(volatile uint32_t *)0x40000024u)
#define TIM2_PSC (*(volatile uint32_t *)0x40000028u)
#define TIM2_ARR (*(volatile uint32_t *)0x4000002Cu)

#define GPIOB_BASE 0x40020400u
#define GPIOB_IDR (*(volatile uint32_t *)(GPIOB_BASE + 0x10u))
#define GPIOB_ODR (*(volatile uint32_t *)(GPIOB_BASE + 0x14u))
#define GPIOB_BSRR (*(volatile uint32_t *)(GPIOB_BASE + 0x18u))

/* The read-only clock-tree hole, its last word, and the word above it. */
#define RCC_CR_ADDR 0x40023800u
#define RCC_CIR_ADDR 0x4002380Cu
#define RCC_CR (*(volatile uint32_t *)RCC_CR_ADDR)
#define RCC_CIR (*(volatile uint32_t *)RCC_CIR_ADDR)
#define ABOVE_HOLE (*(volatile uint32_t *)0x40023810u) /* AHB1RSTR */
/* Past the window: no region covers it, so it faults before the bus. */
#define OUTSIDE_ADDR 0x60000000u
#define OUTSIDE (*(volatile uint32_t *)OUTSIDE_ADDR)

#define ITER 50000u

static volatile uint32_t g_ram_reg;
static volatile uint32_t g_sink;

static void pc_(char c)
{
    UART = (uint8_t)c;
}

static void ps_(const char *s)
{
    while (*s != '\0') {
        pc_(*s++);
    }
}

static void pu_(uint32_t v)
{
    char tmp[10];
    unsigned n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        pc_(tmp[--n]);
    }
}

static void phex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

/* ------------------------------------------------------------------ */
/* Kernels: one copy of each, reached with either target, so the two   */
/* forms are the same translated block (see the RV32 guest).           */
/* ------------------------------------------------------------------ */

#define NOINLINE __attribute__((noinline))

static NOINLINE void k_read(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < ITER; i++) {
        g_sink = *p;
    }
}

static NOINLINE void k_write(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < ITER; i++) {
        *p = (1u << 16);
    }
}

static NOINLINE void k_rmw(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < ITER; i++) {
        *p = *p & ~0xFFu;
    }
}

/* Falls through on the first read: bit 31 of IDR is reserved, reads 0. */
static NOINLINE void k_poll(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < ITER; i++) {
        while ((*p & 0x80000000u) != 0u) {
        }
    }
}

static void report(const char *name, void (*fn)(volatile uint32_t *),
                   volatile uint32_t *target)
{
    const uint32_t t0 = TIM2_CNT;

    fn(target);

    const uint32_t t1 = TIM2_CNT;

    /* name, TIM2 ticks, and ticks per thousand accesses */
    ps_(name);
    pc_(' ');
    pu_(t1 - t0);
    pc_(' ');
    pu_((t1 - t0) / (ITER / 1000u)); /* no 64-bit divide: no libgcc */
    pc_('\n');
}

/* ------------------------------------------------------------------ */
/* Faults                                                              */
/* ------------------------------------------------------------------ */

volatile uint32_t g_traps;
volatile uint32_t g_last_cfsr;
volatile uint32_t g_last_bfar;

/*
 * Record why, clear it, and step over the store: a halfword whose top
 * five bits are 0b11101 or above begins a 32-bit instruction.
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm__ volatile("mrs   r0, msp\n\t"
                     "ldr   r1, [r0, #24]\n\t"
                     "ldrh  r2, [r1]\n\t"
                     "lsrs  r2, r2, #11\n\t"
                     "cmp   r2, #0x1D\n\t"
                     "ite   hs\n\t"
                     "addhs r1, r1, #4\n\t"
                     "addlo r1, r1, #2\n\t"
                     "str   r1, [r0, #24]\n\t"
                     "ldr   r2, =0xE000ED28\n\t"
                     "ldr   r1, [r2]\n\t"
                     "ldr   r3, =g_last_cfsr\n\t"
                     "str   r1, [r3]\n\t"
                     "str   r1, [r2]\n\t" /* write-one-to-clear */
                     "ldr   r2, =0xE000ED38\n\t"
                     "ldr   r1, [r2]\n\t"
                     "ldr   r3, =g_last_bfar\n\t"
                     "str   r1, [r3]\n\t"
                     "ldr   r2, =g_traps\n\t"
                     "ldr   r1, [r2]\n\t"
                     "adds  r1, r1, #1\n\t"
                     "str   r1, [r2]\n\t"
                     "bx    lr\n\t"
                     ".ltorg\n\t");
}

static unsigned g_checks, g_fails;

static void check(const char *name, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_fails++;
        ps_("FAIL ");
        ps_(name);
        ps_(" got ");
        phex(got);
        ps_(" want ");
        phex(want);
        pc_('\n');
    }
}

/* A store that must fault: once, precisely, at this address. */
static void check_refused(const char *name, uint32_t before, uint32_t addr)
{
    check(name, g_traps - before, 1u);
    check(name, g_last_cfsr & (CFSR_PRECISERR | CFSR_BFARVALID),
          CFSR_PRECISERR | CFSR_BFARVALID);
    check(name, g_last_bfar, addr);
}

static void verify(void)
{
    /* Repeated: the first pass may run interpreted, and blocks are on trial. */
    for (unsigned pass = 0; pass < 8u; pass++) {
        uint32_t before;

        GPIOB_BSRR = (1u << 3);
        check("bsrr-set", (GPIOB_ODR >> 3) & 1u, 1u);
        GPIOB_BSRR = (1u << (3 + 16));
        check("bsrr-clear", (GPIOB_ODR >> 3) & 1u, 0u);

        /* A hole is readable... */
        before = g_traps;
        const uint32_t cr = RCC_CR;
        check("hole-load-ok", g_traps - before, 0u);

        /* ...and not writable; the value written back is the one read. */
        before = g_traps;
        RCC_CR = cr;
        check_refused("hole-store", before, RCC_CR_ADDR);

        before = g_traps;
        RCC_CIR = 0u;
        check_refused("hole-end", before, RCC_CIR_ADDR);

        /* Immediately above it: permitted (AHB1RSTR, written its reset 0). */
        before = g_traps;
        ABOVE_HOLE = 0u;
        check("above-hole-ok", g_traps - before, 0u);

        /* Outside the window: a fault, not a wild store. */
        before = g_traps;
        OUTSIDE = 0u;
        check_refused("outside", before, OUTSIDE_ADDR);
    }
}

int main(void)
{
    /* Ungate GPIOB and TIM2; the read back gives the clocks time. */
    RCC_AHB1ENR |= RCC_GPIOBEN;
    RCC_APB1ENR |= RCC_TIM2EN;
    g_sink = RCC_APB1ENR;

    TIM2_PSC = 0u;
    TIM2_ARR = 0xFFFFFFFFu;
    TIM2_EGR = 1u; /* load the prescaler */
    TIM2_CR1 = 1u; /* count */

    ps_("MMIOBENCH-START\n");

    verify();
    ps_("checks ");
    pu_(g_checks);
    ps_(" failures ");
    pu_(g_fails);
    pc_('\n');

    ps_("kernel tim2 tim2_per_1000\n");
    report("read-mmio ", k_read, &GPIOB_IDR);
    report("read-ram  ", k_read, &g_ram_reg);
    report("write-mmio", k_write, &GPIOB_BSRR);
    report("write-ram ", k_write, &g_ram_reg);
    report("rmw-mmio  ", k_rmw, &GPIOB_ODR);
    report("rmw-ram   ", k_rmw, &g_ram_reg);
    report("poll-mmio ", k_poll, &GPIOB_IDR);
    report("poll-ram  ", k_poll, &g_ram_reg);

    /* Again, once the window is inlined: the holes on the inlined path. */
    verify();
    ps_("checks ");
    pu_(g_checks);
    ps_(" failures ");
    pu_(g_fails);
    pc_('\n');

    TIM2_CR1 = 0u;
    ps_(g_fails == 0u ? "MMIOBENCH-END\n" : "MMIOBENCH-FAILED\n");
    return (int)g_fails;
}
