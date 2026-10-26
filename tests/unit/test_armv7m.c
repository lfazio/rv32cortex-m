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
#include "emu/emu_gdb.h"

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

/*
 * sp and pc as operands, as a Cortex-M7 answered: every register choice
 * the manual calls UNPREDICTABLE is UNDEFINSTR on the board, 557 of 557
 * in one sweep and every named encoding after -- but not every
 * UNPREDICTABLE encoding, which is why this is a table of answers and
 * not a rule: an LDM of one register, or one that writes back over a
 * base in its list, executes. Collected by tests/armv7m-diff/rawgen.py
 * --regs and --enc; the frontend executed all of the UNDEFINSTR ones,
 * and `bfi pc, ...` left an odd pc that the next fetch read at an odd
 * host address (fuzz-armv7m-jit).
 *
 * Both decoders are held to the board here, not only to each other:
 * test_decode_agrees_on_undefined would pass if both executed these.
 */
static void test_sp_pc_board_answers(void)
{
    static const struct {
        uint32_t enc;
        bool undef;
    } k[] = {
        {0xf36e0f0cu, true}, /* bfi pc, lr, #0, #13            */
        {0xe9ff0100u, true}, /* ldrd r0, r1, [pc, #0]!         */
        {0xeb000f01u, true}, /* add.w pc, r0, r1               */
        {0xea4f0f00u, true}, /* mov.w pc, r0                   */
        {0xf3c00f00u, true}, /* ubfx pc, r0, #0, #1            */
        {0xfb0d3303u, true}, /* mla r3, sp, r3, r3             */
        {0xea1d5411u, true}, /* ands.w r4, sp, r1, lsr #20     */
        {0xf10f0008u, true}, /* add.w r0, pc, #8               */
        {0xf2480f80u, true}, /* movw pc, #32896                */
        {0xfb9df2f3u, true}, /* sdiv r2, sp, r3                */
        {0xe9cf0100u, true}, /* strd r0, r1, [pc, #0]          */
        {0xf8c0f000u, true}, /* str.w pc, [r0]                 */
        {0xf880d000u, true}, /* strb.w sp, [r0]                */
        {0xf890d000u, true}, /* ldrb.w sp, [r0]                */
        {0xf851000du, true}, /* ldr.w r0, [r1, sp]             */
        {0xf8111b01u, true}, /* ldrb.w r1, [r1], #1            */
        {0xe85f0f00u, true}, /* ldrex r0, [pc]                 */
        {0xe8420000u, true}, /* strex r0, r0, [r2]             */
        {0xe8a00003u, true}, /* stm.w r0!, {r0, r1}            */
        {0xe8d0f00du, true}, /* tbb [r0, sp]                   */
        {0xee00da10u, true}, /* vmov s0, sp                    */
        {0xeee1da10u, true}, /* vmsr fpscr, sp                 */
        {0xeef1da10u, true}, /* vmrs sp, fpscr                 */
        {0xec4d0a10u, true}, /* vmov s0, s1, r0, sp            */
        {0xed8f0a00u, true}, /* vstr s0, [pc, #0]              */
        {0xf38d8800u, true}, /* msr APSR_nzcvq, sp             */
        {0xf60d71fcu, false}, /* addw r1, sp, #4092             */
        {0xf10d0008u, false}, /* add.w r0, sp, #8               */
        {0xebad0340u, false}, /* sub.w r3, sp, r0, lsl #1       */
        {0xf1bd0f00u, false}, /* cmp.w sp, #0                   */
        {0xea4f010du, false}, /* mov.w r1, sp                   */
        {0xf8c0d000u, false}, /* str.w sp, [r0]                 */
        {0xe8900002u, false}, /* ldm.w r0, {r1}: one register   */
        {0xe8b00003u, false}, /* ldm.w r0!, {r0, r1}            */
        {0xfab2f081u, false}, /* clz r0, r1, its two Rm copies differing */
        {0xee100b10u, false}, /* vmov.32 r0, d0[0]              */
        /* 16-bit, the second halfword ignored */
        {0x458f0000u, true}, /* cmp pc, r1                     */
        {0x45780000u, true}, /* cmp r0, pc                     */
        {0x47f80000u, true}, /* blx pc                         */
        {0x45080000u, false}, /* cmp r0, r1, both low in the hi form */
        {0x458d0000u, false}, /* cmp sp, r1                     */
        {0xbfe40000u, false}, /* itt al                         */
        {0xbff80000u, false}, /* it with firstcond 1111         */
    };

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(
        emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    for (uint32_t v = 1u; v < 16u; v++) {
        const uint32_t handler = UA_RAM_BASE + 0x801u;

        memcpy(&g_ua_ram[v * 4u], &handler, 4u);
    }
    g_ua_ram[0x801] = 0xBEu;
    for (uint32_t i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        const uint16_t w0 = (uint16_t)(k[i].enc >> 16);
        const uint16_t w1 = (uint16_t)k[i].enc;
        armv7m_insn_t d;

        armv7m_decode(w0, w1, &d);
        if ((interp_outcome(w0, w1) == UA_UNDEF) != k[i].undef ||
            (d.op == ARMV7M_OP_UNDEF) != k[i].undef) {
            printf("    %08x: board %s, interpreter %s, decoder %s\n",
                   (unsigned)k[i].enc, k[i].undef ? "UNDEFINED" : "executes",
                   interp_outcome(w0, w1) == UA_UNDEF ? "UNDEFINED"
                                                      : "executes",
                   armv7m_op_name(d.op));
            CHECK(false);
        }
    }
}

/*
 * IT blocks, as a Cortex-M7 runs what the manual calls UNPREDICTABLE in
 * them: a branch that is not last, a conditional branch or CBZ inside
 * one, an IT inside one, AL with an else and firstcond 1111. Each
 * sequence is the board's, from rawgen's named halfwords, with the
 * board's r0 and r1 for both values of Z -- see it_ignores_condition and
 * it_after. Until then the frontend raised UNDEFINSTR for all of them.
 */
static void run_seq(const uint16_t *hw, uint32_t n, bool z, uint32_t r0)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    uint32_t done = 0u;

    armv7m_reset_state(c);
    c->bus = &g_ua_bus;
    c->state = EMU_STATE_RUNNING;
    c->vtor = UA_RAM_BASE;
    c->r[0] = r0;
    c->r[1] = 0x11u;
    c->r[ARMV7M_SP] = UA_RAM_BASE + 0xF000u;
    c->r[ARMV7M_PC] = UA_CODE;
    c->xpsr |= z ? ARMV7M_Z : 0u;
    for (uint32_t i = 0u; i < n + 1u; i++) {
        const uint16_t v = (i < n) ? hw[i] : 0xBE00u; /* then BKPT */

        g_ua_ram[0x1000u + 2u * i] = (uint8_t)v;
        g_ua_ram[0x1001u + 2u * i] = (uint8_t)(v >> 8);
    }
    (void)armv7m_run(c, 16u, &done);
}

