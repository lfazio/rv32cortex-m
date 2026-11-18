/*
 * A branch on a CR field a compare set, with the field written again
 * before the branch: by a record form, then by mtcrf. The translator
 * branches on the compare's operands only while nothing since may have
 * written the CR (crbit_cmp in ppc_ir.c); random words never put a
 * compare, a second write of its field and a branch on it in one block,
 * and a mutant that skipped the check ran 180 s unfound.
 */
	.machine e500
	li	%r3, 3
	li	%r9, 4
1:	cmpwi	%r3, 5			/* cr0: LT */
	li	%r4, 0
	and.	%r5, %r4, %r4		/* cr0: EQ */
	beq	2f			/* taken by the and., never the compare */
	addi	%r6, %r6, 1
2:	cmplwi	%cr7, %r3, 2		/* cr7: GT */
	li	%r8, 2
	mtcrf	0x01, %r8		/* cr7: EQ */
	bgt	%cr7, 3f		/* not taken: the compare's GT is gone */
	addi	%r7, %r7, 1
3:	addi	%r3, %r3, 1
	addic.	%r9, %r9, -1
	bne	1b
	.long	0x7C00007C		/* wait */
