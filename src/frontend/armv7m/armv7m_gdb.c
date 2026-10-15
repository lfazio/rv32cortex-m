/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_gdb.c - what gdb needs to know about the ARMv7-M guest, and
 * nothing else. The counterpart of rv_gdb.c and g4mh_gdb.c: the protocol
 * is src/emu/emu_gdb.c and has no ISA in it.
 *
 * The register file is gdb's own M-profile feature,
 * "org.gnu.gdb.arm.m-profile": r0..r12, sp, lr, pc, then xpsr. Naming
 * that feature is what makes gdb treat this as a Cortex-M -- the
 * exception-return unwinder, Thumb-only disassembly, xpsr rather than
 * cpsr -- instead of as a classic ARM with 26 anonymous words.
 *
 * Seventeen registers, numbered by position, and **the pc is 15, not
 * the last**: xpsr follows it. The stop reply carries the pc under
 * pc_regno, and scripts/gdb-check.sh holds that against gdb's own
 * remote number for `pc`.
 *
 * Then gdb's VFP feature, "org.gnu.gdb.arm.vfp": d0..d15 at 64 bits
 * and fpscr. Naming it is what makes gdb build s0..s31 as views of the
 * d registers and print floats as floats. d<n> is s<2n+1>:s<2n>, the
 * single in the low half -- the pairing the architecture defines and the
 * one VMOV between a D and two S registers uses. These are the first
 * registers wider than the stub's 32 bits, which is what reg_width is
 * for: a `g` packet is each register at its own width, back to back.
 *
 * What is not described, and so not visible to gdb: MSP and PSP,
 * PRIMASK, BASEPRI, FAULTMASK and CONTROL. `sp` is the active stack
 * pointer, which is what a backtrace needs; the banked copy and the
 * masks are in the state dump.
 */

#include "emu/emu_gdb.h"
#include "armv7m/armv7m_cpu.h"

#define A7_GDB_SP 13u
#define A7_GDB_PC 15u
#define A7_GDB_XPSR 16u
#define A7_GDB_D0 17u /* d0..d15, 64 bits each */
#define A7_GDB_FPSCR 33u
#define A7_GDB_NREGS 34u

static armv7m_cpu_t *cpu_of_gdb(const emu_cpu_t *cpu)
{
    /* Same cast armv7m_frontend.c makes: the core *is* the cpu. */
    return (armv7m_cpu_t *)(void *)(uintptr_t)cpu;
}

/*
 * xPSR as the architecture defines it.
 *
 * Translated code keeps N, Z, C and V in four words of their own while
 * it runs, and armv7m_ops_run packs them back before returning -- so
 * whenever the stub can ask, which is only while the guest is halted
 * between runs, the packed word is the current one. Packed again here
 * all the same, because reading the flags wrong is silent and the cost
 * is nothing on a path a human is waiting on.
 */
static uint32_t a7_gdb_xpsr(const emu_cpu_t *cpu)
{
    armv7m_cpu_t *const c = cpu_of_gdb(cpu);

    armv7m_flags_pack(c);
    return c->xpsr;
}

static uint64_t a7_gdb_reg_get(const emu_cpu_t *cpu, unsigned n)
{
    const armv7m_cpu_t *c = cpu_of_gdb(cpu);

    if (n < A7_GDB_XPSR) {
        return c->r[n];
    }
    if (n == A7_GDB_XPSR) {
        return a7_gdb_xpsr(cpu);
    }
    if (n >= A7_GDB_D0 && n < A7_GDB_FPSCR) {
        const unsigned d = n - A7_GDB_D0;

        return (uint64_t)c->s[2u * d] | ((uint64_t)c->s[2u * d + 1u] << 32);
    }
    if (n == A7_GDB_FPSCR) {
        return c->fpscr;
    }
    return 0u;
}

static void a7_gdb_reg_set(emu_cpu_t *cpu, unsigned n, uint64_t v64)
{
    const uint32_t v = (uint32_t)v64; /* all but d0..d15 */
    armv7m_cpu_t *c = cpu_of_gdb(cpu);

    if (n == A7_GDB_PC) {
        /* A Thumb pc is halfword aligned; bit 0 is the T bit's business,
         * and that lives in xpsr. */
        c->r[n] = v & ~1u;
    } else if (n == A7_GDB_SP) {
        c->r[n] = v & ~3u; /* SP[1:0] are zero on this profile */
    } else if (n < A7_GDB_XPSR) {
        c->r[n] = v;
    } else if (n == A7_GDB_XPSR) {
        /*
         * The whole word, IPSR and the IT bits included -- it is a
         * debugger, and a debugger that may not clear T cannot show
         * what clearing it does. The translator's context is derived
         * from xpsr when the next run binds, so a changed T or IT state
         * selects different blocks rather than reusing stale ones.
         */
        armv7m_flags_pack(c);
        c->xpsr = v;
    } else if (n >= A7_GDB_D0 && n < A7_GDB_FPSCR) {
        const unsigned d = n - A7_GDB_D0;

        c->s[2u * d] = (uint32_t)v64;
        c->s[2u * d + 1u] = (uint32_t)(v64 >> 32);
    } else if (n == A7_GDB_FPSCR) {
        c->fpscr = v & ARMV7M_FPSCR_WRITABLE; /* what VMSR may change */
    }
}

static uint32_t a7_gdb_pc_get(const emu_cpu_t *cpu)
{
    return cpu_of_gdb(cpu)->r[A7_GDB_PC];
}

static void a7_gdb_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    cpu_of_gdb(cpu)->r[A7_GDB_PC] = pc & ~1u;
}

/*
 * The target description: gdb's own M-profile feature, register for
 * register, in the order reg_get uses. Positions are the numbers, so no
 * regnum attributes; gdb's copy of this feature gives xpsr regnum 25,
 * which is its *internal* number and would leave eight empty slots in
 * the `g` packet if copied here.
 */
static const char k_armv7m_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>armv7e-m</architecture>"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\"/>"
    "<reg name=\"r1\" bitsize=\"32\"/>"
    "<reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/>"
    "<reg name=\"r4\" bitsize=\"32\"/>"
    "<reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/>"
    "<reg name=\"r7\" bitsize=\"32\"/>"
    "<reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/>"
    "<reg name=\"r10\" bitsize=\"32\"/>"
    "<reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\"/>"
    "</feature>"
    "<feature name=\"org.gnu.gdb.arm.vfp\">"
    "<reg name=\"d0\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d1\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d2\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d3\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d4\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d5\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d6\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d7\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d8\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d9\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d10\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d11\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d12\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d13\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d14\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d15\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"fpscr\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "</feature>"
    "</target>";

/* r0..r12, sp, lr, pc, xpsr at four bytes; d0..d15 at eight; fpscr. */
static const uint8_t k_armv7m_width[A7_GDB_NREGS] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4,
};

static const emu_gdb_target_t k_armv7m_gdb_target = {
    .nregs = A7_GDB_NREGS,
    .reg_bytes = 4u,
    .reg_width = k_armv7m_width,
    .pc_regno = A7_GDB_PC, /* not the last: xpsr follows it */
    .reg_get = a7_gdb_reg_get,
    .reg_set = a7_gdb_reg_set,
    .pc_get = a7_gdb_pc_get,
    .pc_set = a7_gdb_pc_set,
    .stop_signal = 5, /* SIGTRAP: an attach looks like a trap */
    .arch = "armv7e-m",
    .target_xml = k_armv7m_xml,
};

const emu_gdb_target_t *armv7m_gdb_target(void);
const emu_gdb_target_t *armv7m_gdb_target(void)
{
    return &k_armv7m_gdb_target;
}
