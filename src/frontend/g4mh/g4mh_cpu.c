/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_cpu.c - Core lifecycle, exception entry, and checked memory.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_memmap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void g4mh_cpu_init(g4mh_cpu_t *c, emu_bus_t *bus, uint32_t coreid)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->coreid = coreid;
    g4mh_cpu_reset(c, 0u);
}

void g4mh_cpu_reset(g4mh_cpu_t *c, uint32_t reset_pc)
{
    memset(c->r, 0, sizeof(c->r));
    memset(c->sr, 0, sizeof(c->sr));
    c->pc = reset_pc;

    /*
     * Reset state per the G4MH manual: interrupts disabled, no exception
     * in progress, machine mode. PSW.ID set is the important half -- a
     * core coming out of reset must not take an interrupt before its
     * handler table exists.
     */
    c->psw = G4MH_PSW_ID;
    c->sr[0][G4MH_SR_PSW] = c->psw;

    /*
     * RBASE is where the reset vector and the exception handlers live.
     * The architecture leaves the reset value to the part; taking it from
     * the reset pc means a guest that never writes RBASE still has its
     * handlers at a defined place -- the top of its own image -- rather
     * than at address zero.
     */
    c->sr[1][G4MH_SR_RBASE] = reset_pc;
    c->sr[1][G4MH_SR_EBASE] = reset_pc;

    /*
     * The PE number, which is what real startup code branches on: every PE
     * comes out of reset at the same address running the same image, so
     * without this a multicore guest cannot tell the cores apart.
     *
     * PE n reports n. The U2B manual §3.2.2.10 has a program identify its
     * core by reading PEID, "a unique number within a multi-processor
     * system", and Table 3.1 numbers the CPUs from 0 -- so the coreid goes
     * in unmodified rather than being placed in a field.
     */
    c->sr[2][G4MH_SR_HTCFG0] = c->coreid;

    g4mh_ll_drop(c);
    c->cycles = 0u;
    c->retired = 0u;
    c->state = EMU_STATE_RUNNING;
    /* Force one evaluation after reset rather than assuming the state. */
    c->irq_dirty = true;

#if EMU_ENABLE_STATS
    c->exc_count = 0u;
#endif
}

void g4mh_cpu_boot(g4mh_cpu_t *c, uint32_t ram_base, uint32_t ram_size)
{
    /* The RH850 ABI wants the stack 4-byte aligned; 8 costs nothing and
     * satisfies any guest that assumes more. */
    c->r[G4MH_REG_SP] = (ram_base + ram_size) & ~7u;
    c->r[6] = c->coreid;
    c->r[7] = ram_size;
}

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

bool g4mh_exc_is_fe(g4mh_exc_t cause)
{
    /*
     * FE-level causes are the ones the core cannot be asked to ignore:
     * a system error, a protection violation, a reserved instruction, a
     * misaligned access, an FP fault, an unusable coprocessor. Everything
     * else -- TRAP, SYSCALL and the interrupts -- is EI level.
     */
    switch (cause) {
    case G4MH_EXC_SYSERR:
    case G4MH_EXC_MIP:
    case G4MH_EXC_MDP:
    case G4MH_EXC_RIE:
    case G4MH_EXC_MAE:
    case G4MH_EXC_FPP:
    case G4MH_EXC_UCPOP:
        return true;
    default:
        /* FETRAP is the one *software-raised* FE-level cause: that is the
         * whole point of it beside TRAP, which is EI level. */
        return cause >= G4MH_EXC_FETRAP &&
               cause < G4MH_EXC_FETRAP + 0x10u;
    }
}

/*
 * Where a handler lives.
 *
 * RBASE normally, EBASE when PSW.EBV is set -- which is the mechanism a
 * system uses to move its vector table after boot without relocating the
 * reset vector. The offset within the table is 0x10 per exception class
 * for the synchronous causes and a single entry for all EI interrupts,
 * which is the "reduced" vector layout G4MH uses when INTCFG selects it.
 */
static uint32_t handler_address(const g4mh_cpu_t *c, g4mh_exc_t cause)
{
    const uint32_t base = (c->psw & G4MH_PSW_EBV)
                        ? c->sr[1][G4MH_SR_EBASE]
                        : c->sr[1][G4MH_SR_RBASE];

    /* RBASE/EBASE carry flags in the low bits; the table is 512-aligned. */
    const uint32_t table = base & ~0x1FFu;

    if (cause >= G4MH_EXC_EIINT_BASE && cause < G4MH_EXC_SYSCALL) {
        return table + 0x0100u;         /* all EI interrupts             */
    }
    if (cause >= G4MH_EXC_TRAP0 && cause < G4MH_EXC_TRAP0 + 0x10u) {
        return table + 0x0040u;         /* TRAP 0..15                    */
    }
    if (cause >= G4MH_EXC_TRAP1 && cause < G4MH_EXC_TRAP1 + 0x10u) {
        return table + 0x0050u;         /* TRAP 16..31                   */
    }
    if (cause >= G4MH_EXC_FETRAP && cause < G4MH_EXC_FETRAP + 0x10u) {
        return table + 0x0030u;         /* FETRAP 1..15                  */
    }
    switch (cause) {
    case G4MH_EXC_SYSERR: return table + 0x0010u;
    case G4MH_EXC_MIP:    return table + 0x0030u;
    case G4MH_EXC_MDP:    return table + 0x0030u;
    case G4MH_EXC_RIE:    return table + 0x0060u;
    case G4MH_EXC_MAE:    return table + 0x0060u;
    case G4MH_EXC_FPP:    return table + 0x0070u;
    case G4MH_EXC_UCPOP:  return table + 0x0080u;
    default:              return table + 0x0090u;   /* SYSCALL and rest  */
    }
}

