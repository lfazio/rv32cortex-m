/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_gdb.c - what gdb needs to know about RV32, and nothing else.
 *
 * The protocol lives in src/emu/emu_gdb.c and has no ISA in it. This
 * file is the whole of the RISC-V half, and it is small because the only
 * thing that genuinely differs between architectures is the shape of the
 * `g` packet: how many registers, in what order, and which one is the
 * pc.
 *
 * That order is not a choice. gdb's rv32 target has x0..x31 followed by
 * pc, each 32 bits, and it does not ask -- it assumes. Getting it wrong
 * produces an `info registers` full of plausible values that are all one
 * slot out, which is a great deal harder to spot than an error would be.
 * A frontend adding itself here should check its architecture's register
 * numbering in gdb's own target description rather than inferring it
 * from its own numbering, which is what rv32_reg_read uses and is only
 * the same by luck for x0..x31.
 */

#include "emu/emu_gdb.h"
#include "rv32/rv_hart.h"

/* The pc is register 32, immediately after the 32 GPRs. */
#define RV_GDB_PC       32u
#define RV_GDB_NREGS    33u

static rv_hart_t *hart_of_gdb(const emu_cpu_t *cpu)
{
    /*
     * The hart is the cpu: rv_hart_t begins with the emu_cpu_t header,
     * which is what lets the frontend hand out one pointer for both. The
     * cast is the same one rv32_frontend.c makes.
     */
    return (rv_hart_t *)(void *)(uintptr_t)cpu;
}

static uint32_t rv_gdb_reg_get(const emu_cpu_t *cpu, unsigned n)
{
    const rv_hart_t *h = hart_of_gdb(cpu);

    if (n < 32u) {
        return h->x[n];
    }
    if (n == RV_GDB_PC) {
        return h->pc;
    }
    return 0u;
}

static void rv_gdb_reg_set(emu_cpu_t *cpu, unsigned n, uint32_t v)
{
    rv_hart_t *h = hart_of_gdb(cpu);

    if (n < 32u) {
        h->x[n] = v;
        h->x[0] = 0u;               /* x0 stays hardwired whatever gdb says */
    } else if (n == RV_GDB_PC) {
        h->pc = v;
    }
}

static uint32_t rv_gdb_pc_get(const emu_cpu_t *cpu)
{
    return hart_of_gdb(cpu)->pc;
}

static void rv_gdb_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    hart_of_gdb(cpu)->pc = pc;
}

/*
 * The target description, as gdb's own DTD wants it.
 *
 * "org.gnu.gdb.riscv.cpu" is a standard feature name, and naming it is
 * what lets gdb apply its built-in knowledge -- the register names, the
 * calling convention, the disassembler -- rather than treating this as
 * an unknown target with 33 anonymous words. The register order here
 * must match reg_get/reg_set above; it is the same list twice, and the
 * only way they can disagree is by editing one of them.
 *
 * Sent whole in a single qXfer reply, which is why it is kept terse:
 * the numbers are implied by position, so no regnum attributes.
 */
static const char k_rv32_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>riscv:rv32</architecture>"
    "<feature name=\"org.gnu.gdb.riscv.cpu\">"
    "<reg name=\"zero\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"ra\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"gp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"tp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"t0\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t1\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t2\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"s1\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a0\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a1\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a2\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a3\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a4\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a5\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a6\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"a7\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s2\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s3\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s4\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s5\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s6\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s7\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s8\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s9\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s10\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"s11\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t3\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t4\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t5\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"t6\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "</feature>"
    "</target>";

/*
 * The guest's memory map. Naming the low window "flash" is what makes
 * gdb use vFlashErase/vFlashWrite for `load` rather than writing it byte
 * by byte with X -- and on this board that is not a preference: the
 * read-only half really is served out of the part's flash, so an X write
 * to it is refused by the bus.
 *
 * The sizes are the guest's address space, not the board's arena: gdb
 * addresses the program, and where it lands is the platform's business.
 */
static const char k_rv32_memmap[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE memory-map SYSTEM \"memory-map.dtd\">"
    "<memory-map>"
    "<memory type=\"flash\" start=\"0x80000000\" length=\"0x00080000\">"
    "<property name=\"blocksize\">0x1000</property>"
    "</memory>"
    "<memory type=\"ram\" start=\"0x80080000\" length=\"0x00080000\"/>"
    "</memory-map>";

static const emu_gdb_target_t k_rv32_gdb_target = {
    .nregs       = RV_GDB_NREGS,
    .reg_bytes   = 4u,
    .reg_get     = rv_gdb_reg_get,
    .reg_set     = rv_gdb_reg_set,
    .pc_get      = rv_gdb_pc_get,
    .pc_set      = rv_gdb_pc_set,
    .stop_signal = 5,               /* SIGTRAP: an attach looks like a trap */
    .arch        = "riscv:rv32",
    .target_xml  = k_rv32_xml,
    .memory_map  = k_rv32_memmap,
};

const emu_gdb_target_t *rv32_gdb_target(void);
const emu_gdb_target_t *rv32_gdb_target(void)
{
    return &k_rv32_gdb_target;
}
