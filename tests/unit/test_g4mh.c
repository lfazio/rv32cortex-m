/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_g4mh.c - Unit tests for the RH850 G4MH frontend.
 *
 * Two things are worth testing here that a guest program could not reach,
 * because there is no RH850 toolchain in this build:
 *
 *   - instruction length classification, which nothing else can catch. A
 *     wrong length does not produce a wrong result, it desynchronises the
 *     instruction stream, and the failure then appears several
 *     instructions later in whatever happened to be decoded out of the
 *     middle of an operand.
 *
 *   - the frontend contract, driven exactly as a platform drives it:
 *     open, add devices, reset, boot, run, status, dump. That is the seam
 *     the whole refactor exists to provide, so it is tested through
 *     emu_cpu_ops_t rather than by calling into the frontend directly.
 *
 * The programs below are assembled by hand from the encodings in
 * g4mh_interp.c. That is deliberate: an encoding helper shared with the
 * interpreter would make a test that passes when both are wrong the same
 * way.
 */

#include "tests.h"

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_config.h"
#include "g4mh/g4mh_types.h"
#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_boot.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_intercpu.h"
#include "g4mh/g4mh_memmap.h"
#include "emu/emu_gdb.h"
#include "emu/emu_jit.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Encoding helpers                                                    */
/* ------------------------------------------------------------------ */

/* Format I: reg1 op reg2. */
#define F1(op, r1, r2)   (uint16_t)(((r2) << 11) | ((op) << 5) | (r1))
/* Format II: imm5 op reg2. */
#define F2(op, imm, r2)  (uint16_t)(((r2) << 11) | ((op) << 5) | ((imm) & 0x1Fu))
/* Bcond disp9: disp[8:4] up top, disp[3:1] at bits 6:4, cond at the bottom. */
#define BCOND(cond, d)   (uint16_t)(((((d) >> 4) & 0x1Fu) << 11) | (0x0Bu << 7) | \
                                    ((((d) >> 1) & 0x7u) << 4) | (cond))

/* First halfword of any 32-bit format: reg2, opcode, reg1. */
#define W0(op, r1, r2)   (uint16_t)(((r2) << 11) | ((op) << 5) | (r1))

#define OP_MOV      0x00u
#define OP_OR       0x08u
#define OP_AND      0x0Au
#define OP_SUB      0x0Du
#define OP_ADD      0x0Eu
#define OP_CMP      0x0Fu
#define OP_MOVI     0x10u
#define OP_ADDI5    0x12u
#define OP_CMPI5    0x13u
#define OP_SHL      0x16u
#define OP_MOVEA    0x31u
#define OP_MOVHI    0x32u
#define OP_ORI      0x34u
#define OP_LD_HW    0x39u
#define OP_LD_B     0x38u
#define OP_ST_B     0x3Au
#define OP_ST_HW    0x3Bu
#define OP_JARL     0x3Cu
#define OP_SYSTEM   0x3Fu

/* Second halfword sub-opcodes of the system group. */
#define SUB_LDSR    0x0020u
#define SUB_STSR    0x0040u
#define SUB_TRAP    0x0100u
#define SUB_HALT    0x0120u
#define SUB_CTRET   0x0144u
#define SUB_EIRET   0x0148u
#define SUB_FERET   0x014Au
#define SUB_DIEI    0x0160u
#define SUB_MUL     0x0220u
#define SUB_DIV     0x02C0u
/*
 * The three-operand register shifts, the high-speed divides, the halfword
 * divides and the imm9 multiplies. Every one of these is what CC-RH emits
 * for ordinary C -- `v >> n` is SHR reg1,reg2,reg3 and `a / b` is DIVQ --
 * and none of them was decoded, so a compiled guest raised RIE on its
 * first shift. The constants come from scripts/g4mh-check-encodings.sh,
 * not from reading the manual's diagrams.
 */
#define SUB_SHR3    0x0082u
#define SUB_SAR3    0x00A2u
#define SUB_SHL3    0x00C2u
#define SUB_DIVQ    0x02FCu
#define SUB_DIVQU   0x02FEu
#define SUB_DIVH    0x0280u
#define SUB_DIVHU   0x0282u
/*
 * The saturating narrowings, and the narrow halves of the link/store-
 * conditional group. reg1 is CLIP's source and reg2 its destination --
 * the opposite sense to most of this group.
 */
#define SUB_CLIPB   0x0008u
#define SUB_CLIPBU  0x000Au
#define SUB_CLIPH   0x000Cu
#define SUB_CLIPHU  0x000Eu
#define SUB_LDLBU   0x0370u
#define SUB_STCB    0x0372u
#define SUB_LDLHU   0x0374u
#define SUB_STCH    0x0376u
#define SUB_LDLW    0x0378u
#define SUB_STCW    0x037Au
/*
 * The reg2 field of the six sub-opcodes above is an opcode extension, not
 * a register: it selects between the link forms and the LD/ST forms that
 * update the pointer. These are the values that go in W0's r2 slot.
 */
#define EXT_LINK    0u          /* LDL.W, STC.B/H/W                     */
#define EXT_LINKU   1u          /* LDL.BU, LDL.HU                       */
#define EXT_POSTI   2u          /* [reg1]+, sign-extending              */
#define EXT_POSTIU  3u          /* [reg1]+, zero-extending              */
#define EXT_POSTD   4u          /* [reg1]-, sign-extending              */
#define EXT_POSTDU  5u          /* [reg1]-, zero-extending              */
/* imm9 splits: bits[8:5] into sub bits[5:2], bits[4:0] into the reg1 field. */
#define SUB_MULI(i) (uint16_t)(0x0240u | ((((i) >> 5) & 0xFu) << 2))
#define SUB_MULUI(i) (uint16_t)(0x0242u | ((((i) >> 5) & 0xFu) << 2))
#define MULI_R1(i)  (uint16_t)((i) & 0x1Fu)
/* Swap and bit-search group; reg1 is a fixed zero in all of them. */
#define SUB_BSW     0x0340u
#define SUB_BSH     0x0342u
#define SUB_HSW     0x0344u
#define SUB_HSH     0x0346u
#define SUB_SCH0R   0x0360u
#define SUB_SCH1R   0x0362u
#define SUB_SCH0L   0x0364u
#define SUB_SCH1L   0x0366u
/* Bit manipulation, register form: bit number from the value in reg2. */
#define SUB_SET1    0x00E0u
#define SUB_NOT1    0x00E2u
#define SUB_CLR1    0x00E4u
#define SUB_TST1    0x00E6u

/*
 * Format VIII: the operation selector sits in bits[15:14] -- the top of
 * the field every other 32-bit format uses for reg2 -- with the 3-bit
 * bit number below it and reg1 at the bottom.
 */
#define BOP_SET1    0u
#define BOP_NOT1    1u
#define BOP_CLR1    2u
#define BOP_TST1    3u
#define BITOP8(sel, bit, r1) \
    (uint16_t)(((sel) << 14) | ((bit) << 11) | (0x3Eu << 5) | (r1))

/* ------------------------------------------------------------------ */
/* Instruction length                                                  */
/* ------------------------------------------------------------------ */

static void test_length(void)
{
    /* Format I and II: every opcode below 0x30 is two bytes. */
    CHECK_EQ(g4mh_insn_len(F1(OP_MOV, 5, 6)), 2u);
    CHECK_EQ(g4mh_insn_len(F1(OP_ADD, 1, 2)), 2u);
    CHECK_EQ(g4mh_insn_len(F2(OP_MOVI, 7, 3)), 2u);
    CHECK_EQ(g4mh_insn_len(F2(OP_SHL, 31, 31)), 2u);

    /* Formats III and IV live in the same range and are also two bytes. */
    CHECK_EQ(g4mh_insn_len(BCOND(0x2u, 8)), 2u);
    CHECK_EQ(g4mh_insn_len((uint16_t)((3u << 11) | (0x0Au << 7) | 0x10u)), 2u);

    /* Formats VI and VII: four bytes. */
    CHECK_EQ(g4mh_insn_len(W0(OP_MOVEA, 1, 2)), 4u);
    CHECK_EQ(g4mh_insn_len(W0(OP_LD_HW, 1, 2)), 4u);
    CHECK_EQ(g4mh_insn_len(W0(OP_ST_HW, 1, 2)), 4u);
    CHECK_EQ(g4mh_insn_len(W0(OP_SYSTEM, 0, 0)), 4u);

    /* JR/JARL: bit 5 is disp[5], so both opcode values are four bytes. */
    CHECK_EQ(g4mh_insn_len(W0(0x3Cu, 0, 0)), 4u);
    CHECK_EQ(g4mh_insn_len(W0(0x3Du, 0, 0)), 4u);

    /*
     * MOV imm32, reg1 is the MOVEA slot with reg2 == 0, and is the one
     * encoding in the 32-bit range that is six bytes. The distinction is
     * reg2, not the opcode, which is exactly the case a length rule
     * written on the opcode alone would get wrong.
     */
    CHECK_EQ(g4mh_insn_len(W0(OP_MOVEA, 9, 0)), 4u);   /* first stage */
    CHECK(g4mh_insn_is_48(W0(OP_MOVEA, 9, 0), 0x1234u));
    CHECK(!g4mh_insn_is_48(W0(OP_MOVEA, 9, 1), 0x1234u));

    /* JR / JARL disp32 (0x17) and JMP disp32 (0x37), both reg2 == 0. */
    CHECK(g4mh_insn_is_48(W0(0x17u, 9, 0), 0x0000u));
    CHECK(!g4mh_insn_is_48(W0(0x17u, 9, 1), 0x0000u));
    CHECK(g4mh_insn_is_48(W0(0x37u, 9, 0), 0x0000u));   /* JMP disp32 */
    CHECK(!g4mh_insn_is_48(W0(0x37u, 9, 0), 0x0001u));  /* LOOP       */

    /*
     * The 0x3C/0x3D slot holds JR disp22 (32-bit), PREPARE and the disp23
     * loads (48-bit), and only bit 0 of the second halfword separates
     * them -- JR's displacement is even.
     */
    CHECK(!g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0100u));  /* JR     */
    /*
     * The rest of this slot is not one thing. 0x0101 is PREPARE's short
     * form and is *four* bytes -- this check used to assert six, on the
     * reasoning that bit 0 of the second halfword separated JR from
     * everything else. It separates JR from everything else; it does not
     * separate the everything else, and PREPARE is in there.
     */
    CHECK(!g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0821u));  /* PREPARE     */
    CHECK(!g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0823u));  /* ..., sp     */
    CHECK(g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x082Bu));   /* ..., imm16  */
    CHECK(g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0105u));   /* disp23 load */

    /*
     * Every disp23 sub-opcode, with disp[0] both ways. Six bytes is the
     * answer for all ten, and the length decoder must not care which:
     * it runs before anything knows what the instruction is, and a
     * wrong length is not a wrong answer but a desynchronised stream.
     *
     * 0x9 is the one to look at. Its low three bits are 001, one bit
     * from PREPARE's 011, and with disp[0] set w1 & 0x1F is 0x19 --
     * near enough to PREPARE's 0x01 that a mask written from memory
     * gets it wrong. Both are here for that reason.
     */
    /*
     * g4mh_is_16bit and g4mh_insn_len must agree on every first
     * halfword. They are two spellings of one question, and they came
     * apart: `JR/JARL disp32` is 48 bits in an opcode slot below 0x30,
     * so the shorthand `op6 < 0x30` is wrong for it while insn_len is
     * right.
     *
     * Asserted as a property over the whole space rather than at the
     * one encoding, because what makes it a defect is the *duplication*
     * -- and because today nothing downstream can see the divergence:
     * the JIT's translator declines op6 0x17 for other reasons, so a
     * wrong answer here changes no result. A capability that depends on
     * nobody exercising it is exactly what this project keeps finding.
     */
    for (uint32_t w = 0; w <= 0xFFFFu; w++) {
        if (g4mh_is_16bit((uint16_t)w) !=
            (g4mh_insn_len((uint16_t)w) == 2u)) {
            CHECK(false);
            break;
        }
    }
    CHECK(true);

    /* And the one encoding the two used to disagree about. */
    CHECK_EQ(g4mh_insn_len(0x02E0u), 4u);       /* jr  disp32 */
    CHECK_EQ(g4mh_insn_len(0x02E0u | 19u), 4u); /* jarl disp32, r19 */
    CHECK(g4mh_insn_is_48(0x02E0u, 0x0008u));
    /* MULH imm5 with a real reg2 stays 16 bits. */
    CHECK_EQ(g4mh_insn_len((uint16_t)((10u << 11) | (0x17u << 5) | 4u)), 2u);

    {
        static const uint16_t sub[5] = { 0x5u, 0x7u, 0x9u, 0xDu, 0xFu };
        for (unsigned i = 0; i < 5u; i++) {
            const uint16_t even = (uint16_t)(0x0100u | sub[i]);
            const uint16_t odd  = (uint16_t)(0x0110u | sub[i]);
            CHECK(g4mh_insn_is_48(W0(0x3Cu, 0, 0), even));
            CHECK(g4mh_insn_is_48(W0(0x3Cu, 0, 0), odd));
            CHECK(g4mh_insn_is_48(W0(0x3Du, 0, 0), even));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Condition codes                                                     */
/* ------------------------------------------------------------------ */

static void test_conditions(void)
{
    /* BE / BNE off Z. */
    CHECK(g4mh_cond(0x2u, G4MH_PSW_Z));
    CHECK(!g4mh_cond(0x2u, 0u));
    CHECK(!g4mh_cond(0xAu, G4MH_PSW_Z));
    CHECK(g4mh_cond(0xAu, 0u));

    /* BR is unconditional. */
    CHECK(g4mh_cond(0x5u, 0u));

    /*
     * The signed comparisons are where a condition table goes wrong: BLT
     * is S != OV, not S. Checked with OV set and clear so a table that
     * ignored OV would fail one of them.
     */
    CHECK(g4mh_cond(0x6u, G4MH_PSW_S));                  /* S=1 OV=0 -> lt */
    CHECK(!g4mh_cond(0x6u, G4MH_PSW_S | G4MH_PSW_OV));   /* S=1 OV=1 -> ge */
    CHECK(g4mh_cond(0x6u, G4MH_PSW_OV));                 /* S=0 OV=1 -> lt */
    CHECK(!g4mh_cond(0x6u, 0u));

    /* BGT is (S == OV) && !Z, so Z alone must defeat it. */
    CHECK(g4mh_cond(0xFu, 0u));
    CHECK(!g4mh_cond(0xFu, G4MH_PSW_Z));

    /* BNH is CY || Z; BH is its complement. */
    CHECK(g4mh_cond(0x3u, G4MH_PSW_CY));
    CHECK(g4mh_cond(0x3u, G4MH_PSW_Z));
    CHECK(!g4mh_cond(0xBu, G4MH_PSW_CY));
}

/* ------------------------------------------------------------------ */
/* Execution, through the frontend contract                            */
/* ------------------------------------------------------------------ */

#define TEST_RAM_SIZE  4096u
/* Well clear of any program these tests load, so a store cannot land on
 * the instruction stream and change what runs next. */
#define TEST_SCRATCH   (EMU_GUEST_RAM_BASE + 0x400u)

static uint8_t   g_ram[TEST_RAM_SIZE];
static emu_bus_t g_bus;
static emu_core_t g_core;

/* Assemble into guest RAM. Returns the offset just past what was written. */
static uint32_t emit(uint32_t off, const uint16_t *hw, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        g_ram[off + i * 2u]      = (uint8_t)(hw[i] & 0xFFu);
        g_ram[off + i * 2u + 1u] = (uint8_t)(hw[i] >> 8);
    }
    return off + n * 2u;
}

/*
 * Build a fresh core with `hw` loaded at the reset address and run it.
 * Driven entirely through emu_cpu_ops_t, in the order a platform uses:
 * open, add the architecture's devices, reset, boot, run.
 */
static bool load_and_run_hooked(const uint16_t *hw, unsigned n,
                                uint32_t budget, emu_run_reason_t *why,
                                uint32_t *retired, emu_syscall_fn hook)
{
    const emu_cpu_ops_t *ops = emu_frontend_find("g4mh");
    if (ops == NULL) {
        return false;
    }

    memset(g_ram, 0, sizeof(g_ram));
    (void)emit(0u, hw, n);

    emu_bus_init(&g_bus);
    if (!emu_bus_add_ram(&g_bus, "ram", EMU_GUEST_RAM_BASE, g_ram,
                         TEST_RAM_SIZE)) {
        return false;
    }
    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        return false;
    }
    if (ops->add_shared_devices != NULL && !ops->add_shared_devices(&g_bus)) {
        return false;
    }
    if (ops->add_core_devices != NULL &&
        !ops->add_core_devices(g_core.cpu, &g_bus, 0u)) {
        return false;
    }
    ops->set_syscall(g_core.cpu, hook, NULL);

    emu_core_reset(&g_core, EMU_GUEST_RAM_BASE);
    emu_core_boot(&g_core, EMU_GUEST_RAM_BASE, TEST_RAM_SIZE);

    *why = emu_core_run(&g_core, budget, retired);
    return true;
}

static bool load_and_run(const uint16_t *hw, unsigned n, uint32_t budget,
                         emu_run_reason_t *why, uint32_t *retired)
{
    return load_and_run_hooked(hw, n, budget, why, retired, NULL);
}

static uint32_t reg(unsigned r)
{
    return g_core.ops->reg_read(g_core.cpu, r);
}

/*
 * PSW and the system register file.
 *
 * `emu_cpu_t *` is a g4mh_cpu_t * in disguise -- the frontend's cpu_of()
 * is the same cast -- and there is no ops entry for either, because
 * neither is a general-purpose register and emu_cpu_ops_t deliberately
 * knows nothing about what an architecture calls its state. Tests that
 * check a *trap* have no other way in: the whole observable effect of
 * FETRAP is which save register the return address landed in.
 */
static uint32_t psw(void)
{
    return ((const g4mh_cpu_t *)(const void *)g_core.cpu)->psw;
}

static uint32_t sreg(unsigned bank, unsigned idx)
{
    return ((const g4mh_cpu_t *)(const void *)g_core.cpu)->sr[bank][idx];
}

/* A word of guest RAM, little-endian. */
static uint32_t ram32(uint32_t off)
{
    return (uint32_t)g_ram[off] | ((uint32_t)g_ram[off + 1u] << 8) |
           ((uint32_t)g_ram[off + 2u] << 16) | ((uint32_t)g_ram[off + 3u] << 24);
}

static void test_alu(void)
{
    /*
     *   mov  7, r10
     *   mov  -3, r11
     *   add  r11, r10      ; r10 = 4
     *   mov  r10, r12
     *   shl  4, r12        ; r12 = 64
     *   or   r10, r12      ; r12 = 68
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 7, 10),
        F2(OP_MOVI, -3, 11),
        F1(OP_ADD, 11, 10),
        F1(OP_MOV, 10, 12),
        F2(OP_SHL, 4, 12),
        F1(OP_OR, 10, 12),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 4u);
    CHECK_EQ(reg(11), 0xFFFFFFFDu);
    CHECK_EQ(reg(12), 68u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

static void test_flags_and_branch(void)
{
    /*
     * A countdown loop, which is the shortest thing that gets CMP, a
     * conditional branch and the pc-relative displacement wrong together
     * if any one of them is wrong:
     *
     *   mov  5, r10
     *   mov  0, r11
     * loop:
     *   add  1, r11
     *   add  -1, r10
     *   cmp  0, r10
     *   bne  loop           ; -6 bytes from the branch
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 5, 10),
        F2(OP_MOVI, 0, 11),
        F2(OP_ADDI5, 1, 11),
        F2(OP_ADDI5, -1, 10),
        F2(OP_CMPI5, 0, 10),
        BCOND(0xAu, (uint32_t)(-6) & 0x1FFu),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 256u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 0u);
    CHECK_EQ(reg(11), 5u);   /* the loop ran exactly five times */
    CHECK_EQ(why, EMU_RUN_WFI);
}

static void test_load_store(void)
{
    /*
     *   movhi 0x8000, r0, r10     ; r10 = 0x80000000, the RAM base
     *   movea 0x100, r10, r10     ; r10 = base + 0x100
     *   mov   42, r11
     *   st.w  r11, 0[r10]
     *   ld.w  0[r10], r12
     *   halt
     *
     * MOVHI/MOVEA is how a 32-bit address is built on RH850, so this also
     * checks that MOVEA does not touch the flags and that the disp16 of
     * the word forms has its low bit taken as the width selector.
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 10),  0x8000u,
        W0(OP_MOVEA, 10, 10), 0x0100u,
        F2(OP_MOVI, 15, 11),
        W0(OP_ST_HW, 10, 11), 0x0001u,   /* disp 0, bit0 = 1 -> st.w */
        W0(OP_LD_HW, 10, 12), 0x0001u,   /* disp 0, bit0 = 1 -> ld.w */
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), EMU_GUEST_RAM_BASE + 0x100u);
    CHECK_EQ(reg(12), 15u);
    CHECK_EQ(ram32(0x100u), 15u);
}

static void test_mov_imm32(void)
{
    /*
     * The 48-bit form. Worth its own test because it is the only place a
     * wrong length is fatal rather than merely unimplemented: if the core
     * reads it as four bytes, the high half of the constant is decoded as
     * the next instruction.
     *
     *   mov  0xDEADBEEF, r10
     *   mov  1, r11              ; must actually execute
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0), 0xBEEFu, 0xDEADu,
        F2(OP_MOVI, 1, 11),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 0xDEADBEEFu);
    CHECK_EQ(reg(11), 1u);        /* proves the pc advanced by six, not four */
    CHECK_EQ(why, EMU_RUN_WFI);
}

static void test_muldiv(void)
{
    /*
     *   mov  -7, r10
     *   mov  3, r11
     *   mul  r10, r11, r12    ; r11 = low, r12 = high of -21
     *   mov  13, r13
     *   mov  5, r14
     *   div  r14, r13, r15    ; r13 = 2, r15 = 3
     *   halt
     *
     * 13 rather than 17 because Format II's immediate is sign-extended
     * from five bits, so the encodable range is -16..15 and 17 would be
     * assembled as -15. The interpreter was right and the first version
     * of this test was wrong, which is the useful direction for that to
     * go.
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, -7, 10),
        F2(OP_MOVI, 3, 11),
        W0(OP_SYSTEM, 10, 11), (uint16_t)((12u << 11) | SUB_MUL),
        F2(OP_MOVI, 13, 13),
        F2(OP_MOVI, 5, 14),
        W0(OP_SYSTEM, 14, 13), (uint16_t)((15u << 11) | SUB_DIV),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), (uint32_t)(-21));
    CHECK_EQ(reg(12), 0xFFFFFFFFu);   /* sign extension of the product */
    CHECK_EQ(reg(13), 2u);            /* quotient  */
    CHECK_EQ(reg(15), 3u);            /* remainder */
}

/*
 * The three-operand register shifts.
 *
 * These are the forms a compiler emits and none of them was decoded, so
 * every `v >> n` in a CC-RH guest raised RIE. That did not present as a
 * clean exception: a flat guest has no vector table, so the RIE handler
 * address (RBASE + 0x60) landed on an ordinary instruction further down
 * the same function and execution carried on with the shift skipped.
 *
 * The destination is deliberately a *different* register from both
 * sources, because the two-operand form writes reg2 and would pass a test
 * that used reg3 == reg2.
 */
