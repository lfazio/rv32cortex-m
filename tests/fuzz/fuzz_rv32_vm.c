/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_rv32_vm.c - RV32 code running in S and U under Sv32 and PMP, on
 * both backends, against the walk written from the spec.
 *
 * fuzz-rv32-mmu holds the data side to rv32_ref.h through rv_hart_load
 * and rv_hart_store, from M-mode by MPRV: the hart never leaves M, so the
 * fetch side is never asked, and the JIT -- whose translated loads and
 * stores probe the data TLB inline -- never runs. This target runs code:
 * the input's page tables map virtual pages onto a two-page code ROM
 * whose slots each hold one load or store and an ECALL, one of them
 * straddling the ROM's page boundary; a run enters the slot in S or U by
 * MRET from an M-mode trampoline, and every trap -- the slot's own ECALL
 * included -- goes to an M-mode ECALL that stops the hart. Each run is
 * compared with the reference: mcause, mepc, mtval, the registers, RAM.
 *
 * The JIT's blocks are kept from run to run, while the steps between
 * change PTEs (then fence, as software must), satp, SUM and MXR, and PMP:
 * a block, and the data TLB its inline probes read, built under one is
 * found again under the next.
 *
 * The reference decodes what the fetch returned, so a virtual page
 * mapped onto the wrong part of the ROM is still predicted: loads,
 * stores and ECALL it executes; anything else -- the halves of two
 * instructions a straddle can join, a compressed parcel -- it does not
 * model, and those runs are compared between the two backends only.
 *
 * Built only with -DEMU_FUZZ=ON; see docs/fuzzing.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_backend.h"
#include "emu/emu_bus.h"
#include "emu/emu_jit.h"
#include "rv32/rv_backend.h"
#include "rv32/rv_csr.h"
#include "rv32/rv_disasm.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_jit.h"

#include "rv32_ref.h"

#if !RV_EXT_SV32 || !RV_EXT_PMP || !RV_EXT_U || !RV_EXT_S
#error "fuzz-rv32-vm needs Sv32, PMP, U and S"
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define PG REF_PG
#define RAM_BASE 0x80000000u
#define RAM_PAGES 16u
#define RAM_SIZE (RAM_PAGES * PG)
#define ROM_BASE 0x80100000u /* the slots: two pages */
#define ROM_SIZE (2u * PG)
#define MROM_BASE                                                              \
    0x80800000u /* M-mode: mret, then the trap handler --
                               * past the 4 MB a megapage over RAM maps,
                               * so nothing the input maps reaches it */
#define MROM_TRAP 0x100u

#define ECALL 0x00000073u
#define MRET 0x30200073u
#define SLOTS 8u
#define STRADDLE 0xFFEu /* the slot across the ROM's page boundary */

typedef struct side {
    const char *name;
    const emu_backend_t *backend;
    rv_hart_t h;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
} side_t;

static side_t g_side[2] = {
    {.name = "interp", .backend = &rv_backend_interp},
    {.name = "jit", .backend = &rv_backend_jit},
};
static uint8_t g_rom[ROM_SIZE];
static uint8_t g_mrom[PG];
static uint8_t g_shadow[RAM_SIZE];
static bool g_table_page[RAM_PAGES];

static rv32_ref_t g_ref = {
    .ram = g_shadow,
    .ram_base = RAM_BASE,
    .ram_size = RAM_SIZE,
    .rom = g_rom,
    .rom_base = ROM_BASE,
    .rom_size = ROM_SIZE,
    .rom2 = g_mrom, /* reachable untranslated, with satp Bare */
    .rom2_base = MROM_BASE,
    .rom2_size = PG,
    .table_page = g_table_page,
};

