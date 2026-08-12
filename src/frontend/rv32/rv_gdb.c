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

static const emu_gdb_target_t k_rv32_gdb_target = {
    .nregs       = RV_GDB_NREGS,
    .reg_bytes   = 4u,
    .reg_get     = rv_gdb_reg_get,
    .reg_set     = rv_gdb_reg_set,
    .pc_get      = rv_gdb_pc_get,
    .pc_set      = rv_gdb_pc_set,
    .stop_signal = 5,               /* SIGTRAP: an attach looks like a trap */
    .arch        = "riscv:rv32",
};

const emu_gdb_target_t *rv32_gdb_target(void);
const emu_gdb_target_t *rv32_gdb_target(void)
{
    return &k_rv32_gdb_target;
}
