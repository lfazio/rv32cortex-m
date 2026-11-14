/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vecbench.c - integer vector kernels, for what the JIT does with the
 * vector unit (Zve32x, -DRV32_EXT_ZVE32X=ON; docs/frontend/rv32-vector.md).
 *
 * Written with the RVV intrinsics rather than left to the vectoriser, so
 * the vector code is the same whatever GCC's cost model decides: each
 * kernel is a strip-mined loop -- vsetvli at its head, vl elements a
 * trip -- as compiled vector code is. They cover what such loops spend
 * their time in:
 *
 *   add     vle32, vadd.vv, vse32 at LMUL 2
 *   axpy    vmacc.vx at LMUL 4
 *   dot     a widening multiply-accumulate, 16 into 32 bits, and vredsum
 *   copy    vle8 / vse8 at LMUL 8: a memcpy
 *   avg     vaaddu on bytes, rounding to nearest-up (vxrm)
 *   clip    vnclip from 32 to 16 bits with saturation (vxsat)
 *   max     vmax.vv and vredmax
 *   gather  vlse32, a strided load, and vmul
 *
 * Every kernel's result goes into one checksum, printed with minstret's
 * count between the markers; the host divides its cycles by that. A
 * trap prints its cause and pc and exits 1 rather than spinning.
 */

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#ifndef VECBENCH_ROUNDS
#define VECBENCH_ROUNDS 40
#endif

#define N 1024

#define csr_read(name)                                                         \
    ({                                                                         \
        uint32_t v_;                                                           \
        __asm__ volatile("csrr %0, " name : "=r"(v_));                         \
        v_;                                                                    \
    })

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

static __attribute__((noreturn)) void exit_(int code)
{
    register int a0 __asm__("a0") = code;
    register int a7 __asm__("a7") = 93;

    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    for (;;) {
    }
}

/* Any trap: its cause and where, then out. */
static __attribute__((interrupt("machine"), aligned(4))) void on_trap(void)
{
    puts_("VECBENCH-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_(" mtval ");
    puthex(csr_read("mtval"));
    puts_("\n");
    exit_(1);
}

static int32_t g_a[N], g_b[N], g_c[N];
static int16_t g_h[N], g_k[N];
static uint8_t g_s[4 * N], g_d[4 * N];

static uint32_t g_seed = 0x2545F491u;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static uint32_t mix(uint32_t acc, uint32_t v)
{
    return ((acc << 5) | (acc >> 27)) ^ v;
}

static uint32_t sum_words(const void *p, size_t bytes)
{
    const uint32_t *w = p;
    uint32_t acc = 0u;

    for (size_t i = 0; i < bytes / 4u; i++) {
        acc = mix(acc, w[i]);
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

static void k_add(int32_t *c, const int32_t *a, const int32_t *b, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);
        const vint32m2_t x = __riscv_vle32_v_i32m2(a, vl);
        const vint32m2_t y = __riscv_vle32_v_i32m2(b, vl);

        __riscv_vse32_v_i32m2(c, __riscv_vadd_vv_i32m2(x, y, vl), vl);
        a += vl;
        b += vl;
        c += vl;
        n -= vl;
    }
}

static void k_axpy(int32_t *y, const int32_t *x, int32_t k, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m4(n);
        vint32m4_t acc = __riscv_vle32_v_i32m4(y, vl);

        acc = __riscv_vmacc_vx_i32m4(acc, k, __riscv_vle32_v_i32m4(x, vl), vl);
        __riscv_vse32_v_i32m4(y, acc, vl);
        x += vl;
        y += vl;
        n -= vl;
    }
}

static int32_t k_dot(const int16_t *a, const int16_t *b, size_t n)
{
    const size_t vlmax = __riscv_vsetvlmax_e32m2();
    vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);

    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e16m1(n);

        acc = __riscv_vwmacc_vv_i32m2_tu(acc, __riscv_vle16_v_i16m1(a, vl),
                                         __riscv_vle16_v_i16m1(b, vl), vl);
        a += vl;
        b += vl;
        n -= vl;
    }
    const vint32m1_t z = __riscv_vmv_s_x_i32m1(0, 1);

    return __riscv_vmv_x_s_i32m1_i32(
        __riscv_vredsum_vs_i32m2_i32m1(acc, z, vlmax));
}

