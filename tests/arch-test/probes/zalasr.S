/* SPDX-License-Identifier: Apache-2.0 */
/*
 * zalasr.S - what Spike does with load-acquire and store-release.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: spike
 * probe-spike-isa: rv32ima_zicsr_zalasr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 a2 a3 a4
 * probe-permitted: spike s8 -- an SC after this hart's own store
 *
 *   scripts/run-probe.py tests/arch-test/probes/zalasr.S
 *
 * Neither riscv-arch-test nor Sail (0.13.1, 0.15, nor the nightly of
 * 2026-10-12) has Zalasr, so until this there was no second reading of
 * the specification -- only isatest, written by the same hand as the
 * implementation. Spike has it. This asks it the questions isatest
 * answers, and the commit log is the answer:
 *
 *   riscv64-unknown-elf-gcc -march=rv32ima_zicsr -mabi=ilp32 \
 *       -nostdlib -T tests/arch-test/rv32cortex-m-rv32/link.ld \
 *       tests/arch-test/probes/zalasr.S -o zalasr.elf
 *   spike --isa=rv32imac_zicsr_zalasr -m0x80000000:0x800000 \
 *       -l --log-commits zalasr.elf 2>&1 | grep -E " x(1[89]|2[0-7]|1[2-6]) "
 *
 * and the same values from the emulator:
 *
 *   build/host/emu-host --ram 0x800000 --max-insn 100000 --dump \
 *       zalasr.elf
 *
 * Spike (riscv-isa-sim) is BSD-licensed and is used here only as an
 * oracle; nothing of it is in this tree.
 *
 *   s2  lb.aq  [base]                     0xffffff84
 *   s3  lh.aq  [base+2]                   0xffff8182
 *   s4  lw.aq  [base]                     0x8182f384
 *   s5  lw.aqrl [base]                    0x8182f384
 *   s6  word after sb.rl, sh.rl           0xccdd33dd
 *   s7  word after sw.rl                  0x55667788
 *   s8  sc.w after lr.w, sw.rl            1 here, 0 on Spike
 *   s9  causes, a nibble each: lh.aq +1, lw.aq +2, sh.rl +1, sw.rl +2,
 *       lw.aq and sw.rl with nothing there        0x00446657
 *   s10 causes for the eight reserved encodings   0x22222222
 *   a2  mtval of the misaligned lh.aq     base + 1
 *   a3  lh.aq's rd after it faulted       0x5a5a5a5a, untouched
 *   a4  the word after the misaligned stores      0x11223344
 *   s1  the last case reached             9
 *
 * Spike (f8a060b) agrees on twelve. **s8 is the thirteenth, and both
 * answers are allowed.** The emulator ends a reservation on any store to
 * the reserved word, its own included -- the store-release goes through
 * the atomic dispatch, and an inlined store has had to drop the
 * reservation by hand before, so the rule is one rule everywhere. Spike
 * keeps the reservation across the hart's own store and the SC
 * succeeds. The A extension requires an SC to fail after a store from
 * *another* hart and lets an implementation fail one for any other
 * reason, so neither is wrong; isatest's `sw.rl-ends-reservation`
 * checks this core's choice, not the architecture's.
 *
 * "Nothing there" is 0x70000000, which is unmapped in both: the
 * emulator's rvmodel_macros.h relies on it, and Spike maps only what
 * -m names.
 */
    .option norvc
    .option arch, +zalasr       # GCC 15 does not take it in -march
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, trap
    csrw mtvec, t0
    li   s9, 0
    li   s10, 0
    li   t6, 0                  # which nibble register the handler fills
    li   a2, 0
    la   a1, data

    # 1: loads, sign-extended at each width
    li   t0, 0x8182f384
    sw   t0, 0(a1)
    lb.aq s2, (a1)
    addi t1, a1, 2
    lh.aq s3, (t1)
    lw.aq s4, (a1)
    lw.aqrl s5, (a1)
    li   s1, 1

    # 2: stores
    li   t0, 0x11223344
    sw   t0, 0(a1)
    li   t0, 0xaabbccdd
    sb.rl t0, (a1)
    addi t1, a1, 2
    sh.rl t0, (t1)
    lw   s6, 0(a1)
    li   t0, 0x55667788
    sw.rl t0, (a1)
    lw   s7, 0(a1)
    li   s1, 2

    # 3: a store-release ends a reservation on its word
    lr.w t0, (a1)
    li   t1, 2
    sw.rl t1, (a1)
    li   t1, 3
    sc.w s8, t1, (a1)
    li   s1, 3

    # 4: misaligned -- trapped, not split, whatever ordinary accesses do
    li   t0, 0x11223344
    sw   t0, 0(a1)
    sw   zero, 4(a1)
    li   a3, 0x5a5a5a5a
    addi t1, a1, 1
    lh.aq a3, (t1)
    addi t1, a1, 2
    lw.aq t2, (t1)
    addi t1, a1, 1
    li   t0, 0xaabbccdd
    sh.rl t0, (t1)
    addi t1, a1, 2
    sw.rl t0, (t1)
    lw   a4, 0(a1)
    li   s1, 4

    # 5: an address with nothing behind it
    li   t1, 0x70000000
    lw.aq t2, (t1)
    sw.rl t0, (t1)
    li   s1, 5

    # 6-9: the reserved encodings, one bit from a working instruction
    li   t6, 1
    li   t0, 0x11223344
    sw   t0, 0(a1)
    .word 0x3005a52f            # lw, no aq
    .word 0x3205a52f            # lw, rl only
    .word 0x3415a52f            # lw.aq with rs2 = 1
    .word 0x3405b52f            # ld.aq: RV64 only
    li   s1, 6
    .word 0x38c5a02f            # sw, no rl
    .word 0x3cc5a02f            # sw, aq only
    .word 0x3ac5a0af            # sw.rl with rd = 1
    .word 0x3ac5b02f            # sd.rl: RV64 only
    li   s1, 9

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
    sw   zero, 4(t0)
1:  j    1b

trap:
    csrr t3, mcause
    andi t3, t3, 0xf
    bnez t6, 2f
    slli s9, s9, 4
    or   s9, s9, t3
    bnez a2, 3f
    csrr a2, mtval              # the first one: lh.aq at base + 1
    j    3f
2:  slli s10, s10, 4
    or   s10, s10, t3
3:  csrr t4, mepc
    addi t4, t4, 4
    csrw mepc, t4
    mret

    .data
    .align 4
data:   .word 0, 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
