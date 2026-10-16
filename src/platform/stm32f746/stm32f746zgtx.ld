/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Linker script for the STM32F746ZG (512 KiB flash, 128 KiB SRAM).
 *
 * The symbol names are the ones ST's startup_stm32f446xx.s expects
 * (_estack, _sidata, _sdata, _edata, _sbss, _ebss); changing them breaks
 * the vendor startup code.
 *
 * SRAM1 (112 KiB) and SRAM2 (16 KiB) are contiguous from 0x20000000, so
 * they are declared as one 128 KiB region.
 */

ENTRY(Reset_Handler)

/*
 * The F746ZG has 320 KiB of RAM in three banks that happen to be
 * contiguous: 64 KiB of DTCM at 0x20000000 followed by 240 KiB of SRAM1
 * and 16 KiB of SRAM2. They are declared as one region because they abut
 * exactly, which is what lets the guest have a single flat block of it.
 *
 * DTCM being first matters: it is tightly coupled, single cycle, and never
 * goes through the D-cache, so everything the emulator itself touches
 * hardest -- the hart struct, the JIT code cache -- lands there simply by
 * being linked early, and guest RAM gets the rest.
 *
 * ITCM at 0x00000000 is 16 KiB that costs the guest nothing, because it
 * is outside the 320 KiB the RAM region above carves up. It does need an
 * explicit copy -- ST's startup only knows about .data -- which board.c
 * does before anything in it can be called.
 *
 * What it is for is narrow and specific: code that must keep running
 * while the flash bank is busy. A sector erase stalls every fetch from
 * flash for seconds, so the routine driving the erase cannot itself live
 * there, and neither can any interrupt handler expected to fire during
 * it. The existing .ramfunc mechanism would work equally well for that
 * -- DTCM is not behind the flash interface either -- but every byte it
 * uses is a byte taken from the guest, and ITCM's are free.
 */
MEMORY
{
    ITCM  (rx)  : ORIGIN = 0x00000000, LENGTH = 16K
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 320K
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

    /*
     * Code that must survive a busy flash bank. Loaded from flash and
     * copied to ITCM by board_init() -- ST's startup copies .data and
     * knows nothing about this, so anything placed here is unreachable
     * until that copy has run.
     *
     * The section is emitted unconditionally, so __itcm_start and
     * __itcm_end are equal when nothing uses it and the copy is a no-op.
     */
    /*
     * At 0x100, not 0: ITCM *is* address zero on this part, so a store
     * through a near-NULL pointer lands in it. picolibc's errno did
     * exactly that until __tls was set (board.c), and on the N6 it
     * zeroed the first instructions of emu_jit_run. With the hot path
     * here that would be every guest instruction's dispatch; below 0x100
     * such a store hits nothing that runs.
     */
    .itcm (ORIGIN(ITCM) + 0x100) :
    {
        . = ALIGN(8);
        __itcm_start = .;
        *(.itcm)
        *(.itcm*)

        /*
         * The HAL's flash driver, whole. Marking only our own routines
         * .itcm was not enough and looked like it was: board_flash_write
         * lands in ITCM and then calls HAL_FLASH_Program, which polls the
         * busy bit from flash -- so the poll loop is fetched from the
         * bank it is waiting on. It still works, because the core stalls
         * and resumes rather than faulting, which is exactly why the gap
         * survived a size check. It just gives back everything ITCM was
         * for.
         *
         * Named by object file because these are vendor sources and not
         * ours to annotate.
         */
        *libstm32cube.a:stm32f7xx_hal_flash.c.obj(.text .text.*)
        *libstm32cube.a:stm32f7xx_hal_flash_ex.c.obj(.text .text.*)
        . = ALIGN(8);
        __itcm_end = .;
    } > ITCM AT> FLASH
    __itcm_load = LOADADDR(.itcm);

    .text :
    {
        . = ALIGN(4);
        /*
         * An instrument, 0 unless -DEMU_LAYOUT_PAD=N: moves every function
         * in .text by N bytes and changes nothing else, so a figure that
         * moves with it is layout and not code. The complement of the A/B
         * this board's notes already use -- the same code at another
         * address, where that one is other code at the same address.
         */
        . = . + (DEFINED(EMU_LAYOUT_PAD) ? EMU_LAYOUT_PAD : 0);
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

        /*
         * Data that must be in DTCM, placed first so that it is -- rather
         * than merely likely to be.
         *
         * DTCM is the first 64 KiB of the RAM region, single cycle and
         * never behind the D-cache, and until now everything relied on
         * that: whatever the linker happened to emit first got it. That
         * worked while .bss was small and stopped being a safe assumption
         * when lwIP added ~18 KiB to it -- .bss is now around 39 KiB of
         * the 64, and nothing would report the day the hart struct or the
         * JIT code cache slid past the boundary into SRAM1. It would just
         * get slower, which is the hardest kind of regression to attribute.
         *
         * Inside .bss rather than in a section of its own so that ST's
         * startup zeroes it along with everything else. A separate NOLOAD
         * region would need its own clearing loop, and the failure mode of
         * forgetting one is uninitialised state that is usually zero
         * anyway.
         */
        . = ALIGN(8);
        __dtcm_start = .;
        *(.dtcm)
        *(.dtcm*)
        . = ALIGN(8);
        __dtcm_end = .;

        /* The instrument for everything else in .bss -- the JIT's code
           buffer and tables, the frontend's state, and guest RAM after
           them: -DEMU_LAYOUT_PAD_BSS=N moves all of it N bytes up and
           leaves .dtcm and .text where they were. */
        . = . + (DEFINED(EMU_LAYOUT_PAD_BSS) ? EMU_LAYOUT_PAD_BSS : 0);
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /*
     * The boundary that was previously only a comment. Overflowing DTCM
     * is not an error at link time by default -- the RAM region is all
     * 320 KiB and the excess simply lands in SRAM1 -- so without this the
     * only symptom is that something hot quietly stopped being fast.
     */
    ASSERT(__dtcm_end <= ORIGIN(RAM) + 64K,
           ".dtcm overflows the 64 KiB DTCM: move something out")

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
        /* The same instrument for data: -DEMU_LAYOUT_PAD_RAM=N moves the
           guest's RAM N bytes up, and nothing else. */
        . = . + (DEFINED(EMU_LAYOUT_PAD_RAM) ? EMU_LAYOUT_PAD_RAM : 0);
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
