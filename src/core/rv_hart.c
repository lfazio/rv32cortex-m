/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_hart.c - Hart lifecycle, trap entry, and permission-checked memory.
 */

#include "rv32/rv_hart.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

uint32_t rv_hart_misa(void)
{
    uint32_t misa = MISA_MXL_32 | MISA_EXT('I');
#if RV_EXT_M
    misa |= MISA_EXT('M');
#endif
#if RV_EXT_A
    misa |= MISA_EXT('A');
#endif
#if RV_EXT_C
    misa |= MISA_EXT('C');
#endif
#if RV_EXT_F
    misa |= MISA_EXT('F');
#endif
    /*
     * B is exactly Zba, Zbb and Zbs -- the three the 2024 ratification
     * folded into it. Zbc is deliberately absent from that list: it was
     * moved out of B before ratification and remains a separate extension,
     * so implementing it neither grants nor withholds this bit.
     *
     * Advertising it matters. A guest that checks misa.B and finds it clear
     * will take its fallback paths for byte swaps, counts and single-bit
     * work, which is the opposite of what a core implementing all three
     * wants. Anything built with -march=..._zba_zbb_zbs and run against a
     * core reporting no B is the same disagreement rv32mi/csr exists to
     * catch, just in the other direction.
     */
#if RV_EXT_ZBA && RV_EXT_ZBB && RV_EXT_ZBS
    misa |= MISA_EXT('B');
#endif
#if RV_EXT_S
    misa |= MISA_EXT('S');
#endif
#if RV_EXT_U
    misa |= MISA_EXT('U');
#endif
    return misa;
}

void rv_hart_init(rv_hart_t *h, rv_bus_t *bus, uint32_t hartid)
{
    memset(h, 0, sizeof(*h));
    h->bus = bus;
    h->hartid = hartid;
    rv_hart_reset(h, 0u);
}

void rv_hart_reset(rv_hart_t *h, uint32_t reset_pc)
{
    memset(h->x, 0, sizeof(h->x));
    h->pc = reset_pc;

    /*
     * Reset values per the privileged spec: the only architecturally
     * required state is that the hart starts in M-mode with interrupts
     * globally disabled and mcause cleared.
     */
    h->mstatus = 0u;
    h->mie = 0u;
    h->mip = 0u;
    h->mtvec = 0u;
    h->mscratch = 0u;
    h->mepc = 0u;
    h->mcause = 0u;
    h->mtval = 0u;
    h->mcountinhibit = 0u;
    h->mcycle = 0u;
    h->minstret = 0u;

#if RV_EXT_F
    memset(h->f, 0, sizeof(h->f));
    h->fcsr = 0u;
    /* Report the FPU as Initial so software can use it without first
     * writing mstatus; the spec leaves the reset value implementation
     * defined and leaving it Off would make every FP instruction trap. */
    h->mstatus |= (1u << MSTATUS_FS_SHIFT);
#endif

    h->mcounteren = 0u;

#if RV_EXT_S
    h->stvec = 0u;
    h->sscratch = 0u;
    h->sepc = 0u;
    h->scause = 0u;
    h->stval = 0u;
    h->scounteren = 0u;
    h->medeleg = 0u;
    h->mideleg = 0u;
#endif

#if RV_EXT_SDTRIG
    h->tselect = 0u;
    memset(h->tdata1, 0, sizeof(h->tdata1));
    memset(h->tdata2, 0, sizeof(h->tdata2));
    h->trig_active = false;
#endif

#if RV_EXT_PMP
    memset(h->pmpcfg, 0, sizeof(h->pmpcfg));
    memset(h->pmpaddr, 0, sizeof(h->pmpaddr));
    h->pmp_active = false;
#endif

#if RV_EXT_A
    h->resv_valid = false;
    h->resv_addr = 0u;
#endif

    h->priv = RV_PRIV_M;
    h->state = RV_STATE_RUNNING;

#if RV_LAZY_IRQ_CHECK
    /* Force one evaluation after reset rather than assuming the state. */
    h->irq_dirty = true;
#endif

#if RV_ENABLE_STATS
    h->trap_count = 0u;
    h->insn_retired_lo = 0u;
#endif
}

