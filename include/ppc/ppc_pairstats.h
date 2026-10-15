/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_pairstats.h - PowerPC's half of the pair histogram.
 *
 * The histogram is frontend-agnostic and lives in emu/emu_pairstats.h;
 * this is only the table that reads an e200 encoding. See the .c for
 * what it is exact about and where it approximates.
 */
#ifndef PPC_PAIRSTATS_H
#define PPC_PAIRSTATS_H

#include "emu/emu_pairstats.h"

#ifdef __cplusplus
extern "C" {
#endif

#if EMU_PAIR_STATS
extern const emu_pair_ops_t ppc_pair_ops;

/*
 * What the interpreter hands emu_pair_note: the instruction as it is in
 * memory (first halfword in bits 31:16), its length, and whether the page
 * holds VLE -- because the same bytes are a different instruction in the
 * other encoding, and the histogram has only the 64 bits to go on.
 */
static inline uint64_t ppc_pair_insn(uint32_t insn, unsigned len, bool vle)
{
    return (uint64_t)insn | ((uint64_t)len << 32) | ((uint64_t)vle << 40);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* PPC_PAIRSTATS_H */
