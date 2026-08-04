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
#define MC_TYPE_SHIFT   28
#define MC_TYPE_MCONTROL 2u
#define MC_DMODE        (1u << 27)
#define MC_ACTION_MASK  (0xFu << 12)
#define MC_MATCH_MASK   (0xFu << 7)
#define MC_M            (1u << 6)
#define MC_S            (1u << 4)
#define MC_U            (1u << 3)
#define MC_EXECUTE      (1u << 2)
#define MC_STORE        (1u << 1)
#define MC_LOAD         (1u << 0)

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
        h->tdata1[i] = ((uint32_t)MC_TYPE_MCONTROL << MC_TYPE_SHIFT) |
                       (val & MC_WMASK);
    }
    rv_trig_refresh(h);
}

void rv_trig_refresh(rv_hart_t *h)
{
    h->trig_active = false;
    for (uint32_t i = 0; i < RV_TRIG_COUNT; i++) {
        /* Armed means mcontrol, enabled for M-mode, and watching something. */
        if ((h->tdata1[i] >> MC_TYPE_SHIFT) == MC_TYPE_MCONTROL &&
            (h->tdata1[i] & MC_M) != 0u &&
            (h->tdata1[i] & (MC_EXECUTE | MC_STORE | MC_LOAD)) != 0u) {
            h->trig_active = true;
            break;
        }
    }
    rv_hart_refresh_fetch_guard(h);
}

bool rv_trig_check(const rv_hart_t *h, uint32_t addr, rv_access_t acc)
{
    uint32_t want;

    switch (acc) {
    case RV_ACC_FETCH: want = MC_EXECUTE; break;
    case RV_ACC_LOAD:  want = MC_LOAD;    break;
    default:           want = MC_STORE;   break;
    }

    for (uint32_t i = 0; i < RV_TRIG_COUNT; i++) {
        const uint32_t d = h->tdata1[i];

        if ((d >> MC_TYPE_SHIFT) != MC_TYPE_MCONTROL) {
            continue;
        }
        if ((d & MC_M) == 0u || (d & want) == 0u) {
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
