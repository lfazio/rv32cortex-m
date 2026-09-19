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

    /* Leave nothing behind for whatever runs next. */
    emu_jit_flush();
}
