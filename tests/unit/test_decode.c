/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_decode.c - RVC expansion.
 *
 * The guest self-test covers the compressed encodings a compiler actually
 * emits. What it cannot reach are the reserved encodings and the HINT
 * space, so those are checked here against hand-assembled bit patterns.
 */

#include "tests.h"

#include "rv32/rv_decode.h"

/* Expected expansions, assembled by hand from the ISA manual. */
static void expand_ok(uint16_t c, uint32_t want)
{
    check_eq(__FILE__, __LINE__, "rv_decode_expand_c", rv_decode_expand_c(c), want);
}

static void expand_illegal(uint16_t c)
{
    check_eq(__FILE__, __LINE__, "rv_decode_expand_c (illegal)",
             rv_decode_expand_c(c), 0u);
}

void test_decode(void)
{
    /*
     * The expected values below are not hand-computed: each pair was
     * produced by assembling the compressed form and, separately, the
     * 32-bit form it must expand to, then reading both back with objdump.
     * See scripts/gen-rvc-table.sh.
     */

    /* ---- quadrant 0 ---- */

    /*
     * CIW takes its destination from bits [4:2], unlike the [9:7] field
     * CL/CS/CB use for rs1'. Getting that wrong silently corrupts every
     * stack-frame address a guest computes, so all three widths of the
     * immediate are pinned down here.
     */
    expand_ok(0x0048u, 0x00410513u);   /* c.addi4spn a0, sp, 4    */
    expand_ok(0x002Cu, 0x00810593u);   /* c.addi4spn a1, sp, 8    */
    expand_ok(0x1FFCu, 0x3FC10793u);   /* c.addi4spn a5, sp, 1020 */

    /* the all-zero halfword is a reserved encoding, not a nop */
    expand_illegal(0x0000u);
    /* c.addi4spn with nzuimm == 0 is reserved even with a valid rd */
    expand_illegal(0x0008u);

    expand_ok(0x4188u, 0x0005A503u);   /* c.lw a0, 0(a1)   */
    expand_ok(0x5EFCu, 0x07C6A783u);   /* c.lw a5, 124(a3) */
    expand_ok(0xC188u, 0x00A5A023u);   /* c.sw a0, 0(a1)   */

#if RV_EXT_D
    /*
     * Zcd: C@2.0 includes the compressed double load/stores when D is
     * present, exactly as it includes Zcf's when F is.
     *
     * The immediate is the thing to test and offset 0 cannot: C.FLD
     * scales by *eight* where C.FLW scales by four, and the two share
     * everything else. So the offsets here are chosen to light each
     * field of the split -- uimm[5:3] alone, then uimm[7:6] as well.
     */
    expand_ok(0x2188u, 0x0005B507u);   /* c.fld fa0, 0(a1)   */
    expand_ok(0x2588u, 0x0085B507u);   /* c.fld fa0, 8(a1)   */
    expand_ok(0x25E8u, 0x0C85B507u);   /* c.fld fa0, 200(a1) */
    expand_ok(0xA188u, 0x00A5B027u);   /* c.fsd fa0, 0(a1)   */
    expand_ok(0xA588u, 0x00A5B427u);   /* c.fsd fa0, 8(a1)   */
#else
    expand_illegal(0x2188u);   /* c.fld */
    expand_illegal(0xA188u);   /* c.fsd */
#endif
#if RV_EXT_F
    /* Zcf: C on RV32F includes the compressed FP load/stores. They expand
     * to FLW/FSW, which share the CL/CS immediate layout with C.LW/C.SW. */
    expand_ok(0x6188u, 0x0005A507u);   /* c.flw fa0, 0(a1) */
    expand_ok(0xE188u, 0x00A5A027u);   /* c.fsw fa0, 0(a1) */
#else
    expand_illegal(0x6188u);
    expand_illegal(0xE188u);
#endif

    /* ---- quadrant 1 ---- */

    /* c.nop -> addi zero, zero, 0 */
    expand_ok(0x0001u, 0x00000013u);
    expand_ok(0x0505u, 0x00150513u);   /* c.addi a0, 1   */
    expand_ok(0x157Du, 0xFFF50513u);   /* c.addi a0, -1  */
    expand_ok(0x4505u, 0x00100513u);   /* c.li a0, 1     */
    /* c.j . -> jal zero, 0 */
    expand_ok(0xA001u, 0x0000006Fu);
    /* c.jal . -> jal ra, 0 (RV32 only) */
    expand_ok(0x2001u, 0x000000EFu);

    expand_ok(0x6505u, 0x00001537u);   /* c.lui a0, 1 */
    /* c.lui with nzimm == 0 is reserved */
    expand_illegal(0x6501u);

    expand_ok(0x6141u, 0x01010113u);   /* c.addi16sp sp, 16   */
    expand_ok(0x7101u, 0xE0010113u);   /* c.addi16sp sp, -512 */
    /* c.addi16sp with nzimm == 0 is reserved */
    expand_illegal(0x6101u);

    expand_ok(0x8105u, 0x00155513u);   /* c.srli a0, 1 */
    expand_ok(0x8505u, 0x40155513u);   /* c.srai a0, 1 */
    expand_ok(0x8905u, 0x00157513u);   /* c.andi a0, 1 */
    /* shamt[5] set is an RV64-only encoding */
    expand_illegal(0x9105u);

    expand_ok(0x8D0Du, 0x40B50533u);   /* c.sub a0, a1 */
    expand_ok(0x8D2Du, 0x00B54533u);   /* c.xor a0, a1 */
    expand_ok(0x8D4Du, 0x00B56533u);   /* c.or  a0, a1 */
    expand_ok(0x8D6Du, 0x00B57533u);   /* c.and a0, a1 */
    /* c.subw / c.addw are RV64 only */
    expand_illegal(0x9D0Du);
    expand_illegal(0x9D2Du);

    /* c.beqz a0, . -> beq a0, zero, 0 */
    expand_ok(0xC101u, 0x00050063u);
    /* c.bnez a0, . -> bne a0, zero, 0 */
    expand_ok(0xE101u, 0x00051063u);

    /* ---- quadrant 2 ---- */

    expand_ok(0x0506u, 0x00151513u);   /* c.slli a0, 1        */
    expand_ok(0x4502u, 0x00012503u);   /* c.lwsp a0, 0(sp)    */
    expand_ok(0x57FEu, 0x0FC12783u);   /* c.lwsp a5, 252(sp)  */
    /* c.lwsp with rd == 0 is reserved */
    expand_illegal(0x4002u);
    expand_ok(0xC02Au, 0x00A12023u);   /* c.swsp a0, 0(sp)    */

    expand_ok(0x8502u, 0x00050067u);   /* c.jr a0    */
    /* c.jr with rs1 == 0 is reserved */
    expand_illegal(0x8002u);
    expand_ok(0x9502u, 0x000500E7u);   /* c.jalr a0  */
    expand_ok(0x852Eu, 0x00B00533u);   /* c.mv a0, a1  */
    expand_ok(0x952Eu, 0x00B50533u);   /* c.add a0, a1 */
    expand_ok(0x9002u, 0x00100073u);   /* c.ebreak     */

    /* ---- HINTs must expand to something harmless, not trap ---- */

    /*
     * c.li x0, 1 / c.mv x0, a1 / c.add x0, a1 / c.slli x0, 1 all sit in the
     * HINT space. The spec requires them to execute as no-ops, and they do
     * because each expands to a write of x0, which the execute path
     * discards. What must not happen is an illegal-instruction trap.
     */
    CHECK(rv_decode_expand_c(0x4005u) != 0u);   /* c.li   x0, 1  */
    CHECK(rv_decode_expand_c(0x802Eu) != 0u);   /* c.mv   x0, a1 */
    CHECK(rv_decode_expand_c(0x902Eu) != 0u);   /* c.add  x0, a1 */
    CHECK(rv_decode_expand_c(0x0006u) != 0u);   /* c.slli x0, 1  */
    /* Each really does target x0. */
    CHECK_EQ(rv_rd(rv_decode_expand_c(0x4005u)), 0u);
    CHECK_EQ(rv_rd(rv_decode_expand_c(0x802Eu)), 0u);

    /* ---- a 0b11 parcel is not compressed at all ---- */
    expand_illegal(0x0003u);

    /* ---- immediate helpers ---- */

    /* Sign extension of the I-type field. */
    CHECK_EQ((uint32_t)rv_imm_i(0xFFF00000u), 0xFFFFFFFFu);
    CHECK_EQ((uint32_t)rv_imm_i(0x7FF00000u), 0x000007FFu);
    /* S-type splits the immediate across two fields. */
    CHECK_EQ((uint32_t)rv_imm_s(0xFE000F80u), 0xFFFFFFFFu);
    /* B and J immediates are always even. */
    CHECK_EQ((uint32_t)rv_imm_b(0xFE000F80u) & 1u, 0u);
    CHECK_EQ((uint32_t)rv_imm_j(0xFFFFF000u) & 1u, 0u);
    CHECK_EQ((uint32_t)rv_imm_j(0x800000EFu), 0xFFF00000u);

    /* Instruction length classification. */
    CHECK_EQ(rv_insn_len(0x0001u), 2u);
    CHECK_EQ(rv_insn_len(0x0003u), 4u);
    CHECK(!rv_is_32bit(0x4505u));
    CHECK(rv_is_32bit(0x0513u));
}
