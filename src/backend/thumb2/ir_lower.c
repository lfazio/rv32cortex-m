/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/thumb2/ir_lower.c - Lowering the IR to Thumb-2.
 *
 * The ARMv7E-M half of frontend -> IR -> optimisation -> backend. It
 * knows the host and nothing about any guest; everything guest-specific
 * arrives through emu_ir_target_t.
 *
 * Register convention
 * -------------------
 *   r4   the emu_cpu_t pointer, callee-saved so it survives a helper
 *   r5   the retired count this block returns
 *   sp   the temp frame; every IR value has exactly one home in it
 *   r0-r3, r12   scratch, and the AAPCS argument registers
 *
 *   r11  the guest RAM window's host address, loaded at a block's
 *        chain entry when it has the window: each inlined access was a
 *        literal load of it, then the access through it
 *
 * Every temp has a home in the frame, and r6-r10 -- everything AAPCS
 * makes callee-saved that is not already spoken for -- are handed out on
 * top of that by emu_ir_regalloc. A temp with a register never touches
 * its slot; a temp without one behaves exactly as everything did before
 * there was an allocator.
 *
 * Five registers where x86-64 has four, and they cost less to save: a
 * whole list goes in one PUSH, so the second and later ones are free
 * where on x86-64 each is its own instruction. r11 was the sixth until
 * the memory stub needed a home (it holds the RAM's address now), and
 * was the cheapest to give up: on
 * x86-64 the same allocator measured 3.6% of CoreMark's emitted code for
 * the first register and 0.6% for the fourth, which says the live sets
 * in these blocks are small -- as they would be at 4.12 guest
 * instructions a block.
 *
 * Encodings are from ARM DDI 0403E (docs/arm/). Where a 16-bit form
 * exists it is *not* used: the register numbers here are frequently
 * above r7, and this project has already been bitten by a 16-bit
 * data-processing encoding silently becoming a different instruction
 * when handed a high register.
 */

#include <string.h>
#include "emu/emu_ir.h"
#include "emu/emu_ir_bytes.h"
#include "emu/emu_thumb2.h"

#if defined(EMU_HOST_JIT_THUMB2)

static EMU_PER_CORE uint16_t g_ntemps;

#define SCRATCH_ADDR ((uint16_t)(g_ntemps))
#define SCRATCH_VAL ((uint16_t)(g_ntemps + 1u))
/* One more for the caller's FPSCR, which this block borrows. */
#define SCRATCH_FPSCR ((uint16_t)(g_ntemps + 2u))

static uint32_t slot(uint16_t n)
{
    return (uint32_t)n * 4u;
}

/*
 * Where the allocator put each temp, and how many registers that took.
 *
 * A temp with a register never touches its frame slot: the slot still
 * exists and is simply unused, which is what stops the two homes from
 * ever disagreeing. Reaching one is a MOV.W against an LDR.W -- four
 * bytes either way here, unlike x86-64 -- so what this saves on this
 * host is the *pair*, the store and the reload, not the width of one
 * access.
 */
static EMU_PER_CORE uint8_t g_reg[EMU_IR_MAX_TEMPS];
static EMU_PER_CORE uint32_t g_nsaved;

/*
 * The register holding temp `n`, or -1 for one that lives in the frame.
 * The bound is not paranoia: SCRATCH_ADDR and SCRATCH_VAL are slots past
 * the end of the block's temps, so they index past what the allocator
 * filled in and must always answer "in the frame".
 */
static int phys(uint16_t n)
{
    if (n >= g_ntemps || n >= EMU_IR_MAX_TEMPS) {
        return -1;
    }
    return (g_reg[n] == EMU_IR_NO_REG) ? -1 : (int)t2_alloc_regs[g_reg[n]];
}

void ld_slot(uint32_t rt, uint16_t n)
{
    t2_ldr_imm(rt, 13u, slot(n)); /* [sp, #off] */
}

/*
 * What r0 still holds from the previous instruction, if anything.
 *
 * A dependent pair otherwise emits a store and then an immediate reload
 * of the same slot, and a quarter to a third of adjacent instruction
 * pairs in this project's guests are dependent. This declines the
 * reload when the value is already there.
 *
 * Not a register allocator: the store still happens, so every slot stays
 * valid and no liveness analysis is needed. It costs nothing -- no setup
 * instruction, no extra register held across a call -- unlike the
 * guest-register cache tried on this host before, which measured 15.5%
 * slower.
 *
 * It matters more here than on x86-64. This is the host where code size
 * sets performance: the cache is 12 KB, CoreMark's translated working
 * set does not fit, and bytes removed are compactions avoided.
 *
 * The window is one instruction wide and closes at the first load of the
 * next. Anything reached by a branch, and anything after a call, starts
 * holding nothing -- which falls out of clearing at the top of every
 * instruction and only re-establishing it at a final store.
 */
/*
 * A store that need not be emitted: the value has exactly one reader,
 * that reader is the next live instruction, and it takes its left
 * operand from r0 -- which the reload elision hands over directly. Both
 * halves matter; `uses == 1` alone would allow a reader further down
 * the block with r0 clobbered in between.
 */
static EMU_PER_CORE uint16_t g_dead_store;

/* Ops whose first act is ld_operand(r0, in->a), so the elision applies. */
static bool reads_a_in_r0(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_MOV:
    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR:
    case EMU_IR_ADDI:
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
    case EMU_IR_XORI:
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_NOT:
    case EMU_IR_NEG:
    case EMU_IR_BSWAP32:
    case EMU_IR_BSWAP16:
    case EMU_IR_HSWAP:
    case EMU_IR_CLZ:
    case EMU_IR_CTZ:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16:
    case EMU_IR_PUT:
    case EMU_IR_SETPC:
    case EMU_IR_EXIT:
        return true;
    default:
        return false;
    }
}

static EMU_PER_CORE uint16_t g_r0_holds;
static EMU_PER_CORE uint16_t g_r0_avail;
static EMU_PER_CORE bool g_r0_first;

void st_slot(uint32_t rt, uint16_t n)
{
    const int p = phys(n);

    if (p >= 0) {
        /*
         * The register is this temp's home, so the write happens even
         * where a dead-store test would skip it -- a later instruction
         * reads the register, not the slot. In practice the two never
         * collide, because the allocator declines exactly the temps the
         * dead-store rule claims; the ordering here is what makes that a
         * coincidence rather than a dependency.
         */
        if (p != (int)rt) {
            t2_mov((uint32_t)p, rt);
        }
        if (rt == T2_R0) {
            g_r0_holds = n;
        }
        return;
    }
    if (rt == T2_R0 && n == g_dead_store) {
        g_r0_holds = n; /* nothing reads the slot */
        return;
    }
    t2_str_imm(rt, 13u, slot(n));

    if (rt == T2_R0) {
        g_r0_holds = n;
    } else if (n == g_r0_holds) {
        g_r0_holds = EMU_IR_NO_TEMP;
    }
}

/*
 * Where to read an operand from, and where to write a result.
 *
 * These are what make allocation worth anything on this host. Replacing
 * a frame access with a register move saves nothing here -- LDR.W and
 * MOV.W are four bytes each, unlike x86-64 where the same substitution
 * is eight bytes against three -- and the first version of this backend
 * did exactly that and measured *worse*: 6040 translations against 5943
 * and 410 buffer overflows, from the wider PUSH and the load detour with
 * nothing on the other side of the ledger.
 *
 * What pays is using the register in place. Thumb-2 is three-address, so
 * an ADD whose two operands and destination are all allocated is one
 * instruction where the frame needs four -- two loads, the add and a
 * store, sixteen bytes down to four.
 *
 * use_reg emits the load only when the temp has no register; def_reg
 * names where to compute, and the st_slot that follows becomes a no-op
 * when that was the temp's own register.
 */
static uint32_t use_reg(uint16_t n, uint32_t scratch);
static uint32_t def_reg(uint16_t n, uint32_t scratch);
static uint32_t use_reg_fwd(uint16_t n, uint32_t scratch);

/* Load an operand, tolerating EMU_IR_NO_TEMP so callers need not check. */
void ld_operand(uint32_t rt, uint16_t n)
{
    /*
     * The elision is consulted before the allocation, because a value
     * both in a register and still in r0 is cheaper taken from r0 -- no
     * instruction at all against a MOV.W.
     */
    if (rt == T2_R0 && g_r0_first) {
        g_r0_first = false;
        if (n != EMU_IR_NO_TEMP && n == g_r0_avail) {
            return; /* already there */
        }
    }

    const int p = phys(n);

    if (p >= 0) {
        if (p != (int)rt) {
            t2_mov(rt, (uint32_t)p);
        }
        return;
    }
    if (n == EMU_IR_NO_TEMP) {
        t2_imm32(rt, 0u);
        return;
    }
    ld_slot(rt, n);
}

/*
 * A constant whose one reader is a compare's right operand (EXIT_IF,
 * SETCC), and that CMP or CMN takes as an immediate: the compare uses
 * it in place and the CONST is not emitted. Index + 1 of the CONST, 0
 * for none. Taken from the IR's `uses`, the count the allocator trusts.
 *
 * What it replaces: `movw r8, #0` then `cmp r7, r8` for every RISC-V
 * beqz/bnez -- a register and an instruction for a constant ARM encodes
 * in the compare -- in the F746's hottest CoreMark block.
 */
static EMU_PER_CORE uint16_t g_kcmp[EMU_IR_MAX_TEMPS];
static EMU_PER_CORE const emu_ir_block_t *g_kcmp_block;

static bool kcmp_folded(uint16_t t)
{
    return t < EMU_IR_MAX_TEMPS && t < g_ntemps && g_kcmp[t] != 0u;
}

static bool kcmp_reader(const emu_ir_insn_t *in, uint16_t k)
{
    if (in->b != k || in->a == k || in->c == k) {
        return false;
    }
    return in->op == (uint8_t)EMU_IR_EXIT_IF ||
           (in->op == (uint8_t)EMU_IR_SETCC && in->aux != (uint8_t)EMU_IR_C_ALWAYS);
}

static bool kcmp_encodes(uint32_t v)
{
    uint16_t enc;

    return t2_expand_imm(v, &enc) || t2_expand_imm(0u - v, &enc);
}

static void kcmp_scan(const emu_ir_block_t *b)
{
    const uint32_t n = (b->next_temp < EMU_IR_MAX_TEMPS) ? b->next_temp : EMU_IR_MAX_TEMPS;

    g_kcmp_block = b;
    memset(g_kcmp, 0, n * sizeof(g_kcmp[0]));
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (!in->dead && in->op == (uint8_t)EMU_IR_CONST && in->uses == 1u &&
            in->dst < n && kcmp_encodes(in->imm)) {
            g_kcmp[in->dst] = (uint16_t)(i + 1u);
        }
    }
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];
        const uint16_t ops[3] = {in->a, in->b, in->c};

        if (in->dead) {
            continue;
        }
        for (uint32_t k = 0; k < 3u; k++) {
            if (ops[k] < n && g_kcmp[ops[k]] != 0u && !kcmp_reader(in, ops[k])) {
                g_kcmp[ops[k]] = 0u;
            }
        }
    }
}

/* CMP ra, b -- by immediate where kcmp_scan kept b's constant. */
static void emit_cmp_operand(uint32_t ra, uint16_t bt, uint32_t scratch)
{
    if (bt == EMU_IR_NO_TEMP) {
        t2_cmp_imm(ra, 0u); /* no second operand: zero (a result's own flags) */
        return;
    }
    if (bt < EMU_IR_MAX_TEMPS && bt < g_ntemps && g_kcmp[bt] != 0u) {
        const uint32_t v = g_kcmp_block->insn[g_kcmp[bt] - 1u].imm;
        uint16_t enc;

        if (t2_expand_imm(v, &enc)) {
            t2_cmp_imm(ra, enc);
        } else {
            (void)t2_expand_imm(0u - v, &enc);
            t2_cmn_imm(ra, enc);
        }
        return;
    }
    t2_cmp(ra, use_reg_fwd(bt, scratch));
}

static uint32_t use_reg(uint16_t n, uint32_t scratch)
{
    const int p = phys(n);

    if (p >= 0) {
        return (uint32_t)p;
    }
    ld_operand(scratch, n);
    return scratch;
}

static uint32_t def_reg(uint16_t n, uint32_t scratch)
{
    const int p = phys(n);

    return (p >= 0) ? (uint32_t)p : scratch;
}

/*
 * An operand which may still be in r0 from the instruction before:
 * nothing in this one has written r0 yet (g_r0_first -- the left
 * operand did not go through it). Only where a lowering reads its
 * operands before it writes r0 (takes_from_r0 lists them). It was a
 * store and an immediate reload into r1 -- `str r0, [sp, #44]; ldr r1,
 * [sp, #44]` -- all through ARMv7-M CoreMark's hottest loop, where
 * frame traffic was 20% of the time in translated code; then a store's
 * value, a load's address and a divide's operands, which the allocator
 * leaves to r0 when the next instruction reads them and which went to
 * the frame instead.
 */
static uint32_t use_reg_fwd(uint16_t n, uint32_t scratch)
{
    if (g_r0_first && n != EMU_IR_NO_TEMP && n == g_r0_avail && phys(n) < 0) {
        return T2_R0;
    }
    return use_reg(n, scratch);
}

/*
 * Whether `in` takes temp `t` from r0 through use_reg_fwd: as the right
 * operand of an operation whose left one goes elsewhere than r0, or as
 * any operand of one that loads neither into r0. The table the lowerings
 * below must agree with, line for line.
 */
static bool kcmp_folded(uint16_t t);

static bool takes_from_r0(const emu_ir_insn_t *in, uint16_t t)
{
    if (in->c == t) {
        return false;
    }
    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_EXIT_IF:
        if (kcmp_folded(in->b)) {
            return false;
        }
        /* fall through */
    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
    case EMU_IR_MUL:
    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR:
        return in->b == t && in->a != t && phys(in->a) >= 0;
    case EMU_IR_MULHS:
    case EMU_IR_MULHU:
    case EMU_IR_DIVS:
    case EMU_IR_DIVU:
    case EMU_IR_REMS:
    case EMU_IR_REMU:
    case EMU_IR_STORE:
        return in->a == t || in->b == t;
    case EMU_IR_LOAD:
        return in->a == t;
    default:
        return false;
    }
}

/*
 * Sites to be patched to the block epilogue. Every early exit -- a
 * trapping access, a taken EXIT_IF, an EXIT -- lands there rather than
 * returning directly, because all paths must undo the same frame
 * adjustment.
 */
#define IR_MAX_EXITS 64u
typedef struct {
    uint8_t *at;
    bool conditional;
} ir_exit_t;

/*
 * The kind is recorded, not recovered from the emitted halfword.
 * Reading it back means deciding conditional-versus-unconditional
 * from an encoding already written, and a misread patches a B<c>.W
 * with B.W's field layout -- landing somewhere plausible instead of
 * somewhere right.
 */
static EMU_PER_CORE ir_exit_t g_exits[IR_MAX_EXITS];
static EMU_PER_CORE uint32_t g_nexits;

/*
 * Patch a forward branch emitted above to reach `target`.
 *
 * Guarded on overflow because `at` may be past the end of a buffer that
 * stopped accepting emissions -- the block is discarded either way, and
 * writing through the pointer would corrupt whatever follows.
 */
/*
 * Where this block's instruction stream begins, and the guest address it
 * begins at. See the identical pair in the x86-64 backend: an EXIT whose
 * constant target is this address is a loop closing on the block itself,
 * and becomes a branch rather than a return to the dispatcher.
 *
 * Every branch here is the wide form, so there is no range cliff. The
 * hand-written backend this replaced chained only when the back edge
 * fitted the 16-bit conditional encoding and silently stopped chaining
 * when a block outgrew it, which cost 2.4x on the loops that crossed
 * the line.
 */
/*
 * The guest-RAM window this block may access without calling a helper.
 * See emu_ir_fastmem_t: the frontend refuses whenever an access could
 * mean more than a move.
 */
static EMU_PER_CORE emu_ir_fastmem_t g_fast;
static EMU_PER_CORE bool g_has_fast;
/* Or the data TLB it may probe instead -- see emu_ir_dtlb_t. */
static EMU_PER_CORE emu_ir_dtlb_t g_dtlb;
static EMU_PER_CORE bool g_has_dtlb;
/* The peripheral window, when the block was translated armed. */
static EMU_PER_CORE emu_ir_ptwin_t g_pt;
static EMU_PER_CORE bool g_has_pt;
/* And the image, for loads, likewise: see emu_ir_rom_window. */
static EMU_PER_CORE emu_ir_fastmem_t g_rom;
static EMU_PER_CORE bool g_has_rom;

/*
 * The slow path of every LOAD and STORE, in one place.
 *
 * It was in line at each access: the cpu into r0, the access's spec into
 * r2, an out-pointer into r3, MOVW/MOVT/BLX to the accessor, and a
 * compare for its verdict -- 22 bytes of a load's 54, on the board where
 * those bytes are the guest's RAM. Now an access says what it is in r2
 * and calls this through a literal into r12, which it clobbers anyway.
 * r11 held it until the RAM window's address took the register.
 *
 *   in   r1  the guest address
 *        r2  the spec, with T2_MEM_STORE set for a store
 *        r3  a store's value
 *        r4  the cpu, as in every block
 *   out  Z   set when the access completed, clear if the accessor
 *            trapped -- the caller's BNE takes the block's exit
 *        r0  a load's value
 *
 * r1-r3, r12 and the flags are clobbered, as by any call. The push is
 * two words so sp stays 8-aligned at the accessor, which AAPCS requires
 * and the block's frame was padded for; the first is the load's out
 * word, and POP hands it back in r0 with the flags untouched.
 *
 * Which accessor is the target's, recorded at every lowering. One table
 * for the cache, because every block in it belongs to one frontend: a
 * different one comes with a new session, which flushes.
 *
 * Not EMU_PER_CORE-safe, and need not be: the Thumb-2 backend runs only
 * on a microcontroller, where EMU_SMP is off and that is a plain static.
 */
#define T2_MEM_STORE 0x80u

/*
 * A load the stub may serve from the guest's image (emu_ir_rom_window),
 * set by a block lowered with the window. Per access, not per stub: the
 * window is only sound where the RAM window is -- PMP, paging, an MPU or
 * a trigger refuse both -- so a block lowered without it must not have
 * its image reads answered here, and the stub cannot tell the blocks
 * apart any other way.
 */
#define T2_MEM_ROM_OK 0x40u

#if EMU_SMP
#error "t2_mem_stub reads its table by absolute address; no TLS here"
#endif

#if defined(EMU_HOT_TEXT_SECTION)
#define T2_STUB_SECTION EMU_HOT_TEXT_SECTION
#else
#define T2_STUB_SECTION ".text.t2_mem_stub"
#endif

/*
 * `used`: the stub's literal is the only reader, and LTO does not see
 * into top-level asm -- without it the table is dropped and the link
 * fails, which is the good outcome.
 */
__attribute__((used)) const void *t2_mem_accessor[2]; /* load, store */

/*
 * The image window the stub serves T2_MEM_ROM_OK loads from: base, size
 * and host address, written whenever a block lowered with the window is.
 * One for the cache, for the reason the accessor table is: a bus change
 * flushes. Size 0 until then, which the stub's bound refuses.
 */
__attribute__((used)) uint32_t t2_rom_window[3];

__asm__(".pushsection " T2_STUB_SECTION ", \"ax\", %progbits\n"
        "    .syntax unified\n"
        "    .thumb\n"
        "    .p2align 2\n"
        "    .global t2_mem_stub\n"
        "    .thumb_func\n"
        "    .type t2_mem_stub, %function\n"
        "t2_mem_stub:\n"
        "    push    {r0, lr}\n"
        "    lsls    r0, r2, #24\n" /* N: bit 7, a store */
        "    bmi     1f\n"
        /*
         * A load the block let use the image: in the window, aligned,
         * then a move from its host address. Anything else is the
         * accessor's. r0, r3, r12 and lr are free here -- lr is on the
         * stack -- and r1 and r2 survive for the accessor.
         */
        "    lsls    r0, r2, #25\n" /* N: bit 6, T2_MEM_ROM_OK */
        "    bpl     2f\n"
        "    ldr     r12, =t2_rom_window\n"
        "    ldr     r0, [r12]\n"
        "    subs    r0, r1, r0\n" /* offset; below the base wraps high */
        "    ldr     r3, [r12, #4]\n"
        "    and     lr, r2, #7\n" /* the access's size */
        "    subs    r3, r3, lr\n" /* last offset it may start at */
        "    bcc     2f\n"         /* no window yet: size 0 */
        "    cmp     r0, r3\n"
        "    bhi     2f\n"
        "    sub     r3, lr, #1\n"
        "    tst     r1, r3\n"
        "    bne     2f\n"         /* misaligned */
        "    ldr     r12, [r12, #8]\n"
        "    cmp     lr, #2\n"
        "    beq     3f\n"
        "    bhi     4f\n"
        "    tst     r2, #8\n"     /* EMU_IR_MEM_SIGNED */
        "    ite     ne\n"
        "    ldrsbne r0, [r12, r0]\n"
        "    ldrbeq  r0, [r12, r0]\n"
        "    b       5f\n"
        "3:\n"
        "    tst     r2, #8\n"
        "    ite     ne\n"
        "    ldrshne r0, [r12, r0]\n"
        "    ldrheq  r0, [r12, r0]\n"
        "    b       5f\n"
        "4:\n"
        "    ldr     r0, [r12, r0]\n"
        "5:\n"
        "    str     r0, [sp]\n"   /* the out word POP returns in r0 */
        "    cmp     r0, r0\n"     /* Z: the access completed */
        "    pop     {r0, pc}\n"
        "2:\n"
        "    ldr     r12, =t2_mem_accessor\n"
        "    ldr     r12, [r12]\n"
        "    bic     r2, r2, #0x40\n"
        "    mov     r0, r4\n"
        "    add     r3, sp, #0\n"
        "    blx     r12\n"
        "    cmp     r0, #0\n"
        "    pop     {r0, pc}\n"
        "1:\n"
        "    ldr     r12, =t2_mem_accessor\n"
        "    ldr     r12, [r12, #4]\n"
        "    bic     r2, r2, #0x80\n"
        "    mov     r0, r4\n"
        "    blx     r12\n"
        "    cmp     r0, #0\n"
        "    pop     {r0, pc}\n"
        "    .ltorg\n"
        "    .size t2_mem_stub, . - t2_mem_stub\n"
        /*
         * The computed exits' stub (emu_jit_ibtc), reached through a
         * literal into r12. Entered with the
         * target pc in r0, the core in r4, the count in r5 and the block's
         * frame given back, as a chained exit leaves them; r1-r3 and r12
         * are free. The return is any block's: every block pushes r4-r11
         * and lr (emu_ir_lower checks), and this pops them.
         */
        "    .p2align 2\n"
        "    .global t2_ibtc_stub\n"
        "    .thumb_func\n"
        "    .type t2_ibtc_stub, %function\n"
        "t2_ibtc_stub:\n"
        "    ldr     r3, =emu_jit_ibtc_state\n"
        "    cmp     r5, #128\n"          /* EMU_JIT_LOOP_CAP */
        "    bhs     9f\n"
        "    ldr     r1, =0x9E3779B1\n"   /* emu_jit_ibtc_index */
        "    mul     r1, r0, r1\n"
        "    lsrs    r1, r1, #26\n"       /* 32 - EMU_JIT_IBTC_BITS */
        "    ldr     r12, =emu_jit_ibtc\n"
        "    add     r12, r12, r1, lsl #5\n"
        "    ldr     r1, [r12]\n"         /* the entry's pc */
        "    cmp     r1, r0\n"
        "    bne     9f\n"
        "    ldr     r2, [r3, #8]\n"      /* ctx_off */
        "    add     r2, r2, r4\n"
        "    ldrd    r0, r1, [r2]\n"      /* the core's context */
        "    ldr     r2, [r12, #8]\n"
        "    cmp     r2, r0\n"
        "    bne     9f\n"
        "    ldr     r2, [r12, #12]\n"
        "    cmp     r2, r1\n"
        "    bne     9f\n"
        "    ldr     r2, [r12, #20]\n"    /* hits, saturating */
        "    ldrh    r1, [r2]\n"
        "    adds    r1, r1, #1\n"
        "    usat    r1, #16, r1\n"
        "    strh    r1, [r2]\n"
        "    ldr     r1, [r12, #4]\n"     /* block: the state's last */
        "    ldr     r2, [r3, #4]\n"
        "    adds    r2, r2, #1\n"        /* and its hit count */
        "    strd    r1, r2, [r3]\n"
        "    ldr     r12, [r12, #16]\n"   /* entry, Thumb bit set */
        "    bx      r12\n"
        "    .global t2_ibtc_miss\n"
        "    .thumb_func\n"
        "    .type t2_ibtc_miss, %function\n"
        "t2_ibtc_miss:\n"
        "9:\n"
        "    movs    r2, #1\n"            /* missed: the dispatcher fills */
        "    str     r2, [r3, #12]\n"
        "    mov     r0, r5\n"
        "    pop     {r4-r11, pc}\n"
        "    .ltorg\n"
        "    .size t2_ibtc_stub, . - t2_ibtc_stub\n"
        ".popsection\n");

_Static_assert(EMU_JIT_LOOP_CAP == 128u && EMU_JIT_IBTC_BITS == 6u &&
                   EMU_JIT_IBTC_SHIFT == 5u,
               "t2_ibtc_stub's constants");
_Static_assert(offsetof(emu_jit_ibtc_t, block) == 4u &&
                   offsetof(emu_jit_ibtc_t, context) == 8u &&
                   offsetof(emu_jit_ibtc_t, entry) == 16u &&
                   offsetof(emu_jit_ibtc_t, hits) == 20u &&
                   sizeof(emu_jit_ibtc_t) == 32u,
               "t2_ibtc_stub's offsets into an entry");
_Static_assert(offsetof(emu_jit_ibtc_state_t, hits) == 4u &&
                   offsetof(emu_jit_ibtc_state_t, ctx_off) == 8u &&
                   offsetof(emu_jit_ibtc_state_t, missed) == 12u,
               "t2_ibtc_stub's offsets into the state");

/*
 * Whether this block's computed jumps may go to t2_ibtc_stub: only where
 * the push list is the one the stub pops.
 */
static EMU_PER_CORE bool g_ibtc_ok;
static EMU_PER_CORE bool g_ibtc_armed;

/*
 * rd = rn + imm, for an access's address. ADDW/SUBW take any 12-bit
 * magnitude in one instruction, which is every displacement a load or
 * store encodes on these guests; MOVW/MOVT and an ADD was eight bytes
 * for the same thing, on half of everything this backend emits.
 */
static void emit_addr(uint32_t rd, uint32_t rn, uint32_t imm)
{
    const int32_t d = (int32_t)imm;

    if (d == 0) {
        if (rd != rn) {
            t2_mov(rd, rn);
        }
    } else if (d > 0 && d < 4096) {
        t2_addw(rd, rn, (uint16_t)d);
    } else if (d < 0 && d > -4096) {
        t2_subw(rd, rn, (uint16_t)-d);
    } else {
        t2_imm32(T2_R2, imm);
        t2_add(rd, rn, T2_R2);
    }
}

/*
 * Guard the inlined access and branch to the slow path on anything
 * unusual. Leaves in r2 what the access indexes with, and returns the
 * shift the access has to apply to it; the slow-path branch is *out.
 *
 * **One compare for the bounds and the alignment.** r2 is the offset
 * from the window's base rotated right by log2(size): an aligned offset
 * becomes offset >> k exactly, a misaligned one carries its low bits to
 * the top where no in-bounds value has any, and an address below the
 * base has wrapped to a huge offset already. So `rotated < size >> k`,
 * unsigned, is the whole test, and the access undoes the rotation with
 * LSL #k. Exact rather than conservative, because every region's size
 * is a multiple of the widest access. It was a subtract, a compare, a
 * TST and two branches, with the base and the limit each a MOVW/MOVT --
 * 34 bytes on every load and store, half of everything this backend
 * emits.
 *
 * The base and the limit are modified immediates when they encode,
 * which RV32's 0x80000000 does; otherwise r12, the procedure-call
 * scratch, holds them as before. The branch is the 16-bit form, because
 * the slow path is the next thing emitted.
 */
static void emit_pooled(uint32_t rd, uint32_t v);

