/* SPDX-License-Identifier: Apache-2.0 */
/*
 * zawrs.S - what the Sail model does with WRS.NTO and WRS.STO.
 *
 * riscv-arch-test has no Zawrs suite, so there is nothing that compares
 * this emulator with the golden model on it. This asks the model
 * directly, seven cases, and the trace is the answer:
 *
 *   riscv64-unknown-elf-gcc -march=rv32ima_zicsr_zawrs -mabi=ilp32 \
 *       -nostdlib -T tests/arch-test/rv32cortex-m-rv32/link.ld \
 *       tests/arch-test/probes/zawrs.S -o zawrs.elf
 *   build/sail/bin/sail_riscv_sim \
 *       --config tests/arch-test/rv32cortex-m-rv32/sail.json \
 *       --trace-instr --trace-exception --trace-gpr --use-abi-names \
 *       --inst-limit 400 --trace-output zawrs.trace zawrs.elf
 *   grep -E "wrs|trapping|s[1237] <- " zawrs.trace
 *
 * s1 numbers the case most recently *passed*; s7 collects, a nibble
 * each, the s1 at which every illegal-instruction trap was taken.
 *
 * Sail 0.13.1, with Zawrs supported and neither form a no-op:
 *
 *   M, nothing reserved, nto      completes
 *   M, sto                        completes
 *   M, reserved, sto              completes, reservation survives (s2 = 0)
 *   M, reserved, nto, no irq      completes, reservation survives (s3 = 0)
 *   S + TW, nothing reserved, nto completes      <- TW is about waiting
 *   S + TW, reserved, sto         completes
 *   S + TW, reserved, nto         illegal instruction (s7 = 7)
 *
 * The emulator agrees on six. On the fourth it parks the hart until an
 * interrupt, as WFI does, where the model carries on; the specification
 * permits both ("may end the stall for any reason"), and that case
 * cannot be put in a self-checking test without one of the two hanging.
 *
 * To see the six on the emulator, leave the fourth out and dump:
 *
 *   riscv64-unknown-elf-gcc -DSKIP_PARKING_CASE ... -o zawrs-emu.elf
 *   build/host/emu-host --ram 0x800000 --max-insn 100000 --dump \
 *       zawrs-emu.elf
 *
 * s1 = 8 says all seven lines were reached, s6 = 2 that two traps were
 * taken (the illegal instruction and the closing ecall), s7 = 7 where,
 * and s2 = 0 that the reservation survived WRS.STO. The run ends at the
 * instruction cap, not by exiting: `tohost` is the model's way of
 * stopping and means nothing to this runner. Without the define it ends
 * after 22 instructions instead, at the fourth case, with the hart
 * parked and nothing enabled to wake it -- which is the difference.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, trap
    csrw mtvec, t0
    li   t0, -1
    csrw pmpaddr0, t0
    li   t0, 0x1f
    csrw pmpcfg0, t0
    la   a0, data
    li   s6, 0

    # 1: nto, nothing reserved
    lw   t2, 0(a0)
    sc.w t1, t2, (a0)
    wrs.nto
    li   s1, 1
    # 2: sto, nothing reserved
    wrs.sto
    li   s1, 2
    # 3: does a reservation survive wrs.sto?  s2 == 0 if so
    lr.w t1, (a0)
    wrs.sto
    sc.w s2, t1, (a0)
    li   s1, 3
    # 4: M-mode, reserved, nto, no interrupt enabled.  s3 == 0 if survived
#ifndef SKIP_PARKING_CASE
    lr.w t1, (a0)
    wrs.nto
    sc.w s3, t1, (a0)
#endif
    li   s1, 4

    # below M, TW set
    li   t0, 1 << 21
    csrs mstatus, t0
    li   t0, 3 << 11
    csrc mstatus, t0
    li   t0, 1 << 11
    csrs mstatus, t0
    la   t0, smode
    csrw mepc, t0
    mret
smode:
    lw   t2, 0(a0)
    sc.w t1, t2, (a0)
    li   s1, 5
    wrs.nto                 # S, TW, nothing reserved
    li   s1, 6
    lr.w t1, (a0)
    wrs.sto                 # S, TW, reserved, sto
    li   s1, 7
    lr.w t1, (a0)
    wrs.nto                 # S, TW, reserved, nto
    li   s1, 8
    ecall

trap:
    csrr t3, mcause
    csrr t4, mepc
    csrr t5, mtval
    addi s6, s6, 1
    li   t6, 9
    beq  t3, t6, done       # ecall from S
    slli s7, s7, 4          # record where each illegal happened: s1 nibble
    or   s7, s7, s1
    addi t4, t4, 4
    csrw mepc, t4
    mret
done:
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
    sw   zero, 4(t0)
1:  j    1b

    .data
    .align 4
data:   .word 0x1234
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
