/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_elf.h - ELF32 image loader.
 *
 * Not host-only any more, and the comment that said so was the reason a
 * board could be sent only a flat binary. An ELF is what every toolchain
 * produces and what every debugger reads; making the board take one
 * removes the objcopy step from the middle of a test harness and lets the
 * *same file* be uploaded and opened in gdb.
 *
 * There are two ways in, because a board and a host differ in one thing
 * that matters: where the image already is.
 *
 *   emu_elf_load   copies every segment into the bus. What a host wants,
 *                  since the file was read into malloc'd memory and guest
 *                  RAM is the only place to run from.
 *
 *   emu_elf_map    copies only the segments that land in guest RAM, and
 *                  *maps* the rest read-only straight out of the image.
 *                  What a board wants: its uploaded image is already in
 *                  flash, and a guest's .text is the largest part of it --
 *                  345 KiB for the biggest architecture tests, against
 *                  243 KiB of guest RAM. Copying would not fit, and
 *                  mapping costs nothing.
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

/*
 * The same, for an image that is already addressable where it sits.
 *
 * A PT_LOAD landing inside [ram_base, ram_base+ram_size) is copied, as
 * emu_elf_load would; anything else becomes a read-only region pointing
 * into `image` itself. `image` must therefore stay put and stay readable
 * for as long as the guest runs -- on a board it is the flash arena,
 * which is exactly that.
 *
 * A segment with a .bss tail outside guest RAM is refused rather than
 * quietly mapped short: there is nowhere to put the zeroes, and a guest
 * whose .bss silently reads as whatever was in flash is the kind of wrong
 * answer that looks like a miscompile.
 */
const char *emu_elf_map(emu_bus_t *bus, const void *image, size_t len,
                        uint16_t machine, uint16_t alt_machine,
                        uint32_t ram_base, uint32_t ram_size,
                        uint32_t *entry, uint16_t *out_machine);

/* Does this look like an ELF at all? Four bytes of magic; the loader
 * checks the rest. */
bool emu_elf_is_elf(const void *image, size_t len);

/* The declared e_machine, or 0 for something too short to have one. */
uint16_t emu_elf_machine(const void *image, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EMU_ELF_H */