static uint32_t emit_window_guard(uint32_t base, uint32_t wsize,
                                  uint32_t addr_reg, uint32_t size,
                                  uint8_t **out)
{
    const uint32_t k = (size == 4u) ? 2u : (size == 2u) ? 1u : 0u;
    const uint32_t lim = wsize >> k;
    uint16_t enc;

    if (t2_expand_imm(base, &enc)) {
        t2_sub_imm(T2_R2, addr_reg, enc);
    } else {
        emit_pooled(T2_R12, base);
        t2_sub(T2_R2, addr_reg, T2_R12);
    }
    if (k != 0u) {
        t2_shift_imm(T2_ROR, T2_R2, T2_R2, k);
    }
    if (t2_expand_imm(lim, &enc)) {
        t2_cmp_imm(T2_R2, enc);
    } else {
        emit_pooled(T2_R12, lim);
        t2_cmp(T2_R2, T2_R12);
    }
    out[0] = t2_bcond_forward_n(0x2u); /* HS: unsigned >= */
    return k;
}

static uint32_t emit_fast_guard(uint32_t addr_reg, uint32_t size, uint8_t **out)
{
    return emit_window_guard(g_fast.base, g_fast.size, addr_reg, size, out);
}

/*
 * The data TLB probe, for an access whose address is in `addr`: branch to
 * the slow path on a misaligned address, on a store while the guard byte
 * is set, or on a miss, and otherwise leave the entry's addend in r12 --
 * so the access is `[r12, addr]`. Uses r0, r2 and r12, which an access
 * leaves free until its slow path; returns how many branches it wrote.
 *
 *     tst.w addr, #size-1 ; bne slow
 *     lsr.w r2, addr, #12 ; and.w r0, r2, #mask
 *     add.w r0, r4, r0, lsl #4
 *     ldr.w r12, [r0, #table + tag] ; cmp r12, r2 ; bne slow
 *     ldr.w r12, [r0, #table + addend]
 *
 * The page number is the index and the tag at once, which is why it is
 * computed once into r2 and compared whole.
 */
static uint32_t emit_dtlb_probe(uint32_t addr, uint32_t size, bool store,
                                uint8_t **out)
{
    uint32_t n = 0u;
    uint32_t off = g_dtlb.table_offset;
    uint16_t enc;

    if (size > 1u) {
        (void)t2_tst_imm8(addr, size - 1u);
        out[n++] = t2_bcond_forward_n(0x1u); /* NE */
    }
    /* See the same guard in STORE: a byte, and before r0 is spoken for. */
    if (store && g_dtlb.store_guard_offset != EMU_IR_NO_GUARD) {
        t2_ldrb_imm(T2_R0, T2_CPU, g_dtlb.store_guard_offset);
        (void)t2_cmp_imm8(T2_R0, 0u);
        out[n++] = t2_bcond_forward_n(0x1u); /* NE */
    }

    t2_shift_imm(T2_LSR, T2_R2, addr, EMU_DTLB_PAGE_SHIFT);
    if (t2_expand_imm(g_dtlb.index_mask, &enc)) {
        t2_and_imm(T2_R0, T2_R2, enc);
    } else {
        /* A mask too wide for a modified immediate: keep the low bits. */
        const uint32_t bits = 32u - (uint32_t)__builtin_popcount(g_dtlb.index_mask);

        t2_shift_imm(T2_LSL, T2_R0, T2_R2, bits);
        t2_shift_imm(T2_LSR, T2_R0, T2_R0, bits);
    }
    t2_add_lsl(T2_R0, T2_CPU, T2_R0, 4u); /* sizeof(emu_dtlb_entry_t) */
    /* LDR.W reaches 4095; past that, move the base instead. */
    if (off + EMU_DTLB_OFF_ADDEND > 4095u) {
        emit_pooled(T2_R12, off);
        t2_add(T2_R0, T2_R0, T2_R12);
        off = 0u;
    }
    t2_ldr_imm(T2_R12, T2_R0,
               off + (store ? EMU_DTLB_OFF_WRITE : EMU_DTLB_OFF_READ));
    t2_cmp(T2_R12, T2_R2);
    out[n++] = t2_bcond_forward_n(0x1u); /* NE */
    t2_ldr_imm(T2_R12, T2_R0, off + EMU_DTLB_OFF_ADDEND);
    return n;
}

/* r12 = the peripheral window's base, which is also its host address. */
static void emit_pt_base(void)
{
    uint16_t enc;

    if (t2_expand_imm(g_pt.base, &enc)) {
        t2_mov_imm(T2_R12, enc);
    } else {
        t2_imm32(T2_R12, g_pt.base);
    }
}

/*
 * Branch to the slow path if a store's rotated offset (r2, shift k) lands
 * in one of the window's read-only holes. Unsigned `r2 - off >> k <
 * len >> k` per hole, in r0, which a store does not need until the slow
 * path loads the cpu into it. Exact for an aligned access, which is all
 * the guard lets through, because every hole is word aligned and a
 * multiple of four long. Returns how many branches it wrote to `out`.
 */
static uint32_t emit_pt_holes(uint32_t k, uint8_t **out)
{
    uint32_t n = 0u;

    for (uint32_t i = 0; i < g_pt.nholes; i++) {
        const uint32_t off = g_pt.hole[i].off >> k;
        const uint32_t len = g_pt.hole[i].len >> k;
        uint16_t enc;

        if (t2_expand_imm(off, &enc)) {
            t2_sub_imm(T2_R0, T2_R2, enc);
        } else {
            t2_imm32(T2_R12, off);
            t2_sub(T2_R0, T2_R2, T2_R12);
        }
        if (t2_expand_imm(len, &enc)) {
            t2_cmp_imm(T2_R0, enc);
        } else {
            t2_imm32(T2_R12, len);
            t2_cmp(T2_R0, T2_R12);
        }
        out[n++] = t2_bcond_forward_n(0x3u); /* LO: inside the hole */
    }
    return n;
}

static EMU_PER_CORE uint8_t *g_body;

/*
 * The guest pc, stored lazily. A constant SETPC emits nothing: its value
 * is pending, and goes to the cpu only where something can read it -- a
 * helper's call, a memory access's slow path, a GET of the register
 * that is the pc -- or an exit stores its own. A block begins with its
 * own pc pending, which is true on every way in (the dispatcher and a
 * chained exit store it), so a loop's back edge need store nothing: the
 * pc stores were two of every iteration's three stores in a five-
 * instruction RV32 loop, six instructions of about thirty on the N6.
 */
static EMU_PER_CORE bool g_pc_pending;
static EMU_PER_CORE uint32_t g_pc_value;
static EMU_PER_CORE uint32_t g_pc_off;

/* Store the pending pc through `scratch`. `settle` in straight-line code;
 * not on a slow path the fast one skips, after which it is pending still. */
static void pc_store(uint32_t scratch, bool settle)
{
    if (!g_pc_pending) {
        return;
    }
    t2_imm32(scratch, g_pc_value);
    t2_str_imm(scratch, T2_CPU, g_pc_off);
    if (settle) {
        g_pc_pending = false;
    }
}
static EMU_PER_CORE uint32_t g_start_pc;
static EMU_PER_CORE bool g_has_start_pc;

static void patch_branch(uint8_t *at, const uint8_t *target, bool conditional)
{
    if (at == NULL || emu_jit_overflowed()) {
        return;
    }

    uint16_t *const hw = (uint16_t *)(void *)at;
    /* The branch reads pc as its own address plus 4. */
    const int32_t off = (int32_t)(target - (at + 4));
    const uint32_t imm = (uint32_t)off >> 1;

    if (conditional) {
        /* imm11 | J2 | imm6 | J1 -- the T3 encoding's split fields. */
        const uint32_t s = (imm >> 19) & 1u;
        const uint32_t j2 = (imm >> 18) & 1u;
        const uint32_t j1 = (imm >> 17) & 1u;

        hw[0] =
            (uint16_t)((hw[0] & 0xFBC0u) | (s << 10) | ((imm >> 11) & 0x3Fu));
        hw[1] = (uint16_t)(0x8000u | (j1 << 13) | (j2 << 11) | (imm & 0x7FFu));
    } else {
        /* T4: S | imm10 | J1 | 1 | J2 | imm11, with J = !(I ^ S). */
        const uint32_t s = (imm >> 23) & 1u;
        const uint32_t i1 = (imm >> 22) & 1u;
        const uint32_t i2 = (imm >> 21) & 1u;
        const uint32_t j1 = (~(i1 ^ s)) & 1u;
        const uint32_t j2 = (~(i2 ^ s)) & 1u;

        hw[0] = (uint16_t)(0xF000u | (s << 10) | ((imm >> 11) & 0x3FFu));
        hw[1] = (uint16_t)(0x9000u | (j1 << 13) | (j2 << 11) | (imm & 0x7FFu));
    }
}

/*
 * Set when an exit could not be recorded, and it makes emu_ir_lower
 * refuse the block. An unrecorded exit is never patched, and the
 * placeholder it was emitted as branches to the next instruction -- so
 * the exit quietly does not leave. See the same flag in the x86-64
 * backend; this table dropped its 65th entry in silence too.
 */
static EMU_PER_CORE bool g_exits_lost;

/*
 * A 16-bit branch to a target emitted moments later. Out of reach would
 * be a defect in this file -- the targets are tens of bytes away -- and
 * is refused like a lost exit rather than truncated into a branch
 * somewhere else.
 */
static void patch_branch_n(uint8_t *at, const uint8_t *target, bool cond)
{
    if (at == NULL || emu_jit_overflowed()) {
        return;
    }
    if (!t2_patch_branch_n(at, target, cond)) {
        g_exits_lost = true;
    }
}

/*
 * **The block's constant pool.** The guest-RAM window's host address is
 * a full 32-bit value -- MOVW and MOVT, eight bytes -- and every inlined
 * load and store needs it. It cannot come from a shared slot, because a
 * frontend may move its window between translations without a flush
 * (ARMv7-M's follows the stack) and each block's guard is baked against
 * its own. So each block carries its own copy, after its last
 * instruction, and an access loads it PC-relative: LDR.W, four bytes,
 * at every access, and four once per block plus two of alignment half
 * the time.
 *
 * Only for a value a block is likely to need more than once. A
 * constant used once costs the same eight bytes either way, and a load
 * where MOVW/MOVT had none.
 *
 * LDR.W reaches 4095 bytes forward, and how far the end of a block is
 * from a load is not known when the load is emitted. So the pool is
 * laid down early -- in line, behind a branch -- once the oldest load
 * waiting on it is POOL_REACH back; what is left of the reach is for
 * the IR instruction being lowered when that is checked, and for the
 * pool. No guest here has a block that long, which is why it is a
 * margin and not a measurement.
 */
#define POOL_VALUES 4u
#define POOL_SITES 64u
#define POOL_REACH 3072u

static EMU_PER_CORE uint32_t g_pool_val[POOL_VALUES];
static EMU_PER_CORE uint32_t g_pool_nval;
static EMU_PER_CORE uint8_t *g_pool_at[POOL_SITES];
static EMU_PER_CORE uint8_t g_pool_idx[POOL_SITES];
static EMU_PER_CORE uint32_t g_pool_nsites;

static void pool_reset(void)
{
    g_pool_nval = 0u;
    g_pool_nsites = 0u;
}

/*
 * rd = v, from the pool -- or MOVW/MOVT, when the pool is full, and
 * MOVW alone when that is all v needs, which is four bytes already.
 */
static void emit_pooled(uint32_t rd, uint32_t v)
{
    uint32_t i = 0u;

    while (i < g_pool_nval && g_pool_val[i] != v) {
        i++;
    }
    if ((v >> 16) == 0u || g_pool_nsites == POOL_SITES || i == POOL_VALUES) {
        t2_imm32(rd, v);
        return;
    }
    if (i == g_pool_nval) {
        g_pool_val[g_pool_nval++] = v;
    }
    g_pool_idx[g_pool_nsites] = (uint8_t)i;
    g_pool_at[g_pool_nsites++] = t2_ldr_literal(rd);
}

/*
 * Lay the pool down here and point every load waiting on it at its
 * word. A load out of reach would be a defect in the margin above, and
 * is refused like a lost exit rather than left loading the wrong word.
 */
static void pool_emit(void)
{
    if (g_pool_nsites == 0u) {
        return;
    }
    if (((uintptr_t)emu_jit_here() & 3u) != 0u) {
        t2_emit16(0xBF00u); /* NOP: the literals are words */
    }

    uint8_t *const base = emu_jit_here();

    for (uint32_t i = 0; i < g_pool_nval; i++) {
        t2_emit16((uint16_t)g_pool_val[i]);
        t2_emit16((uint16_t)(g_pool_val[i] >> 16));
    }
    if (!emu_jit_overflowed()) {
        for (uint32_t i = 0; i < g_pool_nsites; i++) {
            if (!t2_patch_literal(g_pool_at[i], base + 4u * g_pool_idx[i])) {
                g_exits_lost = true;
            }
        }
    }
    pool_reset();
}

/* The pool, in line, if the oldest load waiting on it is getting far. */
static void pool_check_reach(void)
{
    if (g_pool_nsites == 0u ||
        (uint32_t)(emu_jit_here() - g_pool_at[0]) < POOL_REACH) {
        return;
    }

    uint8_t *const over = t2_b_forward_n();

    pool_emit();
    patch_branch_n(over, emu_jit_here(), false);
}

/*
 * The slow path of a LOAD or STORE: what the access was, a call to the
 * shared stub, and the exit if the accessor trapped. Six bytes and a
 * conditional branch, where the call had been set up and made in line --
 * the cpu into r0, the out-pointer into r3, MOVW/MOVT/BLX, a compare --
 * at every access. The address is in r1 and a store's value in r3.
 */
static void note_exit(uint8_t *at, bool conditional);

static void emit_mem_stub(uint32_t spec)
{
    pc_store(T2_R0, false); /* r0 is the stub's out slot: free */
    /* MOVS: the flags are dead here, and the stub sets them anyway. */
    if (!t2_movs_imm8(T2_R2, spec)) {
        t2_imm32(T2_R2, spec);
    }
    /* Through r12, which the stub clobbers anyway: r11 is the RAM's */
    emit_pooled(T2_R12, (uint32_t)(uintptr_t)t2_mem_stub);
    t2_blx(T2_R12);
    note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
}

/*
 * A predicated access (emu_ir_mem_if): branch round all of it while the
 * predicate is zero. Called once the address is in r1 (and a store's
 * value in r3), so r0 is free -- and is where an unallocated predicate
 * is loaded, which lets the reload elision hand it over when the
 * instruction before computed it.
 */
static uint8_t *emit_pred_skip(const emu_ir_insn_t *in)
{
    if (in->c == EMU_IR_NO_TEMP) {
        return NULL;
    }
    const uint32_t rp = use_reg(in->c, T2_R0);

    if (!t2_cmp_imm8(rp, 0u)) {
        t2_cmp_imm(rp, 0u);
    }
    return t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_EQ));
}

/* ...and where the skip lands. Whatever r0 held, the paths differ. */
static void end_pred_skip(uint8_t *skip)
{
    if (skip != NULL) {
        t2_patch_branch(skip, emu_jit_here(), true);
        g_r0_holds = EMU_IR_NO_TEMP;
    }
}

static void note_exit(uint8_t *at, bool conditional)
{
    if (g_nexits < IR_MAX_EXITS) {
        g_exits[g_nexits].at = at;
        g_exits[g_nexits].conditional = conditional;
        g_nexits++;
    } else {
        g_exits_lost = true;
    }
}

/*
 * Exits that have already torn the frame down.
 *
 * A chained exit undoes its own frame before branching into the next
 * block, which builds its own -- otherwise the stack grows by a frame
 * per link and never comes back. So when it *cannot* branch (the budget
 * is spent, or the edge is unlinked) it rejoins the epilogue past the
 * teardown rather than at the top of it.
 */
static struct {
    uint8_t *at;
    bool conditional;
} g_exits_nf[IR_MAX_EXITS];
static EMU_PER_CORE uint32_t g_nexits_nf;

static void note_exit_nf(uint8_t *at, bool conditional)
{
    if (g_nexits_nf < IR_MAX_EXITS) {
        g_exits_nf[g_nexits_nf].at = at;
        g_exits_nf[g_nexits_nf].conditional = conditional;
        g_nexits_nf++;
    } else {
        g_exits_lost = true;
    }
}

/* This block's frame, needed at every chained exit. */
static EMU_PER_CORE uint32_t g_frame;
/* Where its code begins, so a patch site can be an offset. */
static EMU_PER_CORE uint8_t *g_block_start;

/* ADD.W sp, sp, #imm -- the epilogue's teardown, emitted at an exit. */
static void emit_frame_release(uint32_t frame)
{
    if (frame != 0u && !t2_sp_adjust(false, frame)) {
        t2_imm32(T2_R12, frame);
        t2_emit32(0xEB0Du, (uint16_t)((13u << 8) | T2_R12));
    }
}

/*
 * Emit a chained exit to a statically known target.
 *
 * Nothing is linked here: the target usually has not been translated
 * yet. The branch is left pointing at this block's own tail and the
 * framework patches it once both blocks exist and the edge has been
 * taken.
 */
static bool chain_exit(uint32_t target_pc)
{
    if (!emu_jit_layout.chainable) {
        return false;
    }
    /* A site's offset is fifteen bits (EMU_JIT_LINKED), with room for
     * what precedes the jump. */
    if (emu_jit_layout.nlink >= EMU_JIT_MAX_CHAIN ||
        (uint32_t)(emu_jit_here() - g_block_start) > EMU_JIT_SITE_MAX - 64u) {
        emu_jit_note_no_chain_slot();
        return false;
    }
    emit_frame_release(g_frame);
    /*
     * The bound, for the same reason the self-loop has one: a chain
     * never reaches the dispatcher, so nothing checks for a pending
     * interrupt or tests the caller's budget.
     */
    if (!t2_cmp_imm8(T2_CNT, EMU_JIT_LOOP_CAP)) {
        t2_imm32(T2_R12, EMU_JIT_LOOP_CAP);
        t2_cmp(T2_CNT, T2_R12);
    }
    note_exit_nf(t2_bcond_forward(0x2u), true); /* HS */

    uint8_t *const site = t2_b_forward();

    emu_jit_layout.link_pc[emu_jit_layout.nlink] = target_pc;
    emu_jit_layout.link_site[emu_jit_layout.nlink] =
        (uint16_t)(site - g_block_start);
    emu_jit_layout.nlink++;
    /* Aimed at the unlinked epilogue once it exists: see emu_ir_lower. */
    return true;
}

/*
 * Emit a back edge to this block's own body, bounded by the retired
 * count. False if the target is not this block's start, in which case
 * the caller stores the pc and emits an ordinary exit.
 *
 * The pc is not stored on the way round: the body begins with its own
 * pending (g_pc_pending). Only the bound's exit, to the dispatcher,
 * stores it. R0 is free here.
 */
static bool is_loop_back(uint32_t target_pc)
{
    return g_has_start_pc && g_body != NULL && target_pc == g_start_pc;
}

static bool loop_back(uint32_t target_pc)
{
    if (!is_loop_back(target_pc)) {
        return false;
    }
    /*
     * The bound is not optional. A chained loop never reaches the
     * dispatcher, so nothing checks for a pending interrupt or tests
     * the caller's budget, and a guest spinning on a flag a device sets
     * would hang. The count is already in a register because it is the
     * return value.
     */
    if (!t2_cmp_imm8(T2_CNT, EMU_JIT_LOOP_CAP)) {
        t2_imm32(T2_R0, EMU_JIT_LOOP_CAP);
        t2_cmp(T2_CNT, T2_R0);
    }
    /* LO/CC: under the bound, round again; else the pc, and out. */
    patch_branch(t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_LTU)), g_body, true);
    t2_imm32(T2_R0, target_pc);
    t2_str_imm(T2_R0, T2_CPU, g_pc_off);
    note_exit(t2_b_forward(), false);
    return true;
}

#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE
/*
 * A 16-bit dot product's kernel emitted in the block (EMU_IR_KERNEL),
 * where the call to emu_ir_kernel_run cost about what CoreMark's six
 * iterations an entry saved. The same decisions as that function, on the
 * cases it is worth emitting for -- x's stride 2, either element
 * big-endian (LDRH and REVSH, PowerPC's) or not, y's stride within 0x8000
 * so nothing below overflows 32 bits -- and otherwise the scalar body:
 * declining is always correct, since the scalar body
 * then runs the iterations and ends in the same state. fuzz-ir's corpus
 * on the board compares this with that function (scripts/ir-board.sh).
 *
 * At the block's start, before the body: nothing is allocated yet, so
 * r0-r3, r6-r10 and r12 are free (the frame saved r6-r10).
 *
 * KI_CALL: not a dot product, call the C kernel; KI_SKIP: a dot product
 * this does not emit, so nothing -- the call measured no faster than the
 * scalar loop it would have run; KI_DONE: emitted; KI_FAIL: an encoder
 * refused partway, and the block must be declined.
 */
enum { KI_CALL, KI_SKIP, KI_DONE, KI_FAIL };

/*
 * Whether a map operation's constant has no immediate form, so it must
 * be in a register before the loop: its own, or for ADDI, ANDI and ORI
 * the negated or complemented one SUB, BIC and ORN take.
 */
static bool map_needs_reg(uint8_t op, uint32_t imm)
{
    uint16_t e;

    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADDI:
        return !t2_expand_imm(imm, &e) && !t2_expand_imm(0u - imm, &e);
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
        return !t2_expand_imm(imm, &e) && !t2_expand_imm(~imm, &e);
    case EMU_IR_XORI:
        return !t2_expand_imm(imm, &e);
    default:
        return false;
    }
}

/*
 * One map operation, rd = op(rs): as src/backend/common/interp.c defines
 * it. A constant with no immediate form is in `rk`, loaded before the
 * loop. A shift by zero is a move: LSR and ASR #0 mean #32 here.
 */
static void emit_map_op(uint8_t op, uint32_t imm, uint32_t rd, uint32_t rs, uint32_t rk)
{
    const uint32_t k = imm & 31u;
    uint16_t e;

    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADDI:
        if (rk != 0xFFu) {
            t2_add(rd, rs, rk);
        } else if (t2_expand_imm(imm, &e)) {
            t2_add_imm(rd, rs, e);
        } else {
            (void)t2_expand_imm(0u - imm, &e); /* map_needs_reg said one does */
            t2_sub_imm(rd, rs, e);
        }
        break;
    case EMU_IR_ANDI:
        if (rk != 0xFFu) {
            t2_and(rd, rs, rk);
        } else if (t2_expand_imm(imm, &e)) {
            t2_and_imm(rd, rs, e);
        } else {
            (void)t2_expand_imm(~imm, &e);
            t2_bic_imm(rd, rs, e);
        }
        break;
    case EMU_IR_ORI:
        if (rk != 0xFFu) {
            t2_orr(rd, rs, rk);
        } else if (t2_expand_imm(imm, &e)) {
            t2_orr_imm(rd, rs, e);
        } else {
            (void)t2_expand_imm(~imm, &e);
            t2_orn_imm(rd, rs, e);
        }
        break;
    case EMU_IR_XORI:
        if (rk != 0xFFu) {
            t2_eor(rd, rs, rk);
        } else {
            (void)t2_expand_imm(imm, &e);
            t2_eor_imm(rd, rs, e);
        }
        break;
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
        if (k == 0u) {
            if (rd != rs) {
                t2_mov(rd, rs);
            }
        } else {
            t2_shift_imm((op == (uint8_t)EMU_IR_SHLI)   ? T2_LSL
                         : (op == (uint8_t)EMU_IR_SHRI) ? T2_LSR
                         : (op == (uint8_t)EMU_IR_SARI) ? T2_ASR
                                                        : T2_ROR,
                         rd, rs, (op == (uint8_t)EMU_IR_ROTLI) ? 32u - k : k);
        }
        break;
    case EMU_IR_SEXT8:
        t2_sxtb(rd, rs);
        break;
    case EMU_IR_SEXT16:
        t2_sxth(rd, rs);
        break;
    case EMU_IR_ZEXT8:
        t2_uxtb(rd, rs);
        break;
    case EMU_IR_ZEXT16:
        t2_uxth(rd, rs);
        break;
    case EMU_IR_NOT:
        t2_mvn(rd, rs);
        break;
    default: /* EMU_IR_NEG */
        t2_neg(rd, rs);
        break;
    }
}

/*
 * MVE (Helium), where the core has it -- the N6's Cortex-M55: 16 bytes an
 * instruction where the DSP extension has 4, every element width for the
 * multiply and the shifts, and the same tail window. Built where the
 * compiler targets it, used where MVFR1 says the core implements it (read
 * once; the M7's MVFR1 has 0 there). The unmasked single-width operations
 * and the whole-group copies; the rest keeps the DSP lowering.
 */
#if defined(__ARM_FEATURE_MVE)
#define T2_HAVE_MVE 1
#else
#define T2_HAVE_MVE 0
#endif

#if T2_HAVE_MVE
static bool have_mve(void)
{
    static int8_t s_mve = -1;

    if (s_mve < 0) {
        s_mve = (int8_t)((((*(volatile const uint32_t *)0xE000EF44u) >> 8) & 0xFu) != 0u);
    }
    return s_mve > 0;
}
#else
static bool have_mve(void)
{
    return false;
}
#endif

