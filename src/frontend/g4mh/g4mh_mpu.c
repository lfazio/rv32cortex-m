/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_mpu.c - the RH850 G4MH memory protection unit.
 *
 * Spec references are in g4mh_mpu.h. This file is the check and the
 * register window; the two call sites -- the fetch in g4mh_interp.c and
 * g4mh_load/g4mh_store in g4mh_cpu.c -- are guarded by `mpu_active` and
 * do not appear here.
 */

#include "g4mh/g4mh_mpu.h"
#include "g4mh/g4mh_cpu.h"

#include <string.h>

#if G4MH_EXT_MPU

void g4mh_mpu_reset(g4mh_mpu_t *m)
{
    /*
     * MPM resets to zero, so protection is off and MPAT.E is clear on
     * every entry. The addresses are architecturally *undefined* after
     * reset rather than zero, but an emulator has to pick something and
     * zero is the choice that cannot be mistaken for a configured area:
     * with E clear the entry is not consulted at all.
     */
    memset(m, 0, sizeof(*m));
}

bool g4mh_mpu_is_active(const g4mh_mpu_t *m)
{
    return (m->mpm & G4MH_MPM_MPE) != 0u;
}

/*
 * Does `spid` appear in one of the eight MPIDn slots, and is the
 * matching permission bit set for it?
 *
 * The SPID group is a second, independent gate: an area can permit
 * supervisor writes through MPAT.SW and still refuse this master because
 * its SPID is not among the eight, unless the group is bypassed by
 * RG/WG. Treating the two groups as alternatives -- permitting on either
 * -- would open every area to every master the moment SW was set.
 */
static bool spid_permits(const g4mh_mpu_t *m, uint32_t at, bool write,
                         uint32_t spid)
{
    if ((at & (write ? G4MH_MPAT_WG : G4MH_MPAT_RG)) != 0u) {
        return true;                    /* any SPID */
    }

    const unsigned sh = write ? G4MH_MPAT_WMPID_SHIFT
                              : G4MH_MPAT_RMPID_SHIFT;
    const uint32_t perm = (at >> sh) & 0xFFu;

    for (unsigned i = 0; i < 8u; i++) {
        if ((perm & (1u << i)) != 0u &&
            (m->mpid[i] & G4MH_MPID_SPID_MASK) == spid) {
            return true;
        }
    }
    return false;
}

bool g4mh_mpu_permits(const g4mh_mpu_t *m, uint32_t addr, uint32_t size,
                      g4mh_mpu_acc_t acc, bool user_mode, uint32_t spid)
{
    /*
     * "Enable all accesses in SV mode" when MPM.SVP is clear -- and that
     * is *all* accesses, fetch included, not merely the data ones. A
     * supervisor-mode guest that never sets SVP therefore runs
     * unprotected however its entries are configured, which is the
     * reset arrangement and the reason a guest must set SVP before the
     * MPU means anything to its own code.
     */
    if (!user_mode && (m->mpm & G4MH_MPM_SVP) == 0u) {
        return true;
    }

    /*
     * The whole span must be inside one enabled area. MPUA is the
     * *maximum* address and is inclusive -- "the specified maximum
     * address is included in the range of area matching", and its low
     * two bits are treated as 1 -- so the last byte is mpua | 3.
     *
     * An access straddling the top of an area is a violation even
     * though its first byte is inside, which is why `last` is compared
     * rather than `addr` alone. Sizes here are 1, 2 or 4 and the
     * alignment check has already run, so the addition cannot wrap.
     */
    const uint32_t last = addr + size - 1u;

    for (unsigned i = 0; i < G4MH_MPU_ENTRIES; i++) {
        const uint32_t at = m->mpat[i];

        if ((at & G4MH_MPAT_E) == 0u) {
            continue;
        }

        const uint32_t lo = m->mpla[i] & ~3u;
        const uint32_t hi = m->mpua[i] | 3u;

        if (addr < lo || last > hi) {
            continue;
        }

        /*
         * The mode group. RMPIDn covers execution and reading together,
         * so the SPID group is asked about a "read" for both -- but the
         * mode group keeps them apart, and an area that is readable and
         * not executable must refuse a fetch.
         */
        uint32_t need;
        switch (acc) {
        case G4MH_MPU_FETCH:
            need = user_mode ? G4MH_MPAT_UX : G4MH_MPAT_SX;
            break;
        case G4MH_MPU_WRITE:
            need = user_mode ? G4MH_MPAT_UW : G4MH_MPAT_SW;
            break;
        case G4MH_MPU_READ:
        default:
            need = user_mode ? G4MH_MPAT_UR : G4MH_MPAT_SR;
            break;
        }

        if ((at & need) != 0u &&
            spid_permits(m, at, acc == G4MH_MPU_WRITE, spid)) {
            return true;
        }
        /*
         * A matching area that refuses is *not* the end of the search:
         * areas may overlap, and the architecture permits an access
         * allowed by any one of them. Stopping at the first match would
         * make the entry order significant, which it is not.
         */
    }

    /*
     * Matching nothing denies. That is the opposite of the RISC-V PMP's
     * M-mode rule and is worth stating, because this project has been
     * bitten by carrying that assumption across: there is no background
     * region here, so a guest that enables MPM.MPE with no entry
     * covering its own code stops immediately.
     */
    return false;
}

