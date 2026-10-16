/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bigguest.ld - a compiled e200z7 guest too big for cguest.ld's
 * megabyte: a game with its data compiled in. The same layout -- one
 * image at 0x80000000, the vectors first, .bss after it -- in up to
 * 64 MiB, which is what Quake takes: an 18.7 MB PAK in the image and a
 * 24 MiB heap in .bss. r1 is the loader's, the top of whatever RAM the
 * run was given, so --ram has to cover the image, .bss and a stack:
 * 0x2000000 for DOOM, 0x4000000 for Quake.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M
}

SECTIONS
{
    .text :
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
    } > RAM

    .data : ALIGN(8)
    {
        *(.data .data.*)
        *(.sdata .sdata.* .sdata2 .sdata2.*)
    } > RAM

    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.sbss .sbss.* .sbss2 .sbss2.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.gnu.attributes) }
}
