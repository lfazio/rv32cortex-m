/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vecops.c - the integer vector operations the JIT lowers, at every
 * element width and three group sizes, with vl at its edges: one
 * checksum, the same interpreted, translated on the host and on each
 * board.
 *
 * 41 operations -- vadd, vsub, vrsub, vand, vor, vxor, vminu, vmin,
 * vmaxu, vmax, vmul, vsll, vsrl, vsra, vmv, vaaddu, vaadd and the four
 * multiply-adds, .vv and .vx -- then the eight single-width reductions
 * plain and under v0, the two widening sums at e8 and e16, and masked
 * forms and vmerge, v0 random bytes each case: at e8, e16 and e32 and m1, m2 and m8,
 * under each vxrm, with vl 0, 1, past a register's middle, one short of
 * the group, the group, and an AVL past it; the scalars at their edges
 * (0, -1, the sign bits, shift counts past the width). Tail undisturbed:
 * vd starts each case as a pattern no operation makes, and the whole
 * group is folded, so a tail written over shows.
 *
 * Each case is one asm statement -- the three groups loaded whole, the
 * vsetvli, the operation, the store -- so the operation is in a block
 * that set vtype, which is where the JIT lowers it (a block that begins
 * after the vsetvli sends it to the helper; docs/frontend/rv32-vector.md).
 * **Each case runs REPS times in a row**, which is what makes it a test
 * of the JIT on a board: there the buffer has compacted by the time the
 * run starts, so a block is translated only on its EMU_JIT_HOT_THRESHOLD'th
 * entry (8) in one context -- and vxrm is part of the context. Once each,
 * a case met its block 6 times a context, and all but the first group ran
 * interpreted: a mutant of the N6's masked reductions changed one line of
 * 36. The host's buffer never compacts, so the host could not show it.
 *
 * A line a width, group and vxrm, each its own checksum, says where two
 * runs part; the last is all of them. A trap prints its cause and exits 1.
 */

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
    puts_("VECOPS-TRAP mcause ");
    puthex(csr_read("mcause"));
    puts_(" mepc ");
    puthex(csr_read("mepc"));
    puts_("\n");
    exit_(1);
}

/* Eight registers each: v8 (vs2), v16 (vs1), v24 (vd); and v0's mask. */
static uint8_t g_m[16] __attribute__((aligned(16)));
static uint8_t g_a[128] __attribute__((aligned(16)));
static uint8_t g_b[128] __attribute__((aligned(16)));
static uint8_t g_d[128] __attribute__((aligned(16)));
static uint8_t g_d0[128] __attribute__((aligned(16)));

#define RUN(vt, insn)                                                          \
    __asm__ volatile("vl1re8.v v0, (%[m])\n"                                   \
                     "vl8re8.v v8, (%[a])\n"                                   \
                     "vl8re8.v v16, (%[b])\n"                                  \
                     "vl8re8.v v24, (%[d])\n"                                  \
                     "vsetvli t0, %[avl], " vt "\n" insn "\n"                  \
                     "vs8r.v v24, (%[d])\n"                                    \
                     :                                                         \
                     : [m] "r"(g_m), [a] "r"(g_a), [b] "r"(g_b), [d] "r"(g_d), \
                       [avl] "r"(avl), [s] "r"(sc)                             \
                     : "t0", "memory")

