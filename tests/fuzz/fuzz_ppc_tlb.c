/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_ppc_tlb.c - the e200z7's TLB against a reference written from the
 * core reference manual.
 *
 * fuzz-ppc-jit compares the interpreter with the JIT, and both translate
 * with ppc_mmu.c -- a reading of chapter 10 shared by both, and agreed
 * with. This target asks a reference that shares none of it: the match
 * of figure 10-1 and table 10-1, the permissions of figure 10-3, the MAS
 * updates of table 10-15, and the register settings of tables 7-13,
 * 7-15, 7-23 and 7-24, from docs/ppc/e200z759CRM.pdf. For PowerPC, as for
 * G4MH, the manual is the only authority: no e200 is attached.
 *
 * And it asks what fuzz-ppc-jit cannot: whether a translated block
 * outlives what it was built under. The input's first bytes write up to
 * eight one-instruction programs ("slots") into ROM once; then each step
 * either changes the TLB -- an entry through MAS0-3 and tlbwe, tlbivax,
 * MAS4's defaults, a tlbsx -- or MSR's PR, IS and DS, or PID0, or runs a
 * slot with registers pointing near a page's ends. The JIT's blocks are
 * kept from step to step, with r1 -- whose page a block takes as its
 * RAM window -- chosen afresh each time, so a block built under one
 * mapping, mode or PID is found again under another.
 *
 * What the reference reads from the cores is what tlbre would return:
 * the entries as tlbwe stored them. Entries 0, 30 and 31 are the
 * harness's, invalidation-protected: 0 maps the vector table, so every
 * interrupt reaches its handler's `wait`; 30 and 31 map the code in both
 * address spaces, below anything the input writes in 1-29 (the lowest
 * entry wins, this core's choice for what the manual calls a
 * programming error). An input entry over the code or the vectors is
 * made the identity there, without VLE or E, so that what runs is the
 * program and not whatever an alias holds.
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
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_disasm.h"

#if PPC_TLB_ENTRIES != 32u
#error "fuzz-ppc-tlb expects 32 TLB entries"
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u
#define TRAP_BASE 0x20010000u
#define TRAP_SIZE 0x400u /* IVOR 0..63, 16 bytes each */
#define RAM_BASE 0x40000000u
#define RAM_SIZE 0x4000u
#define RAM2_BASE 0x50000000u
#define RAM2_SIZE 0x1000u
#define HOLE 0x70000000u /* nothing mapped */

#define WAIT 0x7C00007Cu
#define SLOTS 8u
#define SLOT_STRIDE 0x80u

#define SENT_SRR0 0x5EE00000u
#define SENT_SRR1 0x5EE00001u
#define SENT_ESR 0x5EE00002u
#define SENT_DEAR 0x5EE00003u

typedef struct side {
    const char *name;
    const emu_backend_t *backend;
    ppc_cpu_t cpu;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE], ram2[RAM2_SIZE];
} side_t;

static side_t g_side[2] = {
    {.name = "interp", .backend = &ppc_backend_interp},
    {.name = "jit", .backend = &ppc_backend_jit},
};
static uint8_t g_code[CODE_SIZE]; /* both sides' ROM */
static uint8_t g_trap[TRAP_SIZE];

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

static struct {
    uint32_t r[32];
    uint32_t pc, msr, srr0, srr1, esr, dear;
    uint32_t mas0, mas1, mas2, mas3, mas4, mas6, pid;
    uint32_t t1[32], t2[32], t3[32]; /* the TLB, as tlbre reads it */
    uint8_t ram[RAM_SIZE], ram2[RAM2_SIZE];
    bool unknown_regs; /* an lmw abandoned partway */
} R;

/* The slots: what each holds, for the reference and a report. */
static struct {
    unsigned kind;
    uint32_t insn;
    int32_t d;
    uint32_t rd;
} g_slot[SLOTS];
static unsigned g_nslots;

static struct {
    unsigned step, slot;
} g_cur;

static struct {
    uint64_t inputs, steps, runs, completed, dtlb, itlb, dsi, isi, align, tlbwe,
        tlbivax, tlbsx, xlated, le, split, translations, entries;
} g_st;

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-ppc-tlb: %llu inputs, %llu steps, %llu runs: %llu "
            "completed, %llu DTLB, %llu ITLB, %llu DSI, %llu ISI, %llu "
            "alignment; %llu accesses through a mapping that is not the "
            "identity, %llu to a little-endian page, %llu across a page; "
            "%llu tlbwe, %llu tlbivax, %llu tlbsx; jit %llu translations, "
            "%llu block entries\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.steps,
            (unsigned long long)g_st.runs, (unsigned long long)g_st.completed,
            (unsigned long long)g_st.dtlb, (unsigned long long)g_st.itlb,
            (unsigned long long)g_st.dsi, (unsigned long long)g_st.isi,
            (unsigned long long)g_st.align, (unsigned long long)g_st.xlated,
            (unsigned long long)g_st.le, (unsigned long long)g_st.split,
            (unsigned long long)g_st.tlbwe, (unsigned long long)g_st.tlbivax,
            (unsigned long long)g_st.tlbsx,
            (unsigned long long)g_st.translations,
            (unsigned long long)g_st.entries);
}

