/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vecfp.c - the vector unit's single precision (Zve32f) on special values
 * -- zeros of both signs, subnormals, infinities, NaNs, the integer
 * ranges' edges -- under each rounding mode: the arithmetic, the fused
 * multiply-adds, min and max, the compares, the conversions both ways
 * and at SEW 16, the reductions and the two estimates, with fflags
 * folded after each.
 *
 * The probes hold rv_vec.c to Spike on the host; this is for the board,
 * where the same C runs on an M7 -- SoftFloat, and the comparisons in
 * host floats that min, max and the compares make, which a flush-to-zero
 * FPU would answer differently for a subnormal. One checksum, the same
 * interpreted, translated on the host and on the F746.
 */

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

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
    puts_("VECFP-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_("\n");
    exit_(1);
}

#define N 32

static const uint32_t k_a[N] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3FC00000, 0x40000000,
    0x3F000000, 0x7F7FFFFF, 0x00800000, 0x00000001, 0x7F800000, 0xFF800000,
    0x7FC00000, 0x7F800001, 0x7FC12345, 0x3EAAAAAB, 0x46FFFE00, 0xC7000000,
    0x47000000, 0x477FFF00, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F7FFFFF,
    0x40490FDB, 0xC0490FDB, 0x007FFFFF, 0x80000001, 0x00400000, 0x80400000,
    0x00000003, 0x80000005,
};
static const uint32_t k_b[N] = {
    0x3F800000, 0x00000000, 0x80000000, 0x40400000, 0x7F800000, 0xFF800000,
    0x40000000, 0x3F000000, 0x7F7FFFFF, 0x00000002, 0x3F7FFFFF, 0x0DA24260,
    0x7149F2CA, 0x40E00000, 0xC1200000, 0x00800001, 0x3FC00001, 0xBFC00000,
    0x4B000001, 0xCB7FFFFF, 0x3F400000, 0xBF400000, 0x3EFFFFFF, 0x00000001,
    0x80000001, 0x00400000, 0x007FFFFF, 0x80800000, 0x00000004, 0x80000004,
    0x7FC00000, 0x7F800001,
};

static uint32_t g_r[N];
static uint16_t g_h[N];

static uint32_t mix(uint32_t acc, uint32_t v)
{
    return (((acc << 5) | (acc >> 27)) ^ v) * 0x9E3779B1u;
}

static uint32_t fold(uint32_t acc, const void *p, size_t words)
{
    const uint32_t *w = p;

    for (size_t i = 0; i < words; i++) {
        acc = mix(acc, w[i]);
    }
    acc = mix(acc, csr_read("fflags"));
    __asm__ volatile("csrw fflags, zero");
    return acc;
}

static void set_frm(unsigned rm)
{
    __asm__ volatile("fsrm %0" : : "r"(rm));
}

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));
    /* FS on: the vector FP instructions need it */
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x2000u));
    __asm__ volatile("csrw fflags, zero");

    puts_("VECFP-START\n");
    uint32_t sum = 0u;
    const size_t vl = __riscv_vsetvl_e32m8(N);
    const vfloat32m8_t a = __riscv_vle32_v_f32m8((const float *)k_a, vl);
    const vfloat32m8_t b = __riscv_vle32_v_f32m8((const float *)k_b, vl);

    for (unsigned rm = 0; rm < 5u; rm++) {
        set_frm(rm);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfadd_vv_f32m8(a, b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfsub_vv_f32m8(a, b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfmul_vv_f32m8(a, b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfdiv_vv_f32m8(a, b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfsqrt_v_f32m8(a, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r,
                              __riscv_vfmacc_vv_f32m8(b, a, b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r,
                              __riscv_vfnmsub_vv_f32m8(a, b, a, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_i32m8((int32_t *)g_r, __riscv_vfcvt_x_f_v_i32m8(a, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_u32m8(g_r, __riscv_vfcvt_xu_f_v_u32m8(b, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r,
                              __riscv_vfcvt_f_x_v_f32m8(
                                  __riscv_vle32_v_i32m8((const int32_t *)k_b, vl), vl),
                              vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfrec7_v_f32m8(a, vl), vl);
        sum = fold(sum, g_r, N);
        __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfrsqrt7_v_f32m8(b, vl), vl);
        sum = fold(sum, g_r, N);

        const size_t hl = __riscv_vsetvl_e16m4(N);

        __riscv_vse16_v_i16m4((int16_t *)g_h,
                              __riscv_vfncvt_x_f_w_i16m4(
                                  __riscv_vle32_v_f32m8((const float *)k_a, hl), hl),
                              hl);
        sum = fold(sum, g_h, N / 2u);
        __riscv_vse32_v_f32m8((float *)g_r,
                              __riscv_vfwcvt_f_x_v_f32m8(
                                  __riscv_vle16_v_i16m4((const int16_t *)k_b, hl), hl),
                              hl);
        sum = fold(sum, g_r, N);
        (void)__riscv_vsetvl_e32m8(N);
    }
    set_frm(0u);
    __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfmin_vv_f32m8(a, b, vl), vl);
    sum = fold(sum, g_r, N);
    __riscv_vse32_v_f32m8((float *)g_r, __riscv_vfmax_vv_f32m8(a, b, vl), vl);
    sum = fold(sum, g_r, N);
    {
        const vbool4_t lt = __riscv_vmflt_vv_f32m8_b4(a, b, vl);
        const vbool4_t le = __riscv_vmfle_vv_f32m8_b4(a, b, vl);
        const vbool4_t eq = __riscv_vmfeq_vv_f32m8_b4(a, b, vl);

        sum = mix(sum, (uint32_t)__riscv_vcpop_m_b4(lt, vl));
        sum = mix(sum, (uint32_t)__riscv_vfirst_m_b4(le, vl));
        sum = mix(sum, (uint32_t)__riscv_vcpop_m_b4(eq, vl));
        sum = fold(sum, g_r, 0);
    }
    {
        const vfloat32m1_t z = __riscv_vfmv_s_f_f32m1(0.0f, 1);
        const float s = __riscv_vfmv_f_s_f32m1_f32(
            __riscv_vfredosum_vs_f32m8_f32m1(__riscv_vle32_v_f32m8((const float *)k_b, 9), z, 9));
        const float m = __riscv_vfmv_f_s_f32m1_f32(
            __riscv_vfredmin_vs_f32m8_f32m1(a, z, vl));
        uint32_t bits;

        __builtin_memcpy(&bits, &s, 4);
        sum = mix(sum, bits);
        __builtin_memcpy(&bits, &m, 4);
        sum = mix(sum, bits);
        sum = fold(sum, g_r, 0);
    }

    puts_("VECFP-END\nsum ");
    puthex(sum);
    puts_("\n");
    return 0;
}
