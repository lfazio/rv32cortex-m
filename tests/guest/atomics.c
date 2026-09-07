/* SPDX-License-Identifier: Apache-2.0 */
/*
 * atomics.c - the A extension, as a guest that reaches the JIT.
 *
 * Why this guest exists
 * ---------------------
 * `isatest` already covers the atomics and cannot guard the thing that
 * broke twice. It arms PMP early, and `blocked = &h->fetch_guard` turns
 * the JIT off from that point on -- so every atomic in it is
 * *interpreted*, whatever the translator can or cannot do. The riscv-tests
 * rv32ua binaries have the same problem from the other end: they drop to
 * U-mode, which arms the same guard.
 *
 * This guest therefore does one thing neither of those does: it stays in
 * M-mode and arms nothing, so its instructions are actually translated.
 *
 * **Correctness alone would not protect the lowering, and that is the
 * whole point.** Both backends call the same rv_hart_amo, so an atomic
 * that stops being translated still computes the right answer -- it just
 * ends the block and interprets. Declining is a correctness-preserving
 * way to have no JIT, and no test of correctness can see it. So the
 * answers are checked here, and the *coverage* is checked by the ctest
 * that runs this with --jit and reads the `declined` counter: the loop at
 * the end issues ATOMIC_ROUNDS * 3 atomics, so a lowering that regressed
 * to declining them cannot come in under the bound.
 *
 * The checks are chosen for the inputs that discriminate, not for
 * representative ones:
 *
 *   - signed against unsigned min/max with -1 and 1, the one pair where
 *     the two orderings disagree. Any positive pair passes both.
 *   - SC with no reservation, and SC to an address other than the LR's:
 *     both must fail. An implementation that ignores the reservation
 *     entirely passes every test that only does LR-then-SC.
 *   - rd aliasing rs2, where the old value has to be read before the
 *     destination is written.
 *   - rd == x0, where the result is discarded and x0 must stay zero.
 */

#include <stdbool.h>
#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        const uint32_t d = (v >> i) & 0xFu;

        UART_THR = (uint8_t)(d < 10u ? ('0' + d) : ('a' + d - 10u));
    }
}

static uint32_t g_fail;

static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        g_fail++;
        puts_("  FAIL ");
        puts_(name);
        puts_(" got 0x");
        puthex(got);
        puts_(" want 0x");
        puthex(want);
        puts_("\n");
    } else {
        puts_("  ok   ");
        puts_(name);
        puts_("\n");
    }
}

/*
 * The cell every operation works on. Volatile because the compiler must
 * not cache it across the inline asm, and static so its address is a
 * plain global rather than something on the stack -- an AMO to a stack
 * slot would still be a valid test, but a fixed address makes a failure
 * easier to read in a trace.
 */
static volatile uint32_t g_cell;

/* Each AMO, written out rather than generated, so the encoding under test
 * is the one the mnemonic names. `old` is what the instruction returns. */
#define AMO(op, addr, src, old)                                                \
    __asm__ volatile(op " %0, %2, (%1)"                                        \
                     : "=&r"(old)                                              \
                     : "r"(addr), "r"(src)                                     \
                     : "memory")

static void test_amo_values(void)
{
    uint32_t old;

    g_cell = 100u;
    AMO("amoadd.w", &g_cell, 5u, old);
    check("amoadd-old", old, 100u);
    check("amoadd-new", g_cell, 105u);

    g_cell = 0xF0F0F0F0u;
    AMO("amoand.w", &g_cell, 0xFF00FF00u, old);
    check("amoand", g_cell, 0xF000F000u);

    g_cell = 0xF0F0F0F0u;
    AMO("amoor.w", &g_cell, 0x0F0F0F0Fu, old);
    check("amoor", g_cell, 0xFFFFFFFFu);

    g_cell = 0xFFFF0000u;
    AMO("amoxor.w", &g_cell, 0xFFFFFFFFu, old);
    check("amoxor", g_cell, 0x0000FFFFu);

    g_cell = 0xDEADBEEFu;
    AMO("amoswap.w", &g_cell, 0x12345678u, old);
    check("amoswap-old", old, 0xDEADBEEFu);
    check("amoswap-new", g_cell, 0x12345678u);
}

/*
 * Signed against unsigned, on the one pair that tells them apart.
 *
 * -1 and 1: signed says -1 is the smaller, unsigned says 1 is. Every
 * pair of positive values agrees, so a test using one would pass against
 * an implementation that had the two confused -- which is the mistake
 * available here, since the four instructions differ by one bit.
 */
