/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir_jit.c - The IR-driven JIT, for any frontend on any host.
 *
 * One JIT per host, which is what the IR was built to make possible.
 * Before it there was one translator per frontend/host *pair* -- three
 * of them here and in the Thumb-2 directory, each carrying its own copy
 * of the same pipeline and the same block-cache adapters. Three
 * frontends and three hosts is nine such files, and every fix to the
 * shared parts has to be made nine times or not at all.
 *
 * What is left is the pipeline:
 *
 *     frontend->translate() -> emu_ir_optimise() -> emu_ir_lower()
 *
 * and the adapters onto emu_backend_t. Nothing here names a guest.
 * Everything a host cannot know arrives through emu_ir_frontend_t.
 *
 * The backends are instantiated by a macro rather than being one runtime
 * object because both frontends can be compiled in at once and each
 * needs its own emu_backend_t symbol for the frontend registry to point
 * at. The macro is the C way of saying "the same code, twice"; the
 * alternative is a global holding the current frontend, which would be
 * a mutable dependency between two things that are otherwise unrelated.
 */

#include "emu/emu_ir.h"

#include <string.h>

#if defined(EMU_JIT_X86_64)

/*
 * One block of IR, reused across translations.
 *
 * Static rather than automatic because it is large -- a block's worth of
 * instructions and temporaries -- and translation is not reentrant: the
 * dispatch loop calls translate for one block at a time, and a helper
 * cannot re-enter it because helpers run inside an *already translated*
 * block.
 */
/* The host this was built for, so the banner does not have to lie. */
#if defined(EMU_JIT_THUMB2)
#  define EMU_IR_JIT_NAME "jit-ir-thumb2"
#else
#  define EMU_IR_JIT_NAME "jit-ir-x86-64"
#endif

static emu_ir_block_t g_ir;

/*
 * A code buffer for hosts that cannot map one.
 *
 * emu_jit_init allocates only where there is an mmap to allocate with;
 * on a microcontroller the platform supplies the memory, and if nobody
 * does, the framework runs everything on the interpreter and says so
 * only through the backend name. That is exactly what happened the first
 * time this file replaced the hand-written Thumb-2 translator: the board
 * printed "backend interp" and every test still passed.
 *
 * The size is the guest's RAM, so it is the microcontroller figure and
 * not the host one. RV32_JIT_CODE_BYTES dominates JIT performance on
 * this target -- see CLAUDE.md -- and the default is deliberately small.
 */
#if !defined(EMU_JIT_MMAP) || !EMU_JIT_MMAP
#  ifndef EMU_IR_JIT_STATIC_BYTES
#    define EMU_IR_JIT_STATIC_BYTES 12288u
#  endif
static uint8_t g_static_code[EMU_IR_JIT_STATIC_BYTES]
    __attribute__((aligned(8)));
#endif

/*
 * Offer the static buffer, where there is one. False means this host can
 * neither map memory nor was given any, and the framework will run the
 * interpreter -- which is correct, and slow, and worth noticing.
 */
static bool emu_ir_jit_static_buffer(void)
{
#if !defined(EMU_JIT_MMAP) || !EMU_JIT_MMAP
    emu_jit_set_buffer(g_static_code, (uint32_t)sizeof(g_static_code));
    return emu_jit_init((uint32_t)sizeof(g_static_code));
#else
    return false;
#endif
}

/*
 * Translate at `pc` for `fe`.
 *
 * live_out is EMU_IR_F_ALL. The block ends where the frontend stopped
 * lowering, and whatever runs next -- the interpreter, a trap handler,
 * the next block -- may read any flag, so none are dead at the boundary.
 * Narrowing it would need the frontend to prove what follows reads, and
 * being wrong deletes a flag definition something depends on. A guest
 * with no flags pays nothing for this either way: it emits no
 * EMU_IR_SETF, so the pass finds nothing.
 */
static uint32_t ir_translate(emu_cpu_t *cpu, uint32_t pc,
                             const emu_ir_frontend_t *fe)
{
    const uint32_t n = fe->translate(cpu, pc, &g_ir);

    if (n == 0u) {
        return 0u;
    }

    emu_ir_optimise(&g_ir, EMU_IR_F_ALL, NULL);

    if (!emu_ir_lower(&g_ir, fe->target)) {
        return 0u;
    }
    return n;
}

/*
 * Instantiate a backend for one frontend.
 *
 * `sym` is the emu_backend_t the frontend registry names; `fe` is the
 * emu_ir_frontend_t it is built from.
 */
#define EMU_IR_DEFINE_X86_BACKEND(sym, fe, prefix)                      \
                                                                        \
static uint32_t prefix##_translate(emu_cpu_t *cpu, uint32_t pc)         \
{                                                                       \
    return ir_translate(cpu, pc, &(fe));                                \
}                                                                       \
                                                                        \
static void prefix##_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)           \
{                                                                       \
    (fe).bind(cpu, out);                                                \
}                                                                       \
                                                                        \
static bool prefix##_is_idle(emu_cpu_t *cpu)                            \
{                                                                       \
    return (fe).is_idle != NULL && (fe).is_idle(cpu);                   \
}                                                                       \
                                                                        \
