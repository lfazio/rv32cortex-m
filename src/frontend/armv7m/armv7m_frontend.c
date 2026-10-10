/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_frontend.c - the ARMv7E-M frontend's emu_cpu_ops_t.
 *
 * Everything a platform is allowed to know about this frontend. Shaped
 * like rv32_frontend.c, g4mh_frontend.c and ppc_frontend.c, because the
 * fourth one being the same file with different contents is the property
 * that says the contract is right.
 *
 * No backend selection here yet: there is only an interpreter. When a
 * JIT arrives it goes through `select_backend` as the other three do,
 * and *not* by assigning a global in init -- that arrangement is what
 * made a G4MH-only build reject `--jit` while running translated
 * anyway, so the interpreter it was supposedly being compared against
 * had never run.
 */

#include "armv7m/armv7m_backend.h"
#include "armv7m/armv7m_cpu.h"
#include "armv7m/armv7m_disasm.h"
#include "armv7m/armv7m_ir.h"

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include <string.h>


static armv7m_cpu_t g_cpu;

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

static emu_run_reason_t interp_run(emu_cpu_t *cpu, uint32_t budget,
                                   uint32_t *retired)
{
    return armv7m_run((armv7m_cpu_t *)(void *)cpu, budget, retired);
}

const emu_backend_t armv7m_backend_interp = {
    .name = "interp",
    .run = interp_run,
};

const emu_backend_t *armv7m_backend = &armv7m_backend_interp;

/*
 * A store landed in the range translated code was built from.
 *
 * The bitmap is what keeps this from being "any store near code": a
 * program loaded into RAM has its data within a few pages of its text,
 * and discarding every translation on every such store would be a
 * correctness-preserving way to have no JIT.
 */
void armv7m_smc_hit(armv7m_cpu_t *c, uint32_t addr)
{
    const uint32_t page = (addr - c->smc_base) >> 12;

    if (!c->smc_wide && page < 32u * (uint32_t)(sizeof(c->smc_bits) /
                                                 sizeof(c->smc_bits[0])) &&
        (c->smc_bits[page >> 5] & (1u << (page & 31u))) == 0u) {
        return;
    }
    c->smc_span = 0u;
    c->smc_wide = false;
    c->smc_watch = 0u;
    memset(c->smc_bits, 0, sizeof(c->smc_bits));
    if (armv7m_backend->invalidate != NULL) {
        armv7m_backend->invalidate((emu_cpu_t *)c, addr, 4u);
    }
}

static EMU_ALWAYS_INLINE armv7m_cpu_t *cpu_of(const emu_cpu_t *cpu)
{
    return (armv7m_cpu_t *)(uintptr_t)(const void *)cpu;
}

static emu_cpu_t *armv7m_instance(unsigned index)
{
    return (index == 0u) ? (emu_cpu_t *)&g_cpu : NULL;
}

static void armv7m_ops_init(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid)
{
    armv7m_cpu_t *const c = cpu_of(cpu);

    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->coreid = coreid;
    armv7m_reset_state(c);
    c->access_priv = true;
    c->state = EMU_STATE_RUNNING;
}

static void armv7m_ops_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    armv7m_cpu_t *const c = cpu_of(cpu);

    armv7m_reset_state(c);
    c->access_priv = true;
    c->state = EMU_STATE_RUNNING;
    c->r[ARMV7M_PC] = reset_pc;
    c->smc_span = 0u;
    c->smc_wide = false;
    c->smc_watch = 0u;
    memset(c->smc_bits, 0, sizeof(c->smc_bits));
    if (armv7m_backend->reset != NULL) {
        armv7m_backend->reset(cpu);
    }

    /*
     * VTOR follows the image, and **not the architecture's reset value
     * of zero**.
     *
     * On real silicon the vector table is at address 0 because that is
     * where the part maps its flash. Here a guest is linked at
     * EMU_GUEST_ROM_BASE and there is nothing at 0, so taking the
     * architectural default would read a table of zeroes, fall back to
     * the loader's entry point, and begin executing at the *table* --
     * whose first word is a stack pointer. That decodes as whatever the
     * address happens to look like, which is the silent-wrong-answer
     * shape rather than a fault.
     */
    c->vtor = reset_pc;
}

