/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_armv7m_jit.c - the ARMv7-M interpreter against the x86-64 JIT.
 *
 * The input is a configuration word and a program. Its first four bytes
 * (little-endian) choose the state the core starts in -- privilege,
 * stack, the CCR traps, the FPU's modes, Handler mode, the masks --
 * because nothing in this machine lets a program change CCR or CPACR,
 * and a block baked under one value of them is exactly what to check.
 * The rest is Thumb code, copied to the start of a read-only region
 * that is otherwise BKPT, and both backends run it from there on cores
 * and buses built identically.
 *
 * The interpreter is the reference: it is what tests/armv7m-diff holds
 * to a Cortex-M7, case by case.
 *
 * How every run ends at the same instruction on both sides:
 *
 *   - BKPT stops the core; it is the emulator's "this guest is done".
 *   - VTOR points at a table in ROM whose every vector -- reset,
 *     faults, SVCall, every interrupt -- is a BKPT. So any exception
 *     is taken, stacks its frame into RAM, and stops at the handler's
 *     first instruction. A fault nothing can take is a lockup, which
 *     stops the core too.
 *   - lr starts at that BKPT as well, so `bx lr` ends a program that
 *     reads like a function.
 *   - Code is ROM and data RAM is not executable, for the reasons the
 *     RV32 target gives: self-modifying code is the one thing an
 *     interpreter and a translator may legally disagree on, and a jump
 *     into data must fault rather than run what the program wrote.
 *   - The System Control Space is not mapped, so a program cannot
 *     enable SysTick or pend an interrupt. Both are taken at a block
 *     boundary under the JIT and an instruction boundary interpreted,
 *     which the architecture permits; with them reachable every run
 *     that used one would be a report. A configuration word is how the
 *     registers there that change instructions are reached instead.
 *
 * Built only with -DEMU_FUZZ=ON; see scripts/fuzz.sh, docs/fuzzing.md
 * and fuzz_harness.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armv7m/armv7m_backend.h"
#include "armv7m/armv7m_cpu.h"
#include "armv7m/armv7m_decode.h"
#include "armv7m/armv7m_disasm.h"
#include "emu/emu_bus.h"

#include "fuzz_harness.h"

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then BKPTs */
#define VEC_BASE 0x20010000u
#define VEC_STUB 0x400u /* offset of the BKPT every vector names */
#define VEC_SIZE 0x800u
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* r0-r12 start pointing here */
#define MSP_INIT (RAM_BASE + 0x1000u)
#define PSP_INIT (RAM_BASE + 0x1800u)

#define BKPT 0xBE00u

/* The configuration word: the input's first four bytes. */
#define CFG_NPRIV (1u << 0) /* Thread mode unprivileged          */
#define CFG_SPSEL (1u << 1) /* Thread mode on the process stack  */
#define CFG_UNALIGN (1u << 2) /* CCR.UNALIGN_TRP                   */
#define CFG_DIV0 (1u << 3) /* CCR.DIV_0_TRP                     */
#define CFG_NOFPU (1u << 4) /* CPACR denies CP10/11              */
#define CFG_RMODE_SHIFT 5 /* FPSCR.RMode, two bits             */
#define CFG_FZ (1u << 7) /* FPSCR.FZ                          */
#define CFG_DN (1u << 8) /* FPSCR.DN                          */
#define CFG_HANDLER (1u << 9) /* in SVCall's handler, with it active */
#define CFG_PRIMASK (1u << 10) /* PRIMASK set                       */
#define CFG_FAULTMASK (1u << 11)
#define CFG_BASEPRI (1u << 12) /* BASEPRI 0x40                      */
#define CFG_FAULTS                                                             \
    (1u << 13) /* SHCSR: MemManage, BusFault and
                                  UsageFault enabled, not escalated */

#define CCR_UNALIGN_TRP (1u << 3)
#define CCR_DIV_0_TRP (1u << 4)
#define SHCSR_FAULT_ENABLES 0x00070000u
#define CPACR_CP10_CP11 0x00F00000u

