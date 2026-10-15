/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sdtrig.S - when a breakpoint trigger may fire, by mode and enable.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: spike
 * probe-spike-isa: rv32ima_zicsr
 * probe-regs: s2 s3 s4 s5 s6 s7 s8 s9
 *
 *   scripts/run-probe.py tests/arch-test/probes/sdtrig.S
 *
 * A trigger whose action is a breakpoint can fire inside the handler of
 * its own breakpoint and overwrite the cause and epc it was entered
 * with. The debug specification (1.0, section 5.4; docs/riscv) offers
 * two cures and a hart "should implement one". The second needs
 * medeleg[3] hard-wired to 0, which a core that delegates breakpoints
 * to S cannot have, so this core takes the first:
 *
 *   no action-0 trigger matches in M-mode while mstatus.MIE is 0, nor
 *   in S-mode while medeleg[3] is 1 and sstatus.SIE is 0.
 *
 * Sail has no Sdtrig. Spike implements the first cure, so it is the
 * reference. Each case arms an mcontrol load trigger on one word for the
 * mode under test, loads it, and records what happened: 0 nothing,
 * 0x103 a breakpoint taken to M, 0x203 one taken to S.
 *
 *   s2  M, MIE 0                            0
 *   s3  M, MIE 1                            0x103
 *   s4  S, breakpoints to M, SIE 0          0x103
 *   s5  S, breakpoints to S, SIE 0          0
 *   s6  S, breakpoints to S, SIE 1          0x203
 *   s7  U, breakpoints to S, SIE 0          0x203: the rule is S-mode's
 *   s8  case 5: stval, the trigger's address  word
 *   s9  the last case reached               6
 *
 * Spike (f8a060b) gives all eight; so does the emulator on both
 * backends.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    la   t0, strap
    csrw stvec, t0
    li   t0, -1
    csrw pmpaddr0, t0
    li   t0, 0x1f
    csrw pmpcfg0, t0
    csrw mie, zero              # nothing to take when MIE is set
    csrw medeleg, zero
    la   s10, word
    li   s9, 0
    csrw tselect, zero
    csrw tdata2, s10

    # 1, 2: M-mode, a trigger for M.
    li   t0, 0x20000041         # mcontrol: m, load
    csrw tdata1, t0
    li   a7, 0
    lw   t0, 0(s10)
    mv   s2, a7
    li   a7, 0
    csrsi mstatus, 8
    lw   t0, 0(s10)
    csrci mstatus, 8
    mv   s3, a7
    li   s9, 2

    # 3-5: S-mode, a trigger for S.
    li   t0, 0x20000011         # mcontrol: s, load
    csrw tdata1, t0
    li   a7, 0
    la   s11, 1f
    la   t2, sload
    li   t3, 1 << 11
    j    to_mode
1:  mv   s4, a7
    li   s9, 3

    li   t0, 1 << 3
    csrw medeleg, t0
    li   a7, 0
    la   s11, 1f
    la   t2, sload
    li   t3, 1 << 11
    j    to_mode
1:  mv   s5, a7
    li   s9, 4

    li   a7, 0
    csrsi mstatus, 2            # SIE
    la   s11, 1f
    la   t2, sload
    li   t3, 1 << 11
    j    to_mode
1:  csrci mstatus, 2
    mv   s6, a7
    mv   s8, a6
    li   s9, 5

    # 6: U-mode, a trigger for U, breakpoints still to S, SIE 0.
    li   t0, 0x20000009         # mcontrol: u, load
    csrw tdata1, t0
    li   a7, 0
    la   s11, 1f
    la   t2, sload
    li   t3, 0
    j    to_mode
1:  mv   s7, a7
    li   s9, 6

    csrw tdata1, zero
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
2:  j    2b

# mret to t2 in the mode t3 names (MPP).
to_mode:
    li   t0, 3 << 11
    csrc mstatus, t0
    csrs mstatus, t3
    csrw mepc, t2
    mret

# Below M: the load under test, then back to M by ecall.
sload:
    lw   t0, 0(s10)
    ecall

    .align 2
strap:
    csrr a7, scause
    ori  a7, a7, 0x200
    csrr a6, stval
    csrr t6, sepc
    addi t6, t6, 4
    csrw sepc, t6
    sret

    .align 2
mtrap:
    csrr t6, mcause
    addi t5, t6, -8
    srli t5, t5, 1
    beqz t5, 3f                 # ecall from U or S: back to M at s11
    ori  a7, t6, 0x100
    csrr a6, mtval
    csrr t6, mepc
    addi t6, t6, 4
    csrw mepc, t6
    mret
3:  li   t6, 3 << 11
    csrs mstatus, t6
    csrw mepc, s11
    mret

    .data
    .align 4
word:   .word 0x12345678
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
