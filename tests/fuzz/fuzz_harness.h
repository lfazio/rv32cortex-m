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
 *
 * Devices
 * -------
 * Each target maps its frontend's interrupt controller and timers,
 * through fz_dev_t, which sits between the bus and the real device. The
 * device is the frontend's own; the wrapper only watches. Two things a
 * device write can do are allowed to come out differently on the two
 * backends, and the wrapper notices both while the run is under way:
 *
 *   - **make an interrupt deliverable at once.** The interpreter takes
 *     it before the next instruction and a JIT when the block returns to
 *     its dispatcher -- or later still, across a chain. The architecture
 *     allows either, for an interrupt raised by a store. FZ_ASIDE_ASYNC.
 *   - **change what a translated block assumed**, where the architecture
 *     defers the change to a synchronising instruction (ARMv7-M's CCR and
 *     MPU before an ISB). FZ_ASIDE_UNSYNC.
 *
 * Either sets the run aside, finished or not, before anything is
 * compared or reported. What stays comparable is an interrupt made
 * deliverable by an instruction -- an enable, a mask lowered, an
 * exception return -- because every frontend's translator declines
 * those, so the interpreter runs them on both sides and takes the
 * interrupt at the same boundary.
 *
 * More than one core
 * ------------------
 * A target may give a side up to FZ_MAX_CORES cores, on one bus each
 * over shared memory. They run in turn, and a core gives way only where
 * it parks -- WFI or halt -- never at a budget or a block boundary, so
 * both backends interleave the cores identically and the comparison
 * still means what it says. A parked core runs again once something
 * wakes it, typically another core's interrupt. The run ends when a
 * whole round retires nothing.
 *
 * What it is for is the JIT's one table of blocks, which every core on
 * the thread shares: a block one core built is found and run by the
 * next, under whatever that block assumed about the first.
 */
#ifndef FUZZ_HARNESS_H
#define FUZZ_HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_backend.h"
#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_jit.h"

#define FZ_MAX_CORES 2u

/* Why a run is set aside rather than compared; see the header. */
#define FZ_ASIDE_ASYNC (1u << 0)
#define FZ_ASIDE_UNSYNC (1u << 1)

typedef struct fz_side {
    const char *name;
    const emu_backend_t *backend;
    emu_cpu_t *cpu; /* core 0, which every target has */
    /* Every core this input runs, cores[0] == cpu; ncores 0 means 1. */
    emu_cpu_t *cores[FZ_MAX_CORES];
    unsigned ncores;
    emu_run_reason_t reason;
    uint64_t retired; /* what run() reported: budget bookkeeping */
    unsigned aside; /* FZ_ASIDE_*, from the devices during the run */
    uint32_t device_accesses; /* through every fz_dev_t of this side */
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
     * cores, pointed at the side about to run. NULL if nothing. */
    void (*before_run)(struct fz_side *sd);
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
    uint64_t asynchronous; /* set aside: FZ_ASIDE_ASYNC, fz_asynchronous */
    uint64_t unsynchronised; /* set aside: FZ_ASIDE_UNSYNC */
    uint64_t with_device; /* compared, and touched a device */
    uint64_t with_interrupt; /* compared, and took an interrupt */
    uint64_t retired;
    uint64_t translations;
    uint64_t block_entries;
    uint64_t interp_fallbacks;
} fz_stats;

static unsigned fz_diffs;
/*
 * FUZZ_SHOW set: print every register compared, equal or not, and how
 * each side ended -- for writing a seed and seeing what it did.
 */
static bool fz_show;

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
            "%s: %llu inputs, %llu compared (%llu touched a device, "
            "%llu took an interrupt), %llu unfinished, "
            "%llu set aside for an asynchronous interrupt and %llu for "
            "an unsynchronised write; "
            "%llu retired, %llu translations, %llu block entries, "
            "%llu interpreted under the JIT\n",
            fz_stats.name, (unsigned long long)fz_stats.inputs,
            (unsigned long long)fz_stats.compared,
            (unsigned long long)fz_stats.with_device,
            (unsigned long long)fz_stats.with_interrupt,
            (unsigned long long)fz_stats.unfinished,
            (unsigned long long)fz_stats.asynchronous,
            (unsigned long long)fz_stats.unsynchronised,
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
    fz_show = getenv("FUZZ_SHOW") != NULL;
}

/* One difference; the first prints the heading. */
static inline void fz_diff(const fz_target_t *t, const char *what, unsigned idx,
                           uint64_t a, uint64_t b)
{
    if (a == b) {
        if (fz_show && a != 0u) {
            fprintf(stderr, "  %-12s[%3u] 0x%016llx\n", what, idx,
                    (unsigned long long)a);
        }
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
        if (va != vb) { /* even under FUZZ_SHOW: 4,096 words */
            fz_diff(t, what, (unsigned)(i / 4u), va, vb);
        }
    }
}

static inline bool fz_finished(const fz_side_t *sd)
{
    return sd->reason == EMU_RUN_HALTED || sd->reason == EMU_RUN_WFI;
}

