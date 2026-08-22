/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_elf.c - Minimal ELF32 loader for guest images.
 *
 * Only what is needed to place a statically linked bare-metal executable
 * into the guest address space: walk the program headers and copy each
 * PT_LOAD segment, zeroing the .bss tail where memsz exceeds filesz.
 *
 * Nothing here is architecture-specific except the e_machine check, and
 * that is the caller's to make: the frontend declares the machine number
 * it accepts and the loader reports what it found. RV32 and RH850 images
 * are both little-endian ELF32 with the same program header layout.
 *
 * The loader is deliberately strict. A malformed image is a bug in the
 * build, not something to recover from, so every field that matters is
 * validated and a bad one is reported rather than worked around.
 */

#include "emu/emu_elf.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* ELF32 structures (little-endian, as used by RV32)                   */
/* ------------------------------------------------------------------ */

#define EI_NIDENT   16

#define ET_EXEC     2
#define PT_LOAD     1

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

/* ------------------------------------------------------------------ */

/*
 * Reads are done through memcpy rather than by casting into the buffer:
 * the image is caller-supplied and need not satisfy the alignment these
 * structures require.
 */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool emu_elf_is_elf(const void *image, size_t len)
{
    const uint8_t *const b = (const uint8_t *)image;

    return len >= 4u && b[0] == 0x7Fu && b[1] == 'E' &&
           b[2] == 'L' && b[3] == 'F';
}

uint16_t emu_elf_machine(const void *image, size_t len)
{
    const uint8_t *const b = (const uint8_t *)image;

    return (len >= 20u) ? rd16(b + 18) : 0u;
}

/*
 * Both entry points, differing only in what they do with a segment.
 *
 * `ram_size` of zero means "copy everything", which is emu_elf_load;
 * otherwise a segment inside guest RAM is copied and everything else is
 * mapped read-only out of the image where it already lies.
 */