typedef struct side {
    fz_side_t run;
    armv7m_cpu_t cpu;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_cfg;
static uint8_t g_vec[VEC_SIZE];
static uint8_t g_ram_init[RAM_SIZE];

static side_t g_interp = {
    .run = {.name = "interp", .backend = &armv7m_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &armv7m_backend_jit}};

static void dump_program(void);

/*
 * Translated code keeps N, Z, C and V in four words of their own and
 * xPSR's copy stale; the frontend's run op packs them on the way out,
 * and this does what it does, so the comparison reads one xPSR.
 */
static void after_run(emu_cpu_t *cpu)
{
    armv7m_flags_pack((armv7m_cpu_t *)(void *)cpu);
}

static const fz_target_t g_target = {
    .name = "fuzz-armv7m-jit",
    .active = &armv7m_backend,
    .budget = 20000u,
    .slice = 1024u,
    .after_run = after_run,
    .dump = dump_program,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void once(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    put32(&g_vec[0], MSP_INIT);
    for (uint32_t i = 1; i < ARMV7M_NEXC; i++) {
        put32(&g_vec[i * 4u], (VEC_BASE + VEC_STUB) | 1u);
    }
    for (uint32_t i = VEC_STUB; i < VEC_SIZE; i += 2u) {
        put16(&g_vec[i], BKPT);
    }
    fz_fill(g_ram_init, RAM_SIZE, 0x9E3779B9u);
    g_interp.run.cpu = (emu_cpu_t *)(void *)&g_interp.cpu;
    g_jit.run.cpu = (emu_cpu_t *)(void *)&g_jit.cpu;
    fz_start(&g_target, &armv7m_backend_jit, g_jit.run.cpu);
}

/* The same machine on each side, from the same state. */
static void build(side_t *sd)
{
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW, /* not X: see the header */
        .widths = EMU_WANY,
    };
    armv7m_cpu_t *c = &sd->cpu;

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "vectors", VEC_BASE, g_vec, VEC_SIZE)) {
        fprintf(stderr, "%s: could not build the bus\n", g_target.name);
        abort();
    }

    /* What armv7m_ops_init and armv7m_ops_reset do, on our own core. */
    memset(c, 0, sizeof(*c));
    c->bus = &sd->bus;
    armv7m_reset_state(c);
    c->access_priv = true;
    c->state = EMU_STATE_RUNNING;
    c->vtor = VEC_BASE;

    for (uint32_t i = 0; i < 13u; i++) {
        c->r[i] = REG_BASE + i * 32u;
    }
    c->r[ARMV7M_LR] = (VEC_BASE + VEC_STUB) | 1u;
    c->r[ARMV7M_PC] = CODE_BASE;
    for (uint32_t i = 0; i < 32u; i++) {
        const float f = (float)((int)i - 8) * 1.5f;

        memcpy(&c->s[i], &f, 4u);
    }

    c->control = ((g_cfg & CFG_NPRIV) ? ARMV7M_CONTROL_NPRIV : 0u) |
                 ((g_cfg & CFG_SPSEL) ? ARMV7M_CONTROL_SPSEL : 0u);
    c->ccr |= ((g_cfg & CFG_UNALIGN) ? CCR_UNALIGN_TRP : 0u) |
              ((g_cfg & CFG_DIV0) ? CCR_DIV_0_TRP : 0u);
    c->cpacr = (g_cfg & CFG_NOFPU) ? 0u : CPACR_CP10_CP11;
    c->fpscr = (((g_cfg >> CFG_RMODE_SHIFT) & 3u) << 22) |
               ((g_cfg & CFG_FZ) ? (1u << 24) : 0u) |
               ((g_cfg & CFG_DN) ? (1u << 25) : 0u);
    c->primask = (g_cfg & CFG_PRIMASK) ? 1u : 0u;
    c->faultmask = (g_cfg & CFG_FAULTMASK) ? 1u : 0u;
    c->basepri = (g_cfg & CFG_BASEPRI) ? 0x40u : 0u;
    c->shcsr = (g_cfg & CFG_FAULTS) ? SHCSR_FAULT_ENABLES : 0u;
    if (g_cfg & CFG_HANDLER) {
        c->xpsr |= ARMV7M_EXC_SVCALL;
        c->active[ARMV7M_EXC_SVCALL / 32u] |= 1u << (ARMV7M_EXC_SVCALL % 32u);
    }

    /* r13 mirrors whichever bank is active. */
    c->msp = MSP_INIT;
    c->psp = PSP_INIT;
    c->r[ARMV7M_SP] =
        (!armv7m_handler_mode(c) && (c->control & ARMV7M_CONTROL_SPSEL))
            ? PSP_INIT
            : MSP_INIT;
}

