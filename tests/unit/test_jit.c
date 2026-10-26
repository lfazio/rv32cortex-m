/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_jit.c - the JIT framework, tested without a frontend.
 *
 * Everything here drives `emu_jit_run` through a stub `emu_jit_ops_t`
 * whose translator always declines and whose interpreter retires
 * instructions without moving anything. That is deliberate: the
 * behaviour under test belongs to the framework, and a test written
 * against a real frontend would be asserting the frontend's lowering
 * decisions as much as the framework's bookkeeping -- which is how this
 * project has twice ended up with a test that passed against the bug it
 * named.
 *
 * No guest assembly, for the same reason. Hand-assembled halfword arrays
 * have hidden three defects in this tree by encoding what the author
 * believed rather than what the assembler produces.
 */

#include <string.h>

#include "tests.h"

#include "emu/emu_backend.h"
#include "emu/emu_jit.h"
#if defined(EMU_HOST_JIT_X86_64)
#include "emu/emu_x86_64.h" /* x86_patch_rel32, for the link hook below */
#endif

/* ------------------------------------------------------------------ */
/* A frontend that translates nothing                                  */
/* ------------------------------------------------------------------ */

/*
 * The pc does not move.
 *
 * That is the whole point: the negative cache exists because a guest
 * revisits a pc the translator has already refused -- a loop containing
 * one untranslatable instruction -- and a stub whose pc advanced would
 * present a fresh address every time and never exercise it.
 */
static uint32_t g_pc = 0x8000u;
static uint32_t g_generation;
static uint32_t g_translate_calls;

/*
 * The loop reads this one *unguarded*, first thing round, so a stub that
 * leaves it NULL segfaults rather than failing an assertion -- which is
 * what the first version of this file did. `hot` is memset to zero
 * before bind, so every pointer the dispatch loop dereferences without a
 * NULL test has to be filled here.
 */
static uint8_t g_state = EMU_STATE_RUNNING;

static emu_run_reason_t stub_interp_run(emu_cpu_t *cpu, uint32_t budget,
                                        uint32_t *retired)
{
    (void)cpu;
    if (retired != NULL) {
        *retired = budget;
    }
    return EMU_RUN_BUDGET;
}

static const emu_backend_t k_stub_interp = {
    .name = "stub-interp",
    .init = NULL,
    .reset = NULL,
    .run = stub_interp_run,
    .invalidate = NULL,
};

static void stub_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    (void)cpu;
    out->pc = &g_pc;
    out->state = &g_state;
    out->generation = &g_generation;
    out->blocked = NULL;
    out->context = NULL;
    out->irq_pending = NULL;
}

/* Always declines, which is the case the negative cache is about. */
static uint32_t stub_translate(emu_cpu_t *cpu, uint32_t pc)
{
    (void)cpu;
    (void)pc;
    g_translate_calls++;
    return 0u;
}

static const emu_jit_ops_t k_stub_ops = {
    .name = "stub",
    .bind = stub_bind,
    .translate = stub_translate,
    .interp = &k_stub_interp,
    .after_interp = NULL,
    .patch_link = NULL,
    .is_idle = NULL,
    .wake = NULL,
    .take_irq = NULL,
    .count = NULL,
    .sync = NULL,
    .relocatable = false,
};

/* A non-NULL cookie; the stub never dereferences it. */
static emu_cpu_t *const k_cpu = (emu_cpu_t *)(uintptr_t)0x1000u;

static uint32_t run_budget(uint32_t budget)
{
    uint32_t retired = 0u;

    (void)emu_jit_run(k_cpu, budget, &retired, &k_stub_ops);
    return retired;
}

/* ------------------------------------------------------------------ */
/* The negative cache                                                  */
/* ------------------------------------------------------------------ */

/*
 * A pc the translator refused must not be offered to it again.
 *
 * `declined` was the largest single term in a Linux boot -- 636.8M
 * attempts against 638.7M interpreted instructions, each a full
 * translation thrown away -- because the loop asked at every dispatch.
 * The answer depends on the code at that pc, so it cannot change
 * without an invalidation.
 */
static void test_declined_pc_is_not_retried(void)
{
    emu_jit_flush();
    g_translate_calls = 0u;

    emu_jit_stats_t before;
    emu_jit_stats_t after;

    emu_jit_get_stats(&before);
    (void)run_budget(4096u);
    emu_jit_get_stats(&after);

    /*
     * Exactly one attempt, however many times the loop came round.
     * A bound would pass against a cache that never hit, because the
     * batch doubles and would keep the count small on its own -- so
     * this is an equality and the translator's own call counter is
     * checked beside the framework's.
     */
    CHECK_EQ(after.declined - before.declined, 1u);
    CHECK_EQ(g_translate_calls, 1u);

    /* And the skips have to be counted, or the fix cannot be measured. */
    CHECK(after.declined_cached > before.declined_cached);
}

/*
 * ...and a flush must make it askable again.
 *
 * Every path that can change the answer -- a generation change, FENCE.I,
 * the guest rewriting its own code -- reaches emu_jit_flush, so that is
 * the single point the cache is invalidated at. **Getting this wrong is
 * silent and permanent**: a pc refused once would never be translated
 * again for the life of the process, which no test of correctness would
 * notice because declining is always a legal answer.
 */
