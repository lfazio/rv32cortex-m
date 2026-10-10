/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_fpu.c - EFPU2 scalar single precision, mode 0.
 *
 * Mode 0 is IEEE 754 arithmetic with the edges replaced by defaults,
 * and the manual's result tables (5-2 to 5-5) reduce to four rules:
 *
 *   - an infinity or NaN operand gives a fixed result, usually the
 *     largest normal number with some operand's sign;
 *   - a denormal operand is a zero of the same sign;
 *   - a result that overflows is that largest normal number;
 *   - a result that underflows is a zero.
 *
 * and each of the first two also sets FINV. "Overflow and underflow
 * conditions are determined after rounding" (5.3.2), with an unbounded
 * exponent -- so a result is rounded first and classified afterwards.
 *
 * **How the exact result is had.** SoftFloat computes in double
 * precision, rounding toward zero and reporting whether it was exact.
 * For every operation here that value holds the exact result's leading
 * 53 bits, and the inexact flag is the OR of everything below them: a
 * single-precision result needs 24 bits, a guard bit and a sticky bit,
 * so both FG and FX come out exact. That matters because FG and FX are
 * architectural -- the round interrupt hands them to a handler, which
 * rounds with them -- and because getting the guard bit wrong would
 * round the ordinary case wrong.
 *
 * Nothing here has been checked against an e200: there is no board for
 * this frontend. Where the manual's prose and its tables disagree the
 * tables are followed, and the places that needed a choice say so.
 */

#include "ppc/ppc_fpu.h"
#include "ppc/ppc_decode.h"

#include "softfloat.h"

#include <stdbool.h>

#define PMAX 0x7F7FFFFFu
#define NMAX 0xFF7FFFFFu

enum { K_ZERO, K_DENORM, K_NORM, K_SPECIAL };

static int kind(uint32_t x)
{
    const uint32_t e = (x >> 23) & 0xFFu;
    const uint32_t f = x & 0x7FFFFFu;

    if (e == 0u) {
        return (f == 0u) ? K_ZERO : K_DENORM;
    }
    return (e == 0xFFu) ? K_SPECIAL : K_NORM;
}

static bool is_nan(uint32_t x)
{
    return (x & 0x7FFFFFFFu) > 0x7F800000u;
}

static uint32_t sgn(uint32_t x)
{
    return x >> 31;
}

static uint32_t max_of(uint32_t s)
{
    return s ? NMAX : PMAX;
}

static uint32_t zero_of(uint32_t s)
{
    return s << 31;
}

/* A denormal operand counts as a zero of its sign. */
static uint32_t flush(uint32_t x)
{
    return (kind(x) == K_DENORM) ? (x & 0x80000000u) : x;
}

/* One operation's outcome, before SPEFSCR decides what happens to it. */
typedef struct {
    uint32_t val;   /* the result to write                          */
    uint32_t trunc; /* the result truncated, for a round interrupt  */
    bool inv, dbz, ovf, unf, inx;
    bool fg, fx;
} res_t;

static void set_default(res_t *r, uint32_t v)
{
    r->val = r->trunc = v;
}

/* ------------------------------------------------------------------ */
/* Rounding                                                            */
/* ------------------------------------------------------------------ */

enum { RN, RZ, RP, RM };

/* How an underflow's zero takes its sign: the result's, or the mode's
 * (+0 except under RM), which is what add, subtract and the multiply-adds
 * say. */
enum { UNF_SIGN, UNF_MODE };

/*
 * Round a double-precision value, truncated toward zero with `sticky`
 * set if anything was lost beneath it, to single precision under `rm`
 * with an unbounded exponent; then classify. The value is never zero
 * or denormal here -- the callers deal with exact zeros, and no
 * operation on single-precision normals reaches the double denormal
 * range.
 */
