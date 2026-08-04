/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pmp.c - Physical memory protection.
 *
 * The thing that makes PMP affordable here is a detail of the privileged
 * spec: an *unlocked* PMP entry does not restrict M-mode. A guest that
 * stays in M-mode and never sets a lock bit therefore cannot be denied
 * anything, and the checks are pure overhead -- which is the common case,
 * since U-mode is optional and most guests here never leave M.
 *
 * rv_hart_t::pmp_active records whether any entry is both locked and
 * enabled. While it is false -- which is the state at reset and stays the
 * state for every guest that never touches PMP -- the access path skips
 * this file entirely, and the bus fast paths keep the cost that two rounds
 * of optimisation bought them.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#if RV_EXT_PMP

/* cfg byte layout. */
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_A     0x18u
#define PMP_L     0x80u

#define PMP_A_OFF   0u
#define PMP_A_TOR   1u
#define PMP_A_NA4   2u
#define PMP_A_NAPOT 3u

static uint32_t pmp_cfg(const rv_hart_t *h, uint32_t i)
{
    return (h->pmpcfg[i / 4u] >> ((i % 4u) * 8u)) & 0xFFu;
}

/*
 * pmpaddr counts four-byte units, so it addresses 34 bits and any bound at
 * or above 2^30 units lies outside what this core can reach. Saturate
 * rather than truncate: a permit-everything entry is written as a bound at
 * the very top, and truncating that wraps it to a low address, turning the
 * widest possible region into one of the narrowest.
 *
 * The cost of saturating is that a region ending exactly at 2^32 is
 * reported as ending at 0xFFFFFFFF, so the single byte there matches
 * nothing. Nothing is mapped at the top of the address space on any
 * platform this runs on, and the alternative -- carrying the end bound in
 * 64 bits through both backends and the JIT's inlined range test -- is not
 * worth it for one unreachable byte.
 */
static uint32_t pmp_bytes(uint64_t words)
{
    return (words >= ((uint64_t)1u << 30)) ? 0xFFFFFFFFu
                                           : (uint32_t)(words << 2);
}

/*
 * The address range entry `i` covers, as [lo, hi) in bytes.
 *
 * Returns false for OFF and for a TOR entry whose range is empty.
 */
static bool pmp_range(const rv_hart_t *h, uint32_t i, uint32_t cfg,
                      uint32_t *lo, uint32_t *hi)
{
    const uint32_t a = (cfg & PMP_A) >> 3;
    const uint32_t addr = h->pmpaddr[i];

    switch (a) {
    case PMP_A_TOR: {
        const uint32_t prev = (i == 0u) ? 0u : h->pmpaddr[i - 1u];
        if (addr <= prev) {
            return false;             /* empty or inverted range */
        }
        *lo = pmp_bytes(prev);
        *hi = pmp_bytes(addr);
        return true;
    }
    case PMP_A_NA4:
        *lo = pmp_bytes(addr);
        *hi = pmp_bytes((uint64_t)addr + 1u);
        return true;
    case PMP_A_NAPOT: {
        /*
         * NAPOT encodes the size in a run of low one bits: the lowest zero
         * bit marks the boundary, and a run of n ones means 2^(n+1) units.
         *
         * The arithmetic is done in 64 bits because the encoding reaches
         * exactly the sizes that overflow 32. `pmpaddr` all ones -- the
         * permit-everything entry riscv-tests installs -- is n == 31, where
         * `1u << (n + 1)` is a shift by 32: undefined, and in practice
         * yields 1, which decodes the widest region in the encoding as a
         * four-byte one at the top of memory. That is invisible in M-mode,
         * where matching nothing permits, and denies everything in U-mode,
         * where matching nothing does not.
         */
        const uint32_t inv = ~addr;
        const uint32_t n = (inv == 0u) ? 31u : (uint32_t)__builtin_ctz(inv);
        const uint64_t size = (uint64_t)1u << (n + 1u);
        const uint64_t base = (uint64_t)addr & ~(size - 1u);

        *lo = pmp_bytes(base);
        *hi = pmp_bytes(base + size);
        return true;
    }
    default:
        return false;                 /* OFF */
    }
}

/*
 * Recompute whether PMP has to be consulted at all.
 *
 * "Some entry is locked and enabled" is the M-mode answer, and it is only
 * the whole answer while M-mode is the only mode. Two things change below
 * M: an *unlocked* entry restricts, and matching no entry at all denies
 * rather than permits. So a hart running below M must always consult PMP,
 * even with no entries configured -- the opposite of the M-mode case, where
 * no entries means nothing can be denied.
 *
 * That is why this depends on h->priv, and why anything that changes the
 * privilege level has to call this. It is not merely a cache of the entry
 * configuration; it is the predicate both backends use to decide whether
 * the access path may skip PMP entirely, and the JIT additionally uses it
 * to decide whether a memory access may be inlined without a permission
 * test. Getting it wrong does not slow anything down -- it permits accesses
 * that should fault.
 */