void rv_hart_boot(rv_hart_t *h, uint32_t ram_base, uint32_t ram_size)
{
    /* The RISC-V psABI wants the stack 16-byte aligned on entry. */
    h->x[2] = (ram_base + ram_size) & ~15u;
    h->x[10] = h->hartid;
    h->x[11] = ram_size;
}

/* ------------------------------------------------------------------ */
/* Traps                                                               */
/* ------------------------------------------------------------------ */

#if RV_EXT_S
/*
 * Does this trap go to S-mode?
 *
 * Only if it is delegated *and* it came from S or U. Delegation cannot
 * move a trap taken in M-mode to a less privileged handler -- that would
 * hand the supervisor the machine's own faults -- so the privilege test is
 * not a shortcut for the common case, it is half the rule.
 */
static bool trap_to_s(const rv_hart_t *h, uint32_t cause)
{
    if (h->priv > RV_PRIV_S) {
        return false;
    }
    const uint32_t code = cause & ~RV_CAUSE_INTERRUPT;
    const uint32_t deleg = (cause & RV_CAUSE_INTERRUPT) ? h->mideleg
                                                        : h->medeleg;
    return code < 32u && ((deleg >> code) & 1u) != 0u;
}
#endif

void rv_hart_trap(rv_hart_t *h, uint32_t cause, uint32_t tval)
{
    uint32_t tvec;

#if RV_EXT_S
    if (trap_to_s(h, cause)) {
        h->sepc = h->pc;
        h->scause = cause;
        h->stval = tval;

        /*
         * The same enable stack as the machine one, one level down: SPIE
         * takes the old SIE, SIE clears, SPP records where we came from.
         * SPP is a single bit because the only modes that can reach an
         * S-mode handler are S and U.
         */
        const uint32_t sie_was = (h->mstatus & MSTATUS_SIE) ? MSTATUS_SPIE : 0u;
        h->mstatus = (h->mstatus & ~(MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP))
                   | sie_was
                   | ((h->priv == RV_PRIV_S) ? MSTATUS_SPP : 0u);

        h->priv = RV_PRIV_S;
        tvec = h->stvec;
    } else
#endif
    {
        h->mepc = h->pc;
        h->mcause = cause;
        h->mtval = tval;

        /*
         * Push the interrupt-enable stack: MPIE takes the old MIE, MIE
         * clears, and MPP records the privilege we came from.
         */
        const uint32_t mie_was = (h->mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0u;
        h->mstatus = (h->mstatus &
                      ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK))
                   | mie_was
                   | ((uint32_t)h->priv << MSTATUS_MPP_SHIFT);

        h->priv = RV_PRIV_M;
        tvec = h->mtvec;
    }

#if RV_EXT_U
    /*
     * Refreshing PMP is not bookkeeping: whether PMP has to be consulted at
     * all depends on the privilege level, because below M matching no entry
     * denies rather than permits. A trap into S-mode raises privilege
     * without reaching M, so this is as load-bearing there as anywhere.
     */
#  if RV_EXT_PMP
    rv_pmp_refresh(h);
#  endif
#endif

    uint32_t base = tvec & ~MTVEC_MODE_MASK;
    if ((tvec & MTVEC_MODE_MASK) == MTVEC_MODE_VECTORED &&
        (cause & RV_CAUSE_INTERRUPT)) {
        /* Vectored mode applies to interrupts only; exceptions use base. */
        base += 4u * (cause & ~RV_CAUSE_INTERRUPT);
    }
    h->pc = base;

    /*
     * A trap breaks any outstanding LR/SC sequence. The spec allows an
     * implementation to keep the reservation, but dropping it is simpler to
     * reason about and guarantees forward progress cannot depend on it.
     */
#if RV_EXT_A
    h->resv_valid = false;
#endif

#if RV_ENABLE_STATS
    h->trap_count++;
#endif
}