static void test_flush_reopens_a_declined_pc(void)
{
    emu_jit_flush();
    g_translate_calls = 0u;

    (void)run_budget(4096u);
    CHECK_EQ(g_translate_calls, 1u);

    emu_jit_flush();

    (void)run_budget(4096u);
    CHECK_EQ(g_translate_calls, 2u);
}

/*
 * Translate only what is hot: below the threshold a missing pc is
 * interpreted, one instruction at a time, and offered to the translator
 * only on the miss that reaches it.
 *
 * Exact counts, for the reason the test above gives: a bound would pass
 * against a threshold that is ignored. Three misses must reach the
 * translator zero times; the fifth exactly once; and `cold` must say
 * four, because that is what the option costs and what it has to be
 * judged by.
 */
static void test_hot_threshold_delays_translation(void)
{
    emu_jit_stats_t before;
    emu_jit_stats_t after;

    emu_jit_flush();
    g_translate_calls = 0u;
    emu_jit_set_hot_threshold(5u);
    emu_jit_get_stats(&before);

    (void)run_budget(3u);
    CHECK_EQ(g_translate_calls, 0u);

    (void)run_budget(4096u);
    CHECK_EQ(g_translate_calls, 1u);
    emu_jit_get_stats(&after);
    CHECK_EQ(after.cold - before.cold, 4u);

    /* A flush says nothing about what is still hot: count again. */
    emu_jit_flush();
    (void)run_budget(4u);
    CHECK_EQ(g_translate_calls, 1u);
    (void)run_budget(1u);
    CHECK_EQ(g_translate_calls, 2u);

    /* Clamped to what the table can count. */
    emu_jit_set_hot_threshold(0u);
    CHECK_EQ(emu_jit_hot_threshold(), 1u);
    emu_jit_set_hot_threshold(1000u);
    CHECK_EQ(emu_jit_hot_threshold(), 127u); /* below the eviction counts */

    emu_jit_set_hot_threshold(EMU_JIT_HOT_THRESHOLD);
}

/*
 * A generation change flushes, so it reopens the pc too -- by the same
 * mechanism, but through the path a frontend actually uses.
 */
static void test_generation_change_reopens_a_declined_pc(void)
{
    emu_jit_flush();
    g_translate_calls = 0u;

    (void)run_budget(4096u);
    CHECK_EQ(g_translate_calls, 1u);

    g_generation++;

    (void)run_budget(4096u);
    CHECK_EQ(g_translate_calls, 2u);
}

/*
 * Two pcs are two answers.
 *
 * A direct-mapped table keyed badly -- or a single remembered pc --
 * would pass both tests above and still refuse to translate anything
 * after the first decline. This is the check that the key is the pc.
 */
static void test_cache_is_per_pc(void)
{
    emu_jit_flush();
    g_translate_calls = 0u;

    g_pc = 0x8000u;
    (void)run_budget(64u);
    CHECK_EQ(g_translate_calls, 1u);

    g_pc = 0x9000u;
    (void)run_budget(64u);
    CHECK_EQ(g_translate_calls, 2u);

    /* Back to the first, which is still remembered. */
    g_pc = 0x8000u;
    (void)run_budget(64u);
    CHECK_EQ(g_translate_calls, 2u);

    g_pc = 0x8000u;
}

/* ------------------------------------------------------------------ */
/* The context key                                                     */
/* ------------------------------------------------------------------ */

/*
 * A frontend whose context the test sets, so the key's width is what is
 * under test.
 *
 * RV32 puts the FP unit's state above bit 31: FS off-ness and frm went
 * into the context when they were found flushing the whole cache twice
 * per Linux system call, and the low word had no room. **A framework that
 * kept only 32 bits of the key anywhere would merge a block built with
 * the FP unit on into the world where it is off** -- and the FP
 * instructions in it would run where they must raise illegal-instruction.
 * So every context below differs from its neighbour only in the high
 * word.
 */
static uint64_t g_ctx;

static void stub_bind_ctx(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    stub_bind(cpu, out);
    out->context = &g_ctx;
}

static const emu_jit_ops_t k_stub_ctx_ops = {
    .name = "stub-ctx",
    .bind = stub_bind_ctx,
    .translate = stub_translate,
    .interp = &k_stub_interp,
    .relocatable = false,
};

static uint32_t run_ctx(uint32_t budget, const emu_jit_ops_t *ops)
{
    uint32_t retired = 0u;

    (void)emu_jit_run(k_cpu, budget, &retired, ops);
    return retired;
}

#define CTX_LO 0x80012345u /* a plausible satp: Sv32, ASID 0, a PPN */

/* The negative cache is keyed on the whole context, not its low word. */
static void test_declines_are_per_context(void)
{
    emu_jit_flush();
    g_translate_calls = 0u;

    g_ctx = CTX_LO;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 1u);

    g_ctx = CTX_LO | (1ull << 32);
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 2u);

    /* Both are remembered. */
    g_ctx = CTX_LO;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    g_ctx = CTX_LO | (1ull << 32);
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 2u);
}

