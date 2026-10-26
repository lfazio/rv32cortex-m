/* A software-settable source raised while MSR[EE] is clear, taken at
 * wrteei, in software vector mode: External Input's slot. Book E. */
	lis	%r10, 0xFFF4
	ori	%r10, %r10, 0x8000	/* INTC0 */
	li	%r11, 0
	stw	%r11, 8(%r10)		/* CPR 0 */
	li	%r12, 5
	stb	%r12, 0x40(%r10)	/* PSR0: priority 5 */
	li	%r13, 2
	stb	%r13, 0x20(%r10)	/* SSCIR0: set */
	wrteei	1			/* taken here */
	addi	%r14, %r14, 1
	wait
