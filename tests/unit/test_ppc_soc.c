/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ppc_soc.c - the MPC5675K's STM and INTC, as devices.
 *
 * Every expectation is the Reference Manual's (Rev. 10, docs/nxp), with
 * the section beside it, or one of the choices ppc_stm.c and ppc_intc.c
 * state where the manual says nothing -- marked "choice" here. None was
 * read back from the implementation: there is no MPC5675K attached, so
 * a value taken from the code would be the code agreeing with itself.
 *
 * The devices are driven through their bus callbacks and with no core,
 * so the clock is ppc_stm_advance and nothing else. What the core does
 * with them -- the stretch ending on a match, the hardware vector, a
 * wait carried to the next match -- is tests/guest/ppc/intc.c's.
 */

#include "tests.h"

#include "ppc/ppc_intc.h"
#include "ppc/ppc_stm.h"

#include <string.h>

static ppc_stm_t g_stm;
static ppc_intc_t g_intc;

static uint32_t rd(const emu_dev_ops_t *ops, void *dev, uint32_t off,
                   uint32_t size, bool *ok)
{
    uint32_t v = 0xDEADBEEFu;

    *ok = ops->read(dev, off, size, &v) == EMU_FAULT_NONE;
    return v;
}

static uint32_t stm_rd(uint32_t off)
{
    bool ok;
    const uint32_t v = rd(&ppc_stm_ops, &g_stm, off, 4u, &ok);

    CHECK(ok);
    return v;
}

static void stm_wr(uint32_t off, uint32_t v)
{
    CHECK(ppc_stm_ops.write(&g_stm, off, 4u, v) == EMU_FAULT_NONE);
}

static uint32_t ic_rd(uint32_t off, uint32_t size)
{
    bool ok;
    const uint32_t v = rd(&ppc_intc_ops, &g_intc, off, size, &ok);

    CHECK(ok);
    return v;
}

static void ic_wr(uint32_t off, uint32_t size, uint32_t v)
{
    CHECK(ppc_intc_ops.write(&g_intc, off, size, v) == EMU_FAULT_NONE);
}

static bool source(uint32_t n)
{
    return ((g_intc.flag[n >> 5] >> (n & 31u)) & 1u) != 0u;
}

static void fresh(void)
{
    memset(&g_stm, 0, sizeof(g_stm));
    memset(&g_intc, 0, sizeof(g_intc));
    ppc_intc_reset(&g_intc);
    g_stm.intc = &g_intc;
    ppc_stm_reset(&g_stm);
}

/* ------------------------------------------------------------------ */
/* STM                                                                 */
/* ------------------------------------------------------------------ */

/* 51.3: word accesses only, and a reserved address is a bus error too. */
static void test_stm_access(void)
{
    static const uint32_t reserved[] = {0x08u, 0x0Cu, 0x1Cu, 0x2Cu,
                                        0x3Cu, 0x4Cu, 0x50u, 0x3FFCu};
    bool ok;
    uint32_t v = 0u;

    fresh();
    for (uint32_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        (void)rd(&ppc_stm_ops, &g_stm, reserved[i], 4u, &ok);
        CHECK(!ok);
        CHECK(ppc_stm_ops.write(&g_stm, reserved[i], 4u, 0u) != EMU_FAULT_NONE);
    }
    (void)rd(&ppc_stm_ops, &g_stm, PPC_STM_CR + 2u, 2u, &ok); /* CPS's half */
    CHECK(!ok);
    (void)rd(&ppc_stm_ops, &g_stm, PPC_STM_CR + 3u, 1u, &ok);
    CHECK(!ok);
    CHECK(ppc_stm_ops.write(&g_stm, PPC_STM_CNT + 2u, 2u, 1u) !=
          EMU_FAULT_NONE);
    CHECK(ppc_stm_ops.read(&g_stm, PPC_STM_CMP(3), 4u, &v) == EMU_FAULT_NONE);

    /* Table 51-2: everything resets to zero. Figure 51-1: CR holds CPS,
     * FRZ and TEN and nothing else. */
    CHECK_EQ(stm_rd(PPC_STM_CR), 0u);
    stm_wr(PPC_STM_CR, 0xFFFFFFFFu);
    CHECK_EQ(stm_rd(PPC_STM_CR), 0x0000FF03u);
    stm_wr(PPC_STM_CCR(1), 0xFFFFFFFFu);
    CHECK_EQ(stm_rd(PPC_STM_CCR(1)), 1u);
    stm_wr(PPC_STM_CMP(2), 0xA5A5A5A5u);
    CHECK_EQ(stm_rd(PPC_STM_CMP(2)), 0xA5A5A5A5u);
    CHECK_EQ(stm_rd(PPC_STM_CMP(1)), 0u); /* not its neighbour */
}

