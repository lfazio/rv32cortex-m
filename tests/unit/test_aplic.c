/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_aplic.c - which privilege the APLIC delivers to.
 *
 * **This is the failure that has no symptom.** An APLIC pointed at the
 * wrong privilege level reads back perfectly: domaincfg, the enable
 * bits, the pending bitmap and topi all say an interrupt is pending and
 * deliverable. What does not happen is the guest taking it -- the line
 * is raised at a privilege it is not running at -- so a driver waits
 * for ever on a queue that already completed, and every register a
 * person would think to inspect agrees that it should have worked.
 *
 * Linux under OpenSBI runs in S-mode and never sees MEIP; every
 * bare-metal guest in this tree runs in M-mode and only sees MEIP. So
 * the same device has to do both, and the test is that it does exactly
 * one of them at a time.
 */

#include "tests.h"

#include "rv32/rv_aplic.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#define SRC 3u

static rv_aplic_t g_aplic;
static rv_hart_t g_hart;

/*
 * Bring the domain up far enough to deliver: interrupts enabled, the
 * source made active and enabled, a priority that is not "never", and
 * the delivery controller switched on. Any one of these left out gives
 * a quiet non-delivery of its own, which is the thing the test is
 * distinguishing from -- so they are all set, once, and the only
 * variable is the privilege.
 */
static void arm_domain(void)
{
    uint32_t v;

    (void)rv_aplic_ops.write(&g_aplic, 0x0000u, 4u, RV_APLIC_DOMAINCFG_IE);
    /* sourcecfg[SRC] = edge-rise, which is a delivering mode. */
    (void)rv_aplic_ops.write(&g_aplic, 0x0004u + (SRC - 1u) * 4u, 4u,
                             RV_APLIC_SM_EDGE_RISE);
    /* target[SRC]: priority 1, meaning "deliver" rather than "never". */
    (void)rv_aplic_ops.write(&g_aplic, 0x3000u + (SRC - 1u) * 4u, 4u, 1u);
    /* setienum: enable the source. */
    (void)rv_aplic_ops.write(&g_aplic, 0x1EDCu, 4u, SRC);
    (void)rv_aplic_ops.write(&g_aplic, RV_APLIC_IDC + RV_APLIC_IDC_IDELIVERY,
                             4u, 1u);
    (void)rv_aplic_ops.read(&g_aplic, 0x0000u, 4u, &v);
}

void test_aplic(void)
{
    rv_hart_init(&g_hart, NULL, 0u);

    /*
     * M-mode, the default, and asserted rather than assumed: a change
     * that flipped it would break every bare-metal guest in this tree
     * silently, which is exactly the failure this file is about.
     */
    rv_aplic_init(&g_aplic, &g_hart);
    arm_domain();
    rv_aplic_raise(&g_aplic, SRC);

    CHECK((g_hart.mip & MIP_MEIP) != 0u);
    CHECK_EQ(g_hart.mip & MIP_SEIP, 0u);

    /*
     * S-mode. The same source, the same raise, the same registers --
     * only the machine's description differs, and the interrupt has to
     * move with it.
     */
    rv_aplic_init(&g_aplic, &g_hart);
    rv_aplic_set_smode(&g_aplic, true);
    g_hart.mip = 0u;
    arm_domain();
    rv_aplic_raise(&g_aplic, SRC);

    CHECK((g_hart.mip & MIP_SEIP) != 0u);
    /*
     * And *not* both. A device that raised the machine line as well
     * would work on Linux and would also interrupt firmware that never
     * enabled it -- which is the kind of extra that passes every test
     * until something else is listening.
     */
    CHECK_EQ(g_hart.mip & MIP_MEIP, 0u);

    /* Back again, so the setter is not one-way. */
    rv_aplic_set_smode(&g_aplic, false);
    g_hart.mip = 0u;
    rv_aplic_raise(&g_aplic, SRC);

    CHECK((g_hart.mip & MIP_MEIP) != 0u);
    CHECK_EQ(g_hart.mip & MIP_SEIP, 0u);
}
