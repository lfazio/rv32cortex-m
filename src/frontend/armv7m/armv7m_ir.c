/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_ir.c - ARMv7-M (Thumb-2) to IR.
 *
 * The frontend half of frontend -> IR -> optimisation -> backend. It
 * knows the guest and nothing about any host.
 *
 * Three outcomes per instruction, and they are counted
 * ----------------------------------------------------
 *   lowered    turned into IR, and from there into host code
 *   helper     a call to the interpreter for this one instruction, from
 *              inside the block -- "a helper call is a translation;
 *              declining is not", which this project measured before
 *              this file existed
 *   declined   the block ends in front of it and the interpreter takes
 *              over: the system instructions, and anything that changes
 *              what a block may assume
 *
 * The helper is armv7m_step_insn, which is the interpreter's own
 * per-instruction core. So an instruction this file does not lower is
 * still executed by the implementation that was checked against a
 * Cortex-M7, and the question a new lowering has to answer is only ever
 * "is this faster", never "is this the same".
 *
 * Flags
 * -----
 * N, Z, C and V are ordinary IR values, held while translated code runs
 * as four guest "registers" -- one word each, read lazily; see jit_nf in
 * armv7m_cpu.h. EMU_IR_SETF is not used: its flag model is the first
 * guest's, where a subtract's C is a *borrow* and a logical operation
 * clears V and C. ARM's C is the inverse of a borrow, and a logical
 * operation leaves V alone and takes C from the shifter -- so the shared
 * flag sources would be wrong in exactly the cases a compiler relies on
 * (`cmp; bhs`, `adds; adcs`).
 *
 * One word each rather than four bits of xPSR, because of who can look.
 * A load can fault, and a fault stacks xPSR, so the flags have to be
 * *recoverable* before every load -- and the first version of this file
 * made them recoverable by packing all four into xPSR each time, which
 * is seventeen operations. Kept apart, N and Z are the result stored
 * twice, and the packing is done by ir_fault, on the path that actually
 * needs it. CoreMark went from 3.0 times the interpreter to the figure
 * in docs/frontend/armv7m.md.
 *
 * The cost is a second representation with a rule about which is
 * current: jit_split, and armv7m_flags_pack / _unpack at every crossing
 * between translated code and C that reads flags. They are all in this
 * file but one -- the frontend's run wrapper packs on the way out.
 *
 * What a block may assume
 * -----------------------
 * A block is built for one value of armv7m_jit_ctx -- Thumb state, not
 * inside an IT block, a mode and a privilege -- and is only entered
 * under it. Three things follow, and each is a rule this file keeps:
 *
 *   - an IT block is lowered whole or not at all, so a block never
 *     begins or ends inside one;
 *   - an instruction that can change the context either ends the block
 *     through the dispatcher or is declined;
 *   - whoever changes the context from inside a block -- a fault taken
 *     by a helper, an interworking branch -- rewrites jit_ctx before the
 *     dispatcher looks at it. See ir_fault and exit_interwork.
 */

#include "armv7m/armv7m_ir.h"

#include "armv7m/armv7m_backend.h"
#include "armv7m/armv7m_cpu.h"
#include "armv7m/armv7m_decode.h"

#include "emu/emu_ir.h"

#include <stddef.h>
#include <string.h>

#if EMU_HAVE_JIT

/*
 * Guest instructions per block. Interrupts and SysTick are delivered
 * between blocks, so this is an interrupt-latency bound as much as a
 * size one.
 */
#define A7_MAX_BLOCK_INSNS 64u

/*
 * Room an instruction may need in the IR. A block that overflows is
 * discarded and its first instruction interpreted for ever, so the
 * translator stops while there is still space for the widest lowering
 * rather than finding out afterwards -- which matters on a Thumb-2 host,
 * where a block is 512 IR instructions and a flag-setting ARM
 * instruction is twenty of them.
 */
#define A7_IR_HEADROOM 120u
#define A7_TEMP_HEADROOM 120u

/* Guest "registers" beyond r0-r15. */
#define R_PC 15u
#define R_XPSR 16u
#define R_CTX 17u /* the low word of jit_ctx */
#define R_NF 18u  /* N is bit 31                 */
#define R_ZF 19u  /* Z is "this word is zero"    */
#define R_CF 20u  /* C, 0 or 1                   */
#define R_VF 21u  /* V is bit 31                 */

/* A frontend-private bit in a LOAD or STORE's spec: MemA, not MemU. */
#define A7_MEM_ALIGNED (1u << 4)

#define NOT EMU_IR_NO_TEMP

typedef emu_ir_block_t B;
typedef armv7m_insn_t I;
typedef armv7m_exc_t X;

static armv7m_ir_stats_t g_stats;

const armv7m_ir_stats_t *armv7m_ir_get_stats(void)
{
    return &g_stats;
}

/* ------------------------------------------------------------------ */
/* What the lowered code calls                                         */
/* ------------------------------------------------------------------ */

/*
 * A fault from inside a block. pc already names the instruction: the
 * translator writes it after every instruction, so it is this one's
 * address until this one has finished.
 *
 * **jit_ctx is rewritten here**, because taking the exception has just
 * changed the mode, and the dispatcher's next lookup is keyed on it. The
 * RV32 frontend learned this as "a trap inside a translated block moves
 * the JIT's context".
 */
static uint32_t ir_fault(armv7m_cpu_t *c, X x)
{
    const uint32_t pc = c->r[R_PC];
    uint16_t w0 = 0u;
    uint16_t w1 = 0u;

    c->fault_pc = pc;
    if (armv7m_fetch_probe(c, pc, &w0)) {
        if (armv7m_is_32bit(w0) && armv7m_fetch_probe(c, pc + 2u, &w1)) {
            c->fault_insn = ((uint32_t)w0 << 16) | w1;
        } else {
            c->fault_insn = w0;
        }
    }
    /* The exception stacks xPSR: this is the reader the flags are kept
     * recoverable for. */
    armv7m_flags_pack(c);
    armv7m_raise(c, x, pc);
    armv7m_flags_unpack(c);
    c->jit_ctx = armv7m_jit_ctx(c);
    c->irq_maybe = true;
    return 1u;
}

static uint32_t a7_ir_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                           uint32_t *out)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);
    uint32_t v = 0u;
    X x;

    if ((spec & A7_MEM_ALIGNED) != 0u && (addr & (size - 1u)) != 0u) {
        x = ARMV7M_X_UNALIGNED;
    } else {
        x = armv7m_load_u(c, addr, size, &v);
    }
    if (EMU_UNLIKELY(x != ARMV7M_X_NONE)) {
        return ir_fault(c, x);
    }
    if ((spec & EMU_IR_MEM_SIGNED) != 0u) {
        v = (size == 1u)   ? (uint32_t)(int32_t)(int8_t)v
            : (size == 2u) ? (uint32_t)(int32_t)(int16_t)v
                           : v;
    }
    *out = v;
    return 0u;
}

static uint32_t a7_ir_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                            uint32_t val)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);
    X x;

    if ((spec & A7_MEM_ALIGNED) != 0u && (addr & (size - 1u)) != 0u) {
        x = ARMV7M_X_UNALIGNED;
    } else {
        x = armv7m_store_u(c, addr, size, val);
    }
    if (EMU_UNLIKELY(x != ARMV7M_X_NONE)) {
        return ir_fault(c, x);
    }
    return 0u;
}

/*
 * The fallback: one instruction, by the interpreter. `insn` is the first
 * halfword in the low half, as the trace hook carries it.
 *
 * Non-zero means the instruction did not complete -- it faulted, and the
 * exception has been entered -- so the block stops there without
 * counting it, exactly as the interpreter does not count it. When it
 * returns zero, r15 is where execution continues, which the translator
 * either knows to be the next instruction or reads back.
 */
static uint32_t a7_ir_step(emu_cpu_t *cpu, uint32_t insn, uint32_t pc)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;
    bool ok;

    armv7m_flags_pack(c);
    ok = armv7m_step_insn(c, (uint16_t)insn, (uint16_t)(insn >> 16), pc);
    armv7m_flags_unpack(c);
    c->jit_ctx = armv7m_jit_ctx(c);
    c->irq_maybe = true;
    return ok ? 0u : 1u;
}

static const void *const a7_ir_helpers[] = {
    (const void *)a7_ir_step,
};
#define A7_HELPER_STEP 0u

static uint32_t a7_reg_offset(uint32_t n)
{
    if (n < 16u) {
        return (uint32_t)offsetof(armv7m_cpu_t, r) + n * 4u;
    }
    switch (n) {
    case R_XPSR:
        return (uint32_t)offsetof(armv7m_cpu_t, xpsr);
    case R_NF:
        return (uint32_t)offsetof(armv7m_cpu_t, jit_nf);
    case R_ZF:
        return (uint32_t)offsetof(armv7m_cpu_t, jit_zf);
    case R_CF:
        return (uint32_t)offsetof(armv7m_cpu_t, jit_cf);
    case R_VF:
        return (uint32_t)offsetof(armv7m_cpu_t, jit_vf);
    default:
        /* R_CTX: the low word of a little-endian 64-bit key. Both hosts
         * are. */
        return (uint32_t)offsetof(armv7m_cpu_t, jit_ctx);
    }
}

static bool a7_reg_zero(uint32_t n)
{
    (void)n;
    return false; /* no register is hardwired on this architecture */
}

const emu_ir_target_t armv7m_ir_target = {
    .reg_offset = a7_reg_offset,
    /*
     * No EMU_IR_SETF is ever emitted, so no flag has a home here: the
     * flags are ordinary values in guest registers of their own. A
     * backend that sees flag_bit[] zero lowers no flag operation.
     */
    .flags_offset = (uint32_t)offsetof(armv7m_cpu_t, xpsr),
    .flag_bit = {0u, 0u, 0u, 0u},
    .reg_is_zero = a7_reg_zero,
    .pc_offset = (uint32_t)offsetof(armv7m_cpu_t, r) + R_PC * 4u,
    .helpers = a7_ir_helpers,
    .helper_count = (uint32_t)(sizeof(a7_ir_helpers) / sizeof(a7_ir_helpers[0])),
    .load = a7_ir_load,
    .store = a7_ir_store,
};