#define OPS(X, vt)                                                             \
    X(vt, "vadd.vv v24, v8, v16")                                              \
    X(vt, "vadd.vx v24, v8, %[s]")                                             \
    X(vt, "vsub.vv v24, v8, v16")                                              \
    X(vt, "vsub.vx v24, v8, %[s]")                                             \
    X(vt, "vrsub.vx v24, v8, %[s]")                                            \
    X(vt, "vand.vv v24, v8, v16")                                              \
    X(vt, "vand.vx v24, v8, %[s]")                                             \
    X(vt, "vor.vv v24, v8, v16")                                               \
    X(vt, "vor.vx v24, v8, %[s]")                                              \
    X(vt, "vxor.vv v24, v8, v16")                                              \
    X(vt, "vxor.vx v24, v8, %[s]")                                             \
    X(vt, "vminu.vv v24, v8, v16")                                             \
    X(vt, "vminu.vx v24, v8, %[s]")                                            \
    X(vt, "vmin.vv v24, v8, v16")                                              \
    X(vt, "vmin.vx v24, v8, %[s]")                                             \
    X(vt, "vmaxu.vv v24, v8, v16")                                             \
    X(vt, "vmaxu.vx v24, v8, %[s]")                                            \
    X(vt, "vmax.vv v24, v8, v16")                                              \
    X(vt, "vmax.vx v24, v8, %[s]")                                             \
    X(vt, "vmul.vv v24, v8, v16")                                              \
    X(vt, "vmul.vx v24, v8, %[s]")                                             \
    X(vt, "vsll.vv v24, v8, v16")                                              \
    X(vt, "vsll.vx v24, v8, %[s]")                                             \
    X(vt, "vsrl.vv v24, v8, v16")                                              \
    X(vt, "vsrl.vx v24, v8, %[s]")                                             \
    X(vt, "vsra.vv v24, v8, v16")                                              \
    X(vt, "vsra.vx v24, v8, %[s]")                                             \
    X(vt, "vmv.v.v v24, v16")                                                  \
    X(vt, "vmv.v.x v24, %[s]")                                                 \
    X(vt, "vaaddu.vv v24, v8, v16")                                            \
    X(vt, "vaaddu.vx v24, v8, %[s]")                                           \
    X(vt, "vaadd.vv v24, v8, v16")                                             \
    X(vt, "vaadd.vx v24, v8, %[s]")                                            \
    X(vt, "vmacc.vv v24, v16, v8")                                             \
    X(vt, "vmacc.vx v24, %[s], v8")                                            \
    X(vt, "vnmsac.vv v24, v16, v8")                                            \
    X(vt, "vnmsac.vx v24, %[s], v8")                                           \
    X(vt, "vmadd.vv v24, v16, v8")                                             \
    X(vt, "vmadd.vx v24, %[s], v8")                                            \
    X(vt, "vnmsub.vv v24, v16, v8")                                            \
    X(vt, "vnmsub.vx v24, %[s], v8")                                           \
    X(vt, "vredsum.vs v24, v8, v16")                                           \
    X(vt, "vredand.vs v24, v8, v16")                                           \
    X(vt, "vredor.vs v24, v8, v16")                                            \
    X(vt, "vredxor.vs v24, v8, v16")                                           \
    X(vt, "vredminu.vs v24, v8, v16")                                          \
    X(vt, "vredmin.vs v24, v8, v16")                                           \
    X(vt, "vredmaxu.vs v24, v8, v16")                                          \
    X(vt, "vredmax.vs v24, v8, v16")                                           \
    X(vt, "vredsum.vs v24, v8, v16, v0.t")                                     \
    X(vt, "vredand.vs v24, v8, v16, v0.t")                                     \
    X(vt, "vredor.vs v24, v8, v16, v0.t")                                      \
    X(vt, "vredxor.vs v24, v8, v16, v0.t")                                     \
    X(vt, "vredminu.vs v24, v8, v16, v0.t")                                    \
    X(vt, "vredmin.vs v24, v8, v16, v0.t")                                     \
    X(vt, "vredmaxu.vs v24, v8, v16, v0.t")                                    \
    X(vt, "vredmax.vs v24, v8, v16, v0.t")                                     \
    X(vt, "vadd.vv v24, v8, v16, v0.t")                                        \
    X(vt, "vsub.vx v24, v8, %[s], v0.t")                                       \
    X(vt, "vrsub.vx v24, v8, %[s], v0.t")                                      \
    X(vt, "vxor.vv v24, v8, v16, v0.t")                                        \
    X(vt, "vminu.vx v24, v8, %[s], v0.t")                                      \
    X(vt, "vmax.vv v24, v8, v16, v0.t")                                        \
    X(vt, "vmul.vv v24, v8, v16, v0.t")                                        \
    X(vt, "vsll.vx v24, v8, %[s], v0.t")                                       \
    X(vt, "vsra.vv v24, v8, v16, v0.t")                                        \
    X(vt, "vaaddu.vv v24, v8, v16, v0.t")                                      \
    X(vt, "vmacc.vv v24, v16, v8, v0.t")                                       \
    X(vt, "vnmsub.vx v24, %[s], v8, v0.t")                                     \
    X(vt, "vmerge.vvm v24, v8, v16, v0")                                       \
    X(vt, "vmerge.vxm v24, v8, %[s], v0")
#define NOPS 73u

/* The widening sums, where a wider element exists: e8 and e16. */
#define OPS_W(X, vt)                                                           \
    X(vt, "vwredsumu.vs v24, v8, v16")                                         \
    X(vt, "vwredsum.vs v24, v8, v16")                                          \
    X(vt, "vwredsumu.vs v24, v8, v16, v0.t")                                   \
    X(vt, "vwredsum.vs v24, v8, v16, v0.t")
#define NOPS_W 4u

/* Past EMU_JIT_HOT_THRESHOLD (8): every case translated at least once. */
#define REPS 10u

#define CASE(vt, insn)                                                         \
    if (op == k++) {                                                           \
        RUN(vt, insn);                                                         \
        return;                                                                \
    }