/* One core until it parks or the budget runs out. */
static inline emu_run_reason_t fz_run_core(const fz_target_t *t,
                                           const fz_side_t *sd, emu_cpu_t *cpu,
                                           uint64_t *total)
{
    emu_run_reason_t r = EMU_RUN_BUDGET;

    while (*total < t->budget) {
        uint32_t n = 0;

        r = sd->backend->run(cpu, t->slice, &n);
        *total += n;
        if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
            break;
        }
    }
    return r;
}

static inline void fz_run(const fz_target_t *t, fz_side_t *sd)
{
    const unsigned ncores = (sd->ncores > 1u) ? sd->ncores : 1u;
    emu_run_reason_t r = EMU_RUN_BUDGET;
    uint64_t total = 0;

    *t->active = sd->backend;
    sd->aside = 0u;
    sd->device_accesses = 0u;
    if (t->before_run != NULL) {
        t->before_run(sd);
    }
    /* Once a side, not once a core: the blocks are every core's. */
    if (sd->backend->reset != NULL) {
        sd->backend->reset(sd->cpu);
    }
    if (ncores == 1u) {
        r = fz_run_core(t, sd, sd->cpu, &total);
    } else {
        /*
         * In turn, each until it parks; finished when a whole round
         * retires nothing, which is every core parked with nothing to
         * wake it. A round that is cut short by the budget is not
         * finished, whatever the last core did.
         */
        unsigned quiet = 0u;
        unsigned k = 0u;

        while (total < t->budget && quiet < ncores) {
            const uint64_t before = total;
            const emu_run_reason_t rk =
                fz_run_core(t, sd, sd->cores[k], &total);

            if (rk != EMU_RUN_HALTED && rk != EMU_RUN_WFI) {
                r = rk; /* the budget, mid-core */
                break;
            }
            quiet = (total == before) ? quiet + 1u : 0u;
            k = (k + 1u) % ncores;
        }
        if (quiet >= ncores) {
            r = EMU_RUN_WFI;
        } else if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
            r = EMU_RUN_BUDGET;
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
 *
 * A run a device set aside is neither: it may have been allowed to end
 * differently, so it is not checked for running past either.
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
    if (fz_show) {
        fprintf(stderr,
                "%s: %s reason %d retired %llu aside %u devices %u; "
                "%s reason %d retired %llu aside %u devices %u\n",
                t->name, interp->name, (int)interp->reason,
                (unsigned long long)interp->retired, interp->aside,
                interp->device_accesses, jit->name, (int)jit->reason,
                (unsigned long long)jit->retired, jit->aside,
                jit->device_accesses);
    }

    if ((interp->aside | jit->aside) != 0u) {
        if (((interp->aside | jit->aside) & FZ_ASIDE_ASYNC) != 0u) {
            fz_stats.asynchronous++;
        } else {
            fz_stats.unsynchronised++;
        }
        return false;
    }
    if (fz_finished(interp) && fz_finished(jit)) {
        fz_stats.compared++;
        fz_stats.retired += interp->retired;
        if (interp->device_accesses != 0u || jit->device_accesses != 0u) {
            fz_stats.with_device++;
        }
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
 * A device as one side sees it: the frontend's own ops and context,
 * and what the target wants to know about each write. Mapped with
 * emu_bus_add_mmio(bus, name, base, size, &fz_dev_ops, &the_fz_dev_t).
 */
typedef struct fz_dev fz_dev_t;
struct fz_dev {
    const emu_dev_ops_t *ops;
    void *ctx;
    fz_side_t *side;
    /*
     * The core whose bus this mapping is on, which is the core making
     * any access through it; NULL means the side's core 0. Only that
     * core can take an interrupt "at once": another learns of one when
     * its turn comes, which is the same instruction on both backends.
     */
    emu_cpu_t *cpu;
    /* Before the device sees a write: may change it. NULL: unchanged. */
    void (*filter)(fz_dev_t *d, uint32_t off, uint32_t size, uint32_t *val);
    /* After the device took a write: the FZ_ASIDE_* it implies. */
    unsigned (*after_write)(fz_dev_t *d, uint32_t off, uint32_t size);
};

static emu_fault_t fz_dev_read(void *ctx, uint32_t off, uint32_t size,
                               uint32_t *out)
{
    fz_dev_t *const d = (fz_dev_t *)ctx;

    d->side->device_accesses++;
    return d->ops->read(d->ctx, off, size, out);
}

static emu_fault_t fz_dev_write(void *ctx, uint32_t off, uint32_t size,
                                uint32_t val)
{
    fz_dev_t *const d = (fz_dev_t *)ctx;
    emu_fault_t f;

    d->side->device_accesses++;
    if (d->filter != NULL) {
        d->filter(d, off, size, &val);
    }
    f = d->ops->write(d->ctx, off, size, val);
    if (f == EMU_FAULT_NONE && d->after_write != NULL) {
        d->side->aside |= d->after_write(d, off, size);
    }
    return f;
}

/* The core a write through `d` came from. */
static inline emu_cpu_t *fz_dev_core(const fz_dev_t *d)
{
    return (d->cpu != NULL) ? d->cpu : d->side->cpu;
}

static const emu_dev_ops_t fz_dev_ops = {
    .read = fz_dev_read,
    .write = fz_dev_write,
    .tick = NULL, /* time is the target's to move, and it does not */
};

/* After a comparison that agreed: the run took an interrupt. */
static inline void fz_took_interrupt(void)
{
    fz_stats.with_interrupt++;
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
