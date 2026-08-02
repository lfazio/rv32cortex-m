/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_hart.h - Architectural state of one RV32IMAC hart.
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
#include "rv_bus.h"
#include "rv_cache.h"
#include "rv_csr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Execution state. */
typedef enum {
    RV_STATE_RUNNING = 0,
    RV_STATE_WFI,        /* parked in WFI, waiting for an interrupt   */
    RV_STATE_HALTED,     /* stopped by the debugger or a fatal fault  */
} rv_state_t;

/* Reason rv_backend_t::run returned. */
typedef enum {
    RV_RUN_BUDGET = 0,   /* instruction budget exhausted, call again  */
    RV_RUN_WFI,          /* hart entered WFI                          */
    RV_RUN_HALTED,       /* hart halted                               */
    RV_RUN_BREAKPOINT,   /* EBREAK with debug attached                */
} rv_run_reason_t;

typedef struct rv_hart {
    /* --- hot state: keep first --- */
    uint32_t x[32];              /* x0 is hardwired zero, written then ignored */
    uint32_t pc;

    struct rv_bus *bus;

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
    const rv_cache_ops_t *cache;
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

    uint32_t hartid;
    uint8_t  priv;               /* always RV_PRIV_M on this implementation */
    uint8_t  state;              /* rv_state_t */

#if RV_ENABLE_STATS
    uint32_t trap_count;
    uint32_t insn_retired_lo;    /* cheap 32-bit mirror for the monitor */
#endif

#if RV_ENABLE_TRACE
    void (*trace)(struct rv_hart *h, uint32_t pc, uint32_t insn, void *user);
    void  *trace_user;
#endif

#if RV_ENABLE_ECALL_HOOK
    /*
     * Optional ECALL interception, for host-side test harnesses and for
     * platforms that want to offer SBI-style services alongside the
     * memory-mapped devices. Return true to consume the ECALL (execution
     * resumes after it); return false to take the normal M-mode trap.
     * When NULL, ECALL always traps, which is the architectural behaviour.
     */
    bool (*ecall)(struct rv_hart *h, void *user);
    void  *ecall_user;
#endif

    void *user;                  /* opaque platform pointer */
} rv_hart_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Zero the hart, attach the bus, and apply reset values. */
void rv_hart_init(rv_hart_t *h, rv_bus_t *bus, uint32_t hartid);

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
bool rv_trig_check(const rv_hart_t *h, uint32_t addr, rv_access_t acc);
#endif

#if RV_EXT_PMP
/*
 * Recompute pmp_active. Call after any write to a pmpcfg CSR *and* after
 * any change to h->priv: below M-mode PMP must always be consulted, because
 * matching no entry denies rather than permits.
 */
void rv_pmp_refresh(rv_hart_t *h);

/*
 * True if the access is permitted. Only consulted when pmp_active, which
 * is what keeps PMP off the hot path for guests that never lock an entry.
 */
bool rv_pmp_check(const rv_hart_t *h, uint32_t addr, uint32_t size,
                  rv_access_t acc);

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
