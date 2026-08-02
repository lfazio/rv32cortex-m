/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mmiobench.c - Driver-shaped guest workload for the passthrough window.
 *
 * bench.c and CoreMark are deliberately I/O free, so neither says anything
 * about the path a guest *driver* actually takes. That path is different:
 * guest RAM is inlined into translated code, while everything else -- the
 * peripheral window included -- leaves the block through a helper call, the
 * hart's load/store checks and a bus region walk. This is the workload that
 * measures the difference.
 *
 * Each kernel comes in two forms with an identical loop shape and an
 * identical guest instruction count, one against the peripheral window and
 * one against guest RAM. The RAM form is the control: dividing the two
 * removes the loop overhead and leaves the cost of the access path itself.
 *
 *   read   load a status register, the pattern behind every poll loop
 *   write  store to a command register
 *   rmw    read-modify-write, the pattern behind every bit twiddle
 *   poll   read, test a bit, branch -- a real driver's wait loop
 *
 * GPIOB is the target because nothing on a Nucleo-F446RE uses it: the
 * ST-LINK virtual console is USART2 on PA2/PA3 and SWD is PA13/PA14. Its
 * pins stay in their reset state (input, no pull), so BSRR and ODR writes
 * have no electrical effect, and IDR reads are harmless whatever is or is
 * not attached.
 *
 * The guest enables GPIOB's clock itself, through RCC AHB1ENR in the
 * passthrough window. That is not incidental -- it is the whole design
 * being exercised, since a guest driver that cannot ungate its own
 * peripheral is a guest driver that has to move into the firmware.
 *
 * On the host build the peripheral window is ordinary simulated RAM, so
 * both forms take the same path and the ratio is 1. The host run is a
 * correctness check; the measurement only means anything on hardware.
 */

#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

/* CLINT mtime, driven at 1 MHz on every platform, so ticks are microseconds. */
#define CLINT_MTIME (*(volatile uint32_t *)0x0200BFF8u)

#define csr_read(name) ({                               \
    uint32_t v_;                                        \
    __asm__ volatile ("csrr %0, " name : "=r"(v_));     \
    v_;                                                 \
})

#define csr_write(name, v) \
    __asm__ volatile ("csrw " name ", %0" :: "r"(v))

/* STM32F4 peripheral addresses, used verbatim from RM0390. */
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830u)
#define RCC_GPIOBEN (1u << 1)

#define GPIOB_BASE  0x40020400u
#define GPIOB_ODR   (*(volatile uint32_t *)(GPIOB_BASE + 0x14u))
#define GPIOB_BSRR  (*(volatile uint32_t *)(GPIOB_BASE + 0x18u))
#define GPIOB_IDR   (*(volatile uint32_t *)(GPIOB_BASE + 0x10u))

/* The control target: an ordinary guest-RAM word. */
static volatile uint32_t g_ram_reg;

#define ITER 50000u

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

static volatile uint32_t g_sink;

/* ------------------------------------------------------------------ */
/* Kernels.                                                             */
/*                                                                      */
/* Each takes the target as a pointer rather than naming it, so the two  */
/* forms of a comparison are not merely the same shape but the same      */
/* machine code, reached by the same translated block, differing only in */
/* the address in the register. An earlier version wrote the addresses   */
/* literally and the compiler folded the peripheral constant while       */
/* keeping a symbol reference for the RAM one, which made the control    */
/* one instruction per iteration cheaper than the thing it controlled.   */
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

/*
 * A wait loop that always falls through on the first read, so the access
 * count matches the others exactly. Testing a bit that reads as one would
 * spin forever on hardware and not at all on the host, which would make the
 * two builds measure different things. Bit 31 of GPIOB_IDR is reserved and
 * reads zero, and the RAM word is never set.
 */
static NOINLINE void k_poll(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < ITER; i++) {
        while ((*p & 0x80000000u) != 0u) { }
    }
}

/* ------------------------------------------------------------------ */

static void report(const char *name, void (*fn)(volatile uint32_t *),
                   volatile uint32_t *target)
{
    const uint32_t t0 = CLINT_MTIME;
    const uint32_t i0 = csr_read("minstret");

    fn(target);

    const uint32_t i1 = csr_read("minstret");
    const uint32_t t1 = CLINT_MTIME;

    /* name us insns ns-per-access, scaled to avoid floating point. */
    puts_(name);
    puts_(" ");
    putu(t1 - t0);
    puts_(" ");
    putu(i1 - i0);
    puts_(" ");
    putu(((t1 - t0) * 1000u) / ITER);
    puts_("\n");
}

/* ------------------------------------------------------------------ */
/* Correctness.                                                         */
/*                                                                      */
/* Speed is not the only thing that changes when an access stops going   */
/* through rv_hart_store: the emitted path has to reach the same address */
/* and refuse the same writes. The read-only sub-ranges are the part     */
/* worth testing hardest, because they are what stops a guest from       */
/* reconfiguring the PLL or erasing the flash the emulator is running    */
/* from, and a fast path that got them wrong would be silent.            */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_traps;
static volatile uint32_t g_last_cause;
static unsigned g_checks, g_fails;

