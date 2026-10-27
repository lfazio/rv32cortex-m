/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pmpstraddle.S - an M-mode access half inside an unlocked PMP entry.
 *
 * probe-march: rv32imafd_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imafdc_zicsr
 * probe-regs: a0 a1 a2 a3 a4 a5
 *
 *   scripts/run-probe.py tests/arch-test/probes/pmpstraddle.S
 *
 * The privileged spec's matching rule comes before its M-mode rule:
 * the lowest entry matching any byte decides, and "must match all bytes
 * of an access, or the access fails, irrespective of the L, R, W, and X
 * bits". Only an entry holding all of it is then exempt for M when its
 * L is clear. This core skipped PMP in M-mode whenever no entry was
 * locked -- an unlocked entry restricts nothing there -- so an access
 * straddling one succeeded. fuzz-rv32-mmu's reference, written from the
 * spec, said it fails.
 *
 * An eight-byte FLD and FSD, aligned, so neither model needs misaligned
 * support: entry 0 is NA4 over the upper word of `buf`, unlocked, RW;
 * entry 1 grants everything, unlocked. M-mode throughout, nothing
 * locked. The handler files mcause by case and steps over.
 *
 *   a0  1  fld over buf, half in entry 0              5  load access fault
 *   a1  2  fsd over buf                               7  store access fault
 *   a2  3  fld over buf + 8, in entry 1 alone         0xdeadbee0: no trap
 *   a3  4  lw of the word entry 0 holds               0xdeadbee0
 *   a4     traps in all                               2
 *   a5  5  the same fld, entry 0 now locked RW        5
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    li   t0, 0x2000                 # FS = Initial
    csrs mstatus, t0

    la   s1, tbl
    li   t1, 0xdeadbee0
    li   t2, 8
    mv   t0, s1
1:  sw   t1, 0(t0)
    addi t0, t0, 4
    addi t2, t2, -1
    bnez t2, 1b
    li   gp, 0

    # entry 0: NA4 over buf + 4, R W, unlocked; entry 1: everything
    la   t0, buf
    addi t0, t0, 4
    srli t0, t0, 2
    csrw pmpaddr0, t0
    li   t0, -1
    csrw pmpaddr1, t0
    li   t0, (0x1f << 8) | 0x13
    csrw pmpcfg0, t0

    la   s2, buf
    # case 1
    li   s0, 0
    fld  f0, 0(s2)
    # case 2
    li   s0, 1
    fsd  f0, 0(s2)
    # case 3
    li   s0, 2
    fld  f1, 8(s2)
    # case 4
    li   s0, 3
    lw   t1, 4(s2)
    mv   a4, gp
    # case 5: lock entry 0, R W
    li   t0, (0x1f << 8) | 0x93
    csrw pmpcfg0, t0
    li   s0, 4
    fld  f0, 0(s2)

    lw   a0, 0(s1)
    lw   a1, 4(s1)
    lw   a2, 8(s1)
    lw   a3, 12(s1)
    lw   a5, 16(s1)

    la   s0, tohost
    li   t0, 1
    sw   t0, 0(s0)
5:  j    5b

    .align 2
mtrap:
    csrr t6, mcause
    slli t5, s0, 2
    add  t5, s1, t5
    sw   t6, 0(t5)
    addi gp, gp, 1
    csrr t6, mepc
    addi t6, t6, 4
    csrw mepc, t6
    mret

    .data
    .align 6
buf:    .fill 4, 4, 0x11111111
    .align 4
tbl:    .fill 8, 4, 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
