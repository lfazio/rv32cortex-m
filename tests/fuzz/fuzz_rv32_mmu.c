/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_rv32_mmu.c - RV32's translation and protection against a walk
 * written from the privileged spec.
 *
 * The other RV32 target compares the interpreter with the JIT, and both
 * call the same rv_mmu.c, rv_pmp.c and rv_dtlb.c -- so a misreading of
 * the spec there is shared, and agreed with. This one asks a reference
 * that shares nothing with them: page tables, satp, mstatus and PMP come
 * from the input, then a sequence of loads, stores, fences and CSR
 * writes runs through rv_hart_load and rv_hart_store, which is the whole
 * chain an access below M takes -- the data TLB probe, the TLB, the
 * walk, PMP, the bus, the data TLB fill. Each result is compared with
 * the reference's: the cause, or the value at the physical address it
 * computed, and after a store all of RAM.
 *
 * And after every step, **every data TLB entry is checked against what
 * it claims**: that each aligned access to its page, at its table's
 * privilege, is a plain load (or store) at the host address it holds.
 * A cache must agree with what it caches for every input.
 *
 * What the reference takes from the hart is only what software can
 * read: satp, mstatus and the PMP CSRs, read back after each write, so
 * their WARL rules are the CSR file's and not this file's question.
 *
 * Where the spec lets an implementation be stale, the harness behaves as
 * software must: a global SFENCE.VMA after any PTE changes (a store that
 * lands in a table page is one) and after any PMP write. satp, SUM, MXR
 * and MPRV take effect at once, which the spec requires of the mstatus
 * bits and this core documents of satp.
 *
 * The data privilege is set by MPRV and MPP from M-mode, as a kernel's
 * copy routines do. Fetch is not covered: the hart never leaves M, so the
 * fetch table is never filled.
 *
 * Built only with -DEMU_FUZZ=ON; see docs/fuzzing.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "emu/emu_dtlb.h"
#include "rv32/rv_csr.h"
#include "rv32/rv_hart.h"

#include "rv32_ref.h"

#if !RV_EXT_SV32 || !RV_EXT_PMP || !RV_HAVE_DTLB || !RV_EXT_U || !RV_EXT_S
#error "fuzz-rv32-mmu needs Sv32, PMP, U, S and the data TLB"
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define PG 4096u
#define RAM_BASE 0x80000000u
#define RAM_PAGES 16u
#define RAM_SIZE (RAM_PAGES * PG)
#define ROM_BASE 0x80100000u
#define DEV_BASE 0x80200000u

static emu_bus_t g_bus;
static rv_hart_t g_h;
static uint8_t g_ram[RAM_SIZE];
static uint8_t g_rom[PG];
static uint8_t g_shadow[RAM_SIZE]; /* the reference's RAM */
static bool g_table_page[RAM_PAGES]; /* pages a walk has read PTEs from */
static uint32_t g_dev_log, g_ref_dev_log;

static uint32_t dev_value(uint64_t pa, uint32_t size)
{
    uint32_t v = 0xD0000000u ^ ((uint32_t)(pa - DEV_BASE) * 0x9E3779B1u);

    return (size == 4u) ? v : v & ((1u << (8u * size)) - 1u);
}

/* A device: reads are a function of the address, writes are logged. */
/* Its value, `size` bytes of it: a device answers the width it is asked. */
static emu_fault_t dev_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    (void)ctx;
    *out = dev_value(DEV_BASE + (uint64_t)off, size);
    return EMU_FAULT_NONE;
}

static emu_fault_t dev_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    (void)ctx;
    g_dev_log = (g_dev_log ^ off ^ (val << 3) ^ size) * 0x01000193u;
    return EMU_FAULT_NONE;
}

static const emu_dev_ops_t k_dev_ops = {.read = dev_read, .write = dev_write};

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

/*
 * The reference is rv32_ref.h's, shared with fuzz-rv32-vm: Sv32 and PMP
 * from the spec, over this machine. What it takes from the hart is only
 * what software can read: satp, mstatus and the PMP CSRs, read back
 * after each write, so their WARL rules are the CSR file's.
 */
