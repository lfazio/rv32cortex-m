/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_cpu.c - e200z7 state, interrupts, special purpose registers and
 * data access.
 *
 * Everything here is from the e200z759n3 Core Reference Manual, Rev. 2;
 * section and table numbers in the comments are that document's.
 */

#include "ppc/ppc_cpu.h"

#include <string.h>

/*
 * Processor version, table 4: Freescale (0b1000), Zen Z7 (0b010110),
 * version 0b1001 for the e200z759n3. The low half comes from p_pvrin
 * pins the SoC ties, so a model of the core alone leaves it zero.
 */
#define PPC_PVR_E200Z759N3 0x81690000u

/*
 * L1 cache configuration, figures 11-6 and 11-7: Harvard, way
 * partitioning, flush/invalidate by set and way, 32-byte lines,
 * pseudo-round-robin, line locking, error checking, 4 ways, 16 KiB --
 * data and instruction alike except that only the data side has
 * CWPA. Read-only, and read by startup code that sizes its loops from
 * them, which is why they are the manual's values rather than zero.
 */
#define PPC_L1CFG0 0x284D1810u
#define PPC_L1CFG1 0x084D1810u

void ppc_cpu_init(ppc_cpu_t *c, struct emu_bus *bus, uint32_t coreid)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->pir = coreid;
    c->pvr = PPC_PVR_E200Z759N3;
    c->vle = true;

    /*
     * The guest is big-endian and the bus is what has to know. Declared
     * once, here, before anything runs: it is a property of the guest
     * architecture, not of a region or a platform.
     */
    emu_bus_set_big_endian(bus, true);
}

void ppc_cpu_reset(ppc_cpu_t *c, uint32_t reset_pc)
{
    /*
     * Table 17. Most of the state is "unaffected" by a reset on the
     * real core; it is cleared here because a reload is a new guest and
     * nothing the previous one left behind is meant to be visible.
     */
    memset(c->r, 0, sizeof(c->r));
    c->pc = reset_pc;
    c->cr = 0u;
    c->xer = 0u;
    c->lr = 0u;
    c->ctr = 0u;
    c->msr = 0u;
    c->spefscr = 0u;
    c->reserve = false;
    c->esr = 0u;
    c->hid0 = c->hid1 = 0u;
    c->l1csr0 = c->l1csr1 = c->l1finv0 = c->l1finv1 = 0u;
    c->bucsr = 0u;
    c->tcr = 0u;
    c->tsr = 0u;
    c->mcsr = 0u;
    memset(c->dbcr, 0, sizeof(c->dbcr));
    c->dbsr = 0x10000000u; /* table 17 */
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = false;
    c->retired = 0u;
    c->cycles = 0u;
    c->clk_synced = 0u;
    ppc_mmu_reset(c);
    c->jit_ctx = ppc_cpu_ctx(c);
    c->jit_flush = true;
}

uint64_t ppc_cpu_ctx(const ppc_cpu_t *c)
{
    return c->vle ? 1u : 0u;
}

