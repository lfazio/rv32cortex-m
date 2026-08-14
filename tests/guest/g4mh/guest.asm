#CC-RH Compiler RH850 Assembler Source
#@	CC-RH Version : V2.08.00  [26 Jun 2026]
#@	Command : -Xcommon=rh850 -Xcpu=g4mh -Xfloat=fpu -S guest.c
#@	compiled at Thu Aug 13 18:54:52 2026

	.file "guest.c"

	$reg_mode 32
	.dbl_size 8

	.public _main

	.section .text, text
_wr.1:
	.stack _wr.1 = 0
	mov r6, r8
	mov r7, r9
	mov 0x00000001, r7
	movea 0x00000040, r0, r6
	._line_top inline_asm
    mov r6, r11        ; nr
    mov r7, r6         ; arg0
    mov r8, r7         ; arg1
    mov r9, r8         ; arg2
    trap 0
	._line_end inline_asm
	jmp [r31]
_puts_.1:
	.stack _puts_.1 = 4
	prepare 0x00000001, 0x00000000
	mov 0x00000000, r7
	br9 .BB.LABEL.2_2
.BB.LABEL.2_1:	; bb
	add 0x00000001, r7
.BB.LABEL.2_2:	; bb2
	mov r6, r2
	add r7, r2
	ld.bu 0x00000000[r2], r2
	cmp 0x00000000, r2
	bnz9 .BB.LABEL.2_1
.BB.LABEL.2_3:	; bb10
	jarl _wr.1, r31
	dispose 0x00000000, 0x00000001, [r31]
_puthex.1:
	.stack _puthex.1 = 16
	prepare 0x00000001, 0x0000000C
	movea 0x00000030, r0, r2
	st.b r2, 0x00000001[r3]
	movea 0x00000078, r0, r2
	st.b r2, 0x00000002[r3]
	mov 0x00000000, r2
	br9 .BB.LABEL.3_2
.BB.LABEL.3_1:	; bb
	mov 0x00000007, r5
	sub r2, r5
	shl 0x00000002, r5
	shr r5, r6, r5
	andi 0x0000000F, r5, r5
	cmp 0x0000000A, r5
	movea 0x00000057, r0, r7
	movea 0x00000030, r0, r8
	cmov 0x00000001, r8, r7, r7
	add r7, r5
	movea 0x00000001, r3, r7
	add r2, r7
	st.b r5, 0x00000002[r7]
	add 0x00000001, r2
.BB.LABEL.3_2:	; bb29
	cmp 0x00000008, r2
	blt9 .BB.LABEL.3_1
.BB.LABEL.3_3:	; bb34
	mov 0x0000000A, r2
	st.b r2, 0x0000000B[r3]
	mov 0x0000000B, r7
	movea 0x00000001, r3, r6
	jarl _wr.1, r31
	dispose 0x0000000C, 0x00000001, [r31]
