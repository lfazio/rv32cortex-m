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

    /*
     * The time base and the decrementer.
     *
     * `tb` is volatile because on a target the platform advances it from
     * an ARM interrupt handler while the run loop is executing -- the
     * same reason the G4MH INTC's counter is.
     *
     * The decrementer is a *separate* counter and not a view of the time
     * base: it counts down at the same rate but is reloaded, written and
     * stopped independently, and modelling it as `some_base - tb` breaks
     * the moment a guest writes DEC.
     */
    volatile uint64_t tb;
    uint32_t dec;
    uint32_t decar;

    /*
     * The external interrupt input, level. Set by the platform through
     * set_irq and cleared by the guest's interrupt controller -- there
     * is none here, so it is cleared when the interrupt is taken, which
     * is what a single edge-triggered source looks like.
     */
    volatile bool ext_pending;

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
     * **True is the default, and it has to be.** It was false, on the
     * reasoning that Book E is what powerpc-linux-gnu-as emits without
     * -mvle -- but nothing in the tree ever set it except
     * tests/unit/test_ppc.c reaching into this struct, so the entire
     * 16-bit half of the interpreter was unreachable by any real guest.
     * Every se_ test passed and none of them could have run outside a
     * unit test. That is the shape this project already has written
     * down: a capability nobody can exercise is not a capability.
     *
     * VLE is also simply what this core runs. The e200z7 in an MPC57xx
     * executes VLE, the scope note in ppc_types.h says "Book E *with*
     * VLE", and every guest here is built -mvle.
     *
     * A real e200 chooses per page from the TLB entry's VLE attribute,
     * so this becomes a property of the translation once there is a TLB.
     * Until then it is core-wide, and core-wide *on*.
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
/* Time base, decrementer and the interrupts they raise                */
/* ------------------------------------------------------------------ */

/*
 * Advance guest time by `ticks`.
 *
 * The time base counts up and the decrementer counts down, both at the
 * same rate, and DEC's 1 -> 0 transition is what sets TSR[DIS]. "The
 * transition" and not "the value" is the whole rule: a decrementer
 * already at zero does not keep raising, and one stepped *past* zero by
 * a long slice raises exactly once. Guest time arrives here in chunks
 * of a run budget, so both of those are the ordinary case rather than
 * corners.
 */
void ppc_cpu_advance(ppc_cpu_t *c, uint32_t ticks);
void ppc_cpu_set_time(ppc_cpu_t *c, uint64_t now);

/* The external input, level. */
void ppc_cpu_set_ext(ppc_cpu_t *c, bool level);

/*
 * The interrupt to take, or -1. Honours MSR[EE], which gates both
 * sources -- Book E has no per-source mask below the enable bit, so a
 * guest running with EE clear takes neither.
 */
int ppc_cpu_pending_irq(const ppc_cpu_t *c);

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
