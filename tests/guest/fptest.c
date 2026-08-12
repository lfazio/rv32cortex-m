/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fptest.c - Floating-point guest that never arms PMP.
 *
 * It exists because isatest cannot measure floating point on the JIT.
 * isatest locks a PMP entry early, and the framework's `blocked` points
 * at h->fetch_guard, so everything after that goes to the interpreter
 * whatever the backend could have lowered -- measured on hardware at 32%
 * interpreted against bench's 3.2%. Three changes to the FP path in a
 * row produced board runs whose counters were identical to the digit,
 * and this is the guest that makes them visible.
 *
 * So: no PMP, no Sdtrig, no paging, no traps at all on the happy path.
 * The only privileged thing it does is turn the FP unit on, which it
 * must, and read minstret.
 *
 * It is self-checking as well as a benchmark, because a coverage number
 * from a guest computing wrong answers is worse than no number. Every
 * kernel accumulates into a checksum compared against a value the host
 * interpreter and the board must both produce -- and since the two
 * execute genuinely different float implementations under the JIT (host
 * SSE, ARM VFP, or SoftFloat with -DEMU_JIT=OFF), agreement between
 * them is a real differential check rather than a restatement.
 *
 * What each kernel is for:
 *
 *   arith    FADD/FSUB/FMUL/FDIV in a dependent chain -- the ops both
 *            backends lower natively, so this is the throughput case
 *   sgnj     FSGNJ/FSGNJN/FSGNJX, which are integer bit work on both
 *            hosts and should never reach the FP unit
 *   cmpsel   FEQ/FLT/FLE feeding branches; ARM declines these, so the
 *            same source measures a native lowering on x86-64 and the
 *            helper path on the board
 *   convert  FCVT both directions, including the inputs where the hosts
 *            and RISC-V disagree
 *   mixed    the loop a real workload looks like: loads, stores, and
 *            arithmetic interleaved with integer work
 */

#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#define csr_read(name) ({                               \
    uint32_t v_;                                        \
    __asm__ volatile ("csrr %0, " name : "=r"(v_));     \
    v_;                                                 \
})

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

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        const uint32_t d = (v >> i) & 0xFu;
        UART_THR = (uint8_t)(d < 10u ? ('0' + d) : ('a' + d - 10u));
    }
}

/*
 * The bit pattern of a float, without <string.h> and without a pointer
 * cast -- which at -O2 is a strict-aliasing violation gcc acts on.
 */
static uint32_t bits(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return c.u;
}

static float unbits(uint32_t u)
{
    union { float f; uint32_t u; } c;
    c.u = u;
    return c.f;
}

/* Checked by comparing bit patterns, so -0.0 and a NaN are not "equal". */
static uint32_t g_sum;

static void mix(uint32_t v)
{
    g_sum = (g_sum * 31u) + v;
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

/*
 * A dependent chain, so nothing here can be hoisted or vectorised and
 * every iteration pays a real FP latency. Kept away from the extremes
 * so it stays exact across implementations: what varies between hosts
 * is rounding of inexact results, and this is meant to measure
 * throughput rather than to catch that.
 */
static void arith(unsigned n)
{
    float a = 1.0f, b = 3.0f;

    for (unsigned i = 0; i < n; i++) {
        a = a + b;
        b = b * 1.5f;
        a = a - (b * 0.25f);
        b = b / 2.0f;
        if (b > 1.0e30f || b < 1.0e-30f) {
            b = 3.0f;                 /* keep it in range, branchlessly */
        }
    }
    mix(bits(a));
    mix(bits(b));
}

/*
 * Sign injection, which both backends do with integer instructions.
 * Driven with a NaN as well as ordinary values, because that is the
 * input that separates bit manipulation from anything routed through
 * the FP unit -- a quietened NaN would show up here and nowhere else.
 */
static void sgnj(unsigned n)
{
    const uint32_t k_in[4] = {
        0x3F800000u,        /* 1.0    */
        0xBF800000u,        /* -1.0   */
        0x7FC00000u,        /* qNaN   */
        0x80000000u,        /* -0.0   */
    };

    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = 0; j < 4u; j++) {
            const float x = unbits(k_in[j]);
            const float y = unbits(k_in[(j + 1u) & 3u]);

            /* The compiler emits FSGNJ for these three idioms. */
            mix(bits(__builtin_copysignf(x, y)));
            mix(bits(-x));
            mix(bits(__builtin_fabsf(x)));
        }
    }
}