rv_exc_t rv_hart_pending_irq(const rv_hart_t *h)
{
    uint32_t active = h->mip & h->mie;
    if (active == 0u) {
        return RV_EXC_NONE;
    }

    /*
     * mstatus.MIE gates M-mode interrupts *only while running in M-mode*.
     * Below M they are always enabled, because a less privileged mode is
     * not allowed to mask the machine's own interrupts by clearing a bit it
     * cannot even see. Testing MIE unconditionally -- which is all that was
     * needed while M was the only mode -- means a guest that MRETs to U
     * with MPIE clear never takes another timer interrupt and simply hangs.
     */
    if (h->priv == RV_PRIV_M && (h->mstatus & MSTATUS_MIE) == 0u) {
        /* Delegated interrupts are not a way around this: an S-mode
         * interrupt does not preempt M-mode either. */
        return RV_EXC_NONE;
    }

#if RV_EXT_S
    /*
     * A delegated interrupt is destined for S-mode, so it obeys SIE rather
     * than MIE and only while running in S; in M-mode it cannot be taken at
     * all, and in U-mode it is always enabled. Undelegated interrupts stay
     * with M and are already enabled by the test above.
     */
    if (h->priv == RV_PRIV_M) {
        active &= ~h->mideleg;
    } else if (h->priv == RV_PRIV_S && (h->mstatus & MSTATUS_SIE) == 0u) {
        active &= ~h->mideleg;
    }
    if (active == 0u) {
        return RV_EXC_NONE;
    }
#endif

    /* Priority order from the privileged spec: MEI, MSI, MTI, then SEI,
     * SSI, STI. Machine interrupts outrank supervisor ones throughout. */
    if (active & MIP_MEIP) {
        return RV_INT_M_EXT;
    }
    if (active & MIP_MSIP) {
        return RV_INT_M_SOFT;
    }
    if (active & MIP_MTIP) {
        return RV_INT_M_TIMER;
    }
#if RV_EXT_S
    if (active & MIP_SEIP) {
        return RV_INT_S_EXT;
    }
    if (active & MIP_SSIP) {
        return RV_INT_S_SOFT;
    }
    return RV_INT_S_TIMER;
#else
    return RV_INT_M_TIMER;
#endif
}

void rv_hart_set_irq(rv_hart_t *h, unsigned cause, bool level)
{
    const uint32_t bit = 1u << cause;
    if (level) {
        h->mip |= bit;
    } else {
        h->mip &= ~bit;
    }

#if RV_LAZY_IRQ_CHECK
    /*
     * Dirty unconditionally rather than only when mip changed: this is
     * called from device code and, on some platforms, from an ARM
     * interrupt handler, and an extra evaluation costs far less than
     * reasoning about whether a redundant-looking write really was.
     */
    h->irq_dirty = true;
#endif
}

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

/*
 * Alignment is checked here rather than in the bus so the correct
 * misaligned-vs-access-fault cause can be reported, and so the bus can
 * assume aligned accesses on its fast paths.
 */
rv_exc_t rv_hart_load(rv_hart_t *h, uint32_t addr, uint32_t size,
                      bool sign_extend, uint32_t *out)
{
#if !RV_MISALIGNED_OK
    if (RV_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_LOAD_MISALIGNED;
    }
#endif

#if RV_EXT_SDTRIG
    if (RV_UNLIKELY(h->trig_active) && rv_trig_check(h, addr, RV_ACC_LOAD)) {
        return RV_EXC_BREAKPOINT;
    }
#endif
#if RV_EXT_PMP
    if (RV_UNLIKELY(h->pmp_active) &&
        !rv_pmp_check(h, addr, size, RV_ACC_LOAD)) {
        return RV_EXC_LOAD_ACCESS_FAULT;
    }
#endif

    uint32_t v;
    const rv_exc_t exc = rv_bus_read(h->bus, addr, size, &v);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        return exc;
    }

    if (sign_extend && size < 4u) {
        v = (uint32_t)rv_sext(v, size * 8u);
    }
    *out = v;
    return RV_EXC_NONE;
}

#if RV_EXT_A

bool rv_amo_valid(uint32_t funct5)
{
    switch (funct5) {
    case RV_AMO_ADD:  case RV_AMO_SWAP: case RV_AMO_LR:  case RV_AMO_SC:
    case RV_AMO_XOR:  case RV_AMO_OR:   case RV_AMO_AND:
    case RV_AMO_MIN:  case RV_AMO_MAX:  case RV_AMO_MINU: case RV_AMO_MAXU:
        return true;
#if RV_EXT_ZACAS
    case RV_AMO_CAS:
        return true;
#endif
    default:
        return false;
    }
}

