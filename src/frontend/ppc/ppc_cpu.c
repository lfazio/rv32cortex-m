/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_cpu.c - e200z7 state, exceptions and checked memory access.
 */

#include "ppc/ppc_cpu.h"

#include <string.h>

/* e200z7 processor version. Reported by mfspr PVR, which is how a guest
 * identifies its core; zero would be a claim to be nothing in particular. */
#define PPC_PVR_E200Z7      0x81560000u

void ppc_cpu_init(ppc_cpu_t *c, struct emu_bus *bus, uint32_t coreid)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->pir = coreid;
    c->pvr = PPC_PVR_E200Z7;

    /*
     * The guest is big-endian and the bus is what has to know. Declared
     * once, here, before anything runs: it is a property of the guest
     * architecture, not of a region or a platform.
     */
    emu_bus_set_big_endian(bus, true);
}

void ppc_cpu_reset(ppc_cpu_t *c, uint32_t reset_pc)
{
    memset(c->r, 0, sizeof(c->r));
    c->pc = reset_pc;
    c->cr = 0u;
    c->xer = 0u;
    c->lr = 0u;
    c->ctr = 0u;
    /*
     * Reset clears MSR entirely: interrupts disabled, supervisor state.
     * A guest enables what it wants. Notably MSR[SPE] is clear, so the
     * SPE unit is unavailable until asked for -- which is the correct
     * report for a unit this frontend does not implement.
     */
    c->msr = 0u;
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = false;
    c->retired = 0u;
    c->cycles = 0u;
}

/*
 * Where a handler lives.
 *
 * IVPR[0:15] || IVORn[16:27] || 0b0000. Two registers, both writable by
 * the guest, and both zero out of reset -- so a guest that takes an
 * interrupt before setting them vectors to 0. That is the architecture's
 * behaviour and not a bug to paper over; it is worth knowing because it
 * looks exactly like a wild branch.
 */
static uint32_t handler_address(const ppc_cpu_t *c, ppc_ivor_t which)
{
    return (c->ivpr & 0xFFFF0000u) | (c->ivor[which] & 0x0000FFF0u);
}

void ppc_cpu_exception(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc)
{
    /*
     * Critical interrupts save to CSRR0/CSRR1 and everything else to
     * SRR0/SRR1. That is the whole of what separates the two levels, and
     * it exists so a critical interrupt inside a normal handler does not
     * destroy the return state the handler is standing on -- the same
     * argument as G4MH's FE level.
     */
    const bool crit = (which == PPC_IVOR_CRITICAL ||
                       which == PPC_IVOR_MACHINE_CHECK ||
                       which == PPC_IVOR_WATCHDOG ||
                       which == PPC_IVOR_DEBUG);

    if (crit) {
        c->csrr0 = ret_pc;
        c->csrr1 = c->msr;
    } else {
        c->srr0 = ret_pc;
        c->srr1 = c->msr;
    }

    /*
     * Entry clears EE and PR: interrupts off and supervisor state. CE is
     * cleared only by a critical interrupt, which is what lets a normal
     * handler still be interrupted by one.
     */
    c->msr &= ~(uint32_t)(PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_DE);
    if (crit) {
        c->msr &= ~(uint32_t)PPC_MSR_CE;
    }
    c->pc = handler_address(c, which);
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * Book E takes an alignment interrupt on a misaligned access. Checked
 * here rather than in the bus so the cause is the architecture's, and so
 * the bus keeps its assumption that callers are aligned.
 */
static EMU_ALWAYS_INLINE bool aligned(uint32_t addr, uint32_t size)
{
    return (addr & (size - 1u)) == 0u;
}

static ppc_exc_t exc_from_fault(emu_fault_t f, bool store)
{
    if (f == EMU_FAULT_NONE) {
        return PPC_EXC_NONE;
    }
    (void)store;
    /* Both directions are a data storage interrupt on this core; what
     * distinguishes them is ESR, which the caller sets. */
    return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
}

ppc_exc_t ppc_load(ppc_cpu_t *c, uint32_t addr, uint32_t size, bool sext,
                   uint32_t *out)
{
    if (EMU_UNLIKELY(!aligned(addr, size))) {
        c->dear = addr;
        return (ppc_exc_t)PPC_IVOR_ALIGNMENT;
    }

    uint32_t v = 0u;
    const emu_fault_t f = emu_bus_read(c->bus, addr, size, &v);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->dear = addr;
        c->esr = 0u;
        return exc_from_fault(f, false);
    }

    if (sext) {
        v = (size == 1u) ? (uint32_t)(int32_t)(int8_t)v
                         : (uint32_t)(int32_t)(int16_t)v;
    }
    *out = v;
    return PPC_EXC_NONE;
}

ppc_exc_t ppc_store(ppc_cpu_t *c, uint32_t addr, uint32_t size, uint32_t val)
{
    if (EMU_UNLIKELY(!aligned(addr, size))) {
        c->dear = addr;
        return (ppc_exc_t)PPC_IVOR_ALIGNMENT;
    }

    const emu_fault_t f = emu_bus_write(c->bus, addr, size, val);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->dear = addr;
        c->esr = 0x00800000u;       /* ESR[ST]: the access was a store */
        return exc_from_fault(f, true);
    }
    return PPC_EXC_NONE;
}