/*
 * The window a backend may read and write without calling the two
 * functions above.
 *
 * Every load and store was a call into a region walk and an MPU check,
 * and on CoreMark those calls were 44% of all host instructions -- twice
 * what the translated code itself cost. Inlined, an access to RAM is a
 * compare, a branch and a move.
 *
 * It says no whenever an access can mean more than a move, and a block
 * that inlines one is specialised on the answer:
 *
 *   the MPU on          every access needs a permission check
 *   CCR.UNALIGN_TRP     an unaligned access has to fault
 *
 * Both move jit_gen when they are written, so such a block does not
 * survive the change. Privilege needs nothing: with the MPU off the
 * default map lets unprivileged code at RAM, and the System Control
 * Space -- which it may not touch -- is not in the window.
 *
 * The window is the RAM the stack is in. That is the region a compiled
 * program's loads and stores overwhelmingly go to, and asking the bus
 * where sp points finds it on any platform without this file knowing a
 * memory map. Flash is not in it; a literal load from flash is folded
 * to a constant instead, see lower_ldst.
 *
 * The store guard is smc_watch: once any block has been translated from
 * writable memory, a store has to go the long way round so that
 * armv7m_wrote can see it.
 */
static bool a7_fast_mem(armv7m_cpu_t *c, emu_ir_fastmem_t *out)
{
    emu_region_t *r;

    if ((c->mpu_ctrl & 1u) != 0u || (c->ccr & (1u << 3)) != 0u ||
        c->bus == NULL) {
        return false;
    }
    r = emu_bus_find(c->bus, c->r[13] - 4u);
    if (r == NULL || r->kind != (uint8_t)EMU_MEM_RAM || r->host == NULL ||
        r->size == 0u || r->base < 0x10000000u ||
        (r->perm & (EMU_PERM_R | EMU_PERM_W)) != (EMU_PERM_R | EMU_PERM_W)) {
        return false;
    }
    out->base = r->base;
    out->size = r->size;
    out->host = r->host;
    out->store_guard_offset = (uint32_t)offsetof(armv7m_cpu_t, smc_watch);
    return true;
}

/* ------------------------------------------------------------------ */
/* Building blocks                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    B *b;
    armv7m_cpu_t *c;
    uint32_t pc;   /* of the instruction being lowered            */
    uint32_t len;
    uint32_t insn; /* w0 | w1 << 16                               */
    uint32_t ctx;  /* what the block is being built for           */
    /*
     * Inside an IT block: `cond` is this instruction's condition as a
     * 0/1 value, and every register and flag write becomes a choice
     * between the new value and the old. `last` says a branch is legal.
     */
    bool in_it;
    bool it_last;
    uint16_t cond;
    uint16_t cmask; /* 0 or ~0 from `cond`, made on first use      */
    bool ends;      /* the instruction left the block              */
    bool counted;   /* ...or emitted its own RETIRE                */
} T;

static uint16_t K(B *b, uint32_t v)
{
    return emu_ir_const(b, v);
}

static uint16_t G(B *b, uint32_t r)
{
    return emu_ir_get(b, r);
}

static uint16_t OP(B *b, emu_ir_op_t op, uint16_t x, uint16_t y)
{
    return emu_ir_alu(b, op, x, y);
}

static uint16_t OPI(B *b, emu_ir_op_t op, uint16_t x, uint32_t imm)
{
    return emu_ir_emit(b, op, 0u, x, NOT, imm, 0u);
}

static uint16_t CC(B *b, emu_ir_cond_t cond, uint16_t x, uint16_t y)
{
    return emu_ir_emit(b, EMU_IR_SETCC, (uint8_t)cond, x, y, 0u, 0u);
}

/* Constants as operands: the fusion pass makes immediates of them where
 * the host has the form, which is not this file's business to know. */
static uint16_t ANDK(B *b, uint16_t x, uint32_t k)
{
    return OP(b, EMU_IR_AND, x, K(b, k));
}

static uint16_t XORK(B *b, uint16_t x, uint32_t k)
{
    return OP(b, EMU_IR_XOR, x, K(b, k));
}

static uint16_t ADDK(B *b, uint16_t x, uint32_t k)
{
    return (k == 0u) ? x : OP(b, EMU_IR_ADD, x, K(b, k));
}

/* Shifts by a constant 0..31; the IR masks the count, so 32 is not this. */
static uint16_t SHL(B *b, uint16_t x, uint32_t n)
{
    return (n == 0u) ? x : OPI(b, EMU_IR_SHLI, x, n);
}

static uint16_t SHR(B *b, uint16_t x, uint32_t n)
{
    return (n == 0u) ? x : OPI(b, EMU_IR_SHRI, x, n);
}

static uint16_t SAR(B *b, uint16_t x, uint32_t n)
{
    return (n == 0u) ? x : OPI(b, EMU_IR_SARI, x, n);
}

static void retire(T *t)
{
    (void)emu_ir_emit(t->b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
    t->counted = true;
}

/* pc as an operand is this instruction's address plus four. */
static uint16_t rreg(T *t, uint32_t n)
{
    return (n == R_PC) ? K(t->b, t->pc + 4u) : G(t->b, n);
}

/* cond ? nv : ov, without a branch: the IR has none inside a block. */
static uint16_t sel(T *t, uint16_t nv, uint16_t ov)
{
    B *const b = t->b;

    if (t->cmask == NOT) {
        t->cmask = OP(b, EMU_IR_SUB, K(b, 0u), t->cond);
    }
    return OP(b, EMU_IR_XOR, ov,
              OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, nv, ov), t->cmask));
}

static void wreg(T *t, uint32_t n, uint16_t v)
{
    if (t->in_it) {
        v = sel(t, v, G(t->b, n));
    }
    emu_ir_put(t->b, n, v);
}

/* One flag word, chosen against its old value inside an IT block. */
static void wflag(T *t, uint32_t reg, uint16_t v)
{
    if (t->in_it) {
        v = sel(t, v, G(t->b, reg));
    }
    emu_ir_put(t->b, reg, v);
}

/* N and Z from a result: the result is both. */
static void flags_nz(T *t, uint16_t res)
{
    wflag(t, R_NF, res);
    wflag(t, R_ZF, res);
}

/* The carry flag as it stands, 0 or 1. */
static uint16_t carry01(B *b)
{
    return G(b, R_CF);
}

/*
 * A condition as a 0/1 value.
 *
 * The eight base conditions; an odd condition number is the inverse of
 * the even one below it, which is how the architecture numbers them. So
 * each case spells both senses and nothing is inverted afterwards.
 */
static uint16_t cond01(B *b, uint32_t cond)
{
    const bool inv = (cond & 1u) != 0u;
    const uint16_t zero = K(b, 0u);

    switch (cond >> 1) {
    case 0u: /* EQ / NE */
        return CC(b, inv ? EMU_IR_C_NE : EMU_IR_C_EQ, G(b, R_ZF), zero);
    case 1u: /* CS / CC */
        return inv ? XORK(b, G(b, R_CF), 1u) : G(b, R_CF);
    case 2u: /* MI / PL */
        return CC(b, inv ? EMU_IR_C_GE : EMU_IR_C_LT, G(b, R_NF), zero);
    case 3u: /* VS / VC */
        return CC(b, inv ? EMU_IR_C_GE : EMU_IR_C_LT, G(b, R_VF), zero);
    case 4u: { /* HI: C and not Z / LS */
        const uint16_t hi = OP(b, EMU_IR_AND, G(b, R_CF),
                               CC(b, EMU_IR_C_NE, G(b, R_ZF), zero));

        return inv ? XORK(b, hi, 1u) : hi;
    }
    case 5u: /* GE: N == V / LT */
        return CC(b, inv ? EMU_IR_C_LT : EMU_IR_C_GE,
                  OP(b, EMU_IR_XOR, G(b, R_NF), G(b, R_VF)), zero);
    case 6u: { /* GT: not Z, and N == V / LE */
        const uint16_t gt =
            OP(b, EMU_IR_AND, CC(b, EMU_IR_C_NE, G(b, R_ZF), zero),
               CC(b, EMU_IR_C_GE, OP(b, EMU_IR_XOR, G(b, R_NF), G(b, R_VF)),
                  zero));

        return inv ? XORK(b, gt, 1u) : gt;
    }
    default: /* AL */
        return K(b, 1u);
    }
}

/*
 * Shift_C with a constant amount, as DecodeImmShift gives it: LSR and ASR
 * run to 32, ROR to 31, and RRX is its own type. `*cout` is the carry
 * out as 0/1, or NOT where the carry is unchanged or was not asked for.
 */
static uint16_t shift_imm(T *t, uint16_t v, uint32_t type, uint32_t n,
                          bool want_c, uint16_t *cout)
{
    B *const b = t->b;

    *cout = NOT;
    switch (type) {
    case ARMV7M_SH_LSL:
        if (n == 0u) {
            return v;
        }
        if (want_c) {
            *cout = ANDK(b, SHR(b, v, 32u - n), 1u);
        }
        return SHL(b, v, n);
    case ARMV7M_SH_LSR:
        if (n >= 32u) {
            if (want_c) {
                *cout = SHR(b, v, 31u);
            }
            return K(b, 0u);
        }
        if (want_c) {
            *cout = ANDK(b, SHR(b, v, n - 1u), 1u);
        }
        return SHR(b, v, n);
    case ARMV7M_SH_ASR:
        if (n >= 32u) {
            if (want_c) {
                *cout = SHR(b, v, 31u);
            }
            return SAR(b, v, 31u);
        }
        if (want_c) {
            *cout = ANDK(b, SHR(b, v, n - 1u), 1u);
        }
        return SAR(b, v, n);
    case ARMV7M_SH_ROR: {
        const uint16_t r =
            OP(b, EMU_IR_OR, SHR(b, v, n & 31u), SHL(b, v, (32u - n) & 31u));

        if (want_c) {
            *cout = SHR(b, r, 31u);
        }
        return r;
    }
    default: { /* RRX */
        const uint16_t r =
            OP(b, EMU_IR_OR, SHL(b, carry01(b), 31u), SHR(b, v, 1u));

        if (want_c) {
            *cout = ANDK(b, v, 1u);
        }
        return r;
    }
    }
}

