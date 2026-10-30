/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_g4mh_mpu.c - G4MH's memory protection against a reference written
 * from the U2B manual.
 *
 * fuzz-g4mh-jit compares the interpreter with the JIT, and both ask the
 * same g4mh_mpu.c and run CAXI, POPSP and LDSR in the same interpreter --
 * so a misreading of the manual there is shared, and agreed with. This
 * target asks a reference that shares none of that: R01UH0923EJ0130
 * section 3.2.5.1 (the check, tables 3.111-3.116), table 3.103 (cause
 * codes), and the instruction pages of the G4MH software manual
 * R01US0209EJ0220 for what each instruction does around an access.
 *
 * The input is a sequence of steps. A step either writes the MPU
 * through its system-register window -- MPIDX, MPLA, MPUA, MPAT, MPM,
 * MPIDn -- or changes the core's SPID and mode, or runs one instruction:
 * placed in RAM at a pc chosen near an area's end, followed by a HALT,
 * on registers pointing near an area's ends. Each runs on two cores, one
 * per backend, and every step is compared with the reference: where the
 * core stopped, PSW, FEPC/FEPSW/FEIC (with its detail half) and MEA, the
 * registers, MCR, and all of memory.
 *
 * What the reference reads from the cores is only what software can: the
 * MPU's registers through the window after each write, so their masks
 * are the window's and not this file's question. **For G4MH the manual is
 * the only authority**: no RH850 is attached, and a reading shared by
 * this file and the core is invisible here. The readings this file makes
 * where the manual leaves room are listed in docs/fuzzing.md.
 *
 * Entry 31 is the harness's: supervisor-executable over the vector
 * table, so every exception can reach its handler's HALT whatever the
 * input did to the other thirty-one. The JIT's blocks are discarded
 * before every step, because the step rewrites the code.
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
#include "g4mh/g4mh_backend.h"
#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_mpu.h"

#if !G4MH_EXT_MPU || G4MH_MPU_ENTRIES != 32
#error "fuzz-g4mh-mpu needs the MPU with its 32 entries"
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define LOW_BASE 0x00000000u /* where a doubleword at the top wraps to */
#define LOW_SIZE 0x1000u
#define CODE_BASE 0x00010000u
#define CODE_SIZE 0x1000u
#define DATA_BASE 0x00020000u
#define DATA_SIZE 0x1000u
#define TRAP_BASE 0x00030000u
#define TRAP_SIZE 0x200u
#define HOLE_BASE 0x00040000u /* nothing mapped */
#define TOP_BASE 0xFFFFF000u
#define TOP_SIZE 0x1000u

#define HALT_W0 0x07E0u
#define HALT_W1 0x0120u
#define VEC_MPU 0x090u /* MIP and MDP's handler, from RBASE */
#define VEC_MAE 0x0C0u

#define TRAP_ENTRY 31u /* the harness's own entry */

/* Before every step, so a field the step did not write shows as itself. */
#define SENT_FEPC 0xFEFE0000u
#define SENT_FEPSW 0x00000000u
#define SENT_FEIC 0x0000DEADu
#define SENT_MEA 0x0BAD0BADu

typedef struct side {
    const char *name;
    const emu_backend_t *backend;
    g4mh_cpu_t cpu;
    g4mh_mpu_t mpu;
    g4mh_intc_t intc;
    g4mh_intc2_t intc2;
    emu_bus_t bus;
    uint8_t low[LOW_SIZE], code[CODE_SIZE], data[DATA_SIZE], top[TOP_SIZE];
} side_t;

static side_t g_side[2] = {
    {.name = "interp", .backend = &g4mh_backend_interp},
    {.name = "jit", .backend = &g4mh_backend_jit},
};
static uint8_t g_trap[TRAP_SIZE];
static bool g_um; /* the mode the input last chose */

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

static struct {
    uint32_t r[32];
    uint32_t pc, psw;
    uint32_t fepc, fepsw, feic, mea;
    uint32_t mcr;
    uint8_t low[LOW_SIZE], code[CODE_SIZE], data[DATA_SIZE], top[TOP_SIZE];
    /* The MPU as software reads it back. */
    uint32_t mpla[32], mpua[32], mpat[32], mpid[8], mpm;
    uint32_t spid;
} R;

/* One step's description, for a report. */
static struct {
    unsigned step;
    const char *what;
    uint32_t pc;
    uint16_t hw[8];
    unsigned nhw;
} g_cur;

static struct {
    uint64_t inputs, steps, insns, completed, mip, mdp, mdp_detail, mae;
    uint64_t straddles, mcc, mcc_ov, mcc_some, translations, entries;
    uint64_t partial;
} g_st;

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-g4mh-mpu: %llu inputs, %llu steps, %llu instructions: "
            "%llu completed, %llu MIP, %llu MDP (%llu by the MPU), %llu MAE; "
            "%llu fetches across a word into another area's decision, "
            "%llu block transfers stopped partway; "
            "%llu setting checks (%llu OV, %llu with a bit set); "
            "jit %llu translations, %llu block entries\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.steps,
            (unsigned long long)g_st.insns, (unsigned long long)g_st.completed,
            (unsigned long long)g_st.mip, (unsigned long long)g_st.mdp,
            (unsigned long long)g_st.mdp_detail, (unsigned long long)g_st.mae,
            (unsigned long long)g_st.straddles,
            (unsigned long long)g_st.partial, (unsigned long long)g_st.mcc,
            (unsigned long long)g_st.mcc_ov, (unsigned long long)g_st.mcc_some,
            (unsigned long long)g_st.translations,
            (unsigned long long)g_st.entries);
}

/* Memory, as the bus maps it: the region holding `addr`, or NULL. */
static uint8_t *ref_at(uint32_t addr, uint32_t size, bool *rom)
{
    *rom = false;
    if (addr - LOW_BASE <= LOW_SIZE - size) {
        return &R.low[addr - LOW_BASE];
    }
    if (addr - CODE_BASE <= CODE_SIZE - size) {
        *rom = true; /* so no store can reach the HALT after a step */
        return &R.code[addr - CODE_BASE];
    }
    if (addr - DATA_BASE <= DATA_SIZE - size) {
        return &R.data[addr - DATA_BASE];
    }
    if (addr - TOP_BASE <= TOP_SIZE - size) {
        return &R.top[addr - TOP_BASE];
    }
    if (addr - TRAP_BASE <= TRAP_SIZE - size) {
        *rom = true;
        return &g_trap[addr - TRAP_BASE];
    }
    return NULL;
}