_main:
	.stack _main = 108
	prepare 0x00000FFF, 0x0000003C
	mov 0x00000007, r2
	st.w r2, 0x00000038[r3]
	mov 0xFFFFFFFD, r2
	st.w r2, 0x00000034[r3]
	movhi 0x00004000, r0, r2
	st.w r2, 0x00000030[r3]
	movhi 0x00003F00, r0, r2
	st.w r2, 0x0000002C[r3]
	mov #.STR.1, r6
	jarl _puts_.1, r31
	ld.w 0x00000038[r3], r20
	ld.w 0x00000034[r3], r21
	ld.w 0x00000038[r3], r22
	ld.w 0x00000034[r3], r23
	ld.w 0x00000038[r3], r24
	ld.w 0x00000034[r3], r25
	ld.w 0x00000038[r3], r26
	ld.w 0x00000038[r3], r27
	ld.w 0x00000034[r3], r28
	mov #.STR.116, r6
	jarl _puts_.1, r31
	ld.w 0x00000030[r3], r29
	ld.w 0x0000002C[r3], r30
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000028[r3]
	ld.w 0x0000002C[r3], r2
	st.w r2, 0x00000024[r3]
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000020[r3]
	ld.w 0x0000002C[r3], r2
	st.w r2, 0x0000001C[r3]
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000018[r3]
	ld.w 0x0000002C[r3], r2
	st.w r2, 0x00000014[r3]
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000010[r3]
	ld.w 0x0000002C[r3], r2
	st.w r2, 0x0000000C[r3]
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000008[r3]
	ld.w 0x0000002C[r3], r2
	st.w r2, 0x00000004[r3]
	ld.w 0x00000030[r3], r2
	st.w r2, 0x00000000[r3]
	mov #.STR.117, r6
	jarl _puts_.1, r31
	add r21, r20
	cmp 0x00000004, r20
	setf 0x0000000A, r2
	mul r23, r22, r0
	addi 0x00000015, r22, r0
	setf 0x0000000A, r5
	divq r25, r24, r0
	cmp 0xFFFFFFFE, r24
	adf 0x0000000A, r5, r2, r2
	mov 0x00000003, r5
	divhu r5, r26, r26
	cmp 0x00000001, r26
	setf 0x0000000A, r5
	mov 0x1FFFFFFF, r6
	and r6, r27
	cmp 0x00000007, r27
	adf 0x0000000A, r5, r2, r2
	mov 0xFFFFFFFE, r5
	and r5, r28
	cmp 0xFFFFFFFC, r28
	setf 0x0000000A, r5
	addf.s r30, r29, r6
	movhi 0x00004020, r0, r7
	cmpf.s 0x00000002, r6, r7
	trfsr 0
	adf 0x0000000A, r5, r2, r2
	ld.w 0x00000028[r3], r5
	ld.w 0x00000024[r3], r6
	subf.s r6, r5, r5
	movhi 0x00003FC0, r0, r6
	cmpf.s 0x00000002, r5, r6
	trfsr 0
	setf 0x0000000A, r5
	ld.w 0x00000020[r3], r6
	ld.w 0x0000001C[r3], r7
	mulf.s r7, r6, r6
	movhi 0x00003F80, r0, r7
	cmpf.s 0x00000002, r6, r7
	trfsr 0
	adf 0x0000000A, r5, r2, r2
	ld.w 0x00000018[r3], r5
	ld.w 0x00000014[r3], r6
	divf.s r6, r5, r5
	movhi 0x00004080, r0, r6
	cmpf.s 0x00000002, r5, r6
	trfsr 0
	setf 0x0000000A, r5
	movhi 0x00004040, r0, r6
	ld.w 0x00000010[r3], r7
	mulf.s r6, r7, r6
	trncf.sw r6, r6
	cmp 0x00000006, r6
	adf 0x0000000A, r5, r2, r2
	ld.w 0x0000000C[r3], r5
	ld.w 0x00000008[r3], r6
	cmpf.s 0x00000007, r6, r5
	trfsr 0
	setf 0x00000002, r5
	ld.w 0x00000004[r3], r7
	ld.w 0x00000000[r3], r6
	cmpf.s 0x00000004, r6, r7
	trfsr 0
	adf 0x00000002, r5, r2, r20
	mov 0x00000000, r2
	br9 .BB.LABEL.4_2
.BB.LABEL.4_1:	; bb158
	mov r2, r5
	mul r5, r5, r0
	mov r2, r6
	shl 0x00000002, r6
	mov #_arr.1, r7
	add r6, r7
	st.w r5, 0x00000000[r7]
	add 0x00000001, r2
.BB.LABEL.4_2:	; bb166
	cmp 0x00000008, r2
	blt9 .BB.LABEL.4_1
.BB.LABEL.4_3:	; bb171
	mov #.STR.118, r6
	jarl _puts_.1, r31
	movhi HIGHW1(#_arr.1+0x0000001C), r0, r2
	ld.w LOWW(#_arr.1+0x0000001C)[r2], r21
	mov #.STR.119, r6
	jarl _puts_.1, r31
	mov #.STR.120, r6
	jarl _puts_.1, r31
	mov 0x0000000F, r6
	jarl _puthex.1, r31
	mov #.STR.121, r6
	jarl _puts_.1, r31
	addi 0xFFFFFFCF, r21, r0
	adf 0x0000000A, r0, r20, r20
	mov r20, r6
	jarl _puthex.1, r31
	cmp 0x00000000, r20
	mov #.STR.122, r2
	mov #.STR.123, r5
	cmov 0x00000002, r5, r2, r6
	jarl _puts_.1, r31
	mov r20, r10
	dispose 0x0000003C, 0x00000FFF, [r31]
	.section .bss, bss
	.align 4
_arr.1:
	.ds (32)
	.section .const, const
.STR.1:
	.db 0x42,0x41,0x4E,0x4E,0x45,0x52,0x0A
	.ds (1)
.STR.116:
	.db 0x49,0x4E,0x54,0x2D,0x4F,0x4B,0x0A
	.ds (1)
.STR.117:
	.db 0x46,0x50,0x2D,0x4F,0x4B,0x0A
	.ds (1)
.STR.118:
	.db 0x41,0x52,0x52,0x2D,0x4F,0x4B,0x0A
	.ds (1)
.STR.119:
	.db 0x4C,0x4F,0x4F,0x50,0x2D,0x4F,0x4B,0x0A
	.ds (1)
.STR.120:
	.db 0x47,0x34,0x4D,0x48,0x2D,0x47,0x55,0x45,0x53,0x54,0x20,0x63,0x68,0x65,0x63,0x6B
	.db 0x73,0x3D
	.ds (1)
.STR.121:
	.db 0x47,0x34,0x4D,0x48,0x2D,0x47,0x55,0x45,0x53,0x54,0x20,0x66,0x61,0x69,0x6C,0x73
	.db 0x3D
	.ds (1)
.STR.122:
	.db 0x46,0x41,0x49,0x4C,0x0A
	.ds (1)
.STR.123:
	.db 0x50,0x41,0x53,0x53,0x0A
	.ds (1)
