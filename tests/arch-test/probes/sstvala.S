/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sstvala.S - what Sail and Spike put in stval, against this core.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imafdc_zicsr_zicntr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 a0 a1 a2 a3 a4 a5 a6 a7
 * probe-regs: t1 t2 t3 t4 t5 t6 tp ra sp gp
 * probe-permitted: sail tp ra -- Sail 0.13.1 has no Sdtrig, so no trigger fires
 * probe-permitted: sail a1 -- Sail's WFI completes at once, and TW traps only one that does not
 *
 *   scripts/run-probe.py tests/arch-test/probes/sstvala.S
 *
 * Sstvala (RVA20S) asks for stval to hold the faulting virtual address
 * for every access, page and misaligned fault and for a hardware
 * breakpoint, and the instruction for an illegal-instruction trap.
 * riscv-arch-test checks tval wherever its handler records a trap, but
 * it has no Sstvala suite, and the cases where an implementation is
 * likeliest to be wrong are the ones it does not reach: an illegal
 * *compressed* instruction, one that is illegal only because of a mode
 * bit, one straddling two pages, a fetch fault on an instruction's
 * second half, and a fault whose physical and virtual addresses differ.
 *
 * Twenty-three traps, all taken from S-mode to S-mode under Sv32. The
 * handler files scause and stval by case number and resumes at a label,
 * not at sepc + 4, so a fetch fault and a 16-bit instruction resume the
 * same way. The answers, then:
 *
 *   s1  causes 1-8, a nibble each                    0x22222222
 *   s2  causes 9-16, case 10's nibble left out      0x2022ccdf
 *   s3  causes 17-22                                 0x00571469
 *
 *   case                                            stval
 *   s4   1  custom-0, a 32-bit illegal instruction   0x1234500b
 *   s5   2  c.lwsp x0: reserved, 16 bits             0x00004002
 *   s6   3  c.flwsp with mstatus.FS off              0x00006002
 *   s7   4  fadd.s with mstatus.FS off               0x00208053
 *   s8   5  csrr mstatus, from S                     0x300022f3
 *   s9   6  mret, from S                             0x30200073
 *   s10  7  sret under TSR                           0x10200073
 *   s11  8  sfence.vma under TVM                     0x12000073
 *   a0   9  csrr satp under TVM                      0x180022f3
 *   a1  10  wfi under TW                             0x10500073
 *   a2  11  rdtime, mcounteren.TM clear              0xc01022f3
 *   a3  12  illegal, straddling two mapped pages     0x5678900b
 *   a4  13  fetch page fault on the second half      0x40002000
 *   a5  14  fetch page fault                         0x40002010
 *   a6  15  load page fault                          0x40002004
 *   a7  16  store page fault, page R and X           0x40000010
 *   t1  17  load access fault, VA mapped to nothing  0x40003008
 *   t2  18  store access fault                       0x4000300c
 *   t3  19  fetch access fault                       0x40003000
 *   t4  20  lr.w misaligned                          0x40004002
 *   t5  21  amoadd.w misaligned                      0x40004001
 *   t6  22  ecall, which carries nothing             0x00000000
 *   tp  23  mcontrol load trigger                    0x40004008
 *   ra  23  ... and its scause                       0x00000003
 *   sp  sepc of case 13: where the instruction starts   0x40001ffe
 *   gp  traps that reached M-mode instead               0
 *
 * Case 3 is the one an emulator that expands compressed code before
 * executing it can get wrong: the instruction that faulted is 16 bits,
 * and the expansion is not what the guest wrote. Cases 13 and 23 are the
 * address of the *portion* that faulted, and 17-19 are virtual
 * addresses whose physical page holds nothing. An unwritten slot reads
 * 0xdeadbee0, so a trap that never happened shows as itself.
 *
 * Sail 0.13.1 and Spike (f8a060b) give all of it, with two permitted
 * differences, both Sail's. It has no Sdtrig, so its load in case 23
 * completes and tp and ra keep their fill. And its WFI is a no-op
 * (`wfi_is_nop`), which completes at once -- TW makes a WFI illegal
 * only if it does not complete within a bound, so a1 keeps its fill
 * there and is a legal reading. Case 10's cause is kept out of s2 for
 * that reason, so the permission covers one register and not eight.
 *
 * The emulator, the first time this ran, differed from both on two:
 * case 6 ran `mret` in S-mode, returning to U at mepc, and case 3
 * reported 0x00012007 -- the 32-bit `flw` that `c.flwsp` expands to.
 * Both are fixed, on both backends; umode.S found `mret` running in
 * U-mode too, and WFI parking the hart there with TW clear.
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

    # Every slot reads 0xdeadbee0 until a trap fills it.
    la   t0, tbl
    li   t1, 0xdeadbee0
    li   t2, 24 * 4
