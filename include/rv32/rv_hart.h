/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_hart.h - Architectural state of one RV32 hart.
 *
 * The struct is laid out so the fields the interpreter touches every
 * instruction (x[], pc) sit at offset 0: on Thumb-2 that keeps them within
 * the 5-bit immediate range of LDR/STR, so register access is a single
 * instruction off the hart pointer.
 */
#ifndef RV32_RV_HART_H
#define RV32_RV_HART_H

#include "rv_types.h"
#include "rv_config.h"
#include "emu/emu_bus.h"
#include "emu/emu_cache.h"
#include "emu/emu_cpu.h"
#include "rv_csr.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The RISC-V frontend's core state -- what emu_cpu_t is a pointer to when
 * rv32_frontend is the active frontend. It is reached through the ops
 * table by the platforms and cast back to this by the backends.
 */
typedef struct rv_hart {
    /* --- hot state: keep first --- */
    uint32_t x[32];              /* x0 is hardwired zero, written then ignored */
    uint32_t pc;

    struct emu_bus *bus;

    /* --- machine trap setup --- */
    uint32_t mstatus;
    uint32_t mie;
    uint32_t mip;
    uint32_t mtvec;
    uint32_t mscratch;
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t mcountinhibit;

    /* --- counters (Zicntr) --- */
    uint64_t mcycle;
    uint64_t minstret;

    /*
     * `time` is architecturally a memory-mapped counter owned by the CLINT,
     * not a hart register. This points at the CLINT's mtime so the time/
     * timeh CSRs can read it without the hart knowing about the device.
     */
    const volatile uint64_t *mtime;

#if RV_EXT_F
    /*
     * Single-precision register file. 32 bits per register because D is not
     * implemented; with D these would be 64-bit and F values NaN-boxed into
     * the low half.
     */
    uint32_t f[32];
    uint32_t fcsr;               /* frm in [7:5], fflags in [4:0] */
#endif

    uint32_t mcounteren;
    uint32_t menvcfg;
#if RV_EXT_S
    uint32_t senvcfg;
#endif

#if RV_EXT_S
    /*
     * The supervisor bank. sstatus, sie and sip are deliberately absent:
     * they are restricted *views* of mstatus, mie and mip rather than
     * registers, so giving them storage would be two places for one piece
     * of state and one of them would go stale.
     */
    uint32_t stvec;
    uint32_t sscratch;
    uint32_t sepc;
    uint32_t scause;
    uint32_t stval;
    uint32_t scounteren;
    uint32_t medeleg;
    uint32_t mideleg;
#endif

#if RV_EXT_SV32
    uint32_t satp;
    /*
     * True when translation is in force for *some* access: satp selects
     * Sv32 and either the fetch privilege or the data privilege is below
     * M. One flag rather than two because it only gates entry to the slow
     * path, which then asks which privilege this particular access uses --
     * MPRV can put those two in different modes.
     */
    bool     vm_active;

    /*
     * Bumped by every rv_mmu_flush. The JIT keys its blocks on *virtual*
     * addresses, so a change of mapping invalidates translated code even
     * though satp may not have moved -- SFENCE.VMA after editing a PTE is
     * exactly that case. Comparing a counter is what lets the JIT notice.
     */
    uint32_t vm_gen;

    /*
     * What the IR JIT's blocks are specialised on, as one word: the
     * rounding mode, whether the FP unit is on, and vm_gen. Maintained
     * on the interpreter fallback -- see rv_ir.c -- because everything
     * in it moves only through a CSR write, and read by the framework on
     * every block entry, which is why it is a cached value rather than
     * something re-derived.
     */
    uint32_t jit_gen;

    /*
     * Direct-mapped TLB. Tagged with the full VPN, so no flush is needed
     * on an ASID change that does not alter the mapping -- but satp writes
     * and SFENCE.VMA flush it wholesale, which is the conservative reading
     * of both and costs nothing measurable next to a walk.
     *
     * Permissions are stored rather than resolved, because whether an
     * entry may serve an access depends on the privilege, SUM and MXR at
     * the time of the access, not at the time of the fill.
     */
    struct {
        uint32_t vpn;      /* virtual page number; entry invalid if !valid */
        uint32_t ppn;      /* physical base of the page                    */
        uint8_t  pte;      /* the PTE's V/R/W/X/U/G/A/D bits, verbatim     */
        bool     valid;
    } tlb[RV_TLB_ENTRIES];
#endif

#if RV_EXT_SDTRIG
    uint32_t tselect;
    uint32_t tdata1[RV_TRIG_COUNT];
    uint32_t tdata2[RV_TRIG_COUNT];
    /* True once some trigger is armed; gates the checks out of the hot path. */
    bool     trig_active;
#endif

#if RV_EXT_PMP
    uint32_t pmpcfg[RV_PMP_ENTRIES / 4u];
    uint32_t pmpaddr[RV_PMP_ENTRIES];
    /*
     * True when some entry is both locked and enabled. Only then can PMP
     * deny an M-mode access, so this gates the check out of the hot path
     * for every guest that does not use it.
     */
    bool     pmp_active;
#endif

#if RV_EXT_A
    /* LR/SC reservation set. One hart, so one reservation. */
    uint32_t resv_addr;
    bool     resv_valid;
#endif

#if RV_EXT_ZICBOM
    /*
     * Platform cache maintenance, or NULL on a system without caches (in
     * which case the CBO instructions retire without doing anything, which
     * the spec permits).
     */
    const emu_cache_ops_t *cache;
#endif

#if RV_LAZY_IRQ_CHECK
    /*
     * Set whenever something may have made an interrupt deliverable: a
     * device raising a line, a write to mstatus or mie, or an MRET
     * restoring MIE. The run loop calls rv_hart_pending_irq only when this
     * is set, and clears it when the check comes back empty.
     *
     * volatile because a platform may raise a line from an ARM interrupt
     * handler while the run loop is executing. The loop must clear this
     * *before* evaluating, not after: clearing afterwards would overwrite
     * a set that an interrupt handler performed during the evaluation, and
     * that interrupt would then go unnoticed until something else happened
     * to dirty the flag again.
     */
    volatile bool irq_dirty;
#endif

    /*
     * trig_active || pmp_active, so the fetch path tests one flag rather
     * than one per feature.
     *
     * Anything on the fetch path is paid by every instruction whether the
     * feature is used or not, and both of these are almost always false.
     * Testing them independently measured 9.3% on CoreMark, which arms
     * neither. Maintained by rv_trig_refresh and rv_pmp_refresh, the only
     * writers of the flags it combines; it lives outside both #ifdefs so
     * the fetch path does not need to know which are configured in.
     */
    bool     fetch_guard;

    uint32_t hartid;
    uint8_t  priv;               /* always RV_PRIV_M on this implementation */
    uint8_t  state;              /* emu_state_t */

#if EMU_ENABLE_STATS
    uint32_t trap_count;
    uint32_t insn_retired_lo;    /* cheap 32-bit mirror for the monitor */
#endif

#if EMU_ENABLE_TRACE
    emu_trace_fn trace;
    void        *trace_user;
#endif

#if RV_ENABLE_ECALL_HOOK
    /*
     * Optional ECALL interception, for host-side test harnesses and for
     * platforms that want to offer SBI-style services alongside the
     * memory-mapped devices. Return true to consume the ECALL (execution
     * resumes after it); return false to take the normal M-mode trap.
     * When NULL, ECALL always traps, which is the architectural behaviour.
     *
     * The handler sees an emu_syscall_t rather than the hart, so the same
     * newlib write/exit implementation serves any frontend: unpacking the
     * RISC-V convention -- number in a7, arguments in a0-a3, result to a0
     * -- happens here, at the one place that knows it.
     */
    emu_syscall_fn ecall;
    void          *ecall_user;
#endif

    void *user;                  /* opaque platform pointer */
} rv_hart_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Zero the hart, attach the bus, and apply reset values. */
void rv_hart_init(rv_hart_t *h, emu_bus_t *bus, uint32_t hartid);