static struct {
    uint64_t inputs, steps, runs, compared, differential, unfinished, completed,
        fetch_faults, straddles, data_faults, fences, translations, entries;
} g_st;

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-rv32-vm: %llu inputs, %llu steps, %llu runs: %llu against "
            "the spec (%llu completed, %llu fetch faults, %llu of them on an "
            "instruction's second half, %llu data faults), %llu between the "
            "backends only, %llu unfinished and set aside; %llu fences; jit "
            "%llu translations, %llu block entries\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.steps,
            (unsigned long long)g_st.runs, (unsigned long long)g_st.compared,
            (unsigned long long)g_st.completed,
            (unsigned long long)g_st.fetch_faults,
            (unsigned long long)g_st.straddles,
            (unsigned long long)g_st.data_faults,
            (unsigned long long)g_st.differential,
            (unsigned long long)g_st.unfinished,
            (unsigned long long)g_st.fences,
            (unsigned long long)g_st.translations,
            (unsigned long long)g_st.entries);
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

/*
 * Physical pages a leaf may name. Only the ROM's pages are ever
 * executable: a leaf that let code run from RAM would run whatever the
 * data is, which no reference here predicts.
 */
static uint32_t phys_page(uint32_t b, bool *rom)
{
    *rom = false;
    switch (b & 7u) {
    case 5:
    case 6:
        *rom = true;
        return (ROM_BASE >> 12) + (b & 1u);
    case 7: /* past 4 GiB, or nothing there below it */
        return (b & 8u) ? 0x100000u | (b >> 4) : 0x10u + (b >> 4);
    default:
        return (RAM_BASE >> 12) + ((b >> 3) % RAM_PAGES);
    }
}

#define NVA 16u
static uint32_t g_va[NVA];
static uint32_t g_nva;
/* Virtual pages whose next page maps the ROM's second: the straddle runs. */
static uint32_t g_pair[NVA];
static uint32_t g_npair;

static void write_pte(uint32_t table_page, uint32_t idx, uint32_t pte)
{
    const uint32_t off = table_page * PG + idx * 4u;

    for (unsigned s = 0; s < 2u; s++) {
        memcpy(&g_side[s].ram[off], &pte, 4u);
    }
    memcpy(&g_shadow[off], &pte, 4u);
    g_table_page[table_page] = true;
}

/* One leaf, or a pointer and a leaf, and the address that walks to it. */
static void add_mapping(in_t *in)
{
    const uint32_t root = byte(in) & 1u;
    const uint32_t i1 = byte(in) & 0x3FFu;
    const uint32_t i0 = (byte(in) | (byte(in) << 8)) & 0x3FFu;
    uint32_t flags = byte(in);
    const uint32_t kind = byte(in);
    const uint32_t usable = byte(in);
    bool rom;
    const uint32_t ppn = phys_page(byte(in), &rom);

    if (usable < 200u) { /* mostly a leaf that some access can use */
        flags |= PTE_V | PTE_A;
        flags &= ~(PTE_U | PTE_R | PTE_W | PTE_X);
        flags |= (kind & 0x20u) ? PTE_U : 0u;
        flags |= (kind & 0x10u) ? PTE_W | PTE_D | PTE_R : PTE_R;
        flags |= rom ? PTE_X : 0u;
        if (rom && (usable & 1u)) {
            flags &= ~(PTE_R | PTE_W); /* execute-only: MXR's case */
        }
    }
    if (!rom) {
        flags &= ~PTE_X;
    }
    if ((kind & 3u) == 0u) {
        /* A megapage over RAM, never executable. */
        const uint32_t pte = ((RAM_BASE >> 22) << 20) |
                             ((kind & 8u) ? 1u << 10 : 0u) | (flags & 0xF7u);

        write_pte(root, i1, pte);
    } else {
        const uint32_t l2 = 2u + ((kind >> 2) & 1u);
        uint32_t ptr = (((RAM_BASE >> 12) + l2) << 10) | PTE_V;

        if ((kind & 0x30u) == 0x30u) {
            ptr |= PTE_A; /* a reserved bit in a pointer */
        }
        write_pte(root, i1, ptr);
        write_pte(l2, i0, (ppn << 10) | (flags & 0xFFu));
    }
    if (g_nva < NVA) {
        g_va[g_nva++] = (i1 << 22) | (i0 << 12);
    }
}

