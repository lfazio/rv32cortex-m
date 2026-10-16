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
     * (manual 3.5), so neither does this -- with one core. With two, the
     * manual leaves cross-core reservations to "external reservation
     * logic" on the bus, and this model is that logic: resv_pa below.
     */
    bool reserve;
    /*
     * Another core has been started (ppc_cpu_start), so a store has to
     * break that core's reservation on the word it writes. The JIT's
     * inlined stores test this byte and take the helper while it is set;
     * ppc_store tests slow_store, which is this or mmu_on, so the store
     * path still has one test. Neither goes back to zero until a reset.
     */
    uint8_t smp_live;
    bool slow_store;

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
     * The translator's view. `jit_ctx` is what a block is built for: the
     * encoding it was decoded as, and under a programmed TLB the address
     * space, privilege and PID its code was fetched under (ppc_cpu_ctx).
     * A TLB change retires the blocks whose code the entry mapped, or
     * moves ppc_jit_gen when it touches a page a block uses as its RAM
     * window (ppc_mmu.c).
     */
    uint64_t jit_ctx;
    bool jit_flush; /* icbi asked for the translations to go */

    emu_syscall_fn syscall;
    void *syscall_user;
#if EMU_ENABLE_TRACE
    emu_trace_fn trace;
    void *trace_user;
#endif

    /*
     * The MMU (ppc_mmu.c), at the end because almost no guest touches
     * it: an access tests `mmu_on` and nothing else unless a guest has
     * programmed the TLB.
     *
     * Entries are kept as the MAS1/MAS2/MAS3 words tlbwe wrote, masked
     * to what is implemented, so tlbre gives back exactly what is there.
     */
    struct {
        uint32_t mas1, mas2, mas3;
    } tlb[PPC_TLB_ENTRIES];
    uint32_t mas0, mas1, mas2, mas3, mas4, mas6;

    /*
     * False while translation is the identity with every permission:
     * the TLB holds only the entry this model loads at reset, and
     * MSR[IS] and MSR[DS] are both 0, which that entry's TS needs to
     * match. Recomputed by ppc_mmu_refresh wherever the TLB or MSR
     * changes, which sets irq_dirty when it moves -- that is what sends
     * the interpreter to the loop that translates. The JIT's `blocked`
     * points here: its inlined accesses do not translate.
     */
    bool mmu_on;
    bool tlb_default; /* the TLB is exactly its reset state */
    /*
     * Entry 0's MAS2 at reset, which carries the image's VLE choice. Kept
     * rather than derived from `vle`, because under a programmed TLB
     * `vle` follows whichever page is executing.
     */
    uint32_t tlb_reset_mas2;
    /*
     * Set by the access path when the data storage interrupt it returns
     * is a byte-ordering one -- a misaligned access across pages of
     * different endianness (11.16.5) -- and moved into ESR[BO] by
     * ppc_cpu_raise, which clears it on every interrupt. The callers
     * raise with ESR[ST] or nothing, as for any other DSI, so this is
     * the one bit the access has to carry to them.
     */
    bool dsi_bo;

    /*
     * The RAM windows blocks were given under a programmed TLB
     * (ppc_fast_mem), each the span a block reads and writes without
     * asking the MMU. A window is in no block's code range, so a TLB
     * change that touches one has to discard every block, where one
     * that does not discards only those whose code it mapped. A count
     * past the array means more were handed out than are held, and then
     * every TLB change discards everything. Cleared when everything is.
     */
    struct {
        uint32_t lo, hi;
    } jit_win[PPC_JIT_WINDOWS];
    uint32_t jit_nwin;

    /*
     * More than one core (ppc_cpu.c). `coreid` is the p_cpuid input PIR
     * resets to, kept apart from PIR because PIR is writable.
     *
     * While another core runs, a reservation has an address: the word,
     * physical, that lwarx read, and the bytes it read there. A store
     * from any other core to that word breaks it, and on a host running
     * cores in parallel stwcx. is a compare-and-swap against those bytes,
     * because the other core's store can land between the check and the
     * write -- the arrangement G4MH's LDL.W/STC.W use.
     */
    uint32_t coreid;
    /*
     * This core's slice of the interpreter's decode cache (ppc_interp.c),
     * found on its first run. A pointer here rather than a local in the
     * loop: as a local it held a register across the loop, and the
     * interpreter's loop paid two host instructions per guest instruction
     * for it.
     */
    void *dcache;
    uint32_t resv_pa;
    uint32_t resv_raw;
    bool resv_raw_ok; /* the word is RAM the host can swap */
    /*
     * What SIU_RSTVEC1 last asked of this core, from another core's
     * thread: PPC_REQ_START or PPC_REQ_HOLD, until this core acts on it.
     * The writer cannot reset this core's state while this core's thread
     * may be running, so it leaves the address and the encoding here and
     * the core starts itself at the top of its next run.
     *
     * **A request, not a state.** The core's own last slice may still be
     * running when it is held or released, and its `wait` then writes
     * WFI over whatever the request wrote -- and a waiting core with
     * interrupts off is one the scheduler never runs again, so the start
     * was lost: smp.c's restart failed one run in twenty. The status
     * reports the request while it is pending, whatever `state` says.
     */
    uint8_t reset_req;
    uint32_t start_pc;
    bool start_vle;
} ppc_cpu_t;