#if defined(EMU_HOST_JIT_X86_64)
static uint32_t g_built;

/*
 * A real block: `mov eax, 1; ret`, which retires one instruction and
 * leaves the pc where it was, so the next pass looks the same pc up again.
 */
static uint32_t stub_translate_ret1(emu_cpu_t *cpu, uint32_t pc)
{
    (void)cpu;
    (void)pc;
    g_built++;
    emu_jit_emit8(0xB8u);
    emu_jit_emit32(1u);
    emu_jit_emit8(0xC3u);
    return 1u;
}

static const emu_jit_ops_t k_stub_build_ops = {
    .name = "stub-build",
    .bind = stub_bind_ctx,
    .translate = stub_translate_ret1,
    .interp = &k_stub_interp,
    .relocatable = false,
};

/*
 * The block table is keyed on the whole context: two contexts differing
 * only above bit 31 are two blocks, both found again afterwards, and
 * nothing is flushed to get there.
 *
 * This is the direction that matters for correctness. The negative
 * cache above is allowed to fold its key, because a false hit only
 * interprets; here a false hit *runs a block built for somewhere else*.
 *
 * What it catches, checked by breaking each: a 32-bit context anywhere
 * on the path from `hot.context` to the table (5 failures). What it does
 * **not** catch is a truncation in lookup's compare alone -- the hash
 * still sees both words and puts the two contexts in different buckets,
 * so they never meet at the compare. That needs two contexts that
 * collide in the hash, and choosing them would mean copying the hash
 * into this file.
 */
static void test_blocks_are_per_context(void)
{
    emu_jit_flush();
    g_built = 0u;

    emu_jit_stats_t before;
    emu_jit_stats_t after;

    emu_jit_get_stats(&before);

    g_ctx = CTX_LO;
    (void)run_ctx(64u, &k_stub_build_ops);
    CHECK_EQ(g_built, 1u);

    g_ctx = CTX_LO | (1ull << 35);
    (void)run_ctx(64u, &k_stub_build_ops);
    CHECK_EQ(g_built, 2u);

    g_ctx = CTX_LO;
    (void)run_ctx(64u, &k_stub_build_ops);
    g_ctx = CTX_LO | (1ull << 35);
    (void)run_ctx(64u, &k_stub_build_ops);
    CHECK_EQ(g_built, 2u);

    emu_jit_get_stats(&after);
    CHECK_EQ(after.flushes - before.flushes, 0u);
    CHECK_EQ(after.blocks, 2u);
}

/* ------------------------------------------------------------------ */
/* Invalidating a range                                                */
/* ------------------------------------------------------------------ */

/*
 * Blocks of a chosen length, so that one can start on a page and run
 * onto the next. `mov eax, n; ret` retires n and leaves the pc alone.
 */
static uint32_t g_len = 1u;

static uint32_t stub_translate_len(emu_cpu_t *cpu, uint32_t pc)
{
    (void)cpu;
    (void)pc;
    g_built++;
    emu_jit_emit8(0xB8u);
    emu_jit_emit32(g_len);
    emu_jit_emit8(0xC3u);
    return g_len;
}

/* Never called -- these blocks are not chainable -- but a host without
 * it gets a flush instead of a retirement, which is not what is tested. */
static void stub_emit_jump(uint8_t *at, const uint8_t *to)
{
    (void)at;
    (void)to;
}

static const emu_jit_ops_t k_stub_len_ops = {
    .name = "stub-len",
    .bind = stub_bind_ctx,
    .translate = stub_translate_len,
    .interp = &k_stub_interp,
    .emit_jump = stub_emit_jump,
    .jump_bytes = 5u,
    .relocatable = false,
};

static void build_at(uint32_t pc, uint32_t len)
{
    g_pc = pc;
    g_len = len;
    (void)run_ctx(64u, &k_stub_len_ops);
}

/* How many of these are rebuilt by running each again. */
static uint32_t rebuilt(const uint32_t *pcs, const uint32_t *lens, uint32_t n)
{
    const uint32_t before = g_built;

    for (uint32_t i = 0; i < n; i++) {
        build_at(pcs[i], lens[i]);
    }
    return g_built - before;
}

/*
 * A PowerPC TLB entry changing: the blocks whose code it covered go, and
 * no others. The framework indexes blocks by the page they start on, so
 * the one case the index alone would miss is first here -- a block
 * starting on the page *before* the range and running onto it, which
 * only the walk back by the longest span finds.
 *
 * Then the same through the path for wide ranges, which scans the table
 * instead, and the narrow path after it, which needs the index that
 * path rebuilt.
 */