static int emit_dot_inline(uint32_t index)
{
    emu_ir_kernel_dot_t d;

    if (!emu_ir_kernel_dot(index, &d)) {
        return KI_CALL;
    }
    const uint32_t s = (uint32_t)((d.estep < 0) ? -d.estep : d.estep);
    const uint32_t offs[] = {d.xo, d.yo, d.eo, d.ao,
                             (d.lo == EMU_IR_KERNEL_NO_REG) ? 0u : d.lo,
                             (d.so == EMU_IR_KERNEL_NO_REG) ? 0u : d.so,
                             d.q ? d.qo : 0u};

    if (d.xstride != 2 || (s != 1u && s != 2u && s != 4u) ||
        d.insns == 0u || d.insns > EMU_JIT_LOOP_CAP || d.size < 2u || d.size > 0x7FFFFFFFu ||
        d.ysshift > 31u) {
        return KI_SKIP;
    }
    for (uint32_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        if (offs[i] > 4095u) {
            return KI_SKIP;
        }
    }
    enum { KX = 6u, KY = 7u, KS = 8u, KXH = 9u, KYH = 10u };
    uint8_t *skip[12];
    uint32_t ns = 0u;
    uint16_t minus1;

    /* Nothing is emitted before every refusal. */
    if (!t2_expand_imm(0xFFFFFFFFu, &minus1)) {
        return KI_SKIP;
    }
    /*
     * The map's constants with no immediate form, in r1 and lr through
     * the loop: r1 is free there (the count is recovered from x's offset
     * after it), and lr in a block's body. A third, and it is not worth a
     * loop of its own.
     */
    uint8_t kreg[2][EMU_IR_DOT_FN_OPS];
    uint32_t nk = 0u;
    static const uint8_t k_slots[2] = {T2_R1, T2_LR};

    if (d.fchains > 2u || (d.q && (d.fchains != 0u || d.qbit > 31u))) {
        return KI_SKIP; /* a sticky bit: SMLABB's own Q, on a product summed unmapped */
    }
    for (uint32_t c = 0u; c < 2u; c++) {
        for (uint32_t i = 0u; i < EMU_IR_DOT_FN_OPS; i++) {
            kreg[c][i] = 0xFFu;
            if (c < d.fchains && i < d.flen[c] && map_needs_reg(d.fop[c][i], d.fimm[c][i])) {
                if (nk == 2u) {
                    return KI_SKIP;
                }
                kreg[c][i] = k_slots[nk++];
            }
        }
    }

    /* m, the iterations after this one: e + ec + m * estep == L (kernel_trip) */
    t2_ldr_imm(T2_R0, T2_CPU, d.eo);
    if (d.lo == EMU_IR_KERNEL_NO_REG) {
        t2_imm32(T2_R1, d.lconst);
    } else {
        t2_ldr_imm(T2_R1, T2_CPU, d.lo);
    }
    t2_sub(T2_R1, T2_R1, T2_R0);
    t2_imm32(T2_R2, (uint32_t)d.ec);
    t2_sub(T2_R1, T2_R1, T2_R2);
    if (d.estep < 0) {
        t2_neg(T2_R1, T2_R1);
    }
    if (s > 1u) {
        /* not a whole number of steps: the loop runs to the cap */
        if (!t2_tst_imm8(T2_R1, s - 1u)) {
            t2_imm32(T2_R2, s - 1u);
            t2_and(T2_R2, T2_R1, T2_R2);
            t2_cmp_imm(T2_R2, 0u);
        }
        skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_NE));
        t2_shift_imm(T2_LSR, T2_R1, T2_R1, (s == 2u) ? 1u : 2u);
    }
    /* no more than the cap leaves, less the body's iteration (kernel_room) */
    if (!t2_cmp_imm8(T2_CNT, EMU_JIT_LOOP_CAP)) {
        t2_imm32(T2_R2, EMU_JIT_LOOP_CAP);
        t2_cmp(T2_CNT, T2_R2);
    }
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GEU));
    t2_imm32(T2_R2, EMU_JIT_LOOP_CAP + d.insns - 1u);
    t2_sub(T2_R2, T2_R2, T2_CNT);
    t2_imm32(T2_R3, d.insns);
    t2_udiv(T2_R2, T2_R2, T2_R3);
    t2_imm32(T2_R3, 1u);
    t2_sub(T2_R2, T2_R2, T2_R3);
    t2_cmp(T2_R1, T2_R2); /* m = min(m, room), unsigned */
    t2_emit16((uint16_t)(0xBF00u | (t2_cond((uint8_t)EMU_IR_C_GTU) << 4) | 0x8u));
    t2_mov(T2_R1, T2_R2);
    t2_cmp_imm(T2_R1, 0u);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_EQ));

    /* x, y and y's stride */
    t2_ldr_imm(KX, T2_CPU, d.xo);
    if (d.xoff != 0) {
        t2_imm32(T2_R3, (uint32_t)d.xoff);
        t2_add(KX, KX, T2_R3);
    }
    t2_ldr_imm(KY, T2_CPU, d.yo);
    if (d.yoff != 0) {
        t2_imm32(T2_R3, (uint32_t)d.yoff);
        t2_add(KY, KY, T2_R3);
    }
    if (d.so == EMU_IR_KERNEL_NO_REG) {
        t2_imm32(KS, (uint32_t)d.ysconst);
    } else {
        t2_ldr_imm(KS, T2_CPU, d.so);
        if (d.ysshift != 0u) {
            t2_shift_imm(T2_LSL, KS, KS, d.ysshift);
        }
    }
    /* halfword-aligned, both of them and the stride (in_window) */
    t2_orr(T2_R3, KX, KY);
    t2_orr(T2_R3, T2_R3, KS);
    if (!t2_tst_imm8(T2_R3, 1u)) {
        t2_imm32(T2_R2, 1u);
        t2_and(T2_R2, T2_R3, T2_R2);
        t2_cmp_imm(T2_R2, 0u);
    }
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_NE));
    /* |ys| <= 0x8000: ys * m below fits, where the C kernel uses 64 bits */
    t2_imm32(T2_R3, 0x8000u);
    t2_add(T2_R3, KS, T2_R3);
    t2_imm32(T2_R2, 0x10000u);
    t2_cmp(T2_R3, T2_R2);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));
    /* x's m + 1 elements in the window: x - base <= size, then + 2m + 2 */
    t2_imm32(T2_R12, d.base);
    t2_sub(KXH, KX, T2_R12);
    t2_imm32(T2_R3, d.size);
    t2_cmp(KXH, T2_R3);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));
    t2_add_lsl(T2_R2, KXH, T2_R1, 1u);
    t2_imm32(T2_R0, 2u);
    t2_add(T2_R2, T2_R2, T2_R0);
    t2_cmp(T2_R2, T2_R3);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));
    /* y's: its first and its last within size - 2, unsigned */
    t2_sub(KYH, KY, T2_R12);
    t2_imm32(T2_R3, d.size - 2u);
    t2_cmp(KYH, T2_R3);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));
    t2_mla(T2_R2, KS, T2_R1, KYH, false);
    t2_cmp(T2_R2, T2_R3);
    skip[ns++] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));

    /* the loop: r0 the sum, r2 the count, r6 and r7 the byte offsets */
    t2_imm32(T2_R12, (uint32_t)(uintptr_t)d.host);
    t2_add(KXH, KXH, T2_R12);
    t2_add(KYH, KYH, T2_R12);
    if (d.q) {
        /*
         * From the sum itself, and the host's Q clear: SMLABB sets Q on
         * the accumulate's signed overflow, step by step, as the guest's
         * does. Nothing before the kernel -- the block's first op -- has
         * flags in the APSR to lose.
         */
        t2_ldr_imm(T2_R0, T2_CPU, d.ao);
        t2_imm32(T2_R3, 0u);
        t2_msr_apsr_nzcvq(T2_R3);
    } else {
        t2_imm32(T2_R0, 0u);
    }
    t2_mov(T2_R2, T2_R1);
    t2_imm32(KX, 0u);
    t2_imm32(KY, 0u);
    for (uint32_t c = 0u; c < d.fchains; c++) {
        for (uint32_t i = 0u; i < d.flen[c]; i++) {
            if (kreg[c][i] != 0xFFu) {
                t2_imm32(kreg[c][i], d.fimm[c][i]); /* r1 no longer holds the count */
            }
        }
    }

    /*
     * Helium, where the core has it (the N6's M55): eight iterations a
     * step while eight are left -- x's eight halfwords in one VLDRH, y's
     * gathered from the offsets {0, ys, .., 7 ys} built once by VIDUP and
     * a multiply, and VMLAVA adding the eight products into r0, modulo
     * 2^32 as the scalar MLA does. The offsets move as the scalar loop's
     * would, which takes what is left, so everything after the loop
     * stands. A plain product only: a map, a sticky Q or a swapped element
     * keeps the scalar loop, as does a stride the gather's sixteen-bit
     * offsets cannot reach (7 ys above 65535, or a negative one).
     */
    uint8_t *vdone = NULL;

    if (have_mve() && d.fchains == 0u && !d.q && !d.xswap && !d.yswap) {
        uint8_t *novec[2];

        t2_cmp_imm(T2_R2, 8u);
        novec[0] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_LTU));
        t2_imm32(T2_R3, 9362u); /* 7 * 9362 = 65534 */
        t2_cmp(KS, T2_R3);
        novec[1] = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GTU));
        t2_imm32(T2_R12, 0u);
        t2_vidup16(2u, T2_R12, 1u); /* q2 = {0 .. 7} */
        t2_vmul16_scalar(2u, 2u, KS); /* q2 = {0, ys, .., 7 ys} */
        const uint8_t *const vtop = emu_jit_here();

        t2_add(T2_R3, KXH, KX);
        t2_vldrh16(0u, T2_R3, 0u);
        t2_add(T2_R12, KYH, KY);
        t2_vldrh16_gather(1u, T2_R12, 2u);
        t2_vmlava16(T2_R0, 0u, 1u);
        t2_add_imm(KX, KX, 16u);
        t2_add_lsl(KY, KY, KS, 3u);
        t2_imm32(T2_R3, 8u);
        t2_sub(T2_R2, T2_R2, T2_R3);
        t2_cmp_imm(T2_R2, 8u);
        patch_branch(t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_GEU)), vtop, true);
        t2_cmp_imm(T2_R2, 0u); /* none left: past the scalar loop */
        vdone = t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_EQ));
        patch_branch(novec[0], emu_jit_here(), true);
        patch_branch(novec[1], emu_jit_here(), true);
    }
    const uint8_t *const top = emu_jit_here();

    /* A big-endian element (PowerPC's) is LDRH and REVSH, a little-endian one LDRSH. */
    if (!t2_ld_reg(T2_R3, KXH, KX, 2u, !d.xswap) || !t2_ld_reg(T2_R12, KYH, KY, 2u, !d.yswap)) {
        return KI_FAIL;
    }
    if (d.xswap) {
        t2_revsh(T2_R3, T2_R3);
    }
    if (d.yswap) {
        t2_revsh(T2_R12, T2_R12);
    }
    t2_add_imm(KX, KX, 2u); /* imm12 2: the byte itself */
    t2_add(KY, KY, KS);
    if (d.q) {
        t2_smlaxy(T2_R0, T2_R3, T2_R12, T2_R0, false, false); /* SMLABB: Q, as the guest's */
    } else if (d.fchains == 0u) {
        t2_mla(T2_R0, T2_R3, T2_R12, T2_R0, false); /* MLA: no Q, unlike SMLABB */
    } else {
        /* DOT16_F: the product in r3, chain 0 into r12, chain 1 in place */
        t2_mul(T2_R3, T2_R3, T2_R12);
        if (d.fchains == 1u) {
            for (uint32_t i = 0u; i < d.flen[0]; i++) {
                emit_map_op(d.fop[0][i], d.fimm[0][i], T2_R3, T2_R3, kreg[0][i]);
            }
            t2_add(T2_R0, T2_R0, T2_R3);
        } else {
            if (d.flen[0] == 0u) {
                t2_mov(T2_R12, T2_R3);
            }
            for (uint32_t i = 0u; i < d.flen[0]; i++) {
                emit_map_op(d.fop[0][i], d.fimm[0][i], T2_R12, (i == 0u) ? T2_R3 : T2_R12,
                            kreg[0][i]);
            }
            for (uint32_t i = 0u; i < d.flen[1]; i++) {
                emit_map_op(d.fop[1][i], d.fimm[1][i], T2_R3, T2_R3, kreg[1][i]);
            }
            t2_mla(T2_R0, T2_R12, T2_R3, T2_R0, false);
        }
    }
    t2_adds_imm(T2_R2, T2_R2, minus1);
    patch_branch(t2_bcond_forward(t2_cond((uint8_t)EMU_IR_C_NE)), top, true);
    if (vdone != NULL) {
        patch_branch(vdone, emu_jit_here(), true);
    }
    t2_shift_imm(T2_LSR, T2_R1, KX, 1u); /* the count again: x moved 2 a step */

    /* the registers, in the C kernel's order: x, y, e, the sum */
    t2_ldr_imm(T2_R3, T2_CPU, d.xo);
    t2_add_lsl(T2_R3, T2_R3, T2_R1, 1u);
    t2_str_imm(T2_R3, T2_CPU, d.xo);
    t2_ldr_imm(T2_R3, T2_CPU, d.yo);
    t2_mla(T2_R3, KS, T2_R1, T2_R3, false);
    t2_str_imm(T2_R3, T2_CPU, d.yo);
    if (d.eo != d.xo && d.eo != d.yo) {
        t2_ldr_imm(T2_R3, T2_CPU, d.eo);
        t2_imm32(T2_R12, (uint32_t)d.estep);
        t2_mla(T2_R3, T2_R12, T2_R1, T2_R3, false);
        t2_str_imm(T2_R3, T2_CPU, d.eo);
    }
    if (d.q) {
        t2_str_imm(T2_R0, T2_CPU, d.ao); /* r0 is the sum, from its own value */
        t2_mrs_apsr(T2_R3);
        t2_ubfx(T2_R3, T2_R3, 27u, 1u); /* the host's Q */
        if (d.qbit != 0u) {
            t2_shift_imm(T2_LSL, T2_R3, T2_R3, d.qbit);
        }
        t2_ldr_imm(T2_R12, T2_CPU, d.qo);
        t2_orr(T2_R12, T2_R12, T2_R3);
        t2_str_imm(T2_R12, T2_CPU, d.qo);
    } else {
        t2_ldr_imm(T2_R3, T2_CPU, d.ao);
        t2_add(T2_R3, T2_R3, T2_R0);
        t2_str_imm(T2_R3, T2_CPU, d.ao);
    }
    /* what it retired, and the report's count */
    t2_imm32(T2_R12, d.insns);
    t2_mla(T2_CNT, T2_R1, T2_R12, T2_CNT, false);
    t2_imm32(T2_R12, (uint32_t)(uintptr_t)&emu_ir_kernel_inline_iters);
    t2_ldr_imm(T2_R3, T2_R12, 0u);
    t2_add(T2_R3, T2_R3, T2_R1);
    t2_str_imm(T2_R3, T2_R12, 0u);
    for (uint32_t i = 0; i < ns; i++) {
        patch_branch(skip[i], emu_jit_here(), true);
    }
    return KI_DONE;
}
#endif

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

/*
 * VFP detects tininess before rounding and RISC-V after, so a result
 * that rounds up to +-2^-126 raised UF here and not in the guest. So UF
 * is not taken from FPSCR at all where the target can redo an operation
 * (its vfop): the block's harvest masks UFC out (emit_fp_harvest), and
 * UF is computed instead, where it can arise. VFP raises UF only from a
 * multiply, a divide or a fused multiply-add -- a tiny sum is exact --
 * and only with a result of magnitude 2^-126 or less, where RISC-V's UF
 * lies too. Each of those three compares its result (s3) with 2^-126,
 * and at or below it calls this, a, b and c in s0-s2 as the hard-float
 * ABI passes them: the operation redone in the guest's arithmetic, its
 * UF handed to the guest through the target's fp_flags, the result --
 * the same either way -- returned for s3. The other flags VFP raises as
 * RISC-V does, and stay FPSCR's.
 */
static uint32_t (*g_uf_vfop)(uint32_t op, uint32_t a, uint32_t b, uint32_t c,
                             uint32_t rm, uint32_t *flags);
static void (*g_uf_fp_flags)(emu_cpu_t *cpu, uint32_t flags);

__attribute__((used, noinline)) uint32_t t2_uf_fix(float fa, float fb, float fc,
                                                   emu_cpu_t *cpu, uint32_t op);
__attribute__((used, noinline)) uint32_t t2_uf_fix(float fa, float fb, float fc,
                                                   emu_cpu_t *cpu, uint32_t op)
{
    uint32_t a, b, c, fl = 0u;

    memcpy(&a, &fa, 4u);
    memcpy(&b, &fb, 4u);
    memcpy(&c, &fc, 4u);

    const uint32_t r = g_uf_vfop(op, a, b, c, EMU_IR_FRM_RNE, &fl);

    if ((fl & EMU_IR_FE_UNDERFLOW) != 0u) {
        g_uf_fp_flags(cpu, EMU_IR_FE_UNDERFLOW);
    }
    return r;
}

/*
 * The result in rd (and s3), its operands in s0-s2: the fix-up at or
 * below 2^-126, which returns the result into rd again.
 */
static void emit_uf_check(uint32_t vf_op, uint32_t rd)
{
    uint16_t enc;

    t2_shift_imm(T2_LSL, T2_R12, rd, 1u); /* the sign out */
    (void)t2_expand_imm(0x01000000u, &enc);
    t2_cmp_imm(T2_R12, enc);

    uint8_t *const ok = t2_bcond_forward(0x8u); /* HI: above 2^-126 */

    t2_mov(T2_R0, T2_CPU);
    (void)t2_movs_imm8(T2_R1, vf_op);
    t2_call((const void *)t2_uf_fix);
    if (rd != T2_R0) {
        t2_mov(rd, T2_R0);
    }
    patch_branch(ok, emu_jit_here(), true);
}

/*
 * Hand the flags FPSCR has accumulated to the frontend, and optionally
 * clear them so what follows starts from nothing.
 *
 * ARM orders them IOC,DZC,OFC,UFC,IXC from bit 0 and the IR
 * NX,UF,OF,DZ,NV -- an exact five-bit reversal, so RBIT of the masked
 * word followed by LSR #27 is the whole conversion. That it is a
 * reversal rather than a permutation is luck worth stating: on x86-64
 * the same mapping needs a lookup table.
 *
 * Called at the block exit, and again *before every helper call* in a
 * block that also computes floats natively. That second use is not
 * belt-and-braces. A helper reaches rv_hart_fp, which reads and clears
 * the host's exception state through <fenv.h> to derive its own -- so a
 * block that raised inexact in an emitted VADD and then called a helper
 * for an FMIN would have the inexact wiped before the exit ever saw it.
 * Harvesting first means the helper starts from a clean slate and
 * cannot destroy anything.
 */
static void emit_fp_harvest(const emu_ir_target_t *t, bool reclear)
{
    t2_vmrs(T2_R0);
    /* without UFC where UF is computed instead (t2_uf_fix) */
    t2_imm32(T2_R1, (g_uf_vfop != NULL) ? 0x17u : 0x1Fu);
    t2_and(T2_R0, T2_R0, T2_R1);
    t2_rbit(T2_R0, T2_R0);
    t2_shift_imm(T2_LSR, T2_R1, T2_R0, 27u);
    t2_mov(T2_R0, T2_CPU);
    t2_call((const void *)t->fp_flags);

    if (reclear) {
        t2_vmrs(T2_R0);
        t2_imm32(T2_R1, ~0x1Fu);
        t2_and(T2_R0, T2_R0, T2_R1);
        t2_vmsr(T2_R0);
    }
}

/*
 * Bisection gate.
 *
 * This host has no suite: the only way to find out which emitted
 * encoding is wrong is to allow one group at a time and flash. Level 0
 * emits nothing but the block frame and the pc and retire bookkeeping,
 * so the guest computes garbage but must still *run* -- if it faults
 * there, the frame is wrong and no instruction encoding is implicated.
 */
#ifndef T2_BISECT
#define T2_BISECT 99
#endif

static bool bisect_allows(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_NOP:
    case EMU_IR_RETIRE:
    case EMU_IR_SETPC:
        return T2_BISECT >= 0;
    case EMU_IR_GET:
    case EMU_IR_PUT:
    case EMU_IR_CONST:
    case EMU_IR_MOV:
        return T2_BISECT >= 1;
    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
    case EMU_IR_NOT:
    case EMU_IR_NEG:
    /*
     * The immediate forms sit with their register counterparts, which is
     * where they belong and is also the bug this table is prone to:
     * `default: return false` means an operation nobody adds here is
     * silently declined, and a declined operation costs the whole block.
     * Adding the ADDI class to lower_one and forgetting this took the
     * F746 from 171 translated blocks and 14,693 interpreted
     * instructions to 144 and **37,634** -- and isatest still passed,
     * because declining is correct.
     */
    case EMU_IR_ADDI:
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
    case EMU_IR_XORI:
        return T2_BISECT >= 2;
    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR:
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
        return T2_BISECT >= 3;
    case EMU_IR_BSWAP32:
    case EMU_IR_BSWAP16:
    case EMU_IR_HSWAP:
    case EMU_IR_CLZ:
    case EMU_IR_CTZ:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16:
        return T2_BISECT >= 4;
    case EMU_IR_CMP3:
    case EMU_IR_SETCC:
        return T2_BISECT >= 5;
    case EMU_IR_EXIT:
    case EMU_IR_EXIT_IF:
        return T2_BISECT >= 6;
    case EMU_IR_MUL:
    case EMU_IR_MULHS:
    case EMU_IR_MULHU:
    case EMU_IR_MAC:
    case EMU_IR_DIVS:
    case EMU_IR_DIVU:
    case EMU_IR_REMS:
    case EMU_IR_REMU:
        return T2_BISECT >= 7;
    case EMU_IR_LOAD:
    case EMU_IR_STORE:
    case EMU_IR_KERNEL: /* a call, as a helper's; a second list it fell off once */
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP:
        return T2_BISECT >= 7;
    case EMU_IR_FGET:
    case EMU_IR_FPUT:
    case EMU_IR_FSGNJ:
    case EMU_IR_FADD:
    case EMU_IR_FSUB:
    case EMU_IR_FMUL:
    case EMU_IR_FDIV:
    case EMU_IR_FSQRT:
    case EMU_IR_FMA:
        return T2_BISECT >= 8;
    /*
     * The condition flags, last, so a guest that miscomputes one can be
     * bisected away from everything else -- which matters more here than
     * for the arithmetic, since a wrong flag is a wrong *branch* and the
     * symptom is a guest that goes somewhere else entirely rather than
     * one that prints a wrong number.
     */
    case EMU_IR_SETF:
        return T2_BISECT >= 9;
    /*
     * The branch conditions, above SETF: the two are only useful
     * together, and this is the half that decides whether either runs.
     */
    case EMU_IR_GETCOND:
        return T2_BISECT >= 10;
    /*
     * The guest's vector registers, on their own level: a gate that
     * defaults to false refuses an operation nobody listed, after
     * emu_ir_can_lower promised it -- 95 blocks lost ("unkept") the first
     * time vecbench ran with EMU_IR_VOP on this host.
     */
    case EMU_IR_VOP:
    case EMU_IR_VOPM:
    case EMU_IR_VMEM:
    case EMU_IR_VRED:
    case EMU_IR_VMEMS:
    case EMU_IR_VFOP:
        return T2_BISECT >= 11;
    default:
        return false;
    }
}

/*
 * What this backend will emit for the floating-point class.
 *
 * The arithmetic goes to VFP under an FPSCR this block sets, which fixes
 * rounding at nearest -- so any other mode is declined rather than
 * silently rounded differently. ARM has no ties-away mode at all, which
 * is the case the hand-written translator beside this one also declines,
 * and the reason `aux` is part of the question.
 *
 * FGET, FPUT and FSGNJ never reach the FP unit: a value is its bit
 * pattern, so moving one is a load and a store and sign injection is
 * three integer instructions. They are answered true unconditionally
 * and cost nothing even on a part without a VFP.
 *
 * Comparison, conversion, FMIN/FMAX and FCLASS are declined for now.
 * The first two need fixups this host has its own shape for -- ARM
 * float-to-int gives 0 for a NaN where the guests want the maximum, and
 * VCMPE has to be told apart from VCMP by which NaN raises invalid --
 * and none of it can be tested except by flashing a board.
 */
#if EMU_IR_VECTOR
/*
 * Which EMU_IR_VOP forms this host lowers, a word of a register at a
 * time: at 32 bits everything but nothing more than the ALU has -- the
 * shifts by one count, the multiply, min and max through a compare --
 * and at 8 and 16 bits what the DSP extension has lanes for: UADD8/16
 * and USUB8/16 for the adds, USUB or SSUB setting GE then SEL for the
 * minimum and maximum, SMULBB and SMULTT packed by PKHBT for a 16-bit
 * multiply. No 8-bit multiply, and no shift narrower than a word: the
 * frontend's helper. A Cortex-M without the DSP extension (M3, M0) is
 * not a host this backend builds for.
 */
static bool vop_lowerable(uint8_t aux)
{
    const uint32_t sew = EMU_IR_VOP_SEW(aux);

    if (sew > 2u) {
        return false;
    }
    switch (EMU_IR_VOP_OP(aux)) {
    case EMU_IR_V_ADD:
    case EMU_IR_V_SUB:
    case EMU_IR_V_RSUB:
    case EMU_IR_V_AND:
    case EMU_IR_V_OR:
    case EMU_IR_V_XOR:
    case EMU_IR_V_MV:
    case EMU_IR_V_MINU:
    case EMU_IR_V_MIN:
    case EMU_IR_V_MAXU:
    case EMU_IR_V_MAX:
        return true;
    case EMU_IR_V_MUL:
        return sew != 0u;
    case EMU_IR_V_SLL:
    case EMU_IR_V_SRL:
    case EMU_IR_V_SRA:
        return sew == 2u && EMU_IR_VOP_VX(aux);
    case EMU_IR_V_AADDU:
    case EMU_IR_V_AADD:
        return true;
    case EMU_IR_V_MACC: /* MLA and MLS a word; SMULBB/TT a halfword pair */
    case EMU_IR_V_NMSAC:
    case EMU_IR_V_MADD:
    case EMU_IR_V_NMSUB:
        return sew >= 1u;
    /*
     * From halfwords: SMLABB and SMLATT signed, MLA after UXTH/SXTH and
     * LSR/ASR for the rest. Not from bytes: two products a halfword
     * lane, four lanes extended, is more registers than a block has.
     */
    case EMU_IR_V_WMACC:
    case EMU_IR_V_WMACCU:
    case EMU_IR_V_WMACCSU:
    case EMU_IR_V_WMACCUS:
        return sew == 1u;
    /*
     * Narrowing to halfwords by one count: the shifts, and the signed clip
     * by SSAT, whose Q flag is the saturation. Not to bytes -- no lane
     * shift -- nor the unsigned clip, whose 32-bit input USAT reads signed.
     */
    case EMU_IR_V_NSRL:
    case EMU_IR_V_NSRA:
    case EMU_IR_V_NCLIP:
        return sew == 1u && EMU_IR_VOP_VX(aux);
    default:
        return false;
    }
}


/* The unmasked operations MVE lowers; the averages under rnu and rdn. */
static bool vop_mve(uint32_t op, uint32_t sew, uint32_t rm)
{
    if (sew > 2u) {
        return false;
    }
    switch (op) {
    case EMU_IR_V_ADD:
    case EMU_IR_V_SUB:
    case EMU_IR_V_RSUB:
    case EMU_IR_V_AND:
    case EMU_IR_V_OR:
    case EMU_IR_V_XOR:
    case EMU_IR_V_MINU:
    case EMU_IR_V_MIN:
    case EMU_IR_V_MAXU:
    case EMU_IR_V_MAX:
    case EMU_IR_V_MUL:
    case EMU_IR_V_SLL:
    case EMU_IR_V_SRL:
    case EMU_IR_V_SRA:
    case EMU_IR_V_MV:
    case EMU_IR_V_MACC:
    case EMU_IR_V_NMSAC:
    case EMU_IR_V_MADD:
    case EMU_IR_V_NMSUB:
        return true;
    case EMU_IR_V_AADDU:
    case EMU_IR_V_AADD:
        return rm == EMU_IR_RM_RNU || rm == EMU_IR_RM_RDN;
    default:
        return false;
    }
}

/*
 * The widening multiply-adds from 8 and 16 bits, and the narrowing
 * shifts to 8 and 16 bits, at any count and mode -- which promise MVE
 * keeps whatever vxrm is. The clips go by mode (vop_narrow_mve_rm): a
 * promise only the DSP path's, for its forms, at the others.
 */
static bool vop_wide_mve(uint32_t op, uint32_t sew)
{
    return sew <= 1u &&
           (EMU_IR_V_IS_WIDE(op) || op == EMU_IR_V_NSRL || op == EMU_IR_V_NSRA);
}

static bool vop_narrow_mve_rm(uint32_t op, uint32_t sew, uint32_t rm)
{
    return sew <= 1u && (op == EMU_IR_V_NSRL || op == EMU_IR_V_NSRA ||
                         ((op == EMU_IR_V_NCLIPU || op == EMU_IR_V_NCLIP) &&
                          (rm == EMU_IR_RM_RNU || rm == EMU_IR_RM_RDN)));
}

/* The register file one block of 16-byte rows from v0, as MVE's loops
 * address it, at a word-aligned offset. */
static bool vfile_mve(const emu_ir_target_t *t)
{
    return t->vreg_offset != NULL && (t->vreg_offset(0u) % 4u) == 0u &&
           t->vreg_offset(31u) == t->vreg_offset(0u) + 31u * EMU_IR_VREG_BYTES;
}

#endif /* EMU_IR_VECTOR */