static const char *elf_walk(emu_bus_t *bus, const void *image, size_t len,
                            uint16_t machine, uint16_t alt_machine,
                            uint32_t ram_base, uint32_t ram_size,
                            uint32_t *entry, uint16_t *out_machine)
{
    const uint8_t *img = (const uint8_t *)image;

    if (len < 52u) {
        return "image shorter than an ELF32 header";
    }
    if (!(img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F')) {
        return "not an ELF file";
    }
    if (img[4] != 1) {
        return "not ELF32";
    }
    if (img[5] != 1) {
        return "not little-endian";
    }

    const uint16_t e_type    = rd16(img + 16);
    const uint16_t e_machine = rd16(img + 18);
    const uint32_t e_entry   = rd32(img + 24);
    const uint32_t e_phoff   = rd32(img + 28);
    const uint16_t e_phentsz = rd16(img + 42);
    const uint16_t e_phnum   = rd16(img + 44);

    if (e_type != ET_EXEC) {
        return "not a static executable (ET_EXEC)";
    }
    /*
     * A frontend may answer to more than one machine number; see the note
     * on EMU_EM_V800. alt_machine is that second answer, or zero.
     */
    if (machine != EMU_ELF_ANY_MACHINE && e_machine != machine &&
        !(alt_machine != 0u && e_machine == alt_machine)) {
        return "wrong architecture for this frontend";
    }
    if (out_machine != NULL) {
        *out_machine = e_machine;
    }
    if (e_phentsz < 32u) {
        return "bad program header entry size";
    }
    if (e_phnum == 0u) {
        return "no program headers";
    }
    /* Bound the header table against the file before touching it. */
    if ((uint64_t)e_phoff + (uint64_t)e_phentsz * e_phnum > len) {
        return "program header table outside file";
    }

    unsigned loaded = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = img + e_phoff + (size_t)i * e_phentsz;

        if (rd32(ph + 0) != PT_LOAD) {
            continue;
        }

        const uint32_t p_offset = rd32(ph + 4);
        const uint32_t p_vaddr  = rd32(ph + 8);
        const uint32_t p_paddr  = rd32(ph + 12);
        const uint32_t p_filesz = rd32(ph + 16);
        const uint32_t p_memsz  = rd32(ph + 20);

        if (p_filesz > p_memsz) {
            return "segment filesz exceeds memsz";
        }
        if ((uint64_t)p_offset + p_filesz > len) {
            return "segment extends past end of file";
        }

        /*
         * Load at the physical address: these are bare-metal images with
         * no MMU, and p_paddr is what a loader is supposed to honour.
         */
        const bool in_ram = ram_size != 0u &&
                            p_paddr >= ram_base &&
                            (uint64_t)p_paddr + p_memsz <=
                                (uint64_t)ram_base + ram_size;

        if (ram_size != 0u && !in_ram) {
            /*
             * Mapped, not copied. This is a guest's .text and .rodata on
             * a board: they are already in flash where the upload put
             * them, they are the largest part of the image, and copying
             * them into guest RAM is what used to make the biggest
             * architecture tests not fit at all.
             */
            if (p_filesz != 0u &&
                !emu_bus_add_rom(bus, "elf", p_paddr, img + p_offset,
                                 p_filesz)) {
                return "no room to map a segment";
            }

            /*
             * The .bss tail belongs at the *run* address, not the load
             * one. In an execute-in-place image those differ -- p_paddr
             * is where .data's initialiser sits in flash and p_vaddr is
             * where it runs -- so zeroing at p_paddr would write into
             * flash, and refusing the segment would reject every guest
             * with a .bss. Which is to say: coremark, whose second
             * segment is 28 bytes of .data and 2 KiB of .bss.
             *
             * The guest's own start.S zeroes this too. Doing it here as
             * well costs nothing and means an image whose crt0 does not
             * still gets what the ELF promises.
             */
            if (p_memsz > p_filesz) {
                const uint32_t bss = p_vaddr + p_filesz;
                uint32_t rem = p_memsz - p_filesz;

                if (bss < ram_base ||
                    (uint64_t)bss + rem > (uint64_t)ram_base + ram_size) {
                    return "segment .bss lands outside guest RAM";
                }
                static const uint8_t zeros[256] = { 0 };
                uint32_t addr = bss;

                while (rem != 0u) {
                    const uint32_t n = (rem > sizeof(zeros))
                                     ? (uint32_t)sizeof(zeros) : rem;

                    if (!emu_bus_load(bus, addr, zeros, n)) {
                        return "bss does not fit in guest memory";
                    }
                    addr += n;
                    rem -= n;
                }
            }
            loaded++;
            continue;
        }

        if (p_filesz != 0u) {
            if (!emu_bus_load(bus, p_paddr, img + p_offset, p_filesz)) {
                return "segment does not fit in guest memory";
            }
        }

        /* Zero the .bss tail. */
        if (p_memsz > p_filesz) {
            static const uint8_t zeros[256] = { 0 };
            uint32_t addr = p_paddr + p_filesz;
            uint32_t rem = p_memsz - p_filesz;
            while (rem != 0u) {
                const uint32_t n = (rem > sizeof(zeros)) ? (uint32_t)sizeof(zeros)
                                                         : rem;
                if (!emu_bus_load(bus, addr, zeros, n)) {
                    return "bss does not fit in guest memory";
                }
                addr += n;
                rem -= n;
            }
        }
        loaded++;
    }

    if (loaded == 0u) {
        return "no PT_LOAD segments";
    }

    if (entry != NULL) {
        *entry = e_entry;
    }
    return NULL;
}

const char *emu_elf_load(emu_bus_t *bus, const void *image, size_t len,
                         uint16_t machine, uint16_t alt_machine,
                         uint32_t *entry, uint16_t *out_machine)
{
    return elf_walk(bus, image, len, machine, alt_machine, 0u, 0u,
                    entry, out_machine);
}

const char *emu_elf_map(emu_bus_t *bus, const void *image, size_t len,
                        uint16_t machine, uint16_t alt_machine,
                        uint32_t ram_base, uint32_t ram_size,
                        uint32_t *entry, uint16_t *out_machine)
{
    if (ram_size == 0u) {
        return "emu_elf_map needs a guest RAM window";
    }
    return elf_walk(bus, image, len, machine, alt_machine, ram_base,
                    ram_size, entry, out_machine);
}
