/* SPDX-License-Identifier: Apache-2.0 */
/*
 * guest.ld - a flat e200z7 image.
 *
 * One contiguous blob loaded at 0x80000000, which is the host runner's
 * default --load address and matches what the RV32 and G4MH guests use.
 * Everything is in one output section on purpose: this image is copied
 * into guest memory as a single span, so a gap between sections would be
 * a gap in the file, and .data landing at the wrong offset is the defect
 * that cost the RV32 guest split three silent bytes -- see CLAUDE.md.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M
}

SECTIONS
{
    .image 0x80000000 :
    {
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
        *(.sdata .sdata.*)
        . = ALIGN(4);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
