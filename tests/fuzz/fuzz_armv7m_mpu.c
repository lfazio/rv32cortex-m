/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_armv7m_mpu.c - ARMv7-M's MPU against a reference written from the
 * architecture's pseudocode.
 *
 * fuzz-armv7m-jit compares the interpreter with the JIT, and both ask
 * armv7m_exc.c's one mpu_permits; tests/armv7m-diff asks the Cortex-M7
 * itself, a few configurations at a time. This target asks a reference
 * that shares nothing with mpu_permits: ValidateAddress(),
 * CheckPermission(), DefaultPermissions() and MemU_with_priv[] from the
 * ARMv7-M ARM (DDI 0403E, B3.5.8 and B2.3), with the details the board
 * answered where the pseudocode leaves the implementation room
 * (tests/armv7m-diff/sys.c `mpu-ua-*`: an unaligned access is bytes in
 * ascending order, MMFAR the first refused, the bytes before it stored).
 *
 * The input is a sequence of steps. A step writes an MPU region or
 * MPU_CTRL through the System Control Space -- the registers a program
 * writes -- or sets Thread mode's privilege and which faults are
 * enabled, or runs one instruction: placed in ROM at a pc chosen near a
 * region's end and followed by BKPT, with r0 pointing near a region's
 * end. It runs on two cores, one per backend, and every step is compared
 * with the reference: the exception taken (or none), CFSR, HFSR, MMFAR
 * and BFAR where valid, the stacked return address, the registers, and
 * the data RAM.
 *
 * What the reference reads from the cores is what software can: the
 * region registers through RNR/RBAR/RASR, after each write. Regions 6
 * and 7 are the harness's -- the handlers' BKPT, privileged and
 * executable, and the stack, read-write for all -- so that every fault
 * can be stacked and taken whatever the input does to the other six;
 * being the highest numbered, nothing the input writes overrides them.
 * The pseudocode's UNPREDICTABLE cases are not asked: AP 100, regions
 * under 32 bytes, subregions below 256, HFNMIENA without ENABLE, and an
 * unaligned access to anything but Normal memory.
 *
 * Built only with -DEMU_FUZZ=ON; see docs/fuzzing.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armv7m/armv7m_backend.h"
#include "armv7m/armv7m_cpu.h"
#include "armv7m/armv7m_decode.h"
#include "armv7m/armv7m_disasm.h"
#include "emu/emu_backend.h"
#include "emu/emu_bus.h"
#include "emu/emu_jit.h"

#if ARMV7M_MPU_REGIONS != 8
#error "fuzz-armv7m-mpu expects eight MPU regions"
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u
#define VEC_BASE 0x20010000u
#define VEC_SIZE 0x800u
#define VEC_STUB 0x400u /* every vector names the BKPT here */
#define STACK_BASE 0x30000000u
#define STACK_SIZE 0x1000u
#define STACK_TOP (STACK_BASE + 0xF00u)
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u

#define BKPT 0xBE00u

#define MPU_TYPE 0xE000ED90u
#define MPU_CTRL 0xE000ED94u
#define MPU_RNR 0xE000ED98u
#define MPU_RBAR 0xE000ED9Cu
#define MPU_RASR 0xE000EDA0u

#define CFSR_IACCVIOL (1u << 0)
#define CFSR_DACCVIOL (1u << 1)
#define CFSR_MMARVALID (1u << 7)
#define CFSR_PRECISERR (1u << 9)
#define CFSR_BFARVALID (1u << 15)
#define CFSR_UNALIGNED (1u << 24)
#define HFSR_FORCED (1u << 30)
#define SHCSR_MEMFAULTENA (1u << 16)
#define SHCSR_BUSFAULTENA (1u << 17)
#define SHCSR_USGFAULTENA (1u << 18)

#define SENT_MMFAR 0x0BAD0BADu
#define SENT_BFAR 0x0BAD0BAEu

/* Normal memory, TEX 001 C 0 B 0: an unaligned access is defined. */
#define RASR_NORMAL (1u << 19)

typedef struct side {
    const char *name;
    const emu_backend_t *backend;
    armv7m_cpu_t cpu;
    emu_bus_t bus;
    uint8_t code[CODE_SIZE], stack[STACK_SIZE], ram[RAM_SIZE];
} side_t;

static side_t g_side[2] = {
    {.name = "interp", .backend = &armv7m_backend_interp},
    {.name = "jit", .backend = &armv7m_backend_jit},
};
static uint8_t g_vec[VEC_SIZE];

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