static void test_invalidate_range_retires_only_overlap(void)
{
    /* page 0xA0 running onto 0xA1; page 0xA2; page 0xA5 */
    static const uint32_t pcs[3] = {0x000A0FF0u, 0x000A2000u, 0x000A5000u};
    static const uint32_t lens[3] = {8u, 1u, 1u};
    emu_jit_stats_t before;
    emu_jit_stats_t after;

    emu_jit_flush();
    g_ctx = CTX_LO;
    g_built = 0u;
    CHECK_EQ(rebuilt(pcs, lens, 3u), 3u);
    CHECK_EQ(rebuilt(pcs, lens, 3u), 0u); /* all found again */

    emu_jit_get_stats(&before);
    emu_jit_invalidate_range(0x000A1000u, 0x000A1FFFu, &k_stub_len_ops);
    emu_jit_get_stats(&after);
    CHECK_EQ(after.page_invalidations - before.page_invalidations, 1u);
    CHECK_EQ(after.flushes - before.flushes, 0u);
    CHECK_EQ(rebuilt(pcs, lens, 1u), 1u); /* the spanning one */
    CHECK_EQ(rebuilt(pcs + 1, lens + 1, 2u), 0u); /* the others stayed */

    /* Wide: a megabyte, past the walk's limit. */
    emu_jit_invalidate_range(0x000A2000u, 0x001A1FFFu, &k_stub_len_ops);
    CHECK_EQ(rebuilt(pcs, lens, 1u), 0u);
    CHECK_EQ(rebuilt(pcs + 1, lens + 1, 2u), 2u);

    /* Narrow again, on the rebuilt index. */
    emu_jit_invalidate_range(0x000A5000u, 0x000A5FFFu, &k_stub_len_ops);
    CHECK_EQ(rebuilt(pcs + 2, lens + 2, 1u), 1u);
    CHECK_EQ(rebuilt(pcs, lens, 2u), 0u);
}

/*
 * And what was refused in the range is asked again, while a refusal
 * outside it is not: the miss handler that maps a page is code the
 * translator declines, and forgetting that at every refill cost 188,773
 * translation attempts in one PowerPC CoreMark run.
 */
static void test_invalidate_range_reopens_only_its_declines(void)
{
    emu_jit_flush();
    g_ctx = CTX_LO;
    g_translate_calls = 0u;

    g_pc = 0x000B0010u;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    g_pc = 0x000C0010u;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 2u);

    emu_jit_invalidate_range(0x000B0000u, 0x000B0FFFu, &k_stub_len_ops);
    g_pc = 0x000B0010u;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 3u); /* in the range: offered again */
    g_pc = 0x000C0010u;
    (void)run_ctx(64u, &k_stub_ctx_ops);
    CHECK_EQ(g_translate_calls, 3u); /* outside it: still refused */
    g_pc = 0x8000u;
}

/* ------------------------------------------------------------------ */
/* Chained blocks across an overflow                                   */
/* ------------------------------------------------------------------ */

/*
 * A guest of four blocks: A -> B -> C -> END, and BIG -> END. END is
 * declined, so the stub interpreter runs there and chooses where the
 * guest goes next. Each block stores its successor's address in the pc
 * and leaves through a patchable jump, which is the whole of what a
 * chained exit is.
 *
 * B, C and BIG carry padding. Their size is the test: B and C have to be
 * worth evicting, and BIG has to fit the buffer only once they are gone.
 */
#define CH_A 0x9000u
#define CH_B 0x9004u
#define CH_C 0x9008u
#define CH_BIG 0x9100u
#define CH_END 0x9200u
#define CH_MID 0x9300u /* big enough to fill the buffer, run once */
#define CH_TRIG 0x9400u /* small; asking for it compacts */

#define CH_BUFFER (64u * 1024u) /* what test_jit hands emu_jit_init */
#define CH_PAD_SMALL 4000u
#define CH_PAD_BIG 60000u
#define CH_PAD_MID 50000u

static uint32_t g_ch_built[7]; /* A, B, C, BIG, other, MID, TRIG */
static uint8_t *g_ch_a_code;
static uint32_t g_ch_a_site;
static uint32_t g_ch_a_tail;
static uint32_t g_ch_end_visits;
static uint32_t g_ch_big_at; /* the END visit after which BIG runs; 0 = never */
static uint32_t g_ch_mid_at; /* ...MID, and TRIG, likewise */
static uint32_t g_ch_trig_at;
static uint32_t g_ch_patches;
static uint32_t g_ch_patched_overflowed;

