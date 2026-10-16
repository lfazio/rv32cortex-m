/* SPDX-License-Identifier: Apache-2.0 */
/*
 * efpu2.c - the e200z7's floating-point unit, as a compiler uses it.
 *
 * **Built by clang, for EFPU2**: --target=powerpc-unknown-eabi
 * -mcpu=e500 -mspe -mefpu2, so every float operation here is one of the
 * core's own instructions -- efsadd, efsmul, efsdiv, efscmplt,
 * efsctsiz, efscfui and the rest. GCC dropped its SPE back end in
 * version 9, and until this guest nothing compiled in the tree used the
 * unit at all; see docs/frontend/ppc.md.
 *
 * The answers are **not** computed here: efpu2_want.py runs the same
 * operations through tests/ppc-check/efpu.py's model, in exact
 * rationals, and writes them and the inputs into efpu2_want.h.
 *
 * Each operation is its own noinline function, so the compiler has to
 * emit the instruction rather than the answer; and float only, because
 * a double would be a compiler-rt call and this guest links no library.
 */
#include "efpu2_want.h"

typedef unsigned int u32;

#define UART (*(volatile unsigned char *)0x10000000u)
#define NOINLINE __attribute__((noinline))
#define N(a) ((u32)(sizeof(a) / sizeof((a)[0])))

static void pc_(char c)
{
    UART = (unsigned char)c;
}

static void ps_(const char *s)
{
    while (*s != '\0') {
        pc_(*s++);
    }
}

static void phex(u32 v)
{
    for (int i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 15u]);
    }
}

static u32 g_checks;
static u32 g_fail;

static void check(const char *what, u32 i, u32 got, u32 want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        if (g_fail <= 20u) {
            ps_("  FAIL ");
            ps_(what);
            ps_(" #");
            phex(i);
            ps_(" got ");
            phex(got);
            ps_(" want ");
            phex(want);
            pc_('\n');
        }
    }
}

static float f_of(u32 x)
{
    union {
        u32 u;
        float f;
    } v;

    v.u = x;
    return v.f;
}

static u32 u_of(float f)
{
    union {
        u32 u;
        float f;
    } v;

    v.f = f;
    return v.u;
}

static NOINLINE float f_add(float a, float b)
{
    return a + b;
}
static NOINLINE float f_sub(float a, float b)
{
    return a - b;
}
static NOINLINE float f_mul(float a, float b)
{
    return a * b;
}
static NOINLINE float f_div(float a, float b)
{
    return a / b;
}
static NOINLINE int f_lt(float a, float b)
{
    return a < b;
}
static NOINLINE int f_gt(float a, float b)
{
    return a > b;
}
static NOINLINE int f_eq(float a, float b)
{
    return a == b;
}
static NOINLINE int f_to_i(float a)
{
    return (int)a;
}
static NOINLINE unsigned f_to_u(float a)
{
    return (unsigned)a;
}
static NOINLINE float f_from_i(int i)
{
    return (float)i;
}
static NOINLINE float f_from_u(unsigned u)
{
    return (float)u;
}

static u32 spefscr(void)
{
    u32 v;

    __asm__ volatile("mfspr %0, 512" : "=r"(v));
    return v;
}

static void spefscr_set(u32 v)
{
    __asm__ volatile("mtspr 512, %0" : : "r"(v));
}

#define STICKY 0x003E0000u /* FINXS, FINVS, FDBZS, FUNFS, FOVFS */

int main(void)
{
    u32 k = 0u;

    ps_("\nPPC-EFPU2-START\n");

    /* Every pair of values, through the four arithmetic operations and
     * the three compares. */
    for (u32 i = 0u; i < N(IN_V); i++) {
        for (u32 j = 0u; j < N(IN_V); j++) {
            const float a = f_of(IN_V[i]);
            const float b = f_of(IN_V[j]);

            check("add", k, u_of(f_add(a, b)), WANT_ARITH[k]);
            check("sub", k + 1u, u_of(f_sub(a, b)), WANT_ARITH[k + 1u]);
            check("mul", k + 2u, u_of(f_mul(a, b)), WANT_ARITH[k + 2u]);
            check("div", k + 3u, u_of(f_div(a, b)), WANT_ARITH[k + 3u]);
            k += 4u;
        }
    }
    k = 0u;
    for (u32 i = 0u; i < N(IN_V); i++) {
        for (u32 j = 0u; j < N(IN_V); j++) {
            const float a = f_of(IN_V[i]);
            const float b = f_of(IN_V[j]);
            const u32 m = (u32)f_lt(a, b) | ((u32)f_gt(a, b) << 1) |
                          ((u32)f_eq(a, b) << 2);

            check("cmp", k, m, WANT_CMP[k]);
            k++;
        }
    }

    /* To integers and back, truncating and saturating. */
    for (u32 i = 0u; i < N(IN_F); i++) {
        check("to-int", 2u * i, (u32)f_to_i(f_of(IN_F[i])),
              WANT_TO_INT[2u * i]);
        check("to-uint", 2u * i + 1u, f_to_u(f_of(IN_F[i])),
              WANT_TO_INT[2u * i + 1u]);
    }
    for (u32 i = 0u; i < N(IN_I); i++) {
        check("from-int", 2u * i, u_of(f_from_i((int)IN_I[i])),
              WANT_FROM_INT[2u * i]);
        check("from-uint", 2u * i + 1u, u_of(f_from_u(IN_I[i])),
              WANT_FROM_INT[2u * i + 1u]);
    }

    /* Everything above has had something special in it, so all five
     * sticky bits are set; clear them, and a polynomial over ordinary
     * values may leave only inexact. */
    check("sticky", 0u, spefscr() & STICKY, WANT_STICKY);
    spefscr_set(0u);
    for (u32 i = 0u; i < N(IN_X); i++) {
        const float x = f_of(IN_X[i]);
        float t = f_of(IN_POLY[3]);

        for (int c = 2; c >= 0; c--) {
            t = f_mul(t, x);
            t = f_add(t, f_of(IN_POLY[c]));
        }
        check("poly", i, u_of(t), WANT_POLY[i]);
    }
    check("poly-sticky", 0u, spefscr() & STICKY, WANT_POLY_STICKY);

    /*
     * The terminator, and the count: a run that printed this executed
     * every check above, which "no FAIL line" alone does not say.
     */
    ps_("PPC-EFPU2-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');
    return (int)g_fail;
}
