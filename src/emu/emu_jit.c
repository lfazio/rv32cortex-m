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

/*
 * Whether the buffer can be allocated here. A hosted build maps it with
 * the execute bit set; a microcontroller has no such call and the
 * platform hands one in instead. Defined either way rather than only when
 * true, so that -Wundef catches a misspelling at the #if below rather
 * than silently taking the wrong branch.
 */
#if defined(__linux__) && !defined(EMU_JIT_STATIC_BUFFER)
#  include <sys/mman.h>
#  define EMU_JIT_MMAP 1
#else
#  define EMU_JIT_MMAP 0
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
#if defined(EMU_JIT_THUMB2)
/*
 * Microcontroller sizes. These tables are .bss, and on a target every byte
 * of them is a byte the guest does not get: at the host figures below they
 * are 192 KB, which is more RAM than the part this runs on has to spare.
 * 256 of each is what the Thumb-2 backend used before the framework
 * existed, and the cache it manages is 12 KB.
 */
#  ifndef EMU_JIT_MAX_BLOCKS
#    define EMU_JIT_MAX_BLOCKS 256u
#  endif
#  ifndef EMU_JIT_HASH_SIZE
#    define EMU_JIT_HASH_SIZE  256u
#  endif
#  ifndef EMU_JIT_BLOCK_RESERVE
#    define EMU_JIT_BLOCK_RESERVE 512u
#  endif
#else
/*
 * Host sizes. There is no tension here -- the tables cost a machine with
 * gigabytes nothing -- and being generous matters: at 256 blocks CoreMark
 * flushed nineteen times in a run, and constant retranslation is exactly
 * what hides a translator bug behind a fresh translation.
 */
#  ifndef EMU_JIT_MAX_BLOCKS
#    define EMU_JIT_MAX_BLOCKS 8192u
#  endif
#  ifndef EMU_JIT_HASH_SIZE
#    define EMU_JIT_HASH_SIZE  8192u
#  endif
#  ifndef EMU_JIT_BLOCK_RESERVE
#    define EMU_JIT_BLOCK_RESERVE 8192u
#  endif
#endif

typedef struct {
    uint32_t guest_pc;
    uint8_t *code;
    uint32_t len;        /* bytes, so compaction can move it      */
    uint32_t insns;      /* guest instructions this block retires */
    uint32_t hits;       /* entries since the last ageing pass    */
    int32_t  next;       /* hash chain, -1 terminates             */
} jit_block_t;

/*
 * Hit counts are bucketed rather than compared directly, so choosing what
 * to retain is a histogram walk instead of a sort.
 */
#define HIT_BINS 16u

static uint32_t hit_bin(uint32_t hits)
{
    return (hits >= HIT_BINS) ? (HIT_BINS - 1u) : hits;
}

static uint8_t    *g_code;
static uint32_t    g_code_size;
static uint32_t    g_code_used;
static bool        g_owned;      /* did we map it, or was it handed over? */

static jit_block_t g_blocks[EMU_JIT_MAX_BLOCKS];
static uint32_t    g_block_count;
static int32_t     g_hash[EMU_JIT_HASH_SIZE];

uint8_t *emu_jit_cursor;
uint8_t *emu_jit_limit;
bool     emu_jit_overflow;

#define g_emit     emu_jit_cursor
#define g_emit_end emu_jit_limit
#define g_overflow emu_jit_overflow

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

#ifdef EMU_JIT_PROFILE
static uint32_t prof_now(void)
{
#if defined(__ARM_ARCH_7EM__)
    return *(volatile uint32_t *)0xE0001004u;   /* DWT CYCCNT */
#else
    return 0u;
#endif
}
#endif

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

/*
 * Chained, not direct-mapped.
 *
 * A one-entry bucket looks adequate -- the table is far larger than the
 * number of live blocks -- but a collision there does not cost a probe, it
 * *hides* a block: the loser is unreachable while still occupying its code
 * and its table slot, so it is translated again, and again, until the next
 * flush. Two hot blocks landing in one bucket retranslate each other every
 * time round the loop.
 *
 * Measured on CoreMark, where the working set is 19 blocks in a 256-entry
 * table and this still happened: 1977 translations against 1465, 205
 * compactions against 131. Nothing computed a wrong answer, which is why
 * the chain has to be here rather than remembered.
 */
static void chain_insert(uint32_t index)
{
    const uint32_t h = pc_hash(g_blocks[index].guest_pc);

    g_blocks[index].next = g_hash[h];
    g_hash[h] = (int32_t)index;
}