static uint32_t stub_translate_chain(emu_cpu_t *cpu, uint32_t pc)
{
    uint32_t next;
    uint32_t pad;
    uint32_t which;

    (void)cpu;
    switch (pc) {
    case CH_A:
        next = CH_B;
        pad = 0u;
        which = 0u;
        break;
    case CH_B:
        next = CH_C;
        pad = CH_PAD_SMALL;
        which = 1u;
        break;
    case CH_C:
        next = CH_END;
        pad = CH_PAD_SMALL;
        which = 2u;
        break;
    case CH_BIG:
        next = CH_END;
        pad = CH_PAD_BIG;
        which = 3u;
        break;
    case CH_MID:
        next = CH_END;
        pad = CH_PAD_MID;
        which = 5u;
        break;
    case CH_TRIG:
        next = CH_END;
        pad = 0u;
        which = 6u;
        break;
    default:
        g_ch_built[4]++;
        return 0u;
    }
    g_ch_built[which]++;

    uint8_t *const start = emu_jit_here();
    const uint64_t pc_addr = (uint64_t)(uintptr_t)&g_pc;

    for (uint32_t i = 0; i < pad; i++) {
        emu_jit_emit8(0x90u); /* nop */
    }
    emu_jit_emit8(0x48u); /* mov rax, &g_pc */
    emu_jit_emit8(0xB8u);
    emu_jit_emit32((uint32_t)pc_addr);
    emu_jit_emit32((uint32_t)(pc_addr >> 32));
    emu_jit_emit8(0xC7u); /* mov dword [rax], next */
    emu_jit_emit8(0x00u);
    emu_jit_emit32(next);
    emu_jit_emit8(0xE9u); /* jmp tail -- the chained exit, unlinked */

    const uint32_t site = (uint32_t)(emu_jit_here() - start);

    emu_jit_emit32(0u);

    const uint32_t tail = (uint32_t)(emu_jit_here() - start);

    emu_jit_emit8(0xB8u); /* mov eax, 1 */
    emu_jit_emit32(1u);
    emu_jit_emit8(0xC3u); /* ret */

    emu_jit_layout.chain_entry = 0u;
    emu_jit_layout.tail = tail;
    emu_jit_layout.link[0].target_pc = next;
    emu_jit_layout.link[0].site = site;
    emu_jit_layout.link[0].linked = false;
    emu_jit_layout.nlink = 1u;
    emu_jit_layout.chainable = true;

    if (pc == CH_A) {
        g_ch_a_code = start;
        g_ch_a_site = site;
        g_ch_a_tail = tail;
    }
    return 1u;
}

/*
 * The link hook, **guarded exactly as both hosts' were**: it goes through
 * the emitter's own patcher, which declines to write while the buffer
 * has overflowed. That guard is right during emission -- the slot may be
 * past the end -- and is why a framework that leaves the flag set across
 * a compaction unlinks nothing while marking everything unlinked.
 */
static void stub_patch_link(uint8_t *site, const uint8_t *target)
{
    g_ch_patches++;
    if (emu_jit_overflowed()) {
        g_ch_patched_overflowed++;
    }
    x86_patch_rel32(site, target);
}

static emu_run_reason_t stub_interp_chain(emu_cpu_t *cpu, uint32_t budget,
                                          uint32_t *retired)
{
    (void)cpu;
    (void)budget;
    g_ch_end_visits++;
    g_pc = (g_ch_end_visits == g_ch_big_at)    ? CH_BIG
           : (g_ch_end_visits == g_ch_mid_at)  ? CH_MID
           : (g_ch_end_visits == g_ch_trig_at) ? CH_TRIG
                                               : CH_A;
    if (retired != NULL) {
        *retired = 1u;
    }
    return EMU_RUN_BUDGET;
}

static const emu_backend_t k_stub_interp_chain = {
    .name = "stub-interp-chain",
    .run = stub_interp_chain,
};

static const emu_jit_ops_t k_stub_chain_ops = {
    .name = "stub-chain",
    .bind = stub_bind,
    .translate = stub_translate_chain,
    .interp = &k_stub_interp_chain,
    .patch_link = stub_patch_link,
    .relocatable = true,
};

static int32_t ch_a_rel32(void)
{
    int32_t rel;

    memcpy(&rel, g_ch_a_code + g_ch_a_site, sizeof(rel));
    return rel;
}

/*
 * A compaction caused by an *overflow* must unlink, exactly as one
 * caused by the reserve running low does.
 *
 * It did not. The overflow flag is the emitter's, it was still set when
 * compact() ran, and each host's link hook reaches the emitter's patcher
 * -- which writes nothing while that flag is up. So unlink_all marked
 * every exit unlinked and rewrote none of them, and the survivors were
 * then moved with jumps into blocks that had been evicted. On the
 * Nucleo-F746ZG that was a branch to 0x7A4 bytes *below* the code
 * buffer, executing .bss until something was undefined.
 *
 * Only reachable when a block is larger than EMU_JIT_BLOCK_RESERVE,
 * because otherwise the reserve triggers the compaction first and the
 * flag is clear. That is 8192 bytes on a host, which no block reaches,
 * so no host run ever overflowed -- and 512 on a microcontroller, where
 * every guest that fills the cache does. RV32 CoreMark at 120
 * iterations overflows 982 times on the Nucleo-F746ZG; built from the
 * commit before the fix it prints its banner and never finishes, and
 * with this one change applied to that tree it completes.
 *
 * The shape here is the board's: A is entered from the dispatcher every
 * time round and stays hot, B and C are reached through the chain and so
 * are looked up once each, and the compaction keeps the caller and
 * evicts what it jumps to.
 */