rv_exc_t rv_hart_amo(rv_hart_t *h, uint32_t funct5, uint32_t rd,
                     uint32_t addr, uint32_t src)
{
    /* Every AMO requires a naturally aligned address. */
    if (RV_UNLIKELY((addr & 3u) != 0u)) {
        return (funct5 == RV_AMO_LR) ? RV_EXC_LOAD_MISALIGNED
                                     : RV_EXC_STORE_MISALIGNED;
    }

    if (funct5 == RV_AMO_LR) {
        uint32_t v;
        const rv_exc_t exc = rv_bus_read(h->bus, addr, 4u, &v);
        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
            return exc;
        }
        h->resv_addr = addr;
        h->resv_valid = true;
        if (rd != 0u) {
            h->x[rd] = v;
        }
        return RV_EXC_NONE;
    }

    if (funct5 == RV_AMO_SC) {
        if (!h->resv_valid || h->resv_addr != addr) {
            if (rd != 0u) {
                h->x[rd] = 1u;      /* non-zero: the store did not occur */
            }
            return RV_EXC_NONE;
        }
        const rv_exc_t exc = rv_bus_write(h->bus, addr, 4u, src);
        h->resv_valid = false;
        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
            return exc;
        }
        if (rd != 0u) {
            h->x[rd] = 0u;          /* zero: success */
        }
        return RV_EXC_NONE;
    }

    uint32_t old;
    rv_exc_t exc = rv_bus_read(h->bus, addr, 4u, &old);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        /* An AMO reports load and store faults alike as store. */
        return RV_EXC_STORE_ACCESS_FAULT;
    }

#if RV_EXT_ZACAS
    if (funct5 == RV_AMO_CAS) {
        /*
         * amocas.w: rd is both the comparand and the destination, so its
         * old value must be read before rd is written. The store happens
         * only on a match, but rd is updated either way.
         */
        if (old == h->x[rd]) {
            exc = rv_bus_write(h->bus, addr, 4u, src);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                return exc;
            }
        }
        if (rd != 0u) {
            h->x[rd] = old;
        }
        return RV_EXC_NONE;
    }
#endif

    uint32_t val;
    switch (funct5) {
    case RV_AMO_ADD:  val = old + src; break;
    case RV_AMO_SWAP: val = src; break;
    case RV_AMO_XOR:  val = old ^ src; break;
    case RV_AMO_OR:   val = old | src; break;
    case RV_AMO_AND:  val = old & src; break;
    case RV_AMO_MIN:  val = ((int32_t)old < (int32_t)src) ? old : src; break;
    case RV_AMO_MAX:  val = ((int32_t)old > (int32_t)src) ? old : src; break;
    case RV_AMO_MINU: val = (old < src) ? old : src; break;
    default:          val = (old > src) ? old : src; break;   /* MAXU */
    }

    exc = rv_bus_write(h->bus, addr, 4u, val);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        return exc;
    }
    /* rd takes the value read, written after the store in case rd == rs2. */
    if (rd != 0u) {
        h->x[rd] = old;
    }
    return RV_EXC_NONE;
}

#if RV_EXT_ZACAS
rv_exc_t rv_hart_amocas_d(rv_hart_t *h, uint32_t rd, uint32_t rs2,
                          uint32_t addr)
{
    /* A 64-bit atomic needs 8-byte alignment, not 4. */
    if (RV_UNLIKELY((addr & 7u) != 0u)) {
        return RV_EXC_STORE_MISALIGNED;
    }

    uint32_t lo, hi;
    if (rv_bus_read(h->bus, addr, 4u, &lo) != RV_EXC_NONE ||
        rv_bus_read(h->bus, addr + 4u, 4u, &hi) != RV_EXC_NONE) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }

    /* x0 names a pair that reads as zero in both halves. */
    const uint32_t cmp_lo = (rd == 0u) ? 0u : h->x[rd];
    const uint32_t cmp_hi = (rd == 0u) ? 0u : h->x[rd + 1u];

    if (lo == cmp_lo && hi == cmp_hi) {
        const uint32_t new_lo = (rs2 == 0u) ? 0u : h->x[rs2];
        const uint32_t new_hi = (rs2 == 0u) ? 0u : h->x[rs2 + 1u];
        rv_exc_t exc = rv_bus_write(h->bus, addr, 4u, new_lo);
        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
            return exc;
        }
        exc = rv_bus_write(h->bus, addr + 4u, 4u, new_hi);
        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
            return exc;
        }
    }

    /* The loaded value goes to rd either way. */
    if (rd != 0u) {
        h->x[rd] = lo;
        h->x[rd + 1u] = hi;
    }
    return RV_EXC_NONE;
}
#endif /* RV_EXT_ZACAS */

