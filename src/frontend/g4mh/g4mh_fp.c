/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_fp.c - one floating-point element, for the FPU and the FXU.
 *
 * The arithmetic is Berkeley SoftFloat, as the RISC-V and ARMv7-M
 * frontends use it. What is here is what the RH850 does around it, from
 * R01UH0923EJ0130 3.2.6.1 (the FPU) and 3.2.6.2 (the FXU), which state
 * the same rules twice:
 *
 *  - **NaNs** (tables 3.128 and 3.134, "Limitation on the Floating-Point
 *    Operation Result"): a signalling NaN anywhere, or an invalid
 *    operation on numbers, gives 7FFF FFFFH -- not IEEE's canonical
 *    7FC0 0000H, which is what this file returned before; otherwise the
 *    first quiet NaN by operand priority, reg2, reg1, reg3, unchanged.
 *  - **Subnormal operands**: with FS clear, an unimplemented-operation
 *    exception (E), which always traps; with FS set, flushed to a zero of
 *    their sign, recorded in IF, and inexact unless the operation was
 *    invalid or a division by zero (3.2.6.1(6)(a)).
 *  - **Subnormal results**, judged *before* rounding: E with FS clear,
 *    and with FS set the value of table 3.126 -- a zero, or the smallest
 *    normal toward a directed rounding -- with U and I. Under RN with FN
 *    set, the nearer of zero and the smallest normal (table 3.127).
 *  - **Conversions** of a NaN, and to an unsigned integer of a negative
 *    number (tables 3.129, 3.135, and each CVTF's page): signed NaNs give
 *    the most negative integer, unsigned give 0, and a negative number is
 *    invalid for an unsigned destination even when it would round to 0.
 *
 * MAXF, MINF, CMPF, ABSF, NEGF, CMOVF and CVTF.HS are exempt from both
 * flushing and E (3.2.6.1(8)(c), (d)).
 *
 * Unverified, because no RH850 is attached: everything above is a
 * reading of the manual, and where it is silent this file chose --
 * operand NaNs are decided before operand subnormals (so an sNaN beside
 * a subnormal is V, not E); FNMAF/FNMSF flush the fused result and then
 * negate it, as their pseudocode reads (`neg(fma(...))`); RSQRTF rounds
 * twice, as a square root and then a division.
 */

#include "g4mh_fp.h"

#include "g4mh/g4mh_config.h"
#include "g4mh/g4mh_types.h"

#if G4MH_EXT_FPU

#include "softfloat.h"

/* ------------------------------------------------------------------ */
/* Formats                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned w; /* bits                                          */
    unsigned mb; /* mantissa bits                                */
    uint64_t dnan; /* an invalid result: tables 3.128, 3.129      */
} fmt_t;

static const fmt_t F16 = {16u, 10u, 0x7FFFu};
static const fmt_t F32 = {32u, 23u, 0x7FFFFFFFu};
static const fmt_t F64 = {64u, 52u, UINT64_C(0x7FFFFFFFFFFFFFFF)};

static uint64_t sign_of(const fmt_t *f)
{
    return UINT64_C(1) << (f->w - 1u);
}
static uint64_t mant_of(const fmt_t *f)
{
    return (UINT64_C(1) << f->mb) - 1u;
}
static uint64_t exp_of(const fmt_t *f)
{
    return (sign_of(f) - 1u) & ~mant_of(f);
}
static bool is_nan(const fmt_t *f, uint64_t x)
{
    return (x & exp_of(f)) == exp_of(f) && (x & mant_of(f)) != 0u;
}
static bool is_snan(const fmt_t *f, uint64_t x)
{
    return is_nan(f, x) && (x & (UINT64_C(1) << (f->mb - 1u))) == 0u;
}
static bool is_sub(const fmt_t *f, uint64_t x)
{
    return (x & exp_of(f)) == 0u && (x & mant_of(f)) != 0u;
}
static bool is_zero(const fmt_t *f, uint64_t x)
{
    return (x & ~sign_of(f)) == 0u;
}
/* 2^Emin, the smallest normal; and 2^(Emin-1), half of it. */
static uint64_t min_normal(const fmt_t *f)
{
    return UINT64_C(1) << f->mb;
}
static uint64_t half_min(const fmt_t *f)
{
    return UINT64_C(1) << (f->mb - 1u);
}

/* ------------------------------------------------------------------ */
/* Around SoftFloat                                                    */
/* ------------------------------------------------------------------ */

/*
 * RH850 orders the modes RN/RZ/RP/RM, SoftFloat near_even/minMag/min/
 * max: RP is `max` and RM `min`, and a cast would swap them.
 */
static uint_fast8_t sf_mode(unsigned rm)
{
    switch (rm & 3u) {
    case G4MH_RM_RZ:
        return softfloat_round_minMag;
    case G4MH_RM_RP:
        return softfloat_round_max;
    case G4MH_RM_RM:
        return softfloat_round_min;
    default:
        return softfloat_round_near_even;
    }
}

/*
 * Tininess is detected *before* rounding here (3.2.6.1(6)(e)), and the
 * specialization this tree builds detects it after -- for RISC-V, which
 * shares the global. So it is set for each operation and put back.
 */
typedef struct {
    uint_fast8_t tininess;
} sf_saved_t;

static sf_saved_t sf_begin(uint_fast8_t mode)
{
    const sf_saved_t s = {softfloat_detectTininess};

    softfloat_detectTininess = softfloat_tininess_beforeRounding;
    softfloat_roundingMode = mode;
    softfloat_exceptionFlags = 0u;
    return s;
}

/* SoftFloat's flags in RH850's order, and the global put back. */
static uint32_t sf_end(sf_saved_t s)
{
    const uint_fast8_t f = softfloat_exceptionFlags;
    uint32_t out = 0u;

    softfloat_detectTininess = s.tininess;
    out |= (f & softfloat_flag_inexact) ? G4MH_FPX_I : 0u;
    out |= (f & softfloat_flag_underflow) ? G4MH_FPX_U : 0u;
    out |= (f & softfloat_flag_overflow) ? G4MH_FPX_O : 0u;
    out |= (f & softfloat_flag_infinite) ? G4MH_FPX_Z : 0u;
    out |= (f & softfloat_flag_invalid) ? G4MH_FPX_V : 0u;
    return out;
}

static float32_t f32v(uint64_t x)
{
    float32_t v;
    v.v = (uint32_t)x;
    return v;
}
static float64_t f64v(uint64_t x)
{
    float64_t v;
    v.v = x;
    return v;
}

/* The operation itself, on operands already through the rules above. */
static uint64_t sf_op(const fmt_t *f, unsigned op, uint64_t x, uint64_t y,
                      uint64_t z)
{
    if (f->w == 32u) {
        const float32_t one = f32v(0x3F800000u);

        switch (op) {
        case G4MH_FOP_ADD:
            return f32_add(f32v(x), f32v(y)).v;
        case G4MH_FOP_SUB:
            return f32_sub(f32v(x), f32v(y)).v;
        case G4MH_FOP_MUL:
            return f32_mul(f32v(x), f32v(y)).v;
        case G4MH_FOP_DIV:
            return f32_div(f32v(x), f32v(y)).v;
        case G4MH_FOP_SQRT:
            return f32_sqrt(f32v(x)).v;
        case G4MH_FOP_RECIP:
            return f32_div(one, f32v(x)).v;
        case G4MH_FOP_RSQRT:
            return f32_div(one, f32_sqrt(f32v(x))).v;
        case G4MH_FOP_FMA:
        case G4MH_FOP_FNMA:
            return f32_mulAdd(f32v(x), f32v(y), f32v(z)).v;
        default: /* FMS, FNMS: the addend negated, one rounding */
            return f32_mulAdd(f32v(x), f32v(y), f32v(z ^ 0x80000000u)).v;
        }
    }
    {
        const float64_t one = f64v(UINT64_C(0x3FF0000000000000));

        switch (op) {
        case G4MH_FOP_ADD:
            return f64_add(f64v(x), f64v(y)).v;
        case G4MH_FOP_SUB:
            return f64_sub(f64v(x), f64v(y)).v;
        case G4MH_FOP_MUL:
            return f64_mul(f64v(x), f64v(y)).v;
        case G4MH_FOP_DIV:
            return f64_div(f64v(x), f64v(y)).v;
        case G4MH_FOP_SQRT:
            return f64_sqrt(f64v(x)).v;
        case G4MH_FOP_RECIP:
            return f64_div(one, f64v(x)).v;
        default: /* RSQRT: there is no double FMA (CC-RH has no fmaf.d) */
            return f64_div(one, f64_sqrt(f64v(x))).v;
        }
    }
}

static unsigned operands(unsigned op)
{
    if (op == G4MH_FOP_SQRT || op == G4MH_FOP_RECIP || op == G4MH_FOP_RSQRT) {
        return 1u;
    }
    return (op >= G4MH_FOP_FMA) ? 3u : 2u;
}

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

/*
 * A result that was subnormal before rounding, with FS set: table 3.126,
 * and 3.127 under RN with FN. `sign` is the result's; `ge_half` says
 * |result| >= 2^(Emin-1), which only flush-to-nearest asks.
 */
static uint64_t flush_result(const g4mh_fp_t *e, const fmt_t *f, uint64_t sign,
                             bool ge_half)
{
    switch (e->rm & 3u) {
    case G4MH_RM_RZ:
        return sign;
    case G4MH_RM_RP:
        return sign ? sign : min_normal(f);
    case G4MH_RM_RM:
        return sign ? (sign | min_normal(f)) : 0u;
    default:
        return (e->fn && ge_half) ? (sign | min_normal(f)) : sign;
    }
}

/*
 * After SoftFloat: an invalid result is the product's NaN, and a tiny
 * one -- SoftFloat's before-rounding underflow when inexact, a subnormal
 * when exact -- is E or flushed. Returns false for E.
 *
 * `rz` recomputes the operation truncated, for flush-to-nearest: a
 * truncated result is at least 2^(Emin-1) exactly when the exact one is,
 * since that value is representable. Asked only on that rare path.
 */
static bool finish(g4mh_fp_t *e, const fmt_t *f, uint64_t *r, uint32_t *flags,
                   uint64_t (*rz)(const void *), const void *arg)
{
    if ((*flags & G4MH_FPX_V) != 0u) {
        *r = f->dnan;
        return true;
    }
    if ((*flags & G4MH_FPX_U) == 0u && !is_sub(f, *r)) {
        return true;
    }
    if (!e->fs) {
        /* "Because E must occur, U does not occur", nor I. */
        *flags = G4MH_FPX_E;
        return false;
    }
    {
        const bool ge = (e->rm & 3u) == G4MH_RM_RN && e->fn && rz != NULL &&
                        (rz(arg) & ~sign_of(f)) >= half_min(f);

        *r = flush_result(e, f, *r & sign_of(f), ge);
    }
    *flags |= G4MH_FPX_U | G4MH_FPX_I;
    return true;
}

/* An input flushed: IF, and inexact unless V or Z (3.2.6.1(6)(a)). */
static void flushed_in(g4mh_fp_t *e, uint32_t *flags)
{
    e->flushed = true;
    if ((*flags & (G4MH_FPX_V | G4MH_FPX_Z)) == 0u) {
        *flags |= G4MH_FPX_I;
    }
}

typedef struct {
    const fmt_t *f;
    unsigned op;
    uint64_t in[3];
} op_args_t;

static uint64_t op_rz(const void *p)
{
    const op_args_t *a = (const op_args_t *)p;
    const sf_saved_t s = sf_begin(softfloat_round_minMag);
    const uint64_t r = sf_op(a->f, a->op, a->in[0], a->in[1], a->in[2]);

    (void)sf_end(s);
    return r;
}

static uint64_t arith(g4mh_fp_t *e, const fmt_t *f, unsigned op, uint64_t x,
                      uint64_t y, uint64_t z)
{
    const unsigned n = operands(op);
    op_args_t a = {f, op, {x, y, z}};
    bool fl = false;
    uint32_t flags;
    uint64_t r;

    for (unsigned i = 0u; i < n; i++) {
        if (is_snan(f, a.in[i])) {
            e->flags |= G4MH_FPX_V;
            return f->dnan;
        }
    }
    for (unsigned i = 0u; i < n; i++) {
        if (is_nan(f, a.in[i])) {
            return a.in[i]; /* the first quiet NaN, as it was */
        }
    }
    for (unsigned i = 0u; i < n; i++) {
        if (is_sub(f, a.in[i])) {
            if (!e->fs) {
                e->flags |= G4MH_FPX_E;
                return 0u;
            }
            a.in[i] &= sign_of(f);
            fl = true;
        }
    }
    {
        const sf_saved_t s = sf_begin(sf_mode(e->rm));

        r = sf_op(f, op, a.in[0], a.in[1], a.in[2]);
        flags = sf_end(s);
    }
    if (!finish(e, f, &r, &flags, op_rz, &a)) {
        e->flags |= flags;
        return 0u;
    }
    if ((op == G4MH_FOP_FNMA || op == G4MH_FOP_FNMS) && !is_nan(f, r)) {
        r ^= sign_of(f);
    }
    if (fl) {
        flushed_in(e, &flags);
    }
    e->flags |= flags;
    return r;
}

uint32_t g4mh_fp32_op(g4mh_fp_t *e, unsigned op, uint32_t x, uint32_t y,
                      uint32_t z)
{
    return (uint32_t)arith(e, &F32, op, x, y, z);
}

uint64_t g4mh_fp64_op(g4mh_fp_t *e, unsigned op, uint64_t x, uint64_t y)
{
    return arith(e, &F64, op, x, y, 0u);
}

/* ------------------------------------------------------------------ */
/* MAXF, MINF, CMPF: no flushing, no E                                 */
/* ------------------------------------------------------------------ */

/* x < y and x == y on two numbers, subnormals compared as they are. */
static bool num_lt(const fmt_t *f, uint64_t x, uint64_t y)
{
    return (f->w == 32u) ? f32_lt_quiet(f32v(x), f32v(y))
                         : f64_lt_quiet(f64v(x), f64v(y));
}
static bool num_eq(const fmt_t *f, uint64_t x, uint64_t y)
{
    return (f->w == 32u) ? f32_eq(f32v(x), f32v(y)) : f64_eq(f64v(x), f64v(y));
}

static uint64_t maxmin(g4mh_fp_t *e, const fmt_t *f, bool max, uint64_t a,
                       uint64_t b)
{
    if (is_snan(f, a) || is_snan(f, b)) {
        e->flags |= G4MH_FPX_V;
        return f->dnan;
    }
    if (is_nan(f, a)) {
        return is_nan(f, b) ? a : b;
    }
    if (is_nan(f, b)) {
        return a;
    }
    if (is_zero(f, a) && is_zero(f, b)) {
        return max ? a : b; /* the operand, not a sign rule */
    }
    {
        const bool lt = num_lt(f, a, b);

        return (lt == max) ? b : a;
    }
}

uint32_t g4mh_fp32_maxmin(g4mh_fp_t *e, bool max, uint32_t first,
                          uint32_t second)
{
    return (uint32_t)maxmin(e, &F32, max, first, second);
}

uint64_t g4mh_fp64_maxmin(g4mh_fp_t *e, bool max, uint64_t first,
                          uint64_t second)
{
    return maxmin(e, &F64, max, first, second);
}

/*
 * The sixteen conditions as a mask -- fcond[2] less, [1] equal, [0]
 * unordered, [3] invalid when unordered -- with the relation reg2 < reg1.
 * A signalling NaN is invalid whatever fcond says: "when SignalingNaN is
 * acknowledged as an operand value in a floating-point instruction
 * (including a comparison), it is regarded as an invalid operation".
 */
static bool cmp(g4mh_fp_t *e, const fmt_t *f, uint32_t fcond, uint64_t x,
                uint64_t y)
{
    const bool un = is_nan(f, x) || is_nan(f, y);
    const bool lt = !un && num_lt(f, x, y);
    const bool eq = !un && num_eq(f, x, y);

    if (is_snan(f, x) || is_snan(f, y) || (un && (fcond & 8u) != 0u)) {
        e->flags |= G4MH_FPX_V;
    }
    return ((fcond & 4u) && lt) || ((fcond & 2u) && eq) || ((fcond & 1u) && un);
}

bool g4mh_fp32_cmp(g4mh_fp_t *e, uint32_t fcond, uint32_t x, uint32_t y)
{
    return cmp(e, &F32, fcond, x, y);
}

bool g4mh_fp64_cmp(g4mh_fp_t *e, uint32_t fcond, uint64_t x, uint64_t y)
{
    return cmp(e, &F64, fcond, x, y);
}

/* ------------------------------------------------------------------ */
/* To and from integers                                                */
/* ------------------------------------------------------------------ */

static uint64_t to_int(g4mh_fp_t *e, const fmt_t *f, uint64_t x, unsigned mode,
                       bool uns, bool wide)
{
    const uint64_t most_neg = wide ? (UINT64_C(1) << 63) : 0x80000000u;
    uint_fast8_t rm;
    bool fl = false;
    uint32_t flags;
    uint64_t r;

    /* A NaN, signalling or quiet: invalid, and the most negative signed
     * integer or 0 (table 3.129). */
    if (is_nan(f, x)) {
        e->flags |= G4MH_FPX_V;
        return uns ? 0u : most_neg;
    }
    if (is_sub(f, x)) {
        if (!e->fs) {
            e->flags |= G4MH_FPX_E;
            return 0u;
        }
        x &= sign_of(f);
        fl = true;
    }
    /* Any negative number is invalid to an unsigned destination, 0 --
     * "-Normal: 0[V]" on every .SUW/.DUW page, -0.5 included. */
    if (uns && (x & sign_of(f)) != 0u && !is_zero(f, x)) {
        e->flags |= G4MH_FPX_V;
        return 0u;
    }
    switch (mode) {
    case G4MH_CVT_RN:
        rm = softfloat_round_near_even;
        break;
    case G4MH_CVT_RZ:
        rm = softfloat_round_minMag;
        break;
    case G4MH_CVT_RP:
        rm = softfloat_round_max;
        break;
    case G4MH_CVT_RM:
        rm = softfloat_round_min;
        break;
    default:
        rm = sf_mode(e->rm);
        break;
    }
    {
        const sf_saved_t s = sf_begin(rm);

        /* Out of range saturates, positive to the largest and negative
         * to the most negative: SoftFloat's RISC-V choices, and the
         * manual's ("+Max Int[V]", "-Max Int[V]"). */
        if (f->w == 32u) {
            r = wide  ? (uns ? f32_to_ui64(f32v(x), rm, true)
                             : (uint64_t)f32_to_i64(f32v(x), rm, true))
                : uns ? f32_to_ui32(f32v(x), rm, true)
                      : (uint32_t)f32_to_i32(f32v(x), rm, true);
        } else {
            r = wide  ? (uns ? f64_to_ui64(f64v(x), rm, true)
                             : (uint64_t)f64_to_i64(f64v(x), rm, true))
                : uns ? f64_to_ui32(f64v(x), rm, true)
                      : (uint32_t)f64_to_i32(f64v(x), rm, true);
        }
        flags = sf_end(s);
    }
    if (fl) {
        flushed_in(e, &flags);
    }
    e->flags |= flags;
    return r;
}

uint64_t g4mh_fp32_to_int(g4mh_fp_t *e, uint32_t x, unsigned mode, bool uns,
                          bool wide)
{
    return to_int(e, &F32, x, mode, uns, wide);
}

uint64_t g4mh_fp64_to_int(g4mh_fp_t *e, uint64_t x, unsigned mode, bool uns,
                          bool wide)
{
    return to_int(e, &F64, x, mode, uns, wide);
}

/* Never tiny, never invalid: only inexact. */
uint32_t g4mh_int_to_fp32(g4mh_fp_t *e, uint64_t v, bool uns, bool wide)
{
    const sf_saved_t s = sf_begin(sf_mode(e->rm));
    const uint32_t r = wide  ? (uns ? ui64_to_f32(v).v : i64_to_f32((int64_t)v).v)
                       : uns ? ui32_to_f32((uint32_t)v).v
                             : i32_to_f32((int32_t)(uint32_t)v).v;

    e->flags |= sf_end(s);
    return r;
}

uint64_t g4mh_int_to_fp64(g4mh_fp_t *e, uint64_t v, bool uns, bool wide)
{
    const sf_saved_t s = sf_begin(sf_mode(e->rm));
    const uint64_t r = wide  ? (uns ? ui64_to_f64(v).v : i64_to_f64((int64_t)v).v)
                       : uns ? ui32_to_f64((uint32_t)v).v
                             : i32_to_f64((int32_t)(uint32_t)v).v;

    e->flags |= sf_end(s);
    return r;
}

/* ------------------------------------------------------------------ */
/* Between formats                                                     */
/* ------------------------------------------------------------------ */

/*
 * A quiet NaN keeps its sign and its payload's top bits, shifted into
 * place; a signalling one is the destination's invalid result, with V
 * (table 3.129). SoftFloat's RISC-V specialization makes every NaN
 * canonical, which is why these are done here.
 */
static bool nan_convert(g4mh_fp_t *e, const fmt_t *from, const fmt_t *to,
                        uint64_t x, uint64_t *out)
{
    if (!is_nan(from, x)) {
        return false;
    }
    if (is_snan(from, x)) {
        e->flags |= G4MH_FPX_V;
        *out = to->dnan;
        return true;
    }
    {
        uint64_t m = x & mant_of(from);

        m = (to->mb > from->mb) ? m << (to->mb - from->mb)
                                : m >> (from->mb - to->mb);
        *out = ((x & sign_of(from)) ? sign_of(to) : 0u) | exp_of(to) | m;
    }
    return true;
}

/* A subnormal input of a conversion that flushes: E, or a signed zero. */
static bool flush_conv_in(g4mh_fp_t *e, const fmt_t *f, uint64_t *x, bool *fl)
{
    if (!is_sub(f, *x)) {
        return true;
    }
    if (!e->fs) {
        e->flags |= G4MH_FPX_E;
        return false;
    }
    *x &= sign_of(f);
    *fl = true;
    return true;
}

typedef struct {
    const fmt_t *from;
    uint64_t x;
} cvt_args_t;

static uint64_t narrow_sf(const fmt_t *from, uint64_t x)
{
    return (from->w == 64u) ? f64_to_f32(f64v(x)).v : f32_to_f16(f32v(x)).v;
}

static uint64_t narrow_rz(const void *p)
{
    const cvt_args_t *a = (const cvt_args_t *)p;
    const sf_saved_t s = sf_begin(softfloat_round_minMag);
    const uint64_t r = narrow_sf(a->from, a->x);

    (void)sf_end(s);
    return r;
}

/* Double to single, single to half: the result may be tiny or huge. */
static uint64_t narrow(g4mh_fp_t *e, const fmt_t *from, const fmt_t *to,
                       uint64_t x)
{
    cvt_args_t a = {from, x};
    bool fl = false;
    uint32_t flags;
    uint64_t r;

    if (nan_convert(e, from, to, x, &r)) {
        return r;
    }
    if (!flush_conv_in(e, from, &a.x, &fl)) {
        return 0u;
    }
    {
        const sf_saved_t s = sf_begin(sf_mode(e->rm));

        r = narrow_sf(from, a.x);
        flags = sf_end(s);
    }
    if (!finish(e, to, &r, &flags, narrow_rz, &a)) {
        e->flags |= flags;
        return 0u;
    }
    if (fl) {
        flushed_in(e, &flags);
    }
    e->flags |= flags;
    return r;
}

uint64_t g4mh_fp32_to_fp64(g4mh_fp_t *e, uint32_t x)
{
    uint64_t r;
    uint64_t v = x;
    bool fl = false;
    uint32_t flags = 0u;

    if (nan_convert(e, &F32, &F64, x, &r)) {
        return r;
    }
    if (!flush_conv_in(e, &F32, &v, &fl)) {
        return 0u;
    }
    r = f32_to_f64(f32v(v)).v; /* exact */
    if (fl) {
        flushed_in(e, &flags);
    }
    e->flags |= flags;
    return r;
}

uint32_t g4mh_fp64_to_fp32(g4mh_fp_t *e, uint64_t x)
{
    return (uint32_t)narrow(e, &F64, &F32, x);
}

uint32_t g4mh_fp32_to_fp16(g4mh_fp_t *e, uint32_t x)
{
    return (uint32_t)narrow(e, &F32, &F16, x);
}

/*
 * Half to single is exact for every half, subnormals included: CVTF.HS
 * is on the list of instructions neither flushed nor E (3.2.6.1(8)(c)),
 * "accurately convert[s] data in all half-precision floating-point
 * formats except not-a-number".
 */
uint32_t g4mh_fp16_to_fp32(g4mh_fp_t *e, uint32_t h)
{
    uint64_t r;
    float16_t v;

    if (nan_convert(e, &F16, &F32, h & 0xFFFFu, &r)) {
        return (uint32_t)r;
    }
    v.v = (uint16_t)h;
    {
        const sf_saved_t s = sf_begin(softfloat_round_near_even);

        r = f16_to_f32(v).v;
        (void)sf_end(s);
    }
    return (uint32_t)r;
}

#endif /* G4MH_EXT_FPU */