/* Reset architectural state and jump to `reset_pc`. */
void rv_hart_reset(rv_hart_t *h, uint32_t reset_pc);

/*
 * Boot protocol. After reset, hand the guest the things only the platform
 * knows: a stack pointer at the top of guest RAM, its hart id, and how
 * much RAM it actually has. Guests that ignore this and use their own link
 * script still work, which is why it is a separate call rather than part
 * of rv_hart_reset.
 *
 *   sp (x2) = ram_base + ram_size, 16-byte aligned
 *   a0 (x10) = hartid
 *   a1 (x11) = ram_size
 */
void rv_hart_boot(rv_hart_t *h, uint32_t ram_base, uint32_t ram_size);

/* misa value for the configured extension set. */
uint32_t rv_hart_misa(void);

/* ------------------------------------------------------------------ */
/* Traps and interrupts                                                */
/* ------------------------------------------------------------------ */

/*
 * Enter a trap: save pc/cause/tval into mepc/mcause/mtval, push MIE to
 * MPIE, and set pc from mtvec. `cause` is a bare exception code, or an
 * interrupt code ORed with RV_CAUSE_INTERRUPT.
 */
void rv_hart_trap(rv_hart_t *h, uint32_t cause, uint32_t tval);

/*
 * Return the pending-and-enabled interrupt cause, or RV_EXC_NONE. Honours
 * mstatus.MIE, so it returns nothing while interrupts are globally masked.
 */