static rv32_ref_t g_csr = {
    .ram = g_shadow,
    .ram_base = RAM_BASE,
    .ram_size = RAM_SIZE,
    .rom = g_rom,
    .rom_base = ROM_BASE,
    .rom_size = PG,
    .dev_base = DEV_BASE,
    .dev_size = PG,
    .dev_value = dev_value,
    .table_page = g_table_page,
};

static void read_back(void)
{
    rv32_ref_read_csrs(&g_csr, &g_h);
}

static uint32_t data_priv(void)
{
    return (g_csr.mstatus & MSTATUS_MPRV)
               ? (g_csr.mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT
               : PRIV_M;
}

static uint32_t shadow_word(uint64_t pa)
{
    uint32_t v;

    memcpy(&v, &g_shadow[pa - RAM_BASE], 4u);
    return v;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t n;
} in_t;

static uint32_t byte(in_t *in)
{
    if (in->n == 0u) {
        return 0u;
    }
    in->n--;
    return *in->p++;
}

static uint32_t word(in_t *in)
{
    uint32_t v = byte(in);

    v |= byte(in) << 8;
    v |= byte(in) << 16;
    return v | (byte(in) << 24);
}

/* Physical pages a PTE may name: RAM, the image, the device, nothing. */
static uint32_t phys_page(uint32_t b)
{
    switch (b & 7u) {
    case 5:
        return ROM_BASE >> 12;
    case 6:
        return DEV_BASE >> 12;
    case 7: /* past 4 GiB, or nothing there below it */
        return (b & 8u) ? 0x100000u | (b >> 4) : 0x10u + (b >> 4);
    default:
        return (RAM_BASE >> 12) + ((b >> 3) % RAM_PAGES);
    }
}

/* Virtual addresses the tables were built for, so most accesses walk. */
#define NVA 16u
static uint32_t g_va[NVA];
static uint32_t g_nva;

static void write_pte(uint32_t table_page, uint32_t idx, uint32_t pte)
{
    const uint32_t off = table_page * PG + idx * 4u;

    memcpy(&g_ram[off], &pte, 4u);
    memcpy(&g_shadow[off], &pte, 4u);
    g_table_page[table_page] = true;
}

/*
 * The tables: two roots (pages 0 and 1) and second-level tables (pages
 * 2 and 3); the leaves and pointers in them from the input. Each entry
 * written also records a virtual address that walks through it.
 */
static void build_tables(in_t *in)
{
    const uint32_t n = 1u + byte(in) % 12u;

    for (uint32_t k = 0; k < n; k++) {
        const uint32_t root = byte(in) & 1u;
        const uint32_t i1 = byte(in) & 0x3FFu;
        const uint32_t i0 = (byte(in) | (byte(in) << 8)) & 0x3FFu;
        uint32_t flags = byte(in);
        const uint32_t kind = byte(in);

        /*
         * Mostly a usable leaf, so most accesses get past the walk -- and
         * some of those execute-only, which only MXR makes readable.
         */
        const uint32_t usable = byte(in);

        if (usable < 200u) {
            flags |= PTE_V | PTE_A;
            flags &= ~(PTE_U | PTE_R | PTE_W);
            flags |= (kind & 0x20u) ? PTE_U : 0u;
            if (usable < 32u) {
                flags |= PTE_X;
            } else {
                flags |= PTE_R;
                flags |= (kind & 0x10u) ? PTE_W | PTE_D : 0u;
            }
        }
        uint32_t pte;

        if ((kind & 3u) == 0u) {
            /* A megapage leaf: PPN[0] zero unless the input says otherwise */
            const uint32_t ppn1 = (kind & 4u) ? (RAM_BASE >> 22) + 1u
                                              : (RAM_BASE >> 22);
            const uint32_t ppn0 = (kind & 8u) ? 1u : 0u;

            pte = (ppn1 << 20) | (ppn0 << 10) | (flags & 0xFFu);
            write_pte(root, i1, pte);
        } else {
            /* A pointer to a second-level table, and a leaf in it. */
            const uint32_t l2 = 2u + ((kind >> 2) & 1u);
            uint32_t ptr = ((RAM_BASE >> 12) + l2) << 10 | PTE_V;

            if ((kind & 0x30u) == 0x30u) {
                ptr |= PTE_A; /* a reserved bit in a pointer */
            }
            if ((kind & 0xC0u) == 0xC0u) {
                ptr = (0x100000u << 10) | PTE_V; /* a table past 4 GiB */
            }
            write_pte(root, i1, ptr);
            pte = (phys_page(byte(in)) << 10) | (flags & 0xFFu);
            if ((kind & 0x08u) != 0u) {
                pte |= 0x80000000u; /* PPN bits beyond a 32-bit bus */
            }
            write_pte(l2, i0, pte);
        }
        if (g_nva < NVA) {
            g_va[g_nva++] = (i1 << 22) | (i0 << 12);
        }
    }
}

static uint32_t pick_va(in_t *in)
{
    const uint32_t b = byte(in);
    /* Page edges, the half-page edge PMP regions end on, misalignment. */
    static const uint32_t k_off[8] = {0u, 4u, 0xFFCu, 0xFFEu,
                                      0xFFDu, 0x7FEu, 0x800u, 2u};
    uint32_t base;

    if (g_nva == 0u || (b & 0xE0u) == 0xE0u) {
        base = word(in) & ~0xFFFu;
    } else if ((b & 0xE0u) == 0xC0u) {
        /* A RAM page as it is: what an untranslated access reaches. */
        base = RAM_BASE + (byte(in) % RAM_PAGES) * PG;
    } else {
        base = g_va[(b >> 3) % g_nva];
    }
    return base + k_off[b & 7u];
}

/* ------------------------------------------------------------------ */
/* Checking                                                            */
/* ------------------------------------------------------------------ */

static unsigned long long g_inputs, g_steps, g_loads_ok, g_stores_ok, g_faults,
    g_dtlb_entries, g_fences, g_dev_skipped;

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-rv32-mmu: %llu inputs, %llu accesses (%llu loads and %llu "
            "stores done, %llu faults), %llu data TLB entries checked, "
            "%llu fences; %llu device accesses not compared\n",
            g_inputs, g_steps, g_loads_ok, g_stores_ok, g_faults,
            g_dtlb_entries, g_fences, g_dev_skipped);
}