1:  sw   t1, 0(t0)
    addi t0, t0, 4
    addi t2, t2, -1
    bnez t2, 1b

    # Sv32. VA 0x80000000 is one megapage, identity, S only, RWX; VA
    # 0x40000000 is a table of 4 KiB pages:
    #   0x40000000  page0  R X     its last halfword starts case 12
    #   0x40001000  page1  R X     its last halfword starts case 13
    #   0x40002000  --     invalid
    #   0x40003000  PA 0x70000000, R W X, where nothing answers
    #   0x40004000  dpage  R W
    la   t0, root
    li   t1, 0x200000cf
    li   t2, 0x200 * 4
    add  t2, t0, t2
    sw   t1, 0(t2)
    la   t1, l2
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t1, t1, 1
    sw   t1, 0x100 * 4(t0)
    la   t2, l2
    la   t1, page0
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t1, t1, 0x4b
    sw   t1, 0(t2)
    la   t1, page1
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t1, t1, 0x4b
    sw   t1, 4(t2)
    li   t1, 0x1c0000cf
    sw   t1, 12(t2)
    la   t1, dpage
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t1, t1, 0xc7
    sw   t1, 16(t2)
    srli t0, t0, 12
    li   t1, 0x80000000
    or   t0, t0, t1
    csrw satp, t0
    sfence.vma

    # Trigger 0: mcontrol, a load from 0x40004008 in S, a breakpoint.
    # Sail has no triggers, and mtrap steps over each of these.
    csrw tselect, zero
    li   t0, 0x40004008
    csrw tdata2, t0
    li   t0, 0x20000011
    csrw tdata1, t0

    li   t0, 0xb3ff             # everything S may take
    csrw medeleg, t0
    csrw mcounteren, zero
    li   t0, (1 << 20) | (1 << 21) | (1 << 22)  # TVM, TW, TSR
    csrs mstatus, t0
    li   t0, 0x6000             # FS off
    csrc mstatus, t0
    li   t0, 3 << 11
    csrc mstatus, t0
    li   t0, 1 << 11
    csrs mstatus, t0
    li   gp, 0
    la   t0, smain
    csrw mepc, t0
    mret

# One case: its number in s0, where to resume in s11, then the
# instruction. Numeric labels may be redefined; 1f is the next one.
.macro T n, insn:vararg
    li   s0, \n
    la   s11, 1f
    \insn
1:
.endm

smain:
    T 1, .word 0x1234500b       # custom-0
    T 2, .half 0x4002           # c.lwsp x0, 0(sp): reserved
    T 3, .half 0x6002           # c.flwsp ft0, 0(sp)
    T 4, .word 0x00208053       # fadd.s ft0, ft1, ft2, rne
    T 5, .word 0x300022f3       # csrr t0, mstatus
    T 6, mret
    T 7, sret
    T 8, sfence.vma
    T 9, .word 0x180022f3       # csrr t0, satp
    T 10, wfi
    T 11, .word 0xc01022f3      # rdtime t0
    li   t0, 0x40000ffe
    T 12, jalr zero, 0(t0)
    li   t0, 0x40001ffe
    T 13, jalr zero, 0(t0)
    li   t0, 0x40002010
    T 14, jalr zero, 0(t0)
    li   t0, 0x40002004
    T 15, lw t1, 0(t0)
    li   t0, 0x40000010
    T 16, sw t1, 0(t0)
    li   t0, 0x40003008
    T 17, lw t1, 0(t0)
    li   t0, 0x4000300c
    T 18, sw t1, 0(t0)
    li   t0, 0x40003000
    T 19, jalr zero, 0(t0)
    li   t0, 0x40004002
    T 20, lr.w t1, (t0)
    li   t0, 0x40004001
    T 21, amoadd.w t1, t1, (t0)
    T 22, ecall
    # Sdtrig keeps a breakpoint trigger quiet in S-mode while breakpoints
    # go to S and SIE is clear -- or a trigger in the handler would loop.
    # Spike holds to that; nothing is pending, so SIE costs nothing here.
    csrsi sstatus, 2
    li   t0, 0x40004008
    T 23, lw t1, 0(t0)

    # The causes, a nibble each.
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
    li   t2, 8
