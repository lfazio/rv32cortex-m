/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_jit_x86_64.c - an x86-64 JIT for the RH850 G4MH frontend.
 *
 * The block management, code buffer and dispatch loop are emu_jit.c's; the
 * instruction encoding is emu_x86_64.c's. What is here is the only part
 * that is about RH850: which guest instructions can be translated, and
 * what each one means.
 *
 * The hard part of this ISA is not the arithmetic, it is PSW. Almost every
 * G4MH instruction writes Z, S, OV and CY, and an emulator that computes
 * those in C does four tests per instruction. x86 has the same four flags
 * for free in EFLAGS -- ZF, SF, OF and CF are exactly Z, S, OV and CY, and
 * CY is a borrow on subtract just as CF is -- so the translation is
 * worthwhile precisely where the interpreter is slowest.
 *
 * Capturing them is the fiddly bit, because everything that combines them
 * also destroys them. `seto` followed by `lahf` gets all four into one
 * register without a single flag-clobbering instruction in between: seto
 * puts OF in al, and lahf loads SF:ZF:x:AF:x:PF:x:CF into ah. Only then is
 * it safe to shift and mask.
 *
 * What is translated: the register-register and imm5 forms of MOV, the
 * logical ops, ADD, SUB, CMP and the shifts. Everything else -- loads,
 * stores, branches, TRAP, the system registers -- ends the block and goes
 * to the interpreter, which is the same place the RV32 backend started
 * from. Blocks of straight-line ALU work are what this buys.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_intc.h"

#include "emu/emu_jit.h"
#include "emu/emu_x86_64.h"

#if defined(RV_JIT_X86_64) || defined(EMU_JIT_X86_64)

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(g4mh_cpu_t, r) == 0,
               "translated code assumes the register file is at offset 0");

#define CPU_PC_OFF  ((uint32_t)offsetof(g4mh_cpu_t, pc))
#define CPU_PSW_OFF ((uint32_t)offsetof(g4mh_cpu_t, psw))

/* Bytes of code cache; a host has no reason to be stingy. */
#define G4MH_JIT_CODE_BYTES (4u * 1024u * 1024u)

/* Guest instructions per block, so one runaway block cannot fill the
 * buffer and so a pending interrupt is never far away. */
#define G4MH_JIT_MAX_BLOCK_INSNS 64u

/* ------------------------------------------------------------------ */
/* Guest registers                                                     */
/* ------------------------------------------------------------------ */

/*
 * r0 is hardwired zero and is materialised rather than loaded, so nothing
 * depends on the register file's slot 0 having been kept clear.
 */
static void ld_gpr(int dst, uint32_t r)
{
    if (r == 0u) {
        x86_mov_imm32(dst, 0u);
        return;
    }
    x86_ld_cpu(dst, r * 4u);
}

static void st_gpr(uint32_t r, int src)
{
    if (r == 0u) {
        return;                       /* writes to r0 vanish */
    }
    x86_st_cpu(src, r * 4u);
}

/* ------------------------------------------------------------------ */
/* PSW                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Turn the x86 flags left by the previous instruction into G4MH's four
 * PSW bits, and merge them into psw.
 *
 * Must be emitted *immediately* after the operation whose flags it wants:
 * seto and lahf are the only two instructions here that do not disturb
 * EFLAGS, which is why they come first and everything else after.
 *
 * Clobbers eax, ecx and edx -- and nothing else, which is load-bearing:
 * the caller stashes the operation's result in esi across this, and an
 * earlier version used esi as a scratch here. The block then stored the
 * flag word where the result belonged, and lram.asm caught it on the
 * first run.
 */