static void test_shift_three_operand(void)
{
    /*
     *   mov  -16, r10          ; 0xFFFFFFF0
     *   mov  2, r11
     *   shr  r11, r10, r12     ; r12 = 0x3FFFFFFC, r10 unchanged
     *   sar  r11, r10, r13     ; r13 = 0xFFFFFFFC
     *   shl  r11, r10, r14     ; r14 = 0xFFFFFFC0
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, -16, 10),
        F2(OP_MOVI, 2, 11),
        W0(OP_SYSTEM, 11, 10), (uint16_t)((12u << 11) | SUB_SHR3),
        W0(OP_SYSTEM, 11, 10), (uint16_t)((13u << 11) | SUB_SAR3),
        W0(OP_SYSTEM, 11, 10), (uint16_t)((14u << 11) | SUB_SHL3),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(12), 0x3FFFFFFCu);   /* logical: zeroes shifted in  */
    CHECK_EQ(reg(13), 0xFFFFFFFCu);   /* arithmetic: sign preserved  */
    CHECK_EQ(reg(14), 0xFFFFFFC0u);
    /* reg2 is a source here, not the destination: it must not be written. */
    CHECK_EQ(reg(10), 0xFFFFFFF0u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * DIVQ and DIVH, the divides a compiler actually emits.
 *
 * DIVQ is DIV with a shorter cycle count and nothing else, so the check
 * that matters is that it reaches the same code. DIVH is the one with its
 * own rule: only the *lower halfword* of reg1 is the divisor, so a divisor
 * whose low half is zero divides by zero however large the register is --
 * which is the case worth pinning, since a wrong implementation that used
 * all 32 bits would agree with a correct one on every small operand.
 */
static void test_divq_divh(void)
{
    /*
     *   mov   13, r10
     *   mov   5, r11
     *   divq  r11, r10, r12     ; r10 = 2, r12 = 3
     *   mov   -9, r13
     *   mov   2, r14
     *   divh  r14, r13, r15     ; r13 = -4, r15 = -1
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 13, 10),
        F2(OP_MOVI, 5, 11),
        W0(OP_SYSTEM, 11, 10), (uint16_t)((12u << 11) | SUB_DIVQ),
        F2(OP_MOVI, -9, 13),
        F2(OP_MOVI, 2, 14),
        W0(OP_SYSTEM, 14, 13), (uint16_t)((15u << 11) | SUB_DIVH),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 2u);
    CHECK_EQ(reg(12), 3u);
    CHECK_EQ(reg(13), (uint32_t)(-4));   /* C truncates toward zero */
    CHECK_EQ(reg(15), (uint32_t)(-1));
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * DIVH's divisor is the low halfword, and DIVHU zero-extends it where
 * DIVH sign-extends. Both are checked with a divisor whose *upper* half is
 * non-zero, so an implementation using the whole register gets a visibly
 * different answer rather than the same one.
 */
static void test_divh_halfword_only(void)
{
    /*
     *   mov    0x00010000, r10   ; low halfword zero -> divide by zero
     *   mov    100, r11
     *   divh   r10, r11, r12     ; OV set, r11 untouched
     *   mov    0xDEAD0002, r13   ; low halfword 2
     *   mov    100, r14
     *   divhu  r13, r14, r15     ; r14 = 50, not 0
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0), 0x0000u, 0x0001u,   /* mov 0x00010000, r10 */
        W0(OP_MOVEA, 11, 0), 0x0064u, 0x0000u,   /* mov 100, r11        */
        W0(OP_SYSTEM, 10, 11), (uint16_t)((12u << 11) | SUB_DIVH),
        W0(OP_MOVEA, 13, 0), 0x0002u, 0xDEADu,   /* mov 0xDEAD0002, r13 */
        W0(OP_MOVEA, 14, 0), 0x0064u, 0x0000u,   /* mov 100, r14        */
        W0(OP_SYSTEM, 13, 14), (uint16_t)((15u << 11) | SUB_DIVHU),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* Divide by zero leaves the dividend alone and sets OV. */
    CHECK_EQ(reg(11), 100u);
    CHECK_EQ(reg(14), 50u);           /* 100 / 2, not 100 / 0xDEAD0002 */
    CHECK_EQ(reg(15), 0u);            /* remainder */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * The imm9 multiplies, whose immediate is split across two fields. The
 * sign is what the split gets wrong: -1 is 0x1FF, so every bit of both
 * halves is set, and a zero-extending implementation returns 511 times
 * the operand instead of minus it.
 */
static void test_mul_imm9(void)
{
    /*
     *   mov  7, r10
     *   mul  -1, r10, r11      ; r10 = -7,  r11 = -1 (sign extension)
     *   mov  7, r12
     *   mulu -1, r12, r13      ; r12 = 7 * 511 = 3577, r13 = 0
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 7, 10),
        W0(OP_SYSTEM, MULI_R1(0x1FFu), 10),
            (uint16_t)((11u << 11) | SUB_MULI(0x1FFu)),
        F2(OP_MOVI, 7, 12),
        W0(OP_SYSTEM, MULI_R1(0x1FFu), 12),
            (uint16_t)((13u << 11) | SUB_MULUI(0x1FFu)),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), (uint32_t)(-7));
    CHECK_EQ(reg(11), 0xFFFFFFFFu);
    CHECK_EQ(reg(12), 7u * 511u);
    CHECK_EQ(reg(13), 0u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * CLIP, on the one input that separates the signed and unsigned forms.
 *
 * They differ in how they read the *source*, not only in where they
 * clamp: 0xFFFFFFFF is -1 to CLIP.B, which is in range and passes
 * through, and 4294967295 to CLIP.BU, which saturates to 255. An
 * implementation that narrowed first and clamped after would give 255 and
 * -1 the other way round, and every small positive input agrees.
 */
/*
 * The gdb `g` packet layout.
 *
 * gdb's rh850 numbering is the contract and gdb does *not* ask: its v850
 * backend rejects target-supplied registers outright ("Target-supplied
 * registers are not supported by the current architecture"), so the
 * target.xml this frontend serves documents the layout and cannot
 * enforce it. Nothing but this test holds the two together.
 *
 * The numbering was taken from gdb itself, not inferred:
 *
 *     gdb-multiarch -batch -ex 'set architecture v850:rh850' \
 *                          -ex 'maint print registers'
 *
 * Getting it wrong does not error. It produces an `info registers` full
 * of plausible values that are all one slot out, which is much harder to
 * spot -- the same failure rv_gdb.c warns about.
 */
const emu_gdb_target_t *g4mh_gdb_target(void);

static void test_gdb_layout(void)
{
    const emu_gdb_target_t *t = g4mh_gdb_target();

    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    /* 32 GPRs + 32 system registers + pc + fp, four bytes each. */
    CHECK_EQ(t->nregs, 66u);
    CHECK_EQ(t->reg_bytes, 4u);

    /* Run something short so the registers hold values worth reading. */
    const uint16_t prog[] = {
        F2(OP_MOVI, 9, 6),                      /* mov 9, r6            */
        F2(OP_MOVI, -4, 29),                    /* mov -4, r29 (the fp) */
        0x07E0u, SUB_HALT,
    };
    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* 0..31 are the general registers, in the frontend's own order. */
    CHECK_EQ(t->reg_get(g_core.cpu, 6u), 9u);
    CHECK_EQ(t->reg_get(g_core.cpu, 29u), (uint32_t)(-4));

    /*
     * 32..63 are the selID 0 bank, which maps straight across: gdb's
     * "psw" is register 37 and this frontend keeps PSW at index 5.
     */
    CHECK_EQ(t->reg_get(g_core.cpu, 32u + G4MH_SR_PSW), psw());
    CHECK_EQ(t->reg_get(g_core.cpu, 32u + G4MH_SR_EIPC),
             sreg(0, G4MH_SR_EIPC));

    /* 64 is the pc, and it is what pc_get reports. */
    CHECK_EQ(t->reg_get(g_core.cpu, 64u), t->pc_get(g_core.cpu));

    /*
     * 65 is gdb's `fp`, which the architecture does not have -- it is
     * the EABI's frame pointer, r29. A read that returned zero would
     * look plausible and be wrong, so this is checked against a value
     * the program actually put there.
     */
    CHECK_EQ(t->reg_get(g_core.cpu, 65u), (uint32_t)(-4));

    /* Writes have to land in the same places. Notably r0 stays zero and
     * a PSW write must reach the shadowed copy, not only the bank. */
    t->reg_set(g_core.cpu, 0u, 0xDEADBEEFu);
    CHECK_EQ(t->reg_get(g_core.cpu, 0u), 0u);

    t->reg_set(g_core.cpu, 65u, 0x1234u);       /* fp -> r29 */
    CHECK_EQ(t->reg_get(g_core.cpu, 29u), 0x1234u);

    t->reg_set(g_core.cpu, 32u + G4MH_SR_PSW, G4MH_PSW_Z);
    CHECK_EQ(psw(), G4MH_PSW_Z);

    /* A description is served, and it names the architecture gdb needs
     * in order to pick its rh850 backend without being told. */
    CHECK(t->target_xml != NULL);
    CHECK(strstr(t->target_xml, "v850:rh850") != NULL);
    CHECK(t->memory_map != NULL);
}

/*
 * The performance counters, and the bank widening they needed.
 *
 * PMCTRL0-7 and PMCOUNT0-7 are at selID 14 and PMUMCTRL at selID 11,
 * and this frontend's system register file had three banks -- so those
 * registers were not merely unimplemented, they were *unstorable*:
 * g4mh_sr_write dropped the write and g4mh_sr_read answered zero, which
 * to a guest is indistinguishable from a register hardwired to zero.
 *
 * The selID matters as much as the value. A test that wrote and read one
 * back through selID 0 would pass with three banks, so this one uses 14
 * and 11 specifically, and checks a bank *above* the old limit round-trips
 * at all before checking that counting works.
 */
static void test_perf_counters(void)
{
    /*
     *   mov  <ctl>, r10          ; CE set, CND = retired instructions
     *   ldsr r10, 0, 14          ; PMCTRL0
     *   mov  1, r11              ; three instructions that should count
     *   mov  2, r12
     *   mov  3, r13
     *   stsr 16, r14, 14         ; PMCOUNT0
     *   halt
     */
    const uint32_t ctl = G4MH_PMCTRL_CE |
                         (G4MH_PM_CND_INSN << G4MH_PMCTRL_CND_SH);
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0), (uint16_t)(ctl & 0xFFFFu),
                             (uint16_t)(ctl >> 16),
        /* LDSR: reg1 is the *source* and reg2 the regID -- the opposite
         * sense to STSR below, which is the reversal this frontend
         * already records and which I got backwards writing this. */
        W0(OP_SYSTEM, 10, G4MH_SR_PMCTRL0),
            (uint16_t)((G4MH_SELID_PM << 11) | SUB_LDSR),
        F2(OP_MOVI, 1, 11),
        F2(OP_MOVI, 2, 12),
        F2(OP_MOVI, 3, 13),
        W0(OP_SYSTEM, G4MH_SR_PMCOUNT0, 14),
            (uint16_t)((G4MH_SELID_PM << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* The control register survived a round trip through selID 14 -- the
     * thing three banks could not do at all. */
    CHECK_EQ(sreg(G4MH_SELID_PM, G4MH_SR_PMCTRL0), ctl);

    /*
     * The channel counted everything retired after it was enabled, and
     * nothing before.
     *
     * The exact figure is checked because it is now derivable: the
     * program is LDSR, three MOVs, STSR and HALT, and all six retire
     * after the channel is on. It was *not* checked at first, and that
     * was right at the time -- the count was 2 under the JIT and 5 under
     * the interpreter, and asserting either would have frozen a bug.
     *
     * `reg(14)` is 0 rather than the count, and that is the documented
     * granularity: the counters advance once per run slice, so an STSR
     * inside the slice reads the value as of the previous boundary. A
     * test that asserted otherwise would be asserting a precision this
     * does not have.
     */
    CHECK_EQ(sreg(G4MH_SELID_PM, G4MH_SR_PMCOUNT0), 6u);
    CHECK_EQ(reg(14), 0u);

    /* A disabled channel must stay put. */
    CHECK_EQ(sreg(G4MH_SELID_PM, G4MH_SR_PMCOUNT0 + 1u), 0u);
}

/*
 * PMUMCTRL lives alone at selID 11, which is a different bank again --
 * and the one most likely to be forgotten when widening, because every
 * other performance register is at 14.
 */
static void test_perf_umctrl_bank(void)
{
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0), 0xBEEFu, 0xDEADu,
        W0(OP_SYSTEM, 10, G4MH_SR_PMUMCTRL),
            (uint16_t)((G4MH_SELID_PMU << 11) | SUB_LDSR),
        W0(OP_SYSTEM, G4MH_SR_PMUMCTRL, 11),
            (uint16_t)((G4MH_SELID_PMU << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 0xDEADBEEFu);
    CHECK_EQ(sreg(G4MH_SELID_PMU, G4MH_SR_PMUMCTRL), 0xDEADBEEFu);
}

/*
 * The counters must agree between the interpreter and the JIT.
 *
 * This is the test that would have caught the bug they shipped with:
 * ticking per instruction in the interpreter counted only *interpreted*
 * instructions, so the same program counted 5 interpreted and 2
 * translated -- and 2 is the worse answer because it is plausible. No
 * single-backend test can see that; only running both can.
 *
 * With no reference model for this frontend, interpreter-against-JIT is
 * the only cross-check there is, and it is worth spending a test on
 * anything where the two could diverge.
 */
static void test_perf_counters_agree(void)
{
    const uint32_t ctl = G4MH_PMCTRL_CE |
                         (G4MH_PM_CND_INSN << G4MH_PMCTRL_CND_SH);
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0), (uint16_t)(ctl & 0xFFFFu),
                             (uint16_t)(ctl >> 16),
        W0(OP_SYSTEM, 10, G4MH_SR_PMCTRL0),
            (uint16_t)((G4MH_SELID_PM << 11) | SUB_LDSR),
        F2(OP_MOVI, 1, 11),
        F2(OP_MOVI, 2, 12),
        F2(OP_MOVI, 3, 13),
        0x07E0u, SUB_HALT,
    };
    const emu_backend_t *saved = g4mh_backend;
    uint32_t counts[2] = { 0u, 0u };

    for (unsigned pass = 0; pass < 2u; pass++) {
        emu_run_reason_t why;
        uint32_t retired = 0;

        /* Pass 0 takes whatever the frontend picked; pass 1 forces the
         * interpreter, so a build with no JIT still compares two runs. */
        if (pass == 1u) {
            g4mh_backend = &g4mh_backend_interp;
        }
        if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) {
            CHECK(false);
            g4mh_backend = saved;
            return;
        }
        counts[pass] = sreg(G4MH_SELID_PM, G4MH_SR_PMCOUNT0);
    }
    g4mh_backend = saved;

    CHECK(counts[0] > 0u);
    CHECK_EQ(counts[0], counts[1]);
}

static void test_clip(void)
{
    /*
     *   mov    -1, r10
     *   clip.b  r10, r11    ; -1 in range   -> -1, OV clear
     *   clip.bu r10, r12    ; huge unsigned -> 255, OV set
     *   mov    200, r13
     *   clip.b  r13, r14    ; 200 > 127     -> 127, OV set
     *   clip.hu r13, r15    ; 200 in range  -> 200
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, -1, 10),
        W0(OP_SYSTEM, 10, 11), SUB_CLIPB,
        W0(OP_SYSTEM, 10, 12), SUB_CLIPBU,
        W0(OP_MOVEA, 13, 0), 0x00C8u, 0x0000u,   /* mov 200, r13 */
        W0(OP_SYSTEM, 13, 14), SUB_CLIPB,
        W0(OP_SYSTEM, 13, 15), SUB_CLIPHU,
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 0xFFFFFFFFu);   /* signed: -1 is in range      */
    CHECK_EQ(reg(12), 255u);          /* unsigned: saturates high    */
    CHECK_EQ(reg(14), 127u);          /* signed: saturates high      */
    CHECK_EQ(reg(15), 200u);          /* halfword unsigned: in range */
    /* The last CLIP did not saturate, but SAT is sticky and two before
     * it did -- so it must still be set. */
    CHECK((psw() & G4MH_PSW_SAT) != 0u);
    CHECK((psw() & G4MH_PSW_OV) == 0u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * FETRAP, which shared opcode 0x02 with DIVH and was being decoded as it.
 *
 * The tell is that a mis-decoded FETRAP does not fault: DIVH by r0 is a
 * divide by zero, which on this architecture sets OV and retires. So the
 * check is not "does it trap" but "did the pc go to the FE-level handler
 * and did FEPC hold the return address" -- a wrong implementation reaches
 * the halt with OV set and FEPC untouched.
 */
static void test_fetrap(void)
{
    /*
     *   fetrap 3
     *   halt                ; only reached if the trap did not happen
     */
    const uint16_t prog[] = {
        (uint16_t)((3u << 11) | (0x02u << 5) | 0u),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* FE level: the return pc is the next instruction, and it lands in
     * FEPC rather than EIPC. */
    CHECK_EQ(sreg(0, G4MH_SR_FEPC), EMU_GUEST_RAM_BASE + 2u);
    CHECK_EQ(sreg(0, G4MH_SR_FEIC), G4MH_EXC_FETRAP + 3u);
    /* PSW.EP and PSW.NP are set on entry, and PSW.ID with them. */
    CHECK((psw() & G4MH_PSW_EP) != 0u);
    CHECK((psw() & G4MH_PSW_ID) != 0u);
    /* And it is not a divide: OV must be untouched. */
    CHECK((psw() & G4MH_PSW_OV) == 0u);
}

/*
 * RESBANK shares reg2 == 0 with DI and differs only in reg3, so decoding
 * on reg2 alone ran it as DI -- masking interrupts and restoring nothing.
 * Register banks are not modelled, so the correct report is RIE; what
 * this pins is that it is *not* silently DI.
 */
static void test_resbank_is_not_di(void)
{
    /*
     *   ei                  ; clear PSW.ID so a stray DI is visible
     *   resbank             ; must raise RIE, not set ID
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_SYSTEM, 0, 0x10u), SUB_DIEI,          /* ei      */
        W0(OP_SYSTEM, 0, 0x00u), (uint16_t)((16u << 11) | SUB_DIEI),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* RIE is FE level, so the cause lands in FEIC. Decoded as DI it would
     * have set PSW.ID and fallen through to the halt with FEIC zero. */
    CHECK_EQ(sreg(0, G4MH_SR_FEIC), G4MH_EXC_RIE);
}

/*
 * The narrow link/store-conditional pairs. LDL.BU and LDL.HU are
 * zero-extending loads -- there is no signed form -- and STC.B/STC.H
 * store only while the reservation stands.
 */
static void test_narrow_atomics(void)
{
    /*
     *   mov   <ram>, r10
     *   mov   0xAB, r11
     *   st.b  r11, 0[r10]
     *   ldl.bu [r10], r12    ; r12 = 0xAB, reservation taken
     *   mov   0x5C, r13
     *   stc.b r13, [r10]     ; succeeds, r13 = 1
     *   ldl.bu [r10], r14    ; r14 = 0x5C
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0),
            (uint16_t)(TEST_SCRATCH & 0xFFFFu),
            (uint16_t)(TEST_SCRATCH >> 16),
        W0(OP_MOVEA, 11, 0), 0x00ABu, 0x0000u,
        W0(OP_ST_B, 10, 11), 0x0000u,
        W0(OP_SYSTEM, 10, 1), (uint16_t)((12u << 11) | SUB_LDLBU),
        W0(OP_MOVEA, 13, 0), 0x005Cu, 0x0000u,
        W0(OP_SYSTEM, 10, 0), (uint16_t)((13u << 11) | SUB_STCB),
        W0(OP_SYSTEM, 10, 1), (uint16_t)((14u << 11) | SUB_LDLHU),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(12), 0xABu);         /* zero-extended, not sign      */
    CHECK_EQ(reg(13), 1u);            /* the conditional store stood  */
    CHECK_EQ(reg(14), 0x005Cu);       /* halfword read of the byte    */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * LD/ST with a pointer update -- the other half of the 0x370-0x37A slot.
 *
 * These share their six sub-opcodes with LDL/STC and are told apart by
 * reg2, which is an opcode extension here rather than a register. The
 * interpreter matched on the sub-opcode alone, so every one of them ran
 * as the link form: the access itself was correct and the pointer never
 * moved. Nothing faulted and nothing computed a wrong value -- a loop
 * simply re-read element 0 for ever, which is why `barrier3` reported
 * three cores' results as `7 7 7` when the memory held 7, 107 and 207.
 *
 * What this checks, and why each part is here:
 *
 *   - **The pointer, not just the loaded value.** Reading the right word
 *     is what the buggy version already did; `mov r10, rN` after each
 *     access is the part that would have failed.
 *   - **A byte with bit 7 set.** 0x80 is the input that separates LD.B
 *     from LD.BU, and the extension bit that picks between them is the
 *     same field being tested. A test using 0x5A would pass against an
 *     implementation that ignored it.
 *   - **Post-decrement as well as post-increment**, since they differ by
 *     one bit of the same field.
 *   - **The store half read back through LDL.BU**, not through the loads
 *     above. Verifying a post-increment store with a post-increment load
 *     would let a symmetric error cancel, which is exactly how the CMOVF
 *     test came to pass against an inverted implementation.
 */
static void test_pointer_update_addressing(void)
{
    /*
     *   mov    <scratch>, r10
     *   mov    0x80, r11
     *   st.b   r11, 0[r10]          ; [s+0] = 0x80, top bit set
     *   mov    0x7F, r11
     *   st.b   r11, 1[r10]          ; [s+1] = 0x7F
     *
     *   ld.b   [r10]+, r12          ; r12 = 0xFFFFFF80, r10 = s+1
     *   ld.bu  [r10]+, r13          ; r13 = 0x0000007F, r10 = s+2
     *   mov    r10, r14             ; r14 = s+2
     *   ld.bu  [r10]-, r15          ; r10 = s+1
     *   mov    r10, r16             ; r16 = s+1
     *
     *   mov    <scratch>+8, r17
     *   mov    0x5A, r18
     *   st.b   r18, [r17]+          ; [s+8] = 0x5A, r17 = s+9
     *   mov    0xA5, r18
     *   st.b   r18, [r17]+          ; [s+9] = 0xA5, r17 = s+10
     *   mov    r17, r19             ; r19 = s+10
     *   mov    <scratch>+8, r20
     *   ldl.bu [r20], r21           ; r21 = 0x5A   (independent read-back)
     *   mov    <scratch>+9, r20
     *   ldl.bu [r20], r22           ; r22 = 0xA5
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVEA, 10, 0),
            (uint16_t)(TEST_SCRATCH & 0xFFFFu),
            (uint16_t)(TEST_SCRATCH >> 16),
        W0(OP_MOVEA, 11, 0), 0x0080u, 0x0000u,
        W0(OP_ST_B, 10, 11), 0x0000u,
        W0(OP_MOVEA, 11, 0), 0x007Fu, 0x0000u,
        W0(OP_ST_B, 10, 11), 0x0001u,

        W0(OP_SYSTEM, 10, EXT_POSTI),  (uint16_t)((12u << 11) | SUB_LDLBU),
        W0(OP_SYSTEM, 10, EXT_POSTIU), (uint16_t)((13u << 11) | SUB_LDLBU),
        W0(0x00u, 10, 14),
        W0(OP_SYSTEM, 10, EXT_POSTDU), (uint16_t)((15u << 11) | SUB_LDLBU),
        W0(0x00u, 10, 16),

        W0(OP_MOVEA, 17, 0),
            (uint16_t)((TEST_SCRATCH + 8u) & 0xFFFFu),
            (uint16_t)((TEST_SCRATCH + 8u) >> 16),
        W0(OP_MOVEA, 18, 0), 0x005Au, 0x0000u,
        W0(OP_SYSTEM, 17, EXT_POSTI),  (uint16_t)((18u << 11) | SUB_STCB),
        W0(OP_MOVEA, 18, 0), 0x00A5u, 0x0000u,
        W0(OP_SYSTEM, 17, EXT_POSTI),  (uint16_t)((18u << 11) | SUB_STCB),
        W0(0x00u, 17, 19),

        W0(OP_MOVEA, 20, 0),
            (uint16_t)((TEST_SCRATCH + 8u) & 0xFFFFu),
            (uint16_t)((TEST_SCRATCH + 8u) >> 16),
        W0(OP_SYSTEM, 20, EXT_LINKU),  (uint16_t)((21u << 11) | SUB_LDLBU),
        W0(OP_MOVEA, 20, 0),
            (uint16_t)((TEST_SCRATCH + 9u) & 0xFFFFu),
            (uint16_t)((TEST_SCRATCH + 9u) >> 16),
        W0(OP_SYSTEM, 20, EXT_LINKU),  (uint16_t)((22u << 11) | SUB_LDLBU),

        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(12), 0xFFFFFF80u);            /* LD.B  sign-extends   */
    CHECK_EQ(reg(13), 0x0000007Fu);            /* LD.BU zero-extends   */
    CHECK_EQ(reg(14), TEST_SCRATCH + 2u);      /* two increments        */
    CHECK_EQ(reg(16), TEST_SCRATCH + 1u);      /* one decrement         */
    CHECK_EQ(reg(19), TEST_SCRATCH + 10u);     /* two store increments  */
    CHECK_EQ(reg(21), 0x5Au);                  /* first byte landed     */
    CHECK_EQ(reg(22), 0xA5u);                  /* second byte landed    */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * The swap group, checked on both the register result and PSW.
 *
 * The flags are the reason these instructions exist -- they let an endian
 * conversion test in one instruction whether the converted value contains
 * a zero element -- and each of the four computes them over a *different*
 * width. A test that only checked reg3 would pass against an
 * implementation that got all four flag widths wrong, so every case reads
 * PSW back with STSR.
 *
 * 0x00FF0000 is chosen because it distinguishes all four: it has zero
 * bytes and a zero halfword, and the results differ in whether the zero
 * lands in the part each instruction looks at.
 */
static void test_swap(void)
{
    /*
     *   mov   0x00FF0000, r10      ; via movhi
     *   bsw   r10, r11             ; r11 = 0x0000FF00
     *   stsr  PSW, r12
     *   hsw   r10, r13             ; r13 = 0x000000FF
     *   stsr  PSW, r14
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 10), 0x00FFu,                    /* movhi 0x00FF,r0,r10 */
        W0(OP_SYSTEM, 0, 10), (uint16_t)((11u << 11) | SUB_BSW),
        W0(OP_SYSTEM, 5, 12), (uint16_t)((0u << 11) | SUB_STSR),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((13u << 11) | SUB_HSW),
        W0(OP_SYSTEM, 5, 14), (uint16_t)((0u << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 0x00FF0000u);
    CHECK_EQ(reg(11), 0x0000FF00u);   /* BSW: whole-word byte reverse   */
    CHECK_EQ(reg(13), 0x000000FFu);   /* HSW: halfword exchange         */

    /*
     * BSW result 0x0000FF00 has zero bytes, is non-zero as a word and has
     * a clear MSB: CY set, Z clear, S clear, OV clear.
     */
    CHECK_EQ(reg(12) & G4MH_PSW_FLAGS, G4MH_PSW_CY);
    /*
     * HSW result 0x000000FF contains a zero halfword (the upper one), so
     * CY is set; the word is non-zero so Z is clear.
     */
    CHECK_EQ(reg(14) & G4MH_PSW_FLAGS, G4MH_PSW_CY);
}

/*
 * BSH and HSH, whose flags are computed on the *lower halfword* where
 * BSW's and HSW's are on the word. Separated from the test above so that
 * a single wrong width fails one case rather than all four.
 */
static void test_swap_halfword_flags(void)
{
    /*
     *   mov   0x12340000, r10
     *   bsh   r10, r11        ; r11 = 0x34120000, lower halfword zero
     *   stsr  PSW, r12
     *   hsh   r10, r13        ; r13 = 0x12340000, lower halfword zero
     *   stsr  PSW, r14
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 10), 0x1234u,
        W0(OP_SYSTEM, 0, 10), (uint16_t)((11u << 11) | SUB_BSH),
        W0(OP_SYSTEM, 5, 12), (uint16_t)((0u << 11) | SUB_STSR),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((13u << 11) | SUB_HSH),
        W0(OP_SYSTEM, 5, 14), (uint16_t)((0u << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* BSH swaps the bytes inside each halfword and leaves the halfwords. */
    CHECK_EQ(reg(11), 0x34120000u);
    /* HSH does not move anything; it is a move that sets flags. */
    CHECK_EQ(reg(13), 0x12340000u);

    /*
     * Both results have a zero lower halfword, so for both Z is set (the
     * *lower halfword* is zero, though the word is not) and CY is set.
     * This is exactly the case that separates these two from BSW/HSW: on
     * word-width flags Z would be clear.
     */
    CHECK_EQ(reg(12) & G4MH_PSW_FLAGS, G4MH_PSW_CY | G4MH_PSW_Z);
    CHECK_EQ(reg(14) & G4MH_PSW_FLAGS, G4MH_PSW_CY | G4MH_PSW_Z);
}

/*
 * Bit search.
 *
 * The result is a one-based distance from the end the search started at,
 * so the two interesting inputs are the one where the match is at the
 * first bit examined (result 1) and the one where there is no match at
 * all (result 0, Z set) -- an off-by-one implementation returning a plain
 * bit index gets 0 for the first and is then indistinguishable from
 * not-found.
 */
static void test_bit_search(void)
{
    /*
     *   mov   1, r10
     *   sch1r r10, r11        ; bit 0 set, searching from LSB -> 1
     *   sch1l r10, r12        ; found at the far end            -> 32, CY
     *   stsr  PSW, r13
     *   mov   0, r14
     *   sch1r r14, r15        ; no bit set -> 0, Z
     *   stsr  PSW, r16
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 1, 10),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((11u << 11) | SUB_SCH1R),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((12u << 11) | SUB_SCH1L),
        W0(OP_SYSTEM, 5, 13), (uint16_t)((0u << 11) | SUB_STSR),
        F2(OP_MOVI, 0, 14),
        W0(OP_SYSTEM, 0, 14), (uint16_t)((15u << 11) | SUB_SCH1R),
        W0(OP_SYSTEM, 5, 16), (uint16_t)((0u << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 1u);    /* first bit examined                   */
    CHECK_EQ(reg(12), 32u);   /* last bit examined                    */
    CHECK_EQ(reg(15), 0u);    /* not found                            */

    /* SCH1L found its bit at the far end, so CY; the result is non-zero. */
    CHECK_EQ(reg(13) & G4MH_PSW_FLAGS, G4MH_PSW_CY);
    /* Not found sets Z and clears CY. */
    CHECK_EQ(reg(16) & G4MH_PSW_FLAGS, G4MH_PSW_Z);
}

/*
 * SCH0L, which searches for a *zero* -- the complement path.
 * 0xFFFFFFFE has its only zero at bit 0, so from the MSB that is a full
 * 32-bit walk, and from the LSB it is the first bit examined.
 */
static void test_bit_search_zero(void)
{
    /*
     *   mov   -2, r10          ; 0xFFFFFFFE
     *   sch0l r10, r11         ; -> 32
     *   sch0r r10, r12         ; -> 1
     *   mov   -1, r13          ; 0xFFFFFFFF, no zero anywhere
     *   sch0l r13, r14         ; -> 0, Z
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, -2, 10),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((11u << 11) | SUB_SCH0L),
        W0(OP_SYSTEM, 0, 10), (uint16_t)((12u << 11) | SUB_SCH0R),
        F2(OP_MOVI, -1, 13),
        W0(OP_SYSTEM, 0, 13), (uint16_t)((14u << 11) | SUB_SCH0L),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 32u);
    CHECK_EQ(reg(12), 1u);
    CHECK_EQ(reg(14), 0u);
}

/*
 * reg1 is a fixed zero in the whole swap/search group, so a non-zero
 * there is not one of these instructions.
 *
 * Worth a test of its own because this is the shape that has already bit
 * this frontend once: an ISA that reuses a register field as an opcode
 * extension will not tell you when you ignore it, and the failure is a
 * silent wrong answer rather than an exception.
 */
static void test_swap_reserved_field(void)
{
    const uint16_t prog[] = {
        W0(OP_SYSTEM, 1, 10), (uint16_t)((11u << 11) | SUB_BSW),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }
    /* RIE is an FE-level exception, so it lands in FEIC, not in r11. */
    CHECK_EQ(reg(11), 0u);
}

/*
 * SET1 / CLR1 / NOT1 / TST1, both encodings.
 *
 * Z is the interesting part: it reports the bit as it was *before* the
 * instruction changed it. An implementation that computed Z from the
 * result would give SET1 a permanently clear Z and CLR1 a permanently
 * set one, and both would still leave the right byte in memory -- so the
 * test sets a bit that was clear and clears a bit that was set, and
 * checks Z each time.
 */
static void test_bit_manipulation(void)
{
    /*
     *   movhi 0x8000, r0, r10     ; r10 = RAM base + 0x100, clear of the
     *   movea 0x100, r10, r10     ;   program itself
     *   mov   1, r11
     *   st.b  r11, 0[r10]         ; [0x100] = 0x01
     *   set1  1, 0[r10]           ; bit 1 was 0 -> Z=1, byte becomes 0x03
     *   stsr  PSW, r12
     *   clr1  0, 0[r10]           ; bit 0 was 1 -> Z=0, byte becomes 0x02
     *   stsr  PSW, r13
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 10),  0x8000u,
        W0(OP_MOVEA, 10, 10), 0x0100u,
        F2(OP_MOVI, 1, 11),
        W0(OP_ST_B, 10, 11), 0x0000u,          /* st.b r11, 0[r10] */
        BITOP8(BOP_SET1, 1, 10), 0x0000u,      /* set1 1, 0[r10]   */
        W0(OP_SYSTEM, 5, 12), (uint16_t)((0u << 11) | SUB_STSR),
        BITOP8(BOP_CLR1, 0, 10), 0x0000u,      /* clr1 0, 0[r10]   */
        W0(OP_SYSTEM, 5, 13), (uint16_t)((0u << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* 0x01, set bit 1 -> 0x03, clear bit 0 -> 0x02. */
    CHECK_EQ((uint32_t)g_ram[0x100], 0x02u);
    CHECK((reg(12) & G4MH_PSW_Z) != 0u);   /* bit 1 had been 0 */
    CHECK((reg(13) & G4MH_PSW_Z) == 0u);   /* bit 0 had been 1 */
}

/*
 * TST1 must not write, and the register form must take its bit number
 * from the value in reg2 rather than from the opcode.
 */
static void test_bit_manipulation_reg(void)
{
    /*
     *   movhi 0x8000, r0, r10
     *   movea 0x100, r10, r10
     *   movea 0x80, r0, r11    ; imm5 only reaches -16..15, so movea
     *   st.b  r11, 0[r10]      ; [0x100] = 0x80
     *   mov   0, r12
     *   tst1  r12, [r10]       ; bit 0 is 0 -> Z=1, memory unchanged
     *   ld.b  0[r10], r16      ; read it back to prove nothing was written
     *   stsr  PSW, r13
     *   mov   3, r14
     *   not1  r14, [r10]       ; bit 3 was 0 -> Z=1, byte becomes 0x88
     *   stsr  PSW, r15
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 10),  0x8000u,
        W0(OP_MOVEA, 10, 10), 0x0100u,
        W0(OP_MOVEA, 0, 11), 0x0080u,          /* movea 0x80, r0, r11 */
        W0(OP_ST_B, 10, 11), 0x0000u,
        F2(OP_MOVI, 0, 12),
        W0(OP_SYSTEM, 10, 12), SUB_TST1,
        W0(OP_LD_B, 10, 16), 0x0000u,
        W0(OP_SYSTEM, 5, 13), (uint16_t)((0u << 11) | SUB_STSR),
        F2(OP_MOVI, 3, 14),
        W0(OP_SYSTEM, 10, 14), SUB_NOT1,
        W0(OP_SYSTEM, 5, 15), (uint16_t)((0u << 11) | SUB_STSR),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /*
     * r16 is the byte as TST1 left it. Tested on a *clear* bit and read
     * back immediately, because the first version of this test used a
     * bit that was already set: a TST1 that wrongly wrote back would
     * have stored the value unchanged and the test would have passed
     * either way. Proven by making TST1 store and watching this fail.
     */
    CHECK_EQ(reg(16), 0xFFFFFF80u);   /* LD.B sign-extends */
    /* NOT1 toggled bit 3. */
    CHECK_EQ((uint32_t)g_ram[0x100], 0x88u);
    CHECK((reg(13) & G4MH_PSW_Z) != 0u);   /* bit 0 was 0 */
    CHECK((reg(15) & G4MH_PSW_Z) != 0u);   /* bit 3 was 0 */
}

/*
 * The IR path, and proof that it is the one being used.
 *
 * The G4MH JIT now goes guest -> IR -> optimise -> x86-64, and on this
 * host it is the default backend, so every test above already runs
 * through it. That is exactly the situation this project has been caught
 * by before: a backend that declines everything and falls back to the
 * interpreter passes every test while proving nothing about the
 * translator. The first x86-64 run interpreted 92% of the RV32 self-test
 * and looked perfectly healthy.
 *
 * So this checks the *ratio*, not just the answer. The program is
 * nothing but instructions the IR lowering covers, so translation must
 * happen and the fallback count must stay near zero.
 */
static void test_ir_backend_is_used(void)
{
    /*
     *   mov   9, r10
     *   mov   4, r11
     *   add   r11, r10      ; 13
     *   sub   r11, r10      ; 9
     *   or    r11, r10      ; 13
     *   and   r11, r10      ; 4
     *   xor   r11, r10      ; 0
     *   cmp   r11, r10
     *   halt
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, 9, 10),
        F2(OP_MOVI, 4, 11),
        F1(OP_ADD, 11, 10),
        F1(OP_SUB, 11, 10),
        F1(OP_OR,  11, 10),
        F1(OP_AND, 11, 10),
        F1(0x09u,  11, 10),          /* XOR */
        F1(OP_CMP, 11, 10),
        /*
         * A store and a load through the generic memory ops. Included
         * here rather than in a test of their own because what is under
         * test is the *ratio*: these used to end the block and send the
         * rest of it to the interpreter, and nothing about the values
         * would have shown that.
         */
        W0(OP_MOVHI, 0, 20),  0x8000u,
        W0(OP_MOVEA, 20, 20), 0x0100u,
        W0(OP_ST_HW, 20, 11), 0x0001u,   /* st.w r11, 0[r20] */
        W0(OP_LD_HW, 20, 21), 0x0001u,   /* ld.w 0[r20], r21 */
        /*
         * A Format VIII bit op and a Format IV short store, both of
         * which used to end the block. Included here rather than tested
         * only for their values, because a lowering that declines them
         * still leaves every value correct -- the interpreter runs them
         * -- and only the fallback count says which happened.
         */
        BITOP8(BOP_SET1, 2, 20), 0x0000u,      /* set1 2, 0[r20] */
        F1(OP_MOV, 20, 30),                    /* ep = r20       */
        (uint16_t)((21u << 11) | (0x07u << 7) | 4u),  /* sst.b r21, 4[ep] */
        0x07E0u, SUB_HALT,
    };

    emu_jit_stats_t before, after;
    emu_jit_get_stats(&before);

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }
    emu_jit_get_stats(&after);

    /* The arithmetic itself, which the IR lowering produced. */
    CHECK_EQ(reg(10), 0u);
    CHECK_EQ(reg(11), 4u);
    /* And the round trip through guest memory. */
    CHECK_EQ(reg(21), 4u);
    /* set1 2 turned 0x04 into 0x04|0x04 = 0x04; sst.b wrote r21 at +4. */
    CHECK_EQ((uint32_t)g_ram[0x104], 4u);

    /*
     * If this is zero the whole block fell back and every check above is
     * testing the interpreter.
     */
    CHECK(after.translations > before.translations);

    /*
     * Only the HALT should reach the interpreter: it is a 32-bit
     * encoding, which the frontend's lowering declines by form. Eight
     * 16-bit instructions ahead of it must all have been translated.
     */
    /*
     * An *exact* count, not a bound.
     *
     * A loose bound does not discriminate: a declined instruction ends
     * the block, the interpreter runs one instruction, and a fresh block
     * starts after it -- so three declines cost three fallbacks, and
     * `<= 2` quietly allowed most of this program to be interpreted.
     * Disabling whole instruction groups changed nothing and the test
     * still passed, which is the failure this project keeps rediscovering
     * in its own coverage checks.
     *
     * Only the 32-bit HALT should reach the interpreter.
     */
    const uint32_t fell_back = after.interp_fallbacks - before.interp_fallbacks;
    CHECK_EQ(fell_back, 1u);
}

static void test_system_registers(void)
{
    /*
     *   mov   -1, r10
     *   ldsr  r10, eipc          ; system register write
     *   stsr  eipc, r11          ; and read back
     *   halt
     *
     * The point of the test is the field order, which the two instructions
     * use in opposite senses:
     *
     *   LDSR  bits[15:11] = regID,  bits[4:0] = reg2  (GPR source)
     *   STSR  bits[15:11] = reg2,   bits[4:0] = regID (SR source)
     *
     * so the encodings below are *not* symmetric, and a test that built
     * them symmetrically would pass against an implementation that had
     * LDSR backwards.
     */
    const uint16_t prog[] = {
        F2(OP_MOVI, -1, 10),
        W0(OP_SYSTEM, 10, G4MH_SR_EIPC), SUB_LDSR,
        W0(OP_SYSTEM, G4MH_SR_EIPC, 11), SUB_STSR,
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 0xFFFFFFFFu);
}

static void test_reserved_instruction(void)
{
    /*
     * An encoding the interpreter does not implement must raise RIE and
     * land in the handler, not silently retire.
     *
     * This used to use opcode 0x3E, which was then the whole
     * unimplemented Format VIII group; 0x3E is now the bit-manipulation
     * instructions, so it points at an unassigned sub-opcode of the
     * system group instead. That is the maintenance cost of this test:
     * it has to name something genuinely absent, and implementing an
     * instruction can take its example away.
     *
     * RBASE defaults to the reset pc, so the RIE handler is at
     * reset + 0x60; a HALT is planted there to prove control arrived.
     */
    uint16_t prog[0x40];
    memset(prog, 0, sizeof(prog));
    prog[0] = W0(OP_SYSTEM, 0, 0);
    /*
     * Must be *below* 0x400: everything from there up is the
     * floating-point group, and with PSW.CU0 clear -- which is how the
     * core comes out of reset -- an FP encoding is a coprocessor-unusable
     * exception rather than a reserved-instruction one. This test used
     * 0x7FE and started failing the day the FPU landed, which is exactly
     * the maintenance cost its own comment predicts.
     *
     * It also has to miss the masked switch: sub-opcodes 0x300-0x3FF are
     * matched as `sub & 0x7E0`, so anything in that range is some CMOV,
     * SBF, ADF or MAC and retires quietly.
     */
    prog[1] = 0x01A0u;              /* no such sub-opcode */
    /* 0x60 bytes in, which is index 0x30 in halfwords. */
    prog[0x30] = 0x07E0u;
    prog[0x31] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK(st.traps >= 1u);
    /*
     * The handler is at RBASE + 0x60 and HALT is a 32-bit instruction, so
     * a core that got there and executed it stops four bytes further on.
     */
    CHECK_EQ(st.pc, EMU_GUEST_RAM_BASE + 0x64u);
}

static bool g_hook_seen;
static uint32_t g_hook_nr;
static uint32_t g_hook_arg0;

static bool exit_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)user;
    g_hook_seen = true;
    g_hook_nr = sc->nr;
    g_hook_arg0 = sc->arg[0];
    sc->ret = 0x5A5Au;
    g_core.ops->halt(cpu);
    return true;
}

static void test_trap_and_syscall(void)
{
    /*
     * TRAP is how the host runner offers write and exit to a G4MH guest,
     * so the hook is the one part of the contract a platform's whole test
     * harness depends on. Run twice: once with a hook, which must consume
     * the call, and once without, which must take the architectural
     * exception rather than silently retiring.
     *
     *   mov  9, r6         ; the first argument register
     *   trap 3             ; vector 3 becomes the syscall number
     *   halt
     */
    uint16_t prog[0x40];
    memset(prog, 0, sizeof(prog));
    /*
     * r11 carries the syscall number and r6 the first argument; the TRAP
     * vector is deliberately a *different* value, because the two used to
     * be conflated. The vector is five bits, so it cannot name the numbers
     * the host harness answers to (newlib's 64 and 93) -- every syscall
     * from a real ccrh-built guest fell through to the architectural trap.
     * Assembling the words here had encoded the same misunderstanding, so
     * this test agreed with the bug until an actual RH850 binary ran.
     */
    prog[0] = F2(OP_MOVI, 9, 6);    /* r6  = 9, argument 0            */
    prog[1] = F2(OP_MOVI, 11, 11);  /* r11 = 11, the syscall number   */
    prog[2] = W0(OP_SYSTEM, 3, 0);  /* trap 3 -- vector, not number   */
    prog[3] = SUB_TRAP;
    prog[4] = 0x07E0u;              /* halt, if the trap is consumed  */
    prog[5] = SUB_HALT;
    /* The TRAP 0-15 handler, at RBASE + 0x40. */
    prog[0x20] = 0x07E0u;
    prog[0x21] = SUB_HALT;
    const unsigned n = sizeof(prog) / sizeof(prog[0]);

    emu_run_reason_t why;
    uint32_t retired = 0;

    /* --- with a hook: consumed, and the arguments arrive unpacked --- */
    g_hook_seen = false;
    if (!load_and_run_hooked(prog, n, 64u, &why, &retired, exit_hook)) {
        CHECK(false);
        return;
    }
    CHECK(g_hook_seen);
    /* From r11, not from the TRAP vector, which is 3 here. */
    CHECK_EQ(g_hook_nr, 11u);
    CHECK_EQ(g_hook_arg0, 9u);    /* r6 is argument 0 */
    CHECK_EQ(reg(10), 0x5A5Au);   /* and the result lands in r10 */
    CHECK_EQ(why, EMU_RUN_HALTED);

    /* --- without: the architectural exception, at RBASE + 0x40 ------ */
    g_hook_seen = false;
    if (!load_and_run(prog, n, 64u, &why, &retired)) {
        CHECK(false);
        return;
    }
    CHECK(!g_hook_seen);

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);
    CHECK(st.traps >= 1u);
    /* Reached the TRAP 0-15 handler at RBASE + 0x40 and halted there. */
    CHECK_EQ(st.pc, EMU_GUEST_RAM_BASE + 0x44u);
}

/* ------------------------------------------------------------------ */
/* Contract                                                            */
/* ------------------------------------------------------------------ */

static void dump_sink(void *ctx, const char *s)
{
    size_t *n = (size_t *)ctx;
    while (*s++ != '\0') {
        (*n)++;
    }
}

static void test_contract(void)
{
    const emu_cpu_ops_t *ops = emu_frontend_find("g4mh");
    CHECK(ops != NULL);
    if (ops == NULL) {
        return;
    }

    /*
     * Every member a platform calls unconditionally must be present. A
     * frontend that leaves one NULL compiles and then crashes inside the
     * host runner, which is a poor place to find out.
     */
    CHECK(ops->instance != NULL);
    CHECK(ops->init != NULL);
    CHECK(ops->reset != NULL);
    CHECK(ops->boot != NULL);
    CHECK(ops->run != NULL);
    CHECK(ops->status != NULL);
    CHECK(ops->dump != NULL);
    CHECK(ops->halt != NULL);
    CHECK(ops->set_syscall != NULL);
    CHECK(ops->set_cache != NULL);
    CHECK(ops->set_unmask_hook != NULL);
    CHECK(ops->reg_name != NULL);
    CHECK(ops->reg_read != NULL);
    CHECK(ops->reg_write != NULL);
    CHECK_EQ(ops->nregs, 32u);
    CHECK_EQ(ops->elf_machine, EMU_EM_V850);

    /* The frontend registry finds it by name and by ELF machine. */
    CHECK(emu_frontend_for_elf(EMU_EM_V850) == ops);

    /* Cores past the configured count must be refused rather than
     * silently aliased onto core 0. */
    CHECK(ops->instance(0u) != NULL);
    CHECK_EQ(ops->ncores, G4MH_PE_COUNT);
    CHECK(ops->instance(G4MH_PE_COUNT) == NULL);

    /* dump must produce something and must terminate. */
    const uint16_t prog[] = { 0x07E0u, SUB_HALT };
    emu_run_reason_t why;
    uint32_t retired = 0;
    if (load_and_run(prog, 2u, 8u, &why, &retired)) {
        size_t n = 0;
        ops->dump(g_core.cpu, dump_sink, &n);
        CHECK(n > 64u);
    }

    /* r0 is hardwired: writing it through the contract must not stick. */
    ops->reg_write(g_core.cpu, 0u, 0xFFFFFFFFu);
    CHECK_EQ(ops->reg_read(g_core.cpu, 0u), 0u);
}

/* ------------------------------------------------------------------ */
/* Atomics and multicore                                               */
/* ------------------------------------------------------------------ */

/* Sub-opcodes of the atomics, from the G4MH software manual. */
#define SUB_CAXI    0x00EEu
#define SUB_LDLW    0x0378u
#define SUB_STCW    0x037Au

static void test_mc_reservation_succeeds(void)
{
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 11),  0x8000u,
        W0(OP_MOVEA, 11, 11), 0x0300u,
        W0(OP_SYSTEM, 11, 0), (uint16_t)((12u << 11) | SUB_LDLW),
        F2(OP_MOVI, 7, 13),
        W0(OP_SYSTEM, 11, 0), (uint16_t)((13u << 11) | SUB_STCW),
        W0(OP_ST_HW, 11, 13), 0x0005u,
        0x07E0u, SUB_HALT,
    };

    /* One core only, so nothing can break the reservation. */
    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(ram32(0x300u), 7u);   /* the store happened */
    CHECK_EQ(ram32(0x304u), 1u);   /* and reported success */
}

static void test_mc_caxi(void)
{
    /*
     *   movhi 0x8000, r0, r11
     *   movea 0x400, r11, r11
     *   mov   5, r12
     *   st.w  r12, 0[r11]        ; memory = 5
     *   mov   5, r13             ; comparand matches
     *   mov   8, r14             ; new value
     *   caxi  [r11], r13, r14    ; swaps; r14 <- old (5)
     *   st.w  r14, 4[r11]
     *   halt
     */
    const uint16_t prog[] = {
        W0(OP_MOVHI, 0, 11),  0x8000u,
        W0(OP_MOVEA, 11, 11), 0x0400u,
        F2(OP_MOVI, 5, 12),
        W0(OP_ST_HW, 11, 12), 0x0001u,
        F2(OP_MOVI, 5, 13),
        F2(OP_MOVI, 8, 14),
        W0(OP_SYSTEM, 11, 13), (uint16_t)((14u << 11) | SUB_CAXI),
        W0(OP_ST_HW, 11, 14), 0x0005u,
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(ram32(0x400u), 8u);   /* swapped in */
    CHECK_EQ(ram32(0x404u), 5u);   /* old value reported */
}

#if G4MH_PE_COUNT > 1

static emu_system_t g_sys;
static emu_bus_t    g_sysbus[EMU_MAX_CORES];

/*
 * Bring up a whole machine with `prog` at the reset address, shared by
 * every core, and run it to completion or until `rounds` scheduling rounds
 * have passed.
 *
 * Every core starts at the same pc. That is how a real G4MH boots too --
 * the PEs come out of reset together and the first thing the code does is
 * read its own core id and branch -- which is exactly what the programs
 * below do, so the dispatch is part of what is under test.
 */
static bool run_system(const uint16_t *hw, unsigned n, uint32_t quantum,
                       unsigned rounds, uint64_t *retired)
{
    const emu_cpu_ops_t *ops = emu_frontend_find("g4mh");
    if (ops == NULL) {
        return false;
    }

    memset(g_ram, 0, sizeof(g_ram));
    (void)emit(0u, hw, n);

    for (unsigned i = 0; i < G4MH_PE_COUNT; i++) {
        emu_bus_init(&g_sysbus[i]);
        if (!emu_bus_add_ram(&g_sysbus[i], "ram", EMU_GUEST_RAM_BASE,
                             g_ram, TEST_RAM_SIZE)) {
            return false;
        }
    }
    if (!emu_system_open(&g_sys, ops, g_sysbus, G4MH_PE_COUNT)) {
        return false;
    }

    emu_system_reset(&g_sys, EMU_GUEST_RAM_BASE);
    emu_system_boot(&g_sys, EMU_GUEST_RAM_BASE, TEST_RAM_SIZE);

    /*
     * Release the secondary PEs, which is what a bootloader does.
     *
     * Only PE0 runs at reset release on this part (BOOTCTRL, U2B
     * 11.4.79); the others sit in EMU_STATE_HELD until something asserts
     * their bit. These programs are about barriers and reservations
     * rather than about start-up, so the harness plays the role PE0's
     * firmware would -- but it has to play it, and when BOOTCTRL was
     * added every one of these tests failed with the secondaries never
     * running, which is the correct new behaviour reported as eleven
     * broken tests.
     */
    (void)emu_bus_write(&g_sysbus[0], G4MH_BOOTCTRL_BASE, 4u,
                        (1u << G4MH_PE_COUNT) - 1u);

    uint64_t total = 0;
    for (unsigned r = 0; r < rounds; r++) {
        bool idle = false;
        total += emu_system_step(&g_sys, quantum, &idle);
        if (idle) {
            break;
        }
    }
    if (retired != NULL) {
        *retired = total;
    }
    return true;
}

/*
 * Each core writes its own id to its own slot and halts.
 *
 *   stsr  htcfg0, r10        ; r10 = this core's id  (sel 2, reg 0)
 *   shl   2, r10             ; byte offset
 *   movhi 0x8000, r0, r11
 *   movea 0x200, r11, r11    ; r11 = RAM + 0x200
 *   add   r10, r11
 *   mov   1, r12
 *   add   r10, r12           ; a distinguishable value per core
 *   st.w  r12, 0[r11]
 *   halt
 *
 * HTCFG0 is where a G4MH reports its PE number, which is how real startup
 * code dispatches. Modelled here as the core id.
 */
static void test_mc_dispatch(void)
{
    const uint16_t prog[] = {
        W0(OP_SYSTEM, G4MH_SR_HTCFG0, 10), (uint16_t)((2u << 11) | SUB_STSR),
        F2(OP_SHL, 2, 10),
        W0(OP_MOVHI, 0, 11),  0x8000u,
        W0(OP_MOVEA, 11, 11), 0x0200u,
        F1(OP_ADD, 10, 11),
        F2(OP_MOVI, 1, 12),
        F1(OP_ADD, 10, 12),
        W0(OP_ST_HW, 11, 12), 0x0001u,      /* st.w */
        0x07E0u, SUB_HALT,
    };

    if (!run_system(prog, sizeof(prog) / sizeof(prog[0]), 64u, 64u, NULL)) {
        CHECK(false);
        return;
    }

    /* Every core ran, and each wrote to its own slot. */
    for (unsigned i = 0; i < G4MH_PE_COUNT; i++) {
        CHECK_EQ(ram32(0x200u + i * 4u), 1u + i * 4u);
    }
}

/*
 * The same program at three quanta. Round-robin is deterministic, so the
 * result must not depend on the interleaving -- and if it does, either the
 * guest has a race or the scheduler does.
 */
static void test_mc_quantum_invariance(void)
{
    const uint16_t prog[] = {
        W0(OP_SYSTEM, G4MH_SR_HTCFG0, 10), (uint16_t)((2u << 11) | SUB_STSR),
        F2(OP_SHL, 2, 10),
        W0(OP_MOVHI, 0, 11),  0x8000u,
        W0(OP_MOVEA, 11, 11), 0x0200u,
        F1(OP_ADD, 10, 11),
        F2(OP_MOVI, 1, 12),
        F1(OP_ADD, 10, 12),
        W0(OP_ST_HW, 11, 12), 0x0001u,
        0x07E0u, SUB_HALT,
    };
    const unsigned n = sizeof(prog) / sizeof(prog[0]);

    uint64_t ret[3] = { 0, 0, 0 };
    const uint32_t q[3] = { 1u, 8u, 1024u };

    for (unsigned k = 0; k < 3u; k++) {
        if (!run_system(prog, n, q[k], 4096u, &ret[k])) {
            CHECK(false);
            return;
        }
        for (unsigned i = 0; i < G4MH_PE_COUNT; i++) {
            CHECK_EQ(ram32(0x200u + i * 4u), 1u + i * 4u);
        }
    }

    /* Same total work at every quantum, including lockstep. */
    CHECK_EQ((uint32_t)ret[0], (uint32_t)ret[1]);
    CHECK_EQ((uint32_t)ret[1], (uint32_t)ret[2]);
}

/*
 * LDL.W / STC.W across cores.
 *
 * Core 0 takes a reservation on a word; core 1 stores to it; core 0's
 * store-conditional must then fail. This is the one cross-core interaction
 * in the model, and it is the thing a spinlock is built out of -- if a
 * store from another core does not break the reservation, every lock in
 * the guest silently becomes a no-op.
 *
 * **The interleaving is made explicit rather than assumed.** The first
 * version relied on core 0's SNOOZE yielding at the right moment and on
 * the two cores reaching their stores in a particular round; they do
 * not. With a four-instruction quantum core 1 reached its store *before*
 * core 0 executed LDL.W, so the reservation was taken after the
 * interfering store, STC.W legitimately succeeded, and the test failed
 * against a correct implementation.
 *
 * The order is now enforced by the guest: core 1 spins until core 0
 * publishes a flag, which it does only after LDL.W. That makes the test
 * about the reservation rather than about the scheduler, and it passes at
 * any quantum -- which is the property test_mc_quantum_invariance asserts
 * for the whole model and which this test was quietly violating.
 *
 *   stsr  htcfg0, r10
 *   cmp   0, r10
 *   bne   core_other
 *   ; --- core 0 ---
 *   movhi 0x8000, r0, r11
 *   movea 0x300, r11, r11
 *   ldl.w [r11], r12          ; take the reservation
 *   snooze                    ; yield, so core 1 gets to run
 *   mov   7, r13
 *   stc.w r13, [r11]          ; must fail: r13 <- 0
 *   st.w  r13, 4[r11]         ; record the outcome
 *   halt
 * core_other:
 *   movhi 0x8000, r0, r11
 *   movea 0x300, r11, r11
 *   mov   9, r14
 *   st.w  r14, 0[r11]         ; breaks core 0's reservation
 *   halt
 */
static void test_mc_reservation(void)
{
    uint16_t prog[64];
    unsigned k = 0;
    memset(prog, 0, sizeof(prog));

    prog[k++] = W0(OP_SYSTEM, G4MH_SR_HTCFG0, 10);
    prog[k++] = (uint16_t)((2u << 11) | SUB_STSR);
    prog[k++] = F2(OP_CMPI5, 0, 10);
    /* bne to the "other core" path; filled in once its offset is known. */
    const unsigned bne_at = k++;

    /*
     * --- core 0 ---
     * ldl.w, then publish a flag at 0x308 so core 1 knows the
     * reservation exists, then snooze until core 1 has stored.
     */
    prog[k++] = W0(OP_MOVHI, 0, 11);  prog[k++] = 0x8000u;
    prog[k++] = W0(OP_MOVEA, 11, 11); prog[k++] = 0x0300u;
    prog[k++] = W0(OP_SYSTEM, 11, 0); prog[k++] = (uint16_t)((12u << 11) | SUB_LDLW);
    /* flag = 1: the reservation is taken. */
    prog[k++] = F2(OP_MOVI, 1, 15);
    prog[k++] = W0(OP_ST_HW, 11, 15); prog[k++] = 0x0009u;   /* st.w 8[r11] */
    /*
     * Wait for core 1's acknowledgement at 0x30C rather than snoozing a
     * fixed number of times: a count would be another timing assumption,
     * which is the defect this test had.
     */
    const unsigned wait0 = k;
    prog[k++] = W0(OP_LD_HW, 11, 16); prog[k++] = 0x000Du;   /* ld.w 12[r11] */
    prog[k++] = F2(OP_CMPI5, 0, 16);
    prog[k++] = BCOND(0x2u, 0u);      /* be -> snooze+loop; patched below */
    const unsigned be0 = k - 1u;
    prog[k++] = F2(OP_MOVI, 7, 13);
    prog[k++] = W0(OP_SYSTEM, 11, 0); prog[k++] = (uint16_t)((13u << 11) | SUB_STCW);
    prog[k++] = W0(OP_ST_HW, 11, 13); prog[k++] = 0x0005u;    /* st.w 4[r11] */
    prog[k++] = 0x07E0u;              prog[k++] = SUB_HALT;

    /* The snooze-and-retry the wait loop branches to. */
    const unsigned spin0 = k;
    prog[k++] = 0x0FE0u;              prog[k++] = SUB_HALT;   /* snooze */
    prog[k++] = BCOND(0xEu, 0u);      /* br back to wait0; patched below */
    const unsigned br0 = k - 1u;

    /*
     * --- other cores ---
     * Spin until core 0's flag appears, then store, then acknowledge.
     */
    const unsigned other = k;
    prog[k++] = W0(OP_MOVHI, 0, 11);  prog[k++] = 0x8000u;
    prog[k++] = W0(OP_MOVEA, 11, 11); prog[k++] = 0x0300u;
    const unsigned wait1 = k;
    prog[k++] = W0(OP_LD_HW, 11, 17); prog[k++] = 0x0009u;   /* ld.w 8[r11] */
    prog[k++] = F2(OP_CMPI5, 0, 17);
    prog[k++] = BCOND(0x2u, 0u);      /* be -> snooze+loop; patched below */
    const unsigned be1 = k - 1u;
    prog[k++] = F2(OP_MOVI, 9, 14);
    prog[k++] = W0(OP_ST_HW, 11, 14); prog[k++] = 0x0001u;   /* the breaking store */
    prog[k++] = F2(OP_MOVI, 1, 18);
    prog[k++] = W0(OP_ST_HW, 11, 18); prog[k++] = 0x000Du;   /* ack at 12[r11] */
    prog[k++] = 0x07E0u;              prog[k++] = SUB_HALT;

    const unsigned spin1 = k;
    prog[k++] = 0x0FE0u;              prog[k++] = SUB_HALT;   /* snooze */
    prog[k++] = BCOND(0xEu, 0u);      /* br back to wait1; patched below */
    const unsigned br1 = k - 1u;

    /* Bcond displacements are from the branch itself, in bytes. */
    prog[bne_at] = BCOND(0xAu, (other - bne_at) * 2u);
    prog[be0]    = BCOND(0x2u, (spin0 - be0) * 2u);
    prog[br0]    = BCOND(0xEu, (uint16_t)((wait0 - br0) * 2u));
    prog[be1]    = BCOND(0x2u, (spin1 - be1) * 2u);
    prog[br1]    = BCOND(0xEu, (uint16_t)((wait1 - br1) * 2u));

    /*
     * Three quanta, because the whole point is that the answer must not
     * depend on the interleaving. The old version passed at none of them.
     */
    static const uint32_t quanta[] = { 1u, 4u, 64u };

    for (unsigned q = 0; q < sizeof quanta / sizeof quanta[0]; q++) {
        if (!run_system(prog, k, quanta[q], 4096u, NULL)) {
            CHECK(false);
            return;
        }
        /* Core 1 got there: the word holds its value, not core 0's 7. */
        CHECK_EQ(ram32(0x300u), 9u);
        /* And core 0's store-conditional reported failure. */
        CHECK_EQ(ram32(0x304u), 0u);
    }
}

/*
 * The same sequence with *no* interfering store must succeed, or the test
 * above would pass against an implementation whose STC.W never works.
 */
/* CAXI: swaps on a match, reports the old value either way. */
#endif /* G4MH_PE_COUNT > 1 */

/* ------------------------------------------------------------------ */
/* The instructions a compiler emits that were missing                 */
/* ------------------------------------------------------------------ */

/* MOV imm32 -- three halfwords -- as the way to build a full address. */
#define MOVI32(r)      W0(OP_MOVEA, (r), 0)
#define LO(v)          (uint16_t)((v) & 0xFFFFu)
#define HI(v)          (uint16_t)((v) >> 16)

#define PREPARE_W0(imm5, l0) \
    (uint16_t)(0x0780u | (((imm5) & 0x1Fu) << 1) | ((l0) & 1u))
#define DISPOSE_W0(imm5, l0) \
    (uint16_t)(0x0640u | (((imm5) & 0x1Fu) << 1) | ((l0) & 1u))

/*
 * PREPARE and DISPOSE, on the three registers that make the list12 table
 * discriminating rather than decorative.
 *
 * r20 is bit 27, r31 is bit 21 and r30 is bit *0* -- of the first
 * halfword, four bits away from the other eleven and out of order with
 * them. Any implementation that derived the bit from the register number
 * would pass a test using r24-r27 and fail this one. Testing that the
 * two instructions round-trip is also not enough on its own, since a
 * matching pair of wrong orders round-trips perfectly; the stored words
 * are checked in memory as well.
 */
static void test_prepare_dispose(void)
{
    const uint32_t sp0 = EMU_GUEST_RAM_BASE + 0x400u;
    const uint16_t prog[] = {
        MOVI32(3),  LO(sp0), HI(sp0),
        MOVI32(20), 0x1111u, 0x1111u,
        MOVI32(30), 0x2222u, 0x2222u,
        MOVI32(31), 0x3333u, 0x3333u,
        /* prepare {r20, r30, r31}, 2 */
        PREPARE_W0(2u, 1u), (uint16_t)((1u << 11) | (1u << 5) | 0x01u),
        F2(OP_MOVI, 0, 20),
        F2(OP_MOVI, 0, 30),
        F2(OP_MOVI, 0, 31),
        /* dispose 2, {r20, r30, r31} */
        DISPOSE_W0(2u, 1u), (uint16_t)((1u << 11) | (1u << 5)),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(20), 0x11111111u);
    CHECK_EQ(reg(30), 0x22222222u);
    CHECK_EQ(reg(31), 0x33333333u);
    CHECK_EQ(reg(3), sp0);              /* the frame is given back      */

    /* Ascending register order, descending addresses: r20 highest. */
    CHECK_EQ(ram32(0x400u - 4u), 0x11111111u);
    CHECK_EQ(ram32(0x400u - 8u), 0x22222222u);
    CHECK_EQ(ram32(0x400u - 12u), 0x33333333u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * The unsigned loads, each against a value whose top bit is set -- which
 * is the only input that tells them from the sign-extending forms that
 * were already there. A test using 0x01 would pass either way.
 */
static void test_unsigned_loads(void)
{
    const uint32_t base = EMU_GUEST_RAM_BASE + 0x200u;
    const uint16_t prog[] = {
        MOVI32(11), LO(base), HI(base),
        MOVI32(12), 0x80FFu, 0x0000u,
        W0(OP_ST_B, 11, 12), 0x0000u,          /* st.b  r12, 0[r11]     */
        W0(OP_ST_HW, 11, 12), 0x0002u,         /* st.h  r12, 2[r11]     */
        /* ld.bu 0[r11], r13 -- disp bit 0 rides in the opcode          */
        (uint16_t)((13u << 11) | (0x3Cu << 5) | 11u), 0x0001u,
        W0(OP_LD_B, 11, 14), 0x0000u,          /* ld.b -- the control   */
        /* ld.hu 2[r11], r15 */
        (uint16_t)((15u << 11) | (0x3Fu << 5) | 11u), 0x0003u,
        F1(OP_MOV, 11, 30),                    /* ep = base             */
        (uint16_t)((16u << 11) | 0x60u | 0u),  /* sld.bu 0, r16         */
        (uint16_t)((17u << 11) | 0x70u | 1u),  /* sld.hu 2, r17         */
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(13), 0x000000FFu);     /* LD.BU  zero-extends          */
    CHECK_EQ(reg(14), 0xFFFFFFFFu);     /* LD.B   still sign-extends    */
    CHECK_EQ(reg(15), 0x000080FFu);     /* LD.HU                        */
    CHECK_EQ(reg(16), 0x000000FFu);     /* SLD.BU                       */
    CHECK_EQ(reg(17), 0x000080FFu);     /* SLD.HU                       */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/* ------------------------------------------------------------------ */
/* Format XIV: the 48-bit disp23 loads and stores                      */
/* ------------------------------------------------------------------ */

/*
 * Build one. Every field position here was read off CC-RH rather than
 * off the manual's diagram -- scripts/g4mh-check-encodings.sh assembles
 *
 *   ld.b 0x123456[r6], r7   ->  86 07 65 3D 68 24
 *
 * which is w0=0x0786 w1=0x3D65 w2=0x2468, and these macros reproduce it
 * exactly. That is the check that the split below is not a reading of a
 * picture: disp[6:0] in w1[10:4] and disp[22:7] in w2, which no amount
 * of staring at `wwwwwddddddd0101` settles on its own.
 */
#define D23_W0(op6, r1)        (uint16_t)(((op6) << 5) | (r1))
#define D23_W1(r3, disp, sub)  (uint16_t)(((r3) << 11) | \
                                          (((disp) & 0x7Fu) << 4) | (sub))
#define D23_W2(disp)           (uint16_t)(((disp) >> 7) & 0xFFFFu)

#define D23(op6, r1, r3, disp, sub) \
    D23_W0(op6, r1), D23_W1(r3, disp, sub), D23_W2(disp)

/*
 * The displacement is the whole point of these encodings, so the
 * displacements here are the awkward ones: a value with bits in all
 * three fields, the odd one that only the byte forms can express, and
 * both ends of the signed 23-bit range. A test using 0 or 4 would pass
 * against a decoder that dropped w2 entirely.
 */
static void test_disp23_loads_stores(void)
{
    const uint32_t cell = EMU_GUEST_RAM_BASE + 0x200u;
    const uint32_t big  = 0x123456u;            /* bits in w1 and w2    */

    /* base = cell - disp, so the *sum* lands on the cell either way. */
    const uint32_t base_big = cell - big;
    const uint32_t base_neg = cell + 0x400000u; /* disp = -0x400000     */

    const uint16_t prog[] = {
        MOVI32(11), LO(base_big), HI(base_big),
        MOVI32(12), LO(base_neg), HI(base_neg),
        MOVI32(13), 0xBEEFu, 0x0000u,

        /* st.w r13, 0x123456[r11] -- writes the cell through w2        */
        D23(0x3Cu, 11, 13, big, 0xFu),
        /* ld.w 0x123456[r11], r14 -- and reads it back                 */
        D23(0x3Cu, 11, 14, big, 0x9u),
        /* ld.w -0x400000[r12], r15 -- the most negative displacement   */
        D23(0x3Cu, 12, 15, 0x400000u, 0x9u),

        /*
         * disp[0], which for the byte forms is a displacement bit and
         * everywhere else is part of the opcode.
         *
         * Two distinct bytes are planted at cell and cell+1 through the
         * *disp16* forms, which decode by a different path entirely, and
         * only the read uses disp23. A disp23 store and a disp23 load
         * both dropping bit 0 would agree with each other perfectly --
         * which is how the first version of this test passed against a
         * decoder that ignored the bit. Mirroring the write and the read
         * cancels the bug out; this does not.
         */
        MOVI32(18), LO(cell), HI(cell),
        MOVI32(19), 0x00A5u, 0x0000u,
        W0(OP_ST_B, 18, 19), 0x0000u,          /* st.b r19, 0[r18]      */
        MOVI32(19), 0x00EFu, 0x0000u,
        W0(OP_ST_B, 18, 19), 0x0001u,          /* st.b r19, 1[r18]      */

        /* ld.bu 0x123457[r11], r16 -- must see 0xEF, not 0xA5          */
        D23(0x3Du, 11, 16, big + 1u, 0x5u),
        /* ld.b  0x123457[r11], r17 -- sign-extends where LD.BU does not */
        D23(0x3Cu, 11, 17, big + 1u, 0x5u),
        /* and the even one, to prove the bit is read rather than
         * always set: 0xA5 is also negative, so LD.B tells them apart
         * by value and not merely by sign.                             */
        D23(0x3Du, 11, 24, big, 0x5u),

        /*
         * The store side, checked the same way round: written with
         * disp23 at an odd displacement, read back with disp16.
         */
        MOVI32(19), 0x005Au, 0x0000u,
        D23(0x3Cu, 11, 19, big + 3u, 0xDu),    /* st.b r19, big+3[r11]  */
        /*
         * ld.bu 3[r18], r25. The disp16 form splits its displacement
         * too: disp[15:1] in w1[15:1], w1 bit 0 is the marker saying
         * this is not JR, and disp[0] is the low bit of the *opcode* --
         * so an odd displacement is 0x3D and an even one 0x3C. Writing
         * 0x3C here read cell+2 and returned a plausible zero.
         */
        (uint16_t)((25u << 11) | (0x3Du << 5) | 18u), 0x0003u,

        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK_EQ(reg(14), 0x0000BEEFu);     /* ST.W then LD.W, disp23       */
    CHECK_EQ(reg(15), 0x0000BEEFu);     /* the same cell, reached from
                                         * the far side                 */
    /* cell+1 holds 0xEF and cell holds 0xA5. Both are negative as
     * bytes, so telling them apart is a test of the address and not of
     * the sign extension -- and the sign extension is checked too, by
     * LD.BU and LD.B disagreeing on the same byte. */
    CHECK_EQ(reg(16), 0x000000EFu);     /* LD.BU, disp[0] = 1           */
    CHECK_EQ(reg(17), 0xFFFFFFEFu);     /* LD.B,  same byte             */
    CHECK_EQ(reg(24), 0x000000A5u);     /* LD.BU, disp[0] = 0           */
    CHECK_EQ(reg(25), 0x0000005Au);     /* ST.B at an odd disp23, read
                                         * back through disp16          */
}

/*
 * LD.DW / ST.DW: the register pair, and the rule that makes an odd
 * register number mean the even one below it.
 *
 * "reg3 must be an even-numbered register. If an odd-numbered register
 * is specified in reg3, bit 0 of the register number is ignored" -- the
 * manual, and the only statement of it, because CC-RH silently aligns
 * the operand down and warns, so no assembler can produce the case.
 */
static void test_disp23_doubleword(void)
{
    const uint32_t cell = EMU_GUEST_RAM_BASE + 0x240u;
    const uint16_t prog[] = {
        MOVI32(11), LO(cell), HI(cell),
        MOVI32(20), 0x1234u, 0x0000u,          /* r20 = low  word      */
        MOVI32(21), 0x5678u, 0x0000u,          /* r21 = high word      */

        /* st.dw r20, 0[r11] -- writes r20 then r21                     */
        D23(0x3Du, 11, 20, 0u, 0xFu),
        /* ld.dw 0[r11], r22 -- reads them into r22 and r23             */
        D23(0x3Du, 11, 22, 0u, 0x9u),
        /* ld.dw 0[r11], r25 -- odd, so it must land in r24 and r25     */
        D23(0x3Du, 11, 25, 0u, 0x9u),

        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK_EQ(reg(22), 0x1234u);         /* low  half at adr             */
    CHECK_EQ(reg(23), 0x5678u);         /* high half at adr + 4         */
    /* The odd operand: r25 named, r24/r25 written. r25 holding the
     * *high* word is what says the pair was rebased rather than the
     * request being honoured as-is -- with r25 as the low register the
     * value there would be 0x1234. */
    CHECK_EQ(reg(24), 0x1234u);
    CHECK_EQ(reg(25), 0x5678u);
}

/*
 * The aligned forms have no disp[0]: the manual gives them a five-bit
 * opcode where the byte forms have four and seven displacement bits.
 * Setting that bit is a reserved encoding, and the report for one is
 * RIE -- not the misaligned-address exception a decoder reading the bit
 * as disp[0] would eventually raise. The two only differ here, which is
 * exactly why it is worth a test.
 */
static void test_disp23_reserved_bit(void)
{
    uint16_t prog[0x60];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));
    prog[k++] = MOVI32(11);
    prog[k++] = LO(EMU_GUEST_RAM_BASE + 0x200u);
    prog[k++] = HI(EMU_GUEST_RAM_BASE + 0x200u);
    /* ld.w 0[r11], r14 with bit 4 set -- disp 1 would be misaligned. */
    prog[k++] = D23_W0(0x3Cu, 11);
    prog[k++] = D23_W1(14, 1u, 0x9u);
    prog[k++] = D23_W2(1u);

    prog[0x30] = 0x07E0u; prog[0x31] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK(st.traps >= 1u);
    CHECK_EQ(reg(14), 0u);                      /* nothing was loaded   */
    /*
     * **The cause, not the fact of a trap.** Reading the bit as disp[0]
     * gives an odd address for a word load, which raises MAE -- also a
     * trap, and one that lands on the same halt because the bytes
     * between the vectors are zeros this guest runs through. So "traps
     * >= 1" and the final pc are both satisfied by the bug, and the
     * first version of this test asserted exactly those two things.
     * RIE against MAE is the whole difference being tested.
     */
    CHECK_EQ(sreg(0, G4MH_SR_FEIC), G4MH_EXC_RIE);
}


/*
 * The disassembler, which had none.
 *
 * CLAUDE.md records that this file "prints confident nonsense" and
 * names one case; there were more, and every one of them is in a slot
 * where reg2 == 0 selects a different instruction. `jr` was printed for
 * the whole 0x3C/0x3D slot -- LD.BU, all three PREPAREs and every
 * disp23 load and store -- with a target computed from their operands.
 * A reader chasing that goes looking for a control-flow bug in a load.
 *
 * Only the mnemonic and the shape are asserted here, not the whole
 * string: a disassembler test that pins spelling breaks on every
 * cosmetic change and gets deleted. What matters is that the reader is
 * pointed at the right instruction.
 */
static void test_disasm_crowded_slots(void)
{
    char buf[64];

    /* ld.b 0x123456[r6], r7 -- CC-RH's bytes, from disp23.asm. The whole
     * displacement prints now, which is what the wider encoding bought. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0786u | (0x3D65ull << 16) | (0x2468ull << 32), 6u);
    CHECK(strncmp(buf, "ld.b 1193046[r6], r7", 20u) == 0);

    /* ld.dw, which is the op6 low bit away from ld.w. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x07A6u | (0x4569ull << 16) | (0x2468ull << 32), 6u);
    CHECK(strncmp(buf, "ld.dw ", 6u) == 0);

    /* st.w r7, 0x123456[r6] */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0786u | (0x3D6Full << 16) | (0x2468ull << 32), 6u);
    CHECK(strncmp(buf, "st.w r7, 1193046[r6]", 20u) == 0);

    /* A negative disp23, which is the sign extension. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0786u | (0x3809ull << 16) | (0x8000ull << 32), 6u);
    CHECK(strncmp(buf, "ld.w -4194304[r6], r7", 21u) == 0);

    /* prepare 0x3, 4 (32-bit) and its imm32 form (64-bit), which is the
     * only encoding in the ISA whose fourth halfword matters. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u, 0x0782u | (0x0061ull << 16), 4u);
    CHECK(strncmp(buf, "prepare ", 8u) == 0);
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0782u | (0x007Bull << 16) | (0x5678ull << 32) |
                (0x1234ull << 48), 8u);
    CHECK(strncmp(buf, "prepare 0x003, 1, 0x12345678", 28u) == 0);

    /* mov imm32, which used to print half its constant. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)W0(OP_MOVEA, 11, 0) | (0x5678ull << 16) |
                (0x1234ull << 32), 6u);
    CHECK(strncmp(buf, "mov 0x12345678, r11", 19u) == 0);

    /* ld.bu disp16, which shares the slot and is *not* reg2 == 0. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)((13u << 11) | (0x3Cu << 5) | 11u) | (1ull << 16),
                4u);
    CHECK(strncmp(buf, "ld.bu ", 6u) == 0);

    /*
     * And JR still works, with the right split. disp22 puts its *high*
     * bits in the first halfword: w0[5:0] is disp[21:16] and w1[15:1]
     * is disp[15:1]. This file had the other order, so it printed a
     * plausible target for a small forward jump and garbage otherwise
     * -- which is why the value is checked here and not just the
     * mnemonic.
     */
    g4mh_disasm(buf, sizeof(buf), 0x1000u, 0x0780u | (0x0010ull << 16), 4u);
    CHECK(strncmp(buf, "jr 0x00001010", 13u) == 0);

    /* The two other reg2 == 0 slots that printed a multiply. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)(0x17u << 5) | (0x1234ull << 16) | (0ull << 32), 6u);
    CHECK(strncmp(buf, "jr ", 3u) == 0);
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)(0x37u << 5) | (0x1234ull << 16) | (0ull << 32), 6u);
    CHECK(strncmp(buf, "jmp ", 4u) == 0);

    /* With reg2 != 0 they are multiplies again, and 32-bit. */
    /* MULH imm5 is *16* bits: passing 4 here is a length disagreement,
     * which is what the test below is about. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)((1u << 11) | (0x17u << 5)), 2u);
    CHECK(strncmp(buf, "mulh ", 5u) == 0);
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)((1u << 11) | (0x37u << 5)) | (0x1234ull << 16), 4u);
    CHECK(strncmp(buf, "mulhi ", 6u) == 0);
}

/*
 * JR and JARL disp32: the 48-bit forms hiding in a 16-bit opcode slot.
 *
 * `JARL disp32, reg1` is 00000 010111 RRRRR -- reg2 zero, op6 0x17,
 * reg1 the link register -- so it lives in MULH imm5's slot, told apart
 * by reg2 alone. The interpreter has always known that and has a full
 * implementation of both.
 *
 * **It could never run.** g4mh_insn_len answers from the first halfword
 * and says 2 bytes for every op6 below 0x30, so the second stage that
 * would have called g4mh_insn_is_48 -- which does handle 0x17 -- was
 * never reached. The instruction decoded as 16 bits, w1 and w2 read as
 * zero, and the jump went to pc + 0: an infinite loop, not a wrong
 * answer. That is why the run below is given a budget and why the
 * retired count is what proves the fix.
 *
 * A shared opcode holding instructions of *different widths* is the
 * defect this frontend records for 0x37 and 0x3C, and this is the same
 * one in the one slot where the length decoder's first stage cannot see
 * past its own rule of thumb.
 */
static void test_jr_disp32(void)
{
    /* jr +8, then a marker that must be skipped, then the target. */
    const uint16_t prog[] = {
        0x02E0u, 0x0008u, 0x0000u,      /* jr 0x00000008              */
        F2(OP_MOVI, 9, 21),             /* skipped: r21 must stay 0   */
        F2(OP_MOVI, 5, 20),             /* +8: the target             */
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK_EQ(reg(20), 5u);              /* the target ran             */
    CHECK_EQ(reg(21), 0u);              /* and the marker did not     */
    /* Three instructions, not a budget's worth of jumping to itself. */
    CHECK_EQ(retired, 3u);

    /* jarl +6, reg1 = r19: the link register gets pc + 6. */
    {
        const uint16_t prog2[] = {
            (uint16_t)(0x02E0u | 19u), 0x0008u, 0x0000u,
            F2(OP_MOVI, 9, 21),         /* skipped                    */
            F2(OP_MOVI, 5, 20),
            0x07E0u, SUB_HALT,
        };
        if (!load_and_run(prog2, sizeof(prog2) / sizeof(prog2[0]), 64u,
                          &why, &retired)) { CHECK(false); return; }
        CHECK_EQ(reg(19), EMU_GUEST_RAM_BASE + 6u);
        CHECK_EQ(reg(20), 5u);
        CHECK_EQ(reg(21), 0u);
    }

    /* And MULH imm5 with a real reg2 is still a multiply, 16 bits
     * wide -- the neighbour that must not have been widened with it. */
    {
        const uint16_t prog3[] = {
            MOVI32(10), 3u, 0u,
            (uint16_t)((10u << 11) | (0x17u << 5) | 4u),  /* mulh 4, r10 */
            F2(OP_MOVI, 5, 20),
            0x07E0u, SUB_HALT,
        };
        if (!load_and_run(prog3, sizeof(prog3) / sizeof(prog3[0]), 64u,
                          &why, &retired)) { CHECK(false); return; }
        CHECK_EQ(reg(10), 12u);
        CHECK_EQ(reg(20), 5u);          /* the next instruction ran   */
    }
}

/*
 * The length the caller passes against the length the encoding implies.
 *
 * On this ISA they are derivable from each other, so `len` is a second
 * opinion rather than new information -- and a disagreement means the
 * caller and the decoder have diverged, which is the defect this
 * frontend keeps recording. It prints `.short` with both numbers
 * instead of a mnemonic that would look authoritative.
 */
static void test_disasm_length_disagreement(void)
{
    char buf[64];

    /* A 48-bit disp23 load, claimed to be four bytes. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0786u | (0x3D65ull << 16) | (0x2468ull << 32), 4u);
    CHECK(strncmp(buf, ".short ", 7u) == 0);

    /* And a 16-bit MULH claimed to be four -- the neighbour of the
     * 48-bit JR that shares its opcode. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                (uint64_t)((1u << 11) | (0x17u << 5)) | (0x1234ull << 16), 4u);
    CHECK(strncmp(buf, ".short ", 7u) == 0);

    /* The agreeing case still disassembles. */
    g4mh_disasm(buf, sizeof(buf), 0x1000u,
                0x0786u | (0x3D65ull << 16) | (0x2468ull << 32), 6u);
    CHECK(strncmp(buf, "ld.b ", 5u) == 0);
}

/*
 * PREPARE list12, imm5, imm32 -- the ISA's only 64-bit encoding.
 *
 * The words are CC-RH's, not this project's:
 *
 *   prepare 0x3, 4, 0x12345678  ->  82 07 7B 00 78 56 34 12
 *
 * which is w0=0x0782 w1=0x007B w2=0x5678 w3=0x1234, so imm32 is
 * (w3 << 16) | w2. Worth taking from the assembler rather than from the
 * manual's diagram because the halfword *order* of a 32-bit immediate
 * split across two of them is exactly the sort of thing a diagram
 * leaves ambiguous and a hand-written test then enshrines.
 *
 * The length is the point, not the value. A 64-bit instruction decoded
 * as 48 does not compute a wrong answer -- it leaves the pc two bytes
 * short and every instruction after it is garbage, so the check is that
 * execution *continues correctly* past it.
 */
static void test_prepare_imm32(void)
{
    const uint32_t sp0 = EMU_GUEST_RAM_BASE + 0x300u;
    const uint16_t prog[] = {
        MOVI32(3), LO(sp0), HI(sp0),           /* sp = a known place    */

        /* prepare 0x3, 4, 0x12345678 */
        0x0782u, 0x007Bu, 0x5678u, 0x1234u,

        /* If the length was right, this runs next and nothing else. */
        F2(OP_MOVI, 5, 21),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK_EQ(reg(30), 0x12345678u);     /* ep <- imm32                  */
    CHECK_EQ(reg(21), 5u);              /* and the pc landed right      */

    /*
     * Two words pushed for list12 = 0x3, then sp dropped by imm5 * 4.
     *
     * imm5 is **1** here, not 4: CC-RH's second operand is a byte count
     * and the field holds it in words, so `prepare 0x3, 4` encodes 1 --
     * which is also why `prepare 0xFFF, 31` warns "immediate must be a
     * multiple of 4" and encodes 7. Reading the operand as the field is
     * how this test first asserted sp0 - 24 against a correct emulator.
     */
    CHECK_EQ(reg(3), sp0 - 8u - 4u);
}

/*
 * The other three ff encodings, for the reason the imm32 one is
 * interesting: they are the neighbours it has to be told apart from,
 * and two of them are a different *length*.
 *
 *   ff = 00  ep <- the new sp      48-bit
 *   ff = 01  ep <- sext(imm16)     48-bit
 *   ff = 10  ep <- imm16 << 16     48-bit
 *   ff = 11  ep <- imm32           64-bit
 *
 * Encodings from CC-RH: `prepare 0x3, 4, sp` is 82 07 63 00 and
 * `prepare 0x3, 4, 0x1234` is 82 07 6B 00 34 12.
 */
static void test_prepare_ff_forms(void)
{
    const uint32_t sp0 = EMU_GUEST_RAM_BASE + 0x300u;

    /* ff = 01: sign-extended imm16, and a negative one, because that is
     * the half of "sign-extended" a positive value cannot check. */
    {
        const uint16_t prog[] = {
            MOVI32(3), LO(sp0), HI(sp0),
            0x0782u, 0x006Bu, 0xFFF0u,          /* prepare .., 0xFFF0   */
            F2(OP_MOVI, 5, 21),
            0x07E0u, SUB_HALT,
        };
        emu_run_reason_t why;
        uint32_t retired = 0;
        if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) { CHECK(false); return; }
        CHECK_EQ(reg(30), 0xFFFFFFF0u);
        CHECK_EQ(reg(21), 5u);
    }

    /* ff = 10: the same immediate shifted up, which is what says the
     * two are read from the same halfword and treated differently. */
    {
        const uint16_t prog[] = {
            MOVI32(3), LO(sp0), HI(sp0),
            0x0782u, 0x0073u, 0xFFF0u,
            F2(OP_MOVI, 5, 21),
            0x07E0u, SUB_HALT,
        };
        emu_run_reason_t why;
        uint32_t retired = 0;
        if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) { CHECK(false); return; }
        CHECK_EQ(reg(30), 0xFFF00000u);
        CHECK_EQ(reg(21), 5u);
    }

    /* ff = 00: ep <- the new sp, and this one is 48 bits with no
     * immediate at all -- so reading a fourth halfword here would eat
     * the instruction after it. */
    {
        const uint16_t prog[] = {
            MOVI32(3), LO(sp0), HI(sp0),
            0x0782u, 0x0063u,
            F2(OP_MOVI, 5, 21),
            0x07E0u, SUB_HALT,
        };
        emu_run_reason_t why;
        uint32_t retired = 0;
        if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) { CHECK(false); return; }
        CHECK_EQ(reg(30), sp0 - 8u - 4u);
        CHECK_EQ(reg(30), reg(3));
        CHECK_EQ(reg(21), 5u);
    }
}

/* ------------------------------------------------------------------ */
/* The inter-CPU peripherals: BARR, IPIR, TPTM                         */
/* ------------------------------------------------------------------ */

/*
 * These are driven through the bus rather than through guest code, and
 * deliberately: a guest program can only ever reach the *self* region,
 * because that is what "self" means, and the whole difficulty of all
 * three peripherals is the routing between one PE's registers and
 * another's. The absolute windows are the only way to be two PEs at
 * once in a one-PE build.
 *
 * One guest-driven test follows them, for the thing this cannot check:
 * that a guest store actually lands on the device.
 */
static bool devbus_up(void)
{
    const uint16_t prog[] = { 0x07E0u, SUB_HALT };
    emu_run_reason_t why;
    uint32_t retired = 0;

    /* Builds the bus and every device, then halts immediately. */
    return load_and_run(prog, 2u, 8u, &why, &retired);
}

static uint32_t devrd(uint32_t addr)
{
    uint32_t v = 0xDEADBEEFu;
    (void)emu_bus_read(&g_bus, addr, 4u, &v);
    return v;
}

static void devwr(uint32_t addr, uint32_t v)
{
    (void)emu_bus_write(&g_bus, addr, 4u, v);
}

/*
 * Advance guest time through the frontend's own hook, which is what the
 * platform calls. Not a test-only entry point into the timer: the thing
 * worth checking is that the TPTM is actually wired to that path, and a
 * direct call would pass with it unwired.
 */
static void tick(uint32_t ticks)
{
    g_core.ops->advance_time(g_core.cpu, ticks);
}

#define BARR_INIT(n)        (G4MH_BARR_BASE + 0x000u + 0x10u * (n))
#define BARR_EN(n)          (G4MH_BARR_BASE + 0x004u + 0x10u * (n))
#define BARR_CHKS(n)        (G4MH_BARR_BASE + 0x100u + 0x10u * (n))
#define BARR_SYNCS(n)       (G4MH_BARR_BASE + 0x104u + 0x10u * (n))
#define BARR_CHK(n, m)      (G4MH_BARR_BASE + 0x800u + 0x10u * (n) + \
                             0x100u * (m))
#define BARR_SYNC(n, m)     (G4MH_BARR_BASE + 0x804u + 0x10u * (n) + \
                             0x100u * (m))

/*
 * A three-PE barrier on channel 3, arriving one PE at a time.
 *
 * The check that matters is the *negative* one after each of the first
 * two arrivals: a barrier that completed early would be a barrier that
 * does not synchronise, and every positive assertion here would still
 * pass. Channel 3 rather than 0 because a decoder that dropped the
 * 0x10 * n stride would answer every channel from channel 0.
 */
static void test_barrier(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(BARR_EN(3), 0x07u);               /* PE0, PE1, PE2 participate */

    CHECK_EQ(devrd(BARR_EN(3)), 0x07u);
    CHECK_EQ(devrd(BARR_SYNC(3, 0)), 0u);

    devwr(BARR_CHKS(3), 0u);                /* PE0 arrives -- value ignored */
    CHECK_EQ(devrd(BARR_CHK(3, 0)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(3, 0)), 0u);   /* not yet */

    devwr(BARR_CHK(3, 1), 1u);              /* PE1 arrives */
    CHECK_EQ(devrd(BARR_SYNC(3, 0)), 0u);   /* still not */

    devwr(BARR_CHK(3, 2), 1u);              /* PE2 arrives: complete */
    CHECK_EQ(devrd(BARR_SYNC(3, 0)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(3, 1)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(3, 2)), 1u);
    /* And the check bits are cleared for the next round, by hardware. */
    CHECK_EQ(devrd(BARR_CHK(3, 0)), 0u);
    CHECK_EQ(devrd(BARR_CHK(3, 1)), 0u);
    CHECK_EQ(devrd(BARR_CHK(3, 2)), 0u);

    /* A neighbouring channel saw none of it. */
    CHECK_EQ(devrd(BARR_SYNC(2, 0)), 0u);
    CHECK_EQ(devrd(BARR_SYNC(4, 0)), 0u);

    /* SYNC takes the value written, unlike CHK. */
    devwr(BARR_SYNC(3, 0), 0u);
    CHECK_EQ(devrd(BARR_SYNC(3, 0)), 0u);

    /* BRnINIT clears both halves of the channel. */
    devwr(BARR_CHK(3, 1), 1u);
    devwr(BARR_INIT(3), 1u);
    CHECK_EQ(devrd(BARR_CHK(3, 1)), 0u);
    CHECK_EQ(devrd(BARR_SYNC(3, 1)), 0u);
}

/*
 * Two rules that a barrier implementation gets wrong quietly.
 *
 * "If all bits of the BRnEN register are 0, BRCHK bit cannot be set" --
 * with no participants there is nothing to complete the barrier, and a
 * check bit set then would never be cleared except by BRnINIT.
 *
 * And a PE that is not enabled neither blocks the barrier nor is
 * cleared by it: BRnCHKm "can be set even if BRnEN.BRENm is 0, but it
 * does not affect the barrier-synchronization".
 */
static void test_barrier_participation(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    /* No participants: the arrival is refused outright. */
    devwr(BARR_CHK(5, 0), 1u);
    CHECK_EQ(devrd(BARR_CHK(5, 0)), 0u);

    /* PE0 and PE1 participate; PE2 does not but arrives anyway. */
    devwr(BARR_EN(5), 0x03u);
    devwr(BARR_CHK(5, 2), 1u);
    CHECK_EQ(devrd(BARR_CHK(5, 2)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(5, 2)), 0u);

    devwr(BARR_CHK(5, 0), 1u);
    devwr(BARR_CHK(5, 1), 1u);

    /* The two participants synchronised, and PE2 was left alone in both
     * directions: its SYNC is not set and its CHK is not cleared. */
    CHECK_EQ(devrd(BARR_SYNC(5, 0)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(5, 1)), 1u);
    CHECK_EQ(devrd(BARR_SYNC(5, 2)), 0u);
    CHECK_EQ(devrd(BARR_CHK(5, 2)), 1u);
}

/*
 * A write to BRnEN re-evaluates the barrier.
 *
 * "Barrier-synchronization is established at the condition that all the
 * BRnCHKm.BRCHK bits of participating PEs are set" -- a level condition,
 * not an event, so anything that changes either side of it has to be
 * checked. Changing BRnEN changes which bits are consulted.
 *
 * **The first version of this test could not tell.** It enabled, then
 * arrived, then asserted -- and the arrival re-evaluates, so deleting
 * the evaluation on the enable changed nothing and the test still
 * passed. The sequence has to end on the *enable* for the enable to be
 * what is under test: PE0 and PE1 participate, PE0 arrives, and then
 * PE1 drops out, at which point every remaining participant has
 * arrived.
 */
static void test_barrier_enable_completes(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(BARR_EN(1), 0x03u);               /* PE0 and PE1 participate  */
    devwr(BARR_CHK(1, 0), 1u);              /* PE0 arrives              */
    CHECK_EQ(devrd(BARR_SYNC(1, 0)), 0u);   /* waiting for PE1          */

    devwr(BARR_EN(1), 0x01u);               /* PE1 drops out            */
    CHECK_EQ(devrd(BARR_SYNC(1, 0)), 1u);   /* so PE0 is now everyone   */
    CHECK_EQ(devrd(BARR_CHK(1, 0)), 0u);    /* and its arrival is spent */
}

#define IPIR_EN(n, m)       (G4MH_IPIR_BASE + 0x800u + 0x20u * (n) + \
                             0x100u * (m))
#define IPIR_FLG(n, m)      (IPIR_EN(n, m) + 0x04u)
#define IPIR_FCLR(n, m)     (IPIR_EN(n, m) + 0x08u)
#define IPIR_REQ(n, m)      (IPIR_EN(n, m) + 0x10u)
#define IPIR_RCLR(n, m)     (IPIR_EN(n, m) + 0x14u)
#define IPIR_ENS(n)         (G4MH_IPIR_BASE + 0x000u + 0x20u * (n))
#define IPIR_FLGS(n)        (G4MH_IPIR_BASE + 0x004u + 0x20u * (n))
#define IPIR_REQS(n)        (G4MH_IPIR_BASE + 0x010u + 0x20u * (n))

/*
 * PE1 asks PE2 for an interrupt on channel 2.
 *
 * Neither PE exists in a one-PE build, which is exactly why they are
 * the ones used: the register matrix is six by six whatever the part
 * populates, and routing REQm[x] to FLGx[m] is the entire peripheral.
 * Using PE0 for both ends would let a decoder that ignored one of the
 * two indices pass.
 */
static void test_ipir_routing(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(IPIR_EN(2, 2), 1u << 1);          /* PE2 accepts from PE1     */
    devwr(IPIR_REQ(2, 1), 1u << 2);         /* PE1 asks PE2             */

    CHECK_EQ(devrd(IPIR_REQ(2, 1)), 1u << 2);
    CHECK_EQ(devrd(IPIR_FLG(2, 2)), 1u << 1);   /* PE2 sees *PE1*       */
    CHECK_EQ(devrd(IPIR_FLG(2, 1)), 0u);        /* and PE1 sees nothing */
    /* Another channel is untouched: 0x20 * n really is the stride. */
    CHECK_EQ(devrd(IPIR_FLG(1, 2)), 0u);
    CHECK_EQ(devrd(IPIR_FLG(3, 2)), 0u);

    /* The receiver dismisses it, which clears the sender's request too. */
    devwr(IPIR_FCLR(2, 2), 1u << 1);
    CHECK_EQ(devrd(IPIR_FLG(2, 2)), 0u);
    CHECK_EQ(devrd(IPIR_REQ(2, 1)), 0u);
}

/*
 * The enable gates the *transfer*, not the request -- table 3.152.
 *
 * A request raised while the receiver has not enabled the sender is
 * remembered in REQ and never arrives, not even once the enable is
 * written. That is the difference from the barrier next door, which
 * does re-evaluate on its enable, and it is why the manual's
 * initial-setting sequence clears with FCLR *before* enabling.
 */
static void test_ipir_enable_gates_transfer(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(IPIR_REQ(0, 3), 1u << 4);         /* PE3 asks PE4, disabled   */
    CHECK_EQ(devrd(IPIR_REQ(0, 3)), 1u << 4);   /* remembered           */
    CHECK_EQ(devrd(IPIR_FLG(0, 4)), 0u);        /* but not delivered    */

    devwr(IPIR_EN(0, 4), 1u << 3);          /* PE4 enables PE3 now      */
    CHECK_EQ(devrd(IPIR_FLG(0, 4)), 0u);    /* still not delivered      */

    /* The sender withdrawing clears its own request; with the enable
     * now set it would clear the flag too, and there is none to clear. */
    devwr(IPIR_RCLR(0, 3), 1u << 4);
    CHECK_EQ(devrd(IPIR_REQ(0, 3)), 0u);

    /* Writing 0 to REQ is ignored -- it is a set-only register. */
    devwr(IPIR_REQ(0, 3), 1u << 4);
    devwr(IPIR_REQ(0, 3), 0u);
    CHECK_EQ(devrd(IPIR_REQ(0, 3)), 1u << 4);
    CHECK_EQ(devrd(IPIR_FLG(0, 4)), 1u << 3);   /* enabled now, so it
                                                 * did arrive           */
}

/*
 * The self region routes to the accessing PE's own registers.
 *
 * This build has one PE, so self is PE0 -- and that is enough to catch
 * the mistake worth catching, which is a self region that reads the
 * wrong PE's registers or none at all. Written through the self alias
 * and read back through PE0's absolute window, so the two paths have to
 * agree about which object they are naming.
 */
static void test_ipir_self_region(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(IPIR_ENS(1), 0x21u);
    CHECK_EQ(devrd(IPIR_EN(1, 0)), 0x21u);
    CHECK_EQ(devrd(IPIR_ENS(1)), 0x21u);

    /* PE0 interrupting itself is legitimate and is the one delivery
     * path a single-PE build can exercise end to end. */
    devwr(IPIR_REQS(1), 1u << 0);
    CHECK_EQ(devrd(IPIR_FLGS(1)), 1u << 0);
}

#define TPTM_SELF(r)        (G4MH_TPTM_BASE + G4MH_TPTM_SELF + (r))

/*
 * The interval timer: down-count, underflow, reload.
 *
 * The awkward number is the reload period. A counter loaded with N
 * underflows after N+1 counts, not N, because the underflow is the step
 * *from* zero rather than the arrival at it -- so this advances by
 * exactly the count that must not have underflowed yet, then by one
 * more.
 */
static void test_tptm_interval(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(TPTM_SELF(G4MH_TPTM_ILD0), 9u);
    devwr(TPTM_SELF(G4MH_TPTM_IRUN), 1u);       /* start channel 0      */

    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ISTR)), 1u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ICNT0)), 9u);

    tick(9u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ICNT0)), 0u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)), 0u);   /* not yet       */

    tick(1u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)), 1u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ICNT0)), 9u);   /* reloaded      */

    /* Write 0 to clear; writing 1 is ignored. */
    devwr(TPTM_SELF(G4MH_TPTM_IUSTR), 1u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)), 1u);
    devwr(TPTM_SELF(G4MH_TPTM_IUSTR), 0u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)), 0u);

    /* Channel 1 was never started and did not move. */
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ICNT1)), 0u);

    devwr(TPTM_SELF(G4MH_TPTM_ISTP), 1u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ISTR)), 0u);
    tick(100u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_ICNT0)), 9u);   /* stopped       */
}

