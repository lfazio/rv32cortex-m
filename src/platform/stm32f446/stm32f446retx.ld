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
_stack_size = 2K;

/*
 * The first FreeRTOS task's stack, where the main stack always was.
 *
 * Once the scheduler runs, the main stack serves interrupts and nothing
 * else, and the run loop, the translator and the console run on this
 * one (src/platform/common/emu_os.c). It is reserved here, beside the
 * main stack at the top of RAM, rather than declared in .bss, because a
 * .bss array moved everything after it: on the F746 the JIT's code
 * buffer slid 9 KiB further out of the DTCM and CoreMark at a 32 KB
 * cache ran 9.6% slower with identical translations. A run measured
 * 1,308 bytes used.
 */
_task_stack_size = 4K;
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

        /*
         * Code that runs from SRAM. Placed inside .data so ST's startup
         * copies it from flash along with everything else -- no extra
         * loader needed. Executing from RAM avoids the 5 flash wait states
         * this part runs at 180 MHz, which matters for the interpreter's
         * dispatch loop and for JIT-emitted code.
         */
        . = ALIGN(8);
        __ramfunc_start = .;
        *(.ramfunc)
        *(.ramfunc*)
        . = ALIGN(8);
        __ramfunc_end = .;

        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    /*
     * Thread-local storage, the way picolibc's own linker script lays it
     * out -- because picolibc keeps errno thread-local and reads it through
     * __tls. Nothing set __tls on these boards, so it stayed 0 and every
     * errno store went to address 8: ITCM on the N6 (and on the F746),
     * where it zeroed the `sub sp` at the head of emu_jit_run. board.c's
     * board_tls_init() copies and zeroes this block and points __tls at it
     * before anything else runs. Separate output sections, not input
     * sections inside .data/.bss: ld takes the TLS segment, and every
     * thread-pointer offset, from output sections it can see are TLS.
     */
    .tdata : ALIGN(8)
    {
        __tls_base = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
    } > RAM AT> FLASH
    __tdata_source = LOADADDR(.tdata);
    __tdata_size = SIZEOF(.tdata);

    .tbss (NOLOAD) : ALIGN(8)
    {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
    } > RAM
    __tbss_offset = ADDR(.tbss) - __tls_base;
    __tbss_size = SIZEOF(.tbss);
    __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));
    __arm32_tls_tcb_offset = MAX(8, __tls_align);

    /* Both ALIGN(8): ld drops an empty .tdata, and .tbss must then start
       where __tls_base says the block does. board_tls_init() checks it. */
    /* .tbss occupies no address space of its own; reserve it. */
    .tbss_space (NOLOAD) :
    {
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > RAM

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
        . = ORIGIN(RAM) + LENGTH(RAM) - _stack_size - _task_stack_size - _min_heap_size;
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
        __task_stack_start = .;
        . = . + _task_stack_size;
        __task_stack_end = .;
        . = . + _min_heap_size;
        . = . + _stack_size;
        . = ALIGN(8);
    } > RAM

    ASSERT(__guest_ram_end > __guest_ram_start,
           "no RAM left for the guest: reduce _stack_size or the firmware's static data")

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
