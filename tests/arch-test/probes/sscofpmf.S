/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sscofpmf.S - what Sail and Spike do with Sscofpmf's registers.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32ima_zicsr_zicntr_zihpm_sscofpmf
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 a2 a3 a4
 *
 *   scripts/run-probe.py tests/arch-test/probes/sscofpmf.S
 *
 * riscv-arch-test has no Sscofpmf suite, and the model counts no events,
 * so neither can say anything about an overflow. What it can answer is
 * everything around one -- which bits exist, who may see them, and where
 * the interrupt ranks -- and isatest's `sscof-*` checks cover the
 * counting. Seven cases; the trace is the answer:
 *
 *   riscv64-unknown-elf-gcc -march=rv32ima_zicsr -mabi=ilp32 \
 *       -nostdlib -T tests/arch-test/rv32cortex-m-rv32/link.ld \
 *       tests/arch-test/probes/sscofpmf.S -o sscofpmf.elf
 *   build/sail/bin/sail_riscv_sim \
 *       --config tests/arch-test/rv32cortex-m-rv32/sail.json \
 *       --trace-instr --trace-exception --trace-gpr --use-abi-names \
 *       --inst-limit 400 --trace-output sscofpmf.trace sscofpmf.elf
 *   grep -E "(s[1-9]|a[2-5]) <- " sscofpmf.trace
 *
 * Sail 0.13.1, with Sscofpmf supported and mideleg's delegatable bits
 * 0x2222:
 *
 *   s2  mhpmevent3h after writing all ones       0xf0000000
 *   s3  mideleg after writing all ones           0x00002222
 *   s4  mip.LCOFIP after M sets it               0x00002000
 *   s5  scountovf from M, OF on 3 and 7          0x00000088
 *   s6  scountovf from S, mcounteren = 1 << 7    0x00000080
 *   s7  sip from S, LCOFIP pending, delegated    0x00002000
 *   s8  sie from S after writing all ones        0x00002222
 *   s9  mip.LCOFIP after S cleared sip's         0x00000000
 *   a2  sip from S, LCOFIP pending, *not* deleg. 0x00000000
 *   a3  mip.LCOFIP after S tried to clear it     0x00002000
 *   a4  the interrupt taken, STIP and LCOFIP     0x80000005
 *
 * Spike (f8a060b) gives the same eleven -- once Zicntr and Zihpm are in
 * its --isa, without which it makes mcounteren's counter bits read-only
 * zero and S-mode's scountovf reads 0. So does the emulator; to see them:
 *
 *   build/host/emu-host --ram 0x800000 --max-insn 100000 --dump \
 *       sscofpmf.elf
 *
 * s1 = 7 says every case was reached and s10 counts the traps: two
 * ecalls from S and the one interrupt, 3. The run ends at the
 * instruction cap: `tohost` is the model's way of stopping and means
 * nothing to this runner.
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
    li   s10, 0

    # 1: which bits of mhpmevent3h exist
    li   t0, -1
    csrw 0x723, t0
    csrr s2, 0x723
    li   s1, 1
    # 2: which interrupts may be delegated
    csrw mideleg, t0
    csrr s3, mideleg
    li   s1, 2
    # 3: M may pend the overflow interrupt itself
    li   t1, 0x2000
    csrs mip, t1
    csrr t4, mip
    and  s4, t4, t1         # not MTIP: the model's mtimecmp resets to 0
    li   s1, 3
    # 4: scountovf from M sees every OF
    li   t2, 0x80000000
    csrw 0x723, t2
    csrw 0x727, t2
    csrr s5, 0xda0
    li   s1, 4

    # 5: from S, mcounteren gates scountovf, and a delegated LCOFIP is
    #    S's to see and to clear. mideleg is 0x2222 from case 2.
    li   t2, 0x80
    csrw mcounteren, t2
    la   s11, back5
    la   t2, smode5
    call to_s
smode5:
    csrr s6, 0xda0
    csrr s7, sip
    li   t0, -1
    csrw sie, t0
    csrr s8, sie
    li   t1, 0x2000
    csrc sip, t1
    ecall
back5:
    csrr t4, mip
    li   t1, 0x2000
    and  s9, t4, t1
    li   s1, 5

    # 6: not delegated, S sees nothing and its write is dropped
    csrw mideleg, zero
    csrw mie, zero          # case 5's sie write set LCOFIE
    li   t1, 0x2000
    csrs mip, t1
    la   s11, back6
    la   t2, smode6
    call to_s
smode6:
    csrr a2, sip
    li   t1, 0x2000
    csrc sip, t1
    ecall
back6:
    csrr t4, mip
    li   t1, 0x2000
    and  a3, t4, t1
    li   s1, 6

    # 7: STI against LCOFI, both for M: which is taken first
    li   t1, 0x2020
    csrs mip, t1
    csrw mie, t1
    csrsi mstatus, 8
    nop
    nop
    csrci mstatus, 8
    li   s1, 7

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
    sw   zero, 4(t0)
1:  j    1b

# mret to t2 in S-mode.
to_s:
    li   t0, 3 << 11
    csrc mstatus, t0
    li   t0, 1 << 11
    csrs mstatus, t0
    csrw mepc, t2
    mret

trap:
    csrr t3, mcause
    addi s10, s10, 1
    bltz t3, irq
    # ecall from S: back to M at s11
    li   t0, 3 << 11
    csrs mstatus, t0
    csrw mepc, s11
    mret
irq:
    mv   a4, t3
    csrw mie, zero
    mret

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
