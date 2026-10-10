/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_fpu.c - the coprocessor space: FPv5, single precision, as the
 * F746's Cortex-M7 implements it.
 *
 * **SoftFloat is the arithmetic, and ARM's rules are applied around it.**
 * This tree has one FP implementation (CLAUDE.md: "There is one FP
 * implementation, and both backends reach it"), built with SoftFloat's
 * RISC-V specialization -- which returns the default NaN for every NaN
 * operand, detects tininess after rounding, and converts NaN to the
 * *largest* integer. ARM differs on all three, so each operation here
 * follows the ARM ARM's pseudocode shape: FPUnpack (flushing a denormal
 * input to zero under FZ, raising IDC), FPProcessNaNs (the first SNaN,
 * quieted, else the first QNaN, unless DN), then the arithmetic, then
 * FPRound's flush of a tiny result to zero under FZ (raising UFC, not
 * IXC). Conversions to integer and fixed point are done exactly in
 * integer arithmetic rather than through SoftFloat, whose NaN answer is
 * the wrong one.
 *
 * Double precision does not exist on this part -- MVFR0 says so, and
 * every sz=1 data-processing encoding is UNDEFINED -- but D registers do,
 * as pairs of S registers, for the loads, stores and moves.
 */

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_bus.h"
#include "softfloat.h"

typedef armv7m_exc_t X;
#define OK ARMV7M_X_NONE

#define FPSCR_IOC 0x01u
#define FPSCR_DZC 0x02u
#define FPSCR_OFC 0x04u
#define FPSCR_UFC 0x08u
#define FPSCR_IXC 0x10u
#define FPSCR_IDC 0x80u
#define FPSCR_FZ (1u << 24)
#define FPSCR_DN (1u << 25)
#define FPSCR_AHP (1u << 26)
#define FPSCR_MODE_MASK 0x07C00000u
#define FPSCR_NZCV_MASK 0xF0000000u
#define FPSCR_WRITABLE 0xF7C0009Fu

#define FPCCR_ASPEN (1u << 31)
#define FPCCR_LSPACT 1u

#define DEFAULT_NAN 0x7FC00000u
#define SIGN 0x80000000u

/* RMode: nearest, +inf, -inf, zero; and FPv5's ties-away. */
enum { RM_RN, RM_RP, RM_RM, RM_RZ, RM_RA };

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

static inline bool is_nan(uint32_t v)
{
    return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu) != 0u;
}

static inline bool is_snan(uint32_t v)
{
    return is_nan(v) && (v & 0x00400000u) == 0u;
}

static inline bool is_qnan(uint32_t v)
{
    return is_nan(v) && (v & 0x00400000u) != 0u;
}

static inline bool is_inf(uint32_t v)
{
    return (v & 0x7FFFFFFFu) == 0x7F800000u;
}

static inline bool is_zero(uint32_t v)
{
    return (v & 0x7FFFFFFFu) == 0u;
}

static inline bool is_denorm(uint32_t v)
{
    return (v & 0x7F800000u) == 0u && (v & 0x007FFFFFu) != 0u;
}

/* ------------------------------------------------------------------ */
/* The pseudocode's helpers                                            */
/* ------------------------------------------------------------------ */

/* FPUnpack's flush: a denormal input under FZ is a zero, and IDC. */
static uint32_t unpack(armv7m_cpu_t *c, uint32_t v)
{
    if ((c->fpscr & FPSCR_FZ) != 0u && is_denorm(v)) {
        c->fpscr |= FPSCR_IDC;
        return v & SIGN;
    }
    return v;
}

static uint32_t process_nan(armv7m_cpu_t *c, uint32_t v)
{
    if (is_snan(v)) {
        v |= 0x00400000u;
        c->fpscr |= FPSCR_IOC;
    }
    return ((c->fpscr & FPSCR_DN) != 0u) ? DEFAULT_NAN : v;
}

/* FPProcessNaNs: signalling before quiet, then operand order. */
static bool process_nans(armv7m_cpu_t *c, uint32_t a, uint32_t b, uint32_t *r)
{
    if (is_snan(a)) {
        *r = process_nan(c, a);
    } else if (is_snan(b)) {
        *r = process_nan(c, b);
    } else if (is_qnan(a)) {
        *r = process_nan(c, a);
    } else if (is_qnan(b)) {
        *r = process_nan(c, b);
    } else {
        return false;
    }
    return true;
}

