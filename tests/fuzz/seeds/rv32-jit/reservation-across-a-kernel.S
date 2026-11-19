/*
 * A reservation, then strcpy's byte loop over the reserved word -- a
 * block the JIT gives a kernel (emu_ir_kernel.c) -- then the
 * store-conditional. A store here breaks the reservation, and the
 * inlined stores' fast path is declined while one is held (resv_valid,
 * the RAM window's store guard): the kernel must decline too, or the SC
 * succeeds under the JIT alone. fuzz-ir's model target cannot see this:
 * its slow-path store is the same write as the fast path's.
 */
    li      a0, 0x80003000      /* the destination */
    li      a1, 0x80003100      /* the source: eight bytes and a NUL */
    li      t0, 0x41424344
    sw      t0, 0(a1)
    sw      t0, 4(a1)
    sw      zero, 8(a1)
    /*
     * The reserved word is the destination's second: the loop's first
     * iteration runs in the block before the one that loops (and has the
     * kernel), and a reservation on the first word would be broken there.
     */
    addi    a2, a0, 4
    lr.w    t1, (a2)
    mv      a5, a0
1:  lbu     a4, 0(a1)
    addi    a1, a1, 1
    addi    a5, a5, 1
    sb      a4, -1(a5)
    bnez    a4, 1b
    li      t2, 7
    sc.w    t3, t2, (a2)        /* fails: t3 is 1 */
    ecall