static void emit_flags_from_eflags(void)
{
    /* seto al -- OF, the one flag lahf does not carry. */
    emu_jit_emit8(0x0F);
    emu_jit_emit8(0x90);
    emu_jit_emit8(0xC0);
    /* lahf -- ah = SF:ZF:0:AF:0:PF:1:CF */
    emu_jit_emit8(0x9F);

    /* edx = the flag byte; al still holds OF. */
    x86_mov_rr(X86_EDX, X86_EAX);
    x86_shift_imm(X86_EDX, X86_SHR, 8u);

    /* ecx = Z, bit 0. */
    x86_mov_rr(X86_ECX, X86_EDX);
    x86_shift_imm(X86_ECX, X86_SHR, 6u);
    x86_and_imm8(X86_ECX, 1);

    /* OV, bit 2 -- taken while al is still OF and before eax is reused. */
    x86_and_imm8(X86_EAX, 1);
    x86_shift_imm(X86_EAX, X86_SHL, 2u);
    x86_alu_rr(X86_OR, X86_ECX, X86_EAX);

    /* S, bit 1. */
    x86_mov_rr(X86_EAX, X86_EDX);
    x86_shift_imm(X86_EAX, X86_SHR, 7u);
    x86_and_imm8(X86_EAX, 1);
    x86_shift_imm(X86_EAX, X86_SHL, 1u);
    x86_alu_rr(X86_OR, X86_ECX, X86_EAX);

    /* CY, bit 3. */
    x86_and_imm8(X86_EDX, 1);
    x86_shift_imm(X86_EDX, X86_SHL, 3u);
    x86_alu_rr(X86_OR, X86_ECX, X86_EDX);

    /* psw = (psw & ~FLAGS) | ecx */
    x86_ld_cpu(X86_EAX, CPU_PSW_OFF);
    x86_mov_imm32(X86_EDX, ~(uint32_t)G4MH_PSW_FLAGS);
    x86_alu_rr(X86_AND, X86_EAX, X86_EDX);
    x86_alu_rr(X86_OR, X86_EAX, X86_ECX);
    x86_st_cpu(X86_EAX, CPU_PSW_OFF);
}

/*
 * The logical operations are the cheap case: Z and S come from the result,
 * OV is defined as 0, and CY is left alone. Emitted with the result still
 * in `res`, whose own flags from the AND/OR/XOR are exactly what is
 * wanted -- x86 clears OF and CF on those, and G4MH wants OV clear, so
 * only CY has to be preserved by hand.
 */
