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
#include <sys/mman.h>
#define EMU_JIT_MMAP 1
#else
#define EMU_JIT_MMAP 0
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
#if defined(EMU_HOST_JIT_THUMB2)
/*
 * Microcontroller sizes. These tables are .bss, and on a target every byte
 * of them is a byte the guest does not get: at the host figures below they
 * are 192 KB, which is more RAM than the part this runs on has to spare.
 * 256 of each is what the Thumb-2 backend used before the framework
 * existed, and the cache it manages is 12 KB.
 */
#ifndef EMU_JIT_MAX_BLOCKS
#define EMU_JIT_MAX_BLOCKS 256u
#endif
#ifndef EMU_JIT_HASH_SIZE
#define EMU_JIT_HASH_SIZE 256u
#endif
#ifndef EMU_JIT_BLOCK_RESERVE
#define EMU_JIT_BLOCK_RESERVE 512u
#endif
#else
/*
 * Host sizes. There is no tension here -- the tables cost a machine with
 * gigabytes nothing -- and being generous matters: at 256 blocks CoreMark
 * flushed nineteen times in a run, and constant retranslation is exactly
 * what hides a translator bug behind a fresh translation.
 */
#ifndef EMU_JIT_MAX_BLOCKS
#define EMU_JIT_MAX_BLOCKS 8192u
#endif
#ifndef EMU_JIT_HASH_SIZE
#define EMU_JIT_HASH_SIZE 8192u
#endif
#ifndef EMU_JIT_BLOCK_RESERVE
#define EMU_JIT_BLOCK_RESERVE 8192u
#endif
#endif

emu_jit_layout_t emu_jit_layout;

typedef struct {
    uint32_t guest_pc;
    uint32_t context; /* see emu_jit_hot_t::context           */
    emu_jit_layout_t layout; /* how its exits may be chained   */
    uint8_t *code;
    uint32_t len; /* bytes, so compaction can move it      */
    uint32_t insns; /* guest instructions this block retires */
    uint32_t hits; /* entries since the last ageing pass    */
    int32_t next; /* hash chain, -1 terminates             */
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

static uint8_t *g_code;
static uint32_t g_code_size;
static uint32_t g_code_used;
static bool g_owned; /* did we map it, or was it handed over? */

static jit_block_t g_blocks[EMU_JIT_MAX_BLOCKS];
static uint32_t g_block_count;
static int32_t g_hash[EMU_JIT_HASH_SIZE];

uint8_t *emu_jit_cursor;
uint8_t *emu_jit_limit;
bool emu_jit_overflow;

#define g_emit emu_jit_cursor
#define g_emit_end emu_jit_limit
#define g_overflow emu_jit_overflow

static uint32_t g_generation;
static bool g_have_generation;

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
    return *(volatile uint32_t *)0xE0001004u; /* DWT CYCCNT */
#else
    return 0u;
#endif
}
#endif

/* ------------------------------------------------------------------ */
/* Blocks                                                              */
/* ------------------------------------------------------------------ */

/*
 * The context is mixed in, not just compared.
 *
 * **Leaving it out turns the chain into a linked list.** Blocks that
 * differ only by context -- the same user address under a dozen
 * processes, the same address in user and in kernel -- would hash to one
 * bucket and be told apart only by the compare, so a lookup walks every
 * one of them. Under Linux that was measured at roughly 383ns per block
 * entry, which is about fifteen hundred cycles to find a block.
 *
 * The shift keeps the pc's own bits where they were, so a guest with a
 * single context hashes exactly as before.
 */
static uint32_t pc_hash(uint32_t pc, uint32_t context)
{
    return ((pc >> 1) ^ (context * 2654435761u)) & (EMU_JIT_HASH_SIZE - 1u);
}

/* ------------------------------------------------------------------ */
/* The negative cache                                                  */
/* ------------------------------------------------------------------ */