static void test_overflow_compaction_unlinks(void)
{
    emu_jit_stats_t before;
    emu_jit_stats_t after;

    /*
     * With chain heat off. What is under test is that survivors' jumps
     * into blocks a compaction evicts are unlinked, and under chain heat
     * a survivor never jumps into an evicted block: B and C inherit A's
     * count and survive with it. The scenario needs the eviction.
     */
    emu_jit_set_chain_heat(false);
    emu_jit_flush();
    memset(g_ch_built, 0, sizeof(g_ch_built));
    g_ch_end_visits = 0u;
    g_ch_big_at = 0u;
    g_ch_mid_at = 0u;
    g_ch_trig_at = 0u;
    g_ch_patches = 0u;
    g_ch_patched_overflowed = 0u;
    g_ch_a_code = NULL;

    /* Warm up: A becomes hot, and A->B and B->C get linked. */
    uint32_t retired = 0u;

    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 64u, &retired, &k_stub_chain_ops);
    CHECK_EQ(g_ch_built[0], 1u);
    CHECK_EQ(g_ch_built[1], 1u);
    CHECK_EQ(g_ch_built[2], 1u);
    CHECK(g_ch_a_code != NULL);
    if (g_ch_a_code == NULL) {
        return;
    }
    /* Linked: A's exit no longer reaches its own tail. */
    CHECK(ch_a_rel32() != (int32_t)(g_ch_a_tail - (g_ch_a_site + 4u)));

    emu_jit_get_stats(&before);

    /* BIG does not fit until B and C have gone. */
    g_ch_big_at = g_ch_end_visits + 1u;
    (void)emu_jit_run(k_cpu, 16u, &retired, &k_stub_chain_ops);

    emu_jit_get_stats(&after);

    /*
     * The scenario happened -- without these the rest is a statement
     * about a compaction that was never caused this way. The first
     * attempt at BIG overflowed, a compaction followed, and the second
     * attempt fitted.
     *
     * Two compactions, and only the first is the one under test: BIG
     * then fills the buffer to within the reserve, so bringing B back
     * compacts again -- by the ordinary route, with the flag clear. A
     * count of one here means B was never asked for, which is the bug.
     */
    CHECK_EQ(after.overflowed - before.overflowed, 1u);
    CHECK_EQ(after.compactions - before.compactions, 2u);
    CHECK_EQ(after.flushes - before.flushes, 0u);
    CHECK_EQ(g_ch_built[3], 2u);

    /* No hook on committed code ever saw the emitter's flag. */
    CHECK(g_ch_patches != 0u);
    CHECK_EQ(g_ch_patched_overflowed, 0u);

    /*
     * B was evicted, so A had to come back through the dispatcher for
     * it. With the stale jump A instead runs straight into BIG's padding,
     * which now occupies the bytes B had, and B is never asked for.
     */
    CHECK_EQ(g_ch_built[1], 2u);
    emu_jit_set_chain_heat(EMU_JIT_CHAIN_HEAT != 0);
}

/*
 * Chain heat: a block reached only through a chain keeps its place.
 *
 * A is entered from the dispatcher every lap, B and C only through A's
 * and B's linked exits -- so by lookups B and C have run zero times and
 * A dozens. MID fills the buffer and runs once; asking for TRIG then
 * compacts, ordinarily (no overflow, no flush). With heat B survives and
 * is not built again; without it B is evicted and rebuilt when A next
 * exits to it. Both are asserted, so the test cannot pass against a
 * switch that does nothing.
 */
static uint32_t chain_heat_rebuilds_b(bool heat)
{
    emu_jit_stats_t before;
    emu_jit_stats_t after;
    uint32_t retired = 0u;

    emu_jit_set_chain_heat(heat);
    emu_jit_flush();
    memset(g_ch_built, 0, sizeof(g_ch_built));
    g_ch_end_visits = 0u;
    g_ch_big_at = 0u;
    g_ch_mid_at = 0u;
    g_ch_trig_at = 0u;

    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 64u, &retired, &k_stub_chain_ops);

    g_ch_mid_at = g_ch_end_visits + 1u;
    (void)emu_jit_run(k_cpu, 16u, &retired, &k_stub_chain_ops);

    emu_jit_get_stats(&before);
    g_ch_trig_at = g_ch_end_visits + 1u;
    (void)emu_jit_run(k_cpu, 32u, &retired, &k_stub_chain_ops);
    emu_jit_get_stats(&after);

    /* The scenario: one ordinary compaction, and MID built once. */
    CHECK_EQ(after.compactions - before.compactions, 1u);
    CHECK_EQ(after.overflowed - before.overflowed, 0u);
    CHECK_EQ(after.flushes - before.flushes, 0u);
    CHECK_EQ(g_ch_built[5], 1u);
    CHECK_EQ(g_ch_built[6], 1u);

    emu_jit_set_chain_heat(EMU_JIT_CHAIN_HEAT != 0);
    return g_ch_built[1];
}

static void test_chain_heat_keeps_chained_blocks(void)
{
    CHECK_EQ(chain_heat_rebuilds_b(true), 1u);
    CHECK_EQ(chain_heat_rebuilds_b(false), 2u);
}

/*
 * After emu_jit_unchain, a budget of 1 runs one block.
 *
 * What the gdb stub runs between breakpoints. Linked, A's exit goes
 * straight into B and B's into C, and the call comes back once with the
 * pc past all three -- over any breakpoint in B or C, and in a debug
 * session the links are whatever an earlier unbroken run left. The pc
 * says how far a call went; the retired count cannot, because each
 * block reports its own and only the last one's comes back.
 */