#define FN(name, vt)                                                           \
    static void name(unsigned op, size_t avl, uint32_t sc)                     \
    {                                                                          \
        unsigned k = 0;                                                        \
                                                                               \
        OPS(CASE, vt)                                                          \
    }
#define FN_W(name, vt)                                                         \
    static void name(unsigned op, size_t avl, uint32_t sc)                     \
    {                                                                          \
        unsigned k = 0;                                                        \
                                                                               \
        OPS(CASE, vt)                                                          \
        OPS_W(CASE, vt)                                                        \
    }

FN_W(op_e8m1, "e8, m1, tu, mu")
FN_W(op_e8m2, "e8, m2, tu, mu")
FN_W(op_e8m8, "e8, m8, tu, mu")
FN_W(op_e16m1, "e16, m1, tu, mu")
FN_W(op_e16m2, "e16, m2, tu, mu")
FN_W(op_e16m8, "e16, m8, tu, mu")
FN(op_e32m1, "e32, m1, tu, mu")
FN(op_e32m2, "e32, m2, tu, mu")
FN(op_e32m8, "e32, m8, tu, mu")

static const struct {
    void (*fn)(unsigned, size_t, uint32_t);
    const char *name;
    uint32_t lanes;
    unsigned nops;
} k_fns[] = {
    {op_e8m1, "e8m1", 16u, NOPS + NOPS_W},   {op_e8m2, "e8m2", 32u, NOPS + NOPS_W},
    {op_e8m8, "e8m8", 128u, NOPS + NOPS_W},  {op_e16m1, "e16m1", 8u, NOPS + NOPS_W},
    {op_e16m2, "e16m2", 16u, NOPS + NOPS_W}, {op_e16m8, "e16m8", 64u, NOPS + NOPS_W},
    {op_e32m1, "e32m1", 4u, NOPS},           {op_e32m2, "e32m2", 8u, NOPS},
    {op_e32m8, "e32m8", 32u, NOPS},
};

static const uint32_t k_sc[] = {0u,          1u,          0xFFFFFFFFu, 0x80u,
                                0x8000u,     0x80000000u, 7u,          33u,
                                0x7FFFu,     0xFEu,       0x7Fu};

static uint32_t mix(uint32_t acc, uint32_t v)
{
    return (((acc << 5) | (acc >> 27)) ^ v) * 0x9E3779B1u;
}

int main(void)
{
    uint32_t seed = 0x2545F491u;
    uint32_t sum = 0u;

    __asm__ volatile("csrw mtvec, %0" : : "r"(on_trap));
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x200u)); /* VS on */
    for (uint32_t i = 0; i < 128u; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        g_a[i] = (uint8_t)seed;
        g_b[i] = (uint8_t)(seed >> 8);
        g_d0[i] = (uint8_t)(0xA5u ^ (i * 29u));
    }
    puts_("VECOPS-START\n");
    for (uint32_t xrm = 0; xrm < 4u; xrm++) {
        __asm__ volatile("csrw vxrm, %0" : : "r"(xrm));
        for (uint32_t f = 0; f < sizeof(k_fns) / sizeof(k_fns[0]); f++) {
            const uint32_t n = k_fns[f].lanes;
            const size_t avls[6] = {0u, 1u, n / 2u + 1u, n - 1u, n, n + 3u};
            uint32_t line = 0u; /* this group's alone: a line says which */

            for (unsigned op = 0; op < k_fns[f].nops; op++) {
                for (uint32_t j = 0; j < 6u; j++) {
                    const uint32_t sc =
                        k_sc[(op + j + xrm) % (sizeof(k_sc) / sizeof(k_sc[0]))];

                    for (uint32_t i = 0; i < 16u; i++) {
                        seed ^= seed << 13;
                        seed ^= seed >> 17;
                        seed ^= seed << 5;
                        g_m[i] = (uint8_t)seed;
                    }
                    for (uint32_t k = 0; k < REPS; k++) {
                        for (uint32_t i = 0; i < 128u; i++) {
                            g_d[i] = g_d0[i];
                        }
                        k_fns[f].fn(op, avls[j], sc);
                        for (uint32_t i = 0; i < 128u; i += 4u) {
                            line = mix(line, (uint32_t)g_d[i] |
                                                 ((uint32_t)g_d[i + 1u] << 8) |
                                                 ((uint32_t)g_d[i + 2u] << 16) |
                                                 ((uint32_t)g_d[i + 3u] << 24));
                        }
                    }
                }
            }
            puts_(k_fns[f].name);
            puts_(" vxrm ");
            puthex(xrm);
            puts_(" ");
            puthex(line);
            puts_("\n");
            sum = mix(sum, line);
        }
    }
    puts_("VECOPS-END\nsum ");
    puthex(sum);
    puts_("\n");
    return 0;
}
