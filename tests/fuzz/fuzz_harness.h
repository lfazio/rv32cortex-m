/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_harness.h - what the interpreter-against-JIT targets share.
 *
 * Each target is one frontend. It builds two cores on two buses from
 * identical state, runs one under the interpreter and one under the
 * x86-64 JIT, and compares everything the architecture lets a program
 * see. What is here is the part that belongs to no architecture: the
 * run loop, the rule for a run that did not finish, the report and the
 * stats line. All static, because each target is one translation unit
 * and links nothing else of the harness.
 *
 * The interpreter is the reference. It is what the suites and every
 * board test run against, so a difference is a JIT defect until the
 * interpreter is shown wrong -- by Sail and Spike for RV32, by the
 * F746 for ARMv7-M, and for G4MH and PowerPC by the manual, which is
 * all there is. No GPL emulator is consulted.
 *
 * A JIT stops only between blocks, so a run still going at the budget
 * stops at a different instruction on each side. Those are not
 * compared. Every target arranges for a trap of any kind to stop the
 * core, so that a run that ends ends at the same instruction on both.
 */
#ifndef FUZZ_HARNESS_H
#define FUZZ_HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_backend.h"
#include "emu/emu_cpu.h"
#include "emu/emu_jit.h"

typedef struct fz_side {
    const char *name;
    const emu_backend_t *backend;
    emu_cpu_t *cpu;
    emu_run_reason_t reason;
    uint64_t retired; /* what run() reported: budget bookkeeping */
} fz_side_t;

typedef struct fz_target {
    const char *name; /* "fuzz-armv7m-jit", printed in every report */
    /*
     * The frontend's selected-backend global. Set to each side's
     * backend before it runs, because a frontend reaches its backend
     * through it from inside a run -- RV32's FENCE.I, ARMv7-M's store
     * into translated code -- and the interpreter's run must not
     * invalidate the JIT's blocks or the other way round.
     */
    const emu_backend_t **active;
    uint32_t budget;
    uint32_t slice;
    /* Before each backend's run: what the frontend keeps outside the
     * core, pointed at the side about to run. NULL if nothing. */
    void (*before_run)(emu_cpu_t *cpu);
    /* After each backend's run: state a frontend keeps elsewhere while
     * translated code runs, put back where the comparison reads it. */
    void (*after_run)(emu_cpu_t *cpu);
    /* Print the program, for a report. */
    void (*dump)(void);
} fz_target_t;

/* What the runs did, printed at exit when FUZZ_STATS is set. */
static struct {
    const char *name;
    uint64_t inputs;
    uint64_t compared;
    uint64_t unfinished;
    uint64_t asynchronous; /* compared nothing: see fz_asynchronous */
    uint64_t retired;
    uint64_t translations;
    uint64_t block_entries;
    uint64_t interp_fallbacks;
} fz_stats;

static unsigned fz_diffs;

/* xorshift32, so data RAM's initial bytes are arbitrary but fixed. */
static inline uint32_t fz_prng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static inline void fz_fill(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed;

    for (size_t i = 0; i + 4u <= n; i += 4u) {
        const uint32_t v = fz_prng(&s);

        memcpy(&buf[i], &v, 4u);
    }
}

static void fz_print_stats(void)
{
    fprintf(stderr,
            "%s: %llu inputs, %llu compared, %llu unfinished, "
            "%llu ended in an asynchronous interrupt; "
            "%llu retired, %llu translations, %llu block entries, "
            "%llu interpreted under the JIT\n",
            fz_stats.name, (unsigned long long)fz_stats.inputs,
            (unsigned long long)fz_stats.compared,
            (unsigned long long)fz_stats.unfinished,
            (unsigned long long)fz_stats.asynchronous,
            (unsigned long long)fz_stats.retired,
            (unsigned long long)fz_stats.translations,
            (unsigned long long)fz_stats.block_entries,
            (unsigned long long)fz_stats.interp_fallbacks);
}

/* Once per process: the JIT's code buffer, and the stats line. */
static inline void fz_start(const fz_target_t *t, const emu_backend_t *jit,
                            emu_cpu_t *cpu)
{
    fz_stats.name = t->name;
    if (jit->init != NULL && !jit->init(cpu)) {
        fprintf(stderr, "%s: the JIT did not start\n", t->name);
        abort();
    }
    if (getenv("FUZZ_STATS") != NULL) {
        atexit(fz_print_stats);
    }
}

