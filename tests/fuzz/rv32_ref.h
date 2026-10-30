/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv32_ref.h - Sv32 and PMP written from the privileged spec, for the
 * RV32 targets that hold the hart to it: fuzz-rv32-mmu (data accesses
 * through rv_hart_load and rv_hart_store) and fuzz-rv32-vm (code running
 * in S and U on both backends).
 *
 * It shares nothing with rv_mmu.c or rv_pmp.c. One copy, because two
 * references to one spec would be two readings of it, and a fix to one
 * is the bug left in the other.
 *
 * Physical addresses are 34 bits. Where the spec leaves a choice to the
 * implementation the choice is this core's, made explicit where it is
 * made -- and asked of Sail and Spike by a probe where they could:
 *
 *   - Svade: A and D are checked, never written.
 *   - A table is read wherever a load would read, a device included
 *     (which regions may hold page tables is a PMA; probes/sv32ppn.S).
 *   - A misaligned data access splits at a page only when translated,
 *     each part its own physical access to PMP; untranslated it is one.
 *   - A fetch is by 16-bit parcel: each parcel of a 32-bit instruction
 *     is translated and PMP-checked on its own, as Sail and Spike fetch.
 *
 * Header-only and static, like fuzz_harness.h: each target is one
 * translation unit and describes its machine in an rv32_ref_t.
 */
#ifndef FUZZ_RV32_REF_H
#define FUZZ_RV32_REF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rv32/rv_csr.h"
#include "rv32/rv_hart.h"

#define REF_PG 4096u

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define PRIV_U 0u
#define PRIV_S 1u
#define PRIV_M 3u

#define C_FETCH_ACCESS 1u
#define C_LOAD_ACCESS 5u
#define C_STORE_ACCESS 7u
#define C_FETCH_PAGE 12u
#define C_LOAD_PAGE 13u
#define C_STORE_PAGE 15u

typedef enum { ACC_LOAD, ACC_STORE, ACC_EXEC } acc_t;
typedef enum { MEM_NONE, MEM_RAM, MEM_ROM, MEM_DEV } mem_t;

typedef struct rv32_ref {
    /* What software reads back after each write. */
    uint32_t satp, mstatus;
    uint32_t pmpcfg[4], pmpaddr[16];
    /* The machine. */
    uint8_t *ram; /* the reference's own copy */
    uint32_t ram_base, ram_size;
    const uint8_t *rom;
    uint32_t rom_base, rom_size;
    const uint8_t *rom2; /* a second read-only region, or size 0 */
    uint32_t rom2_base, rom2_size;
    uint32_t dev_base, dev_size; /* 0: none */
    uint32_t (*dev_value)(uint64_t pa, uint32_t size);
    /* Per RAM page, set when a walk reads a PTE there; may be NULL. */
    bool *table_page;
} rv32_ref_t;

/* The CSRs the reference reads, from the hart, as software reads them. */
static inline void rv32_ref_read_csrs(rv32_ref_t *r, rv_hart_t *h)
{
    (void)rv_csr_read(h, CSR_SATP, &r->satp);
    (void)rv_csr_read(h, CSR_MSTATUS, &r->mstatus);
    for (uint32_t i = 0; i < 4u; i++) {
        (void)rv_csr_read(h, CSR_PMPCFG0 + i, &r->pmpcfg[i]);
    }
    for (uint32_t i = 0; i < 16u; i++) {
        (void)rv_csr_read(h, CSR_PMPADDR0 + i, &r->pmpaddr[i]);
    }
}

/*
 * PMP, section 3.7: the lowest entry holding any byte decides, and must
 * hold them all; no match permits M only.
 */
static inline bool rv32_ref_pmp(const rv32_ref_t *r, uint64_t pa, uint32_t size,
                                acc_t acc, uint32_t priv)
{
    const uint64_t last = pa + size - 1u;

    for (uint32_t i = 0; i < 16u; i++) {
        const uint32_t cfg = (r->pmpcfg[i / 4u] >> (8u * (i % 4u))) & 0xFFu;
        const uint32_t a = (cfg >> 3) & 3u;
        uint64_t lo, hi; /* [lo, hi) */

        if (a == 0u) {
            continue;
        }
        if (a == 1u) { /* TOR */
            lo = (i == 0u) ? 0u : (uint64_t)r->pmpaddr[i - 1u] << 2;
            hi = (uint64_t)r->pmpaddr[i] << 2;
            if (hi <= lo) {
                continue;
            }
        } else if (a == 2u) { /* NA4 */
            lo = (uint64_t)r->pmpaddr[i] << 2;
            hi = lo + 4u;
        } else { /* NAPOT: k trailing ones in pmpaddr, 2^(k+3) bytes */
            const uint64_t v = ((uint64_t)r->pmpaddr[i] << 2) | 3u;
            uint64_t ones = 0u;

            while (((v >> ones) & 1u) != 0u && ones < 34u) {
                ones++;
            }
            const uint64_t sz = (uint64_t)1 << (ones + 1u);

            lo = v & ~(sz - 1u);
            hi = lo + sz;
        }
        if (last < lo || pa >= hi) {
            continue;
        }
        if (pa < lo || last >= hi) {
            return false; /* straddles: fails whatever the entry says */
        }
        if ((cfg & 0x80u) == 0u && priv == PRIV_M) {
            return true; /* unlocked binds below M only */
        }
        return (acc == ACC_LOAD)    ? (cfg & 1u) != 0u
               : (acc == ACC_STORE) ? (cfg & 2u) != 0u
                                    : (cfg & 4u) != 0u;
    }
    return priv == PRIV_M;
}