static void emit_flags_logic(int res)
{
    /* ecx = Z|S, computed from the result rather than from EFLAGS, so this
     * is safe to emit after anything. */
    x86_alu_rr(X86_TEST, res, res);

    emu_jit_emit8(0x0F); emu_jit_emit8(0x94); emu_jit_emit8(0xC1); /* setz cl */
    x86_movzx8(X86_ECX, X86_ECX);

    x86_mov_rr(X86_EDX, res);
    x86_shift_imm(X86_EDX, X86_SHR, 31u);
    x86_shift_imm(X86_EDX, X86_SHL, 1u);
    x86_alu_rr(X86_OR, X86_ECX, X86_EDX);

    /* psw = (psw & ~(Z|S|OV)) | ecx  -- CY survives. */
    x86_ld_cpu(X86_EAX, CPU_PSW_OFF);
    x86_mov_imm32(X86_EDX,
                  ~(uint32_t)(G4MH_PSW_Z | G4MH_PSW_S | G4MH_PSW_OV));
    x86_alu_rr(X86_AND, X86_EAX, X86_EDX);
    x86_alu_rr(X86_OR, X86_EAX, X86_ECX);
    x86_st_cpu(X86_EAX, CPU_PSW_OFF);
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

typedef enum { XLAT_NO = 0, XLAT_OK } xlat_t;

/*
 * One 16-bit instruction. Only the forms whose whole effect is a register
 * write and a flag update are here; anything that can fault, branch or
 * touch a system register ends the block.
 */
static xlat_t translate_one(uint16_t w0)
{
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r2 = g4mh_reg2(w0);
    const uint32_t op = g4mh_op6(w0);

    switch (op) {
    case 0x00:                                  /* MOV reg1, reg2   */
        if (r1 == 0u || r2 == 0u) {
            /* reg2 == 0 is NOP, and reg1 == 0 shares the slot with other
             * encodings. Neither is worth a special case here. */
            return XLAT_NO;
        }
        ld_gpr(X86_EAX, r1);
        st_gpr(r2, X86_EAX);
        return XLAT_OK;

    case 0x08:                                  /* OR               */
    case 0x09:                                  /* XOR              */
    case 0x0A: {                                /* AND              */
        static const uint8_t alu[3] = { X86_OR, X86_XOR, X86_AND };

        ld_gpr(X86_EAX, r2);
        ld_gpr(X86_ECX, r1);
        x86_alu_rr(alu[op - 0x08u], X86_EAX, X86_ECX);
        /* Keep the result before the flag sequence clobbers eax. */
        x86_mov_rr(X86_ESI, X86_EAX);
        st_gpr(r2, X86_ESI);
        emit_flags_logic(X86_ESI);
        return XLAT_OK;
    }

    case 0x0B:                                  /* TST              */
        ld_gpr(X86_EAX, r2);
        ld_gpr(X86_ECX, r1);
        x86_alu_rr(X86_AND, X86_EAX, X86_ECX);
        x86_mov_rr(X86_ESI, X86_EAX);
        emit_flags_logic(X86_ESI);
        return XLAT_OK;

    case 0x0D:                                  /* SUB  r2 = r2 - r1 */
    case 0x0E: {                                /* ADD  r2 = r2 + r1 */
        ld_gpr(X86_EAX, r2);
        ld_gpr(X86_ECX, r1);
        x86_alu_rr((op == 0x0Du) ? X86_SUB : X86_ADD, X86_EAX, X86_ECX);
        /* The result has to be stashed before the flags are read, because
         * reading them clobbers eax -- and stashing must not itself touch
         * EFLAGS, which mov does not. */
        x86_mov_rr(X86_ESI, X86_EAX);
        emit_flags_from_eflags();
        st_gpr(r2, X86_ESI);
        return XLAT_OK;
    }

    case 0x0F:                                  /* CMP r2, r1       */
        ld_gpr(X86_EAX, r2);
        ld_gpr(X86_ECX, r1);
        x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
        emit_flags_from_eflags();
        return XLAT_OK;

    case 0x10:                                  /* MOV imm5, reg2   */
        if (r2 == 0u) {
            return XLAT_NO;                     /* CALLT shares the slot */
        }
        x86_mov_imm32(X86_EAX, (uint32_t)g4mh_imm5(w0));
        st_gpr(r2, X86_EAX);
        return XLAT_OK;

    case 0x12:                                  /* ADD imm5, reg2   */
        ld_gpr(X86_EAX, r2);
        x86_mov_imm32(X86_ECX, (uint32_t)g4mh_imm5(w0));
        x86_alu_rr(X86_ADD, X86_EAX, X86_ECX);
        x86_mov_rr(X86_ESI, X86_EAX);
        emit_flags_from_eflags();
        st_gpr(r2, X86_ESI);
        return XLAT_OK;

    case 0x13:                                  /* CMP imm5, reg2   */
        ld_gpr(X86_EAX, r2);
        x86_mov_imm32(X86_ECX, (uint32_t)g4mh_imm5(w0));
        x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
        emit_flags_from_eflags();
        return XLAT_OK;

    default:
        return XLAT_NO;
    }
}

static uint32_t g4mh_jit_translate(emu_cpu_t *cpu, uint32_t pc)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;
    uint32_t cur = pc;
    uint32_t count = 0u;

    x86_prologue();

    while (count < G4MH_JIT_MAX_BLOCK_INSNS && !emu_jit_overflowed()) {
        uint16_t w0;

        if (emu_bus_fetch16(c->bus, cur, &w0) != EMU_FAULT_NONE) {
            break;
        }
        /*
         * 32-bit and longer encodings are left alone. Their first
         * halfword is distinguishable, and translating one by accident as
         * a 16-bit instruction would be silent -- so the test is on the
         * form, not on whether the opcode happens to be recognised.
         */
        if (!g4mh_is_16bit(w0)) {
            break;
        }

        uint8_t *const before = emu_jit_here();
        if (translate_one(w0) == XLAT_NO) {
            emu_jit_rewind(before);
            break;
        }

        x86_count_one();
        cur += 2u;
        count++;

        /* pc after every instruction, so a trap taken by whatever runs
         * next sees the right address. */
        x86_mov_imm32(X86_EAX, cur);
        x86_st_cpu(X86_EAX, CPU_PC_OFF);
    }

    if (count == 0u) {
        return 0u;
    }
    x86_epilogue();
    return count;
}