bool emu_ir_can_lower(emu_ir_op_t op, uint8_t aux)
{
    switch (op) {
    case EMU_IR_FGET:
    case EMU_IR_FPUT:
    case EMU_IR_FSGNJ:
        return true;

    case EMU_IR_FADD:
    case EMU_IR_FSUB:
    case EMU_IR_FMUL:
    case EMU_IR_FDIV:
    case EMU_IR_FSQRT:
        return EMU_IR_FRM(aux) == EMU_IR_FRM_RNE;

    /*
     * **The fused multiply-adds, natively.** This is the host that makes
     * the IR's third operand worth having: VFMA and VFMS round once, in
     * hardware, which is the entire content of the operation.
     *
     * All four RISC-V forms are reachable from two ARM instructions
     * because the sign of the product can be folded into the accumulate
     * -- see the lowering. Only RNE, as with the rest of the FP set:
     * FPSCR carries the mode and the block is specialised on frm, but
     * RMM has no ARM encoding and stays on the helper.
     */
    case EMU_IR_FMA:
        return EMU_IR_FRM(aux) == EMU_IR_FRM_RNE;

    /*
     * MLA and MLS, both of which this host has. No rounding question --
     * integer multiply-accumulate wraps the same fused or not -- so
     * there is nothing to gate on.
     */
    case EMU_IR_MAC:
        (void)aux;
        return true;

    case EMU_IR_FMIN:
    case EMU_IR_FMAX:
    case EMU_IR_FCMP:
    case EMU_IR_FCVT_TO_I:
    case EMU_IR_FCVT_FROM_I:
    case EMU_IR_FCLASS:
        return false;

    /*
     * The integer operations lower_one also declines.
     *
     * emu_ir.h calls answering true a promise, and this was breaking it:
     * `default: return true` covered ROTL, ROTLI and the four value bit
     * operations, every one of which lower_one refuses -- so a caller
     * that believed the answer built a block and lost it at emission,
     * having already paid for the translation and the optimisation.
     *
     * It cost nothing while only frontends asked, because the frontends
     * ask about the FP class and nothing else. It stopped being free the
     * moment an IR pass started asking, which is what surfaced it.
     */
    /*
     * The rotations lower natively -- ARM has ROR as a shift type and as
     * a register form -- so a left rotation by n is a right rotation by
     * 32 - n. The four value bit operations still decline: none has a
     * single-instruction ARM equivalent, and each would be three or four
     * instructions of open-coded shifting and masking, which is a second
     * copy of semantics the core already owns.
     */
    case EMU_IR_BEXT:
    case EMU_IR_BSET:
    case EMU_IR_BCLR:
    case EMU_IR_BINV:
        return false;

    /*
     * **The register-amount rotation is still declined**, and it has to
     * be named here rather than left to the default.
     *
     * `default: return true` makes this function a promise the lowering
     * has to keep, and emu_ir.h says so. ROTLI gained a case in
     * lower_one; ROTL did not, so falling through to the default would
     * claim an operation that lower_one refuses -- which costs the whole
     * block rather than one instruction, silently. That is the defect
     * this file already records for ROTL and the four bit operations
     * when the promise and the lowering last disagreed.
     *
     * No guest here executes one: the crypto guest's 21,710 rotations
     * are all `rori`, the immediate form.
     */
    case EMU_IR_ROTL:
        return false;

    /*
     * And the memory bit operations, which lower_one has always refused
     * and this default promised -- the same broken promise, found when
     * G4MH started asking. A frontend told no sends them to a helper and
     * keeps the block; told yes, it lost the block at emission.
     */
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
        return false;

    /*
     * And the rest of what lower_one refuses, which nothing named here
     * until a frontend asked: CLMUL and AESSB, which an M7 has no
     * instruction for, and SELECT and POPCNT. The default promised CLMUL
     * to RV32's clmul, and GCC turns CoreMark's CRC loops into clmul: on
     * the F746 910,195 instructions fell back to the interpreter where 1
     * had, and CoreMark under the JIT was 32% slower, with every host
     * suite green. A broken promise is counted now (emu_ir_note_refused)
     * and printed as `unkept` in the run's report.
     */
    case EMU_IR_CLMUL:
    case EMU_IR_AESSB:
    case EMU_IR_SM4SB:
    case EMU_IR_SELECT:
    case EMU_IR_POPCNT:
        return false;

    /*
     * The guest's vector registers, with the DSP extension's SIMD within
     * a register: listed, because the default below promises whatever it
     * is not told.
     */
#if EMU_IR_VECTOR
    case EMU_IR_VOP: /* the DSP's, or MVE's -- whose averages are only rnu
                      * and rdn, which the DSP lowering does too */
        return vop_lowerable(aux) ||
               (have_mve() && (vop_mve(EMU_IR_VOP_OP(aux), EMU_IR_VOP_SEW(aux),
                                       EMU_IR_RM_RNU) ||
                               vop_wide_mve(EMU_IR_VOP_OP(aux), EMU_IR_VOP_SEW(aux))));
    case EMU_IR_VMEM: /* word moves; without a window, the helper alone */
        return true;
    case EMU_IR_VOPM: /* every form as unmasked, and the merge */
        if (EMU_IR_VOP_OP(aux) == EMU_IR_V_MERGE) {
            return EMU_IR_VOP_SEW(aux) <= 2u;
        }
        return vop_lowerable(aux);
    /* every reduction at every width; the widening sums from 8 and 16 */
    case EMU_IR_VRED:
        return EMU_IR_VRED_OP(aux) < EMU_IR_R_COUNT && EMU_IR_VRED_SEW(aux) <= 2u &&
               (!EMU_IR_R_IS_WIDE(EMU_IR_VRED_OP(aux)) || EMU_IR_VRED_SEW(aux) <= 1u);
    case EMU_IR_VMEMS: /* a loop of moves; without a window, the helper */
        return true;
    /*
     * VFP, all fifteen, under round-to-nearest as the scalar FP: another
     * mode is the helper's. Tininess is detected before rounding here,
     * as for the scalar operations (docs/TODO.md).
     */
    case EMU_IR_VFOP:
        return EMU_IR_VFOP_OP(aux) < EMU_IR_VF_COUNT &&
               EMU_IR_VFOP_RM(aux) == EMU_IR_FRM_RNE;
#else
    case EMU_IR_VOP:
    case EMU_IR_VOPM:
    case EMU_IR_VMEM:
    case EMU_IR_VRED:
    case EMU_IR_VMEMS:
    case EMU_IR_VFOP:
        return false;
#endif

#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE
    case EMU_IR_KERNEL:
        /*
         * Through bisect_allows, as CMP3: answering true here while that
         * list refused the operation lost every block with a kernel --
         * "declined 2", RV32 CoreMark 78% slower on the F746, while every
         * host check passed (they lower on x86-64).
         *
         * And a dot product only of the shape emit_dot_inline takes: the
         * call to the C kernel was no faster than the scalar loop.
         */
        if (((aux & 0x7Fu) == (uint8_t)EMU_IR_LOOP_DOT16 ||
             (aux & 0x7Fu) == (uint8_t)EMU_IR_LOOP_DOT16_F) &&
            (aux & EMU_IR_KERNEL_PLAIN) == 0u) {
            return false;
        }
        return bisect_allows((uint8_t)op);
#endif
    case EMU_IR_CMP3: /* named: the default promises what it does not see */
        return (aux == (uint8_t)EMU_IR_C_LT || aux == (uint8_t)EMU_IR_C_LTU) &&
               bisect_allows((uint8_t)op);

    default:
        return true;
    }
}

/* Does this block touch the FP unit, and so need FPSCR framed? */
static EMU_PER_CORE bool g_has_fp;

/* ------------------------------------------------------------------ */
/* Condition flags                                                     */
/* ------------------------------------------------------------------ */

/*
 * EMU_IR_SETF, from the processor's own NZCV.
 *
 * ARM has the flags this IR is describing, so the operation is
 * recomputed with its flag-setting form and the answer read back with
 * MRS: two instructions where deriving carry and overflow arithmetically
 * takes about fifteen, and no second statement of when an add overflows.
 *
 * The two new encoders that costs are the boundary this backend has been
 * caught at twice -- a register that did not fit assembled as a
 * *different valid instruction* -- so they are checked against the
 * assembler, r12 in each position included, and the bytes are written
 * down beside them in encode.c.
 *
 * **ARM's C on a subtract is the inverse of the borrow every guest here
 * defines.** SUBS sets C when there was *no* borrow; G4MH's CY, and the
 * IR's FS_SUB, mean borrow. One EOR, and it is the only place the host's
 * idea of a flag and the guest's disagree.
 *
 * `ADDS Rd, Rn, #0` is what a logical operation wants: value unchanged,
 * N and Z from it, C and V cleared. So FS_LOGIC needs no case of its own
 * -- extracting C and V from the APSR it produced yields the zeros the
 * definition asks for.
 *
 * Only live flags are extracted. pass_dead_flags has already reduced
 * `live` to what something reads, so a G4MH `cmp` feeding a `bne` pays
 * for Z alone.
 *
 * Registers: r0 scratch, r1 the second operand, r2 the recomputed
 * result, r3 the accumulator, r12 the captured APSR.
 */
#define SETF_A T2_R0
#define SETF_B T2_R1
#define SETF_RES T2_R2
#define SETF_ACC T2_R3
#define SETF_PSR T2_R12

/*
 * dst = the guest's flag `f` as 0 or 1, from a flag word already loaded
 * into `word`. A flag the frontend declares absent reads as false rather
 * than as the low bit of an unrelated register.
 */
static void setf_flag_bit(uint32_t dst, uint32_t word, const emu_ir_target_t *t,
                          unsigned f)
{
    if (t->flag_bit[f] == 0u) {
        t2_imm32(dst, 0u);
        return;
    }
    t2_ubfx(dst, word, (uint32_t)__builtin_ctz(t->flag_bit[f]), 1u);
}

/* C as the conditions read it, a borrow: ARM's inverted (flags_arm_carry). */
static void getcond_borrow(uint32_t rd, const emu_ir_target_t *t)
{
    if (t->flags_arm_carry && t->flag_bit[3] != 0u) {
        t2_eor_imm(rd, rd, 1u);
    }
}

static bool setf_wants(const emu_ir_insn_t *in, const emu_ir_target_t *t,
                       unsigned f)
{
    return (in->live & (1u << f)) != 0u && t->flag_bit[f] != 0u;
}

static void emit_setf(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const emu_ir_flagsrc_t src = (emu_ir_flagsrc_t)in->aux;
    const bool arith = (src == EMU_IR_FS_ADD) || (src == EMU_IR_FS_SUB);
    /* Where each guest flag sits in the APSR: Z, S(N), V, C. */
    static const uint8_t k_psr_bit[4] = {30u, 31u, 28u, 29u};

    if (in->live == 0u) {
        return;
    }

    /*
     * The operands where they are: copied into r0 and r1 first, a G4MH
     * `add` paid two MOVs before its ADDS on every SETF, five times in
     * the hottest block of its CoreMark profile. use_reg answers an
     * allocated register or the scratch it is given: `b` is read again
     * at the end (a shift's carry), so it must not be r0, r3 or r12,
     * which the sequence writes -- r1 or allocated -- and `a` may be r0,
     * its scratch, because nothing reads it after the ADDS.
     */
    const uint32_t ra = use_reg(in->a, SETF_A);
    const uint32_t rb = (arith || src == EMU_IR_FS_SHIFT) ? use_reg(in->b, SETF_B) : SETF_B;

    /*
     * The IR hands over the *operands* for ADD and SUB -- carry and
     * overflow cannot be recovered from the result alone -- so the
     * operation is recomputed here. For everything else `a` is already
     * the result and the add of zero is only there to set N and Z.
     *
     * MRS immediately after, with nothing between that could disturb
     * what was just set.
     */
    if (src == EMU_IR_FS_ADD) {
        t2_adds(SETF_RES, ra, rb);
    } else if (src == EMU_IR_FS_SUB) {
        t2_subs(SETF_RES, ra, rb);
    } else {
        t2_adds_imm(SETF_RES, ra, 0u);
    }
    t2_mrs_apsr(SETF_PSR);

    /*
     * A word that holds nothing but the four, all of them live, in the
     * APSR's own places: the APSR is the word, less Q and GE and with a
     * subtract's C made a borrow -- no load, no bit-field moves
     * (flags_alone). A shift's C comes from `b`, not the APSR.
     */
    if (t->flags_alone && (in->live & EMU_IR_F_ALL) == EMU_IR_F_ALL &&
        t->flag_bit[0] == (1u << 30) && t->flag_bit[1] == (1u << 31) &&
        t->flag_bit[2] == (1u << 28) && t->flag_bit[3] == (1u << 29) &&
        src != EMU_IR_FS_SHIFT && src != EMU_IR_FS_ZS) {
        uint16_t imm;

        if (src == EMU_IR_FS_SUB && !t->flags_arm_carry) {
            (void)t2_expand_imm(0x20000000u, &imm); /* one byte, rotated: encodes */
            t2_eor_imm(SETF_PSR, SETF_PSR, imm);
        }
        (void)t2_expand_imm(0xF0000000u, &imm);
        t2_and_imm(SETF_PSR, SETF_PSR, imm);
        t2_str_imm(SETF_PSR, T2_CPU, t->flags_offset);
        return;
    }

    /*
     * Into the guest's flag word in place, a run of flags at a time:
     * read-modify-write, because the word holds more than these four
     * bits -- G4MH's PSW carries the interrupt-disable and privilege
     * state, and clobbering those is a control-flow bug. A run is flags
     * adjacent in the APSR and in the same order in the guest's word,
     * one UBFX and one BFI: G4MH's Z,S sit as the APSR's Z,N and its
     * OV,CY as V,C, so all four are two runs. Each flag was LSR, AND,
     * LSL and ORR, then a MOVW+MOVT mask -- 22 instructions where 5 do.
     */
    struct {
        uint8_t psr, dst, inv;
    } fl[4];
    uint32_t n = 0u;

    t2_ldr_imm(SETF_ACC, T2_CPU, t->flags_offset);
    for (unsigned f = 0; f < 4u; f++) {
        if (!setf_wants(in, t, f)) {
            continue;
        }
        if (src == EMU_IR_FS_SHIFT && f == 3u) {
            /*
             * A shift's carry is the bit that left, which the frontend
             * computed and handed over in `b`; the APSR knows nothing
             * about it. Only bit 0 is read.
             */
            t2_bfi(SETF_ACC, rb, (uint32_t)__builtin_ctz(t->flag_bit[f]), 1u);
            continue;
        }
        fl[n].psr = k_psr_bit[f];
        fl[n].dst = (uint8_t)__builtin_ctz(t->flag_bit[f]);
        /* ARM's C after a subtract is no borrow: a borrow, unless the
         * guest's C is ARM's too (flags_arm_carry) */
        fl[n].inv = (f == 3u && src == EMU_IR_FS_SUB && !t->flags_arm_carry) ? 1u : 0u;
        for (uint32_t k = n; k > 0u && fl[k - 1u].psr > fl[k].psr; k--) {
            const uint8_t p = fl[k].psr, d = fl[k].dst, v = fl[k].inv;

            fl[k] = fl[k - 1u];
            fl[k - 1u].psr = p;
            fl[k - 1u].dst = d;
            fl[k - 1u].inv = v;
        }
        n++;
    }
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        uint32_t inv = fl[i].inv;

        while (j + 1u < n && fl[j + 1u].psr == fl[j].psr + 1u &&
               fl[j + 1u].dst == fl[j].dst + 1u) {
            j++;
            inv |= (uint32_t)fl[j].inv << (j - i);
        }
        t2_ubfx(SETF_A, SETF_PSR, fl[i].psr, j - i + 1u);
        if (inv != 0u) {
            t2_eor_imm(SETF_A, SETF_A, (uint16_t)inv); /* < 16: encodes as itself */
        }
        t2_bfi(SETF_ACC, SETF_A, fl[i].dst, j - i + 1u);
        i = j + 1u;
    }
    t2_str_imm(SETF_ACC, T2_CPU, t->flags_offset);
}

/*
 * EMU_IR_GETCOND: a guest condition, as 0 or 1, from the guest's own
 * flag word.
 *
 * **This is what actually blocked G4MH on the board**, and SETF was not.
 * Lowering SETF alone changed nothing measurable: with `-DT2_BISECT=8`
 * the N6 reported `declined 465 blocks 37 retired 846` and with SETF on
 * it reported the same to the digit, which by this file's own rule means
 * the code never ran. It could not -- a G4MH block that sets flags
 * almost always goes on to branch on them, the branch lowers to a
 * GETCOND, and a declined GETCOND takes the whole block including the
 * SETF that fed it. The flags and the branch have to arrive together or
 * neither arrives.
 *
 * Not from the host's APSR, unlike SETF beside it. SETF *recomputes* the
 * operation, so the flags it wants are the ones it just made; GETCOND is
 * asked long afterwards, with any number of instructions in between, and
 * the APSR by then describes whichever of them last set it. The guest's
 * flag word is the only thing that still holds the answer -- which is
 * exactly the bug the x86-64 version of this had, where a bare `setcc`
 * read host flags belonging to an address computation.
 *
 * The derivations are eval_cond's in src/backend/common/interp.c, which
 * is the reference: LT is S != V rather than S, and getting that from
 * one place is worth more than the three instructions it costs.
 */
static void emit_getcond(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const emu_ir_cond_t c = (emu_ir_cond_t)in->aux;
    const uint32_t rd = def_reg(in->dst, T2_R0);

    if (c == EMU_IR_C_ALWAYS) {
        t2_imm32(rd, 1u);
        st_slot(rd, in->dst);
        return;
    }

    t2_ldr_imm(T2_R12, T2_CPU, t->flags_offset);

    switch (c) {
    case EMU_IR_C_EQ:
    case EMU_IR_C_NE:
        setf_flag_bit(rd, T2_R12, t, 0); /* Z */
        break;

    case EMU_IR_C_LTU:
    case EMU_IR_C_GEU:
        setf_flag_bit(rd, T2_R12, t, 3); /* C */
        getcond_borrow(rd, t);
        break;

    case EMU_IR_C_LT:
    case EMU_IR_C_GE:
        setf_flag_bit(rd, T2_R12, t, 1); /* S */
        setf_flag_bit(T2_R1, T2_R12, t, 2); /* V */
        t2_eor(rd, rd, T2_R1); /* signed less: S != V */
        break;

    case EMU_IR_C_LE:
    case EMU_IR_C_GT:
        setf_flag_bit(rd, T2_R12, t, 1);
        setf_flag_bit(T2_R1, T2_R12, t, 2);
        t2_eor(rd, rd, T2_R1);
        setf_flag_bit(T2_R1, T2_R12, t, 0);
        t2_orr(rd, rd, T2_R1); /* Z || (S != V) */
        break;

    case EMU_IR_C_LEU:
    case EMU_IR_C_GTU:
        setf_flag_bit(rd, T2_R12, t, 3);
        getcond_borrow(rd, t);
        setf_flag_bit(T2_R1, T2_R12, t, 0);
        t2_orr(rd, rd, T2_R1); /* C || Z */
        break;

    case EMU_IR_C_MI:
    case EMU_IR_C_PL:
        setf_flag_bit(rd, T2_R12, t, 1); /* S */
        break;

    case EMU_IR_C_VS:
    case EMU_IR_C_VC:
        setf_flag_bit(rd, T2_R12, t, 2); /* V */
        break;

    default:
        t2_imm32(rd, 1u);
        st_slot(rd, in->dst);
        return;
    }

    /*
     * The seven negated forms are the positive one with bit 0 flipped,
     * which is exact because everything above is already 0 or 1.
     */
    if (c == EMU_IR_C_NE || c == EMU_IR_C_GEU || c == EMU_IR_C_GE ||
        c == EMU_IR_C_GT || c == EMU_IR_C_GTU || c == EMU_IR_C_PL ||
        c == EMU_IR_C_VC) {
        t2_eor_imm(rd, rd, 1u);
    }
    st_slot(rd, in->dst);
}

#if EMU_IR_VECTOR
/*
 * 128 bytes of ones then 128 of zeros: the word at window + 128 - k is the
 * mask of a word's first k bytes, none for k <= 0 and all for k >= 4, for
 * k from -124 to 128 -- every word of a group of up to eight registers
 * against a vl of up to the group's elements. Read unaligned, which the
 * M7 does for normal memory.
 */
#define VOP_ONES16                                                             \
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,      \
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
static const uint8_t k_vop_window[256] = {
    VOP_ONES16, VOP_ONES16, VOP_ONES16, VOP_ONES16,
    VOP_ONES16, VOP_ONES16, VOP_ONES16, VOP_ONES16,
};

/* Thumb-2 condition codes for the 32-bit minimum and maximum's IT. */
#define T2_CC_HI 8u
#define T2_CC_LO 3u
#define T2_CC_LT 11u
#define T2_CC_GT 12u

/*
 * One word's operation: r0 = r0 op rb, by lane. r2 is free to use: the
 * mask goes into it after. The DSP forms set GE and nothing else; the
 * 32-bit minimum and maximum compare and so write NZCV, which no IR
 * instruction here carries to the next -- EXIT_IF compares for itself,
 * SETF recomputes its result.
 */
static void vop_word(uint32_t op, uint32_t sew, uint32_t rb, uint32_t rm)
{
    const uint32_t add = (sew == 0u) ? T2_PAR_ADD8 : T2_PAR_ADD16;
    const uint32_t subp = (sew == 0u) ? T2_PAR_SUB8 : T2_PAR_SUB16;

    switch (op) {
    case EMU_IR_V_ADD:
        if (sew == 2u) {
            t2_add(T2_R0, T2_R0, rb);
        } else {
            t2_par(add, true, T2_R0, T2_R0, rb);
        }
        break;
    case EMU_IR_V_SUB:
        if (sew == 2u) {
            t2_sub(T2_R0, T2_R0, rb);
        } else {
            t2_par(subp, true, T2_R0, T2_R0, rb);
        }
        break;
    case EMU_IR_V_RSUB:
        if (sew == 2u) {
            t2_sub(T2_R0, rb, T2_R0);
        } else {
            t2_par(subp, true, T2_R0, rb, T2_R0);
        }
        break;
    case EMU_IR_V_AND:
        t2_and(T2_R0, T2_R0, rb);
        break;
    case EMU_IR_V_OR:
        t2_orr(T2_R0, T2_R0, rb);
        break;
    case EMU_IR_V_XOR:
        t2_eor(T2_R0, T2_R0, rb);
        break;
    case EMU_IR_V_MV:
        t2_mov(T2_R0, rb);
        break;
    case EMU_IR_V_MINU:
    case EMU_IR_V_MIN:
    case EMU_IR_V_MAXU:
    case EMU_IR_V_MAX: {
        const bool max = op == EMU_IR_V_MAXU || op == EMU_IR_V_MAX;
        const bool sign = op == EMU_IR_V_MIN || op == EMU_IR_V_MAX;

        if (sew == 2u) {
            /* if (a op b fails) a = b: unsigned max takes b when a < b */
            const uint32_t cc = max ? (sign ? T2_CC_LT : T2_CC_LO)
                                    : (sign ? T2_CC_GT : T2_CC_HI);

            t2_cmp(T2_R0, rb);
            t2_emit16((uint16_t)(0xBF00u | (cc << 4) | 0x8u)); /* IT cc */
            t2_mov(T2_R0, rb);
        } else {
            /* GE where a >= b, lane by lane; SEL takes a there for max */
            t2_par(subp, !sign, T2_R2, T2_R0, rb);
            if (max) {
                t2_sel(T2_R0, T2_R0, rb);
            } else {
                t2_sel(T2_R0, rb, T2_R0);
            }
        }
        break;
    }
    case EMU_IR_V_MUL:
        if (sew == 2u) {
            t2_mul(T2_R0, T2_R0, rb);
        } else {
            /* each halfword's low 16 bits: signed or not, the same */
            t2_smulxy(T2_R2, T2_R0, rb, false, false);
            t2_smulxy(T2_R0, T2_R0, rb, true, true);
            t2_pkhbt16(T2_R0, T2_R2, T2_R0);
        }
        break;
    /*
     * Round-to-nearest-up averages: the halving add, which truncates,
     * plus each lane's lost bit, (a ^ b) & 1 -- (a + b + 1) >> 1 is
     * ((a + b) >> 1) + ((a + b) & 1), and the lane cannot carry out. At 32
     * bits (a | b) - ((a ^ b) >> 1), arithmetic for the signed form.
     */
    case EMU_IR_V_AADDU:
    case EMU_IR_V_AADD: {
        /*
         * The other modes from the floor and the lost bit lb, by lane:
         * rne adds lb & floor, rdn is the floor, rod ORs lb in. At 32 bits
         * the floor is (a & b) + ((a ^ b) >> 1), one ADD with a shifted
         * operand, lb then (a ^ b) & 1.
         */
        const bool u = op == EMU_IR_V_AADDU;
        uint16_t enc;

        t2_eor(T2_R2, T2_R0, rb);
        if (sew == 2u && rm == EMU_IR_RM_RNU) {
            t2_shift_imm(u ? T2_LSR : T2_ASR, T2_R2, T2_R2, 1u);
            t2_orr(T2_R0, T2_R0, rb);
            t2_sub(T2_R0, T2_R0, T2_R2);
            break;
        }
        if (sew == 2u) {
            t2_and(T2_R0, T2_R0, rb);
            t2_add_shifted(T2_R0, T2_R0, T2_R2, u ? T2_LSR : T2_ASR, 1u);
            (void)t2_expand_imm(1u, &enc);
            t2_and_imm(T2_R2, T2_R2, enc);
        } else {
            (void)t2_expand_imm((sew == 0u) ? 0x01010101u : 0x00010001u, &enc);
            t2_and_imm(T2_R2, T2_R2, enc);
            t2_par_h(add, u, T2_R0, T2_R0, rb);
        }
        if (rm == EMU_IR_RM_RNE) {
            t2_and(T2_R2, T2_R2, T2_R0);
        }
        if (rm == EMU_IR_RM_RNU || rm == EMU_IR_RM_RNE) {
            if (sew == 2u) {
                t2_add(T2_R0, T2_R0, T2_R2);
            } else {
                t2_par(add, true, T2_R0, T2_R0, T2_R2);
            }
        } else if (rm == EMU_IR_RM_ROD) {
            t2_orr(T2_R0, T2_R0, T2_R2);
        }
        break;
    }
    /* r2 holds d, the word of vd as it was (lower_vop loads it). */
    case EMU_IR_V_MACC: /* d + s1 * s2 */
    case EMU_IR_V_NMSAC: /* d - s1 * s2 */
    case EMU_IR_V_MADD: /* s1 * d + s2 */
    case EMU_IR_V_NMSUB: /* s2 - s1 * d */
        if (sew == 1u) {
            /*
             * Two halfword lanes: each product's low 16 bits, signed or
             * not the same, from SMULBB and SMULTT (lr, which lower_vop
             * leaves free for this: the element mask comes after),
             * packed by PKHBT, then added or subtracted by lane.
             */
            const bool by_d = op == EMU_IR_V_MADD || op == EMU_IR_V_NMSUB;
            const uint32_t x = by_d ? T2_R2 : T2_R0; /* times s1 */
            const uint32_t y = by_d ? T2_R0 : T2_R2; /* what it adds to */
            const bool sub = op == EMU_IR_V_NMSAC || op == EMU_IR_V_NMSUB;

            t2_smulxy(T2_LR, x, rb, true, true);
            t2_smulxy(x, x, rb, false, false);
            t2_pkhbt16(x, x, T2_LR);
            t2_par(sub ? T2_PAR_SUB16 : T2_PAR_ADD16, true, T2_R0, y, x);
            break;
        }
        if (op == EMU_IR_V_MACC) {
            t2_mla(T2_R0, T2_R0, rb, T2_R2, false);
        } else if (op == EMU_IR_V_NMSAC) {
            t2_mla(T2_R0, T2_R0, rb, T2_R2, true);
        } else if (op == EMU_IR_V_MADD) {
            t2_mla(T2_R0, rb, T2_R2, T2_R0, false);
        } else {
            t2_mla(T2_R0, rb, T2_R2, T2_R0, true);
        }
        break;
    case EMU_IR_V_SLL:
        t2_shift_reg(T2_LSL, T2_R0, T2_R0, rb);
        break;
    case EMU_IR_V_SRL:
        t2_shift_reg(T2_LSR, T2_R0, T2_R0, rb);
        break;
    default: /* EMU_IR_V_SRA */
        t2_shift_reg(T2_ASR, T2_R0, T2_R0, rb);
        break;
    }
}

/*
 * EMU_IR_VOP, a word at a time: vs2's word into r0, the second operand
 * in r1 (vs1's word) or r12 (the scalar, masked to the element and
 * repeated across the word's lanes, or a shift's count), the operation,
 * then vd's bytes past vl kept through the window: r3 is window + 128
 * less vl's bytes for the whole group, so a word's mask is one LDR from
 * it, and (new & mask) | (old & ~mask) is AND, BIC, ORR.
 */
/*
 * vwmacc from signed halfwords: a source word holds elements 2k and
 * 2k + 1, whose products go to vd's words 2k and 2k + 1 -- SMLABB and
 * SMLATT, each accumulating into the word it replaces. Both products are
 * formed before either is kept (r2 and lr), so r0 and r1 are free for
 * the masks and the old words. lr is free inside a block: every block
 * pushes it, and nothing here calls.
 */
/*
 * Under a mask, vd's word k -- one 32-bit element of a widening
 * operation -- is kept only where element k's bit is set: r0, its word
 * of the window's mask, cleared where it is not. r2 and lr hold the
 * products, so the bit is tested in place: LDRB, TST, and a MOV under IT.
 * NZCV go with it, which no IR instruction carries.
 */
static void vop_wide_elem_bit(const emu_ir_target_t *t, uint32_t mreg, uint32_t k)
{
    t2_ldrb_imm(T2_R1, T2_CPU, t->vreg_offset(mreg) + (k >> 3));
    (void)t2_tst_imm8(T2_R1, 1u << (k & 7u));
    t2_emit16((uint16_t)(0xBF00u | (0x0u << 4) | 0x8u)); /* IT EQ */
    t2_emit16(0x2000u);                                  /* MOV r0, #0 */
}

