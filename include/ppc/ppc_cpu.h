/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_cpu.h - e200z7 core state.
 *
 * Shaped like g4mh_cpu.h and rv_hart.h: the hot state first, the cpu
 * pointer handed out by the frontend *is* this structure, and nothing
 * here is on a per-instruction path that the contract forbids.
 */
#ifndef PPC_CPU_H
#define PPC_CPU_H

#include "emu/emu_backend.h"
#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_jit.h"

#include "ppc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ppc_cpu {
    /* --- hot state: keep first --- */
    uint32_t r[PPC_NGPR];
    uint32_t pc;

    struct emu_bus *bus;

    /*
     * Condition register, held as one 32-bit word rather than eight
     * nibbles. Reads of a single field are a shift and a mask either way,
     * and mfcr/mtcr want the whole thing -- splitting it would make the
     * common case cheap and the architectural case a reassembly.
     */
    uint32_t cr;
    uint32_t xer;
    uint32_t lr;
    uint32_t ctr;
    uint32_t msr;

    /*
     * Special purpose registers, sparse.
     *
     * The architectural space is 1024 and a flat array of it is 4 KiB --
     * more than a part with 320 KiB of SRAM should spend on registers
     * that are overwhelmingly unimplemented. mfspr/mtspr map through
     * ppc_spr_slot(), and an unmapped number raises a program interrupt
     * rather than reading zero, because "reads zero" is how a guest
     * silently mis-detects its own core.
     */
    uint32_t sprg[8];
    uint32_t ivor[PPC_IVOR_COUNT];
    uint32_t ivpr;
    uint32_t srr0, srr1;
    uint32_t csrr0, csrr1;
    uint32_t dear, esr;
    uint32_t tsr, tcr;
    uint32_t pir, pvr;

    /* --- counters --- */
    uint64_t cycles;
    uint64_t retired;

    /* --- run control --- */
    emu_state_t state;
    bool        irq_dirty;

    /*
     * Which instruction encoding this core is decoding.
     *
     * VLE and classic Book E are *different encodings of the same
     * bytes*, not a superset and a subset: 0x48000009 is `bl` in Book E
     * and a 16-bit se_ form followed by something else in VLE. A real
     * e200 chooses per page, from the VLE attribute in the TLB entry,
     * so this will become a property of the translation once there is a
     * TLB. Until then it is a core-wide mode.
     *
     * False -- classic Book E, fixed 32-bit -- is the default because it
     * is what this interpreter actually decodes and what
     * powerpc-linux-gnu-as emits without -mvle.
     */
    bool        vle;

    emu_syscall_fn syscall;
    void          *syscall_user;
#if EMU_ENABLE_TRACE
    emu_trace_fn trace;
    void        *trace_user;
#endif
} ppc_cpu_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ppc_cpu_init(ppc_cpu_t *c, struct emu_bus *bus, uint32_t coreid);
void ppc_cpu_reset(ppc_cpu_t *c, uint32_t reset_pc);

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

/*
 * Take a Book E interrupt.
 *
 * The handler address is IVPR[0:15] || IVORn[16:27] || 0b0000 -- the
 * vector lives *in a register*, not at a fixed offset from a base. That
 * is the main structural difference from RISC-V's mtvec and from G4MH's
 * RBASE table, and it means a guest that has not written IVPR and the
 * IVORs vectors to address zero rather than to something recognisable.
 */
void ppc_cpu_exception(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * Alignment is checked here so a misaligned access can be reported as an
 * alignment interrupt, and so the bus can assume aligned accesses on its
 * fast paths -- the same split the other two frontends use.
 *
 * Byte order is *not* handled here. It belongs to the bus, which is the
 * only place that knows whether an access composes bytes (RAM) or takes
 * a value (MMIO, passthrough). A frontend that swapped on top of that
 * would double-swap RAM and corrupt every peripheral register.
 */
ppc_exc_t ppc_load(ppc_cpu_t *c, uint32_t addr, uint32_t size, bool sext,
                   uint32_t *out);
ppc_exc_t ppc_store(ppc_cpu_t *c, uint32_t addr, uint32_t size, uint32_t val);

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

extern const emu_backend_t ppc_backend_interp;
extern const emu_backend_t *ppc_backend;

emu_run_reason_t ppc_step(ppc_cpu_t *c);

#ifdef __cplusplus
}
#endif

#endif /* PPC_CPU_H */
