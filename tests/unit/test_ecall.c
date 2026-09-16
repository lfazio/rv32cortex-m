/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ecall.c - who owns an ECALL: the emulator, or the guest's kernel.
 *
 * **The emulator offers semihosting, and semihosting must not outrank a
 * guest that brought its own operating system.** The hook answers
 * `write` and `exit` so a bare-metal guest can print and terminate with
 * no kernel underneath it, and those guests run in M-mode. Consulting
 * it for *every* ECALL steals the syscalls of a guest that has a kernel:
 * under Linux, a process calling write(2) traps from U-mode, and the
 * hook answered it instead of the kernel.
 *
 * What made that expensive to find is how well it imitated success. The
 * hook returns the length it was given, so write() reported every byte
 * written; it ignores the descriptor, so `write(-1, ...)` reported
 * success too; and it reads the buffer with a *physical* bus access,
 * which under Sv32 is not where a user pointer points, so it printed
 * nothing. Userspace was mute while every call succeeded, and the
 * kernel function that would have printed was never entered -- a
 * combination that reads as a broken console rather than a stolen
 * syscall.
 *
 * So the rule is the privilege level, and it is asserted in all three
 * directions below. An S-mode ECALL is an SBI call and belongs to the
 * firmware; a U-mode one belongs to whatever kernel is above it.
 */

#include "tests.h"

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"
#include "rv32/rv_backend.h"

#include "emu/emu_bus.h"

#define RAM_BASE 0x80000000u

static rv_hart_t g_hart;
static emu_bus_t g_bus;
static uint8_t g_ram[4096];

static int g_hook_calls;

static bool count_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)cpu;
    (void)user;
    g_hook_calls++;
    sc->ret = 0x1234u;
    return true; /* consumed */
}

/*
 * One ECALL at the reset pc, executed from `priv`.
 *
 * mtvec is set somewhere harmless so a taken trap has a destination and
 * the pc moving there is what says the trap happened -- checking only
 * that the hook did not run would pass against an emulator that dropped
 * the instruction entirely.
 */
static void run_ecall_at(uint32_t priv)
{
    emu_bus_init(&g_bus);
    (void)emu_bus_add_ram(&g_bus, "ram", RAM_BASE, g_ram, sizeof(g_ram));
    rv_hart_init(&g_hart, &g_bus, 0u);
    rv_hart_reset(&g_hart, RAM_BASE);

    /* ECALL, then something that is not, so a fall-through is visible. */
    g_ram[0] = 0x73u;
    g_ram[1] = 0x00u;
    g_ram[2] = 0x00u;
    g_ram[3] = 0x00u;

    g_hart.mtvec = RAM_BASE + 0x100u;

    /*
     * A PMP entry that permits everything, which below M-mode is not
     * optional: matching *no* entry denies rather than permits, so a
     * U-mode fetch with an empty table takes an instruction access
     * fault and never reaches the ECALL at all. The first version of
     * this test did exactly that and reported mcause 1 where it wanted
     * 8 -- which is this tree's own recorded rule, met head-on.
     *
     * NAPOT with pmpaddr all ones is the whole address space; the cfg
     * byte is R|W|X plus A=NAPOT.
     */
    g_hart.pmpaddr[0] = 0xFFFFFFFFu;
    g_hart.pmpcfg[0] = 0x1Fu;

    g_hart.priv = priv;
    rv_pmp_refresh(&g_hart);

#if RV_ENABLE_ECALL_HOOK
    g_hart.ecall = count_hook;
    g_hart.ecall_user = NULL;
#endif
    g_hook_calls = 0;

    (void)rv_step(&g_hart);
}

void test_ecall(void)
{
#if !RV_ENABLE_ECALL_HOOK
    return;
#else
    /*
     * M-mode: the hook owns it. This is every bare-metal guest in the
     * tree, and the one case that has to keep working -- `exit()` in a
     * guest with no kernel is this and nothing else.
     */
    run_ecall_at(RV_PRIV_M);
    CHECK_EQ((uint32_t)g_hook_calls, 1u);
    CHECK_EQ(g_hart.x[10], 0x1234u);
    /* Consumed, so execution resumed after the instruction. */
    CHECK_EQ(g_hart.pc, RAM_BASE + 4u);

#if RV_EXT_U
    /*
     * U-mode: the guest's kernel owns it. The hook must not be
     * consulted at all, and the trap must be taken.
     */
    run_ecall_at(RV_PRIV_U);
    CHECK_EQ((uint32_t)g_hook_calls, 0u);
    CHECK_EQ(g_hart.pc, RAM_BASE + 0x100u);
    CHECK_EQ(g_hart.mcause, RV_EXC_ECALL_U);
#endif

#if RV_EXT_S
    /*
     * S-mode: an SBI call, which belongs to the firmware in M-mode.
     * Linux makes these constantly -- console, timer, IPI -- so a hook
     * that answered them would break a kernel far more loudly than the
     * U-mode case did, and for the same reason.
     */
    run_ecall_at(RV_PRIV_S);
    CHECK_EQ((uint32_t)g_hook_calls, 0u);
    CHECK_EQ(g_hart.pc, RAM_BASE + 0x100u);
    CHECK_EQ(g_hart.mcause, RV_EXC_ECALL_S);
#endif
#endif /* RV_ENABLE_ECALL_HOOK */
}
