/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vecround.c - the vector unit's fixed-point rounding under each of
 * vxrm's four modes (rnu, rne, rdn, rod), on data made of ties.
 *
 * The averaging adds and the narrowing clips round a value shifted
 * right, and the modes differ only where bits are shifted out -- most of
 * all at an exact half, and only while the result is not saturated. So
 * the inputs are small and dense: every pair of values from -9 to 9 for
 * the averages (odd sums are ties), and for the clips every value from
 * -40 to 40 shifted by 0 to 3 -- remainders below, at and above half,
 * quotients of both parities -- plus a few that saturate. Random words
 * hardly ever reach this, which is why the JIT's lowerings of the other
 * modes have this guest: the same checksum interpreted, translated on
 * the host and translated on the F746.
 *
 * Each result goes into one checksum with vxsat, printed between
 * markers. A trap prints its cause and exits 1.
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
    puts_("VECROUND-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_("\n");
    exit_(1);
}

#define NA 19  /* -9..9 */
#define NC 81  /* -40..40, and the saturating ones after */
#define NCX (NC + 7)

static int8_t g_a8[NA * NA], g_b8[NA * NA], g_r8[NA * NA];
static uint8_t g_ua[NA * NA], g_ub[NA * NA], g_ur[NA * NA];
static int32_t g_a32[NA * NA], g_b32[NA * NA], g_r32[NA * NA];
static int32_t g_c32[NCX];
static uint16_t g_c16[NCX];
static int16_t g_r16[NCX];
static uint8_t g_ru8[NCX];

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

static void set_vxrm(unsigned rm)
{
    __asm__ volatile("csrw vxrm, %0" : : "r"(rm));
}

/* One mode: every kernel, strip-mined, under it. vxrm is set before the
 * loops, so the intrinsics' own mode must agree: each takes it as an
 * argument, and a mismatched constant would set vxrm again. */
#define KERNELS(RM)                                                            \
    do {                                                                       \
        for (size_t i = 0, n = NA * NA; n > 0u;) {                             \
            const size_t vl = __riscv_vsetvl_e8m1(n);                          \
            __riscv_vse8_v_i8m1(                                               \
                g_r8 + i,                                                      \
                __riscv_vaadd_vv_i8m1(__riscv_vle8_v_i8m1(g_a8 + i, vl),       \
                                      __riscv_vle8_v_i8m1(g_b8 + i, vl), RM,   \
                                      vl),                                     \
                vl);                                                           \
            __riscv_vse8_v_u8m1(                                               \
                g_ur + i,                                                      \
                __riscv_vaaddu_vv_u8m1(__riscv_vle8_v_u8m1(g_ua + i, vl),      \
                                       __riscv_vle8_v_u8m1(g_ub + i, vl), RM,  \
                                       vl),                                    \
                vl);                                                           \
            i += vl;                                                           \
            n -= vl;                                                           \
        }                                                                      \
        sum = mix(sum, sum_bytes(g_r8, sizeof(g_r8)));                         \
        sum = mix(sum, sum_bytes(g_ur, sizeof(g_ur)));                         \
        for (size_t i = 0, n = NA * NA; n > 0u;) {                             \
            const size_t vl = __riscv_vsetvl_e32m2(n);                         \
            __riscv_vse32_v_i32m2(                                             \
                g_r32 + i,                                                     \
                __riscv_vaadd_vv_i32m2(__riscv_vle32_v_i32m2(g_a32 + i, vl),   \
                                       __riscv_vle32_v_i32m2(g_b32 + i, vl),   \
                                       RM, vl),                                \
                vl);                                                           \
            i += vl;                                                           \
            n -= vl;                                                           \
        }                                                                      \
        sum = mix(sum, sum_bytes(g_r32, sizeof(g_r32)));                       \
        for (unsigned sh = 0; sh < 4u; sh++) {                                 \
            for (size_t i = 0, n = NCX; n > 0u;) {                             \
                const size_t vl = __riscv_vsetvl_e16m1(n);                     \
                __riscv_vse16_v_i16m1(                                         \
                    g_r16 + i,                                                 \
                    __riscv_vnclip_wx_i16m1(                                   \
                        __riscv_vle32_v_i32m2(g_c32 + i, vl), sh, RM, vl),     \
                    vl);                                                       \
                i += vl;                                                       \
                n -= vl;                                                       \
            }                                                                  \
            sum = mix(sum, sum_bytes(g_r16, sizeof(g_r16)));                   \
            for (size_t i = 0, n = NCX; n > 0u;) {                             \
                const size_t vl = __riscv_vsetvl_e8m1(n);                      \
                __riscv_vse8_v_u8m1(                                           \
                    g_ru8 + i,                                                 \
                    __riscv_vnclipu_wx_u8m1(                                   \
                        __riscv_vle16_v_u16m2(g_c16 + i, vl), sh, RM, vl),     \
                    vl);                                                       \
                i += vl;                                                       \
                n -= vl;                                                       \
            }                                                                  \
            sum = mix(sum, sum_bytes(g_ru8, sizeof(g_ru8)));                   \
        }                                                                      \
        sum = mix(sum, csr_read("vxsat"));                                     \
        __asm__ volatile("csrw vxsat, zero");                                  \
    } while (0)

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));

    for (int i = 0; i < NA; i++) {
        for (int j = 0; j < NA; j++) {
            g_a8[i * NA + j] = (int8_t)(i - 9);
            g_b8[i * NA + j] = (int8_t)(j - 9);
            g_ua[i * NA + j] = (uint8_t)(i * 13 + 120);
            g_ub[i * NA + j] = (uint8_t)(j * 13 + 121);
            g_a32[i * NA + j] = (int32_t)(i - 9) * 0x10001;
            g_b32[i * NA + j] = (int32_t)(j - 9) * 0x10001 + 1;
        }
    }
    for (int i = 0; i < NC; i++) {
        g_c32[i] = i - 40;
        g_c16[i] = (uint16_t)(i * 13);
    }
    /* past each clip's range, either way */
    g_c32[NC + 0] = 0x7FFFF;
    g_c32[NC + 1] = -0x80001;
    g_c32[NC + 2] = 0x20003;
    g_c32[NC + 3] = 0x1FFFF;
    g_c32[NC + 4] = -0x20005;
    g_c32[NC + 5] = 0x10001;
    g_c32[NC + 6] = -0x10001;
    g_c16[NC + 0] = 0xFFFFu;
    g_c16[NC + 1] = 0x0807u;
    g_c16[NC + 2] = 0x0405u;
    g_c16[NC + 3] = 0x03FDu;
    g_c16[NC + 4] = 0x0201u;
    g_c16[NC + 5] = 0x01FFu;
    g_c16[NC + 6] = 0x0101u;

    puts_("VECROUND-START\n");
    uint32_t sum = 0u;

    set_vxrm(0u);
    KERNELS(__RISCV_VXRM_RNU);
    set_vxrm(1u);
    KERNELS(__RISCV_VXRM_RNE);
    set_vxrm(2u);
    KERNELS(__RISCV_VXRM_RDN);
    set_vxrm(3u);
    KERNELS(__RISCV_VXRM_ROD);

    puts_("VECROUND-END\nsum ");
    puthex(sum);
    puts_("\n");
    return 0;
}
