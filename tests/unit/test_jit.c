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

#include "tests.h"

#include "emu/emu_backend.h"
#include "emu/emu_jit.h"

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
#endif

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
    test_flush_reopens_a_declined_pc();
    test_generation_change_reopens_a_declined_pc();
    test_cache_is_per_pc();
    test_declines_are_per_context();
#if defined(EMU_HOST_JIT_X86_64)
    test_blocks_are_per_context();
#endif

    /* Leave nothing behind for whatever runs next. */
    emu_jit_flush();
}