3:  lw   t1, 0(t0)
    andi t1, t1, 15
    slli s2, s2, 4
    or   s2, s2, t1
    addi t0, t0, 16
    addi t2, t2, -1
    bnez t2, 3b
    li   t1, 0xf0ffffff        # case 10: see the header
    and  s2, s2, t1
    li   s3, 0
    li   t2, 6
4:  lw   t1, 0(t0)
    andi t1, t1, 15
    slli s3, s3, 4
    or   s3, s3, t1
    addi t0, t0, 16
    addi t2, t2, -1
    bnez t2, 4b

    # The tvals, and the two extras.
    la   t0, tbl
    lw   s4, 1 * 16 + 4(t0)
    lw   s5, 2 * 16 + 4(t0)
    lw   s6, 3 * 16 + 4(t0)
    lw   s7, 4 * 16 + 4(t0)
    lw   s8, 5 * 16 + 4(t0)
    lw   s9, 6 * 16 + 4(t0)
    lw   s10, 7 * 16 + 4(t0)
    lw   s11, 8 * 16 + 4(t0)
    lw   a0, 9 * 16 + 4(t0)
    lw   a1, 10 * 16 + 4(t0)
    lw   a2, 11 * 16 + 4(t0)
    lw   a3, 12 * 16 + 4(t0)
    lw   a4, 13 * 16 + 4(t0)
    lw   a5, 14 * 16 + 4(t0)
    lw   a6, 15 * 16 + 4(t0)
    lw   a7, 16 * 16 + 4(t0)
    lw   t1, 17 * 16 + 4(t0)
    lw   t2, 18 * 16 + 4(t0)
    lw   t3, 19 * 16 + 4(t0)
    lw   t4, 20 * 16 + 4(t0)
    lw   t5, 21 * 16 + 4(t0)
    lw   t6, 22 * 16 + 4(t0)
    lw   tp, 23 * 16 + 4(t0)
    lw   ra, 23 * 16 + 0(t0)
    lw   sp, 13 * 16 + 8(t0)

    la   s0, tohost
    li   t0, 1
    sw   t0, 0(s0)
5:  j    5b

# S-mode: file scause, stval and sepc under the case in s0, and resume
# at s11. Not sret: TSR is set, and every trap here is S to S, so there
# is no mode to return to.
    .align 2
strap:
    csrr t4, scause
    csrr t5, stval
    csrr t6, sepc
    la   t3, tbl
    slli t2, s0, 4
    add  t3, t3, t2
    sw   t4, 0(t3)
    sw   t5, 4(t3)
    sw   t6, 8(t3)
    jr   s11

# M-mode: a trap from S that should have been S's is counted in gp and
# resumed like any other; one from M -- the trigger set-up on a model
# without triggers -- steps over the instruction.
    .align 2
mtrap:
    csrr t6, mstatus
    srli t6, t6, 11
    andi t6, t6, 3
    li   t5, 3
    beq  t6, t5, 6f
    addi gp, gp, 1
    csrw mepc, s11
    mret
6:  csrr t6, mepc
    addi t6, t6, 4
    csrw mepc, t6
    mret

    .data
    .align 12
root:   .fill 1024, 4, 0
l2:     .fill 1024, 4, 0
page0:  .fill 2047, 2, 0
        .half 0x900b            # the low half of 0x5678900b ...
page1:  .half 0x5678            # ... and its high half, one page on
        .fill 2046, 2, 0
        .half 0x0013            # the low half of addi x0, x0, 0
dpage:  .fill 1024, 4, 0
    .align 4
tbl:    .fill 24 * 4, 4, 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
