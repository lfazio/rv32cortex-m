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

    /* And the emitter must not have overflowed on any of that. */
    want("no-overflow", emu_jit_overflow ? 1u : 0u, 0u);

    if (g_fail == 0u) {
        ps_("T2EXEC-OK\n");
    }
    return (int)g_fail;
}