static void lower_vop_wide(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const bool masked = in->op == (uint8_t)EMU_IR_VOPM;
    const uint32_t mreg = EMU_IR_VOP_MREG(in->imm);
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    /* a is vs2's element, b vs1's or the scalar, as emu_ir_vop_ref */
    const bool sign_a = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCUS;
    const bool sign_b = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCSU;
    const bool vx = EMU_IR_VOP_VX(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vd = EMU_IR_VOP_VD(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const uint32_t vs1 = EMU_IR_VOP_VS1(in->imm);

    if (vx) {
        ld_operand(T2_R12, in->a);
        t2_uxth(T2_R12, T2_R12);
        t2_add_lsl(T2_R12, T2_R12, T2_R12, 16u);
    }
    ld_operand(T2_R3, in->b);
    t2_shift_imm(T2_LSL, T2_R3, T2_R3, 2u); /* vl in vd's bytes */
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);

    for (uint32_t r = 0; r < nregs; r++) {
        for (uint32_t w = 0; w < EMU_IR_VREG_BYTES / 4u; w++) {
            /* vd's words 2w and 2w + 1 of its registers 2r and 2r + 1 */
            const uint32_t k = 8u * r + 2u * w; /* vd's word index */
            const uint32_t olo = t->vreg_offset(vd + k / 4u) + 4u * (k % 4u);
            const uint32_t ohi = t->vreg_offset(vd + (k + 1u) / 4u) +
                                 4u * ((k + 1u) % 4u);
            uint32_t rb = T2_R12;

            t2_ldr_imm(T2_R0, T2_CPU, t->vreg_offset(vs2 + r) + 4u * w);
            if (!vx) {
                t2_ldr_imm(T2_R1, T2_CPU, t->vreg_offset(vs1 + r) + 4u * w);
                rb = T2_R1;
            }
            if (op == EMU_IR_V_WMACC) {
                t2_ldr_imm(T2_R2, T2_CPU, olo);
                t2_smlaxy(T2_R2, T2_R0, rb, T2_R2, false, false);
                t2_ldr_imm(T2_LR, T2_CPU, ohi);
                t2_smlaxy(T2_LR, T2_R0, rb, T2_LR, true, true);
            } else {
                /*
                 * Unsigned or mixed: each halfword extended -- UXTH/SXTH
                 * for the low, LSR/ASR 16 for the high -- and MLA. f is
                 * whichever of r1 and r12 holds no operand.
                 */
                const uint32_t f = (rb == T2_R1) ? T2_R12 : T2_R1;

                if (sign_a) {
                    t2_sxth(T2_LR, T2_R0);
                } else {
                    t2_uxth(T2_LR, T2_R0);
                }
                if (sign_b) {
                    t2_sxth(f, rb);
                } else {
                    t2_uxth(f, rb);
                }
                t2_ldr_imm(T2_R2, T2_CPU, olo);
                t2_mla(T2_R2, T2_LR, f, T2_R2, false);
                t2_shift_imm(sign_a ? T2_ASR : T2_LSR, T2_LR, T2_R0, 16u);
                t2_shift_imm(sign_b ? T2_ASR : T2_LSR, f, rb, 16u);
                t2_mul(T2_LR, T2_LR, f);
                t2_ldr_imm(f, T2_CPU, ohi);
                t2_add(T2_LR, T2_LR, f);
            }

            t2_ldr_imm(T2_R0, T2_R3, 4u * k);  /* the masks */
            if (masked) {
                vop_wide_elem_bit(t, mreg, k);
            }
            t2_ldr_imm(T2_R1, T2_CPU, olo);
            t2_and(T2_R2, T2_R2, T2_R0);
            t2_bic(T2_R1, T2_R1, T2_R0);
            t2_orr(T2_R2, T2_R2, T2_R1);
            t2_str_imm(T2_R2, T2_CPU, olo);
            t2_ldr_imm(T2_R0, T2_R3, 4u * (k + 1u));
            if (masked) {
                vop_wide_elem_bit(t, mreg, k + 1u);
            }
            t2_ldr_imm(T2_R1, T2_CPU, ohi);
            t2_and(T2_LR, T2_LR, T2_R0);
            t2_bic(T2_R1, T2_R1, T2_R0);
            t2_orr(T2_LR, T2_LR, T2_R1);
            t2_str_imm(T2_LR, T2_CPU, ohi);
        }
    }
}

/*
 * lr = the mask of word k of the group's elements -- each lane all ones
 * where its bit of mask register `mreg` is set -- with r2 for the
 * constants. The word's elements' bits are one, two or four bits of one
 * byte: a single one negated; two spread to the halfwords by 0x8001 and
 * 0x00010001; four to the bytes by 0x00204081 and 0x01010101; then each
 * lane's bit made its whole width by a multiply. lr is free inside a
 * block (every block pushes it).
 */
static void vop_word_mask(const emu_ir_target_t *t, uint32_t mreg, uint32_t sew,
                          uint32_t k)
{
    const uint32_t per = 4u >> sew;      /* elements a word */
    const uint32_t e = k * per;          /* the word's first element */
    uint16_t enc;

    t2_ldrb_imm(T2_LR, T2_CPU, t->vreg_offset(mreg) + (e >> 3));
    if ((e & 7u) != 0u) {
        t2_shift_imm(T2_LSR, T2_LR, T2_LR, e & 7u);
    }
    t2_and_imm(T2_LR, T2_LR, (uint16_t)((1u << per) - 1u));
    if (sew == 2u) {
        t2_neg(T2_LR, T2_LR);
        return;
    }
    t2_imm32(T2_R2, (sew == 1u) ? 0x8001u : 0x00204081u);
    t2_mul(T2_LR, T2_LR, T2_R2);
    (void)t2_expand_imm((sew == 1u) ? 0x00010001u : 0x01010101u, &enc);
    t2_and_imm(T2_LR, T2_LR, enc);
    t2_imm32(T2_R2, (sew == 1u) ? 0xFFFFu : 0xFFu);
    t2_mul(T2_LR, T2_LR, T2_R2);
}

/* r = (r != 0): CLZ is 32 only for 0, so bit 5 of it is the test. */
static void t2_nonzero(uint32_t r)
{
    t2_clz(r, r);
    t2_shift_imm(T2_LSR, r, r, 5u);
    t2_eor_imm(r, r, 1u);
}

/*
 * One 32-bit source element in `x`, shifted by r12 and, for the clip,
 * rounded by rm through lr (and r2 for rne): v[d-1] is ((x << 1) >> d)
 * & 1, 0 for a shift of 0; the bits below it are x & ((1 << d) - 1) --
 * for rne, (1 << d) >> 1 less 1 -- tested for 0 by t2_nonzero; v[d] is
 * bit 0 after the shift. rdn is the shift alone, rod ORs "anything
 * shifted out" in.
 */
static void vop_narrow_word(uint32_t op, uint32_t x, uint32_t rm)
{
    const bool clip = op == EMU_IR_V_NCLIP;

    if (op == EMU_IR_V_NSRL) {
        t2_shift_reg(T2_LSR, x, x, T2_R12);
        return;
    }
    if (clip && (rm == EMU_IR_RM_RNU || rm == EMU_IR_RM_RNE)) {
        t2_shift_imm(T2_LSL, T2_LR, x, 1u);
        t2_shift_reg(T2_LSR, T2_LR, T2_LR, T2_R12);
        t2_and_imm(T2_LR, T2_LR, 1u);
    }
    if (clip && rm == EMU_IR_RM_ROD) {
        t2_imm32(T2_LR, 1u);
        t2_shift_reg(T2_LSL, T2_LR, T2_LR, T2_R12);
        t2_sub_imm(T2_LR, T2_LR, 1u);
        t2_and(T2_LR, T2_LR, x);
        t2_nonzero(T2_LR);
    }
    if (clip && rm == EMU_IR_RM_RNE) {
        t2_imm32(T2_R2, 1u);
        t2_shift_reg(T2_LSL, T2_R2, T2_R2, T2_R12);
        t2_shift_imm(T2_LSR, T2_R2, T2_R2, 1u);
        t2_sub_imm(T2_R2, T2_R2, 1u);
        t2_and(T2_R2, T2_R2, x);
        t2_nonzero(T2_R2);
    }
    t2_shift_reg(T2_ASR, x, x, T2_R12);
    if (clip && rm == EMU_IR_RM_RNE) {
        t2_orr(T2_R2, T2_R2, x); /* or v[d] */
        t2_and(T2_LR, T2_LR, T2_R2);
        t2_and_imm(T2_LR, T2_LR, 1u);
    }
    if (clip && (rm == EMU_IR_RM_RNU || rm == EMU_IR_RM_RNE)) {
        t2_add(x, x, T2_LR);
    } else if (clip && rm == EMU_IR_RM_ROD) {
        t2_orr(x, x, T2_LR);
    }
}

/*
 * The narrowing operations to halfwords, a word of vd at a time from two
 * source words. The clip saturates by SSAT, and Q is its vxsat: cleared
 * first, and the tail's inputs zeroed by the word's mask before the SSAT
 * -- a halfword's 0xFFFF sign-extended (SXTH, ASR 16) -- so that only an
 * active element can set it; read after, into the saturation word, and
 * cleared again. NZCV go with it, which no IR instruction carries.
 */
static void lower_vop_narrow(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vd = EMU_IR_VOP_VD(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const bool clip = op == EMU_IR_V_NCLIP;

    ld_operand(T2_R12, in->a);
    t2_and_imm(T2_R12, T2_R12, 31u); /* the count, mod 32 */
    ld_operand(T2_R3, in->b);
    t2_shift_imm(T2_LSL, T2_R3, T2_R3, 1u); /* vl in vd's bytes */
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);
    if (clip) {
        t2_imm32(T2_R0, 0u);
        t2_msr_apsr_nzcvq(T2_R0);
    }
    for (uint32_t k = 0; k < 4u * nregs; k++) {
        /* vd's word k: source words 2k and 2k + 1 of vs2's group */
        const uint32_t od = t->vreg_offset(vd + k / 4u) + 4u * (k % 4u);
        const uint32_t o0 = t->vreg_offset(vs2 + (2u * k) / 4u) + 4u * ((2u * k) % 4u);
        const uint32_t o1 =
            t->vreg_offset(vs2 + (2u * k + 1u) / 4u) + 4u * ((2u * k + 1u) % 4u);

        t2_ldr_imm(T2_R0, T2_CPU, o0);
        t2_ldr_imm(T2_R1, T2_CPU, o1);
        vop_narrow_word(op, T2_R0, EMU_IR_VOP_RM(in->imm));
        vop_narrow_word(op, T2_R1, EMU_IR_VOP_RM(in->imm));
        if (in->op == (uint8_t)EMU_IR_VOPM) {
            /* lr: word k's elements' bits; r2 and lr are free here */
            vop_word_mask(t, EMU_IR_VOP_MREG(in->imm), 1u, k);
        }
        t2_ldr_imm(T2_R2, T2_R3, 4u * k); /* the mask */
        if (in->op == (uint8_t)EMU_IR_VOPM) {
            t2_and(T2_R2, T2_R2, T2_LR); /* and the set elements only */
        }
        if (clip) {
            t2_sxth(T2_LR, T2_R2);
            t2_and(T2_R0, T2_R0, T2_LR);
            t2_shift_imm(T2_ASR, T2_LR, T2_R2, 16u);
            t2_and(T2_R1, T2_R1, T2_LR);
            t2_ssat16(T2_R0, T2_R0);
            t2_ssat16(T2_R1, T2_R1);
        }
        t2_pkhbt16(T2_R0, T2_R0, T2_R1);
        t2_ldr_imm(T2_R1, T2_CPU, od);
        t2_and(T2_R0, T2_R0, T2_R2);
        t2_bic(T2_R1, T2_R1, T2_R2);
        t2_orr(T2_R0, T2_R0, T2_R1);
        t2_str_imm(T2_R0, T2_CPU, od);
    }
    if (clip) {
        t2_mrs_apsr(T2_R0);
        t2_shift_imm(T2_LSR, T2_R0, T2_R0, 27u);
        t2_and_imm(T2_R0, T2_R0, 1u); /* Q */
        t2_ldr_imm(T2_R1, T2_CPU, t->vsat_offset);
        t2_orr(T2_R1, T2_R1, T2_R0);
        t2_str_imm(T2_R1, T2_CPU, t->vsat_offset);
        t2_imm32(T2_R0, 0u);
        t2_msr_apsr_nzcvq(T2_R0);
    }
}

static void lower_vop(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    const uint32_t sew = EMU_IR_VOP_SEW(in->aux);
    const bool vx = EMU_IR_VOP_VX(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vd = EMU_IR_VOP_VD(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const uint32_t vs1 = EMU_IR_VOP_VS1(in->imm);
    const bool shift =
        op == EMU_IR_V_SLL || op == EMU_IR_V_SRL || op == EMU_IR_V_SRA;
    const bool masked = in->op == (uint8_t)EMU_IR_VOPM;

    if (EMU_IR_V_IS_WIDE(op)) {
        lower_vop_wide(in, t);
        return;
    }
    if (EMU_IR_V_IS_NARROW(op)) {
        lower_vop_narrow(in, t);
        return;
    }
    if (vx) {
        ld_operand(T2_R12, in->a);
        if (shift) {
            t2_and_imm(T2_R12, T2_R12, 31u);
        } else if (sew == 0u) {
            t2_uxtb(T2_R12, T2_R12);
            t2_add_lsl(T2_R12, T2_R12, T2_R12, 8u);
            t2_add_lsl(T2_R12, T2_R12, T2_R12, 16u);
        } else if (sew == 1u) {
            t2_uxth(T2_R12, T2_R12);
            t2_add_lsl(T2_R12, T2_R12, T2_R12, 16u);
        }
    }
    ld_operand(T2_R3, in->b);
    if (sew != 0u) {
        t2_shift_imm(T2_LSL, T2_R3, T2_R3, sew); /* vl in bytes */
    }
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);

    for (uint32_t r = 0; r < nregs; r++) {
        for (uint32_t w = 0; w < EMU_IR_VREG_BYTES / 4u; w++) {
            const uint32_t od = t->vreg_offset(vd + r) + 4u * w;
            uint32_t rb = T2_R12;

            if (!vx) {
                t2_ldr_imm(T2_R1, T2_CPU, t->vreg_offset(vs1 + r) + 4u * w);
                rb = T2_R1;
            }
            if (op != EMU_IR_V_MV) {
                t2_ldr_imm(T2_R0, T2_CPU, t->vreg_offset(vs2 + r) + 4u * w);
            }
            if (op >= EMU_IR_V_MACC && op <= EMU_IR_V_NMSUB) {
                t2_ldr_imm(T2_R2, T2_CPU, od); /* d, for the multiply-adds */
            }
            /*
             * The element mask before a merge, which chooses by it; after
             * any other operation, which may use lr and r2 -- d for a
             * multiply-add -- and needs the mask only for the keep.
             */
            if (masked && op == EMU_IR_V_MERGE) {
                vop_word_mask(t, EMU_IR_VOP_MREG(in->imm), sew, 4u * r + w);
            }
            if (op == EMU_IR_V_MERGE) {
                /* s1 where the bit is set, s2 (in r0) where not */
                t2_bic(T2_R0, T2_R0, T2_LR);
                t2_and(T2_R2, rb, T2_LR);
                t2_orr(T2_R0, T2_R0, T2_R2);
            } else {
                vop_word(op, sew, rb, EMU_IR_VOP_RM(in->imm));
                if (masked) {
                    vop_word_mask(t, EMU_IR_VOP_MREG(in->imm), sew, 4u * r + w);
                }
            }
            t2_ldr_imm(T2_R2, T2_R3, 16u * r + 4u * w); /* the mask */
            if (masked && op != EMU_IR_V_MERGE) {
                t2_and(T2_R2, T2_R2, T2_LR); /* and only the set bits */
            }
            t2_ldr_imm(T2_R1, T2_CPU, od);               /* the old word */
            t2_and(T2_R0, T2_R0, T2_R2);
            t2_bic(T2_R1, T2_R1, T2_R2);
            t2_orr(T2_R0, T2_R0, T2_R1);
            t2_str_imm(T2_R0, T2_CPU, od);
        }
    }
}

/*
 * EMU_IR_VOP in MVE, a register of the group at a time: vs2 in Q0, vs1
 * in Q1 -- or the scalar, duplicated into it once -- and vd's old value
 * in Q2 for the multiply-adds; the operation into Q0; then, unless vl
 * fills the group (Z, set once), vd's old value in Q2 and the tail's
 * byte mask from the window in Q3, Q0 = old ^ ((new ^ old) & mask); and
 * the store. r12 the
 * register file, r3 the window less vl's bytes, r0 a shift's count mask.
 * A shift's count is masked to the element's bits and, for a right
 * shift, negated: VSHL shifts by a signed count. Q0-Q3 only: Q4-Q7 are
 * S16-S31, which the block's caller expects kept.
 */
static void lower_vop_mve(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    const uint32_t sew = EMU_IR_VOP_SEW(in->aux);
    const bool vx = EMU_IR_VOP_VX(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vd = EMU_IR_VOP_VD(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const uint32_t vs1 = EMU_IR_VOP_VS1(in->imm);
    const uint32_t rm = EMU_IR_VOP_RM(in->imm);
    const bool shift = op == EMU_IR_V_SLL || op == EMU_IR_V_SRL || op == EMU_IR_V_SRA;
    const bool right = op == EMU_IR_V_SRL || op == EMU_IR_V_SRA;
    const uint16_t sz = T2_MVE_SZ(sew);
    const uint32_t base = t->vreg_offset(0u);

    if (vx) {
        ld_operand(T2_R0, in->a);
        if (shift) {
            t2_and_imm(T2_R0, T2_R0, (uint16_t)((8u << sew) - 1u));
            if (right) {
                t2_neg(T2_R0, T2_R0);
            }
        }
        t2_vdup(sew, 1u, T2_R0);
    } else if (shift) {
        (void)t2_movs_imm8(T2_R0, (8u << sew) - 1u);
    }
    ld_operand(T2_R3, in->b);
    if (sew != 0u) {
        t2_shift_imm(T2_LSL, T2_R3, T2_R3, sew); /* vl in bytes */
    }
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);
    if (base < 4096u) {
        t2_addw(T2_R12, T2_CPU, (uint16_t)base);
    } else {
        t2_imm32(T2_R12, base);
        t2_add(T2_R12, T2_R12, T2_CPU);
    }
    /*
     * Z: vl fills the group, so there is no tail to keep -- a loop's
     * every trip but its last. Nothing below writes the flags (MVE's
     * operations, loads and stores do not), so each register's blend is
     * branched over on it.
     */
    {
        uint16_t enc;

        (void)t2_expand_imm(EMU_IR_VREG_BYTES * nregs, &enc);
        t2_sub_imm(T2_R2, T2_R2, enc);
        t2_cmp(T2_R3, T2_R2);
    }
    for (uint32_t r = 0; r < nregs; r++) {
        const int32_t od = (int32_t)(EMU_IR_VREG_BYTES * (vd + r));
        const bool reads_d = op == EMU_IR_V_MACC || op == EMU_IR_V_NMSAC ||
                             op == EMU_IR_V_MADD || op == EMU_IR_V_NMSUB;

        if (op != EMU_IR_V_MV) {
            t2_vldrw(0u, T2_R12, (int32_t)(EMU_IR_VREG_BYTES * (vs2 + r)));
        }
        if (!vx) {
            t2_vldrw(1u, T2_R12, (int32_t)(EMU_IR_VREG_BYTES * (vs1 + r)));
            if (shift) {
                t2_vdup(sew, 3u, T2_R0);
                t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 1u, 1u, 3u);
                if (right) {
                    t2_vneg(sew, 1u, 1u);
                }
            }
        }
        if (reads_d) {
            t2_vldrw(2u, T2_R12, od);
        }
        switch (op) {
        case EMU_IR_V_ADD: t2_mve3(T2_MVE_VADD_HI | sz, T2_MVE_VADD_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_SUB: t2_mve3(T2_MVE_VSUB_HI | sz, T2_MVE_VSUB_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_RSUB: t2_mve3(T2_MVE_VSUB_HI | sz, T2_MVE_VSUB_LO, 0u, 1u, 0u); break;
        case EMU_IR_V_AND: t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_OR: t2_mve3(T2_MVE_VORR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_XOR: t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_MINU:
            t2_mve3(T2_MVE_VMIN_HI | T2_MVE_U | sz, T2_MVE_VMIN_LO, 0u, 0u, 1u);
            break;
        case EMU_IR_V_MIN: t2_mve3(T2_MVE_VMIN_HI | sz, T2_MVE_VMIN_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_MAXU:
            t2_mve3(T2_MVE_VMAX_HI | T2_MVE_U | sz, T2_MVE_VMAX_LO, 0u, 0u, 1u);
            break;
        case EMU_IR_V_MAX: t2_mve3(T2_MVE_VMAX_HI | sz, T2_MVE_VMAX_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_MUL: t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 0u, 0u, 1u); break;
        case EMU_IR_V_SLL:
        case EMU_IR_V_SRL: t2_vshl(sew, true, 0u, 0u, 1u); break;
        case EMU_IR_V_SRA: t2_vshl(sew, false, 0u, 0u, 1u); break;
        case EMU_IR_V_MV: t2_mve3(T2_MVE_VORR_HI, T2_MVE_VLOGIC_LO, 0u, 1u, 1u); break;
        case EMU_IR_V_AADDU:
        case EMU_IR_V_AADD: {
            const uint16_t u = (op == EMU_IR_V_AADDU) ? T2_MVE_U : 0u;

            if (rm == EMU_IR_RM_RNU) {
                t2_mve3(T2_MVE_VRHADD_HI | u | sz, T2_MVE_VRHADD_LO, 0u, 0u, 1u);
            } else { /* rdn */
                t2_mve3(T2_MVE_VHADD_HI | u | sz, T2_MVE_VHADD_LO, 0u, 0u, 1u);
            }
            break;
        }
        case EMU_IR_V_MACC: /* d + s1 * s2 */
            t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 0u, 0u, 1u);
            t2_mve3(T2_MVE_VADD_HI | sz, T2_MVE_VADD_LO, 0u, 0u, 2u);
            break;
        case EMU_IR_V_NMSAC: /* d - s1 * s2 */
            t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 0u, 0u, 1u);
            t2_mve3(T2_MVE_VSUB_HI | sz, T2_MVE_VSUB_LO, 0u, 2u, 0u);
            break;
        case EMU_IR_V_MADD: /* s1 * d + s2 */
            t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 3u, 1u, 2u);
            t2_mve3(T2_MVE_VADD_HI | sz, T2_MVE_VADD_LO, 0u, 0u, 3u);
            break;
        default: /* NMSUB: s2 - s1 * d */
            t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 3u, 1u, 2u);
            t2_mve3(T2_MVE_VSUB_HI | sz, T2_MVE_VSUB_LO, 0u, 0u, 3u);
            break;
        }
        {
            uint8_t *const full = t2_bcond_forward(0x0u); /* EQ: no tail */

            if (!reads_d) {
                t2_vldrw(2u, T2_R12, od);
            }
            t2_vldrb(3u, T2_R3, (int32_t)(16u * r)); /* the tail's byte mask */
            t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 2u);
            t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 3u);
            t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 2u);
            patch_branch(full, emu_jit_here(), true);
        }
        t2_vstrw(0u, T2_R12, od);
    }
}

/* r12 = the cpu's register file, from v0's offset. */
static void mve_vfile(const emu_ir_target_t *t)
{
    const uint32_t base = t->vreg_offset(0u);

    if (base < 4096u) {
        t2_addw(T2_R12, T2_CPU, (uint16_t)base);
    } else {
        t2_imm32(T2_R12, base);
        t2_add(T2_R12, T2_R12, T2_CPU);
    }
}

/* rd = r12 + off, a register of the file's address in a low register. */
static void mve_vreg_addr(uint32_t rd, uint32_t off)
{
    uint16_t enc;

    if (t2_expand_imm(off, &enc)) {
        t2_add_imm(rd, T2_R12, enc);
    } else {
        t2_addw(rd, T2_R12, (uint16_t)off);
    }
}

/*
 * EMU_IR_VOP's widening multiply-adds in MVE, from 8 or 16 bits: a
 * register of vd's group at a time, its elements' sources read widened
 * in order -- VLDRB.<S|U>16 or VLDRH.<S|U>32 from 8 bytes of vs2's group
 * (r1) and vs1's (r2), or the scalar extended and duplicated once -- vd
 * multiplied and added at the wide width, then the tail kept as
 * lower_vop_mve keeps it, unless vl fills the group.
 */
static void lower_vop_wide_mve(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    const uint32_t sew = EMU_IR_VOP_SEW(in->aux); /* the sources' */
    const bool vx = EMU_IR_VOP_VX(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm); /* the sources' */
    const uint32_t vd = EMU_IR_VOP_VD(in->imm);
    const bool sgn_a = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCUS; /* vs2 */
    const bool sgn_b = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCSU; /* vs1 */
    const uint32_t wsew = sew + 1u;
    const uint16_t sz = T2_MVE_SZ(wsew);
    uint16_t enc;

    if (vx) {
        ld_operand(T2_R0, in->a);
        if (sew == 0u) {
            if (sgn_b) {
                t2_sxtb(T2_R0, T2_R0);
            } else {
                t2_uxtb(T2_R0, T2_R0);
            }
        } else if (sgn_b) {
            t2_sxth(T2_R0, T2_R0);
        } else {
            t2_uxth(T2_R0, T2_R0);
        }
        t2_vdup(wsew, 1u, T2_R0);
    }
    ld_operand(T2_R3, in->b);
    t2_shift_imm(T2_LSL, T2_R3, T2_R3, wsew); /* vl in vd's bytes */
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);
    mve_vfile(t);
    mve_vreg_addr(T2_R1, EMU_IR_VREG_BYTES * EMU_IR_VOP_VS2(in->imm));
    (void)t2_expand_imm(EMU_IR_VREG_BYTES * 2u * nregs, &enc);
    t2_sub_imm(T2_R0, T2_R2, enc);
    if (!vx) {
        mve_vreg_addr(T2_R2, EMU_IR_VREG_BYTES * EMU_IR_VOP_VS1(in->imm));
    }
    t2_cmp(T2_R3, T2_R0); /* Z: vl fills vd's group */
    for (uint32_t j = 0; j < 2u * nregs; j++) {
        const int32_t od = (int32_t)(EMU_IR_VREG_BYTES * (vd + j));

        t2_vldr_widen(sew, sgn_a, 0u, T2_R1, 8u * j);
        if (!vx) {
            t2_vldr_widen(sew, sgn_b, 1u, T2_R2, 8u * j);
        }
        t2_vldrw(2u, T2_R12, od);
        t2_mve3(T2_MVE_VMUL_HI | sz, T2_MVE_VMUL_LO, 0u, 0u, 1u);
        t2_mve3(T2_MVE_VADD_HI | sz, T2_MVE_VADD_LO, 0u, 0u, 2u);
        {
            uint8_t *const full = t2_bcond_forward(0x0u); /* EQ: no tail */

            t2_vldrb(3u, T2_R3, (int32_t)(16u * j));
            t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 2u);
            t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 3u);
            t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 2u);
            patch_branch(full, emu_jit_here(), true);
        }
        t2_vstrw(0u, T2_R12, od);
    }
}

/*
 * The narrowing shifts and clips in MVE, to 8 or 16 bits: a register of
 * vs2's group at a time, at the wide width -- shifted right by the count
 * masked to the wide element's bits (VSHL by its negation; VRSHL, which
 * rounds half up, for a clip under rnu), a clip clamped to the narrow
 * range with VMIN/VMAX and its saturated active lanes found by comparing
 * with the unclamped value (VCMP into P0, ANDed with the active lanes,
 * ORed into lr) -- then its active elements' low halves stored by a
 * predicated narrowing store into 8 bytes of vd's group (r1). The count
 * is the scalar, duplicated once, or vs1's elements read widened from r2.
 * vxsat is set at the end if lr is not 0.
 */
