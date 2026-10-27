/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sv32ppn.S - a leaf whose physical address is past 4 GiB.
 *
 * probe-march: rv32ima_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imac_zicsr
 * probe-regs: a0 a1 a2 a3 a4 a5 a6 a7
 * probe-permitted: sail a7 -- which regions may hold page tables is a PMA, the platform's to say; Sail's walk refuses MMIO, Spike's and this core's read it
 *
 *   scripts/run-probe.py tests/arch-test/probes/sv32ppn.S
 *
 * Sv32 gives a 34-bit physical address: a PTE's PPN is 22 bits, the top
 * two of them address bits 32 and 33. This core's bus is 32 bits wide,
 * and rv_mmu.c dropped those two bits -- so a leaf naming 0x2_8000_1000
 * read 0x8000_1000. fuzz-rv32-mmu's reference walk, written from the
 * spec, said there is nothing there to read: an access fault.
 *
 * Every access is a data access from M-mode at S by MPRV, so nothing
 * here leaves M; the M-mode handler files mcause by case and steps over
 * the instruction. MRET leaves MPP at U, so each case sets it back to S.
 * The answers:
 *
 *   a0  1  load through an ordinary leaf: the word    0x12345678
 *   a1  2  load, leaf PTE bit 31 (PA bit 33)           5  load access fault
 *   a2  3  load, leaf PTE bit 30 (PA bit 32)           5
 *   a3  4  store, leaf PTE bit 31                      7  store access fault
 *   a4  5  load, a megapage whose PTE has bit 31       5
 *   a5  6  load, a pointer to a table past 4 GiB       5
 *   a6     traps in case 1                              0
 *   a7  7  load, a pointer to a table in the CLINT     d  load page fault
 *
 * Case 7 is the platform's to answer, and the two models answer it
 * differently: Spike reads the CLINT as a page table, finds no valid PTE
 * and raises a page fault, as this core does; Sail's walk refuses a
 * device with an access fault. Which regions may hold page tables is a
 * physical memory attribute, so both are readings the spec allows.
 *
 * An unwritten slot reads 0xdeadbee0, so a trap that never happened
 * shows as itself.
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

    la   t0, tbl
    li   t1, 0xdeadbee0
    li   t2, 8
1:  sw   t1, 0(t0)
    addi t0, t0, 4
    addi t2, t2, -1
    bnez t2, 1b
    li   a6, 0

    la   t0, dpage
    li   t1, 0x12345678
    sw   t1, 0(t0)

    # VA 0x40000000: a table of 4 KiB leaves, each R W A D, S only:
    #   0x40000000  dpage
    #   0x40001000  dpage, PTE bit 31 set
    #   0x40002000  dpage, PTE bit 30 set
    # VA 0x40400000: a megapage at 0x80000000, PTE bit 31 set.
    # VA 0x40800000: a pointer to a table at l2's address + 2^32.
    la   t0, root
    la   t1, l2
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t2, t1, 1
    li   t3, 0x100 * 4
    add  t3, t0, t3
    sw   t2, 0(t3)                  # root[0x100] -> l2

    li   t4, 0x40000000             # PPN bit 20, as a PTE bit
    or   t2, t2, t4
    li   t3, 0x102 * 4
    add  t3, t0, t3
    sw   t2, 0(t3)                  # root[0x102] -> l2 + 2^32

    li   t2, 0x200000c7 | 0x80000000
    li   t3, 0x101 * 4
    add  t3, t0, t3
    sw   t2, 0(t3)                  # root[0x101]: megapage, bit 31

    li   t2, (0x02000000 >> 12 << 10) | 1
    li   t3, 0x103 * 4
    add  t3, t0, t3
    sw   t2, 0(t3)                  # root[0x103] -> a table in the CLINT

    la   t5, l2
    la   t1, dpage
    srli t1, t1, 12
    slli t1, t1, 10
    ori  t1, t1, 0xc7               # D A W R V
    sw   t1, 0(t5)
    li   t4, 0x80000000
    or   t2, t1, t4
    sw   t2, 4(t5)
    li   t4, 0x40000000
    or   t2, t1, t4
    sw   t2, 8(t5)

    srli t0, t0, 12
    li   t1, 0x80000000
    or   t0, t0, t1
    csrw satp, t0
    sfence.vma

    la   s1, tbl
    li   s3, (1 << 17) | (1 << 11)  # MPRV, MPP = S
    li   s4, 3 << 11

    # case 1: the control
    li   s0, 0
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40000000
    lw   a0, 0(t0)
    mv   a6, gp
    # case 2
    li   s0, 1
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40001000
    lw   t1, 0(t0)
    # case 3
    li   s0, 2
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40002000
    lw   t1, 0(t0)
    # case 4
    li   s0, 3
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40001000
    sw   t1, 0(t0)
    # case 5
    li   s0, 4
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40400000
    lw   t1, 0(t0)
    # case 6
    li   s0, 5
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40800000
    lw   t1, 0(t0)
    # case 7
    li   s0, 6
    csrc mstatus, s4
    csrs mstatus, s3
    li   t0, 0x40C00000
    lw   t1, 0(t0)

    li   t0, (1 << 17)
    csrc mstatus, t0
    lw   a1, 4(s1)
    lw   a2, 8(s1)
    lw   a3, 12(s1)
    lw   a4, 16(s1)
    lw   a5, 20(s1)
    lw   a7, 24(s1)

    la   s0, tohost
    li   t0, 1
    sw   t0, 0(s0)
5:  j    5b

# File mcause under the case in s0, count the trap, step over.
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
    .align 12
root:   .fill 1024, 4, 0
l2:     .fill 1024, 4, 0
dpage:  .fill 1024, 4, 0
    .align 4
tbl:    .fill 8, 4, 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
