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
 * Then SPEFSCR, the FP unit's status and control, in a feature of its
 * own -- **not** gdb's "org.gnu.gdb.power.spe", which this core cannot
 * honestly serve: that feature is the SPE's ev0h..ev31h upper halves and
 * its 64-bit `acc`, and this model has neither (EFPU2 is scalar; the
 * vector half is not modelled). Describing them would mean registers
 * that read zero and drop every write, which a debugger shows as values.
 * gdb prints a register in a feature it does not know under its name,
 * so `p $spefscr` works. EFPU2's operands are in r0..r31 already.
 *
 * Not described: the SPRs -- SRR0/1, CSRR0/1, ESR, DEAR, the IVORs, the
 * MMU's. They are in the state dump.
 */

#include "emu/emu_gdb.h"
#include "ppc/ppc_cpu.h"

#define PPC_GDB_PC 32u
#define PPC_GDB_MSR 33u
#define PPC_GDB_CR 34u
#define PPC_GDB_LR 35u
#define PPC_GDB_CTR 36u
#define PPC_GDB_XER 37u
#define PPC_GDB_SPEFSCR 38u
#define PPC_GDB_NREGS 39u

static ppc_cpu_t *cpu_of_gdb(const emu_cpu_t *cpu)
{
    /* Same cast ppc_frontend.c makes: the core *is* the cpu. */
    return (ppc_cpu_t *)(void *)(uintptr_t)cpu;
}

static uint64_t ppc_gdb_reg_get(const emu_cpu_t *cpu, unsigned n)
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
    case PPC_GDB_SPEFSCR:
        return c->spefscr;
    default:
        return 0u;
    }
}

static void ppc_gdb_reg_set(emu_cpu_t *cpu, unsigned n, uint64_t v64)
{
    const uint32_t v = (uint32_t)v64; /* every register here is 32 bits */
    ppc_cpu_t *c = cpu_of_gdb(cpu);

    if (n < 32u) {
        c->r[n] = v;
        return;
    }
    switch (n) {
    case PPC_GDB_PC:
        c->pc = v & ~1u; /* see ppc_gdb_pc_set */
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
        ppc_mmu_refresh(c); /* IS and DS choose the address space */
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
    case PPC_GDB_SPEFSCR:
        c->spefscr = v & PPC_SPEFSCR_IMPL; /* as mtspr leaves it */
        break;
    default:
        break;
    }
}

static uint32_t ppc_gdb_pc_get(const emu_cpu_t *cpu)
{
    return cpu_of_gdb(cpu)->pc;
}

/* Bit 0 is 0 in Book E and in VLE alike; only a debugger's write could
 * make it 1 (fuzz-gdb). */
static void ppc_gdb_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    cpu_of_gdb(cpu)->pc = pc & ~1u;
}

/*
 * gdb's core feature, in reg_get's order, as `powerpc:e500` -- the
 * nearest core gdb has: Book E, the SPE and embedded floating point, the
 * MAS TLB. Attached, gdb takes this over the file's architecture, so it
 * decides every name. Under `powerpc:common` the start-up's IVOR writes
 * read as 603 BAT writes (`mtibatu 0` for IVOR32), and at an address no
 * file covers `tlbwe` was the 603e's `tlbld` and `efsmul` AltiVec's
 * `vmuleud`. e500 lacks `rfdi` and `wait`, which print as `.long`.
 * Not `powerpc:vle`: the frontend runs Book E images as well as VLE ones.
 */
static const char k_ppc_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>powerpc:e500</architecture>"
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
    "<feature name=\"org.rv32cortexm.power.efpu2\">"
    "<reg name=\"spefscr\" bitsize=\"32\" type=\"uint32\" group=\"float\"/>"
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
    .arch = "powerpc:e500",
    .target_xml = k_ppc_xml,
};

const emu_gdb_target_t *ppc_gdb_target(void);
const emu_gdb_target_t *ppc_gdb_target(void)
{
    return &k_ppc_gdb_target;
}