static void lower_vop_narrow_mve(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VOP_OP(in->aux);
    const uint32_t sew = EMU_IR_VOP_SEW(in->aux); /* the result's */
    const bool vx = EMU_IR_VOP_VX(in->aux);
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm); /* the result's */
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const uint32_t rm = EMU_IR_VOP_RM(in->imm);
    const uint32_t wsew = sew + 1u;
    const uint16_t sz = T2_MVE_SZ(wsew);
    const bool clip = op == EMU_IR_V_NCLIPU || op == EMU_IR_V_NCLIP;
    const bool sign = op == EMU_IR_V_NSRA || op == EMU_IR_V_NCLIP;
    const uint32_t cmask = (16u << sew) - 1u; /* the wide element's bits */
    const uint32_t per = EMU_IR_VREG_BYTES >> wsew; /* elements a source register */
    const uint32_t bits = 8u << sew;
    uint16_t enc;

    if (vx) {
        ld_operand(T2_R0, in->a);
        t2_and_imm(T2_R0, T2_R0, (uint16_t)cmask);
        t2_neg(T2_R0, T2_R0);
        t2_vdup(wsew, 1u, T2_R0);
    }
    ld_operand(T2_R3, in->b);
    mve_vfile(t);
    if (!vx) {
        mve_vreg_addr(T2_R2, EMU_IR_VREG_BYTES * EMU_IR_VOP_VS1(in->imm));
    }
    if (clip) {
        /* Not MOVS: its 16-bit form has no lr, and t2_movs_imm8 then emits
         * nothing -- lr kept what it held, and vxsat read it as set. */
        t2_imm32(T2_LR, 0u);
    }
    for (uint32_t i = 0; i < 2u * nregs; i++) {
        t2_vldrw(0u, T2_R12, (int32_t)(EMU_IR_VREG_BYTES * (vs2 + i)));
        if (!vx) {
            t2_vldr_widen(sew, false, 1u, T2_R2, 8u * i);
            t2_imm32(T2_R0, cmask);
            t2_vdup(wsew, 3u, T2_R0);
            t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 1u, 1u, 3u);
            t2_vneg(wsew, 1u, 1u);
        }
        if (clip && rm == EMU_IR_RM_RNU) {
            t2_mve3((uint16_t)(T2_MVE_VRSHL_HI | (sign ? 0u : T2_MVE_U) | sz), T2_MVE_VRSHL_LO,
                    0u, 1u, 0u);
        } else {
            t2_vshl(wsew, !sign, 0u, 0u, 1u);
        }
        /* the active elements' count, into r0 */
        if (i == 0u) {
            t2_usat(T2_R0, 8u, T2_R3);
        } else {
            (void)t2_expand_imm(per * i, &enc);
            t2_sub_imm(T2_R0, T2_R3, enc);
            t2_usat(T2_R0, 8u, T2_R0);
        }
        if (clip) {
            t2_mve3(T2_MVE_VORR_HI, T2_MVE_VLOGIC_LO, 2u, 0u, 0u); /* unclamped */
            if (sign) {
                t2_imm32(T2_R1, (1u << (bits - 1u)) - 1u);
                t2_vdup(wsew, 3u, T2_R1);
                t2_mve3(T2_MVE_VMIN_HI | sz, T2_MVE_VMIN_LO, 0u, 0u, 3u);
                t2_imm32(T2_R1, ~((1u << (bits - 1u)) - 1u));
                t2_vdup(wsew, 3u, T2_R1);
                t2_mve3(T2_MVE_VMAX_HI | sz, T2_MVE_VMAX_LO, 0u, 0u, 3u);
            } else {
                t2_imm32(T2_R1, (1u << bits) - 1u);
                t2_vdup(wsew, 3u, T2_R1);
                t2_mve3(T2_MVE_VMIN_HI | T2_MVE_U | sz, T2_MVE_VMIN_LO, 0u, 0u, 3u);
            }
            t2_vctp(wsew, T2_R0);
            t2_vmrs_p0(T2_R1); /* the active lanes */
            t2_vcmp_ne(wsew, 0u, 2u);
            t2_vmrs_p0(T2_R0); /* the saturated ones */
            t2_and(T2_R0, T2_R0, T2_R1);
            t2_orr(T2_LR, T2_LR, T2_R0);
            t2_vmsr_p0(T2_R1);
        } else {
            t2_vctp(wsew, T2_R0);
        }
        mve_vreg_addr(T2_R1, EMU_IR_VREG_BYTES * EMU_IR_VOP_VD(in->imm));
        t2_vpst();
        t2_vstr_narrow(sew, 0u, T2_R1, 8u * i);
    }
    if (clip) {
        (void)t2_cmp_imm8(T2_LR, 0u);
        uint8_t *const none = t2_bcond_forward(0x0u); /* EQ: nothing saturated */

        t2_ldr_imm(T2_R0, T2_CPU, t->vsat_offset);
        t2_orr_imm(T2_R0, T2_R0, 1u);
        t2_str_imm(T2_R0, T2_CPU, t->vsat_offset);
        patch_branch(none, emu_jit_here(), true);
    }
}

/* What each single-width reduction combines with, as an EMU_IR_V_* op. */
static const uint8_t k_t2_vred_vop[8] = {
    EMU_IR_V_ADD,  EMU_IR_V_AND, EMU_IR_V_OR,   EMU_IR_V_XOR,
    EMU_IR_V_MINU, EMU_IR_V_MIN, EMU_IR_V_MAXU, EMU_IR_V_MAX,
};

/* A word of the identity: what an inactive lane becomes. */
static uint32_t vred_identity(uint32_t rop, uint32_t sew)
{
    static const uint32_t smax[3] = {0x7F7F7F7Fu, 0x7FFF7FFFu, 0x7FFFFFFFu};

    switch (rop) {
    case EMU_IR_R_AND:
    case EMU_IR_R_MINU:
        return 0xFFFFFFFFu;
    case EMU_IR_R_MIN:
        return smax[sew];
    case EMU_IR_R_MAX:
        return ~smax[sew];
    default:
        return 0u;
    }
}

/*
 * EMU_IR_VRED, a word of vs2's group at a time into r0: the inactive
 * lanes -- past vl through the window's mask in r2, or with their mask
 * bit clear -- set to the identity (cleared for the sums, OR, XOR and the
 * unsigned maximum; set for AND and the unsigned minimum; r12's for the
 * signed two), then combined into r1 lane by lane with vop_word; the
 * widening sums add each lane extended instead, SXTAB16/UXTAB16 from
 * bytes and SXTAH/UXTAH from halfwords, twice a word. Then r1's lanes
 * folded into lane 0 by rotating it a half and a quarter; then vs1's
 * element 0; then lane 0 stored into vd unless vl is 0.
 */
static void lower_vred(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t rop = EMU_IR_VRED_OP(in->aux);
    const uint32_t sew = EMU_IR_VRED_SEW(in->aux);
    const bool masked = EMU_IR_VRED_MASKED(in->aux);
    const bool wide = EMU_IR_R_IS_WIDE(rop);
    const uint32_t rsew = wide ? sew + 1u : sew;
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const uint32_t vop = wide ? EMU_IR_V_ADD : k_t2_vred_vop[rop];
    const uint32_t ident = wide ? 0u : vred_identity(rop, sew);
    const bool ones = ident == 0xFFFFFFFFu;
    const bool zero = ident == 0u;

    ld_operand(T2_R3, in->b);
    if (sew != 0u) {
        t2_shift_imm(T2_LSL, T2_R3, T2_R3, sew); /* vl in bytes */
    }
    t2_imm32(T2_R2, (uint32_t)(uintptr_t)(k_vop_window + 128u));
    t2_sub(T2_R3, T2_R2, T2_R3);
    t2_imm32(T2_R1, ident);
    if (!zero && !ones) {
        t2_imm32(T2_R12, ident);
    }
    for (uint32_t r = 0; r < nregs; r++) {
        for (uint32_t w = 0; w < EMU_IR_VREG_BYTES / 4u; w++) {
            t2_ldr_imm(T2_R0, T2_CPU, t->vreg_offset(vs2 + r) + 4u * w);
            if (masked) {
                vop_word_mask(t, EMU_IR_VOP_MREG(in->imm), sew, 4u * r + w);
            }
            t2_ldr_imm(T2_R2, T2_R3, 16u * r + 4u * w); /* the tail's mask */
            if (masked) {
                t2_and(T2_R2, T2_R2, T2_LR);
            }
            if (ones) {
                t2_mvn(T2_R2, T2_R2);
                t2_orr(T2_R0, T2_R0, T2_R2);
            } else {
                t2_and(T2_R0, T2_R0, T2_R2);
                if (!zero) {
                    t2_bic(T2_R2, T2_R12, T2_R2);
                    t2_orr(T2_R0, T2_R0, T2_R2);
                }
            }
            if (wide) {
                const bool u = rop == EMU_IR_R_WSUMU;
                const uint32_t kind = (sew == 0u)
                                          ? (u ? T2_XTA_UXTAB16 : T2_XTA_SXTAB16)
                                          : (u ? T2_XTA_UXTAH : T2_XTA_SXTAH);

                t2_xta(kind, T2_R1, T2_R1, T2_R0, 0u);
                t2_xta(kind, T2_R1, T2_R1, T2_R0, (sew == 0u) ? 1u : 2u);
            } else {
                vop_word(vop, sew, T2_R1, EMU_IR_RM_RNU);
                t2_mov(T2_R1, T2_R0);
            }
        }
    }
    /* r1's lanes into lane 0: with itself rotated by 16, then by 8. */
    for (uint32_t rot = 16u; rot >= (8u << rsew); rot >>= 1) {
        t2_shift_imm(T2_ROR, T2_R0, T2_R1, rot);
        vop_word(vop, rsew, T2_R1, EMU_IR_RM_RNU);
        t2_mov(T2_R1, T2_R0);
    }
    t2_ldr_imm(T2_R0, T2_CPU, t->vreg_offset(EMU_IR_VOP_VS1(in->imm)));
    vop_word(vop, rsew, T2_R1, EMU_IR_RM_RNU);

    ld_operand(T2_R2, in->b);
    (void)t2_cmp_imm8(T2_R2, 0u);
    uint8_t *const none = t2_bcond_forward(0x0u); /* EQ: vl 0, vd kept */

    t2_imm32(T2_R1, t->vreg_offset(EMU_IR_VOP_VD(in->imm)));
    (void)t2_st_reg(T2_R0, T2_CPU, T2_R1, 1u << rsew);
    patch_branch(none, emu_jit_here(), true);
}

/* A nibble's bits each doubled, and each quadrupled: a mask's element
 * bits spread to P0's byte bits at 16 and 32 bits. */
static const uint8_t k_spread2[16] = {
    0x00u, 0x03u, 0x0Cu, 0x0Fu, 0x30u, 0x33u, 0x3Cu, 0x3Fu,
    0xC0u, 0xC3u, 0xCCu, 0xCFu, 0xF0u, 0xF3u, 0xFCu, 0xFFu,
};
static const uint16_t k_spread4[16] = {
    0x0000u, 0x000Fu, 0x00F0u, 0x00FFu, 0x0F00u, 0x0F0Fu, 0x0FF0u, 0x0FFFu,
    0xF000u, 0xF00Fu, 0xF0F0u, 0xF0FFu, 0xFF00u, 0xFF0Fu, 0xFFF0u, 0xFFFFu,
};

/*
 * P0 for register r of a group: its active elements -- VCTP of vl (r3)
 * less the register's first element, saturated at 0 -- and under a mask
 * only those whose bit in the mask register (16 * mreg from r12) is set,
 * the bits spread to bytes. r1, r2 and lr are scratch.
 */
static void mve_active(uint32_t sew, uint32_t r, bool masked, uint32_t mreg)
{
    const uint32_t per = EMU_IR_VREG_BYTES >> sew; /* elements a register */
    const uint32_t mo = EMU_IR_VREG_BYTES * mreg;
    uint16_t enc;

    if (r == 0u) {
        t2_usat(T2_R2, 8u, T2_R3);
    } else {
        (void)t2_expand_imm(per * r, &enc);
        t2_sub_imm(T2_R2, T2_R3, enc);
        t2_usat(T2_R2, 8u, T2_R2);
    }
    t2_vctp(sew, T2_R2);
    if (!masked) {
        return;
    }
    switch (sew) {
    case 0u: /* 16 bits, one a byte */
        t2_imm32(T2_R1, mo + 2u * r);
        (void)t2_ld_reg(T2_R1, T2_R12, T2_R1, 2u, false);
        break;
    case 1u: /* 8 bits, each doubled */
        t2_ldrb_imm(T2_R1, T2_R12, mo + r);
        emit_pooled(T2_LR, (uint32_t)(uintptr_t)k_spread2);
        t2_and_imm(T2_R2, T2_R1, 15u);
        t2_shift_imm(T2_LSR, T2_R1, T2_R1, 4u);
        (void)t2_ld_reg(T2_R2, T2_LR, T2_R2, 1u, false);
        (void)t2_ld_reg(T2_R1, T2_LR, T2_R1, 1u, false);
        t2_add_lsl(T2_R1, T2_R2, T2_R1, 8u);
        break;
    default: /* 4 bits, each quadrupled */
        t2_ldrb_imm(T2_R1, T2_R12, mo + r / 2u);
        if ((r & 1u) != 0u) {
            t2_shift_imm(T2_LSR, T2_R1, T2_R1, 4u);
        }
        t2_and_imm(T2_R1, T2_R1, 15u);
        emit_pooled(T2_LR, (uint32_t)(uintptr_t)k_spread4);
        (void)t2_ld_reg_lsl(T2_R1, T2_LR, T2_R1, 1u, 2u, false);
        break;
    }
    t2_vmrs_p0(T2_R2);
    t2_and(T2_R2, T2_R2, T2_R1);
    t2_vmsr_p0(T2_R2);
}

/* The reductions MVE has an across-vector instruction for. */
static bool vred_mve(uint32_t rop)
{
    return rop != EMU_IR_R_AND && rop != EMU_IR_R_OR && rop != EMU_IR_R_XOR &&
           rop < EMU_IR_R_COUNT;
}

/*
 * EMU_IR_VRED in MVE: r0 starts as vs1's element 0 -- extended as the
 * operation reads it -- and each register of vs2's group goes into it
 * through a predicated across-vector instruction, P0 its active elements
 * (mve_active): VADDVA adds the lanes in 32 bits, which is also the
 * widening sums' arithmetic, VMINV and VMAXV take the extreme of r0's
 * element and the lanes. Inactive lanes take no part, so no identity is
 * needed. Then r0's element into vd unless vl is 0.
 */
static void lower_vred_mve(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t rop = EMU_IR_VRED_OP(in->aux);
    const uint32_t sew = EMU_IR_VRED_SEW(in->aux);
    const bool masked = EMU_IR_VRED_MASKED(in->aux);
    const uint32_t rsew = EMU_IR_R_IS_WIDE(rop) ? sew + 1u : sew;
    const uint32_t nregs = EMU_IR_VOP_NREGS(in->imm);
    const uint32_t vs2 = EMU_IR_VOP_VS2(in->imm);
    const bool sign = rop == EMU_IR_R_MIN || rop == EMU_IR_R_MAX || rop == EMU_IR_R_WSUM;
    const uint32_t base = t->vreg_offset(0u);

    ld_operand(T2_R3, in->b);
    if (base < 4096u) {
        t2_addw(T2_R12, T2_CPU, (uint16_t)base);
    } else {
        t2_imm32(T2_R12, base);
        t2_add(T2_R12, T2_R12, T2_CPU);
    }
    t2_imm32(T2_R0, EMU_IR_VREG_BYTES * EMU_IR_VOP_VS1(in->imm));
    (void)t2_ld_reg(T2_R0, T2_R12, T2_R0, 1u << rsew, sign && rsew < 2u);
    for (uint32_t r = 0; r < nregs; r++) {
        mve_active(sew, r, masked, EMU_IR_VOP_MREG(in->imm));
        t2_vldrw(0u, T2_R12, (int32_t)(EMU_IR_VREG_BYTES * (vs2 + r)));
        t2_vpst();
        switch (rop) {
        case EMU_IR_R_MINU:
        case EMU_IR_R_MIN:
            t2_vredv(T2_MVE_VMINV_HI, T2_MVE_VMINV_LO, sew, rop == EMU_IR_R_MINU, T2_R0, 0u);
            break;
        case EMU_IR_R_MAXU:
        case EMU_IR_R_MAX:
            t2_vredv(T2_MVE_VMAXV_HI, T2_MVE_VMAXV_LO, sew, rop == EMU_IR_R_MAXU, T2_R0, 0u);
            break;
        default: /* SUM, WSUMU, WSUM */
            t2_vredv(T2_MVE_VADDVA_HI, T2_MVE_VADDVA_LO, sew, rop != EMU_IR_R_WSUM, T2_R0,
                     0u);
            break;
        }
    }
    (void)t2_cmp_imm8(T2_R3, 0u);
    uint8_t *const none = t2_bcond_forward(0x0u); /* EQ: vl 0, vd kept */

    t2_imm32(T2_R1, EMU_IR_VREG_BYTES * EMU_IR_VOP_VD(in->imm));
    (void)t2_st_reg(T2_R0, T2_R12, T2_R1, 1u << rsew);
    patch_branch(none, emu_jit_here(), true);
}

/*
 * The element loop both inline vector accesses share: r2 the first
 * element's offset in the RAM window, r3 vl's bytes -- not 0, and every
 * element in the window, both checked by the caller -- and the stride in
 * `rs`, which the loop does not write. Then r12 the window's host
 * address, r3 the element's offset in it, r2 its offset in the cpu, lr
 * where that stops, r0 the element. The M7 does an unaligned halfword or
 * word for normal memory.
 */
static void emit_vmem_loop(uint32_t eew, bool store, uint32_t voff, uint32_t rs)
{
    const uint32_t esz = 1u << eew;
    uint16_t enc;

    t2_imm32(T2_R12, voff);
    t2_add(T2_LR, T2_R3, T2_R12); /* the end, in the cpu */
    t2_mov(T2_R3, T2_R2);
    t2_mov(T2_R2, T2_R12);
    emit_pooled(T2_R12, (uint32_t)(uintptr_t)g_fast.host);

    uint8_t *const loop = emu_jit_here();

    if (store) {
        (void)t2_ld_reg(T2_R0, T2_CPU, T2_R2, esz, false);
        (void)t2_st_reg(T2_R0, T2_R12, T2_R3, esz);
    } else {
        (void)t2_ld_reg(T2_R0, T2_R12, T2_R3, esz, false);
        (void)t2_st_reg(T2_R0, T2_CPU, T2_R2, esz);
    }
    t2_add(T2_R3, T2_R3, rs);
    (void)t2_expand_imm(esz, &enc);
    t2_add_imm(T2_R2, T2_R2, enc);
    t2_cmp(T2_R2, T2_LR);

    uint8_t *const back = t2_bcond_forward(0x1u); /* NE: the next */

    patch_branch(back, loop, true);
}

/*
 * EMU_IR_VMEMS: inline when vl is not 0, the stride under
 * EMU_IR_VMEMS_STRIDE_MAX, and the first and last elements both in the
 * window -- as x86-64's, the last as first + (vl - 1) * stride in an
 * unsigned word -- then a loop: r12 the window's host address, r3 the
 * element's offset in it, r2 its offset in the cpu, lr where that stops,
 * r0 the element, the stride where use_reg left it. The M7 does an
 * unaligned halfword or word for normal memory. Otherwise the helper,
 * r0 the cpu and then imm, the address and vl.
 */
static void lower_vmems(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t eew = EMU_IR_VMEM_EEW(in->imm);
    const uint32_t esz = 1u << eew;
    const bool store = EMU_IR_VMEM_STORE(in->imm);
    const uint32_t voff = t->vreg_offset(EMU_IR_VMEM_VD(in->imm));
    uint8_t *slow[4];
    uint32_t ns = 0u;
    /* Patched by kind: a B<c>.W given B.W's fields becomes a B.W. */
    uint8_t *none = NULL;
    uint8_t *done = NULL;
    uint16_t enc;

    if (g_has_fast && g_fast.size >= esz) {
        const uint32_t lim = g_fast.size - esz;

        ld_operand(T2_R3, in->b);
        (void)t2_cmp_imm8(T2_R3, 0u);
        none = t2_bcond_forward(0x0u); /* EQ: vl 0, nothing moves */

        const uint32_t rs = use_reg(in->c, T2_R1);

        (void)t2_expand_imm(EMU_IR_VMEMS_STRIDE_MAX, &enc);
        t2_add_imm(T2_R0, rs, enc);
        (void)t2_expand_imm(2u * EMU_IR_VMEMS_STRIDE_MAX, &enc);
        t2_cmp_imm(T2_R0, enc);
        slow[ns++] = t2_bcond_forward(0x2u); /* HS: a stride too large */

        const uint32_t ra = use_reg(in->a, T2_R2);

        if (t2_expand_imm(g_fast.base, &enc)) {
            t2_sub_imm(T2_R2, ra, enc);
        } else {
            emit_pooled(T2_R12, g_fast.base);
            t2_sub(T2_R2, ra, T2_R12);
        }
        emit_pooled(T2_R12, lim);
        t2_cmp(T2_R2, T2_R12);
        slow[ns++] = t2_bcond_forward(0x8u); /* HI: the first outside */
        (void)t2_expand_imm(1u, &enc);
        t2_sub_imm(T2_R0, T2_R3, enc);
        t2_mul(T2_R0, T2_R0, rs);
        t2_add(T2_R0, T2_R0, T2_R2);
        t2_cmp(T2_R0, T2_R12);
        slow[ns++] = t2_bcond_forward(0x8u); /* HI: the last outside */
        if (store && g_fast.store_guard_offset != EMU_IR_NO_GUARD) {
            t2_ldrb_imm(T2_R0, T2_CPU, g_fast.store_guard_offset);
            (void)t2_cmp_imm8(T2_R0, 0u);
            slow[ns++] = t2_bcond_forward(0x1u); /* NE: a reservation */
        }
        t2_shift_imm(T2_LSL, T2_R3, T2_R3, eew); /* vl's bytes */
        emit_vmem_loop(eew, store, voff, rs);
        done = t2_b_forward();
        for (uint32_t i = 0; i < ns; i++) {
            patch_branch(slow[i], emu_jit_here(), true);
        }
    }
    if (g_has_fp) {
        emit_fp_harvest(t, true);
    }

    const uint32_t rb = use_reg(in->b, T2_R3);

    if (rb != T2_R3) {
        t2_mov(T2_R3, rb);
    }

    const uint32_t ra = use_reg(in->a, T2_R2);

    if (ra != T2_R2) {
        t2_mov(T2_R2, ra);
    }
    t2_imm32(T2_R1, in->imm);
    pc_store(T2_R0, false); /* the helper may trap: a slow path */
    t2_mov(T2_R0, T2_CPU);
    t2_call(t->helpers[in->aux]);
    (void)t2_cmp_imm8(T2_R0, 0u);
    note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
    if (none != NULL) {
        patch_branch(none, emu_jit_here(), true);
        patch_branch(done, emu_jit_here(), false);
    }
}

/*
 * EMU_IR_VMEM: the inline copy when the block has a RAM window that can
 * hold the group -- vl's bytes against the group's, the offset in the
 * window against its size less the group's, the reservation guard for
 * a store -- a word at a time through r12, which the M7 does unaligned
 * for normal memory; anything else the helper, called as
 * EMU_IR_HELPER_TRAP's are with r0 the cpu, then c, the address and vl.
 */
static void emit_vmem_loop(uint32_t eew, bool store, uint32_t voff, uint32_t rs);

static void lower_vmem(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t bytes = EMU_IR_VMEM_BYTES(in->imm);
    const uint32_t eew = EMU_IR_VMEM_EEW(in->imm);
    const bool store = EMU_IR_VMEM_STORE(in->imm);
    const uint32_t vd = EMU_IR_VMEM_VD(in->imm);
    uint8_t *slow[7];
    uint32_t ns = 0u;
    uint8_t *done = NULL;
    uint8_t *done_part = NULL;
    uint16_t enc;

    if (g_has_fast && g_fast.size >= bytes) {
        ld_operand(T2_R3, in->b);
        if (eew != 0u) {
            t2_shift_imm(T2_LSL, T2_R3, T2_R3, eew); /* vl in bytes */
        }
        (void)t2_cmp_imm8(T2_R3, bytes);
        uint8_t *const part = t2_bcond_forward(0x1u); /* NE: not the whole group */

        const uint32_t ra = use_reg(in->a, T2_R1);

        if (t2_expand_imm(g_fast.base, &enc)) {
            t2_sub_imm(T2_R2, ra, enc);
        } else {
            emit_pooled(T2_R12, g_fast.base);
            t2_sub(T2_R2, ra, T2_R12);
        }
        if (t2_expand_imm(g_fast.size - bytes, &enc)) {
            t2_cmp_imm(T2_R2, enc);
        } else {
            emit_pooled(T2_R12, g_fast.size - bytes);
            t2_cmp(T2_R2, T2_R12);
        }
        slow[ns++] = t2_bcond_forward(0x8u); /* HI: past the window */
        if (store && g_fast.store_guard_offset != EMU_IR_NO_GUARD) {
            t2_ldrb_imm(T2_R0, T2_CPU, g_fast.store_guard_offset);
            (void)t2_cmp_imm8(T2_R0, 0u);
            slow[ns++] = t2_bcond_forward(0x1u); /* NE: a reservation */
        }
        emit_pooled(T2_R12, (uint32_t)(uintptr_t)g_fast.host);
        t2_add(T2_R12, T2_R12, T2_R2);
        /*
         * MVE: 16 bytes a pair, VLDRB/VSTRB on the RAM side -- any
         * alignment -- and VLDRW/VSTRW on the registers', from r2 at vd.
         */
        const bool mve_copy = have_mve() && vfile_mve(t);

        if (mve_copy) {
            const uint32_t vo = t->vreg_offset(vd);

            if (vo < 4096u) {
                t2_addw(T2_R2, T2_CPU, (uint16_t)vo);
            } else {
                t2_imm32(T2_R2, vo);
                t2_add(T2_R2, T2_R2, T2_CPU);
            }
            for (uint32_t r = 0; r < bytes / EMU_IR_VREG_BYTES; r++) {
                const int32_t o = (int32_t)(EMU_IR_VREG_BYTES * r);

                if (store) {
                    t2_vldrw(0u, T2_R2, o);
                    t2_vstrb(0u, T2_R12, o);
                } else {
                    t2_vldrb(0u, T2_R12, o);
                    t2_vstrw(0u, T2_R2, o);
                }
            }
        }
        for (uint32_t k = 0; !mve_copy && k < bytes / 4u; k++) {
            const uint32_t off =
                t->vreg_offset(vd + k / 4u) + 4u * (k % 4u);

            if (store) {
                t2_ldr_imm(T2_R0, T2_CPU, off);
                t2_str_imm(T2_R0, T2_R12, 4u * k);
            } else {
                t2_ldr_imm(T2_R0, T2_R12, 4u * k);
                t2_str_imm(T2_R0, T2_CPU, off);
            }
        }
        done = t2_b_forward();

        /*
         * Part of the group, if vl is not 0 and its bytes lie in the
         * window: memcpy, out of line -- r3 is vl's bytes here. It was
         * the element loop, inline at every access: about 90 bytes each,
         * and a byte a turn for an e8 group's last trip, which the
         * helper's own copy had done faster (vecbench 0.5% slower).
         */
        patch_branch(part, emu_jit_here(), true);
        (void)t2_cmp_imm8(T2_R3, 0u);
        slow[ns++] = t2_bcond_forward(0x0u); /* EQ: vl 0, the helper's */

        const uint32_t ra2 = use_reg(in->a, T2_R2);

        if (t2_expand_imm(g_fast.base, &enc)) {
            t2_sub_imm(T2_R2, ra2, enc);
        } else {
            emit_pooled(T2_R12, g_fast.base);
            t2_sub(T2_R2, ra2, T2_R12);
        }
        emit_pooled(T2_R12, g_fast.size);
        t2_cmp(T2_R2, T2_R12);
        slow[ns++] = t2_bcond_forward(0x8u); /* HI: starting outside */
        t2_add(T2_R0, T2_R2, T2_R3);
        t2_cmp(T2_R0, T2_R12);
        slow[ns++] = t2_bcond_forward(0x8u); /* HI: ending outside */
        if (store && g_fast.store_guard_offset != EMU_IR_NO_GUARD) {
            t2_ldrb_imm(T2_R0, T2_CPU, g_fast.store_guard_offset);
            (void)t2_cmp_imm8(T2_R0, 0u);
            slow[ns++] = t2_bcond_forward(0x1u); /* NE: a reservation */
        }
        {
            /* memcpy(dst, src, n): RAM and the registers, by direction */
            const uint32_t ram = store ? T2_R0 : T2_R1;
            const uint32_t reg = store ? T2_R1 : T2_R0;

            emit_pooled(ram, (uint32_t)(uintptr_t)g_fast.host);
            t2_add(ram, ram, T2_R2);
            t2_imm32(reg, t->vreg_offset(vd));
            t2_add(reg, reg, T2_CPU);
            t2_mov(T2_R2, T2_R3);
            t2_call((const void *)memcpy);
        }
        done_part = t2_b_forward();
        for (uint32_t i = 0; i < ns; i++) {
            patch_branch(slow[i], emu_jit_here(), true);
        }
    }
    if (g_has_fp) {
        emit_fp_harvest(t, true);
    }

    const uint32_t rb = use_reg(in->b, T2_R3);

    if (rb != T2_R3) {
        t2_mov(T2_R3, rb);
    }

    const uint32_t ra = use_reg(in->a, T2_R2);

    if (ra != T2_R2) {
        t2_mov(T2_R2, ra);
    }

    const uint32_t rc = use_reg(in->c, T2_R1);

    if (rc != T2_R1) {
        t2_mov(T2_R1, rc);
    }
    pc_store(T2_R0, false); /* the helper may trap: a slow path */
    t2_mov(T2_R0, T2_CPU);
    t2_call(t->helpers[in->aux]);
    (void)t2_cmp_imm8(T2_R0, 0u);
    note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
    if (done != NULL) {
        patch_branch(done, emu_jit_here(), false);
        patch_branch(done_part, emu_jit_here(), false);
    }
}