static inline mem_t rv32_ref_mem(const rv32_ref_t *r, uint64_t pa,
                                 uint32_t size)
{
    if (pa >= r->ram_base && pa + size <= (uint64_t)r->ram_base + r->ram_size) {
        return MEM_RAM;
    }
    if (pa >= r->rom_base && pa + size <= (uint64_t)r->rom_base + r->rom_size) {
        return MEM_ROM;
    }
    if (r->rom2_size != 0u && pa >= r->rom2_base &&
        pa + size <= (uint64_t)r->rom2_base + r->rom2_size) {
        return MEM_ROM;
    }
    if (r->dev_size != 0u && pa >= r->dev_base &&
        pa + size <= (uint64_t)r->dev_base + r->dev_size) {
        return MEM_DEV;
    }
    return MEM_NONE;
}

static inline uint32_t rv32_ref_read(const rv32_ref_t *r, uint64_t pa,
                                     uint32_t len)
{
    uint32_t v = 0u;

    switch (rv32_ref_mem(r, pa, len)) {
    case MEM_RAM:
        for (uint32_t i = 0; i < len; i++) {
            v |= (uint32_t)r->ram[pa - r->ram_base + i] << (8u * i);
        }
        return v;
    case MEM_ROM: {
        const bool second = r->rom2_size != 0u && pa >= r->rom2_base &&
                            pa - r->rom2_base < r->rom2_size;
        const uint8_t *const p =
            second ? &r->rom2[pa - r->rom2_base] : &r->rom[pa - r->rom_base];

        for (uint32_t i = 0; i < len; i++) {
            v |= (uint32_t)p[i] << (8u * i);
        }
        return v;
    }
    default:
        return r->dev_value(pa, len);
    }
}

/*
 * Sv32, section 4.3.2, with Svade. A non-leaf PTE with D, A or U set
 * holds bits reserved for future use, which the current spec makes a
 * page fault. A fetch needs X, and U for U-mode; S never executes a U
 * page, SUM or not, and MXR is for loads.
 */
static inline uint32_t rv32_ref_translate(rv32_ref_t *r, uint32_t va, acc_t acc,
                                          uint32_t priv, uint64_t *pa)
{
    const uint32_t pf = (acc == ACC_LOAD)    ? C_LOAD_PAGE
                        : (acc == ACC_STORE) ? C_STORE_PAGE
                                             : C_FETCH_PAGE;
    const uint32_t af = (acc == ACC_LOAD)    ? C_LOAD_ACCESS
                        : (acc == ACC_STORE) ? C_STORE_ACCESS
                                             : C_FETCH_ACCESS;

    if (priv == PRIV_M || (r->satp & SATP_MODE_SV32) == 0u) {
        *pa = va;
        return RV_EXC_NONE;
    }
    uint64_t a = (uint64_t)(r->satp & 0x3FFFFFu) * REF_PG;

    for (int i = 1; i >= 0; i--) {
        const uint64_t pte_at =
            a + ((va >> (12u + 10u * (unsigned)i)) & 0x3FFu) * 4u;

        /*
         * The walk's reads are PMP-checked at S, and read from wherever a
         * load would: a table in the image or the device is read there.
         */
        if (!rv32_ref_pmp(r, pte_at, 4u, ACC_LOAD, PRIV_S) ||
            rv32_ref_mem(r, pte_at, 4u) == MEM_NONE) {
            return af;
        }
        const uint32_t pte = rv32_ref_read(r, pte_at, 4u);

        /* A page a walk has read is a table: a store to it is a PTE write. */
        if (r->table_page != NULL && rv32_ref_mem(r, pte_at, 4u) == MEM_RAM) {
            r->table_page[(pte_at - r->ram_base) / REF_PG] = true;
        }

        if ((pte & PTE_V) == 0u || ((pte & PTE_W) && !(pte & PTE_R))) {
            return pf;
        }
        if ((pte & (PTE_R | PTE_X)) == 0u) {
            if (i == 0 || (pte & (PTE_D | PTE_A | PTE_U)) != 0u) {
                return pf;
            }
            a = (uint64_t)(pte >> 10) * REF_PG;
            continue;
        }
        /* A leaf. */
        if (priv == PRIV_U) {
            if ((pte & PTE_U) == 0u) {
                return pf;
            }
        } else if ((pte & PTE_U) != 0u &&
                   (acc == ACC_EXEC || !(r->mstatus & MSTATUS_SUM))) {
            return pf;
        }
        if (acc == ACC_LOAD) {
            if (!(pte & PTE_R) &&
                !((r->mstatus & MSTATUS_MXR) && (pte & PTE_X))) {
                return pf;
            }
        } else if (acc == ACC_STORE) {
            if (!(pte & PTE_W)) {
                return pf;
            }
        } else if (!(pte & PTE_X)) {
            return pf;
        }
        if (i == 1 && ((pte >> 10) & 0x3FFu) != 0u) {
            return pf; /* a misaligned megapage */
        }
        if (!(pte & PTE_A) || (acc == ACC_STORE && !(pte & PTE_D))) {
            return pf;
        }
        const uint64_t ppn1 = pte >> 20;
        const uint64_t ppn0 =
            (i == 1) ? ((va >> 12) & 0x3FFu) : ((pte >> 10) & 0x3FFu);

        *pa = (ppn1 << 22) | (ppn0 << 12) | (va & 0xFFFu);
        /*
         * Past the 32-bit bus there is nothing: an access fault, found by
         * the walk as a table past it is -- so before a split access's
         * second page is translated (this core's order; the spec leaves
         * a misaligned access's to the implementation).
         */
        if ((*pa >> 32) != 0u) {
            return af;
        }
        return RV_EXC_NONE;
    }
    return pf;
}

