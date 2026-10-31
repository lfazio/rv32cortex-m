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

    /* r0 = address; loads a halfword from it. */
    .thumb_func
    .global sys_do_loadh
sys_do_loadh:
    .global sys_loadh_insn
sys_loadh_insn:
    ldrh    r0, [r0]
    .global sys_after_loadh
sys_after_loadh:
    bx      lr

    /* r0 = address, r1 = value; stores a halfword. */
    .thumb_func
    .global sys_do_storeh
sys_do_storeh:
    .global sys_storeh_insn
sys_storeh_insn:
    strh    r1, [r0]
    .global sys_after_storeh
sys_after_storeh:
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

/*
 * SVCall handlers that each return by one pc-writer, the value being
 * lr's EXC_RETURN: does that instruction make an exception return in
 * Handler mode? The manual names BX, LDR, POP and LDM; MOV and ADD to the
 * pc go through ALUWritePC, a plain branch that clears bit 0, and BLX
 * through BLXWritePC. Each keeps EXC_RETURN in r1, so that a fault
 * handler can send it on to sys_excret_resume, which returns as a
 * handler should whatever the instruction did to lr.
 */
    .thumb_func
    .global sys_svc_ret_bx
sys_svc_ret_bx:
    mov     r1, lr
    bx      lr

    .thumb_func
    .global sys_svc_ret_ldr
sys_svc_ret_ldr:
    mov     r1, lr
    str     lr, [sp, #-8]!
    ldr     pc, [sp], #8

    .thumb_func
    .global sys_svc_ret_pop
sys_svc_ret_pop:
    mov     r1, lr
    push    {r0, lr}
    pop     {r0, pc}

    .thumb_func
    .global sys_svc_ret_mov
sys_svc_ret_mov:
    mov     r1, lr
    mov     pc, lr

    /* ADD pc, Rm: pc reads as this add's address + 4. */
    .thumb_func
    .global sys_svc_ret_add
sys_svc_ret_add:
    mov     r1, lr
    mov     r2, pc              /* r2 = (address of this mov) + 4      */
    sub.w   r0, lr, r2
    subs    r0, #8              /* the add is 8 bytes past that mov    */
    add     pc, r0              /* pc = (mov + 8) + 4 + r0 = lr        */

    .thumb_func
    .global sys_svc_ret_blx
sys_svc_ret_blx:
    mov     r1, lr
    blx     r1

    .thumb_func
    .global sys_excret_resume
sys_excret_resume:
    bx      r1

/* As sys_svc_ret_mov, to the address in sys_mov_target instead of lr. */
    .thumb_func
    .global sys_svc_mov_target
sys_svc_mov_target:
    mov     r1, lr
    ldr     r0, =sys_mov_target
    ldr     r0, [r0]
    mov     pc, r0
    .ltorg

/*
 * An exception entry onto a process stack the test chose, to see what
 * a stacking fault does. r0 = the value for the PSP; r1 = 0 to enter by
 * SVC, else ICSR's address, and PendSV is pended; r2 = 1 to run the
 * entry unprivileged. Thread mode on the PSP with FPCA clear, so the
 * frame is the basic eight words, and r0-r3, r12 and lr hold markers so
 * the words that landed can be told apart. Whatever handler takes the
 * fault leaves a frame of its own on the PSP whose pc is
 * sys_after_stacking, and privilege back; this puts CONTROL and the PSP
 * back as they were.
 */
    .thumb_func
    .global sys_stacking
sys_stacking:
    push    {r4-r7, lr}
    mov     r4, r1
    mrs     r5, control
    mrs     r6, psp
    msr     psp, r0
    bic     r3, r5, #5          /* FPCA off: a basic frame; nPRIV off */
    orr     r3, r3, #2          /* SPSEL: the PSP */
    orr     r3, r3, r2          /* nPRIV, if asked */
    msr     control, r3
    isb
    mov     r7, #(1 << 28)      /* ICSR.PENDSVSET */
    ldr     r0, =0xA0A0A0A0
    ldr     r1, =0xA1A1A1A1
    ldr     r2, =0xA2A2A2A2
    ldr     r3, =0xA3A3A3A3
    ldr     r12, =0xACACACAC
    ldr     lr, =0xAEAEAEAE
    cbz     r4, 1f
    str     r7, [r4]
    dsb
    isb
    b       2f
1:  svc     #0x45
    .thumb_func
    .global sys_after_stacking
sys_after_stacking:
2:
    msr     control, r5
    isb
    msr     psp, r6
    pop     {r4-r7, pc}
    .ltorg

/*
 * Where a frame sys_stacking's handler built resumes when the return
 * that pops it leaves the thread unprivileged: a stack that is the
 * thread's own, then SVC #0x46 for privilege back, then the way out.
 */
    .thumb_func
    .global sys_unpriv_land
sys_unpriv_land:
    ldr     r0, =sys_land_sp
    ldr     r0, [r0]
    mov     sp, r0
    svc     #0x46
    .global sys_land_after
sys_land_after:
    b       sys_after_stacking
    .ltorg