/* 51.4: system clock over CPS + 1, only while TEN is set. */
static void test_stm_counts(void)
{
    fresh();
    ppc_stm_advance(&g_stm, 100u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 0u); /* TEN clear: stopped */

    stm_wr(PPC_STM_CR, PPC_STM_CR_TEN);
    ppc_stm_advance(&g_stm, 10u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 10u);

    stm_wr(PPC_STM_CNT, 0u);
    stm_wr(PPC_STM_CR, (3u << PPC_STM_CR_CPS_SHIFT) | PPC_STM_CR_TEN); /* /4 */
    ppc_stm_advance(&g_stm, 10u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 2u); /* 10 ticks: two increments, 2 over */
    ppc_stm_advance(&g_stm, 2u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 3u); /* the 2 carried make a third */

    /* "rolls over at 0xFFFF_FFFF to 0x0000_0000" */
    stm_wr(PPC_STM_CR, PPC_STM_CR_TEN);
    stm_wr(PPC_STM_CNT, 0xFFFFFFFEu);
    ppc_stm_advance(&g_stm, 3u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 1u);

    /* 255: divide by 256, the largest. */
    stm_wr(PPC_STM_CNT, 0u);
    stm_wr(PPC_STM_CR, PPC_STM_CR_CPS | PPC_STM_CR_TEN);
    ppc_stm_advance(&g_stm, 255u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 0u);
    ppc_stm_advance(&g_stm, 1u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 1u);
}

/*
 * 51.3.2.5 and 51.4: an enabled channel sets CIF and requests on a
 * match, the request being INTC source 30 + n (table 32-10), cleared by
 * writing 1 to CIF.
 */
static void test_stm_match(void)
{
    fresh();
    stm_wr(PPC_STM_CMP(2), 5u);
    stm_wr(PPC_STM_CCR(2), PPC_STM_CCR_CEN);
    stm_wr(PPC_STM_CR, PPC_STM_CR_TEN);
    CHECK_EQ(ppc_stm_until(&g_stm), 5u);

    ppc_stm_advance(&g_stm, 4u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 0u);
    CHECK(!source(32u));
    ppc_stm_advance(&g_stm, 1u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 1u);
    CHECK(source(32u));
    CHECK(!source(30u) && !source(31u) && !source(33u));
    /* Flagged: matching again would change nothing, so no stop owed. */
    CHECK_EQ(ppc_stm_until(&g_stm), 0xFFFFFFFFu);

    stm_wr(PPC_STM_CIR(2), 0u); /* "A write of 0 ... has no effect" */
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 1u);
    stm_wr(PPC_STM_CIR(2), 1u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 0u);
    CHECK(!source(32u));

    /* Choice: reaching, not landing -- a slice that passes the value
     * still matches. */
    stm_wr(PPC_STM_CNT, 0u);
    ppc_stm_advance(&g_stm, 1000u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 1u);
    stm_wr(PPC_STM_CIR(2), 1u);

    /* Across the roll-over. */
    stm_wr(PPC_STM_CNT, 0xFFFFFFFEu);
    stm_wr(PPC_STM_CMP(2), 1u);
    CHECK_EQ(ppc_stm_until(&g_stm), 3u);
    ppc_stm_advance(&g_stm, 2u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 0u);
    ppc_stm_advance(&g_stm, 1u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 1u);
    stm_wr(PPC_STM_CIR(2), 1u);

    /* Choice: writing the counter onto the compare value matches
     * nothing; the next match is a whole turn away. */
    stm_wr(PPC_STM_CNT, 1u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 0u);
    CHECK_EQ(ppc_stm_until(&g_stm), 0xFFFFFFFFu);

    /* A disabled channel never matches. */
    stm_wr(PPC_STM_CCR(2), 0u);
    stm_wr(PPC_STM_CNT, 0u);
    ppc_stm_advance(&g_stm, 10u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(2)), 0u);

    /* Choice: the request is CIF, so clearing CEN does not withdraw one. */
    stm_wr(PPC_STM_CCR(0), PPC_STM_CCR_CEN);
    stm_wr(PPC_STM_CMP(0), 12u);
    ppc_stm_advance(&g_stm, 2u);
    CHECK(source(30u));
    stm_wr(PPC_STM_CCR(0), 0u);
    CHECK(source(30u));
    CHECK_EQ(stm_rd(PPC_STM_CIR(0)), 1u);
}