/* ------------------------------------------------------------------ */
/* The framework's hooks                                               */
/* ------------------------------------------------------------------ */

static bool g4mh_jit_is_idle(emu_cpu_t *cpu)
{
    return ((g4mh_cpu_t *)cpu)->state == EMU_STATE_WFI;
}

/*
 * Waking is the interpreter's business: a parked G4MH core restarts only
 * when a channel is pending, and deciding that means walking the INTC.
 * Returning false here leaves the framework to report WFI, and the
 * interrupt path below is what actually restarts it.
 */
static bool g4mh_jit_wake(emu_cpu_t *cpu)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    if (g4mh_cpu_pending_irq(c) < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = true;
    return true;
}

static bool g4mh_jit_take_irq(emu_cpu_t *cpu)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    if (!c->irq_dirty) {
        return false;
    }
    /* Cleared before the evaluation, not after; see the interpreter. */
    c->irq_dirty = false;

    const int ch = g4mh_cpu_pending_irq(c);
    if (ch < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;
    g4mh_intc_ack(c->intc, (uint32_t)ch);
    g4mh_cpu_exception(c, G4MH_EXC_EIINT_BASE + (uint32_t)ch, c->pc);
    return true;
}

static void g4mh_jit_count(emu_cpu_t *cpu, uint32_t n)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    c->cycles += n;
    c->retired += n;
}

static uint8_t g4mh_jit_state(emu_cpu_t *cpu)
{
    return ((g4mh_cpu_t *)cpu)->state;
}

static uint32_t g4mh_jit_pc(emu_cpu_t *cpu)
{
    return ((g4mh_cpu_t *)cpu)->pc;
}

extern const emu_backend_t g4mh_backend_interp;

static const emu_jit_ops_t g_jit_ops = {
    .name       = "jit-x86-64",
    .translate  = g4mh_jit_translate,
    .may_run    = NULL,
    /*
     * Nothing is baked into a block: every instruction translated here
     * reads only its own operands, and anything that could change the
     * meaning of a later one -- a system register write, a mode change --
     * is not translated at all. The day one is, it needs a generation.
     */
    .generation = NULL,
    .interp     = &g4mh_backend_interp,
    .is_idle    = g4mh_jit_is_idle,
    .wake       = g4mh_jit_wake,
    .take_irq   = g4mh_jit_take_irq,
    .count      = g4mh_jit_count,
    .state      = g4mh_jit_state,
    .pc         = g4mh_jit_pc,
};

static bool jit_init(emu_cpu_t *cpu)
{
    (void)cpu;
    return emu_jit_init(G4MH_JIT_CODE_BYTES);
}

static void jit_reset(emu_cpu_t *cpu)
{
    (void)cpu;
    emu_jit_flush();
}

static void jit_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    (void)cpu;
    (void)addr;
    (void)len;
    emu_jit_flush();
}

static emu_run_reason_t jit_run(emu_cpu_t *cpu, uint32_t budget,
                                uint32_t *retired)
{
    return emu_jit_run(cpu, budget, retired, &g_jit_ops);
}

const emu_backend_t g4mh_backend_jit = {
    .name       = "jit-x86-64",
    .init       = jit_init,
    .reset      = jit_reset,
    .run        = jit_run,
    .invalidate = jit_invalidate,
};

#endif /* x86-64 */
