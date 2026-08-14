/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_gdb.c - what gdb needs to know about RH850 G4MH, and nothing else.
 *
 * The counterpart of rv_gdb.c: the protocol lives in src/emu/emu_gdb.c
 * and has no ISA in it, and this file is the whole of the G4MH half.
 *
 * The `g` packet layout is not a choice and it is not the frontend's
 * numbering. gdb's own rh850 target says what it is, and the way to find
 * out is to ask gdb rather than to infer it:
 *
 *     gdb-multiarch -batch -ex 'set architecture v850:rh850' \
 *                          -ex 'maint print registers'
 *
 *   0..31   r0..r31
 *   32..63  the selID 0 system bank: eipc, eipsw, fepc, fepsw, ecr, psw,
 *           then sr6..sr31 by number
 *   64      pc
 *   65      fp
 *
 * 66 registers of four bytes, so the packet is 264 bytes. Note the last
 * one: gdb carries `fp` as a *raw* register rather than a pseudo-register
 * derived from r29, so it occupies a slot in the packet and leaving it
 * out makes every reply four bytes short -- which gdb reports as a
 * malformed packet only sometimes, and otherwise as a pc of zero.
 *
 * The system bank maps straight across, which is luck rather than design
 * and is worth stating: gdb numbers 32+n is `sr[0][n]`, so gdb's "ecr" at
 * 36 is selID 0 index 4 and its "psw" at 37 is index 5, exactly where
 * this frontend keeps them.
 */

#include "emu/emu_gdb.h"
#include "g4mh/g4mh_cpu.h"

#define G4MH_GDB_SR_BASE  32u   /* first system register            */
#define G4MH_GDB_PC       64u
#define G4MH_GDB_FP       65u
#define G4MH_GDB_NREGS    66u

/*
 * The RH850 EABI's frame pointer. gdb wants it as its own register and
 * the architecture does not have one -- r29 is what the ABI uses and what
 * CC-RH emits, so that is what is reported. Reading it as a copy of r29
 * is right; writing it has to write r29, or `set $fp` would be accepted
 * and then silently lost.
 */
#define G4MH_ABI_FP_REG   29u

static g4mh_cpu_t *cpu_of_gdb(const emu_cpu_t *cpu)
{
    /* Same cast g4mh_frontend.c makes: the core *is* the cpu. */
    return (g4mh_cpu_t *)(void *)(uintptr_t)cpu;
}

static uint32_t g4mh_gdb_reg_get(const emu_cpu_t *cpu, unsigned n)
{
    const g4mh_cpu_t *c = cpu_of_gdb(cpu);

    if (n < 32u) {
        return c->r[n];
    }
    if (n < G4MH_GDB_PC) {
        return c->sr[0][n - G4MH_GDB_SR_BASE];
    }
    if (n == G4MH_GDB_PC) {
        return c->pc;
    }
    if (n == G4MH_GDB_FP) {
        return c->r[G4MH_ABI_FP_REG];
    }
    return 0u;
}

static void g4mh_gdb_reg_set(emu_cpu_t *cpu, unsigned n, uint32_t v)
{
    g4mh_cpu_t *c = cpu_of_gdb(cpu);

    if (n < 32u) {
        c->r[n] = v;
        c->r[0] = 0u;               /* r0 stays zero whatever gdb says */
    } else if (n < G4MH_GDB_PC) {
        c->sr[0][n - G4MH_GDB_SR_BASE] = v;
        /*
         * PSW is shadowed into its own field for the hot path, so writing
         * the bank alone would be visible to STSR and to nothing else.
         */
        if ((n - G4MH_GDB_SR_BASE) == G4MH_SR_PSW) {
            c->psw = v;
        }
    } else if (n == G4MH_GDB_PC) {
        c->pc = v;
    } else if (n == G4MH_GDB_FP) {
        c->r[G4MH_ABI_FP_REG] = v;
    }
}

static uint32_t g4mh_gdb_pc_get(const emu_cpu_t *cpu)
{
    return cpu_of_gdb(cpu)->pc;
}

static void g4mh_gdb_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    cpu_of_gdb(cpu)->pc = pc;
}

/*
 * The target description -- and only half of it is honoured, which is
 * worth knowing before trusting it.
 *
 * gdb reads the `<architecture>` element and selects its v850:rh850
 * tdep from it, so a session works without `set architecture`. It then
 * *rejects the register list*:
 *
 *     warning: Target-supplied registers are not supported by
 *              the current architecture
 *
 * because gdb's v850 backend does not implement target descriptions for
 * registers. So the `<reg>` elements below document the layout and
 * cannot enforce it: **gdb's built-in numbering is the contract**, and
 * reg_get/reg_set above must match *that*, not this.
 *
 * Kept anyway for the architecture element and because it is the list a
 * reader needs, with test_g4mh_gdb_layout() as the thing that actually
 * holds the two together -- editing one without the other is otherwise
 * silent.
 */
static const char k_g4mh_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>v850:rh850</architecture>"
    "<feature name=\"org.gnu.gdb.v850.core\">"
    "<reg name=\"r0\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r1\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r2\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"gp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"tp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"r6\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r7\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r8\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r9\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r10\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r11\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r12\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r13\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r14\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r15\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r16\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r17\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r18\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r19\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r20\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r21\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r22\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r23\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r24\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r25\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r26\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r27\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r28\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"r29\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"ep\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lp\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"eipc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"eipsw\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"fepc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"fepsw\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"ecr\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"psw\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr6\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr7\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr8\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr9\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr10\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr11\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr12\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr13\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr14\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr15\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr16\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr17\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr18\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr19\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr20\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr21\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr22\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr23\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr24\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr25\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr26\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr27\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr28\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr29\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr30\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"sr31\" bitsize=\"32\" type=\"int\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "</feature>"
    "</target>";

/*
 * The guest's memory map, as this frontend backs it.
 *
 * Code flash is named flash, which is what makes gdb use
 * vFlashErase/vFlashWrite for `load` rather than writing it byte by byte
 * with X -- and on the firmware that is not a preference but a
 * requirement, because flash really is served read-only out of the
 * part's own and an X write to it is refused by the bus. See
 * docs/memory.md.
 *
 * Local RAM is the SELF window, which is what a debugger attached to one
 * core should see; the absolute per-PE windows are deliberately left out
 * rather than described three times.
 */
static const char k_g4mh_memmap[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE memory-map SYSTEM \"memory-map.dtd\">"
    "<memory-map>"
    "<memory type=\"flash\" start=\"0x00000000\" length=\"0x00300000\">"
    "<property name=\"blocksize\">0x1000</property>"
    "</memory>"
    "<memory type=\"ram\" start=\"0xFDE00000\" length=\"0x00010000\"/>"
    "<memory type=\"ram\" start=\"0xFE000000\" length=\"0x00060000\"/>"
    "</memory-map>";

static const emu_gdb_target_t k_g4mh_gdb_target = {
    .nregs       = G4MH_GDB_NREGS,
    .reg_bytes   = 4u,
    .reg_get     = g4mh_gdb_reg_get,
    .reg_set     = g4mh_gdb_reg_set,
    .pc_get      = g4mh_gdb_pc_get,
    .pc_set      = g4mh_gdb_pc_set,
    .stop_signal = 5,               /* SIGTRAP: an attach looks like a trap */
    .arch        = "v850:rh850",
    .target_xml  = k_g4mh_xml,
    .memory_map  = k_g4mh_memmap,
};

const emu_gdb_target_t *g4mh_gdb_target(void);
const emu_gdb_target_t *g4mh_gdb_target(void)
{
    return &k_g4mh_gdb_target;
}