void rv_pmp_refresh(rv_hart_t *h)
{
    /*
     * Either privilege being below M is enough: fetch is checked at
     * h->priv and data at rv_hart_data_priv, and MPRV can pull the second
     * below M while the first is still M.
     */
    h->pmp_active = false;
    if (h->priv != RV_PRIV_M || rv_hart_data_priv(h) != RV_PRIV_M) {
        h->pmp_active = true;
    } else {
        for (uint32_t i = 0; i < RV_PMP_ENTRIES; i++) {
            const uint32_t cfg = pmp_cfg(h, i);
            if ((cfg & PMP_L) != 0u && (cfg & PMP_A) != 0u) {
                h->pmp_active = true;
                break;
            }
        }
    }
    rv_hart_refresh_fetch_guard(h);
}

bool rv_pmp_simple(const rv_hart_t *h, uint32_t *lo, uint32_t *hi)
{
    bool found = false;

    /*
     * Report the bounds of the single enabled entry, if there is exactly
     * one, so a caller can inline a range test instead of calling the full
     * check.
     *
     * The contract is deliberately weak, and that is what makes it safe:
     * the range says where the *slow path is required*, not what is
     * permitted. An address outside every entry matches nothing, and the
     * no-match rule for M-mode is allow -- so the inlined path is correct
     * for anything outside. An address inside goes to the helper, which
     * applies permissions, the straddle rule and privilege exactly as
     * before.
     *
     * Reasoning about which side is permitted would be a mistake here: a
     * single entry that grants R denies W at the same addresses, so
     * "permitted window" is not even well defined without fixing the access
     * kind, and getting it backwards would silently allow a denied store.
     * Bounds alone need no such reasoning.
     *
     * Two or more enabled entries return false, because then the lowest
     * match wins and one compare cannot express which entry an address
     * hits first.
     */
    for (uint32_t i = 0; i < RV_PMP_ENTRIES; i++) {
        uint32_t elo, ehi;

        if (!pmp_range(h, i, pmp_cfg(h, i), &elo, &ehi)) {
            continue;
        }
        if (found) {
            return false;
        }
        *lo = elo;
        *hi = ehi;
        found = true;
    }
    return found;
}

bool rv_pmp_check(const rv_hart_t *h, uint32_t addr, uint32_t size,
                  rv_access_t acc)
{
    /*
     * The spec requires every byte of the access to fall in the same entry;
     * an access straddling two entries fails even if both would allow it.
     * Checking the first and last byte is enough, because entries are
     * contiguous ranges and the lowest matching one wins.
     */
    const uint32_t last = addr + size - 1u;

    /*
     * Fetch is checked at the privilege the hart is running at; a load or
     * store at whatever MPRV says. Both the M-mode exemption for unlocked
     * entries and the no-match rule below turn on this, not on h->priv.
     */
    const uint32_t eff = (acc == RV_ACC_FETCH) ? (uint32_t)h->priv
                                               : rv_hart_data_priv(h);

    for (uint32_t i = 0; i < RV_PMP_ENTRIES; i++) {
        const uint32_t cfg = pmp_cfg(h, i);
        uint32_t lo, hi;

        if (!pmp_range(h, i, cfg, &lo, &hi)) {
            continue;
        }
        if (addr < lo || addr >= hi) {
            continue;                 /* first byte is not in this entry */
        }
        if (last >= hi) {
            return false;             /* access straddles the top */
        }

        /*
         * Lowest matching entry decides, matched or not.
         *
         * L says *who the entry applies to*, not whether it is in force. An
         * unlocked entry is invisible to M-mode and fully binding below it,
         * so the lock bit may only be consulted together with the privilege
         * level. Reading it alone -- "unlocked, therefore permitted" -- is
         * right in M-mode and denies every U-mode access that matches an
         * unlocked entry, which is the usual configuration: the background
         * region a supervisor leaves unlocked so it can still edit it.
         */
        if ((cfg & PMP_L) == 0u && eff == RV_PRIV_M) {
            return true;
        }
        switch (acc) {
        case RV_ACC_FETCH: return (cfg & PMP_X) != 0u;
        case RV_ACC_LOAD:  return (cfg & PMP_R) != 0u;
        default:           return (cfg & PMP_W) != 0u;
        }
    }

    /* No entry matched: M-mode is unrestricted, anything else is denied. */
    return eff == RV_PRIV_M;
}

#endif /* RV_EXT_PMP */