/* The second operand of a data-processing instruction. */
static uint16_t operand2(T *t, const I *d, bool want_c, uint16_t *cout)
{
    *cout = NOT;
    if (d->opnd == ARMV7M_OPND_IMM) {
        if (want_c && d->imm_carry != 2u) {
            *cout = K(t->b, d->imm_carry);
        }
        return K(t->b, d->imm);
    }
    return shift_imm(t, rreg(t, d->rm), d->shift_t, d->shift_n, want_c, cout);
}

/* Does this instruction set the flags, where it is? */
static bool sets_flags(const T *t, const I *d)
{
    return d->setflags == ARMV7M_SF_YES ||
           (d->setflags == ARMV7M_SF_NOT_IN_IT && !t->in_it);
}

/* N and Z from a result, and C where the shifter produced one. */
static void flags_logical(T *t, uint16_t res, uint16_t cout)
{
    flags_nz(t, res);
    if (cout != NOT) {
        wflag(t, R_CF, cout);
    }
}

/* All four, given the carry as 0/1 and a word whose bit 31 is overflow. */
static void flags_arith(T *t, uint16_t res, uint16_t c01, uint16_t vword)
{
    flags_nz(t, res);
    wflag(t, R_CF, c01);
    wflag(t, R_VF, vword);
}

/*
 * a + m + carry-in, the one adder every ARM arithmetic instruction is:
 * SUB is a + ~m + 1 and SBC is a + ~m + C, which is also why ARM's carry
 * after a subtract is the inverse of a borrow.
 *
 * `cin` is NOT for a carry-in of zero, or a 0/1 value. `one` adds the
 * constant one instead, for SUB and CMP -- kept apart from `cin` so that
 * the common subtract is one SUB and one compare rather than three adds.
 */
static uint16_t adder(T *t, uint16_t a, uint16_t m, bool sub, uint16_t cin,
                      bool flags)
{
    B *const b = t->b;
    uint16_t res;
    uint16_t c01 = NOT;
    uint16_t vword = NOT;

    if (cin == NOT) {
        if (sub) {
            res = OP(b, EMU_IR_SUB, a, m);
            if (flags) {
                c01 = CC(b, EMU_IR_C_GEU, a, m);
                vword = OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, a, m),
                           OP(b, EMU_IR_XOR, a, res));
            }
        } else {
            res = OP(b, EMU_IR_ADD, a, m);
            if (flags) {
                c01 = CC(b, EMU_IR_C_LTU, res, a);
                vword = OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, a, res),
                           OP(b, EMU_IR_XOR, m, res));
            }
        }
    } else {
        /* With a carry in: two adds, and a carry out of either is one. */
        const uint16_t mm = sub ? XORK(b, m, 0xFFFFFFFFu) : m;
        const uint16_t s1 = OP(b, EMU_IR_ADD, a, mm);

        res = OP(b, EMU_IR_ADD, s1, cin);
        if (flags) {
            c01 = OP(b, EMU_IR_OR, CC(b, EMU_IR_C_LTU, s1, a),
                     CC(b, EMU_IR_C_LTU, res, s1));
            vword = OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, a, res),
                       OP(b, EMU_IR_XOR, mm, res));
        }
    }
    if (flags) {
        flags_arith(t, res, c01, vword);
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* Leaving a block                                                     */
/* ------------------------------------------------------------------ */

static void exit_const(T *t, uint32_t target)
{
    retire(t);
    (void)emu_ir_emit(t->b, EMU_IR_EXIT, 0u, NOT, NOT, target, 0u);
    t->ends = true;
}

/*
 * An address for a MemA access -- LDRD, LDM, PUSH and the rest -- that
 * an inlined access cannot be allowed to perform if it is misaligned.
 *
 * The inlined path checks that an address is inside the window and
 * nothing else, so a misaligned LDRD into RAM would simply load, where
 * the architecture requires a fault. There is no branch to put in front
 * of it. Instead a misaligned address has its top four bits set, which
 * takes it out of any window and so to the checked path -- and that
 * tests the low bits, which are untouched, before it looks at the rest.
 * UNALIGNED records no address, so the bits set here are never seen.
 *
 * Four operations per instruction, not per access: the alignment of
 * every word a PUSH writes is the alignment of its base.
 */
static uint16_t aligned_base(T *t, uint16_t base)
{
    B *const b = t->b;

    if (!b->has_fast) {
        return base;
    }
    return OP(b, EMU_IR_OR, base,
              ANDK(b, OP(b, EMU_IR_SUB, K(b, 0u), ANDK(b, base, 3u)),
                   0xF0000000u));
}

/* Leave if a 0/1 value is set; fall through otherwise. */
static void exit_if(T *t, uint16_t c01, uint32_t target)
{
    retire(t);
    (void)emu_ir_emit(t->b, EMU_IR_EXIT_IF, EMU_IR_C_NE, c01, K(t->b, 0u),
                      target, 0u);
}

/*
 * Leave if a condition holds.
 *
 * The ten conditions that are one flag word against zero are *that
 * compare*, with no 0/1 value in between: `cmp; bne` is a subtract and a
 * branch on its result, which is what it is on the machine being
 * emulated. Only HI, LS, GT and LE combine two flags and go through
 * cond01.
 */
static void exit_cond(T *t, uint32_t cond, uint32_t target)
{
    B *const b = t->b;
    const bool inv = (cond & 1u) != 0u;
    emu_ir_cond_t cc;
    uint16_t v;

    switch (cond >> 1) {
    case 0u:
        cc = inv ? EMU_IR_C_NE : EMU_IR_C_EQ;
        v = G(b, R_ZF);
        break;
    case 1u:
        cc = inv ? EMU_IR_C_EQ : EMU_IR_C_NE;
        v = G(b, R_CF);
        break;
    case 2u:
        cc = inv ? EMU_IR_C_GE : EMU_IR_C_LT;
        v = G(b, R_NF);
        break;
    case 3u:
        cc = inv ? EMU_IR_C_GE : EMU_IR_C_LT;
        v = G(b, R_VF);
        break;
    case 5u:
        cc = inv ? EMU_IR_C_LT : EMU_IR_C_GE;
        v = OP(b, EMU_IR_XOR, G(b, R_NF), G(b, R_VF));
        break;
    default:
        exit_if(t, cond01(b, cond), target);
        return;
    }
    retire(t);
    (void)emu_ir_emit(b, EMU_IR_EXIT_IF, (uint8_t)cc, v, K(b, 0u), target, 0u);
}

/*
 * BXWritePC in Thread mode: bit 0 of the value becomes EPSR.T and the
 * rest is the address.
 *
 * **Thread mode only**, and the caller checks: in Handler mode a value
 * of 0xFxxxxxxx is an exception return, which is the interpreter's.
 *
 * T is written whichever way it goes, and so is the context word --
 * a cleared T is not a mistake this code can refuse, it is an INVSTATE
 * fault the *next* fetch has to take, and what makes that happen is the
 * dispatcher finding no block for a context with T clear.
 */
static void exit_interwork(T *t, uint16_t v)
{
    B *const b = t->b;
    const uint16_t tbit = ANDK(b, v, 1u);

    emu_ir_put(b, R_XPSR,
               OP(b, EMU_IR_OR, ANDK(b, G(b, R_XPSR), ~ARMV7M_T),
                  SHL(b, tbit, 24u)));
    emu_ir_put(b, R_CTX,
               OP(b, EMU_IR_OR, K(b, t->ctx & ~ARMV7M_CTX_T), tbit));
    retire(t);
    (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, ANDK(b, v, 0xFFFFFFFEu), NOT, 0u,
                      0u);
    t->ends = true;
}

static bool thread_mode(const T *t)
{
    return (t->ctx & ARMV7M_CTX_HANDLER) == 0u;
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

static bool lower_dp(T *t, const I *d)
{
    B *const b = t->b;
    const bool sf = sets_flags(t, d);
    uint16_t cy;
    uint16_t m;
    uint16_t a;
    uint16_t res;

    /* A write to the pc is a branch, and those have their own rules. */
    if (d->rd == R_PC) {
        return false;
    }
    if (d->opnd != ARMV7M_OPND_IMM && d->opnd != ARMV7M_OPND_REG) {
        return false;
    }

    switch (d->op) {
    case ARMV7M_OP_AND:
    case ARMV7M_OP_BIC:
    case ARMV7M_OP_ORR:
    case ARMV7M_OP_ORN:
    case ARMV7M_OP_EOR:
    case ARMV7M_OP_TST:
    case ARMV7M_OP_TEQ:
        a = rreg(t, d->rn);
        m = operand2(t, d, sf, &cy);
        if (d->op == ARMV7M_OP_BIC || d->op == ARMV7M_OP_ORN) {
            m = XORK(b, m, 0xFFFFFFFFu);
        }
        res = OP(b,
                 (d->op == ARMV7M_OP_AND || d->op == ARMV7M_OP_BIC ||
                  d->op == ARMV7M_OP_TST)
                     ? EMU_IR_AND
                     : (d->op == ARMV7M_OP_ORR || d->op == ARMV7M_OP_ORN)
                           ? EMU_IR_OR
                           : EMU_IR_XOR,
                 a, m);
        if (sf) {
            flags_logical(t, res, cy);
        }
        if (d->op != ARMV7M_OP_TST && d->op != ARMV7M_OP_TEQ) {
            wreg(t, d->rd, res);
        }
        return true;

    case ARMV7M_OP_MOV:
    case ARMV7M_OP_MVN:
        res = operand2(t, d, sf, &cy);
        if (d->op == ARMV7M_OP_MVN) {
            res = XORK(b, res, 0xFFFFFFFFu);
        }
        if (sf) {
            flags_logical(t, res, cy);
        }
        wreg(t, d->rd, res);
        return true;

    case ARMV7M_OP_ADD:
    case ARMV7M_OP_CMN:
        a = rreg(t, d->rn);
        m = operand2(t, d, false, &cy);
        res = adder(t, a, m, false, NOT, sf);
        if (d->op == ARMV7M_OP_ADD) {
            wreg(t, d->rd, res);
        }
        return true;

    case ARMV7M_OP_SUB:
    case ARMV7M_OP_CMP:
        a = rreg(t, d->rn);
        m = operand2(t, d, false, &cy);
        res = adder(t, a, m, true, NOT, sf);
        if (d->op == ARMV7M_OP_SUB) {
            wreg(t, d->rd, res);
        }
        return true;

    case ARMV7M_OP_RSB:
        a = rreg(t, d->rn);
        m = operand2(t, d, false, &cy);
        wreg(t, d->rd, adder(t, m, a, true, NOT, sf));
        return true;

    case ARMV7M_OP_ADC:
    case ARMV7M_OP_SBC: {
        const uint16_t cin = carry01(b);

        a = rreg(t, d->rn);
        m = operand2(t, d, false, &cy);
        wreg(t, d->rd, adder(t, a, m, d->op == ARMV7M_OP_SBC, cin, sf));
        return true;
    }

    default:
        return false;
    }
}

/*
 * A shift by a register: the count is the low *byte* of Rm, so it runs
 * to 255, where the IR's shifts take five bits.
 *
 * Everything past 31 is a rule rather than a shift -- LSL and LSR give
 * zero, ASR gives the sign, ROR goes round -- and so is the carry: the
 * last bit out for a count up to 32, zero beyond it for the logical
 * pair, the sign for ASR, and *unchanged* for a count of zero. There is
 * no branch inside a block, so each rule is a mask.
 *
 * It looks like a lot of operations for a shift, and for the flags it
 * is. But the 16-bit forms set flags only because every 16-bit ALU
 * instruction does, and a compiler that writes `lsls r0, r1` wants the
 * value: the carry is overwritten before anything reads it and the dead
 * value pass deletes its whole computation.
 */
static bool lower_shift_reg(T *t, const I *d)
{
    B *const b = t->b;
    const bool sf = sets_flags(t, d);
    const uint32_t type = (uint32_t)d->op - (uint32_t)ARMV7M_OP_LSL;
    uint16_t n;
    uint16_t amt;
    uint16_t zero;
    uint16_t m32; /* ~0 where the count is below 32, else 0 */
    uint16_t res;
    uint16_t cn = NOT;

    if (d->rn >= 15u || d->rm >= 15u || type > ARMV7M_SH_ROR) {
        return false;
    }
    n = G(b, d->rn);
    amt = ANDK(b, G(b, d->rm), 0xFFu);
    zero = K(b, 0u);
    m32 = OP(b, EMU_IR_SUB, zero, CC(b, EMU_IR_C_LTU, amt, K(b, 32u)));

    switch (type) {
    case ARMV7M_SH_LSL:
        res = OP(b, EMU_IR_AND, OP(b, EMU_IR_SHL, n, amt), m32);
        if (sf) {
            /* Bit 32 - count, for a count of 1..32. */
            cn = OP(b, EMU_IR_AND,
                    ANDK(b,
                         OP(b, EMU_IR_SHR, n,
                            ANDK(b, OP(b, EMU_IR_SUB, K(b, 32u), amt), 31u)),
                         1u),
                    CC(b, EMU_IR_C_LEU, amt, K(b, 32u)));
        }
        break;
    case ARMV7M_SH_LSR:
        res = OP(b, EMU_IR_AND, OP(b, EMU_IR_SHR, n, amt), m32);
        if (sf) {
            /* Bit count - 1, for a count of 1..32. */
            cn = OP(b, EMU_IR_AND,
                    ANDK(b,
                         OP(b, EMU_IR_SHR, n,
                            ANDK(b, OP(b, EMU_IR_SUB, amt, K(b, 1u)), 31u)),
                         1u),
                    CC(b, EMU_IR_C_LEU, amt, K(b, 32u)));
        }
        break;
    case ARMV7M_SH_ASR: {
        /* The count, and the count less one, each held at 31. */
        const uint16_t hi = ANDK(b, XORK(b, m32, 0xFFFFFFFFu), 31u);

        res = OP(b, EMU_IR_SAR, n,
                 OP(b, EMU_IR_OR, OP(b, EMU_IR_AND, amt, m32), hi));
        if (sf) {
            const uint16_t am1 = OP(b, EMU_IR_SUB, amt, K(b, 1u));

            cn = ANDK(b,
                      OP(b, EMU_IR_SHR, n,
                         ANDK(b,
                              OP(b, EMU_IR_OR, OP(b, EMU_IR_AND, am1, m32), hi),
                              31u)),
                      1u);
        }
        break;
    }
    default: { /* ROR: a rotation by 32 is the value, with its top bit out */
        const uint16_t r = ANDK(b, amt, 31u);

        res = OP(b, EMU_IR_OR, OP(b, EMU_IR_SHR, n, r),
                 OP(b, EMU_IR_SHL, n,
                    ANDK(b, OP(b, EMU_IR_SUB, K(b, 32u), r), 31u)));
        if (sf) {
            cn = SHR(b, res, 31u);
        }
        break;
    }
    }
    if (sf) {
        /* A count of zero leaves the carry alone. */
        const uint16_t keep = OP(b, EMU_IR_SUB, zero, CC(b, EMU_IR_C_EQ, amt, zero));
        const uint16_t old = G(b, R_CF);

        flags_nz(t, res);
        wflag(t, R_CF,
              OP(b, EMU_IR_XOR, cn,
                 OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, cn, old), keep)));
    }
    wreg(t, d->rd, res);
    return true;
}

