/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_jit.c - The half of a JIT that is neither the guest ISA nor the host.
 *
 * See emu_jit.h for what this owns and why. The short version: everything
 * that three independently written JITs did identically, including the
 * mistakes.
 */

#include "emu/emu_jit.h"

#include <string.h>

#if defined(__linux__) && !defined(EMU_JIT_STATIC_BUFFER)
#  include <sys/mman.h>
#  define EMU_JIT_MMAP 1
#endif

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

/*
 * How many blocks and how large a hash.
 *
 * These are host-sized, not target-sized. The Thumb-2 backend uses 256 of
 * each because on a microcontroller those bytes come out of the guest's
 * RAM; on a host the same numbers made CoreMark flush nineteen times in a
 * run, and constant retranslation is exactly what hides a translator bug
 * behind a fresh translation.
 */
#ifndef EMU_JIT_MAX_BLOCKS
#  define EMU_JIT_MAX_BLOCKS 8192u
#endif
#ifndef EMU_JIT_HASH_SIZE
#  define EMU_JIT_HASH_SIZE  8192u
#endif

/* Headroom kept free so a translation has somewhere to go. */
#ifndef EMU_JIT_BLOCK_RESERVE
#  define EMU_JIT_BLOCK_RESERVE 8192u
#endif

typedef struct {
    uint32_t guest_pc;
    uint8_t *code;
    uint32_t insns;      /* guest instructions this block retires */
} jit_block_t;

static uint8_t    *g_code;
static uint32_t    g_code_size;
static uint32_t    g_code_used;
static bool        g_owned;      /* did we map it, or was it handed over? */

static jit_block_t g_blocks[EMU_JIT_MAX_BLOCKS];
static uint32_t    g_block_count;
static int32_t     g_hash[EMU_JIT_HASH_SIZE];

static uint8_t    *g_emit;
static uint8_t    *g_emit_end;
static bool        g_overflow;

static uint32_t    g_generation;
static bool        g_have_generation;

static emu_jit_stats_t g_stats;

/* ------------------------------------------------------------------ */
/* Code buffer                                                         */
/* ------------------------------------------------------------------ */

void emu_jit_set_buffer(void *mem, uint32_t bytes)
{
    g_code = (uint8_t *)mem;
    g_code_size = bytes;
    g_owned = false;
    emu_jit_flush();
}

