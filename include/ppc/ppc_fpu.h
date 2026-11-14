/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_fpu.h - the e200z7's embedded floating-point unit, scalar half.
 *
 * EFPU2 in mode 0, the only mode this core has: single precision in the
 * low word of a GPR, no infinities, NaNs or denormals produced, and a
 * default result for every input that is one (manual chapter 5 and
 * tables 5-2 to 5-5).
 */
#ifndef PPC_FPU_H
#define PPC_FPU_H

#include "ppc/ppc_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execute one scalar EFPU instruction, `sem` being its PPC_S_EFS*
 * semantics. Writes rD (or CR field `crf` for a compare) and SPEFSCR.
 *
 * Returns PPC_EXC_NONE; PPC_IVOR_FP_DATA when an enabled invalid,
 * divide-by-zero, overflow or underflow suppressed the instruction --
 * nothing written but SPEFSCR, and the interrupt names this
 * instruction; or PPC_IVOR_FP_ROUND when an enabled inexact result was
 * written *truncated*, and the interrupt names the next instruction.
 * The caller raises either with ESR[SPE].
 */
uint32_t ppc_fpu_exec(ppc_cpu_t *c, uint32_t sem, uint32_t rd, uint32_t ra,
                      uint32_t rb, uint32_t crf);

/*
 * efsadd, efssub, efsmul and efsdiv with their operands given and the
 * result to `dst`: ppc_fpu_exec's cases without its dispatch, for a
 * translated block (ppc_ir.c). The same return values.
 */
uint32_t ppc_fpu_add(ppc_cpu_t *c, uint32_t a, uint32_t b, uint32_t *dst);
uint32_t ppc_fpu_sub(ppc_cpu_t *c, uint32_t a, uint32_t b, uint32_t *dst);
uint32_t ppc_fpu_mul(ppc_cpu_t *c, uint32_t a, uint32_t b, uint32_t *dst);
uint32_t ppc_fpu_div(ppc_cpu_t *c, uint32_t a, uint32_t b, uint32_t *dst);

/*
 * The common cases of efsadd, efssub and efsmul, computed in integers:
 * here, forced inline, so a translated block's helper (ppc_ir.c) runs
 * them without a second call -- the call into the unit after the block's
 * own was a call level the PC profile showed on every FP instruction.
 * Each answers false, having written nothing, for anything it does not
 * cover, and the caller then takes ppc_fpu_add, ppc_fpu_sub or
 * ppc_fpu_mul. The interpreter never calls them: its efs* are
 * ppc_fpu_exec's general path, which is what lets fuzz-ppc-jit hold one
 * against the other (efsadd-edges.S, efsmul-edges.S).
 */
/*
 * SPEFSCR after a fast case's inexact or exact result, as finish()
 * leaves it: the status replaced, FG and FX from the guard and sticky
 * bits, FINXS if either is set. Nothing else can happen in a fast case.
 */
static EMU_ALWAYS_INLINE uint32_t ppc_fpu_fast_status(uint32_t f, uint32_t g,
                                              uint32_t x)
{
    return (f & ~(uint32_t)PPC_SPEFSCR_STATUS) | (g ? PPC_SPEFSCR_FG : 0u) |
           (x ? PPC_SPEFSCR_FX : 0u) | ((g | x) ? PPC_SPEFSCR_FINXS : 0u);
}

/*
 * efsadd's and efssub's common case first, as efsmul's below: two normal
 * operands, round to nearest, FINXE clear, and a result that is not an
 * exact zero and whose exponent before rounding is in 1..253. In 32-bit
 * integers:
 *
 *   - the larger magnitude's significand at bits 30..7, the smaller's
 *     shifted right by the exponents' difference d, every bit it loses
 *     in a sticky bit s -- none for d <= 7, the low seven being zero;
 *   - a sum is A + (B >> d), whose floor is exact; a difference is
 *     A - (B >> d) - s, the floor of the exact difference, s taken as a
 *     borrow. ORing s into the low bit instead would do as well -- the
 *     two differ only when B >> d is odd, and a carry from there into
 *     the guard needs the difference's six low bits all ones, which
 *     makes B >> d even -- and its mutant is equivalent;
 *   - normalised to bit 31 by its leading zeros: a difference loses more
 *     than one leading bit only where d <= 1, which is exact, so the
 *     shift brings in zeros only;
 *   - then the significand, the guard and the sticky bit as for the
 *     product, the exponent added below the significand.
 *
 * Everything else -- specials, denormals, a cancellation to zero (whose
 * sign is finish_sum's) or below the normal range, an exponent at the
 * edges, another mode or an enable -- takes op_add and finish in
 * ppc_fpu.c. Only the translated path calls these; the interpreter keeps the general path,
 * and fuzz-ppc-jit holds the two against each other (efsadd-edges.S).
 */