/*
 * The divider, and its remainder.
 *
 * Guest time arrives in slices of a run budget, not one tick at a time,
 * so a divider that dropped its remainder at each call would run slow
 * by a factor of the slice length rather than merely rounding. Advancing
 * in threes with a divider of 3 -- a period of 4 -- is what makes the
 * carry visible: neither 3 nor 6 is a multiple of 4, and only the third
 * call reaches the second count.
 */
static void test_tptm_divider_carry(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(TPTM_SELF(G4MH_TPTM_FDIV), 3u);       /* count every 4 ticks  */
    devwr(TPTM_SELF(G4MH_TPTM_FRUN), 1u);

    tick(3u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_FCNT)), 0u);
    tick(3u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_FCNT)), 1u);     /* 6 / 4        */
    tick(3u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_FCNT)), 2u);     /* 9 / 4        */
    tick(3u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_FCNT)), 3u);     /* 12 / 4       */
}

/*
 * The up timer's comparison, and why it is not an equality test.
 *
 * With a divider or a long slice the counter steps *over* the compare
 * value: here it goes 0 -> 20 in one call and the compare is 7. An
 * implementation testing `count == cmp` would never fire, and would
 * pass any test that advanced one tick at a time.
 */
static void test_tptm_up_compare(void)
{
    if (!devbus_up()) { CHECK(false); return; }

    devwr(TPTM_SELF(G4MH_TPTM_UCMP0(1)), 7u);
    devwr(TPTM_SELF(G4MH_TPTM_URUN), 1u);       /* start up timer 0     */

    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_USTR)), 1u);
    tick(20u);

    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_UCNT0)), 20u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_UCSTR)), 1u << 1);   /* value 1  */

    /*
     * Up timer 1's flags live at bits 11:8, not 7:4. Checked by writing
     * a compare there and confirming the bit that lights: the two halves
     * of that field are the kind of thing that gets written twice and
     * disagrees.
     */
    devwr(TPTM_SELF(G4MH_TPTM_UCMP1(2)), 5u);
    devwr(TPTM_SELF(G4MH_TPTM_URUN), 2u);       /* start up timer 1     */
    tick(6u);
    CHECK((devrd(TPTM_SELF(G4MH_TPTM_UCSTR)) & (1u << 10)) != 0u);

    /* Write 0 to clear, 1 ignored -- the same rule as IUSTR. */
    devwr(TPTM_SELF(G4MH_TPTM_UCSTR), 0u);
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_UCSTR)), 0u);
}


