/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_armv7m.c - the ARMv7E-M frontend.
 *
 * The length decoder gets a property test over every possible input,
 * because that is the one function in a mixed-width decoder whose
 * mistakes do not produce wrong answers. G4MH's equivalent was spelled
 * twice, the two spellings disagreed on exactly one slot, and the result
 * was an *infinite loop* in an implementation that had never executed --
 * the second halfword of a 48-bit jump read as zero, so the branch went
 * to pc + 0.
 *
 * The rest drives the interpreter through hand-built halfwords, which
 * this tree normally warns against. It is acceptable here only because
 * every encoding is checked against `arm-none-eabi-as` by
 * scripts/t2-check-encodings.sh in the backend direction already -- and
 * the moment there is a compiled guest, that is the test that matters.
 */

#include "tests.h"

#include "armv7m/armv7m_cpu.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* The length decoder                                                  */
/* ------------------------------------------------------------------ */

/*
 * Exhaustive, and it can be: there are only 65536 first halfwords.
 *
 * ARMv7-M says a halfword introduces a 32-bit instruction exactly when
 * bits 15:11 are 0b11101, 0b11110 or 0b11111. The test spells that
 * independently of the implementation -- as a bit test on the three
 * values rather than by calling the same helper -- because a test that
 * reuses the function it is checking only proves the function is
 * consistent with itself.
 */
static void test_insn_len_exhaustive(void)
{
    uint32_t wide = 0u;
    uint32_t narrow = 0u;
    bool mismatch = false;

    for (uint32_t i = 0u; i <= 0xFFFFu; i++) {
        const uint16_t hw = (uint16_t)i;
        const uint32_t top5 = i >> 11;
        const bool expect_wide =
            (top5 == 0x1Du) || (top5 == 0x1Eu) || (top5 == 0x1Fu);

        if (armv7m_is_32bit(hw) != expect_wide) {
            mismatch = true;
        }
        /* The two spellings must agree, since one is defined by the other
         * and a future optimisation of either could break that. */
        if (armv7m_insn_len(hw) != (expect_wide ? 4u : 2u)) {
            mismatch = true;
        }
        if (expect_wide) {
            wide++;
        } else {
            narrow++;
        }
    }

    CHECK(!mismatch);

    /*
     * And the counts, so a decoder that answered the same way for
     * *everything* could not pass. Three of the thirty-two 5-bit
     * prefixes are wide, so 3/32 of 65536 is 6144.
     */
    CHECK_EQ(wide, 6144u);
    CHECK_EQ(narrow, 65536u - 6144u);
}

/*
 * The boundary, named explicitly.
 *
 * 0xE800 is the first wide encoding and 0xE7FF the last narrow one --
 * the unconditional branch. This project's own rule: when the whole
 * difficulty is one input, test that input rather than a representative
 * one.
 */
static void test_insn_len_boundary(void)
{
    CHECK(!armv7m_is_32bit(0xE7FFu)); /* B (unconditional), 16-bit */
    CHECK(armv7m_is_32bit(0xE800u)); /* first 32-bit encoding      */
    CHECK(armv7m_is_32bit(0xF000u)); /* BL and the data-processing  */
    CHECK(armv7m_is_32bit(0xFFFFu));
    CHECK(!armv7m_is_32bit(0x0000u)); /* MOVS r0, r0                */
    CHECK(!armv7m_is_32bit(0xBE00u)); /* BKPT                       */
}

void test_armv7m(void)
{
    test_insn_len_exhaustive();
    test_insn_len_boundary();
}