static EMU_ALWAYS_INLINE bool ppc_fpu_add_fast(ppc_cpu_t *c, uint32_t a, uint32_t b,
                                       uint32_t *dst)
{
    const uint32_t f = c->spefscr;
    uint32_t ea = (a >> 23) & 0xFFu;
    uint32_t eb = (b >> 23) & 0xFFu;

    if (!(ea - 1u < 254u && eb - 1u < 254u &&
          (f & (PPC_SPEFSCR_FRMC | PPC_SPEFSCR_FINXE)) == 0u)) {
        return false;
    }
    if ((a & 0x7FFFFFFFu) < (b & 0x7FFFFFFFu)) {
        const uint32_t t = a, te = ea;

        a = b;
        b = t;
        ea = eb;
        eb = te;
    }
    {
        const uint32_t d = ea - eb;
        const uint32_t sa = ((a & 0x7FFFFFu) | 0x800000u) << 7;
        const uint32_t sb = ((b & 0x7FFFFFu) | 0x800000u) << 7;
        const uint32_t bs = (d < 32u) ? (sb >> d) : 0u;
        const uint32_t st =
            (d == 0u) ? 0u : ((d < 32u) ? ((sb << (32u - d)) != 0u) : 1u);
        const uint32_t sum =
            (((a ^ b) & 0x80000000u) != 0u) ? sa - bs - st : sa + bs;

        if (sum == 0u) {
            return false;
        }
        {
            const uint32_t lz = (uint32_t)__builtin_clz(sum);
            /* The biased exponent less one, ea + 1 - lz - 1; wraps above
             * 253 when too small. */
            const uint32_t em1 = ea - lz;

            if (em1 >= 253u) {
                return false;
            }
            {
                const uint32_t n = sum << lz;
                const uint32_t sig = n >> 8;
                const uint32_t g = (n >> 7) & 1u;
                const uint32_t x = (((n & 0x7Fu) | st) != 0u) ? 1u : 0u;

                *dst = (a & 0x80000000u) |
                       ((em1 << 23) + sig + (g & (x | (sig & 1u))));
                c->spefscr = ppc_fpu_fast_status(f, g, x);
                return true;
            }
        }
    }
}

/*
 * efsmul's common case first: two normal operands, round to nearest,
 * the inexact interrupt off, and an exponent before rounding in 1..253
 * -- so nothing can overflow or underflow, and no other enable can
 * matter. The result is IEEE's single-precision product, and FG and FX
 * the bits round_f64 reads, from the exact 48-bit product of the two
 * significands: each shifted up 8 with its implicit bit at 31, the high
 * word holds the product's bits 47..16 and the low its bits 15..0. The
 * leading bit (47 or 46) shifted to 31, the 24 bits from it are the
 * significand, the next the guard, and everything below the sticky
 * bit; the exponent is added in below the significand, so rounding's
 * carry moves it up. Everything else takes op_mul and finish
 * (ppc_fpu.c), which ppc_fpu_mul calls when this says no.
 *
 * Only the translated path calls this (ppc_ir.c); the interpreter's
 * efsmul is ppc_fpu_exec's, so fuzz-ppc-jit holds one against the
 * other (efsmul-edges.S).
 */
static EMU_ALWAYS_INLINE bool ppc_fpu_mul_fast(ppc_cpu_t *c, uint32_t a,
                                               uint32_t b, uint32_t *dst)
{
    const uint32_t f = c->spefscr;
    const uint32_t ea = (a >> 23) & 0xFFu;
    const uint32_t eb = (b >> 23) & 0xFFu;

    if (EMU_LIKELY(ea - 1u < 254u && eb - 1u < 254u &&
                   (f & (PPC_SPEFSCR_FRMC | PPC_SPEFSCR_FINXE)) == 0u)) {
        const uint64_t p = (uint64_t)((a << 8) | 0x80000000u) *
                           ((b << 8) | 0x80000000u);
        const uint32_t hi = (uint32_t)(p >> 32);
        const uint32_t top = hi >> 31;
        /* The biased exponent less one; wraps above 253 when too small. */
        const uint32_t em1 = ea + eb + top - 128u;

        if (EMU_LIKELY(em1 < 253u)) {
            const uint32_t h = hi << (top ^ 1u);
            const uint32_t sig = h >> 8;
            const uint32_t g = (h >> 7) & 1u;
            const uint32_t x = (((h & 0x7Fu) | (uint32_t)p) != 0u) ? 1u : 0u;

            *dst = ((a ^ b) & 0x80000000u) |
                   ((em1 << 23) + sig + (g & (x | (sig & 1u))));
            c->spefscr = ppc_fpu_fast_status(f, g, x);
            return true;
        }
    }
    return false;
}

/* efssub's: the subtrahend's sign turned, which is all a fast case needs. */
static EMU_ALWAYS_INLINE bool ppc_fpu_sub_fast(ppc_cpu_t *c, uint32_t a,
                                               uint32_t b, uint32_t *dst)
{
    return ppc_fpu_add_fast(c, a, b ^ 0x80000000u, dst);
}

#ifdef __cplusplus
}
#endif

#endif /* PPC_FPU_H */