static void test_it_board_answers(void)
{
    static const struct {
        uint16_t hw[4];
        uint32_t n;
        uint32_t r0; /* in */
        uint32_t r0_z0, r0_z1; /* out, as the board left it */
        uint32_t r1_z0, r1_z1;
    } k[] = {
        /* itt al; it eq; mov r0, #1 -- the inner IT replaces the block */
        {{0xbfe4, 0xbf08, 0x2001}, 3u, 0x55u, 0x55u, 1u, 0x11u, 0x11u},
        /* it eq; bne +0; movs r0, #1 -- bne on its own condition */
        {{0xbf08, 0xd100, 0x2001}, 3u, 0x55u, 0x55u, 1u, 0x11u, 0x11u},
        /* it al; beq +0; movs r0, #1 */
        {{0xbfe8, 0xd000, 0x2001}, 3u, 0x55u, 1u, 0x55u, 0x11u, 0x11u},
        /* it eq; cbnz r0, +0; movs r0, #1 -- cbnz ignores the block */
        {{0xbf08, 0xb900, 0x2001}, 3u, 0x55u, 0x55u, 0x55u, 0x11u, 0x11u},
        /* ite eq; b +0; mov r0, #1; movs r1, #7 -- a taken branch ends
         * the block, so the movs it lands on is not a member */
        {{0xbf0c, 0xe000, 0x2001, 0x2107}, 4u, 0x55u, 1u, 0x55u, 7u, 7u},
        /* it with firstcond 1111: always */
        {{0xbff8, 0x2001}, 2u, 0x55u, 1u, 1u, 0x11u, 0x11u},
        /* ite al; mov r0, #1; mov r1, #2: the else is 1111, also always */
        {{0xbfec, 0x2001, 0x2102}, 3u, 0x55u, 1u, 1u, 2u, 2u},
        /* itt eq; bx r0; mov r0, #1 under movs r2, #1 (Z clear): the bx
         * is skipped and still ends the block, so the mov runs */
        {{0x2201, 0xbf04, 0x4700, 0x2001}, 4u, 0x55u, 1u, 1u, 0x11u, 0x11u},
        /* the same for pop {pc}, and for a skipped b */
        {{0x2201, 0xbf04, 0xbd00, 0x2001}, 4u, 0x55u, 1u, 1u, 0x11u, 0x11u},
        {{0x2201, 0xbf04, 0xe000, 0x2001}, 4u, 0x55u, 1u, 1u, 0x11u, 0x11u},
        /* and a skipped ordinary member does not: the mov stays a member */
        {{0x2201, 0xbf04, 0x2102, 0x2001},
         4u,
         0x55u,
         0x55u,
         0x55u,
         0x11u,
         0x11u},
    };

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(
        emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    for (uint32_t i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        for (uint32_t z = 0u; z < 2u; z++) {
            run_seq(k[i].hw, k[i].n, z != 0u, k[i].r0);
            CHECK_EQ(g_ua_cpu.cfsr, 0u);
            CHECK_EQ(g_ua_cpu.r[0], z ? k[i].r0_z1 : k[i].r0_z0);
            CHECK_EQ(g_ua_cpu.r[1], z ? k[i].r1_z1 : k[i].r1_z0);
            CHECK(!armv7m_in_it(g_ua_cpu.xpsr));
        }
    }
    /*
     * itt eq; bne +0; movs r0, #1 with Z set: the bne is not taken and
     * still ends the block, so the movs runs outside it and clears Z --
     * a member would have left the flags alone. The board's answer.
     */
    {
        static const uint16_t hw[3] = {0xbf04, 0xd100, 0x2001};

        run_seq(hw, 3u, true, 0x55u);
        CHECK_EQ(g_ua_cpu.r[0], 1u);
        CHECK_EQ(g_ua_cpu.xpsr & ARMV7M_Z, 0u);
    }
}

/*
 * Every other instruction that can write the pc, inside an IT block and
 * not last: ADD pc, POP and POP.W, LDM.W and LDR.W into pc, TBB, TBH,
 * and BX, BLX and MOV pc *taken* -- the ones test_it_board_answers did
 * not ask. The F746's answer is the same for all ten: the block ends
 * whether the instruction is skipped by the condition or taken
 * (tests/armv7m-diff, build/armv7m-it7, 2026-10-26).
 *
 * Each sequence sets Z itself -- movs r2, #1 to skip under `itt eq`,
 * movs r2, #0 to take -- and computes any target from `mov r5, pc`, so
 * it runs anywhere. A taken branch lands on movs r1, #7, which clears Z
 * only if it runs outside the block; a skipped one is followed by
 * movs r0, #1, which runs only if the block ended there. For TBB and
 * TBH skipped, the instruction after is the table, 0x0002 -- movs r2,
 * r0 -- and r2 says which. The stack pushes are balanced by add sp.
 */
static void test_it_pc_writers_board_answers(void)
{
    static const struct {
        uint16_t hw[11];
        uint32_t n;
        uint32_t r0, r2; /* as the board left them; r1 is always 7 */
    } k[] = {
        /* skipped: the next instruction runs, outside the block */
        {{0x467d, 0x3509, 0x2201, 0xbf04, 0x4728, 0x2001, 0x2107}, 7u, 1u, 1u},
        {{0x467d, 0x3509, 0x2201, 0xbf04, 0x47a8, 0x2001, 0x2107}, 7u, 1u, 1u},
        {{0x467d, 0x3509, 0x2201, 0xbf04, 0x46af, 0x2001, 0x2107}, 7u, 1u, 1u},
        {{0x2300, 0x2201, 0xbf04, 0x449f, 0x2001, 0x2107}, 6u, 1u, 1u},
        {{0x467d, 0x350b, 0xb420, 0x2201, 0xbf04, 0xbd00, 0x2001, 0x2107,
          0xb001},
         9u,
         1u,
         1u},
        {{0x467d, 0x350d, 0xb430, 0x2201, 0xbf04, 0xe8bd, 0x8010, 0x2001,
          0x2107, 0xb002},
         10u,
         1u,
         1u},
        {{0x467d, 0x350d, 0xb430, 0x2201, 0xbf04, 0xe89d, 0x8010, 0x2001,
          0x2107, 0xb002},
         10u,
         1u,
         1u},
        {{0x467d, 0x350d, 0xb420, 0x2201, 0xbf04, 0xf8dd, 0xf000, 0x2001,
          0x2107, 0xb001},
         10u,
         1u,
         1u},
        {{0x2700, 0x2201, 0xbf04, 0xe8df, 0xf007, 0x0002, 0x2001, 0x2107},
         8u,
         1u,
         0x55u}, /* the table ran: movs r2, r0 */
        {{0x2700, 0x2201, 0xbf04, 0xe8df, 0xf017, 0x0002, 0x2001, 0x2107},
         8u,
         1u,
         0x55u},
        /* taken: lands on movs r1, #7, outside the block */
        {{0x467d, 0x3509, 0x2200, 0xbf04, 0x4728, 0x2001, 0x2107},
         7u,
         0x55u,
         0u},
        {{0x467d, 0x3509, 0x2200, 0xbf04, 0x47a8, 0x2001, 0x2107},
         7u,
         0x55u,
         0u},
        {{0x467d, 0x3509, 0x2200, 0xbf04, 0x46af, 0x2001, 0x2107},
         7u,
         0x55u,
         0u},
        {{0x2300, 0x2200, 0xbf04, 0x449f, 0x2001, 0x2107}, 6u, 0x55u, 0u},
        {{0x467d, 0x350b, 0xb420, 0x2200, 0xbf04, 0xbd00, 0x2001, 0x2107},
         8u,
         0x55u,
         0u},
        {{0x467d, 0x350d, 0xb430, 0x2200, 0xbf04, 0xe8bd, 0x8010, 0x2001,
          0x2107},
         9u,
         0x55u,
         0u},
        {{0x467d, 0x350d, 0xb430, 0x2200, 0xbf04, 0xe89d, 0x8010, 0x2001,
          0x2107, 0xb002},
         10u,
         0x55u,
         0u},
        {{0x467d, 0x350d, 0xb420, 0x2200, 0xbf04, 0xf8dd, 0xf000, 0x2001,
          0x2107, 0xb001},
         10u,
         0x55u,
         0u},
        {{0x2700, 0x2200, 0xbf04, 0xe8df, 0xf007, 0x0002, 0x2001, 0x2107},
         8u,
         0x55u,
         0u},
        {{0x2700, 0x2200, 0xbf04, 0xe8df, 0xf017, 0x0002, 0x2001, 0x2107},
         8u,
         0x55u,
         0u},
    };

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(
        emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    for (uint32_t i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        run_seq(k[i].hw, k[i].n, false, 0x55u);
        CHECK_EQ(g_ua_cpu.cfsr, 0u);
        CHECK_EQ(g_ua_cpu.r[0], k[i].r0);
        CHECK_EQ(g_ua_cpu.r[1], 7u);
        CHECK_EQ(g_ua_cpu.r[2], k[i].r2);
        CHECK_EQ(g_ua_cpu.xpsr & ARMV7M_Z, 0u); /* movs r1 set the flags */
        CHECK_EQ(g_ua_cpu.r[ARMV7M_SP], UA_RAM_BASE + 0xF000u);
        CHECK(!armv7m_in_it(g_ua_cpu.xpsr));
    }
}

/*
 * A vector that looks like EXC_RETURN is an address, not a return.
 *
 * Exception entry *branches* to the vector (ExceptionTaken, BranchTo);
 * only POP, LDM, LDR and BX make an exception return. So a handler
 * address of 0xFFFFB88E is fetched, and the fetch faults on the
 * execute-never region: UDF -> HardFault (UsageFault not enabled) -> a
 * fetch fault at HardFault's priority -> lockup. The run loop read it
 * as a return instead, the return failed, the fault went back to the
 * same vector, and the slice never ended -- fuzz-armv7m-jit found it as
 * a hang, armv7m-jit/vector-exc-return-shape.
 *
 * **With the fix reverted this test does not fail, it does not return**:
 * the unit binary hangs here, which is the defect. Run it under a
 * timeout when checking that.
 */
static void test_vector_shaped_like_exc_return(void)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    uint32_t done = 0u;
    emu_run_reason_t why;

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(
        emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    for (uint32_t i = 1u; i < 16u; i++) { /* every vector but the SP */
        const uint32_t v = 0xFFFFB88Fu;

        memcpy(&g_ua_ram[4u * i], &v, 4u);
    }
    g_ua_ram[0x1000u] = 0x00u; /* udf #0 */
    g_ua_ram[0x1001u] = 0xDEu;

    armv7m_reset_state(c);
    c->bus = &g_ua_bus;
    c->state = EMU_STATE_RUNNING;
    c->vtor = UA_RAM_BASE;
    c->r[ARMV7M_SP] = UA_RAM_BASE + 0xF000u;
    c->msp = UA_RAM_BASE + 0xF000u;
    c->r[ARMV7M_PC] = UA_CODE;
    why = armv7m_run(c, 64u, &done);

    CHECK(why != EMU_RUN_BUDGET); /* it ended, and not by running out */
    CHECK(c->lockup);
    CHECK_EQ(c->xpsr & 0x1FFu, 3u); /* in HardFault */
    CHECK_EQ(c->cfsr & 0x00010001u, 0x00010001u); /* UNDEFINSTR, IACCVIOL */
    CHECK_EQ(c->hfsr & 0x40000000u, 0x40000000u); /* FORCED */
}

/*
 * gdb's d registers, against the single-precision file they are made of.
 *
 * Not through gdb: gdb builds s0..s31 from d0..d15 itself, so a session
 * shows a swapped pairing as consistent nonsense -- `p $s0` agrees with
 * `p $d0` whichever half the stub put first. The architecture's pairing
 * is the thing to hold it to: D<n> is S<2n+1>:S<2n>.
 */
const emu_gdb_target_t *armv7m_gdb_target(void);

static void test_gdb_fp_registers(void)
{
    const emu_gdb_target_t *const t = armv7m_gdb_target();
    armv7m_cpu_t *const c = &g_ua_cpu;
    emu_cpu_t *const cpu = (emu_cpu_t *)(void *)c;

    memset(c->s, 0, sizeof(c->s));
    c->s[0] = 0x3F800000u; /* 1.0f, the low half of d0 */
    c->s[1] = 0x40000000u; /* 2.0f, the high half */
    c->s[31] = 0xDEADBEEFu;
    CHECK(t->reg_width != NULL);
    CHECK_EQ(t->reg_width[17], 8u); /* d0 */
    CHECK_EQ(t->reg_width[33], 4u); /* fpscr */
    CHECK_EQ64(t->reg_get(cpu, 17u), 0x400000003F800000ull);
    CHECK_EQ64(t->reg_get(cpu, 32u) >> 32, 0xDEADBEEFu); /* d15's top */

    t->reg_set(cpu, 18u, 0x1122334455667788ull); /* d1 */
    CHECK_EQ(c->s[2], 0x55667788u);
    CHECK_EQ(c->s[3], 0x11223344u);

    /* FPSCR takes what VMSR would: the reserved bits stay clear. */
    t->reg_set(cpu, 33u, 0xFFFFFFFFu);
    CHECK_EQ(c->fpscr, ARMV7M_FPSCR_WRITABLE);
    CHECK_EQ64(t->reg_get(cpu, 33u), ARMV7M_FPSCR_WRITABLE);
}

/*
 * A fetch from the default map's execute-never space faults, even from
 * memory the bus serves from its cache.
 *
 * The inline fetch answers from the bus's fetch cache when the MPU is
 * off, so it has to apply the execute-never test itself. A fetch that
 * misses the cache goes to the checked path, which refuses it -- so the
 * case only the inline test can get wrong is a region that *straddles*
 * the line: fetching its lower half fills the cache with all of it.
 * tests/armv7m-diff's `xn-default` fetches from the peripherals, which
 * no cache holds, and passed with the inline test removed.
 *
 * This region runs from 0x3FFFF000 to 0x40001000, with a NOP either
 * side of 0x40000000. The first two retire; the third faults, MemManage
 * escalated to HardFault, whose handler is a BKPT.
 */
#define XN_RAM_BASE 0x3FFFF000u

static uint8_t g_xn_ram[0x2000];
static emu_bus_t g_xn_bus;

static void put16(uint8_t *m, uint32_t off, uint16_t v)
{
    m[off] = (uint8_t)v;
    m[off + 1u] = (uint8_t)(v >> 8);
}

static void test_fetch_execute_never(void)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    const uint32_t handler = UA_CODE;
    uint32_t n = 0u;
    uint32_t stacked_pc = 0u;

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    memset(g_xn_ram, 0, sizeof(g_xn_ram));
    emu_bus_init(&g_xn_bus);
    CHECK(
        emu_bus_add_ram(&g_xn_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    CHECK(emu_bus_add_ram(&g_xn_bus, "xn", XN_RAM_BASE, g_xn_ram,
                          sizeof(g_xn_ram)));

    put16(g_ua_ram, 3u * 4u, (uint16_t)(handler | 1u)); /* HardFault */
    put16(g_ua_ram, 3u * 4u + 2u, (uint16_t)(handler >> 16));
    put16(g_ua_ram, handler - UA_RAM_BASE, 0xBE00u); /* bkpt */
    for (uint32_t pc = 0x3FFFFFFCu; pc <= 0x40000000u; pc += 2u) {
        put16(g_xn_ram, pc - XN_RAM_BASE, 0xBF00u); /* nop */
    }

    armv7m_reset_state(c);
    c->bus = &g_xn_bus;
    c->state = EMU_STATE_RUNNING;
    c->vtor = UA_RAM_BASE;
    c->r[ARMV7M_SP] = UA_RAM_BASE + 0xF000u;
    c->r[ARMV7M_PC] = 0x3FFFFFFCu;

    (void)armv7m_run(c, 100u, &n);

    CHECK_EQ(c->state, EMU_STATE_HALTED);
    CHECK_EQ(n, 3u); /* two NOPs, then the handler's BKPT */
    CHECK_EQ(c->cfsr & 0xFFu, 0x01u); /* IACCVIOL, and MMFAR not valid */
    CHECK_EQ(c->r[ARMV7M_PC], handler);
    for (uint32_t i = 0u; i < 4u; i++) {
        stacked_pc |=
            (uint32_t)g_ua_ram[c->r[ARMV7M_SP] - UA_RAM_BASE + 24u + i]
            << (8u * i);
    }
    CHECK_EQ(stacked_pc, 0x40000000u);
}

/*
 * AIRCR.SYSRESETREQ stops the core and says why: the status the run
 * summary prints from names it. Halting with nothing printed made a
 * guest's reset request read like a guest that finished. And without
 * VECTKEY the write is ignored (B3.2.6), which is the case that matters
 * for a reset: nothing else may cause one.
 */
extern const emu_cpu_ops_t armv7m_frontend;

static void test_sysresetreq_reported(void)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    emu_cpu_status_t st;

    armv7m_reset_state(c);
    c->state = EMU_STATE_RUNNING;
    c->access_priv = true; /* what armv7m_mem_write sets around a store */
    CHECK(armv7m_scs_ops.write(c, 0xD0Cu, 4u, 0x00000004u) == EMU_FAULT_NONE);
    CHECK_EQ(c->state, EMU_STATE_RUNNING); /* no VECTKEY */
    CHECK(armv7m_scs_ops.write(c, 0xD0Cu, 4u, 0x05FA0004u) == EMU_FAULT_NONE);
    CHECK_EQ(c->state, EMU_STATE_HALTED);
    memset(&st, 0xA5, sizeof(st));
    armv7m_frontend.status((const emu_cpu_t *)(const void *)c, &st);
    CHECK(st.reset_by != NULL && strcmp(st.reset_by, "SYSRESETREQ") == 0);

    armv7m_reset_state(c);
    memset(&st, 0xA5, sizeof(st));
    armv7m_frontend.status((const emu_cpu_t *)(const void *)c, &st);
    CHECK(st.reset_by == NULL); /* and a reset clears it */
}

/*
 * `pending_any` is the OR of `pending`, after everything that changes
 * it: the interpreter tests it instead of the words before each
 * instruction, so a writer that skipped it would lose an exception. A
 * random walk over the exception model -- pend, unpend, raise and drop
 * a level line, take what is pending or a synchronous fault (sometimes
 * onto a stack that faults, which leaves the original pending), return
 * (which re-pends a line still asserted) -- checked after every step,
 * in both directions.
 *
 * Bypassing the summary in `armv7m_take`'s stacking-fault path or in
 * `deactivate` passed every other test in the tree; this is the test
 * for them, and it counts that it reached both.
 */
static bool any_pending(const armv7m_cpu_t *c)
{
    uint32_t any = 0u;

    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        any |= c->pending[w];
    }
    return any != 0u;
}

static void pend_walk_setup(armv7m_cpu_t *c)
{
    armv7m_reset_state(c);
    c->bus = &g_ua_bus;
    c->state = EMU_STATE_RUNNING;
    c->vtor = UA_RAM_BASE;
    c->msp = c->r[ARMV7M_SP] = UA_RAM_BASE + 0xF000u;
    c->r[ARMV7M_PC] = UA_CODE;
    /* Every line and the configurable faults enabled, so that what is
     * pending can be taken. */
    memset(c->enabled, 0xFF, sizeof(c->enabled));
    c->shcsr |= 0x00070000u; /* MEMFAULTENA, BUSFAULTENA, USGFAULTENA */
}

static void test_pending_summary(void)
{
    armv7m_cpu_t *const c = &g_ua_cpu;
    uint32_t seed = 12345u;
    uint32_t wrong = 0u;
    uint32_t repends = 0u;
    uint32_t stack_faults = 0u;
    uint32_t takes = 0u;
    uint32_t sync_left = 0u;

    memset(g_ua_ram, 0, sizeof(g_ua_ram));
    emu_bus_init(&g_ua_bus);
    CHECK(
        emu_bus_add_ram(&g_ua_bus, "ram", UA_RAM_BASE, g_ua_ram, UA_RAM_SIZE));
    for (uint32_t v = 1u; v < ARMV7M_NEXC; v++) {
        const uint32_t handler = UA_RAM_BASE + 0x801u;

        memcpy(&g_ua_ram[v * 4u], &handler, 4u);
    }
    pend_walk_setup(c);

    for (uint32_t i = 0u; i < 20000u; i++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t r = seed >> 8;

        switch (r % 6u) {
        case 0u:
            armv7m_set_pending(c, 2u + (r >> 4) % (ARMV7M_NEXC - 2u),
                               ((r >> 12) & 1u) != 0u);
            break;
        case 1u:
            armv7m_set_irq(c, (r >> 4) % 8u, ((r >> 12) & 3u) != 0u);
            break;
        case 2u: {
            /* Take something pending -- a quarter of the time onto a
             * stack with no memory under it. */
            bool preempts = false;
            const uint32_t exc = armv7m_pending_exc(c, &preempts);
            const bool bad = ((r >> 4) & 3u) == 0u;

            if (exc == 0u) {
                break;
            }
            if (bad) {
                c->r[ARMV7M_SP] = 0x00000100u;
            }
            takes++;
            (void)armv7m_take(c, exc, c->r[ARMV7M_PC]);
            if (bad && armv7m_is_pending(c, exc)) {
                stack_faults++;
            }
            break;
        }
        case 4u: {
            /* A synchronous one, never pending, as armv7m_raise takes a
             * fault or an SVC -- onto a bad stack half the time, which
             * is what leaves it pending afterwards. */
            static const uint32_t sync[] = {3u, 4u, 5u, 6u, 11u};
            const uint32_t exc = sync[(r >> 4) % 5u];

            if (((r >> 8) & 1u) != 0u) {
                c->r[ARMV7M_SP] = 0x00000100u;
            }
            takes++;
            (void)armv7m_take(c, exc, c->r[ARMV7M_PC]);
            if (armv7m_is_pending(c, exc)) {
                sync_left++;
            }
            break;
        }
        case 3u:
            if (armv7m_handler_mode(c) && c->r[ARMV7M_SP] >= UA_RAM_BASE) {
                const uint32_t exc = c->xpsr & ARMV7M_IPSR_MASK;

                (void)armv7m_exc_return(c, c->r[ARMV7M_LR]);
                if (exc >= ARMV7M_EXC_EXTERNAL && armv7m_is_pending(c, exc)) {
                    repends++;
                }
            }
            break;
        default:
            if (((r >> 4) & 63u) == 0u) {
                pend_walk_setup(c);
            }
            break;
        }
        /* Checked before a lockup is cleared away: a locked-up core runs
         * nothing, but the summary is still meant to be the OR. */
        if (c->pending_any != any_pending(c)) {
            wrong++;
        }
        if (c->state != EMU_STATE_RUNNING || c->r[ARMV7M_SP] < UA_RAM_BASE) {
            pend_walk_setup(c); /* a lockup, or a frame that never was */
        }
    }
    CHECK_EQ(wrong, 0u);
    if (getenv("ARMV7M_WALK") != NULL) {
        printf("    pending walk: %u takes, %u re-pends, %u stacking faults, "
               "%u synchronous left pending\n",
               (unsigned)takes, (unsigned)repends, (unsigned)stack_faults,
               (unsigned)sync_left);
    }
    /* And that it went where it was meant to. */
    CHECK(takes > 1000u);
    CHECK(repends > 50u);
    CHECK(stack_faults > 50u);
    CHECK(sync_left > 50u);
}

void test_armv7m(void)
{
    test_pending_summary();
    test_sysresetreq_reported();
    test_fetch_execute_never();
    test_gdb_fp_registers();
    test_insn_len_exhaustive();
    test_insn_len_boundary();
    test_decode_agrees_on_undefined();
    test_sp_pc_board_answers();
    test_it_board_answers();
    test_it_pc_writers_board_answers();
    test_vector_shaped_like_exc_return();
}
