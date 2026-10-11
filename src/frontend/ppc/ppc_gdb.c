/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_gdb.c - what gdb needs to know about the e200z7, and nothing else.
 * The counterpart of rv_gdb.c, g4mh_gdb.c and armv7m_gdb.c: the protocol
 * is src/emu/emu_gdb.c and has no ISA in it.
 *
 * The register file is gdb's "org.gnu.gdb.power.core" feature, which
 * gdb insists on by name and checks register by register: r0..r31, pc,
 * msr, cr, lr, ctr, xer. Thirty-eight registers numbered by position,
 * and the pc is 32 -- five control registers follow it, so it is *not*
 * the last, which the stub assumed until there was a target here to
 * make that wrong.
 *
 * **This is the first big-endian target, and the stub encoded every
 * register little-endian.** gdb reads a `g` packet in the target's
 * byte order; the stub now takes that order from the bus, where
 * ppc_frontend.c states it once, so nothing here says it again.
 * scripts/gdb-check.sh holds the pc at a breakpoint against the ELF's
 * symbol for `main`, which is the comparison that can see it: a
 * register written and read back cannot, because a swapped encoding is
 * swapped both ways.
 *
 * Not described, and so not visible to gdb:
 *
 *   - the SPE upper halves and SPEFSCR. gdb's "org.gnu.gdb.power.spe"
 *     requires `acc`, a 64-bit register, and the stub's registers are
 *     all reg_bytes wide. EFPU2 keeps its operands in the GPRs, so the
 *     single-precision values are visible as r0..r31 regardless.
 *   - the SPRs: SRR0/1, CSRR0/1, ESR, DEAR, the IVORs. They are in the
 *     state dump.
 */

#include "emu/emu_gdb.h"
#include "ppc/ppc_cpu.h"

#define PPC_GDB_PC 32u
#define PPC_GDB_MSR 33u
#define PPC_GDB_CR 34u
#define PPC_GDB_LR 35u
#define PPC_GDB_CTR 36u
#define PPC_GDB_XER 37u
#define PPC_GDB_NREGS 38u

static ppc_cpu_t *cpu_of_gdb(const emu_cpu_t *cpu)
{
    /* Same cast ppc_frontend.c makes: the core *is* the cpu. */
    return (ppc_cpu_t *)(void *)(uintptr_t)cpu;
}

static uint32_t ppc_gdb_reg_get(const emu_cpu_t *cpu, unsigned n)
{
    const ppc_cpu_t *c = cpu_of_gdb(cpu);

    if (n < 32u) {
        return c->r[n];
    }
    switch (n) {
    case PPC_GDB_PC:
        return c->pc;
    case PPC_GDB_MSR:
        return c->msr;
    case PPC_GDB_CR:
        return c->cr;
    case PPC_GDB_LR:
        return c->lr;
    case PPC_GDB_CTR:
        return c->ctr;
    case PPC_GDB_XER:
        return c->xer;
    default:
        return 0u;
    }
}

static void ppc_gdb_reg_set(emu_cpu_t *cpu, unsigned n, uint32_t v)
{
    ppc_cpu_t *c = cpu_of_gdb(cpu);

    if (n < 32u) {
        c->r[n] = v;
        return;
    }
    switch (n) {
    case PPC_GDB_PC:
        c->pc = v;
        break;
    case PPC_GDB_MSR:
        /*
         * Written whole, as mtmsr would not be: a debugger setting
         * EE has to see what EE does. Interrupt delivery re-reads MSR
         * before each run, and the translator's context is taken from
         * the state when the next run binds.
         */
        c->msr = v;
        c->irq_dirty = true;
        break;
    case PPC_GDB_CR:
        c->cr = v;
        break;
    case PPC_GDB_LR:
        c->lr = v;
        break;
    case PPC_GDB_CTR:
        c->ctr = v;
        break;
    case PPC_GDB_XER:
        c->xer = v;
        break;
    default:
        break;
    }
}

static uint32_t ppc_gdb_pc_get(const emu_cpu_t *cpu)
{
    return cpu_of_gdb(cpu)->pc;
}

static void ppc_gdb_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    cpu_of_gdb(cpu)->pc = pc;
}

/*
 * gdb's core feature, in reg_get's order. `powerpc:common` rather than
 * `powerpc:vle`: the frontend runs Book E images as well as VLE ones,
 * and an ELF tells gdb which it has -- the description should not
 * claim one encoding for both.
 */
static const char k_ppc_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>powerpc:common</architecture>"
    "<feature name=\"org.gnu.gdb.power.core\">"
    "<reg name=\"r0\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r1\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r2\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r3\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r4\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r5\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r6\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r7\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r8\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r9\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r10\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r11\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r12\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r13\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r14\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r15\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r16\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r17\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r18\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r19\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r20\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r21\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r22\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r23\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r24\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r25\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r26\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r27\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r28\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r29\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r30\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"r31\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"msr\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"cr\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"ctr\" bitsize=\"32\" type=\"uint32\"/>"
    "<reg name=\"xer\" bitsize=\"32\" type=\"uint32\"/>"
    "</feature>"
    "</target>";

static const emu_gdb_target_t k_ppc_gdb_target = {
    .nregs = PPC_GDB_NREGS,
    .reg_bytes = 4u,
    .pc_regno = PPC_GDB_PC, /* not the last: msr..xer follow it */
    .reg_get = ppc_gdb_reg_get,
    .reg_set = ppc_gdb_reg_set,
    .pc_get = ppc_gdb_pc_get,
    .pc_set = ppc_gdb_pc_set,
    .stop_signal = 5, /* SIGTRAP: an attach looks like a trap */
    .arch = "powerpc:common",
    .target_xml = k_ppc_xml,
    /* No memory map, for armv7m_gdb.c's reason. */
    .memory_map = NULL,
};

const emu_gdb_target_t *ppc_gdb_target(void);
const emu_gdb_target_t *ppc_gdb_target(void)
{
    return &k_ppc_gdb_target;
}
