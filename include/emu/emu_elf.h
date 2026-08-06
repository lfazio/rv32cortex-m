/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_elf.h - ELF32 image loader.
 *
 * Host-only: the firmware carries a flat binary, because parsing program
 * headers on a part with 512 KiB of flash buys nothing a linker script has
 * not already done.
 */
#ifndef EMU_ELF_H
#define EMU_ELF_H

#include "emu_bus.h"
#include "emu_cpu.h"   /* EMU_EM_*, declared with the frontends */

#ifdef __cplusplus
extern "C" {
#endif

/* Accept whatever the image declares, and report it through out_machine. */
#define EMU_ELF_ANY_MACHINE 0u

/*
 * Load a static ELF32 executable into the guest address space. Returns
 * NULL on success, or a static string describing the failure.
 *
 * `machine` is the e_machine the caller requires, or EMU_ELF_ANY_MACHINE
 * to take any. `alt_machine` is a second number the same frontend answers
 * to, or zero -- RH850 needs it, because Renesas and GNU disagree about
 * which one to emit. The entry point is stored through `entry` and the
 * image's declared machine through `out_machine`; either may be NULL.
 */
const char *emu_elf_load(emu_bus_t *bus, const void *image, size_t len,
                         uint16_t machine, uint16_t alt_machine,
                         uint32_t *entry, uint16_t *out_machine);

#ifdef __cplusplus
}
#endif

#endif /* EMU_ELF_H */
