/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ppc.c - the e200z7 frontend.
 *
 * The programs here are **words produced by binutils**, not hand-invented
 * constants: every one was assembled with
 *
 *     scripts/ppc-check-encodings.sh
 *
 * and pasted with the mnemonic beside it. That is the difference between
 * this frontend and G4MH's start, where three real bugs hid behind tests
 * that were self-consistent with a wrong reading of the manual.
 *
 * Note the byte order. A PowerPC image is big-endian, so a program is
 * laid out most-significant byte first and the bus reverses it on fetch.
 * emit() below writes bytes in that order deliberately: writing words
 * would make the test pass against a frontend that had byte order wrong
 * in both directions.
 */

#include "tests.h"

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"

#include <string.h>

#define TEST_RAM_SIZE  4096u

static uint8_t    g_ram[TEST_RAM_SIZE];
/* Reachable from a 16-bit program: 64 << 6. See load_and_run_vle. */
#define VLE_SCRATCH   0x1000u
static uint8_t    g_scratch[256];
static emu_bus_t  g_bus;
static emu_core_t g_core;

/* Lay instructions out big-endian, which is how a PowerPC image is. */
static void emit(const uint32_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        g_ram[i * 4u + 0u] = (uint8_t)(w[i] >> 24);
        g_ram[i * 4u + 1u] = (uint8_t)(w[i] >> 16);
        g_ram[i * 4u + 2u] = (uint8_t)(w[i] >> 8);
        g_ram[i * 4u + 3u] = (uint8_t)(w[i]);
    }
}

static bool load_and_run(const uint32_t *w, unsigned n, uint32_t budget,
                         emu_run_reason_t *why, uint32_t *retired)
{
    const emu_cpu_ops_t *ops = emu_frontend_find("ppc");
    if (ops == NULL) {
        return false;
    }

    memset(g_ram, 0, sizeof(g_ram));
    emit(w, n);

    emu_bus_init(&g_bus);
    if (!emu_bus_add_ram(&g_bus, "ram", EMU_GUEST_RAM_BASE, g_ram,
                         TEST_RAM_SIZE)) {
        return false;
    }
    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        return false;
    }
    emu_core_reset(&g_core, EMU_GUEST_RAM_BASE);
    emu_core_boot(&g_core, EMU_GUEST_RAM_BASE, TEST_RAM_SIZE);

    *why = emu_core_run(&g_core, budget, retired);
    return true;
}

static uint32_t reg(unsigned r)
{
    return g_core.ops->reg_read(g_core.cpu, r);
}

static const ppc_cpu_t *core(void)
{
    return (const ppc_cpu_t *)(const void *)g_core.cpu;
}

/*
 * The length rule, which everything else stands on. Derived from the
 * assembler across thirteen values of the top four bits, so it is checked
 * against that table rather than against itself.
 *
 * 0x9, 0xB and 0xD are the ones that matter: they have bit 0 set and are
 * *16-bit*, so a `top4 & 1` reading -- which the layout invites --
 * desynchronises on se_lwz (0xC0..) and se_stw (0xD0..).
 */
static void test_vle_length(void)
{
    static const struct { uint16_t w0; unsigned len; const char *what; } k[] = {
        { 0x0143u, 2u, "se_mr r3,r4"       },
        { 0x0443u, 2u, "se_add r3,r4"      },
        { 0x2033u, 2u, "se_addi r3,4"      },
        { 0x4853u, 2u, "se_li r3,5"        },
        { 0x6043u, 2u, "se_bclri r3,4"     },
        { 0xC034u, 2u, "se_lwz r3,0(r4)"   },
        { 0xD034u, 2u, "se_stw r3,0(r4)"   },
        { 0xE800u, 2u, "se_b"              },
        { 0x1C64u, 4u, "e_add16i r3,r4,100"},
        { 0x1803u, 4u, "e_cmpi"            },
        { 0x5064u, 4u, "e_lwz r3,8(r4)"    },
        { 0x7060u, 4u, "e_li r3,1000"      },
        { 0x7800u, 4u, "e_b"               },
        { 0x7C64u, 4u, "add r3,r4,r5"      },
    };

    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        CHECK_EQ(ppc_vle_len(k[i].w0), k[i].len);
    }
}

/*
 * Integer arithmetic and the rA==0 rule.
 *
 * `li r3,100` is `addi r3,0,100`, and rA==0 there means the *literal*
 * zero rather than r0 -- PowerPC's one pervasive irregularity. The test
 * puts a poison value in r0 first, so an implementation that read r[0]
 * gets 0x1234 + 100 instead of 100 and is caught.
 */
