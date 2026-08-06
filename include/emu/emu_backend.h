/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_backend.h - Execution engine abstraction.
 *
 * A frontend supplies the semantics of an instruction set; a backend
 * supplies a way of executing them. The two are independent, so each
 * frontend may ship several backends and pick between them at build time
 * or at run time:
 *
 *   rv32   a threaded interpreter, and a Thumb-2 JIT
 *   g4mh   a threaded interpreter
 *
 *   - `run` is budgeted rather than free-running, so a JIT can execute a
 *     whole translated block and report how many instructions it retired.
 *   - `invalidate` tells a JIT that guest memory changed underneath it
 *     (self-modifying code, a fresh image load, a debugger write). An
 *     interpreter ignores it.
 *   - `reset` lets a backend drop translated state on core reset.
 *
 * A JIT backend additionally needs cache maintenance on the ARM side
 * (DSB/ISB plus D-cache clean + I-cache invalidate on M7); that belongs in
 * the backend, not here.
 *
 * The `emu_cpu_t *` these take is the frontend's own core state, opaque to
 * this header -- a backend belongs to exactly one frontend and casts it
 * back on entry. That cast is paid once per budget (4096 instructions by
 * default), not per instruction, so the indirection here costs nothing
 * measurable. Nothing in this file may end up on a per-instruction path.
 */
#ifndef EMU_BACKEND_H
#define EMU_BACKEND_H

#include "emu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct emu_cpu;

typedef struct emu_backend {
    const char *name;

    /* Optional one-time setup. Returns false on failure. May be NULL. */
    bool (*init)(struct emu_cpu *cpu);

    /* Drop any cached translation state. May be NULL. */
    void (*reset)(struct emu_cpu *cpu);

    /*
     * Execute at most `budget` instructions. Returns the reason for
     * stopping and stores the number of instructions actually retired
     * through *retired (may be NULL).
     */
    emu_run_reason_t (*run)(struct emu_cpu *cpu, uint32_t budget,
                            uint32_t *retired);

    /* Guest memory [addr, addr+len) changed. May be NULL. */
    void (*invalidate)(struct emu_cpu *cpu, uint32_t addr, uint32_t len);
} emu_backend_t;

#ifdef __cplusplus
}
#endif

#endif /* EMU_BACKEND_H */