/*
 * Which pcs the translator has already refused.
 *
 * **`declined` was the largest single term in a Linux run**: 636.8M
 * attempts against 638.7M interpreted instructions, 99.7%, each one a
 * full translation that produced nothing and was thrown away. The
 * reason a pc declines is a property of the code there -- an opcode no
 * backend lowers, a fetch that faults -- so asking a second time gets
 * the same answer for the same work.
 *
 * Keyed on the context as well as the pc, because the bytes at a
 * virtual address depend on the address space; the same pc under a
 * different satp is a different question.
 *
 * **Invalidated by an epoch rather than by clearing.** Every path that
 * could make a declined pc translatable again -- a generation change, a
 * FENCE.I, guest code being rewritten -- reaches emu_jit_flush, and a
 * Linux boot flushes 129,285 times. A memset of this table per flush
 * would be a new cost proportional to the old one; bumping a counter is
 * not.
 */
#define JIT_NEG_SIZE 1024u /* power of two */

static struct {
    uint32_t pc;
    uint32_t context;
    uint32_t epoch;
} g_neg[JIT_NEG_SIZE];

/*
 * Starts at 1 so that a zeroed table -- epoch 0 everywhere -- reads as
 * empty rather than as a valid entry for pc 0.
 */
static uint32_t g_neg_epoch = 1u;

static uint32_t neg_slot(uint32_t pc, uint32_t context)
{
    /*
     * Multiply and take the *high* bits, rather than masking the low
     * ones.
     *
     * The first version was `(pc >> 1) & (SIZE - 1)`, which reads only
     * bits 1..10 of the address -- so any two pcs 0x800 apart share a
     * slot and evict each other. That is not a rare collision, it is
     * every pair of declined addresses at the same offset in different
     * pages, which is precisely the shape guest code has. A unit test
     * using 0x8000 and 0x9000 caught it: the second evicted the first
     * and the first was translated a second time.
     *
     * Multiplying by a 32-bit odd constant carries every input bit
     * upward, so the top bits depend on the whole address.
     *
     * A collision is only ever a wasted translation -- the pc and the
     * context are compared before the entry is believed -- so this is a
     * question of how often, not of whether the answer is right.
     */
    uint32_t h = (pc >> 1) ^ (context * 2654435761u);

    h *= 2654435761u;
    return (h >> 16) & (JIT_NEG_SIZE - 1u);
}

static bool neg_hit(uint32_t pc, uint32_t context)
{
    const uint32_t s = neg_slot(pc, context);

    return g_neg[s].epoch == g_neg_epoch && g_neg[s].pc == pc &&
           g_neg[s].context == context;
}

static void neg_note(uint32_t pc, uint32_t context)
{
    const uint32_t s = neg_slot(pc, context);

    g_neg[s].pc = pc;
    g_neg[s].context = context;
    g_neg[s].epoch = g_neg_epoch;
}

static void neg_invalidate(void)
{
    g_neg_epoch++;
    if (g_neg_epoch == 0u) {
        /*
         * Wrapped, so stale entries would read as current. Only reachable
         * after 2^32 flushes -- about 33,000 Linux boots -- and cheap
         * enough to handle rather than document as a limit.
         */
        memset(g_neg, 0, sizeof(g_neg));
        g_neg_epoch = 1u;
    }
}