/*
 * The reset sequence, which on this architecture is a *table read* and
 * not a jump.
 *
 * Offset 0 of the vector table is the initial stack pointer and offset 4
 * is the reset address, with its low bit set because every branch target
 * in Thumb state carries it. A guest linked by any ARM toolchain
 * provides both, and reading them is what makes an ordinary `.bin` run
 * without the loader knowing where main is.
 *
 * **The low bit has to be masked off the pc.** It selects the
 * instruction set on a real core and is not part of the address; leaving
 * it in makes every fetch misaligned by one, which presents as garbage
 * instructions rather than as a bad branch.
 *
 * If the table is unreadable -- a guest loaded somewhere else, a bus
 * with no memory at VTOR -- the fall-back is the entry point the loader
 * gave us, and the stack goes to the top of RAM. That keeps a flat
 * `.bin` with no table runnable, which is what the other three
 * frontends' guests are.
 */
static void armv7m_ops_boot(emu_cpu_t *cpu, const emu_boot_info_t *info)
{
    armv7m_cpu_t *const c = cpu_of(cpu);
    uint32_t sp = 0u;
    uint32_t entry = 0u;

    c->r[ARMV7M_SP] = (info->ram_base + info->ram_size) & ~7u;

    if (emu_bus_read(c->bus, c->vtor, 4u, &sp) == EMU_FAULT_NONE &&
        emu_bus_read(c->bus, c->vtor + 4u, 4u, &entry) == EMU_FAULT_NONE &&
        sp != 0u && entry != 0u) {
        c->r[ARMV7M_SP] = sp & ~3u;
        c->r[ARMV7M_PC] = entry & ~1u;
        /* EPSR.T comes from the reset vector's low bit, as on silicon. */
        c->xpsr = (c->xpsr & ~ARMV7M_T) | ((entry & 1u) ? ARMV7M_T : 0u);
    }
    /* The main stack is the one reset selects, and r13 mirrors it. */
    c->msp = c->r[ARMV7M_SP];
}

/*
 * The SysTick and NVIC block, which every Cortex-M puts at the same
 * address -- so it is the frontend's device and not a platform's.
 *
 * add_core_devices rather than add_shared_devices: the SCS is banked per
 * core on a multi-core part, and getting that wrong on a single-core
 * frontend would be invisible now and wrong the moment there are two.
 */
static bool armv7m_add_core_devices(emu_cpu_t *cpu, emu_bus_t *bus,
                                    unsigned index)
{
    (void)index;
    return emu_bus_add_mmio(bus, "scs", ARMV7M_SCS_BASE, ARMV7M_SCS_SIZE,
                            &armv7m_scs_ops, cpu_of(cpu));
}

/*
 * A device raising its line. Source numbers are external IRQ numbers --
 * 0 is exception 16 -- which is the numbering a guest's NVIC_ISER bit
 * positions use, so a platform does not have to add the offset.
 */
static void armv7m_ops_set_irq(emu_cpu_t *cpu, uint32_t source, bool level)
{
    armv7m_set_irq(cpu_of(cpu), source, level);
}

static emu_run_reason_t armv7m_ops_run(emu_cpu_t *cpu, uint32_t budget,
                                       uint32_t *retired)
{
    const emu_run_reason_t why = armv7m_backend->run(cpu, budget, retired);

    /*
     * Whatever ran, xPSR is whole again before anything outside looks:
     * the status line, a state dump, a debugger. A no-op after the
     * interpreter, which never leaves the flags anywhere else.
     */
    armv7m_flags_pack(cpu_of(cpu));
    return why;
}

/*
 * The platform says which backend it wants, as it does for the other
 * frontends. On a host that is a statement about coverage: "the cases
 * match the board interpreted" and "they match through translated code"
 * are different claims, and a runner that inherited a default could make
 * neither honestly.
 */
static bool armv7m_select_backend(emu_cpu_t *cpu, bool want_jit)
{
#if EMU_HAVE_JIT
    armv7m_backend = want_jit ? &armv7m_backend_jit : &armv7m_backend_interp;
#else
    (void)want_jit;
    armv7m_backend = &armv7m_backend_interp;
#endif
    return armv7m_backend->init == NULL || armv7m_backend->init(cpu);
}

