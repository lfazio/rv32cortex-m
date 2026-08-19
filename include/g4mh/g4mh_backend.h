/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_backend.h - The G4MH frontend's execution engines.
 *
 * The counterpart of rv32/rv_backend.h, and deliberately the same shape:
 * two implementations of emu_backend_t executing G4MH for the same
 * g4mh_cpu_t -- a threaded interpreter that is always available, and an
 * IR JIT built only where the host has an emitter. The JIT is a fast path
 * over the interpreter rather than a replacement: it declines what it
 * cannot translate and the interpreter runs that, so correctness never
 * depends on translation coverage.
 *
 * This existed only as a handful of declarations inside g4mh_cpu.h, which
 * is the file about *architectural state*. The asymmetry was the kind
 * this project has been bitten by before -- the two frontends are meant
 * to be the same file with different contents, and a thing that is a
 * header on one side and a paragraph on the other is where they drift.
 *
 * The generic interface is emu/emu_backend.h; it takes an emu_cpu_t
 * because a platform may hold any frontend's core, and both backends here
 * cast it straight back once per budget.
 */
#ifndef G4MH_G4MH_BACKEND_H
#define G4MH_G4MH_BACKEND_H

#include "emu/emu_backend.h"
#include "emu/emu_jit.h"

#include "g4mh_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The threaded interpreter. Always available. */
extern const emu_backend_t g4mh_backend_interp;

#if EMU_HAVE_JIT
/*
 * The IR JIT. Preferred by g4mh_frontend.c, which is right for firmware:
 * there it is a speed choice and the backend falls back per instruction.
 *
 * On a host it is a *coverage* choice and the caller must be able to say
 * which it wants. That matters more here than for RV32, which has three
 * reference models to disagree with: G4MH has none, so the interpreter is
 * the only statement of what an answer should be, and a run that silently
 * translated cannot be diffed against one that did not.
 */
extern const emu_backend_t g4mh_backend_jit;
#endif

/*
 * The backend the platform selected. Defined once by the frontend so the
 * rest of the code can call g4mh_run() without caring which it is.
 */
extern const emu_backend_t *g4mh_backend;

/* Convenience wrappers around the active backend. */
static inline emu_run_reason_t g4mh_run(g4mh_cpu_t *c, uint32_t budget,
                                        uint32_t *retired)
{
    return g4mh_backend->run((emu_cpu_t *)c, budget, retired);
}

/*
 * Discard translations covering [addr, addr+len).
 *
 * This is what SYNCI and the CACHE instruction mean to a translating
 * backend, and the *only* thing they mean to this model: there is no
 * cache here to invalidate, but there is a cache of translated blocks,
 * and a guest that writes instructions and then synchronises is saying
 * that cache is stale. Without it the JIT keeps running the code the
 * guest replaced -- proven rather than assumed by
 * test_synci_discards_translations, which the interpreter passes and the
 * JIT failed.
 *
 * The RV32 side has drawn the same line since it was written: FENCE is a
 * no-op here, because a single-threaded execution model already gives the
 * guest a total order over its own accesses, while FENCE.I is not,
 * because it is about code.
 */
static inline void g4mh_invalidate(g4mh_cpu_t *c, uint32_t addr, uint32_t len)
{
    if (g4mh_backend->invalidate != NULL) {
        g4mh_backend->invalidate((emu_cpu_t *)c, addr, len);
    }
}

/* Execute exactly one instruction. Used by tests and the debug monitor. */
emu_run_reason_t g4mh_step(g4mh_cpu_t *c);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_BACKEND_H */
