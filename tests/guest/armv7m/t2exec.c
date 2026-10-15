/* SPDX-License-Identifier: Apache-2.0 */
/*
 * t2exec.c - the Thumb-2 emitters, emitting into RAM and then *running*
 * what they wrote.
 *
 * **This is the test the Thumb-2 backend has never had.** Its bugs have
 * all needed a board, a flash cycle and a UART to find, and three of
 * them were live for months because no host suite could reach them --
 * the two this file's header list names are a 16-bit CMP that assembled
 * as a different instruction because a register did not fit the field,
 * and an imm5 shift where `LSR #0` means `LSR #32`. Neither computed a
 * wrong answer at the point of the mistake; both produced a working
 * program that did something else.
 *
 * `scripts/t2-check-encodings.sh` compares emitted bytes against
 * arm-none-eabi-as, which catches a wrong *encoding*. It cannot catch a
 * wrong *choice* of encoding -- an emitter that assembles perfectly and
 * is the wrong instruction for the operands it was given. Only executing
 * the bytes does that, and until now executing them meant hardware.
 *
 * Here the guest is a Cortex-M running on the ARMv7-M frontend, so the
 * emitters run as the backend compiles them, write into guest RAM, and
 * the guest branches to the result. What the frontend then decodes is
 * exactly what a real core would fetch.
 *
 * **What this does not test**: the IR, the register allocator, the block
 * cache, or any decision about *which* instruction to emit. It is the
 * encoder, end to end. That is the layer whose mistakes are silent.
 */

#include "emu/emu_thumb2.h"

#include <stdint.h>

#define UART (*(volatile unsigned char *)0x10000000u)

static void pc_(char c)
{
    UART = (unsigned char)c;
}
static void ps_(const char *s)
{
    while (*s != '\0') {
        pc_(*s++);
    }
}
static void phex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static unsigned g_fail;

static void want(const char *tag, uint32_t got, uint32_t expect)
{
    if (got != expect) {
        g_fail++;
        ps_("FAIL ");
        ps_(tag);
        ps_(" got ");
        phex(got);
        ps_(" want ");
        phex(expect);
        pc_('\n');
    }
}

/*
 * What the emitters write through. These are the backend's own globals,
 * declared in emu_jit.h and defined by emu_jit.c -- which is not part of
 * this guest, so they are defined here.
 */
uint8_t *emu_jit_cursor;
uint8_t *emu_jit_limit;
bool emu_jit_overflow;

/*
 * The cache maintenance t2_sync_code reaches for.
 *
 * A no-op is correct *here* and would not be on a Cortex-M7: this guest
 * runs on an emulator whose instruction fetch reads the same memory the
 * stores went to, so there is no stale line to invalidate. On real
 * silicon this is a clean-to-PoU and an invalidate-by-address, and
 * getting it wrong there executes arbitrary bytes rather than producing
 * a wrong answer. The frontend has no caches to model, so the absence of
 * maintenance is not something this test can see -- which is worth
 * saying rather than leaving as an apparent pass.
 */
void board_sync_icache(const void *addr, uint32_t len);
void board_sync_icache(const void *addr, uint32_t len)
{
    (void)addr;
    (void)len;
}

/*
 * Where emitted code goes. In .bss, so it is in RAM and writable --
 * the guest's .text is served from a read-only window.
 *
 * 8-aligned because a branch target wants to be, and the low bit is set
 * when calling: Cortex-M selects its instruction set with that bit, and
 * a Thumb-only core takes a UsageFault rather than executing an
 * ARM-state instruction. The framework does this in emu_jit.c for the
 * same reason.
 */
static uint8_t g_code[512] __attribute__((aligned(8)));

typedef uint32_t (*fn1_t)(uint32_t);
typedef uint32_t (*fn2_t)(uint32_t, uint32_t);
typedef uint32_t (*fn3_t)(uint32_t, uint32_t, uint32_t);

/*
 * A block begins by pushing lr and ends by popping it into pc, which is
 * how the backend's own prologue and epilogue are shaped -- there is no
 * `bx lr` emitter because a translated block never needs one.
 */
