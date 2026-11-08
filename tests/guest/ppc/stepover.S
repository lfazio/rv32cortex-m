/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stepover.S - a data storage interrupt stepped over, 100 times, second
 * in its block. VLE, as isatest.S.
 *
 * Each pass stores the counter to RAM, then loads from an address no
 * region covers -- DSI, whose handler moves SRR0 past the load and
 * returns -- then counts. The run's total must be the same on both
 * backends (tests/guest/same-on-both-backends.py): the interpreter
 * charges an interrupt one unit of the budget, and a block that raised
 * one partway was charged only what it had retired: one short under the
 * JIT for each (emu_jit_hot_t::charged). The load is second in its
 * block; first, the dispatcher's floor of one would charge it anyway.
 *
 * Prints STEPOVER-OK if the loop ran all 100 passes and the last store
 * before the last interrupt landed, STEPOVER-BAD otherwise.
 */
	.text
	.globl	_start
_start:
	e_lis	%r3, 0x8000
	mtspr	63, %r3			/* IVPR: this image's 64 KB page */
	e_li	%r3, 0x100
	mtspr	402, %r3		/* IVOR2, data storage: handler */
	e_lis	%r4, 0x7000		/* no region */
	e_lis	%r5, word@ha
	e_add16i %r5, %r5, word@l
	e_li	%r6, 0
	e_li	%r7, 100
loop:
	e_stw	%r6, 0(%r5)		/* permitted */
	e_lwz	%r8, 0(%r4)		/* DSI, stepped over */
	e_addi	%r6, %r6, 1
	cmpw	%r6, %r7
	e_blt	loop

	e_lwz	%r9, 0(%r5)
	e_lis	%r10, okmsg@ha
	e_add16i %r10, %r10, okmsg@l
	e_cmp16i %r9, 99
	e_beq	print
	e_lis	%r10, badmsg@ha
	e_add16i %r10, %r10, badmsg@l
print:
	e_lis	%r11, 0x1000		/* the NS16550's transmit register */
1:	e_lbz	%r12, 0(%r10)
	e_cmp16i %r12, 0
	e_beq	2f
	e_stb	%r12, 0(%r11)
	e_addi	%r10, %r10, 1
	e_b	1b
2:	e_li	%r3, 0
	e_li	%r0, 93			/* exit */
	se_sc
3:	e_b	3b

	.org	0x100
handler:
	mfsrr0	%r20
	e_addi	%r20, %r20, 4		/* e_lwz is four bytes */
	mtsrr0	%r20
	se_rfi

	.data
	.align	2
word:	.long	0
okmsg:	.string	"STEPOVER-OK\n"
badmsg:	.string	"STEPOVER-BAD\n"
