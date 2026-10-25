/* SPDX-License-Identifier: Apache-2.0 */
/*
 * instret.S - does an instruction that traps retire?
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32ima_zicsr_zicntr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8
 * probe-permitted: sail s4 s6 s8
 *
 *   scripts/run-probe.py tests/arch-test/probes/instret.S
 *
 * The fuzzer that runs the interpreter against the JIT found them
 * disagreeing on its first faulting load: the interpreter counted it in
 * minstret and mcycle, the JIT did not. This asks the models.
 *
 * Each case reads the counter, runs one instruction, and reads it
 * again. A trap goes to a four-instruction handler that steps over the
 * instruction, so a delta is
 *
 *   1 (the first read) + 1 (the instruction, if it retires)
 *   + 4 (the handler, if it trapped)
 *
 * The answers, Sail and Spike alike for minstret:
 *
 *   reg    case                     minstret   mcycle
 *   s1/s2  nop                          2         2
 *   s3/s4  illegal instruction          5         5 (Sail 6)
 *   s5/s6  ebreak                       5         5 (Sail 6)
 *   s7/s8  load access fault            5         5 (Sail 6)
 *
 * So a trapping instruction does not retire. The interpreter said 6
 * for all six until this probe; the JIT, which counts only what a block
 * retired, said 5.
 *
 * mcycle is asked too because the interpreter advanced both together.
 * Cycles are the implementation's to define, and the two models differ:
 * Sail charges the trap a cycle and Spike does not. This emulator has
 * no cycle model -- mcycle follows minstret, on both backends, which
 * is Spike's answer -- so Sail's is the permitted difference.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    csrw mcountinhibit, zero

    csrr t1, minstret
    nop
    csrr t2, minstret
    sub  s1, t2, t1
    csrr t1, mcycle
    nop
    csrr t2, mcycle
    sub  s2, t2, t1

    csrr t1, minstret
    .word 0                 # illegal
    csrr t2, minstret
    sub  s3, t2, t1
    csrr t1, mcycle
    .word 0
    csrr t2, mcycle
    sub  s4, t2, t1

    csrr t1, minstret
    ebreak
    csrr t2, minstret
    sub  s5, t2, t1
    csrr t1, mcycle
    ebreak
    csrr t2, mcycle
    sub  s6, t2, t1

    li   t3, 0xF0000000     # nothing there in any of the three
    csrr t1, minstret
    lw   t4, 0(t3)
    csrr t2, minstret
    sub  s7, t2, t1
    csrr t1, mcycle
    lw   t4, 0(t3)
    csrr t2, mcycle
    sub  s8, t2, t1

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

# Step over the instruction that trapped: four instructions, each of
# which retires.
    .align 2
mtrap:
    csrr t5, mepc
    addi t5, t5, 4
    csrw mepc, t5
    mret

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
