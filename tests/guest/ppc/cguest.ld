/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cguest.ld - a compiled e200z7 guest, flat in RAM.
 *
 * One image at 0x80000000, which is the 64 KiB-aligned address IVPR
 * needs: the vectors are the first thing in it. Loaded as ELF, so .bss
 * need not be in the file; crt0 zeroes it.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M
}

SECTIONS
{
    .text :
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
    } > RAM

    .data : ALIGN(4)
    {
        *(.data .data.*)
        *(.sdata .sdata.* .sdata2 .sdata2.*)
    } > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.sbss .sbss.* .sbss2 .sbss2.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.gnu.attributes) }
}
