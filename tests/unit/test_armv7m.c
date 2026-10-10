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
#include "armv7m/armv7m_decode.h"

#include "emu/emu_bus.h"

#include <stdlib.h>
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

/* ------------------------------------------------------------------ */
/* Two descriptions of one encoding space                              */
/* ------------------------------------------------------------------ */

/*
 * armv7m_decode and the interpreter each decide, independently, which
 * encodings are instructions. The interpreter's answer is the one a
 * Cortex-M7 was asked about; the decoder's is what the disassembler, the
 * pair statistics and the IR translator believe. This is the test that
 * they are the same answer.
 *
 * It matters most for the translator. A block is built from what
 * armv7m_decode says, so an encoding it calls an instruction and the
 * interpreter calls UNDEFINED would be *executed* translated and
 * *faulted* interpreted -- and the reverse would merely decline a block,
 * which nothing would ever notice.
 *
 * The interpreter is asked by running it: one step on a core with the
 * FPU on, every register pointing into RAM, and then UFSR.UNDEFINSTR.
 * UDF is the one deliberate difference -- an instruction to the decoder,
 * and to the core the instruction whose whole meaning is that fault.
 */
#define UA_RAM_BASE 0x20000000u
#define UA_RAM_SIZE 0x10000u
#define UA_CODE (UA_RAM_BASE + 0x1000u)

static uint8_t g_ua_ram[UA_RAM_SIZE];
static emu_bus_t g_ua_bus;
static armv7m_cpu_t g_ua_cpu;

/* What one step did, as far as this test cares. */
enum { UA_RAN, UA_UNDEF, UA_NOCP };

static uint32_t interp_outcome(uint16_t w0, uint16_t w1)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    uint32_t n = 0u;

    armv7m_reset_state(c);
    c->bus = &g_ua_bus;
    c->state = EMU_STATE_RUNNING;
    c->vtor = UA_RAM_BASE;
    c->cpacr = 0xFu << 20;
    for (uint32_t r = 0u; r < 13u; r++) {
        c->r[r] = UA_RAM_BASE + 0x8000u;
    }
    c->r[ARMV7M_SP] = UA_RAM_BASE + 0xF000u;
    c->r[ARMV7M_LR] = UA_CODE + 0x101u;
    c->r[ARMV7M_PC] = UA_CODE;

    g_ua_ram[0x1000] = (uint8_t)w0;
    g_ua_ram[0x1001] = (uint8_t)(w0 >> 8);
    g_ua_ram[0x1002] = (uint8_t)w1;
    g_ua_ram[0x1003] = (uint8_t)(w1 >> 8);

    (void)armv7m_run(c, 1u, &n);
    if ((c->cfsr & (1u << 16)) != 0u) {
        return UA_UNDEF;
    }
    return ((c->cfsr & (1u << 19)) != 0u) ? UA_NOCP : UA_RAN;
}

/*
 * Disagreements, grouped: a wrong test in a decoder is wrong for a whole
 * slot, so what is useful is one line per (operation, outcome, top byte)
 * with a count and an example, not the first thousand members of one.
 */
typedef struct {
    uint16_t op;
    uint8_t outcome;
    uint8_t top;
    uint16_t w0, w1;
    uint32_t count;
} ua_group_t;

static ua_group_t g_ua_groups[64];
static uint32_t g_ua_ngroups;

static bool undef_agrees(uint16_t w0, uint16_t w1)
{
    armv7m_insn_t d;
    uint32_t dec;
    uint32_t run;

    armv7m_decode(w0, w1, &d);
    dec = (d.op == ARMV7M_OP_UNDEF || d.op == ARMV7M_OP_UDF) ? UA_UNDEF
          : (d.op == ARMV7M_OP_NOCP)                         ? UA_NOCP
                                                             : UA_RAN;
    run = interp_outcome(w0, w1);
    if (dec == run) {
        return true;
    }
    for (uint32_t i = 0u; i < g_ua_ngroups; i++) {
        ua_group_t *const g = &g_ua_groups[i];

        if (g->op == d.op && g->outcome == run && g->top == (w0 >> 8)) {
            g->count++;
            return false;
        }
    }
    if (g_ua_ngroups < sizeof(g_ua_groups) / sizeof(g_ua_groups[0])) {
        ua_group_t *const g = &g_ua_groups[g_ua_ngroups++];

        g->op = d.op;
        g->outcome = (uint8_t)run;
        g->top = (uint8_t)(w0 >> 8);
        g->w0 = w0;
        g->w1 = w1;
        g->count = 1u;
    }
    return false;
}

static void undef_report(void)
{
    static const char *const k_out[3] = {"executes", "UNDEFINED", "NOCP"};

    for (uint32_t i = 0u; i < g_ua_ngroups; i++) {
        const ua_group_t *const g = &g_ua_groups[i];

        printf("    %04x %04x  decoder: %-12s interpreter: %-9s x%u\n", g->w0,
               g->w1, armv7m_op_name(g->op), k_out[g->outcome],
               (unsigned)g->count);
    }
}

static void test_decode_agrees_on_undefined(void)
{
    /*
     * Every 16-bit encoding, and for each of the 6144 first halfwords of
     * a 32-bit one a spread of second halfwords -- or all 65536 of them
     * with ARMV7M_UNDEF_FULL set, which is 402 million steps and is how
     * this was first run. The multiplier is odd, so the spread never
     * repeats a value.
     */
    const uint32_t per = (getenv("ARMV7M_UNDEF_FULL") != NULL) ? 65536u : 192u;
    uint32_t differ = 0u;
    uint32_t undef = 0u;
    uint32_t total = 0u;

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    /* Every vector at a BKPT, so a fault that is taken has somewhere to go. */
    for (uint32_t v = 1u; v < 16u; v++) {
        const uint32_t handler = UA_RAM_BASE + 0x801u;

        memcpy(&g_ua_ram[v * 4u], &handler, 4u);
    }
    g_ua_ram[0x800] = 0x00u;
    g_ua_ram[0x801] = 0xBEu;
    g_ua_ngroups = 0u;

    for (uint32_t i = 0u; i <= 0xFFFFu; i++) {
        const uint16_t w0 = (uint16_t)i;

        if (!armv7m_is_32bit(w0)) {
            armv7m_insn_t d;

            total++;
            armv7m_decode(w0, 0u, &d);
            undef += (d.op == ARMV7M_OP_UNDEF) ? 1u : 0u;
            differ += undef_agrees(w0, 0u) ? 0u : 1u;
            continue;
        }
        for (uint32_t k = 0u; k < per; k++) {
            const uint16_t w1 = (uint16_t)(k * 0x9E37u + i * 0x0101u);
            armv7m_insn_t d;

            total++;
            armv7m_decode(w0, w1, &d);
            undef += (d.op == ARMV7M_OP_UNDEF) ? 1u : 0u;
            differ += undef_agrees(w0, w1) ? 0u : 1u;
        }
    }

    CHECK_EQ(differ, 0u);
    undef_report();
    /*
     * And that the answer is not the same for everything: a decoder that
     * knew no instructions, against an interpreter that executed none,
     * would agree perfectly.
     */
    CHECK(undef > total / 20u);
    CHECK(undef < total - total / 4u);
    if (getenv("ARMV7M_UNDEF_FULL") != NULL || differ != 0u) {
        printf("    undefined-agreement: %u encodings, %u undefined, %u differ\n",
               (unsigned)total, (unsigned)undef, (unsigned)differ);
    }
}

void test_armv7m(void)
{
    test_insn_len_exhaustive();
    test_insn_len_boundary();
    test_decode_agrees_on_undefined();
}
