/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_mmu.c - the e200z7's MMU: one TLB of 32 entries, the MAS registers,
 * and the five TLB instructions (manual chapter 10).
 *
 * **Every rule here is read from docs/ppc/e200z759CRM.pdf and from
 * nothing else.** There is no e200 to compare with and this project
 * takes nothing from a GPL emulator, not even as an oracle, so a result
 * is "what the manual says" and two readings of one sentence agree with
 * each other by construction. The section a rule comes from is beside
 * it; where the manual leaves a choice, the choice is stated.
 *
 * **The reset state is not the manual's.** A real core comes out of
 * reset with entry 0 a single 4 KB page at the reset base (table 10-7),
 * and gets past that because the part's boot assist ROM programs the TLB
 * before user code runs. Nothing in this model is that ROM, and every
 * guest in the tree is linked for flat physical addresses, so entry 0
 * here is the whole 4 GB, identity, every permission -- the same
 * attributes as the manual's entry otherwise: valid, TS 0, TID 0,
 * cache-inhibited, invalidation-protected, VLE from the image. A guest
 * that programs the TLB overwrites it as it would the ROM's entries.
 */

#include "ppc/ppc_cpu.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* An entry                                                            */
/* ------------------------------------------------------------------ */

/*
 * The bits of an effective address an entry's EPN is compared on, by its
 * size (table 10-1): TSIZE n is 2^n KB. A TSIZE above 4 GB is undefined
 * in the manual; such an entry matches nothing here, which is a choice.
 */
static bool page_mask(uint32_t mas1, uint32_t *mask)
{
    const uint32_t tsize = (mas1 & PPC_MAS1_TSIZE) >> PPC_MAS1_TSIZE_SHIFT;

    if (tsize > PPC_TSIZE_MAX) {
        return false;
    }
    *mask = (uint32_t)~((1024ull << tsize) - 1ull); /* 4 GB: 0 */
    return true;
}

static bool ea_on_page(uint32_t mas1, uint32_t mas2, uint32_t ea)
{
    uint32_t mask;

    return page_mask(mas1, &mask) && ((ea ^ mas2) & mask & PPC_MAS2_EPN) == 0u;
}

/*
 * Figure 10-1: valid, TS equal to the access's address space, TID equal
 * to the PID or zero (shared), EPN equal on the page's bits. "A hit to
 * multiple TLB entries is considered to be a programming error ... the
 * TLB generates an invalid address" -- here the lowest-numbered entry
 * wins, which is a choice and is deterministic.
 */
