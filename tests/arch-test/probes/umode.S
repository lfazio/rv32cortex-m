/* SPDX-License-Identifier: Apache-2.0 */
/*
 * umode.S - what U-mode may not execute, and what the trap reports.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imac_zicsr_svinval
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 a0 a1 a2
 *
 *   scripts/run-probe.py tests/arch-test/probes/umode.S
 *
 * Every privileged instruction from U-mode, with TW, TSR and TVM all
 * clear -- so each trap is the instruction's own rule and not a control
 * bit's. Both suites passed while `mret` ran in U-mode, returning to
 * whatever mode MPP named, and isatest never went below S -- so none of
 * this had been asked.
 *
 * Nothing is delegated; M-mode files mcause and mtval by case number and
 * resumes at a label. The answers:
 *
 *   s1  causes 1-8, a nibble each                    0x22222222
 *   s2  causes 9-10                                  0x00000028
 *
 *   case                                            mtval
 *   s3   1  mret                                     0x30200073
 *   s4   2  sret                                     0x10200073
 *   s5   3  wfi, TW clear                            0x10500073
 *   s6   4  sfence.vma                               0x12000073
 *   s7   5  csrr t0, sstatus                         0x100022f3
 *   s8   6  csrr t0, mstatus                         0x300022f3
 *   s9   7  sinval.vma                               0x16000073
 *   s10  8  sfence.w.inval                           0x18000073
 *   s11  9  sfence.inval.ir                          0x18100073
 *   a0  10  ecall, which carries nothing             0x00000000
 *   a1  11, set once every case has run
 *   a2  traps taken from a mode other than U         0
 *
 * Case 3 is the one a reading can get wrong: with TW clear, WFI is
 * still illegal in U-mode once S-mode exists, unless it completes
 * within a bound -- and a WFI that parks until an interrupt has none.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    li   t0, -1
    csrw pmpaddr0, t0
    li   t0, 0x1f
    csrw pmpcfg0, t0
    csrw medeleg, zero
    li   t0, (1 << 20) | (1 << 21) | (1 << 22)  # TVM, TW, TSR: clear
    csrc mstatus, t0

    la   t0, tbl
    li   t1, 0xdeadbee0
    li   t2, 12 * 4
1:  sw   t1, 0(t0)
    addi t0, t0, 4
    addi t2, t2, -1
    bnez t2, 1b

    li   a2, 0
    li   t0, 3 << 11            # MPP = U
    csrc mstatus, t0
    la   t0, umain
    csrw mepc, t0
    mret

.macro T n, insn:vararg
    li   s0, \n
    la   s11, 1f
    \insn
1:
.endm

umain:
    T 1, mret
    T 2, sret
    T 3, wfi
    T 4, sfence.vma
    T 5, .word 0x100022f3       # csrr t0, sstatus
    T 6, .word 0x300022f3       # csrr t0, mstatus
    T 7, .word 0x16000073       # sinval.vma zero, zero
    T 8, .word 0x18000073       # sfence.w.inval
    T 9, .word 0x18100073       # sfence.inval.ir
    T 10, ecall
    li   a1, 11

    la   t0, tbl + 16
    li   s1, 0
    li   t2, 8
2:  lw   t1, 0(t0)
    andi t1, t1, 15
    slli s1, s1, 4
    or   s1, s1, t1
    addi t0, t0, 16
    addi t2, t2, -1
    bnez t2, 2b
    li   s2, 0
    li   t2, 2
3:  lw   t1, 0(t0)
    andi t1, t1, 15
    slli s2, s2, 4
    or   s2, s2, t1
    addi t0, t0, 16
    addi t2, t2, -1
    bnez t2, 3b

    la   t0, tbl
    lw   s3, 1 * 16 + 4(t0)
    lw   s4, 2 * 16 + 4(t0)
    lw   s5, 3 * 16 + 4(t0)
    lw   s6, 4 * 16 + 4(t0)
    lw   s7, 5 * 16 + 4(t0)
    lw   s8, 6 * 16 + 4(t0)
    lw   s9, 7 * 16 + 4(t0)
    lw   s10, 8 * 16 + 4(t0)
    lw   s11, 9 * 16 + 4(t0)
    lw   a0, 10 * 16 + 4(t0)

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

# M-mode: file mcause and mtval under the case in s0 and resume at s11
# in U-mode. A trap from any other mode is counted in a2 and resumed the
# same way, in U-mode, so the run carries on and says so.
    .align 2
mtrap:
    csrr t4, mcause
    csrr t5, mtval
    la   t3, tbl
    slli t2, s0, 4
    add  t3, t3, t2
    sw   t4, 0(t3)
    sw   t5, 4(t3)
    csrr t6, mstatus
    li   t2, 3 << 11
    and  t2, t6, t2
    beqz t2, 5f
    addi a2, a2, 1
    li   t2, 3 << 11
    csrc mstatus, t2
5:  csrw mepc, s11
    mret

    .data
    .align 4
tbl:    .fill 12 * 4, 4, 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
