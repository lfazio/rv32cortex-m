/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_boot.h - BOOTCTRL: which PEs are running.
 *
 * R01UH0923EJ0130 section 11.4.79. See g4mh_boot.c for why this exists
 * and what it deliberately does not model.
 */
#ifndef G4MH_G4MH_BOOT_H
#define G4MH_G4MH_BOOT_H

#include "emu/emu_bus.h"

#include "g4mh_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct g4mh_cpu;

#define G4MH_BOOTCTRL_BASE      0xFFFB2000u
#define G4MH_BOOTCTRL_SIZE      0x00000010u
#define G4MH_BOOTCTRL_OFF       0x0000u
#define G4MH_BOOTCTRL_BC_MASK   0x3Fu     /* BC5..BC0, one per PE */

typedef struct g4mh_boot {
    uint32_t bc;
    struct g4mh_cpu *cpu[G4MH_PE_COUNT];
} g4mh_boot_t;

void g4mh_boot_init(g4mh_boot_t *b);
void g4mh_boot_attach(g4mh_boot_t *b, unsigned pe, struct g4mh_cpu *c);

extern const emu_dev_ops_t g4mh_boot_ops;

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_BOOT_H */
