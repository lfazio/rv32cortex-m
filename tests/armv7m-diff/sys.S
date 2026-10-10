/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys.S - the pieces of the system-level suite that must be exact
 * instructions rather than whatever the compiler chooses.
 *
 * One entry stub serves every exception: it finds the frame on whichever
 * stack the interrupted code was using, and hands frame, EXC_RETURN and
 * IPSR to C. It returns by popping EXC_RETURN into the pc, which is the
 * architecture's only way back.
 */
    .syntax unified
    .thumb

    .text
    .thumb_func
    .global sys_exc_entry
sys_exc_entry:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    mov     r1, lr
    mrs     r2, ipsr
    push    {r4, lr}
    bl      sys_handler
    pop     {r4, pc}

/*
 * Instructions a test needs at a known address, each followed by the
 * label the fault handler resumes at. The handler is told where by the
 * test, never by guessing the instruction's length.
 */
    .thumb_func
    .global sys_do_udf
sys_do_udf:
    udf     #7
    .global sys_after_udf
sys_after_udf:
    bx      lr

    .thumb_func
    .global sys_do_svc
sys_do_svc:
    svc     #0x42
    .global sys_after_svc
sys_after_svc:
    bx      lr

    /* r0 = address; loads a word from it. */
    .thumb_func
    .global sys_do_load
sys_do_load:
    .global sys_load_insn
sys_load_insn:
    ldr     r0, [r0]
    .global sys_after_load
sys_after_load:
    bx      lr

    /* r0 = address, r1 = value; stores a word. */
    .thumb_func
    .global sys_do_store
sys_do_store:
    .global sys_store_insn
sys_store_insn:
    str     r1, [r0]
    .global sys_after_store
sys_after_store:
    bx      lr

    /* r0 = address; LDRD from it, which must be word aligned. */
    .thumb_func
    .global sys_do_ldrd
sys_do_ldrd:
    .global sys_ldrd_insn
sys_ldrd_insn:
    ldrd    r2, r3, [r0]
    .global sys_after_ldrd
sys_after_ldrd:
    bx      lr

    /* r0 / r1, signed. */
    .thumb_func
    .global sys_do_sdiv
sys_do_sdiv:
    .global sys_sdiv_insn
sys_sdiv_insn:
    sdiv    r0, r0, r1
    .global sys_after_sdiv
sys_after_sdiv:
    bx      lr

    /* Branch to r0 exactly as given: bit 0 is the Thumb bit. */
    .thumb_func
    .global sys_do_bx
sys_do_bx:
    bx      r0

    /*
     * LDREX, then SVC, then STREX to the same address: does taking an
     * exception clear the monitor? Returns the STREX status.
     */
    .thumb_func
    .global sys_excl_across_svc
sys_excl_across_svc:
    push    {r4, lr}
    mov     r4, r0
    ldrex   r1, [r4]
    svc     #0x43
    strex   r0, r1, [r4]
    pop     {r4, pc}

    /*
     * Run fn (r0) on the process stack whose top is r2, unprivileged if
     * r1 says so. The process stack has to be its own memory: a handler
     * pushes onto the *main* stack, and if the two started at the same
     * address the handler's frames would land on the thread's locals.
     */
    .thumb_func
    .global sys_thread_on_psp
sys_thread_on_psp:
    push    {r4-r7, lr}
    mov     r4, r0
    mrs     r5, control
    msr     psp, r2
    orr     r2, r5, #2          /* SPSEL */
    orr     r2, r2, r1          /* nPRIV, if asked */
    msr     control, r2
    isb
    blx     r4
    /* Back to privileged: only an exception can do that. */
    svc     #0x44
    msr     control, r5
    isb
    pop     {r4-r7, pc}

    /* Read the current sp and CONTROL, for a test running on the PSP. */
    .thumb_func
    .global sys_read_sp
sys_read_sp:
    mov     r0, sp
    bx      lr

    /* Misalign sp by 4, pend PendSV, take it, restore: r0 = ICSR addr. */
    .thumb_func
    .global sys_pendsv_misaligned
sys_pendsv_misaligned:
    push    {r4, lr}
    mov     r4, sp
    sub     sp, sp, #4
    mov     r1, #(1 << 28)
    str     r1, [r0]
    dsb
    isb
    mov     r0, sp
    sub     r0, r4, r0          /* 4 if sp came back exactly */
    add     sp, sp, #4
    pop     {r4, pc}