/* The next stop under a prescaler counts the ticks already in it. */
static void test_stm_until_prescaled(void)
{
    fresh();
    stm_wr(PPC_STM_CMP(1), 3u);
    stm_wr(PPC_STM_CCR(1), PPC_STM_CCR_CEN);
    stm_wr(PPC_STM_CMP(3), 2u);
    stm_wr(PPC_STM_CR, (9u << PPC_STM_CR_CPS_SHIFT) | PPC_STM_CR_TEN); /* /10 */
    CHECK_EQ(ppc_stm_until(&g_stm), 30u); /* channel 3 is not enabled */
    ppc_stm_advance(&g_stm, 7u);
    CHECK_EQ(ppc_stm_until(&g_stm), 23u);
    ppc_stm_advance(&g_stm, 22u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(1)), 0u); /* one tick short */
    ppc_stm_advance(&g_stm, 1u);
    CHECK_EQ(stm_rd(PPC_STM_CIR(1)), 1u);
    CHECK_EQ(stm_rd(PPC_STM_CNT), 3u);

    /* Choice: a CR write that changes neither CPS nor enables the timer
     * leaves the prescaler counting. */
    stm_wr(PPC_STM_CIR(1), 1u);
    stm_wr(PPC_STM_CMP(1), 4u);
    ppc_stm_advance(&g_stm, 4u);
    stm_wr(PPC_STM_CR,
           (9u << PPC_STM_CR_CPS_SHIFT) | PPC_STM_CR_TEN | PPC_STM_CR_FRZ);
    CHECK_EQ(ppc_stm_until(&g_stm), 6u);
    /* ...and one that changes CPS restarts it. */
    stm_wr(PPC_STM_CR, (4u << PPC_STM_CR_CPS_SHIFT) | PPC_STM_CR_TEN);
    CHECK_EQ(ppc_stm_until(&g_stm), 5u);
}

/* ------------------------------------------------------------------ */
/* INTC                                                                */
/* ------------------------------------------------------------------ */

/* 32.3.2: any width within a word, a halfword in the middle included. */
static void test_intc_access(void)
{
    bool ok;

    fresh();
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 15u); /* table 32-2 */
    CHECK_EQ(ic_rd(PPC_INTC_CPR + 3u, 1u), 15u);
    CHECK_EQ(ic_rd(PPC_INTC_CPR + 1u, 2u), 0u);
    ic_wr(PPC_INTC_CPR + 3u, 1u, 0x27u); /* PRI is four bits */
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 7u);
    (void)rd(&ppc_intc_ops, &g_intc, PPC_INTC_CPR + 2u, 4u, &ok); /* crosses */
    CHECK(!ok);
    (void)rd(&ppc_intc_ops, &g_intc, 0x04u, 4u, &ok); /* reserved */
    CHECK(!ok);
    (void)rd(&ppc_intc_ops, &g_intc, 0x28u, 4u, &ok);
    CHECK(!ok);
    (void)rd(&ppc_intc_ops, &g_intc, 0x194u, 4u, &ok); /* past PSR336 */
    CHECK(!ok);

    /* BCR holds VTES and HVEN (figure 32-2). */
    ic_wr(PPC_INTC_BCR, 4u, 0xFFFFFFFFu);
    CHECK_EQ(ic_rd(PPC_INTC_BCR, 4u), 0x21u);

    /* PSR: a byte per source from 0x40, PRI in its low four bits; PSR336
     * is the first byte of the last word (figure 32-10). */
    ic_wr(PPC_INTC_PSR0 + 30u, 1u, 0xFFu);
    CHECK_EQ(ic_rd(PPC_INTC_PSR0 + 28u, 4u), 0x00000F00u);
    ic_wr(0x190u, 4u, 0x0A0B0C0Du);
    CHECK_EQ(ic_rd(0x190u, 4u), 0x0A000000u);
    CHECK_EQ(g_intc.psr[336], 10u);

    /* EOIR reads 0 (32.3.2.4). */
    CHECK_EQ(ic_rd(PPC_INTC_EOIR, 4u), 0u);
}

