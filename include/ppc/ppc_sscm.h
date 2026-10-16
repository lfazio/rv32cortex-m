/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_sscm.h - the MPC5675K's System Status and Configuration Module, as
 * far as a second e200z7 needs it.
 *
 * MPC5675K Reference Manual, Rev. 10 (docs/nxp), chapter 49 and section
 * 5.6: in decoupled parallel mode (DPM, the default) core 0 runs out of
 * reset and core 1 is held until core 0 writes its start address to
 * SSCM_DPMBOOT and then 0x5AF0 and 0xA50F to SSCM_DPMKEY. After the
 * second key, "Core_1 jumps to its reset vector".
 *
 * Modelled: DPMBOOT and DPMKEY; STATUS reading DPM (LSM 0); MEMCONFIG
 * at its reset value; ERROR and DEBUGPORT holding what is written; the
 * password words taking writes; UOPS reading the user options as all
 * zero, which is DPM. What those registers *do* -- bus abort enables,
 * the debug port, unlocking a secured part -- is not, and lockstep mode
 * is not either: a part configured for LSM runs both cores as one.
 */
#ifndef PPC_SSCM_H
#define PPC_SSCM_H

#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_SSCM_BASE 0xC3FD8000u
#define PPC_SSCM_SIZE 0x24u /* through SSCM_UOPS; the rest is reserved */

/* Table 49-1. */
#define PPC_SSCM_STATUS 0x00u
#define PPC_SSCM_MEMCONFIG 0x02u
#define PPC_SSCM_ERROR 0x06u
#define PPC_SSCM_DEBUGPORT 0x08u
#define PPC_SSCM_PWCMPH 0x0Cu
#define PPC_SSCM_PWCMPL 0x10u
#define PPC_SSCM_DPMBOOT 0x18u
#define PPC_SSCM_DPMKEY 0x1Cu
#define PPC_SSCM_UOPS 0x20u

/* Figure 49-8: P2BOOT in bits 0:29, DVLE in bit 30, bit 31 reserved. */
#define PPC_SSCM_DPMBOOT_P2BOOT 0xFFFFFFFCu
#define PPC_SSCM_DPMBOOT_DVLE 0x00000002u

/* Table 49-14: the two writes of DPMKEY[KEY] that release core 1. */
#define PPC_SSCM_KEY1 0x5AF0u
#define PPC_SSCM_KEY2 0xA50Fu

#define PPC_SSCM_MEMCONFIG_RESET 0xB021u

struct ppc_cpu;

typedef struct ppc_sscm {
    uint32_t dpmboot;
    uint16_t error;
    uint16_t debugport;
    bool key_armed; /* 0x5AF0 was the last key written */
    struct ppc_cpu *core1; /* the core DPMKEY releases, or NULL */
} ppc_sscm_t;

void ppc_sscm_reset(ppc_sscm_t *s);

/*
 * PPC_SSCM_SIZE bytes at PPC_SSCM_BASE. Every register takes 8-, 16- and
 * 32-bit accesses, aligned, as 49.3 says; DPMKEY and the password words
 * read as zero.
 */
extern const emu_dev_ops_t ppc_sscm_ops;

#ifdef __cplusplus
}
#endif

#endif /* PPC_SSCM_H */