void emu_jit_flush(void)
{
    for (uint32_t i = 0; i < EMU_JIT_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    g_block_count = 0u;
    g_code_used = 0u;
    g_stats.flushes++;
    neg_invalidate();
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
    const uint32_t h =
        pc_hash(g_blocks[index].guest_pc, g_blocks[index].context);

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

/*
 * Point every chained exit back at its own block's tail.
 *
 * **Called whenever a block can move or die**, which here is compaction
 * -- it relocates the survivors and drops the rest, so a jump from a
 * surviving block into a moved or evicted one becomes a jump into
 * whatever now occupies those bytes. There is no test that would catch
 * that: the guest executes something, and what it executes depends on
 * what the allocator happened to put there.
 *
 * A flush needs none of this. It empties the table, so no block is
 * reachable and no stale jump inside one can be executed.
 */
static void unlink_all(const emu_jit_ops_t *ops)
{
    if (ops->patch_link == NULL) {
        return;
    }
    for (uint32_t i = 0; i < g_block_count; i++) {
        jit_block_t *const b = &g_blocks[i];

        for (uint32_t k = 0; k < b->layout.nlink; k++) {
            if (b->layout.link[k].linked) {
                ops->patch_link(b->code + b->layout.link[k].site,
                                b->code + b->layout.tail);
                b->layout.link[k].linked = false;
            }
        }
    }
}

/*
 * Link the exit the previous block took, now that its target is known.
 *
 * Done here rather than at translation because the target usually does
 * not exist yet: a block is built, runs, and only then is its successor
 * translated. Waiting until both are present links exactly the edges a
 * guest actually takes, and costs two compares on the dispatch that
 * would have happened anyway.
 */
static void link_exit(jit_block_t *from, const jit_block_t *to,
                      const emu_jit_ops_t *ops)
{
    if (ops->patch_link == NULL || from == NULL || !to->layout.chainable) {
        return;
    }
    for (uint32_t k = 0; k < from->layout.nlink; k++) {
        emu_jit_link_t *const l = &from->layout.link[k];

        if (!l->linked && l->target_pc == to->guest_pc) {
            ops->patch_link(from->code + l->site,
                            to->code + to->layout.chain_entry);
            l->linked = true;
            g_stats.links++;
            return;
        }
    }
}

static jit_block_t *lookup(uint32_t pc, uint32_t context)
{
    for (int32_t i = g_hash[pc_hash(pc, context)]; i >= 0;
         i = g_blocks[i].next) {
        if (g_blocks[i].guest_pc == pc && g_blocks[i].context == context) {
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
    uint32_t threshold = HIT_BINS; /* nothing retained by default */

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

    /*
     * Before anything moves. A surviving block's chained jump points
     * into another block's code, and compaction relocates or discards
     * that other block.
     */
    unlink_all(ops);

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
        b.hits >>= 1; /* age */
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
                                   const emu_jit_ops_t *ops,
                                   uint32_t context)
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
    memset(&emu_jit_layout, 0, sizeof(emu_jit_layout));

    const uint32_t insns = ops->translate(cpu, pc);
#ifdef EMU_JIT_PROFILE
    g_stats.cyc_translate += prof_now() - t0;
#endif

    if (insns == 0u || g_overflow) {
        if (g_overflow) {
            g_stats.overflowed++;
        } else {
            /*
             * Noted here and not in the caller, because this is the one
             * place that knows the attempt failed for a reason belonging
             * to the pc rather than to the buffer. An overflow must not
             * be remembered: nothing is wrong with that pc.
             */
            g_stats.declined++;
            neg_note(pc, context);
        }
        return NULL;
    }

    jit_block_t *const b = &g_blocks[g_block_count++];
    b->guest_pc = pc;
    b->context = context;
    b->layout = emu_jit_layout;
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
static jit_block_t *translate(emu_cpu_t *cpu, uint32_t pc, uint32_t context,
                              const emu_jit_ops_t *ops)
{
    if (!space_low()) {
        jit_block_t *b = translate_once(cpu, pc, ops, context);
        if (b != NULL) {
            return b;
        }
        if (!g_overflow) {
            return NULL; /* nothing here to translate */
        }
    }

    /*
     * Genuinely out of room. Keep the hot blocks if the emitted code can
     * be moved; otherwise there is nothing to do but start again.
     */
    if (ops->relocatable) {
        compact(ops);
        if (!space_low()) {
            jit_block_t *b = translate_once(cpu, pc, ops, context);
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
    return translate_once(cpu, pc, ops, context);
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
#if defined(EMU_HOST_JIT_THUMB2)
    return (block_fn_t)(uintptr_t)((uintptr_t)code | 1u);
#else
    return (block_fn_t)(uintptr_t)code;
#endif
}

#ifdef EMU_JIT_DIFF
/*
 * Run a block, and where the frontend offers a reference, run that too
 * and compare.
 *
 * The reference goes first and its result is set aside, then the guest
 * state is put back and the compiled code runs on the same input. That
 * ordering matters: it is the compiled code's state the caller must be
 * left with, because the reference is the thing under suspicion of being
 * right, not the thing being executed.
 */
#ifndef EMU_JIT_DIFF_MAX
#define EMU_JIT_DIFF_MAX 1024u
#endif

static uint8_t g_diff_before[EMU_JIT_DIFF_MAX];
static uint8_t g_diff_want[EMU_JIT_DIFF_MAX];

/* Reported by the platform; see the host runner. */
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got);

static uint32_t run_block_checked(emu_cpu_t *cpu, const jit_block_t *b,
                                  const emu_jit_ops_t *ops, bool fresh)
{
    /*
     * Only a block that was *just* translated can be checked.
     *
     * The reference runs the frontend's IR buffer, and that buffer holds
     * the last block translated -- not the last block entered. Blocks
     * are entered far more often than they are translated (104,686
     * against 356 on CoreMark), so checking on every entry compares a
     * block against a different block's IR and reports a stream of
     * mismatches that are all the harness's own fault.
     */
    if (!fresh || ops->diff_ref == NULL || ops->state_bytes == 0u ||
        ops->state_bytes > EMU_JIT_DIFF_MAX) {
        return block_entry(b->code)(cpu);
    }

    const uint32_t pc = b->guest_pc;

    memcpy(g_diff_before, cpu, ops->state_bytes);
    if (!ops->diff_ref(cpu)) {
        /* Declined -- restore and run normally, comparing nothing. */
        g_stats.diff_declined++;
        memcpy(cpu, g_diff_before, ops->state_bytes);
        return block_entry(b->code)(cpu);
    }
    g_stats.diff_checked++;
    memcpy(g_diff_want, cpu, ops->state_bytes);
    memcpy(cpu, g_diff_before, ops->state_bytes);

    const uint32_t n = block_entry(b->code)(cpu);

    const uint8_t *const got = (const uint8_t *)cpu;
    for (uint32_t off = 0; off + 4u <= ops->state_bytes; off += 4u) {
        uint32_t a, e;

        memcpy(&a, got + off, 4u);
        memcpy(&e, g_diff_want + off, 4u);
        if (a != e) {
            emu_jit_diff_report(pc, off, e, a);
            break; /* the first divergence is the one */
        }
    }
    return n;
}
#endif /* EMU_JIT_DIFF */

/*
 * How many instructions one fallback may interpret.
 *
 * **Not an interrupt-latency knob**, unlike the loop cap: a frontend's
 * interpreter takes interrupts inside its own loop -- RV32 checks
 * `irq_dirty` every instruction -- so a longer batch delays nothing.
 * What it trades is *coverage*: the batch can run past the point where
 * translatable code resumes, and those instructions are interpreted
 * instead of becoming a block.
 *
 * Which is why the size adapts rather than being fixed. See the note on
 * `batch` in emu_jit_run.
 */
#ifndef EMU_JIT_FALLBACK_MAX
#define EMU_JIT_FALLBACK_MAX 64u
#endif

/*
 * Interpret, when the translator declined or a guard is up.
 *
 * **One call per instruction was costing more than the instructions.**
 * This used to ask for exactly 1, so every declined instruction paid
 * the interpreter's whole run-loop entry -- the prologue, the hot-state
 * reload, the loop setup -- which the interpreter proper amortises over
 * a budget of thousands. Under Linux that is 228 million such calls in
 * a 600M-instruction run, and it is why 38% of instructions interpreted
 * cost far more than 38% of the interpreter's time.
 */
static emu_run_reason_t run_interp_batch(emu_cpu_t *cpu,
                                         const emu_jit_ops_t *ops,
                                         uint32_t *done, uint32_t budget,
                                         uint32_t batch)
{
    const uint32_t left = budget - *done;
    uint32_t want = (batch < left) ? batch : left;
    uint32_t n = 0u;

    if (want == 0u) {
        want = 1u;
    }

    const emu_run_reason_t r = ops->interp->run(cpu, want, &n);

    *done += n;
    g_stats.interp_fallbacks += n;
    /*
     * After the whole batch, not after each instruction. Everything the
     * key carries -- the rounding mode, FS, the PMP configuration, the
     * page-table generation, the privilege -- matters only to a
     * *translated block*, and none runs until this returns.
     */
    if (ops->after_interp != NULL) {
        ops->after_interp(cpu);
    }
    return r;
}

EMU_HOT_TEXT emu_run_reason_t emu_jit_run(emu_cpu_t *cpu, uint32_t budget,
                                          uint32_t *retired,
                                          const emu_jit_ops_t *ops)
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

    /*
     * How much the next fallback may interpret in one go.
     *
     * **Adaptive, because a fixed size is wrong at both ends.** One
     * instruction per call is what made the fallback dominate; a large
     * fixed batch would interpret straight past the point where
     * translatable code resumes, and those instructions never become a
     * block.
     *
     * So it doubles while translation keeps failing -- a trap handler
     * full of CSR writes is hundreds of instructions the translator
     * will not take, and paying a call each is the case being fixed --
     * and resets to one the moment a block is found. That keeps the
     * common shape exact: a hot loop containing a single declined
     * instruction interprets exactly that instruction and translates
     * everything around it, which is what it did before.
     *
     * The overshoot is bounded by the batch at the point translation
     * resumes, which is at most the length of the stretch that
     * declined.
     */
    uint32_t batch = 1u;
    /*
     * The block that last returned to this loop, so the exit it took can
     * be pointed straight at its successor. Cleared whenever the guest
     * did something other than fall out of a block -- an interpreted
     * instruction, a trap, a flush -- because then the edge about to be
     * taken is not the one that block's exit encodes.
     */
    jit_block_t *prev = NULL;

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
                    prev = NULL; /* every block it could name is gone */
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
            /*
             * A guard is long-lived -- Sdtrig stays armed -- so there is
             * nothing to be gained by creeping through it one
             * instruction at a time.
             */
            const emu_run_reason_t r =
                run_interp_batch(cpu, ops, &done, budget, batch);

            prev = NULL;
            if (batch < EMU_JIT_FALLBACK_MAX) {
                batch *= 2u;
            }
            if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                reason = r;
                break;
            }
            continue;
        }

        const uint32_t pc = *hot.pc;
        /*
         * Part of a block's identity, not a reason to throw blocks away.
         * See emu_jit_hot_t::context.
         */
        const uint32_t context = (hot.context != NULL) ? *hot.context : 0u;
        jit_block_t *b = lookup(pc, context);

        bool fresh = false;

        if (b == NULL) {
            /*
             * Do not ask again. The translator has already refused this
             * pc in this context and nothing has flushed since, so a
             * second attempt does the same work for the same answer --
             * and that work was 99.7% of the interpreted instructions in
             * a Linux boot.
             */
            if (neg_hit(pc, context)) {
                g_stats.declined_cached++;

                const emu_run_reason_t r =
                    run_interp_batch(cpu, ops, &done, budget, batch);

                prev = NULL;
                if (batch < EMU_JIT_FALLBACK_MAX) {
                    batch *= 2u;
                }
                if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                    reason = r;
                    break;
                }
                continue;
            }

            b = translate(cpu, pc, context, ops);
            fresh = (b != NULL);
            /* It may have compacted, which moved or dropped `prev`. */
            prev = NULL;
            if (b == NULL) {
                const emu_run_reason_t r =
                    run_interp_batch(cpu, ops, &done, budget, batch);

                prev = NULL;
                if (batch < EMU_JIT_FALLBACK_MAX) {
                    batch *= 2u;
                }
                if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
                    reason = r;
                    break;
                }
                continue;
            }
        }

        /*
         * A block is about to run, so whatever declined is behind us:
         * the next decline is an isolated one until proven otherwise.
         *
         * **Here rather than beside the translation.** Resetting only
         * when a *fresh* block is built leaves the batch pegged at its
         * maximum in steady state, because a warm cache answers from
         * the lookup and translates nothing -- and the batch then runs
         * past translatable code every time. Measured: interpreted rose
         * from 228M to 443M and block entries fell from 99M to 22M,
         * which is a JIT quietly turning itself off.
         */
        batch = 1u;

        /*
         * Point the exit `prev` took at the block about to run. Both are
         * present and the context matched, which is the moment the edge
         * is known to be real.
         */
        link_exit(prev, b, ops);
        prev = b;

#ifdef EMU_JIT_DIFF
        const uint32_t n = run_block_checked(cpu, b, ops, fresh);
#else
        const uint32_t n = block_entry(b->code)(cpu);
#endif
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