void g4mh_cpu_exception(g4mh_cpu_t *c, g4mh_exc_t cause, uint32_t ret_pc)
{
    const bool fe = g4mh_exc_is_fe(cause);

    if (fe) {
        c->sr[0][G4MH_SR_FEPC]  = ret_pc;
        c->sr[0][G4MH_SR_FEPSW] = c->psw;
        c->sr[0][G4MH_SR_FEIC]  = cause;
        /*
         * NP is what makes an FE exception non-reentrant: with it set, a
         * second FE exception is a system error rather than a silent
         * overwrite of FEPC. EP marks that this is an exception rather
         * than an interrupt, which is what RETI reads to know which pair
         * of save registers to restore from.
         */
        c->psw |= G4MH_PSW_NP | G4MH_PSW_EP | G4MH_PSW_ID;
    } else {
        c->sr[0][G4MH_SR_EIPC]  = ret_pc;
        c->sr[0][G4MH_SR_EIPSW] = c->psw;
        c->sr[0][G4MH_SR_EIIC]  = cause;
        c->psw |= G4MH_PSW_ID;
        /*
         * EP distinguishes a synchronous exception from an interrupt at
         * the same level. TRAP sets it; an EI interrupt does not, and
         * that is the only difference in how the two are entered.
         */
        if (cause < G4MH_EXC_EIINT_BASE || cause >= G4MH_EXC_SYSCALL) {
            c->psw |= G4MH_PSW_EP;
        } else {
            c->psw &= ~G4MH_PSW_EP;
        }
    }

    c->sr[0][G4MH_SR_PSW] = c->psw;
    c->pc = handler_address(c, cause);
    c->irq_dirty = true;

    /*
     * An exception breaks any outstanding reservation. The architecture
     * permits keeping it, but dropping it is simpler to reason about and
     * guarantees forward progress cannot depend on a handler leaving it
     * intact -- the same call the RISC-V side makes for LR/SC.
     */
    g4mh_ll_drop(c);

#if EMU_ENABLE_STATS
    c->exc_count++;
#endif
}

int g4mh_cpu_pending_irq(const g4mh_cpu_t *c)
{
    /*
     * PSW.ID masks EI interrupts and PSW.NP masks them during an FE
     * exception. Either alone is enough to refuse delivery, and testing
     * them together costs one AND.
     */
    if ((c->psw & (G4MH_PSW_ID | G4MH_PSW_NP)) != 0u) {
        return -1;
    }
    if (c->intc == NULL) {
        return -1;
    }
    return g4mh_intc_pending(c->intc, c->coreid);
}

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

/*
 * Every core, so a store can break the others' reservations. The only
 * cross-core mutable state in the model -- see docs/host/g4mh/multicore.md;
 * if a second one ever appears, that is the moment threading stops being
 * a locking exercise and becomes a redesign.
 */
static g4mh_cpu_t *g_cores[G4MH_PE_COUNT];
static unsigned    g_core_count;

/*
 * How many cores currently hold one. Kept as a count rather than derived
 * by walking, because g4mh_ll_break is called from the store path and the
 * common answer is "none" -- one predictable branch for a guest that never
 * takes a reservation, which is most of them.
 */
static unsigned g_ll_held;

void g4mh_ll_register(g4mh_cpu_t *c)
{
    if (g_core_count < G4MH_PE_COUNT) {
        g_cores[g_core_count++] = c;
    }
}

bool g4mh_any_reservation(void)
{
    return g_ll_held != 0u;
}

void g4mh_ll_break(uint32_t addr)
{
    const uint32_t word = addr & ~3u;

    for (unsigned i = 0; i < g_core_count; i++) {
        g4mh_cpu_t *c = g_cores[i];
        if (c->ll_valid && c->ll_addr == word) {
            c->ll_valid = false;
            g_ll_held--;
        }
    }
}

/* Take a reservation on `addr` for `c`, replacing any it already held. */
void g4mh_ll_take(g4mh_cpu_t *c, uint32_t addr)
{
    if (!c->ll_valid) {
        g_ll_held++;
    }
    c->ll_valid = true;
    c->ll_addr = addr & ~3u;
}

