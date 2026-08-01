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