/* One difference; the first prints the heading. */
static inline void fz_diff(const fz_target_t *t, const char *what, unsigned idx,
                           uint64_t a, uint64_t b)
{
    if (a == b) {
        return;
    }
    if (fz_diffs++ == 0u) {
        fprintf(stderr, "%s: the interpreter and the JIT disagree\n", t->name);
    }
    fprintf(stderr, "  %-12s[%3u] interp 0x%016llx jit 0x%016llx\n", what, idx,
            (unsigned long long)a, (unsigned long long)b);
}

/* Two byte arrays a word at a time, so a report names the word. */
static inline void fz_diff_mem(const fz_target_t *t, const char *what,
                               const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 4u <= n; i += 4u) {
        uint32_t va, vb;

        memcpy(&va, &a[i], 4u);
        memcpy(&vb, &b[i], 4u);
        fz_diff(t, what, (unsigned)(i / 4u), va, vb);
    }
}

static inline bool fz_finished(const fz_side_t *sd)
{
    return sd->reason == EMU_RUN_HALTED || sd->reason == EMU_RUN_WFI;
}

static inline void fz_run(const fz_target_t *t, fz_side_t *sd)
{
    emu_run_reason_t r = EMU_RUN_BUDGET;
    uint64_t total = 0;

    *t->active = sd->backend;
    if (t->before_run != NULL) {
        t->before_run(sd->cpu);
    }
    if (sd->backend->reset != NULL) {
        sd->backend->reset(sd->cpu);
    }
    while (total < t->budget) {
        uint32_t n = 0;

        r = sd->backend->run(sd->cpu, t->slice, &n);
        total += n;
        if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
            break;
        }
    }
    if (t->after_run != NULL) {
        t->after_run(sd->cpu);
    }
    sd->reason = r;
    sd->retired = total;
}

static inline void fz_report(const fz_target_t *t, const fz_side_t *a,
                             const fz_side_t *b)
{
    fprintf(stderr, "  run counts: %s %llu, %s %llu; reasons %d and %d\n",
            a->name, (unsigned long long)a->retired, b->name,
            (unsigned long long)b->retired, (int)a->reason, (int)b->reason);
    if (t->dump != NULL) {
        t->dump();
    }
    abort();
}

/*
 * Both sides, in order, with the JIT's own counters around its run.
 * True if both finished, and the caller compares; false if the input
 * is not comparable. Aborts on the one divergence visible without a
 * comparison: one side finished and the other ran *past* that point.
 * A JIT returns at a block boundary, so it can reach the budget a
 * little short of an interpreter's halt as easily as beyond it, and
 * only beyond is wrong.
 */
static inline bool fz_pair(const fz_target_t *t, fz_side_t *interp,
                           fz_side_t *jit)
{
    emu_jit_stats_t before, after;

    fz_run(t, interp);
    emu_jit_get_stats(&before);
    fz_run(t, jit);
    emu_jit_get_stats(&after);

    fz_stats.inputs++;
    fz_stats.translations += after.translations - before.translations;
    fz_stats.block_entries += after.block_entries - before.block_entries;
    fz_stats.interp_fallbacks +=
        after.interp_fallbacks - before.interp_fallbacks;

    if (fz_finished(interp) && fz_finished(jit)) {
        fz_stats.compared++;
        fz_stats.retired += interp->retired;
        fz_diffs = 0;
        return true;
    }
    if (fz_finished(interp) != fz_finished(jit)) {
        const fz_side_t *done = fz_finished(interp) ? interp : jit;
        const fz_side_t *open = (done == interp) ? jit : interp;

        if (open->retired > done->retired) {
            fz_diffs = 0;
            fz_diff(t, "reason", 0u, interp->reason, jit->reason);
            fz_diff(t, "retired", 0u, interp->retired, jit->retired);
            fz_report(t, interp, jit);
        }
    }
    fz_stats.unfinished++;
    return false;
}

/*
 * A run that ended in an asynchronous interrupt -- a timer's, on a
 * frontend whose program can arm one -- is not compared after all. The
 * interpreter takes it on the instruction the timer expires on and a
 * JIT at the next block boundary, which the architecture permits, and
 * everything after differs by that much. Each target's handlers stop
 * the core at once, so where it stopped says which interrupt it was.
 * Called instead of comparing; moves the run from compared to here.
 */
static inline void fz_asynchronous(void)
{
    fz_stats.compared--;
    fz_stats.asynchronous++;
}

/* After a comparison: print both sides and abort if anything differed. */
static inline void fz_verdict(const fz_target_t *t, const fz_side_t *interp,
                              const fz_side_t *jit)
{
    if (fz_diffs != 0u) {
        fz_report(t, interp, jit);
    }
}

#endif /* FUZZ_HARNESS_H */
