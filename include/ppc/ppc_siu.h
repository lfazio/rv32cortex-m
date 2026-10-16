/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_siu.h - the one SIU register a second e200z7 needs: SIU_RSTVEC1,
 * which holds core 1 in reset and says where it starts.
 *
 * MPC5777C. Its fields are NXP AN5191's ("MPC5777C Software
 * Initialization and Optimization", 3.9): RSTVEC in bits 0:29, RST in
 * bit 30 and VLE in bit 31 -- in C, the address masked by 0xFFFFFFFC, RST
 * 0x2 and VLE 0x1. RSTVEC resets to 0xFFFF_FFFC, the boot assist module,
 * and RST to 1, so core 1 is held until software clears it. The address,
 * 0xC3F9_09B0, is the SIU's base (0xC3F9_0000) plus the offset NXP's own
 * example code writes to; this tree has no copy of the reference manual,
 * so the register is modelled as far as those two sources say and no
 * further.
 *
 * Not modelled: SIU_HLT1, the other way to hold core 1 -- clearing
 * either releases it, and AN5191's example clears RST -- and VLE's reset
 * value, which neither source gives and this model takes as 0.
 */
#ifndef PPC_SIU_H
#define PPC_SIU_H

#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_SIU_RSTVEC1_ADDR 0xC3F909B0u
#define PPC_SIU_RSTVEC_VLE 0x00000001u
#define PPC_SIU_RSTVEC_RST 0x00000002u
#define PPC_SIU_RSTVEC_ADDR 0xFFFFFFFCu
#define PPC_SIU_RSTVEC1_RESET (0xFFFFFFFCu | PPC_SIU_RSTVEC_RST)

struct ppc_cpu;

typedef struct ppc_siu {
    uint32_t rstvec1;
    struct ppc_cpu *core1; /* the core it holds, or NULL */
} ppc_siu_t;

void ppc_siu_reset(ppc_siu_t *s);

/* Four bytes at PPC_SIU_RSTVEC1_ADDR, read and written whole. */
extern const emu_dev_ops_t ppc_siu_ops;

#ifdef __cplusplus
}
#endif

#endif /* PPC_SIU_H */