/* LSL, LSR, ASR, ROR and RRX as instructions, by a constant. */
static bool lower_shift(T *t, const I *d)
{
    const bool sf = sets_flags(t, d);
    uint32_t type;
    uint16_t cy;
    uint16_t res;

    if (d->rd == R_PC) {
        return false;
    }
    if (d->opnd == ARMV7M_OPND_REGSHIFT) {
        return lower_shift_reg(t, d);
    }
    type = (uint32_t)d->op - (uint32_t)ARMV7M_OP_LSL;
    res = shift_imm(t, rreg(t, d->rn), type, d->imm, sf, &cy);
    if (sf) {
        flags_logical(t, res, cy);
    }
    wreg(t, d->rd, res);
    return true;
}

/* Half of a register as a signed sixteen bits: the top or the bottom. */
static uint16_t half16(B *b, uint16_t v, bool top)
{
    return top ? SAR(b, v, 16u)
               : emu_ir_emit(b, EMU_IR_SEXT16, 0u, v, NOT, 0u, 0u);
}

/* Q, sticky, from a word whose bit 31 says the result overflowed. */
static void set_q(T *t, uint16_t vword)
{
    B *const b = t->b;
    const uint16_t x = G(b, R_XPSR);
    uint16_t nx = OP(b, EMU_IR_OR, x, ANDK(b, SHR(b, vword, 4u), 1u << 27));

    if (t->in_it) {
        nx = sel(t, nx, x);
    }
    emu_ir_put(b, R_XPSR, nx);
}

static bool lower_mul(T *t, const I *d)
{
    B *const b = t->b;

    if (d->rd == R_PC || d->rn == R_PC || d->rm == R_PC) {
        return false;
    }
    switch (d->op) {
    case ARMV7M_OP_SMULBB:
    case ARMV7M_OP_SMULBT:
    case ARMV7M_OP_SMULTB:
    case ARMV7M_OP_SMULTT:
    case ARMV7M_OP_SMLABB:
    case ARMV7M_OP_SMLABT:
    case ARMV7M_OP_SMLATB:
    case ARMV7M_OP_SMLATT: {
        /* The suffix names Rn's half and then Rm's. */
        const bool acc = d->op >= ARMV7M_OP_SMLABB && d->op <= ARMV7M_OP_SMLATT;
        const uint32_t which =
            (uint32_t)d->op - (uint32_t)(acc ? ARMV7M_OP_SMLABB : ARMV7M_OP_SMULBB);
        const uint16_t p = OP(b, EMU_IR_MUL, half16(b, G(b, d->rn), (which & 2u) != 0u),
                              half16(b, G(b, d->rm), (which & 1u) != 0u));

        if (!acc) {
            wreg(t, d->rd, p); /* cannot overflow: 16 x 16 fits */
            return true;
        }
        if (d->ra >= 15u) {
            return false;
        }
        {
            const uint16_t a = G(b, d->ra);
            const uint16_t res = OP(b, EMU_IR_ADD, p, a);

            /* The accumulate can, and says so in Q rather than in V. */
            set_q(t, OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, p, res),
                        OP(b, EMU_IR_XOR, a, res)));
            wreg(t, d->rd, res);
        }
        return true;
    }
    case ARMV7M_OP_UDIV:
    case ARMV7M_OP_SDIV: {
        /*
         * The IR's divide has the host's corners and not the guest's:
         * it must not be given a zero divisor, nor INT_MIN / -1. ARM
         * defines both -- zero, and INT_MIN -- so the divisor is made 1
         * wherever the host would trap, and the quotient cleared where
         * the divisor was zero. Dividing INT_MIN by 1 is already the
         * answer for the other.
         *
         * With CCR.DIV_0_TRP set a zero divisor is a fault, which is the
         * interpreter's to raise; the bit moves the generation.
         */
        const bool sgn = d->op == ARMV7M_OP_SDIV;
        const emu_ir_op_t op = sgn ? EMU_IR_DIVS : EMU_IR_DIVU;
        uint16_t n;
        uint16_t m;
        uint16_t z;
        uint16_t bad;

        if ((t->c->ccr & (1u << 4)) != 0u || !emu_ir_can_lower(op, 0u)) {
            return false;
        }
        n = G(b, d->rn);
        m = G(b, d->rm);
        z = CC(b, EMU_IR_C_EQ, m, K(b, 0u));
        bad = z;
        if (sgn) {
            bad = OP(b, EMU_IR_OR, z,
                     OP(b, EMU_IR_AND, CC(b, EMU_IR_C_EQ, n, K(b, 0x80000000u)),
                        CC(b, EMU_IR_C_EQ, m, K(b, 0xFFFFFFFFu))));
        }
        /* bad ? 1 : m, then 0 where the divisor was zero. */
        m = OP(b, EMU_IR_OR,
               OP(b, EMU_IR_AND, m, OP(b, EMU_IR_SUB, bad, K(b, 1u))), bad);
        wreg(t, d->rd,
             OP(b, EMU_IR_AND, OP(b, op, n, m), OP(b, EMU_IR_SUB, z, K(b, 1u))));
        return true;
    }
    case ARMV7M_OP_MUL: {
        const uint16_t res =
            OP(b, EMU_IR_MUL, G(b, d->rn), G(b, d->rm));

        if (sets_flags(t, d)) {
            flags_nz(t, res);
        }
        wreg(t, d->rd, res);
        return true;
    }
    case ARMV7M_OP_MLA:
    case ARMV7M_OP_MLS: {
        const uint16_t p = OP(b, EMU_IR_MUL, G(b, d->rn), G(b, d->rm));

        if (d->ra >= 15u) {
            return false;
        }
        wreg(t, d->rd,
             OP(b, d->op == ARMV7M_OP_MLA ? EMU_IR_ADD : EMU_IR_SUB,
                G(b, d->ra), p));
        return true;
    }
    case ARMV7M_OP_UMULL:
    case ARMV7M_OP_SMULL:
    case ARMV7M_OP_UMLAL:
    case ARMV7M_OP_SMLAL: {
        const bool sgn = d->op == ARMV7M_OP_SMULL || d->op == ARMV7M_OP_SMLAL;
        const bool acc = d->op == ARMV7M_OP_UMLAL || d->op == ARMV7M_OP_SMLAL;
        const uint16_t n = G(b, d->rn);
        const uint16_t m = G(b, d->rm);
        uint16_t lo = OP(b, EMU_IR_MUL, n, m);
        uint16_t hi = OP(b, sgn ? EMU_IR_MULHS : EMU_IR_MULHU, n, m);

        if (d->ra >= 15u || d->rd == d->ra) {
            return false;
        }
        if (acc) {
            const uint16_t sum = OP(b, EMU_IR_ADD, lo, G(b, d->rd));

            hi = OP(b, EMU_IR_ADD, OP(b, EMU_IR_ADD, hi, G(b, d->ra)),
                    CC(b, EMU_IR_C_LTU, sum, lo));
            lo = sum;
        }
        wreg(t, d->rd, lo);
        wreg(t, d->ra, hi);
        return true;
    }
    default:
        return false;
    }
}