/* 32.4.2.1: the highest priority wins, then the lowest vector; 0 never. */
static void test_intc_arbitration(void)
{
    fresh();
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    ppc_intc_set_source(&g_intc, 40u, true); /* priority 0 */
    CHECK(!g_intc.req);

    ic_wr(PPC_INTC_PSR0 + 40u, 1u, 3u);
    CHECK(g_intc.req);
    CHECK_EQ(g_intc.intvec, 40u);

    /* A higher priority after the request asserted does not move the
     * latched vector (32.4.3.1.1: it "retains that value until the next
     * time the interrupt request to the processor is asserted"). */
    ic_wr(PPC_INTC_PSR0 + 50u, 1u, 9u);
    ppc_intc_set_source(&g_intc, 50u, true);
    CHECK_EQ(g_intc.intvec, 40u);
    /* A CPR write re-decides it: now the higher one. */
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    CHECK_EQ(g_intc.intvec, 50u);

    /* Equal priorities: the lower vector. */
    ic_wr(PPC_INTC_PSR0 + 40u, 1u, 9u);
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    CHECK_EQ(g_intc.intvec, 40u);

    /* "masks any ... request at the same or lower priority" (32.3.2.2) */
    ic_wr(PPC_INTC_CPR, 4u, 9u);
    CHECK(!g_intc.req);
    ic_wr(PPC_INTC_CPR, 4u, 8u);
    CHECK(g_intc.req);
}

/*
 * 32.2.1.1 and figure 32-11: in software vector mode reading IACKR
 * returns VTBA and the vector, pushes CPR, installs the request's
 * priority and negates the request; EOIR pops it back.
 */
static void test_intc_software_vector(void)
{
    fresh();
    ic_wr(PPC_INTC_IACKR, 4u, 0x12345FFFu); /* VTBA: bits 0:20 */
    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u), 0x12345800u);
    ic_wr(PPC_INTC_PSR0 + 31u, 1u, 5u);
    ic_wr(PPC_INTC_CPR, 4u, 1u);
    ppc_intc_set_source(&g_intc, 31u, true);
    CHECK(g_intc.req);

    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u), 0x12345800u | (31u << 2));
    CHECK(!g_intc.req);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 5u);
    /* Still asserted, and not above 5: no new request. */
    CHECK(!g_intc.req);
    /* Choice: a read with nothing asserted changes nothing. */
    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u), 0x12345800u | (31u << 2));
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 5u);

    ppc_intc_set_source(&g_intc, 31u, false); /* the ISR cleared it */
    ic_wr(PPC_INTC_EOIR, 4u, 0x12345678u); /* whatever is written */
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 1u);

    /* VTES: 8-byte entries, so INTVEC one bit up and VTBA 20 bits. */
    ic_wr(PPC_INTC_BCR, 4u, PPC_INTC_BCR_VTES);
    ppc_intc_set_source(&g_intc, 31u, true);
    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u), 0x12345000u | (31u << 3));

    /* 32.4.1: no spurious vector. A request that negates before it is
     * acknowledged is still the one acknowledged. */
    ic_wr(PPC_INTC_EOIR, 4u, 0u);
    ppc_intc_set_source(&g_intc, 31u, false);
    ic_wr(PPC_INTC_BCR, 4u, 0u);
    ppc_intc_set_source(&g_intc, 31u, true);
    ppc_intc_set_source(&g_intc, 31u, false);
    CHECK(g_intc.req);
    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u) & 0x7FCu, 31u << 2);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 5u);
}