static struct {
    uint32_t r[16];
    uint32_t xpsr;
    uint32_t exc; /* the exception taken, or 0 */
    uint32_t ret; /* its stacked return address */
    uint32_t cfsr, hfsr, mmfar, bfar;
    bool unknown_regs; /* an abandoned LDM or LDRD: UNKNOWN registers */
    bool bfar_unasked; /* a BusFault partway through an unaligned access */
    uint8_t ram[RAM_SIZE];
    /* What software reads back. */
    uint32_t rbar[8], rasr[8], ctrl;
    /* What the harness sets. */
    bool npriv;
    uint32_t shcsr;
} R;

static struct {
    unsigned step;
    const char *what;
    uint32_t pc;
    uint16_t hw[2];
    unsigned nhw;
} g_cur;

static struct {
    uint64_t inputs, steps, insns, completed, memmanage, busfault, usage,
        hardfault, forced, iacc, dacc, split, partial, translations, entries;
} g_st;

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-armv7m-mpu: %llu inputs, %llu steps, %llu instructions: "
            "%llu completed, %llu MemManage (%llu IACCVIOL, %llu DACCVIOL), "
            "%llu BusFault, %llu UsageFault, %llu HardFault (%llu forced); "
            "%llu unaligned accesses across a region's end, %llu stopped "
            "partway; jit %llu translations, %llu block entries\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.steps,
            (unsigned long long)g_st.insns, (unsigned long long)g_st.completed,
            (unsigned long long)g_st.memmanage, (unsigned long long)g_st.iacc,
            (unsigned long long)g_st.dacc, (unsigned long long)g_st.busfault,
            (unsigned long long)g_st.usage, (unsigned long long)g_st.hardfault,
            (unsigned long long)g_st.forced, (unsigned long long)g_st.split,
            (unsigned long long)g_st.partial,
            (unsigned long long)g_st.translations,
            (unsigned long long)g_st.entries);
}

typedef enum { A_FETCH, A_READ, A_WRITE } acc_t;

/*
 * ValidateAddress() and CheckPermission(), B3.5.8 and B2.3.8: the
 * highest-numbered enabled region holding the address, unless the
 * address's subregion is disabled in it; PRIVDEFENA's background for a
 * privileged access; the default map for the PPB, with the MPU off, and
 * below priority 0 without HFNMIENA; and XN over 0xE0000000 up.
 */
static bool ref_permits(uint32_t addr, acc_t acc, bool priv, bool neg_prio)
{
    uint32_t ap = 3u, xn;
    bool hit = false;

    switch (addr >> 29) { /* DefaultPermissions() */
    case 2u:
    case 5u:
    case 6u:
    case 7u:
        xn = 1u;
        break;
    default:
        xn = 0u;
        break;
    }
    if ((addr >> 20) == 0xE00u || (R.ctrl & 1u) == 0u ||
        (neg_prio && (R.ctrl & 2u) == 0u)) {
        hit = true;
    } else {
        if ((R.ctrl & 4u) != 0u && priv) {
            hit = true;
        }
        for (unsigned r = 0; r < 8u; r++) {
            const uint32_t rasr = R.rasr[r];
            const uint32_t lsbit = ((rasr >> 1) & 31u) + 1u;

            if ((rasr & 1u) == 0u) {
                continue;
            }
            if (lsbit != 32u && (addr >> lsbit) != (R.rbar[r] >> lsbit)) {
                continue;
            }
            const uint32_t sub =
                (lsbit >= 8u) ? (addr >> (lsbit - 3u)) & 7u : 0u;

            if (lsbit >= 8u && ((rasr >> (8u + sub)) & 1u) != 0u) {
                continue;
            }
            ap = (rasr >> 24) & 7u;
            xn = (rasr >> 28) & 1u;
            hit = true;
        }
    }
    if ((addr >> 29) == 7u) {
        xn = 1u;
    }
    if (!hit) {
        return false;
    }
    bool fault;
    const bool wr = acc == A_WRITE;

    switch (ap) {
    case 0u:
        fault = true;
        break;
    case 1u:
        fault = !priv;
        break;
    case 2u:
        fault = !priv && wr;
        break;
    case 3u:
        fault = false;
        break;
    case 5u:
        fault = !priv || wr;
        break;
    default: /* 6, 7; 4 is never written */
        fault = wr;
        break;
    }
    return !(fault || (acc == A_FETCH && xn != 0u));
}