static void fail(const char *what, uint32_t va, uint32_t got, uint32_t want)
{
    fprintf(stderr,
            "fuzz-rv32-mmu: %s at va 0x%08x: emulator 0x%08x, reference "
            "0x%08x\n  satp 0x%08x mstatus 0x%08x (data privilege %u)\n",
            what, va, got, want, g_csr.satp, g_csr.mstatus, data_priv());
    for (uint32_t i = 0; i < 4u; i++) {
        fprintf(stderr, "  pmpcfg%u 0x%08x\n", i, g_csr.pmpcfg[i]);
    }
    for (uint32_t i = 0; i < 16u; i++) {
        if (g_csr.pmpaddr[i] != 0u) {
            fprintf(stderr, "  pmpaddr%u 0x%08x\n", i, g_csr.pmpaddr[i]);
        }
    }
    for (uint32_t t = 0; t < RAM_PAGES; t++) {
        if (!g_table_page[t]) {
            continue;
        }
        for (uint32_t i = 0; i < 1024u; i++) {
            const uint32_t pte = shadow_word(RAM_BASE + t * PG + i * 4u);

            if ((pte & PTE_V) != 0u) {
                fprintf(stderr, "  table page %u [%4u] = 0x%08x\n", t, i, pte);
            }
        }
    }
    abort();
}

/*
 * Every data TLB entry, against what it claims: that every aligned
 * access to its page at its table's privilege is a plain access at
 * va + addend -- a load, and a store too if it is writable.
 */