static bool ref_read(uint32_t addr, uint32_t size, uint32_t *out)
{
    bool rom;
    const uint8_t *p = ref_at(addr, size, &rom);
    uint32_t v = 0u;

    if (p == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint32_t)p[i] << (8u * i);
    }
    *out = v;
    return true;
}

static bool ref_write(uint32_t addr, uint32_t size, uint32_t val)
{
    bool rom;
    uint8_t *p = ref_at(addr, size, &rom);

    if (p == NULL || rom) {
        return false;
    }
    for (uint32_t i = 0; i < size; i++) {
        p[i] = (uint8_t)(val >> (8u * i));
    }
    return true;
}

typedef enum { A_FETCH, A_READ, A_WRITE } acc_t;

/*
 * 3.2.5.1(2)-(4): the SPID group -- bypassed by RG/WG, or the SPID in an
 * MPIDn whose RMPIDn/WMPIDn bit is set ("if any of the corresponding bit
 * is set") -- and the mode group, both, in one enabled area holding the
 * whole span; overlapping areas "allow most"; nothing matching refuses.
 * MPM.SVP clear opens all of supervisor mode. An access wrapping past
 * 0xFFFFFFFF is in no area (table 3.114).
 */
static bool spid_ok(uint32_t at, bool write, uint32_t spid)
{
    if ((at & (write ? (1u << 15) : (1u << 14))) != 0u) {
        return true;
    }
    const uint32_t bits = (at >> (write ? 24u : 16u)) & 0xFFu;

    for (unsigned n = 0; n < 8u; n++) {
        if ((bits >> n) & 1u && (R.mpid[n] & 0x1Fu) == spid) {
            return true;
        }
    }
    return false;
}

static bool area_permits(unsigned i, acc_t acc, bool um, uint32_t spid)
{
    static const uint32_t need[2][3] = {
        /* SX, SR, SW */ {1u << 5, 1u << 3, 1u << 4},
        /* UX, UR, UW */ {1u << 2, 1u << 0, 1u << 1},
    };
    const uint32_t at = R.mpat[i];

    return (at & need[um ? 1 : 0][acc]) != 0u &&
           spid_ok(at, acc == A_WRITE, spid);
}

static bool ref_permits(uint32_t addr, uint32_t size, acc_t acc)
{
    const bool um = (R.psw & G4MH_PSW_UM) != 0u;
    const uint64_t last = (uint64_t)addr + size - 1u;

    if ((R.mpm & 1u) == 0u || (!um && (R.mpm & 2u) == 0u)) {
        return true;
    }
    if (last > 0xFFFFFFFFull) {
        return false;
    }
    for (unsigned i = 0; i < 32u; i++) {
        const uint64_t lo = R.mpla[i] & ~3u, hi = R.mpua[i] | 3u;

        if ((R.mpat[i] & (1u << 7)) != 0u && addr >= lo && last <= hi &&
            area_permits(i, acc, um, R.spid)) {
            return true;
        }
    }
    return false;
}

/* The halfwords of an instruction, each where the MPU puts it. */
static bool ref_fetch_ok(uint32_t pc, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += 2u) {
        if (!ref_permits(pc + off, 2u, A_FETCH)) {
            return false;
        }
    }
    return true;
}

/*
 * The setting check, 3.2.5.1(6) and tables 3.67-3.71, 3.116: the span
 * MCA .. MCA + MCS - 1 modulo 2^32 inside one enabled area, per mode and
 * permission, with MCI as the SPID, whatever PSW.UM is. OV for a span
 * passing 0xFFFFFFFF -> 0 or 0x7FFFFFFF -> 0x80000000.
 */
static uint32_t ref_check(uint32_t mca, uint32_t mcs, uint32_t mci)
{
    const uint32_t sv = (R.mpm & 2u) == 0u ? 0x38u : 0u;
    const uint64_t lo = mca;
    const uint64_t hi = (uint64_t)mca + (mcs == 0u ? 0x100000000ull : mcs) - 1u;
    uint32_t r = 0u;

    if ((R.mpm & 1u) == 0u) {
        return 0x3Fu;
    }
    if (hi > 0xFFFFFFFFull || (lo < 0x80000000ull && hi >= 0x80000000ull)) {
        return 0x100u | sv;
    }
    for (unsigned i = 0; i < 32u; i++) {
        if ((R.mpat[i] & (1u << 7)) == 0u || lo < (R.mpla[i] & ~3u) ||
            hi > (R.mpua[i] | 3u)) {
            continue;
        }
        for (unsigned um = 0; um < 2u; um++) {
            for (unsigned a = 0; a < 3u; a++) {
                /* MCR: URE UWE UXE SRE SWE SXE from bit 0. */
                static const unsigned bit[2][3] = {{5u, 3u, 4u}, {2u, 0u, 1u}};

                if (area_permits(i, (acc_t)a, um != 0u, mci & 0x1Fu)) {
                    r |= 1u << bit[um][a];
                }
            }
        }
    }
    return r | sv;
}

/* Table 3.103's cause, with table 3.115's detail for MDP and MIP. */
static uint32_t mdp_cause(bool rd, bool wr, uint32_t kind)
{
    const bool um = (R.psw & G4MH_PSW_UM) != 0u;

    return 0x91u | kind | (rd ? (um ? 1u << 16 : 1u << 19) : 0u) |
           (wr ? (um ? 1u << 17 : 1u << 20) : 0u);
}