/*
 * The interval timer's interrupt, both ways round.
 *
 * Time has to pass *inside* a run for this to be an interrupt rather
 * than a pending flag, so the guest issues a TRAP and the syscall hook
 * advances the clock -- which also makes the moment the timer fires a
 * property of the program rather than of the harness. Everything before
 * the trap is setup written by the guest through ordinary stores, so
 * this is also the check that a guest store reaches these devices at
 * all: every other test here drives them from the bus side.
 */
static bool g_tick_hook_seen;

static bool tick_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)cpu; (void)sc; (void)user;
    g_tick_hook_seen = true;
    tick(64u);                      /* past the reload, whatever it is */
    return true;                    /* consumed: no architectural trap */
}

/*
 * TPTMSEL = 1: the interval interrupt is EIINT31.
 *
 * The handler is the shared EI vector at RBASE + 0x100, which is where
 * every EI interrupt lands under the reduced vector layout.
 */
static void test_tptm_interrupt_ei(void)
{
    uint16_t prog[0x120];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));

    /* r11 = TPTM base, r12 = INTIF base, r13 = INTC1 self base */
    prog[k++] = MOVI32(11);
    prog[k++] = LO(G4MH_TPTM_BASE); prog[k++] = HI(G4MH_TPTM_BASE);
    prog[k++] = MOVI32(12);
    prog[k++] = LO(G4MH_INTIF_BASE); prog[k++] = HI(G4MH_INTIF_BASE);
    prog[k++] = MOVI32(13);
    prog[k++] = LO(G4MH_INTC1_SELF_BASE); prog[k++] = HI(G4MH_INTC1_SELF_BASE);

    /* TPTMSEL = 1: route PE0's TPTM interrupt to EIINT31. */
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 12, 10); prog[k++] = G4MH_INTIF_TPTMSEL | 1u;

    /*
     * EIC31 = 0: unmasked, highest priority.
     *
     * A *halfword* store. EICn is 16 bits at 0x02 * n, so channel 31 is
     * at 0x3E -- and the ST.W form encodes its width in bit 0 of the
     * second halfword, so writing `| 1` there makes it a word store to
     * an address that is not word aligned. That is an MAE, which
     * vectors to RBASE + 0x60, runs through the zeros to the interrupt
     * handler and sets r20 exactly as a delivered interrupt would.
     * Every check but the cause register passed.
     */
    prog[k++] = F2(OP_MOVI, 0, 10);
    prog[k++] = W0(OP_ST_HW, 13, 10); prog[k++] = (31u * 2u);

    /* ILD0 = 3, IIEN = 1, IRUN = 1 */
    prog[k++] = F2(OP_MOVI, 3, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_ILD0 | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_IIEN | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_IRUN | 1u;

    /*
     * EI: unmask interrupts. **PSW.ID is set out of reset**, so without
     * this the channel is raised, refused, and the guest runs on to its
     * halt -- which looks exactly like a timer that never fired.
     */
    prog[k++] = W0(OP_SYSTEM, 0, 0x10u); prog[k++] = SUB_DIEI;

    /* Let the clock run: the hook consumes this and advances time. */
    prog[k++] = W0(OP_SYSTEM, 0, 0);        /* trap 0 */
    prog[k++] = SUB_TRAP;

    /* If no interrupt arrives, fall through to a halt with r20 clear. */
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    /* The EI handler, at RBASE + 0x100. */
    prog[0x80] = F2(OP_MOVI, 7, 20);        /* r20 = 7: we got here */
    prog[0x81] = 0x07E0u; prog[0x82] = SUB_HALT;

    g_tick_hook_seen = false;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_hooked(prog, sizeof(prog) / sizeof(prog[0]), 256u,
                             &why, &retired, tick_hook)) {
        CHECK(false);
        return;
    }

    CHECK(g_tick_hook_seen);
    CHECK_EQ(reg(20), 7u);                  /* the EI handler ran      */
    CHECK_EQ(sreg(0, G4MH_SR_EIIC), G4MH_EXC_EIINT_BASE + 31u);
    /* And the underflow flag is set whether or not IIEN was: it is the
     * enable that gates the interrupt, not the status. */
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)) & 1u, 1u);
}