static void check_dtlb(void)
{
    for (uint32_t t = 0; t < RV_DTLB_TABLES; t++) {
        const uint32_t priv = (t == 0u) ? PRIV_U : PRIV_S;

        for (uint32_t s = 0; s < RV_DTLB_ENTRIES; s++) {
            const emu_dtlb_entry_t *const e = &g_h.dtlb[t][s];

            if (e->read == EMU_DTLB_NONE) {
                if (e->write != EMU_DTLB_NONE) {
                    fail("data TLB entry writable but not readable", s << 12,
                         e->write, EMU_DTLB_NONE);
                }
                continue;
            }
            g_dtlb_entries++;
            const uint32_t va = e->read << 12;

            if ((e->read & (RV_DTLB_ENTRIES - 1u)) != s) {
                fail("data TLB entry in the wrong slot", va, s, e->read);
            }
            for (int st = 0; st < 2; st++) {
                const acc_t acc = st ? ACC_STORE : ACC_LOAD;
                const bool claims = !st || e->write == e->read;
                uint64_t pa;
                const uint32_t save = g_csr.mstatus;

                if (st && e->write != EMU_DTLB_NONE && e->write != e->read) {
                    fail("data TLB write tag for another page", va, e->write,
                         e->read);
                }
                if (!claims) {
                    continue;
                }
                /* The entry's own privilege; and neither SUM nor MXR may
                 * be what grants it, since the fill excludes those pages. */
                g_csr.mstatus = (save & ~(MSTATUS_MPRV | MSTATUS_MPP_MASK |
                                          MSTATUS_SUM | MSTATUS_MXR)) |
                                MSTATUS_MPRV | (priv << MSTATUS_MPP_SHIFT);
                const uint32_t c = rv32_ref_translate(&g_csr, va, acc, priv, &pa);
                g_csr.mstatus = save;
                if (c != RV_EXC_NONE) {
                    fail(st ? "data TLB grants a store the walk refuses"
                            : "data TLB grants a load the walk refuses",
                         va, 0u, c);
                }
                const mem_t m = rv32_ref_mem(&g_csr, pa, PG);

                if (!rv32_ref_pmp(&g_csr, pa, PG, acc, priv) ||
                    (m != MEM_RAM && m != MEM_ROM) ||
                    (st && m != MEM_RAM)) {
                    fail(st ? "data TLB grants a store PMP or the bus refuses"
                            : "data TLB grants a load PMP or the bus refuses",
                         va, (uint32_t)(pa >> 12), m);
                }
                const uintptr_t want =
                    (m == MEM_RAM) ? (uintptr_t)&g_ram[pa - RAM_BASE]
                                   : (uintptr_t)&g_rom[pa - ROM_BASE];

                if ((uintptr_t)va + e->addend != want) {
                    fail("data TLB entry points at the wrong host page", va,
                         (uint32_t)((uintptr_t)va + e->addend),
                         (uint32_t)want);
                }
            }
        }
    }
}

static void fence_all(void)
{
    rv_mmu_flush(&g_h);
    g_fences++;
}

/*
 * A device sees transactions, and how the bus cuts a misaligned access
 * into them is the bus's business: there only the cause is compared.
 */
static bool dev_uncomparable(const uint64_t pa[2], const uint32_t len[2],
                             uint32_t nparts)
{
    for (uint32_t p = 0; p < nparts; p++) {
        if (rv32_ref_mem(&g_csr, pa[p], len[p]) == MEM_DEV &&
            (nparts > 1u || (pa[p] & (len[p] - 1u)) != 0u)) {
            return true;
        }
    }
    return false;
}

