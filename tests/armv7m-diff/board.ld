/* SPDX-License-Identifier: Apache-2.0 */
/* board.ld - the harness in the F746's flash, its data in SRAM. */
ENTRY(Reset_Handler)

MEMORY {
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 320K
}

__stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text    : { *(.text*) *(.rodata*) } > FLASH

    .data : {
        . = ALIGN(4);
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT> FLASH
    __data_lma = LOADADDR(.data);

    /*
     * SRAM1, which -- unlike the DTCM the rest of RAM starts with -- the
     * core can execute from: sys.c runs an instruction from here under
     * an MPU region to see execute-never refuse it.
     */
    .sram1 0x20040000 (NOLOAD) : { *(.sram1*) }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
}
