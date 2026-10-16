/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_siu.c - SIU_RSTVEC1, the register that starts core 1.
 *
 * "While core 0 begins execution immediately after device reset, core 1
 * remains held in reset ... When core 1 comes out of reset, it begins
 * executing code at the address specified in the RSTVEC field of
 * register SIU_RSTVEC1" -- NXP AN5191, 3.9, which is the whole of what
 * is known here; see ppc_siu.h for the rest and what is not modelled.
 *
 * Clearing RST releases the core and setting it puts the core back in
 * reset. The second is what the part does with SIU_HLT1 at its reset
 * value, which is set, and the register's address, the encoding and the
 * core's reset state are taken at the moment RST goes from 1 to 0.
 */

#include "ppc/ppc_siu.h"
#include "ppc/ppc_cpu.h"

void ppc_siu_reset(ppc_siu_t *s)
{
    s->rstvec1 = PPC_SIU_RSTVEC1_RESET;
}

static emu_fault_t siu_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    const ppc_siu_t *const s = (const ppc_siu_t *)ctx;

    /* Whole and aligned, the one access this model is sure of. */
    if (off != 0u || size != 4u) {
        return EMU_FAULT_LOAD;
    }
    *out = s->rstvec1;
    return EMU_FAULT_NONE;
}

static emu_fault_t siu_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    ppc_siu_t *const s = (ppc_siu_t *)ctx;
    const uint32_t was = s->rstvec1;

    if (off != 0u || size != 4u) {
        return EMU_FAULT_STORE;
    }
    s->rstvec1 = val;
    if (s->core1 == NULL) {
        return EMU_FAULT_NONE;
    }
    if ((was & PPC_SIU_RSTVEC_RST) != 0u && (val & PPC_SIU_RSTVEC_RST) == 0u) {
        ppc_cpu_start(s->core1, val & PPC_SIU_RSTVEC_ADDR,
                      (val & PPC_SIU_RSTVEC_VLE) != 0u);
    } else if ((was & PPC_SIU_RSTVEC_RST) == 0u &&
               (val & PPC_SIU_RSTVEC_RST) != 0u) {
        ppc_cpu_hold(s->core1);
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t ppc_siu_ops = {
    .read = siu_read,
    .write = siu_write,
    .tick = NULL,
};
