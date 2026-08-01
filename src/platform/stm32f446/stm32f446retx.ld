/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Linker script for the STM32F446RE (512 KiB flash, 128 KiB SRAM).
 *
 * The symbol names are the ones ST's startup_stm32f446xx.s expects
 * (_estack, _sidata, _sdata, _edata, _sbss, _ebss); changing them breaks
 * the vendor startup code.
 *
 * SRAM1 (112 KiB) and SRAM2 (16 KiB) are contiguous from 0x20000000, so
 * they are declared as one 128 KiB region.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/*
 * Stack grows down from the top of RAM. The emulator's run loop is
 * iterative and the deepest call chain is the HAL's clock init, so 4 KiB
 * is generous; every byte saved here goes to the guest.
 */
_stack_size = 4K;
_min_heap_size = 0;

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        /* The embedded guest image lives here; see guest_image.S. */
        . = ALIGN(4);
        *(.rodata.guest)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : {
        . = ALIGN(4);
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        . = ALIGN(4);
    } > FLASH

    .ARM : {
        . = ALIGN(4);
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
        . = ALIGN(4);
    } > FLASH

    .preinit_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        . = ALIGN(4);
    } > FLASH

    .init_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        . = ALIGN(4);
    } > FLASH

    .fini_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(4);
    } > FLASH

    /* Initialised data: stored in flash, copied to RAM by the startup code. */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /*
     * Guest RAM: every byte between the end of the firmware's own .bss and
     * the stack. Sizing it here rather than as a fixed-size C array means
     * the guest automatically gets all the memory the firmware is not
     * using, and that shrinking the ARM side hands the difference straight
     * to the guest. NOLOAD keeps it out of the .bin.
     *
     * If the firmware ever grows past the available RAM, the location
     * counter moves backwards here and the link fails, which is the loud
     * failure we want.
     */
    .guest_ram (NOLOAD) :
    {
        . = ALIGN(8);
        __guest_ram_start = .;
        . = ORIGIN(RAM) + LENGTH(RAM) - _stack_size - _min_heap_size;
        __guest_ram_end = .;
    } > RAM

    /*
     * Reserve the stack (and heap, if ever needed) so the link fails
     * loudly rather than silently overlapping guest RAM at runtime.
     */
    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _min_heap_size;
        . = . + _stack_size;
        . = ALIGN(8);
    } > RAM

    ASSERT(__guest_ram_end > __guest_ram_start,
           "no RAM left for the guest: reduce _stack_size or the firmware's static data")

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
