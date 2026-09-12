/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_perf.h - the host's own instruction and cycle counters.
 *
 * See host_perf.c for why these can be unavailable and what the
 * fallback measures. The caller has to ask rather than assume: a zero
 * from a counter that is not there looks exactly like a host doing no
 * work, which is the one reading that cannot be true.
 */
#ifndef HOST_PERF_H
#define HOST_PERF_H

#include <stdbool.h>
#include <stdint.h>

void host_perf_init(void);

/* Whether a real instruction count is available. There is no fallback:
 * nothing unprivileged counts instructions. */
bool host_perf_have_insns(void);

/* Whether *something* counts cycles -- perf, or the time-stamp counter. */
bool host_perf_have_cycles(void);

/* True when the cycle figure is the TSC, which ticks at a fixed rate and
 * is therefore elapsed time rather than work. Worth labelling, because
 * the two differ by exactly the thing anyone reads cycles to learn. */
bool host_perf_cycles_are_tsc(void);

/*
 * One sample of everything, taken together.
 *
 * A struct rather than out-parameters because the counters are read in
 * one pass and a caller that took them separately would sample them at
 * slightly different instants -- which for a *ratio* like cycles per
 * instruction is exactly the error that matters.
 */
typedef struct {
    uint64_t insns;
    uint64_t cycles;
    uint64_t branches;
    uint64_t branch_misses;
} host_perf_sample_t;

/* Whether branch counters are available. Separate from the instruction
 * ones: there are only so many hardware counters and they are shared,
 * so a machine can have some and not others. */
bool host_perf_have_branches(void);

void host_perf_read(host_perf_sample_t *out);

#endif /* HOST_PERF_H */
