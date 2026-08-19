/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_boot.c - BOOTCTRL, the register that starts the other PEs.
 *
 * RH850/U2B hardware manual R01UH0923EJ0130 section 11.4.79, table
 * 11.84: one 32-bit register at FFFB_2000H whose low six bits are BC5 to
 * BC0, "this bit triggers the start-up of PEn", 0 inactive and 1 active.
 *
 * **Only PE0 runs at reset release.** "At the time of reset release,
 * either PE0 or ICUMHB start according to the Flash Option Byte. After
 * reset release, PE can be started selectively by asserting the
 * corresponding bit of this register."
 *
 * Which is what this emulator did *not* do: every PE came out of reset
 * in EMU_STATE_RUNNING and began executing the same image at the same
 * address, so a multicore guest appeared to work while doing something
 * the part never does. Guests written against it would have no start-up
 * sequence at all, and would then hang on real silicon waiting for
 * secondaries that nobody had released.
 *
 * BC0's reset value is `x` in the manual because the flash option byte
 * decides whether PE0 or the ICU boots. There is no option byte here, so
 * PE0 starts: the alternative is an emulator that runs nothing.
 */

#include "g4mh/g4mh_boot.h"
#include "g4mh/g4mh_cpu.h"

#include <string.h>

void g4mh_boot_init(g4mh_boot_t *b)
{
    memset(b, 0, sizeof(*b));
    /*
     * PE0 is already running, so its bit reads back set from the start.
     * A guest that reads BOOTCTRL to discover which PEs are up gets the
     * truth rather than a zero that contradicts the core it is executing
     * on.
     */
    b->bc = 1u;
}

void g4mh_boot_attach(g4mh_boot_t *b, unsigned pe, struct g4mh_cpu *c)
{
    if (pe < G4MH_PE_COUNT) {
        b->cpu[pe] = c;
    }
}

static emu_fault_t boot_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_boot_t *b = (const g4mh_boot_t *)ctx;
    (void)size;

    *out = (off == G4MH_BOOTCTRL_OFF) ? (b->bc & G4MH_BOOTCTRL_BC_MASK) : 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t boot_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_boot_t *b = (g4mh_boot_t *)ctx;
    (void)size;

    if (off != G4MH_BOOTCTRL_OFF) {
        return EMU_FAULT_NONE;
    }

    const uint32_t bc = val & G4MH_BOOTCTRL_BC_MASK;

    /*
     * Releasing is one-way here. The manual's stop sequence is "write 0
     * to BCn, then issue a module reset of PEn" -- two steps, of which
     * the second is the one that actually stops the core, and there is
     * no module reset controller in this model. So a 1->0 transition is
     * recorded in the register and does nothing to the core, which is
     * what the part does *until* the reset arrives. Pretending the write
     * alone stopped a PE would be the more dangerous simplification: a
     * guest would appear to stop a core that real silicon leaves
     * running.
     */
    for (unsigned pe = 0; pe < G4MH_PE_COUNT; pe++) {
        const bool now = ((bc >> pe) & 1u) != 0u;
        const bool was = ((b->bc >> pe) & 1u) != 0u;

        if (now && !was && b->cpu[pe] != NULL &&
            b->cpu[pe]->state == EMU_STATE_HELD) {
            b->cpu[pe]->state = EMU_STATE_RUNNING;
        }
    }
    b->bc = bc;
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_boot_ops = {
    .read = boot_read, .write = boot_write, .tick = NULL,
};