static void test_unchain_runs_one_block(void)
{
    uint32_t retired = 0u;

    emu_jit_flush();
    memset(g_ch_built, 0, sizeof(g_ch_built));
    g_ch_end_visits = 0u;
    g_ch_big_at = 0u;
    g_ch_mid_at = 0u;
    g_ch_trig_at = 0u;
    g_ch_a_code = NULL;

    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 64u, &retired, &k_stub_chain_ops);
    CHECK(g_ch_a_code != NULL);
    if (g_ch_a_code == NULL) {
        return;
    }
    CHECK(ch_a_rel32() !=
          (int32_t)(g_ch_a_tail - (g_ch_a_site + 4u))); /* linked */

    /* Linked: one call from A runs the chain to its end. */
    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 1u, &retired, &k_stub_chain_ops);
    CHECK_EQ(g_pc, CH_END);

    /* Unchained: A alone, and its exit is its own tail again. */
    emu_jit_unchain();
    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 1u, &retired, &k_stub_chain_ops);
    CHECK_EQ(g_pc, CH_B);
    CHECK_EQ(ch_a_rel32(), (int32_t)(g_ch_a_tail - (g_ch_a_site + 4u)));

    /* Calls of one block link nothing, so it stays that way... */
    (void)emu_jit_run(k_cpu, 1u, &retired, &k_stub_chain_ops);
    CHECK_EQ(g_pc, CH_C);
    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 1u, &retired, &k_stub_chain_ops);
    CHECK_EQ(g_pc, CH_B);

    /* ...until whole budgets link the edges again. */
    g_pc = CH_A;
    (void)emu_jit_run(k_cpu, 64u, &retired, &k_stub_chain_ops);
    CHECK(ch_a_rel32() != (int32_t)(g_ch_a_tail - (g_ch_a_site + 4u)));
}

/* ------------------------------------------------------------------ */
/* A hot set larger than the buffer                                    */
/* ------------------------------------------------------------------ */

/*
 * Eight blocks of 12 KB run in a cycle, 96 KB against the 64 KB buffer:
 * every block is evicted before its turn comes round, whatever compaction
 * keeps. Each sets the pc to the next and retires one; the interpreter
 * stub does the same, so an interpreted turn and a translated one are the
 * same step of the cycle.
 */
#define TH_N 8u
#define TH_BASE 0xA000u
#define TH_PAD 12000u

static uint32_t g_th_built;

static uint32_t th_next(uint32_t pc)
{
    return TH_BASE + ((((pc - TH_BASE) >> 8) + 1u) % TH_N << 8);
}

static uint32_t stub_translate_thrash(emu_cpu_t *cpu, uint32_t pc)
{
    const uint64_t pc_addr = (uint64_t)(uintptr_t)&g_pc;

    (void)cpu;
    if (pc < TH_BASE || pc >= TH_BASE + (TH_N << 8)) {
        return 0u;
    }
    g_th_built++;
    for (uint32_t i = 0; i < TH_PAD; i++) {
        emu_jit_emit8(0x90u); /* nop */
    }
    emu_jit_emit8(0x48u); /* mov rax, &g_pc */
    emu_jit_emit8(0xB8u);
    emu_jit_emit32((uint32_t)pc_addr);
    emu_jit_emit32((uint32_t)(pc_addr >> 32));
    emu_jit_emit8(0xC7u); /* mov dword [rax], next */
    emu_jit_emit8(0x00u);
    emu_jit_emit32(th_next(pc));
    emu_jit_emit8(0xB8u); /* mov eax, 1 */
    emu_jit_emit32(1u);
    emu_jit_emit8(0xC3u); /* ret */
    return 1u;
}

static emu_run_reason_t stub_interp_thrash(emu_cpu_t *cpu, uint32_t budget,
                                           uint32_t *retired)
{
    (void)cpu;
    (void)budget;
    g_pc = th_next(g_pc);
    if (retired != NULL) {
        *retired = 1u;
    }
    return EMU_RUN_BUDGET;
}

static const emu_backend_t k_stub_interp_thrash = {
    .name = "stub-interp-thrash",
    .run = stub_interp_thrash,
};

static const emu_jit_ops_t k_stub_thrash_ops = {
    .name = "stub-thrash",
    .bind = stub_bind,
    .translate = stub_translate_thrash,
    .interp = &k_stub_interp_thrash,
    .relocatable = true,
};

/* Translations made by `turns` steps of the cycle. */
static uint32_t th_run(uint32_t turns)
{
    const uint32_t before = g_th_built;
    uint32_t retired = 0u;

    (void)emu_jit_run(k_cpu, turns, &retired, &k_stub_thrash_ops);
    return g_th_built - before;
}

/*
 * A pc whose block keeps being evicted is given up on, a few at a time,
 * until what is left fits -- and then nothing is translated at all.
 *
 * Off, every turn is a translation, for ever: Whetstone with the F746's
 * tables at 32 KB, 516,732 translations, 20 times slower than the
 * interpreter. On, translation stops while some of the cycle is still
 * translated (the marks went in fours, not all at once), the pcs given
 * up are interpreted, and once the retry count is reached the marks are
 * forgotten and the cycle is tried again.
 */
