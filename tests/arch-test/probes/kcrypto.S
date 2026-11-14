/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kcrypto.S - every S-box entry, through the instructions that read one.
 *
 * probe-march: rv32im_zbkb_zbkx_zknd_zkne_zknh_zksed_zksh
 * probe-models: sail spike
 * probe-spike-isa: rv32im_zbkb_zbkx_zknd_zkne_zknh_zksed_zksh
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8
 *
 *   scripts/run-probe.py tests/arch-test/probes/kcrypto.S
 *
 * The architecture suites run each scalar-crypto instruction on a few
 * dozen inputs; an S-box has 256 entries, and a wrong one would pass
 * every suite that never asked for it. Here each of six instructions
 * runs on all 256 bytes, rs1 zero and bs 0, and folds its results into
 * one register -- rotate left by 5, then XOR -- an order-dependent sum,
 * so any entry that differs changes it:
 *
 *   reg  instruction   reads
 *   s1   aes32esi      the AES S-box
 *   s2   aes32esmi     the AES S-box, times {3, 1, 1, 2}
 *   s3   aes32dsi      the inverse S-box
 *   s4   aes32dsmi     the inverse S-box, times {11, 13, 9, 14}
 *   s5   sm4ed         the SM4 S-box and its round's linear map
 *   s6   sm4ks         the SM4 S-box and the key schedule's
 *   s7   aes32esmi with bs 3 over the same bytes in the top lane, and
 *        rs1 nonzero: the byte select and the rotate back
 *   s8   the rest, on 16 operand pairs from an LCG: pack, packh, brev8,
 *        zip, unzip, xperm4, xperm8, the four sha256 and six sha512
 *        instructions, sm3p0 and sm3p1
 *
 * isatest's test_kcrypto expects the same eight sums, on the board.
 *
 * Asked when the instructions were added (rv_crypto.c), whose AES tables
 * are computed and whose SM4 table is transcribed; both models agree.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    li   s1, 0
    li   s2, 0
    li   s3, 0
    li   s4, 0
    li   s5, 0
    li   s6, 0
    li   s7, 0
    li   t0, 0               /* the byte */
    li   t6, 256
1:
    aes32esi  t1, zero, t0, 0
    slli t2, s1, 5
    srli s1, s1, 27
    or   s1, s1, t2
    xor  s1, s1, t1

    aes32esmi t1, zero, t0, 0
    slli t2, s2, 5
    srli s2, s2, 27
    or   s2, s2, t2
    xor  s2, s2, t1

    aes32dsi  t1, zero, t0, 0
    slli t2, s3, 5
    srli s3, s3, 27
    or   s3, s3, t2
    xor  s3, s3, t1

    aes32dsmi t1, zero, t0, 0
    slli t2, s4, 5
    srli s4, s4, 27
    or   s4, s4, t2
    xor  s4, s4, t1

    sm4ed     t1, zero, t0, 0
    slli t2, s5, 5
    srli s5, s5, 27
    or   s5, s5, t2
    xor  s5, s5, t1

    sm4ks     t1, zero, t0, 0
    slli t2, s6, 5
    srli s6, s6, 27
    or   s6, s6, t2
    xor  s6, s6, t1

    slli t3, t0, 24          /* the byte in the top lane */
    li   t4, 0x5a5a1234
    aes32esmi t1, t4, t3, 3
    slli t2, s7, 5
    srli s7, s7, 27
    or   s7, s7, t2
    xor  s7, s7, t1

    addi t0, t0, 1
    bne  t0, t6, 1b

    li   s8, 0
    li   a0, 0x12345678      /* LCG state: a = a * 1664525 + 1013904223 */
    li   a1, 1664525
    li   a2, 1013904223
    li   t6, 16
2:
    mul  a0, a0, a1
    add  a0, a0, a2
    mv   t4, a0              /* rs1 */
    mul  a0, a0, a1
    add  a0, a0, a2
    mv   t5, a0              /* rs2 */
    .irp insn, pack, packh, xperm4, xperm8, sha512sum0r, sha512sum1r, sha512sig0l, sha512sig0h, sha512sig1l, sha512sig1h
    \insn t1, t4, t5
    slli t2, s8, 5
    srli s8, s8, 27
    or   s8, s8, t2
    xor  s8, s8, t1
    .endr
    .irp insn, brev8, zip, unzip, sha256sum0, sha256sum1, sha256sig0, sha256sig1, sm3p0, sm3p1
    \insn t1, t4
    slli t2, s8, 5
    srli s8, s8, 27
    or   s8, s8, t2
    xor  s8, s8, t1
    .endr
    addi t6, t6, -1
    bnez t6, 2b

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