/*
 * TPTMSEL = 0, which is the *reset* value: the interval interrupt is
 * FEINT.
 *
 * This is the path a guest gets without configuring anything, and it is
 * the one an implementation is most likely to leave out -- an emulator
 * with only the EI half works perfectly for a guest that sets the bit
 * and does nothing at all for one that does not.
 *
 * The guest sets PSW.ID first, which is what makes the level matter:
 * an EI interrupt would be refused outright, and this one is not.
 */
static void test_tptm_interrupt_feint(void)
{
    uint16_t prog[0x120];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));

    prog[k++] = MOVI32(11);
    prog[k++] = LO(G4MH_TPTM_BASE); prog[k++] = HI(G4MH_TPTM_BASE);

    /*
     * DI: mask EI interrupts. PSW.ID is already set out of reset, so
     * this is here to say what the test is about rather than to change
     * anything -- an FE-level interrupt arrives with it set.
     *
     * reg2 picks DI from EI, and reg3 picks RESBANK from DI. Writing
     * `(16 << 11) | SUB_DIEI` here is RESBANK, which raises RIE, lands
     * in the zeros and runs on to the handler -- setting r20 exactly as
     * a delivered FEINT would. There is a test two hundred lines up
     * whose whole subject is that those two are not the same
     * instruction, and this still got it wrong.
     */
    prog[k++] = W0(OP_SYSTEM, 0, 0x00u); prog[k++] = SUB_DIEI;

    prog[k++] = F2(OP_MOVI, 3, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_ILD0 | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_IIEN | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_IRUN | 1u;

    prog[k++] = W0(OP_SYSTEM, 0, 0); prog[k++] = SUB_TRAP;
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;      /* not reached     */

    /* The FEINT handler, at RBASE + 0xF0. */
    prog[0x78] = F2(OP_MOVI, 9, 20);
    prog[0x79] = 0x07E0u; prog[0x7A] = SUB_HALT;

    g_tick_hook_seen = false;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_hooked(prog, sizeof(prog) / sizeof(prog[0]), 256u,
                             &why, &retired, tick_hook)) {
        CHECK(false);
        return;
    }

    CHECK(g_tick_hook_seen);
    CHECK_EQ(reg(20), 9u);                  /* the FE handler ran      */
    /* FE level, so the cause is in FEIC and not in EIIC. */
    CHECK_EQ(sreg(0, G4MH_SR_FEIC), G4MH_EXC_FEINT);
    /* PSW.ID was set and did not stop it -- which is the whole point
     * of the level, and is what an EI-only implementation gets wrong. */
    CHECK((sreg(0, G4MH_SR_FEPSW) & G4MH_PSW_ID) != 0u);
}