static bool lower_misc(T *t, const I *d)
{
    B *const b = t->b;

    if (d->rd == R_PC) {
        return false;
    }
    switch (d->op) {
    case ARMV7M_OP_MOVW:
        wreg(t, d->rd, K(b, d->imm));
        return true;
    case ARMV7M_OP_MOVT:
        wreg(t, d->rd,
             OP(b, EMU_IR_OR, ANDK(b, G(b, d->rd), 0xFFFFu), K(b, d->imm << 16)));
        return true;
    case ARMV7M_OP_ADR: {
        const uint32_t base = (t->pc + 4u) & ~3u;

        wreg(t, d->rd, K(b, d->add ? base + d->imm : base - d->imm));
        return true;
    }
    case ARMV7M_OP_SXTB:
    case ARMV7M_OP_SXTH:
    case ARMV7M_OP_UXTB:
    case ARMV7M_OP_UXTH:
    case ARMV7M_OP_SXTAB:
    case ARMV7M_OP_SXTAH:
    case ARMV7M_OP_UXTAB:
    case ARMV7M_OP_UXTAH: {
        static const uint8_t k_ext[8] = {
            EMU_IR_SEXT8,  EMU_IR_SEXT16, EMU_IR_ZEXT8,  EMU_IR_ZEXT16,
            EMU_IR_SEXT8,  EMU_IR_SEXT16, EMU_IR_ZEXT8,  EMU_IR_ZEXT16,
        };
        const uint32_t which = (uint32_t)d->op - (uint32_t)ARMV7M_OP_SXTB;
        uint16_t v;

        if (d->rm == R_PC || d->rn == R_PC || which >= 12u) {
            return false;
        }
        v = G(b, d->rm);
        if (d->imm != 0u) {
            v = OP(b, EMU_IR_OR, SHR(b, v, d->imm), SHL(b, v, 32u - d->imm));
        }
        /* SXTB SXTH UXTB UXTH [SXTB16 UXTB16] SXTAB SXTAH UXTAB UXTAH */
        v = emu_ir_emit(b, (emu_ir_op_t)k_ext[(which < 4u) ? which : which - 2u],
                        0u, v, NOT, 0u, 0u);
        if (d->rn != ARMV7M_NOREG) {
            v = OP(b, EMU_IR_ADD, G(b, d->rn), v);
        }
        wreg(t, d->rd, v);
        return true;
    }
    case ARMV7M_OP_REV:
    case ARMV7M_OP_REV16:
    case ARMV7M_OP_REVSH: {
        uint16_t v;

        if (d->rm == R_PC) {
            return false;
        }
        v = G(b, d->rm);
        if (d->op == ARMV7M_OP_REV) {
            v = OP(b, EMU_IR_OR,
                   OP(b, EMU_IR_OR, SHL(b, v, 24u),
                      SHL(b, ANDK(b, v, 0xFF00u), 8u)),
                   OP(b, EMU_IR_OR, ANDK(b, SHR(b, v, 8u), 0xFF00u),
                      SHR(b, v, 24u)));
        } else {
            v = OP(b, EMU_IR_OR, SHL(b, ANDK(b, v, 0x00FF00FFu), 8u),
                   ANDK(b, SHR(b, v, 8u), 0x00FF00FFu));
            if (d->op == ARMV7M_OP_REVSH) {
                v = emu_ir_emit(b, EMU_IR_SEXT16, 0u, v, NOT, 0u, 0u);
            }
        }
        wreg(t, d->rd, v);
        return true;
    }
    case ARMV7M_OP_CLZ:
        if (d->rm >= 15u || !emu_ir_can_lower(EMU_IR_CLZ, 0u)) {
            return false;
        }
        wreg(t, d->rd, emu_ir_emit(b, EMU_IR_CLZ, 0u, G(b, d->rm), NOT, 0u, 0u));
        return true;
    case ARMV7M_OP_UBFX:
    case ARMV7M_OP_SBFX: {
        const uint32_t lsb = d->imm;
        const uint32_t width = d->imm2;
        uint16_t v;

        if (d->rn == R_PC || width == 0u || lsb + width > 32u) {
            return false;
        }
        v = G(b, d->rn);
        if (d->op == ARMV7M_OP_UBFX) {
            v = SHR(b, v, lsb);
            if (width < 32u) {
                v = ANDK(b, v, (1u << width) - 1u);
            }
        } else {
            v = SAR(b, SHL(b, v, 32u - lsb - width), 32u - width);
        }
        wreg(t, d->rd, v);
        return true;
    }
    case ARMV7M_OP_BFI:
    case ARMV7M_OP_BFC: {
        const uint32_t lsb = d->imm;
        const uint32_t width = d->imm2;
        const uint32_t mask =
            ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << lsb;
        uint16_t v;

        if (width == 0u || lsb + width > 32u) {
            return false;
        }
        v = ANDK(b, G(b, d->rd), ~mask);
        if (d->op == ARMV7M_OP_BFI) {
            if (d->rn == R_PC) {
                return false;
            }
            v = OP(b, EMU_IR_OR, v, ANDK(b, SHL(b, G(b, d->rn), lsb), mask));
        }
        wreg(t, d->rd, v);
        return true;
    }
    default:
        return false;
    }
}

/* The value at `addr`, if nothing the guest can do will ever change it. */
static bool literal_const(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                          uint32_t *out)
{
    const emu_region_t *const r = emu_bus_find(c->bus, addr);

    if ((c->mpu_ctrl & 1u) != 0u || r == NULL ||
        r->kind != (uint8_t)EMU_MEM_ROM || (r->perm & EMU_PERM_W) != 0u ||
        addr + size > r->base + r->size || (addr & (size - 1u)) != 0u) {
        return false;
    }
    return emu_bus_read(c->bus, addr, size, out) == EMU_FAULT_NONE;
}

/* LDR and STR and their byte and halfword forms, one access. */
static bool lower_ldst(T *t, const I *d)
{
    B *const b = t->b;
    const bool load = d->op <= ARMV7M_OP_LDRSH;
    const uint8_t spec = EMU_IR_MEM_AUX(d->size, d->sext);
    uint16_t addr;
    uint16_t wb = NOT;
    uint32_t disp = 0u;

    /* A memory access is not something to do and then choose to undo. */
    if (t->in_it) {
        return false;
    }
    if (d->rd == R_PC && (!load || d->size != 4u || !thread_mode(t))) {
        return false; /* LDR pc is a branch; anything else here is not ours */
    }
    switch (d->addr) {
    case ARMV7M_ADDR_LITERAL: {
        const uint32_t base = (t->pc + 4u) & ~3u;
        const uint32_t at = d->add ? base + d->imm : base - d->imm;
        uint32_t v = 0u;

        /*
         * A literal in flash is a constant, and is lowered as one.
         *
         * "In flash" is the bus saying the region cannot be written, so
         * what is there now is what will be there for as long as the
         * block exists -- a fresh image flushes every block. And the MPU
         * must be off, since with it on the *permission* to read is
         * something a load checks each time; turning it on moves the
         * generation.
         *
         * This is most of a compiled function's loads from flash: every
         * address and every constant too wide for an immediate arrives
         * this way.
         */
        if (literal_const(t->c, at, d->size, &v)) {
            if (d->sext) {
                v = (d->size == 1u)   ? (uint32_t)(int32_t)(int8_t)v
                    : (d->size == 2u) ? (uint32_t)(int32_t)(int16_t)v
                                      : v;
            }
            if (d->rd == R_PC) {
                exit_interwork(t, K(b, v));
            } else {
                emu_ir_put(b, d->rd, K(b, v));
            }
            return true;
        }
        addr = K(b, at);
        break;
    }
    case ARMV7M_ADDR_IMM: {
        const uint32_t off = d->add ? d->imm : (0u - d->imm);

        if (d->rn == R_PC || (d->wback && d->rn == d->rd)) {
            return false;
        }
        addr = G(b, d->rn);
        if (d->index) {
            disp = off;
        }
        if (d->wback) {
            wb = ADDK(b, addr, off);
        }
        break;
    }
    case ARMV7M_ADDR_REG:
        if (d->rn == R_PC || d->rm == R_PC) {
            return false;
        }
        addr = OP(b, EMU_IR_ADD, G(b, d->rn), SHL(b, G(b, d->rm), d->shift_n));
        break;
    default:
        return false;
    }

    if (load) {
        const uint16_t v = emu_ir_emit(b, EMU_IR_LOAD, spec, addr, NOT, disp, 0u);

        if (wb != NOT) {
            emu_ir_put(b, d->rn, wb);
        }
        if (d->rd == R_PC) {
            exit_interwork(t, v);
        } else {
            emu_ir_put(b, d->rd, v);
        }
    } else {
        (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(d->size, 0u), addr,
                          G(b, d->rd), disp, 0u);
        if (wb != NOT) {
            emu_ir_put(b, d->rn, wb);
        }
    }
    return true;
}