static int lookup(const ppc_cpu_t *c, uint32_t ea, uint32_t as, uint32_t pid)
{
    for (unsigned i = 0u; i < PPC_TLB_ENTRIES; i++) {
        const uint32_t m1 = c->tlb[i].mas1;
        const uint32_t tid = (m1 & PPC_MAS1_TID) >> PPC_MAS1_TID_SHIFT;

        if ((m1 & PPC_MAS1_V) == 0u ||
            ((m1 & PPC_MAS1_TS) != 0u) != (as != 0u) ||
            (tid != 0u && tid != pid)) {
            continue;
        }
        if (ea_on_page(m1, c->tlb[i].mas2, ea)) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* The reset entry, and whether translation is the identity            */
/* ------------------------------------------------------------------ */

#define DEFAULT_MAS1                                                           \
    (PPC_MAS1_V | PPC_MAS1_IPROT | (PPC_TSIZE_MAX << PPC_MAS1_TSIZE_SHIFT))
#define DEFAULT_MAS3 PPC_MAS3_PERMS

static void recompute_default(ppc_cpu_t *c)
{
    bool d = c->tlb[0].mas1 == DEFAULT_MAS1 &&
             c->tlb[0].mas2 == c->tlb_reset_mas2 &&
             c->tlb[0].mas3 == DEFAULT_MAS3;

    for (unsigned i = 1u; d && i < PPC_TLB_ENTRIES; i++) {
        d = (c->tlb[i].mas1 & PPC_MAS1_V) == 0u;
    }
    c->tlb_default = d;
}

void ppc_mmu_refresh(ppc_cpu_t *c)
{
    const bool on =
        !c->tlb_default || (c->msr & (PPC_MSR_IS | PPC_MSR_DS)) != 0u;

    if (on != c->mmu_on) {
        c->mmu_on = on;
        c->irq_dirty = true; /* the interpreter re-chooses its loop */
        if (!on) {
            /*
             * Back to the identity, where every page is entry 0: the
             * encoding is entry 0's again, whatever page the translating
             * loop last fetched from.
             */
            c->vle = (c->tlb_reset_mas2 & PPC_MAS2_VLE) != 0u;
            c->jit_ctx = ppc_cpu_ctx(c);
        }
    }
}

void ppc_mmu_reset(ppc_cpu_t *c)
{
    memset(c->tlb, 0, sizeof(c->tlb));
    /* EPN 0, cache-inhibited (table 10-7's WIMG 0100), big-endian, and
     * the encoding the image was chosen to run in. */
    c->tlb_reset_mas2 = PPC_MAS2_I | (c->vle ? PPC_MAS2_VLE : 0u);
    c->tlb[0].mas1 = DEFAULT_MAS1;
    c->tlb[0].mas2 = c->tlb_reset_mas2;
    c->tlb[0].mas3 = DEFAULT_MAS3;
    c->mas0 = c->mas1 = c->mas2 = c->mas3 = c->mas4 = c->mas6 = 0u;
    c->tlb_default = true;
    c->mmu_on = false;
    ppc_mmu_refresh(c);
}

/*
 * After any change to the TLB itself -- and the JIT's generation moves,
 * because a block holds code fetched through the mapping that was there
 * when it was translated. Coarse: every block goes, not those under the
 * entry that changed. A guest that maps its memory at start-up pays it
 * once; one that refills the TLB from a miss handler would pay it per
 * refill, and per-entry invalidation is what that would need.
 */
static void tlb_changed(ppc_cpu_t *c)
{
    recompute_default(c);
    ppc_mmu_refresh(c);
    c->jit_gen++;
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

static uint32_t pid_of(const ppc_cpu_t *c)
{
    return c->pid0 & 0xFFu; /* 10.2.3: eight bits on this core */
}

/*
 * Table 10-15, the "Instr/data TLB error" column: the MAS registers set
 * up for the handler, which with MAS4's defaults right has only MAS3 to
 * write before tlbwe.
 */
static void tlb_error_update(ppc_cpu_t *c, uint32_t ea, uint32_t as)
{
    const uint32_t nv = c->mas0 & PPC_MAS0_NV;
    const uint32_t tidsel =
        (c->mas4 & PPC_MAS4_TIDSELD) >> PPC_MAS4_TIDSELD_SHIFT;
    /* TIDSELD 00 is PID0 and 11 is zero; 01 and 10 are reserved, and
     * read as PID0 here. */
    const uint32_t tid = (tidsel == 3u) ? 0u : pid_of(c);

    c->mas0 = (c->mas4 & PPC_MAS4_TLBSELD) | (nv << PPC_MAS0_ESEL_SHIFT) | nv;
    c->mas1 = PPC_MAS1_V | (tid << PPC_MAS1_TID_SHIFT) |
              (as != 0u ? PPC_MAS1_TS : 0u) | (c->mas4 & PPC_MAS4_TSIZED);
    c->mas2 =
        (ea & PPC_MAS2_EPN) | (c->mas4 & (PPC_MAS4_VLED | PPC_MAS4_DWIMGE));
    c->mas3 = 0u;
    c->mas6 =
        (pid_of(c) << PPC_MAS6_SPID_SHIFT) | (as != 0u ? PPC_MAS6_SAS : 0u);
}

/* The address space an access is in: MSR[IS] for a fetch, [DS] for data. */
static uint32_t as_of(const ppc_cpu_t *c, bool exec)
{
    return (c->msr & (exec ? PPC_MSR_IS : PPC_MSR_DS)) != 0u ? 1u : 0u;
}

/* The page's real address: RPN on the page's bits, the offset below. */
static uint32_t real_of(const ppc_cpu_t *c, int i, uint32_t ea)
{
    uint32_t mask;

    (void)page_mask(c->tlb[i].mas1, &mask); /* it matched, so it is valid */
    return (c->tlb[i].mas3 & PPC_MAS3_RPN & mask) | (ea & ~mask);
}

bool ppc_mmu_probe_exec(const ppc_cpu_t *c, uint32_t ea, uint32_t *pa,
                        bool *vle, bool *le)
{
    const int i = lookup(c, ea, as_of(c, true), pid_of(c));
    const bool user = (c->msr & PPC_MSR_PR) != 0u;

    if (i < 0 || (c->tlb[i].mas3 & (user ? PPC_MAS3_UX : PPC_MAS3_SX)) == 0u) {
        return false;
    }
    *vle = (c->tlb[i].mas2 & PPC_MAS2_VLE) != 0u;
    *le = (c->tlb[i].mas2 & PPC_MAS2_E) != 0u;
    *pa = real_of(c, i, ea);
    return true;
}

bool ppc_mmu_data_page(const ppc_cpu_t *c, uint32_t ea, uint32_t *va,
                       uint32_t *pa, uint64_t *size)
{
    const uint32_t as = as_of(c, false);
    const uint32_t pid = pid_of(c);
    const int i = lookup(c, ea, as, pid);
    const bool user = (c->msr & PPC_MSR_PR) != 0u;
    const uint32_t need =
        user ? (PPC_MAS3_UR | PPC_MAS3_UW) : (PPC_MAS3_SR | PPC_MAS3_SW);
    uint32_t mask;

    if (i < 0 || (c->tlb[i].mas3 & need) != need ||
        (c->tlb[i].mas2 & PPC_MAS2_E) != 0u ||
        !page_mask(c->tlb[i].mas1, &mask)) {
        return false;
    }
    const uint32_t lo = c->tlb[i].mas2 & PPC_MAS2_EPN & mask;
    const uint64_t n = (uint64_t)(uint32_t)~mask + 1u;

    /*
     * The lowest-numbered entry wins (lookup), so one below i that
     * shares any address with this page would translate that part
     * differently. Entries above i never win while i matches.
     */
    for (int j = 0; j < i; j++) {
        const uint32_t m1 = c->tlb[j].mas1;
        const uint32_t tid = (m1 & PPC_MAS1_TID) >> PPC_MAS1_TID_SHIFT;
        uint32_t mj;

        if ((m1 & PPC_MAS1_V) == 0u ||
            ((m1 & PPC_MAS1_TS) != 0u) != (as != 0u) ||
            (tid != 0u && tid != pid) || !page_mask(m1, &mj)) {
            continue;
        }
        const uint32_t jlo = c->tlb[j].mas2 & PPC_MAS2_EPN & mj;
        const uint64_t jn = (uint64_t)(uint32_t)~mj + 1u;

        if ((uint64_t)jlo < (uint64_t)lo + n && (uint64_t)lo < jlo + jn) {
            return false;
        }
    }
    *va = lo;
    *pa = c->tlb[i].mas3 & PPC_MAS3_RPN & mask;
    *size = n;
    return true;
}

ppc_exc_t ppc_mmu_xlate(ppc_cpu_t *c, uint32_t ea, ppc_acc_t acc, uint32_t *pa,
                        bool *vle, bool *le)
{
    const bool exec = (acc == PPC_ACC_EXEC);
    const uint32_t as = as_of(c, exec);
    const int i = lookup(c, ea, as, pid_of(c));

    if (i < 0) {
        tlb_error_update(c, ea, as);
        if (!exec) {
            c->dear = ea; /* table 7-23 */
        }
        return exec ? (ppc_exc_t)PPC_IVOR_ITLB_ERROR
                    : (ppc_exc_t)PPC_IVOR_DTLB_ERROR;
    }

    const uint32_t m2 = c->tlb[i].mas2;
    const uint32_t m3 = c->tlb[i].mas3;
    const bool user = (c->msr & PPC_MSR_PR) != 0u;
    uint32_t need;

    if (vle != NULL) {
        *vle = (m2 & PPC_MAS2_VLE) != 0u;
    }
    if (le != NULL) {
        *le = (m2 & PPC_MAS2_E) != 0u;
    }
    /* Figure 10-3. */
    switch (acc) {
    case PPC_ACC_EXEC:
        need = user ? PPC_MAS3_UX : PPC_MAS3_SX;
        break;
    case PPC_ACC_WRITE:
        need = user ? PPC_MAS3_UW : PPC_MAS3_SW;
        break;
    default:
        need = user ? PPC_MAS3_UR : PPC_MAS3_SR;
        break;
    }
    if ((m3 & need) == 0u) {
        if (!exec) {
            c->dear = ea; /* table 7-13: a byte within the page */
        }
        return exec ? (ppc_exc_t)PPC_IVOR_INST_STORAGE
                    : (ppc_exc_t)PPC_IVOR_DATA_STORAGE;
    }

    *pa = real_of(c, i, ea);
    return PPC_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* The instructions (10.5)                                             */
/* ------------------------------------------------------------------ */

static unsigned esel_of(const ppc_cpu_t *c)
{
    return (c->mas0 & PPC_MAS0_ESEL) >> PPC_MAS0_ESEL_SHIFT;
}

/*
 * The instructions are out of line on purpose. Each has one caller, so
 * LTO folded all five -- loops included -- into the interpreter's switch,
 * and on the F746 that alone made CoreMark 7-14% slower: 1.2 KB more in
 * the hot function, which moves the cases around a 4 KB I-cache. Nothing
 * here runs often enough to be worth a byte of it.
 */

/* MAS0[TLBSEL] is ignored: there is one TLB (table 10-9). */
EMU_COLD_NOINLINE void ppc_tlbre(ppc_cpu_t *c)
{
    const unsigned i = esel_of(c);

    c->mas1 = c->tlb[i].mas1;
    c->mas2 = c->tlb[i].mas2;
    c->mas3 = c->tlb[i].mas3;
}

EMU_COLD_NOINLINE void ppc_tlbwe(ppc_cpu_t *c)
{
    const unsigned i = esel_of(c);

    c->tlb[i].mas1 = c->mas1 & PPC_MAS1_IMPL;
    c->tlb[i].mas2 = c->mas2 & PPC_MAS2_IMPL;
    c->tlb[i].mas3 = c->mas3 & PPC_MAS3_IMPL;
    tlb_changed(c);
}

/*
 * 10.5.3 and table 10-15's two tlbsx columns: searched with MAS6's PID
 * and address space, not the current ones. MAS1[V] is the result.
 */
EMU_COLD_NOINLINE void ppc_tlbsx(ppc_cpu_t *c, uint32_t ea)
{
    const uint32_t as = c->mas6 & PPC_MAS6_SAS;
    const uint32_t spid = (c->mas6 & PPC_MAS6_SPID) >> PPC_MAS6_SPID_SHIFT;
    const uint32_t nv = c->mas0 & PPC_MAS0_NV;
    const int i = lookup(c, ea, as, spid);

    if (i >= 0) {
        c->mas0 = PPC_MAS0_TLBSEL1 | ((uint32_t)i << PPC_MAS0_ESEL_SHIFT) | nv;
        c->mas1 = c->tlb[i].mas1;
        c->mas2 = c->tlb[i].mas2;
        c->mas3 = c->tlb[i].mas3;
        return;
    }
    c->mas0 = (c->mas4 & PPC_MAS4_TLBSELD) | (nv << PPC_MAS0_ESEL_SHIFT) | nv;
    c->mas1 = (spid << PPC_MAS1_TID_SHIFT) | (as != 0u ? PPC_MAS1_TS : 0u) |
              (c->mas4 & PPC_MAS4_TSIZED);
    c->mas2 =
        (ea & PPC_MAS2_EPN) | (c->mas4 & (PPC_MAS4_VLED | PPC_MAS4_DWIMGE));
    c->mas3 = 0u;
}

/*
 * 10.5.4 and 10.8: by EPN alone -- AS and TID are ignored, so one
 * tlbivax can take several entries -- or every entry when EA bit 29
 * (INV_ALL) is set. An IPROT entry survives both.
 */
EMU_COLD_NOINLINE void ppc_tlbivax(ppc_cpu_t *c, uint32_t ea)
{
    const bool all = (ea & 0x4u) != 0u;

    for (unsigned i = 0u; i < PPC_TLB_ENTRIES; i++) {
        const uint32_t m1 = c->tlb[i].mas1;

        if ((m1 & PPC_MAS1_V) == 0u || (m1 & PPC_MAS1_IPROT) != 0u) {
            continue;
        }
        if (all || ea_on_page(m1, c->tlb[i].mas2, ea)) {
            c->tlb[i].mas1 = m1 & ~PPC_MAS1_V;
        }
    }
    tlb_changed(c);
}

/* MMUCSR0[TLB1_FI] (table 10-8): every entry not invalidation-protected. */
EMU_COLD_NOINLINE void ppc_mmu_flash_invalidate(ppc_cpu_t *c)
{
    ppc_tlbivax(c, 0x4u);
}