/*
 * IIEN gates the interrupt and not the flag.
 *
 * "When underflow occurs [...] TPTMnIUSTR.IUSTRm bit is set whether
 * IIENm bit is set or not. If IIENm bit is set, TPTM_IRQ[n] is
 * asserted." Two separate things, and an implementation that raises
 * unconditionally passes every other test here -- the enable is written
 * in all of them.
 */
static void test_tptm_interrupt_masked(void)
{
    uint16_t prog[0x120];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));

    prog[k++] = MOVI32(11);
    prog[k++] = LO(G4MH_TPTM_BASE); prog[k++] = HI(G4MH_TPTM_BASE);
    prog[k++] = MOVI32(12);
    prog[k++] = LO(G4MH_INTIF_BASE); prog[k++] = HI(G4MH_INTIF_BASE);
    prog[k++] = MOVI32(13);
    prog[k++] = LO(G4MH_INTC1_SELF_BASE); prog[k++] = HI(G4MH_INTC1_SELF_BASE);

    /*
     * EIC31 unmasked and PSW.ID clear, so that IIEN is the *only* thing
     * left that can suppress the interrupt. Without these two the test
     * would pass against an implementation that ignored IIEN entirely,
     * because something else was refusing the channel.
     */
    prog[k++] = F2(OP_MOVI, 0, 10);
    prog[k++] = W0(OP_ST_HW, 13, 10); prog[k++] = (31u * 2u);

    /* TPTMSEL = 1, so a raised interrupt would be EIINT31 -- and EI, so
     * PSW.ID is not what suppresses it. Only IIEN is left. */
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 12, 10); prog[k++] = G4MH_INTIF_TPTMSEL | 1u;
    prog[k++] = W0(OP_SYSTEM, 0, 0x10u); prog[k++] = SUB_DIEI;

    /* ILD0 = 3 and start, with IIEN left at its reset value of 0. */
    prog[k++] = F2(OP_MOVI, 3, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_ILD0 | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = G4MH_TPTM_IRUN | 1u;

    prog[k++] = W0(OP_SYSTEM, 0, 0); prog[k++] = SUB_TRAP;
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;      /* the expected end */

    prog[0x80] = F2(OP_MOVI, 7, 20);                /* EI vector       */
    prog[0x81] = 0x07E0u; prog[0x82] = SUB_HALT;

    g_tick_hook_seen = false;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run_hooked(prog, sizeof(prog) / sizeof(prog[0]), 256u,
                             &why, &retired, tick_hook)) {
        CHECK(false);
        return;
    }

    CHECK(g_tick_hook_seen);
    CHECK_EQ(reg(20), 0u);                          /* no handler ran  */
    CHECK_EQ(sreg(0, G4MH_SR_EIIC), 0u);
    /* But the underflow happened and is recorded. */
    CHECK_EQ(devrd(TPTM_SELF(G4MH_TPTM_IUSTR)) & 1u, 1u);

    /*
     * And setting IIEN now asserts it immediately -- the manual's own
     * caution, and the reason the write to IIEN is not just a store.
     */
    devwr(TPTM_SELF(G4MH_TPTM_IIEN), 1u);
    CHECK(g4mh_cpu_pending_irq((const g4mh_cpu_t *)(const void *)g_core.cpu)
          == 31);
}

/*
 * IPIR really reaches the interrupt controller.
 *
 * Every other IPIR test here reads the flag registers back, and a
 * peripheral that maintained them perfectly and raised nothing would
 * pass all of them. PE0 interrupts itself on channel 1, which in a
 * one-PE build is the only delivery that has anywhere to go.
 */
static void test_ipir_delivers(void)
{
    uint16_t prog[0x120];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));

    prog[k++] = MOVI32(11);
    prog[k++] = LO(G4MH_IPIR_BASE); prog[k++] = HI(G4MH_IPIR_BASE);
    prog[k++] = MOVI32(13);
    prog[k++] = LO(G4MH_INTC1_SELF_BASE); prog[k++] = HI(G4MH_INTC1_SELF_BASE);

    /* EIC1 = 0: channel 1 unmasked. A halfword store -- see the note
     * in test_tptm_interrupt_ei about what a word store does here. */
    prog[k++] = F2(OP_MOVI, 0, 10);
    prog[k++] = W0(OP_ST_HW, 13, 10); prog[k++] = (1u * 2u);

    /* EI: PSW.ID is set out of reset and would refuse the channel. */
    prog[k++] = W0(OP_SYSTEM, 0, 0x10u); prog[k++] = SUB_DIEI;

    /* IPI1ENS = 1 (accept from PE0), then IPI1REQS = 1. */
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = (0x20u + 0x00u) | 1u;
    prog[k++] = F2(OP_MOVI, 1, 10);
    prog[k++] = W0(OP_ST_HW, 11, 10); prog[k++] = (0x20u + 0x10u) | 1u;

    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;      /* not reached     */

    prog[0x80] = F2(OP_MOVI, 5, 20);                /* EI vector       */
    prog[0x81] = 0x07E0u; prog[0x82] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 256u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(20), 5u);
    CHECK_EQ(sreg(0, G4MH_SR_EIIC), G4MH_EXC_EIINT_BASE + 1u);
}

/*
 * The disp23 group through the JIT.
 *
 * Two questions, and only the second one is hard. The values must match
 * the interpreter -- but they would match a JIT that declined every one
 * of these and let the interpreter run them, which is what it did until
 * the lowering existed. So the fallback count is the test, and it is an
 * exact number rather than a bound: a decline ends the block, the
 * interpreter runs one instruction and a fresh block starts, so N
 * declines cost exactly N fallbacks and `<= k` hides most of them.
 */
static void test_disp23_jit(void)
{
    const uint32_t cell = EMU_GUEST_RAM_BASE + 0x200u;
    const uint32_t big  = 0x123456u;
    const uint32_t base = cell - big;

    /*
     * Six disp23 instructions and four MOV imm32 -- which is itself a
     * 48-bit form, and is why this count is what it is. Written
     * expecting one fallback, it reported five: the constants were
     * ending a block each and the disp23 lowering underneath them
     * looked as though it had not fired. Both are lowered now, so the
     * only fallback left is the 32-bit HALT.
     */
    const uint16_t prog[] = {
        MOVI32(11), LO(base), HI(base),
        MOVI32(12), 0x5A5Au, 0x0000u,
        MOVI32(20), 0x1111u, 0x0000u,
        MOVI32(21), 0x2222u, 0x0000u,

        D23(0x3Cu, 11, 12, big, 0xFu),         /* st.w  r12, big[r11]  */
        D23(0x3Cu, 11, 13, big, 0x9u),         /* ld.w  big[r11], r13  */
        D23(0x3Cu, 11, 12, big + 4u, 0xDu),    /* st.b  r12, big+4     */
        D23(0x3Du, 11, 14, big + 4u, 0x5u),    /* ld.bu big+4, r14     */
        D23(0x3Du, 11, 20, big + 8u, 0xFu),    /* st.dw r20, big+8     */
        D23(0x3Du, 11, 22, big + 8u, 0x9u),    /* ld.dw big+8, r22     */

        0x07E0u, SUB_HALT,
    };

    const emu_backend_t *saved = g4mh_backend;
    uint32_t vals[2][5];

    for (unsigned pass = 0; pass < 2u; pass++) {
        emu_jit_stats_t before, after;
        emu_run_reason_t why;
        uint32_t retired = 0;

        /* Pass 0 is whatever the frontend picked -- the JIT where there
         * is one; pass 1 forces the interpreter, so a build without a
         * JIT still compares two runs rather than silently one. */
        if (pass == 1u) {
            g4mh_backend = &g4mh_backend_interp;
        }
        emu_jit_get_stats(&before);
        if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                          &retired)) {
            CHECK(false);
            g4mh_backend = saved;
            return;
        }
        emu_jit_get_stats(&after);

        vals[pass][0] = reg(13);
        vals[pass][1] = reg(14);
        vals[pass][2] = reg(22);
        vals[pass][3] = reg(23);
        vals[pass][4] = (uint32_t)g_ram[0x204];

        if (pass == 0u && G4MH_HAVE_JIT && saved != &g4mh_backend_interp) {
            CHECK(after.translations > before.translations);
            /* Only the 32-bit HALT. Six disp23 instructions ahead of it
             * were translated, or this is 7. */
            CHECK_EQ(after.interp_fallbacks - before.interp_fallbacks, 1u);
        }
    }
    g4mh_backend = saved;

    CHECK_EQ(vals[0][0], 0x00005A5Au);      /* ST.W then LD.W          */
    CHECK_EQ(vals[0][1], 0x0000005Au);      /* ST.B then LD.BU         */
    CHECK_EQ(vals[0][2], 0x1111u);          /* LD.DW low               */
    CHECK_EQ(vals[0][3], 0x2222u);          /* LD.DW high              */
    CHECK_EQ(vals[0][4], 0x5Au);            /* the byte really landed  */

    for (unsigned i = 0; i < 5u; i++) {
        CHECK_EQ(vals[0][i], vals[1][i]);
    }
}

/*
 * The branchless group. Each is checked with its condition both true and
 * false, because every one of them has a well-defined "else" that a
 * naive implementation drops -- CMOV would leave the destination alone,
 * ADF would add nothing, SASF would not shift.
 */
static void test_conditional_ops(void)
{
    /* cccc: 0x2 is Z (equal), 0xA is its complement. */
    const uint16_t prog[] = {
        F2(OP_MOVI, 5, 10),
        F2(OP_MOVI, 5, 11),
        F1(OP_CMP, 10, 11),                    /* Z = 1                 */
        F2(OP_MOVI, 7, 12),
        F2(OP_MOVI, 9, 13),
        /* cmov z, r12, r13, r14   -> 7  (taken)                        */
        (uint16_t)((13u << 11) | (0x3Fu << 5) | 12u),
        (uint16_t)((14u << 11) | 0x320u | (0x2u << 1)),
        /* cmov nz, r12, r13, r15  -> 9  (not taken)                    */
        (uint16_t)((13u << 11) | (0x3Fu << 5) | 12u),
        (uint16_t)((15u << 11) | 0x320u | (0xAu << 1)),
        /* adf z,  r12, r13, r16   -> 7 + 9 + 1 = 17                    */
        (uint16_t)((13u << 11) | (0x3Fu << 5) | 12u),
        (uint16_t)((16u << 11) | 0x3A0u | (0x2u << 1)),
        /* the flags moved, so re-establish Z before the next two       */
        F1(OP_CMP, 10, 11),
        /* sbf z,  r12, r13, r17   -> 9 - 7 - 1 = 1                     */
        (uint16_t)((13u << 11) | (0x3Fu << 5) | 12u),
        (uint16_t)((17u << 11) | 0x380u | (0x2u << 1)),
        F1(OP_CMP, 10, 11),
        F2(OP_MOVI, 5, 18),
        /* sasf z, r18             -> (5 << 1) | 1 = 11                 */
        (uint16_t)((18u << 11) | (0x3Fu << 5) | 0x2u), 0x0200u,
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(14), 7u);
    CHECK_EQ(reg(15), 9u);
    CHECK_EQ(reg(16), 17u);
    CHECK_EQ(reg(17), 1u);
    CHECK_EQ(reg(18), 11u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * MAC, BINS and ROTL.
 *
 * MAC's operands are chosen so the product overflows 32 bits and the
 * accumulator is non-zero: a version that ignored the accumulate, or one
 * that kept only the low word, gives a different answer for both halves.
 * BINS uses a field that straddles bit 16, which is what the three
 * sub-opcodes exist to distinguish. ROTL is checked for the carry it
 * defines, including the rotate-by-zero case the manual calls out.
 */
static void test_mac_bins_rotl(void)
{
    const uint16_t prog[] = {
        MOVI32(10), 0x0000u, 0x0001u,          /* r10 = 0x00010000      */
        MOVI32(11), 0x0000u, 0x0002u,          /* r11 = 0x00020000      */
        MOVI32(20), 0x0007u, 0x0000u,          /* r20 = 7  (acc low)    */
        MOVI32(21), 0x0000u, 0x0000u,          /* r21 = 0  (acc high)   */
        /* mac r10, r11, r20, r22: r23||r22 = r11*r10 + r21||r20        */
        (uint16_t)((11u << 11) | (0x3Fu << 5) | 10u),
        (uint16_t)(((20u >> 1) << 12) | 0x3C0u | ((22u >> 1) << 1)),
        MOVI32(12), 0xFFFFu, 0xFFFFu,
        MOVI32(13), 0x0000u, 0x0000u,
        /* bins r12, 15, 17, r13: lsb 15, msb 17 -> three bits at 15    */
        (uint16_t)((13u << 11) | (0x3Fu << 5) | 12u),
        (uint16_t)((1u << 12) | (1u << 11) | 0x0B0u | (7u << 1)),
        MOVI32(14), 0x0001u, 0x8000u,          /* r14 = 0x80000001      */
        /* rotl 1, r14, r15 -> 0x00000003, CY from bit 0 of the result  */
        (uint16_t)((14u << 11) | (0x3Fu << 5) | 1u),
        (uint16_t)((15u << 11) | 0x0C4u),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    /* 0x20000 * 0x10000 = 0x2_0000_0000, plus 7. */
    CHECK_EQ(reg(22), 0x00000007u);
    CHECK_EQ(reg(23), 0x00000002u);
    /* msb 17, lsb 15: bits 17..15 of r13 become the low three of r12. */
    CHECK_EQ(reg(13), 0x00038000u);
    CHECK_EQ(reg(15), 0x00000003u);
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * LOOP, which is the only backward branch in the architecture whose
 * displacement is unsigned and *subtracted*. Reading it as a signed
 * forward displacement, which is what every other branch here does,
 * jumps into the middle of the program instead of round the loop.
 */
static void test_loop(void)
{
    const uint16_t prog[] = {
        F2(OP_MOVI, 3, 10),                    /* counter               */
        F2(OP_MOVI, 0, 11),                    /* accumulator           */
        F2(OP_ADDI5, 5, 11),                   /* loop body: r11 += 5   */
        /* loop r10, 2  -- back to the add, two bytes above             */
        (uint16_t)((0u << 11) | (0x37u << 5) | 10u), (uint16_t)(2u | 1u),
        0x07E0u, SUB_HALT,
    };

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(10), 0u);
    CHECK_EQ(reg(11), 15u);             /* three passes, not one        */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/*
 * CALLT: an indirect call through a table of *halfword* offsets from
 * CTBP. Three things can be wrong without faulting -- the entry width,
 * the zero-extension, and which register the return address lands in --
 * so the target reads CTPC back and the caller checks where it came
 * from as well as that it arrived.
 *
 * Vector 33 is deliberate. Its bit 5 is the low bit of the opcode, which
 * puts this encoding in the SATADD imm5 slot rather than the MOV one;
 * that half used to retire as a saturating add into r0, making the call
 * silently not happen.
 */
static void test_callt(void)
{
    const uint32_t ctbp = EMU_GUEST_RAM_BASE;
    uint16_t prog[64];
    unsigned k = 0;

    prog[k++] = MOVI32(10); prog[k++] = LO(ctbp); prog[k++] = HI(ctbp);
    prog[k++] = W0(OP_SYSTEM, 10, G4MH_SR_CTBP); prog[k++] = SUB_LDSR;
    prog[k++] = (uint16_t)(0x0200u | 33u);      /* callt 33, at byte 10 */
    prog[k++] = F2(OP_MOVI, 1, 12);             /* never reached        */
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    /* The vector table is the program image: entry 33 is halfword 33. */
    while (k < 33u) {
        prog[k++] = 0u;
    }
    prog[k++] = 80u;                            /* -> byte 80, hw 40    */
    while (k < 40u) {
        prog[k++] = 0u;
    }
    prog[k++] = W0(OP_SYSTEM, G4MH_SR_CTPC, 13); prog[k++] = SUB_STSR;
    prog[k++] = F2(OP_MOVI, 9, 11);
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 64u, &why, &retired)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(reg(11), 9u);                      /* the target ran       */
    CHECK_EQ(reg(12), 0u);                      /* and the caller did not */
    CHECK_EQ(reg(13), EMU_GUEST_RAM_BASE + 12u);/* CTPC is the next insn */
    CHECK_EQ(why, EMU_RUN_WFI);
}

/* ------------------------------------------------------------------ */
/* Floating point                                                      */
/* ------------------------------------------------------------------ */

#if G4MH_EXT_FPU

/*
 * Format F:I, as the manual draws it.
 *
 *   first  halfword: reg2[15:11] opcode[10:5]=0x3F reg1[4:0]
 *   second halfword: reg3[15:11] sub[10:0]
 *
 * sub is instruction bits 26..16, which is the same field the integer
 * Format X group is keyed on -- floating point simply lives above 0x400
 * in it.
 */
#define FP0(r1, r2)      W0(OP_SYSTEM, (r1), (r2))
#define FP1(r3, sub)     (uint16_t)(((r3) << 11) | (sub))

#define FSUB_ABSNEG   0x448u
#define FSUB_SQRT     0x44Eu
#define FSUB_ADD      0x460u
#define FSUB_SUB      0x462u
#define FSUB_MUL      0x464u
#define FSUB_MAX      0x468u
#define FSUB_MIN      0x46Au
#define FSUB_DIV      0x46Eu
#define FSUB_CVT_TOI  0x440u
#define FSUB_CVT_FROMI 0x442u
#define FSUB_FMA      0x4E0u
#define FSUB_CMP      0x420u
#define FSUB_CMOV     0x400u

/* Bit patterns, so the expected values are exact rather than rounded. */
#define F_1_0    0x3F800000u
#define F_2_0    0x40000000u
#define F_3_0    0x40400000u
#define F_4_0    0x40800000u
#define F_6_0    0x40C00000u
#define F_0_5    0x3F000000u
#define F_M2_0   0xC0000000u
#define F_QNAN   0x7FC00000u
#define F_SNAN   0x7F800001u

/*
 * Load a 32-bit constant into a register, enable the FPU in PSW.CU0, and
 * leave the program ready for one FP instruction.
 *
 * CU0 has to be set explicitly: out of reset it is clear, and every FP
 * instruction then raises a coprocessor-unusable exception rather than
 * executing -- which is correct, and would make every test below pass
 * for the wrong reason if it were not turned on. test_fp_disabled checks
 * that path on its own.
 */
static unsigned fp_prologue(uint16_t *prog, unsigned k)
{
    /* PSW.CU0 = 1 << 16 */
    prog[k++] = MOVI32(20); prog[k++] = LO(G4MH_PSW_CU0); prog[k++] = HI(G4MH_PSW_CU0);
    prog[k++] = FP0(20, G4MH_SR_PSW); prog[k++] = SUB_LDSR;
    return k;
}

static unsigned fp_ldi(uint16_t *prog, unsigned k, unsigned r, uint32_t v)
{
    prog[k++] = MOVI32(r); prog[k++] = LO(v); prog[k++] = HI(v);
    return k;
}

/* 2.0 + 1.0 = 3.0, and reg3 <- reg2 OP reg1 rather than the other way. */
static void test_fp_arith(void)
{
    uint16_t prog[64];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_1_0);             /* reg1 */
    k = fp_ldi(prog, k, 11, F_2_0);             /* reg2 */

    prog[k++] = FP0(10, 11); prog[k++] = FP1(12, FSUB_ADD);   /* 2+1 */
    prog[k++] = FP0(10, 11); prog[k++] = FP1(13, FSUB_SUB);   /* 2-1 */
    prog[k++] = FP0(10, 11); prog[k++] = FP1(14, FSUB_MUL);   /* 2*1 */
    prog[k++] = FP0(11, 11); prog[k++] = FP1(15, FSUB_MUL);   /* 2*2 */
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(12), F_3_0);
    /*
     * Subtract is the one that discriminates operand order: with the
     * operands the other way round this is -1.0, which is a different
     * bit pattern and not merely a different sign of zero.
     */
    CHECK_EQ(reg(13), F_1_0);
    CHECK_EQ(reg(14), F_2_0);
    CHECK_EQ(reg(15), F_4_0);
}

/* Divide, square root, and the reciprocals that share sub 0x44E. */
static void test_fp_div_sqrt(void)
{
    uint16_t prog[64];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_2_0);
    k = fp_ldi(prog, k, 11, F_6_0);
    k = fp_ldi(prog, k, 12, F_4_0);

    prog[k++] = FP0(10, 11); prog[k++] = FP1(13, FSUB_DIV);   /* 6/2   */
    /*
     * reg1 is an opcode extension here, not a source: 0 is SQRTF.S,
     * 1 RECIPF.S, 2 RSQRTF.S. The operand is reg2 in all three.
     */
    prog[k++] = FP0(0, 12);  prog[k++] = FP1(14, FSUB_SQRT);  /* sqrt 4 */
    prog[k++] = FP0(1, 10);  prog[k++] = FP1(15, FSUB_SQRT);  /* 1/2    */
    prog[k++] = FP0(2, 12);  prog[k++] = FP1(16, FSUB_SQRT);  /* 1/sqrt4*/
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(13), F_3_0);
    CHECK_EQ(reg(14), F_2_0);
    CHECK_EQ(reg(15), F_0_5);
    CHECK_EQ(reg(16), F_0_5);
}