bool emu_jit_init(uint32_t bytes)
{
    if (g_code != NULL) {
        return true;
    }
#if EMU_JIT_MMAP
    /*
     * Writable and executable at once, which a hardened host may refuse.
     * The alternative is mprotect between translating and running; the
     * honest note is that nothing here runs guest-controlled data as code
     * that the interpreter would not also have executed.
     */
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    g_code = (uint8_t *)p;
    g_code_size = bytes;
    g_owned = true;
    emu_jit_flush();
    return true;
#else
    /* No mapping available: the platform must supply the buffer. */
    (void)bytes;
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* Blocks                                                              */
/* ------------------------------------------------------------------ */

static uint32_t pc_hash(uint32_t pc)
{
    return (pc >> 1) & (EMU_JIT_HASH_SIZE - 1u);
}

void emu_jit_flush(void)
{
    for (uint32_t i = 0; i < EMU_JIT_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    g_block_count = 0u;
    g_code_used = 0u;
    g_stats.flushes++;
}

static jit_block_t *lookup(uint32_t pc)
{
    const int32_t i = g_hash[pc_hash(pc)];

    if (i >= 0 && g_blocks[i].guest_pc == pc) {
        return &g_blocks[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Emitting                                                            */
/* ------------------------------------------------------------------ */

void emu_jit_emit8(uint8_t b)
{
    if (g_emit >= g_emit_end) {
        g_overflow = true;
        return;
    }
    *g_emit++ = b;
}

void emu_jit_emit16(uint16_t h)
{
    emu_jit_emit8((uint8_t)h);
    emu_jit_emit8((uint8_t)(h >> 8));
}

void emu_jit_emit32(uint32_t w)
{
    emu_jit_emit16((uint16_t)w);
    emu_jit_emit16((uint16_t)(w >> 16));
}

void emu_jit_emit64(uint64_t d)
{
    emu_jit_emit32((uint32_t)d);
    emu_jit_emit32((uint32_t)(d >> 32));
}

uint8_t *emu_jit_here(void)   { return g_emit; }

void emu_jit_rewind(uint8_t *to)
{
    if (to >= g_code && to <= g_emit) {
        g_emit = to;
    }
}
bool emu_jit_overflowed(void) { return g_overflow; }

void emu_jit_get_stats(emu_jit_stats_t *out)
{
    g_stats.blocks = g_block_count;
    g_stats.code_used = g_code_used;
    g_stats.code_size = g_code_size;
    *out = g_stats;
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

static jit_block_t *translate(emu_cpu_t *cpu, uint32_t pc,
                              const emu_jit_ops_t *ops)
{
    if (g_block_count >= EMU_JIT_MAX_BLOCKS ||
        g_code_used + EMU_JIT_BLOCK_RESERVE > g_code_size) {
        emu_jit_flush();
    }

    g_emit = g_code + g_code_used;
    g_emit_end = g_code + g_code_size;
    g_overflow = false;

    uint8_t *const start = g_emit;
    const uint32_t insns = ops->translate(cpu, pc);

    /*
     * Nothing translatable here is the ordinary case -- every interpreted
     * instruction reaches it -- and must not be confused with the buffer
     * being full. Conflating them made an early Thumb-2 build flush the
     * whole cache once per interpreted divide.
     */
    if (insns == 0u || g_overflow) {
        return NULL;
    }

    jit_block_t *const b = &g_blocks[g_block_count++];
    b->guest_pc = pc;
    b->code = start;
    b->insns = insns;
    g_code_used = (uint32_t)(g_emit - g_code);
    g_hash[pc_hash(pc)] = (int32_t)(g_block_count - 1u);

    g_stats.translations++;
    return b;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

typedef uint32_t (*block_fn_t)(emu_cpu_t *);

static emu_run_reason_t run_interp_one(emu_cpu_t *cpu,
                                       const emu_jit_ops_t *ops,
                                       uint32_t *done)
{
    uint32_t n = 0u;
    const emu_run_reason_t r = ops->interp->run(cpu, 1u, &n);

    *done += n;
    g_stats.interp_fallbacks += n;
    return r;
}

emu_run_reason_t emu_jit_run(emu_cpu_t *cpu, uint32_t budget,
                             uint32_t *retired, const emu_jit_ops_t *ops)
{
    uint32_t done = 0u;
    emu_run_reason_t reason = EMU_RUN_BUDGET;

    if (g_code == NULL) {
        return ops->interp->run(cpu, budget, retired);
    }

    if (ops->is_idle != NULL && ops->is_idle(cpu)) {
        if (!ops->wake(cpu)) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return EMU_RUN_WFI;
        }
    }

    while (done < budget) {
        const uint8_t st = ops->state(cpu);
        if (st != EMU_STATE_RUNNING) {
            reason = (st == EMU_STATE_HALTED) ? EMU_RUN_HALTED : EMU_RUN_WFI;
            break;
        }

        if (ops->take_irq != NULL && ops->take_irq(cpu)) {
            done++;
            continue;
        }

        /*
         * Whatever the translator specialised on, checked here rather than
         * left to each backend. Every staleness bug this project has found
         * was a block outliving something it baked in, so the comparison
         * is unconditional and the flush is total: a JIT that is subtly
         * wrong is worth far less than one that is slightly slower.
         */
        if (ops->generation != NULL) {
            const uint32_t gen = ops->generation(cpu);
            if (!g_have_generation || gen != g_generation) {
                if (g_have_generation && gen != g_generation) {
                    emu_jit_flush();
                }
                g_generation = gen;
                g_have_generation = true;
            }
        }

        /*
         * State the emitted code cannot check for itself. Unlike the
         * generation above this is not a staleness question -- blocks stay
         * valid -- it simply must not run while the guard is up.
         */
        if (ops->may_run != NULL && !ops->may_run(cpu)) {
            const emu_run_reason_t r = run_interp_one(cpu, ops, &done);
            if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                reason = r;
                break;
            }
            continue;
        }

        const uint32_t pc = ops->pc(cpu);
        jit_block_t *b = lookup(pc);

        if (b == NULL) {
            b = translate(cpu, pc, ops);
            if (b == NULL) {
                const emu_run_reason_t r = run_interp_one(cpu, ops, &done);
                if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                    reason = r;
                    break;
                }
                continue;
            }
        }

        const uint32_t n = ((block_fn_t)(void *)b->code)(cpu);
        g_stats.block_entries++;

        /*
         * A block that trapped on its first instruction retires nothing,
         * yet it made progress -- the trap moved pc into a handler.
         * Charging the budget nothing for that spins this loop forever
         * while the guest runs on underneath, which no instruction cap can
         * break because the cap is the caller's.
         */
        done += (n != 0u) ? n : 1u;

        if (n != 0u && ops->count != NULL) {
            ops->count(cpu, n);
        }
    }

    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}
