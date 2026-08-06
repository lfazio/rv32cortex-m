/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_backend.h - The RISC-V frontend's execution engines.
 *
 * Two implementations of emu_backend_t, both executing RV32 for the same
 * rv_hart_t: a threaded interpreter that is always available, and a
 * Thumb-2 JIT that is built only on an ARM host. The JIT is a fast path
 * over the interpreter rather than a replacement -- it declines whatever
 * it cannot translate and the interpreter runs it -- so correctness never
 * depends on translation coverage.
 *
 * The generic interface is in emu/emu_backend.h; it takes an emu_cpu_t
 * because a platform can hold any frontend's core. Both backends here cast
 * it straight back to rv_hart_t on entry, once per budget.
 */
#ifndef RV32_RV_BACKEND_H
#define RV32_RV_BACKEND_H

#include "emu/emu_backend.h"

#include "rv_types.h"
#include "rv_hart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The threaded interpreter. Always available. */
extern const emu_backend_t rv_backend_interp;

/*
 * The backend the platform selected. Defined once by the frontend so the
 * rest of the code can call rv_run() without caring which it is.
 */
extern const emu_backend_t *rv_backend;

/* Convenience wrappers around the active backend. */
static inline emu_run_reason_t rv_run(rv_hart_t *h, uint32_t budget,
                                      uint32_t *retired)
{
    return rv_backend->run((emu_cpu_t *)h, budget, retired);
}

static inline void rv_invalidate(rv_hart_t *h, uint32_t addr, uint32_t len)
{
    if (rv_backend->invalidate) {
        rv_backend->invalidate((emu_cpu_t *)h, addr, len);
    }
}

/* Execute exactly one instruction. Used by tests and the debug monitor. */
emu_run_reason_t rv_step(rv_hart_t *h);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_BACKEND_H */