#endif /* RV_EXT_A */

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ

bool rv_cbo_valid(uint32_t op)
{
    switch (op) {
#if RV_EXT_ZICBOZ
    case RV_CBO_OP_ZERO:
        return true;
#endif
#if RV_EXT_ZICBOM
    case RV_CBO_OP_INVAL:
    case RV_CBO_OP_CLEAN:
    case RV_CBO_OP_FLUSH:
        return true;
#endif
    default:
        return false;
    }
}

rv_exc_t rv_hart_cbo(rv_hart_t *h, uint32_t op, uint32_t addr,
                     uint32_t *fault_addr)
{
    const uint32_t base = addr & ~(RV_CACHE_BLOCK_SIZE - 1u);

    *fault_addr = base;

#if RV_EXT_ZICBOZ
    if (op == RV_CBO_OP_ZERO) {
        /*
         * Unlike the maintenance operations this has an architecturally
         * visible effect, so it goes through the permission-checked store
         * path rather than writing host memory directly.
         */
        for (uint32_t i = 0; i < RV_CACHE_BLOCK_SIZE; i += 4u) {
            const rv_exc_t exc = rv_hart_store(h, base + i, 4u, 0u);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                *fault_addr = base + i;
                return exc;
            }
        }
        return RV_EXC_NONE;
    }
#endif

#if RV_EXT_ZICBOM
    {
        /*
         * Maintenance applies to the host memory backing the block, so a
         * guest cleaning a DMA buffer cleans the ARM cache lines that
         * really hold it.
         */
        void *host = rv_bus_host_ptr(h->bus, base, RV_CACHE_BLOCK_SIZE);
        if (RV_UNLIKELY(host == NULL)) {
            return RV_EXC_STORE_ACCESS_FAULT;
        }
        if (h->cache != NULL && h->cache->maint != NULL) {
            static const rv_cbo_op_t map[3] = {
                RV_CBO_INVAL, RV_CBO_CLEAN, RV_CBO_FLUSH
            };
            h->cache->maint(h->cache->ctx, host, RV_CACHE_BLOCK_SIZE, map[op]);
        }
        /* No cache maintenance configured: retiring without doing anything
         * is architecturally legal. */
        return RV_EXC_NONE;
    }
#else
    return RV_EXC_ILLEGAL_INSN;
#endif
}

#endif /* RV_EXT_ZICBOM || RV_EXT_ZICBOZ */

rv_exc_t rv_hart_store(rv_hart_t *h, uint32_t addr, uint32_t size, uint32_t val)
{
#if !RV_MISALIGNED_OK
    if (RV_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_STORE_MISALIGNED;
    }
#endif

#if RV_EXT_SDTRIG
    if (RV_UNLIKELY(h->trig_active) && rv_trig_check(h, addr, RV_ACC_STORE)) {
        return RV_EXC_BREAKPOINT;
    }
#endif
#if RV_EXT_PMP
    if (RV_UNLIKELY(h->pmp_active) &&
        !rv_pmp_check(h, addr, size, RV_ACC_STORE)) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }
#endif

#if RV_EXT_A
    /*
     * Any store to the reserved word breaks the reservation. A single hart
     * cannot race with itself, but its own stores (and a trap handler's)
     * must still invalidate an outstanding LR.
     */
    if (RV_UNLIKELY(h->resv_valid && (addr & ~3u) == h->resv_addr)) {
        h->resv_valid = false;
    }
#endif

    return rv_bus_write(h->bus, addr, size, val);
}