static bool lower_dual(T *t, const I *d)
{
    B *const b = t->b;
    const bool load = d->op == ARMV7M_OP_LDRD;
    const uint8_t spec = (uint8_t)(EMU_IR_MEM_AUX(4u, 0u) | A7_MEM_ALIGNED);
    const uint32_t off = d->add ? d->imm : (0u - d->imm);
    uint16_t addr;
    uint16_t acc;
    uint32_t disp;

    if (t->in_it || d->rd >= 15u || d->ra >= 15u) {
        return false;
    }
    if (d->addr == ARMV7M_ADDR_LITERAL) {
        const uint32_t base = (t->pc + 4u) & ~3u;

        if (d->wback || !d->index) {
            return false;
        }
        addr = K(b, base + off);
        disp = 0u;
    } else {
        if (d->rn == R_PC || (d->wback && (d->rn == d->rd || d->rn == d->ra))) {
            return false;
        }
        addr = G(b, d->rn);
        disp = d->index ? off : 0u;
    }
    /* The offset is a multiple of four, so the base decides alignment. */
    acc = aligned_base(t, addr);
    if (load) {
        const uint16_t v1 = emu_ir_emit(b, EMU_IR_LOAD, spec, acc, NOT, disp, 0u);
        const uint16_t v2 =
            emu_ir_emit(b, EMU_IR_LOAD, spec, acc, NOT, disp + 4u, 0u);

        if (d->wback) {
            emu_ir_put(b, d->rn, ADDK(b, addr, off));
        }
        emu_ir_put(b, d->rd, v1);
        emu_ir_put(b, d->ra, v2);
    } else {
        (void)emu_ir_emit(b, EMU_IR_STORE, spec, acc, G(b, d->rd), disp, 0u);
        (void)emu_ir_emit(b, EMU_IR_STORE, spec, acc, G(b, d->ra), disp + 4u,
                          0u);
        if (d->wback) {
            emu_ir_put(b, d->rn, ADDK(b, addr, off));
        }
    }
    return true;
}

/*
 * LDM, STM, PUSH and POP, in the interpreter's order: every load into a
 * value first, then the base, then the registers -- so a fault part way
 * leaves the registers alone -- and the stores one at a time, with
 * whatever landed before a fault left where it landed.
 */
static bool lower_multi(T *t, const I *d)
{
    B *const b = t->b;
    const bool load = d->op == ARMV7M_OP_LDM || d->op == ARMV7M_OP_LDMDB ||
                      d->op == ARMV7M_OP_POP;
    const bool before = d->op == ARMV7M_OP_LDMDB || d->op == ARMV7M_OP_STMDB ||
                        d->op == ARMV7M_OP_PUSH;
    const uint8_t spec = (uint8_t)(EMU_IR_MEM_AUX(4u, 0u) | A7_MEM_ALIGNED);
    const uint32_t list = d->imm & 0xFFFFu;
    const uint32_t count = (uint32_t)__builtin_popcount(list);
    const uint32_t first = before ? (0u - 4u * count) : 0u;
    const uint32_t final = before ? (0u - 4u * count) : 4u * count;
    uint16_t vals[16];
    uint16_t base;
    uint16_t acc;
    uint32_t k = 0u;

    if (t->in_it || d->rn >= 15u || count == 0u || (list & (1u << 13)) != 0u) {
        return false;
    }
    if ((list & (1u << 15)) != 0u && (!load || !thread_mode(t))) {
        return false;
    }
    /* A dozen loads and their writes: leave the helper the long lists
     * where IR room is short. */
    if (count > 9u && EMU_IR_MAX_INSNS < 1024u) {
        return false;
    }
    base = G(b, d->rn);
    acc = aligned_base(t, base);
    for (uint32_t i = 0u; i < 16u; i++) {
        if ((list & (1u << i)) == 0u) {
            continue;
        }
        if (load) {
            vals[i] = emu_ir_emit(b, EMU_IR_LOAD, spec, acc, NOT,
                                  first + 4u * k, 0u);
        } else {
            (void)emu_ir_emit(b, EMU_IR_STORE, spec, acc, G(b, i),
                              first + 4u * k, 0u);
        }
        k++;
    }
    if (d->wback) {
        emu_ir_put(b, d->rn, ADDK(b, base, final));
    }
    if (load) {
        for (uint32_t i = 0u; i < 15u; i++) {
            if ((list & (1u << i)) != 0u) {
                emu_ir_put(b, i, vals[i]);
            }
        }
        if ((list & (1u << 15)) != 0u) {
            exit_interwork(t, vals[15]);
        }
    }
    return true;
}

static bool lower_branch(T *t, const I *d)
{
    B *const b = t->b;

    switch (d->op) {
    case ARMV7M_OP_B:
        if (t->in_it) {
            /* Legal only as the last instruction of the block, where
             * ITSTATE is zero whichever way the condition goes. */
            if (!t->it_last) {
                return false;
            }
            exit_if(t, t->cond, t->pc + 4u + d->imm);
            return true;
        }
        exit_const(t, t->pc + 4u + d->imm);
        return true;
    case ARMV7M_OP_BCC:
        if (t->in_it) {
            return false;
        }
        exit_cond(t, d->cond, t->pc + 4u + d->imm);
        return true;
    case ARMV7M_OP_CBZ:
    case ARMV7M_OP_CBNZ:
        if (t->in_it || d->rn >= 8u) {
            return false;
        }
        retire(t);
        (void)emu_ir_emit(b, EMU_IR_EXIT_IF,
                          (d->op == ARMV7M_OP_CBZ) ? EMU_IR_C_EQ : EMU_IR_C_NE,
                          G(b, d->rn), K(b, 0u), t->pc + 4u + d->imm, 0u);
        return true;
    case ARMV7M_OP_BL:
        if (t->in_it) {
            return false;
        }
        emu_ir_put(b, 14u, K(b, (t->pc + 4u) | 1u));
        exit_const(t, t->pc + 4u + d->imm);
        return true;
    case ARMV7M_OP_TBB:
    case ARMV7M_OP_TBH: {
        /*
         * A switch: load an entry, double it, add it to the pc. The
         * table is nearly always just after the instruction, in flash,
         * so the load takes the checked path -- and is still far
         * cheaper than the interpreter was, which is what a compiler's
         * every dense `switch` cost.
         */
        const bool half = d->op == ARMV7M_OP_TBH;
        uint16_t idx;
        uint16_t e;

        if (t->in_it || d->rm >= 15u || d->rn == 13u) {
            return false;
        }
        idx = G(b, d->rm);
        e = emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(half ? 2u : 1u, 0u),
                        OP(b, EMU_IR_ADD, rreg(t, d->rn),
                           half ? SHL(b, idx, 1u) : idx),
                        NOT, 0u, 0u);
        retire(t);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u,
                          OP(b, EMU_IR_ADD, K(b, t->pc + 4u), SHL(b, e, 1u)), NOT,
                          0u, 0u);
        t->ends = true;
        return true;
    }
    case ARMV7M_OP_BX:
    case ARMV7M_OP_BLX: {
        uint16_t v;

        if (t->in_it || !thread_mode(t) || d->rm > 15u) {
            return false;
        }
        /* The target first: `blx lr` jumps to the old lr, not the new. */
        v = rreg(t, d->rm);
        if (d->op == ARMV7M_OP_BLX) {
            emu_ir_put(b, 14u, K(b, (t->pc + t->len) | 1u));
        }
        exit_interwork(t, v);
        return true;
    }
    default:
        return false;
    }
}

/*
 * Lower one instruction to IR, or say it cannot be.
 *
 * **Returning false must leave nothing behind**: a lowering may have
 * emitted operands before reaching the field that tells it to decline,
 * and the caller rewinds the block to where the instruction began.
 */
