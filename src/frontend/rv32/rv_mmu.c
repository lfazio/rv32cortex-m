/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_mmu.c - Sv32 address translation.
 *
 * Two levels, 4 KiB pages with 4 MiB megapages at the first, and a
 * direct-mapped TLB in front so the common case is a tag compare rather
 * than two dependent loads.
 *
 * The whole file is reached only through h->vm_active, which is false
 * whenever satp is Bare or the access is M-mode's. That flag is the same
 * shape as pmp_active and exists for the same reason: this is on the fetch
 * and access paths, where anything unconditional is paid by every
 * instruction of every guest, including the ones that never enable paging.
 *
 * A and D are *checked*, not written -- the Svade behaviour. An access to
 * a page whose A is clear, or a store to one whose D is clear, raises a
 * page fault and leaves the table alone, which the privileged spec permits
 * and which keeps this file out of the business of writing guest memory
 * atomically. Software is expected to set them in its fault handler.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#if RV_EXT_SV32

/* PTE bits. */
#define PTE_V   0x001u
#define PTE_R   0x002u
#define PTE_W   0x004u
#define PTE_X   0x008u
#define PTE_U   0x010u
#define PTE_G   0x020u
#define PTE_A   0x040u
#define PTE_D   0x080u

/*
 * A physical page number, as it fits this core's 32-bit bus: PA[31:12].
 * Both satp and a PTE carry 22 architectural bits, and the top two of
 * those cannot address anything here.
 */
#define PPN_MASK        0x000FFFFFu

#define PAGE_SHIFT      12u
#define PAGE_SIZE       (1u << PAGE_SHIFT)

static rv_exc_t fault_for(emu_access_t acc)
{
    switch (acc) {
    case EMU_ACC_FETCH: return RV_EXC_INSN_PAGE_FAULT;
    case EMU_ACC_LOAD:  return RV_EXC_LOAD_PAGE_FAULT;
    default:           return RV_EXC_STORE_PAGE_FAULT;
    }
}

/*
 * The privilege an access is translated at. Fetch always uses the hart's
 * own; a load or store uses MPRV's answer, exactly as PMP does.
 */
static uint32_t acc_priv(const rv_hart_t *h, emu_access_t acc)
{
    return (acc == EMU_ACC_FETCH) ? (uint32_t)h->priv : rv_hart_data_priv(h);
}

void rv_mmu_flush(rv_hart_t *h)
{
    for (uint32_t i = 0; i < RV_TLB_ENTRIES; i++) {
        h->tlb[i].valid = false;
    }
    /* Tells the JIT its blocks may no longer describe the code at the
     * virtual addresses they were built from. */
    h->vm_gen++;
}

void rv_mmu_refresh(rv_hart_t *h)
{
    const bool paging = (h->satp & SATP_MODE_SV32) != 0u;

    /*
     * Either privilege being below M is enough to arm this, for the same
     * reason as in rv_pmp_refresh: MPRV can put data accesses in U-mode
     * while instructions are still fetched as M.
     */
    h->vm_active = paging &&
                   (h->priv != RV_PRIV_M ||
                    rv_hart_data_priv(h) != RV_PRIV_M);

    rv_hart_refresh_fetch_guard(h);
}

/*
 * Does this leaf PTE permit the access, at this privilege?
 *
 * Split out because the TLB has to ask the same question of a cached entry
 * that the walk asks of a fresh one -- and it must ask it *at the time of
 * the access*, since SUM, MXR and the privilege can all change without the
 * mapping changing at all.
 */
static bool leaf_permits(const rv_hart_t *h, uint32_t pte, emu_access_t acc,
                         uint32_t priv)
{
    /* U pages are unreachable from S unless SUM says otherwise, and SUM
     * never makes them executable. */
    if (priv == RV_PRIV_U) {
        if ((pte & PTE_U) == 0u) {
            return false;
        }
    } else {
        if ((pte & PTE_U) != 0u) {
            if (acc == EMU_ACC_FETCH || (h->mstatus & MSTATUS_SUM) == 0u) {
                return false;
            }
        }
    }

    switch (acc) {
    case EMU_ACC_FETCH:
        if ((pte & PTE_X) == 0u) {
            return false;
        }
        break;
    case EMU_ACC_LOAD:
        /* MXR makes execute-only pages readable; without it X alone does
         * not grant a load. */
        if ((pte & PTE_R) == 0u &&
            !((h->mstatus & MSTATUS_MXR) != 0u && (pte & PTE_X) != 0u)) {
            return false;
        }
        break;
    default:
        if ((pte & PTE_W) == 0u) {
            return false;
        }
        break;
    }

    /*
     * Svade: the accessed and dirty bits are checked and never written, so
     * a first touch of a page faults and software sets them. Doing it the
     * other way needs an atomic read-modify-write of guest memory from
     * inside the walk, for a hart with no one to race against.
     */
    if ((pte & PTE_A) == 0u) {
        return false;
    }
    if (acc == EMU_ACC_STORE && (pte & PTE_D) == 0u) {
        return false;
    }
    return true;
}

