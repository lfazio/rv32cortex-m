/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pmp.c - Physical memory protection.
 *
 * The thing that makes PMP affordable here is a detail of the privileged
 * spec: an *unlocked* PMP entry does not restrict M-mode. This core runs
 * M-mode only, so until software sets a lock bit, PMP cannot deny anything
 * and the checks are pure overhead.
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
 * The address range entry `i` covers, as [lo, hi) in bytes.
 *
 * pmpaddr holds address bits [33:2], so everything here works in units of
 * four bytes and is shifted up at the end. Returns false for OFF and for a
 * TOR entry whose range is empty.
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
        *lo = prev << 2;
        *hi = addr << 2;
        return true;
    }
    case PMP_A_NA4:
        *lo = addr << 2;
        *hi = *lo + 4u;
        return true;
    case PMP_A_NAPOT: {
        /*
         * NAPOT encodes the size in a run of low one bits: the lowest zero
         * bit marks the boundary. All ones would mean the whole address
         * space, which cannot be expressed in 32 bits here, so it is
         * treated as the largest range that can.
         */
        const uint32_t inv = ~addr;
        if (inv == 0u) {
            *lo = 0u;
            *hi = 0xFFFFFFFFu;
            return true;
        }
        /* Bit position of the lowest zero. */
        const uint32_t n = (uint32_t)__builtin_ctz(inv);
        const uint32_t size_words = 1u << (n + 1u);
        const uint32_t base = (addr & ~((size_words) - 1u)) << 2;
        *lo = base;
        /* size in bytes; guard the shift that would overflow 32 bits. */
        if (n + 3u >= 32u) {
            *hi = 0xFFFFFFFFu;
        } else {
            *hi = base + (size_words << 2);
        }
        return true;
    }
    default:
        return false;                 /* OFF */
    }
}

void rv_pmp_refresh(rv_hart_t *h)
{
    h->pmp_active = false;
    for (uint32_t i = 0; i < RV_PMP_ENTRIES; i++) {
        const uint32_t cfg = pmp_cfg(h, i);
        if ((cfg & PMP_L) != 0u && (cfg & PMP_A) != 0u) {
            h->pmp_active = true;
            return;
        }
    }
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

        /* Lowest matching entry decides, matched or not. */
        if ((cfg & PMP_L) == 0u) {
            /* Unlocked entries do not constrain M-mode. */
            return h->priv == RV_PRIV_M;
        }
        switch (acc) {
        case RV_ACC_FETCH: return (cfg & PMP_X) != 0u;
        case RV_ACC_LOAD:  return (cfg & PMP_R) != 0u;
        default:           return (cfg & PMP_W) != 0u;
        }
    }

    /* No entry matched: M-mode is unrestricted, anything else is denied. */
    return h->priv == RV_PRIV_M;
}

#endif /* RV_EXT_PMP */
