/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_dtlb.c - the data TLB in host addresses: see include/emu/emu_dtlb.h.
 *
 * An entry caches the answer to three questions at once -- where the
 * page is, whether this privilege may load from it or store to it, and
 * whether it is plain memory -- for every aligned access in the page.
 * So it may be filled only when all three answers are the same for the
 * whole page, and must be cleared whenever any of them can change.
 *
 * **What it depends on, and what clears it:**
 *
 *   the mapping           SFENCE.VMA (rv_mmu_flush_page, rv_mmu_flush),
 *                         satp writes (rv_mmu_flush_tlb)
 *   the PTE's own bits    the same: the architecture requires an
 *                         SFENCE.VMA before a changed PTE need be seen
 *   SUM and MXR           nothing -- pages they decide are never filled
 *   the privilege         nothing -- each privilege has its own table,
 *                         and the JIT's context carries the one a
 *                         block probes
 *   the PMP configuration every pmpcfg and pmpaddr write
 *   the region table      reset; it is fixed while a guest runs
 *
 * It holds nothing the TLB in rv_mmu.c could not give again, so it may
 * keep an entry after that TLB has evicted it: an evicted translation is
 * still the right one until an SFENCE.VMA says otherwise.
 */

#include <string.h>

#include "rv32/rv_hart.h"

#if RV_HAVE_DTLB

/*
 * All three out of line, and that is measured rather than tidy. They are
 * reached from the SFENCE.VMA and CSR paths, which LTO inlines into the
 * interpreter's loop -- and inlined there they cost an M-mode guest that
 * never touches this table 2% of its host instructions, because the loop
 * is one function and the register allocator gave the fetched halfword a
 * stack slot to make room for them.
 */
EMU_NOINLINE void rv_dtlb_flush(rv_hart_t *h)
{
    /* EMU_DTLB_NONE in every tag, which no page number equals. */
    memset(h->dtlb, 0xFF, sizeof(h->dtlb));
    h->dtlb_flushes++;
}

EMU_NOINLINE void rv_dtlb_flush_page(rv_hart_t *h, uint32_t va)
{
    const uint32_t vpn = va >> EMU_DTLB_PAGE_SHIFT;
    const uint32_t slot = vpn & (RV_DTLB_ENTRIES - 1u);

    for (uint32_t t = 0; t < RV_DTLB_TABLES; t++) {
        emu_dtlb_entry_t *const e = &h->dtlb[t][slot];

        if (e->read == vpn || e->write == vpn) {
            e->read = EMU_DTLB_NONE;
            e->write = EMU_DTLB_NONE;
        }
    }
}

EMU_NOINLINE void rv_dtlb_fill(rv_hart_t *h, uint32_t va, uint32_t pa)
{
    const uint32_t priv = rv_hart_data_priv(h);

    if (priv == RV_PRIV_M) {
        return; /* M-mode has the RAM window, or nothing */
    }

    const uint32_t vpn = va >> EMU_DTLB_PAGE_SHIFT;
    emu_dtlb_entry_t *const e =
        &h->dtlb[rv_dtlb_table(priv)][vpn & (RV_DTLB_ENTRIES - 1u)];

    if (e->read == vpn) {
        /*
         * Already there, and this access missed for some other reason:
         * misaligned, or a store to a page filled without write. Asking
         * again would give the same answer.
         */
        return;
    }

    /* The page, by the PTE. Without translation the PTE grants all. */
    bool rd = true;
    bool wr = true;

#if RV_EXT_SV32
    if ((h->satp & SATP_MODE_SV32) != 0u &&
        !rv_mmu_dtlb_rights(h, va, priv, &wr)) {
        return;
    }
#endif

    /* By PMP. */
    const uint32_t page = pa & ~(RV_PAGE_SIZE - 1u);
#if RV_EXT_PMP
    {
        bool pr, pw;

        rv_pmp_page_rights(h, page, priv, &pr, &pw);
        rd = rd && pr;
        wr = wr && pw;
    }
#endif
    if (!rd) {
        return; /* an entry is a read entry first; W without R faults */
    }

    /*
     * By the bus: memory with a host buffer behind the whole page, at
     * every width, so that a load or store really is a move. ROM is
     * offered for loads; a store to it must still fault.
     */
    const emu_region_t *const r = emu_bus_find(h->bus, page);

    if (r == NULL || r->host == NULL ||
        (r->kind != (uint8_t)EMU_MEM_RAM && r->kind != (uint8_t)EMU_MEM_ROM) ||
        (page - r->base) > r->size || r->size - (page - r->base) < RV_PAGE_SIZE ||
        (r->widths & EMU_WANY) != EMU_WANY || (r->perm & EMU_PERM_R) == 0u) {
        return;
    }
    wr = wr && r->kind == (uint8_t)EMU_MEM_RAM && (r->perm & EMU_PERM_W) != 0u;

    const uintptr_t host = (uintptr_t)r->host + (page - r->base);

    e->read = vpn;
    e->write = wr ? vpn : EMU_DTLB_NONE;
    e->addend = host - (uintptr_t)(va & ~(RV_PAGE_SIZE - 1u));
    h->dtlb_fills++;
}

#endif /* RV_HAVE_DTLB */