/*
 * The code's two pages at two consecutive virtual pages, executable, in
 * one mode: what lets the straddling slot run, and PMP's edge between
 * its halves matter.
 */
static void add_code_pair(in_t *in)
{
    const uint32_t root = byte(in) & 1u;
    const uint32_t i1 = byte(in) & 0x3FFu;
    const uint32_t i0 = (byte(in) | (byte(in) << 8)) % 0x3FFu;
    const uint32_t l2 = 2u + (byte(in) & 1u);
    const uint32_t u = (byte(in) & 1u) ? PTE_U : 0u;
    const uint32_t flags = PTE_V | PTE_A | PTE_X | PTE_R | u;

    write_pte(root, i1, (((RAM_BASE >> 12) + l2) << 10) | PTE_V);
    write_pte(l2, i0, ((ROM_BASE >> 12) << 10) | flags);
    write_pte(l2, i0 + 1u, (((ROM_BASE >> 12) + 1u) << 10) | flags);
    if (g_npair < NVA) {
        g_pair[g_npair++] = (i1 << 22) | (i0 << 12);
    }
    if (g_nva < NVA) {
        g_va[g_nva++] = (i1 << 22) | (i0 << 12);
    }
}

/* A data address near a page edge of a mapped page. */
static uint32_t pick_va(in_t *in)
{
    const uint32_t b = byte(in);
    static const uint32_t k_off[8] = {0u,     4u,     0xFFCu, 0xFFEu,
                                      0xFFDu, 0x7FEu, 0x800u, 2u};

    if (g_nva == 0u || (b & 0xE0u) == 0xE0u) {
        return (word(in) & ~0xFFFu) + k_off[b & 7u];
    }
    return g_va[(b >> 3) % g_nva] + k_off[b & 7u];
}

/* ------------------------------------------------------------------ */
/* The two harts                                                       */
/* ------------------------------------------------------------------ */

/* The M-mode ECALL that ends a run: halt, and give a0 back as it was --
 * the frontend writes the hook's result there. */
static bool halt_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)user;
    sc->ret = sc->arg[0];
    ((rv_hart_t *)cpu)->state = EMU_STATE_HALTED;
    return true;
}

static void csr_both(uint32_t csr, uint32_t v)
{
    for (unsigned s = 0; s < 2u; s++) {
        (void)rv_csr_write(&g_side[s].h, csr, v);
    }
    rv32_ref_read_csrs(&g_ref, &g_side[0].h);
}

/* SFENCE.VMA with x0, x0: what the instruction does, on both. */
static void fence_both(void)
{
    for (unsigned s = 0; s < 2u; s++) {
        rv_mmu_flush(&g_side[s].h);
    }
    g_st.fences++;
}

static void build_side(side_t *sd)
{
    rv_hart_t *const h = &sd->h;
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, g_shadow, RAM_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) ||
        !emu_bus_add_rom(&sd->bus, "code", ROM_BASE, g_rom, ROM_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "mrom", MROM_BASE, g_mrom, PG)) {
        fprintf(stderr, "fuzz-rv32-vm: could not build the bus\n");
        abort();
    }
    rv_hart_init(h, &sd->bus, 0u);
    rv_hart_reset(h, MROM_BASE);
    h->mtvec = MROM_BASE + MROM_TRAP;
    h->ecall = halt_hook;
    h->ecall_user = NULL;
}

static unsigned g_fails;
static uint32_t g_run_va, g_run_priv;
static uint32_t g_budget = 256u;

