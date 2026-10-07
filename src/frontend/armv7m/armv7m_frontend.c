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

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include <string.h>

emu_run_reason_t armv7m_run(armv7m_cpu_t *c, uint32_t budget,
                            uint32_t *retired);

static armv7m_cpu_t g_cpu;

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
    c->xpsr = ARMV7M_T;
    c->state = EMU_STATE_RUNNING;
}

static void armv7m_ops_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    armv7m_cpu_t *const c = cpu_of(cpu);

    memset(c->r, 0, sizeof(c->r));
    c->xpsr = ARMV7M_T;
    c->faulted = false;
    c->fault_pc = 0u;
    c->fault_insn = 0u;
    c->state = EMU_STATE_RUNNING;
    c->r[ARMV7M_PC] = reset_pc;

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
    }
}

static emu_run_reason_t armv7m_ops_run(emu_cpu_t *cpu, uint32_t budget,
                                       uint32_t *retired)
{
    return armv7m_run(cpu_of(cpu), budget, retired);
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

static void armv7m_ops_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const armv7m_cpu_t *const c = cpu_of(cpu);

    memset(out, 0, sizeof(*out));
    out->pc = c->r[ARMV7M_PC];
    out->retired = c->retired;
    out->state = c->state;
    out->backend = "interp";
    out->faulted = c->faulted;
    out->fault_pc = c->fault_pc;
    out->fault_insn = c->fault_insn;
}

/*
 * The names AAPCS uses, because they are what a disassembly of any
 * compiled guest prints and what a person reading `info registers`
 * expects. r12 is ip, r13 sp, r14 lr, r15 pc.
 */
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
    .status = armv7m_ops_status,

    .reg_name = armv7m_reg_name,
    .reg_read = armv7m_reg_read,
    .reg_write = armv7m_reg_write,
};
