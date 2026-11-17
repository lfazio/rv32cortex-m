/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vfbench.c - single-precision vector kernels (Zve32f), for what the JIT
 * does with the vector unit's floating point; vecbench.c's integer
 * counterpart, built the same way: the RVV intrinsics, strip-mined loops
 * a vsetvli at the head.
 *
 *   saxpy    vfmacc.vf at LMUL 4
 *   dot      vfmacc.vv, then vfredusum
 *   arith    vfadd.vv, vfmul.vv, vfsub.vf at LMUL 2
 *   norm     x / sqrt(x * x + 1): vfmul, vfadd.vf, vfsqrt, vfdiv
 *   clamp    vfmax.vf, vfmin.vf
 *   horner   a cubic by vfmadd.vf
 *   convert  vfcvt.x.f.v and back
 *
 * The results go into one checksum with fflags, printed with minstret's
 * count between the markers. A trap prints its cause and exits 1.
 */

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#ifndef VFBENCH_ROUNDS
#define VFBENCH_ROUNDS 20
#endif

#define N 512

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

static __attribute__((interrupt("machine"), aligned(4))) void on_trap(void)
{
    puts_("VFBENCH-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_("\n");
    exit_(1);
}

static float g_x[N], g_y[N], g_z[N];
static int32_t g_i[N];

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
    return (((acc << 5) | (acc >> 27)) ^ v) * 0x9E3779B1u;
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

static uint32_t bits_of(float f)
{
    uint32_t b;

    __builtin_memcpy(&b, &f, 4);
    return b;
}

static void k_saxpy(float *y, const float *x, float a, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m4(n);
        vfloat32m4_t acc = __riscv_vle32_v_f32m4(y, vl);

        acc = __riscv_vfmacc_vf_f32m4(acc, a, __riscv_vle32_v_f32m4(x, vl), vl);
        __riscv_vse32_v_f32m4(y, acc, vl);
        x += vl;
        y += vl;
        n -= vl;
    }
}

static float k_dot(const float *x, const float *y, size_t n)
{
    const size_t vlmax = __riscv_vsetvlmax_e32m2();
    vfloat32m2_t acc = __riscv_vfmv_v_f_f32m2(0.0f, vlmax);

    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);

        acc = __riscv_vfmacc_vv_f32m2_tu(acc, __riscv_vle32_v_f32m2(x, vl),
                                         __riscv_vle32_v_f32m2(y, vl), vl);
        x += vl;
        y += vl;
        n -= vl;
    }
    const vfloat32m1_t z = __riscv_vfmv_s_f_f32m1(0.0f, 1);

    return __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredusum_vs_f32m2_f32m1(acc, z, vlmax));
}

static void k_arith(float *z, const float *x, const float *y, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);
        const vfloat32m2_t a = __riscv_vle32_v_f32m2(x, vl);
        const vfloat32m2_t b = __riscv_vle32_v_f32m2(y, vl);
        vfloat32m2_t c = __riscv_vfadd_vv_f32m2(a, b, vl);

        c = __riscv_vfmul_vv_f32m2(c, b, vl);
        c = __riscv_vfsub_vf_f32m2(c, 0.5f, vl);
        __riscv_vse32_v_f32m2(z, c, vl);
        x += vl;
        y += vl;
        z += vl;
        n -= vl;
    }
}

static void k_norm(float *z, const float *x, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);
        const vfloat32m2_t a = __riscv_vle32_v_f32m2(x, vl);
        const vfloat32m2_t d =
            __riscv_vfsqrt_v_f32m2(__riscv_vfadd_vf_f32m2(__riscv_vfmul_vv_f32m2(a, a, vl),
                                                          1.0f, vl),
                                   vl);

        __riscv_vse32_v_f32m2(z, __riscv_vfdiv_vv_f32m2(a, d, vl), vl);
        x += vl;
        z += vl;
        n -= vl;
    }
}

static void k_clamp(float *z, const float *x, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m4(n);
        vfloat32m4_t a = __riscv_vle32_v_f32m4(x, vl);

        a = __riscv_vfmax_vf_f32m4(a, -0.75f, vl);
        a = __riscv_vfmin_vf_f32m4(a, 0.75f, vl);
        __riscv_vse32_v_f32m4(z, a, vl);
        x += vl;
        z += vl;
        n -= vl;
    }
}

static void k_horner(float *z, const float *x, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m2(n);
        const vfloat32m2_t a = __riscv_vle32_v_f32m2(x, vl);
        vfloat32m2_t p = __riscv_vfmv_v_f_f32m2(0.25f, vl);

        p = __riscv_vfmadd_vv_f32m2(p, a, __riscv_vfmv_v_f_f32m2(-0.5f, vl), vl);
        p = __riscv_vfmadd_vv_f32m2(p, a, __riscv_vfmv_v_f_f32m2(1.0f, vl), vl);
        p = __riscv_vfmadd_vv_f32m2(p, a, __riscv_vfmv_v_f_f32m2(0.125f, vl), vl);
        __riscv_vse32_v_f32m2(z, p, vl);
        x += vl;
        z += vl;
        n -= vl;
    }
}

static void k_convert(float *z, int32_t *iv, const float *x, size_t n)
{
    while (n > 0u) {
        const size_t vl = __riscv_vsetvl_e32m4(n);
        const vint32m4_t i =
            __riscv_vfcvt_x_f_v_i32m4(__riscv_vfmul_vf_f32m4(__riscv_vle32_v_f32m4(x, vl),
                                                             1000.0f, vl),
                                      vl);

        __riscv_vse32_v_i32m4(iv, i, vl);
        __riscv_vse32_v_f32m4(z, __riscv_vfcvt_f_x_v_f32m4(i, vl), vl);
        x += vl;
        z += vl;
        iv += vl;
        n -= vl;
    }
}

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x2000u)); /* FS on */

    for (size_t i = 0; i < N; i++) {
        /* in [-2, 2), and some exact */
        g_x[i] = (float)((int32_t)(rnd() >> 8) - (1 << 23)) / (float)(1 << 22);
        g_y[i] = (float)((int32_t)(rnd() >> 12) - (1 << 19)) / (float)(1 << 18);
    }

    puts_("VFBENCH-START\n");
    const uint32_t i0 = csr_read("minstret");
    uint32_t sum = 0u;

    for (int r = 0; r < VFBENCH_ROUNDS; r++) {
        const size_t q = (size_t)(r % 64); /* lengths' shortfall: any round count */

        k_saxpy(g_y, g_x, 0.5f - (float)q * 0.03125f, N - q);
        sum = mix(sum, bits_of(k_dot(g_x, g_y, N - 3u * q)));
        k_arith(g_z, g_x, g_y, N - q);
        sum = mix(sum, sum_words(g_z, sizeof(g_z)));
        k_norm(g_z, g_y, N);
        sum = mix(sum, sum_words(g_z, sizeof(g_z)));
        k_clamp(g_z, g_z, N - q);
        k_horner(g_y, g_z, N);
        k_convert(g_z, g_i, g_y, N - 2u * q);
        sum = mix(sum, sum_words(g_z, sizeof(g_z)));
        sum = mix(sum, sum_words(g_i, sizeof(g_i)));
    }
    sum = mix(sum, sum_words(g_y, sizeof(g_y)));
    sum = mix(sum, csr_read("fflags"));

    const uint32_t i1 = csr_read("minstret");

    puts_("VFBENCH-END\nsum ");
    puthex(sum);
    puts_("\nguest instructions ");
    putu(i1 - i0);
    puts_("\n");
    return 0;
}