/*
 * EMU_IR_VFOP, an element at a time in VFP under the block's FPSCR:
 * round to nearest, and the default NaN, which is RISC-V's canonical
 * one; the flags are harvested at the exit as the scalar FP's are. r12
 * the vector file -- a group's registers adjacent in it, as the vector
 * moves' loop assumes too -- r3 vl's bytes, at most the group's, r2 the
 * element's offset, r0 r12 + r2, and for a masked operation r1 the
 * mask's first word, shifted out a bit an element: 32 elements at most.
 * An inactive element is not computed, so it raises nothing. An element
 * is written as soon as it is computed: element i reads only element i
 * of each source, and two groups overlap whole or not at all.
 *
 * The fused forms accumulate into s0 -- vd for vfmacc and its kin, vs2
 * for vfmadd's -- with the instruction the scalar FMA picks: NEG_ADD
 * (the addend negated) chooses VFNMS/VFNMA, NEG_MUL is `sub`.
 */
static void lower_vfop(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint32_t op = EMU_IR_VFOP_OP(in->aux);
    const uint32_t lanes = EMU_IR_VOP_NREGS(in->imm) * (EMU_IR_VREG_BYTES / 4u);
    const uint32_t od = EMU_IR_VREG_BYTES * EMU_IR_VOP_VD(in->imm);
    const uint32_t o2 = EMU_IR_VREG_BYTES * EMU_IR_VOP_VS2(in->imm);
    const uint32_t o1 = EMU_IR_VREG_BYTES * EMU_IR_VOP_VS1(in->imm);
    const bool masked = EMU_IR_VFOP_MASKED(in->imm) != 0u;
    const uint32_t sb = EMU_IR_VFOP_VF(in->imm) ? T2_S3 : T2_S2; /* b */
    const uint32_t base = t->vreg_offset(0u);
    uint8_t *skip = NULL;
    uint16_t enc;

    if (EMU_IR_VFOP_VF(in->imm)) {
        t2_vmov_core(T2_S3, use_reg(in->a, T2_R0), false);
    }
    ld_operand(T2_R3, in->b);
    (void)t2_cmp_imm8(T2_R3, lanes);

    uint8_t *const fits = t2_bcond_forward(0x9u); /* LS: vl within the group */

    (void)t2_movs_imm8(T2_R3, lanes);
    patch_branch(fits, emu_jit_here(), true);
    (void)t2_cmp_imm8(T2_R3, 0u);

    uint8_t *const none = t2_bcond_forward(0x0u); /* EQ: vl 0, vd kept */

    t2_shift_imm(T2_LSL, T2_R3, T2_R3, 2u);
    if (base < 4096u) {
        t2_addw(T2_R12, T2_CPU, (uint16_t)base);
    } else {
        t2_imm32(T2_R12, base);
        t2_add(T2_R12, T2_R12, T2_CPU);
    }
    if (masked) {
        t2_ldr_imm(T2_R1, T2_R12, EMU_IR_VREG_BYTES * EMU_IR_VOP_MREG(in->imm));
    }
    (void)t2_movs_imm8(T2_R2, 0u);

    uint8_t *const loop = emu_jit_here();

    if (masked) {
        t2_emit16(0x0849u); /* LSRS r1, r1, #1: this element's bit to C */
        skip = t2_bcond_forward(0x3u); /* CC: inactive */
    }
    t2_add(T2_R0, T2_R12, T2_R2);
    if (EMU_IR_VF_IS_FMA(op)) {
        const bool acc = op <= EMU_IR_VF_NMSAC; /* vd the addend */
        const bool neg_add = op == EMU_IR_VF_NMACC || op == EMU_IR_VF_MSAC ||
                             op == EMU_IR_VF_NMADD || op == EMU_IR_VF_MSUB;
        const bool neg_mul = op == EMU_IR_VF_NMACC || op == EMU_IR_VF_NMSAC ||
                             op == EMU_IR_VF_NMADD || op == EMU_IR_VF_NMSUB;

        t2_vldr(T2_S0, T2_R0, acc ? od : o2); /* the addend */
        t2_vldr(T2_S1, T2_R0, acc ? o2 : od); /* the multiplier */
        if (sb == T2_S2) {
            t2_vldr(T2_S2, T2_R0, o1);
        }
        t2_vfp3(neg_add ? T2_VFNMS_VFNMA : T2_VFMA_VFMS, neg_mul, T2_S0, sb, T2_S1);
    } else {
        t2_vldr(T2_S1, T2_R0, o2);
        if (sb == T2_S2 && op != EMU_IR_VF_SQRT) {
            t2_vldr(T2_S2, T2_R0, o1);
        }
        switch (op) {
        case EMU_IR_VF_ADD: t2_vfp3(T2_VADD, false, T2_S0, T2_S1, sb); break;
        case EMU_IR_VF_SUB: t2_vfp3(T2_VSUB, true, T2_S0, T2_S1, sb); break;
        case EMU_IR_VF_RSUB: t2_vfp3(T2_VSUB, true, T2_S0, sb, T2_S1); break;
        case EMU_IR_VF_MUL: t2_vfp3(T2_VMUL, false, T2_S0, T2_S1, sb); break;
        case EMU_IR_VF_DIV: t2_vfp3(T2_VDIV, false, T2_S0, T2_S1, sb); break;
        case EMU_IR_VF_RDIV: t2_vfp3(T2_VDIV, false, T2_S0, sb, T2_S1); break;
        default: t2_vsqrt(T2_S0, T2_S1); break; /* SQRT */
        }
    }
    /*
     * UF, as for the scalar operations (t2_uf_fix): at or below 2^-126 a
     * multiply, divide or fused multiply-add is redone in the guest's
     * arithmetic -- a, b and c reloaded into s0-s2, the loop's registers
     * kept across the call, and the scalar in s3, which it may clobber.
     */
    if (g_uf_vfop != NULL && (op == EMU_IR_VF_MUL || op == EMU_IR_VF_DIV ||
                              op == EMU_IR_VF_RDIV || EMU_IR_VF_IS_FMA(op))) {
        const uint32_t keep = 0x0Fu | (1u << T2_R12) | T2_LIST_LR;
        uint16_t e2;

        t2_vmov_core(T2_S0, T2_LR, true);
        t2_shift_imm(T2_LSL, T2_LR, T2_LR, 1u); /* the sign out */
        (void)t2_expand_imm(0x01000000u, &e2);
        t2_cmp_imm(T2_LR, e2);

        uint8_t *const ok = t2_bcond_forward(0x8u); /* HI: above 2^-126 */

        t2_vldr(T2_S0, T2_R0, o2);
        if (sb == T2_S3) {
            t2_vmov_core(T2_S3, T2_LR, true);
            t2_vmov_core(T2_S1, T2_LR, false);
        } else {
            t2_vldr(T2_S1, T2_R0, o1);
        }
        t2_vldr(T2_S2, T2_R0, od);
        t2_push(keep);
        t2_mov(T2_R0, T2_CPU);
        (void)t2_movs_imm8(T2_R1, op);
        t2_call((const void *)t2_uf_fix);
        t2_vmov_core(T2_S0, T2_R0, false);
        t2_pop(keep);
        if (sb == T2_S3) {
            t2_vmov_core(T2_S3, T2_LR, false);
        }
        patch_branch(ok, emu_jit_here(), true);
    }
    t2_vstr(T2_S0, T2_R0, od);
    if (skip != NULL) {
        patch_branch(skip, emu_jit_here(), true);
    }
    (void)t2_expand_imm(4u, &enc);
    t2_add_imm(T2_R2, T2_R2, enc);
    t2_cmp(T2_R2, T2_R3);

    uint8_t *const back = t2_bcond_forward(0x1u); /* NE: the next */

    patch_branch(back, loop, true);
    patch_branch(none, emu_jit_here(), true);
}

#endif /* EMU_IR_VECTOR */