static void armv7m_ops_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    if (armv7m_backend->invalidate != NULL) {
        armv7m_backend->invalidate(cpu, addr, len);
    }
}

static emu_run_reason_t armv7m_ops_step(emu_cpu_t *cpu)
{
    uint32_t n = 0u;

    return armv7m_run(cpu_of(cpu), 1u, &n);
}

static void armv7m_ops_halt(emu_cpu_t *cpu)
{
    cpu_of(cpu)->state = EMU_STATE_HALTED;
}

static void armv7m_ops_set_trace(emu_cpu_t *cpu, emu_trace_fn fn, void *user)
{
#if EMU_ENABLE_TRACE
    armv7m_cpu_t *const c = cpu_of(cpu);

    c->trace = fn;
    c->trace_user = user;
#else
    (void)cpu;
    (void)fn;
    (void)user;
#endif
}

static void armv7m_ops_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const armv7m_cpu_t *const c = cpu_of(cpu);

    memset(out, 0, sizeof(*out));
    out->pc = c->r[ARMV7M_PC];
    out->retired = c->retired;
    out->state = c->state;
    out->backend = armv7m_backend->name;
    out->faulted = c->faulted;
    out->fault_pc = c->fault_pc;
    out->fault_insn = c->fault_insn;
}

/*
 * The names AAPCS uses, because they are what a disassembly of any
 * compiled guest prints and what a person reading `info registers`
 * expects. r12 is ip, r13 sp, r14 lr, r15 pc.
 */
static const char *hex8(char *b, uint32_t v)
{
    for (int i = 7; i >= 0; i--) {
        b[7 - i] = "0123456789abcdef"[(v >> (4 * i)) & 0xFu];
    }
    b[8] = '\0';
    return b;
}

/*
 * The state a fault leaves behind, in the order one reads it: where the
 * core is and in what mode, then the registers, then the stacks, the
 * masks, and the fault status that says why. CFSR is printed whole and
 * MMFAR/BFAR beside it, because which of the two addresses is valid is a
 * CFSR bit -- reading the wrong one is the classic misdiagnosis.
 */
static void armv7m_dump(const emu_cpu_t *cpu, emu_print_fn out, void *ctx)
{
    const armv7m_cpu_t *const c = cpu_of(cpu);
    char b[9];

    out(ctx, "\n  pc   ");
    out(ctx, hex8(b, c->r[ARMV7M_PC]));
    out(ctx, "  xpsr ");
    out(ctx, hex8(b, c->xpsr));
    out(ctx, "  ipsr ");
    out(ctx, hex8(b, c->xpsr & ARMV7M_IPSR_MASK));
    out(ctx, c->lockup ? "  LOCKUP" : "");
    for (unsigned i = 0u; i < 13u; i++) {
        static const char *const names[13] = {
            "r0  ", "r1  ", "r2  ", "r3  ", "r4  ", "r5  ", "r6  ",
            "r7  ", "r8  ", "r9  ", "r10 ", "r11 ", "r12 ",
        };

        out(ctx, (i % 4u) == 0u ? "\n  " : "  ");
        out(ctx, names[i]);
        out(ctx, hex8(b, c->r[i]));
    }
    out(ctx, "\n  sp   ");
    out(ctx, hex8(b, c->r[ARMV7M_SP]));
    out(ctx, "  lr   ");
    out(ctx, hex8(b, c->r[ARMV7M_LR]));
    out(ctx, "  msp  ");
    out(ctx, hex8(b, c->msp));
    out(ctx, "  psp  ");
    out(ctx, hex8(b, c->psp));
    out(ctx, "\n  control ");
    out(ctx, hex8(b, c->control));
    out(ctx, "  primask ");
    out(ctx, hex8(b, c->primask));
    out(ctx, "  faultmask ");
    out(ctx, hex8(b, c->faultmask));
    out(ctx, "  basepri ");
    out(ctx, hex8(b, c->basepri));
    out(ctx, "\n  cfsr ");
    out(ctx, hex8(b, c->cfsr));
    out(ctx, "  hfsr ");
    out(ctx, hex8(b, c->hfsr));
    out(ctx, "  mmfar ");
    out(ctx, hex8(b, c->mmfar));
    out(ctx, "  bfar ");
    out(ctx, hex8(b, c->bfar));
    out(ctx, "\n  last fault at ");
    out(ctx, hex8(b, c->fault_pc));
    out(ctx, " encoding ");
    out(ctx, hex8(b, c->fault_insn));
    out(ctx, "\n");
}