/* RAM, read-write; the code, which is ROM: read-only to a data access. */
static uint8_t *ref_byte(uint32_t addr, bool write)
{
    if (addr - RAM_BASE < RAM_SIZE) {
        return &R.ram[addr - RAM_BASE];
    }
    if (!write && addr - CODE_BASE < CODE_SIZE) {
        return &g_side[0].code[addr - CODE_BASE];
    }
    return NULL;
}

/* An exception from an instruction at `pc`, as ExceptionTaken() and the
 * escalation rules of B1.5.12 have it at Thread-mode priority. */
static void ref_take(uint32_t exc, uint32_t pc)
{
    const uint32_t ena = (exc == 4u)   ? SHCSR_MEMFAULTENA
                         : (exc == 5u) ? SHCSR_BUSFAULTENA
                         : (exc == 6u) ? SHCSR_USGFAULTENA
                                       : 0u;

    if (ena != 0u && (R.shcsr & ena) == 0u) {
        R.hfsr |= HFSR_FORCED;
        exc = 3u;
        g_st.forced++;
    }
    R.exc = exc;
    R.ret = pc;
    g_st.memmanage += exc == 4u;
    g_st.busfault += exc == 5u;
    g_st.usage += exc == 6u;
    g_st.hardfault += exc == 3u;
}

/*
 * One aligned access of `size` bytes as MemA_with_priv[] makes it: the
 * MPU, then the bus. False with the fault recorded.
 */
static bool ref_mema(uint32_t pc, uint32_t addr, uint32_t size, acc_t acc,
                     bool unpriv, uint32_t *val)
{
    const bool priv = !unpriv && !R.npriv;

    if (!ref_permits(addr, acc, priv, false)) {
        R.cfsr |= CFSR_DACCVIOL;
        if ((R.cfsr & CFSR_MMARVALID) == 0u) {
            R.mmfar = addr;
            R.cfsr |= CFSR_MMARVALID;
        }
        g_st.dacc++;
        ref_take(4u, pc);
        return false;
    }
    if (ref_byte(addr, acc == A_WRITE) == NULL ||
        ref_byte(addr + size - 1u, acc == A_WRITE) == NULL) {
        R.cfsr |= CFSR_PRECISERR | CFSR_BFARVALID;
        R.bfar = addr;
        ref_take(5u, pc);
        return false;
    }
    uint8_t *const p = ref_byte(addr, acc == A_WRITE);

    if (acc == A_WRITE) {
        for (uint32_t i = 0; i < size; i++) {
            p[i] = (uint8_t)(*val >> (8u * i));
        }
    } else {
        uint32_t v = 0u;

        for (uint32_t i = 0; i < size; i++) {
            v |= (uint32_t)p[i] << (8u * i);
        }
        *val = v;
    }
    return true;
}

/* MemU_with_priv[]: aligned, or a byte at a time, ascending. */
static bool ref_memu(uint32_t pc, uint32_t addr, uint32_t size, acc_t acc,
                     bool unpriv, uint32_t *val)
{
    if ((addr & (size - 1u)) == 0u) {
        return ref_mema(pc, addr, size, acc, unpriv, val);
    }
    uint32_t v = 0u;

    if ((addr >> 5) != ((addr + size - 1u) >> 5)) {
        g_st.split++;
    }
    for (uint32_t i = 0; i < size; i++) {
        uint32_t b = (*val >> (8u * i)) & 0xFFu;

        if (!ref_mema(pc, addr + i, 1u, acc, unpriv, &b)) {
            g_st.partial += i != 0u;
            /* "For unaligned access faults, the value returned is the
             * address requested by the instruction" (BFAR) -- or the
             * byte's: not yet asked of the board. */
            R.bfar_unasked = (i != 0u) && (R.cfsr & CFSR_BFARVALID) != 0u;
            return false;
        }
        v |= (b & 0xFFu) << (8u * i);
    }
    if (acc == A_READ) {
        *val = v;
    }
    return true;
}