/* Table 10-1: TSIZE n is 2^n KB; above 22 the manual says nothing, and
 * such an entry matches nothing (the implementation's choice too). */
static bool page_mask(uint32_t mas1, uint32_t *mask)
{
    const uint32_t tsize = (mas1 >> 7) & 0x1Fu;

    if (tsize > 22u) {
        return false;
    }
    *mask = (uint32_t)~((1024ull << tsize) - 1ull);
    return true;
}

/* Figure 10-1: V, TS, TID 0 or PID, EPN on the page's bits; lowest wins. */
static int ref_lookup(uint32_t ea, uint32_t as, uint32_t pid)
{
    for (unsigned i = 0; i < 32u; i++) {
        const uint32_t m1 = R.t1[i];
        const uint32_t tid = (m1 >> 16) & 0xFFu;
        uint32_t mask;

        if ((m1 & 0x80000000u) == 0u || ((m1 >> 12) & 1u) != as ||
            (tid != 0u && tid != pid) || !page_mask(m1, &mask)) {
            continue;
        }
        if (((ea ^ R.t2[i]) & mask & 0xFFFFFC00u) == 0u) {
            return (int)i;
        }
    }
    return -1;
}

/* Table 10-15's "Instr/data TLB error" column. */
static void ref_tlb_error(uint32_t ea, uint32_t as)
{
    const uint32_t nv = R.mas0 & 0x1Fu;
    const uint32_t tidsel = (R.mas4 >> 16) & 3u;
    const uint32_t tid = (tidsel == 3u) ? 0u : R.pid;

    R.mas0 = (R.mas4 & 0x30000000u) | (nv << 16) | nv;
    R.mas1 = 0x80000000u | (tid << 16) | (as << 12) | (R.mas4 & 0xF80u);
    R.mas2 = (ea & 0xFFFFFC00u) | (R.mas4 & 0x3Fu);
    R.mas3 = 0u;
    R.mas6 = (R.pid << 16) | as;
}

/* Interrupt entry (7.7): which pair, MSR keeping CE, ME, DE and RI. */
static void ref_interrupt(unsigned ivor, uint32_t pc, uint32_t esr)
{
    R.srr0 = pc;
    R.srr1 = R.msr;
    R.esr = esr;
    R.msr &= PPC_MSR_CE | PPC_MSR_ME | PPC_MSR_DE | PPC_MSR_RI;
    R.pc = TRAP_BASE + ivor * 16u + 4u; /* parked one in, after `wait` */
    switch (ivor) {
    case PPC_IVOR_DTLB_ERROR:
        g_st.dtlb++;
        break;
    case PPC_IVOR_ITLB_ERROR:
        g_st.itlb++;
        break;
    case PPC_IVOR_DATA_STORAGE:
        g_st.dsi++;
        break;
    case PPC_IVOR_INST_STORAGE:
        g_st.isi++;
        break;
    default:
        g_st.align++;
        break;
    }
}

typedef enum { A_EXEC, A_READ, A_WRITE } acc_t;

/*
 * Figure 10-2 and 10-3: the real address, or the interrupt. A miss
 * updates the MAS registers and, for data, DEAR; a refusal only DEAR.
 */
static bool ref_xlate(uint32_t pc, uint32_t ea, acc_t acc, uint32_t *pa,
                      bool *le)
{
    const bool exec = acc == A_EXEC;
    const uint32_t as = (R.msr & (exec ? PPC_MSR_IS : PPC_MSR_DS)) ? 1u : 0u;
    const int i = ref_lookup(ea, as, R.pid);
    const bool user = (R.msr & PPC_MSR_PR) != 0u;

    if (i < 0) {
        ref_tlb_error(ea, as);
        if (!exec) {
            R.dear = ea;
        }
        ref_interrupt(exec ? PPC_IVOR_ITLB_ERROR : PPC_IVOR_DTLB_ERROR, pc,
                      (acc == A_WRITE) ? PPC_ESR_ST : 0u);
        return false;
    }
    static const uint32_t need[2][3] = {
        /* SX, SR, SW */ {0x10u, 0x01u, 0x04u},
        /* UX, UR, UW */ {0x20u, 0x02u, 0x08u},
    };
    if ((R.t3[i] & need[user ? 1 : 0][acc]) == 0u) {
        if (!exec) {
            R.dear = ea;
        }
        ref_interrupt(exec ? PPC_IVOR_INST_STORAGE : PPC_IVOR_DATA_STORAGE, pc,
                      (acc == A_WRITE) ? PPC_ESR_ST : 0u);
        return false;
    }
    uint32_t mask;

    (void)page_mask(R.t1[i], &mask);
    *pa = (R.t3[i] & 0xFFFFFC00u & mask) | (ea & ~mask);
    *le = (R.t2[i] & 1u) != 0u;
    g_st.xlated += *pa != ea;
    return true;
}

/* The bus: RAM, RAM2, the code and vectors read-only, nothing else. */
static uint8_t *ref_mem(uint32_t pa, bool write)
{
    if (pa - RAM_BASE < RAM_SIZE) {
        return &R.ram[pa - RAM_BASE];
    }
    if (pa - RAM2_BASE < RAM2_SIZE) {
        return &R.ram2[pa - RAM2_BASE];
    }
    if (!write && pa - CODE_BASE < CODE_SIZE) {
        return &g_code[pa - CODE_BASE];
    }
    if (!write && pa - TRAP_BASE < TRAP_SIZE) {
        return &g_trap[pa - TRAP_BASE];
    }
    return NULL;
}