rv_exc_t rv_hart_pending_irq(const rv_hart_t *h);

/*
 * True if WFI should resume.
 *
 * Deliberately *not* rv_hart_pending_irq. The enable bits mstatus.MIE and
 * mstatus.SIE decide whether a trap is taken; they do not decide whether a
 * parked hart wakes. The spec is explicit that the global enable is not
 * consulted for resumption, and the reason is visible the moment there is
 * a second privilege level: a supervisor executing WFI with SIE clear --
 * which is the normal state inside its own trap handler -- would otherwise
 * park forever on an interrupt that is pending and enabled for it.
 *
 * Waking without a deliverable interrupt is harmless and intended:
 * execution simply resumes at the instruction after the WFI.
 */
static EMU_ALWAYS_INLINE bool rv_hart_wfi_wake(const rv_hart_t *h)
{
    return (h->mip & h->mie) != 0u;
}

/* Raise or clear a device interrupt line (RV_INT_M_*). */
void rv_hart_set_irq(rv_hart_t *h, unsigned cause, bool level);

/* ------------------------------------------------------------------ */
/* Memory helpers (permission-checked, used by the interpreter)        */
/* ------------------------------------------------------------------ */

rv_exc_t rv_hart_load(rv_hart_t *h, uint32_t addr, uint32_t size,
                      bool sign_extend, uint32_t *out);
rv_exc_t rv_hart_store(rv_hart_t *h, uint32_t addr, uint32_t size, uint32_t val);

#if RV_EXT_SDTRIG
void rv_trig_refresh(rv_hart_t *h);
void rv_trig_write_tdata1(rv_hart_t *h, uint32_t val);

/* True if an armed trigger matches. Only consulted when trig_active. */
bool rv_trig_check(const rv_hart_t *h, uint32_t addr, emu_access_t acc);
#endif

/*
 * The privilege level a load or store is checked at, which is not always
 * the one the hart is running at.
 *
 * MPRV makes data accesses use MPP's privilege instead, so that M-mode
 * software can touch memory exactly as the mode it is about to return to
 * would see it -- the way a kernel validates a pointer a user process
 * handed it. Instruction fetch is explicitly *not* affected and always
 * uses h->priv, which is what makes this a separate accessor rather than a
 * correction applied to h->priv itself.
 *
 * MPRV can only be set by M-mode, and MRET clears it when returning below
 * M, so it cannot outlive the mode that armed it.
 */
/* Recompute h->fetch_guard. Called by rv_trig_refresh and rv_pmp_refresh. */
static EMU_ALWAYS_INLINE void rv_hart_refresh_fetch_guard(rv_hart_t *h)
{
    bool g = false;
#if RV_EXT_SDTRIG
    g = g || h->trig_active;
#endif
#if RV_EXT_PMP
    g = g || h->pmp_active;
#endif
#if RV_EXT_SV32
    g = g || h->vm_active;
#endif
    h->fetch_guard = g;
}

