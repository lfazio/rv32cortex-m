/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_pairstats.h - G4MH's half of the pair histogram.
 *
 * The histogram is frontend-agnostic and lives in emu/emu_pairstats.h;
 * this is only the table that reads an RH850 encoding. See the .c for
 * what it is exact about and where it approximates.
 */
#ifndef G4MH_PAIRSTATS_H
#define G4MH_PAIRSTATS_H

#include "emu/emu_pairstats.h"

#ifdef __cplusplus
extern "C" {
#endif

#if EMU_PAIR_STATS
extern const emu_pair_ops_t g4mh_pair_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* G4MH_PAIRSTATS_H */
