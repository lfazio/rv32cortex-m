/*
 * Calls and returns, for the stub a computed jump goes through
 * (emu_jit_ibtc): blr after bl, and bctrl through CTR, round a loop, so
 * each finds its target's block in the table. Random words reached it
 * only while every dispatch filled an entry; filled only after the stub
 * misses, as it is now, they made none (FUZZ_STATS: "computed jumps sent
 * on"), and nothing told whether bclr and bcctr were marked
 * (EMU_IR_EXIT_JUMP) or the stub sent them to the right block.
 */
	.machine e500
	li	%r3, 24
	lis	%r9, g@h
	ori	%r9, %r9, g@l
	mtctr	%r9
1:	bl	f			/* blr */
	addi	%r4, %r4, 3
	bctrl				/* to g, and blr */
	addi	%r5, %r5, 5
	addic.	%r3, %r3, -1
	bne	1b
	.long	0x7C00007C		/* wait */
f:	addi	%r6, %r6, 1
	blr
g:	addi	%r7, %r7, 2
	blr
