/* Hardware vector mode: taking the request is the acknowledgement, and
 * the core goes to IVPR | vector << 4 rather than IVOR4. */
	lis	%r10, 0xFFF4
	ori	%r10, %r10, 0x8000
	li	%r11, 1
	stw	%r11, 0(%r10)		/* BCR.HVEN */
	li	%r11, 0
	stw	%r11, 8(%r10)
	li	%r12, 6
	stb	%r12, 0x43(%r10)	/* PSR3: priority 6 */
	li	%r13, 2
	stb	%r13, 0x23(%r10)	/* SSCIR3: set */
	wrteei	1			/* to IVPR + 0x30 */
	addi	%r14, %r14, 1
	wait