/* The access fault to report for an access of this kind. */
static rv_exc_t access_fault_for(emu_access_t acc)
{
    switch (acc) {
    case EMU_ACC_FETCH: return RV_EXC_INSN_ACCESS_FAULT;
    case EMU_ACC_LOAD:  return RV_EXC_LOAD_ACCESS_FAULT;
    default:           return RV_EXC_STORE_ACCESS_FAULT;
    }
}

/*
 * Read a PTE. The walk's own accesses are physical and PMP-checked.
 *
 * `acc` is the *original* access, not the implicit load this performs:
 * when PMP denies the page table, the guest must be told that its store
 * faulted, not that some load it never issued did. The implicit accesses
 * of a walk are not separately visible to software, so they cannot have
 * their own cause.
 */
static rv_exc_t read_pte(rv_hart_t *h, uint32_t pa, emu_access_t acc,
                         uint32_t *out)
{
#if RV_EXT_PMP
    /*
     * PMP applies to the page table itself, and a table placed where PMP
     * denies reads must fault rather than be read anyway.
     */
    if (EMU_UNLIKELY(h->pmp_active) &&
        !rv_pmp_check(h, pa, 4u, EMU_ACC_LOAD)) {
        return access_fault_for(acc);
    }
#endif
    if (emu_bus_read(h->bus, pa, 4u, out) != EMU_FAULT_NONE) {
        return access_fault_for(acc);
    }
    return RV_EXC_NONE;
}

static uint32_t tlb_slot(uint32_t vpn)
{
    return vpn & (RV_TLB_ENTRIES - 1u);
}

rv_exc_t rv_mmu_translate(rv_hart_t *h, uint32_t va, emu_access_t acc,
                          uint32_t *pa)
{
    const uint32_t priv = acc_priv(h, acc);

    /*
     * M-mode is never translated. vm_active is true if *either* privilege
     * is below M, so an M-mode access can still arrive here when MPRV has
     * armed translation for data only -- or the reverse.
     */
    if (priv == RV_PRIV_M) {
        *pa = va;
        return RV_EXC_NONE;
    }

    const uint32_t vpn = va >> PAGE_SHIFT;
    const uint32_t off = va & (PAGE_SIZE - 1u);
    const uint32_t slot = tlb_slot(vpn);

    if (EMU_LIKELY(h->tlb[slot].valid && h->tlb[slot].vpn == vpn)) {
        if (EMU_LIKELY(leaf_permits(h, h->tlb[slot].pte, acc, priv))) {
            *pa = h->tlb[slot].ppn | off;
            return RV_EXC_NONE;
        }
        /*
         * A hit that does not permit this access is not a fault yet: the
         * entry may be stale in a way the walk will resolve. Falling
         * through costs a walk on a path that is about to fault anyway.
         */
    }

    /* --- the walk ------------------------------------------------- */

    uint32_t table = (h->satp & PPN_MASK) << PAGE_SHIFT;

    for (int level = 1; level >= 0; level--) {
        const uint32_t idx = (va >> (PAGE_SHIFT + 10u * (uint32_t)level))
                             & 0x3FFu;
        uint32_t pte;

        const rv_exc_t exc = read_pte(h, table + idx * 4u, acc, &pte);
        if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
            /* An access fault reading the table is reported as it is; only
             * the mapping's own problems become page faults. */
            return exc;
        }

        if ((pte & PTE_V) == 0u ||
            ((pte & PTE_W) != 0u && (pte & PTE_R) == 0u)) {
            /* Invalid, or the reserved write-without-read encoding. */
            return fault_for(acc);
        }

        if ((pte & (PTE_R | PTE_X)) == 0u) {
            /* A pointer to the next level. There is no level below 0. */
            if (level == 0) {
                return fault_for(acc);
            }
            /*
             * D, A and U are reserved in a non-leaf PTE and must be clear.
             * The spec does not merely discourage setting them -- software
             * that does gets a page fault, which is what keeps the
             * encodings free for a future extension to define.
             */
            if ((pte & (PTE_D | PTE_A | PTE_U)) != 0u) {
                return fault_for(acc);
            }
            table = ((pte >> 10) & PPN_MASK) << PAGE_SHIFT;
            continue;
        }

        /* A leaf. */
        uint32_t base = ((pte >> 10) & PPN_MASK) << PAGE_SHIFT;
        if (level == 1) {
            /*
             * A megapage. Its physical base must be 4 MiB aligned -- a
             * misaligned superpage is a fault, not a rounding.
             */
            if ((base & 0x003FFFFFu) != 0u) {
                return fault_for(acc);
            }
            base |= va & 0x003FF000u;
        }

        if (!leaf_permits(h, pte, acc, priv)) {
            return fault_for(acc);
        }

        h->tlb[slot].vpn = vpn;
        h->tlb[slot].ppn = base;
        h->tlb[slot].pte = (uint8_t)(pte & 0xFFu);
        h->tlb[slot].valid = true;

        *pa = base | off;
        return RV_EXC_NONE;
    }

    return fault_for(acc);
}

#endif /* RV_EXT_SV32 */
