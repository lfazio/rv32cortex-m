/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bigguest.ld - a compiled e200z7 guest too big for cguest.ld's
 * megabyte: a game with its data compiled in. The same layout -- one
 * image at 0x80000000, the vectors first, .bss after it -- in 32 MiB,
 * so run it with --ram 0x2000000 or more. r1 is the loader's, the top
 * of whatever RAM the run was given.
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 32M
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