static bool lower_native(T *t, const I *d)
{
    switch (d->op) {
    case ARMV7M_OP_AND:
    case ARMV7M_OP_BIC:
    case ARMV7M_OP_ORR:
    case ARMV7M_OP_ORN:
    case ARMV7M_OP_EOR:
    case ARMV7M_OP_ADD:
    case ARMV7M_OP_ADC:
    case ARMV7M_OP_SBC:
    case ARMV7M_OP_SUB:
    case ARMV7M_OP_RSB:
    case ARMV7M_OP_MOV:
    case ARMV7M_OP_MVN:
    case ARMV7M_OP_TST:
    case ARMV7M_OP_TEQ:
    case ARMV7M_OP_CMN:
    case ARMV7M_OP_CMP:
        return lower_dp(t, d);

    case ARMV7M_OP_LSL:
    case ARMV7M_OP_LSR:
    case ARMV7M_OP_ASR:
    case ARMV7M_OP_ROR:
    case ARMV7M_OP_RRX:
        return lower_shift(t, d);

    case ARMV7M_OP_MUL:
    case ARMV7M_OP_MLA:
    case ARMV7M_OP_MLS:
    case ARMV7M_OP_UMULL:
    case ARMV7M_OP_SMULL:
    case ARMV7M_OP_UMLAL:
    case ARMV7M_OP_SMLAL:
    case ARMV7M_OP_SMULBB:
    case ARMV7M_OP_SMULBT:
    case ARMV7M_OP_SMULTB:
    case ARMV7M_OP_SMULTT:
    case ARMV7M_OP_SMLABB:
    case ARMV7M_OP_SMLABT:
    case ARMV7M_OP_SMLATB:
    case ARMV7M_OP_SMLATT:
    case ARMV7M_OP_UDIV:
    case ARMV7M_OP_SDIV:
        return lower_mul(t, d);

    case ARMV7M_OP_MOVW:
    case ARMV7M_OP_MOVT:
    case ARMV7M_OP_ADR:
    case ARMV7M_OP_SXTB:
    case ARMV7M_OP_SXTH:
    case ARMV7M_OP_UXTB:
    case ARMV7M_OP_UXTH:
    case ARMV7M_OP_SXTAB:
    case ARMV7M_OP_SXTAH:
    case ARMV7M_OP_UXTAB:
    case ARMV7M_OP_UXTAH:
    case ARMV7M_OP_REV:
    case ARMV7M_OP_REV16:
    case ARMV7M_OP_REVSH:
    case ARMV7M_OP_CLZ:
    case ARMV7M_OP_UBFX:
    case ARMV7M_OP_SBFX:
    case ARMV7M_OP_BFI:
    case ARMV7M_OP_BFC:
        return lower_misc(t, d);

    case ARMV7M_OP_LDR:
    case ARMV7M_OP_LDRB:
    case ARMV7M_OP_LDRH:
    case ARMV7M_OP_LDRSB:
    case ARMV7M_OP_LDRSH:
    case ARMV7M_OP_STR:
    case ARMV7M_OP_STRB:
    case ARMV7M_OP_STRH:
        return lower_ldst(t, d);

    case ARMV7M_OP_LDRD:
    case ARMV7M_OP_STRD:
        return lower_dual(t, d);

    case ARMV7M_OP_LDM:
    case ARMV7M_OP_LDMDB:
    case ARMV7M_OP_STM:
    case ARMV7M_OP_STMDB:
    case ARMV7M_OP_PUSH:
    case ARMV7M_OP_POP:
        return lower_multi(t, d);

    case ARMV7M_OP_B:
    case ARMV7M_OP_BCC:
    case ARMV7M_OP_BL:
    case ARMV7M_OP_BX:
    case ARMV7M_OP_BLX:
    case ARMV7M_OP_CBZ:
    case ARMV7M_OP_CBNZ:
    case ARMV7M_OP_TBB:
    case ARMV7M_OP_TBH:
        return lower_branch(t, d);

    /* The hints do nothing here, as they do nothing in the interpreter. */
    case ARMV7M_OP_NOP:
    case ARMV7M_OP_YIELD:
    case ARMV7M_OP_SEV:
    case ARMV7M_OP_DSB:
    case ARMV7M_OP_DMB:
    case ARMV7M_OP_PLD:
    case ARMV7M_OP_PLI:
        return true;

    /*
     * ISB is where an interrupt the guest has just pended must be taken.
     *
     * The interpreter looks for one before every instruction; a block
     * looks only when it returns to the dispatcher, and a chained exit
     * does not even do that. So `str r1, [ICSR]; dsb; isb; add sp, #4`
     * ran the ADD first and the PendSV found the stack somewhere else --
     * two of the board's system tests, both about the alignment of the
     * frame, and nothing else in fifty-four.
     *
     * The architecture promises exactly this much and no more: the
     * instruction after a write that pends an exception may or may not
     * run first, and the one after an ISB will not. So the block ends
     * here, through the dispatcher: the target is given as a value
     * rather than a constant precisely so that the exit cannot be
     * chained past the check.
     */
    case ARMV7M_OP_ISB:
        if (t->in_it) {
            return false;
        }
        retire(t);
        (void)emu_ir_emit(t->b, EMU_IR_EXIT, 0u, K(t->b, t->pc + t->len), NOT,
                          0u, 0u);
        t->ends = true;
        return true;

    default:
        return false;
    }
}

/*
 * Must the block end in front of this instruction?
 *
 * These are the ones that can change what a block assumes, or that the
 * architecture defines as an exception. **Declining them is a design
 * decision, not a gap**: it makes the interpreter fallback the single
 * place the masks, CONTROL and the MPU can be written from, which is
 * where the context and generation are re-derived.
 */
static bool must_decline(const I *d)
{
    switch (d->op) {
    case ARMV7M_OP_UNDEF:
    case ARMV7M_OP_UNPRED:
    case ARMV7M_OP_NOCP:
    case ARMV7M_OP_SVC:
    case ARMV7M_OP_BKPT:
    case ARMV7M_OP_UDF:
    case ARMV7M_OP_CPSIE:
    case ARMV7M_OP_CPSID:
    case ARMV7M_OP_MSR:
    case ARMV7M_OP_MRS:
    case ARMV7M_OP_WFI:
    case ARMV7M_OP_WFE:
    case ARMV7M_OP_IT:
        return true;
    default:
        return false;
    }
}

/*
 * After the interpreter has run this instruction, is the next one
 * certainly the next address?
 *
 * **The default is no.** An instruction that may write the pc ends the
 * block and the pc is read back; saying yes for one that could branch
 * would run the instructions after it as though it had not. So the list
 * is of what is known to fall through, and anything nobody has thought
 * about leaves through the dispatcher -- slower, and right.
 */
static bool helper_falls_through(const I *d)
{
    const uint32_t op = d->op;

    if (op >= ARMV7M_OP_VADD) {
        /* The FPU, except a move into a core register named as the pc. */
        const bool to_core =
            (op == ARMV7M_OP_VMOV_CORE || op == ARMV7M_OP_VMOV_CORE2 ||
             op == ARMV7M_OP_VMOV_SCALAR) &&
            d->imm != 0u;

        return !(to_core && (d->rd == R_PC || d->ra == R_PC));
    }
    switch (op) {
    case ARMV7M_OP_CLREX:
    case ARMV7M_OP_TST:
    case ARMV7M_OP_TEQ:
    case ARMV7M_OP_CMN:
    case ARMV7M_OP_CMP:
        return true;
    case ARMV7M_OP_STR:
    case ARMV7M_OP_STRB:
    case ARMV7M_OP_STRH:
    case ARMV7M_OP_STRT:
    case ARMV7M_OP_STRBT:
    case ARMV7M_OP_STRHT:
    case ARMV7M_OP_STRD:
    case ARMV7M_OP_STM:
    case ARMV7M_OP_STMDB:
    case ARMV7M_OP_PUSH:
        return true;
    case ARMV7M_OP_LDM:
    case ARMV7M_OP_LDMDB:
    case ARMV7M_OP_POP:
        return (d->imm & 0x8000u) == 0u;
    default:
        break;
    }
    /* Data processing, multiplies, saturation, bit fields, extension,
     * and the loads: wherever the destination is not the pc. */
    if ((op >= ARMV7M_OP_AND && op <= ARMV7M_OP_PARALLEL) ||
        (op >= ARMV7M_OP_LDR && op <= ARMV7M_OP_STREXH)) {
        return d->rd != R_PC && d->ra != R_PC;
    }
    return false;
}

/* The interpreter, for this instruction, from inside the block. */
static void lower_helper(T *t, const I *d)
{
    B *const b = t->b;

    (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, K(b, t->insn), K(b, t->pc),
                      A7_HELPER_STEP, 0u);
    if (!helper_falls_through(d)) {
        retire(t);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, G(b, R_PC), NOT, 0u, 0u);
        t->ends = true;
    }
}

static bool fetch(const armv7m_cpu_t *c, uint32_t pc, uint16_t *w0,
                  uint16_t *w1, uint32_t *len)
{
    *w1 = 0u;
    if (!armv7m_fetch_probe(c, pc, w0)) {
        return false;
    }
    *len = armv7m_insn_len(*w0);
    return *len == 2u || armv7m_fetch_probe(c, pc + 2u, w1);
}

static void set_pc(B *b, uint32_t pc)
{
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, NOT, NOT, pc, 0u);
}

/*
 * An IT block, whole.
 *
 * Every member is lowered with its writes made conditional, so the
 * result is straight-line code and ITSTATE never has to exist at run
 * time: nothing inside can fault, call out or be interrupted, so nothing
 * can observe it, and at the far end it is zero either way.
 *
 * That only works for members that are *pure* -- registers and flags --
 * plus a direct branch in the last position. Anything else, and the
 * whole block is given back: the IT instruction is declined and the
 * interpreter runs all of it. Half an IT block lowered is not an option,
 * because the other half would begin inside one.
 *
 * Returns the instructions folded in, counting the IT itself, or 0.
 */