static void dump(void)
{
    uint32_t parcel = 0u;
    char buf[96];

    fprintf(stderr, "  run at va 0x%08x in %s, satp 0x%08x mstatus 0x%08x\n",
            g_run_va, g_run_priv == PRIV_U ? "U" : "S", g_ref.satp,
            g_ref.mstatus);
    if (rv32_ref_fetch16(&g_ref, g_run_va, g_run_priv, &parcel) ==
        RV_EXC_NONE) {
        uint32_t hi = 0u;

        (void)rv32_ref_fetch16(&g_ref, g_run_va + 2u, g_run_priv, &hi);
        rv_disasm(buf, sizeof(buf), g_run_va, parcel | (hi << 16), 4u);
        fprintf(stderr, "    %s\n", buf);
    }
    for (unsigned i = 0; i < 16u; i++) {
        const uint32_t cfg = (g_ref.pmpcfg[i / 4u] >> (8u * (i % 4u))) & 0xFFu;

        if (cfg != 0u) {
            fprintf(stderr, "  pmp%u cfg 0x%02x addr 0x%08x\n", i, cfg,
                    g_ref.pmpaddr[i]);
        }
    }
}

static void fail(const char *who, const char *what, uint32_t got, uint32_t want)
{
    if (g_fails++ == 0u) {
        fprintf(stderr, "fuzz-rv32-vm: %s disagrees\n", who);
        dump();
    }
    fprintf(stderr, "  %-14s got 0x%08x want 0x%08x\n", what, got, want);
}

static void run_side(side_t *sd, uint32_t va, uint32_t priv, const uint32_t *x)
{
    rv_hart_t *const h = &sd->h;
    emu_jit_stats_t before, after;
    uint32_t total = 0u;

    /* M, at the trampoline: MRET to the slot in the run's mode. */
    h->priv = RV_PRIV_M;
    h->pc = MROM_BASE;
    h->state = EMU_STATE_RUNNING;
    memcpy(h->x, x, sizeof(h->x));
    (void)rv_csr_write(h, CSR_MEPC, va);
    (void)rv_csr_write(h, CSR_MSTATUS,
                       (h->mstatus & ~(MSTATUS_MPP_MASK | MSTATUS_MPRV)) |
                           (priv << MSTATUS_MPP_SHIFT));
    (void)rv_csr_write(h, CSR_MCAUSE, 0xDEAD0000u);
    (void)rv_csr_write(h, CSR_MTVAL, 0xDEAD0001u);

    rv_backend = sd->backend;
    emu_jit_get_stats(&before);
    while (total < g_budget && h->state == EMU_STATE_RUNNING) {
        uint32_t n = 0u;

        (void)sd->backend->run((emu_cpu_t *)h, 16u, &n);
        total += n + 1u;
    }
    emu_jit_get_stats(&after);
    if (sd == &g_side[1]) {
        g_st.translations += after.translations - before.translations;
        g_st.entries += after.block_entries - before.block_entries;
    }
}

/* ------------------------------------------------------------------ */
/* The reference: one run                                              */
/* ------------------------------------------------------------------ */

static struct {
    uint32_t mcause, mepc, mtval;
    uint32_t x[32];
    bool modelled;
    bool trap_either; /* after a PTE rewritten unfenced: any trap is legal */
} E;

static void ref_trap(uint32_t cause, uint32_t epc, uint32_t tval)
{
    E.mcause = cause;
    E.mepc = epc;
    E.mtval = tval;
}

/*
 * Fetch the instruction at `va`: its parcels, each translated and
 * checked on its own. False with the trap taken, mtval the parcel that
 * faulted and mepc the instruction.
 */
static bool ref_fetch(uint32_t va, uint32_t priv, uint32_t *insn, uint32_t *len)
{
    uint32_t lo, hi;
    uint32_t c = rv32_ref_fetch16(&g_ref, va, priv, &lo);

    if (c != RV_EXC_NONE) {
        ref_trap(c, va, va);
        g_st.fetch_faults++;
        return false;
    }
    if ((lo & 3u) != 3u) {
        *insn = lo;
        *len = 2u;
        return true;
    }
    c = rv32_ref_fetch16(&g_ref, va + 2u, priv, &hi);
    if (c != RV_EXC_NONE) {
        ref_trap(c, va, va + 2u);
        g_st.fetch_faults++;
        g_st.straddles += ((va + 2u) & 0xFFFu) == 0u;
        return false;
    }
    *insn = lo | (hi << 16);
    *len = 4u;
    return true;
}