static void emit_begin(void)
{
    emu_jit_cursor = g_code;
    emu_jit_limit = g_code + sizeof(g_code);
    emu_jit_overflow = false;
    t2_push(T2_LIST_LR);
}

static void *emit_end(void)
{
    t2_pop(T2_LIST_PC);
    t2_sync_code(g_code, (uint32_t)(emu_jit_cursor - g_code));
    /* The Thumb bit. Without it the call faults rather than running. */
    return (void *)(uintptr_t)((uintptr_t)g_code | 1u);
}

int main(void)
{
    ps_("T2EXEC-START\n");

    /*
     * A move and a return: the smallest thing that proves emitted code
     * is reachable at all. If this fails nothing below means anything.
     */
    emit_begin();
    t2_mov(0u, 1u); /* r0 = r1 */
    {
        const fn2_t f = (fn2_t)emit_end();

        want("mov-reg", f(0u, 0x1234u), 0x1234u);
    }

    /*
     * **The shift-by-zero boundary**, which is the defect this file's
     * header names: `imm5 == 0` does not mean "no shift" for three of
     * the four types. LSR #0 *is* LSR #32, ASR #0 is ASR #32, and ROR #0
     * is RRX. A RISC-V `srli rd, rs, 0` means a move, and emitting it as
     * a shift by zero gave zero or a sign extension.
     *
     * Checked by executing, which is the only way: the encoding for
     * `LSR #0` is perfectly valid and the assembler agrees with it.
     */
    emit_begin();
    t2_shift_imm(T2_LSR, 0u, 1u, 0u); /* r0 = r1 >> 0 */
    {
        const fn2_t f = (fn2_t)emit_end();

        want("lsr-0-is-a-move", f(0u, 0xDEADBEEFu), 0xDEADBEEFu);
    }

    emit_begin();
    t2_shift_imm(T2_ASR, 0u, 1u, 0u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("asr-0-is-a-move", f(0u, 0x80000001u), 0x80000001u);
    }

    /* ...and a shift that really is by an amount. */
    emit_begin();
    t2_shift_imm(T2_LSR, 0u, 1u, 4u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("lsr-4", f(0u, 0xDEADBEEFu), 0x0DEADBEEu);
    }

    emit_begin();
    t2_shift_imm(T2_ASR, 0u, 1u, 4u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("asr-4", f(0u, 0x80000000u), 0xF8000000u);
    }

    /*
     * The ALU, with both operands and the destination named -- Thumb-2
     * is three-address, which is the property that made computing into
     * an allocated register pay where routing through the frame did not.
     */
    emit_begin();
    t2_add(0u, 0u, 1u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("add-reg", f(7u, 11u), 18u);
    }

    emit_begin();
    t2_sub(0u, 0u, 1u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("sub-reg", f(30u, 11u), 19u);
    }

    /*
     * **A high register, which is the other defect in the list.**
     * `emit_dp_reg(DP_CMP, R8, R1)` set a bit belonging to `rm` and
     * assembled as `CMP r0, r1` -- so a loop cap never applied and
     * chained loops ran unbounded. Nothing computed a wrong answer.
     *
     * Moving through r8 and back is the executable form of that check:
     * an emitter that silently used a different register returns the
     * wrong value rather than assembling wrongly.
     */
    /*
     * r8 is callee-saved, so it is pushed and popped with lr -- the
     * emitted block has to honour the ABI of the C function calling it,
     * exactly as a translated block honours the run loop's. The
     * framework's note about a uniform save set across chained exits is
     * the same constraint seen from the other end.
     */
    emu_jit_cursor = g_code;
    emu_jit_limit = g_code + sizeof(g_code);
    emu_jit_overflow = false;
    t2_push((1u << 8) | T2_LIST_LR);
    t2_mov(8u, 1u); /* r8 = r1  */
    t2_mov(0u, 8u); /* r0 = r8  */
    t2_pop((1u << 8) | T2_LIST_PC);
    t2_sync_code(g_code, (uint32_t)(emu_jit_cursor - g_code));
    {
        const fn2_t f = (fn2_t)(void *)(uintptr_t)((uintptr_t)g_code | 1u);

        want("high-register-round-trip", f(0u, 0xABCDEF01u), 0xABCDEF01u);
    }

    /* A 32-bit immediate, which needs MOVW and MOVT. */
    emit_begin();
    t2_imm32(0u, 0xCAFEBABEu);
    {
        const fn1_t f = (fn1_t)emit_end();

        want("mov-imm32", f(0u), 0xCAFEBABEu);
    }

    /*
     * **The 16-bit forms.** MOV, CMP and the frame and register-file
     * loads and stores were always the 32-bit encodings, and code size is
     * what the board's cache pays for. Each case checks the answer *and*
     * the length: the answer says the encoding is the right instruction
     * for those registers, the length says the short form is the one
     * chosen -- without it, falling back to the 32-bit form would pass.
     *
     * CMP is the one to be careful with. T1 holds two low registers in
     * three-bit fields and T2 any pair, UNPREDICTABLE when both are low;
     * a high register in T1 is the defect named above. The pairings below
     * are chosen so a compare of the wrong registers gives other flags.
     */
    {
        static const struct {
            const char *tag;
            uint32_t rn, rm; /* 12 and 14 hold the arguments' copies */
            uint32_t len;
            uint32_t nzcv; /* for f(5, 2) */
        } k_cmp[] = {
            {"cmp-lo-lo", 0u, 1u, 2u, 0x2u},   /* 5 - 2: C          */
            {"cmp-hi-lo", 12u, 0u, 2u, 0x8u},  /* 2 - 5: N          */
            {"cmp-lo-hi", 0u, 12u, 2u, 0x2u},  /* 5 - 2: C          */
            {"cmp-hi-hi", 12u, 14u, 2u, 0x8u}, /* 2 - 5: N          */
        };

        for (unsigned i = 0; i < sizeof(k_cmp) / sizeof(k_cmp[0]); i++) {
            emit_begin();
            t2_mov(12u, 1u); /* r12 = b */
            t2_mov(14u, 0u); /* lr = a; the real one is on the stack */
            const uint8_t *const at = emu_jit_cursor;

            t2_cmp(k_cmp[i].rn, k_cmp[i].rm);
            want(k_cmp[i].tag, (uint32_t)(emu_jit_cursor - at), k_cmp[i].len);
            t2_mrs_apsr(0u);
            t2_shift_imm(T2_LSR, 0u, 0u, 28u);
            {
                const fn2_t f = (fn2_t)emit_end();

                want(k_cmp[i].tag, f(5u, 2u), k_cmp[i].nzcv);
            }
        }
    }

    /* CMP #imm: 16 bits for a low register, CMP.W for a high one. */
    emit_begin();
    {
        const uint8_t *const at = emu_jit_cursor;

        (void)t2_cmp_imm8(0u, 5u);
        want("cmp-imm-lo-len", (uint32_t)(emu_jit_cursor - at), 2u);
    }
    t2_mrs_apsr(0u);
    t2_shift_imm(T2_LSR, 0u, 0u, 28u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("cmp-imm-lo", f(5u, 0u), 0x6u); /* 5 - 5: Z, C */
    }
    emit_begin();
    t2_mov(12u, 1u);
    {
        const uint8_t *const at = emu_jit_cursor;

        (void)t2_cmp_imm8(12u, 5u);
        want("cmp-imm-hi-len", (uint32_t)(emu_jit_cursor - at), 4u);
    }
    t2_mrs_apsr(0u);
    t2_shift_imm(T2_LSR, 0u, 0u, 28u);
    {
        const fn2_t f = (fn2_t)emit_end();

        want("cmp-imm-hi", f(0u, 2u), 0x8u); /* 2 - 5: N */
    }

    /* MOV: 16 bits for any pair but SP and PC, and no flags. */
    emit_begin();
    {
        const uint8_t *const at = emu_jit_cursor;

        t2_mov(12u, 1u);
        t2_mov(0u, 12u);
        want("mov-short-len", (uint32_t)(emu_jit_cursor - at), 4u);
    }
    {
        const fn2_t f = (fn2_t)emit_end();

        want("mov-through-r12", f(0u, 0x13579BDFu), 0x13579BDFu);
    }
    emit_begin();
    {
        const uint8_t *const at = emu_jit_cursor;

        t2_mov(0u, 13u); /* r0 = sp */
        want("mov-sp-len", (uint32_t)(emu_jit_cursor - at), 4u);
    }
    {
        const fn2_t f = (fn2_t)emit_end();

        want("mov-sp-aligned", f(0u, 0u) & 3u, 0u);
    }

    /*
     * LDR/STR: [rn, #imm5*4] for a low base up to 124, [sp, #imm8*4] up
     * to 1020, and the .W form past either or for a high rt.
     */
    {
        static uint32_t arr[40];

        for (unsigned i = 0; i < 40u; i++) {
            arr[i] = 0xA0000000u + i;
        }

        static const struct {
            const char *tag;
            uint32_t rt, off, len;
        } k_ld[] = {
            {"ldr-lo-8", 0u, 8u, 2u},
            {"ldr-lo-124", 0u, 124u, 2u},
            {"ldr-lo-128", 0u, 128u, 4u},
            {"ldr-hi-8", 12u, 8u, 4u},
        };

        for (unsigned i = 0; i < sizeof(k_ld) / sizeof(k_ld[0]); i++) {
            emit_begin();
            const uint8_t *const at = emu_jit_cursor;

            t2_ldr_imm(k_ld[i].rt, 1u, k_ld[i].off);
            want(k_ld[i].tag, (uint32_t)(emu_jit_cursor - at), k_ld[i].len);
            if (k_ld[i].rt != 0u) {
                t2_mov(0u, k_ld[i].rt);
            }
            {
                const fn2_t f = (fn2_t)emit_end();

                want(k_ld[i].tag, f(0u, (uint32_t)(uintptr_t)arr),
                     0xA0000000u + k_ld[i].off / 4u);
            }
        }

        emit_begin();
        {
            const uint8_t *const at = emu_jit_cursor;

            t2_str_imm(0u, 1u, 12u);
            want("str-lo-12-len", (uint32_t)(emu_jit_cursor - at), 2u);
        }
        {
            const fn2_t f = (fn2_t)emit_end();

            (void)f(0x600DF00Du, (uint32_t)(uintptr_t)arr);
            want("str-lo-12", arr[3], 0x600DF00Du);
            want("str-lo-12-only", arr[2] + arr[4],
                 (0xA0000000u + 2u) + (0xA0000000u + 4u));
        }
    }

    /*
     * [sp, #imm8*4]: push r1 and r2 under lr, then read the second word
     * of the frame, which must be r2 -- a wrong base or scale reads lr
     * or r1 instead.
     */
    emu_jit_cursor = g_code;
    emu_jit_limit = g_code + sizeof(g_code);
    emu_jit_overflow = false;
    t2_push((1u << 1) | (1u << 2) | T2_LIST_LR);
    {
        const uint8_t *const at = emu_jit_cursor;

        t2_ldr_imm(0u, 13u, 4u);
        want("ldr-sp-4-len", (uint32_t)(emu_jit_cursor - at), 2u);
    }
    t2_pop((1u << 1) | (1u << 2) | T2_LIST_PC);
    t2_sync_code(g_code, (uint32_t)(emu_jit_cursor - g_code));
    {
        const fn3_t f = (fn3_t)(void *)(uintptr_t)((uintptr_t)g_code | 1u);

        want("ldr-sp-4", f(0u, 0x11111111u, 0x22222222u), 0x22222222u);
    }

    /*
     * **The window guard**, built from the same encoders in the same
     * order as emit_fast_guard: subtract the base as a modified
     * immediate, rotate right by log2(size), compare with size >> k as
     * another, and take the 16-bit HS branch to the slow path. The
     * rotation is what folds the alignment test into the bounds test, so
     * the cases are the edges of both: the first and last aligned slots,
     * one past the end, below the base, and every misalignment.
     */
    {
        static const uint32_t base = 0x80000000u, size = 0x29000u;
        static const struct {
            uint32_t sz, off, slow;
        } k_g[] = {
            {4u, 0u, 0u},          {4u, 4u, 0u},
            {4u, size - 4u, 0u},   {4u, size, 1u},
            {4u, size - 2u, 1u},   {4u, 1u, 1u},
            {4u, 2u, 1u},          {4u, 3u, 1u},
            {4u, 0xFFFFFFFCu, 1u}, /* base - 4 */
            {2u, 0u, 0u},          {2u, size - 2u, 0u},
            {2u, size, 1u},        {2u, 1u, 1u},
            {2u, 0xFFFFFFFEu, 1u}, {1u, 0u, 0u},
            {1u, size - 1u, 0u},   {1u, size, 1u},
            {1u, 0xFFFFFFFFu, 1u},
        };

        for (unsigned i = 0; i < sizeof(k_g) / sizeof(k_g[0]); i++) {
            const uint32_t sz = k_g[i].sz;
            const uint32_t k = (sz == 4u) ? 2u : (sz == 2u) ? 1u : 0u;
            uint16_t enc;

            emit_begin();
            (void)t2_expand_imm(base, &enc);
            t2_sub_imm(2u, 0u, enc); /* r2 = addr - base */
            if (k != 0u) {
                t2_shift_imm(T2_ROR, 2u, 2u, k);
            }
            (void)t2_expand_imm(size >> k, &enc);
            t2_cmp_imm(2u, enc);
            uint8_t *const slow = t2_bcond_forward_n(0x2u); /* HS */

            (void)t2_movs_imm8(0u, 0u);
            uint8_t *const done = t2_b_forward_n();

            want("guard-patch-slow",
                 t2_patch_branch_n(slow, emu_jit_cursor, true) ? 1u : 0u, 1u);
            (void)t2_movs_imm8(0u, 1u);
            want("guard-patch-done",
                 t2_patch_branch_n(done, emu_jit_cursor, false) ? 1u : 0u,
                 1u);
            {
                const fn1_t f = (fn1_t)emit_end();

                if (f(base + k_g[i].off) != k_g[i].slow) {
                    g_fail++;
                    ps_("FAIL guard sz ");
                    phex(sz);
                    ps_(" off ");
                    phex(k_g[i].off);
                    pc_('\n');
                }
            }
        }
    }

    /* The access that undoes the rotation: [r12, r2, LSL #k]. */
    {
        static uint32_t w[8] = {0x10u, 0x11u, 0x12u, 0x13u,
                                0x14u, 0x15u, 0x16u, 0x17u};
        static uint16_t h[8] = {0x20u, 0x21u, 0x22u, 0x23u,
                                0x24u, 0x25u, 0x26u, 0x27u};

        emit_begin();
        t2_mov(12u, 0u);
        t2_mov(2u, 1u);
        (void)t2_ld_reg_lsl(0u, 12u, 2u, 2u, 4u, false);
        {
            const fn2_t f = (fn2_t)emit_end();

            want("ldr-lsl-2", f((uint32_t)(uintptr_t)w, 5u), 0x15u);
        }
        emit_begin();
        t2_mov(12u, 0u);
        t2_mov(2u, 1u);
        (void)t2_ld_reg_lsl(0u, 12u, 2u, 1u, 2u, false);
        {
            const fn2_t f = (fn2_t)emit_end();

            want("ldrh-lsl-1", f((uint32_t)(uintptr_t)h, 6u), 0x26u);
        }
        emit_begin();
        t2_mov(12u, 1u);
        t2_mov(2u, 2u);
        (void)t2_st_reg_lsl(0u, 12u, 2u, 2u, 4u);
        {
            const fn3_t f = (fn3_t)emit_end();

            (void)f(0xFEEDu, (uint32_t)(uintptr_t)w, 3u);
            want("str-lsl-2", w[3], 0xFEEDu);
            want("str-lsl-2-only", w[2] + w[4], 0x12u + 0x14u);
        }
    }

    /*
     * LDRB: the store guard is one byte, and a word load of it reads the
     * three after it too. The neighbours are set here so a word load, or
     * the wrong byte, gives another answer.
     */
    {
        static uint8_t flags[160];

        for (unsigned i = 0; i < sizeof(flags); i++) {
            flags[i] = 0xEEu;
        }
        flags[31] = 0x00u;
        flags[100] = 0x5Au;

        emit_begin();
        {
            const uint8_t *const at = emu_jit_cursor;

            t2_ldrb_imm(0u, 1u, 31u);
            want("ldrb-31-len", (uint32_t)(emu_jit_cursor - at), 2u);
        }
        {
            const fn2_t f = (fn2_t)emit_end();

            want("ldrb-31", f(0u, (uint32_t)(uintptr_t)flags), 0x00u);
        }
        emit_begin();
        {
            const uint8_t *const at = emu_jit_cursor;

            t2_ldrb_imm(0u, 1u, 100u);
            want("ldrb-100-len", (uint32_t)(emu_jit_cursor - at), 4u);
        }
        {
            const fn2_t f = (fn2_t)emit_end();

            want("ldrb-100", f(0u, (uint32_t)(uintptr_t)flags), 0x5Au);
        }
    }

    /* SUB/ADD SP, #imm7*4 and ADD rd, SP, #imm8*4, by what sp becomes. */
    emit_begin();
    t2_mov(1u, 13u);
    want("sp-sub-16", t2_sp_adjust(true, 40u) ? 1u : 0u, 1u);
    (void)t2_add_sp_imm(2u, 40u); /* r2 = sp + 40 = the old sp */
    want("sp-add-16", t2_sp_adjust(false, 40u) ? 1u : 0u, 1u);
    t2_sub(0u, 1u, 2u); /* 0 if both agree */
    {
        const fn1_t f = (fn1_t)emit_end();

        want("sp-adjust-round-trip", f(0u), 0u);
    }
    want("sp-adjust-limit", t2_sp_adjust(true, 512u) ? 1u : 0u, 0u);

    /*
     * LDR.W (literal), loaded from each halfword alignment: Align(pc, 4)
     * rounds the two differently, and a patch that ignored it would load
     * the halfword pair beside the literal. A second word follows the
     * first so that an offset one word out loads something recognisable.
     */
    for (unsigned pad = 0; pad < 2u; pad++) {
        const char *const tag =
            pad ? "ldr-literal-at-0mod4" : "ldr-literal-at-2mod4";
        uint8_t *site;
        uint8_t *lit;

        emit_begin(); /* a 16-bit PUSH: the cursor is 2 mod 4 */
        if (pad != 0u) {
            t2_emit16(0xBF00u);
        }
        site = t2_ldr_literal(0u);
        t2_pop(T2_LIST_PC);
        if (((uintptr_t)emu_jit_cursor & 3u) != 0u) {
            t2_emit16(0xBF00u);
        }
        lit = emu_jit_cursor;
        t2_emit16(0x5678u);
        t2_emit16(0x1234u);
        t2_emit16(0xDEF0u);
        t2_emit16(0x9ABCu);
        want(tag, (uint32_t)((uintptr_t)site & 3u), pad ? 0u : 2u);
        want(tag, t2_patch_literal(site, lit) ? 1u : 0u, 1u);
        t2_sync_code(g_code, (uint32_t)(emu_jit_cursor - g_code));
        {
            const fn2_t f = (fn2_t)(void *)(uintptr_t)((uintptr_t)g_code | 1u);

            want(tag, f(0u, 0u), 0x12345678u);
        }
    }

    /* Refused rather than truncated: behind, past 4095, and unaligned. */
    emit_begin();
    {
        uint8_t *const site = t2_ldr_literal(0u);
        uint8_t *const pc =
            (uint8_t *)(((uintptr_t)site + 4u) & ~(uintptr_t)3u);

        want("ldr-literal-behind", t2_patch_literal(site, g_code) ? 1u : 0u,
             0u);
        want("ldr-literal-4092", t2_patch_literal(site, pc + 4092) ? 1u : 0u,
             1u);
        want("ldr-literal-4096", t2_patch_literal(site, pc + 4096) ? 1u : 0u,
             0u);
        want("ldr-literal-unaligned", t2_patch_literal(site, pc + 6) ? 1u : 0u,
             0u);
    }

    /* And the emitter must not have overflowed on any of that. */
    want("no-overflow", emu_jit_overflow ? 1u : 0u, 0u);

    if (g_fail == 0u) {
        ps_("T2EXEC-OK\n");
    }
    return (int)g_fail;
}
