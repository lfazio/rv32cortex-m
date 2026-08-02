/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_types.h - Base types, exception codes and bit helpers.
 *
 * Everything here is host-independent: the core compiles unchanged for
 * ARMv6-M, ARMv7E-M, ARMv8.1-M and for a native host build.
 */
#ifndef RV32_RV_TYPES_H
#define RV32_RV_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rv_config.h"   /* RV_EXT_U, for RV_PRIV_LEAST below */

/* ------------------------------------------------------------------ */
/* Exception / trap causes (RISC-V privileged spec, mcause low bits)   */
/* ------------------------------------------------------------------ */

/*
 * Cause code 0 is a real exception (instruction address misaligned), so a
 * sentinel outside the encodable range marks "no trap". Functions that can
 * fault return rv_exc_t and callers branch on != RV_EXC_NONE.
 */
typedef uint32_t rv_exc_t;

#define RV_EXC_NONE                 0xFFFFFFFFu

#define RV_EXC_INSN_MISALIGNED      0u
#define RV_EXC_INSN_ACCESS_FAULT    1u
#define RV_EXC_ILLEGAL_INSN         2u
#define RV_EXC_BREAKPOINT           3u
#define RV_EXC_LOAD_MISALIGNED      4u
#define RV_EXC_LOAD_ACCESS_FAULT    5u
#define RV_EXC_STORE_MISALIGNED     6u
#define RV_EXC_STORE_ACCESS_FAULT   7u
#define RV_EXC_ECALL_U              8u
#define RV_EXC_ECALL_S              9u
#define RV_EXC_ECALL_M              11u
#define RV_EXC_INSN_PAGE_FAULT      12u
#define RV_EXC_LOAD_PAGE_FAULT      13u
#define RV_EXC_STORE_PAGE_FAULT     15u

/* Interrupt cause codes (mcause with the MSB set). */
#define RV_INT_M_SOFT               3u
#define RV_INT_M_TIMER              7u
#define RV_INT_M_EXT                11u

#define RV_CAUSE_INTERRUPT          0x80000000u

/* ------------------------------------------------------------------ */
/* Privilege levels                                                    */
/* ------------------------------------------------------------------ */

#define RV_PRIV_U                   0u
#define RV_PRIV_S                   1u
#define RV_PRIV_M                   3u

/*
 * What MRET leaves in MPP: the least-privileged mode the implementation
 * supports, which the privileged spec requires rather than merely allows.
 */
#if RV_EXT_U
#  define RV_PRIV_LEAST             RV_PRIV_U
#else
#  define RV_PRIV_LEAST             RV_PRIV_M
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Sign-extend the low `bits` bits of v. */
static inline int32_t rv_sext(uint32_t v, unsigned bits)
{
    const unsigned sh = 32u - bits;
    return (int32_t)(v << sh) >> sh;
}

static inline uint32_t rv_bit(uint32_t v, unsigned n)
{
    return (v >> n) & 1u;
}

/* Extract bits [hi:lo] of v (inclusive). */
static inline uint32_t rv_bits(uint32_t v, unsigned hi, unsigned lo)
{
    return (v >> lo) & (0xFFFFFFFFu >> (31u - (hi - lo)));
}

#if defined(__GNUC__)
#  define RV_LIKELY(x)    __builtin_expect(!!(x), 1)
#  define RV_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#  define RV_HOT          __attribute__((hot))
#  define RV_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#  define RV_LIKELY(x)    (x)
#  define RV_UNLIKELY(x)  (x)
#  define RV_HOT
#  define RV_ALWAYS_INLINE inline
#endif

#endif /* RV32_RV_TYPES_H */