static void test_alu(void)
{
    static const uint32_t prog[] = {
        0x38001234u,   /* li    r0,0x1234        -- poison r0            */
        0x38600064u,   /* li    r3,100                                   */
        0x38800007u,   /* li    r4,7                                     */
        0x7CA32214u,   /* add   r5,r3,r4         -- 107                  */
        0x7CC32050u,   /* subf  r6,r3,r4         -- 7 - 100 = -93        */
        0x7C671B78u,   /* or    r7,r3,r3         -- mr r7,r3             */
        0x7C881838u,   /* and   r8,r4,r3         -- 7 & 100 = 4          */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(3), 100u);                   /* rA==0 is literal zero  */
    CHECK_EQ(reg(4), 7u);
    CHECK_EQ(reg(5), 107u);
    CHECK_EQ(reg(6), (uint32_t)(-93));        /* subf is rB - rA        */
    CHECK_EQ(reg(7), 100u);
    CHECK_EQ(reg(8), 4u);
    /* r0 keeps its poison: nothing above should have written it. */
    CHECK_EQ(reg(0), 0x1234u);
}

/*
 * Loads and stores, which is where byte order becomes visible.
 *
 * The program stores a word and reads back its individual bytes. On a
 * big-endian guest the most significant byte is at the *lowest* address,
 * so lbz from offset 0 must give 0x12 and not 0x78. A frontend that had
 * byte order wrong would still pass a store-then-load-word round trip --
 * that is why the byte reads are here.
 */
static void test_load_store_byte_order(void)
{
    static const uint32_t prog[] = {
        0x3C601234u,   /* lis   r3,0x1234                                */
        0x60635678u,   /* ori   r3,r3,0x5678  -- r3 = 0x12345678         */
        0x3C808000u,   /* lis   r4,0x8000                                */
        0x60840800u,   /* ori   r4,r4,0x800   -- guest RAM + 0x800       */
        0x90640000u,   /* stw   r3,0(r4)                                 */
        0x88A40000u,   /* lbz   r5,0(r4)      -- 0x12 if big-endian      */
        0x88C40003u,   /* lbz   r6,3(r4)      -- 0x78                    */
        0xA0E40000u,   /* lhz   r7,0(r4)      -- 0x1234                  */
        0x80040000u,   /* lwz   r0,0(r4)      -- round trip              */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(3), 0x12345678u);
    CHECK_EQ(reg(5), 0x12u);        /* MSB at the lowest address */
    CHECK_EQ(reg(6), 0x78u);
    CHECK_EQ(reg(7), 0x1234u);
    CHECK_EQ(reg(0), 0x12345678u);

    /* And the bytes really are in that order in guest memory. */
    CHECK_EQ(g_ram[0x800], 0x12u);
    CHECK_EQ(g_ram[0x803], 0x78u);
    (void)retired;
}

/*
 * The condition register, whose fields are numbered from the left: CR0 is
 * the *top* nibble. Writing the shift as 4*n rather than 4*(7-n) puts CR0
 * where CR7 belongs, which no single-field test would notice -- so this
 * one compares two different fields.
 */