static void do_access(in_t *in)
{
    static const uint32_t k_size[4] = {1u, 2u, 4u, 4u};
    const uint32_t b = byte(in);
    const uint32_t size = k_size[b & 3u];
    const acc_t acc = (b & 4u) ? ACC_STORE : ACC_LOAD;
    const uint32_t va = pick_va(in);
    uint64_t pa[2];
    uint32_t len[2], nparts;
    const uint32_t want = rv32_ref_access(&g_csr, va, size, acc, data_priv(), RV_MISALIGNED_OK != 0, pa, len,
                        &nparts, NULL);
    uint32_t got;

    g_steps++;
    if (acc == ACC_LOAD) {
        uint32_t v = 0u;

        got = (uint32_t)rv_hart_load(&g_h, va, size, false, &v);
        if (got != want) {
            fail("load cause", va, got, want);
        }
        if (got == RV_EXC_NONE) {
            uint32_t ref = rv32_ref_read(&g_csr, pa[0], len[0]);

            if (nparts == 2u) {
                ref |= rv32_ref_read(&g_csr, pa[1], len[1]) << (8u * len[0]);
            }
            if (dev_uncomparable(pa, len, nparts)) {
                g_dev_skipped++;
            } else if (v != ref) {
                fail("load value", va, v, ref);
            }
            g_loads_ok++;
        } else {
            g_faults++;
        }
        return;
    }

    const uint32_t val = word(in);

    got = (uint32_t)rv_hart_store(&g_h, va, size, val);
    if (got != want) {
        fail("store cause", va, got, want);
    }
    if (got != RV_EXC_NONE) {
        g_faults++;
        return;
    }
    g_stores_ok++;
    bool table = false;
    uint32_t done = 0u;

    for (uint32_t p = 0; p < nparts; p++) {
        const uint32_t part = val >> (8u * done);

        if (rv32_ref_mem(&g_csr, pa[p], len[p]) == MEM_DEV) {
            g_ref_dev_log = (g_ref_dev_log ^ (uint32_t)(pa[p] - DEV_BASE) ^
                             (part << 3) ^ len[p]) *
                            0x01000193u;
        } else {
            for (uint32_t i = 0; i < len[p]; i++) {
                g_shadow[pa[p] - RAM_BASE + i] = (uint8_t)(part >> (8u * i));
            }
            if (g_table_page[(pa[p] - RAM_BASE) / PG]) {
                table = true;
            }
        }
        done += len[p];
    }
    if (memcmp(g_ram, g_shadow, RAM_SIZE) != 0u) {
        uint32_t i = 0;

        while (g_ram[i] == g_shadow[i]) {
            i++;
        }
        fail("RAM after a store", va, RAM_BASE + i, g_shadow[i]);
    }
    if (dev_uncomparable(pa, len, nparts)) {
        g_dev_skipped++;
        g_ref_dev_log = g_dev_log;
    } else if (g_dev_log != g_ref_dev_log) {
        fail("device writes", va, g_dev_log, g_ref_dev_log);
    }
    if (table) {
        fence_all(); /* a PTE changed: software fences */
    }
}

static void write_csr(uint32_t csr, uint32_t val)
{
    (void)rv_csr_write(&g_h, csr, val);
    read_back();
}