/*
 * A load or store, from the spec's RV32I: the access through rv32_ref.h,
 * mtval the portion that faulted (the privileged spec's rule for a
 * misaligned access). False if not one this models.
 */
static bool ref_exec(uint32_t insn, uint32_t va, uint32_t priv, bool *trapped)
{
    const uint32_t op = insn & 0x7Fu;
    const uint32_t f3 = (insn >> 12) & 7u;
    const uint32_t rd = (insn >> 7) & 31u;
    const uint32_t rs1 = (insn >> 15) & 31u;
    const uint32_t rs2 = (insn >> 20) & 31u;
    uint64_t pa[2];
    uint32_t len[2], nparts, size;
    acc_t acc;
    int32_t imm;

    *trapped = false;
    if (op == 0x03u &&
        (f3 == 0u || f3 == 1u || f3 == 2u || f3 == 4u || f3 == 5u)) {
        acc = ACC_LOAD;
        imm = (int32_t)insn >> 20;
        size = 1u << (f3 & 3u);
    } else if (op == 0x23u && f3 <= 2u) {
        acc = ACC_STORE;
        imm =
            (int32_t)((((insn >> 25) << 5) | ((insn >> 7) & 31u)) << 20) >> 20;
        size = 1u << f3;
    } else {
        return false;
    }
    const uint32_t ea = E.x[rs1] + (uint32_t)imm;
    uint32_t bad = 0u;
    const uint32_t c =
        rv32_ref_access(&g_ref, ea, size, acc, priv, RV_MISALIGNED_OK != 0, pa,
                        len, &nparts, &bad);

    if (c != RV_EXC_NONE) {
        /* mtval: "the virtual address of the portion of the access that
         * caused the fault" -- the second part's, when it was refused. */
        ref_trap(c, va, bad ? ea + len[0] : ea);
        g_st.data_faults++;
        *trapped = true;
        return true;
    }
    if (acc == ACC_LOAD) {
        uint32_t v = rv32_ref_read(&g_ref, pa[0], len[0]);

        if (nparts == 2u) {
            v |= rv32_ref_read(&g_ref, pa[1], len[1]) << (8u * len[0]);
        }
        if (f3 == 0u) {
            v = (uint32_t)(int32_t)(int8_t)v;
        } else if (f3 == 1u) {
            v = (uint32_t)(int32_t)(int16_t)v;
        }
        if (rd != 0u) {
            E.x[rd] = v;
        }
    } else {
        uint32_t v = E.x[rs2];

        for (uint32_t p = 0; p < nparts; p++) {
            for (uint32_t i = 0; i < len[p]; i++) {
                g_shadow[pa[p] - RAM_BASE + i] = (uint8_t)v;
                v >>= 8;
            }
        }
    }
    return true;
}

/* The whole run: the slot's instruction, then what follows it. */
static void ref_run(uint32_t va, uint32_t priv)
{
    uint32_t insn, len;
    bool trapped;

    E.modelled = true;
    E.trap_either = false;
    if (!ref_fetch(va, priv, &insn, &len)) {
        return;
    }
    if (insn == ECALL) {
        ref_trap(priv == PRIV_U ? 8u : 9u, va, 0u);
        return;
    }
    uint8_t tables[4u * PG];

    memcpy(tables, g_shadow, sizeof(tables));
    if (!ref_exec(insn, va, priv, &trapped)) {
        E.modelled = false;
        return;
    }
    if (trapped) {
        return;
    }
    /*
     * A store that rewrote a PTE, with no SFENCE.VMA after it: the next
     * fetch may use the old translation or the new one -- the spec
     * permits both, and the interpreter (its fetch table) and the JIT
     * (a fresh walk for the next block) have been seen to choose
     * differently. What the store did is still exact; how the run ends
     * is not asked.
     */
    if (memcmp(tables, g_shadow, sizeof(tables)) != 0) {
        E.trap_either = true;
        return;
    }
    if (!ref_fetch(va + len, priv, &insn, &len)) {
        return;
    }
    if (insn != ECALL) {
        E.modelled = false;
        return;
    }
    ref_trap(priv == PRIV_U ? 8u : 9u, va + 4u, 0u);
    g_st.completed++;
}

