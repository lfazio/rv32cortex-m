/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vecmask.c - the vector unit's masked forms (v0.t), under masks of both
 * values in every byte: widening multiply-adds, narrowing shifts and
 * clips (whose vxsat only an active element may set), single-width
 * arithmetic and vmerge.
 *
 * Written for what fuzz-ir's corpus does not reach: no input of 12,667
 * told Thumb-2's masked widening multiply-add from its unmasked one.
 * The masks come from memory (vlm.v), so they are what this file says;
 * every result goes into one checksum, printed between markers -- the
 * same interpreted, translated on the host and translated on the F746.
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
    puts_("VECMASK-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_("\n");
    exit_(1);
}

#define N 100

static const uint8_t k_masks[4][16] = {
    {0xA5, 0x3C, 0x0F, 0xF0, 0x55, 0xAA, 0x01, 0x80,
     0x7E, 0x81, 0x99, 0x66, 0xC3, 0x18, 0xE7, 0x24},
    {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
     0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF},
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
     0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F},
    {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
     0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A},
};

static int16_t g_h[N], g_k[N];
static uint16_t g_uh[N];
static int32_t g_w[N], g_acc[N];
static uint32_t g_uw[N];
static int16_t g_n16[N];
static uint16_t g_nu16[N];
static uint8_t g_nu8[N];
static int32_t g_s32[N];

static uint32_t g_seed = 0x9E3779B9u;

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

static uint32_t sum_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t acc = 0u;

    for (size_t i = 0; i < n; i++) {
        acc = mix(acc, b[i]);
    }
    return acc;
}

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));
    __asm__ volatile("csrw vxrm, zero");

    puts_("VECMASK-START\n");
    uint32_t sum = 0u;

    for (unsigned m = 0; m < 4u; m++) {
        const uint8_t *const mk = k_masks[m];

        for (size_t i = 0; i < N; i++) {
            g_h[i] = (int16_t)rnd();
            g_k[i] = (int16_t)(rnd() >> 20); /* small: no 32-bit wrap */
            g_uh[i] = (uint16_t)rnd();
            g_w[i] = (int32_t)rnd() >> (i % 13u);
            g_uw[i] = rnd() >> (i % 11u);
            g_acc[i] = (int32_t)rnd();
            g_s32[i] = (int32_t)rnd();
            g_n16[i] = (int16_t)0x5A5A;
            g_nu16[i] = 0xA5A5u;
            g_nu8[i] = 0x3Cu;
        }
        /* Widening multiply-adds, e16 into e32, under the mask. */
        for (size_t i = 0, n = N; n > 0u;) {
            const size_t vl = __riscv_vsetvl_e16m1(n < 16u ? n : 16u);
            const vbool16_t v0 = __riscv_vlm_v_b16(mk, vl);
            const vint16m1_t x = __riscv_vle16_v_i16m1(g_h + i, vl);
            const vint16m1_t y = __riscv_vle16_v_i16m1(g_k + i, vl);
            vint32m2_t acc = __riscv_vle32_v_i32m2(g_acc + i, vl);

            acc = __riscv_vwmacc_vv_i32m2_mu(v0, acc, x, y, vl);
            acc = __riscv_vwmacc_vx_i32m2_mu(v0, acc, (int16_t)(m * 77 - 99), y, vl);
            __riscv_vse32_v_i32m2(g_acc + i, acc, vl);

            vuint32m2_t uacc = __riscv_vle32_v_u32m2(g_uw + i, vl);

            uacc = __riscv_vwmaccu_vv_u32m2_mu(v0, uacc,
                                               __riscv_vle16_v_u16m1(g_uh + i, vl),
                                               __riscv_vle16_v_u16m1(g_uh + i, vl), vl);
            __riscv_vse32_v_u32m2(g_uw + i, uacc, vl);
            i += vl;
            n -= vl;
        }
        sum = mix(sum, sum_bytes(g_acc, sizeof(g_acc)));
        sum = mix(sum, sum_bytes(g_uw, sizeof(g_uw)));

        /* Narrowing shifts and clips, e32 to e16, under the mask. */
        for (size_t i = 0, n = N; n > 0u;) {
            const size_t vl = __riscv_vsetvl_e16m1(n < 16u ? n : 16u);
            const vbool16_t v0 = __riscv_vlm_v_b16(mk, vl);
            const vint32m2_t w = __riscv_vle32_v_i32m2(g_w + i, vl);

            __riscv_vse16_v_i16m1(
                g_n16 + i,
                __riscv_vnclip_wx_i16m1_mu(v0, __riscv_vle16_v_i16m1(g_n16 + i, vl),
                                           w, m + 3u, __RISCV_VXRM_RNU, vl),
                vl);
            __riscv_vse16_v_u16m1(
                g_nu16 + i,
                __riscv_vnsrl_wx_u16m1_mu(v0, __riscv_vle16_v_u16m1(g_nu16 + i, vl),
                                          __riscv_vle32_v_u32m2(g_uw + i, vl), m + 9u,
                                          vl),
                vl);
            i += vl;
            n -= vl;
        }
        sum = mix(sum, sum_bytes(g_n16, sizeof(g_n16)));
        sum = mix(sum, sum_bytes(g_nu16, sizeof(g_nu16)));
        sum = mix(sum, csr_read("vxsat"));
        __asm__ volatile("csrw vxsat, zero");

        /* Narrowing e16 to e8, and single-width forms and the merge. */
        for (size_t i = 0, n = N; n > 0u;) {
            const size_t vl = __riscv_vsetvl_e8m1(n < 16u ? n : 16u);
            const vbool8_t v0 = __riscv_vlm_v_b8(mk, vl);

            __riscv_vse8_v_u8m1(
                g_nu8 + i,
                __riscv_vnclipu_wx_u8m1_mu(v0, __riscv_vle8_v_u8m1(g_nu8 + i, vl),
                                           __riscv_vle16_v_u16m2(g_uh + i, vl), 7u,
                                           __RISCV_VXRM_RNU, vl),
                vl);
            i += vl;
            n -= vl;
        }
        sum = mix(sum, sum_bytes(g_nu8, sizeof(g_nu8)));
        sum = mix(sum, csr_read("vxsat"));
        __asm__ volatile("csrw vxsat, zero");
        for (size_t i = 0, n = N; n > 0u;) {
            const size_t vl = __riscv_vsetvl_e32m1(n < 4u ? n : 4u);
            const vbool32_t v0 = __riscv_vlm_v_b32(mk + (i / 8u) % 16u, vl);
            const vint32m1_t a = __riscv_vle32_v_i32m1(g_s32 + i, vl);
            const vint32m1_t b = __riscv_vle32_v_i32m1(g_acc + i, vl);
            vint32m1_t r = __riscv_vadd_vv_i32m1_mu(v0, a, a, b, vl);

            r = __riscv_vmax_vx_i32m1_mu(v0, r, r, (int32_t)m - 2, vl);
            r = __riscv_vmerge_vvm_i32m1(r, b, v0, vl);
            __riscv_vse32_v_i32m1(g_s32 + i, r, vl);
            i += vl;
            n -= vl;
        }
        sum = mix(sum, sum_bytes(g_s32, sizeof(g_s32)));
    }

    puts_("VECMASK-END\nsum ");
    puthex(sum);
    puts_("\n");
    return 0;
}