static void do_step(in_t *in)
{
    const uint32_t op = byte(in) % 16u;

    switch (op) {
    case 0: /* satp: Bare, or either root, at once */
    {
        const uint32_t b = byte(in);
        const uint32_t root = (RAM_BASE >> 12) + (b & 1u);

        write_csr(CSR_SATP, (b & 2u) ? root | SATP_MODE_SV32 | ((b >> 2) << 22)
                                     : 0u);
        break;
    }
    case 1: /* mstatus: SUM, MXR, MPRV and MPP */
    {
        const uint32_t b = byte(in);
        uint32_t m = g_csr.mstatus & ~(MSTATUS_SUM | MSTATUS_MXR |
                                       MSTATUS_MPRV | MSTATUS_MPP_MASK);

        m |= (b & 1u) ? MSTATUS_SUM : 0u;
        m |= (b & 2u) ? MSTATUS_MXR : 0u;
        m |= (b & 4u) ? MSTATUS_MPRV : 0u;
        m |= ((b >> 3) & 3u) << MSTATUS_MPP_SHIFT;
        write_csr(CSR_MSTATUS, m);
        break;
    }
    case 2: /* a PMP entry, then the fence software owes it */
    {
        const uint32_t i = byte(in) % 16u;
        const uint32_t cfg = byte(in);
        uint32_t addr;
        const uint32_t sel = byte(in);

        switch (sel & 3u) {
        case 0: /* NAPOT over part of RAM: a page down to 512 bytes */
            addr = ((RAM_BASE + (sel >> 2) % RAM_PAGES * PG) >> 2) |
                   ((PG >> 3) - 1u) >> ((sel >> 6) & 3u);
            break;
        case 1: /* a TOR bound on a page or a half page */
            addr = (RAM_BASE + ((sel >> 2) % (RAM_PAGES + 1u)) * PG +
                    ((sel & 0x80u) ? PG / 2u : 0u)) >> 2;
            break;
        case 2:
            addr = 0xFFFFFFFFu; /* everything */
            break;
        default:
            addr = word(in);
            break;
        }
        write_csr(CSR_PMPADDR0 + i, addr);
        const uint32_t w = i / 4u, sh = 8u * (i % 4u);

        write_csr(CSR_PMPCFG0 + w,
                  (g_csr.pmpcfg[w] & ~(0xFFu << sh)) | ((cfg & 0x9Fu) << sh));
        fence_all();
        break;
    }
    case 3: /* a PTE changed, then fenced */
    {
        const uint32_t t = byte(in) % 4u;
        const uint32_t idx = (byte(in) | (byte(in) << 8)) & 0x3FFu;
        const uint32_t pte = (phys_page(byte(in)) << 10) | byte(in);

        write_pte(t, idx, pte);
        fence_all();
        break;
    }
    case 4: /* SFENCE.VMA of one page, which never makes anything wrong */
        rv_mmu_flush_page(&g_h, pick_va(in));
        break;
    case 5:
        fence_all();
        break;
    default:
        do_access(in);
        break;
    }
    check_dtlb();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    in_t in = {data, size};

    if (!once) {
        once = true;
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
    }
    g_inputs++;

    /* A fresh machine. */
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = g_ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };
    uint32_t seed = 0x9E3779B9u;

    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        g_ram[i] = (uint8_t)(seed >> 24);
    }
    for (uint32_t i = 0; i < PG; i++) {
        g_rom[i] = (uint8_t)(i * 7u + 3u);
    }
    /* The table pages start empty: every PTE invalid. */
    memset(g_ram, 0, 4u * PG);
    memcpy(g_shadow, g_ram, RAM_SIZE);
    memset(g_table_page, 0, sizeof(g_table_page));
    g_nva = 0u;
    g_dev_log = g_ref_dev_log = 0u;

    emu_bus_init(&g_bus);
    if (!emu_bus_add(&g_bus, &ram) ||
        !emu_bus_add_rom(&g_bus, "rom", ROM_BASE, g_rom, PG) ||
        !emu_bus_add_mmio(&g_bus, "dev", DEV_BASE, PG, &k_dev_ops, NULL)) {
        fprintf(stderr, "fuzz-rv32-mmu: could not build the bus\n");
        abort();
    }
    rv_hart_init(&g_h, &g_bus, 0u);
    rv_hart_reset(&g_h, RAM_BASE);
    read_back();

    build_tables(&in);

    /* The step count, and in its top bits how the machine starts. */
    const uint32_t sb = byte(&in);
    const uint32_t steps = 1u + sb % 64u;

    /*
     * Where an OS starts: Sv32 on the first root, data at S by MPRV, and
     * the last PMP entry granting everything below M -- the entry a
     * kernel's boot leaves -- so the input's entries, lower, override it.
     * Or that entry locked, which makes M consult PMP, and data at M: the
     * two an M-mode access needs before an unlocked entry's lock bit can
     * be the one that decides.
     */
    write_csr(CSR_PMPADDR0 + 15u, 0xFFFFFFFFu);
    write_csr(CSR_PMPCFG0 + 3u,
              (((sb & 0x40u) ? 0x80u : 0u) | 0x18u | 0x07u) << 24);
    write_csr(CSR_SATP, SATP_MODE_SV32 | (RAM_BASE >> 12));
    write_csr(CSR_MSTATUS,
              (g_csr.mstatus & ~(MSTATUS_MPP_MASK | MSTATUS_MPRV)) |
                  ((sb & 0x80u) ? 0u : MSTATUS_MPRV) |
                  (PRIV_S << MSTATUS_MPP_SHIFT));
    fence_all();

    for (uint32_t s = 0; s < steps && (s == 0u || in.n > 0u); s++) {
        do_step(&in);
    }
    return 0;
}