static void test_thrash_gives_up_on_evicted_pcs(void)
{
    emu_jit_stats_t before;
    emu_jit_stats_t after;

    /* Off: the old behaviour, a translation per turn. */
    emu_jit_flush();
    emu_jit_set_thrash_evictions(0u);
    g_pc = TH_BASE;
    g_th_built = 0u;
    (void)th_run(TH_N * 10u);
    CHECK_EQ(th_run(TH_N * 40u), TH_N * 40u);

    /*
     * On, one turn at a time: no compaction may make more than its
     * share of marks. All at once, the blocks a compaction evicts reach
     * the limit together and are all given up, more than had to be.
     */
    emu_jit_flush();
    emu_jit_set_thrash_evictions(EMU_JIT_THRASH_EVICTIONS);
    g_pc = TH_BASE;
    emu_jit_get_stats(&before);
    {
        emu_jit_stats_t prev = before;
        uint32_t worst = 0u;

        for (uint32_t t = 0u; t < TH_N * 40u; t++) {
            emu_jit_stats_t now;

            (void)th_run(1u);
            emu_jit_get_stats(&now);
            const uint32_t marks = now.thrashed - prev.thrashed;
            const uint32_t compactions = now.compactions - prev.compactions;

            if (marks > EMU_JIT_THRASH_MARKS * compactions + worst) {
                worst = marks - EMU_JIT_THRASH_MARKS * compactions;
            }
            prev = now;
        }
        CHECK_EQ(worst, 0u);
    }
    CHECK_EQ(th_run(TH_N * 40u), 0u); /* settled: nothing translated */
    emu_jit_get_stats(&after);
    CHECK(after.thrashed - before.thrashed >= TH_N - 3u);
    CHECK(after.thrashed - before.thrashed < TH_N); /* not all of it */
    CHECK(after.blocks >= 1u);
    CHECK(after.thrash_cached - before.thrash_cached > 0u);
    CHECK_EQ(after.thrash_retries - before.thrash_retries, 0u);

    /* The retry count reached: the marks go, and it is tried again. */
    emu_jit_get_stats(&before);
    const uint32_t retranslated = th_run(EMU_JIT_THRASH_RETRY * 2u);
    emu_jit_get_stats(&after);
    CHECK(after.thrash_retries - before.thrash_retries >= 1u);
    CHECK(retranslated > 0u);

    emu_jit_set_thrash_evictions(EMU_JIT_THRASH_EVICTIONS);
    emu_jit_flush();
}
#endif

/*
 * emu_jit_step: one instruction through the interpreter, then what the
 * dispatcher does after one -- after_interp, which is where every
 * frontend re-derives what its blocks were built under, and where
 * PowerPC performs the flush an interpreted instruction asked for.
 * Calling the interpreter alone would skip that, and translating would
 * not be one instruction.
 */
static uint32_t g_step_budget;
static uint32_t g_step_after;

static emu_run_reason_t stub_interp_step(emu_cpu_t *cpu, uint32_t budget,
                                         uint32_t *retired)
{
    (void)cpu;
    g_step_budget = budget;
    if (retired != NULL) {
        *retired = budget;
    }
    return EMU_RUN_BUDGET;
}

static void stub_after_step(emu_cpu_t *cpu)
{
    (void)cpu;
    g_step_after++;
}

static const emu_backend_t k_stub_interp_step = {
    .name = "stub-interp-step",
    .run = stub_interp_step,
};

static const emu_jit_ops_t k_stub_step_ops = {
    .name = "stub-step",
    .bind = stub_bind,
    .translate = stub_translate,
    .interp = &k_stub_interp_step,
    .after_interp = stub_after_step,
};

static void test_step_is_interpreted_and_followed(void)
{
    const uint32_t translations = g_translate_calls;

    g_step_budget = 0u;
    g_step_after = 0u;
    CHECK_EQ(emu_jit_step(k_cpu, &k_stub_step_ops), EMU_RUN_BUDGET);
    CHECK_EQ(g_step_budget, 1u);
    CHECK_EQ(g_step_after, 1u);
    CHECK_EQ(g_translate_calls, translations);
}

void test_jit(void)
{
    /*
     * **Without a code buffer emu_jit_run hands the whole budget to the
     * interpreter and returns**, so every assertion below would be about
     * a dispatch loop that never ran. The first version of this file
     * omitted it and all ten checks failed at once -- which is the good
     * outcome, and only because they are equalities: a test asserting
     * that `declined` stayed *small* would have passed against a JIT
     * that was never entered.
     */
    if (!emu_jit_init(64u * 1024u)) {
        CHECK(false); /* no buffer, so nothing below means anything */
        return;
    }

    test_declined_pc_is_not_retried();
    test_hot_threshold_delays_translation();
    test_flush_reopens_a_declined_pc();
    test_generation_change_reopens_a_declined_pc();
    test_cache_is_per_pc();
    test_declines_are_per_context();
#if defined(EMU_HOST_JIT_X86_64)
    test_blocks_are_per_context();
    test_invalidate_range_retires_only_overlap();
    test_invalidate_range_reopens_only_its_declines();
    test_overflow_compaction_unlinks();
    test_chain_heat_keeps_chained_blocks();
    test_unchain_runs_one_block();
    test_thrash_gives_up_on_evicted_pcs();
#endif
    test_step_is_interpreted_and_followed();

    /* Leave nothing behind for whatever runs next. */
    emu_jit_flush();
}