/*
 * ABSF.S and NEGF.S share sub 0x448 and are told apart by reg1 alone.
 *
 * Worth its own test because that is the encoding shape this frontend has
 * been bitten by repeatedly: a register field carrying an opcode. Decoding
 * on the sub-opcode alone would make NEGF.S an ABSF.S and lose the sign,
 * which on a positive input is invisible -- hence the negative one.
 */
static void test_fp_abs_neg(void)
{
    uint16_t prog[64];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_M2_0);

    prog[k++] = FP0(0, 10); prog[k++] = FP1(11, FSUB_ABSNEG);  /* ABSF  */
    prog[k++] = FP0(1, 10); prog[k++] = FP1(12, FSUB_ABSNEG);  /* NEGF  */
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(11), F_2_0);                   /* |-2.0| = +2.0        */
    CHECK_EQ(reg(12), F_2_0);                   /* -(-2.0) = +2.0       */
}

/* MAXF/MINF, on the inputs where "the larger one" is not the answer. */
static void test_fp_minmax(void)
{
    uint16_t prog[64];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_1_0);
    k = fp_ldi(prog, k, 11, F_QNAN);
    k = fp_ldi(prog, k, 12, F_M2_0);

    /* A quiet NaN operand gives the *other* operand, not a NaN. */
    prog[k++] = FP0(10, 11); prog[k++] = FP1(13, FSUB_MAX);
    prog[k++] = FP0(10, 11); prog[k++] = FP1(14, FSUB_MIN);
    /* And a plain ordered pair, where sign matters. */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(15, FSUB_MAX);
    prog[k++] = FP0(10, 12); prog[k++] = FP1(16, FSUB_MIN);
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(13), F_1_0);
    CHECK_EQ(reg(14), F_1_0);
    CHECK_EQ(reg(15), F_1_0);
    CHECK_EQ(reg(16), F_M2_0);
}

/* Conversions both ways, including the rounding the opcode names. */
static void test_fp_convert(void)
{
    uint16_t prog[64];
    unsigned k = 0;
    /* 2.5, which every rounding mode answers differently. */
    const uint32_t f_2_5 = 0x40200000u;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, f_2_5);
    k = fp_ldi(prog, k, 11, 7u);                /* integer 7            */

    /* reg1 selects rounding: 0 round, 1 trunc, 2 ceil, 3 floor. */
    prog[k++] = FP0(0, 10); prog[k++] = FP1(12, FSUB_CVT_TOI); /* ROUNDF */
    prog[k++] = FP0(1, 10); prog[k++] = FP1(13, FSUB_CVT_TOI); /* TRNCF  */
    prog[k++] = FP0(2, 10); prog[k++] = FP1(14, FSUB_CVT_TOI); /* CEILF  */
    prog[k++] = FP0(3, 10); prog[k++] = FP1(15, FSUB_CVT_TOI); /* FLOORF */
    prog[k++] = FP0(0, 11); prog[k++] = FP1(16, FSUB_CVT_FROMI);/* WS    */
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    /*
     * 2.5 is the input that separates the four, which is the whole
     * reason it is the one used: round-to-nearest-even gives 2, not 3.
     */
    CHECK_EQ(reg(12), 2u);
    CHECK_EQ(reg(13), 2u);
    CHECK_EQ(reg(14), 3u);
    CHECK_EQ(reg(15), 2u);
    CHECK_EQ(reg(16), 0x40E00000u);             /* 7.0                  */
}

/*
 * CMPF.S writes a CC bit in FPSR, and CMOVF.S reads it back. They are a
 * pair, so testing them together is what proves the bit number in reg3
 * is the same one on both sides -- a compare that wrote CC0 and a move
 * that read CC1 would each look right alone.
 */
static void test_fp_compare_and_move(void)
{
    uint16_t prog[80];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_1_0);
    k = fp_ldi(prog, k, 11, F_2_0);

    /*
     * fcond 4 is OLT, and the relation is `reg2 < reg1` -- the same
     * operand order as every other FP instruction here. So reg1=r10
     * (1.0), reg2=r11 (2.0) asks "is 2.0 < 1.0", which is false.
     *
     * fcond goes in the *reg3* field and fcbit in the sub-opcode's low
     * bits, which is the opposite of what this test first assumed. Both
     * orders pass a test that only ever uses fcbit 0, so the encoding
     * below is taken from what the vendor assembler emits.
     */
    prog[k++] = FP0(10, 11); prog[k++] = FP1(4, FSUB_CMP | (0u << 1));
    prog[k++] = FP0(10, 11); prog[k++] = FP1(12, FSUB_CMOV | (0u << 1));
    /* Swap the operands: "is 1.0 < 2.0" is true, into the same CC bit. */
    prog[k++] = FP0(11, 10); prog[k++] = FP1(4, FSUB_CMP | (0u << 1));
    prog[k++] = FP0(10, 11); prog[k++] = FP1(13, FSUB_CMOV | (0u << 1));
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    /*
     * CMOVF.S is reg3 <- cc ? reg1 : reg2. This test asserted the other
     * direction and passed against an inverted implementation, because
     * it only ever checked which of two registers came out -- and with
     * both the comparison and the select mirrored, the two errors
     * cancelled. The direction is pinned to CC-RH's own codegen now; see
     * the note in g4mh_fpu.c.
     */
    CHECK_EQ(reg(12), F_2_0);                   /* cc clear -> reg2     */
    CHECK_EQ(reg(13), F_1_0);                   /* cc set   -> reg1     */
}

/*
 * An FP instruction with PSW.CU0 clear is a coprocessor-unusable
 * exception, not a reserved instruction and not a silent execution.
 *
 * This is the state the core comes out of reset in, so without this test
 * every other one above could be passing because the prologue happens to
 * work rather than because the arithmetic does.
 */
static void test_fp_disabled(void)
{
    /* UCPOP vectors to RBASE + 0x80; a HALT there proves control arrived. */
    uint16_t prog[0x60];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));
    k = fp_ldi(prog, k, 10, F_1_0);
    prog[k++] = FP0(10, 10); prog[k++] = FP1(11, FSUB_ADD);
    prog[0x40] = 0x07E0u; prog[0x41] = SUB_HALT;   /* 0x80 bytes in */

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) { CHECK(false); return; }

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK(st.traps >= 1u);
    CHECK_EQ(reg(11), 0u);                      /* destination untouched */
    /* RBASE + 0x80, plus the 4 bytes of the HALT that stopped us. */
    CHECK_EQ(st.pc, EMU_GUEST_RAM_BASE + 0x84u);
}

/* ------------------------------------------------------------------ */
/* Double precision                                                    */
/* ------------------------------------------------------------------ */

/*
 * A double lives in a register pair: the **lower** 32 bits in rN and the
 * higher in rN+1, N even. Same convention as LD.DW.
 */
static unsigned fp_ldd(uint16_t *prog, unsigned k, unsigned r, uint64_t v)
{
    k = fp_ldi(prog, k, r, (uint32_t)v);
    k = fp_ldi(prog, k, r + 1u, (uint32_t)(v >> 32));
    return k;
}

#define D_1_0    UINT64_C(0x3FF0000000000000)
#define D_2_0    UINT64_C(0x4000000000000000)
#define D_3_0    UINT64_C(0x4008000000000000)
#define D_0_5    UINT64_C(0x3FE0000000000000)
#define D_M2_0   UINT64_C(0xC000000000000000)

/* Read a double back out of the pair the guest left it in. */
static uint64_t regd(unsigned r)
{
    return (uint64_t)reg(r) | ((uint64_t)reg(r + 1u) << 32);
}

/*
 * The two-operand group. `reg3 <- reg2 OP reg1`, the same direction as
 * single precision -- and SUBF is where that matters, because ADDF and
 * MULF commute and would pass either way round.
 */
static void test_fp_double_arith(void)
{
    uint16_t prog[80];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldd(prog, k, 10, D_1_0);            /* r10:r11 = 1.0 */
    k = fp_ldd(prog, k, 12, D_3_0);            /* r12:r13 = 3.0 */

    /* addf.d r10, r12, r14  ->  r14 = 3.0 + 1.0 */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(14, 0x470u);
    /* subf.d r10, r12, r16  ->  r16 = 3.0 - 1.0, not 1.0 - 3.0 */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(16, 0x472u);
    /* mulf.d r10, r12, r18 */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(18, 0x474u);
    /* divf.d r10, r12, r20  ->  3.0 / 1.0 */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(20, 0x47Eu);
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ64(regd(14), UINT64_C(0x4010000000000000));   /* 4.0 */
    CHECK_EQ64(regd(16), D_2_0);                          /* 2.0 */
    CHECK_EQ64(regd(18), D_3_0);                          /* 3.0 */
    CHECK_EQ64(regd(20), D_3_0);                          /* 3.0 */
}

/*
 * The pair convention itself, which is the half of double precision
 * that has nothing to do with arithmetic.
 *
 * Two things are checked that an implementation reading only one
 * register would still pass every arithmetic test with: that the *high*
 * word is read (a value differing only above bit 32), and that an odd
 * register number names the even pair below it -- the manual's rule and
 * what CC-RH does with a warning.
 */
static void test_fp_double_pairs(void)
{
    uint16_t prog[80];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    /*
     * 2.0 and 2.0000000000000004: identical in their low 32 bits for
     * the first and differing only in the last mantissa bit, which is
     * the *low* word. So one of these catches a high-word-only read and
     * the other a low-word-only read.
     */
    k = fp_ldd(prog, k, 10, D_2_0);
    k = fp_ldd(prog, k, 12, UINT64_C(0x4000000000000002));

    /* subf.d r10, r12, r14 -> the difference is 2 ulp, not zero */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(14, 0x472u);

    /* absf.d r10, r17 -- an *odd* destination, which must land in
     * r16:r17 and not in r17:r18. */
    prog[k++] = FP0(0, 10); prog[k++] = FP1(17, 0x458u);
    /* negf.d r11, r18 -- an odd *source*, which must read r10:r11. */
    prog[k++] = FP0(1, 11); prog[k++] = FP1(18, 0x458u);

    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    /* Non-zero, and small: reading only the high word would give 0. */
    CHECK(regd(14) != 0u);
    /*
     * 0x4000000000000002 is 2.0 * (1 + 2/2^52) = 2 + 2^-50, so the
     * difference is exactly 2^-50: exponent 1023 - 50 = 0x3CD, mantissa
     * zero. Derived rather than copied from the run -- the first value
     * here was 0x3CB, which is what "two ulp" gives if you forget the
     * exponent is 1 and not 0.
     */
    CHECK_EQ64(regd(14), UINT64_C(0x3CD0000000000000));   /* 2^-50 */

    CHECK_EQ64(regd(16), D_2_0);          /* absf.d wrote r16:r17 */
    CHECK_EQ(reg(18), (uint32_t)D_M2_0);
    CHECK_EQ64(regd(18), D_M2_0);         /* negf.d read r10:r11  */
}

/*
 * The conversions between the two precisions and to and from 32-bit
 * integers, which share sub-opcode 0x452 with reg1 as the selector.
 */
static void test_fp_double_convert(void)
{
    uint16_t prog[96];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_3_0);            /* single 3.0        */
    k = fp_ldd(prog, k, 12, D_2_0);            /* double 2.0        */
    k = fp_ldi(prog, k, 14, 7u);               /* integer 7         */
    k = fp_ldi(prog, k, 15, 0xFFFFFFFFu);      /* -1, or 4294967295 */

    /* cvtf.sd r10, r16 -- single 3.0 to double */
    prog[k++] = FP0(2, 10); prog[k++] = FP1(16, 0x452u);
    /* cvtf.ds r12, r18 -- double 2.0 back to single */
    prog[k++] = FP0(3, 12); prog[k++] = FP1(18, 0x452u);
    /* cvtf.wd r14, r20 -- integer 7 to double */
    prog[k++] = FP0(0, 14); prog[k++] = FP1(20, 0x452u);
    /* cvtf.dw r12, r22 -- double 2.0 to integer */
    prog[k++] = FP0(4, 12); prog[k++] = FP1(22, 0x450u);
    /* cvtf.uwd r15, r24 -- *unsigned* word to double: 0xFFFFFFFF is
     * 4294967295 here and -1 through the signed form, which is the one
     * thing that tells the two apart. */
    prog[k++] = FP0(0x10u, 15); prog[k++] = FP1(24, 0x452u);
    /* cvtf.wd r15, r26 -- the signed reading of the same bits */
    prog[k++] = FP0(0, 15); prog[k++] = FP1(26, 0x452u);

    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ64(regd(16), D_3_0);
    CHECK_EQ(reg(18), F_2_0);
    CHECK_EQ64(regd(20), UINT64_C(0x401C000000000000));   /* 7.0  */
    CHECK_EQ(reg(22), 2u);
    CHECK_EQ64(regd(24), UINT64_C(0x41EFFFFFFFE00000));   /* 4294967295.0 */
    CHECK_EQ64(regd(26), UINT64_C(0xBFF0000000000000));   /* -1.0 */
}

/*
 * The 64-bit integer conversions -- the .L and .UL forms, whose result
 * or source is a register pair.
 *
 * The value is chosen to need more than 32 bits: 2^40 + 5 is exact in a
 * double and is truncated to 5 by anything that keeps only the low
 * word.
 */
static void test_fp_long_convert(void)
{
    uint16_t prog[96];
    unsigned k = 0;
    const uint64_t big = (UINT64_C(1) << 40) + 5u;

    k = fp_prologue(prog, k);
    k = fp_ldd(prog, k, 10, big);              /* integer, as a pair */

    /* cvtf.ld r10, r12 -- long to double */
    prog[k++] = FP0(1, 10); prog[k++] = FP1(12, 0x452u);
    /* cvtf.dl r12, r14 -- and back */
    prog[k++] = FP0(4, 12); prog[k++] = FP1(14, 0x454u);
    /* cvtf.ls r10, r16 -- long to *single*, which cannot hold it
     * exactly, so this also says the rounding happened */
    prog[k++] = FP0(1, 10); prog[k++] = FP1(16, 0x442u);
    /* cvtf.sl r16, r18 -- single back to long */
    prog[k++] = FP0(4, 16); prog[k++] = FP1(18, 0x444u);
    /* trncf.dl on a value with a fraction: 2.75 truncates to 2 */
    k = fp_ldd(prog, k, 20, UINT64_C(0x4006000000000000));
    prog[k++] = FP0(1, 20); prog[k++] = FP1(22, 0x454u);

    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ64(regd(14), big);            /* round trip through a double */
    /* Through a single it rounds: 2^40 + 5 has 41 significant bits and
     * a float has 24, so the 5 is lost. That is the point -- a result
     * of exactly `big` here would mean the single step did nothing. */
    CHECK_EQ64(regd(18), UINT64_C(1) << 40);
    CHECK_EQ64(regd(22), 2u);             /* trncf.dl of 2.75 */
}

/*
 * CMPF.D and CMOVF.D, at the single forms' sub-opcodes with bit 4 set.
 *
 * The relation is `reg2 < reg1`, which is the direction this file has
 * already had backwards once for CMPF.S -- so it is checked with
 * operands that are not equal, in both orders.
 */
static void test_fp_double_compare(void)
{
    uint16_t prog[96];
    unsigned k = 0;

    k = fp_prologue(prog, k);
    k = fp_ldd(prog, k, 10, D_1_0);
    k = fp_ldd(prog, k, 12, D_2_0);
    k = fp_ldi(prog, k, 20, 0xAAu);
    k = fp_ldi(prog, k, 21, 0u);
    k = fp_ldi(prog, k, 22, 0xBBu);
    k = fp_ldi(prog, k, 23, 0u);

    /* cmpf.d 0x4 (OLT), reg1 = r12 (2.0), reg2 = r10 (1.0), fcbit 0.
     * 1.0 < 2.0 is true, so CC0 is set. */
    prog[k++] = FP0(12, 10); prog[k++] = FP1(4, 0x430u);
    /* cmovf.d fcbit 0: r24:r25 <- CC0 ? r20:r21 : r22:r23 */
    prog[k++] = FP0(20, 22); prog[k++] = FP1(24, 0x410u);

    /* The other way round: 2.0 < 1.0 is false, into fcbit 1. */
    prog[k++] = FP0(10, 12); prog[k++] = FP1(4, 0x432u);
    prog[k++] = FP0(20, 22); prog[k++] = FP1(26, 0x412u);

    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(24), 0xAAu);           /* CC0 set   -> reg1 */
    CHECK_EQ(reg(26), 0xBBu);           /* CC1 clear -> reg2 */
}

/*
 * What is still declined, and the reason it is worth a test: FMAF has
 * no double form at all. CC-RH rejects `fmaf.d`, so 0x4F0 is not
 * "FMAF.D" -- and the .D sub-opcodes being the .S ones with bit 4 set
 * is a fact about the encodings that exist, not a rule for generating
 * new ones. An implementation that applied the rule would execute
 * something.
 */
static void test_fp_double_gaps(void)
{
    uint16_t prog[0x60];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));
    k = fp_prologue(prog, k);
    k = fp_ldd(prog, k, 10, D_1_0);
    prog[k++] = FP0(10, 10); prog[k++] = FP1(12, 0x4F0u);   /* not FMAF.D */
    prog[0x30] = 0x07E0u; prog[0x31] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 128u, &why,
                      &retired)) { CHECK(false); return; }

    CHECK_EQ(sreg(0, G4MH_SR_FEIC), G4MH_EXC_RIE);
    CHECK_EQ64(regd(12), 0u);             /* nothing was computed */
}

/*
 * The fused multiply-add rounds once.
 *
 * a*b + c where the product is not representable: 1.0 + 2^-24 squared
 * has a rounding error that a separate multiply and add would discard
 * before the add sees it. Checked the same way rv_fpu's FMA is -- the
 * value differs from the unfused sequence by one ulp, and nothing else
 * about the instruction distinguishes the two.
 */
static void test_fp_fma_rounds_once(void)
{
    uint16_t prog[64];
    unsigned k = 0;
    /*
     * The inputs are the whole test.
     *
     * (1+u) * (1-u/2) - 1, with u one ulp of 1.0. Rounded once the answer
     * is 0x337FFFFE; a multiply that rounds and *then* adds gives exactly
     * **zero**, because the product rounds to 1.0 and the subtraction
     * cancels. So a two-rounding implementation is not one ulp out here,
     * it loses the result entirely -- which is what makes this a test
     * rather than a restatement.
     *
     * The first version of this test used (1+u)^2 - 1 and asserted a
     * value that was simply wrong. Worse, the two implementations agree
     * on that input: it would have passed against an unfused multiply-add
     * and proved nothing. Found by computing both forms for a few hundred
     * operand pairs and keeping one where they differ.
     */
    const uint32_t f_a = 0x3F800001u;           /* 1 + u                */
    const uint32_t f_b = 0x3F7FFFFFu;           /* 1 - u/2              */
    const uint32_t f_c = 0xBF800000u;           /* -1.0                 */

    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, f_a);
    k = fp_ldi(prog, k, 11, f_b);
    k = fp_ldi(prog, k, 12, f_c);               /* the accumulator      */

    /* FMAF.S: reg3 <- reg3 + reg2*reg1, so reg3 is read as well. */
    prog[k++] = FP0(10, 11); prog[k++] = FP1(12, FSUB_FMA);
    prog[k++] = 0x07E0u; prog[k++] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, k, 128u, &why, &retired)) { CHECK(false); return; }

    CHECK_EQ(reg(12), 0x337FFFFEu);
    CHECK(reg(12) != 0u);                       /* the unfused answer   */
}

#endif /* G4MH_EXT_FPU */

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* INTC1 / INTC2 registers                                             */
/* ------------------------------------------------------------------ */

/*
 * These drive the device ops directly rather than through a guest
 * program, because what is under test is the *register model* -- which
 * bits alias which -- and a guest program can only reach it through
 * stores whose width and alignment become part of what is being tested.
 * The tests that already exist here for the TPTM go the other way and
 * should: they are about delivery.
 *
 * Everything asserted below is from the RH850/U2B hardware manual
 * R01UH0923EJ0130 tables 6.15 (EICn), 6.16 (IMRm) and 6.20 (EEICn).
 */

static g4mh_intc_t g_tic;
static g4mh_cpu_t  g_tic_cpu;

static void tic_reset(void)
{
    memset(&g_tic_cpu, 0, sizeof(g_tic_cpu));
    g4mh_intc_init(&g_tic, &g_tic_cpu, NULL);
}

static uint32_t tic1_rd(uint32_t off)
{
    uint32_t v = 0u;
    (void)g4mh_intc1_ops.read(&g_tic, off, 4u, &v);
    return v;
}

static void tic1_wr(uint32_t off, uint32_t v)
{
    (void)g4mh_intc1_ops.write(&g_tic, off, 4u, v);
}

static uint32_t tic2_rd(uint32_t off)
{
    uint32_t v = 0u;
    (void)g4mh_intc2_ops.read(&g_tic, off, 4u, &v);
    return v;
}

static void tic2_wr(uint32_t off, uint32_t v)
{
    (void)g4mh_intc2_ops.write(&g_tic, off, 4u, v);
}

/*
 * **IMRm is the EIMK bits, not a register beside them.** The manual says
 * a write to either is reflected in the other, and holding them
 * separately is what let the mask be stored and then ignored: IMR reset
 * to 0 against an architectural FFFF_FFFFH, and nothing consulted it
 * when choosing a channel.
 *
 * Both directions are checked, because one of them alone passes against
 * an implementation that keeps two copies and updates one from the
 * other.
 */
static void test_intc_imr_aliases_eimk(void)
{
    tic_reset();

    /* Reset is every channel masked. */
    CHECK_EQ(tic1_rd(G4MH_INTC1_IMR0), G4MH_IMR_RESET);
    CHECK_EQ(tic1_rd(0u * 2u) & G4MH_EIC_EIMK, (uint32_t)G4MH_EIC_EIMK);

    /* EICn -> IMRm: unmask channel 5 through EIC5. */
    tic1_wr(5u * 2u, G4MH_EIC_RESET & ~(uint32_t)G4MH_EIC_EIMK);
    CHECK_EQ(tic1_rd(G4MH_INTC1_IMR0) & (1u << 5), 0u);
    /* and nothing else moved */
    CHECK_EQ(tic1_rd(G4MH_INTC1_IMR0) | (1u << 5), G4MH_IMR_RESET);

    /* IMRm -> EICn: unmask channel 9 through IMR0. */
    tic1_wr(G4MH_INTC1_IMR0, G4MH_IMR_RESET & ~(1u << 9));
    CHECK_EQ(tic1_rd(9u * 2u) & G4MH_EIC_EIMK, 0u);
    /* writing IMR0 re-masked 5, which is what "reflected" means */
    CHECK_EQ(tic1_rd(5u * 2u) & G4MH_EIC_EIMK, (uint32_t)G4MH_EIC_EIMK);
}

/*
 * The functional consequence, which is the reason the aliasing matters:
 * a channel masked through IMRm must not be delivered. This is the check
 * that fails against the old code no matter how IMR was stored, because
 * the chooser never looked at it.
 */
static void test_intc_imr_masks_delivery(void)
{
    tic_reset();

    g4mh_intc_raise(&g_tic, 7u);
    CHECK_EQ((uint32_t)(g4mh_intc_pending(&g_tic, 0u) + 1), 0u); /* -1 */

    /* Unmask only through IMR0 -- never touching EIC7. */
    tic1_wr(G4MH_INTC1_IMR0, G4MH_IMR_RESET & ~(1u << 7));
    CHECK_EQ((uint32_t)g4mh_intc_pending(&g_tic, 0u), 7u);

    /* Re-mask through IMR0 and it must go away again. */
    tic1_wr(G4MH_INTC1_IMR0, G4MH_IMR_RESET);
    CHECK_EQ((uint32_t)(g4mh_intc_pending(&g_tic, 0u) + 1), 0u);
}

/*
 * EEICn is the same word with six priority bits where EICn shows four.
 *
 * The awkward case is a priority that needs EIP[5:4]: 16 and 0 are
 * indistinguishable through the EICn window, so an implementation that
 * compares only the low nibble ranks them equal and picks the lower
 * channel number instead of the higher priority.
 */
static void test_intc_eeic_six_bit_priority(void)
{
    tic_reset();

    /* Channel 3 at priority 16, channel 8 at priority 0, both unmasked. */
    tic1_wr(G4MH_INTC1_EEIC + 3u * 4u, 16u);
    tic1_wr(G4MH_INTC1_EEIC + 8u * 4u, 0u);
    g4mh_intc_raise(&g_tic, 3u);
    g4mh_intc_raise(&g_tic, 8u);

    /* 0 beats 16. Comparing four bits makes them tie and picks 3. */
    CHECK_EQ((uint32_t)g4mh_intc_pending(&g_tic, 0u), 8u);

    /* Through the narrow window both look like priority 0. */
    CHECK_EQ(tic1_rd(3u * 2u) & G4MH_EIC_EIP_MASK, 0u);
    CHECK_EQ(tic1_rd(8u * 2u) & G4MH_EIC_EIP_MASK, 0u);
    /* but the wide one still says 16 */
    CHECK_EQ(tic1_rd(G4MH_INTC1_EEIC + 3u * 4u) & G4MH_EEIC_EIP_MASK, 16u);

    /*
     * A 16-bit write must leave EIP[5:4] alone: it cannot express them,
     * and clearing them would promote this channel from 16 to 0.
     */
    tic1_wr(3u * 2u, G4MH_EIC_EITB | 0x2u);
    CHECK_EQ(tic1_rd(G4MH_INTC1_EEIC + 3u * 4u) & G4MH_EEIC_EIP_MASK,
             16u + 2u);
}

/*
 * EICT is read-only in both windows -- it describes how the source is
 * wired, not a preference -- and EIOV records an edge that arrived while
 * one was already pending. Neither was implemented: EICT was taken from
 * whatever software wrote, and EIOV was never set at all, so an overrun
 * was indistinguishable from a single interrupt.
 */
