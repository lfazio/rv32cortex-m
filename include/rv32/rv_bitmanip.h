/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_bitmanip.h - the Zbb and Zbc operations that are not one C operator.
 *
 * Here, and not in rv_interp.c where they started, because there are two
 * callers now: the interpreter, and the helper the translator calls for
 * the handful of these it does not lower. Two copies of a carry-less
 * multiply is exactly the arrangement this tree keeps dismantling --
 * rv_hart_amo and rv_hart_cbo sit beside the hart for the same reason.
 *
 * Everything else in the B extension is an expression the interpreter
 * writes inline and the translator lowers to IR; only what needs a loop
 * or a builtin with an undefined case is here.
 */
#ifndef RV32_RV_BITMANIP_H
#define RV32_RV_BITMANIP_H

#include "rv_config.h"
#include "rv_types.h"

#if RV_EXT_ZBB
/* __builtin_clz/ctz are undefined for zero; Zbb defines both as 32. */
static EMU_ALWAYS_INLINE uint32_t zbb_clz(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_clz(v);
}

static EMU_ALWAYS_INLINE uint32_t zbb_ctz(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_ctz(v);
}

/* Rotates by a multiple of 32 must not shift by 32, which is UB in C. */
static EMU_ALWAYS_INLINE uint32_t zbb_ror(uint32_t v, uint32_t n)
{
    n &= 31u;
    return (n == 0u) ? v : ((v >> n) | (v << (32u - n)));
}

static EMU_ALWAYS_INLINE uint32_t zbb_rol(uint32_t v, uint32_t n)
{
    n &= 31u;
    return (n == 0u) ? v : ((v << n) | (v >> (32u - n)));
}

/* orc.b: each byte becomes 0xFF if any of its bits are set, else 0x00. */
static EMU_ALWAYS_INLINE uint32_t zbb_orcb(uint32_t v)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        if ((v & (0xFFu << (i * 8u))) != 0u) {
            r |= 0xFFu << (i * 8u);
        }
    }
    return r;
}
#endif /* RV_EXT_ZBB */

#if RV_EXT_ZBC
/*
 * Carry-less multiply: the same shift-and-accumulate as an ordinary
 * multiply with XOR in place of addition, so there are no carries between
 * bit positions. clmulh takes the high half of the 64-bit product and
 * clmulr the middle 32 bits (63:31), which is why their shifts differ by
 * one.
 *
 * **Branch-free by construction, for Zkt**, which promises clmul and
 * clmulh a latency independent of their operands. Each step masks its
 * partial product with all-ones or zero from the bit of b, rather than
 * testing the bit: the `if` this replaced compiled to CMOV on x86-64 and
 * IT on the M7, so it never actually branched -- but that was the
 * compilers' choice, and a timing claim should not rest on one. Always
 * 32 steps either way.
 */
static inline uint32_t zbc_bit_mask(uint32_t b, unsigned i)
{
    return 0u - ((b >> i) & 1u);
}

static inline uint32_t zbc_clmul(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 32u; i++) {
        r ^= (a << i) & zbc_bit_mask(b, i);
    }
    return r;
}

static inline uint32_t zbc_clmulh(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 1; i < 32u; i++) {
        r ^= (a >> (32u - i)) & zbc_bit_mask(b, i);
    }
    return r;
}

static inline uint32_t zbc_clmulr(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 32u; i++) {
        r ^= (a >> (31u - i)) & zbc_bit_mask(b, i);
    }
    return r;
}
#endif /* RV_EXT_ZBC */

#endif /* RV32_RV_BITMANIP_H */