static void compare_one(const side_t *sd, const char *who)
{
    const rv_hart_t *const h = &sd->h;

    if (h->state != EMU_STATE_HALTED || h->priv != RV_PRIV_M) {
        fail(who, "stopped", (uint32_t)h->state | (h->priv << 8),
             (uint32_t)EMU_STATE_HALTED | (RV_PRIV_M << 8));
    }
    if (!E.trap_either && h->mcause != E.mcause) {
        fail(who, "mcause", h->mcause, E.mcause);
    }
    if (!E.trap_either && h->mepc != E.mepc) {
        fail(who, "mepc", h->mepc, E.mepc);
    }
    if (!E.trap_either && h->mtval != E.mtval) {
        fail(who, "mtval", h->mtval, E.mtval);
    }
    for (unsigned i = 1; i < 32u; i++) {
        if (h->x[i] != E.x[i]) {
            char what[16];

            snprintf(what, sizeof(what), "x%u", i);
            fail(who, what, h->x[i], E.x[i]);
        }
    }
    if (memcmp(sd->ram, g_shadow, RAM_SIZE) != 0) {
        uint32_t i = 0;

        while (sd->ram[i] == g_shadow[i]) {
            i++;
        }
        fail(who, "ram byte at", RAM_BASE + i, g_shadow[i]);
    }
}

static void do_run(in_t *in)
{
    const uint32_t sel = byte(in);
    const uint32_t s = sel % (SLOTS + 1u);
    const uint32_t priv = (sel & 0x80u) ? PRIV_U : PRIV_S;
    const uint32_t off = (s == SLOTS) ? STRADDLE : s * 0x40u;
    const uint32_t vb = byte(in);
    /* Mostly a page that maps the code: a run that cannot fetch asks
     * nothing of the data side. */
    uint32_t code[NVA], ncode = 0u;

    for (uint32_t i = 0; i < g_nva; i++) {
        uint64_t pa;

        if (rv32_ref_translate(&g_ref, g_va[i], ACC_EXEC, priv, &pa) ==
                RV_EXC_NONE &&
            pa - ROM_BASE < ROM_SIZE) {
            code[ncode++] = g_va[i];
        }
    }
    uint32_t vbase;

    if (s == SLOTS && g_npair != 0u && (vb & 3u) != 0u) {
        vbase = g_pair[(vb >> 2) % g_npair];
    } else if (ncode != 0u && (vb & 3u) != 0u) {
        vbase = code[(vb >> 2) % ncode];
    } else if (g_nva == 0u || (vb & 0xF0u) == 0xF0u) {
        vbase = word(in) & ~0xFFFu;
    } else {
        vbase = g_va[vb % g_nva];
    }
    const uint32_t va = vbase + off;
    uint32_t x[32];

    g_run_va = va;
    g_run_priv = priv;
    g_st.runs++;
    for (unsigned i = 0; i < 32u; i++) {
        x[i] = 0x9E3779B9u * (i + 13u * (unsigned)g_st.runs);
    }
    x[0] = 0u;
    x[10] = pick_va(in);
    memcpy(E.x, x, sizeof(x));

    uint8_t tables[4u * PG];

    memcpy(tables, g_shadow, sizeof(tables));
    ref_run(va, priv);
    for (unsigned k = 0; k < 2u; k++) {
        run_side(&g_side[k], va, priv, x);
    }
    /*
     * A side still running at the budget gets a long one more, so that a
     * JIT looping where the interpreter stops is a finding, not a run set
     * aside: only what neither finishes is.
     */
    for (unsigned k = 0; k < 2u; k++) {
        rv_hart_t *const h = &g_side[k].h;
        uint32_t more = 0u;

        rv_backend = g_side[k].backend;
        while (more < 16384u && h->state == EMU_STATE_RUNNING) {
            uint32_t n = 0u;

            (void)g_side[k].backend->run((emu_cpu_t *)h, 64u, &n);
            more += n + 1u;
        }
    }
    if (g_side[0].h.state != EMU_STATE_HALTED &&
        g_side[1].h.state != EMU_STATE_HALTED) {
        /*
         * Not finished at the budget, which only code the reference does
         * not model can do (a loop of joined halves): a JIT stops between
         * blocks, so the two stop at different instructions, as
         * fuzz_harness.h says. Set aside, and both sides put back in step
         * on the interpreter's memory, fenced, before the next step.
         */
        if (E.modelled) {
            fail("both", "unfinished run the reference modelled", 0u, 1u);
            abort();
        }
        g_st.unfinished++;
        memcpy(g_shadow, g_side[0].ram, RAM_SIZE);
        memcpy(g_side[1].ram, g_side[0].ram, RAM_SIZE);
        fence_both();
        return;
    }
    if (E.modelled) {
        g_st.compared++;
        compare_one(&g_side[0], "interp");
        compare_one(&g_side[1], "jit");
    } else {
        /* What the reference does not model: the two backends agree. */
        g_st.differential++;
        memcpy(g_shadow, g_side[0].ram, RAM_SIZE);
        E.mcause = g_side[0].h.mcause;
        E.mepc = g_side[0].h.mepc;
        E.mtval = g_side[0].h.mtval;
        memcpy(E.x, g_side[0].h.x, sizeof(E.x));
        compare_one(&g_side[1], "jit, against interp,");
    }
    if (g_fails != 0u) {
        abort();
    }
    /*
     * A store into a table page is a PTE write, and software fences after
     * one -- and only then: a fence moves the JIT's generation, and
     * fencing after every run left no block to be found again.
     */
    if (memcmp(tables, g_shadow, sizeof(tables)) != 0) {
        fence_both();
    }
}