static void rebuild_hash(void)
{
    for (uint32_t i = 0; i < EMU_JIT_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    for (uint32_t i = 0; i < g_block_count; i++) {
        chain_insert(i);
    }
}

static jit_block_t *lookup(uint32_t pc)
{
    for (int32_t i = g_hash[pc_hash(pc)]; i >= 0; i = g_blocks[i].next) {
        if (g_blocks[i].guest_pc == pc) {
            if (g_blocks[i].hits != UINT32_MAX) {
                g_blocks[i].hits++;
            }
            return &g_blocks[i];
        }
    }
    return NULL;
}

/*
 * Reclaim space by discarding the least-used blocks and sliding the rest
 * down, rather than throwing the whole cache away.
 *
 * This matters most where the cache is smallest. On a microcontroller a
 * full flush at every exhaustion means the hot working set is retranslated
 * continuously; the policy below -- retain by hit count, with a budget so
 * there is room to translate afterwards, and an ageing pass so a block
 * that was hot long ago cannot hold its place forever -- was measured on
 * that target and is carried here unchanged.
 *
 * Only sound for relocatable code; see emu_jit_ops_t.
 */
static void compact(const emu_jit_ops_t *ops)
{
    uint32_t bytes[HIT_BINS];
#ifdef EMU_JIT_PROFILE
    const uint32_t t0 = prof_now();
#endif

    memset(bytes, 0, sizeof(bytes));
    for (uint32_t i = 0; i < g_block_count; i++) {
        bytes[hit_bin(g_blocks[i].hits)] += g_blocks[i].len;
    }

    /* Walk from hottest to coldest, taking bins while they fit. */
    const uint32_t budget =
        g_code_size - (g_code_size / 4u) - EMU_JIT_BLOCK_RESERVE;
    uint32_t acc = 0u;
    uint32_t threshold = HIT_BINS;      /* nothing retained by default */

    for (int32_t b = (int32_t)HIT_BINS - 1; b >= 0; b--) {
        if (acc + bytes[b] > budget) {
            break;
        }
        acc += bytes[b];
        threshold = (uint32_t)b;
    }

    /*
     * Never retain blocks that have run only once: they are as likely to
     * be first-execution noise as working set, and keeping them is what
     * filled the cache in the first place.
     */
    if (threshold < 2u) {
        threshold = 2u;
    }

    uint8_t *dst = g_code;
    uint32_t kept = 0u;

    /* Blocks are appended by a bump allocator, so this array is already in
     * increasing code-address order and dst never overtakes the source. */
    for (uint32_t i = 0; i < g_block_count; i++) {
        jit_block_t b = g_blocks[i];

        if (hit_bin(b.hits) < threshold) {
            g_stats.evictions++;
            continue;
        }
        dst = (uint8_t *)(((uintptr_t)dst + 3u) & ~(uintptr_t)3u);
        if (dst != b.code) {
            memmove(dst, b.code, b.len);
        }
        b.code = dst;
        b.hits >>= 1;                   /* age */
        dst += b.len;
        g_blocks[kept++] = b;
    }

    g_block_count = kept;
    g_code_used = (uint32_t)(dst - g_code);
    rebuild_hash();

    /* Every survivor moved, so the whole live range is freshly written. */
    if (ops->sync != NULL) {
        ops->sync(g_code, g_code_used);
    }
    g_stats.compactions++;
#ifdef EMU_JIT_PROFILE
    g_stats.cyc_compact += prof_now() - t0;
#endif
}

/* ------------------------------------------------------------------ */
/* Emitting                                                            */
/* ------------------------------------------------------------------ */


void emu_jit_emit_begin(void *buf, uint32_t bytes)
{
    g_emit = (uint8_t *)buf;
    g_emit_end = g_emit + bytes;
    g_overflow = false;
}

void emu_jit_rewind(uint8_t *to)
{
    if (to >= g_code && to <= g_emit) {
        g_emit = to;
    }
}

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

/* True when the block table or the code buffer has no room for a block. */
static bool space_low(void)
{
    return g_block_count >= EMU_JIT_MAX_BLOCKS ||
           g_code_used + EMU_JIT_BLOCK_RESERVE >= g_code_size;
}

/*
 * One translation attempt into the space that is there.
 *
 * Returns NULL two ways that must not be confused, which is what the
 * caller below is for: the translator declined (ordinary and frequent --
 * every interpreted instruction lands here), or it ran off the end of the
 * buffer, which is a reclaim signal.
 */
static jit_block_t *translate_once(emu_cpu_t *cpu, uint32_t pc,
                                   const emu_jit_ops_t *ops)
{
    /* Blocks are entered by branching to them, so keep them aligned. */
    g_code_used = (g_code_used + 3u) & ~3u;

    g_emit = g_code + g_code_used;
    g_emit_end = g_code + g_code_size;
    g_overflow = false;

    uint8_t *const start = g_emit;
#ifdef EMU_JIT_PROFILE
    const uint32_t t0 = prof_now();
#endif
    const uint32_t insns = ops->translate(cpu, pc);
#ifdef EMU_JIT_PROFILE
    g_stats.cyc_translate += prof_now() - t0;
#endif

    if (insns == 0u || g_overflow) {
        if (g_overflow) {
            g_stats.overflowed++;
        } else {
            g_stats.declined++;
        }
        return NULL;
    }

    jit_block_t *const b = &g_blocks[g_block_count++];
    b->guest_pc = pc;
    b->code = start;
    b->len = (uint32_t)(g_emit - start);
    b->insns = insns;
    b->hits = 0u;
    g_code_used = (uint32_t)(g_emit - g_code);
    chain_insert(g_block_count - 1u);

    /* The code was written as data; make it fetchable. */
    if (ops->sync != NULL) {
        ops->sync(b->code, b->len);
    }

    g_stats.translations++;
    return b;
}

/*
 * Translate at `pc`, reclaiming space if that is what is standing in the
 * way.
 *
 * The three outcomes have to stay distinct. "The translator declined" is
 * the common case and must reclaim nothing -- compacting once per
 * interpreted divide is a pathology this project has already had, and it
 * retranslated the same blocks tens of thousands of times. "The buffer
 * overflowed" is the opposite: nothing is wrong with the pc, there is
 * simply no room, so the attempt has to be retried after compacting
 * rather than handed to the interpreter.
 *
 * Getting that second case wrong does not fail a test. It cost 65% of all
 * host cycles on CoreMark -- 957 full translations emitted and thrown
 * away, the guest creeping forward one interpreted instruction at a time
 * between them -- while every suite passed and the guest computed the
 * right answer.
 */
static jit_block_t *translate(emu_cpu_t *cpu, uint32_t pc,
                              const emu_jit_ops_t *ops)
{
    if (!space_low()) {
        jit_block_t *b = translate_once(cpu, pc, ops);
        if (b != NULL) {
            return b;
        }
        if (!g_overflow) {
            return NULL;         /* nothing here to translate */
        }
    }

    /*
     * Genuinely out of room. Keep the hot blocks if the emitted code can
     * be moved; otherwise there is nothing to do but start again.
     */
    if (ops->relocatable) {
        compact(ops);
        if (!space_low()) {
            jit_block_t *b = translate_once(cpu, pc, ops);
            if (b != NULL) {
                return b;
            }
            if (!g_overflow) {
                return NULL;
            }
        }
    }

    /* Compaction could not free enough, or blocks cannot move. Start over. */
    emu_jit_flush();
    return translate_once(cpu, pc, ops);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

typedef uint32_t (*block_fn_t)(emu_cpu_t *);

/*
 * Turn the address of a translated block into something callable.
 *
 * On most hosts that is the identity. On Cortex-M it is not: the low bit
 * of a branch target selects the instruction set, and a Thumb-only core
 * takes a UsageFault rather than executing an ARM-state instruction. The
 * buffer is byte-addressed and its blocks are halfword-aligned, so that
 * bit is always clear and has to be put back.
 *
 * It lives here rather than in the frontend because it is a property of
 * the *host*, which is what this file knows about -- and because getting
 * it wrong does not produce a wrong answer to be caught by a test suite.
 * It faults on the first block entry, which is what the first framework
 * build of the Thumb-2 backend did: the banner printed and nothing else.
 */
static block_fn_t block_entry(const uint8_t *code)
{
#if defined(EMU_JIT_THUMB2)
    return (block_fn_t)(uintptr_t)((uintptr_t)code | 1u);
#else
    return (block_fn_t)(uintptr_t)code;
#endif
}

static emu_run_reason_t run_interp_one(emu_cpu_t *cpu,
                                       const emu_jit_ops_t *ops,
                                       uint32_t *done)
{
    uint32_t n = 0u;
    const emu_run_reason_t r = ops->interp->run(cpu, 1u, &n);

    *done += n;
    g_stats.interp_fallbacks += n;
    if (ops->after_interp != NULL) {
        ops->after_interp(cpu);
    }
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

    /* Once per call, not per dispatch; see emu_jit_hot_t. */
    emu_jit_hot_t hot;
    memset(&hot, 0, sizeof(hot));
    ops->bind(cpu, &hot);

    if (ops->is_idle != NULL && ops->is_idle(cpu)) {
        if (!ops->wake(cpu)) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return EMU_RUN_WFI;
        }
    }

    while (done < budget) {
        const uint8_t st = *hot.state;
        if (st != EMU_STATE_RUNNING) {
            reason = (st == EMU_STATE_HALTED) ? EMU_RUN_HALTED : EMU_RUN_WFI;
            break;
        }

        if (ops->take_irq != NULL &&
            (hot.irq_pending == NULL || *hot.irq_pending) &&
            ops->take_irq(cpu)) {
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
        if (hot.generation != NULL) {
            const uint32_t gen = *hot.generation;
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
        if (hot.blocked != NULL && *hot.blocked) {
            const emu_run_reason_t r = run_interp_one(cpu, ops, &done);
            if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                reason = r;
                break;
            }
            continue;
        }

        const uint32_t pc = *hot.pc;
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

        const uint32_t n = block_entry(b->code)(cpu);
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