/* An access that must be aligned, or a UsageFault (MemA_with_priv[]). */
static bool ref_aligned(uint32_t pc, uint32_t addr)
{
    if ((addr & 3u) != 0u) {
        R.cfsr |= CFSR_UNALIGNED;
        ref_take(6u, pc);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Instructions                                                        */
/* ------------------------------------------------------------------ */

enum {
    K_LDR,
    K_LDRH,
    K_LDRB,
    K_STR,
    K_STRH,
    K_STRB,
    K_LDRSH,
    K_LDRW,
    K_STRW,
    K_LDRD,
    K_STRD,
    K_LDM,
    K_STM,
    K_LDRT,
    K_STRT,
    K_LDREX,
    K_NOP,
    K_NOPW,
    K_LDRLIT,
    K_COUNT
};

static const char *const k_name[K_COUNT] = {
    "ldr",   "ldrh",    "ldrb",       "str",         "strh",
    "strb",  "ldrsh.w", "ldr.w #imm", "str.w #imm",  "ldrd",
    "strd",  "ldm.w",   "stm.w",      "ldrt",        "strt",
    "ldrex", "nop",     "nop.w",      "ldr literal",
};

/* r0 the address, r1 the value stored, r2.. the results. */
static unsigned encode(unsigned k, uint32_t imm, uint16_t *hw)
{
    static const uint16_t k16[] = {
        [K_LDR] = 0x6802u, [K_LDRH] = 0x8802u, [K_LDRB] = 0x7802u,
        [K_STR] = 0x6001u, [K_STRH] = 0x8001u, [K_STRB] = 0x7001u,
        [K_NOP] = 0xBF00u,
    };

    switch (k) {
    case K_LDRSH:
        hw[0] = 0xF9B0u;
        hw[1] = 0x2000u;
        return 2;
    case K_LDRW:
        hw[0] = 0xF8D0u;
        hw[1] = (uint16_t)(0x2000u | (imm & 0xFFFu));
        return 2;
    case K_STRW:
        hw[0] = 0xF8C0u;
        hw[1] = (uint16_t)(0x1000u | (imm & 0xFFFu));
        return 2;
    case K_LDRD:
        hw[0] = 0xE9D0u;
        hw[1] = 0x2300u;
        return 2;
    case K_STRD:
        hw[0] = 0xE9C0u;
        hw[1] = 0x1200u;
        return 2;
    case K_LDM:
        hw[0] = 0xE890u;
        hw[1] = 0x003Cu; /* r2-r5 */
        return 2;
    case K_STM:
        hw[0] = 0xE880u;
        hw[1] = 0x001Eu; /* r1-r4 */
        return 2;
    case K_LDRT:
        hw[0] = 0xF850u;
        hw[1] = 0x2E00u;
        return 2;
    case K_STRT:
        hw[0] = 0xF840u;
        hw[1] = 0x1E00u;
        return 2;
    case K_LDREX:
        hw[0] = 0xE850u;
        hw[1] = 0x2F00u;
        return 2;
    case K_NOPW:
        hw[0] = 0xF3AFu;
        hw[1] = 0x8000u;
        return 2;
    case K_LDRLIT: /* ldr r2, [pc, #imm8 * 4]: a constant from the code */
        hw[0] = (uint16_t)(0x4A00u | (imm & 0xFFu));
        return 1;
    default:
        hw[0] = k16[k];
        return 1;
    }
}

static bool ref_exec(unsigned k, uint32_t pc, uint32_t imm)
{
    const uint32_t a = R.r[0];
    uint32_t v = 0u;

    switch (k) {
    case K_LDR:
    case K_LDRH:
    case K_LDRB:
    case K_LDRSH:
    case K_LDRW:
    case K_LDRT: {
        const uint32_t size = (k == K_LDRB)                   ? 1u
                              : (k == K_LDRH || k == K_LDRSH) ? 2u
                                                              : 4u;
        const uint32_t addr = a + ((k == K_LDRW) ? (imm & 0xFFFu) : 0u);

        if (!ref_memu(pc, addr, size, A_READ, k == K_LDRT, &v)) {
            return false;
        }
        if (k == K_LDRSH) {
            v = (uint32_t)(int32_t)(int16_t)v;
        }
        R.r[2] = v;
        return true;
    }
    case K_STR:
    case K_STRH:
    case K_STRB:
    case K_STRW:
    case K_STRT: {
        const uint32_t size = (k == K_STRB) ? 1u : (k == K_STRH) ? 2u : 4u;
        const uint32_t addr = a + ((k == K_STRW) ? (imm & 0xFFFu) : 0u);

        v = R.r[1];
        return ref_memu(pc, addr, size, A_WRITE, k == K_STRT, &v);
    }
    case K_LDRD:
    case K_LDM:
    case K_LDREX: {
        const unsigned n = (k == K_LDM) ? 4u : (k == K_LDRD) ? 2u : 1u;
        uint32_t got[4];

        if (!ref_aligned(pc, a)) {
            return false;
        }
        for (unsigned i = 0; i < n; i++) {
            if (!ref_mema(pc, a + 4u * i, 4u, A_READ, false, &got[i])) {
                /* B1.5.10: an abandoned load leaves its destinations
                 * UNKNOWN; the base, here not written back, is kept. */
                R.unknown_regs = i != 0u;
                g_st.partial += i != 0u;
                return false;
            }
        }
        for (unsigned i = 0; i < n; i++) {
            R.r[2u + i] = got[i];
        }
        return true;
    }
    case K_STRD:
    case K_STM: {
        const unsigned n = (k == K_STM) ? 4u : 2u;

        if (!ref_aligned(pc, a)) {
            return false;
        }
        for (unsigned i = 0; i < n; i++) {
            v = R.r[1u + i];
            if (!ref_mema(pc, a + 4u * i, 4u, A_WRITE, false, &v)) {
                g_st.partial += i != 0u;
                return false;
            }
        }
        return true;
    }
    case K_LDRLIT: /* Align(PC, 4) + imm32, where PC reads pc + 4 */
        if (!ref_mema(pc, ((pc + 4u) & ~3u) + (imm & 0xFFu) * 4u, 4u, A_READ,
                      false, &v)) {
            return false;
        }
        R.r[2] = v;
        return true;
    default: /* the NOPs */
        return true;
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

/* Near where things are: code, RAM, or an end of an input region. */
static uint32_t pick_near(in_t *in)
{
    const uint32_t sel = byte(in);
    const uint32_t k = half(in);

    switch (sel % 6u) {
    case 0:
        return CODE_BASE + (k % CODE_SIZE);
    case 1:
    case 2:
        return RAM_BASE + (k % RAM_SIZE);
    case 3:
    case 4: { /* a byte or a few either side of a region's end */
        const unsigned r = k % 6u;
        const uint32_t lsbit = ((R.rasr[r] >> 1) & 31u) + 1u;
        const uint32_t size = (lsbit >= 32u) ? 0u : (1u << lsbit);
        const uint32_t base = R.rbar[r] & ~(size - 1u) & ~31u;
        const uint32_t sub = (sel & 0x80u) ? (size / 8u) * ((k >> 3) & 7u) : 0u;

        return base + ((sel & 0x40u) ? size : sub) - 4u + ((k >> 6) & 7u);
    }
    default:
        return RAM_BASE + RAM_SIZE - 64u + (k & 63u);
    }
}

/* ------------------------------------------------------------------ */
/* The two cores                                                       */
/* ------------------------------------------------------------------ */

static void scs_write(uint32_t addr, uint32_t v)
{
    for (unsigned s = 0; s < 2u; s++) {
        if (emu_bus_write(&g_side[s].bus, addr, 4u, v) != EMU_FAULT_NONE) {
            fprintf(stderr, "fuzz-armv7m-mpu: SCS write 0x%08x faulted\n",
                    addr);
            abort();
        }
    }
}

static uint32_t scs_read(unsigned s, uint32_t addr)
{
    uint32_t v = 0u;

    (void)emu_bus_read(&g_side[s].bus, addr, 4u, &v);
    return v;
}

static void read_back(void)
{
    for (unsigned r = 0; r < 8u; r++) {
        scs_write(MPU_RNR, r);
        for (unsigned s = 0; s < 2u; s++) {
            const uint32_t rbar = scs_read(s, MPU_RBAR);
            const uint32_t rasr = scs_read(s, MPU_RASR);

            if (s == 0u) {
                R.rbar[r] = rbar;
                R.rasr[r] = rasr;
            } else if (rbar != R.rbar[r] || rasr != R.rasr[r]) {
                fprintf(stderr, "fuzz-armv7m-mpu: the two MPUs read back "
                                "differently\n");
                abort();
            }
        }
    }
    R.ctrl = scs_read(0, MPU_CTRL);
}

static void write_region(unsigned r, uint32_t rbar, uint32_t rasr)
{
    scs_write(MPU_RNR, r);
    scs_write(MPU_RBAR, rbar & ~0x1Fu);
    scs_write(MPU_RASR, rasr);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void build_side(side_t *sd)
{
    armv7m_cpu_t *const c = &sd->cpu;
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };
    const emu_region_t stack = {
        .base = STACK_BASE,
        .size = STACK_SIZE,
        .host = sd->stack,
        .name = "stack",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW,
        .widths = EMU_WANY,
    };

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, R.ram, RAM_SIZE);
    memset(sd->stack, 0, STACK_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) || !emu_bus_add(&sd->bus, &stack) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, sd->code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "vectors", VEC_BASE, g_vec, VEC_SIZE) ||
        !emu_bus_add_mmio(&sd->bus, "scs", ARMV7M_SCS_BASE, ARMV7M_SCS_SIZE,
                          &armv7m_scs_ops, c)) {
        fprintf(stderr, "fuzz-armv7m-mpu: could not build the bus\n");
        abort();
    }
    memset(c, 0, sizeof(*c));
    c->bus = &sd->bus;
    armv7m_reset_state(c);
    c->access_priv = true;
    c->vtor = VEC_BASE;
    for (uint32_t i = 0; i < CODE_SIZE; i += 2u) {
        put16(&sd->code[i], BKPT);
    }
}

static void dump(void)
{
    char buf[96];

    armv7m_disasm(buf, sizeof(buf), g_cur.pc,
                  (uint64_t)g_cur.hw[0] | ((uint64_t)g_cur.hw[1] << 16),
                  2u * g_cur.nhw);
    fprintf(stderr, "  step %u: %s at 0x%08x (%s), Thread %s, SHCSR 0x%08x\n",
            g_cur.step, g_cur.what, g_cur.pc, buf,
            R.npriv ? "unprivileged" : "privileged", R.shcsr);
    fprintf(stderr, "  r0 0x%08x r1 0x%08x  MPU_CTRL %u\n", R.r[0], R.r[1],
            R.ctrl);
    for (unsigned r = 0; r < 8u; r++) {
        if ((R.rasr[r] & 1u) != 0u) {
            fprintf(stderr, "  region %u  rbar 0x%08x rasr 0x%08x\n", r,
                    R.rbar[r], R.rasr[r]);
        }
    }
}

static unsigned g_fails;

static void expect(const side_t *sd, const char *what, unsigned idx,
                   uint32_t got, uint32_t want)
{
    if (got == want) {
        return;
    }
    if (g_fails++ == 0u) {
        fprintf(stderr,
                "fuzz-armv7m-mpu: %s disagrees with the "
                "architecture\n",
                sd->name);
        dump();
    }
    fprintf(stderr, "  %-6s %-6s[%2u] got 0x%08x want 0x%08x\n", sd->name, what,
            idx, got, want);
}

static void run_side(side_t *sd, uint32_t pc, const uint32_t *regs)
{
    armv7m_cpu_t *const c = &sd->cpu;
    emu_jit_stats_t before, after;
    uint32_t total = 0u;

    memcpy(c->r, regs, sizeof(c->r));
    c->r[ARMV7M_PC] = pc;
    c->r[ARMV7M_LR] = (VEC_BASE + VEC_STUB) | 1u;
    c->msp = STACK_TOP;
    c->psp = STACK_TOP - 0x400u;
    c->r[ARMV7M_SP] = STACK_TOP;
    c->xpsr = 1u << 24; /* Thumb, Thread mode */
    c->control = R.npriv ? ARMV7M_CONTROL_NPRIV : 0u;
    memset(c->active, 0, sizeof(c->active));
    memset(c->pending, 0, sizeof(c->pending));
    c->primask = c->faultmask = c->basepri = 0u;
    c->shcsr = R.shcsr;
    c->cfsr = 0u;
    c->hfsr = 0u;
    c->mmfar = SENT_MMFAR;
    c->bfar = SENT_BFAR;
    c->excl_open = false;
    c->lockup = false;
    c->faulted = false;
    c->state = EMU_STATE_RUNNING;
    c->jit_gen++;

    armv7m_backend = sd->backend;
    if (sd->backend->reset != NULL) {
        sd->backend->reset((emu_cpu_t *)c); /* the code is new */
    }
    emu_jit_get_stats(&before);
    while (total < 64u && c->state == EMU_STATE_RUNNING) {
        uint32_t n = 0u;

        (void)sd->backend->run((emu_cpu_t *)c, 16u, &n);
        total += n + 1u;
    }
    armv7m_flags_pack(c);
    emu_jit_get_stats(&after);
    if (sd == &g_side[1]) {
        g_st.translations += after.translations - before.translations;
        g_st.entries += after.block_entries - before.block_entries;
    }
}

static void compare(const side_t *sd)
{
    const armv7m_cpu_t *const c = &sd->cpu;
    const uint32_t exc = c->xpsr & 0x1FFu;

    expect(sd, "lockup", 0u, c->lockup, 0u);
    expect(sd, "exc", 0u, exc, R.exc);
    expect(sd, "pc", 0u, c->r[ARMV7M_PC],
           R.exc ? VEC_BASE + VEC_STUB : g_cur.pc + 2u * g_cur.nhw);
    expect(sd, "cfsr", 0u, c->cfsr, R.cfsr);
    expect(sd, "hfsr", 0u, c->hfsr, R.hfsr);
    expect(sd, "mmfar", 0u, c->mmfar,
           (R.cfsr & CFSR_MMARVALID) ? R.mmfar : SENT_MMFAR);
    if (!R.bfar_unasked) {
        expect(sd, "bfar", 0u, c->bfar,
               (R.cfsr & CFSR_BFARVALID) ? R.bfar : SENT_BFAR);
    }
    if (R.exc != 0u) {
        uint32_t ret = 0u;
        const uint32_t sp = c->r[ARMV7M_SP];

        if (sp - STACK_BASE < STACK_SIZE - 32u) {
            memcpy(&ret, &sd->stack[sp - STACK_BASE + 24u], 4u);
        }
        expect(sd, "stacked-pc", 0u, ret, R.ret);
    }
    for (unsigned i = 0; i < 13u; i++) {
        if (R.unknown_regs && i >= 2u && i <= 5u) {
            continue;
        }
        expect(sd, "r", i, c->r[i], R.r[i]);
    }
    for (uint32_t i = 0; i < RAM_SIZE; i += 4u) {
        uint32_t a, b;

        memcpy(&a, &sd->ram[i], 4u);
        memcpy(&b, &R.ram[i], 4u);
        if (a != b) {
            expect(sd, "ram@", 0u, RAM_BASE + i, 0u);
            expect(sd, "word", 0u, a, b);
            break;
        }
    }
}

/* A pc for an instruction of `len` bytes and its BKPT. */
static uint32_t pick_pc(in_t *in, uint32_t len)
{
    const uint32_t room = CODE_SIZE - len - 2u;
    const uint32_t sel = byte(in);
    uint32_t off = (half(in) % room) & ~1u;

    if ((sel & 3u) != 0u) {
        /* Ending at, or straddling, an input region's end in the code. */
        const unsigned r = (sel >> 2) % 6u;
        const uint32_t lsbit = ((R.rasr[r] >> 1) & 31u) + 1u;
        const uint32_t size = (lsbit >= 32u) ? 0u : (1u << lsbit);
        const uint32_t end = (R.rbar[r] & ~(size - 1u)) + size;
        const uint32_t pc = end - 2u * (1u + ((sel >> 5) & 3u));

        if (pc - CODE_BASE <= room) {
            off = (pc - CODE_BASE) & ~1u;
        }
    }
    return CODE_BASE + off;
}

static void do_insn(in_t *in, unsigned k)
{
    uint16_t hw[2] = {0u, 0u};
    const uint32_t imm = (k == K_LDRLIT) ? half(in) & 0xFFu : half(in) & 0x1Fu;
    const unsigned n = encode(k, imm, hw);
    const uint32_t len = 2u * n;
    const uint32_t pc = pick_pc(in, len);
    uint32_t regs[16];

    for (unsigned i = 0; i < 13u; i++) {
        R.r[i] = 0x9E3779B9u * (i + 13u * (unsigned)g_cur.step);
    }
    R.r[0] = pick_near(in) - ((k == K_LDRW || k == K_STRW) ? imm : 0u);
    R.r[13] = STACK_TOP;
    memcpy(regs, R.r, sizeof(regs));

    g_cur.what = k_name[k];
    g_cur.pc = pc;
    g_cur.nhw = n;
    g_cur.hw[0] = hw[0];
    g_cur.hw[1] = hw[1];
    for (unsigned s = 0; s < 2u; s++) {
        uint8_t *const code = g_side[s].code;

        for (uint32_t i = 0; i < CODE_SIZE; i += 2u) {
            put16(&code[i], BKPT);
        }
        for (unsigned i = 0; i < n; i++) {
            put16(&code[pc - CODE_BASE + 2u * i], hw[i]);
        }
    }

    /* The reference: fetch every halfword, execute, fetch the BKPT. */
    R.exc = 0u;
    R.cfsr = R.hfsr = 0u;
    R.unknown_regs = false;
    R.bfar_unasked = false;
    {
        const bool priv = !R.npriv;
        bool ok = true;

        for (uint32_t off = 0; off < len; off += 2u) {
            if (!ref_permits(pc + off, A_FETCH, priv, false)) {
                R.cfsr |= CFSR_IACCVIOL;
                g_st.iacc++;
                ref_take(4u, pc);
                ok = false;
                break;
            }
        }
        if (ok) {
            ok = ref_exec(k, pc, imm);
        }
        if (ok) {
            if (!ref_permits(pc + len, A_FETCH, priv, false)) {
                R.cfsr |= CFSR_IACCVIOL;
                g_st.iacc++;
                ref_take(4u, pc + len);
            } else {
                g_st.completed++;
            }
        }
    }

    for (unsigned s = 0; s < 2u; s++) {
        run_side(&g_side[s], pc, regs);
        compare(&g_side[s]);
    }
    if (g_fails != 0u) {
        abort();
    }
    /* RAM as the cores left it, if the reference could not say. */
    g_st.insns++;
}

static void do_step(in_t *in)
{
    const uint32_t op = byte(in);

    g_cur.step++;
    g_st.steps++;
    switch (op % 16u) {
    case 0:
    case 1:
    case 2: { /* a region */
        const unsigned r = byte(in) % 6u;
        const uint32_t sel = byte(in);
        /* 32 bytes to 4 KB, mostly; sometimes everything. */
        const uint32_t sz = (sel & 0x80u) ? 31u : 4u + (sel & 7u);
        static const uint8_t k_ap[7] = {0u, 1u, 2u, 3u, 5u, 6u, 7u};
        const uint32_t attr = byte(in);
        const uint32_t srd = (sz >= 7u) ? byte(in) : 0u;
        uint32_t rasr = ((uint32_t)k_ap[attr % 7u] << 24) |
                        (((attr >> 3) & 1u) << 28) | RASR_NORMAL | (srd << 8) |
                        (sz << 1);

        if ((attr & 0x80u) == 0u) {
            rasr |= 1u; /* enabled, mostly */
        }
        write_region(r, pick_near(in), rasr);
        read_back();
        break;
    }
    case 3: { /* MPU_CTRL: no HFNMIENA without ENABLE */
        const uint32_t v = byte(in) & 7u;

        scs_write(MPU_CTRL, (v & 1u) ? v : (v & 4u));
        read_back();
        break;
    }
    case 4: { /* Thread privilege and the fault enables */
        const uint32_t sel = byte(in);

        R.npriv = (sel & 1u) != 0u;
        R.shcsr = ((sel & 2u) ? SHCSR_MEMFAULTENA : 0u) |
                  ((sel & 4u) ? SHCSR_BUSFAULTENA : 0u) |
                  ((sel & 8u) ? SHCSR_USGFAULTENA : 0u);
        break;
    }
    default:
        do_insn(in, byte(in) % K_COUNT);
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
        for (uint32_t i = 0; i < 4u; i++) {
            g_vec[i] = (uint8_t)(STACK_TOP >> (8u * i));
        }
        for (uint32_t e = 1; e < ARMV7M_NEXC; e++) {
            const uint32_t v = (VEC_BASE + VEC_STUB) | 1u;

            for (uint32_t i = 0; i < 4u; i++) {
                g_vec[e * 4u + i] = (uint8_t)(v >> (8u * i));
            }
        }
        for (uint32_t i = VEC_STUB; i < VEC_SIZE; i += 2u) {
            put16(&g_vec[i], BKPT);
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
        if (armv7m_backend_jit.init != NULL &&
            !armv7m_backend_jit.init((emu_cpu_t *)&g_side[1].cpu)) {
            fprintf(stderr, "fuzz-armv7m-mpu: the JIT did not start\n");
            abort();
        }
    }
    g_st.inputs++;

    memset(&R, 0, sizeof(R));
    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        R.ram[i] = (uint8_t)(seed >> 24);
    }
    for (unsigned s = 0; s < 2u; s++) {
        build_side(&g_side[s]);
    }
    /*
     * The harness's two regions: the handlers' BKPT, privileged read-only
     * and executable, and the stack, read-write for all. Highest numbered,
     * so nothing the input writes overrides them.
     */
    write_region(6u, VEC_BASE, (5u << 24) | RASR_NORMAL | (10u << 1) | 1u);
    write_region(7u, STACK_BASE,
                 (1u << 28) | (3u << 24) | RASR_NORMAL | (11u << 1) | 1u);
    read_back();
    R.shcsr = SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA | SHCSR_USGFAULTENA;
    g_cur.step = 0u;
    g_fails = 0u;

    for (unsigned s = 0; s < 64u && in.n > 0u; s++) {
        do_step(&in);
        /* The reference's RAM is the cores' after every step. */
        memcpy(R.ram, g_side[0].ram, RAM_SIZE);
    }
    return 0;
}
