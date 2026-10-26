/* Refused by CPR, then admitted when CPR is lowered -- all with EE
 * clear -- and taken at wrteei. */
	lis	%r10, 0xFFF4
	ori	%r10, %r10, 0x8000
	li	%r11, 7
	stw	%r11, 8(%r10)		/* CPR 7 */
	li	%r12, 5
	stb	%r12, 0x41(%r10)	/* PSR1: priority 5, below CPR */
	li	%r13, 2
	stb	%r13, 0x21(%r10)	/* SSCIR1: set, refused */
	li	%r11, 0
	stw	%r11, 8(%r10)		/* CPR 0: admitted, EE still clear */
	lwz	%r15, 0(%r10)		/* BCR */
	wrteei	1
	addi	%r14, %r14, 1
	wait