static void test_condition_register(void)
{
    static const uint32_t prog[] = {
        0x38600005u,   /* li    r3,5                                     */
        0x38800009u,   /* li    r4,9                                     */
        0x7C032000u,   /* cmpw  cr0,r3,r4     -- 5 < 9  -> LT in CR0     */
        0x7D832000u,   /* cmpw  cr3,r3,r4     -- same, into CR3          */
        0x7C641800u,   /* cmpw  cr0,r4,r3     -- 9 > 5  -> GT, overwrite */
        0x7CA00026u,   /* mfcr  r5                                       */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* CR0 is the top nibble and now holds GT; CR3 still holds LT. */
    CHECK_EQ((reg(5) >> 28) & 0xFu, (uint32_t)PPC_CR_GT);
    CHECK_EQ((reg(5) >> 16) & 0xFu, (uint32_t)PPC_CR_LT);
}

/*
 * mfspr/mtspr, whose SPR number is split and *swapped*: the low five bits
 * come first. LR is SPR 8, which read unswapped is 256 -- so `mflr` on a
 * naive implementation addresses a different register entirely and reads
 * zero, which looks exactly like a link register that was never set.
 */
static void test_spr_number_is_swapped(void)
{
    static const uint32_t prog[] = {
        0x38601234u,   /* li    r3,0x1234                                */
        0x7C6803A6u,   /* mtlr  r3            -- mtspr 8                 */
        0x7C8802A6u,   /* mflr  r4            -- mfspr 8                 */
        0x38A05678u,   /* li    r5,0x5678                                */
        0x7CA903A6u,   /* mtctr r5            -- mtspr 9                 */
        0x7CC902A6u,   /* mfctr r6                                       */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(4), 0x1234u);
    CHECK_EQ(reg(6), 0x5678u);
    CHECK_EQ(core()->lr, 0x1234u);
    CHECK_EQ(core()->ctr, 0x5678u);
}

/*
 * Branch, and the fact that `bl` sets LR to the *next* instruction.
 *
 * The displacement is a signed 26-bit byte offset whose low two bits are
 * architecturally zero, so it is a field of bits 6:29 already shifted.
 */
static void test_branch(void)
{
    static const uint32_t prog[] = {
        0x38600001u,   /* li    r3,1                                     */
        0x48000009u,   /* bl    +8  (to the li r5)                       */
        0x38600002u,   /* li    r3,2   -- skipped                        */
        0x38A00003u,   /* li    r5,3                                     */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(3), 1u);           /* the skipped li did not run */
    CHECK_EQ(reg(5), 3u);
    /* LK set, so LR is the instruction after the branch. */
    CHECK_EQ(core()->lr, EMU_GUEST_RAM_BASE + 8u);
}

/*
 * An unimplemented encoding raises a program interrupt -- and this is
 * the check that it is *reported*, not merely raised. With IVPR and the
 * IVORs zero the handler address is 0, which in a flat guest is the
 * image's own entry: the pc goes back to the start and the instruction
 * does not retire. That is exactly the shape that cost three sessions on
 * G4MH, so it is pinned here rather than discovered later.
 */
static void test_unimplemented_reports(void)
{
    static const uint32_t prog[] = {
        0x38600007u,   /* li    r3,7                                     */
        0x7C000268u,   /* an X-form extended opcode this core lacks      */
        0x44000002u,   /* sc                                             */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    /*
     * Budget 2: the li, then the bad encoding. Stopping there matters --
     * run on and the vector at address 0 faults in its turn, and that
     * second interrupt overwrites SRR0 with 0. That is the architecture
     * behaving correctly, so the test stops rather than the emulator
     * being changed to suit it.
     */
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 2u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* SRR0 holds the faulting pc, not the next one: Book E restarts the
     * instruction after the handler returns. */
    CHECK_EQ(core()->srr0, EMU_GUEST_RAM_BASE + 4u);
    /* Entry clears EE, and the vector really is address zero here. */
    CHECK_EQ(core()->msr & PPC_MSR_EE, 0u);
}

/* ------------------------------------------------------------------ */
/* VLE                                                                 */
/* ------------------------------------------------------------------ */

/* VLE programs are halfwords, so they need their own emitter -- still
 * big-endian, still most significant byte first. */
static bool load_and_run_vle(const uint16_t *hw, unsigned n, uint32_t budget,
                             emu_run_reason_t *why, uint32_t *retired)
{
    const emu_cpu_ops_t *ops = emu_frontend_find("ppc");
    if (ops == NULL) {
        return false;
    }

    memset(g_ram, 0, sizeof(g_ram));
    for (unsigned i = 0; i < n; i++) {
        g_ram[i * 2u + 0u] = (uint8_t)(hw[i] >> 8);
        g_ram[i * 2u + 1u] = (uint8_t)(hw[i]);
    }

    memset(g_scratch, 0, sizeof(g_scratch));

    emu_bus_init(&g_bus);
    if (!emu_bus_add_ram(&g_bus, "ram", EMU_GUEST_RAM_BASE, g_ram,
                         TEST_RAM_SIZE)) {
        return false;
    }
    /*
     * A low scratch region, because the 16-bit forms cannot build a high
     * address: se_li reaches 0..127 and there is no se_lis. A VLE-only
     * program addresses memory through a register something else set up
     * -- in a real guest, the linker's small-data pointer -- so the test
     * gives it somewhere reachable rather than pretending otherwise.
     */
    if (!emu_bus_add_ram(&g_bus, "scratch", VLE_SCRATCH, g_scratch,
                         sizeof(g_scratch))) {
        return false;
    }
    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        return false;
    }
    emu_core_reset(&g_core, EMU_GUEST_RAM_BASE);
    emu_core_boot(&g_core, EMU_GUEST_RAM_BASE, TEST_RAM_SIZE);
    /* VLE and Book E are different encodings of the same bytes, so the
     * mode has to be stated before the first fetch. */
    ((ppc_cpu_t *)(void *)g_core.cpu)->vle = true;

    *why = emu_core_run(&g_core, budget, retired);
    return true;
}

/*
 * The se_ arithmetic, and the compressed register field.
 *
 * That field is four bits and maps 0-7 to r0-r7 and 8-15 to *r24-r31*,
 * not r0-r15. The test uses r24 and r25 for exactly that reason: read as
 * a plain index they become r8 and r9, which are ordinary live registers,
 * so nothing faults and the wrong ones are written.
 *
 * se_sub and se_subf are both here because they are two instructions with
 * opposite senses rather than one with swapped operands, and both write
 * rX -- so a test using only one of them passes either way.
 */
static void test_se_alu(void)
{
    static const uint16_t prog[] = {
        0x4E43u,   /* se_li   r3,100                                  */
        0x4874u,   /* se_li   r4,7                                    */
        0x0135u,   /* se_mr   r5,r3                                   */
        0x0445u,   /* se_add  r5,r4      -- 107                       */
        0x0643u,   /* se_sub  r3,r4      -- 100 - 7  = 93             */
        0x4896u,   /* se_li   r6,9                                    */
        0x0746u,   /* se_subf r6,r4      -- 7 - 9    = -2             */
        0x48C7u,   /* se_li   r7,12                                   */
        0x6827u,   /* se_srwi r7,2       -- 3                         */
        0x4858u,   /* se_li   r24,5                                   */
        0x0189u,   /* se_mr   r25,r24                                 */
        0x21F9u,   /* se_addi r25,32     -- OIM5: 31 encodes 32       */
        0x0002u,   /* se_sc                                           */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_vle(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                          &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(5), 107u);
    CHECK_EQ(reg(3), 93u);
    CHECK_EQ(reg(6), (uint32_t)(-2));     /* subf is rY - rX */
    CHECK_EQ(reg(7), 3u);
    CHECK_EQ(reg(24), 5u);                /* field 8  -> r24, not r8  */
    CHECK_EQ(reg(25), 37u);               /* field 9  -> r25, and +32 */
    /* r8 and r9 must be untouched, which is what a plain-index read of
     * the field would have written instead. */
    CHECK_EQ(reg(8), 0u);
    CHECK_EQ(reg(9), 0u);
}

/*
 * SD4-form loads and stores, and the branches.
 *
 * Two traps in one test. The displacement is *scaled by the access size*,
 * so the same nibble is 3 for a byte and 12 for a word. And the operand
 * sense is reversed from every two-register form: the data register is
 * bits[7:4] and the base is bits[3:0], where se_mr has the source in
 * bits[7:4] and the destination in bits[3:0].
 */
static void test_se_memory_and_branch(void)
{
    static const uint16_t prog[] = {
        0x4C04u,   /* se_li   r4,64                                   */
        0x6C64u,   /* se_slwi r4,6       -- r4 = 0x1000, the scratch  */
        0x4803u,   /* se_li   r3,0                                    */
        0xD034u,   /* se_stw  r3,0(r4)   -- zero the word             */
        0x4DA5u,   /* se_li   r5,90                                   */
        0x9354u,   /* se_stb  r5,3(r4)   -- the *last* byte, BE       */
        0x8364u,   /* se_lbz  r6,3(r4)   -- 90                        */
        0xC074u,   /* se_lwz  r7,0(r4)   -- 90, in the low byte       */
        0x4858u,   /* se_li   r24,5                                   */
        0x2A58u,   /* se_cmpi r24,5      -- EQ into CR0               */
        0xE602u,   /* se_beq  +4         -- taken                     */
        0x4EF9u,   /* se_li   r25,111    -- skipped                   */
        0x481Au,   /* se_li   r26,1                                   */
        0xE902u,   /* se_bl   +4                                      */
        0x4E3Bu,   /* se_li   r27,99     -- skipped                   */
        0x008Cu,   /* se_mflr r28                                     */
        0x0002u,   /* se_sc                                           */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_vle(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                          &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(4), VLE_SCRATCH);
    CHECK_EQ(reg(6), 90u);
    /* Big-endian: a byte at offset 3 is the *least* significant of the
     * word, so the word reads as 90 and not 90 << 24. */
    CHECK_EQ(reg(7), 90u);
    CHECK_EQ(g_scratch[3], 90u);

    CHECK_EQ(reg(25), 0u);                /* se_beq was taken         */
    CHECK_EQ(reg(26), 1u);
    CHECK_EQ(reg(27), 0u);                /* se_bl skipped it         */
    /* se_bl links to the instruction after itself. */
    CHECK_EQ(reg(28), EMU_GUEST_RAM_BASE + 28u);
}

/*
 * SD4 is scaled by the *access size*, so the same nibble means a
 * different byte offset per width.
 *
 * This exists because the first version of the test above could not see
 * it: it used a byte store (scale 1) and a word load at offset 0, and
 * scaled and unscaled agree on both. Reverting the scaling changed
 * nothing and the suite still passed -- which is the recurring failure
 * in this tree, a test that reads as coverage. Here nibble 1 must mean
 * byte 4 for a word and byte 2 for a halfword.
 */
static void test_se_sd4_is_scaled(void)
{
    static const uint16_t prog[] = {
        0x4C04u,   /* se_li   r4,64                                   */
        0x6C64u,   /* se_slwi r4,6       -- the scratch region        */
        0x4B73u,   /* se_li   r3,55                                   */
        0xD134u,   /* se_stw  r3,4(r4)   -- nibble 1, *4 = byte 4     */
        0xC164u,   /* se_lwz  r6,4(r4)   -- reads it back             */
        0xA174u,   /* se_lhz  r7,2(r4)   -- nibble 1, *2 = byte 2     */
        0x0002u,   /* se_sc                                           */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_vle(prog, sizeof(prog) / sizeof(prog[0]), 32u, &why,
                          &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(6), 55u);
    /* The word really is at byte 4, not byte 1: unscaled it would have
     * landed straddling bytes 1..4 and the halfword read would differ. */
    CHECK_EQ(g_scratch[4], 0u);
    CHECK_EQ(g_scratch[7], 55u);      /* big-endian: LSB last */
    /* Halfword at byte 2 is the top half of that word, so zero. */
    CHECK_EQ(reg(7), 0u);
}

/*
 * The 32-bit e_ forms.
 *
 * A real VLE program mixes widths freely, so this one ends on a 16-bit
 * se_sc after 32-bit instructions -- which also exercises the length
 * decoder in the direction that matters, since a 32-bit instruction
 * misread as 16-bit desynchronises everything after it.
 *
 * Three things here are not guessable and are checked deliberately:
 *
 *   - **SCI8 is not a plain immediate.** Eleven bits hold a fill bit and
 *     a two-bit *scale* as well as the eight-bit value, and the scale
 *     picks which byte of the word the value lands in. e_ori r8,r3,7
 *     with the value in byte 0 is 7; the same UI8 at scale 1 is 1792.
 *   - **The logical forms reverse the register sense**, writing rA from
 *     rS where the arithmetic forms write rD from rA.
 *   - **LI20 is split**, five bits above the displacement and fifteen
 *     below, with bit 15 clear -- bit 15 set is a different group.
 */
static void test_e_forms(void)
{
    static const uint16_t prog[] = {
        0x7060u, 0x03E8u,   /* e_li     r3,1000                       */
        0x709Fu, 0x7F9Cu,   /* e_li     r4,-100    -- LI20 sign        */
        0x1CA3u, 0x0018u,   /* e_add16i r5,r3,24   -- 1024            */
        0x18C3u, 0x800Au,   /* e_addi   r6,r3,10   -- SCI8, 1010      */
        0x18E4u, 0xB00Au,   /* e_subfic r7,r4,10   -- 10 - (-100)     */
        0x1868u, 0xD007u,   /* e_ori    r8,r3,7    -- rA<-rS, 1007    */
        0x1869u, 0xC00Cu,   /* e_andi   r9,r3,12   -- 1000 & 12 = 8   */
        0x7142u, 0x0000u,   /* e_li     r10,4096   -- the scratch     */
        0x546Au, 0x0008u,   /* e_stw    r3,8(r10)                     */
        0x516Au, 0x0008u,   /* e_lwz    r11,8(r10)                    */
        0x318Au, 0x0008u,   /* e_lbz    r12,8(r10) -- MSB, so 0       */
        0x5C8Au, 0x0010u,   /* e_sth    r4,16(r10)                    */
        0x59AAu, 0x0010u,   /* e_lhz    r13,16(r10)                   */
        0x1803u, 0xA807u,   /* e_cmpi   cr0,r3,7   -- 1000 > 7 -> GT  */
        0x7A11u, 0x0008u,   /* e_bgt    +8         -- taken           */
        0x71C0u, 0x0037u,   /* e_li     r14,55     -- skipped         */
        0x71E0u, 0x0042u,   /* e_li     r15,66                        */
        0x7800u, 0x0009u,   /* e_bl     +8                            */
        0x7200u, 0x004Du,   /* e_li     r16,77     -- skipped         */
        0x0002u,            /* se_sc               -- 16-bit          */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_vle(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(3), 1000u);
    CHECK_EQ(reg(4), (uint32_t)(-100));   /* LI20 sign-extends       */
    CHECK_EQ(reg(5), 1024u);
    CHECK_EQ(reg(6), 1010u);              /* SCI8 scale 0            */
    CHECK_EQ(reg(7), 110u);               /* subfic is imm - rA      */
    CHECK_EQ(reg(8), 1000u | 7u);         /* rA written from rS      */
    CHECK_EQ(reg(9), 1000u & 12u);

    CHECK_EQ(reg(11), 1000u);
    CHECK_EQ(reg(12), 0u);                /* big-endian: MSB of 1000 */
    CHECK_EQ(reg(13), 0xFF9Cu);           /* low half of -100        */

    CHECK_EQ(reg(14), 0u);                /* e_bgt was taken         */
    CHECK_EQ(reg(15), 66u);
    CHECK_EQ(reg(16), 0u);                /* e_bl skipped it         */
    CHECK_EQ(reg(0), 0u);
    /* e_bl links past itself: the bl is at byte 68, so LR is 72. */
    CHECK_EQ(core()->lr, EMU_GUEST_RAM_BASE + 72u);
}

/*
 * SCI8's scale and fill, and e_lha's sign extension.
 *
 * Both exist because the e_ test above could not see either. It used
 * SCI8 only with scale 0 and fill 0 -- where the encoding coincides
 * exactly with a plain 11-bit immediate -- and never used e_lha at all,
 * so reverting the scale logic and the sign extension each changed
 * nothing and the suite still passed.
 *
 * That is the third time in this frontend that an A/B found a test which
 * read as coverage. The pattern is always the same: the case chosen was
 * the one where the right and wrong readings agree.
 */
static void test_e_sci8_and_lha(void)
{
    static const uint16_t prog[] = {
        0x7142u, 0x0000u,   /* e_li   r10,4096                        */
        0x709Fu, 0x7F9Cu,   /* e_li   r4,-100                         */
        0x5C8Au, 0x0010u,   /* e_sth  r4,16(r10)  -- 0xFF9C           */
        0x38AAu, 0x0010u,   /* e_lha  r5,16(r10)  -- sign-extends     */
        0x58CAu, 0x0010u,   /* e_lhz  r6,16(r10)  -- does not         */
        0x1947u, 0xD201u,   /* e_ori  r7,r10,0x10000  -- SCL=2        */
        0x190Au, 0x84FFu,   /* e_addi r8,r10,-1       -- F=1          */
        0x1949u, 0xC400u,   /* e_andi r9,r10,-256     -- F=1, UI8=0   */
        0x0002u,            /* se_sc                                  */
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_vle(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) {
        CHECK(false);
        return;
    }

    /* e_lha and e_lhz differ only in the sign, so both are needed. */
    CHECK_EQ(reg(5), (uint32_t)(-100));
    CHECK_EQ(reg(6), 0xFF9Cu);

    /* SCL picks which byte the eight-bit value lands in. */
    CHECK_EQ(reg(7), 4096u | 0x10000u);
    /* F fills the other three bytes, so this is an immediate of -1... */
    CHECK_EQ(reg(8), 4096u - 1u);
    /* ...and here of 0xFFFFFF00, which a plain read would make 0. */
    CHECK_EQ(reg(9), 4096u & 0xFFFFFF00u);
}

void test_ppc(void)
{
    test_vle_length();
    test_alu();
    test_load_store_byte_order();
    test_condition_register();
    test_spr_number_is_swapped();
    test_branch();
    test_unimplemented_reports();
    test_se_alu();
    test_se_memory_and_branch();
    test_se_sd4_is_scaled();
    test_e_forms();
    test_e_sci8_and_lha();
}