static uint32_t lower_it(T *t, const I *it, uint32_t room, uint32_t *next_pc,
                         bool *ends)
{
    B *const b = t->b;
    const uint32_t mark = b->count;
    const uint32_t mark_temp = b->next_temp;
    uint32_t state = ((uint32_t)it->cond << 4) | (it->imm & 15u);
    uint32_t cur = t->pc + 2u;
    uint32_t n = 0u;

    /* Members: four less the position of the lowest set mask bit. */
    for (uint32_t low = 0u; low < 4u; low++) {
        if ((it->imm >> low) & 1u) {
            n = 4u - low;
            break;
        }
    }
    if (n == 0u || 1u + n > room) {
        return 0u;
    }

    (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
    set_pc(b, cur);

    for (uint32_t i = 0u; i < n; i++) {
        uint16_t w0;
        uint16_t w1;
        uint32_t len;
        I d;
        T m = *t;

        if (!fetch(t->c, cur, &w0, &w1, &len)) {
            goto give_back;
        }
        armv7m_decode(w0, w1, &d);
        m.pc = cur;
        m.len = len;
        m.insn = (uint32_t)w0 | ((uint32_t)w1 << 16);
        m.in_it = true;
        m.it_last = (i + 1u == n);
        m.cond = cond01(b, state >> 4);
        m.cmask = NOT;
        m.ends = false;
        m.counted = false;
        if (must_decline(&d) || !lower_native(&m, &d) || m.ends) {
            goto give_back;
        }
        cur += len;
        if (!m.counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
        }
        set_pc(b, cur);
        /* ITAdvance. */
        state = ((state & 7u) == 0u) ? 0u
                                     : ((state & 0xE0u) | ((state << 1) & 0x1Fu));
    }
    if (b->overflow) {
        goto give_back;
    }
    g_stats.native += 1u + n;
    *next_pc = cur;
    *ends = false;
    return 1u + n;

give_back:
    b->count = mark;
    b->next_temp = mark_temp;
    return 0u;
}

/* Remember that translated code came from memory the guest can write. */
static void smc_mark(armv7m_cpu_t *c, uint32_t lo, uint32_t hi)
{
    const emu_region_t *const r = emu_bus_find(c->bus, lo);
    const uint32_t window = 32u * 4096u *
                            (uint32_t)(sizeof(c->smc_bits) / sizeof(c->smc_bits[0]));

    if (r == NULL || (r->perm & EMU_PERM_W) == 0u) {
        return; /* flash: nothing can change it underneath a block */
    }
    lo &= ~0xFFFu;
    hi = (hi + 0xFFFu) & ~0xFFFu;
    c->smc_watch = 1u;
    if (c->smc_span == 0u) {
        c->smc_base = lo & ~(window - 1u);
        c->smc_lo = lo;
        c->smc_span = hi - lo;
        c->smc_wide = false;
        memset(c->smc_bits, 0, sizeof(c->smc_bits));
    } else {
        const uint32_t old_hi = c->smc_lo + c->smc_span;
        const uint32_t new_lo = (lo < c->smc_lo) ? lo : c->smc_lo;
        const uint32_t new_hi = (hi > old_hi) ? hi : old_hi;

        c->smc_lo = new_lo;
        c->smc_span = new_hi - new_lo;
    }
    if (c->smc_lo < c->smc_base || c->smc_lo + c->smc_span > c->smc_base + window) {
        c->smc_wide = true;
        return;
    }
    for (uint32_t a = lo; a < hi; a += 4096u) {
        const uint32_t page = (a - c->smc_base) >> 12;

        c->smc_bits[page >> 5] |= 1u << (page & 31u);
    }
}

uint32_t armv7m_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;
    const uint32_t ctx = (uint32_t)c->jit_ctx;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emu_ir_reset(b);
    b->start_pc = pc;
    b->has_fast = a7_fast_mem(c, &b->fast);

    /*
     * No block for a core that is not in Thumb state, or is part way
     * through an IT block; and none at an exception-return address,
     * which is not an address.
     */
    if ((ctx & ARMV7M_CTX_T) == 0u || (ctx & ARMV7M_CTX_IT) != 0u ||
        ((ctx & ARMV7M_CTX_HANDLER) != 0u &&
         (pc & ARMV7M_EXC_RETURN_MASK) == ARMV7M_EXC_RETURN_MASK)) {
        return 0u;
    }

    while (count < A7_MAX_BLOCK_INSNS && !b->overflow &&
           b->count + A7_IR_HEADROOM <= EMU_IR_MAX_INSNS &&
           b->next_temp + A7_TEMP_HEADROOM <= EMU_IR_MAX_TEMPS) {
        uint16_t w0;
        uint16_t w1;
        uint32_t len;
        I d;
        T t;

        if (!fetch(c, cur, &w0, &w1, &len)) {
            break;
        }
        armv7m_decode(w0, w1, &d);

        memset(&t, 0, sizeof(t));
        t.b = b;
        t.c = c;
        t.pc = cur;
        t.len = len;
        t.insn = (uint32_t)w0 | ((uint32_t)w1 << 16);
        t.ctx = ctx;
        t.cond = NOT;
        t.cmask = NOT;

        if (d.op == ARMV7M_OP_IT) {
            bool ends = false;
            uint32_t next = cur;
            const uint32_t n =
                lower_it(&t, &d, A7_MAX_BLOCK_INSNS - count, &next, &ends);

            if (n == 0u) {
                g_stats.declined++;
                g_stats.declined_by_op[d.op]++;
                break;
            }
            count += n;
            cur = next;
            continue;
        }
        if (must_decline(&d)) {
            g_stats.declined++;
            g_stats.declined_by_op[d.op]++;
            break;
        }

        {
            const uint32_t mark = b->count;
            const uint32_t mark_temp = b->next_temp;

            if (lower_native(&t, &d)) {
                g_stats.native++;
            } else {
                /* Whatever the attempt emitted, discard: a half-lowered
                 * instruction does not fail, it computes something else. */
                b->count = mark;
                b->next_temp = mark_temp;
                t.ends = false;
                t.counted = false;
                lower_helper(&t, &d);
                g_stats.helper++;
                g_stats.helper_by_op[d.op]++;
            }
        }

        cur += len;
        count++;
        if (!t.counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
        }
        if (t.ends) {
            break;
        }
        /*
         * pc after every instruction, so that a fault, an interrupt or
         * the interpreter picking up where the block stopped sees the
         * right address -- and so that the *next* instruction's fault
         * records its own.
         */
        set_pc(b, cur);
    }

    if (b->overflow || count == 0u) {
        return 0u;
    }
    b->guest_insns = count;
    smc_mark(c, pc, cur);
    return count;
}

/* ------------------------------------------------------------------ */
/* What the host's jit.c needs from this frontend                      */
/* ------------------------------------------------------------------ */

/*
 * An interrupt, between blocks -- the same test the interpreter makes
 * between instructions, through the same functions.
 *
 * **Not at an exception-return address.** The interpreter recognises
 * EXC_RETURN before it looks for a pending exception, and this has to
 * agree: taking one here would stack 0xFFFFFFF9 as a return address.
 * The dispatcher finds no block there, the interpreter performs the
 * return, and its own loop takes whatever is pending afterwards.
 */
static bool a7_take_irq(emu_cpu_t *cpu)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;
    bool preempts = false;
    uint32_t exc;

    if (armv7m_handler_mode(c) &&
        (c->r[R_PC] & ARMV7M_EXC_RETURN_MASK) == ARMV7M_EXC_RETURN_MASK) {
        return false; /* and irq_maybe stays set: the interpreter is next */
    }
    /* Cleared before the look, not after: a line raised by a host
     * interrupt during it must not be lost to the clear. */
    c->irq_maybe = false;
    exc = armv7m_pending_exc(c, &preempts);
    if (!preempts) {
        return false;
    }
    armv7m_flags_pack(c); /* entry stacks xPSR */
    (void)armv7m_take(c, exc, c->r[R_PC]);
    armv7m_flags_unpack(c);
    c->jit_ctx = armv7m_jit_ctx(c);
    c->irq_maybe = true;
    return true;
}

/*
 * Time, at block granularity. The interpreter ticks SysTick once per
 * instruction; a block reports how many it retired and they are applied
 * here. The cost is latency -- the interrupt arrives at the end of the
 * block that passed zero -- and a guest that reads SYST_CVR in a block
 * sees the value as of the block's start.
 *
 * **Counting down by n is the whole of it unless the counter reaches
 * zero**, and it nearly never does: a block is a few dozen instructions
 * and a tick period is thousands. So that case is one subtraction, and
 * only a block that crosses zero is walked an instruction at a time --
 * which keeps the reload, the count flag and a period shorter than a
 * block exactly as the interpreter has them.
 *
 * Walking every block that way was 16% of all host instructions on
 * CoreMark: a loop per block, to decrement a number.
 */
static void a7_count(emu_cpu_t *cpu, uint32_t n)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;

    c->retired += n;
    if ((c->systick_ctrl & 1u) == 0u) {
        return;
    }
    if (c->systick_value > n) {
        c->systick_value -= n;
        return;
    }
    for (uint32_t i = 0u; i < n; i++) {
        armv7m_systick_tick(c, 1u);
    }
}

/*
 * The interpreter, as the framework's fallback. It reads and writes the
 * flags in xPSR, so they are packed on the way in; a7_after_interp,
 * which the framework calls once the batch is done, unpacks them again.
 */
static emu_run_reason_t a7_interp_run(emu_cpu_t *cpu, uint32_t budget,
                                      uint32_t *retired)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;

    armv7m_flags_pack(c);
    return armv7m_run(c, budget, retired);
}

static const emu_backend_t a7_interp_for_jit = {
    .name = "interp",
    .run = a7_interp_run,
};

/* The interpreter ran: whatever it changed, the key and the flags follow. */
static void a7_after_interp(emu_cpu_t *cpu)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;

    armv7m_flags_unpack(c);
    c->jit_ctx = armv7m_jit_ctx(c);
    c->irq_maybe = true;
}

static void a7_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)cpu;

    armv7m_flags_unpack(c);
    c->jit_ctx = armv7m_jit_ctx(c);
    c->irq_maybe = true;
    out->irq_pending = &c->irq_maybe;
    out->pc = &c->r[R_PC];
    out->state = (const uint8_t *)&c->state;
    out->generation = &c->jit_gen;
    out->context = &c->jit_ctx;
}

const emu_ir_frontend_t armv7m_ir_frontend = {
    .name = "armv7m",
    .translate = armv7m_ir_translate,
    .target = &armv7m_ir_target,
    .bind = a7_bind,
    .interp = &a7_interp_for_jit,
    .is_idle = NULL,
    .wake = NULL,
    .take_irq = a7_take_irq,
    .count = a7_count,
    .after_interp = a7_after_interp,
    .code_bytes = EMU_HOST_JIT_CODE_BYTES,
    /* r0-r15 and xPSR. */
    .diff_state_bytes = (uint32_t)offsetof(armv7m_cpu_t, xpsr) + 4u,
};

#endif /* EMU_HAVE_JIT */
