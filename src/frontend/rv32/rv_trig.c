/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_trig.c - Sdtrig debug triggers (mcontrol, type 2).
 *
 * Enough of the trigger module for software to set an address watchpoint on
 * an instruction fetch, a load or a store and take a breakpoint exception
 * when it matches. That is what a debugger uses it for and what
 * rv32mi/breakpoint exercises.
 *
 * As with PMP, the cost is gated: rv_hart_t::trig_active is false until
 * software arms a trigger, and while it is false neither the access path
 * nor the fetch path calls into here at all.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#if RV_EXT_SDTRIG

/* mcontrol fields, RV32. */
#define MC_TYPE_SHIFT 28
#define MC_TYPE_MCONTROL 2u
#define MC_DMODE (1u << 27)
#define MC_ACTION_MASK (0xFu << 12)
#define MC_MATCH_MASK (0xFu << 7)
#define MC_M (1u << 6)
#define MC_S (1u << 4)
#define MC_U (1u << 3)
#define MC_EXECUTE (1u << 2)
#define MC_STORE (1u << 1)
#define MC_LOAD (1u << 0)

/*
 * The bits software may set. Everything else is read-only:
 *
 *   type      always 2, this being the only trigger kind implemented
 *   dmode     0, there being no external debugger to reserve triggers
 *   maskmax   0, only exact address matches are supported
 *   action    0, meaning raise a breakpoint exception rather than enter
 *             debug mode, which is the only action available without a
 *             debug module
 *   match     0, equality
 */
#define MC_WMASK (MC_M | MC_S | MC_U | MC_EXECUTE | MC_STORE | MC_LOAD)

void rv_trig_write_tdata1(rv_hart_t *h, uint32_t val)
{
    const uint32_t i = h->tselect;
    if (i >= RV_TRIG_COUNT) {
        return;
    }
    /*
     * A write selecting any type other than mcontrol disables the trigger,
     * which is how the spec says an implementation reports that it does not
     * support the requested kind: the type field reads back as what was
     * actually installed.
     */
    const uint32_t type = val >> MC_TYPE_SHIFT;
    if (type != MC_TYPE_MCONTROL) {
        h->tdata1[i] = 0u;
    } else {
        h->tdata1[i] =
            ((uint32_t)MC_TYPE_MCONTROL << MC_TYPE_SHIFT) | (val & MC_WMASK);
    }
    rv_trig_refresh(h);
}

void rv_trig_refresh(rv_hart_t *h)
{
    h->trig_active = false;
    for (uint32_t i = 0; i < RV_TRIG_COUNT; i++) {
        /* Armed means mcontrol, enabled in some mode, and watching
         * something. */
        if ((h->tdata1[i] >> MC_TYPE_SHIFT) == MC_TYPE_MCONTROL &&
            (h->tdata1[i] & (MC_M | MC_S | MC_U)) != 0u &&
            (h->tdata1[i] & (MC_EXECUTE | MC_STORE | MC_LOAD)) != 0u) {
            h->trig_active = true;
            break;
        }
    }
    rv_hart_refresh_fetch_guard(h);
}

/*
 * m, s and u say which modes a trigger matches in -- the mode the hart is
 * running in, not the one MPRV lends a load. Only m was read, from when
 * M was the only mode: a trigger set for S or U never fired, and one set
 * for M alone fired in all three. tests/arch-test/probes/sstvala.S's
 * case 23 is an S-only load trigger, held to Spike.
 */
static uint32_t mode_bit(uint32_t priv)
{
    switch (priv) {
    case RV_PRIV_M:
        return MC_M;
#if RV_EXT_S
    case RV_PRIV_S:
        return MC_S;
#endif
    default:
        return MC_U;
    }
}

/*
 * Reentrancy (debug specification 1.0, 5.4; docs/riscv). A trigger
 * whose action is a breakpoint -- every trigger here -- can fire inside
 * the handler of its own breakpoint and overwrite the cause and epc it
 * was entered with. The specification offers two cures and a hart
 * "should implement one". The second, tcontrol.mte and mpte, needs
 * medeleg[3] hard-wired to 0, which a core delegating breakpoints to S
 * cannot have. This is the first: no match in M-mode while mstatus.MIE
 * is 0, nor in S-mode while breakpoints are delegated and sstatus.SIE is
 * 0 -- which is what a handler has on entry. U-mode is unaffected.
 * Spike does the same; tests/arch-test/probes/sdtrig.S holds the two.
 */
static bool may_match(const rv_hart_t *h)
{
    switch (h->priv) {
    case RV_PRIV_M:
        return (h->mstatus & MSTATUS_MIE) != 0u;
#if RV_EXT_S
    case RV_PRIV_S:
        return ((h->medeleg >> RV_EXC_BREAKPOINT) & 1u) == 0u ||
               (h->mstatus & MSTATUS_SIE) != 0u;
#endif
    default:
        return true;
    }
}

bool rv_trig_check(const rv_hart_t *h, uint32_t addr, emu_access_t acc)
{
    uint32_t want;

    if (!may_match(h)) {
        return false;
    }
    switch (acc) {
    case EMU_ACC_FETCH:
        want = MC_EXECUTE;
        break;
    case EMU_ACC_LOAD:
        want = MC_LOAD;
        break;
    default:
        want = MC_STORE;
        break;
    }

    for (uint32_t i = 0; i < RV_TRIG_COUNT; i++) {
        const uint32_t d = h->tdata1[i];

        if ((d >> MC_TYPE_SHIFT) != MC_TYPE_MCONTROL) {
            continue;
        }
        if ((d & mode_bit(h->priv)) == 0u || (d & want) == 0u) {
            continue;
        }
        /* match == 0: the address must equal tdata2 exactly. */
        if (h->tdata2[i] == addr) {
            return true;
        }
    }
    return false;
}

#endif /* RV_EXT_SDTRIG */