/*
 * One data access, all of it or none: each page it touches is
 * translated, in order, then each physical part is checked by PMP and
 * must be memory. Returns the cause, RV_EXC_NONE for success, with the
 * parts' addresses, and in `*bad` (if not NULL) which part was refused --
 * the second only when the first got past the same stage, which is
 * where mtval points. `misaligned_ok` is the build's RV_MISALIGNED.
 */
static inline uint32_t rv32_ref_access(rv32_ref_t *r, uint32_t va,
                                       uint32_t size, acc_t acc, uint32_t priv,
                                       bool misaligned_ok, uint64_t pa[2],
                                       uint32_t len[2], uint32_t *nparts,
                                       uint32_t *bad)
{
    const uint32_t af = (acc == ACC_LOAD) ? C_LOAD_ACCESS : C_STORE_ACCESS;
    uint32_t va2;

    if (!misaligned_ok && (va & (size - 1u)) != 0u) {
        return (acc == ACC_LOAD) ? 4u : 6u; /* misaligned, before anything */
    }
    len[0] = size;
    *nparts = 1u;
    if (priv != PRIV_M && (r->satp & SATP_MODE_SV32) != 0u &&
        (va & (REF_PG - 1u)) + size > REF_PG) {
        len[0] = REF_PG - (va & (REF_PG - 1u));
        len[1] = size - len[0];
        *nparts = 2u;
    }
    va2 = va + len[0];
    for (uint32_t p = 0; p < *nparts; p++) {
        const uint32_t c =
            rv32_ref_translate(r, p ? va2 : va, acc, priv, &pa[p]);

        if (c != RV_EXC_NONE) {
            if (bad != NULL) {
                *bad = p;
            }
            return c;
        }
    }
    for (uint32_t p = 0; p < *nparts; p++) {
        const mem_t m = rv32_ref_mem(r, pa[p], len[p]);

        if (!rv32_ref_pmp(r, pa[p], len[p], acc, priv) || m == MEM_NONE ||
            (acc == ACC_STORE && m == MEM_ROM)) {
            if (bad != NULL) {
                *bad = p;
            }
            return af;
        }
    }
    return RV_EXC_NONE;
}

/*
 * One 16-bit parcel of an instruction fetch at `va`: translated, PMP
 * held to X, and memory. RV_EXC_NONE with the parcel, or the cause --
 * and the address that faulted is `va` itself, which is what mtval
 * gets: "the portion of the instruction that caused the fault".
 */
static inline uint32_t rv32_ref_fetch16(rv32_ref_t *r, uint32_t va,
                                        uint32_t priv, uint32_t *parcel)
{
    uint64_t pa;
    const uint32_t c = rv32_ref_translate(r, va, ACC_EXEC, priv, &pa);

    if (c != RV_EXC_NONE) {
        return c;
    }
    if (!rv32_ref_pmp(r, pa, 2u, ACC_EXEC, priv) ||
        rv32_ref_mem(r, pa, 2u) == MEM_NONE ||
        rv32_ref_mem(r, pa, 2u) == MEM_DEV) {
        return C_FETCH_ACCESS;
    }
    *parcel = rv32_ref_read(r, pa, 2u);
    return RV_EXC_NONE;
}

#endif /* FUZZ_RV32_REF_H */
