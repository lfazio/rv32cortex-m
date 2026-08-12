/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Link script for the official architecture tests running on rv32cortex-m.
 *
 * RAM_ORIGIN matches the emulator's guest RAM base. The tests are far
 * larger than anything the STM32 build can hold, so this configuration is
 * a host-side check of the core; the same core runs on the target with a
 * much smaller RAM carve-out.
 */

RAM_ORIGIN = 0x80000000;
RAM_LENGTH = 0x00800000;   /* 8 MiB, matches the runner's --ram */
TEST_BASE  = 0x80000000;   /* guest reset pc */
NUM_HARTS  = 1;

/* Most users should not need to modify anything below this line. */
/*
 * 16 KiB, not the 128 KiB this inherited.
 *
 * The stack sits *between* .bss and .text.rvmodel, so its size is not
 * an allocation -- it is a hole in the middle of the image, and the
 * emulator has to have RAM covering the whole span either way. At
 * 128 KiB it was the single largest thing in the address range of every
 * test: the widest was 478 KiB of span for 343 KiB of actual bytes.
 *
 * That is free on the host, where the runner is given 8 MiB. It is the
 * difference between running and not running on the STM32, where the
 * guest carve-out is 284 KiB. These tests are short assembly sequences
 * with a trap handler; 16 KiB is already generous.
 */
STACK_SIZE = 0x4000;

OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

MEMORY
{
  ram (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH
}

PROVIDE(__stack_size = STACK_SIZE);
PROVIDE(__num_harts = NUM_HARTS);

SECTIONS
{
  ASSERT(TEST_BASE >= ORIGIN(ram) && TEST_BASE < ORIGIN(ram) + LENGTH(ram), "TEST_BASE is outside RAM; update TEST_BASE and RAM_ORIGIN/RAM_LENGTH in link.ld")

  .text.init   TEST_BASE : { *(.text.init) } > ram
  .text.rvtest . : { *(.text.rvtest) *(.text.rvtest.*) } > ram

  /*
   * A page, not four. The Sv32 tests need their data page-aligned and
   * nothing needs more than that, so the extra 12 KiB was padding.
   */
  . = ALIGN(0x1000);
  .rodata . : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) } > ram

  /*
   * Where the read-only half ends, so the STM32 build can serve
   * everything below it out of the part's flash instead of copying it
   * into guest RAM -- see tests/guest/guest.ld.in, which carries the
   * same symbol for the ordinary guests.
   *
   * Here, and *only* after .rodata. It has to mark a contiguous
   * read-only prefix, and in this layout .data sits between .rodata and
   * .bss -- so putting it any later hands the guest's own signature
   * area to a ROM region and its first write faults. Placing it after
   * .data measured a beautiful result, 274 of 274 tests needing 22 KiB
   * of RAM, by classifying 341 KiB of writable memory as read-only.
   * A measurement that good is the first thing to distrust.
   */
  . = ALIGN(4);
  __guest_ro_end = .;

  .data   . : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) } > ram

  . = ALIGN(16);
  .bss . : {
    __bss_start = .;
    *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > ram

  . = ALIGN(16);
  __stack_bottom = .;
  . += __stack_size * __num_harts;
  __stack_top = .;

  . = ALIGN(0x1000);
  .text.rvmodel . : { *(.text.rvmodel) *(.text.rvmodel.*) *(.text) *(.text.*) } > ram

  . = ALIGN(0x1000);
  _end = .;

  ASSERT(_end <= ORIGIN(ram) + LENGTH(ram), "ACT ELF exceeds RAM; increase RAM_LENGTH here and --ram in the runner command")
}
