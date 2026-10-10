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
     * The embedded floating-point unit's status and control. EFPU2
     * keeps its operands in the GPRs, so this is the whole of its state.
     */
    uint32_t spefscr;

    /*
     * The reservation, as HID1[ATS] describes it: a flag and no
     * address. The e200 compares no address on a store conditional
     * (manual 3.5), so neither does this.
     */
    bool reserve;

    /*
     * Which instruction encoding this core is decoding.
     *
     * VLE and classic Book E are *different encodings of the same
     * bytes*, not a superset and a subset: 0x48000009 is `bl` in Book E
     * and a 16-bit se_ form followed by something else in VLE. A real
     * e200 chooses per page, from the VLE attribute of the TLB entry;
     * this model has no MMU, so the frontend chooses once per image from
     * the ELF's PF_PPC_VLE segment flag, which is the same attribute as
     * the linker records it. A raw binary is VLE, which is what this
     * core runs out of reset (the p_rst_vlemode input).
     *
     * It was once false by default and written only by a unit test, so
     * the whole 16-bit half of the interpreter was unreachable by any
     * real guest -- a capability nobody can exercise is not one.
     */
    bool vle;

    /*
     * Special purpose registers, each where it can be named.
     *
     * Read and written through ppc_spr_read/ppc_spr_write, which apply
     * the manual's rules for numbers that do not exist: an illegal
     * instruction, or a privileged one from user mode when the number's
     * privilege bit is set (section 3.15). "Reads zero" is how a guest
     * silently mis-detects its own core, so nothing reads zero by
     * default.
     */
    uint32_t sprg[10];
    uint32_t usprg0;
    uint32_t ivor[PPC_IVOR_COUNT];
    uint32_t ivpr;
    uint32_t srr0, srr1;
    uint32_t csrr0, csrr1;
    uint32_t dsrr0, dsrr1;
    uint32_t mcsrr0, mcsrr1;
    uint32_t dear, esr;
    uint32_t mcsr, mcar;
    uint32_t tsr, tcr;
    uint32_t pir, pvr, svr;
    uint32_t pid0;
    uint32_t hid0, hid1;
    uint32_t l1csr0, l1csr1, l1finv0, l1finv1, bucsr;
    /*
     * The debug facility's registers. They hold what is written and
     * nothing acts on them: the debug architecture is not modelled, and
     * a guest that writes DBCR0 to make sure it is off should not trap
     * for it.
     */
    uint32_t dbcr[7], dbsr, iac[8], dac[2], dvc[2], dbcnt, ddam, devent;

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
     * What the two count, which HID0 chooses (table 2-7): nothing until
     * TBEN is set, then either the processor clock or the p_tbclk
     * input.
     *
     * The processor clock is one tick per instruction here, exactly:
     * `cycles` is the count and `clk_synced` how much of it the timers
     * have been given. Kept lazily -- handed over when something looks
     * at a timer or when the run loop reaches the instruction the
     * decrementer expires on -- so counting costs the interpreter
     * nothing per instruction and a decrementer interrupt still lands
     * on the exact instruction, the same one every run.
     *
     * p_tbclk is the platform's time, arriving through set_time;
     * `tbclk_last` is where it had got to.
     */
    uint64_t clk_synced;
    uint64_t tbclk_last;

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
    bool irq_dirty;

    /*
     * The translator's view. `jit_ctx` is what a block is built for,
     * which is the encoding it was decoded as and nothing else: privilege
     * and MSR[SPE] decide nothing in lowered code, because everything
     * that depends on either is left to the interpreter, and with no
     * MMU there is no permission a block could have been checked
     * against. `jit_gen` moves when something a block has specialised
     * on changes; nothing does, so it never moves.
     */
    uint64_t jit_ctx;
    uint32_t jit_gen;
    bool jit_flush; /* icbi asked for the translations to go */

    emu_syscall_fn syscall;
    void *syscall_user;
#if EMU_ENABLE_TRACE
    emu_trace_fn trace;
    void *trace_user;
#endif
} ppc_cpu_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ppc_cpu_init(ppc_cpu_t *c, struct emu_bus *bus, uint32_t coreid);
void ppc_cpu_reset(ppc_cpu_t *c, uint32_t reset_pc);

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

/* ESR left as it is: the asynchronous interrupts do not describe
 * themselves there. */