static void round_f64(res_t *r, float64_t d, bool sticky, uint32_t rm,
                      int unf_rule)
{
    const uint64_t bits = d.v;
    const uint32_t s = (uint32_t)(bits >> 63);
    int32_t e = (int32_t)((bits >> 52) & 0x7FFu) - 1023;
    const uint64_t sig = (bits & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    uint32_t t = (uint32_t)(sig >> 29);         /* 24 bits */
    const uint64_t rest = sig & ((1ull << 29) - 1u);
    const bool g = ((rest >> 28) & 1u) != 0u;
    const bool x = (rest & ((1ull << 28) - 1u)) != 0u || sticky;
    const uint32_t t0 = t;
    const int32_t e0 = e;
    bool up;

    switch (rm) {
    case RN:
        up = g && (x || (t & 1u) != 0u);
        break;
    case RP:
        up = (g || x) && s == 0u;
        break;
    case RM:
        up = (g || x) && s != 0u;
        break;
    default:
        up = false;
        break;
    }
    if (up) {
        t++;
        if (t == (1u << 24)) {
            t >>= 1;
            e++;
        }
    }
    r->inx = g || x;
    r->fg = g;
    r->fx = x;
    if (e > 127) {
        r->ovf = true;
        set_default(r, max_of(s));
        return;
    }
    if (e < -126) {
        r->unf = true;
        set_default(r, zero_of(unf_rule == UNF_SIGN ? s : (rm == RM ? 1u : 0u)));
        return;
    }
    r->val = (s << 31) | ((uint32_t)(e + 127) << 23) | (t & 0x7FFFFFu);
    /* Truncated below pmin would be a denormal, which this unit never
     * writes; a zero is the truncation that is representable. */
    r->trunc = (e0 > 127) ? max_of(s)
               : (e0 < -126)
                   ? zero_of(s)
                   : ((s << 31) | ((uint32_t)(e0 + 127) << 23) | (t0 & 0x7FFFFFu));
}

static uint32_t rmode(const ppc_cpu_t *c)
{
    return c->spefscr & PPC_SPEFSCR_FRMC;
}

static void sf_begin(void)
{
    softfloat_roundingMode = softfloat_round_minMag;
    softfloat_exceptionFlags = 0u;
}

static bool sf_inexact(void)
{
    return (softfloat_exceptionFlags & softfloat_flag_inexact) != 0u;
}

static float64_t f64_of(uint32_t x)
{
    float32_t f;

    f.v = x;
    return f32_to_f64(f);
}

/*
 * Finish an arithmetic result held exactly as truncated double
 * precision. An exact zero takes the IEEE sign of a sum: the common
 * sign of its two addends when they agree, otherwise +0, or -0 under
 * RM. `sa` and `sb` are those addends' signs.
 */
static void finish_sum(res_t *r, float64_t d, uint32_t rm, uint32_t sa,
                       uint32_t sb, int unf_rule)
{
    const bool inexact = sf_inexact();

    if ((d.v & 0x7FFFFFFFFFFFFFFFull) == 0u && !inexact) {
        set_default(r, zero_of(sa == sb ? sa : (rm == RM ? 1u : 0u)));
        return;
    }
    round_f64(r, d, inexact, rm, unf_rule);
}

/* ------------------------------------------------------------------ */
/* The operations                                                      */
/* ------------------------------------------------------------------ */

/* efsadd and efssub (table 5-2): `neg_b` makes it a subtraction. */
static void op_add(res_t *r, uint32_t a, uint32_t b, bool neg_b, uint32_t rm)
{
    const int ka = kind(a);
    const int kb = kind(b);

    r->inv = ka == K_SPECIAL || kb == K_SPECIAL || ka == K_DENORM || kb == K_DENORM;
    if (ka == K_SPECIAL) {
        set_default(r, max_of(sgn(a)));
        return;
    }
    if (kb == K_SPECIAL) {
        set_default(r, max_of(sgn(b) ^ (neg_b ? 1u : 0u)));
        return;
    }
    a = flush(a);
    b = flush(b) ^ (neg_b ? 0x80000000u : 0u);
    sf_begin();
    finish_sum(r, f64_add(f64_of(a), f64_of(b)), rm, sgn(a), sgn(b), UNF_MODE);
}

/* efsmul: a zero or denormal operand wins over an infinity or a NaN. */
static void op_mul(res_t *r, uint32_t a, uint32_t b, uint32_t rm)
{
    const int ka = kind(a);
    const int kb = kind(b);
    const uint32_t s = sgn(a) ^ sgn(b);

    r->inv = ka == K_SPECIAL || kb == K_SPECIAL || ka == K_DENORM || kb == K_DENORM;
    if (ka == K_ZERO || ka == K_DENORM || kb == K_ZERO || kb == K_DENORM) {
        set_default(r, zero_of(s));
        return;
    }
    if (ka == K_SPECIAL || kb == K_SPECIAL) {
        set_default(r, max_of(s));
        return;
    }
    sf_begin();
    {
        const float64_t d = f64_mul(f64_of(a), f64_of(b));

        round_f64(r, d, sf_inexact(), rm, UNF_SIGN);
    }
}

/* efsdiv (table 5-2): the divisor decides first. */
static void op_div(res_t *r, uint32_t a, uint32_t b, uint32_t rm)
{
    const int ka = kind(a);
    const int kb = kind(b);
    const uint32_t s = sgn(a) ^ sgn(b);

    r->inv = ka == K_SPECIAL || kb == K_SPECIAL || ka == K_DENORM || kb == K_DENORM;
    if (kb == K_SPECIAL) {
        set_default(r, zero_of(s));
        return;
    }
    if (kb == K_ZERO || kb == K_DENORM) {
        /* 0/0 is invalid; a finite non-zero over zero divides by zero. */
        if (ka == K_ZERO) {
            r->inv = true;
        } else if (ka == K_NORM && kb == K_ZERO) {
            r->dbz = true;
        }
        set_default(r, max_of(s));
        return;
    }
    if (ka == K_SPECIAL) {
        set_default(r, max_of(s));
        return;
    }
    if (ka == K_ZERO || ka == K_DENORM) {
        set_default(r, zero_of(s));
        return;
    }
    sf_begin();
    {
        const float64_t d = f64_div(f64_of(a), f64_of(b));

        round_f64(r, d, sf_inexact(), rm, UNF_SIGN);
    }
}

/*
 * The multiply-adds (table 5-3). `sub` subtracts rD from the product;
 * `neg` negates the rounded result, which is what makes the defaults
 * and the underflow zero of efsnmadd and efsnmsub the negation of
 * efsmadd's and efsmsub's -- the table's reading.
 *
 * The prose for efsnmadd says an infinite product's default "is used
 * for the result" without negating it, where efsnmsub's says it is
 * negated; the table negates both, and so does this.
 */
static void op_madd(res_t *r, uint32_t a, uint32_t b, uint32_t d, bool sub,
                    bool neg, uint32_t rm)
{
    const int ka = kind(a);
    const int kb = kind(b);
    const int kd = kind(d);
    const uint32_t sp = sgn(a) ^ sgn(b);
    const uint32_t sd = sgn(d) ^ (sub ? 1u : 0u); /* sign of the addend */
    bool pzero;
    bool pspecial;

    r->inv = ka == K_SPECIAL || kb == K_SPECIAL || ka == K_DENORM ||
             kb == K_DENORM || kd == K_SPECIAL || kd == K_DENORM;
    pzero = ka == K_ZERO || ka == K_DENORM || kb == K_ZERO || kb == K_DENORM;
    pspecial = !pzero && (ka == K_SPECIAL || kb == K_SPECIAL);

    if (pspecial) {
        set_default(r, max_of(sp));
    } else if (kd == K_SPECIAL) {
        set_default(r, max_of(sd));
    } else if (pzero) {
        if (kd == K_ZERO || kd == K_DENORM) {
            set_default(r, zero_of(sp == sd ? sp : (rm == RM ? 1u : 0u)));
        } else {
            set_default(r, (d & 0x7FFFFFFFu) | (sd << 31));
        }
    } else {
        float64_t fa = f64_of(a);
        float64_t fb = f64_of(b);
        float64_t fd = f64_of(flush(d) ^ (sub ? 0x80000000u : 0u));

        /*
         * The rounding direction is the un-negated result's: the
         * negation comes after rounding, so efsnmadd under RP rounds
         * the sum up and then makes it negative.
         */
        sf_begin();
        finish_sum(r, f64_mulAdd(fa, fb, fd), rm, sp, sgn(fd.v >> 32), UNF_MODE);
    }
    if (neg) {
        r->val ^= 0x80000000u;
        r->trunc ^= 0x80000000u;
    }
}

/* efssqrt (table 5-4). */
static void op_sqrt(res_t *r, uint32_t a, uint32_t rm)
{
    const int ka = kind(a);
    const uint32_t s = sgn(a);

    if (ka == K_ZERO) {
        set_default(r, a);
        return;
    }
    if (ka == K_DENORM) {
        r->inv = true;
        set_default(r, zero_of(s));
        return;
    }
    if (s != 0u) {
        r->inv = true; /* negative, NaN and infinity alike */
        set_default(r, 0x80000000u);
        return;
    }
    if (ka == K_SPECIAL) {
        r->inv = true;
        set_default(r, PMAX);
        return;
    }
    sf_begin();
    {
        const float64_t d = f64_sqrt(f64_of(a));

        round_f64(r, d, sf_inexact(), rm, UNF_MODE);
    }
}

/*
 * The ordering the compares and min/max use: "NaNs, Infinities and
 * Denorms as normalized numbers, using their values of e and f
 * directly", i.e. sign and magnitude. `zero_signed` keeps -0 below +0,
 * which min and max want and the compares do not.
 */
static int64_t order(uint32_t x, bool zero_signed)
{
    const int64_t m = (int64_t)(x & 0x7FFFFFFFu);

    if (m == 0 && !zero_signed) {
        return 0;
    }
    return (sgn(x) != 0u) ? -m - (zero_signed ? 1 : 0) : m;
}

/* efsmax and efsmin (table 5-5). */
static void op_minmax(res_t *r, uint32_t a, uint32_t b, bool max)
{
    const int64_t oa = order(a, true);
    const int64_t ob = order(b, true);
    uint32_t t;

    r->inv = kind(a) == K_SPECIAL || kind(b) == K_SPECIAL || kind(a) == K_DENORM ||
             kind(b) == K_DENORM;
    if (max) {
        t = (oa < ob) ? b : a;
    } else {
        t = (oa < ob) ? a : b;
    }
    if (is_nan(a) && !is_nan(b)) {
        t = b;
    } else if (is_nan(b) && !is_nan(a)) {
        t = a;
    }
    switch (kind(t)) {
    case K_DENORM:
        t &= 0x80000000u;
        break;
    case K_SPECIAL:
        t = max_of(sgn(t));
        break;
    default:
        break;
    }
    set_default(r, t);
}

/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

/*
 * From a 32-bit integer or fraction. `scale` is the power of two the
 * integer is divided by: 0, or 31 and 32 for the signed and unsigned
 * fractions. Exact in double precision, so only the final rounding is
 * a rounding.
 */
static void cvt_from(res_t *r, uint32_t b, bool sign, uint32_t scale, uint32_t rm)
{
    float64_t d = sign ? i32_to_f64((int32_t)b) : ui32_to_f64(b);

    if (b == 0u) {
        set_default(r, 0u);
        return;
    }
    d.v -= (uint64_t)scale << 52;
    round_f64(r, d, false, rm, UNF_SIGN);
}

/*
 * To a 32-bit integer or fraction (the pseudocode beside efsctsi and
 * its siblings). `scale` multiplies first: 0 for an integer, 31 or 32
 * for a fraction. Saturates, and a saturation is an overflow, which
 * this unit reports as FINV.
 *
 * **A negative value converted to an unsigned type** is 0 by the
 * pseudocode, which gives it a branch of its own without saying
 * whether it counts as the overflow that sets FINV. It is not
 * representable, and saturating to zero is what an overflow does, so
 * here it sets FINV -- the same choice for -0.4 as for -4e9, since
 * the pseudocode makes it before any rounding.
 */
static void cvt_to(res_t *r, uint32_t b, bool sign, uint32_t scale, uint32_t rm)
{
    const int kb = kind(b);
    const uint32_t s = sgn(b);
    const uint32_t lim_pos = sign ? 0x7FFFFFFFu : 0xFFFFFFFFu;
    const uint32_t lim_neg = sign ? 0x80000000u : 0u;
    int32_t e;
    uint64_t sig;
    uint64_t ip;
    bool g = false;
    bool x = false;
    bool up;

    if (kb == K_ZERO) {
        set_default(r, 0u);
        return;
    }
    if (kb == K_DENORM || is_nan(b)) {
        r->inv = true;
        set_default(r, 0u);
        return;
    }
    if (kb == K_SPECIAL) {
        r->inv = true;
        set_default(r, s ? lim_neg : lim_pos);
        return;
    }
    if (!sign && s != 0u) {
        r->inv = true;
        set_default(r, 0u);
        return;
    }
    /* |b| = sig * 2^(e - 23), scaled by 2^scale. */
    e = (int32_t)((b >> 23) & 0xFFu) - 127 + (int32_t)scale;
    sig = (uint64_t)((b & 0x7FFFFFu) | 0x800000u);
    if (e >= 40) {
        r->inv = true;
        set_default(r, s ? lim_neg : lim_pos);
        return;
    }
    if (e >= 23) {
        ip = sig << (uint32_t)(e - 23);
    } else if (e >= -1) {
        const uint32_t sh = (uint32_t)(23 - e);

        ip = sig >> sh;
        g = ((sig >> (sh - 1u)) & 1u) != 0u;
        x = (sig & ((1ull << (sh - 1u)) - 1u)) != 0u;
    } else {
        ip = 0u;
        g = false;
        x = true; /* below the guard bit, but not zero */
    }
    switch (rm) {
    case RN:
        up = g && (x || (ip & 1u) != 0u);
        break;
    case RP:
        up = (g || x) && s == 0u;
        break;
    case RM:
        up = (g || x) && s != 0u;
        break;
    default:
        up = false;
        break;
    }
    {
        const uint64_t mag = ip + (up ? 1u : 0u);
        const uint64_t lim = s ? (sign ? 0x80000000ull : 0u) : (uint64_t)lim_pos;

        if (mag > lim || ip > lim) {
            r->inv = true;
            set_default(r, s ? lim_neg : lim_pos);
            return;
        }
        r->val = s ? (uint32_t)(0u - (uint32_t)mag) : (uint32_t)mag;
        r->trunc = s ? (uint32_t)(0u - (uint32_t)ip) : (uint32_t)ip;
        r->inx = g || x;
        r->fg = g;
        r->fx = x;
    }
}

/* efscfh: half to single, exact for every normal half. */
static void cvt_from_half(res_t *r, uint32_t b)
{
    const uint32_t s = (b >> 15) & 1u;
    const uint32_t e = (b >> 10) & 0x1Fu;
    const uint32_t f = b & 0x3FFu;

    if (e == 0u && f == 0u) {
        set_default(r, zero_of(s));
    } else if (e == 0x1Fu) {
        r->inv = true;
        set_default(r, max_of(s));
    } else if (e == 0u) {
        r->inv = true;
        set_default(r, zero_of(s));
    } else {
        set_default(r, (s << 31) | ((e - 15u + 127u) << 23) | (f << 13));
    }
}

/*
 * efscth: single to half, rounded under FRMC. "Would not round up to
 * bmin" is read as the single-precision rule is: round with an
 * unbounded exponent, then call anything still below 2^-14 an
 * underflow.
 */
static void cvt_to_half(res_t *r, uint32_t b, uint32_t rm)
{
    const int kb = kind(b);
    const uint32_t s = sgn(b);
    const uint32_t hmax = (s << 15) | 0x7BFFu;
    int32_t e;
    uint32_t t;
    uint32_t t0;
    bool g;
    bool x;
    bool up;

    if (kb == K_ZERO) {
        set_default(r, s << 15);
        return;
    }
    if (kb == K_SPECIAL) {
        r->inv = true;
        set_default(r, hmax);
        return;
    }
    if (kb == K_DENORM) {
        r->inv = true;
        set_default(r, s << 15);
        return;
    }
    e = (int32_t)((b >> 23) & 0xFFu) - 127;
    t = ((b & 0x7FFFFFu) | 0x800000u) >> 13; /* 11 bits */
    g = ((b >> 12) & 1u) != 0u;
    x = (b & 0xFFFu) != 0u;
    t0 = t;
    switch (rm) {
    case RN:
        up = g && (x || (t & 1u) != 0u);
        break;
    case RP:
        up = (g || x) && s == 0u;
        break;
    case RM:
        up = (g || x) && s != 0u;
        break;
    default:
        up = false;
        break;
    }
    if (up) {
        t++;
        if (t == (1u << 11)) {
            t >>= 1;
            e++;
        }
    }
    r->inx = g || x;
    r->fg = g;
    r->fx = x;
    if (e > 15) {
        r->ovf = true;
        set_default(r, hmax);
        return;
    }
    if (e < -14) {
        r->unf = true;
        set_default(r, s << 15);
        return;
    }
    r->val = (s << 15) | ((uint32_t)(e + 15) << 10) | (t & 0x3FFu);
    r->trunc = ((int32_t)((b >> 23) & 0xFFu) - 127 > 15)
                   ? hmax
                   : ((s << 15) |
                      ((uint32_t)((int32_t)((b >> 23) & 0xFFu) - 127 + 15) << 10) |
                      (t0 & 0x3FFu));
}

/* ------------------------------------------------------------------ */
/* SPEFSCR and the two interrupts                                      */
/* ------------------------------------------------------------------ */

#define STATUS                                                                 \
    (PPC_SPEFSCR_FGH | PPC_SPEFSCR_FXH | PPC_SPEFSCR_FINVH | PPC_SPEFSCR_FDBZH | \
     PPC_SPEFSCR_FUNFH | PPC_SPEFSCR_FOVFH | PPC_SPEFSCR_FG | PPC_SPEFSCR_FX |  \
     PPC_SPEFSCR_FINV | PPC_SPEFSCR_FDBZ | PPC_SPEFSCR_FUNF | PPC_SPEFSCR_FOVF)

/*
 * Record the outcome and decide: suppress (data interrupt), write
 * truncated (round interrupt), or write. The per-instruction status
 * bits are replaced, the sticky ones accumulate, and the "H" bits of
 * the vector half are cleared, as every scalar instruction does.
 */
static uint32_t finish(ppc_cpu_t *c, const res_t *r, uint32_t *dst)
{
    uint32_t f = c->spefscr & ~(uint32_t)STATUS;
    const bool err = r->inv || r->dbz || r->ovf || r->unf;
    bool data;
    bool inx;

    if (r->inv) {
        f |= PPC_SPEFSCR_FINV | PPC_SPEFSCR_FINVS;
    }
    if (r->dbz) {
        f |= PPC_SPEFSCR_FDBZ | PPC_SPEFSCR_FDBZS;
    }
    if (r->unf) {
        f |= PPC_SPEFSCR_FUNF | PPC_SPEFSCR_FUNFS;
    }
    if (r->ovf) {
        f |= PPC_SPEFSCR_FOVF | PPC_SPEFSCR_FOVFS;
    }
    if (!err) {
        f |= (r->fg ? PPC_SPEFSCR_FG : 0u) | (r->fx ? PPC_SPEFSCR_FX : 0u);
    }
    data = (r->inv && (f & PPC_SPEFSCR_FINVE) != 0u) ||
           (r->dbz && (f & PPC_SPEFSCR_FDBZE) != 0u) ||
           (r->unf && (f & PPC_SPEFSCR_FUNFE) != 0u) ||
           (r->ovf && (f & PPC_SPEFSCR_FOVFE) != 0u);
    if (data) {
        c->spefscr = f;
        return (uint32_t)PPC_IVOR_FP_DATA;
    }
    inx = (r->inx && !r->inv && !r->dbz) || r->ovf || r->unf;
    if (inx) {
        f |= PPC_SPEFSCR_FINXS;
    }
    c->spefscr = f;
    if (inx && (f & PPC_SPEFSCR_FINXE) != 0u) {
        *dst = r->trunc;
        return (uint32_t)PPC_IVOR_FP_ROUND;
    }
    *dst = r->val;
    return PPC_EXC_NONE;
}

uint32_t ppc_fpu_exec(ppc_cpu_t *c, uint32_t sem, uint32_t rd, uint32_t ra,
                      uint32_t rb, uint32_t crf)
{
    const uint32_t a = c->r[ra];
    const uint32_t b = c->r[rb];
    const uint32_t rm = rmode(c);
    res_t r = {0u, 0u, false, false, false, false, false, false, false};

    switch (sem) {
    case PPC_S_EFSADD:
        op_add(&r, a, b, false, rm);
        break;
    case PPC_S_EFSSUB:
        op_add(&r, a, b, true, rm);
        break;
    case PPC_S_EFSMUL:
        op_mul(&r, a, b, rm);
        break;
    case PPC_S_EFSDIV:
        op_div(&r, a, b, rm);
        break;
    case PPC_S_EFSMADD:
        op_madd(&r, a, b, c->r[rd], false, false, rm);
        break;
    case PPC_S_EFSMSUB:
        op_madd(&r, a, b, c->r[rd], true, false, rm);
        break;
    case PPC_S_EFSNMADD:
        op_madd(&r, a, b, c->r[rd], false, true, rm);
        break;
    case PPC_S_EFSNMSUB:
        op_madd(&r, a, b, c->r[rd], true, true, rm);
        break;
    case PPC_S_EFSSQRT:
        op_sqrt(&r, a, rm);
        break;
    case PPC_S_EFSMAX:
        op_minmax(&r, a, b, true);
        break;
    case PPC_S_EFSMIN:
        op_minmax(&r, a, b, false);
        break;
    case PPC_S_EFSABS:
    case PPC_S_EFSNABS:
    case PPC_S_EFSNEG:
        /* The bits, whatever they are: only the flag says "special". */
        r.inv = kind(a) == K_SPECIAL || kind(a) == K_DENORM;
        set_default(&r, (sem == PPC_S_EFSABS)    ? (a & 0x7FFFFFFFu)
                        : (sem == PPC_S_EFSNABS) ? (a | 0x80000000u)
                                                 : (a ^ 0x80000000u));
        break;
    case PPC_S_EFSCMPGT:
    case PPC_S_EFSCMPLT:
    case PPC_S_EFSCMPEQ:
    case PPC_S_EFSTSTGT:
    case PPC_S_EFSTSTLT:
    case PPC_S_EFSTSTEQ: {
        /*
         * crfD = undefined || cl || undefined || undefined. The three
         * undefined bits are written zero, which is one of the things
         * they are allowed to be.
         */
        const int64_t oa = order(a, false);
        const int64_t ob = order(b, false);
        const bool test = sem == PPC_S_EFSTSTGT || sem == PPC_S_EFSTSTLT ||
                          sem == PPC_S_EFSTSTEQ;
        const bool cl = (sem == PPC_S_EFSCMPGT || sem == PPC_S_EFSTSTGT) ? oa > ob
                        : (sem == PPC_S_EFSCMPLT || sem == PPC_S_EFSTSTLT) ? oa < ob
                                                                           : oa == ob;
        const unsigned sh = 4u * (7u - crf);

        if (!test) {
            uint32_t unused = 0u;

            r.inv = kind(a) == K_SPECIAL || kind(b) == K_SPECIAL ||
                    kind(a) == K_DENORM || kind(b) == K_DENORM;
            if (finish(c, &r, &unused) != PPC_EXC_NONE) {
                return (uint32_t)PPC_IVOR_FP_DATA;
            }
        }
        c->cr = (c->cr & ~(0xFu << sh)) | ((cl ? PPC_CR_GT : 0u) << sh);
        return PPC_EXC_NONE;
    }
    case PPC_S_EFSCFSI:
        cvt_from(&r, b, true, 0u, rm);
        break;
    case PPC_S_EFSCFUI:
        cvt_from(&r, b, false, 0u, rm);
        break;
    case PPC_S_EFSCFSF:
        cvt_from(&r, b, true, 31u, rm);
        break;
    case PPC_S_EFSCFUF:
        cvt_from(&r, b, false, 32u, rm);
        break;
    case PPC_S_EFSCTSI:
        cvt_to(&r, b, true, 0u, rm);
        break;
    case PPC_S_EFSCTUI:
        cvt_to(&r, b, false, 0u, rm);
        break;
    case PPC_S_EFSCTSIZ:
        cvt_to(&r, b, true, 0u, RZ);
        break;
    case PPC_S_EFSCTUIZ:
        cvt_to(&r, b, false, 0u, RZ);
        break;
    case PPC_S_EFSCTSF:
        /* -1.0 is the one value with ebl == 127 that fits. */
        if (b == 0xBF800000u) {
            set_default(&r, 0x80000000u);
        } else {
            cvt_to(&r, b, true, 31u, rm);
        }
        break;
    case PPC_S_EFSCTUF:
        cvt_to(&r, b, false, 32u, rm);
        break;
    case PPC_S_EFSCFH:
        cvt_from_half(&r, b & 0xFFFFu);
        break;
    case PPC_S_EFSCTH:
        cvt_to_half(&r, b, rm);
        break;
    default:
        return (uint32_t)PPC_IVOR_PROGRAM;
    }
    return finish(c, &r, &c->r[rd]);
}
