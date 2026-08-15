/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_decode.h - field extraction and instruction length for VLE.
 *
 * PowerPC numbers bits from the most significant, so every shift here is
 * `31 - manual_bit`. The conversion happens once, in this file, and
 * nothing downstream deals in the manual's convention.
 */
#ifndef PPC_DECODE_H
#define PPC_DECODE_H

#include "ppc/ppc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Primary opcode: bits 0:5 of a 32-bit instruction. */
static EMU_ALWAYS_INLINE uint32_t ppc_op6(uint32_t w)  { return (w >> 26) & 0x3Fu; }

/* The three register fields of the classic X/D forms. */
static EMU_ALWAYS_INLINE uint32_t ppc_rd(uint32_t w)   { return (w >> 21) & 0x1Fu; }
static EMU_ALWAYS_INLINE uint32_t ppc_ra(uint32_t w)   { return (w >> 16) & 0x1Fu; }
static EMU_ALWAYS_INLINE uint32_t ppc_rb(uint32_t w)   { return (w >> 11) & 0x1Fu; }

/* Extended opcode, bits 21:30, and the Rc bit that asks for CR0. */
static EMU_ALWAYS_INLINE uint32_t ppc_xo10(uint32_t w) { return (w >> 1) & 0x3FFu; }
static EMU_ALWAYS_INLINE bool     ppc_rc(uint32_t w)   { return (w & 1u) != 0u; }
static EMU_ALWAYS_INLINE bool     ppc_oe(uint32_t w)   { return (w & (1u << 10)) != 0u; }

/* D-form's signed 16-bit displacement. */
static EMU_ALWAYS_INLINE int32_t ppc_d16(uint32_t w)
{
    return (int32_t)(int16_t)(uint16_t)(w & 0xFFFFu);
}

/*
 * Instruction length, from the first halfword -- **in VLE only**.
 *
 * Classic Book E is fixed 32-bit and this function must not be applied
 * to it. The two are different encodings of the same bytes rather than a
 * superset and a subset, and mixing them is not a subtle error: `bl`
 * (0x48000009) has top4 = 0x4, so the VLE rule calls it 16-bit and the
 * stream desynchronises from there. That mistake was made here first
 * time round and caught by test_branch.
 *
 * **Derived from the assembler, not from a diagram.** Tabulated with
 * scripts/ppc-check-encodings.sh across thirteen values of bits 0:3:
 *
 *     0 2 4 6 8 9 A B C D E  ->  16-bit
 *     1 3 5 7                ->  32-bit
 *
 * so 32-bit is exactly "bit 3 clear and bit 0 set". Note 0x9, 0xB and
 * 0xD: they have bit 0 set and are 16-bit, so the test is not `top4 & 1`
 * -- which is the guess the layout invites, and which would
 * desynchronise on se_lwz (0xC0..) and se_stw (0xD0..) among others.
 *
 * A wrong length here is not a wrong answer. It is a desynchronised
 * instruction stream, and every instruction after it is garbage.
 */
static EMU_ALWAYS_INLINE unsigned ppc_vle_len(uint16_t w0)
{
    return (((w0 >> 12) & 0x9u) == 0x1u) ? 4u : 2u;
}

#ifdef __cplusplus
}
#endif

#endif /* PPC_DECODE_H */
