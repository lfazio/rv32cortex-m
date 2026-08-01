/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RV32_RV_ELF_H
#define RV32_RV_ELF_H

#include "rv32/rv_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load a static RV32 ELF executable into the guest address space.
 * Returns NULL on success, or a static string describing the failure.
 * The entry point is stored through `entry` when it is not NULL.
 */
const char *rv_elf_load(rv_bus_t *bus, const void *image, size_t len,
                        uint32_t *entry);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_ELF_H */