/* ------------------------------------------------------------------ */
/* The selID-5 window                                                  */
/* ------------------------------------------------------------------ */

/*
 * MPIDX names the entry that MPLA, MPUA and MPAT refer to. Out of range
 * -- "if the value more than MPCFG.NMPUE is specified" -- the three are
 * "handled as an undefined register", so reads give zero and writes are
 * dropped rather than wrapping onto entry 0, which would corrupt a
 * configured area from a typo.
 */
static bool entry_index(const g4mh_mpu_t *m, unsigned *out)
{
    const uint32_t idx = m->mpidx & 0x1Fu;

    if (idx >= G4MH_MPU_ENTRIES) {
        return false;
    }
    *out = (unsigned)idx;
    return true;
}

bool g4mh_mpu_sr_read(const g4mh_mpu_t *m, unsigned reg, uint32_t *out)
{
    unsigned e;

    switch (reg) {
    case G4MH_SR_MPM:    *out = m->mpm;   return true;
    case G4MH_SR_MPCFG:  *out = G4MH_MPCFG_VALUE; return true;
    case G4MH_SR_MPIDX:  *out = m->mpidx; return true;
    case G4MH_SR_MPBK:   *out = 0u;       return true;  /* one bank */
    case G4MH_SR_MCA:    *out = m->mca;   return true;
    case G4MH_SR_MCS:    *out = m->mcs;   return true;
    case G4MH_SR_MCC:    *out = m->mcc;   return true;
    case G4MH_SR_MCR:    *out = m->mcr;   return true;
    case G4MH_SR_MCI:    *out = m->mci;   return true;

    case G4MH_SR_MPLA:
        *out = entry_index(m, &e) ? m->mpla[e] : 0u;
        return true;
    case G4MH_SR_MPUA:
        *out = entry_index(m, &e) ? m->mpua[e] : 0u;
        return true;
    case G4MH_SR_MPAT:
        *out = entry_index(m, &e) ? m->mpat[e] : 0u;
        return true;

    default:
        if (reg >= G4MH_SR_MPID0 && reg < G4MH_SR_MPID0 + 8u) {
            *out = m->mpid[reg - G4MH_SR_MPID0];
            return true;
        }
        return false;
    }
}

bool g4mh_mpu_sr_write(g4mh_mpu_t *m, unsigned reg, uint32_t val)
{
    unsigned e;

    switch (reg) {
    case G4MH_SR_MPM:
        m->mpm = val & (G4MH_MPM_MPE | G4MH_MPM_SVP);
        return true;

    case G4MH_SR_MPCFG:
    case G4MH_SR_MPBK:
        return true;                    /* read-only; the write is dropped */

    case G4MH_SR_MPIDX:  m->mpidx = val & 0x1Fu; return true;
    case G4MH_SR_MCA:    m->mca = val; return true;
    case G4MH_SR_MCS:    m->mcs = val; return true;
    case G4MH_SR_MCC:    m->mcc = val; return true;
    case G4MH_SR_MCR:    m->mcr = val; return true;
    case G4MH_SR_MCI:    m->mci = val & G4MH_MPID_SPID_MASK; return true;

    /*
     * The addresses keep bits 31:2; bits 1:0 are reserved and read back
     * as zero. Storing them as written would make an entry whose MPLA
     * was set from an unaligned pointer refuse its own first word.
     */
    case G4MH_SR_MPLA:
        if (entry_index(m, &e)) { m->mpla[e] = val & ~3u; }
        return true;
    case G4MH_SR_MPUA:
        if (entry_index(m, &e)) { m->mpua[e] = val & ~3u; }
        return true;
    case G4MH_SR_MPAT:
        if (entry_index(m, &e)) {
            /* Bits 13:8 and 6 are reserved and read back zero. */
            m->mpat[e] = val & 0xFFFFC0BFu;
        }
        return true;

    default:
        if (reg >= G4MH_SR_MPID0 && reg < G4MH_SR_MPID0 + 8u) {
            m->mpid[reg - G4MH_SR_MPID0] = val & G4MH_MPID_SPID_MASK;
            return true;
        }
        return false;
    }
}

#endif /* G4MH_EXT_MPU */