/* 32.2.1.2: in hardware vector mode the core's acknowledge does it, and
 * an IACKR read has no side effect. */
static void test_intc_hardware_vector(void)
{
    fresh();
    ic_wr(PPC_INTC_BCR, 4u, PPC_INTC_BCR_HVEN);
    CHECK(ppc_intc_hw_vectored(&g_intc));
    ic_wr(PPC_INTC_PSR0 + 33u, 1u, 4u);
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    ppc_intc_set_source(&g_intc, 33u, true);
    CHECK(g_intc.req);
    CHECK_EQ(ic_rd(PPC_INTC_IACKR, 4u), 33u << 2);
    CHECK(g_intc.req);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 0u);

    CHECK_EQ(ppc_intc_ack(&g_intc), 33u);
    CHECK(!g_intc.req);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 4u);
    ic_wr(PPC_INTC_EOIR, 4u, 0u);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 0u);
    CHECK(g_intc.req); /* still asserted, and above 0 again */
}

/*
 * 32.4.2.2: 14 entries; pushing a full LIFO overwrites the oldest, and
 * popping an empty one gives 0. Fifteen nested acknowledgements push
 * 0, 1, ..., 14, so the pops give 14 down to 1 -- then 0, which was the
 * overwritten entry "regenerated with the popping of an empty LIFO".
 */
static void test_intc_lifo(void)
{
    fresh();
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    for (uint32_t p = 1u; p <= 15u; p++) {
        const uint32_t src = 100u + p;

        ic_wr(PPC_INTC_PSR0 + src, 1u, p);
        ppc_intc_set_source(&g_intc, src, true);
        CHECK(g_intc.req);
        CHECK_EQ((ic_rd(PPC_INTC_IACKR, 4u) >> 2) & 0x1FFu, src);
        CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), p);
    }
    for (uint32_t p = 14u; p >= 1u; p--) {
        ppc_intc_set_source(&g_intc, 100u + p + 1u, false);
        ic_wr(PPC_INTC_EOIR, 4u, 0u);
        CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), p);
    }
    ppc_intc_set_source(&g_intc, 101u, false);
    ic_wr(PPC_INTC_EOIR, 4u, 0u);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 0u);
    ic_wr(PPC_INTC_EOIR, 4u, 0u);
    CHECK_EQ(ic_rd(PPC_INTC_CPR, 4u), 0u);
}

/*
 * 32.3.2.5: SETx sets the flag and reads 0; CLRx is the flag, cleared by
 * writing 1; both at once set it. Software requests are vectors 0-7.
 */
static void test_intc_software_settable(void)
{
    fresh();
    ic_wr(PPC_INTC_SSCIR0_3, 1u, PPC_INTC_SSCIR_SET); /* SET0 */
    ic_wr(PPC_INTC_SSCIR4_7 + 3u, 1u, PPC_INTC_SSCIR_SET); /* SET7 */
    CHECK(source(0u) && source(7u) && !source(1u));
    CHECK_EQ(ic_rd(PPC_INTC_SSCIR0_3, 4u), 0x01000000u);
    CHECK_EQ(ic_rd(PPC_INTC_SSCIR4_7, 4u), 0x00000001u);
    ic_wr(PPC_INTC_SSCIR0_3, 4u, 0x01000000u); /* CLR0 */
    CHECK(!source(0u));
    ic_wr(PPC_INTC_SSCIR0_3, 4u, 0x00030000u); /* SET1 and CLR1 */
    CHECK(source(1u));
    /* A peripheral's call cannot reach a software request. */
    ppc_intc_set_source(&g_intc, 1u, false);
    CHECK(source(1u));

    /* And it requests like any other, at its PSR. */
    ic_wr(PPC_INTC_PSR0 + 1u, 1u, 2u);
    ic_wr(PPC_INTC_CPR, 4u, 0u);
    CHECK(g_intc.req);
    CHECK_EQ(g_intc.intvec, 1u);
}

void test_ppc_soc(void)
{
    test_stm_access();
    test_stm_counts();
    test_stm_match();
    test_stm_until_prescaled();
    test_intc_access();
    test_intc_arbitration();
    test_intc_software_vector();
    test_intc_hardware_vector();
    test_intc_lifo();
    test_intc_software_settable();
}
