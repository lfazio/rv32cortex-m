/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_frontend.c - the e200z7 frontend's emu_cpu_ops_t.
 *
 * Shaped like rv32_frontend.c and g4mh_frontend.c, which is the point:
 * if a third frontend needs something neither of the first two has, it
 * belongs in the contract rather than in a platform #ifdef. This one is
 * the test of that claim, because it is the first big-endian guest --
 * and the only thing it needed beyond the existing contract was
 * emu_bus_set_big_endian().
 *
 * Note what is *not* here: no memory. The platform provides the backing
 * store, which is the rule docs/memory.md states and which the G4MH
 * frontend had to be rescued from.
 */

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include "ppc/ppc_cpu.h"

#include <string.h>

static ppc_cpu_t g_cpu;

static EMU_ALWAYS_INLINE ppc_cpu_t *cpu_of(const emu_cpu_t *cpu)
{
    return (ppc_cpu_t *)(uintptr_t)(const void *)cpu;
}

static emu_cpu_t *ppc_instance(unsigned index)
{
    return (index == 0u) ? (emu_cpu_t *)&g_cpu : NULL;
}

static void ppc_ops_init(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid)
{
    ppc_cpu_init(cpu_of(cpu), bus, coreid);
    if (ppc_backend->init != NULL && !ppc_backend->init(cpu)) {
        ppc_backend = &ppc_backend_interp;
    }
}

static void ppc_ops_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    ppc_cpu_reset(cpu_of(cpu), reset_pc);
}

static void ppc_ops_boot(emu_cpu_t *cpu, uint32_t ram_base, uint32_t ram_size)
{
    /*
     * The stack pointer convention is the ABI's, not the architecture's:
     * r1 is the stack pointer on every PowerPC ABI, and a guest linked
     * by gcc expects it set. Pointed at the top of RAM, 8-aligned as the
     * EABI requires.
     */
    ppc_cpu_t *c = cpu_of(cpu);
    c->r[1] = (ram_base + ram_size) & ~7u;
}

static emu_run_reason_t ppc_ops_run(emu_cpu_t *cpu, uint32_t budget,
                                    uint32_t *retired)
{
    return ppc_backend->run(cpu, budget, retired);
}

static emu_run_reason_t ppc_ops_step(emu_cpu_t *cpu)
{
    return ppc_step(cpu_of(cpu));
}

static void ppc_ops_halt(emu_cpu_t *cpu)
{
    cpu_of(cpu)->state = EMU_STATE_HALTED;
}

static void ppc_ops_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const ppc_cpu_t *c = cpu_of(cpu);

    memset(out, 0, sizeof(*out));
    out->pc      = c->pc;
    out->retired = c->retired;
    out->state   = c->state;
    out->backend = ppc_backend->name;
}

/*
 * Register names, in the ABI's terms where it has them. r1 is sp and r2
 * is the small-data pointer on the EABI; the rest are plain numbers,
 * which is what every PowerPC disassembler prints.
 */
static const char *ppc_reg_name(unsigned r)
{
    static const char *const k[PPC_NGPR] = {
        "r0", "sp", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16","r17","r18", "r19", "r20", "r21", "r22", "r23",
        "r24","r25","r26", "r27", "r28", "r29", "r30", "r31",
    };
    return (r < PPC_NGPR) ? k[r] : "?";
}

static uint32_t ppc_reg_read(const emu_cpu_t *cpu, unsigned r)
{
    return (r < PPC_NGPR) ? cpu_of(cpu)->r[r] : 0u;
}

static void ppc_reg_write(emu_cpu_t *cpu, unsigned r, uint32_t v)
{
    if (r < PPC_NGPR) {
        cpu_of(cpu)->r[r] = v;
    }
}

/*
 * Guest time. The time base counts up and the decrementer down, both
 * from this one call -- the platform's tick is the guest's clock, the
 * same arrangement the other two frontends use.
 */
static void ppc_ops_advance_time(emu_cpu_t *cpu, uint32_t ticks)
{
    ppc_cpu_advance(cpu_of(cpu), ticks);
}

static void ppc_ops_set_time(emu_cpu_t *cpu, uint64_t now)
{
    ppc_cpu_set_time(cpu_of(cpu), now);
}

/*
 * The external input. `level` false is honoured rather than dropped --
 * unlike the RV32 side, where the APLIC has no lower operation, this
 * core has no interrupt controller at all, so the line *is* the state.
 */
static void ppc_ops_set_irq(emu_cpu_t *cpu, uint32_t source, bool level)
{
    (void)source;
    ppc_cpu_set_ext(cpu_of(cpu), level);
}

static void ppc_set_syscall(emu_cpu_t *cpu, emu_syscall_fn fn, void *user)
{
    ppc_cpu_t *c = cpu_of(cpu);
    c->syscall = fn;
    c->syscall_user = user;
}

#if EMU_ENABLE_TRACE
static void ppc_set_trace(emu_cpu_t *cpu, emu_trace_fn fn, void *user)
{
    ppc_cpu_t *c = cpu_of(cpu);
    c->trace = fn;
    c->trace_user = user;
}
#endif

const emu_cpu_ops_t ppc_frontend = {
    .name        = "ppc",
    .nregs       = PPC_NGPR,
    .ncores      = 1u,
    /*
     * EM_PPC. The 64-bit variant has its own number and is a different
     * architecture as far as the loader is concerned, so it is not
     * listed as an alternate.
     */
    .elf_machine = 20u,

    .instance = ppc_instance,
    .init     = ppc_ops_init,
    .reset    = ppc_ops_reset,
    .boot     = ppc_ops_boot,
    .run      = ppc_ops_run,
    .step     = ppc_ops_step,
    .halt     = ppc_ops_halt,
    .status   = ppc_ops_status,

    .reg_name  = ppc_reg_name,
    .reg_read  = ppc_reg_read,
    .reg_write = ppc_reg_write,

    .advance_time = ppc_ops_advance_time,
    .set_time     = ppc_ops_set_time,
    .set_irq      = ppc_ops_set_irq,

    .set_syscall = ppc_set_syscall,
#if EMU_ENABLE_TRACE
    .set_trace   = ppc_set_trace,
#endif
};