static bool process_nans3(armv7m_cpu_t *c, uint32_t a, uint32_t b, uint32_t d,
                          uint32_t *r)
{
    if (is_snan(a)) {
        *r = process_nan(c, a);
    } else if (is_snan(b)) {
        *r = process_nan(c, b);
    } else if (is_snan(d)) {
        *r = process_nan(c, d);
    } else if (is_qnan(a)) {
        *r = process_nan(c, a);
    } else if (is_qnan(b)) {
        *r = process_nan(c, b);
    } else if (is_qnan(d)) {
        *r = process_nan(c, d);
    } else {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Around SoftFloat                                                    */
/* ------------------------------------------------------------------ */

static uint_fast8_t g_saved_tininess;

static uint_fast8_t sf_mode(uint32_t rm)
{
    switch (rm) {
    case RM_RP:
        return softfloat_round_max;
    case RM_RM:
        return softfloat_round_min;
    case RM_RZ:
        return softfloat_round_minMag;
    case RM_RA:
        return softfloat_round_near_maxMag;
    default:
        return softfloat_round_near_even;
    }
}

/*
 * ARM detects tininess *before* rounding, and the specialization this
 * tree builds detects it after. The global is restored on the way out,
 * because the RV32 and G4MH frontends share it and rely on its default.
 */
static void sf_begin(uint32_t rm)
{
    g_saved_tininess = softfloat_detectTininess;
    softfloat_detectTininess = softfloat_tininess_beforeRounding;
    softfloat_roundingMode = sf_mode(rm);
    softfloat_exceptionFlags = 0u;
}

/*
 * The flags, and FPRound's flush-to-zero: a result whose *unrounded*
 * exponent is below the normal range becomes a signed zero under FZ,
 * with UFC and without IXC. "Unrounded below the range" is exactly what
 * SoftFloat's before-rounding underflow says when it is inexact, and a
 * subnormal result when it is exact.
 */
static uint32_t sf_end(armv7m_cpu_t *c, uint32_t r)
{
    uint_fast8_t f = softfloat_exceptionFlags;

    softfloat_detectTininess = g_saved_tininess;
    if ((c->fpscr & FPSCR_FZ) != 0u &&
        (is_denorm(r) || (f & softfloat_flag_underflow) != 0u)) {
        r &= SIGN;
        c->fpscr |= FPSCR_UFC;
        f &= (uint_fast8_t) ~(softfloat_flag_underflow | softfloat_flag_inexact);
    }
    if ((f & softfloat_flag_invalid) != 0u) {
        c->fpscr |= FPSCR_IOC;
    }
    if ((f & softfloat_flag_infinite) != 0u) {
        c->fpscr |= FPSCR_DZC;
    }
    if ((f & softfloat_flag_overflow) != 0u) {
        c->fpscr |= FPSCR_OFC;
    }
    if ((f & softfloat_flag_underflow) != 0u) {
        c->fpscr |= FPSCR_UFC;
    }
    if ((f & softfloat_flag_inexact) != 0u) {
        c->fpscr |= FPSCR_IXC;
    }
    return r;
}

static inline uint32_t rmode(const armv7m_cpu_t *c)
{
    return (c->fpscr >> 22) & 3u;
}

static inline float32_t f32(uint32_t v)
{
    float32_t f;

    f.v = v;
    return f;
}

/* ------------------------------------------------------------------ */
/* The operations                                                      */
/* ------------------------------------------------------------------ */

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV };

static uint32_t fp_binop(armv7m_cpu_t *c, uint32_t op, uint32_t a, uint32_t b)
{
    uint32_t r;

    a = unpack(c, a);
    b = unpack(c, b);
    if (process_nans(c, a, b, &r)) {
        return r;
    }
    sf_begin(rmode(c));
    switch (op) {
    case OP_ADD:
        r = f32_add(f32(a), f32(b)).v;
        break;
    case OP_SUB:
        r = f32_sub(f32(a), f32(b)).v;
        break;
    case OP_MUL:
        r = f32_mul(f32(a), f32(b)).v;
        break;
    default:
        r = f32_div(f32(a), f32(b)).v;
        break;
    }
    return sf_end(c, r);
}

/* FPMulAdd: addend + x*y, one rounding. */
static uint32_t fp_muladd(armv7m_cpu_t *c, uint32_t addend, uint32_t x,
                          uint32_t y)
{
    uint32_t r;
    bool done;

    addend = unpack(c, addend);
    x = unpack(c, x);
    y = unpack(c, y);
    done = process_nans3(c, addend, x, y, &r);
    /* A quiet-NaN addend does not excuse an infinity times zero. */
    if (is_qnan(addend) && ((is_inf(x) && is_zero(y)) || (is_zero(x) && is_inf(y)))) {
        c->fpscr |= FPSCR_IOC;
        return DEFAULT_NAN;
    }
    if (done) {
        return r;
    }
    sf_begin(rmode(c));
    r = f32_mulAdd(f32(x), f32(y), f32(addend)).v;
    return sf_end(c, r);
}

static uint32_t fp_sqrt(armv7m_cpu_t *c, uint32_t a)
{
    a = unpack(c, a);
    if (is_nan(a)) {
        return process_nan(c, a);
    }
    sf_begin(rmode(c));
    return sf_end(c, f32_sqrt(f32(a)).v);
}

/* FPCompare, into FPSCR.NZCV. `e` is VCMPE: quiet NaNs signal too. */
static void fp_cmp(armv7m_cpu_t *c, uint32_t a, uint32_t b, bool e)
{
    uint32_t nzcv;

    a = unpack(c, a);
    b = unpack(c, b);
    if (is_nan(a) || is_nan(b)) {
        nzcv = 0x3u;
        if (e || is_snan(a) || is_snan(b)) {
            c->fpscr |= FPSCR_IOC;
        }
    } else if ((is_zero(a) && is_zero(b)) || a == b) {
        nzcv = 0x6u;
    } else {
        /* Sign-magnitude order: compare as integers, flipped if negative. */
        const int32_t ia = (a & SIGN) ? -(int32_t)(a & 0x7FFFFFFFu) : (int32_t)a;
        const int32_t ib = (b & SIGN) ? -(int32_t)(b & 0x7FFFFFFFu) : (int32_t)b;

        nzcv = (ia < ib) ? 0x8u : 0x2u;
    }
    c->fpscr = (c->fpscr & ~FPSCR_NZCV_MASK) | (nzcv << 28);
}

/* FPMaxNum / FPMinNum: a quiet NaN loses to a number. */
static uint32_t fp_maxmin(armv7m_cpu_t *c, uint32_t a, uint32_t b, bool max)
{
    uint32_t r;

    a = unpack(c, a);
    b = unpack(c, b);
    if (is_qnan(a) && !is_nan(b)) {
        a = max ? 0xFF800000u : 0x7F800000u;
    } else if (is_qnan(b) && !is_nan(a)) {
        b = max ? 0xFF800000u : 0x7F800000u;
    }
    if (process_nans(c, a, b, &r)) {
        return r;
    }
    if (is_zero(a) && is_zero(b)) {
        /* +0 is the larger of the two zeros. */
        return max ? (a & b) : (a | b);
    }
    {
        const int32_t ia = (a & SIGN) ? -(int32_t)(a & 0x7FFFFFFFu) : (int32_t)a;
        const int32_t ib = (b & SIGN) ? -(int32_t)(b & 0x7FFFFFFFu) : (int32_t)b;

        return ((ia > ib) == max) ? a : b;
    }
}

/* FPRoundInt: VRINT*. `exact` is VRINTX, the one that raises IXC. */
static uint32_t fp_round_int(armv7m_cpu_t *c, uint32_t a, uint32_t rm, bool exact)
{
    uint32_t r;

    a = unpack(c, a);
    if (is_nan(a)) {
        return process_nan(c, a);
    }
    if (is_inf(a) || is_zero(a)) {
        return a;
    }
    sf_begin(rm);
    r = f32_roundToInt(f32(a), sf_mode(rm), exact).v;
    softfloat_detectTininess = g_saved_tininess;
    if ((softfloat_exceptionFlags & softfloat_flag_inexact) != 0u) {
        c->fpscr |= FPSCR_IXC;
    }
    return r;
}

/*
 * FPToFixed, exactly: value * 2^fbits, rounded, saturated to `size`
 * bits. NaN is zero with IOC -- not the largest integer, which is what
 * SoftFloat's RISC-V specialization would give -- and saturation is IOC
 * without IXC.
 */
static uint32_t fp_to_fixed(armv7m_cpu_t *c, uint32_t a, uint32_t size,
                            uint32_t fbits, bool is_unsigned, uint32_t rm)
{
    const int64_t hi = is_unsigned ? (int64_t)((1ull << size) - 1u)
                                   : (int64_t)((1ull << (size - 1u)) - 1u);
    const int64_t lo = is_unsigned ? 0 : -(int64_t)(1ull << (size - 1u));
    const bool neg = (a & SIGN) != 0u;
    const int32_t exp = (int32_t)((a >> 23) & 0xFFu);
    uint64_t mant = a & 0x007FFFFFu;
    int64_t v;
    bool inexact = false;

    a = unpack(c, a);
    if (is_nan(a)) {
        c->fpscr |= FPSCR_IOC;
        return 0u;
    }
    if (is_zero(a)) {
        return 0u;
    }
    if (is_inf(a)) {
        c->fpscr |= FPSCR_IOC;
        return (uint32_t)(neg ? lo : hi);
    }
    if (exp != 0) {
        mant |= 0x00800000u;
    }
    {
        /* value = mant * 2^(e) with e the exponent of the lowest bit. */
        const int32_t e = ((exp != 0) ? exp : 1) - 127 - 23 + (int32_t)fbits;
        uint64_t ip;
        uint64_t frac_num;
        uint64_t frac_den;

        if (e >= 0) {
            /*
             * Past 2^32 every destination saturates, and stopping here
             * keeps `mant << e` well inside a signed 64-bit value -- at
             * e = 40 it would not be, and would saturate the wrong way.
             */
            if (e > 32) {
                c->fpscr |= FPSCR_IOC;
                return (uint32_t)(neg ? lo : hi);
            }
            ip = mant << e;
            frac_num = 0u;
            frac_den = 1u;
        } else if (e > -64) {
            const uint32_t s = (uint32_t)-e;

            ip = (s >= 64u) ? 0u : (mant >> s);
            frac_den = (s >= 63u) ? 0u : (1ull << s);
            frac_num = (s >= 64u) ? mant : (mant & ((1ull << s) - 1u));
        } else {
            ip = 0u;
            frac_num = mant;
            frac_den = 0u; /* i.e. 2^64: the fraction is tiny but non-zero */
        }
        if (frac_num != 0u) {
            /* Compare the fraction against one half. */
            int half;

            inexact = true;
            if (frac_den == 0u) {
                half = -1;
            } else {
                const uint64_t twice = frac_num * 2u;

                half = (twice < frac_den) ? -1 : ((twice == frac_den) ? 0 : 1);
            }
            switch (rm) {
            case RM_RN:
                if (half > 0 || (half == 0 && (ip & 1u))) {
                    ip++;
                }
                break;
            case RM_RA:
                if (half >= 0) {
                    ip++;
                }
                break;
            case RM_RP:
                if (!neg) {
                    ip++;
                }
                break;
            case RM_RM:
                if (neg) {
                    ip++;
                }
                break;
            default:
                break; /* toward zero: magnitude truncates */
            }
        }
        v = neg ? -(int64_t)ip : (int64_t)ip;
    }
    if (v > hi || v < lo) {
        c->fpscr |= FPSCR_IOC;
        return (uint32_t)((v > hi) ? hi : lo);
    }
    if (inexact) {
        c->fpscr |= FPSCR_IXC;
    }
    return (uint32_t)v;
}

/* FixedToFP: an integer over 2^fbits, rounded once. */
static uint32_t fixed_to_fp(armv7m_cpu_t *c, uint32_t v, uint32_t size,
                            uint32_t fbits, bool is_unsigned, uint32_t rm)
{
    int64_t iv;
    uint32_t r;

    if (size == 16u) {
        iv = is_unsigned ? (int64_t)(v & 0xFFFFu) : (int64_t)(int16_t)v;
    } else {
        iv = is_unsigned ? (int64_t)v : (int64_t)(int32_t)v;
    }
    if (iv == 0) {
        return 0u;
    }
    sf_begin(rm);
    r = is_unsigned ? ui32_to_f32((uint32_t)iv).v : i32_to_f32((int32_t)iv).v;
    softfloat_detectTininess = g_saved_tininess;
    /* Dividing by 2^fbits is exact: the result is at least 2^-32. */
    if (fbits != 0u) {
        r -= fbits << 23;
    }
    if ((softfloat_exceptionFlags & softfloat_flag_inexact) != 0u) {
        c->fpscr |= FPSCR_IXC;
    }
    return r;
}

/* Half precision: VCVTB and VCVTT. */
static uint32_t half_to_single(armv7m_cpu_t *c, uint32_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t frac = h & 0x3FFu;

    if (exp == 0x1Fu && (c->fpscr & FPSCR_AHP) == 0u) {
        if (frac == 0u) {
            return sign | 0x7F800000u;
        }
        {
            uint32_t r = sign | 0x7F800000u | (frac << 13);

            if ((frac & 0x200u) == 0u) {
                c->fpscr |= FPSCR_IOC;
                r |= 0x00400000u;
            }
            return ((c->fpscr & FPSCR_DN) != 0u) ? DEFAULT_NAN : r;
        }
    }
    if (exp == 0u && frac == 0u) {
        return sign;
    }
    {
        float16_t f;

        f.v = (uint16_t)(h & 0x7FFFu);
        if (exp == 0x1Fu) {
            /* AHP: no infinities or NaNs, the top exponent is a number. */
            const uint32_t m = 0x400u | frac;
            const uint32_t v = (uint32_t)(m << 13);

            return sign | ((uint32_t)(31 - 15 + 127) << 23) | (v & 0x007FFFFFu);
        }
        sf_begin(RM_RN);
        {
            const uint32_t r = f16_to_f32(f).v;

            softfloat_detectTininess = g_saved_tininess;
            return sign | r;
        }
    }
}

static uint32_t single_to_half(armv7m_cpu_t *c, uint32_t a)
{
    const uint32_t sign = (a >> 16) & 0x8000u;
    const bool ahp = (c->fpscr & FPSCR_AHP) != 0u;

    a = unpack(c, a);
    if (is_nan(a)) {
        if (ahp) {
            c->fpscr |= FPSCR_IOC;
            return sign;
        }
        if (is_snan(a)) {
            c->fpscr |= FPSCR_IOC;
        }
        if ((c->fpscr & FPSCR_DN) != 0u) {
            return 0x7E00u;
        }
        return sign | 0x7E00u | ((a >> 13) & 0x1FFu);
    }
    if (is_inf(a)) {
        if (ahp) {
            c->fpscr |= FPSCR_IOC;
            return sign | 0x7FFFu;
        }
        return sign | 0x7C00u;
    }
    if (is_zero(a)) {
        return sign;
    }
    if (ahp && (a & 0x7FFFFFFFu) >= 0x47000000u) {
        /*
         * The alternative format spends exponent 31 on numbers rather
         * than on infinity and NaN, so it reaches 131008. Converting half
         * the value and putting the halving back into the exponent is
         * exact here -- at 2^15 and above the halved value is a normal
         * half -- and a result that still overflows saturates, with IOC
         * rather than OFC.
         */
        sf_begin(rmode(c));
        {
            /*
             * **With its sign.** Halving the magnitude alone rounded a
             * negative number as though it were positive, so toward
             * minus infinity went toward zero: -108885.19 under RM came
             * out 0xFEA5 where the board gives 0xFEA6. It took a random
             * FPSCR against a random operand to find -- alternative
             * half precision, a directed mode, a negative value and a
             * magnitude past 2^15, all at once.
             */
            const uint32_t r = f32_to_f16(f32(a - (1u << 23))).v & 0x7FFFu;
            const uint_fast8_t f = softfloat_exceptionFlags;

            softfloat_detectTininess = g_saved_tininess;
            /*
             * Overflow is the *flag*, not an infinite result: rounding
             * toward zero answers an overflow with the largest finite
             * half, 0x7BFF, and testing for 0x7C00 missed it -- the board
             * saturated with IOC where this reported a rounded IXC.
             */
            if (r >= 0x7C00u || (f & softfloat_flag_overflow) != 0u) {
                c->fpscr |= FPSCR_IOC;
                return sign | 0x7FFFu;
            }
            if ((f & softfloat_flag_inexact) != 0u) {
                c->fpscr |= FPSCR_IXC;
            }
            return sign | (r + 0x0400u);
        }
    }
    sf_begin(rmode(c));
    {
        const uint32_t r = f32_to_f16(f32(a)).v;
        const uint_fast8_t f = softfloat_exceptionFlags;

        softfloat_detectTininess = g_saved_tininess;
        if ((f & softfloat_flag_invalid) != 0u) {
            c->fpscr |= FPSCR_IOC;
        }
        if ((f & softfloat_flag_overflow) != 0u) {
            c->fpscr |= FPSCR_OFC;
        }
        if ((f & softfloat_flag_underflow) != 0u) {
            c->fpscr |= FPSCR_UFC;
        }
        if ((f & softfloat_flag_inexact) != 0u) {
            c->fpscr |= FPSCR_IXC;
        }
        return r & 0xFFFFu;
    }
}

/* ------------------------------------------------------------------ */
/* Enablement and lazy state preservation                              */
/* ------------------------------------------------------------------ */

bool armv7m_fpu_enabled(const armv7m_cpu_t *c)
{
    const uint32_t cp10 = (c->cpacr >> 20) & 3u;

    if (cp10 == 3u) {
        return true;
    }
    return cp10 == 1u && armv7m_privileged(c);
}

/*
 * ExecuteFPCheck: every FP instruction first makes sure the unit is on,
 * then finishes a lazy save the exception entry deferred, then -- if this
 * is the first FP instruction of a new context -- takes FPSCR's modes
 * from FPDSCR and marks the context as using the FPU.
 */
static X fp_check(armv7m_cpu_t *c)
{
    if (!armv7m_fpu_enabled(c)) {
        return ARMV7M_X_NOCP;
    }
    if ((c->fpccr & FPCCR_LSPACT) != 0u) {
        for (uint32_t i = 0u; i < 16u; i++) {
            (void)emu_bus_write(c->bus, c->fpcar + 4u * i, 4u, c->s[i]);
            armv7m_wrote(c, c->fpcar + 4u * i);
        }
        (void)emu_bus_write(c->bus, c->fpcar + 0x40u, 4u, c->fpscr);
        armv7m_wrote(c, c->fpcar + 0x40u);
        c->fpccr &= ~FPCCR_LSPACT;
    }
    if ((c->fpccr & FPCCR_ASPEN) != 0u &&
        (c->control & ARMV7M_CONTROL_FPCA) == 0u) {
        c->fpscr = (c->fpscr & ~FPSCR_MODE_MASK) | (c->fpdscr & FPSCR_MODE_MASK);
        c->control |= ARMV7M_CONTROL_FPCA;
    }
    return OK;
}

/* ------------------------------------------------------------------ */
/* Decode                                                              */
/* ------------------------------------------------------------------ */

/* Data processing: 111T 1110 | ... 101 sz ... 0 ... */
static X fp_dp(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const bool t = ((w0 >> 12) & 1u) != 0u;
    const uint32_t opc2 = w0 & 15u;
    const uint32_t opc3 = (w1 >> 6) & 3u;
    const uint32_t d = (((w1 >> 12) & 15u) << 1) | ((w0 >> 6) & 1u);
    const uint32_t n = ((uint32_t)(w0 & 15u) << 1) | ((w1 >> 7) & 1u);
    const uint32_t m = ((uint32_t)(w1 & 15u) << 1) | ((w1 >> 5) & 1u);
    const bool op = (opc3 & 1u) != 0u;
    uint32_t *const s = c->s;

    if (((w1 >> 8) & 1u) != 0u) {
        return ARMV7M_X_UNDEF; /* double precision: not on this part */
    }
    /* opc1 here is bits 23,21,20 of the instruction -- D (bit 22) is not
     * part of it -- packed as <3>:<1>:<0> into a three-bit value. */
    {
        const uint32_t o1 = (((w0 >> 7) & 1u) << 2) | ((w0 >> 4) & 3u);

        if (t) { /* FPv5: VSEL, VMAXNM/VMINNM, VRINT{A,N,P,M}, VCVT{A,N,P,M} */
            if ((o1 & 4u) == 0u) { /* VSEL */
                const uint32_t cc = (w0 >> 4) & 3u;
                const bool nz = (c->xpsr & ARMV7M_Z) != 0u;
                const bool nn = (c->xpsr & ARMV7M_N) != 0u;
                const bool nv = (c->xpsr & ARMV7M_V) != 0u;
                bool cond;

                if (op) {
                    return ARMV7M_X_UNDEF;
                }
                switch (cc) {
                case 0u:
                    cond = nz;
                    break;
                case 1u:
                    cond = nv;
                    break;
                case 2u:
                    cond = nn == nv;
                    break;
                default:
                    cond = !nz && nn == nv;
                    break;
                }
                {
                    const X x = fp_check(c);

                    if (x != OK) {
                        return x;
                    }
                }
                s[d] = cond ? s[n] : s[m];
                return OK;
            }
            if (o1 == 4u) { /* VMAXNM / VMINNM */
                const X x = fp_check(c);

                if (x != OK) {
                    return x;
                }
                s[d] = fp_maxmin(c, s[n], s[m], !op);
                return OK;
            }
            if (o1 == 7u && (opc2 & 0xCu) == 0x8u && (opc3 & 1u) != 0u) { /* VRINTA..M */
                static const uint8_t k_rm[4] = {RM_RA, RM_RN, RM_RP, RM_RM};
                const X x = fp_check(c);

                if (x != OK) {
                    return x;
                }
                s[d] = fp_round_int(c, s[m], k_rm[opc2 & 3u], false);
                return OK;
            }
            if (o1 == 7u && (opc2 & 0xCu) == 0xCu && (opc3 & 1u) != 0u) { /* VCVTA..M */
                static const uint8_t k_rm[4] = {RM_RA, RM_RN, RM_RP, RM_RM};
                const bool sgn = (opc3 & 2u) != 0u;
                const X x = fp_check(c);

                if (x != OK) {
                    return x;
                }
                s[d] = fp_to_fixed(c, s[m], 32u, 0u, !sgn, k_rm[opc2 & 3u]);
                return OK;
            }
            return ARMV7M_X_UNDEF;
        }

        {
            const X x = fp_check(c);

            if (x != OK) {
                return x;
            }
        }
        switch (o1) {
        case 0u: { /* VMLA / VMLS: two roundings */
            const uint32_t p = fp_binop(c, OP_MUL, s[n], s[m]);

            s[d] = fp_binop(c, OP_ADD, s[d], op ? (p ^ SIGN) : p);
            return OK;
        }
        case 1u: { /* VNMLA (op=1) / VNMLS (op=0) */
            const uint32_t p = fp_binop(c, OP_MUL, s[n], s[m]);

            s[d] = fp_binop(c, OP_ADD, s[d] ^ SIGN, op ? (p ^ SIGN) : p);
            return OK;
        }
        case 2u: { /* VMUL / VNMUL */
            const uint32_t p = fp_binop(c, OP_MUL, s[n], s[m]);

            s[d] = op ? (p ^ SIGN) : p;
            return OK;
        }
        case 3u: /* VADD / VSUB */
            s[d] = fp_binop(c, op ? OP_SUB : OP_ADD, s[n], s[m]);
            return OK;
        case 4u: /* VDIV */
            if (op) {
                return ARMV7M_X_UNDEF;
            }
            s[d] = fp_binop(c, OP_DIV, s[n], s[m]);
            return OK;
        case 5u: /* VFNMA (op=1) / VFNMS (op=0): -Sd + (+/-Sn)*Sm */
            s[d] = fp_muladd(c, s[d] ^ SIGN, op ? (s[n] ^ SIGN) : s[n], s[m]);
            return OK;
        case 6u: /* VFMA (op=0) / VFMS (op=1): Sd + (+/-Sn)*Sm */
            s[d] = fp_muladd(c, s[d], op ? (s[n] ^ SIGN) : s[n], s[m]);
            return OK;
        default:
            break;
        }

        /* opc1 = 1x11: the "other" group, selected by opc2 and opc3. */
        if ((opc3 & 1u) == 0u) { /* VMOV (immediate) */
            if ((w1 & 0x00A0u) != 0u) {
                return ARMV7M_X_UNDEF; /* (0) 0 (0) 0 */
            }
            s[d] = armv7m_vfp_expand_imm(((uint32_t)(w0 & 15u) << 4) | (w1 & 15u));
            return OK;
        }
        switch (opc2) {
        case 0x0u: /* VMOV (register) / VABS */
            s[d] = (opc3 == 3u) ? (s[m] & ~SIGN) : s[m];
            return OK;
        case 0x1u: /* VNEG / VSQRT */
            s[d] = (opc3 == 3u) ? fp_sqrt(c, s[m]) : (s[m] ^ SIGN);
            return OK;
        case 0x2u: { /* VCVTB/VCVTT, from half */
            const uint32_t sh = ((w1 >> 7) & 1u) ? 16u : 0u;

            s[d] = half_to_single(c, (s[m] >> sh) & 0xFFFFu);
            return OK;
        }
        case 0x3u: { /* VCVTB/VCVTT, to half */
            const uint32_t sh = ((w1 >> 7) & 1u) ? 16u : 0u;
            const uint32_t h = single_to_half(c, s[m]);

            s[d] = (s[d] & ~(0xFFFFu << sh)) | (h << sh);
            return OK;
        }
        case 0x4u: /* VCMP / VCMPE */
            fp_cmp(c, s[d], s[m], (w1 & 0x80u) != 0u);
            return OK;
        case 0x5u: /* VCMP / VCMPE with zero */
            if ((w1 & 0x2Fu) != 0u) {
                return ARMV7M_X_UNDEF;
            }
            fp_cmp(c, s[d], 0u, (w1 & 0x80u) != 0u);
            return OK;
        case 0x6u: /* VRINTR (op 0) / VRINTZ (op 1) */
            s[d] = fp_round_int(c, s[m], ((w1 >> 7) & 1u) ? RM_RZ : rmode(c), false);
            return OK;
        case 0x7u: /* VRINTX; opc3 11 would be VCVT f32<->f64 */
            if (opc3 != 1u) {
                return ARMV7M_X_UNDEF;
            }
            s[d] = fp_round_int(c, s[m], rmode(c), true);
            return OK;
        case 0x8u: { /* VCVT int -> float, FPSCR rounding */
            const bool sgn = (w1 & 0x80u) != 0u;

            s[d] = fixed_to_fp(c, s[m], 32u, 0u, !sgn, rmode(c));
            return OK;
        }
        case 0xAu:
        case 0xBu:
        case 0xEu:
        case 0xFu: { /* VCVT, fixed point: Sd in place */
            const bool to_fixed = (opc2 & 4u) != 0u;
            const bool uns = (opc2 & 1u) != 0u;
            const uint32_t size = (w1 & 0x80u) ? 32u : 16u;
            const uint32_t imm5 = ((w1 & 15u) << 1) | ((w1 >> 5) & 1u);
            const uint32_t fbits = size - imm5;

            if (imm5 > size) {
                return ARMV7M_X_UNDEF;
            }
            if (to_fixed) {
                const uint32_t r = fp_to_fixed(c, s[d], size, fbits, uns, RM_RZ);

                s[d] = (size == 16u)
                           ? (uns ? (r & 0xFFFFu) : (uint32_t)(int32_t)(int16_t)r)
                           : r;
            } else {
                s[d] = fixed_to_fp(c, s[d], size, fbits, uns, RM_RN);
            }
            return OK;
        }
        case 0xCu:
        case 0xDu: { /* VCVT / VCVTR float -> int */
            const bool sgn = (opc2 & 1u) != 0u;
            const bool rz = (w1 & 0x80u) != 0u;

            s[d] = fp_to_fixed(c, s[m], 32u, 0u, !sgn, rz ? RM_RZ : rmode(c));
            return OK;
        }
        default:
            return ARMV7M_X_UNDEF;
        }
    }
}

/* Loads and stores: 1110 110x. */
static X fp_ldst(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t opcode = (w0 >> 4) & 31u;
    const bool p = (opcode & 16u) != 0u;
    const bool u = (opcode & 8u) != 0u;
    const bool w = (opcode & 2u) != 0u;
    const bool load = (opcode & 1u) != 0u;
    const bool dbl = ((w1 >> 8) & 1u) != 0u;
    const uint32_t rn = w0 & 15u;
    const uint32_t imm8 = w1 & 0xFFu;
    const uint32_t dbit = (w0 >> 6) & 1u;
    const uint32_t vd = (w1 >> 12) & 15u;
    /* The first S register: Vd:D for singles, 2 * D:Vd for doubles. */
    const uint32_t first = dbl ? 2u * ((dbit << 4) | vd) : ((vd << 1) | dbit);
    const X x = fp_check(c);

    if (x != OK) {
        return x;
    }
    if (p && !w) { /* VLDR / VSTR */
        const uint32_t base = (rn == ARMV7M_PC) ? ((pc + 4u) & ~3u) : c->r[rn];
        const uint32_t addr = u ? base + imm8 * 4u : base - imm8 * 4u;
        const uint32_t nwords = dbl ? 2u : 1u;

        if (first + nwords > 32u) {
            return ARMV7M_X_UNDEF;
        }
        for (uint32_t i = 0u; i < nwords; i++) {
            uint32_t v = c->s[first + i];
            X e;

            if ((addr & 3u) != 0u) {
                return ARMV7M_X_UNALIGNED;
            }
            e = load ? armv7m_mem_read(c, addr + 4u * i, 4u, false, &v)
                     : armv7m_mem_write(c, addr + 4u * i, 4u, false, v);
            if (e != OK) {
                return e;
            }
            if (load) {
                c->s[first + i] = v;
            }
        }
        return OK;
    }
    {
        /* VLDM / VSTM / VPUSH / VPOP: imm8 words. */
        const uint32_t base = c->r[rn];
        const uint32_t nwords = imm8 & (dbl ? 0xFEu : 0xFFu);
        uint32_t addr = (p && !u) ? base - imm8 * 4u : base;
        uint32_t vals[32];

        if ((p == u && w) || nwords == 0u || first + nwords > 32u ||
            rn == ARMV7M_PC) {
            return ARMV7M_X_UNDEF;
        }
        if ((addr & 3u) != 0u) {
            return ARMV7M_X_UNALIGNED;
        }
        for (uint32_t i = 0u; i < nwords; i++) {
            const X e = load ? armv7m_mem_read(c, addr, 4u, false, &vals[i])
                             : armv7m_mem_write(c, addr, 4u, false,
                                                c->s[first + i]);

            if (e != OK) {
                return e;
            }
            addr += 4u;
        }
        if (w) {
            c->r[rn] = u ? base + imm8 * 4u : base - imm8 * 4u;
        }
        if (load) {
            for (uint32_t i = 0u; i < nwords; i++) {
                c->s[first + i] = vals[i];
            }
        }
        return OK;
    }
}

/* 32-bit transfers: VMOV core<->S, VMRS, VMSR, VMOV core<->scalar. */
static X fp_xfer32(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t a = (w0 >> 5) & 7u;
    const bool l = ((w0 >> 4) & 1u) != 0u;
    const bool cbit = ((w1 >> 8) & 1u) != 0u;
    const uint32_t rt = (w1 >> 12) & 15u;
    const uint32_t vn = w0 & 15u;
    const uint32_t nbit = (w1 >> 7) & 1u;
    const X x = fp_check(c);

    if (x != OK) {
        return x;
    }
    if ((w1 & 0x006Fu) != 0u) {
        return ARMV7M_X_UNDEF; /* (0) everywhere below the coprocessor but N */
    }
    if (!cbit && a == 0u) { /* VMOV Sn <-> Rt */
        const uint32_t sn = (vn << 1) | nbit;

        if (l) {
            c->r[rt] = c->s[sn];
        } else {
            c->s[sn] = c->r[rt];
        }
        return OK;
    }
    if (!cbit && a == 7u) { /* VMRS / VMSR: FPSCR only on M-profile */
        if (vn != 1u || nbit != 0u) {
            return ARMV7M_X_UNDEF;
        }
        if (l) {
            if (rt == ARMV7M_PC) { /* APSR_nzcv */
                c->xpsr = (c->xpsr & ~FPSCR_NZCV_MASK) | (c->fpscr & FPSCR_NZCV_MASK);
            } else {
                c->r[rt] = c->fpscr;
            }
        } else {
            c->fpscr = c->r[rt] & FPSCR_WRITABLE;
        }
        return OK;
    }
    if (cbit && (a & 6u) == 0u && ((w1 >> 5) & 3u) == 0u) { /* scalar */
        const uint32_t dn = (nbit << 4) | vn;
        const uint32_t idx = 2u * dn + (a & 1u);

        if (idx >= 32u) {
            return ARMV7M_X_UNDEF;
        }
        if (l) {
            c->r[rt] = c->s[idx];
        } else {
            c->s[idx] = c->r[rt];
        }
        return OK;
    }
    return ARMV7M_X_UNDEF;
}

/* 64-bit transfers: VMOV two core registers <-> two S, or one D. */
static X fp_xfer64(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const bool l = ((w0 >> 4) & 1u) != 0u;
    const uint32_t rt2 = w0 & 15u;
    const uint32_t rt = (w1 >> 12) & 15u;
    const bool cbit = ((w1 >> 8) & 1u) != 0u;
    const uint32_t vm = w1 & 15u;
    const uint32_t mbit = (w1 >> 5) & 1u;
    const uint32_t first = cbit ? 2u * ((mbit << 4) | vm) : ((vm << 1) | mbit);
    const X x = fp_check(c);

    if (x != OK) {
        return x;
    }
    if ((w1 & 0xD0u) != 0x10u || first > 30u || (l && rt == rt2)) {
        return ARMV7M_X_UNDEF; /* ...or both halves into one register */
    }
    if (l) {
        c->r[rt] = c->s[first];
        c->r[rt2] = c->s[first + 1u];
    } else {
        c->s[first] = c->r[rt];
        c->s[first + 1u] = c->r[rt2];
    }
    return OK;
}

armv7m_exc_t armv7m_fpu_exec(armv7m_cpu_t *c, uint16_t w0, uint16_t w1,
                             uint32_t pc)
{
    const uint32_t coproc = (w1 >> 8) & 15u;

    /*
     * Two slots are not coprocessor instructions whatever number the
     * second halfword carries, and a Cortex-M7 answers UNDEFINSTR for
     * them rather than NOCP: the Advanced SIMD data-processing space,
     * which M-profile leaves empty, and the load/store row with P, U, D
     * and W all clear. Asked of the board, with every coprocessor number.
     */
    if ((w0 & 0x0F00u) == 0x0F00u || (w0 & 0x0FE0u) == 0x0C00u) {
        return ARMV7M_X_UNDEF;
    }
    if ((coproc & 0xEu) != 0xAu) {
        return ARMV7M_X_NOCP; /* no other coprocessor exists */
    }
    if ((w0 & 0xEF00u) == 0xEE00u) {
        if ((w1 & 0x10u) == 0u) {
            return fp_dp(c, w0, w1);
        }
        if ((w0 & 0x1000u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        return fp_xfer32(c, w0, w1);
    }
    if ((w0 & 0xFFE0u) == 0xEC40u) {
        return fp_xfer64(c, w0, w1);
    }
    if ((w0 & 0xFE00u) == 0xEC00u) {
        /* 0010x is the 64-bit transfer above; 00x0x otherwise unallocated. */
        if (((w0 >> 4) & 0x1Au) == 0u) {
            return ARMV7M_X_UNDEF;
        }
        return fp_ldst(c, w0, w1, pc);
    }
    return ARMV7M_X_UNDEF;
}
