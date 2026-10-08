/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start.S - the ARMv7-M guests' start-up.
 *
 * The vector table, .data installed, .bss zeroed, main called. Doing
 * that here rather than in the loader is the conclusion this tree
 * reached for its RISC-V guests the hard way: a guest that cannot
 * initialise itself is a core image rather than a program, and the
 * split it forces on the loader was wrong in three bytes wherever
 * .rodata ended unaligned.
 *
 * **The table is addresses, not instructions**, which is the thing an
 * ARMv7-M vector table gets right compared with every other
 * architecture in this tree: offset 0 is the initial stack pointer and
 * offset 4 is the reset address with its Thumb bit set. A guest that
 * gets it wrong faults before its first instruction rather than twenty
 * bytes later.
 */

    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x80100000          /* 0  initial SP: top of the RAM window */
    .word   _start + 1          /* 1  reset, Thumb bit set              */
    /*
     * The rest of the system exceptions, up to SysTick at 15.
     *
     * **A table with only reset in it is a table that cannot report.**
     * Every unused slot points at `.Lunhandled`, which halts on a BKPT
     * -- so an exception this guest did not expect stops *here*, at a
     * known address, instead of vectoring through a zero and executing
     * whatever is at the bottom of the image. That is the fourth time
     * this project has written that lesson down, and the first time the
     * vectors were right the first time because of it.
     *
     * SysTick_Handler is weak-by-convention rather than weak: a guest
     * that does not define one gets a link error, which is the honest
     * failure for a test that is *about* the handler running.
     */
    .word   .Lunhandled + 1     /* 2  NMI          */
    .word   .Lunhandled + 1     /* 3  HardFault    */
    .word   .Lunhandled + 1     /* 4  MemManage    */
    .word   .Lunhandled + 1     /* 5  BusFault     */
    .word   .Lunhandled + 1     /* 6  UsageFault   */
    .word   0                   /* 7  reserved     */
    .word   0                   /* 8  reserved     */
    .word   0                   /* 9  reserved     */
    .word   0                   /* 10 reserved     */
    .word   .Lunhandled + 1     /* 11 SVCall       */
    .word   .Lunhandled + 1     /* 12 DebugMonitor */
    .word   0                   /* 13 reserved     */
    .word   .Lunhandled + 1     /* 14 PendSV       */
    .word   SysTick_Handler + 1 /* 15 SysTick      */
    .word   IRQ0_Handler + 1    /* 16 external 0   */
    .word   .Lunhandled + 1     /* 17 external 1   */

    .text
    .thumb_func
    .global _start
_start:
    /* .data from its load address in ROM to its run address in RAM. */
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
.Lcopy:
    cmp     r1, r2
    beq     .Lzero
    ldr     r3, [r0]
    str     r3, [r1]
    adds    r0, r0, #4
    adds    r1, r1, #4
    b       .Lcopy

.Lzero:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
.Lbss:
    cmp     r0, r1
    beq     .Lmain
    str     r2, [r0]
    adds    r0, r0, #4
    b       .Lbss

.Lmain:
    bl      main
    bkpt    #0

/*
 * Any exception the guest did not arrange for. Halting with the
 * exception number in r0 is what turns "it stopped" into "it stopped
 * because something vectored here" -- the emulator prints the pc, and
 * this address is in the map.
 */
    .thumb_func
.Lunhandled:
    mrs     r0, ipsr
    bkpt    #0xAD
    b       .Lunhandled

/*
 * A guest with no SysTick handler still links, and still reports: this
 * is overridden by any strong definition.
 */
    .weak   SysTick_Handler
    .thumb_func
SysTick_Handler:
    b       .Lunhandled

    .weak   IRQ0_Handler
    .thumb_func
IRQ0_Handler:
    b       .Lunhandled
