/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fpedge.c - floating-point results at the edge of underflow, and the
 * flags each raises: what an FPU that detects tininess before rounding
 * (ARM's VFP) answers differently from RISC-V, which detects it after.
 *
 * A product whose exact value lies just below the smallest normal,
 * 2^-126, and rounds to it at 24 bits with an unbounded exponent -- the
 * fifth case, 2^-126 (1 - 2^-46) -- is tiny before rounding and not
 * after: RISC-V raises NX alone, a before-rounding FPU NX and UF. The
 * first is a single already, tiny either way. Each case prints its
 * result and fflags. The host prints RISC-V's answers on both backends
 * (guest-fpedge, which pins them), and so do the F746 and the N6, both
 * backends -- since the Thumb-2 lowering stopped taking UF from VFP's
 * UFC, which raised it for the fifth and sixth cases (flags 3), and
 * redoes a multiply, divide or fused multiply-add whose result is at or
 * below 2^-126 in the guest's arithmetic (t2_uf_fix,
 * docs/backend/thumb2.md).
 *
 * Built with Zve32f (fpedge-v), it asks the vector unit the same, one
 * element at a time: vfmul.vv, vfmacc.vv onto zero and vfdiv.vv, whose
 * Thumb-2 lowering is the same VFP and takes the same fix.
 */

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
        UART_THR = (uint8_t)"0123456789abcdef"[(v >> i) & 15u];
    }
}

/*
 * r = a op b under rounding mode rm, and the flags it raised -- asked
 * REPS times, so that the JIT translates it: run once, after the code
 * buffer has compacted, a case waits for the hot threshold and is
 * interpreted, which answers for SoftFloat and not for the lowering.
 */
#define REPS 64

#define CASE(op, a, b, rm)                                                     \
    do {                                                                       \
        uint32_t r_, f_;                                                       \
        for (int k_ = 0; k_ < REPS; k_++)                                      \
        __asm__ volatile("fsrm %4\n"                                           \
                         "csrw fflags, zero\n"                                 \
                         "fmv.w.x ft0, %2\n"                                   \
                         "fmv.w.x ft1, %3\n" op " ft2, ft0, ft1\n"             \
                         "fmv.x.w %0, ft2\n"                                   \
                         "frflags %1\n"                                        \
                         : "=r"(r_), "=r"(f_)                                  \
                         : "r"(a), "r"(b), "r"(rm)                             \
                         : "ft0", "ft1", "ft2");                               \
        puts_(op " ");                                                         \
        puthex(a);                                                             \
        puts_(" ");                                                            \
        puthex(b);                                                             \
        puts_(" rm ");                                                         \
        puthex(rm);                                                            \
        puts_(" -> ");                                                         \
        puthex(r_);                                                            \
        puts_(" flags ");                                                      \
        puthex(f_);                                                            \
        puts_("\n");                                                           \
    } while (0)

#if defined(__riscv_zve32f)
/*
 * vd = a op b for one element, vd 0 before, under rm, and its flags --
 * the operation in a loop of its own that starts at the vsetivli, with
 * no CSR access inside: the JIT translates a vector instruction only in
 * a block that set vtype, and a CSR access is interpreted, in a batch
 * that would carry the vsetivli with it.
 */
#define VASM(insn)                                                             \
    __asm__ volatile("vsetivli zero, 1, e32, m1, ta, ma\n"                     \
                     "vle32.v v1, (%0)\n"                                      \
                     "vle32.v v2, (%1)\n"                                      \
                     "vmv.v.i v3, 0\n" insn " v3, v1, v2\n"                    \
                     "vse32.v v3, (%2)\n"                                      \
                     :                                                         \
                     : "r"(&s_a), "r"(&s_b), "r"(&s_r)                         \
                     : "memory")

static void vcase(const char *op, uint32_t a, uint32_t b, uint32_t rm)
{
    static uint32_t s_a, s_b, s_r;
    uint32_t f;

    s_a = a;
    s_b = b;
    s_r = 0u;
    __asm__ volatile("fsrm %0\n"
                     "csrw fflags, zero\n"
                     :
                     : "r"(rm));
    if (op[2] == 'm' && op[3] == 'a') {
        for (int k = 0; k < REPS; k++) {
            VASM("vfmacc.vv");
        }
    } else if (op[2] == 'd') {
        for (int k = 0; k < REPS; k++) {
            VASM("vfdiv.vv");
        }
    } else {
        for (int k = 0; k < REPS; k++) {
            VASM("vfmul.vv");
        }
    }
    __asm__ volatile("frflags %0" : "=r"(f));
    puts_(op);
    puts_(" ");
    puthex(a);
    puts_(" ");
    puthex(b);
    puts_(" rm ");
    puthex(rm);
    puts_(" -> ");
    puthex(s_r);
    puts_(" flags ");
    puthex(f);
    puts_("\n");
}
#endif

int main(void)
{
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x2000u));
#if defined(__riscv_zve32f)
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x200u)); /* VS */
#endif
    puts_("FPEDGE-START\n");
    /* (1 - 2^-24) * 2^-126 is a single: tiny before and after rounding */
    CASE("fmul.s", 0x3F7FFFFFu, 0x00800000u, 0u);
    CASE("fmul.s", 0xBF7FFFFFu, 0x00800000u, 0u);
    CASE("fmul.s", 0x3F7FFFFFu, 0x00800000u, 3u); /* rup */
    CASE("fmul.s", 0x3F7FFFFFu, 0x00800000u, 1u); /* rtz: stays below */
    /* 2^-126 (1 - 2^-46): rounds to 2^-126 with an unbounded exponent,
     * so tiny before rounding and not after -- NX alone on RISC-V */
    CASE("fmul.s", 0x3F7FFFFEu, 0x00800001u, 0u);
    CASE("fmul.s", 0xBF7FFFFEu, 0x00800001u, 0u);
    CASE("fmul.s", 0x3F7FFFFEu, 0x00800001u, 3u);
    /* 2^-126 - 2^-150 exactly, rounding up to the smallest normal */
    CASE("fdiv.s", 0x00FFFFFFu, 0x40000000u, 0u);
    /* a sum: 2^-126 less half a subnormal step */
    CASE("fadd.s", 0x00800000u, 0x80000001u, 0u);
    CASE("fsub.s", 0x00800001u, 0x00000001u, 0u);
#if defined(__riscv_zve32f)
    vcase("vfmul.vv", 0x3F7FFFFFu, 0x00800000u, 0u);
    vcase("vfmul.vv", 0x3F7FFFFEu, 0x00800001u, 0u);
    vcase("vfmul.vv", 0xBF7FFFFEu, 0x00800001u, 0u);
    vcase("vfmacc.vv", 0x3F7FFFFEu, 0x00800001u, 0u);
    vcase("vfdiv.vv", 0x00FFFFFFu, 0x40000000u, 0u);
#endif
    puts_("FPEDGE-END\n");
    return 0;
}
