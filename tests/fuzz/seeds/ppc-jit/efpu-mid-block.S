/* The four EFPU operations with a helper each (ppc_ir.c), in the middle
 * of a block: operands that are not symmetric, so swapping them shows;
 * results in r0, r17 and r31 as well, so a lost bit of rd shows; and an
 * inexact sum, a denormal operand and a divide by zero, for the round
 * and data interrupts the configuration word's enables ask for. */
	.machine e500
	lis	%r3, 0x4040		/* 3.0 */
	lis	%r4, 0x40E0		/* 7.0 */
	lis	%r5, 0x4B80
	ori	%r5, %r5, 1		/* 16777218.0 */
	lis	%r6, 0x3F80		/* 1.0 */
	li	%r7, 0			/* +0 */
	lis	%r8, 0x0040		/* a denormal */
	efssub	%r9, %r3, %r4		/* -4.0 */
	efsdiv	%r17, %r4, %r3		/* 7/3, inexact */
	efsmul	%r31, %r3, %r9		/* -12.0 */
	efsadd	%r0, %r5, %r6		/* inexact */
	efsadd	%r10, %r8, %r6		/* FINV */
	efsdiv	%r11, %r6, %r7		/* FDBZ */
	efssub	%r12, %r9, %r17
	efsmul	%r13, %r17, %r17
	.long	0x7C00007C		/* wait: e500's own mnemonic is another word */