void ppc_cpu_set_msr(ppc_cpu_t *c, uint32_t v)
{
    c->msr = v & PPC_MSR_IMPL;
    c->irq_dirty = true;
    ppc_mmu_refresh(c); /* IS and DS choose the address space */
    c->jit_ctx = ppc_cpu_ctx(c);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static uint32_t handler_address(const ppc_cpu_t *c, ppc_ivor_t which)
{
    return (c->ivpr & 0xFFFF0000u) | (c->ivor[which] & 0x0000FFF0u);
}

void ppc_cpu_raise(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc,
                   uint32_t esr)
{
    uint32_t keep;

    /*
     * Which save/restore pair, and which MSR bits survive, from the
     * register-settings table of each interrupt (7.7). The non-critical
     * class keeps CE, ME, DE and RI and clears the rest; critical input
     * and watchdog clear CE as well, and DE because the debug APU is not
     * enabled in this model; a machine check clears ME and RI too.
     */
    switch (which) {
    case PPC_IVOR_CRITICAL:
    case PPC_IVOR_WATCHDOG:
        c->csrr0 = ret_pc;
        c->csrr1 = c->msr;
        keep = PPC_MSR_ME | PPC_MSR_RI;
        break;
    case PPC_IVOR_MACHINE_CHECK:
        c->mcsrr0 = ret_pc;
        c->mcsrr1 = c->msr;
        keep = 0u;
        break;
    case PPC_IVOR_DEBUG:
        c->dsrr0 = ret_pc;
        c->dsrr1 = c->msr;
        keep = PPC_MSR_ME | PPC_MSR_RI;
        break;
    default:
        c->srr0 = ret_pc;
        c->srr1 = c->msr;
        keep = PPC_MSR_CE | PPC_MSR_ME | PPC_MSR_DE | PPC_MSR_RI;
        break;
    }
    if (esr != PPC_ESR_KEEP) {
        /*
         * Table 7-24: an instruction TLB error sets ESR[MIF] and clears
         * everything else, VLEMI included -- there is no instruction yet
         * to have been a VLE one.
         */
        const bool vlemi = c->vle && which != PPC_IVOR_ITLB_ERROR;

        c->esr = esr | (vlemi ? PPC_ESR_VLEMI : 0u);
    }
    c->msr &= keep;
    c->pc = handler_address(c, which);
    ppc_mmu_refresh(c); /* the handler runs with IS and DS clear */
    c->jit_ctx = ppc_cpu_ctx(c);
}

void ppc_cpu_exception(ppc_cpu_t *c, ppc_ivor_t which, uint32_t ret_pc)
{
    ppc_cpu_raise(c, which, ret_pc, PPC_ESR_KEEP);
}

/* ------------------------------------------------------------------ */
/* Special purpose registers                                           */
/* ------------------------------------------------------------------ */

/* Bit 5 of the split field: 0x10 in the number as software writes it. */
static bool spr_privileged(uint32_t spr)
{
    return (spr & 0x10u) != 0u;
}

/*
 * Where a plain read/write register lives, or NULL. The ones with side
 * effects, the read-only ones and the write-only ones are handled in
 * the two functions below.
 */
static uint32_t *spr_slot(ppc_cpu_t *c, uint32_t spr)
{
    if (spr >= PPC_SPR_IVOR0 && spr <= PPC_SPR_IVOR0 + 15u) {
        return &c->ivor[spr - PPC_SPR_IVOR0];
    }
    if (spr >= PPC_SPR_IVOR32 && spr <= PPC_SPR_IVOR32 + 3u) {
        return &c->ivor[32u + (spr - PPC_SPR_IVOR32)];
    }
    if (spr >= PPC_SPR_SPRG0 && spr <= PPC_SPR_SPRG0 + 7u) {
        return &c->sprg[spr - PPC_SPR_SPRG0];
    }
    if (spr >= PPC_SPR_IAC1 && spr <= PPC_SPR_IAC1 + 3u) {
        return &c->iac[spr - PPC_SPR_IAC1];
    }
    if (spr >= PPC_SPR_IAC5 && spr <= PPC_SPR_IAC5 + 3u) {
        return &c->iac[4u + (spr - PPC_SPR_IAC5)];
    }
    if (spr >= PPC_SPR_DBCR0 && spr <= PPC_SPR_DBCR0 + 2u) {
        return &c->dbcr[spr - PPC_SPR_DBCR0];
    }
    switch (spr) {
    case PPC_SPR_LR:
        return &c->lr;
    case PPC_SPR_CTR:
        return &c->ctr;
    case PPC_SPR_SRR0:
        return &c->srr0;
    case PPC_SPR_SRR1:
        return &c->srr1;
    case PPC_SPR_CSRR0:
        return &c->csrr0;
    case PPC_SPR_CSRR1:
        return &c->csrr1;
    case PPC_SPR_DSRR0:
        return &c->dsrr0;
    case PPC_SPR_DSRR1:
        return &c->dsrr1;
    case PPC_SPR_MCSRR0:
        return &c->mcsrr0;
    case PPC_SPR_MCSRR1:
        return &c->mcsrr1;
    case PPC_SPR_MCAR:
        return &c->mcar;
    case PPC_SPR_DEAR:
        return &c->dear;
    case PPC_SPR_ESR:
        return &c->esr;
    case PPC_SPR_IVPR:
        return &c->ivpr;
    case PPC_SPR_DECAR:
        return &c->decar;
    case PPC_SPR_USPRG0:
        return &c->usprg0;
    case PPC_SPR_SPRG8:
        return &c->sprg[8];
    case PPC_SPR_SPRG9:
        return &c->sprg[9];
    case PPC_SPR_PIR:
        return &c->pir;
    case PPC_SPR_HID1:
        return &c->hid1;
    case PPC_SPR_L1CSR1:
        return &c->l1csr1;
    case PPC_SPR_L1FINV0:
        return &c->l1finv0;
    case PPC_SPR_L1FINV1:
        return &c->l1finv1;
    case PPC_SPR_BUCSR:
        return &c->bucsr;
    case PPC_SPR_DAC1:
        return &c->dac[0];
    case PPC_SPR_DAC1 + 1u:
        return &c->dac[1];
    case PPC_SPR_DVC1:
        return &c->dvc[0];
    case PPC_SPR_DVC1 + 1u:
        return &c->dvc[1];
    case PPC_SPR_DBCR3:
        return &c->dbcr[3];
    case PPC_SPR_DBCR4:
        return &c->dbcr[4];
    case PPC_SPR_DBCR5:
        return &c->dbcr[5];
    case PPC_SPR_DBCR6:
        return &c->dbcr[6];
    case PPC_SPR_DBCNT:
        return &c->dbcnt;
    case PPC_SPR_DDAM:
        return &c->ddam;
    case PPC_SPR_DEVENT:
        return &c->devent;
    default:
        return NULL;
    }
}

uint32_t ppc_spr_read(ppc_cpu_t *c, uint32_t spr, uint32_t *out)
{
    uint32_t *slot;

    /*
     * 2.5.1 and 3.15: a privileged number from user mode is a privilege
     * fault *whether or not the register exists*, so this is decided on
     * the number, before anything looks for the register. Everything
     * below it that does not exist is an illegal instruction.
     */
    if (spr_privileged(spr) && (c->msr & PPC_MSR_PR) != 0u) {
        return PPC_ESR_PPR;
    }
    switch (spr) {
    case PPC_SPR_XER:
        *out = c->xer;
        return PPC_EXC_NONE;
    case PPC_SPR_DEC:
        ppc_cpu_sync_clock(c);
        *out = c->dec;
        return PPC_EXC_NONE;
    case PPC_SPR_TBL_R:
        ppc_cpu_sync_clock(c);
        *out = (uint32_t)c->tb;
        return PPC_EXC_NONE;
    case PPC_SPR_TBU_R:
        ppc_cpu_sync_clock(c);
        *out = (uint32_t)(c->tb >> 32);
        return PPC_EXC_NONE;
    case PPC_SPR_SPRG4_R:
    case PPC_SPR_SPRG4_R + 1u:
    case PPC_SPR_SPRG4_R + 2u:
    case PPC_SPR_SPRG4_R + 3u:
        *out = c->sprg[4u + (spr - PPC_SPR_SPRG4_R)];
        return PPC_EXC_NONE;
    case PPC_SPR_PVR:
        *out = c->pvr;
        return PPC_EXC_NONE;
    case PPC_SPR_SVR:
        *out = c->svr;
        return PPC_EXC_NONE;
    case PPC_SPR_TSR:
        *out = c->tsr;
        return PPC_EXC_NONE;
    case PPC_SPR_TCR:
        *out = c->tcr;
        return PPC_EXC_NONE;
    case PPC_SPR_DBSR:
        *out = c->dbsr;
        return PPC_EXC_NONE;
    case PPC_SPR_MCSR:
        *out = c->mcsr;
        return PPC_EXC_NONE;
    case PPC_SPR_SPEFSCR:
        *out = c->spefscr;
        return PPC_EXC_NONE;
    case PPC_SPR_L1CFG0:
        *out = PPC_L1CFG0;
        return PPC_EXC_NONE;
    case PPC_SPR_L1CFG1:
        *out = PPC_L1CFG1;
        return PPC_EXC_NONE;
    case PPC_SPR_HID0:
        *out = c->hid0;
        return PPC_EXC_NONE;
    case PPC_SPR_L1CSR0:
        *out = c->l1csr0;
        return PPC_EXC_NONE;
    case PPC_SPR_DBERC0:
        *out = 0u; /* no external debug resources */
        return PPC_EXC_NONE;
    /* The MMU (chapter 10). */
    case PPC_SPR_MAS0:
        *out = c->mas0;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS1:
        *out = c->mas1;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS2:
        *out = c->mas2;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS3:
        *out = c->mas3;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS4:
        *out = c->mas4;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS6:
        *out = c->mas6;
        return PPC_EXC_NONE;
    case PPC_SPR_MMUCSR0:
        *out = 0u; /* TLB1_FI reads 0 once the invalidation is done */
        return PPC_EXC_NONE;
    case PPC_SPR_MMUCFG:
        *out = PPC_MMUCFG;
        return PPC_EXC_NONE;
    case PPC_SPR_TLB0CFG:
        *out = PPC_TLB0CFG;
        return PPC_EXC_NONE;
    case PPC_SPR_TLB1CFG:
        *out = PPC_TLB1CFG;
        return PPC_EXC_NONE;
    case PPC_SPR_PID0:
        *out = c->pid0;
        return PPC_EXC_NONE;
    case PPC_SPR_TBL_W:
    case PPC_SPR_TBU_W:
        return PPC_ESR_PIL; /* write-only: "an invalid SPR reference" */
    default:
        break;
    }
    slot = spr_slot(c, spr);
    if (slot == NULL) {
        return PPC_ESR_PIL;
    }
    *out = *slot;
    return PPC_EXC_NONE;
}

uint32_t ppc_spr_write(ppc_cpu_t *c, uint32_t spr, uint32_t v)
{
    uint32_t *slot;

    if (spr_privileged(spr) && (c->msr & PPC_MSR_PR) != 0u) {
        return PPC_ESR_PPR;
    }
    switch (spr) {
    case PPC_SPR_XER:
        c->xer = v & PPC_XER_IMPL;
        return PPC_EXC_NONE;
    /*
     * The timers are brought up to date before they are changed, under
     * the settings that were in force while that time passed, and the
     * run loop is told to look again -- it may be counting down to an
     * expiry that has just moved.
     */
    case PPC_SPR_DEC:
        ppc_cpu_sync_clock(c);
        c->dec = v;
        c->irq_dirty = true;
        return PPC_EXC_NONE;
    case PPC_SPR_TBL_W:
        ppc_cpu_sync_clock(c);
        c->tb = (c->tb & 0xFFFFFFFF00000000ull) | v;
        return PPC_EXC_NONE;
    case PPC_SPR_TBU_W:
        ppc_cpu_sync_clock(c);
        c->tb = (c->tb & 0xFFFFFFFFull) | ((uint64_t)v << 32);
        return PPC_EXC_NONE;
    case PPC_SPR_TSR:
        /* Write one to clear. */
        c->tsr &= ~v;
        c->irq_dirty = true;
        return PPC_EXC_NONE;
    case PPC_SPR_TCR:
        ppc_cpu_sync_clock(c);
        c->tcr = v;
        c->irq_dirty = true;
        return PPC_EXC_NONE;
    case PPC_SPR_DECAR:
        ppc_cpu_sync_clock(c);
        c->decar = v;
        return PPC_EXC_NONE;
    case PPC_SPR_DBSR:
        c->dbsr &= ~v;
        return PPC_EXC_NONE;
    case PPC_SPR_MCSR:
        c->mcsr &= ~v;
        return PPC_EXC_NONE;
    case PPC_SPR_SPEFSCR:
        /* MODE reads back 0: mode 1 is not implemented (table 5-1). */
        c->spefscr = v & PPC_SPEFSCR_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_HID0:
        ppc_cpu_sync_clock(c);
        c->hid0 = v;
        c->irq_dirty = true;
        return PPC_EXC_NONE;
    case PPC_SPR_L1CSR0:
        c->l1csr0 = v;
        return PPC_EXC_NONE;
    /*
     * The MMU's. Writing a MAS register changes nothing until tlbwe;
     * the bits a register does not implement read as zero (tables
     * 10-9 to 10-14).
     */
    case PPC_SPR_MAS0:
        c->mas0 = v & PPC_MAS0_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS1:
        c->mas1 = v & PPC_MAS1_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS2:
        c->mas2 = v & PPC_MAS2_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS3:
        c->mas3 = v & PPC_MAS3_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS4:
        c->mas4 = v & PPC_MAS4_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MAS6:
        c->mas6 = v & PPC_MAS6_IMPL;
        return PPC_EXC_NONE;
    case PPC_SPR_MMUCSR0:
        if ((v & PPC_MMUCSR0_TLB1_FI) != 0u) {
            ppc_mmu_flash_invalidate(c);
        }
        return PPC_EXC_NONE;
    /*
     * 10.2.3: eight bits on this core, the rest read as zero. No refresh:
     * the reset entry is shared (TID 0), so the PID cannot make the
     * reset TLB stop being the identity, and a programmed TLB is looked
     * up afresh on every access.
     */
    case PPC_SPR_PID0:
        c->pid0 = v & 0xFFu;
        return PPC_EXC_NONE;
    /* Read-only: a write is an invalid reference, 3.15. */
    case PPC_SPR_TBL_R:
    case PPC_SPR_TBU_R:
    case PPC_SPR_SPRG4_R:
    case PPC_SPR_SPRG4_R + 1u:
    case PPC_SPR_SPRG4_R + 2u:
    case PPC_SPR_SPRG4_R + 3u:
    case PPC_SPR_PVR:
    case PPC_SPR_SVR:
    case PPC_SPR_L1CFG0:
    case PPC_SPR_L1CFG1:
    case PPC_SPR_DBERC0:
    case PPC_SPR_MMUCFG:
    case PPC_SPR_TLB0CFG:
    case PPC_SPR_TLB1CFG:
        return PPC_ESR_PIL;
    default:
        break;
    }
    slot = spr_slot(c, spr);
    if (slot == NULL) {
        return PPC_ESR_PIL;
    }
    *slot = v;
    return PPC_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* Time base, decrementer and the interrupts they raise                */
/* ------------------------------------------------------------------ */

/*
 * Two clocks, and HID0 says which one the timers are on (table 2-7).
 * Out of reset neither: TBEN is clear and the time base does not move,
 * which is the core's behaviour and the first thing its startup code
 * changes.
 */
static bool on_core_clock(const ppc_cpu_t *c)
{
    return (c->hid0 & (PPC_HID0_TBEN | PPC_HID0_SEL_TBCLK)) == PPC_HID0_TBEN;
}

static bool on_tbclk(const ppc_cpu_t *c)
{
    return (c->hid0 & (PPC_HID0_TBEN | PPC_HID0_SEL_TBCLK)) ==
           (PPC_HID0_TBEN | PPC_HID0_SEL_TBCLK);
}

void ppc_cpu_set_time(ppc_cpu_t *c, uint64_t now)
{
    const uint32_t d = (uint32_t)(now - c->tbclk_last);

    c->tbclk_last = now;
    if (on_tbclk(c)) {
        ppc_cpu_advance(c, d);
    }
}

void ppc_cpu_sync_clock(ppc_cpu_t *c)
{
    const uint64_t n = c->cycles - c->clk_synced;

    c->clk_synced = c->cycles;
    if (n != 0u && on_core_clock(c)) {
        ppc_cpu_advance(c, (uint32_t)n);
    }
}

uint32_t ppc_cpu_clock_until(const ppc_cpu_t *c)
{
    return (on_core_clock(c) && c->dec != 0u) ? c->dec : 0xFFFFFFFFu;
}

void ppc_cpu_advance(ppc_cpu_t *c, uint32_t ticks)
{
    if (ticks == 0u) {
        return;
    }
    c->tb += ticks;

    /*
     * The decrementer, and the rule that matters is the **transition**
     * through zero rather than the value at it.
     *
     * A slice is thousands of ticks, so a decrementer loaded with ten
     * is stepped far past zero in one call: testing `dec == 0` would
     * miss it entirely, and testing `dec <= ticks` without consuming
     * the remainder would raise again on the next slice. Both are the
     * ordinary case here, not corners -- guest time never arrives one
     * tick at a time.
     */
    if (c->dec != 0u) {
        if (c->dec > ticks) {
            c->dec -= ticks;
        } else {
            const uint32_t past = ticks - c->dec;

            c->tsr |= PPC_TSR_DIS;
            if ((c->tcr & PPC_TCR_ARE) != 0u && c->decar != 0u) {
                /*
                 * Auto-reload. The modulo keeps a long slice from
                 * leaving DEC above DECAR, which a plain reload would:
                 * the counter is meant to be periodic and one slice may
                 * span several periods.
                 */
                c->dec = c->decar - (past % c->decar);
            } else {
                c->dec = 0u; /* stopped until software reloads it */
            }
            c->irq_dirty = true;
        }
    }
}

void ppc_cpu_set_ext(ppc_cpu_t *c, bool level)
{
    c->ext_pending = level;
    if (level) {
        c->irq_dirty = true;
    }
}

int ppc_cpu_pending_irq(const ppc_cpu_t *c)
{
    if ((c->msr & PPC_MSR_EE) == 0u) {
        return -1;
    }
    /*
     * External first. Book E does not order them, and a real part has a
     * controller deciding; the choice here is that a device asking for
     * attention outranks a periodic tick, which is what a controller
     * with default priorities would do.
     */
    if (c->ext_pending) {
        return (int)PPC_IVOR_EXTERNAL;
    }
    /*
     * The decrementer interrupt needs TSR[DIS] *and* TCR[DIE]. DIS is
     * set by the hardware whether or not DIE is, so a guest polling TSR
     * with interrupts off still sees the tick -- which is what DIE
     * gating the interrupt rather than the flag means.
     */
    if ((c->tsr & PPC_TSR_DIS) != 0u && (c->tcr & PPC_TCR_DIE) != 0u) {
        return (int)PPC_IVOR_DECREMENTER;
    }
    return -1;
}

bool ppc_cpu_wake(ppc_cpu_t *c)
{
    /*
     * The processor clock does not stop for a wait, and here it only
     * moves when instructions do -- so a core waiting on its own
     * decrementer is carried straight to the expiry. Without this it
     * would wait for ever on a clock that waits for it.
     */
    if (ppc_cpu_pending_irq(c) < 0) {
        uint32_t until;

        ppc_cpu_sync_clock(c);
        until = ppc_cpu_clock_until(c);
        if (until != 0xFFFFFFFFu) {
            c->cycles += until;
            ppc_cpu_sync_clock(c);
        }
    }
    if (ppc_cpu_pending_irq(c) < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = true;
    return true;
}

bool ppc_cpu_take_irq(ppc_cpu_t *c)
{
    const int which = ppc_cpu_pending_irq(c);

    if (which < 0) {
        return false;
    }
    /*
     * The external input is edge-like here because there is no
     * interrupt controller to hold it: taking the interrupt consumes
     * it. TSR[DIS] is *not* cleared -- it is write-1-to-clear by the
     * handler, which is how a guest distinguishes "I took a tick" from
     * "a tick is pending".
     */
    if (which == (int)PPC_IVOR_EXTERNAL) {
        c->ext_pending = false;
        if ((c->hid0 & PPC_HID0_ICR) != 0u) {
            c->reserve = false; /* 3.5, mechanism 3 */
        }
    }
    c->state = EMU_STATE_RUNNING;
    ppc_cpu_raise(c, (ppc_ivor_t)which, c->pc, PPC_ESR_KEEP);
    return true;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * The two accesses under a programmed TLB, out of line: the flat path
 * pays one test of mmu_on and nothing else.
 *
 * Byte by byte, each byte translated, because an unaligned access may
 * cross into a page mapped elsewhere or not at all -- and the manual
 * sets DEAR to "the effective address of a byte" of the faulting access,
 * which is the first byte that faults. An aligned access cannot cross a
 * page (the smallest is 1 KB), so it translates once.
 */
static EMU_COLD_NOINLINE ppc_exc_t load_xlate(ppc_cpu_t *c, uint32_t addr,
                                              uint32_t size, uint32_t *out)
{
    uint32_t v = 0u;

    if ((addr & (size - 1u)) == 0u) {
        uint32_t pa;
        const ppc_exc_t x = ppc_mmu_xlate(c, addr, PPC_ACC_READ, &pa, NULL);

        if (x != PPC_EXC_NONE) {
            return x;
        }
        if (emu_bus_read_be(c->bus, pa, size, &v) != EMU_FAULT_NONE) {
            c->dear = addr;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
        *out = v;
        return PPC_EXC_NONE;
    }
    for (uint32_t i = 0u; i < size; i++) {
        uint32_t pa;
        uint32_t b = 0u;
        const ppc_exc_t x = ppc_mmu_xlate(c, addr + i, PPC_ACC_READ, &pa, NULL);

        if (x != PPC_EXC_NONE) {
            return x;
        }
        if (emu_bus_read_be(c->bus, pa, 1u, &b) != EMU_FAULT_NONE) {
            c->dear = addr + i;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
        v = (v << 8) | (b & 0xFFu);
    }
    *out = v;
    return PPC_EXC_NONE;
}

static EMU_COLD_NOINLINE ppc_exc_t store_xlate(ppc_cpu_t *c, uint32_t addr,
                                               uint32_t size, uint32_t val)
{
    if ((addr & (size - 1u)) == 0u) {
        uint32_t pa;
        const ppc_exc_t x = ppc_mmu_xlate(c, addr, PPC_ACC_WRITE, &pa, NULL);

        if (x != PPC_EXC_NONE) {
            return x;
        }
        if (emu_bus_write_be(c->bus, pa, size, val) != EMU_FAULT_NONE) {
            c->dear = addr;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
        return PPC_EXC_NONE;
    }
    /*
     * Every byte translated before any is written, so a store that
     * faults half way leaves memory as it was -- the rule this tree
     * holds RV32's accesses to, and the one the interrupt's "the
     * instruction did not happen" implies.
     */
    uint32_t pa[4];

    for (uint32_t i = 0u; i < size; i++) {
        const ppc_exc_t x =
            ppc_mmu_xlate(c, addr + i, PPC_ACC_WRITE, &pa[i], NULL);

        if (x != PPC_EXC_NONE) {
            return x;
        }
    }
    for (uint32_t i = 0u; i < size; i++) {
        const uint32_t b = (val >> (8u * (size - 1u - i))) & 0xFFu;

        if (emu_bus_write_be(c->bus, pa[i], 1u, b) != EMU_FAULT_NONE) {
            c->dear = addr + i;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
    }
    return PPC_EXC_NONE;
}

ppc_exc_t ppc_load(ppc_cpu_t *c, uint32_t addr, uint32_t size, bool sext,
                   uint32_t *out)
{
    uint32_t v = 0u;

    if (EMU_UNLIKELY(c->mmu_on)) {
        const ppc_exc_t x = load_xlate(c, addr, size, &v);

        if (x != PPC_EXC_NONE) {
            return x;
        }
    } else if (EMU_LIKELY((addr & (size - 1u)) == 0u)) {
        if (EMU_UNLIKELY(emu_bus_read_be(c->bus, addr, size, &v) != EMU_FAULT_NONE)) {
            c->dear = addr;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
    } else {
        /*
         * Unaligned, which the core performs (3.4). Byte by byte, most
         * significant first, because that is what big-endian means and
         * a byte access is the one width with no order to get wrong.
         */
        for (uint32_t i = 0u; i < size; i++) {
            uint32_t b = 0u;

            if (emu_bus_read_be(c->bus, addr + i, 1u, &b) != EMU_FAULT_NONE) {
                c->dear = addr + i;
                return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
            }
            v = (v << 8) | (b & 0xFFu);
        }
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
    if (EMU_UNLIKELY(c->mmu_on)) {
        return store_xlate(c, addr, size, val);
    }
    if (EMU_LIKELY((addr & (size - 1u)) == 0u)) {
        if (EMU_UNLIKELY(emu_bus_write_be(c->bus, addr, size, val) != EMU_FAULT_NONE)) {
            c->dear = addr;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
        return PPC_EXC_NONE;
    }
    for (uint32_t i = 0u; i < size; i++) {
        const uint32_t b = (val >> (8u * (size - 1u - i))) & 0xFFu;

        if (emu_bus_write_be(c->bus, addr + i, 1u, b) != EMU_FAULT_NONE) {
            c->dear = addr + i;
            return (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
        }
    }
    return PPC_EXC_NONE;
}
