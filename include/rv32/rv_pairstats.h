/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pairstats.h - Adjacent-instruction-pair histogram.
 *
 * Measurement scaffolding for deciding whether the translator should fuse
 * guest instruction pairs into fewer Thumb-2 instructions. Answers the
 * only question that matters first: which pairs actually *execute*, as
 * opposed to which ones look fusible in a listing.
 *
 * Counted on the interpreter because that is what runs on the host, where
 * CoreMark can be run in a second rather than flashed. A pair is only
 * counted when the two instructions are consecutive in the executed
 * stream *and* contiguous in memory -- a branch between them means the
 * translator would never see them as a pair either.
 */
#ifndef RV32_RV_PAIRSTATS_H
#define RV32_RV_PAIRSTATS_H

#include "rv_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RV_PAIR_STATS

/* Record one executed instruction. */
void rv_pair_note(uint32_t pc, uint32_t insn, unsigned len);

/* Print the histogram, most frequent first, to stderr. */
void rv_pair_report(unsigned top_n);

#endif

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_PAIRSTATS_H */
