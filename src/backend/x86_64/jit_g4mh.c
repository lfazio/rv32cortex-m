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
#include "g4mh/g4mh_ir.h"
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
/* PSW                                                                 */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

typedef enum { XLAT_NO = 0, XLAT_OK } xlat_t;

/*
 * One 16-bit instruction. Only the forms whose whole effect is a register
 * write and a flag update are here; anything that can fault, branch or
 * touch a system register ends the block.
 */

/*
 * Guest -> IR -> optimise -> host, which is all that is left of this
 * translator.
 *
 * Everything that used to be here -- the per-opcode switch, the operand
 * loads, the flag materialisation -- moved either into the frontend
 * (src/frontend/g4mh/g4mh_ir.c, which names no host) or into the shared
 * lowering (ir_lower.c, which names no guest). What remains is the glue,
 * and it is the same three lines any frontend/host pair needs, which is
 * the point: when RV32 emits IR too, this file and jit_rv32.c become one
 * jit.c for this host.
 *
 * live_out is EMU_IR_F_ALL. The block ends where the frontend stopped
 * lowering, and whatever runs next -- the interpreter, a trap handler,
 * the next block -- may read any flag, so none of them are dead at the
 * boundary. Narrowing this would need the frontend to prove what the
 * following instruction reads, and being wrong deletes a flag definition
 * something depends on.
 */
static emu_ir_block_t g_ir;

static uint32_t g4mh_jit_translate(emu_cpu_t *cpu, uint32_t pc)
{
    const uint32_t n = g4mh_ir_translate(cpu, pc, &g_ir);

    if (n == 0u) {
        return 0u;
    }

    emu_ir_optimise(&g_ir, EMU_IR_F_ALL, NULL);

    x86_prologue();
    if (!emu_ir_lower(&g_ir, &g4mh_ir_target)) {
        return 0u;
    }
    x86_epilogue();
    return n;
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

extern const emu_backend_t g4mh_backend_interp;

/*
 * generation stays NULL: nothing is baked into a block. Every instruction
 * translated here reads only its own operands, and anything that could
 * change the meaning of a later one -- a system register write, a mode
 * change -- is not translated at all. The day one is, it needs a
 * generation.
 */
static void g4mh_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    out->pc = &c->pc;
    out->state = &c->state;
}

static const emu_jit_ops_t g_jit_ops = {
    .name       = "jit-x86-64",
    .bind       = g4mh_jit_bind,
    .translate  = g4mh_jit_translate,
    /*
     * Relocatable: every branch inside a block is a rel32 whose ends move
     * together, and every address that is not -- helpers, guest pc -- is
     * an absolute immediate. x86 needs no sync, its caches being coherent
     * with instruction fetch.
     */
    .relocatable = true,
    .sync       = NULL,
    .interp     = &g4mh_backend_interp,
    .is_idle    = g4mh_jit_is_idle,
    .wake       = g4mh_jit_wake,
    .take_irq   = g4mh_jit_take_irq,
    .count      = g4mh_jit_count,
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
