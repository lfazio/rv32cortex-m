/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_backend.h - Execution engine abstraction.
 *
 * The core (state, bus, CSRs, traps) knows nothing about how instructions
 * get executed. Today there is one backend, a threaded interpreter. The
 * interface exists so a Thumb-2 JIT can be added later as a drop-in
 * alternative without touching the core:
 *
 *   - `run` is budgeted rather than free-running, so a JIT can execute a
 *     whole translated block and report how many instructions it retired.
 *   - `invalidate` tells a JIT that guest memory changed underneath it
 *     (self-modifying code, a fresh image load, a debugger write). The
 *     interpreter ignores it.
 *   - `reset` lets a backend drop translated state on hart reset.
 *
 * A JIT backend would additionally need cache maintenance on the ARM side
 * (DSB/ISB plus D-cache clean + I-cache invalidate on M7); that belongs in
 * the backend, not here.
 */
#ifndef RV32_RV_BACKEND_H
#define RV32_RV_BACKEND_H

#include "rv_types.h"
#include "rv_hart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rv_backend {
    const char *name;

    /* Optional one-time setup. Returns false on failure. May be NULL. */
    bool (*init)(rv_hart_t *h);

    /* Drop any cached translation state. May be NULL. */
    void (*reset)(rv_hart_t *h);

    /*
     * Execute at most `budget` instructions. Returns the reason for
     * stopping and stores the number of instructions actually retired
     * through *retired (may be NULL).
     */
    rv_run_reason_t (*run)(rv_hart_t *h, uint32_t budget, uint32_t *retired);

    /* Guest memory [addr, addr+len) changed. May be NULL. */
    void (*invalidate)(rv_hart_t *h, uint32_t addr, uint32_t len);
} rv_backend_t;

/* The threaded interpreter. Always available. */
extern const rv_backend_t rv_backend_interp;

/*
 * The backend the platform selected. Defined once by the platform so the
 * rest of the firmware can call rv_run() without caring which it is.
 */
extern const rv_backend_t *rv_backend;

/* Convenience wrappers around the active backend. */
static inline rv_run_reason_t rv_run(rv_hart_t *h, uint32_t budget,
                                     uint32_t *retired)
{
    return rv_backend->run(h, budget, retired);
}

static inline void rv_invalidate(rv_hart_t *h, uint32_t addr, uint32_t len)
{
    if (rv_backend->invalidate) {
        rv_backend->invalidate(h, addr, len);
    }
}

/* Execute exactly one instruction. Used by tests and the debug monitor. */
rv_run_reason_t rv_step(rv_hart_t *h);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_BACKEND_H */
