/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stm32n657xx_lrun.ld - the Nucleo-N657X0-Q, load-and-run from AXISRAM.
 *
 * Started from ST's own STM32N657XX_LRUN.ld and given a .guest_ram
 * section, exactly as the F446 and F746 scripts are.
 *
 * **Why LRUN and not the AXISRAM2 FSBL shape.** The N6 has no internal
 * user flash at all, so every arrangement here is about where in AXISRAM
 * the image lands and who puts it there. ST ships three:
 *
 *   AXISRAM2 FSBL   ROM 0x34180400 255K + RAM 0x341C0000 256K
 *   LRUN            ROM 0x34000400 511K + RAM 0x34080000 1536K
 *   XIP             ROM in external OSPI + RAM 0x34000000 2048K
 *
 * All three are loaded by the boot ROM; LRUN gives the guest **1536 KiB**
 * against the FSBL shape's 256, which is six times what the F746 has, and
 * costs nothing extra -- there is no XIP driver to write because the code
 * is copied to RAM before it runs. The XIP shape would need the Octo-SPI
 * flash brought up before the first instruction, which is a bring-up
 * problem to solve when 1.5 MB stops being enough.
 *
 * **The +0x400 on ORIGIN is the boot ROM's header, not slack.** Getting
 * it wrong means the ROM refuses the image rather than the image
 * misbehaving, which is at least a loud failure.
 *
 * Everything the F746 does with internal flash has no equivalent here:
 * the guest image is still embedded in the firmware image (and so still
 * ends up in AXISRAM), and there is no sector-erasable arena, so this
 * board takes no TFTP uploads -- board_flash_arena_size() returns 0,
 * which is how a platform declines. Giving it a RAM-backed arena is a
 * later change and a small one.
 */

ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of "RAM" Ram type memory */
_sstack = _estack - _Min_Stack_Size;

/*
 * 8 KiB of stack, not ST's 2. This firmware runs an lwIP stack and a JIT
 * translator on the same stack a template blinks an LED on, and the F746
 * has needed 8 for exactly that. A stack that overflows here runs into
 * guest RAM, where it corrupts the guest rather than faulting -- so the
 * guest misbehaves and the firmware looks fine.
 */
_Min_Heap_Size = 0x200;   /* picolibc's vsnprintf wants a little */
_Min_Stack_Size = 0x2000;

/* Memories definition */
/*
 * **This is an FSBL, so it links where the boot ROM puts an FSBL.**
 *
 * The pack ships two scripts and the names do not say which is which:
 * STM32N657XX_LRUN.ld is for the *application* a first-stage loader
 * copies out of external flash, and STM32N657XX_AXISRAM2_fsbl.ld is for
 * the loader itself. ST's own Template_FSBL_LRUN uses the second for its
 * Boot project and the first for its AppS project -- Boot at 0x34180400,
 * AppS at 0x34000400.
 *
 * This firmware is the whole thing: there is no second stage, so it is
 * the FSBL and belongs at 0x34180400 in AXISRAM2. Linked at the
 * application's address instead, everything works right up to the point
 * of running -- the image downloads, reads back correctly, and the
 * signing tool finds the right entry point -- and the ROM simply never
 * jumps there, because in development boot it runs an FSBL and nothing
 * else. No error anywhere; the core just keeps doing what it was doing.
 *
 * Guest RAM moves to AXISRAM1, which the FSBL would otherwise have
 * copied an application into and which is free here for the same reason.
 * The guest ends up with much the same space from a different bank.
 *
 * **AXISRAM1 starts at 0x34000400, not 0x34000000**, for the same reason
 * the FSBL does: the first kilobyte is a header area. ST's AppS script
 * leaves it alone and so must this. Put .data there and it reads back as
 * "STM2" -- the ROM's own signature sitting where a pointer should be,
 * which looks exactly like a .data copy that half worked.
 */
MEMORY
{
  /*
   * AXISRAM2 is 0x34180000..0x34200000, and the two regions divide it.
   * ST's template splits it 255K/256K, which suits a blinky and not a
   * 133 KB emulator with a frontend's .bss behind it -- the G4MH build
   * overflowed `RAM' by 3144 bytes at that split. The boundary is ours
   * to choose; what is *not* negotiable is that RAM sit above ROM, so
   * that _estack (ORIGIN(RAM) + LENGTH(RAM)) lands at the top of the
   * bank rather than underneath the code.
   */
  ROM    (xrw)    : ORIGIN = 0x34180400,   LENGTH = 191K
  RAM    (xrw)    : ORIGIN = 0x341B0000,   LENGTH = 320K
  GUEST  (xrw)    : ORIGIN = 0x34000400,   LENGTH = 1535K
}

/* Sections */
SECTIONS
{
  /* The startup code into "RAM" Ram type memory */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >ROM

  /* The program code and other data into "RAM" Ram type memory */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >ROM

  /* Constant data into "RAM" Ram type memory */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >ROM

  .ARM.extab   : {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >ROM

  .ARM : {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >ROM

  .preinit_array     :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >ROM

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >ROM

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >ROM

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */

  } >RAM AT> ROM

  .noncacheable :
  {
    . = ALIGN(8);
    __snoncacheable = .;/* create symbol for start of section */
    KEEP(*(.noncacheable))
    . = ALIGN(8);
    __enoncacheable = .;  /* create symbol for end of section */
  } > RAM

  .gnu.sgstubs :
  {
    _sNSCVeneer = .;
    . = ALIGN(4);
    *(.gnu.sgstubs*)   /* Secure Gateway stubs */
    . = ALIGN(4);
  } >ROM
  _eNSCVeneer = .;

  /* Uninitialized data section into "RAM" Ram type memory */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /*
   * Guest RAM: everything between the firmware's .bss and the stack.
   *
   * NOLOAD because it is the guest's and the firmware clears it at start
   * -- putting it in the image would make the boot ROM copy 1.5 MB of
   * nothing over the link every reset.
   */
  /*
   * The guest gets AXISRAM1 whole, as its own region.
   *
   * It used to be carved out of the end of >RAM, which is what the F4/F7
   * ports do -- there the firmware and the guest share one bank and the
   * guest takes what is left. Here they are different banks, and sharing
   * the *region* had a consequence that took ST's own template to see:
   * _estack is ORIGIN(RAM) + LENGTH(RAM), so putting RAM in AXISRAM1
   * *below* the code in AXISRAM2 put the initial stack pointer at
   * 0x34180000 -- underneath this image's own first byte. Reset_Handler
   * sets that as SP in its first two instructions, before anything else,
   * so nothing ever ran and no diagnostic could say why.
   */
  .guest_ram (NOLOAD) :
  {
    . = ALIGN(8);
    __guest_ram_start = .;
    . = ORIGIN(GUEST) + LENGTH(GUEST);
    __guest_ram_end = .;
  } >GUEST

  /*
   * Reserved after it, so the link fails loudly rather than silently
   * overlapping guest RAM at run time.
   */
  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  ASSERT(__guest_ram_end > __guest_ram_start,
         "no AXISRAM left for the guest: the firmware filled it")

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