static bool prefix##_wake(emu_cpu_t *cpu)                               \
{                                                                       \
    return (fe).wake != NULL && (fe).wake(cpu);                         \
}                                                                       \
                                                                        \
static bool prefix##_take_irq(emu_cpu_t *cpu)                           \
{                                                                       \
    return (fe).take_irq != NULL && (fe).take_irq(cpu);                 \
}                                                                       \
                                                                        \
static void prefix##_count(emu_cpu_t *cpu, uint32_t n)                  \
{                                                                       \
    if ((fe).count != NULL) {                                           \
        (fe).count(cpu, n);                                             \
    }                                                                   \
}                                                                       \
                                                                        \
static const emu_jit_ops_t prefix##_jit_ops = {                         \
    .name        = EMU_IR_JIT_NAME,                                     \
    .bind        = prefix##_bind,                                       \
    .translate   = prefix##_translate,                                  \
    /*                                                                  \
     * Relocatable: every branch inside a block is a rel32 whose ends    \
     * move together, and every address that is not -- helpers, guest    \
     * pc -- is an absolute immediate. x86 needs no sync, its caches     \
     * being coherent with instruction fetch.                           \
     */                                                                 \
    .relocatable = true,                                                \
    .sync        = NULL,                                                \
    .interp      = NULL,   /* filled below; see the note there */       \
    .is_idle     = prefix##_is_idle,                                    \
    .wake        = prefix##_wake,                                       \
    .take_irq    = prefix##_take_irq,                                   \
    .count       = prefix##_count,                                      \
};                                                                      \
                                                                        \
static emu_jit_ops_t prefix##_ops_live;                                 \
                                                                        \
static bool prefix##_init(emu_cpu_t *cpu)                               \
{                                                                       \
    (void)cpu;                                                          \
    /*                                                                  \
     * `interp` is a pointer to another translation unit's const object \
     * and so is not a constant expression for a static initialiser.    \
     * Bound here, once, rather than left NULL -- the dispatch loop      \
     * calls it for every declined instruction, which is the common      \
     * case rather than an edge one.                                    \
     */                                                                 \
    prefix##_ops_live = prefix##_jit_ops;                               \
    prefix##_ops_live.interp = (fe).interp;                             \
    if (emu_jit_init((fe).code_bytes)) {                                \
        return true;                                                    \
    }                                                                   \
    return emu_ir_jit_static_buffer();                                  \
}                                                                       \
                                                                        \
static void prefix##_reset(emu_cpu_t *cpu)                              \
{                                                                       \
    (void)cpu;                                                          \
    emu_jit_flush();                                                    \
}                                                                       \
                                                                        \
static void prefix##_invalidate(emu_cpu_t *cpu, uint32_t a, uint32_t l) \
{                                                                       \
    (void)cpu;                                                          \
    (void)a;                                                            \
    (void)l;                                                            \
    /* Whole-cache flush: translations are cheap to rebuild, and         \
     * tracking which blocks covered a range costs more than it saves. */\
    emu_jit_flush();                                                    \
}                                                                       \
                                                                        \
static emu_run_reason_t prefix##_run(emu_cpu_t *cpu, uint32_t budget,   \
                                     uint32_t *retired)                 \
{                                                                       \
    return emu_jit_run(cpu, budget, retired, &prefix##_ops_live);       \
}                                                                       \
                                                                        \
const emu_backend_t sym = {                                             \
    .name       = EMU_IR_JIT_NAME,                                      \
    .init       = prefix##_init,                                        \
    .reset      = prefix##_reset,                                       \
    .run        = prefix##_run,                                         \
    .invalidate = prefix##_invalidate,                                  \
}

#if EMU_FRONTEND_RV32
#include "rv32/rv_ir.h"
#include "rv32/rv_jit.h"
EMU_IR_DEFINE_X86_BACKEND(rv_backend_jit, rv_ir_frontend, rv);

/*
 * The statistics the host runner prints. Mostly the framework's, plus
 * the two that say *why* a translation produced nothing -- read those
 * before believing a passing test, because a backend that declines
 * everything and falls back passes every suite while proving nothing.
 */
void rv_jit_get_stats(rv_jit_stats_t *out)
{
    emu_jit_stats_t st;

    emu_jit_get_stats(&st);
    memset(out, 0, sizeof(*out));
    out->blocks = st.blocks;
    out->translations = st.translations;
    out->block_entries = st.block_entries;
    out->interp_fallbacks = st.interp_fallbacks;
    out->flushes = st.flushes;
    out->compactions = st.compactions;
    out->evictions = st.evictions;
    out->declined = st.declined;
    out->overflowed = st.overflowed;
    out->code_used = st.code_used;
    out->code_size = st.code_size;
}

void rv_jit_flush(void)
{
    emu_jit_flush();
}
#endif /* EMU_FRONTEND_RV32 */

#if EMU_FRONTEND_G4MH
#include "g4mh/g4mh_ir.h"
EMU_IR_DEFINE_X86_BACKEND(g4mh_backend_jit, g4mh_ir_frontend, g4mh);
#endif

#endif /* EMU_JIT_X86_64 */
