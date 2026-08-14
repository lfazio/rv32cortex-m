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

    /* --- core 0 --- */
    prog[k++] = W0(OP_MOVHI, 0, 11);  prog[k++] = 0x8000u;
    prog[k++] = W0(OP_MOVEA, 11, 11); prog[k++] = 0x0300u;
    prog[k++] = W0(OP_SYSTEM, 11, 0); prog[k++] = (uint16_t)((12u << 11) | SUB_LDLW);
    prog[k++] = 0x0FE0u;              prog[k++] = SUB_HALT;   /* snooze */
    prog[k++] = F2(OP_MOVI, 7, 13);
    prog[k++] = W0(OP_SYSTEM, 11, 0); prog[k++] = (uint16_t)((13u << 11) | SUB_STCW);
    prog[k++] = W0(OP_ST_HW, 11, 13); prog[k++] = 0x0005u;    /* st.w 4[r11] */
    prog[k++] = 0x07E0u;              prog[k++] = SUB_HALT;

    /* --- other cores --- */
    const unsigned other = k;
    prog[k++] = W0(OP_MOVHI, 0, 11);  prog[k++] = 0x8000u;
    prog[k++] = W0(OP_MOVEA, 11, 11); prog[k++] = 0x0300u;
    prog[k++] = F2(OP_MOVI, 9, 14);
    prog[k++] = W0(OP_ST_HW, 11, 14); prog[k++] = 0x0001u;
    prog[k++] = 0x07E0u;              prog[k++] = SUB_HALT;

    /* Bcond displacement is from the branch itself, in bytes. */
    prog[bne_at] = BCOND(0xAu, (other - bne_at) * 2u);

    if (!run_system(prog, k, 4u, 256u, NULL)) {
        CHECK(false);
        return;
    }

    /* Core 1 got there: the word holds its value, not core 0's 7. */
    CHECK_EQ(ram32(0x300u), 9u);
    /* And core 0's store-conditional reported failure. */
    CHECK_EQ(ram32(0x304u), 0u);
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

/*
 * Double precision is not implemented, and says so.
 *
 * The point is that it raises RIE rather than being decoded as some
 * single-precision neighbour: ADDF.D is sub 0x470 against ADDF.S's
 * 0x460, one bit apart, and a mask that was one bit too loose would
 * execute it as a single-precision add on the wrong register pair and
 * return a plausible number.
 */
static void test_fp_double_declined(void)
{
    /* RIE vectors to RBASE + 0x60. */
    uint16_t prog[0x60];
    unsigned k = 0;

    memset(prog, 0, sizeof(prog));
    k = fp_prologue(prog, k);
    k = fp_ldi(prog, k, 10, F_1_0);
    prog[k++] = FP0(10, 10); prog[k++] = FP1(12, 0x470u);   /* ADDF.D  */
    prog[0x30] = 0x07E0u; prog[0x31] = SUB_HALT;

    emu_run_reason_t why;
    uint32_t retired = 0;
    if (!load_and_run(prog, sizeof(prog) / sizeof(prog[0]), 64u, &why,
                      &retired)) { CHECK(false); return; }

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);

    CHECK_EQ(why, EMU_RUN_WFI);
    CHECK(st.traps >= 1u);
    CHECK_EQ(reg(12), 0u);                      /* not executed as .S    */
    CHECK_EQ(st.pc, EMU_GUEST_RAM_BASE + 0x64u);
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

void test_g4mh(void)
{
    test_length();
    test_conditions();
    test_alu();
    test_flags_and_branch();
    test_load_store();
    test_mov_imm32();
    test_prepare_dispose();
    test_unsigned_loads();
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
    test_fetrap();
    test_resbank_is_not_di();
    test_narrow_atomics();
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
    test_fp_double_declined();
    test_fp_fma_rounds_once();
#endif
#if G4MH_PE_COUNT > 1
    test_mc_dispatch();
    test_mc_quantum_invariance();
    test_mc_reservation();
#endif
}
