/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_pairstats.h - ARMv7-M's half of the pair histogram.
 *
 * The histogram is frontend-agnostic and lives in emu/emu_pairstats.h;
 * this is only the table that reads a Thumb-2 encoding. See the .c for
 * what it is exact about and where it approximates.
 */
#ifndef ARMV7M_PAIRSTATS_H
#define ARMV7M_PAIRSTATS_H

#include "emu/emu_pairstats.h"

#ifdef __cplusplus
extern "C" {
#endif

#if EMU_PAIR_STATS
extern const emu_pair_ops_t armv7m_pair_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_PAIRSTATS_H */