static void k_copy(uint8_t *d, const uint8_t *s, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e8m8(n);

        __riscv_vse8_v_u8m8(d, __riscv_vle8_v_u8m8(s, vl), vl);
        d += vl;
        s += vl;
        n -= vl;
    }
}

static void k_avg(uint8_t *d, const uint8_t *a, const uint8_t *b, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e8m2(n);
        const vuint8m2_t x = __riscv_vle8_v_u8m2(a, vl);
        const vuint8m2_t y = __riscv_vle8_v_u8m2(b, vl);

        __riscv_vse8_v_u8m2(d, __riscv_vaaddu_vv_u8m2(x, y, __RISCV_VXRM_RNU, vl),
                            vl);
        a += vl;
        b += vl;
        d += vl;
        n -= vl;
    }
}

static void k_clip(int16_t *d, const int32_t *a, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e16m1(n);
        const vint32m2_t x = __riscv_vle32_v_i32m2(a, vl);

        __riscv_vse16_v_i16m1(d, __riscv_vnclip_wx_i16m1(x, 4, __RISCV_VXRM_RNU, vl),
                              vl);
        a += vl;
        d += vl;
        n -= vl;
    }
}

static int32_t k_max(const int32_t *a, const int32_t *b, size_t n)
{
    const size_t vlmax = __riscv_vsetvlmax_e32m4();
    vint32m4_t m = __riscv_vmv_v_x_i32m4(INT32_MIN, vlmax);

    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m4(n);
        const vint32m4_t x = __riscv_vmax_vv_i32m4(__riscv_vle32_v_i32m4(a, vl),
                                                   __riscv_vle32_v_i32m4(b, vl), vl);

        m = __riscv_vmax_vv_i32m4_tu(m, m, x, vl);
        a += vl;
        b += vl;
        n -= vl;
    }
    const vint32m1_t z = __riscv_vmv_s_x_i32m1(INT32_MIN, 1);

    return __riscv_vmv_x_s_i32m1_i32(__riscv_vredmax_vs_i32m4_i32m1(m, z, vlmax));
}

/* Every fourth word of a, times b: a strided load. */
static void k_gather(int32_t *c, const int32_t *a, const int32_t *b, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);
        const vint32m2_t x =
            __riscv_vlse32_v_i32m2(a, 4 * (ptrdiff_t)sizeof(int32_t), vl);

        __riscv_vse32_v_i32m2(c, __riscv_vmul_vv_i32m2(x, __riscv_vle32_v_i32m2(b, vl), vl),
                              vl);
        a += 4u * vl;
        b += vl;
        c += vl;
        n -= vl;
    }
}

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));

    for (size_t i = 0; i < N; i++) {
        g_a[i] = (int32_t)rnd();
        g_b[i] = (int32_t)(rnd() >> 8);
        g_h[i] = (int16_t)rnd();
        g_k[i] = (int16_t)rnd();
    }
    for (size_t i = 0; i < 4u * N; i++) {
        g_s[i] = (uint8_t)rnd();
    }

    puts_("VECBENCH-START\n");
    const uint32_t i0 = csr_read("minstret");
    uint32_t sum = 0u;

    for (int r = 0; r < VECBENCH_ROUNDS; r++) {
        k_add(g_c, g_a, g_b, N - (size_t)r);
        sum = mix(sum, sum_words(g_c, sizeof(g_c)));
        k_axpy(g_c, g_a, 3 - r, N);
        sum = mix(sum, (uint32_t)k_dot(g_h, g_k, N - 3u * (size_t)r));
        k_copy(g_d, g_s, 4u * N - (size_t)r);
        k_avg(g_d, g_d, g_s + 1, 4u * N - 1u);
        k_clip(g_h, g_c, N);
        sum = mix(sum, (uint32_t)k_max(g_c, g_a, N - (size_t)r));
        k_gather(g_c, g_a, g_b, N / 4u);
    }
    sum = mix(sum, sum_words(g_c, sizeof(g_c)));
    sum = mix(sum, sum_words(g_h, sizeof(g_h)));
    sum = mix(sum, sum_words(g_d, sizeof(g_d)));
    sum = mix(sum, csr_read("vxsat"));

    const uint32_t i1 = csr_read("minstret");

    puts_("VECBENCH-END\nsum ");
    puthex(sum);
    puts_("\nguest instructions ");
    putu(i1 - i0);
    puts_("\n");
    return 0;
}