static EMU_ALWAYS_INLINE uint32_t rv_hart_data_priv(const rv_hart_t *h)
{
#if RV_EXT_U
    if ((h->mstatus & MSTATUS_MPRV) != 0u) {
        return (h->mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT;
    }
#endif
    return h->priv;
}

#if RV_EXT_SV32
/*
 * Recompute vm_active. Call after any write to satp or mstatus, and after
 * any change to h->priv -- all three can start or stop translation.
 */
void rv_mmu_refresh(rv_hart_t *h);

/* Discard every cached translation. SFENCE.VMA and satp writes land here. */
void rv_mmu_flush(rv_hart_t *h);

/*
 * Translate a virtual address for one access. Returns RV_EXC_NONE and
 * writes *pa, or the page-fault cause for `acc`.
 *
 * Only called when vm_active; a hart with satp in Bare mode, or running in
 * M-mode, never reaches it.
 */
rv_exc_t rv_mmu_translate(rv_hart_t *h, uint32_t va, emu_access_t acc,
                          uint32_t *pa);
#endif

#if RV_EXT_PMP
/*
 * Recompute pmp_active. Call after any write to a pmpcfg CSR, after any
 * change to h->priv, and after any write to mstatus (MPRV and MPP move the
 * privilege data accesses are checked at). Below M-mode PMP must always be
 * consulted, because matching no entry denies rather than permits.
 */
void rv_pmp_refresh(rv_hart_t *h);

/*
 * True if the access is permitted. Only consulted when pmp_active, which
 * is what keeps PMP off the hot path for guests that never lock an entry.
 */
bool rv_pmp_check(const rv_hart_t *h, uint32_t addr, uint32_t size,
                  emu_access_t acc);

/*
 * When exactly one PMP entry is enabled, report its bounds and return true.
 * The range is where the full check is *required*, not what is permitted:
 * anything outside matches no entry and is allowed in M-mode. The JIT uses
 * it to keep its inlined memory path when a guest arms PMP.
 */
bool rv_pmp_simple(const rv_hart_t *h, uint32_t *lo, uint32_t *hi);
#endif

#if RV_EXT_F
/*
 * Execute one F-extension instruction. `insn` is the full 32-bit encoding;
 * OP-FP, the fused multiply-adds, FLW and FSW all route here. Returns
 * RV_EXC_NONE, or the cause to report; *tval receives the value for mtval,
 * which for a load or store fault is the address.
 *
 * Kept out of the interpreter's switch so the same implementation can back
 * a JIT helper without duplicating the rounding and flag rules.
 */
rv_exc_t rv_hart_fp(rv_hart_t *h, uint32_t insn, uint32_t *tval);
#endif

#if RV_EXT_A
/* funct5 field of an AMO encoding (inst[31:27]). */
#define RV_AMO_ADD   0x00u
#define RV_AMO_SWAP  0x01u
#define RV_AMO_LR    0x02u
#define RV_AMO_SC    0x03u
#define RV_AMO_XOR   0x04u
#define RV_AMO_OR    0x08u
#define RV_AMO_AND   0x0Cu
#define RV_AMO_MIN   0x10u
#define RV_AMO_MAX   0x14u
#define RV_AMO_MINU  0x18u
#define RV_AMO_MAXU  0x1Cu
#define RV_AMO_CAS   0x05u   /* Zacas: amocas.w */

/* True if `funct5` names an operation this core implements. */
bool rv_amo_valid(uint32_t funct5);

/*
 * Execute one A-extension operation, including the alignment check, the
 * reservation bookkeeping and the write of rd. `src` is the rs2 value,
 * unused by LR. Returns RV_EXC_NONE, or the cause to report with the
 * address as mtval.
 *
 * Shared by the interpreter and the JIT deliberately: two copies of the
 * LR/SC reservation rules would be a place for them to drift apart, and a
 * divergence there is exactly the kind of bug that only shows up under a
 * specific interleaving. `funct5` must already have passed rv_amo_valid.
 */
rv_exc_t rv_hart_amo(rv_hart_t *h, uint32_t funct5, uint32_t rd,
                     uint32_t addr, uint32_t src);

#if RV_EXT_ZACAS
/*
 * amocas.d on RV32: a 64-bit compare-and-swap whose operands are even-odd
 * register pairs, (rd, rd+1) and (rs2, rs2+1), low half first. This needs
 * its own entry point because rv_hart_amo's single-register interface
 * cannot express a pair. rd and rs2 must be even; the caller checks that.
 * x0 names a pair that reads as zero and discards the result.
 */
rv_exc_t rv_hart_amocas_d(rv_hart_t *h, uint32_t rd, uint32_t rs2,
                          uint32_t addr);
#endif
#endif

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ
/* The 12-bit immediate of a CBO encoding selects the operation. */
#define RV_CBO_OP_INVAL  0u
#define RV_CBO_OP_CLEAN  1u
#define RV_CBO_OP_FLUSH  2u
#define RV_CBO_OP_ZERO   4u

bool rv_cbo_valid(uint32_t op);

/*
 * Execute one cache-block operation on the block containing `addr`.
 * Returns RV_EXC_NONE, or the cause to report; on failure *fault_addr
 * receives the address to place in mtval, which for cbo.zero is the
 * specific word that faulted rather than the block base.
 *
 * Shared by the interpreter and the JIT.
 */
rv_exc_t rv_hart_cbo(rv_hart_t *h, uint32_t op, uint32_t addr,
                     uint32_t *fault_addr);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_HART_H */