/* A bus error on a load or store is a DSI with DEAR at the access. */
static void ref_bus_error(uint32_t pc, uint32_t ea, bool store)
{
    R.dear = ea;
    ref_interrupt(PPC_IVOR_DATA_STORAGE, pc, store ? PPC_ESR_ST : 0u);
}

/*
 * One load of `size` bytes, as the core makes it (3.4): aligned, one
 * translation; unaligned, a byte at a time, each translated, most
 * significant first, and a page with the other byte order after the
 * first is a byte-ordering DSI (7.7.3) at that byte.
 */
static bool ref_load(uint32_t pc, uint32_t ea, uint32_t size, uint32_t *out)
{
    uint32_t pa, v = 0u;
    bool le;

    if ((ea & (size - 1u)) == 0u) {
        if (!ref_xlate(pc, ea, A_READ, &pa, &le)) {
            return false;
        }
        const uint8_t *const p = ref_mem(pa, false);

        if (p == NULL || ref_mem(pa + size - 1u, false) == NULL) {
            ref_bus_error(pc, ea, false);
            return false;
        }
        for (uint32_t i = 0; i < size; i++) {
            v = le ? v | ((uint32_t)p[i] << (8u * i)) : (v << 8) | p[i];
        }
        g_st.le += le;
        *out = v;
        return true;
    }
    bool le0 = false;

    if (((ea ^ (ea + size - 1u)) & ~0x3FFu) != 0u) {
        g_st.split++;
    }
    for (uint32_t i = 0; i < size; i++) {
        bool le_i;

        if (!ref_xlate(pc, ea + i, A_READ, &pa, &le_i)) {
            return false;
        }
        if (i == 0u) {
            le0 = le_i;
        } else if (le_i != le0) {
            R.dear = ea + i;
            ref_interrupt(PPC_IVOR_DATA_STORAGE, pc, PPC_ESR_BO);
            return false;
        }
        const uint8_t *const p = ref_mem(pa, false);

        if (p == NULL) {
            ref_bus_error(pc, ea + i, false);
            return false;
        }
        v = le0 ? v | ((uint32_t)*p << (8u * i)) : (v << 8) | *p;
    }
    *out = v;
    return true;
}

/*
 * One store: aligned, one translation; unaligned, every byte translated
 * before any is written -- this core's reading of "the instruction did
 * not happen", which the manual does not spell out for a store across a
 * page -- and a bus error stops it where it is.
 */
