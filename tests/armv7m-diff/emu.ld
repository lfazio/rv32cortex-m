/* SPDX-License-Identifier: Apache-2.0 */
/*
 * guest.ld - where an ARMv7-M guest's sections go.
 *
 * Two regions, matching what the platform maps: the image is served
 * read-only from EMU_GUEST_ROM_BASE and RAM is at EMU_GUEST_RAM_BASE.
 *
 * **.data and .bss have to be placed, and the first version of this
 * script omitted them.** With only .vectors and .text, every global
 * landed at whatever address the linker picked for an unplaced section
 * -- the guest loaded a literal-pool pointer and faulted on the
 * ordinary `ldr` that followed, which reported the *load* as an
 * unimplemented encoding. The instruction was fine; the address was
 * nowhere. Same shape as the RV32 guests' own history: a `.data` the
 * firmware never installed, read as zeroes, with nothing to show for
 * it.
 *
 * .data gets a load address in ROM and a run address in RAM, and
 * start.S copies it -- the arrangement any crt0 uses, and the one this
 * tree arrived at for RV32 after trying to have the loader do it.
 */

ENTRY(_start)

MEMORY {
    ROM (rx)  : ORIGIN = 0x20000000, LENGTH = 1M
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M
}

SECTIONS {
    /* sys.c executes from this under an MPU region; RAM is executable here. */
    .sram1 (NOLOAD) : { . = ALIGN(1024); *(.sram1*) } > RAM
    .vectors : { KEEP(*(.vectors)) } > ROM
    .text    : { *(.text*) *(.rodata*) } > ROM

    /*
     * AT> ROM is what makes this work: the run addresses accumulate in
     * RAM while the load addresses stay in ROM, so `objcopy -O binary`
     * -- which lays out by load address -- produces one contiguous blob
     * and __data_lma is where the bytes actually are.
     */
    .data : {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > RAM AT> ROM
    __data_lma = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM
}