#define PPC_REQ_NONE 0u
#define PPC_REQ_START 1u
#define PPC_REQ_HOLD 2u

/*
 * The JIT's generation: every block goes when it moves. One for every
 * core rather than one each, because on a build where the cores share
 * one block table (a board, or --serial) two counters that differed
 * would discard everything at every switch from one core to the other.
 * Moved by a TLB change that touches a block's RAM window.
 */
extern uint32_t ppc_jit_gen;

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
/* More than one core                                                  */
/* ------------------------------------------------------------------ */

/*
 * Take a held core out of reset at `pc`, decoding VLE or Book E: the
 * SIU_RSTVEC1 write that clears RST (NXP AN5191, 3.9). Called on the
 * thread of the core that wrote the register. From here every core's
 * stores break the others' reservations, and any reservation held now
 * is dropped -- it was taken with no address, and a store-conditional
 * is allowed to fail.
 */
void ppc_cpu_start(ppc_cpu_t *c, uint32_t pc, bool vle);

/* Put a core back in reset: RST set again. It stops at its next slice. */
void ppc_cpu_hold(ppc_cpu_t *c);

/* The start ppc_cpu_start asked for, on the core's own thread. */
void ppc_cpu_take_start(ppc_cpu_t *c);

/*
 * lwarx and stwcx. (and the byte and halfword forms) while another core
 * runs: the reservation takes the word's physical address, and the
 * store succeeds only if no other core wrote the word since. `stored`
 * says whether stwcx. wrote. PPC_EXC_NONE or the interrupt to take,
 * with DEAR set.
 */
ppc_exc_t ppc_larx_smp(ppc_cpu_t *c, uint32_t ea, uint32_t size, uint32_t *out);
ppc_exc_t ppc_stcx_smp(ppc_cpu_t *c, uint32_t ea, uint32_t size, uint32_t val,
                       bool *stored);

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

/* ------------------------------------------------------------------ */
/* The MMU (ppc_mmu.c)                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    PPC_ACC_READ,
    PPC_ACC_WRITE,
    PPC_ACC_EXEC,
} ppc_acc_t;

/*
 * Reset state: entry 0 the whole 4 GB, every other entry invalid. See
 * ppc_mmu.c for why that, and not the manual's 4 KB page.
 */
void ppc_mmu_reset(ppc_cpu_t *c);

/* Recompute mmu_on. Called wherever the TLB or MSR changes. */
void ppc_mmu_refresh(ppc_cpu_t *c);

/*
 * Effective to real. PPC_EXC_NONE, or the interrupt to take -- data or
 * instruction TLB error on a miss, data or instruction storage on a
 * permission -- with DEAR and the MAS registers already updated as the
 * manual says that interrupt leaves them. `vle` and `le` (either may be
 * NULL) are the page's VLE and E attributes: its encoding, and whether
 * it is accessed in true little-endian byte order.
 */
ppc_exc_t ppc_mmu_xlate(ppc_cpu_t *c, uint32_t ea, ppc_acc_t acc, uint32_t *pa,
                        bool *vle, bool *le);

/*
 * The translator's question: may the core execute at `ea`, and from
 * where? ppc_mmu_xlate's answer for a fetch, with none of its side
 * effects -- a miss touches neither the MAS registers nor anything else,
 * because the translator reads ahead of what will run and an instruction
 * it looks at may never execute. False for a miss or no execute
 * permission; the interpreter, reaching it, takes the interrupt.
 */
bool ppc_mmu_probe_exec(const ppc_cpu_t *c, uint32_t ea, uint32_t *pa,
                        bool *vle, bool *le);

/*
 * The translator's other question: is the page holding `ea` one that
 * loads and stores may treat as plain memory? True, with the page's
 * effective and real bases and its size (up to 4 GB, hence 64 bits),
 * when the entry translating `ea` for data in the current address space
 * and PID grants both read and write in the current mode, is big-endian,
 * and no lower-numbered entry -- which would win the lookup for some of
 * its addresses -- overlaps it. Every effective address on the page then
 * translates the same way with the same permissions. No side effects.
 */
bool ppc_mmu_data_page(const ppc_cpu_t *c, uint32_t ea, uint32_t *va,
                       uint32_t *pa, uint64_t *size);

/* The TLB instructions, after the privilege check. */
void ppc_tlbre(ppc_cpu_t *c);
void ppc_tlbwe(ppc_cpu_t *c);
void ppc_tlbsx(ppc_cpu_t *c, uint32_t ea);
void ppc_tlbivax(ppc_cpu_t *c, uint32_t ea);
void ppc_mmu_flash_invalidate(ppc_cpu_t *c); /* MMUCSR0[TLB1_FI] */

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
 *
 * Under a programmed TLB the address is translated first, and the
 * result is PPC_EXC_NONE or the interrupt the access raises: data
 * storage for a bus error or a permission, data TLB error for a miss.
 * DEAR is set either way, and the caller raises it with ESR.
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
