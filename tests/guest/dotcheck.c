/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dotcheck.c - the 16-bit dot-product kernels against a reference, shape
 * by shape.
 *
 * dot() is CoreMark's loop -- x stepping by a halfword, y by a register
 * stride, the products summed in 32 bits -- which the JIT turns into a
 * kernel: the C routine on x86-64, a loop emitted in the block on
 * Thumb-2, eight lanes at a time under Helium on the N6. dot_ref() is
 * the same sum in 64 bits, which no kernel takes, and its high half is
 * printed as well so the compiler cannot narrow it back into dot().
 *
 * The cases are where a vector loop can be wrong: lengths below, at and
 * around multiples of 8 (the remainder the scalar loop finishes); strides
 * from 1 to past the gather's reach (7 * 2 * ys above 65535 bytes: the
 * scalar loop again); x and y starting on an odd halfword; and values
 * at both ends of int16_t, so products and sums wrap.
 *
 * Prints one FAIL line a difference, then the count and DOTCHECK-END.
 */

#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#ifndef DOT_ELEMS
#define DOT_ELEMS 8192u /* y's array, in halfwords: the build sizes it */
#endif

static int16_t xa[72];
static int16_t ya[DOT_ELEMS];

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        UART_THR = (uint8_t)"0123456789abcdef"[(v >> i) & 15u];
    }
}

/* The kernel's shape. */
__attribute__((noinline)) static int32_t dot(const int16_t *x, const int16_t *y, uint32_t n,
                                             uint32_t s)
{
    int32_t sum = 0;

    for (uint32_t i = 0; i < n; i++) {
        sum += x[i] * y[i * s];
    }
    return sum;
}

/* The reference: 64 bits, which no kernel takes. */
__attribute__((noinline)) static int64_t dot_ref(const int16_t *x, const int16_t *y, uint32_t n,
                                                 uint32_t s)
{
    int64_t sum = 0;

    for (uint32_t i = 0; i < n; i++) {
        sum += (int64_t)(x[i] * y[i * s]);
    }
    return sum;
}

static uint32_t g_seed = 0x2545F491u;

static int16_t next(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    switch (g_seed >> 29) {
    case 0:
        return 32767;
    case 1:
        return -32768;
    default:
        return (int16_t)(g_seed >> 8);
    }
}

int main(void)
{
    static const uint32_t lens[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 63, 64};
    static const uint32_t strides[] = {1, 2, 3, 7, 13, 16, 100, 1000, 4681, 4682, 9000};
    uint32_t cases = 0, fails = 0, high = 0;

    for (uint32_t i = 0; i < sizeof(xa) / sizeof(xa[0]); i++) {
        xa[i] = next();
    }
    for (uint32_t i = 0; i < DOT_ELEMS; i++) {
        ya[i] = next();
    }
    /* Each case three times: the third runs translated, kernel and all. */
    for (uint32_t rep = 0; rep < 3u; rep++) {
        for (uint32_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            for (uint32_t si = 0; si < sizeof(strides) / sizeof(strides[0]); si++) {
                for (uint32_t off = 0; off < 2u; off++) {
                    const uint32_t n = lens[li], s = strides[si];

                    if ((n - 1u) * s + 1u + off > DOT_ELEMS) {
                        continue;
                    }
                    const int32_t got = dot(xa + off, ya + off, n, s);
                    const int64_t want = dot_ref(xa + off, ya + off, n, s);

                    cases++;
                    high ^= (uint32_t)((uint64_t)want >> 32);
                    if ((uint32_t)got != (uint32_t)want) {
                        fails++;
                        puts_("FAIL n ");
                        puthex(n);
                        puts_(" s ");
                        puthex(s);
                        puts_(" off ");
                        puthex(off);
                        puts_(" got ");
                        puthex((uint32_t)got);
                        puts_(" want ");
                        puthex((uint32_t)want);
                        puts_("\n");
                    }
                }
            }
        }
    }
    puts_("DOTCHECK cases ");
    puthex(cases);
    puts_(" fails ");
    puthex(fails);
    puts_(" high ");
    puthex(high);
    puts_("\nDOTCHECK-END\n");
    return 0;
}
