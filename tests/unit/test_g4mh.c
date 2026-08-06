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
    CHECK(g4mh_insn_is_48(W0(0x37u, 9, 0), 0x0000u));

    /*
     * The 0x3C/0x3D slot holds JR disp22 (32-bit), PREPARE and the disp23
     * loads (48-bit), and only bit 0 of the second halfword separates
     * them -- JR's displacement is even.
     */
    CHECK(!g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0100u));  /* JR    */
    CHECK(g4mh_insn_is_48(W0(0x3Cu, 0, 0), 0x0101u));   /* disp23 */
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
     * land in the handler, not silently retire. 0x3E is the Format VIII
     * bit-manipulation group.
     *
     * RBASE defaults to the reset pc, so the RIE handler is at
     * reset + 0x60; a HALT is planted there to prove control arrived.
     */
    uint16_t prog[0x40];
    memset(prog, 0, sizeof(prog));
    prog[0] = W0(0x3Eu, 0, 0);
    prog[1] = 0x0000u;
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

void test_g4mh(void)
{
    test_length();
    test_conditions();
    test_alu();
    test_flags_and_branch();
    test_load_store();
    test_mov_imm32();
    test_muldiv();
    test_system_registers();
    test_reserved_instruction();
    test_trap_and_syscall();
    test_contract();
    test_mc_caxi();
    test_mc_reservation_succeeds();
#if G4MH_PE_COUNT > 1
    test_mc_dispatch();
    test_mc_quantum_invariance();
    test_mc_reservation();
#endif
}