/*
 * The "machine" interrupt attribute is what emits the register save/restore
 * and the closing mret; without it the handler ends in a plain ret and the
 * first fault stops the guest dead.
 */
__attribute__((interrupt("machine"), aligned(4), used))
static void trap_handler(void)
{
    g_last_cause = csr_read("mcause");
    g_traps++;

    /* Step over the faulting instruction; all of them here are 32-bit. */
    const uint32_t epc = csr_read("mepc");
    const uint16_t parcel = *(const volatile uint16_t *)epc;
    csr_write("mepc", epc + (((parcel & 3u) == 3u) ? 4u : 2u));
}

static void check(const char *name, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_fails++;
        puts_("FAIL ");
        puts_(name);
        puts_(" got ");
        putu(got);
        puts_(" want ");
        putu(want);
        puts_("\n");
    }
}

/*
 * The read-only clock-tree hole is 0x40023800..0x40023810: RCC's CR,
 * PLLCFGR, CFGR and CIR. Its edges are bracketed with the first and last
 * words inside it and the first word past it.
 *
 * Every hole in the policy table begins just above reserved address space,
 * so there is no register immediately *below* one to test with -- and
 * touching the reserved gap is not a harmless near miss. An unimplemented
 * peripheral address makes the AHB signal an error, which on this part is
 * a HardFault in the emulator rather than a fault delivered to the guest:
 * the firmware dies instead of the test failing. Probing the passthrough
 * window means probing registers that exist.
 */
#define RCC_CR       (*(volatile uint32_t *)0x40023800u)  /* first in hole */
#define RCC_CIR      (*(volatile uint32_t *)0x4002380Cu)  /* last in hole  */
#define ABOVE_HOLE   (*(volatile uint32_t *)0x40023810u)  /* AHB1RSTR      */
/* One past the end of the passthrough window; no region covers it, so the
 * helper faults without ever reaching the bus. */
#define OUTSIDE      (*(volatile uint32_t *)0x60000000u)

static void verify(void)
{
    /*
     * Repeated because a block is translated on first execution: the first
     * pass may run interpreted, and it is the emitted code that is on
     * trial here. Every pass must agree.
     */
    for (unsigned pass = 0; pass < 8u; pass++) {
        uint32_t before;

        /* Store then load, through real silicon and back. */
        GPIOB_BSRR = (1u << 3);
        check("bsrr-set", (GPIOB_ODR >> 3) & 1u, 1u);
        GPIOB_BSRR = (1u << (3 + 16));
        check("bsrr-clear", (GPIOB_ODR >> 3) & 1u, 0u);

        /* A hole is readable... */
        before = g_traps;
        const uint32_t cr = RCC_CR;
        check("hole-load-ok", g_traps - before, 0u);
        check("hole-load-hsi", cr & 1u, 1u);   /* HSION, always set */

        /*
         * ...and not writable. Writing back the value just read keeps the
         * failure mode harmless: if the emulator wrongly lets this through,
         * the hardware sees a write of the value it already holds.
         */
        before = g_traps;
        RCC_CR = cr;
        check("hole-store-traps", g_traps - before, 1u);
        check("hole-store-cause", g_last_cause, 7u);   /* store access fault */

        /* The last word of the hole is inside it too. */
        before = g_traps;
        RCC_CIR = 0u;
        check("hole-end-traps", g_traps - before, 1u);

        /* Immediately above it: permitted. Zero is AHB1RSTR's reset value. */
        before = g_traps;
        ABOVE_HOLE = 0u;
        check("above-hole-ok", g_traps - before, 0u);

        /* Outside the window: still a fault, not a wild store. */
        before = g_traps;
        OUTSIDE = 0u;
        check("outside-traps", g_traps - before, 1u);
    }
}

int main(void)
{
    csr_write("mtvec", (uint32_t)&trap_handler);

    /*
     * Ungate GPIOB. Reading back is not paranoia: on this part a write to a
     * clock-enable register needs a couple of cycles to take effect before
     * the peripheral answers, and the read provides them.
     */
    RCC_AHB1ENR |= RCC_GPIOBEN;
    g_sink = RCC_AHB1ENR;

    puts_("MMIOBENCH-START\n");

    verify();
    puts_("checks ");
    putu(g_checks);
    puts_(" failures ");
    putu(g_fails);
    puts_("\n");

    puts_("kernel us insns ns_per_access\n");

    /* BSRR for stores and IDR for loads: neither disturbs pin state. */
    report("read-mmio ", k_read,  &GPIOB_IDR);
    report("read-ram  ", k_read,  &g_ram_reg);
    report("write-mmio", k_write, &GPIOB_BSRR);
    report("write-ram ", k_write, &g_ram_reg);
    report("rmw-mmio  ", k_rmw,   &GPIOB_ODR);
    report("rmw-ram   ", k_rmw,   &g_ram_reg);
    report("poll-mmio ", k_poll,  &GPIOB_IDR);
    report("poll-ram  ", k_poll,  &g_ram_reg);

    puts_("MMIOBENCH-END\n");
    return 0;
}