static void test_intc_eict_readonly_and_eiov(void)
{
    tic_reset();

    /* Try to make channel 2 level-detected. The bit must not move. */
    tic1_wr(2u * 2u, G4MH_EIC_EICT);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EICT, 0u);
    tic1_wr(G4MH_INTC1_EEIC + 2u * 4u, G4MH_EEIC_EICT);
    CHECK_EQ(tic1_rd(G4MH_INTC1_EEIC + 2u * 4u) & G4MH_EEIC_EICT, 0u);

    /* First edge sets EIRF and leaves EIOV clear. */
    tic_reset();
    g4mh_intc_raise(&g_tic, 2u);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EIRF, (uint32_t)G4MH_EIC_EIRF);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EIOV, 0u);

    /* A second one while the first is pending sets EIOV. */
    g4mh_intc_raise(&g_tic, 2u);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EIOV, (uint32_t)G4MH_EIC_EIOV);

    /* Acknowledging clears EIRF -- edge detection -- but not EIOV, which
     * is the guest's to clear once it has seen it. */
    g4mh_intc_ack(&g_tic, 2u);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EIRF, 0u);
    CHECK_EQ(tic1_rd(2u * 2u) & G4MH_EIC_EIOV, (uint32_t)G4MH_EIC_EIOV);
}

/*
 * IMRn lives at <INTC2_base> + 1000H + 04H * n for n = 1..31, so the
 * register at offset 0x1000 is IMR0's slot -- which belongs to INTC1 and
 * is not mapped in INTC2. Biasing the index instead aliases every
 * register onto its neighbour, which is a mistake this file exists to
 * catch and which was made and caught while writing it.
 */
static void test_intc_imr_addressing(void)
{
    tic_reset();

    /* IMR1 covers channels 32..63; unmask channel 32 through it. */
    tic2_wr(G4MH_INTC2_IMR + 1u * 4u, G4MH_IMR_RESET & ~1u);
    CHECK_EQ(tic2_rd(32u * 2u) & G4MH_EIC_EIMK, 0u);
    /* channel 64, which IMR2 covers, must be untouched */
    CHECK_EQ(tic2_rd(64u * 2u) & G4MH_EIC_EIMK, (uint32_t)G4MH_EIC_EIMK);

    /* IMR0's slot is not INTC2's: writing it must change nothing. */
    tic2_wr(G4MH_INTC2_IMR, 0u);
    CHECK_EQ(tic1_rd(G4MH_INTC1_IMR0), G4MH_IMR_RESET);
    CHECK_EQ(tic2_rd(G4MH_INTC2_IMR), 0u);

    /* And IMR2 reaches channel 64. */
    tic2_wr(G4MH_INTC2_IMR + 2u * 4u, G4MH_IMR_RESET & ~1u);
    CHECK_EQ(tic2_rd(64u * 2u) & G4MH_EIC_EIMK, 0u);
}

/* ------------------------------------------------------------------ */
/* MPU                                                                 */
/* ------------------------------------------------------------------ */

#if G4MH_EXT_MPU

/*
 * Driven through g4mh_mpu_permits directly. The alternative -- a guest
 * program that faults -- tests the *plumbing* and is worth having, but
 * it cannot enumerate the permission matrix, and the matrix is where an
 * MPU is wrong: two independent gates (the mode group and the SPID
 * group) that both have to allow, plus an enable bit, plus SVP.
 *
 * Bit positions from R01UH0923EJ0130 table 3.76.
 */

static g4mh_mpu_t g_tm;

/* Configure entry `e` to cover [lo,hi] with attributes `at`. */
static void mpu_entry(unsigned e, uint32_t lo, uint32_t hi, uint32_t at)
{
    g_tm.mpidx = e;
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPLA, lo);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPUA, hi);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPAT, at);
}

static void test_mpu_modes_and_enable(void)
{
    g4mh_mpu_reset(&g_tm);
    CHECK(!g4mh_mpu_is_active(&g_tm));

    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPM, G4MH_MPM_MPE);
    CHECK(g4mh_mpu_is_active(&g_tm));

    /*
     * Supervisor with SVP clear is "enable all accesses in SV mode" --
     * *all*, fetch included, and with no entry configured. A model that
     * only bypassed the data path would refuse to fetch its own code the
     * moment a guest set MPE, which is the reset arrangement and would
     * make the MPU unusable rather than merely wrong.
     */
    CHECK(g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_READ, false, 0u));
    CHECK(g4mh_mpu_permits(&g_tm, 0x1000u, 2u, G4MH_MPU_FETCH, false, 0u));
    CHECK(g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_WRITE, false, 0u));

    /* User mode is checked even with SVP clear, and matches nothing. */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_READ, true, 0u));

    /* With SVP set, supervisor is checked too. */
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPM,
                            G4MH_MPM_MPE | G4MH_MPM_SVP);
    CHECK(!g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_READ, false, 0u));

    /* An area with E clear is not consulted, however permissive. */
    mpu_entry(0u, 0x1000u, 0x1FFFu,
              G4MH_MPAT_SR | G4MH_MPAT_RG);          /* no E */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_READ, false, 0u));
    mpu_entry(0u, 0x1000u, 0x1FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_RG);
    CHECK(g4mh_mpu_permits(&g_tm, 0x1000u, 4u, G4MH_MPU_READ, false, 0u));
}

/*
 * The permission matrix. Read, write and execute are separate bits per
 * mode, and the awkward pair is read-vs-execute: RMPIDn covers both in
 * the SPID group, so a model that reused it for the mode group as well
 * would let a readable area be executed.
 */
static void test_mpu_permission_matrix(void)
{
    g4mh_mpu_reset(&g_tm);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPM,
                            G4MH_MPM_MPE | G4MH_MPM_SVP);

    /* Readable, not executable, not writable -- supervisor. */
    mpu_entry(0u, 0x2000u, 0x2FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_RG | G4MH_MPAT_WG);
    CHECK(g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_READ, false, 0u));
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2000u, 2u, G4MH_MPU_FETCH, false, 0u));
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_WRITE, false, 0u));

    /* Executable but not readable is a real combination and distinct. */
    mpu_entry(0u, 0x2000u, 0x2FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SX | G4MH_MPAT_RG | G4MH_MPAT_WG);
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_READ, false, 0u));
    CHECK(g4mh_mpu_permits(&g_tm, 0x2000u, 2u, G4MH_MPU_FETCH, false, 0u));

    /* The user bits are separate from the supervisor ones. */
    mpu_entry(0u, 0x2000u, 0x2FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_RG | G4MH_MPAT_WG);
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_READ, true, 0u));
    mpu_entry(0u, 0x2000u, 0x2FFFu,
              G4MH_MPAT_E | G4MH_MPAT_UR | G4MH_MPAT_RG | G4MH_MPAT_WG);
    CHECK(g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_READ, true, 0u));
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2000u, 4u, G4MH_MPU_READ, false, 0u));
}

/*
 * Bounds. MPUA is inclusive and its low two bits read as 1, and the
 * *whole span* of an access must be inside -- an access straddling the
 * top is a violation even though its first byte is not.
 */
static void test_mpu_bounds(void)
{
    g4mh_mpu_reset(&g_tm);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPM,
                            G4MH_MPM_MPE | G4MH_MPM_SVP);
    mpu_entry(0u, 0x3000u, 0x3FFCu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_SW |
              G4MH_MPAT_RG | G4MH_MPAT_WG);

    /* Both ends are included. */
    CHECK(g4mh_mpu_permits(&g_tm, 0x3000u, 4u, G4MH_MPU_READ, false, 0u));
    CHECK(g4mh_mpu_permits(&g_tm, 0x3FFCu, 4u, G4MH_MPU_READ, false, 0u));

    /* One byte before and one word after are outside. */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x2FFFu, 1u, G4MH_MPU_READ, false, 0u));
    CHECK(!g4mh_mpu_permits(&g_tm, 0x4000u, 4u, G4MH_MPU_READ, false, 0u));

    /*
     * Straddling the top. The first byte is inside, so a check that
     * looked only at the start address would permit it.
     */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x3FFEu, 4u, G4MH_MPU_READ, false, 0u));

    /*
     * Overlapping areas: permitted by *any* is permitted. A search that
     * stopped at the first matching entry would make the order
     * significant, and here entry 0 refuses what entry 1 allows.
     */
    mpu_entry(0u, 0x5000u, 0x5FFFu, G4MH_MPAT_E | G4MH_MPAT_RG |
                                    G4MH_MPAT_WG);   /* no permissions */
    mpu_entry(1u, 0x5000u, 0x5FFFu, G4MH_MPAT_E | G4MH_MPAT_SR |
                                    G4MH_MPAT_RG | G4MH_MPAT_WG);
    CHECK(g4mh_mpu_permits(&g_tm, 0x5000u, 4u, G4MH_MPU_READ, false, 0u));
}

/*
 * The SPID group, which is the gate a mode-only model has no idea
 * exists. RG/WG bypass it; without them the accessing SPID must appear
 * in one of the eight MPIDn slots whose matching RMPIDn/WMPIDn bit is
 * set.
 */
static void test_mpu_spid_group(void)
{
    g4mh_mpu_reset(&g_tm);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPM,
                            G4MH_MPM_MPE | G4MH_MPM_SVP);

    /* SPID 3 in slot 2, and only slot 2 permitted to read. */
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPID0 + 2u, 3u);
    mpu_entry(0u, 0x6000u, 0x6FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_SW |
              (1u << (G4MH_MPAT_RMPID_SHIFT + 2u)));

    CHECK(g4mh_mpu_permits(&g_tm, 0x6000u, 4u, G4MH_MPU_READ, false, 3u));
    /* Same area, same mode bits, different master: refused. */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x6000u, 4u, G4MH_MPU_READ, false, 4u));

    /*
     * SW is set but no WMPIDn is, so writing is refused for every SPID.
     * A model that treated the two groups as alternatives -- permit on
     * either -- would allow this.
     */
    CHECK(!g4mh_mpu_permits(&g_tm, 0x6000u, 4u, G4MH_MPU_WRITE, false, 3u));

    /* WG bypasses the SPID group entirely. */
    mpu_entry(0u, 0x6000u, 0x6FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SR | G4MH_MPAT_SW |
              (1u << (G4MH_MPAT_RMPID_SHIFT + 2u)) | G4MH_MPAT_WG);
    CHECK(g4mh_mpu_permits(&g_tm, 0x6000u, 4u, G4MH_MPU_WRITE, false, 9u));

    /* RMPIDn covers execution as well as reading. */
    mpu_entry(0u, 0x6000u, 0x6FFFu,
              G4MH_MPAT_E | G4MH_MPAT_SX |
              (1u << (G4MH_MPAT_RMPID_SHIFT + 2u)));
    CHECK(g4mh_mpu_permits(&g_tm, 0x6000u, 2u, G4MH_MPU_FETCH, false, 3u));
    CHECK(!g4mh_mpu_permits(&g_tm, 0x6000u, 2u, G4MH_MPU_FETCH, false, 4u));
}

/*
 * The MPIDX window: MPLA/MPUA/MPAT refer to whichever entry MPIDX
 * selects, and an out-of-range index is "handled as an undefined
 * register" -- reads zero, writes dropped. Wrapping onto entry 0 instead
 * would corrupt a configured area from a typo.
 */
static void test_mpu_window(void)
{
    uint32_t v;

    g4mh_mpu_reset(&g_tm);

    mpu_entry(5u, 0x7000u, 0x7FFFu, G4MH_MPAT_E | G4MH_MPAT_SR);
    mpu_entry(6u, 0x8000u, 0x8FFFu, G4MH_MPAT_E | G4MH_MPAT_SW);

    g_tm.mpidx = 5u;
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0x7000u);
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPAT, &v));
    CHECK_EQ(v, (uint32_t)(G4MH_MPAT_E | G4MH_MPAT_SR));
    g_tm.mpidx = 6u;
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0x8000u);

    /*
     * Out of range: reads zero, writes go nowhere, and above all they do
     * not wrap onto entry 0 and corrupt a configured area.
     *
     * **Reachable only when the build has fewer than 32 entries.** MPIDX
     * is five bits and is masked to them on write, so at the default of
     * 32 every index is valid and the guard in entry_index() cannot be
     * taken -- an A/B that removes it changes nothing, which is exactly
     * what happened when this test was first written with the check
     * compiled out and silently asserting nothing.
     *
     * So the assertion is made where it means something, and the
     * *reason* it is absent otherwise is asserted instead. Building with
     * -DG4MH_MPU_ENTRIES=8 is what exercises the other side; it is not
     * the default because the part has 32.
     */
#if G4MH_MPU_ENTRIES < 32u
    {
        /*
         * Snapshot every entry, write through an out-of-range index,
         * and require that *nothing* moved.
         *
         * Checking one neighbour is not enough and the first version of
         * this did exactly that: with the guard removed, MPLA at index N
         * runs off the end of mpla[] and lands on mpua[0], so a test
         * that read back MPLA of a configured entry saw the right value
         * and passed. Comparing the whole array is what makes the
         * assertion about "does not corrupt an entry" rather than about
         * one entry that happens not to be the one hit.
         */
        uint32_t la[G4MH_MPU_ENTRIES], ua[G4MH_MPU_ENTRIES];
        uint32_t at[G4MH_MPU_ENTRIES];

        for (unsigned e = 0; e < G4MH_MPU_ENTRIES; e++) {
            g_tm.mpidx = e;
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &la[e]);
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPUA, &ua[e]);
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPAT, &at[e]);
        }

        g_tm.mpidx = G4MH_MPU_ENTRIES;      /* the first invalid index */
        CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0u);
        (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPLA, 0xDEADBE00u);
        (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPUA, 0xDEADBE00u);
        (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPAT, 0xFFFFFFFFu);

        for (unsigned e = 0; e < G4MH_MPU_ENTRIES; e++) {
            uint32_t x;
            g_tm.mpidx = e;
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &x); CHECK_EQ(x, la[e]);
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPUA, &x); CHECK_EQ(x, ua[e]);
            (void)g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPAT, &x); CHECK_EQ(x, at[e]);
        }
    }
#else
    /* Every five-bit index is a valid entry, which is why there is
     * nothing to test above. */
    CHECK_EQ((uint32_t)G4MH_MPU_ENTRIES, 32u);
    g_tm.mpidx = 31u;
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPLA, 0xABCD0000u);
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0xABCD0000u);
    g_tm.mpidx = 5u;
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0x7000u);
#endif

    /* MPCFG reports the geometry and is read-only. */
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPCFG, &v));
    CHECK_EQ(v & 0x1Fu, G4MH_MPU_ENTRIES - 1u);
    CHECK_EQ((v >> 16) & 0xFu, G4MH_MPCFG_ARCH);
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPCFG, 0u);
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPCFG, &v));
    CHECK_EQ(v & 0x1Fu, G4MH_MPU_ENTRIES - 1u);

    /* The address registers drop bits 1:0, which read back zero. */
    g_tm.mpidx = 5u;
    (void)g4mh_mpu_sr_write(&g_tm, G4MH_SR_MPLA, 0x9003u);
    CHECK(g4mh_mpu_sr_read(&g_tm, G4MH_SR_MPLA, &v)); CHECK_EQ(v, 0x9000u);
}
#endif /* G4MH_EXT_MPU */

/* ------------------------------------------------------------------ */
/* Interrupt priority ceiling: ISPR, PSW.EIMASK, PLMR                  */
/* ------------------------------------------------------------------ */

/*
 * Driven through g4mh_cpu_pending_irq_pri / ack / eiret rather than
 * through a guest, because what is under test is the *decision* and a
 * guest program can only observe it as "a handler ran". The counts and
 * thresholds are from U2B tables 3.44 (ISPR), 3.49 (PLMR) and 3.52
 * (PSW.EIMASK), with the combining rule from figure 3.17.
 */

/* Give channel `ch` priority `pri`, unmasked, and raise it. */
static void ceil_arm(unsigned ch, unsigned pri)
{
    (void)g4mh_intc1_ops.write(&g_tic, G4MH_INTC1_EEIC + ch * 4u, 4u, pri);
    g4mh_intc_raise(&g_tic, ch);
}

static void ceil_reset(void)
{
    tic_reset();
    g_tic_cpu.psw = 0u;                 /* PSW.ID clear: interrupts on */
    g_tic_cpu.sr[0][G4MH_SR_PSW] = 0u;
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR]   = G4MH_PLMR_RESET;
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_INTCFG] = G4MH_INTCFG_RESET;
    g_tic_cpu.intc = &g_tic;
}

/*
 * The whole point of the ceiling: while a handler is in service, an
 * interrupt of the *same or lower* priority must not preempt it, and a
 * higher-priority one must. Without ISPR every pending channel is
 * delivered the moment PSW.ID allows, so a handler is re-entered by its
 * own source and a priority scheme means nothing.
 */
static void test_int_ceiling_ispr(void)
{
    unsigned pri = 99u;

    ceil_reset();

    /* Priority 5 arrives and is accepted. */
    ceil_arm(3u, 5u);
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);
    CHECK_EQ(pri, 5u);
    g4mh_cpu_ack_priority(&g_tic_cpu, 5u);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 1u << 5);
    g4mh_intc_ack(&g_tic, 3u);

    /* Same priority: refused. */
    ceil_arm(4u, 5u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* Lower priority (numerically larger): refused. */
    g4mh_intc_ack(&g_tic, 4u);
    ceil_arm(5u, 9u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* Higher priority: accepted, and nests. */
    ceil_arm(6u, 2u);
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 6u);
    CHECK_EQ(pri, 2u);
    g4mh_cpu_ack_priority(&g_tic_cpu, 2u);
    /*
     * The *channel* has to be acknowledged too, and forgetting it is
     * what the first draft of this test did: EIRET then dropped the
     * ceiling but channel 6 was still requesting, so the next query
     * returned 6 again rather than the priority-9 channel underneath.
     * Two separate acknowledgements, because they are two separate
     * things -- the controller's request flag and the core's ceiling.
     */
    g4mh_intc_ack(&g_tic, 6u);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR],
             (1u << 5) | (1u << 2));

    /*
     * EIRET clears the *highest* priority in service -- the lowest set
     * bit -- which is what lets the bits be the nesting stack. Clearing
     * the most recently set bit would be the same thing here only
     * because they nested in order.
     */
    g4mh_cpu_eiret_priority(&g_tic_cpu);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 1u << 5);
    /* Priority 9 is still refused: the outer handler is still in service. */
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    g4mh_cpu_eiret_priority(&g_tic_cpu);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 0u);
    /* Now it is taken. */
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 5u);

    /* EIRET with nothing in service must not underflow. */
    g4mh_cpu_eiret_priority(&g_tic_cpu);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 0u);
}

/*
 * PLMR applies in both modes and is the software ceiling. Its field
 * counts *acceptable* levels, so PLM = n admits priorities 0..n-1 --
 * which is why its reset of 16 admits exactly the sixteen ISPR can
 * represent, and why priority 63 is never acknowledged.
 */
static void test_int_ceiling_plmr(void)
{
    unsigned pri = 99u;

    ceil_reset();
    ceil_arm(3u, 4u);

    /* Default PLM = 16 admits priority 4. */
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);

    /* PLM = 4 admits 0..3, so 4 is refused -- the off-by-one case. */
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 4u;
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* PLM = 5 admits it. */
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 5u;
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);

    /* PLM = 0 admits nothing at all. */
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 0u;
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* Priority 63 is refused even at the maximum PLM. */
    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 63u;
    ceil_arm(3u, 63u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);
    /* 62 is the lowest that can be. */
    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 63u;
    ceil_arm(3u, 62u);
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);
}

/*
 * 64-priority mode: INTCFG.EPL swaps ISPR out for PSW.EIMASK, and
 * acknowledging stores the priority there. ISPR must then do nothing at
 * all -- "the function of the ISPR register is disabled".
 */
static void test_int_ceiling_eimask(void)
{
    unsigned pri = 99u;

    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_INTCFG] =
        G4MH_INTCFG_RESET | G4MH_INTCFG_EPL;
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 63u;   /* PLMR out of the way */

    /* EIMASK resets to 0, which admits nothing. */
    ceil_arm(3u, 10u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* EIMASK = 11 admits 0..10. */
    g_tic_cpu.psw = 11u << G4MH_PSW_EIMASK_SHIFT;
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);
    /* EIMASK = 10 does not. */
    g_tic_cpu.psw = 10u << G4MH_PSW_EIMASK_SHIFT;
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* Acknowledging stores the priority, which blocks the same level. */
    g_tic_cpu.psw = 11u << G4MH_PSW_EIMASK_SHIFT;
    g4mh_cpu_ack_priority(&g_tic_cpu, 10u);
    CHECK_EQ((g_tic_cpu.psw & G4MH_PSW_EIMASK_MASK) >>
             G4MH_PSW_EIMASK_SHIFT, 10u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* And ISPR was not touched, because EPL disables it. */
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 0u);

    /* A 6-bit priority beyond what ISPR could hold still works here. */
    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_INTCFG] =
        G4MH_INTCFG_RESET | G4MH_INTCFG_EPL;
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 63u;
    g_tic_cpu.psw = 40u << G4MH_PSW_EIMASK_SHIFT;
    ceil_arm(3u, 33u);
    CHECK_EQ((uint32_t)g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri), 3u);
    CHECK_EQ(pri, 33u);
}

/*
 * Two edges of the 16-priority mode that a straightforward reading gets
 * wrong: a priority of 16 or above sets no ISPR bit when acknowledged,
 * and is refused while *any* bit is set; and INTCFG.ISPC turns the
 * automatic update off so software can run its own ceiling.
 */
static void test_int_ceiling_edges(void)
{
    unsigned pri = 99u;

    /* Priority >= 16 records nothing. */
    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_PLMR] = 63u;
    g4mh_cpu_ack_priority(&g_tic_cpu, 20u);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 0u);

    /* But any bit set refuses it. */
    g4mh_cpu_ack_priority(&g_tic_cpu, 15u);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 1u << 15);
    ceil_arm(3u, 20u);
    CHECK_EQ((uint32_t)(g4mh_cpu_pending_irq_pri(&g_tic_cpu, &pri) + 1), 0u);

    /* ISPC = 1: acknowledging records nothing and EIRET clears nothing. */
    ceil_reset();
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_INTCFG] =
        G4MH_INTCFG_RESET | G4MH_INTCFG_ISPC;
    g4mh_cpu_ack_priority(&g_tic_cpu, 4u);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 0u);
    /* Software's own value survives EIRET. */
    g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR] = 1u << 7;
    g4mh_cpu_eiret_priority(&g_tic_cpu);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 1u << 7);

    /*
     * EIRET returning from an *exception* (PSW.EP set) leaves the
     * ceiling alone -- an exception never raised it.
     */
    ceil_reset();
    g4mh_cpu_ack_priority(&g_tic_cpu, 6u);
    g_tic_cpu.psw |= G4MH_PSW_EP;
    g4mh_cpu_eiret_priority(&g_tic_cpu);
    CHECK_EQ(g_tic_cpu.sr[G4MH_SELID_INT][G4MH_SR_ISPR], 1u << 6);
}

void test_g4mh(void)
{
    test_int_ceiling_ispr();
    test_int_ceiling_plmr();
    test_int_ceiling_eimask();
    test_int_ceiling_edges();
#if G4MH_EXT_MPU
    test_mpu_modes_and_enable();
    test_mpu_permission_matrix();
    test_mpu_bounds();
    test_mpu_spid_group();
    test_mpu_window();
#endif
    test_intc_imr_aliases_eimk();
    test_intc_imr_masks_delivery();
    test_intc_eeic_six_bit_priority();
    test_intc_eict_readonly_and_eiov();
    test_intc_imr_addressing();
    test_length();
    test_conditions();
    test_alu();
    test_flags_and_branch();
    test_load_store();
    test_mov_imm32();
    test_prepare_dispose();
    test_unsigned_loads();
    test_disp23_loads_stores();
    test_disp23_doubleword();
    test_disp23_reserved_bit();
    test_disp23_jit();
    test_disasm_crowded_slots();
    test_disasm_length_disagreement();
    test_jr_disp32();
    test_prepare_imm32();
    test_prepare_ff_forms();
    test_barrier();
    test_barrier_participation();
    test_barrier_enable_completes();
    test_ipir_routing();
    test_ipir_enable_gates_transfer();
    test_ipir_self_region();
    test_tptm_interval();
    test_tptm_divider_carry();
    test_tptm_up_compare();
    test_tptm_interrupt_ei();
    test_tptm_interrupt_feint();
    test_tptm_interrupt_masked();
    test_ipir_delivers();
    test_conditional_ops();
    test_mac_bins_rotl();
    test_loop();
    test_callt();
    test_muldiv();
    test_shift_three_operand();
    test_divq_divh();
    test_divh_halfword_only();
    test_mul_imm9();
    test_clip();
    test_gdb_layout();
    test_perf_counters();
    test_perf_umctrl_bank();
    test_perf_counters_agree();
    test_fetrap();
    test_resbank_is_not_di();
    test_narrow_atomics();
    test_pointer_update_addressing();
    test_swap();
    test_swap_halfword_flags();
    test_bit_search();
    test_bit_search_zero();
    test_swap_reserved_field();
    test_bit_manipulation();
    test_bit_manipulation_reg();
    test_ir_backend_is_used();
    test_system_registers();
    test_reserved_instruction();
    test_trap_and_syscall();
    test_contract();
    test_mc_caxi();
    test_mc_reservation_succeeds();
#if G4MH_EXT_FPU
    test_fp_arith();
    test_fp_div_sqrt();
    test_fp_abs_neg();
    test_fp_minmax();
    test_fp_convert();
    test_fp_compare_and_move();
    test_fp_disabled();
    test_fp_double_arith();
    test_fp_double_pairs();
    test_fp_double_convert();
    test_fp_long_convert();
    test_fp_double_compare();
    test_fp_double_gaps();
    test_fp_fma_rounds_once();
#endif
#if G4MH_PE_COUNT > 1
    test_mc_dispatch();
    test_mc_quantum_invariance();
    test_mc_reservation();
#endif
}