static void do_step(in_t *in)
{
    const uint32_t op = byte(in) % 16u;

    g_st.steps++;
    switch (op) {
    case 0: /* satp: Bare, or either root */
    {
        const uint32_t b = byte(in);
        const uint32_t root = (RAM_BASE >> 12) + (b & 1u);

        csr_both(CSR_SATP, (b & 2u) ? root | SATP_MODE_SV32 : 0u);
        break;
    }
    case 1: /* SUM and MXR */
    {
        const uint32_t b = byte(in);
        uint32_t m = g_ref.mstatus & ~(MSTATUS_SUM | MSTATUS_MXR);

        m |= (b & 1u) ? MSTATUS_SUM : 0u;
        m |= (b & 2u) ? MSTATUS_MXR : 0u;
        csr_both(CSR_MSTATUS, m);
        break;
    }
    case 2: /* a PMP entry, unlocked, then the fence software owes it */
    {
        const uint32_t i = byte(in) % 15u;
        const uint32_t cfg = byte(in) & 0x1Fu;
        const uint32_t sel = byte(in);
        uint32_t addr;

        switch (sel & 3u) {
        case 0: /* NAPOT over part of RAM or of either ROM page */
            addr =
                (((sel & 0x80u) ? ROM_BASE + ((sel >> 2) & 1u) * PG
                                : RAM_BASE + ((sel >> 2) % RAM_PAGES) * PG) >>
                 2) |
                (((PG >> 3) - 1u) >> ((sel >> 5) & 3u));
            break;
        case 1: /* a TOR bound on a ROM or RAM page, or a half page */
            addr = (((sel & 0x80u) ? ROM_BASE : RAM_BASE) +
                    ((sel >> 2) % 4u) * PG + ((sel & 0x40u) ? PG / 2u : 0u)) >>
                   2;
            break;
        case 2:
            addr = 0xFFFFFFFFu;
            break;
        default:
            addr = word(in);
            break;
        }
        csr_both(CSR_PMPADDR0 + i, addr);
        const uint32_t w = i / 4u, sh = 8u * (i % 4u);

        csr_both(CSR_PMPCFG0 + w,
                 (g_ref.pmpcfg[w] & ~(0xFFu << sh)) | (cfg << sh));
        fence_both();
        break;
    }
    case 3: /* a mapping changed, then fenced */
        add_mapping(in);
        fence_both();
        break;
    case 4: /* the code at two consecutive pages, then fenced */
        add_code_pair(in);
        fence_both();
        break;
    default:
        do_run(in);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The slots                                                           */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* A load or store through x10, with rd x12-x15 or rs2 x11. */
static uint32_t slot_insn(in_t *in)
{
    const uint32_t b = byte(in);
    const int32_t imm = (int32_t)(byte(in) & 0x0Fu) - 8;
    static const uint8_t k_f3[8] = {0u, 1u, 2u, 4u, 5u, 0u, 1u, 2u};
    const uint32_t f3 = k_f3[b & 7u];

    if ((b & 7u) < 5u) {
        return ((uint32_t)(imm & 0xFFF) << 20) | (10u << 15) | (f3 << 12) |
               ((12u + ((b >> 3) & 3u)) << 7) | 0x03u;
    }
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | (11u << 20) | (10u << 15) |
           (f3 << 12) | ((uint32_t)(imm & 31) << 7) | 0x23u;
}

static void build_rom(in_t *in)
{
    /*
     * Page 0: a slot every 64 bytes, ECALL at every other word, and the
     * straddling slot's lower half at its last halfword. Page 1: that
     * slot's upper half, then ECALL at every halfword offset 2 mod 4, so
     * the instruction after the straddle is an ECALL.
     */
    memset(g_rom, 0, ROM_SIZE);
    for (uint32_t o = 0; o + 4u <= PG - 4u; o += 4u) {
        put32(&g_rom[o], ECALL);
    }
    for (uint32_t o = PG + 2u; o + 4u <= ROM_SIZE; o += 4u) {
        put32(&g_rom[o], ECALL);
    }
    for (unsigned s = 0; s < SLOTS; s++) {
        put32(&g_rom[s * 0x40u], slot_insn(in));
    }
    const uint32_t st = slot_insn(in);

    put16(&g_rom[STRADDLE], st);
    put16(&g_rom[PG], st >> 16);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    in_t in = {data, size};
    uint32_t seed = 0x9E3779B9u;

    if (!once) {
        once = true;
        put32(&g_mrom[0], MRET);
        for (uint32_t o = 4u; o < PG; o += 4u) {
            put32(&g_mrom[o], ECALL);
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
        if (rv_backend_jit.init != NULL &&
            !rv_backend_jit.init((emu_cpu_t *)&g_side[1].h)) {
            fprintf(stderr, "fuzz-rv32-vm: the JIT did not start\n");
            abort();
        }
    }
    g_st.inputs++;
    g_fails = 0u;

    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        g_shadow[i] = (uint8_t)(seed >> 24);
    }
    memset(g_shadow, 0, 4u * PG); /* the table pages: every PTE invalid */
    memset(g_table_page, 0, sizeof(g_table_page));
    g_nva = 0u;
    g_npair = 0u;
    build_rom(&in);
    for (unsigned s = 0; s < 2u; s++) {
        build_side(&g_side[s]);
        if (g_side[s].backend->reset != NULL) {
            rv_backend = g_side[s].backend;
            g_side[s].backend->reset((emu_cpu_t *)&g_side[s].h);
        }
    }

    /* An OS's start: the last PMP entry granting everything below M,
     * Sv32 on the first root, and the input's mappings. */
    csr_both(CSR_PMPADDR0 + 15u, 0xFFFFFFFFu);
    csr_both(CSR_PMPCFG0 + 3u, (0x18u | 0x07u) << 24);
    csr_both(CSR_SATP, SATP_MODE_SV32 | (RAM_BASE >> 12));
    for (uint32_t n = 1u + byte(&in) % 8u; n > 0u; n--) {
        add_mapping(&in);
    }
    fence_both();

    for (unsigned s = 0; s < 96u && in.n > 0u; s++) {
        do_step(&in);
    }
    return 0;
}