#define PPC_ESR_KEEP 0xFFFFFFFFu

/*
 * Take a Book E interrupt.
 *
 * The handler address is IVPR[0:15] || IVORn[16:27] || 0b0000 -- the
 * vector lives *in a register*, not at a fixed offset from a base. A
 * guest that has not written IVPR and the IVORs vectors to address
 * zero rather than to something recognisable.
 *
 * `ret_pc` is what the save/restore register receives: the faulting
 * instruction for most, the next one for a system call, a round
 * exception and anything asynchronous. `esr` is the syndrome to write,
 * or PPC_ESR_KEEP; ESR[VLEMI] is added here for a synchronous interrupt
 * taken while decoding VLE, as the manual lists it for each of them.
 */
void ppc_cpu_raise(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc,
                   uint32_t esr);

/* The older spelling, for an interrupt with no syndrome. */
void ppc_cpu_exception(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc);

/* What a block is built for: which of the two encodings it decoded. */
uint64_t ppc_cpu_ctx(const ppc_cpu_t *c);

/*
 * Leave the wait state if an enabled interrupt is pending, carrying a
 * core that is waiting on its own decrementer forward to the expiry.
 * True if the core is running again.
 */
bool ppc_cpu_wake(ppc_cpu_t *c);

/* MSR written by software: keep the implemented bits, refresh what
 * depends on them. */
void ppc_cpu_set_msr(ppc_cpu_t *c, uint32_t v);

/* ------------------------------------------------------------------ */
/* Special purpose registers                                           */
/* ------------------------------------------------------------------ */

/*
 * mfspr and mtspr. Return PPC_EXC_NONE, or the ESR bit of the program
 * interrupt the access raises: PIL for a number that does not exist or
 * a write to a read-only one, PPR for a privileged number from user
 * mode -- which is decided by the number alone, implemented or not.
 */
uint32_t ppc_spr_read(ppc_cpu_t *c, uint32_t spr, uint32_t *out);
uint32_t ppc_spr_write(ppc_cpu_t *c, uint32_t spr, uint32_t v);

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

/* The p_tbclk input: the platform's time. Counted only while HID0 has
 * the time base enabled and selects it. */
void ppc_cpu_set_time(ppc_cpu_t *c, uint64_t now);

/*
 * The processor clock: give the timers the instructions executed since
 * they were last given any. Called before anything reads or writes a
 * timer, and by the run loop.
 */
void ppc_cpu_sync_clock(ppc_cpu_t *c);

/* Instructions until the decrementer expires on the processor clock,
 * or UINT32_MAX when that is not what will happen next. */
uint32_t ppc_cpu_clock_until(const ppc_cpu_t *c);

/* The external input, level. */
void ppc_cpu_set_ext(ppc_cpu_t *c, bool level);

/*
 * The interrupt to take, or -1. Honours MSR[EE], which gates both
 * sources -- Book E has no per-source mask below the enable bit, so a
 * guest running with EE clear takes neither.
 */
int ppc_cpu_pending_irq(const ppc_cpu_t *c);

/* Take a pending interrupt, if there is one; true if one was taken. */
bool ppc_cpu_take_irq(ppc_cpu_t *c);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * Data accesses. Unaligned ones are performed, as the e200 does in
 * hardware (manual 3.4) -- only the reservation, multiple and context
 * save instructions require alignment, and they check it themselves.
 * An access the bus refuses is a data storage interrupt; these set
 * DEAR and return the IVOR, and the caller raises it with ESR.
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
/* Execution                                                           */
/* ------------------------------------------------------------------ */

struct ppc_insn;

/*
 * Execute one decoded instruction at `pc`. True if it completed, with
 * c->pc where execution continues; false if it raised an interrupt
 * instead, with c->pc at the handler. The interpreter's whole semantics,
 * and the translator's fallback for what it does not lower -- one copy,
 * so the two cannot disagree.
 */
bool ppc_exec(ppc_cpu_t *c, const struct ppc_insn *d, uint32_t pc);

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

extern const emu_backend_t ppc_backend_interp;
#if EMU_HAVE_JIT
extern const emu_backend_t ppc_backend_jit;
#endif
extern const emu_backend_t *ppc_backend;

emu_run_reason_t ppc_step(ppc_cpu_t *c);

#ifdef __cplusplus
}
#endif

#endif /* PPC_CPU_H */