static bool ref_store(uint32_t pc, uint32_t ea, uint32_t size, uint32_t val)
{
    uint32_t pa[4];
    bool le = false;

    if ((ea & (size - 1u)) == 0u) {
        if (!ref_xlate(pc, ea, A_WRITE, &pa[0], &le)) {
            return false;
        }
        uint8_t *const p = ref_mem(pa[0], true);

        if (p == NULL || ref_mem(pa[0] + size - 1u, true) == NULL) {
            ref_bus_error(pc, ea, true);
            return false;
        }
        for (uint32_t i = 0; i < size; i++) {
            p[i] = (uint8_t)(val >> (8u * (le ? i : size - 1u - i)));
        }
        g_st.le += le;
        return true;
    }
    if (((ea ^ (ea + size - 1u)) & ~0x3FFu) != 0u) {
        g_st.split++;
    }
    for (uint32_t i = 0; i < size; i++) {
        bool le_i;

        if (!ref_xlate(pc, ea + i, A_WRITE, &pa[i], &le_i)) {
            return false;
        }
        if (i == 0u) {
            le = le_i;
        } else if (le_i != le) {
            R.dear = ea + i;
            ref_interrupt(PPC_IVOR_DATA_STORAGE, pc, PPC_ESR_ST | PPC_ESR_BO);
            return false;
        }
    }
    for (uint32_t i = 0; i < size; i++) {
        uint8_t *const p = ref_mem(pa[i], true);

        if (p == NULL) {
            ref_bus_error(pc, ea + i, true);
            return false;
        }
        *p = (uint8_t)(val >> (8u * (le ? i : size - 1u - i)));
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Instructions                                                        */
/* ------------------------------------------------------------------ */

enum {
    K_LWZ,
    K_LBZ,
    K_LHZ,
    K_LHA,
    K_STW,
    K_STB,
    K_STH,
    K_LMW,
    K_STMW,
    K_COUNT
};

static const uint8_t k_opcd[K_COUNT] = {32, 34, 40, 42, 36, 38, 44, 46, 47};

/* The instruction in a slot, at `pc`: rA is r3, rS r4 or rD from rd. */
static bool ref_exec(unsigned s, uint32_t pc)
{
    const unsigned k = g_slot[s].kind;
    const uint32_t ea = R.r[3] + (uint32_t)g_slot[s].d;
    const uint32_t rd = g_slot[s].rd;
    uint32_t v = 0u;

    switch (k) {
    case K_LWZ:
    case K_LBZ:
    case K_LHZ:
    case K_LHA: {
        const uint32_t size = (k == K_LWZ) ? 4u : (k == K_LBZ) ? 1u : 2u;

        if (!ref_load(pc, ea, size, &v)) {
            return false;
        }
        R.r[rd] = (k == K_LHA) ? (uint32_t)(int32_t)(int16_t)v : v;
        return true;
    }
    case K_STW:
    case K_STB:
    case K_STH:
        return ref_store(pc, ea,
                         (k == K_STW)   ? 4u
                         : (k == K_STB) ? 1u
                                        : 2u,
                         R.r[4]);
    default: { /* lmw, stmw: word aligned or an alignment interrupt */
        const bool st = k == K_STMW;

        if ((ea & 3u) != 0u) {
            R.dear = ea;
            ref_interrupt(PPC_IVOR_ALIGNMENT, pc, st ? PPC_ESR_ST : 0u);
            return false;
        }
        uint32_t got[32];

        for (uint32_t r = rd; r < 32u; r++) {
            const uint32_t a = ea + 4u * (r - rd);

            if (st ? !ref_store(pc, a, 4u, R.r[r])
                   : !ref_load(pc, a, 4u, &got[r])) {
                R.unknown_regs = !st && r != rd;
                return false;
            }
        }
        if (!st) {
            for (uint32_t r = rd; r < 32u; r++) {
                R.r[r] = got[r];
            }
        }
        return true;
    }
    }
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

static uint32_t half(in_t *in)
{
    return byte(in) | (byte(in) << 8);
}

static uint32_t word(in_t *in)
{
    return half(in) | (half(in) << 16);
}

/* The boundary the last pair of pages built (case 14), if any. */
static uint32_t g_hot_ea;

/* An effective address near where pages are. */
static uint32_t pick_ea(in_t *in)
{
    const uint32_t sel = byte(in);
    const uint32_t k = half(in);

    switch (sel % 8u) {
    case 0:
    case 1:
        return RAM_BASE + (k % RAM_SIZE);
    case 2:
        return 0x60000000u + (k % 0x4000u); /* only through a mapping */
    case 3:
        return RAM2_BASE + (k % RAM2_SIZE);
    case 4: /* an end of an input entry's page */
    {
        const unsigned i = 1u + k % 29u;
        uint32_t mask;

        if (!page_mask(R.t1[i], &mask)) {
            return RAM_BASE;
        }
        return (R.t2[i] & mask) + ((sel & 0x80u) ? ~mask + 1u : 0u) - 4u +
               ((k >> 5) & 7u);
    }
    case 5:
        return CODE_BASE + (k % CODE_SIZE);
    case 6:
        if (g_hot_ea != 0u && (sel & 0x80u) != 0u) {
            return g_hot_ea - 4u + (k & 7u); /* across a pair's boundary */
        }
        return 0xFFFFFC00u + (k & 0x3FFu);
    default:
        return word(in);
    }
}

static uint32_t pick_pa(in_t *in, uint32_t epn)
{
    const uint32_t sel = byte(in);
    const uint32_t k = byte(in);

    switch (sel % 6u) {
    case 0:
        return epn;
    case 1:
    case 2:
        return RAM_BASE + (k % 16u) * 0x400u;
    case 3:
        return RAM2_BASE + (k % 4u) * 0x400u;
    case 4:
        return CODE_BASE;
    default:
        return HOLE;
    }
}

/* ------------------------------------------------------------------ */
/* The two cores                                                       */
/* ------------------------------------------------------------------ */

/* What tlbre would return for every entry: the TLB as tlbwe left it. */
static void read_back(void)
{
    for (unsigned i = 0; i < 32u; i++) {
        const ppc_cpu_t *const a = &g_side[0].cpu;
        const ppc_cpu_t *const b = &g_side[1].cpu;

        R.t1[i] = a->tlb[i].mas1;
        R.t2[i] = a->tlb[i].mas2;
        R.t3[i] = a->tlb[i].mas3;
        if (b->tlb[i].mas1 != R.t1[i] || b->tlb[i].mas2 != R.t2[i] ||
            b->tlb[i].mas3 != R.t3[i]) {
            fprintf(stderr, "fuzz-ppc-tlb: the two TLBs differ\n");
            abort();
        }
    }
}

/* Software's MAS registers, read back after it wrote them. */
static void read_mas(void)
{
    const ppc_cpu_t *const c = &g_side[0].cpu;

    R.mas0 = c->mas0;
    R.mas1 = c->mas1;
    R.mas2 = c->mas2;
    R.mas3 = c->mas3;
    R.mas4 = c->mas4;
    R.mas6 = c->mas6;
}

/* An SPR written as the supervisor writes it, whatever mode the last
 * run left the core in: these are privileged, and a refusal here is the
 * harness's mistake, not a finding. */
static void spr_both(uint32_t spr, uint32_t v)
{
    for (unsigned s = 0; s < 2u; s++) {
        ppc_cpu_t *const c = &g_side[s].cpu;
        const uint32_t msr = c->msr;

        c->msr = 0u;
        if (ppc_spr_write(c, spr, v) != PPC_EXC_NONE) {
            fprintf(stderr, "fuzz-ppc-tlb: mtspr %u refused\n", spr);
            abort();
        }
        c->msr = msr;
    }
}

/*
 * An instruction the translator declines, run as the interpreter runs
 * it -- and then what the JIT does after any interpreted instruction:
 * recompute the context a block is keyed on.
 */
static void after_system_insn(void)
{
    for (unsigned s = 0; s < 2u; s++) {
        ppc_cpu_t *const c = &g_side[s].cpu;

        c->jit_ctx = ppc_cpu_ctx(c);
    }
}

static void tlbwe_both(unsigned i, uint32_t m1, uint32_t m2, uint32_t m3)
{
    spr_both(PPC_SPR_MAS0, PPC_MAS0_TLBSEL1 | (i << PPC_MAS0_ESEL_SHIFT));
    spr_both(PPC_SPR_MAS1, m1);
    spr_both(PPC_SPR_MAS2, m2);
    spr_both(PPC_SPR_MAS3, m3);
    for (unsigned s = 0; s < 2u; s++) {
        ppc_tlbwe(&g_side[s].cpu);
    }
    after_system_insn();
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void build_side(side_t *sd)
{
    ppc_cpu_t *const c = &sd->cpu;
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };
    const emu_region_t ram2 = {
        .base = RAM2_BASE,
        .size = RAM2_SIZE,
        .host = sd->ram2,
        .name = "ram2",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, R.ram, RAM_SIZE);
    memcpy(sd->ram2, R.ram2, RAM2_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) || !emu_bus_add(&sd->bus, &ram2) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE)) {
        fprintf(stderr, "fuzz-ppc-tlb: could not build the bus\n");
        abort();
    }
    ppc_cpu_init(c, &sd->bus, 0u);
    ppc_cpu_reset(c, CODE_BASE);
    c->vle = false;
    ppc_mmu_reset(c);
    c->ivpr = TRAP_BASE;
    for (uint32_t i = 0; i < PPC_IVOR_COUNT; i++) {
        c->ivor[i] = i * 16u;
    }
    ppc_cpu_set_msr(c, 0u);
    c->irq_dirty = false;
}

static unsigned g_fails;

static void dump(void)
{
    char buf[96];
    const uint32_t pc = CODE_BASE + g_cur.slot * SLOT_STRIDE;

    ppc_disasm(buf, sizeof(buf), pc, g_slot[g_cur.slot].insn, 4u);
    fprintf(stderr, "  step %u: slot %u at 0x%08x: %s; MSR 0x%08x PID %u\n",
            g_cur.step, g_cur.slot, pc, buf,
            R.srr1 == SENT_SRR1 ? R.msr : R.srr1, R.pid);
    for (unsigned i = 0; i < 32u; i++) {
        if ((R.t1[i] & 0x80000000u) != 0u) {
            fprintf(stderr, "  tlb %2u  mas1 0x%08x mas2 0x%08x mas3 0x%08x\n",
                    i, R.t1[i], R.t2[i], R.t3[i]);
        }
    }
}

static void expect(const side_t *sd, const char *what, unsigned idx,
                   uint32_t got, uint32_t want)
{
    if (got == want) {
        return;
    }
    if (g_fails++ == 0u) {
        fprintf(stderr, "fuzz-ppc-tlb: %s disagrees with the manual\n",
                sd->name);
        dump();
    }
    fprintf(stderr, "  %-6s %-6s[%2u] got 0x%08x want 0x%08x\n", sd->name, what,
            idx, got, want);
}

static void run_side(side_t *sd, uint32_t pc, uint32_t msr,
                     const uint32_t *regs)
{
    ppc_cpu_t *const c = &sd->cpu;
    emu_jit_stats_t before, after;
    uint32_t total = 0u;

    memcpy(c->r, regs, sizeof(c->r));
    c->pc = pc;
    ppc_cpu_set_msr(c, msr);
    c->srr0 = SENT_SRR0;
    c->srr1 = SENT_SRR1;
    c->esr = SENT_ESR;
    c->dear = SENT_DEAR;
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = false;
    c->jit_ctx = ppc_cpu_ctx(c);

    ppc_backend = sd->backend;
    emu_jit_get_stats(&before);
    while (total < 64u && c->state == EMU_STATE_RUNNING) {
        uint32_t n = 0u;

        (void)sd->backend->run((emu_cpu_t *)c, 16u, &n);
        total += n + 1u;
    }
    emu_jit_get_stats(&after);
    if (sd == &g_side[1]) {
        g_st.translations += after.translations - before.translations;
        g_st.entries += after.block_entries - before.block_entries;
    }
}

static void compare(const side_t *sd)
{
    const ppc_cpu_t *const c = &sd->cpu;

    expect(sd, "state", 0u, (uint32_t)c->state, (uint32_t)EMU_STATE_WFI);
    expect(sd, "pc", 0u, c->pc, R.pc);
    expect(sd, "msr", 0u, c->msr, R.msr);
    expect(sd, "srr0", 0u, c->srr0, R.srr0);
    expect(sd, "srr1", 0u, c->srr1, R.srr1);
    expect(sd, "esr", 0u, c->esr, R.esr);
    expect(sd, "dear", 0u, c->dear, R.dear);
    expect(sd, "mas0", 0u, c->mas0, R.mas0);
    expect(sd, "mas1", 0u, c->mas1, R.mas1);
    expect(sd, "mas2", 0u, c->mas2, R.mas2);
    expect(sd, "mas3", 0u, c->mas3, R.mas3);
    expect(sd, "mas6", 0u, c->mas6, R.mas6);
    for (unsigned i = 0; i < 32u; i++) {
        if (R.unknown_regs && i >= g_slot[g_cur.slot].rd) {
            continue;
        }
        expect(sd, "r", i, c->r[i], R.r[i]);
    }
    for (uint32_t i = 0; i < RAM_SIZE; i += 4u) {
        if (memcmp(&sd->ram[i], &R.ram[i], 4u) != 0) {
            uint32_t a, b;

            memcpy(&a, &sd->ram[i], 4u);
            memcpy(&b, &R.ram[i], 4u);
            expect(sd, "ram@", 0u, RAM_BASE + i, 0u);
            expect(sd, "word", 0u, a, b);
            break;
        }
    }
    if (memcmp(sd->ram2, R.ram2, RAM2_SIZE) != 0) {
        expect(sd, "ram2", 0u, 1u, 0u);
    }
}

static void do_run(in_t *in)
{
    const unsigned s = byte(in) % g_nslots;
    const uint32_t pc = CODE_BASE + s * SLOT_STRIDE;
    const uint32_t msr = R.msr;
    uint32_t regs[32];

    g_cur.slot = s;
    g_st.runs++;
    for (unsigned i = 0; i < 32u; i++) {
        R.r[i] = 0x9E3779B9u * (i + 17u * (unsigned)g_cur.step);
    }
    R.r[3] = pick_ea(in);
    R.r[1] = (byte(in) & 1u) ? R.r[3] : pick_ea(in); /* the block's window */
    memcpy(regs, R.r, sizeof(regs));

    R.srr0 = SENT_SRR0;
    R.srr1 = SENT_SRR1;
    R.esr = SENT_ESR;
    R.dear = SENT_DEAR;
    R.unknown_regs = false;
    {
        uint32_t pa;
        bool le;

        const bool fetched = ref_xlate(pc, pc, A_EXEC, &pa, &le);

        /* The reference runs the slot it wrote: the fetch must reach it. */
        if (fetched && pa != pc) {
            fprintf(stderr,
                    "fuzz-ppc-tlb: the harness let slot code "
                    "translate to 0x%08x\n",
                    pa);
            abort();
        }
        if (fetched && ref_exec(s, pc)) {
            if (ref_xlate(pc + 4u, pc + 4u, A_EXEC, &pa, &le)) {
                R.pc = pc + 8u; /* the `wait` completed */
                g_st.completed++;
            }
        }
    }
    for (unsigned k = 0; k < 2u; k++) {
        run_side(&g_side[k], pc, msr, regs);
        compare(&g_side[k]);
    }
    if (g_fails != 0u) {
        abort();
    }
    /* Both cores now hold the reference's state; the mode is the
     * input's, restored for the next run. */
    R.msr = msr;
}

/*
 * Over the code or the vectors, an entry is the identity, Book E and
 * big-endian, whoever writes it: what a slot runs must be the slot.
 */
static void make_safe(uint32_t m1, uint32_t *m2, uint32_t *m3)
{
    uint32_t mask;

    if (!page_mask(m1, &mask)) {
        return;
    }
    const uint32_t lo = *m2 & mask & 0xFFFFFC00u;
    const uint64_t hi = (uint64_t)lo + (uint64_t)(uint32_t)~mask;

    if ((uint64_t)CODE_BASE <= hi && lo < TRAP_BASE + TRAP_SIZE) {
        *m2 &= ~(PPC_MAS2_VLE | PPC_MAS2_E);
        *m3 = (*m3 & 0x3FFu) | lo;
    }
}

/* An entry the input writes, made safe over the code and the vectors. */
static void do_tlbwe(in_t *in)
{
    const unsigned i = 1u + byte(in) % 29u;
    const uint32_t sel = byte(in);
    const uint32_t ts = (sel >> 5) & 3u;
    const uint32_t tsize = (ts == 0u)   ? 0u
                           : (ts == 1u) ? 1u + (sel & 1u)
                           : (ts == 2u) ? 3u + (sel & 7u)
                                        : ((sel & 1u) ? 22u : 12u + (sel & 7u));
    const uint32_t ea = pick_ea(in);
    const uint32_t bits = byte(in);
    uint32_t mask;
    uint32_t m1 = ((bits & 0x80u) ? 0u : 0x80000000u) |
                  ((bits & 0x40u) ? 0x40000000u : 0u) |
                  (((bits & 0x20u) ? 1u + (bits & 3u) % 3u : 0u) << 16) |
                  ((bits & 0x10u) ? 0x1000u : 0u) | (tsize << 7);
    uint32_t m2, m3;

    (void)page_mask(m1, &mask);
    m2 = (ea & mask & 0xFFFFFC00u) | ((bits & 0x0Cu) == 0x0Cu ? 1u : 0u) |
         (byte(in) & 0x1Eu);
    m3 = (pick_pa(in, ea & mask) & 0xFFFFFC00u) | (byte(in) & 0x3Fu);
    make_safe(m1, &m2, &m3);
    tlbwe_both(i, m1, m2, m3);
    g_st.tlbwe++;
    read_back();
    read_mas();
}

static void do_step(in_t *in)
{
    const uint32_t op = byte(in);

    g_cur.step++;
    g_st.steps++;
    switch (op % 24u) {
    case 0:
    case 1:
    case 2:
    case 3:
        do_tlbwe(in);
        break;
    case 15: /* MSR[DS] alone: the data side's address space, nothing else */
        R.msr ^= PPC_MSR_DS;
        break;
    case 4: { /* tlbivax: one page, or all; IPROT survives */
        const uint32_t ea = pick_ea(in) & ~0x7u;
        const uint32_t all = (byte(in) & 0x0Fu) ? 0u : 4u;

        for (unsigned s = 0; s < 2u; s++) {
            ppc_tlbivax(&g_side[s].cpu, ea | all);
        }
        after_system_insn();
        g_st.tlbivax++;
        read_back();
        break;
    }
    case 5: /* MSR: PR, IS, DS */
    {
        const uint32_t b = byte(in);

        R.msr = ((b & 1u) ? PPC_MSR_PR : 0u) | ((b & 2u) ? PPC_MSR_IS : 0u) |
                ((b & 4u) ? PPC_MSR_DS : 0u);
        break;
    }
    case 6: /* PID0 */
        R.pid = byte(in) & 3u;
        spr_both(PPC_SPR_PID0, R.pid);
        after_system_insn();
        break;
    case 7: /* MAS4's defaults, and MAS6 for tlbsx */
        spr_both(PPC_SPR_MAS4, word(in));
        spr_both(PPC_SPR_MAS6, word(in));
        read_mas();
        break;
    case 8: { /* tlbsx: table 10-15's hit and miss columns */
        const uint32_t ea = pick_ea(in);
        const uint32_t as = R.mas6 & 1u;
        const uint32_t spid = (R.mas6 >> 16) & 0xFFu;
        const int i = ref_lookup(ea, as, spid);
        const uint32_t nv = R.mas0 & 0x1Fu;

        for (unsigned s = 0; s < 2u; s++) {
            ppc_tlbsx(&g_side[s].cpu, ea);
        }
        after_system_insn();
        g_st.tlbsx++;
        if (i >= 0) {
            R.mas0 = 0x10000000u | ((uint32_t)i << 16) | nv;
            R.mas1 = R.t1[i];
            R.mas2 = R.t2[i];
            R.mas3 = R.t3[i];
        } else {
            R.mas0 = (R.mas4 & 0x30000000u) | (nv << 16) | nv;
            R.mas1 = (spid << 16) | (as << 12) | (R.mas4 & 0xF80u);
            R.mas2 = (ea & 0xFFFFFC00u) | (R.mas4 & 0x3Fu);
            R.mas3 = 0u;
        }
        for (unsigned s = 0; s < 2u; s++) {
            const ppc_cpu_t *const c = &g_side[s].cpu;

            expect(&g_side[s], "mas0", 0u, c->mas0, R.mas0);
            expect(&g_side[s], "mas1", 0u, c->mas1, R.mas1);
            expect(&g_side[s], "mas2", 0u, c->mas2, R.mas2);
            expect(&g_side[s], "mas3", 0u, c->mas3, R.mas3);
        }
        if (g_fails != 0u) {
            abort();
        }
        break;
    }
    case 9: { /* the code's own entry in one address space, taken away
               * or made private: what an ITLB error needs */
        const unsigned i = 30u + (byte(in) & 1u);
        const uint32_t b = byte(in);
        const uint32_t m1 = ((b & 1u) ? 0u : 0x80000000u) |
                            ((i == 31u) ? 0x1000u : 0u) |
                            (((b >> 1) & 3u) << 16) | (2u << 7);

        tlbwe_both(i, m1, CODE_BASE, CODE_BASE | 0x33u);
        g_st.tlbwe++;
        read_back();
        read_mas();
        break;
    }
    case 10:
    case 11: { /* a data page a program would have: read-write, shared */
        const unsigned i = 1u + byte(in) % 29u;
        const uint32_t b = byte(in);
        const uint32_t tsize = 1u + (b & 3u); /* 2 KB to 16 KB */
        const uint32_t ea =
            ((b & 0x10u) ? 0x60000000u : RAM_BASE) + ((half(in) % 16u) << 10);
        const uint32_t mask = (uint32_t)~((1024u << tsize) - 1u);
        const uint32_t pa =
            (b & 0x20u) ? RAM_BASE + ((byte(in) % 16u) << 10) : (ea & mask);

        tlbwe_both(i,
                   0x80000000u | ((R.msr & PPC_MSR_DS) ? 0x1000u : 0u) |
                       (tsize << 7),
                   ea & mask, (pa & mask) | 0x0Fu | ((b & 0x40u) ? 0u : 0x30u));
        g_st.tlbwe++;
        read_back();
        read_mas();
        break;
    }
    case 12: { /* a private data page: this PID, this address space */
        const unsigned i = 1u + byte(in) % 29u;
        const uint32_t b = byte(in);
        const uint32_t tid = (R.pid != 0u) ? R.pid : 1u + (b & 1u);
        const uint32_t ea = RAM_BASE + ((b % 16u) << 10);

        tlbwe_both(i,
                   0x80000000u | (tid << 16) |
                       ((R.msr & PPC_MSR_DS) ? 0x1000u : 0u) | (0u << 7),
                   ea, ea | 0x0Fu | ((b & 0x40u) ? 0u : 0x30u));
        g_st.tlbwe++;
        read_back();
        read_mas();
        break;
    }
    case 13: { /* a smaller page inside another, at a lower entry */
        const unsigned i = 2u + byte(in) % 28u;
        const unsigned j = 1u + byte(in) % (i - 1u);
        const uint32_t b = byte(in);
        uint32_t mask;

        if ((R.t1[i] & 0x80000000u) == 0u || !page_mask(R.t1[i], &mask) ||
            ((R.t1[i] >> 7) & 0x1Fu) == 0u) {
            break;
        }
        const uint32_t tsize = (b & 1u) % ((R.t1[i] >> 7) & 0x1Fu);
        const uint32_t sub = (uint32_t)(1024u << tsize);
        const uint32_t lo = R.t2[i] & mask & 0xFFFFFC00u;
        const uint32_t ea = lo + ((byte(in) * sub) & ~mask);

        uint32_t m2 = ea;
        uint32_t m3 = (RAM_BASE + ((b >> 1) % 16u) * 0x400u) |
                      ((b & 0x20u) ? 0x3Fu : 0x15u);
        const uint32_t m1 = (R.t1[i] & 0x80FF1000u) | (tsize << 7);

        make_safe(m1, &m2, &m3);
        tlbwe_both(j, m1, m2, m3);
        g_st.tlbwe++;
        read_back();
        read_mas();
        break;
    }
    case 14: { /* two 1 KB pages end to end, one little-endian */
        const unsigned i = 1u + byte(in) % 28u;
        const uint32_t b = byte(in);
        const uint32_t ea =
            ((b & 0x10u) ? 0x60000000u : RAM_BASE) + ((byte(in) % 15u) << 10);
        const uint32_t ts = (R.msr & PPC_MSR_DS) ? 0x1000u : 0u;
        const uint32_t pa = RAM_BASE + ((b % 15u) << 10);

        tlbwe_both(i, 0x80000000u | ts, ea | ((b & 0x20u) ? 1u : 0u),
                   pa | 0x3Fu);
        tlbwe_both(i + 1u, 0x80000000u | ts,
                   (ea + 0x400u) | ((b & 0x20u) ? 0u : 1u),
                   (pa + 0x400u) | 0x3Fu);
        g_hot_ea = ea + 0x400u;
        g_st.tlbwe += 2u;
        read_back();
        read_mas();
        break;
    }
    default:
        do_run(in);
        break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    in_t in = {data, size};
    uint32_t seed = 0x2545F491u;

    if (!once) {
        once = true;
        for (uint32_t i = 0; i < TRAP_SIZE; i += 4u) {
            put32be(&g_trap[i], WAIT);
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
        if (ppc_backend_jit.init != NULL &&
            !ppc_backend_jit.init((emu_cpu_t *)&g_side[1].cpu)) {
            fprintf(stderr, "fuzz-ppc-tlb: the JIT did not start\n");
            abort();
        }
    }
    g_st.inputs++;

    memset(&R, 0, sizeof(R));
    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        R.ram[i] = (uint8_t)(seed >> 24);
        if (i < RAM2_SIZE) {
            R.ram2[i] = (uint8_t)(seed >> 16);
        }
    }

    /* The slots: one instruction and a `wait` each, the rest `wait`. */
    for (uint32_t i = 0; i < CODE_SIZE; i += 4u) {
        put32be(&g_code[i], WAIT);
    }
    g_nslots = 1u + byte(&in) % SLOTS;
    for (unsigned s = 0; s < g_nslots; s++) {
        const unsigned k = byte(&in) % K_COUNT;
        const int32_t d = (int32_t)(byte(&in) & 0x0Fu) - 8;
        const uint32_t rd = (k == K_LMW || k == K_STMW) ? 28u + byte(&in) % 4u
                            : (k >= K_STW)              ? 4u
                                                        : 5u + byte(&in) % 4u;

        g_slot[s].kind = k;
        g_slot[s].d = d;
        g_slot[s].rd = rd;
        g_slot[s].insn = ((uint32_t)k_opcd[k] << 26) | (rd << 21) | (3u << 16) |
                         ((uint32_t)d & 0xFFFFu);
        put32be(&g_code[s * SLOT_STRIDE], g_slot[s].insn);
    }

    for (unsigned s = 0; s < 2u; s++) {
        build_side(&g_side[s]);
        if (g_side[s].backend->reset != NULL) {
            ppc_backend = g_side[s].backend;
            g_side[s].backend->reset((emu_cpu_t *)&g_side[s].cpu);
        }
    }
    /* The harness's entries, invalidation-protected: the vectors, and the
     * code in both address spaces, every permission but write. */
    tlbwe_both(0u, 0xC0000000u | (6u << 7), TRAP_BASE, TRAP_BASE | 0x11u);
    tlbwe_both(30u, 0xC0000000u | (2u << 7), CODE_BASE, CODE_BASE | 0x33u);
    tlbwe_both(31u, 0xC0001000u | (2u << 7), CODE_BASE, CODE_BASE | 0x33u);
    /* Entry 0 was the reset entry, the whole 4 GB; it is the vectors'
     * now, and every data access goes through an entry the input wrote. */
    read_back();
    read_mas();
    R.pid = 0u;
    g_cur.step = 0u;
    g_fails = 0u;
    g_hot_ea = 0u;

    for (unsigned s = 0; s < 96u && in.n > 0u; s++) {
        do_step(&in);
    }
    return 0;
}