static bool lower_one(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    g_r0_avail = g_r0_holds;
    g_r0_holds = EMU_IR_NO_TEMP;
    g_r0_first = true;

    if (!bisect_allows(in->op)) {
        return false;
    }

    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_NOP:
    case EMU_IR_RETIRE:
        if (in->op == (uint8_t)EMU_IR_RETIRE) {
            /* ADD.W r5, r5, #n -- no flags: they may be live here */
            const uint32_t n = (in->imm != 0u) ? in->imm : 1u;
            uint16_t enc;

            if (t2_expand_imm(n, &enc)) {
                t2_add_imm(T2_CNT, T2_CNT, enc);
            } else {
                t2_addw(T2_CNT, T2_CNT, (uint16_t)n);
            }
        }
        break;

    case EMU_IR_GET: {
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            t2_imm32(rd, 0u);
        } else {
            if (t->reg_offset(in->imm) == t->pc_offset) {
                pc_store(rd, true); /* a register that is the pc */
            }
            t2_ldr_imm(rd, T2_CPU, t->reg_offset(in->imm));
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_PUT:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            break;
        }
        if (t->reg_offset(in->imm) == t->pc_offset) {
            g_pc_pending = false; /* written here: nothing older may follow */
        }
        t2_str_imm(use_reg(in->a, T2_R0), T2_CPU, t->reg_offset(in->imm));
        break;

    case EMU_IR_CONST: {
        if (in->dst < g_ntemps && in->dst < EMU_IR_MAX_TEMPS && g_kcmp[in->dst] != 0u) {
            break; /* its compare takes it as an immediate */
        }
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_imm32(rd, in->imm);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_MOV: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (rd != ra) {
            t2_mov(rd, ra);
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg_fwd(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_ADD:
            t2_add(rd, ra, rb);
            break;
        case EMU_IR_SUB:
            t2_sub(rd, ra, rb);
            break;
        case EMU_IR_AND:
            t2_and(rd, ra, rb);
            break;
        case EMU_IR_OR:
            t2_orr(rd, ra, rb);
            break;
        default:
            t2_eor(rd, ra, rb);
            break;
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg_fwd(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        /*
         * ARM shifts use the low *byte* of the count and saturate past
         * 31, where both guests define only the low five bits and expect
         * a shift of 32 to be a shift of 0. Masking is therefore not
         * optional.
         *
         * The mask lands in r2 rather than back in `rb`: with allocation
         * `rb` may be the count's own register, and narrowing it in
         * place would corrupt a value later instructions still read.
         */
        t2_and_imm(T2_R2, rb, 31u); /* 31 encodes as itself */
        t2_shift_reg((in->op == (uint8_t)EMU_IR_SHL)   ? T2_LSL
                     : (in->op == (uint8_t)EMU_IR_SHR) ? T2_LSR
                                                       : T2_ASR,
                     rd, ra, T2_R2);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * The immediate ALU forms, which pass_fuse produces from a constant
     * feeding a register operation.
     *
     * Three encodings and a fallback, because ARM's are not one thing:
     * ADDW/SUBW take a plain 0..4095 and reach every small displacement;
     * the T3 forms take a *modified* immediate, which reaches 0x00FF00FF
     * and not 4000; and a constant in neither is materialised and used
     * from a register -- which is exactly what the un-fused code did, so
     * the worst case is no worse than not folding.
     *
     * That fallback is why can_lower may answer yes without being told
     * the value. Answering per-value is not possible through that
     * interface, and promising something the emitter cannot do discards
     * the whole block.
     */
    case EMU_IR_ADDI:
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
    case EMU_IR_XORI: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);
        const uint32_t v = in->imm;
        uint16_t i12;

        if (in->op == (uint8_t)EMU_IR_ADDI && v <= 0xFFFu) {
            t2_addw(rd, ra, (uint16_t)v);
        } else if (in->op == (uint8_t)EMU_IR_ADDI && 0u - v <= 0xFFFu) {
            /* A negative addend is a subtraction, and SUBW reaches the
             * same 4096 downwards that ADDW reaches upwards. Negated as
             * unsigned: -(int32_t)v is undefined for 0x80000000. */
            t2_subw(rd, ra, (uint16_t)(0u - v));
        } else if (t2_expand_imm(v, &i12)) {
            switch ((emu_ir_op_t)in->op) {
            case EMU_IR_ADDI:
                t2_add_imm(rd, ra, i12);
                break;
            case EMU_IR_ANDI:
                t2_and_imm(rd, ra, i12);
                break;
            case EMU_IR_ORI:
                t2_orr_imm(rd, ra, i12);
                break;
            default:
                t2_eor_imm(rd, ra, i12);
                break;
            }
        } else if (in->op == (uint8_t)EMU_IR_ANDI && t2_expand_imm(~v, &i12)) {
            /* 0xFFFFFFF0, a flags word's mask: BIC #15, not MOVW+MOVT+AND */
            t2_bic_imm(rd, ra, i12);
        } else if (in->op == (uint8_t)EMU_IR_ORI && t2_expand_imm(~v, &i12)) {
            t2_orn_imm(rd, ra, i12);
        } else if (in->op == (uint8_t)EMU_IR_ADDI && t2_expand_imm(0u - v, &i12)) {
            t2_sub_imm(rd, ra, i12);
        } else {
            /* No immediate form: materialise and use the register one. */
            const uint32_t rt = (rd == T2_R1) ? T2_R2 : T2_R1;

            t2_imm32(rt, v);
            switch ((emu_ir_op_t)in->op) {
            case EMU_IR_ADDI:
                t2_add(rd, ra, rt);
                break;
            case EMU_IR_ANDI:
                t2_and(rd, ra, rt);
                break;
            case EMU_IR_ORI:
                t2_orr(rd, ra, rt);
                break;
            default:
                t2_eor(rd, ra, rt);
                break;
            }
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SETF:
        emit_setf(in, t);
        break;

    case EMU_IR_GETCOND:
        emit_getcond(in, t);
        break;

    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_shift_imm((in->op == (uint8_t)EMU_IR_SHLI)   ? T2_LSL
                     : (in->op == (uint8_t)EMU_IR_SHRI) ? T2_LSR
                                                        : T2_ASR,
                     rd, ra, in->imm);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * Rotate left by `imm`, as a rotate right by 32 - imm.
     *
     * **A rotation by zero must not become ROR #32.** ARM has no such
     * encoding: imm5 == 0 in a ROR is RRX, a 33-bit rotate through the
     * carry, which is a different instruction and not a rotation at all.
     * `(32 - imm) & 31` keeps the identity an identity. t2_shift_imm
     * rewrites a zero-amount ROR to a move for exactly this reason, so
     * the mask is belt and braces -- but the arithmetic has to be right
     * before it gets there.
     */
    case EMU_IR_ROTLI: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_shift_imm(T2_ROR, rd, ra, (32u - (in->imm & 31u)) & 31u);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_NOT: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mvn(rd, ra);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_NEG: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_neg(rd, ra);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * The bit and byte group, one host instruction each -- which is the
     * whole reason these are IR operations rather than shift-and-mask
     * sequences the backend would have to pattern-match back.
     */
    case EMU_IR_BSWAP32:
    case EMU_IR_BSWAP16:
    case EMU_IR_HSWAP:
    case EMU_IR_CLZ:
    case EMU_IR_CTZ:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_BSWAP32:
            t2_rev(rd, ra);
            break;
        case EMU_IR_BSWAP16:
            t2_rev16(rd, ra);
            break;
        case EMU_IR_HSWAP:
            t2_shift_imm(T2_ROR, rd, ra, 16u);
            break;
        case EMU_IR_CLZ:
            t2_clz(rd, ra);
            break;
        /*
         * RBIT then CLZ. Both are defined for a zero input -- CLZ of
         * zero is 32, which is what the IR specifies -- so unlike the
         * x86 lowering this needs no fixup for the case a bit search is
         * most often handed.
         */
        case EMU_IR_CTZ:
            t2_rbit(rd, ra);
            t2_clz(rd, rd);
            break;
        case EMU_IR_SEXT8:
            t2_sxtb(rd, ra);
            break;
        case EMU_IR_SEXT16:
            t2_sxth(rd, ra);
            break;
        case EMU_IR_ZEXT8:
            t2_uxtb(rd, ra);
            break;
        default:
            t2_uxth(rd, ra);
            break;
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SETCC: {
        /*
         * Compare and materialise a 0 or 1 without branching, using the
         * IT block ARM provides for exactly this.
         *
         * MOVS on a low register writes N and Z, so the false value has
         * to be set up *before* the compare -- doing it between the CMP
         * and the IT destroys the comparison, which is a mistake this
         * project has made and recorded.
         */
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            t2_imm32(rd, 1u);
            st_slot(rd, in->dst);
            break;
        }

        const uint32_t ra = use_reg(in->a, T2_R1);

        t2_imm32(rd, 0u); /* MOVS sets flags: before the compare */
        g_r0_first = false; /* rd may be r0: nothing to forward now */
        emit_cmp_operand(ra, in->b, T2_R2);
        t2_emit16(
            (uint16_t)(0xBF00u | (t2_cond(in->aux) << 4) | 0x8u)); /* IT */
        t2_imm32(rd, 1u);
        st_slot(rd, in->dst);
        break;
    }

#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE
    case EMU_IR_KERNEL: {
        const int ki = emit_dot_inline(in->imm);

        if (ki == KI_FAIL) {
            return false;
        }
        if (ki == KI_CALL) {
            /* As on x86-64: the call ahead of the body, the count added. */
            t2_mov(T2_R0, T2_CPU);
            t2_imm32(T2_R1, in->imm);
            t2_mov(T2_R2, T2_CNT); /* the count so far: the cap bounds the kernel too */
            t2_call((const void *)emu_ir_kernel_run);
            t2_add(T2_CNT, T2_CNT, T2_R0);
        }
        g_r0_holds = EMU_IR_NO_TEMP;
        break;
    }
#endif

    case EMU_IR_CMP3: {
        /*
         * 2, the compare, then IT LT (or LO) MOV 8 and IT GT (or HI) MOV
         * 4: the first move, in an IT block, leaves the flags for the
         * second. As SETCC, the value before the compare, since a MOVS
         * outside an IT writes N and Z.
         */
        const bool u = in->aux == (uint8_t)EMU_IR_C_LTU;
        const uint32_t rd = def_reg(in->dst, T2_R0);
        const uint32_t ra = use_reg(in->a, T2_R1);

        t2_imm32(rd, 2u);
        g_r0_first = false; /* rd may be r0: nothing to forward now */
        emit_cmp_operand(ra, in->b, T2_R2);
        t2_emit16((uint16_t)(0xBF00u |
                             (t2_cond(u ? EMU_IR_C_LTU : EMU_IR_C_LT) << 4) | 0x8u));
        t2_imm32(rd, 8u);
        t2_emit16((uint16_t)(0xBF00u |
                             (t2_cond(u ? EMU_IR_C_GTU : EMU_IR_C_GT) << 4) | 0x8u));
        t2_imm32(rd, 4u);
        st_slot(rd, in->dst);
        break;
    }

    /* ---- floating point ----------------------------------------- */
    case EMU_IR_FGET: {
        if (t->freg_offset == NULL) {
            return false;
        }
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_ldr_imm(rd, T2_CPU, t->freg_offset(in->imm));
        if ((in->aux & EMU_IR_FP_BOX) != 0u) {
            /*
             * An unboxed register is not a single-precision value and
             * reads as the canonical NaN.
             *
             * The constant is materialised *before* the IT and moved
             * inside it, which is not a stylistic choice: t2_imm32 emits
             * MOVW and then MOVT for anything with a high half, so
             * putting it in the block would leave the MOVT outside a
             * one-instruction IT -- unconditional, and executed for
             * every boxed value. That is this file's own recorded trap,
             * an encoder whose wrong answer is another valid
             * instruction. MOV.W is one, and r2/r3 are scratch here
             * whatever the allocator did with rd, which is r0 or r6-r11.
             *
             * MOVW/MOVT do not write the flags, so the CMP survives to
             * the IT that consumes it -- the other trap in this file.
             */
            t2_ldr_imm(T2_R3, T2_CPU, t->freg_offset(in->imm) + 4u);
            t2_imm32(T2_R2, 0xFFFFFFFFu);
            t2_cmp(T2_R3, T2_R2);
            t2_imm32(T2_R2, 0x7FC00000u);
            t2_emit16(0xBF18u); /* IT NE */
            t2_mov(rd, T2_R2);
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_FPUT:
        if (t->freg_offset == NULL) {
            return false;
        }
        t2_str_imm(use_reg(in->a, T2_R0), T2_CPU, t->freg_offset(in->imm));
        if ((in->aux & EMU_IR_FP_BOX) != 0u) {
            t2_imm32(T2_R1, 0xFFFFFFFFu);
            t2_str_imm(T2_R1, T2_CPU, t->freg_offset(in->imm) + 4u);
        }
        break;

    case EMU_IR_FSGNJ: {
        /*
         * Integer work, so it never reaches the FP unit -- which is also
         * why it is exact for a NaN operand, where anything routed
         * through a VFP register risks the default-NaN rule quietening
         * one.
         */
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_imm32(T2_R2, 0x80000000u);
        if (in->aux == EMU_IR_FSGNJ_X) {
            t2_eor(T2_R3, ra, rb);
            t2_and(T2_R3, T2_R3, T2_R2);
        } else {
            t2_and(T2_R3, rb, T2_R2);
            if (in->aux == EMU_IR_FSGNJ_N) {
                t2_eor(T2_R3, T2_R3, T2_R2);
            }
        }
        t2_imm32(T2_R2, 0x7FFFFFFFu);
        t2_and(T2_R2, ra, T2_R2);
        t2_orr(rd, T2_R2, T2_R3);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_FADD:
    case EMU_IR_FSUB:
    case EMU_IR_FMUL:
    case EMU_IR_FDIV: {
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) {
            return false;
        }
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);
        const bool sub = in->op == (uint8_t)EMU_IR_FSUB;
        const uint16_t hi = (in->op == (uint8_t)EMU_IR_FMUL)   ? T2_VMUL
                            : (in->op == (uint8_t)EMU_IR_FDIV) ? T2_VDIV
                                                               : T2_VADD;

        const bool fix = g_uf_vfop != NULL && (in->op == (uint8_t)EMU_IR_FMUL ||
                                                in->op == (uint8_t)EMU_IR_FDIV);

        t2_vmov_core(T2_S0, ra, false);
        t2_vmov_core(T2_S1, rb, false);
        if (fix) { /* UF as the guest raises it: g_uf_vfop */
            t2_vfp3(hi, sub, T2_S3, T2_S0, T2_S1);
            t2_vmov_core(T2_S3, rd, true);
            emit_uf_check((in->op == (uint8_t)EMU_IR_FMUL) ? EMU_IR_VF_MUL : EMU_IR_VF_DIV,
                          rd);
        } else {
            t2_vfp3(hi, sub, T2_S0, T2_S0, T2_S1);
            t2_vmov_core(T2_S0, rd, true);
        }
        st_slot(rd, in->dst);
        break;
    }

    /*
     * dst = +/-(a * b) +/- c, rounded once.
     *
     * ARM's fused instructions accumulate *into the destination*, so the
     * addend goes into S0 first and the product's sign becomes the `sub`
     * bit rather than a separate negate:
     *
     *   FMADD    a*b + c   VFMA  S0, S1, S2   sub=false, c in S0
     *   FMSUB    a*b - c   VFMA  with c negated -- VFNMS, see below
     *   FNMSUB  -a*b + c   VFMS  S0, S1, S2   sub=true,  c in S0
     *   FNMADD  -a*b - c   VFNMS S0, S1, S2
     *
     * The mapping is not one instruction per RISC-V opcode, because
     * RISC-V negates the *product* in its "n" forms and ARM's VFNM*
     * negate the *accumulator*. Working it through:
     *
     *   NEG_MUL clear, NEG_ADD clear ->  c + a*b   VFMA   0xEEA0 sub=0
     *   NEG_MUL set,   NEG_ADD clear ->  c - a*b   VFMS   0xEEA0 sub=1
     *   NEG_MUL clear, NEG_ADD set   -> -c + a*b   VFNMS  0xEE90 sub=0
     *   NEG_MUL set,   NEG_ADD set   -> -c - a*b   VFNMA  0xEE90 sub=1
     *
     * so NEG_ADD picks the encoding pair and NEG_MUL *is* the sub bit,
     * uniformly. The first version had sub inverted for the NEG_ADD
     * case, on the reasoning that VFNMA/VFNMS are "named backwards" --
     * they are, but the sub bit is not, and fptest's mixed kernel came
     * back 0x00000000 against the host's 0x49370308.
     */
    case EMU_IR_FMA: {
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) {
            return false;
        }

        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rc = use_reg(in->c, T2_R2);
        const uint32_t rd = def_reg(in->dst, T2_R0);
        const bool neg_mul = (in->aux & EMU_IR_FMA_NEG_MUL) != 0u;
        const bool neg_add = (in->aux & EMU_IR_FMA_NEG_ADD) != 0u;
        const uint16_t hi = neg_add ? T2_VFNMS_VFNMA : T2_VFMA_VFMS;
        const bool sub = neg_mul;

        if (g_uf_vfop != NULL) {
            /* a, b and c kept in s0-s2 for the UF fix-up, the addend
             * accumulated in s3 */
            t2_vmov_core(T2_S0, ra, false);
            t2_vmov_core(T2_S1, rb, false);
            t2_vmov_core(T2_S2, rc, false);
            t2_vmov_core(T2_S3, rc, false);
            t2_vfp3(hi, sub, T2_S3, T2_S0, T2_S1);
            t2_vmov_core(T2_S3, rd, true);
            emit_uf_check(neg_mul ? (neg_add ? EMU_IR_VF_NMACC : EMU_IR_VF_NMSAC)
                                  : (neg_add ? EMU_IR_VF_MSAC : EMU_IR_VF_MACC),
                          rd);
            st_slot(rd, in->dst);
            break;
        }
        /* The addend is the accumulator, so it goes to the destination
         * register the instruction reads and writes. */
        t2_vmov_core(T2_S0, rc, false);
        t2_vmov_core(T2_S1, ra, false);
        t2_vmov_core(T2_S2, rb, false);
        t2_vfp3(hi, sub, T2_S0, T2_S1, T2_S2);
        t2_vmov_core(T2_S0, rd, true);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_FSQRT: {
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) {
            return false;
        }
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_vmov_core(T2_S0, ra, false);
        t2_vsqrt(T2_S0, T2_S0);
        t2_vmov_core(T2_S0, rd, true);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SETPC:
    case EMU_IR_EXIT: {
        uint32_t rp = T2_R0;

        if (in->op == (uint8_t)EMU_IR_SETPC && in->a == EMU_IR_NO_TEMP) {
            g_pc_pending = true; /* stored where something reads it */
            g_pc_value = in->imm;
            break;
        }
        if (in->op == (uint8_t)EMU_IR_EXIT && in->a == EMU_IR_NO_TEMP &&
            is_loop_back(in->imm)) {
            (void)loop_back(in->imm); /* stores the pc on its way out */
            g_pc_pending = false;
            break;
        }
        if (in->a != EMU_IR_NO_TEMP) {
            rp = use_reg(in->a, T2_R0);
        } else {
            t2_imm32(T2_R0, in->imm);
        }
        t2_str_imm(rp, T2_CPU, t->pc_offset);
        g_pc_pending = false;
        /*
         * A plain jump through a register goes to the stub, which finds
         * most of them a block to jump to (emu_jit_ibtc): the target in
         * r0, the frame given back, `LDR r12, =t2_ibtc_stub ; BX r12`.
         */
        if (in->op == (uint8_t)EMU_IR_EXIT && !emu_ir_exit_is_const(in) &&
            (in->aux & EMU_IR_EXIT_JUMP) != 0u && emu_jit_layout.chainable &&
            g_ibtc_ok) {
            if (rp != T2_R0) {
                t2_mov(T2_R0, rp);
            }
            emit_frame_release(g_frame);
            emit_pooled(T2_R12, (uint32_t)(uintptr_t)t2_ibtc_stub);
            t2_emit16((uint16_t)(0x4700u | (T2_R12 << 3))); /* BX r12 */
            g_r0_holds = EMU_IR_NO_TEMP;
            break;
        }
        if (in->op == (uint8_t)EMU_IR_EXIT) {
            /*
             * A computed target cannot be matched against the block's
             * start at translation time, so only the constant form
             * chains.
             */
            if (!emu_ir_exit_is_const(in) ||
                (!loop_back(in->imm) && !chain_exit(in->imm))) {
                note_exit(t2_b_forward(), false);
            }
        }
        break;
    }

#if EMU_IR_VECTOR
    case EMU_IR_VOP:
    case EMU_IR_VOPM:
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux) ||
            t->vreg_offset == NULL) {
            return false;
        }
        if (in->op == (uint8_t)EMU_IR_VOP && have_mve() && vfile_mve(t)) {
            const uint32_t vop = EMU_IR_VOP_OP(in->aux);
            const uint32_t vsew = EMU_IR_VOP_SEW(in->aux);

            if (EMU_IR_V_IS_WIDE(vop) && vsew <= 1u) {
                lower_vop_wide_mve(in, t);
                break;
            }
            if (EMU_IR_V_IS_NARROW(vop) &&
                vop_narrow_mve_rm(vop, vsew, EMU_IR_VOP_RM(in->imm))) {
                lower_vop_narrow_mve(in, t);
                break;
            }
            if (vop_mve(vop, vsew, EMU_IR_VOP_RM(in->imm))) {
                lower_vop_mve(in, t);
                break;
            }
        }
        if (!vop_lowerable(in->aux) && in->op == (uint8_t)EMU_IR_VOP) {
            return false; /* MVE's alone, and MVE cannot here */
        }
        lower_vop(in, t);
        break;

    case EMU_IR_VMEM:
        if (t->vreg_offset == NULL || t->helpers == NULL ||
            in->aux >= t->helper_count) {
            return false;
        }
        lower_vmem(in, t);
        break;

    case EMU_IR_VRED:
        if (!emu_ir_can_lower(EMU_IR_VRED, in->aux) || t->vreg_offset == NULL) {
            return false;
        }
        if (have_mve() && vred_mve(EMU_IR_VRED_OP(in->aux)) && vfile_mve(t)) {
            lower_vred_mve(in, t);
            break;
        }
        lower_vred(in, t);
        break;

    case EMU_IR_VMEMS:
        if (t->vreg_offset == NULL || t->helpers == NULL ||
            in->aux >= t->helper_count) {
            return false;
        }
        lower_vmems(in, t);
        break;

    case EMU_IR_VFOP: /* the loop addresses the file from v0, by offset */
        if (!emu_ir_can_lower(EMU_IR_VFOP, in->aux) || t->vreg_offset == NULL ||
            t->vreg_offset(31u) != t->vreg_offset(0u) + 31u * EMU_IR_VREG_BYTES) {
            return false;
        }
        lower_vfop(in, t);
        break;
#endif

    case EMU_IR_EXIT_IF: {
        const uint32_t ra = use_reg(in->a, T2_R0);

        emit_cmp_operand(ra, in->b, T2_R1);
        /* Branch *over* the exit on the inverse condition. */
        uint8_t *const skip = t2_bcond_forward(t2_cond(in->aux) ^ 1u);
        if (!loop_back(in->imm)) { /* which stores the pc only to leave */
            t2_imm32(T2_R0, in->imm);
            t2_str_imm(T2_R0, T2_CPU, t->pc_offset);
            if (!chain_exit(in->imm)) {
                note_exit(t2_b_forward(), false);
            }
        }
        patch_branch(skip, emu_jit_here(), true);
        break;
    }

    case EMU_IR_MUL: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg_fwd(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mul(rd, ra, rb);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * dst = c +/- (a * b), in one instruction.
     *
     *   MLA  rd, rn, rm, ra    rd = ra + rn * rm
     *   MLS  rd, rn, rm, ra    rd = ra - rn * rm
     *
     * Both are four-register, and MLS is the asymmetric one -- the
     * accumulator is subtracted *from*, which is why pass_fuse only
     * produces the sub form when the product is the SUB's second
     * operand.
     */
    case EMU_IR_MAC: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rc = use_reg(in->c, T2_R2);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mla(rd, ra, rb, rc, (in->aux & EMU_IR_MAC_SUB) != 0u);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_MULHS:
    case EMU_IR_MULHU: {
        /*
         * SMULL/UMULL write both halves and their two destinations must
         * differ, so the low half lands in a register that is then
         * discarded. Only the high half is wanted.
         *
         * r1 takes the discard because the high half is either an
         * allocated register -- r6 and above -- or r0, so the two can
         * never name the same register whatever the allocator did.
         */
        const uint32_t ra = use_reg_fwd(in->a, T2_R2);
        const uint32_t rb = use_reg_fwd(in->b, T2_R3);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mull(in->op == (uint8_t)EMU_IR_MULHS, T2_R1, rd, ra, rb);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_DIVS:
    case EMU_IR_DIVU:
    case EMU_IR_REMS:
    case EMU_IR_REMU: {
        /*
         * SDIV/UDIV, and a remainder is the divide plus MLS -- there is
         * no remainder instruction on ARM.
         *
         * Nothing checks the divisor. EMU_IR_DIV* is defined as a host
         * divide with the divisor guaranteed non-zero, because ARM's
         * answer for zero (0) and x86's (a fault) are both wrong for
         * RISC-V and the frontend already normalises. Adding a test here
         * would be a second implementation of the same rule, and a
         * slower one.
         */
        const bool sgn =
            (in->op == (uint8_t)EMU_IR_DIVS || in->op == (uint8_t)EMU_IR_REMS);
        const bool rem =
            (in->op == (uint8_t)EMU_IR_REMS || in->op == (uint8_t)EMU_IR_REMU);
        const uint32_t ra = use_reg_fwd(in->a, T2_R2);
        const uint32_t rb = use_reg_fwd(in->b, T2_R3);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (rem) {
            /*
             * The quotient goes to r1 and is then consumed by the MLS,
             * so it needs no allocated register of its own -- and r1
             * cannot collide with rd, which is r0 or an allocated one.
             */
            if (sgn) {
                t2_sdiv(T2_R1, ra, rb);
            } else {
                t2_udiv(T2_R1, ra, rb);
            }
            t2_mls(rd, T2_R1, rb, ra); /* rd = a - (a / b) * b */
        } else if (sgn) {
            t2_sdiv(rd, ra, rb);
        } else {
            t2_udiv(rd, ra, rb);
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_LOAD: {
        if (t->load == NULL || (in->aux & T2_MEM_STORE) != 0u) {
            return false;
        }
        /*
         * Every path leaves the value in r0 when the destination has no
         * register -- the inlined ones load it there and the stub
         * returns it there -- so the dead-store rule holds here as it
         * does for any other operation: a load read once, by the next
         * instruction, never reaches its slot. Not under a predicate,
         * whose skip leaves r0 holding something else.
         */
        const int pd = phys(in->dst);
        if (in->c != EMU_IR_NO_TEMP) {
            g_dead_store = EMU_IR_NO_TEMP;
        }
        const uint32_t ra = use_reg_fwd(in->a, T2_R1);

        /*
         * The address has to end up in r1, and `ra` may be an allocated
         * register a later instruction still reads -- so the sum is
         * built into r1 rather than back into `ra`.
         */
        emit_addr(T2_R1, ra, in->imm);
        uint8_t *const skip = emit_pred_skip(in);
        /*
         * The inlined path: about 40% of a guest's instructions are a
         * memory access, and each was a call into the region walk.
         */
        uint8_t *ldone = NULL;
        uint8_t *ldone_pt = NULL;
        if (g_has_fast) {
            const uint32_t sz = EMU_IR_MEM_SIZE(in->aux);
            const bool sign = (in->aux & EMU_IR_MEM_SIGNED) != 0u;
            uint8_t *slow[1];
            const uint32_t k = emit_fast_guard(T2_R1, sz, slow);
            const uint32_t rt = (pd >= 0) ? (uint32_t)pd : T2_R0;

            if (t2_ld_reg_lsl(rt, T2_RAMB, T2_R2, k, sz, sign)) {
                if (pd < 0) {
                    st_slot(T2_R0, in->dst);
                }
                ldone = t2_b_forward_n();
            }
            patch_branch_n(slow[0], emu_jit_here(), true);

            /*
             * Not guest RAM: the peripheral window, if the block is armed
             * for it. The same guard and the same access, with the base
             * as the host address, because the window is an identity
             * map. Every span in it is readable, so a load tests no holes.
             */
            if (g_has_pt && sz <= 4u) {
                uint8_t *pslow[1];
                const uint32_t kp = emit_window_guard(g_pt.base, g_pt.size,
                                                      T2_R1, sz, pslow);

                emit_pt_base();
                if (t2_ld_reg_lsl(rt, T2_R12, T2_R2, kp, sz, sign)) {
                    if (pd < 0) {
                        st_slot(T2_R0, in->dst);
                    }
                    ldone_pt = t2_b_forward_n();
                }
                patch_branch_n(pslow[0], emu_jit_here(), true);
            }
        } else if (g_has_dtlb) {
            const uint32_t sz = EMU_IR_MEM_SIZE(in->aux);
            const bool sign = (in->aux & EMU_IR_MEM_SIGNED) != 0u;
            uint8_t *slow[3];
            const uint32_t n = emit_dtlb_probe(T2_R1, sz, false, slow);
            const uint32_t rt = (pd >= 0) ? (uint32_t)pd : T2_R0;

            if (t2_ld_reg(rt, T2_R12, T2_R1, sz, sign)) {
                if (pd < 0) {
                    st_slot(T2_R0, in->dst);
                }
                ldone = t2_b_forward_n();
            }
            for (uint32_t i = 0; i < n; i++) {
                patch_branch_n(slow[i], emu_jit_here(), true);
            }
        }

        /*
         * The slow path: the shared stub, which returns the value in r0
         * -- and which serves a read of the image itself when the block
         * is armed for one. Not in line, as on x86-64: an inline test is
         * 26 bytes on every load, and that pushed ARMv7-M CoreMark out of
         * the 96 KB cache (34.65 host cycles an instruction to 49.19)
         * where in the stub it is no bytes at all.
         */
        emit_mem_stub((uint32_t)in->aux | (g_has_rom ? T2_MEM_ROM_OK : 0u));
        st_slot(T2_R0, in->dst);
        patch_branch_n(ldone_pt, emu_jit_here(), false);
        patch_branch_n(ldone, emu_jit_here(), false);
        /*
         * The paths merge holding the value in r0 when it has no
         * register; with one, the inlined paths never touched r0.
         */
        g_r0_holds = (pd < 0) ? in->dst : EMU_IR_NO_TEMP;
        end_pred_skip(skip);
        break;
    }

    case EMU_IR_STORE: {
        if (t->store == NULL || (in->aux & T2_MEM_STORE) != 0u) {
            return false;
        }
        const uint32_t ra = use_reg_fwd(in->a, T2_R1);

        emit_addr(T2_R1, ra, in->imm); /* r1, and r2 past 4 KB: not r0 */

        const uint32_t rv = use_reg_fwd(in->b, T2_R3);

        if (rv != T2_R3) {
            t2_mov(T2_R3, rv);
        }
        uint8_t *const skip = emit_pred_skip(in);

        uint8_t *sdone = NULL;
        uint8_t *sdone_pt = NULL;
        if (g_has_fast) {
            const uint32_t sz = EMU_IR_MEM_SIZE(in->aux);
            uint8_t *resv = NULL;
            uint8_t *slow[1];

            /*
             * A store to the reserved word must break an LR/SC
             * reservation, which an inlined one would not -- so decline
             * while one is outstanding rather than replicating the
             * address compare. That is only between an LR and its SC.
             * First, so it covers both windows.
             */
            if (g_fast.store_guard_offset != EMU_IR_NO_GUARD) {
                /*
                 * r0 is free until the slow path loads the cpu into it.
                 * A byte, as emu_ir_fastmem_t says the guard is: this was
                 * a word load, which read the three bytes after the flag
                 * as well and so could send every store to the slow path
                 * on what happened to follow it in the struct.
                 */
                t2_ldrb_imm(T2_R0, T2_CPU, g_fast.store_guard_offset);
                (void)t2_cmp_imm8(T2_R0, 0u);
                resv = t2_bcond_forward_n(0x1u); /* NE */
            }

            const uint32_t k = emit_fast_guard(T2_R1, sz, slow);

            if (t2_st_reg_lsl(T2_R3, T2_RAMB, T2_R2, k, sz)) {
                sdone = t2_b_forward_n();
            }
            patch_branch_n(slow[0], emu_jit_here(), true);

            /*
             * The peripheral window: as for a load, and then each
             * read-only hole, because a store into one has to fault --
             * those spans are what stops a guest reconfiguring the PLL or
             * erasing the flash the emulator runs from.
             */
            if (g_has_pt && sz <= 4u) {
                uint8_t *pslow[1u + EMU_IR_PT_HOLES];
                const uint32_t kp = emit_window_guard(g_pt.base, g_pt.size,
                                                      T2_R1, sz, pslow);
                const uint32_t np = 1u + emit_pt_holes(kp, &pslow[1]);

                emit_pt_base();
                if (t2_st_reg_lsl(T2_R3, T2_R12, T2_R2, kp, sz)) {
                    sdone_pt = t2_b_forward_n();
                }
                for (uint32_t i = 0; i < np; i++) {
                    patch_branch_n(pslow[i], emu_jit_here(), true);
                }
            }
            patch_branch_n(resv, emu_jit_here(), true);
        } else if (g_has_dtlb) {
            const uint32_t sz = EMU_IR_MEM_SIZE(in->aux);
            uint8_t *slow[3];
            const uint32_t n = emit_dtlb_probe(T2_R1, sz, true, slow);

            if (t2_st_reg(T2_R3, T2_R12, T2_R1, sz)) {
                sdone = t2_b_forward_n();
            }
            for (uint32_t i = 0; i < n; i++) {
                patch_branch_n(slow[i], emu_jit_here(), true);
            }
        }

        /* The slow path: the shared stub, value already in r3. */
        emit_mem_stub((uint32_t)in->aux | T2_MEM_STORE);
        patch_branch_n(sdone_pt, emu_jit_here(), false);
        if (sdone != NULL) {
            patch_branch_n(sdone, emu_jit_here(), false);
            /* The paths merge; r0 is not the same on both. */
            g_r0_holds = EMU_IR_NO_TEMP;
        }
        end_pred_skip(skip);
        break;
    }

    /*
     * Not lowered yet. Each is real work rather than an oversight:
     * SETF and GETCOND need the guest's flag word rebuilt from APSR,
     * SELECT needs it read back, the memory bit ops need the
     * load-modify-store sequence, and the multiplies need UMULL/SMULL
     * with their register pairs. Returning false discards the block,
     * which the framework treats exactly as a declined translation --
     * so this is a coverage cost, not a correctness one.
     */
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP: {
        /*
         * The escape hatch, and the thing that makes emu_ir_can_lower
         * mean anything here. Without it every operation this backend
         * honestly declines cost the whole block instead of a call, and
         * the capability query was inert -- which is exactly how it
         * shipped, and presented as a board run whose counters were
         * identical to the previous one to the digit.
         *
         * AAPCS: r0 the cpu, r1 and r2 the arguments, r3 `aux` when it
         * is non-zero (emu_ir.h). r4-r11 are callee-saved, so allocated
         * temps survive with nothing emitted around them, which is the
         * whole reason the allocator was given those registers.
         */
        if (t->helpers == NULL || in->imm >= t->helper_count) {
            return false;
        }
        if (g_has_fp) {
            emit_fp_harvest(t, true);
        }

        const uint32_t ra = use_reg(in->a, T2_R1);

        if (ra != T2_R1) {
            t2_mov(T2_R1, ra);
        }

        const uint32_t rb = use_reg(in->b, T2_R2);

        if (rb != T2_R2) {
            t2_mov(T2_R2, rb);
        }
        if (in->aux != 0u) {
            /* MOVS: its N and Z die at the call, which clobbers both. */
            (void)t2_movs_imm8(T2_R3, in->aux);
        }
        pc_store(T2_R0, true); /* a helper may read it */
        t2_mov(T2_R0, T2_CPU);
        t2_call(t->helpers[in->imm]);

        if (in->op == (uint8_t)EMU_IR_HELPER_TRAP) {
            /*
             * Non-zero means it entered a trap: pc is already in the
             * handler and the rest of the block must not run.
             */
            (void)t2_cmp_imm8(T2_R0, 0u);
            note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
        }
        if (in->dst != EMU_IR_NO_TEMP) {
            st_slot(T2_R0, in->dst);
        }
        break;
    }

    case EMU_IR_FMIN:
    case EMU_IR_FMAX:
    case EMU_IR_FCMP:
    case EMU_IR_FCVT_TO_I:
    case EMU_IR_FCVT_FROM_I:
    case EMU_IR_FCLASS:
    case EMU_IR_SELECT:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    case EMU_IR_POPCNT:
    case EMU_IR_CLMUL: /* no carry-less multiply on an M7 */
    case EMU_IR_AESSB: /* nor AES */
    case EMU_IR_SM4SB: /* nor SM4 */
    case EMU_IR_BEXT:
    case EMU_IR_BSET:
    case EMU_IR_BCLR:
    case EMU_IR_BINV:
    default:
        return false;
    }
    return true;
}

bool emu_ir_lower(const emu_ir_block_t *b, const emu_ir_target_t *t)
{
    if (b->overflow) {
        return false;
    }

    g_ntemps = (uint16_t)b->next_temp;
    g_r0_holds = EMU_IR_NO_TEMP;
    g_dead_store = EMU_IR_NO_TEMP;
    g_nexits = 0u;
    g_exits_lost = false;
    g_has_fast = b->has_fast;
    g_has_dtlb = !b->has_fast && b->has_dtlb;
    g_dtlb = b->dtlb;
    g_fast = b->fast;
    g_has_pt = b->has_fast && b->has_pt;
    g_pt = b->pt;
    g_has_rom = b->has_fast && b->has_rom;
    g_rom = b->rom;
    g_block_start = emu_jit_here();
    g_nexits_nf = 0u;
    /*
     * The links are this function's output: a caller that lowers block
     * after block without the framework's reset (test_ir, fuzz-ir) must
     * not have the last block's sites patched into this one.
     */
    emu_jit_layout.nlink = 0u;
    emu_jit_layout.unlinked = 0u;
    g_body = NULL;
    g_start_pc = b->start_pc;
    g_has_start_pc = true;
    g_pc_off = t->pc_offset;
    g_pc_pending = false;
    g_uf_vfop = (t->fp_flags != NULL) ? t->vfop : NULL;
    g_uf_fp_flags = t->fp_flags;
    pool_reset();

    /*
     * **A uniform save set, so a chained block's epilogue matches
     * whichever block's prologue actually ran.** A chain enters the
     * successor past the push, so the pop at the end undoes a push made
     * by a *different* block, and a variable list would restore the
     * wrong registers and return to the wrong place.
     *
     * Set before the frame is sized, because the frame's alignment pad
     * is chosen from this count: sizing the pad from the allocator's
     * number and then pushing a different one leaves sp misaligned at a
     * helper call, which AAPCS requires and nothing here would fault on
     * until something used LDRD or a double.
     */
    (void)emu_ir_regalloc(b, T2_ALLOC_REGS, g_reg);
    g_nsaved = T2_ALLOC_REGS;
    kcmp_scan(b);

    /*
     * Asked once. Everything FPSCR costs -- saving it, setting the rules
     * this backend's answers depend on, reading the flags back and
     * handing them to the frontend -- is paid per block, and must not be
     * paid by blocks that never look at a float.
     */
    g_has_fp = false;
    /*
     * Found here, before the prologue decides whether the block may
     * chain -- which it may not, since every exit must pass through the
     * harvest in the tail.
     *
     * An FPUT alone does not count: it is a register write, and needs
     * neither FPSCR nor the tail. It used to, when the tail's fp_flags
     * call was how RV32 marked FS Dirty, and so blocks that only moved
     * floats could not chain (fuzz-rv32-jit, on c.flw then jal: one
     * that did skipped the call). RV32 marks FS in the block itself now,
     * at its first f write (`fput` in rv_ir.c).
     */
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->insn[i].dead) {
            continue;
        }
        if ((b->insn[i].op >= (uint8_t)EMU_IR_FADD &&
             b->insn[i].op <= (uint8_t)EMU_IR_FCLASS) ||
            b->insn[i].op == (uint8_t)EMU_IR_VFOP) {
            g_has_fp = true;
        }
    }
    if (g_has_fp && (t->fp_flags == NULL || t->freg_offset == NULL)) {
        return false;
    }

    uint32_t list = (1u << T2_CPU) | (1u << T2_CNT) | (1u << T2_RAMB);

    for (uint32_t i = 0; i < g_nsaved; i++) {
        list |= 1u << t2_alloc_regs[i];
    }

    /*
     * The computed jumps' stub pops r4-r11 and pc, whatever block it
     * returns for; a block that pushed anything else must not go there.
     * Its empty entries are aimed at its return once, here, before the
     * first block that can use it exists.
     */
    {
        g_ibtc_ok = list == 0x0FF0u && ((uintptr_t)t2_ibtc_stub & 1u) != 0u;
        if (g_ibtc_ok && !g_ibtc_armed) {
            g_ibtc_armed = true;
            emu_jit_ibtc_set_miss((const void *)t2_ibtc_miss);
        }
    }

    /* What r11 will hold, and what the stub it names will call. */
    emu_jit_block_arg = (const void *)t2_mem_stub;
    t2_mem_accessor[0] = (const void *)t->load;
    t2_mem_accessor[1] = (const void *)t->store;
    if (g_has_rom) {
        t2_rom_window[0] = g_rom.base;
        t2_rom_window[1] = g_rom.size;
        t2_rom_window[2] = (uint32_t)(uintptr_t)g_rom.host;
    }

    /*
     * The frame, padded so that sp is 8-byte aligned at a helper call.
     *
     * AAPCS requires that at a public interface and a helper is one.
     * What decides it is the *total* below the caller's sp, which is the
     * saved registers plus the frame -- and three saved registers is 12
     * bytes, so before there was anything to allocate this was already
     * four out. That was benign here only because nothing a helper
     * reaches uses LDRD or a double.
     */
    uint32_t frame = (((b->next_temp + 4u) * 4u) + 7u) & ~7u;

    /* Pushed: the allocator's, the cpu, the count, the stub, and lr. */
    if ((((g_nsaved + 4u) * 4u) + frame) % 8u != 0u) {
        frame += 4u;
    }

    t2_push(list | T2_LIST_LR);
    t2_mov(T2_CPU, T2_R0);
    /* MOVS: the flags are dead on entry to a block. */
    if (!t2_movs_imm8(T2_CNT, 0u)) {
        t2_imm32(T2_CNT, 0u);
    }
    g_frame = frame;
    /*
     * Where a predecessor branches in: past the push, past the cpu
     * pointer it already holds, and past the counter zeroing -- a chain
     * accumulates one retired count across all its blocks -- at this
     * block's own frame setup.
     *
     * Blocks that touch the FP unit are not chainable: every exit of
     * theirs must pass through the harvest in the tail, and a chained
     * one would skip it.
     */
    emu_jit_layout.chain_entry = (uint32_t)(emu_jit_here() - g_block_start);
    emu_jit_layout.chainable = !g_has_fp;
    if (frame != 0u && !t2_sp_adjust(true, frame)) {
        /* SUB.W sp, sp, r12 -- past the 16-bit form's 508 bytes */
        t2_imm32(T2_R12, frame);
        t2_emit32(0xEBADu, (uint16_t)((13u << 8) | T2_R12));
    }
    /*
     * Past the chain entry, so a predecessor -- which may not have the
     * window, or not this one -- cannot leave r11 wrong; before the
     * body, so a back edge does not reload it.
     */
    if (g_has_fast) {
        bool used = false;

        for (uint32_t i = 0; i < b->count && !used; i++) {
            used = !b->insn[i].dead && (b->insn[i].op == (uint8_t)EMU_IR_LOAD ||
                                        b->insn[i].op == (uint8_t)EMU_IR_STORE);
        }
        if (used) {
            emit_pooled(T2_RAMB, (uint32_t)(uintptr_t)g_fast.host);
        }
    }

    if (g_has_fp) {
        /*
         * Borrow FPSCR for the length of the block.
         *
         * Three things are set, and each is a rule the guests state and
         * ARM does not default to:
         *
         *   DN  set. ARM's default NaN is what RISC-V calls the
         *       canonical NaN, and without it a NaN operand propagates
         *       instead of being replaced.
         *   FZ  clear. Both guests define subnormals as ordinary values;
         *       flush-to-zero would quietly turn the smallest results
         *       into zero and raise the wrong flag doing it.
         *   RMode nearest. emu_ir_can_lower promises this backend emits
         *       arithmetic only for round-to-nearest, and that is a
         *       promise only if the block sets it rather than inheriting
         *       whatever ran before.
         *
         * The sticky flags are cleared for the same reason the x86-64
         * backend clears MXCSR's: a guest that clears its own and then
         * runs an operation raising none would otherwise still see the
         * host's leftovers, and nothing else ever clears them.
         */
        t2_vmrs(T2_R0);
        st_slot(T2_R0, SCRATCH_FPSCR);
        t2_imm32(T2_R1, 0x02000000u); /* DN */
        t2_orr(T2_R0, T2_R0, T2_R1);
        t2_imm32(T2_R1, ~0x01C0001Fu); /* FZ, RMode, IOC..IXC */
        t2_and(T2_R0, T2_R0, T2_R1);
        t2_vmsr(T2_R0);
    }

    /*
     * The body starts here: after the frame and any mode setup, before
     * the first guest instruction. A back edge branches to this rather
     * than to the block's entry, because the frame is already pushed and
     * re-running the prologue would push it again.
     */
    /*
     * A loop kernel goes ahead of the body: run once an entry, not at
     * every back edge (EMU_IR_KERNEL). The guest pc is the block's
     * start, already in the cpu, and the kernel does not read it.
     */
    uint32_t first = 0u;

    while (first < b->count && b->insn[first].op == (uint8_t)EMU_IR_KERNEL) {
        g_r0_holds = EMU_IR_NO_TEMP;
        if (!lower_one(&b->insn[first], t)) {
            return false;
        }
        first++;
    }
    g_body = emu_jit_here();
    g_pc_pending = true; /* this block's own, already in the cpu */
    g_pc_value = g_start_pc;

    for (uint32_t i = first; i < b->count; i++) {
        if (b->insn[i].dead) {
            continue;
        }
        g_dead_store = EMU_IR_NO_TEMP;
        if (b->insn[i].dst != EMU_IR_NO_TEMP && b->insn[i].uses == 1u) {
            uint32_t j = i + 1u;

            while (j < b->count && b->insn[j].dead) {
                j++;
            }
            if (j < b->count && b->insn[j].a == b->insn[i].dst &&
                reads_a_in_r0(b->insn[j].op)) {
                g_dead_store = b->insn[i].dst;
            } else if (j < b->count && takes_from_r0(&b->insn[j], b->insn[i].dst)) {
                g_dead_store = b->insn[i].dst; /* its reader takes r0 */
            }
        }

        pool_check_reach();
#if EMU_JIT_BYTE_STATS
        uint8_t *const at = emu_jit_here();
#endif
        if (!lower_one(&b->insn[i], t)) {
            emu_ir_note_refused(b->insn[i].op, b->insn[i].aux);
            return false;
        }
#if EMU_JIT_BYTE_STATS
        emu_ir_bytes_op(b->insn[i].op, (uint32_t)(emu_jit_here() - at));
#endif
        if (emu_jit_overflowed()) {
            return false;
        }
    }

    /*
     * Every early exit lands here, before the frame is given back, so
     * all paths undo the same adjustment. Branching straight to the
     * return instead would leave sp low and pop the wrong words.
     */
    for (uint32_t i = 0; i < g_nexits; i++) {
        patch_branch(g_exits[i].at, emu_jit_here(), g_exits[i].conditional);
    }

    if (g_has_fp) {
        emit_fp_harvest(t, false);
        ld_slot(T2_R0, SCRATCH_FPSCR);
        t2_vmsr(T2_R0);
    }

    emit_frame_release(frame);
    /*
     * Past the teardown: where an exit that has already undone its own
     * frame rejoins, and what the framework aims a chained branch back
     * at when it unlinks one.
     */
    emu_jit_layout.tail = (uint32_t)(emu_jit_here() - g_block_start);
    for (uint32_t i = 0; i < g_nexits_nf; i++) {
        patch_branch(g_exits_nf[i].at, emu_jit_here(),
                     g_exits_nf[i].conditional);
    }
    t2_mov(T2_R0, T2_CNT);
    t2_pop(list | T2_LIST_PC);

    /*
     * Where an exit not yet linked goes: the same return, with
     * EMU_JIT_RET_UNLINKED in the count, so that the dispatcher looks for
     * an exit to link after this return and no other.
     */
    if (emu_jit_layout.nlink != 0u) {
        uint8_t *const tail = g_block_start + emu_jit_layout.tail;
        uint8_t *const unlinked = emu_jit_here();
        uint16_t flag;

        (void)t2_expand_imm(EMU_JIT_RET_UNLINKED, &flag);
        t2_orr_imm(T2_CNT, T2_CNT, flag);
        patch_branch_n(t2_b_forward_n(), tail, false); /* two bytes back */
        emu_jit_layout.unlinked = (uint16_t)(unlinked - tail);
        for (uint32_t k = 0; k < emu_jit_layout.nlink; k++) {
            patch_branch(g_block_start + emu_jit_layout.link_site[k],
                         unlinked, false);
        }
    }
    pool_emit();

    return !emu_jit_overflowed() && !g_exits_lost;
}

#endif /* EMU_HOST_JIT_THUMB2 */