/* FE-level entry (R01UH0865EJ0140 2.1.3.3), then the handler's HALT. */
static void ref_trap(uint32_t cause, uint32_t ret_pc, bool set_mea,
                     uint32_t mea)
{
    R.fepc = ret_pc;
    R.fepsw = R.psw;
    R.feic = cause;
    if (set_mea) {
        R.mea = mea;
    }
    R.psw = (R.psw | G4MH_PSW_NP | G4MH_PSW_EP | G4MH_PSW_ID) & ~G4MH_PSW_UM;
    R.pc = TRAP_BASE + (((cause & 0xFFFFu) == 0xC0u) ? VEC_MAE : VEC_MPU) + 4u;
    if ((cause & 0xFFFFu) == 0x90u) {
        g_st.mip++;
    } else if ((cause & 0xFFFFu) == 0x91u) {
        g_st.mdp++;
        g_st.mdp_detail += (cause >> 16) != 0u;
    } else {
        g_st.mae++;
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

/* An address an area bound or an access is near: where things are. */
static uint32_t pick_near(in_t *in)
{
    const uint32_t sel = byte(in);
    const uint32_t k = half(in);

    switch (sel % 10u) {
    case 0:
        return CODE_BASE + (k % CODE_SIZE);
    case 1:
    case 2:
        return DATA_BASE + (k % DATA_SIZE);
    case 3:
        return LOW_BASE + (k % LOW_SIZE);
    case 4:
        return TOP_BASE + (k % TOP_SIZE);
    case 5:
        return 0xFFFFFFF0u + (k & 0xFu);
    case 6:
        return 0x7FFFFFF0u + (k & 0x1Fu);
    case 7: { /* an end of an entry the input has written */
        const unsigned i = k % TRAP_ENTRY;

        return ((sel & 0x10u) ? (R.mpua[i] | 3u) + 1u : (R.mpla[i] & ~3u)) -
               8u + ((k >> 5) & 0xFu);
    }
    case 8:
        return HOLE_BASE + (k & 0xFFu);
    default:
        return word(in);
    }
}

/* ------------------------------------------------------------------ */
/* The two cores                                                       */
/* ------------------------------------------------------------------ */

static void read_back(void)
{
    g4mh_cpu_t *const c = &g_side[0].cpu;
    const uint32_t idx = g4mh_sr_read(c, 5u, G4MH_SR_MPIDX);

    for (unsigned s = 0; s < 2u; s++) {
        g4mh_cpu_t *const cs = &g_side[s].cpu;

        for (unsigned i = 0; i < 32u; i++) {
            g4mh_sr_write(cs, 5u, G4MH_SR_MPIDX, i);
            const uint32_t la = g4mh_sr_read(cs, 5u, G4MH_SR_MPLA);
            const uint32_t ua = g4mh_sr_read(cs, 5u, G4MH_SR_MPUA);
            const uint32_t at = g4mh_sr_read(cs, 5u, G4MH_SR_MPAT);

            if (s == 0u) {
                R.mpla[i] = la;
                R.mpua[i] = ua;
                R.mpat[i] = at;
            } else if (la != R.mpla[i] || ua != R.mpua[i] || at != R.mpat[i]) {
                fprintf(stderr, "fuzz-g4mh-mpu: the two MPUs read back "
                                "differently\n");
                abort();
            }
        }
        g4mh_sr_write(cs, 5u, G4MH_SR_MPIDX, idx);
    }
    for (unsigned n = 0; n < 8u; n++) {
        R.mpid[n] = g4mh_sr_read(c, 5u, G4MH_SR_MPID0 + n);
    }
    R.mpm = g4mh_sr_read(c, 5u, G4MH_SR_MPM);
}

static void sr_write_both(unsigned reg, uint32_t val)
{
    for (unsigned s = 0; s < 2u; s++) {
        g4mh_sr_write(&g_side[s].cpu, 5u, reg, val);
    }
}

static void write_entry(unsigned i, uint32_t la, uint32_t ua, uint32_t at)
{
    sr_write_both(G4MH_SR_MPIDX, i);
    sr_write_both(G4MH_SR_MPLA, la);
    sr_write_both(G4MH_SR_MPUA, ua);
    sr_write_both(G4MH_SR_MPAT, at);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void build_side(side_t *sd)
{
    g4mh_cpu_t *const c = &sd->cpu;
    bool ok;

    emu_bus_init(&sd->bus);
    ok = emu_bus_add_ram(&sd->bus, "low", LOW_BASE, sd->low, LOW_SIZE) &&
         emu_bus_add_rom(&sd->bus, "code", CODE_BASE, sd->code, CODE_SIZE) &&
         emu_bus_add_ram(&sd->bus, "data", DATA_BASE, sd->data, DATA_SIZE) &&
         emu_bus_add_rom(&sd->bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE) &&
         emu_bus_add_ram(&sd->bus, "top", TOP_BASE, sd->top, TOP_SIZE);
    if (!ok) {
        fprintf(stderr, "fuzz-g4mh-mpu: could not build the bus\n");
        abort();
    }
    g4mh_ll_drop(c);
    g4mh_cpu_init(c, &sd->bus, 0u);
    c->mpu = &sd->mpu;
    g4mh_mpu_reset(&sd->mpu);
    g4mh_intc_init(&sd->intc, c, NULL, &sd->intc2);
    g4mh_cpu_reset(c, CODE_BASE);
    c->sr[1][G4MH_SR_RBASE] = TRAP_BASE;
    c->sr[1][G4MH_SR_EBASE] = TRAP_BASE;
    memcpy(sd->low, R.low, LOW_SIZE);
    memcpy(sd->code, R.code, CODE_SIZE);
    memcpy(sd->data, R.data, DATA_SIZE);
    memcpy(sd->top, R.top, TOP_SIZE);
}

/* ------------------------------------------------------------------ */
/* Instructions                                                        */
/* ------------------------------------------------------------------ */

enum {
    K_LDB,
    K_LDBU,
    K_LDH,
    K_LDHU,
    K_LDW,
    K_STB,
    K_STH,
    K_STW,
    K_LDDW,
    K_STDW,
    K_BIT,
    K_CAXI,
    K_PUSHSP,
    K_POPSP,
    K_SLDW,
    K_SSTW,
    K_MOV32,
    K_NOP,
    K_MCC,
    K_PREPARE,
    K_DISPOSE,
    K_COUNT
};

static const char *const k_name[K_COUNT] = {
    "ld.b",
    "ld.bu",
    "ld.h",
    "ld.hu",
    "ld.w",
    "st.b",
    "st.h",
    "st.w",
    "ld.dw",
    "st.dw",
    "set1/not1/clr1/tst1",
    "caxi",
    "pushsp",
    "popsp",
    "sld.w",
    "sst.w",
    "mov imm32",
    "nop",
    "ldsr mcc; stsr mcr",
    "prepare",
    "dispose",
};

/*
 * list12 (PREPARE and DISPOSE): which register each bit names, from the
 * table on both instructions' pages -- bits 31..21 and 0, the rest
 * "don't care".
 */
static const struct {
    uint8_t bit, reg;
} k_list12[12] = {
    {27, 20}, {26, 21}, {25, 22}, {24, 23}, {31, 24}, {30, 25},
    {29, 26}, {28, 27}, {23, 28}, {22, 29}, {0, 30},  {21, 31},
};

/* The instruction's halfwords, from the formats in R01US0209EJ0220. */
static unsigned encode(unsigned k, uint32_t a, uint32_t b, uint32_t d,
                       uint16_t *hw)
{
    switch (k) {
    case K_LDB: /* rrrrr111000RRRRR dddddddddddddddd */
        hw[0] = (uint16_t)((a << 11) | (0x38u << 5) | 10u);
        hw[1] = (uint16_t)d;
        return 2;
    case K_LDH: /* rrrrr111001RRRRR ddddddddddddddd0 */
        hw[0] = (uint16_t)((a << 11) | (0x39u << 5) | 10u);
        hw[1] = (uint16_t)(d & ~1u);
        return 2;
    case K_LDW: /* rrrrr111001RRRRR ddddddddddddddd1 */
        hw[0] = (uint16_t)((a << 11) | (0x39u << 5) | 10u);
        hw[1] = (uint16_t)((d & ~1u) | 1u);
        return 2;
    case K_LDBU: /* rrrrr11110bRRRRR ddddddddddddddd1, b = disp[0] */
        hw[0] = (uint16_t)((a << 11) | ((0x3Cu | (d & 1u)) << 5) | 10u);
        hw[1] = (uint16_t)((d & ~1u) | 1u);
        return 2;
    case K_LDHU: /* rrrrr111111RRRRR ddddddddddddddd1 */
        hw[0] = (uint16_t)((a << 11) | (0x3Fu << 5) | 10u);
        hw[1] = (uint16_t)((d & ~1u) | 1u);
        return 2;
    case K_STB: /* rrrrr111010RRRRR dddddddddddddddd */
        hw[0] = (uint16_t)((a << 11) | (0x3Au << 5) | 10u);
        hw[1] = (uint16_t)d;
        return 2;
    case K_STH: /* rrrrr111011RRRRR ddddddddddddddd0 */
        hw[0] = (uint16_t)((a << 11) | (0x3Bu << 5) | 10u);
        hw[1] = (uint16_t)(d & ~1u);
        return 2;
    case K_STW: /* rrrrr111011RRRRR ddddddddddddddd1 */
        hw[0] = (uint16_t)((a << 11) | (0x3Bu << 5) | 10u);
        hw[1] = (uint16_t)((d & ~1u) | 1u);
        return 2;
    case K_LDDW: /* 00000111101RRRRR wwwwwddddddd1001 DDDDDDDDDDDDDDDD */
    case K_STDW: /* 00000111101RRRRR wwwwwddddddd1111 DDDDDDDDDDDDDDDD */
        hw[0] = (uint16_t)((0x3Du << 5) | 10u);
        hw[1] = (uint16_t)((a << 11) | ((d & 0x7Eu) << 4) |
                           (k == K_LDDW ? 0x9u : 0xFu));
        hw[2] = (uint16_t)((d >> 7) & 0xFFFFu);
        return 3;
    case K_BIT: /* ssbbb111110RRRRR dddddddddddddddd */
        hw[0] = (uint16_t)(((a & 3u) << 14) | ((b & 7u) << 11) | (0x3Eu << 5) |
                           10u);
        hw[1] = (uint16_t)d;
        return 2;
    case K_CAXI: /* rrrrr111111RRRRR wwwww00011101110: [r10], r11, a */
        hw[0] = (uint16_t)((11u << 11) | (0x3Fu << 5) | 10u);
        hw[1] = (uint16_t)((a << 11) | 0x0EEu);
        return 2;
    case K_PUSHSP: /* 01000111111RRRRR wwwww00101100000 */
    case K_POPSP: /* 01100111111RRRRR wwwww00101100000 */
        hw[0] = (uint16_t)((k == K_PUSHSP ? 0x47E0u : 0x67E0u) | a);
        hw[1] = (uint16_t)((b << 11) | 0x0160u);
        return 2;
    case K_SLDW: /* rrrrr1010dddddd0, disp = dddddd * 4 */
    case K_SSTW: /* rrrrr1010dddddd1 */
        hw[0] = (uint16_t)((a << 11) | (0xAu << 7) | ((d & 0x3Fu) << 1) |
                           (k == K_SSTW ? 1u : 0u));
        return 1;
    case K_MOV32: /* 00000110001RRRRR, imm32 low then high */
        hw[0] = (uint16_t)(0x0620u | a);
        hw[1] = (uint16_t)d;
        hw[2] = (uint16_t)(d >> 16);
        return 3;
    case K_NOP:
        hw[0] = 0x0000u;
        return 1;
    case K_PREPARE: /* 0000011110iiiiiL LLLLLLLLLLL00001, list12 in a */
    case K_DISPOSE: /* 0000011001iiiiiL LLLLLLLLLLL00000 */
        hw[0] = (uint16_t)((k == K_PREPARE ? 0x0780u : 0x0640u) |
                           ((b & 0x1Fu) << 1) | (a & 1u));
        hw[1] = (uint16_t)(((a >> 16) & 0xFFE0u) | (k == K_PREPARE ? 1u : 0u));
        return 2;
    default: /* K_MCC: ldsr r0, sr10, 5; stsr sr11, a, 5 */
        hw[0] = (uint16_t)((10u << 11) | (0x3Fu << 5) | 0u);
        hw[1] = (uint16_t)((5u << 11) | 0x020u);
        hw[2] = (uint16_t)((a << 11) | (0x3Fu << 5) | 11u);
        hw[3] = (uint16_t)((5u << 11) | 0x040u);
        return 4;
    }
}

/* PSW's four flags from a - b, as CMP: the software manual's CAXI. */
static uint32_t sub_flags(uint32_t a, uint32_t b)
{
    const uint32_t res = a - b;

    return (res == 0u ? G4MH_PSW_Z : 0u) |
           ((res & 0x80000000u) ? G4MH_PSW_S : 0u) |
           (a < b ? G4MH_PSW_CY : 0u) |
           (((a ^ b) & (a ^ res) & 0x80000000u) ? G4MH_PSW_OV : 0u);
}

static void ref_set(uint32_t reg, uint32_t v)
{
    if (reg != 0u) {
        R.r[reg] = v;
    }
}

/*
 * One data access as the core makes it: misalignment (MAE), the MPU
 * (MDP with detail), the bus (MDP without: this frontend reports a bus
 * fault as the protection exception). `kind` is the instruction's
 * detail bit. False with the trap entered; `pc` is the instruction's.
 */
static bool ref_access(uint32_t pc, uint32_t addr, uint32_t size,
                       uint32_t align, acc_t acc, uint32_t kind, uint32_t *val)
{
    if ((addr & (align - 1u)) != 0u) {
        ref_trap(0xC0u, pc, true, addr);
        return false;
    }
    if (!ref_permits(addr, size, acc)) {
        ref_trap(mdp_cause(acc == A_READ, acc == A_WRITE, kind), pc, true,
                 addr);
        return false;
    }
    if (acc == A_READ ? !ref_read(addr, size, val)
                      : !ref_write(addr, size, *val)) {
        ref_trap(0x91u | kind, pc, true, addr);
        return false;
    }
    return true;
}

/*
 * The doubleword: one eight-byte span to the MPU (3.2.5.1(3)(d), tables
 * 3.112 and 3.114), then two words on the bus, low first.
 */
static bool ref_dw(uint32_t pc, uint32_t addr, bool store, uint32_t rd)
{
    uint32_t lo = R.r[rd], hi = R.r[rd + 1u];
    const acc_t acc = store ? A_WRITE : A_READ;

    if ((addr & 3u) != 0u) {
        ref_trap(0xC0u, pc, true, addr);
        return false;
    }
    if (!ref_permits(addr, 8u, acc)) {
        ref_trap(mdp_cause(!store, store, 0u), pc, true, addr);
        return false;
    }
    if (store ? !ref_write(addr, 4u, lo) : !ref_read(addr, 4u, &lo)) {
        ref_trap(0x91u, pc, true, addr);
        return false;
    }
    if (store ? !ref_write(addr + 4u, 4u, hi) : !ref_read(addr + 4u, 4u, &hi)) {
        ref_trap(0x91u, pc, true, addr + 4u);
        return false;
    }
    if (!store) {
        ref_set(rd, lo);
        ref_set(rd + 1u, hi);
    }
    return true;
}

/*
 * A read-modify-write: SET1, NOT1, CLR1 and CAXI are refused with both
 * permissions known, before anything moves, and say so (RMW). Which of
 * read and write they name is this file's reading: each refused one.
 */
static bool ref_rmw_ok(uint32_t pc, uint32_t addr, uint32_t size)
{
    if ((addr & (size - 1u)) != 0u) {
        ref_trap(0xC0u, pc, true, addr);
        return false;
    }
    if ((R.mpm & 1u) == 0u) {
        return true;
    }
    const bool rd = !ref_permits(addr, size, A_READ);
    const bool wr = !ref_permits(addr, size, A_WRITE);

    if (rd || wr) {
        ref_trap(mdp_cause(rd, wr, 1u << 22), pc, true, addr);
        return false;
    }
    return true;
}

/* Execute instruction `k` at `pc`; false if it trapped. */
static bool ref_exec(unsigned k, uint32_t pc, uint32_t a, uint32_t b,
                     uint32_t d)
{
    const uint32_t ea = R.r[10] + (uint32_t)emu_sext(d & 0xFFFFu, 16);
    uint32_t v = 0u;

    switch (k) {
    case K_LDB:
    case K_LDBU:
    case K_LDH:
    case K_LDHU:
    case K_LDW: {
        const uint32_t size = (k == K_LDB || k == K_LDBU) ? 1u
                              : (k == K_LDW)              ? 4u
                                                          : 2u;
        const uint32_t addr =
            (k == K_LDB) ? ea
                         : R.r[10] + (uint32_t)emu_sext(d & 0xFFFEu, 16) +
                               ((k == K_LDBU) ? (d & 1u) : 0u);

        if (!ref_access(pc, addr, size, size, A_READ, 0u, &v)) {
            return false;
        }
        if (k == K_LDB || k == K_LDH) {
            v = (uint32_t)emu_sext(v, size * 8u);
        }
        ref_set(a, v);
        return true;
    }
    case K_STB:
    case K_STH:
    case K_STW: {
        const uint32_t size = (k == K_STB) ? 1u : (k == K_STH) ? 2u : 4u;
        const uint32_t addr =
            (k == K_STB) ? ea : R.r[10] + (uint32_t)emu_sext(d & 0xFFFEu, 16);

        v = R.r[a];
        return ref_access(pc, addr, size, size, A_WRITE, 0u, &v);
    }
    case K_LDDW:
    case K_STDW:
        return ref_dw(pc, R.r[10] + (uint32_t)emu_sext(d & 0x7FFFFEu, 23),
                      k == K_STDW, a & 0x1Eu);
    case K_BIT: {
        const uint32_t op = a & 3u; /* SET1, NOT1, CLR1, TST1 */
        const uint32_t mask = 1u << (b & 7u);

        if (op != 3u && !ref_rmw_ok(pc, ea, 1u)) {
            return false;
        }
        if (!ref_access(pc, ea, 1u, 1u, A_READ, 0u, &v)) {
            return false;
        }
        R.psw = (R.psw & ~G4MH_PSW_Z) | ((v & mask) ? 0u : G4MH_PSW_Z);
        if (op == 3u) {
            return true;
        }
        v = (op == 0u) ? (v | mask) : (op == 1u) ? (v ^ mask) : (v & ~mask);
        return ref_access(pc, ea, 1u, 1u, A_WRITE, 0u, &v);
    }
    case K_CAXI: {
        const uint32_t addr = R.r[10];

        if (!ref_rmw_ok(pc, addr, 4u) ||
            !ref_access(pc, addr, 4u, 4u, A_READ, 0u, &v)) {
            return false;
        }
        uint32_t w = (R.r[11] == v) ? R.r[a] : v;

        if (!ref_access(pc, addr, 4u, 4u, A_WRITE, 0u, &w)) {
            return false;
        }
        R.psw = (R.psw & ~G4MH_PSW_FLAGS) | sub_flags(R.r[11], v);
        ref_set(a, v);
        return true;
    }
    case K_PUSHSP: /* rh = a ascending, each below the last */
    case K_POPSP: { /* rt = b descending, each above the last */
        uint32_t tmp = R.r[3];

        if (a > b) {
            return true;
        }
        for (uint32_t n = 0; n <= b - a; n++) {
            const uint32_t cur = (k == K_PUSHSP) ? a + n : b - n;

            if (k == K_PUSHSP) {
                tmp -= 4u;
                v = R.r[cur];
            }
            if (!ref_access(pc, tmp & ~3u, 4u, 1u,
                            k == K_PUSHSP ? A_WRITE : A_READ, 1u << 23, &v)) {
                g_st.partial += n != 0u;
                return false;
            }
            if (k == K_POPSP) {
                if (cur != 3u) { /* "the sp is not loaded" */
                    ref_set(cur, v);
                }
                tmp += 4u;
            }
        }
        R.r[3] = tmp;
        return true;
    }
    case K_SLDW:
    case K_SSTW: {
        const uint32_t addr = R.r[30] + (d & 0x3Fu) * 4u;

        if (k == K_SSTW) {
            v = R.r[a];
            return ref_access(pc, addr, 4u, 4u, A_WRITE, 0u, &v);
        }
        if (!ref_access(pc, addr, 4u, 4u, A_READ, 0u, &v)) {
            return false;
        }
        ref_set(a, v);
        return true;
    }
    case K_PREPARE: /* r20 up, each at the word below the last */
    case K_DISPOSE: { /* r31 down, each at the word above: its inverse */
        uint32_t tmp = R.r[3] + (k == K_DISPOSE ? (b & 0x1Fu) * 4u : 0u);
        unsigned done = 0u;

        for (unsigned j = 0; j < 12u; j++) {
            const unsigned e = (k == K_PREPARE) ? j : 11u - j;
            const uint32_t reg = k_list12[e].reg;

            if (((a >> k_list12[e].bit) & 1u) == 0u) {
                continue;
            }
            if (k == K_PREPARE) {
                tmp -= 4u;
                v = R.r[reg];
            }
            if (!ref_access(pc, tmp & ~3u, 4u, 1u,
                            k == K_PREPARE ? A_WRITE : A_READ, 1u << 23, &v)) {
                g_st.partial += done != 0u;
                return false;
            }
            if (k == K_DISPOSE) {
                ref_set(reg, v);
                tmp += 4u;
            }
            done++;
        }
        R.r[3] = (k == K_PREPARE) ? tmp - (b & 0x1Fu) * 4u : tmp;
        return true;
    }
    case K_MOV32:
        ref_set(a, d);
        return true;
    default: /* K_NOP */
        return true;
    }
}

/* ------------------------------------------------------------------ */
/* A step                                                              */
/* ------------------------------------------------------------------ */

static void dump(void)
{
    char buf[96];
    uint64_t insn = 0u;

    fprintf(stderr, "  step %u: %s at 0x%08x, mode %s, SPID %u, MPM %u\n",
            g_cur.step, g_cur.what, g_cur.pc,
            (R.psw & G4MH_PSW_UM) ? "user" : "supervisor", R.spid, R.mpm);
    for (unsigned i = 0; i < g_cur.nhw && i < 4u; i++) {
        insn |= (uint64_t)g_cur.hw[i] << (16u * i);
    }
    g4mh_disasm(buf, sizeof(buf), g_cur.pc, insn, 2u * g_cur.nhw);
    fprintf(stderr, "    %s\n", buf);
    fprintf(stderr, "  r3 0x%08x r10 0x%08x r11 0x%08x r30 0x%08x\n", R.r[3],
            R.r[10], R.r[11], R.r[30]);
    for (unsigned i = 0; i < 32u; i++) {
        if ((R.mpat[i] & (1u << 7)) != 0u) {
            fprintf(stderr, "  entry %2u  0x%08x..0x%08x  attr 0x%08x\n", i,
                    R.mpla[i] & ~3u, R.mpua[i] | 3u, R.mpat[i]);
        }
    }
    for (unsigned n = 0; n < 8u; n++) {
        fprintf(stderr, "%s mpid%u %u", n == 0u ? " " : "", n, R.mpid[n]);
    }
    fprintf(stderr, "\n");
}

static unsigned g_fails;

static void expect(const side_t *sd, const char *what, unsigned idx,
                   uint32_t got, uint32_t want)
{
    if (got == want) {
        return;
    }
    if (g_fails++ == 0u) {
        fprintf(stderr, "fuzz-g4mh-mpu: %s disagrees with the manual\n",
                sd->name);
        dump();
    }
    fprintf(stderr, "  %-6s %-6s[%2u] got 0x%08x want 0x%08x\n", sd->name, what,
            idx, got, want);
}

static void expect_mem(const side_t *sd, const char *what, uint32_t base,
                       const uint8_t *got, const uint8_t *want, size_t n)
{
    for (size_t i = 0; i < n; i += 4u) {
        uint32_t a, b;

        memcpy(&a, &got[i], 4u);
        memcpy(&b, &want[i], 4u);
        if (a != b) {
            expect(sd, what, 0u, base + (uint32_t)i, 0u);
            expect(sd, "word", 0u, a, b);
            return;
        }
    }
}

static void run_side(side_t *sd, uint32_t pc, uint32_t psw,
                     const uint32_t *regs)
{
    g4mh_cpu_t *const c = &sd->cpu;
    uint32_t total = 0u;
    emu_jit_stats_t before, after;

    c->pc = pc;
    c->psw = psw;
    c->sr[0][G4MH_SR_PSW] = psw;
    c->sr[0][G4MH_SR_FEPC] = SENT_FEPC;
    c->sr[0][G4MH_SR_FEPSW] = SENT_FEPSW;
    c->sr[0][G4MH_SR_FEIC] = SENT_FEIC;
    c->sr[2][G4MH_SR_MEA] = SENT_MEA;
    c->state = EMU_STATE_RUNNING;
    memcpy(c->r, regs, sizeof(c->r));
    g4mh_jit_ctx_refresh(c);

    g4mh_backend = sd->backend;
    g4mh_ll_register(c);
    if (sd->backend->reset != NULL) {
        sd->backend->reset((emu_cpu_t *)c); /* the code is new */
    }
    emu_jit_get_stats(&before);
    while (total < 64u) {
        uint32_t n = 0u;
        const emu_run_reason_t why = sd->backend->run((emu_cpu_t *)c, 16u, &n);

        total += n + 1u;
        if (why == EMU_RUN_WFI || why == EMU_RUN_HALTED) {
            break;
        }
    }
    emu_jit_get_stats(&after);
    if (sd == &g_side[1]) {
        g_st.translations += after.translations - before.translations;
        g_st.entries += after.block_entries - before.block_entries;
    }
}

static void compare(const side_t *sd)
{
    const g4mh_cpu_t *const c = &sd->cpu;

    expect(sd, "state", 0u, (uint32_t)c->state, (uint32_t)EMU_STATE_WFI);
    expect(sd, "pc", 0u, c->pc, R.pc);
    expect(sd, "psw", 0u, c->psw, R.psw);
    expect(sd, "fepc", 0u, c->sr[0][G4MH_SR_FEPC], R.fepc);
    expect(sd, "fepsw", 0u, c->sr[0][G4MH_SR_FEPSW], R.fepsw);
    expect(sd, "feic", 0u, c->sr[0][G4MH_SR_FEIC], R.feic);
    expect(sd, "mea", 0u, c->sr[2][G4MH_SR_MEA], R.mea);
    expect(sd, "mcr", 0u, g4mh_sr_read(c, 5u, G4MH_SR_MCR), R.mcr);
    for (unsigned i = 1; i < 32u; i++) {
        expect(sd, "r", i, c->r[i], R.r[i]);
    }
    expect_mem(sd, "low", LOW_BASE, sd->low, R.low, LOW_SIZE);
    expect_mem(sd, "data", DATA_BASE, sd->data, R.data, DATA_SIZE);
    expect_mem(sd, "top", TOP_BASE, sd->top, R.top, TOP_SIZE);
    expect_mem(sd, "code", CODE_BASE, sd->code, R.code, CODE_SIZE);
}

/* A pc for an instruction of `len` bytes and its HALT. */
static uint32_t pick_pc(in_t *in, uint32_t len)
{
    const uint32_t room = CODE_SIZE - len - 4u;
    const uint32_t sel = byte(in);
    uint32_t off = (half(in) % room) & ~1u;

    if ((sel & 3u) != 0u) {
        /* Ending 2 to 8 bytes into the word after an entry's last. */
        const unsigned i = (sel >> 2) % TRAP_ENTRY;
        const uint32_t after = (R.mpua[i] | 3u) + 1u;
        const uint32_t pc = after - 2u * (1u + ((sel >> 7) & 1u)) -
                            ((sel & 2u) ? len - 2u : 0u);

        if (pc - CODE_BASE <= room) {
            off = (pc - CODE_BASE) & ~1u;
        }
    }
    return CODE_BASE + off;
}

static void do_insn(in_t *in, unsigned k, uint32_t flags)
{
    uint32_t a = 0u, b = 0u, d = 0u, psw;
    uint16_t hw[8];

    /* Operands; r10 is the address register throughout. */
    switch (k) {
    case K_PUSHSP:
    case K_POPSP:
        a = 1u + byte(in) % 31u;
        b = a + byte(in) % 6u;
        b = b > 31u ? 31u : b;
        break;
    case K_BIT:
        a = byte(in);
        b = byte(in);
        d = (byte(in) & 7u) - 4u;
        break;
    case K_MOV32:
        a = 1u + byte(in) % 31u;
        d = word(in);
        break;
    case K_LDDW:
    case K_STDW:
        a = 12u + 2u * (byte(in) % 4u);
        d = ((byte(in) & 7u) - 4u) * 2u;
        break;
    case K_SLDW:
    case K_SSTW:
        a = 11u + byte(in) % 8u;
        d = byte(in) & 3u;
        break;
    case K_MCC:
        a = 13u;
        break;
    case K_PREPARE:
    case K_DISPOSE: /* a: list12, mostly short; b: imm5 */
        a = (half(in) << 16) | byte(in);
        if ((a & 0x80000000u) != 0u) {
            a &= 0x0FE00001u; /* three or four registers */
        }
        b = byte(in);
        break;
    default:
        a = 11u + byte(in) % 8u;
        d = (byte(in) & 7u) - 4u;
        break;
    }
    const unsigned n = encode(k, a, b, d, hw);
    const uint32_t len = 2u * n;
    const uint32_t pc = pick_pc(in, len);

    /* The registers: a pattern, then what the instruction addresses. */
    for (unsigned i = 1; i < 32u; i++) {
        R.r[i] = 0x9E3779B9u * (i + 7u * (unsigned)g_cur.step);
    }
    R.r[10] = pick_near(in);
    R.r[3] = (byte(in) & 1u) ? R.r[10] : pick_near(in);
    R.r[30] = R.r[10] - (d & 0x3Fu) * 4u;
    if ((byte(in) & 3u) == 0u) {
        uint32_t v = 0u;

        (void)ref_read(R.r[10], 4u, &v);
        R.r[11] = v; /* CAXI's compare succeeds */
    }
    uint32_t mcr_new = 0u;

    if (k == K_MCC) {
        const uint32_t sel = byte(in);
        /* Mostly a span inside an entry, where the bits are decided. */
        const unsigned e = byte(in) % 32u;
        const uint32_t mca = (sel & 0x40u)
                                 ? pick_near(in)
                                 : (R.mpla[e] & ~3u) + (byte(in) & 0x3Cu);
        const uint32_t mcs = (sel & 0x80u) ? word(in) : (sel & 0x7Fu) * 4u;
        const uint32_t mci = byte(in) & 0x1Fu;

        sr_write_both(G4MH_SR_MCA, mca);
        sr_write_both(G4MH_SR_MCS, mcs);
        sr_write_both(G4MH_SR_MCI, mci);
        mcr_new = ref_check(mca, mcs, mci);
    }
    /*
     * Flags and mode from the input; ID set; no exception in progress.
     * The setting check is run from supervisor mode, LDSR to the MPU
     * being supervisor's.
     */
    psw = ((g_um && k != K_MCC) ? G4MH_PSW_UM : 0u) | (flags & G4MH_PSW_FLAGS) |
          G4MH_PSW_ID | G4MH_PSW_CU0;
    R.psw = psw;

    g_cur.what = k_name[k];
    g_cur.pc = pc;
    g_cur.nhw = n;
    memcpy(g_cur.hw, hw, sizeof(hw));

    /* The code, with its HALT, in the reference and both cores. */
    for (unsigned i = 0; i < n; i++) {
        put16(&R.code[pc - CODE_BASE + 2u * i], hw[i]);
    }
    put16(&R.code[pc - CODE_BASE + len], HALT_W0);
    put16(&R.code[pc - CODE_BASE + len + 2u], HALT_W1);
    for (unsigned s = 0; s < 2u; s++) {
        memcpy(g_side[s].code, R.code, CODE_SIZE);
    }

    /* The reference, from the state the cores start in. */
    uint32_t regs[32];

    memcpy(regs, R.r, sizeof(regs));
    R.fepc = SENT_FEPC;
    R.fepsw = SENT_FEPSW;
    R.feic = SENT_FEIC;
    R.mea = SENT_MEA;
    {
        const bool um = (R.psw & G4MH_PSW_UM) != 0u;
        /* Its halfwords, or (for MCC) the two instructions' each. */
        const uint32_t ilen = (k == K_MCC) ? 4u : len;
        bool ok = true;

        if (ilen > 2u && ((pc ^ (pc + ilen - 1u)) & ~3u) != 0u &&
            ref_permits(pc, 2u, A_FETCH) !=
                ref_permits(pc + ilen - 2u, 2u, A_FETCH)) {
            g_st.straddles++;
        }
        if (!ref_fetch_ok(pc, ilen)) {
            ref_trap(0x90u | (um ? 1u << 18 : 1u << 21), pc, true, pc);
            ok = false;
        } else if (k == K_MCC) {
            /* The check ran: STSR at pc + 4, its own fetch first. */
            R.mcr = mcr_new;
            g_st.mcc++;
            g_st.mcc_ov += (R.mcr & 0x100u) != 0u;
            g_st.mcc_some += (R.mcr & 0x3Fu) != 0u;
            if (!ref_fetch_ok(pc + 4u, 4u)) {
                ref_trap(0x90u | (1u << 21), pc + 4u, true, pc + 4u);
                ok = false;
            } else {
                ref_set(a, R.mcr);
            }
        } else {
            ok = ref_exec(k, pc, a, b, d);
        }
        if (ok) {
            const uint32_t halt = pc + len;

            if (!ref_fetch_ok(halt, 4u)) {
                ref_trap(0x90u | (um ? 1u << 18 : 1u << 21), halt, true, halt);
            } else {
                R.pc = halt + 4u;
                g_st.completed++;
            }
        }
    }
    for (unsigned s = 0; s < 2u; s++) {
        run_side(&g_side[s], pc, psw, regs);
        compare(&g_side[s]);
    }
    if (g_fails != 0u) {
        abort();
    }
    g_st.insns++;
}

static void do_step(in_t *in)
{
    const uint32_t op = byte(in);

    g_cur.step++;
    g_st.steps++;
    switch (op % 24u) {
    case 0:
    case 1:
    case 2: { /* an entry */
        const unsigned i = byte(in) % TRAP_ENTRY;
        const uint32_t la = pick_near(in);
        const uint32_t sel = byte(in);
        const uint32_t ua =
            (sel & 1u) ? pick_near(in) : la + ((sel >> 1) & 0x3Fu) * 4u;
        uint32_t at = word(in);

        if ((sel & 0x80u) == 0u) {
            at |= 1u << 7; /* enabled, mostly */
        }
        write_entry(i, la, ua, at);
        read_back();
        break;
    }
    case 3: /* MPM */
        sr_write_both(G4MH_SR_MPM, byte(in));
        read_back();
        break;
    case 4: { /* an MPIDn */
        const uint32_t sel = byte(in);

        sr_write_both(G4MH_SR_MPID0 + (sel & 7u),
                      (sel & 0x80u) ? (sel >> 3) : (sel >> 3) & 3u);
        read_back();
        break;
    }
    case 6: /* an area a program would have: code, executable */
    case 7: { /* or data, readable and writable */
        const unsigned i = byte(in) % TRAP_ENTRY;
        const uint32_t sel = byte(in);
        const uint32_t base = (op % 24u == 6u) ? CODE_BASE : DATA_BASE;
        const uint32_t la = base + (half(in) % 0x1000u) * 4u % 0x1000u;
        const uint32_t ua = la + (half(in) % 0x100u) * 4u;
        /* E, both modes' read and execute or read and write, any SPID,
         * and from the input whichever of those it takes away. */
        const uint32_t give = (op % 24u == 6u) ? 0x2Du : 0x1Bu;
        uint32_t at = (1u << 7) | (1u << 14) | (1u << 15) | give;

        at &= ~((sel & 0x7u) ? 0u : (uint32_t)(sel >> 3));
        write_entry(i, la, ua, at);
        sr_write_both(G4MH_SR_MPM, (sel & 0x80u) ? 1u : 3u);
        read_back();
        break;
    }
    case 8: { /* two areas end to end, or one over all of memory */
        const unsigned i = byte(in) % TRAP_ENTRY;
        const unsigned j = (i + 1u + byte(in) % (TRAP_ENTRY - 1u)) % TRAP_ENTRY;
        const uint32_t sel = byte(in);
        const uint32_t at = (1u << 7) | (1u << 14) | (1u << 15) | 0x3Fu;

        if ((sel & 1u) != 0u) {
            write_entry(i, 0u, 0xFFFFFFFCu, at);
        } else {
            const uint32_t mid = pick_near(in) & ~3u;

            write_entry(i, mid - 0x40u, mid - 4u, at);
            write_entry(j, mid, mid + 0x3Cu, at);
        }
        sr_write_both(G4MH_SR_MPM, (sel & 2u) ? 1u : 3u);
        read_back();
        break;
    }
    case 5: { /* the core's SPID and mode */
        const uint32_t sel = byte(in);

        R.spid = (sel & 0x80u) ? (sel >> 2) & 0x1Fu : (sel >> 2) & 3u;
        for (unsigned s = 0; s < 2u; s++) {
            g_side[s].cpu.spid = R.spid;
        }
        g_um = (sel & 1u) != 0u;
        break;
    }
    default: {
        const uint32_t flags = byte(in);

        do_insn(in, byte(in) % K_COUNT, flags);
        break;
    }
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
        for (uint32_t i = 0; i + 4u <= TRAP_SIZE; i += 4u) {
            put16(&g_trap[i], HALT_W0);
            put16(&g_trap[i + 2u], HALT_W1);
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
        if (g4mh_backend_jit.init != NULL &&
            !g4mh_backend_jit.init((emu_cpu_t *)&g_side[1].cpu)) {
            fprintf(stderr, "fuzz-g4mh-mpu: the JIT did not start\n");
            abort();
        }
    }
    g_st.inputs++;

    memset(&R, 0, sizeof(R));
    for (uint32_t i = 0; i < DATA_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        R.data[i] = (uint8_t)(seed >> 24);
        R.low[i] = (uint8_t)(seed >> 16);
        R.top[i] = (uint8_t)(seed >> 8);
    }
    for (uint32_t i = 0; i + 4u <= CODE_SIZE; i += 4u) {
        put16(&R.code[i], HALT_W0);
        put16(&R.code[i + 2u], HALT_W1);
    }
    for (unsigned s = 0; s < 2u; s++) {
        build_side(&g_side[s]);
    }
    /*
     * Entry 31: the vector table, supervisor-executable for any SPID, so
     * an exception's handler runs whatever the input does to the rest.
     */
    write_entry(TRAP_ENTRY, TRAP_BASE, TRAP_BASE + TRAP_SIZE - 4u,
                (1u << 7) | (1u << 5) | (1u << 3) | (1u << 14));
    read_back();
    R.mcr = g4mh_sr_read(&g_side[0].cpu, 5u, G4MH_SR_MCR);
    g_cur.step = 0u;
    g_fails = 0u;
    g_um = false;

    for (unsigned s = 0; s < 64u && in.n > 0u; s++) {
        do_step(&in);
    }
    return 0;
}
