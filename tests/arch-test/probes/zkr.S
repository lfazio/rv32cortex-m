/* SPDX-License-Identifier: Apache-2.0 */
/*
 * zkr.S - who may poll `seed`, with which instruction, and what it reads.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32ima_zicsr_zkr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 a1
 *
 *   scripts/run-probe.py tests/arch-test/probes/zkr.S
 *
 * Zkr's rules, each from the side that refuses: `seed` only through an
 * instruction that writes it (CSRRS/CSRRC with x0 and the immediate
 * forms with 0 are read-only, so illegal); M always, S only with
 * mseccfg.SSEED, U only with mseccfg.USEED; mseccfg itself M's alone.
 * No suite asks any of it -- ACT generates a Zkr test only for some
 * configurations, and none here had one.
 *
 * The bits are each model's own entropy, so only what the specification
 * fixes is compared: OPST, polled past WAIT (at most 64 times), and the
 * reserved and custom bits, which read zero.
 *
 *   s1  cases that raised illegal instruction, a bit each  0x00076c3c
 *   s2  cases that trapped for anything else               0
 *   s3  the top nibble of six permitted reads: ES16 is 8   0x00888888
 *   s4  their bits [29:16] ORed                            0
 *   s5  mseccfg after writing USEED and SSEED              0x00000300
 *   s6  mseccfg after writing 0                            0
 *   s7  mseccfgh                                           0
 *   a1  set once every case has run                        19
 *
 *   case  mode  mseccfg  access                      illegal
 *     1   M     0        csrrw  t0, seed, x0
 *     2   M     0        csrrs  t0, seed, x0         yes
 *     3   M     0        csrrsi t0, seed, 0          yes
 *     4   M     0        csrrc  t0, seed, x0         yes
 *     5   M     0        csrrci t0, seed, 0          yes
 *     6   M     0        csrrs  t0, seed, t6 (1)
 *     7   M     0        csrrw  x0, seed, t6
 *     8   M     0        csrrwi t0, seed, 0
 *     9   M     0        csrrc  t0, seed, t6
 *    10   S     0        csrrw  t0, seed, x0         yes
 *    11   S     0        csrr   t0, mseccfg          yes
 *    12   S     SSEED    csrrw  t0, seed, x0
 *    13   S     SSEED    csrrs  t0, seed, x0         yes
 *    14   U     SSEED    csrrw  t0, seed, x0         yes
 *    15   U     both     csrrw  t0, seed, x0
 *    16   U     both     csrrsi t0, seed, 0          yes
 *    17   S     USEED    csrrw  t0, seed, x0         yes
 *    18   S     0        csrrw  x0, seed, t6         yes
 *
 * The permitted reads are cases 1, 6, 8, 9, 12 and 15; cases 7 and 18
 * read nothing, and 18 is refused all the same: the write is an access. CSR numbers are written out (seed 0x015, mseccfg 0x747,
 * mseccfgh 0x757): the assembler need not know Zkr.
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
    li   s1, 0
    li   s2, 0
    li   s3, 0
    li   s4, 0
    li   a1, 0
    li   t6, 1

/* A case: s0 its number, s11 where a trap resumes. */
.macro C n, insn:vararg
    li   s0, \n
    la   s11, 1f
    \insn
1:
.endm

/* A permitted read into t0, polled past WAIT; its top nibble and bits
 * [29:16] recorded. A trap records an F nibble and the bits. */
.macro SEED n, insn:vararg
    li   s0, \n
    li   t3, 64
1:  la   s11, 2f
    \insn
    srli t1, t0, 30
    addi t1, t1, -1             # OPST WAIT is 1
    bnez t1, 3f
    addi t3, t3, -1
    bnez t3, 1b
    j    3f
2:  li   t0, -1
3:  srli t1, t0, 28
    slli s3, s3, 4
    or   s3, s3, t1
    li   t1, 0x3fff0000
    and  t1, t0, t1
    or   s4, s4, t1
.endm

/* From M to mode m (1 S, 0 U), carrying on at the next line. */
.macro TO m
    li   t2, 3 << 11
    csrc mstatus, t2
    li   t2, \m << 11
    csrs mstatus, t2
    la   t2, 1f
    csrw mepc, t2
    mret
1:
.endm

/* Back to M: an ecall the handler turns into a return to M. */
.macro TOM
    la   s11, 1f
    ecall
1:
.endm

    SEED 1, csrrw t0, 0x015, x0
    C 2, csrrs t0, 0x015, x0
    C 3, csrrsi t0, 0x015, 0
    C 4, csrrc t0, 0x015, x0
    C 5, csrrci t0, 0x015, 0
    SEED 6, csrrs t0, 0x015, t6
    C 7, csrrw x0, 0x015, t6
    SEED 8, csrrwi t0, 0x015, 0
    SEED 9, csrrc t0, 0x015, t6

    li   t0, 0x300
    csrw 0x747, t0
    csrr s5, 0x747
    csrw 0x747, zero
    csrr s6, 0x747
    csrr s7, 0x757

    TO 1
    C 10, csrrw t0, 0x015, x0
    C 11, csrr t0, 0x747
    C 18, csrrw x0, 0x015, t6
    TOM
    li   t0, 0x200              # SSEED
    csrw 0x747, t0
    TO 1
    SEED 12, csrrw t0, 0x015, x0
    C 13, csrrs t0, 0x015, x0
    TOM
    TO 0
    C 14, csrrw t0, 0x015, x0
    TOM
    li   t0, 0x300              # both
    csrw 0x747, t0
    TO 0
    SEED 15, csrrw t0, 0x015, x0
    C 16, csrrsi t0, 0x015, 0
    TOM
    li   t0, 0x100              # USEED alone
    csrw 0x747, t0
    TO 1
    C 17, csrrw t0, 0x015, x0
    TOM
    li   a1, 19

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

# An ecall from S or U returns to M at s11. Anything else sets case s0's
# bit -- in s1 for illegal instruction, in s2 otherwise -- and resumes at
# s11 in the mode it came from.
    .align 2
mtrap:
    csrr t4, mcause
    addi t2, t4, -8             # ecall from U
    beqz t2, 6f
    addi t2, t4, -9             # ecall from S
    beqz t2, 6f
    li   t2, 1
    sll  t2, t2, s0
    addi t5, t4, -2
    bnez t5, 5f
    or   s1, s1, t2
    j    7f
5:  or   s2, s2, t2
7:  csrw mepc, s11
    mret
6:  li   t2, 3 << 11
    csrs mstatus, t2
    csrw mepc, s11
    mret

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