static const char *armv7m_reg_name(unsigned r)
{
    static const char *const k[ARMV7M_NREGS] = {
        "r0", "r1", "r2",  "r3",  "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "ip", "sp", "lr", "pc",
    };
    return (r < ARMV7M_NREGS) ? k[r] : "?";
}

static uint32_t armv7m_reg_read(const emu_cpu_t *cpu, unsigned r)
{
    return (r < ARMV7M_NREGS) ? cpu_of(cpu)->r[r] : 0u;
}

static void armv7m_reg_write(emu_cpu_t *cpu, unsigned r, uint32_t v)
{
    if (r < ARMV7M_NREGS) {
        cpu_of(cpu)->r[r] = v;
    }
}

#if EMU_HAVE_JIT
/*
 * How the translator handled what it was given: lowered, handed to the
 * interpreter from inside a block, or declined. Counted per instruction
 * *translated*, not per instruction executed -- so it says what the
 * translator met, and a loop counts once however often it runs.
 */
static void put_dec(emu_print_fn out, void *ctx, uint64_t v)
{
    char buf[24];
    unsigned i = sizeof(buf);

    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    out(ctx, &buf[i]);
}

static void put_top(emu_print_fn out, void *ctx, const char *title,
                    const uint32_t *by_op)
{
    bool used[ARMV7M_OP_COUNT] = {false};

    out(ctx, title);
    for (unsigned k = 0u; k < 12u; k++) {
        uint32_t best = 0u;
        uint32_t at = 0u;

        for (uint32_t op = 0u; op < (uint32_t)ARMV7M_OP_COUNT; op++) {
            if (!used[op] && by_op[op] > best) {
                best = by_op[op];
                at = op;
            }
        }
        if (best == 0u) {
            break;
        }
        used[at] = true;
        out(ctx, " ");
        out(ctx, armv7m_op_name(at));
        out(ctx, ":");
        put_dec(out, ctx, best);
    }
    out(ctx, "\n");
}

static void armv7m_report(emu_print_fn out, void *ctx)
{
    const armv7m_ir_stats_t *const s = armv7m_ir_get_stats();

    if (s->native + s->helper + s->declined == 0u) {
        return;
    }
    out(ctx, "\n-- armv7m translator (per instruction translated) --\n"
             "  lowered  ");
    put_dec(out, ctx, s->native);
    out(ctx, "  helper ");
    put_dec(out, ctx, s->helper);
    out(ctx, "  declined ");
    put_dec(out, ctx, s->declined);
    out(ctx, "\n");
    put_top(out, ctx, "  helper  ", s->helper_by_op);
    put_top(out, ctx, "  declined", s->declined_by_op);
}
#endif

const emu_cpu_ops_t armv7m_frontend = {
    .name = "armv7m",
    .desc = "ARM Cortex-M (ARMv7E-M, Thumb-2)",
    .nregs = ARMV7M_NREGS,
    .ncores = 1u,
    /* EM_ARM. A Thumb-2 image is an ARM ELF; the instruction set is
     * recorded in the entry point's low bit and in attributes, not in
     * e_machine. */
    .elf_machine = 40u,

    .instance = armv7m_instance,
    .init = armv7m_ops_init,
    .reset = armv7m_ops_reset,
    .boot = armv7m_ops_boot,
    .run = armv7m_ops_run,
    .step = armv7m_ops_step,
    .halt = armv7m_ops_halt,
    .set_trace = armv7m_ops_set_trace,
    .select_backend = armv7m_select_backend,
    .invalidate = armv7m_ops_invalidate,
    .status = armv7m_ops_status,

    .add_core_devices = armv7m_add_core_devices,
    .set_irq = armv7m_ops_set_irq,

    .dump = armv7m_dump,
    .reg_name = armv7m_reg_name,
    .reg_read = armv7m_reg_read,
    .reg_write = armv7m_reg_write,
#if ARMV7M_ENABLE_DISASM
    .disasm = armv7m_disasm,
#else
    .disasm = NULL,
#endif
#if EMU_HAVE_JIT
    .report = armv7m_report,
#endif
};
