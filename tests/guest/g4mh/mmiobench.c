/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mmiobench.c - the driver-shaped workload, for the G4MH frontend.
 *
 * The four kernels of the RV32 and ARMv7-M guests (tests/guest/
 * mmiobench.c, tests/guest/armv7m/mmiobench.c), each against a
 * peripheral register in the passthrough window and against a word of
 * guest RAM with the same machine code -- the RAM form is the control:
 *
 *   read   load a status register, the pattern behind every poll loop
 *   write  store to a command register
 *   rmw    read-modify-write, the pattern behind every bit twiddle
 *   poll   read, test a bit, branch -- a real driver's wait loop
 *
 * GPIOB on the Nucleo-F746ZG, as for the others: BSRR and ODR writes
 * leave its pins in their reset modes, and IDR reads are harmless. The
 * clock is TIM2, which the guest ungates and runs itself: G4MH's own
 * timers count instructions here, and every kernel would read the same.
 * On the host the window is simulated memory and the count stays at
 * zero; the host run is a correctness check.
 *
 * BSRR's set and clear are checked through ODR before the kernels and
 * after them, when the JIT has inlined the window: the same accesses on
 * the helper's path and on the inlined one. **Not asked here**: a store
 * the window refuses -- a read-only hole, an address past it -- which
 * the ARMv7-M guest checks for its cause and address. The crt0 every
 * G4MH guest shares reports a trap and halts, so the check would stop
 * the guest; the inlined lowering that has to send those stores to the
 * helper is the Thumb-2 backend's, shared with the other two.
 *
 * Built by CC-RH (scripts/g4mh-build-guest.sh); the .bin beside this is
 * the committed build.
 */

#define UART_THR (*(volatile unsigned char *)0x10000000u)

/* STM32F7 peripheral addresses, from RM0385. */
#define RCC_AHB1ENR (*(volatile unsigned *)0x40023830u)
#define RCC_APB1ENR (*(volatile unsigned *)0x40023840u)
#define RCC_GPIOBEN (1u << 1)
#define RCC_TIM2EN (1u << 0)

#define TIM2_CR1 (*(volatile unsigned *)0x40000000u)
#define TIM2_EGR (*(volatile unsigned *)0x40000014u)
#define TIM2_CNT (*(volatile unsigned *)0x40000024u)
#define TIM2_PSC (*(volatile unsigned *)0x40000028u)
#define TIM2_ARR (*(volatile unsigned *)0x4000002Cu)

#define GPIOB_BASE 0x40020400u
#define GPIOB_IDR (*(volatile unsigned *)(GPIOB_BASE + 0x10u))
#define GPIOB_ODR (*(volatile unsigned *)(GPIOB_BASE + 0x14u))
#define GPIOB_BSRR (*(volatile unsigned *)(GPIOB_BASE + 0x18u))

#define ITER 50000u

static volatile unsigned g_ram_reg;
static volatile unsigned g_sink;

static void pc_(char c)
{
    UART_THR = (unsigned char)c;
}

static void ps_(const char *s)
{
    while (*s != '\0') {
        pc_(*s++);
    }
}

static void pu_(unsigned v)
{
    char tmp[10];
    unsigned n = 0u;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        pc_(tmp[--n]);
    }
}

static void phex(unsigned v)
{
    int i;

    for (i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

/* ------------------------------------------------------------------ */
/* Kernels: one copy of each, reached with either target, so the two   */
/* forms are the same translated block.                                */
/* ------------------------------------------------------------------ */

#pragma noinline k_read, k_write, k_rmw, k_poll

static void k_read(volatile unsigned *p)
{
    unsigned i;

    for (i = 0u; i < ITER; i++) {
        g_sink = *p;
    }
}

static void k_write(volatile unsigned *p)
{
    unsigned i;

    for (i = 0u; i < ITER; i++) {
        *p = (1u << 16);
    }
}

static void k_rmw(volatile unsigned *p)
{
    unsigned i;

    for (i = 0u; i < ITER; i++) {
        *p = *p & ~0xFFu;
    }
}

/* Falls through on the first read: bit 31 of IDR is reserved, reads 0. */
static void k_poll(volatile unsigned *p)
{
    unsigned i;

    for (i = 0u; i < ITER; i++) {
        while ((*p & 0x80000000u) != 0u) {
        }
    }
}

static void report(const char *name, void (*fn)(volatile unsigned *),
                   volatile unsigned *target)
{
    const unsigned t0 = TIM2_CNT;
    unsigned t1;

    fn(target);
    t1 = TIM2_CNT;

    /* name, TIM2 ticks, and ticks per thousand accesses */
    ps_(name);
    pc_(' ');
    pu_(t1 - t0);
    pc_(' ');
    pu_((t1 - t0) / (ITER / 1000u));
    pc_('\n');
}

static unsigned g_checks, g_fails;

static void check(const char *name, unsigned got, unsigned want)
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

static void verify(void)
{
    unsigned pass;

    /* Repeated: the first pass may run interpreted. */
    for (pass = 0u; pass < 8u; pass++) {
        GPIOB_BSRR = (1u << 3);
        check("bsrr-set", (GPIOB_ODR >> 3) & 1u, 1u);
        GPIOB_BSRR = (1u << (3 + 16));
        check("bsrr-clear", (GPIOB_ODR >> 3) & 1u, 0u);
        /* A read-modify-write through the window keeps the other bits. */
        GPIOB_BSRR = (1u << 5);
        GPIOB_ODR = GPIOB_ODR | (1u << 7);
        check("odr-rmw", GPIOB_ODR & 0xA0u, 0xA0u);
        GPIOB_BSRR = (1u << (5 + 16)) | (1u << (7 + 16));
        check("odr-clear", GPIOB_ODR & 0xA0u, 0u);
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

    /* Again, once the window is inlined. */
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
