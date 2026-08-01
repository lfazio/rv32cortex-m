/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bench.c - Compute-bound guest workload for measuring emulator throughput.
 *
 * Everything here is deliberately I/O free between the start and end
 * markers. The earlier measurements taken from the self-test and the
 * driver demo were meaningless as throughput numbers, because both spent
 * almost all of their time blocked on a 115200-baud UART or spinning in a
 * delay loop; what they measured was the console, not the interpreter.
 *
 * The kernels are chosen to exercise the paths that cost an interpreter
 * differently:
 *
 *   fib      call/return and branch prediction, almost no memory traffic
 *   sieve    byte loads and stores over a large working set
 *   sort     unpredictable branches and swaps
 *   crc32    shift/xor chains with a table lookup per byte
 *   muldiv   the M extension, whose helpers are the slowest ALU ops
 *
 * The guest reports instructions retired from minstret; the host reports
 * the cycles it took. Dividing gives host cycles per guest instruction,
 * which is the number that actually characterises the interpreter.
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

/* Keep the compiler from folding away results we never otherwise use. */
static volatile uint32_t g_sink;

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

static uint32_t fib(uint32_t n)
{
    return (n < 2u) ? n : fib(n - 1u) + fib(n - 2u);
}

#define SIEVE_N 4096
static uint8_t g_sieve[SIEVE_N];

static uint32_t sieve(void)
{
    uint32_t count = 0;

    for (int i = 0; i < SIEVE_N; i++) {
        g_sieve[i] = 1u;
    }
    for (int i = 2; i < SIEVE_N; i++) {
        if (g_sieve[i]) {
            count++;
            for (int j = i + i; j < SIEVE_N; j += i) {
                g_sieve[j] = 0u;
            }
        }
    }
    return count;
}

#define SORT_N 512
static uint32_t g_sort[SORT_N];

static uint32_t sort_kernel(void)
{
    uint32_t seed = 12345u;

    for (int i = 0; i < SORT_N; i++) {
        /* Cheap LCG: the point is unpredictable data, not randomness. */
        seed = seed * 1103515245u + 12345u;
        g_sort[i] = seed >> 16;
    }
    /* Insertion sort: O(n^2) with a data-dependent inner loop. */
    for (int i = 1; i < SORT_N; i++) {
        const uint32_t key = g_sort[i];
        int j = i - 1;
        while (j >= 0 && g_sort[j] > key) {
            g_sort[j + 1] = g_sort[j];
            j--;
        }
        g_sort[j + 1] = key;
    }
    return g_sort[SORT_N / 2];
}

static uint32_t crc32_buf(const uint8_t *buf, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            /* Bitwise form on purpose: no table, so this is pure ALU work. */
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t muldiv_kernel(uint32_t iters)
{
    uint32_t acc = 1u;

    for (uint32_t i = 1; i <= iters; i++) {
        acc = acc * 2654435761u + i;
        acc ^= acc >> 13;
        acc = acc / (i | 1u);
        acc += (uint32_t)((int32_t)acc % (int32_t)((i & 0xFFu) | 1u));
    }
    return acc;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    puts_("BENCH-START\n");

    const uint32_t i0 = csr_read("minstret");

    g_sink = fib(21);
    g_sink = sieve();
    g_sink = sort_kernel();
    g_sink = crc32_buf(g_sieve, SIEVE_N);
    g_sink = muldiv_kernel(20000);

    const uint32_t i1 = csr_read("minstret");

    puts_("BENCH-END\nguest instructions ");
    putu(i1 - i0);
    puts_("\n");

    return 0;
}
