/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pairstats.h - RV32's half of the pair histogram.
 *
 * The histogram is in emu/emu_pairstats.h and is frontend-agnostic; this
 * is only the table telling it how to read a RISC-V encoding. A second
 * frontend wanting the same measurement writes the equivalent forty lines
 * and gets everything else for free -- which is the whole reason the
 * split exists, because the frontends *without* a reference model are the
 * ones where "what does this guest actually execute" is hardest to answer
 * any other way.
 */
#ifndef RV32_RV_PAIRSTATS_H
#define RV32_RV_PAIRSTATS_H

#include "emu/emu_pairstats.h"

#ifdef __cplusplus
extern "C" {
#endif

#if EMU_PAIR_STATS
extern const emu_pair_ops_t rv_pair_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_PAIRSTATS_H */
