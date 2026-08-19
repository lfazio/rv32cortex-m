/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start.S - Reset entry for RV32 guest test images.
 *
 * The emulator jumps here with nothing initialised, so this sets up the
 * stack and global pointer, clears .bss, and calls main(). On return it
 * issues the newlib exit syscall, which the host runner intercepts.
 */

    .section .text.init, "ax"
    .globl _start
    .type _start, @function
_start:
    /*
     * gp is set up with relaxation disabled: the linker would otherwise
     * rewrite `la gp, __global_pointer$` into a gp-relative load, which
     * cannot work while gp is still undefined.
     */
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    /*
     * The emulator hands us a stack pointer in sp and the size of guest
     * RAM in a1, because only the platform knows how much memory it could
     * spare. Fall back to the link script's _stack_top only if sp is zero,
     * which is what a loader that does not implement the boot protocol
     * leaves behind.
     */
    bnez    sp, 1f
    la      sp, _stack_top
1:

    /*
     * Copy .data from its load address to its run address.
     *
     * **The guest's job, not the emulator's.** This used to be done by
     * the firmware, which memcpy'd the writable half of a pre-split
     * image into the base of guest RAM before entry -- so a guest could
     * not be loaded as a single blob and run, and the split it required
     * had already lost three bytes wherever .rodata ended unaligned.
     *
     * __data_lma is in the ROM window (the whole image, read-only, at
     * EMU_GUEST_ROM_BASE); __data_start is in guest RAM. When the two
     * coincide -- a loader that mapped the image writable in place --
     * the loop copies a region onto itself, which is why it compares
     * first rather than assuming a copy is needed.
     */
    la      t0, __data_lma
    la      t1, __data_start
    la      t2, __data_end
    beq     t0, t1, 2f
1:  bgeu    t1, t2, 2f
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       1b
2:

    /* Clear .bss. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:

    /* a0 = hartid, a1 = guest RAM size, both set by the emulator. */
    call    main

    /* exit(main()) */
    li      a7, 93
    ecall

    /* The host halts us at the ecall; spin if it ever returns. */
3:  j       3b
    .size _start, . - _start