#define SAME(field)                                                            \
    fz_diff(&g_target, #field, 0u, g_interp.cpu.field, g_jit.cpu.field)
#define SAME_AT(field, i)                                                      \
    fz_diff(&g_target, #field, (unsigned)(i), g_interp.cpu.field[i],           \
            g_jit.cpu.field[i])

static void dump_program(void)
{
    char buf[96];

    fprintf(stderr, "  (ram[n] is the word at 0x%08x + 4n)\n", RAM_BASE);
    fprintf(stderr, "  config 0x%08x; program, %zu bytes at 0x%08x:\n", g_cfg,
            g_len, CODE_BASE);
    for (size_t off = 0; off < g_len;) {
        const uint16_t hw = (uint16_t)(g_code[off] | (g_code[off + 1] << 8));
        const unsigned n = armv7m_insn_len(hw);
        const uint16_t w1 =
            (n == 4u) ? (uint16_t)(g_code[off + 2] | (g_code[off + 3] << 8))
                      : 0u;

        armv7m_disasm(buf, sizeof(buf), CODE_BASE + (uint32_t)off,
                      (uint64_t)hw | ((uint64_t)w1 << 16), n);
        if (n == 4u) {
            fprintf(stderr, "    %08x: %04x %04x  %s\n",
                    CODE_BASE + (unsigned)off, hw, w1, buf);
        } else {
            fprintf(stderr, "    %08x: %04x       %s\n",
                    CODE_BASE + (unsigned)off, hw, buf);
        }
        off += n;
    }
}

static void compare(void)
{
    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
    for (unsigned i = 0; i < ARMV7M_NREGS; i++) {
        SAME_AT(r, i);
    }
    SAME(xpsr);
    SAME(state);
    SAME(msp);
    SAME(psp);
    SAME(control);
    SAME(primask);
    SAME(faultmask);
    SAME(basepri);
    for (unsigned i = 0; i < ARMV7M_NEXC_WORDS; i++) {
        SAME_AT(pending, i);
        SAME_AT(active, i);
    }
    SAME(vtor);
    SAME(ccr);
    SAME(shcsr);
    SAME(cfsr);
    SAME(hfsr);
    SAME(mmfar);
    SAME(bfar);
    SAME(cpacr);
    SAME(excl_open);
    for (unsigned i = 0; i < 32u; i++) {
        SAME_AT(s, i);
    }
    SAME(fpscr);
    SAME(fpccr);
    SAME(fpcar);
    /*
     * The core's own count, which SysTick counts down: architectural
     * once SysTick is on, so the two backends must agree on it even
     * while nothing here can turn SysTick on.
     */
    SAME(retired);
    SAME(faulted);
    SAME(lockup);
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    once();
    g_cfg = 0u;
    if (size >= 4u) {
        g_cfg = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        data += 4;
        size -= 4u;
    } else {
        size = 0u;
    }
    g_len = (size < CODE_SIZE - 4u) ? size & ~(size_t)1 : CODE_SIZE - 4u;
    for (uint32_t i = 0; i < CODE_SIZE; i += 2u) {
        put16(&g_code[i], BKPT);
    }
    memcpy(g_code, data, g_len);

    build(&g_interp);
    build(&g_jit);
    if (fz_pair(&g_target, &g_interp.run, &g_jit.run)) {
        compare();
    }
    return 0;
}