static void test_amo_signedness(void)
{
    uint32_t old;

    g_cell = 0xFFFFFFFFu; /* -1 */
    AMO("amomax.w", &g_cell, 1u, old);
    check("amomax-signed", g_cell, 1u); /* max(-1, 1) = 1 */

    g_cell = 0xFFFFFFFFu;
    AMO("amomaxu.w", &g_cell, 1u, old);
    check("amomaxu", g_cell, 0xFFFFFFFFu); /* max(4294967295, 1) */

    g_cell = 0xFFFFFFFFu;
    AMO("amomin.w", &g_cell, 1u, old);
    check("amomin-signed", g_cell, 0xFFFFFFFFu); /* min(-1, 1) = -1 */

    g_cell = 0xFFFFFFFFu;
    AMO("amominu.w", &g_cell, 1u, old);
    check("amominu", g_cell, 1u); /* min(4294967295, 1) = 1 */
}

/*
 * rd aliasing rs2, and rd == x0.
 *
 * `amoadd.w a0, a0, (a1)` names one register as both the addend and the
 * destination, so the old value has to be read before the destination is
 * written -- the same shape as `jalr ra, ra`, which this project has
 * already been caught by. And an AMO with rd == x0 still performs the
 * memory operation; only the result is discarded.
 */
static void test_amo_operand_aliasing(void)
{
    register uint32_t v __asm__("a0") = 7u;
    volatile uint32_t *const p = &g_cell;

    g_cell = 30u;
    __asm__ volatile("amoadd.w a0, a0, (%1)"
                     : "+r"(v)
                     : "r"(p)
                     : "memory");
    check("amoadd-rd-is-rs2-old", v, 30u); /* the old value, not 37 */
    check("amoadd-rd-is-rs2-mem", g_cell, 37u);

    g_cell = 11u;
    __asm__ volatile("amoswap.w x0, %0, (%1)"
                     :
                     : "r"(99u), "r"(p)
                     : "memory");
    check("amoswap-rd-x0", g_cell, 99u);
}

/*
 * LR/SC, and the two cases that discriminate.
 *
 * An implementation that ignores the reservation altogether passes any
 * test that only does LR then SC, because that one is meant to succeed.
 * What it cannot pass is an SC with no reservation at all, or an SC to a
 * different address from the LR -- both of which must fail and leave
 * memory alone.
 */
static void test_lr_sc(void)
{
    static volatile uint32_t other;
    uint32_t v, ok;

    g_cell = 42u;
    __asm__ volatile("lr.w %0, (%1)" : "=r"(v) : "r"(&g_cell) : "memory");
    check("lr-value", v, 42u);
    __asm__ volatile("sc.w %0, %2, (%1)"
                     : "=r"(ok)
                     : "r"(&g_cell), "r"(43u)
                     : "memory");
    check("sc-succeeds", ok, 0u); /* zero is success */
    check("sc-wrote", g_cell, 43u);

    /*
     * An SC on its own. The reservation was consumed by the SC above, so
     * this one has nothing to hold and must fail without writing.
     */
    g_cell = 50u;
    __asm__ volatile("sc.w %0, %2, (%1)"
                     : "=r"(ok)
                     : "r"(&g_cell), "r"(51u)
                     : "memory");
    check("sc-without-lr-fails", (ok != 0u) ? 1u : 0u, 1u);
    check("sc-without-lr-no-write", g_cell, 50u);

    /*
     * A reservation on one address does not license a store to another.
     */
    other = 60u;
    g_cell = 70u;
    __asm__ volatile("lr.w %0, (%1)" : "=r"(v) : "r"(&g_cell) : "memory");
    __asm__ volatile("sc.w %0, %2, (%1)"
                     : "=r"(ok)
                     : "r"(&other), "r"(61u)
                     : "memory");
    check("sc-wrong-address-fails", (ok != 0u) ? 1u : 0u, 1u);
    check("sc-wrong-address-no-write", other, 60u);
}

/*
 * The measured part, and the reason the ctest can check coverage.
 *
 * Three atomics per round, so a translator that declined them again would
 * report at least 3 * ATOMIC_ROUNDS interpreted instructions -- far past
 * the bound the JIT test allows. The checksum keeps the loop alive
 * against a compiler that would otherwise see the results are unused.
 */
#ifndef ATOMIC_ROUNDS
#define ATOMIC_ROUNDS 2000u
#endif

static uint32_t bench(void)
{
    uint32_t sum = 0u;
    uint32_t old;

    g_cell = 0u;
    for (uint32_t i = 0; i < ATOMIC_ROUNDS; i++) {
        AMO("amoadd.w", &g_cell, 1u, old);
        sum += old;
        AMO("amoxor.w", &g_cell, i, old);
        sum ^= old;
        AMO("amoswap.w", &g_cell, i, old);
        sum += old;
    }
    return sum;
}

int main(void)
{
    puts_("\nATOMICS-START\n");

    test_amo_values();
    test_amo_signedness();
    test_amo_operand_aliasing();
    test_lr_sc();

    const uint32_t sum = bench();

    puts_("  rounds   0x");
    puthex(ATOMIC_ROUNDS);
    puts_("\n  checksum 0x");
    puthex(sum);
    puts_("\n  failures 0x");
    puthex(g_fail);
    puts_("\nATOMICS-END\n");

    return (int)g_fail;
}
