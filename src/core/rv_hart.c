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

void rv_hart_trap(rv_hart_t *h, uint32_t cause, uint32_t tval)
{
    h->mepc = h->pc;
    h->mcause = cause;
    h->mtval = tval;

    /*
     * Push the interrupt-enable stack: MPIE takes the old MIE, MIE clears,
     * and MPP records the privilege we came from. With M-mode only, MPP is
     * always M.
     */
    const uint32_t mie_was = (h->mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0u;
    h->mstatus = (h->mstatus & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK))
               | mie_was
               | ((uint32_t)RV_PRIV_M << MSTATUS_MPP_SHIFT);

    uint32_t base = h->mtvec & ~MTVEC_MODE_MASK;
    if ((h->mtvec & MTVEC_MODE_MASK) == MTVEC_MODE_VECTORED &&
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
    /* Globally masked: nothing can be taken, even if pending and enabled. */
    if ((h->mstatus & MSTATUS_MIE) == 0u) {
        return RV_EXC_NONE;
    }

    const uint32_t active = h->mip & h->mie;
    if (active == 0u) {
        return RV_EXC_NONE;
    }

    /* Priority order from the privileged spec: MEI, MSI, MTI. */
    if (active & MIP_MEIP) {
        return RV_INT_M_EXT;
    }
    if (active & MIP_MSIP) {
        return RV_INT_M_SOFT;
    }
    return RV_INT_M_TIMER;
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

rv_exc_t rv_hart_store(rv_hart_t *h, uint32_t addr, uint32_t size, uint32_t val)
{
#if !RV_MISALIGNED_OK
    if (RV_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_STORE_MISALIGNED;
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