/*
 * Comparisons feeding branches. x86-64 lowers these; ARM declines them
 * and takes the helper -- so the same source measures a native lowering
 * on one host and the fallback path on the other, which is exactly the
 * comparison the capability query exists to make.
 */
static void cmpsel(unsigned n)
{
    float acc = 0.0f;

    for (unsigned i = 0; i < n; i++) {
        const float x = (float)(int32_t)(i & 63u) - 32.0f;
        const float y = (float)(int32_t)(i & 15u) - 8.0f;

        if (x < y)        { acc += 1.0f; }
        if (x <= y)       { acc += 2.0f; }
        if (x == y)       { acc += 4.0f; }
        acc = (x > acc) ? acc : (acc * 0.5f);
    }
    mix(bits(acc));
}

/*
 * Conversion both ways, over inputs chosen so the answer differs
 * between a correct implementation and one that trusted the host: a
 * negative that saturates, and the values either side of the signed
 * range. NaN is deliberately absent -- it is checked in the unit tests,
 * and producing one here would need a division this loop cannot afford
 * to make inexact.
 */
static void convert(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const int32_t k = (int32_t)i - (int32_t)(n / 2u);
        const float f = (float)k;

        mix((uint32_t)(int32_t)f);
        mix(bits(f));
        mix((uint32_t)(float)(uint32_t)i);
    }
}

/*
 * The shape a real workload has: an FP array walked with integer index
 * arithmetic, loads and stores interleaved with the arithmetic. This is
 * the one whose block structure most resembles compiled code, and so
 * the one whose translated-versus-interpreted ratio means the most.
 */
#define MIX_N 256u
static float g_buf[MIX_N];

static void mixed(unsigned n)
{
    for (unsigned i = 0; i < MIX_N; i++) {
        g_buf[i] = (float)(int32_t)(i & 31u) + 0.5f;
    }
    for (unsigned pass = 0; pass < n; pass++) {
        float acc = 0.0f;

        for (unsigned i = 0; i < MIX_N; i++) {
            const float v = g_buf[i] * 1.25f;

            g_buf[i] = v - 0.125f;
            acc += v;
        }
        mix(bits(acc));
    }
}

static void report(const char *name)
{
    puts_("  ");
    puts_(name);
    puts_(" 0x");
    puthex(g_sum);
    puts_("\n");
    g_sum = 0u;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /*
     * mstatus.FS to Initial. Every FP instruction is illegal until this
     * happens, and the JIT's generation key carries FS off-ness, so
     * doing it here rather than in start.S also exercises the flush
     * that the change from off to on must cause.
     *
     * This is the *only* privileged register this guest touches. Adding
     * a PMP entry here would make it as blind as isatest.
     */
    __asm__ volatile ("csrs mstatus, %0" :: "r"(1u << 13));

    puts_("FPTEST-START\n");

    const uint32_t i0 = csr_read("minstret");

    /*
     * Reported per kernel, not just as a total. A single checksum says
     * two implementations disagree; it does not say where, and the
     * alternative is one board reflash per kernel to find out.
     */
    arith(2000u);   report("arith  ");
    sgnj(200u);     report("sgnj   ");
    cmpsel(2000u);  report("cmpsel ");
    convert(2000u); report("convert");
    mixed(50u);     report("mixed  ");

    const uint32_t i1 = csr_read("minstret");

    puts_("FPTEST-END\nguest instructions ");
    putu(i1 - i0);
    puts_("\nchecksum 0x");
    puthex(g_sum);
    puts_("\n");

    return 0;
}
