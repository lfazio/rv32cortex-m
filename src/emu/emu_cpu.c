/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_cpu.c - The frontend registry.
 *
 * The one file that names every frontend. Adding an ISA means adding a
 * declaration and a table entry here, and nothing else in the ISA-agnostic
 * runtime changes.
 */

#include "emu/emu_cpu.h"

#include <string.h>

#if EMU_FRONTEND_RV32
extern const emu_cpu_ops_t rv32_frontend;
#endif
#if EMU_FRONTEND_G4MH
extern const emu_cpu_ops_t g4mh_frontend;
#endif
#if EMU_FRONTEND_PPC
extern const emu_cpu_ops_t ppc_frontend;
#endif

/*
 * Order is the default order: emu_frontend_default returns the first, so a
 * build with several frontends compiled in picks whichever the build system
 * listed first.
 */
const emu_cpu_ops_t *const emu_frontends[] = {
#if EMU_FRONTEND_RV32
    &rv32_frontend,
#endif
#if EMU_FRONTEND_G4MH
    &g4mh_frontend,
#endif
#if EMU_FRONTEND_PPC
    &ppc_frontend,
#endif
    NULL,
};

const emu_cpu_ops_t *emu_frontend_find(const char *name)
{
    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        if (strcmp((*p)->name, name) == 0) {
            return *p;
        }
    }
    return NULL;
}

const emu_cpu_ops_t *emu_frontend_default(void)
{
    return emu_frontends[0];
}

const emu_cpu_ops_t *emu_frontend_for_elf(uint16_t machine)
{
    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        /* elf_machine 0 means "takes anything", for a frontend with no
         * ELF machine number of its own. elf_machine_alt is a second
         * number the same frontend answers to -- see EMU_EM_V800. */
        if ((*p)->elf_machine == 0u || (*p)->elf_machine == machine ||
            ((*p)->elf_machine_alt != 0u &&
             (*p)->elf_machine_alt == machine)) {
            return *p;
        }
    }
    return NULL;
}

bool emu_core_open(emu_core_t *core, const emu_cpu_ops_t *ops,
                   emu_bus_t *bus, unsigned index)
{
    emu_cpu_t *cpu = ops->instance(index);
    if (cpu == NULL) {
        return false;
    }
    core->ops = ops;
    core->cpu = cpu;
    core->bus = bus;
    ops->init(cpu, bus, index);
    return true;
}

/* ------------------------------------------------------------------ */
/* Systems                                                             */
/* ------------------------------------------------------------------ */

bool emu_system_open(emu_system_t *sys, const emu_cpu_ops_t *ops,
                     emu_bus_t *buses, unsigned ncores)
{
    if (ncores == 0u) {
        ncores = (ops->ncores != 0u) ? ops->ncores : 1u;
    }
    if (ncores > EMU_MAX_CORES) {
        return false;
    }

    sys->ops = ops;
    sys->ncores = ncores;

    for (unsigned i = 0; i < ncores; i++) {
        emu_bus_t *bus = &buses[i];

        /*
         * Shared devices go into every bus. The mapping is per bus; the
         * state behind it is one object, so all cores see the same
         * interrupt controller through their own region tables.
         */
        if (ops->add_shared_devices != NULL && !ops->add_shared_devices(bus)) {
            return false;
        }
        if (!emu_core_open(&sys->core[i], ops, bus, i)) {
            return false;
        }
        if (ops->add_core_devices != NULL &&
            !ops->add_core_devices(sys->core[i].cpu, bus, i)) {
            return false;
        }
    }
    return true;
}

void emu_system_reset(emu_system_t *sys, uint32_t reset_pc)
{
    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_core_reset(&sys->core[i], reset_pc);
    }
}

void emu_system_boot(emu_system_t *sys, uint32_t ram_base, uint32_t ram_size)
{
    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_core_boot(&sys->core[i], ram_base, ram_size);
    }
}

void emu_system_invalidate(emu_system_t *sys, uint32_t addr, uint32_t len)
{
    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_core_invalidate(&sys->core[i], addr, len);
    }
}

uint32_t emu_system_step(emu_system_t *sys, uint32_t quantum, bool *all_idle)
{
    uint32_t total = 0u;
    bool idle = true;

    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_core_t *c = &sys->core[i];
        emu_cpu_status_t st;

        emu_core_status(c, &st);
        if (st.state == EMU_STATE_HALTED) {
            continue;                 /* done for good */
        }
        /*
         * Held at reset. Skipped like a halted core but *not* counted as
         * live: a core waiting to be released cannot release itself, so
         * if every other core has finished, the system is idle and the
         * run must end rather than spinning over cores that will never
         * start.
         */
        if (st.state == EMU_STATE_HELD) {
            continue;
        }
        if (st.state == EMU_STATE_WFI && !st.wakeable) {
            continue;                 /* parked, and nothing can wake it */
        }

        /*
         * Anything not halted and not permanently parked counts as live,
         * including a core that yields immediately: it is waiting on
         * another core, and that other core is in this same round.
         */
        idle = false;

        uint32_t did = 0u;
        (void)emu_core_run(c, quantum, &did);
        total += did;
    }

    if (all_idle != NULL) {
        *all_idle = idle;
    }
    return total;
}