/* Drop `c`'s reservation, if it has one. */
void g4mh_ll_drop(g4mh_cpu_t *c)
{
    if (c->ll_valid) {
        c->ll_valid = false;
        g_ll_held--;
    }
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

g4mh_exc_t g4mh_load(g4mh_cpu_t *c, uint32_t addr, uint32_t size,
                     bool sign_extend, uint32_t *out)
{
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }

    uint32_t v;
    const emu_fault_t f = emu_bus_read(c->bus, addr, size, &v);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_exc_from_fault(f);
    }

    if (sign_extend && size < 4u) {
        v = (uint32_t)emu_sext(v, size * 8u);
    }
    *out = v;
    return G4MH_EXC_NONE;
}

g4mh_exc_t g4mh_store(g4mh_cpu_t *c, uint32_t addr, uint32_t size,
                      uint32_t val)
{
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }

    const emu_fault_t f = emu_bus_write(c->bus, addr, size, val);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_exc_from_fault(f);
    }

    /*
     * Any store, from any core, breaks a reservation covering the word.
     * Gated on the global count so this is one predictable branch when no
     * core is in an LL/SC sequence, which is the overwhelmingly common
     * case and the reason the count exists at all.
     */
    if (EMU_UNLIKELY(g4mh_any_reservation())) {
        g4mh_ll_break(addr);
    }
    return G4MH_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* System registers                                                    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Performance measurement                                             */
/* ------------------------------------------------------------------ */

/*
 * Advance the enabled performance channels.
 *
 * Eight channels, each a PMCTRLn saying whether it counts and what, and a
 * PMCOUNTn holding the total. Only two events are sourced, and that is
 * deliberate: an emulator can honestly report instructions retired and
 * its own notion of cycles, and everything else on a real part -- cache
 * misses, branch mispredictions, stall cycles -- has no counterpart here.
 * Reporting zero for those is better than reporting a plausible number,
 * because a guest tuning against a fabricated miss rate would be tuning
 * against nothing.
 *
 * Called once per retired instruction from the interpreter, and only when
 * `pm_active` says some channel is enabled -- a loop over eight channels
 * on every instruction is exactly the per-instruction cost the contract
 * forbids, and the overwhelmingly common case is that none is on.
 */
void g4mh_pm_tick(g4mh_cpu_t *c, uint32_t insns)
{
    for (unsigned n = 0; n < G4MH_PM_CHANNELS; n++) {
        const uint32_t ctl = c->sr[G4MH_SELID_PM][G4MH_SR_PMCTRL0 + n];

        if ((ctl & G4MH_PMCTRL_CE) == 0u) {
            continue;
        }
        switch ((ctl >> G4MH_PMCTRL_CND_SH) & G4MH_PMCTRL_CND_MSK) {
        case G4MH_PM_CND_CYCLE:
        case G4MH_PM_CND_INSN:
            /*
             * One cycle per instruction is what this interpreter's own
             * `cycles` counter already assumes, so the two events give
             * the same number here rather than a fabricated difference.
             */
            c->sr[G4MH_SELID_PM][G4MH_SR_PMCOUNT0 + n] += insns;
            break;
        default:
            /* An event this emulator cannot source. Left alone. */
            break;
        }
    }
}

/* True if any channel is enabled. Maintained on write, not recomputed. */
static void pm_refresh(g4mh_cpu_t *c)
{
    bool on = false;

    for (unsigned n = 0; n < G4MH_PM_CHANNELS; n++) {
        if ((c->sr[G4MH_SELID_PM][G4MH_SR_PMCTRL0 + n] &
             G4MH_PMCTRL_CE) != 0u) {
            on = true;
            break;
        }
    }
    c->pm_active = on;
}

uint32_t g4mh_sr_read(const g4mh_cpu_t *c, unsigned bank, unsigned reg)
{
    if (bank >= G4MH_SR_BANKS || reg >= G4MH_SR_PER_BANK) {
        return 0u;   /* reserved: reads as zero, no exception */
    }
    if (bank == 0u && reg == G4MH_SR_PSW) {
        return c->psw;
    }
    return c->sr[bank][reg];
}

void g4mh_sr_write(g4mh_cpu_t *c, unsigned bank, unsigned reg, uint32_t val)
{
    if (bank >= G4MH_SR_BANKS || reg >= G4MH_SR_PER_BANK) {
        return;      /* reserved: writes dropped */
    }

    if (bank == 0u && reg == G4MH_SR_PSW) {
        /*
         * PSW is the one system register with a hot shadow, because the
         * interpreter reads its flags on every conditional and reading
         * them out of the array would cost an extra indexing step per
         * branch. Writing it can unmask interrupts, so it dirties.
         */
        c->psw = val;
        c->sr[0][G4MH_SR_PSW] = val;
        c->irq_dirty = true;
        return;
    }

    c->sr[bank][reg] = val;

    /*
     * Enabling a channel is the only thing that can turn counting on, so
     * the flag is maintained here rather than tested per instruction.
     * Same shape as the RV32 side's fetch_guard: the check that costs is
     * the one on the hot path, so keep it to a single load of a flag
     * whose writers are few and known.
     */
    if (bank == G4MH_SELID_PM && reg < G4MH_PM_CHANNELS) {
        pm_refresh(c);
    }
}
