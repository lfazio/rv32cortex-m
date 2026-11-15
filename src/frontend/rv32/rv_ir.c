/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_ir.c - RV32 to IR.
 *
 * The frontend half of frontend -> IR -> optimisation -> backend for
 * RISC-V. It names no host.
 *
 * RISC-V has no condition flags, and that shows up here as an absence:
 * this file never emits EMU_IR_SETF, so the shared dead-flag pass finds
 * nothing to do and costs nothing. `SLT` and `SLTU` become EMU_IR_SETCC,
 * which compares two values directly rather than setting flags and
 * reading them back -- inventing a flag word this architecture does not
 * have would defeat the pass for the one frontend that should pay
 * nothing for it.
 *
 * What is *not* here, and why
 * ---------------------------
 * Loads, stores and the FP groups go through EMU_IR_HELPER_TRAP to the
 * same helpers the direct translator used, rather than being open-coded.
 * That is the choice this project has already measured twice: inlining a
 * memory access on the ARM backend cost it the LR/SC reservation, then
 * PMP, then address translation, each found on hardware rather than by a
 * suite. Everything rv_hart_load and rv_hart_store do beyond the access
 * itself is had for free by calling them, and on a host that predicts
 * calls well the cost is small.
 *
 * SYSTEM, the atomics and anything else end the block and land on the
 * interpreter, which is where the state they touch can be observed.
 */

#include "emu/emu_jit.h" /* EMU_JIT_HAS_ROM_WINDOW */
#include "emu/emu_memmap.h"

#include "rv32/rv_hart.h"
#include "rv32/rv_bitmanip.h"
#include "rv32/rv_crypto.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_ir.h"
#include "rv32/rv_jit.h"
#include "rv32/rv_vec.h"

#include <stddef.h>

/*
 * Guest instructions folded into one block. Interrupts are delivered
 * between blocks, so this bounds interrupt latency as much as code size.
 */
#define RV_IR_MAX_BLOCK_INSNS 64u

/*
 * Code cache on a host. Generous where the framework's microcontroller
 * default is not: on a target those bytes are the guest's, but here
 * constant retranslation is what would mask a translator bug behind a
 * fresh translation -- at 12 KB CoreMark flushed nineteen times a run.
 */
#if defined(EMU_HOST_JIT_THUMB2)
/* On a target these bytes are the guest's; see CLAUDE.md. */
#define RV_JIT_HOST_CODE_BYTES RV_JIT_CODE_SIZE
#else
#define RV_JIT_HOST_CODE_BYTES EMU_HOST_JIT_CODE_BYTES
#endif

extern const emu_backend_t rv_backend_interp;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Every trap a helper enters goes through here, and is counted: the
 * interpreter charges a trap one unit of the budget, and the dispatcher
 * charges a block what it retired plus this count's change
 * (emu_jit_hot_t::charged). Without it a block that trapped partway was
 * charged one less, and a guest stepping over 100 faulting loads
 * retired 1,211 interpreted and 1,113 under the JIT. Interrupts are not
 * counted here: neither backend charges their entry.
 */
static void ir_trap(rv_hart_t *h, uint32_t cause, uint32_t tval)
{
    h->ir_traps++;
    rv_hart_trap(h, cause, tval);
}

/*
 * The generic accessors the shared lowering calls.
 *
 * The guest pc already points at the faulting instruction when either is
 * reached -- the frontend emits EMU_IR_SETPC ahead of every memory
 * operation -- so rv_hart_trap records the right address.
 */
EMU_HOT_TEXT static uint32_t rv_ir_load(emu_cpu_t *cpu, uint32_t addr,
                                        uint32_t spec, uint32_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const rv_exc_t exc = rv_hart_load(h, addr, EMU_IR_MEM_SIZE(spec),
                                      (spec & EMU_IR_MEM_SIGNED) != 0u, out);

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        ir_trap(h, exc, addr);
        return 1u;
    }
#if EMU_JIT_HAS_ROM_WINDOW
    /* A read of the image a block could not inline: see rom_armed. */
    if (EMU_UNLIKELY(!h->rom_armed) && emu_bus_in_rodata(h->bus, addr)) {
        h->rom_reads++;
    }
#endif
    return 0u;
}

EMU_HOT_TEXT static uint32_t rv_ir_store(emu_cpu_t *cpu, uint32_t addr,
                                         uint32_t spec, uint32_t val)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const rv_exc_t exc = rv_hart_store(h, addr, EMU_IR_MEM_SIZE(spec), val);

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        ir_trap(h, exc, addr);
        return 1u;
    }
    return 0u;
}

/*
 * The window a backend may access without calling the helpers above.
 *
 * **This says no far more often than it says yes, and every refusal is
 * a case where an access means more than a move.** Paging, PMP and
 * execute triggers all have to run per access; a misaligned access may
 * have to be split; a device write has side effects. The one case left
 * is an ordinary read-write RAM region on a little-endian guest with
 * none of that armed, which is what every bare-metal guest in this tree
 * spends its time in.
 *
 * A block that inlines this is specialised on the answer. That is safe
 * because the two things which can change it -- the PMP configuration
 * and paging -- are already in the generation key, and privilege is in
 * the context, so such a block is thrown away or never entered when
 * either moves. Triggers are separate: the JIT does not run at all
 * while one is armed.
 *
 * The RAM region is found through the *bus*, not from the platform's
 * board_ram: src/frontend may not depend on src/platform, and the bus
 * is the one description of the machine the hart already has.
 */
static bool rv_ir_fast_mem(emu_cpu_t *cpu, emu_ir_fastmem_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

#if RV_EXT_SV32
    if (h->vm_active) {
        return false;
    }
#endif
#if RV_EXT_PMP
    if (h->pmp_active) {
        return false;
    }
#endif
#if RV_EXT_SDTRIG
    if (h->trig_active) {
        return false;
    }
#endif
    if (h->bus == NULL) {
        return false;
    }

    emu_region_t *const r = emu_bus_find(h->bus, EMU_GUEST_RAM_BASE);

    if (r == NULL || r->kind != (uint8_t)EMU_MEM_RAM || r->host == NULL ||
        r->size == 0u ||
        (r->perm & (EMU_PERM_R | EMU_PERM_W)) != (EMU_PERM_R | EMU_PERM_W)) {
        return false;
    }

    out->base = r->base;
    out->size = r->size;
    out->host = r->host;
    /*
     * A store to the reserved word must break an LR/SC reservation, and
     * an inlined one would not. Rather than replicate the address
     * compare, the fast path is simply declined while a reservation is
     * outstanding -- which is only ever between an LR and its SC.
     */
#if RV_EXT_A
    out->store_guard_offset = (uint32_t)offsetof(rv_hart_t, resv_valid);
#else
    out->store_guard_offset = EMU_IR_NO_GUARD;
#endif
    return true;
}

/*
 * The data TLB a block probes instead, when there is no window: one per
 * privilege below M, chosen by the privilege the block's loads and stores
 * are made at -- which rv_jit_ctx_key carries, so a block is never
 * entered at a privilege whose table it does not name. Everything else
 * the table depends on, the frontend invalidates in the table itself
 * (rv_dtlb.c): a block probes it afresh at every access and bakes in
 * nothing but where it is.
 *
 * M-mode gets none. Its accesses are the window's when nothing is armed,
 * and when PMP has a locked entry they are rare enough -- isatest, not an
 * operating system -- to leave to the helper.
 */
static bool rv_ir_dtlb(emu_cpu_t *cpu, emu_ir_dtlb_t *out)
{
#if RV_HAVE_DTLB
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const uint32_t priv = rv_hart_data_priv(h);

    if (priv == RV_PRIV_M || h->bus == NULL) {
        return false;
    }
#if RV_EXT_SDTRIG
    if (h->trig_active) {
        return false;
    }
#endif
    out->table_offset = (uint32_t)(offsetof(rv_hart_t, dtlb) +
                                   rv_dtlb_table(priv) * sizeof(h->dtlb[0]));
    out->index_mask = RV_DTLB_ENTRIES - 1u;
#if RV_EXT_A
    out->store_guard_offset = (uint32_t)offsetof(rv_hart_t, resv_valid);
#else
    out->store_guard_offset = EMU_IR_NO_GUARD;
#endif
    return true;
#else
    (void)cpu;
    (void)out;
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* The target descriptor                                               */
/* ------------------------------------------------------------------ */

/*
 * mstatus, as the IR's register 32: x0-x31 are 0-31, and the IR names up
 * to 64. Read and written by fput alone, to make FS Dirty where an f
 * register is written.
 */
#define RV_IR_MSTATUS 32u
#if RV_EXT_ZVE32X
/* The vector unit's state an inline vsetvli writes and its operations read. */
#define RV_IR_VL 33u
#define RV_IR_VTYPE 34u
#define RV_IR_VSTART 35u
#endif

static uint32_t rv_reg_offset(uint32_t n)
{
    if (n == RV_IR_MSTATUS) {
        return (uint32_t)offsetof(rv_hart_t, mstatus);
    }
#if RV_EXT_ZVE32X
    if (n == RV_IR_VL) {
        return (uint32_t)offsetof(rv_hart_t, vl);
    }
    if (n == RV_IR_VTYPE) {
        return (uint32_t)offsetof(rv_hart_t, vtype);
    }
    if (n == RV_IR_VSTART) {
        return (uint32_t)offsetof(rv_hart_t, vstart);
    }
#endif
    return (uint32_t)offsetof(rv_hart_t, x) + n * 4u;
}

/* x0 reads as zero and discards writes. */
static bool rv_reg_zero(uint32_t n)
{
    return n == 0u;
}

#if RV_EXT_ZVE32X
_Static_assert(RV_VLENB == EMU_IR_VREG_BYTES,
               "EMU_IR_VOP's registers are VLEN 128's");

static uint32_t rv_vreg_offset(uint32_t n)
{
    return (uint32_t)offsetof(rv_hart_t, v) + n * RV_VLENB;
}
#endif

#if RV_EXT_F
static uint32_t rv_freg_offset(uint32_t n)
{
    /*
     * sizeof rather than 4, because FLEN follows D: the file is 32 bits
     * wide without it and 64 with. It was written as 4 and stayed that
     * way when D widened the file, which was invisible only because
     * nothing emitted an FGET or an FPUT from that moment on -- every FP
     * instruction went to the helper. A stride that is right by not
     * being used is not right.
     */
    return (uint32_t)(offsetof(rv_hart_t, f) +
                      n * sizeof(((rv_hart_t *)0)->f[0]));
}

/*
 * The box, on the FGET and FPUT of a *single-precision* value.
 *
 * Zero without D, where FLEN is 32 and there is no upper half to fill.
 * Not every FP move wants it even with D -- FSW and FMV.X.W move bits
 * and take the low half raw -- so it is applied per instruction rather
 * than folded into rv_freg_offset. See rv_fpu.c, which makes the same
 * distinction in the same four places through fr32/fw32.
 */
#if RV_EXT_D
#define RV_IR_BOX EMU_IR_FP_BOX
#else
#define RV_IR_BOX 0u
#endif

/*
 * One block's worth of floating-point flags, on the one path every exit
 * takes.
 *
 * The IR hands over its own neutral flag encoding, which is deliberately
 * RISC-V's fflags order -- so the accumulate is a mask and an OR, and
 * the two backends that had to reorder theirs did it once each rather
 * than the frontend doing it per host.
 *
 * FS goes Dirty here only if fflags changed -- the interpreter's rule
 * (f_accrue), and Sail's. An f register written marks it where it is
 * written (fput), so a block that only moves floats calls nothing here
 * and may chain. A context switch reads FS to decide whether the
 * register file needs saving; Dirty is *not* part of the generation
 * key -- the key tracks off-ness -- which is what stops either write
 * from flushing the code cache every time a block uses a float.
 *
 * This used to mark FS Dirty for every block that held any FP
 * operation, on every exit: a block that left by a side exit before
 * its flw still made FS Dirty, which the spec permits ("may report
 * dirty even when not modified") and a program clearing one FS bit
 * then saw as Clean where the interpreter gave Off -- found by
 * fuzz-rv32-jit, rv32-jit/side-exit-fs-dirty.
 */
static void rv_ir_fp_flags(emu_cpu_t *cpu, uint32_t flags)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const uint32_t f = flags & 0x1Fu;

    if ((h->fcsr | f) != h->fcsr) {
        h->fcsr |= f;
        h->mstatus |= MSTATUS_FS_MASK;
    }
}

/*
 * The escape hatch, for everything a backend declined: FMIN/FMAX, the
 * classifications, the conversions a host has no encoding for, the fused
 * multiply-adds, and any rounding mode this host cannot spell.
 *
 * It is the same rv_hart_fp the interpreter uses, so nothing about
 * rounding, NaN handling or the flag rules exists twice -- which is what
 * makes routing to it cheaper than getting a second implementation
 * subtly wrong. Non-zero means it entered a trap and the block must
 * stop; the pc it records was written by the SETPC the caller emits.
 */
static uint32_t rv_ir_fp_helper(emu_cpu_t *cpu, uint32_t insn, uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t tval = 0u;
    const rv_exc_t e = rv_hart_fp(h, insn, &tval);

    (void)unused;
    if (e == RV_EXC_NONE) {
        return 0u;
    }
    ir_trap(h, e, tval);
    return 1u;
}

#endif /* RV_EXT_F */

#if RV_EXT_A
/*
 * Atomics, through the same rv_hart_amo the interpreter calls.
 *
 * **This exists so a block does not end at every lock.** The A extension
 * was declined outright, and CLAUDE.md has the rule: what you decline
 * costs more than what you translate badly, because ending the block
 * fragments hot code. An atomic is not rare in the guests that matter --
 * a Linux kernel takes one on every lock, every refcount and every
 * percpu update -- so declining opcode 0x2F cut a block at each of them.
 *
 * The whole of it is rv_hart_atomic, which is also the whole of the
 * interpreter's AMO case: the same width and validity tests, the same
 * causes. This used to be a copy of that case "deliberately line for
 * line" -- a second implementation held to the first by intention,
 * which is the mistake the FP helper beside it was written to avoid.
 *
 * Reading h->x[] here is sound because the optimiser treats a helper as
 * touching everything: pass_reg_traffic drops every forwarded register at
 * the call, and the dead-PUT pass keeps the stores before it. A helper
 * that quietly read a stale register would be the sharpest edge in this
 * file, so it is stated rather than assumed.
 */
static uint32_t rv_ir_amo_helper(emu_cpu_t *cpu, uint32_t insn, uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t tval;

    (void)unused;

    const rv_exc_t exc = rv_hart_atomic(h, insn, &tval);

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        ir_trap(h, exc, tval);
        return 1u;
    }
    return 0u;
}
#endif /* RV_EXT_A */

/*
 * Divide and remainder, natively, with RISC-V's answers built around a
 * host divide that must never see the awkward operands.
 *
 * **The arithmetic was never the problem; the two cases around it are.**
 * RISC-V *defines* divide by zero and signed overflow as producing
 * values -- x86 raises #DE for both and ARM's SDIV returns zero -- so a
 * host divide is right for every other input and wrong, differently per
 * host, for exactly two. This is the same shape as the FP work: the
 * operation matches and what surrounds it does not.
 *
 * The divisor is substituted rather than branched around, so the emitted
 * code is straight-line and the host divide never faults:
 *
 *      z   = (b == 0)
 *      ov  = (a == INT_MIN) & (b == -1)      -- signed only
 *      sel = z | ov
 *      b'  = sel ? 1 : b
 *
 * With b' = 1 the divide is exact and cheap, and every answer falls out
 * of a mask rather than a second branch:
 *
 *      DIVU  q = a / b'        result = q | -z          (b==0 -> ~0)
 *      DIVS  q = a / b'        result = q | -z
 *            ...and the overflow case needs nothing: a / 1 is a, which
 *            is INT_MIN, which is the answer the architecture gives.
 *      REMU  r = a % b'        result = z ? a : r
 *      REMS  r = a % b'        result = z ? a : r
 *            ...and overflow again needs nothing: a % 1 is 0.
 *
 * Both overflow cases being free is not luck, it is why substituting 1
 * was chosen over substituting anything else.
 */
static bool rv_ir_lower_divrem(emu_ir_block_t *b, uint32_t insn, uint32_t f3,
                               uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    const bool sgn = (f3 == 4u || f3 == 6u);
    const bool rem = (f3 >= 6u);
    const uint16_t x = emu_ir_get(b, rs1);
    const uint16_t y = emu_ir_get(b, rs2);
    const uint16_t zero = emu_ir_const(b, 0u);

    (void)insn;

    /* z = (b == 0), as 0 or 1. */
    const uint16_t z =
        emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_EQ, y, zero, 0u, 0u);
    uint16_t sel = z;

    if (sgn) {
        /*
         * The overflow pair, and it has to be both halves: INT_MIN by
         * anything else is ordinary, and -1 into anything else is too.
         */
        const uint16_t is_min =
            emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_EQ, x,
                        emu_ir_const(b, 0x80000000u), 0u, 0u);
        const uint16_t is_m1 =
            emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_EQ, y,
                        emu_ir_const(b, 0xFFFFFFFFu), 0u, 0u);
        const uint16_t ov = emu_ir_alu(b, EMU_IR_AND, is_min, is_m1);

        sel = emu_ir_alu(b, EMU_IR_OR, z, ov);
    }

    /*
     * b' = sel ? 1 : b, as (b & ~mask) | sel with mask = -sel. One more
     * instruction than a conditional move and no branch at all, which
     * matters more: this sits in the middle of whatever loop the guest
     * put the divide in.
     */
    const uint16_t mask = emu_ir_alu(b, EMU_IR_SUB, zero, sel);
    const uint16_t keep = emu_ir_alu(
        b, EMU_IR_AND, y,
        emu_ir_alu(b, EMU_IR_XOR, mask, emu_ir_const(b, 0xFFFFFFFFu)));
    const uint16_t safe = emu_ir_alu(b, EMU_IR_OR, keep, sel);

    const emu_ir_op_t op = rem ? (sgn ? EMU_IR_REMS : EMU_IR_REMU)
                               : (sgn ? EMU_IR_DIVS : EMU_IR_DIVU);
    const uint16_t q = emu_ir_alu(b, op, x, safe);

    const uint16_t zmask = emu_ir_alu(b, EMU_IR_SUB, zero, z);

    if (rem) {
        /* result = z ? a : r */
        const uint16_t a_part = emu_ir_alu(b, EMU_IR_AND, x, zmask);
        const uint16_t r_part = emu_ir_alu(
            b, EMU_IR_AND, q,
            emu_ir_alu(b, EMU_IR_XOR, zmask, emu_ir_const(b, 0xFFFFFFFFu)));

        emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_OR, a_part, r_part));
    } else {
        /* result = q | -z, which is all-ones exactly when b was zero */
        emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_OR, q, zmask));
    }
    return true;
}

/*
 * Divide, remainder and MULHSU.
 *
 * EMU_IR_HELPER rather than HELPER_TRAP: RISC-V *defines* divide by zero
 * and signed overflow as producing values, so none of these can take a
 * trap and the caller needs no exit check.
 *
 * **Declining these cost far more than the divide does.** A Linux boot
 * executes DIV 356,462,035 times -- 99.4% of everything still
 * interpreted once `time` was lowered -- and each one ended a block, so
 * the loop containing it paid a dispatch and an interpreter entry every
 * iteration.
 *
 * The arithmetic is rv_hart.h's, shared with the interpreter rather than
 * written again here.
 */
static uint32_t rv_ir_muldiv_helper(emu_cpu_t *cpu, uint32_t insn,
                                    uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const uint32_t a = h->x[rv_rs1(insn)];
    const uint32_t d = h->x[rv_rs2(insn)];
    uint32_t v;

    (void)unused;
    switch (rv_funct3(insn)) {
    case 2u: /* MULHSU: the one multiply with no single host instruction */
        v = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)d) >> 32);
        break;
    case 4u:
        v = rv_div_s((int32_t)a, (int32_t)d);
        break;
    case 5u:
        v = rv_div_u(a, d);
        break;
    case 6u:
        v = rv_rem_s((int32_t)a, (int32_t)d);
        break;
    default:
        v = rv_rem_u(a, d);
        break;
    }
    if (rv_rd(insn) != 0u) {
        h->x[rv_rd(insn)] = v;
    }
    return 0u;
}

/*
 * The bit-manipulation instructions that are not worth IR of their own.
 *
 * Which ones is a measurement, not a judgement about difficulty. Counted
 * per instruction with -DEMU_PAIR_STATS=ON across every guest here, the
 * B extension is the three shifted adds, `sext.h`, `zext.h`, `andn`,
 * `rori`, `rev8` and `bexti` -- all of which lower_one emits natively,
 * in one or two operations each -- and then nothing: `minu` eight times
 * in the crypto guest, `bext` three times in CoreMark. What comes here
 * is everything else, plus the two things no amount of frequency would
 * move:
 *
 *   clmul, clmulh, clmulr   a 32-step loop; neither host has the
 *                           instruction (x86-64 would need PCLMULQDQ
 *                           and a feature test, a Cortex-M has nothing)
 *   orc.b                   four compares
 *   cpop                    declined by both backends as EMU_IR_POPCNT
 *   min, max, minu, maxu    five IR operations each without a select,
 *                           and EMU_IR_SELECT reads flags RISC-V lacks
 *   rol, ror                four, EMU_IR_ROTL being declined by both
 *
 * CoreMark does execute clmul -- 2,352 times, its CRC -- and that is the
 * case for the helper rather than against it: declined, each one ended
 * its block and took an interpreter entry.
 *
 * EMU_IR_HELPER, not HELPER_TRAP: none of these can fault.
 *
 * **The decode below is the interpreter's, a second time, for the
 * subset sent here** -- lower_one decides what that subset is, so an
 * instruction it does not route cannot arrive. The arithmetic is
 * rv_bitmanip.h's, which the interpreter shares.
 */
#if RV_EXT_ZBB || RV_EXT_ZBC
static uint32_t rv_ir_bitop_helper(emu_cpu_t *cpu, uint32_t insn,
                                   uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const uint32_t a = h->x[rv_rs1(insn)];
    const uint32_t s = h->x[rv_rs2(insn)];
    const uint32_t f3 = rv_funct3(insn);
    uint32_t v = 0u;

    (void)unused;
    (void)s;
#if RV_EXT_ZBB
    if ((insn & 0x7Fu) == 0x13u) {
        /* OP-IMM: cpop shares slli's funct3, orc.b shares srli's. */
        v = (f3 == 1u) ? (uint32_t)__builtin_popcount(a) : zbb_orcb(a);
    } else if (rv_funct7(insn) == 0x30u) {
        v = (f3 == 1u) ? zbb_rol(a, s) : zbb_ror(a, s);
    } else
#endif
    {
        switch (f3) {
#if RV_EXT_ZBC
        case 1u:
            v = zbc_clmul(a, s);
            break;
        case 2u:
            v = zbc_clmulr(a, s);
            break;
        case 3u:
            v = zbc_clmulh(a, s);
            break;
#endif
#if RV_EXT_ZBB
        case 4u:
            v = ((int32_t)a < (int32_t)s) ? a : s;
            break;
        case 5u:
            v = (a < s) ? a : s;
            break;
        case 6u:
            v = ((int32_t)a > (int32_t)s) ? a : s;
            break;
        case 7u:
            v = (a > s) ? a : s;
            break;
#endif
        default:
            break;
        }
    }
    if (rv_rd(insn) != 0u) {
        h->x[rv_rd(insn)] = v;
    }
    return 0u;
}
#endif

/*
 * Read one counter CSR.
 *
 * HELPER_TRAP because the read itself can fault: `time` is readable from
 * S- and U-mode only while the corresponding counteren bit is set, and
 * rv_csr_read is where that rule lives. Doing the check here rather than
 * at translation is not an optimisation -- counteren can change under a
 * block, and a translate-time check is only half a guard.
 */
static uint32_t rv_ir_csr_helper(emu_cpu_t *cpu, uint32_t insn, uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t v = 0u;
    const rv_exc_t exc = rv_csr_read(h, insn >> 20, &v);

    (void)unused;
    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        ir_trap(h, exc, insn);
        return 1u;
    }
    if (rv_rd(insn) != 0u) {
        h->x[rv_rd(insn)] = v;
    }
    return 0u;
}

/*
 * The helper table.
 *
 * **The indices are fixed whatever the configuration**, because an id is
 * baked into emitted code and a table that shrinks with a build option
 * would silently call the wrong function. A slot whose extension is
 * absent points at a helper that traps illegal-instruction, which cannot
 * be reached -- the translator declines those instructions -- and is the
 * honest thing to put there if it ever were.
 */
/* Wanted by every slot that can be absent: F, A, the bit operations,
 * the scalar crypto and the vector unit. */
#if !(RV_EXT_F && RV_EXT_A && (RV_EXT_ZBB || RV_EXT_ZBC) && RV_EXT_ZK && \
      RV_EXT_ZVE32X)
static uint32_t rv_ir_helper_absent(emu_cpu_t *cpu, uint32_t insn,
                                    uint32_t unused)
{
    (void)unused;
    ir_trap((rv_hart_t *)cpu, RV_EXC_ILLEGAL_INSN, insn);
    return 1u;
}
#endif

#if RV_EXT_F
#define RV_IR_H_FP ((const void *)rv_ir_fp_helper)
#else
#define RV_IR_H_FP ((const void *)rv_ir_helper_absent)
#endif
#if RV_EXT_A
#define RV_IR_H_AMO ((const void *)rv_ir_amo_helper)
#else
#define RV_IR_H_AMO ((const void *)rv_ir_helper_absent)
#endif

#if RV_EXT_ZBB || RV_EXT_ZBC
#define RV_IR_H_BITOP ((const void *)rv_ir_bitop_helper)
#else
#define RV_IR_H_BITOP ((const void *)rv_ir_helper_absent)
#endif

/*
 * The scalar crypto, every instruction of it: rv_crypto_exec is the
 * interpreter's code too, and an S-box read that touches all 256 entries
 * (Zkt) is no instruction a lowering could offer. None can fault.
 */
#if RV_EXT_ZK
static uint32_t rv_ir_crypto_helper(emu_cpu_t *cpu, uint32_t insn,
                                    uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t v = 0u;

    (void)unused;
    (void)rv_crypto_exec(insn, h->x[rv_rs1(insn)], h->x[rv_rs2(insn)], &v);
    if (rv_rd(insn) != 0u) {
        h->x[rv_rd(insn)] = v;
    }
    return 0u;
}
#define RV_IR_H_CRYPTO ((const void *)rv_ir_crypto_helper)
#else
#define RV_IR_H_CRYPTO ((const void *)rv_ir_helper_absent)
#endif

#if RV_EXT_ZVE32X
/*
 * Every vector instruction -- OP-V, and the loads and stores that share
 * LOAD-FP and STORE-FP -- through the same rv_hart_vec the interpreter
 * calls: one implementation of the vector unit, held to Spike by the
 * probes. They were declined, so each ended its block and a vector loop
 * was a block per instruction; a helper call is a translation.
 *
 * Nothing a block is specialised on is vector state: vtype, vl and
 * vstart are read here, at run time, by rv_hart_vec. Non-zero means it
 * entered a trap -- an illegal encoding, VS off, an element that
 * faulted -- and the block must stop; the pc it records is the SETPC's.
 */
static uint32_t rv_ir_vec_helper(emu_cpu_t *cpu, uint32_t insn,
                                 uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t tval = 0u;
    const rv_exc_t e = rv_hart_vec(h, insn, &tval);

    (void)unused;
    if (e == RV_EXC_NONE) {
        return 0u;
    }
    ir_trap(h, e, tval);
    return 1u;
}

/*
 * EMU_IR_VMEMS's helper, given its imm: the strided access it stands for
 * rebuilt -- vlse<eew>.v or vsse<eew>.v, unmasked, one field, with the
 * vd, rs1 and rs2 rv_ir_vmem put in imm -- and run as any other, so a
 * fault is rv_hart_vec's and tval the same bits.
 */
static uint32_t rv_ir_vmems_helper(emu_cpu_t *cpu, uint32_t imm, uint32_t addr,
                                   uint32_t vl)
{
    const uint32_t eew = EMU_IR_VMEM_EEW(imm);
    const uint32_t arg = EMU_IR_VMEMS_ARG(imm);
    const uint32_t insn = (EMU_IR_VMEM_STORE(imm) ? 0x27u : 0x07u) |
                          (EMU_IR_VMEM_VD(imm) << 7) |
                          (((eew == 0u) ? 0u : eew + 4u) << 12) |
                          ((arg & 31u) << 15) | (((arg >> 5) & 31u) << 20) |
                          (1u << 25) | (2u << 26);

    (void)addr;
    (void)vl;
    return rv_ir_vec_helper(cpu, insn, 0u);
}
#define RV_IR_H_VEC ((const void *)rv_ir_vec_helper)
#define RV_IR_H_VMEMS ((const void *)rv_ir_vmems_helper)
#else
#define RV_IR_H_VEC ((const void *)rv_ir_helper_absent)
#define RV_IR_H_VMEMS ((const void *)rv_ir_helper_absent)
#endif

static const void *const rv_ir_helpers[] = {
    RV_IR_H_FP,     RV_IR_H_AMO,   (const void *)rv_ir_csr_helper,
    (const void *)rv_ir_muldiv_helper, RV_IR_H_BITOP, RV_IR_H_CRYPTO,
    RV_IR_H_VEC,    RV_IR_H_VMEMS};
#define RV_IR_HELPER_FP 0u
#define RV_IR_HELPER_AMO 1u
#define RV_IR_HELPER_CSR 2u
#define RV_IR_HELPER_MULDIV 3u
#define RV_IR_HELPER_BITOP 4u
#define RV_IR_HELPER_CRYPTO 5u
#define RV_IR_HELPER_VEC 6u
#define RV_IR_HELPER_VMEMS 7u

/* One instruction, handed whole to rv_ir_bitop_helper. */
static bool rv_ir_bitop_call(emu_ir_block_t *b, uint32_t insn)
{
    (void)emu_ir_emit(b, EMU_IR_HELPER, 0u, emu_ir_const(b, insn),
                      EMU_IR_NO_TEMP, RV_IR_HELPER_BITOP, 0u);
    return true;
}

/* A one-operand IR operation on `x`. */
static uint16_t rv_ir_unary(emu_ir_block_t *b, emu_ir_op_t op, uint16_t x)
{
    return emu_ir_emit(b, op, 0u, x, EMU_IR_NO_TEMP, 0u, 0u);
}

const emu_ir_target_t rv_ir_target = {
    .reg_offset = rv_reg_offset,
#if RV_EXT_ZVE32X
    .vreg_offset = rv_vreg_offset,
    .vsat_offset = (uint32_t)offsetof(rv_hart_t, vxsat),
#endif
    /*
     * No condition flags at all. flags_offset is unused while every
     * flag_bit is zero, and nothing this file emits reads or writes
     * them -- stated here rather than left implicit, because a backend
     * that assumed a flag word existed would otherwise write into
     * whatever sits at offset zero of the hart.
     */
    .flags_offset = 0u,
    .flag_bit = {0u, 0u, 0u, 0u},
    .reg_is_zero = rv_reg_zero,
    .pc_offset = (uint32_t)offsetof(rv_hart_t, pc),
    .helpers = rv_ir_helpers,
    .helper_count =
        (uint32_t)(sizeof(rv_ir_helpers) / sizeof(rv_ir_helpers[0])),
#if RV_EXT_F
    .freg_offset = rv_freg_offset,
    .fp_flags = rv_ir_fp_flags,
#endif
    .load = rv_ir_load,
    .store = rv_ir_store,
};

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

#if RV_EXT_F
/* Is the FP unit off? Every FP instruction is illegal while it is. */
static bool h_fs_off(const emu_cpu_t *cpu)
{
    return (((const rv_hart_t *)cpu)->mstatus & MSTATUS_FS_MASK) == 0u;
}

/*
 * Whether this block has made FS Dirty yet: set by its first f-register
 * write, cleared where translation of a block begins.
 */
static EMU_PER_CORE bool g_fs_marked;

/*
 * An f register written: the FPUT, and FS made Dirty on the path that
 * wrote it -- the interpreter's rule, which marks it as it writes. A
 * side exit taken before this never reaches it. Through mstatus as an IR
 * register, three ordinary operations the passes can see. **It is the
 * only mark an f write gets**: the backends call nothing for an FPUT,
 * which is what lets a block that only moves floats chain.
 *
 * **At the block's first write only.** A block is straight-line with
 * side exits and no joins, so a later write on any path means the first
 * ran too; marking at every write cost Whetstone under the JIT 3.00% of
 * host instructions, and at the first 0.38%. A declined instruction is
 * rolled back, which could discard the mark this sets -- and ends the
 * block, so no later write leans on it.
 */
static void fput(emu_ir_block_t *b, uint32_t aux, uint16_t v, uint32_t rd)
{
    (void)emu_ir_emit(b, EMU_IR_FPUT, aux, v, EMU_IR_NO_TEMP, rd, 0u);
    if (!g_fs_marked) {
        g_fs_marked = true;
        emu_ir_put(b, RV_IR_MSTATUS,
                   emu_ir_emit(b, EMU_IR_ORI, 0u, emu_ir_get(b, RV_IR_MSTATUS),
                               EMU_IR_NO_TEMP, MSTATUS_FS_MASK, 0u));
    }
}

/* fcsr's frm, for resolving a "dynamic" rounding mode at translation. */
#define RV_IR_FRM(cpu) ((((const rv_hart_t *)(cpu))->fcsr >> 5) & 7u)

/*
 * Route one FP instruction to rv_hart_fp. HELPER_TRAP rather than
 * HELPER: the helper can enter a trap, and then pc already points at
 * the handler and the rest of the block must not run.
 */
static bool rv_ir_fp_fallback(emu_ir_block_t *b, uint32_t pc, uint32_t insn)
{
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, pc,
                      0u);
    (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, emu_ir_const(b, insn),
                      EMU_IR_NO_TEMP, RV_IR_HELPER_FP, 0u);
    return true;
}
#endif

/*
 * Lower one instruction. Returns false for anything not modelled, which
 * ends the block with nothing emitted for it.
 *
 * `ends_block` is set when the instruction wrote pc itself, so the
 * caller stops rather than continuing at pc + 4.
 */
/*
 * `cpu` is read only for the state the block is *specialised* on -- the
 * rounding mode and whether the FP unit is on -- and both are carried in
 * rv_jit_ctx_key, so a block is never entered once either differs. Reading
 * anything else here would be a staleness bug; that is the rule, and
 * this file has one place it applies.
 */
#if RV_EXT_ZK
/*
 * The scalar crypto, lowered (rv_crypto.h has the semantics, and the
 * helper below has all of it).
 *
 * **The rotate-and-XOR instructions are plain IR on every host**:
 * sha256sum0 is three rotates and two XORs, which both backends lower to
 * three ROR/ROL and two XOR, where the helper was a call. sha512*, sm3p*,
 * pack and packh likewise -- constant time by construction, each a shift
 * by an immediate or a logic operation (Zkt).
 *
 * **aes32 is EMU_IR_AESSB and the rest around it**, where the host has the
 * S-box as an instruction -- x86-64 with AES-NI -- and the helper where it
 * does not: the byte select, the S-box, MixColumns' column by doublings in
 * GF(2^8) written without a test, the rotate back, the XOR. **sm4ed and
 * sm4ks are EMU_IR_SM4SB** the same way -- x86-64 with AES-NI and SSSE3,
 * AES's S-box between two affine maps (scripts/sm4-affine.py) -- and
 * their linear maps as shifts and XORs. brev8, zip, unzip and xperm stay
 * with the helper: no host here has them, and none is a short sequence.
 */
static uint16_t rv_ir_k(emu_ir_block_t *b, emu_ir_op_t op, uint16_t x,
                        uint32_t n)
{
    return emu_ir_emit(b, op, 0u, x, EMU_IR_NO_TEMP, n, 0u);
}

/* x times 2 in GF(2^8): (x << 1) ^ (0x1B & -(x >> 7)), a byte. */
static uint16_t rv_ir_xt2(emu_ir_block_t *b, uint16_t x)
{
    const uint16_t hi = emu_ir_emit(b, EMU_IR_NEG, 0u,
                                    rv_ir_k(b, EMU_IR_SHRI, x, 7u),
                                    EMU_IR_NO_TEMP, 0u, 0u);

    return rv_ir_k(b, EMU_IR_ANDI,
                   emu_ir_alu(b, EMU_IR_XOR, rv_ir_k(b, EMU_IR_SHLI, x, 1u),
                              rv_ir_k(b, EMU_IR_ANDI, hi, 0x1Bu)),
                   0xFFu);
}

/* (x rol p) ^ (x rol q) ^ (x rol r), or with r 0, x ^ ... */
static uint16_t rv_ir_rol3(emu_ir_block_t *b, uint16_t x, uint32_t p,
                           uint32_t q, uint32_t r)
{
    const uint16_t t = emu_ir_alu(b, EMU_IR_XOR, rv_ir_k(b, EMU_IR_ROTLI, x, p),
                                  rv_ir_k(b, EMU_IR_ROTLI, x, q));

    return emu_ir_alu(b, EMU_IR_XOR, t,
                      (r == 0u) ? x : rv_ir_k(b, EMU_IR_ROTLI, x, r));
}

/* Sum of a list of shifted copies: +n is a left shift by n, -n right. */
static uint16_t rv_ir_shifts(emu_ir_block_t *b, uint16_t a, const int8_t *sa,
                             uint16_t bb, const int8_t *sb)
{
    uint16_t v = EMU_IR_NO_TEMP;

    for (int pass = 0; pass < 2; pass++) {
        const int8_t *s = pass ? sb : sa;
        const uint16_t x = pass ? bb : a;

        for (; *s != 0; s++) {
            const uint16_t t = (*s > 0)
                                   ? rv_ir_k(b, EMU_IR_SHLI, x, (uint32_t)*s)
                                   : rv_ir_k(b, EMU_IR_SHRI, x, (uint32_t)-*s);

            v = (v == EMU_IR_NO_TEMP) ? t : emu_ir_alu(b, EMU_IR_XOR, v, t);
        }
    }
    return v;
}

static bool rv_ir_crypto(emu_ir_block_t *b, uint32_t insn)
{
    const uint32_t op = insn & 0x7Fu;
    const uint32_t f3 = rv_funct3(insn);
    const uint32_t f7 = insn >> 25;
    const uint16_t a = emu_ir_get(b, rv_rs1(insn));
    uint16_t v = EMU_IR_NO_TEMP;

    if (op == 0x13u) {
        if (f7 == 0x08u && f3 == 1u) {
            switch (rv_rs2(insn)) {
            case 0u: /* sha256sum0 */
                v = rv_ir_rol3(b, a, 30u, 19u, 10u);
                break;
            case 1u: /* sha256sum1 */
                v = rv_ir_rol3(b, a, 26u, 21u, 7u);
                break;
            case 2u: /* sha256sig0 */
            case 3u: /* sha256sig1 */ {
                const bool s1 = rv_rs2(insn) == 3u;
                const uint16_t t = emu_ir_alu(
                    b, EMU_IR_XOR, rv_ir_k(b, EMU_IR_ROTLI, a, s1 ? 15u : 25u),
                    rv_ir_k(b, EMU_IR_ROTLI, a, s1 ? 13u : 14u));

                v = emu_ir_alu(b, EMU_IR_XOR, t,
                               rv_ir_k(b, EMU_IR_SHRI, a, s1 ? 10u : 3u));
                break;
            }
            case 8u: /* sm3p0 */
                v = rv_ir_rol3(b, a, 9u, 17u, 0u);
                break;
            case 9u: /* sm3p1 */
                v = rv_ir_rol3(b, a, 15u, 23u, 0u);
                break;
            default:
                break;
            }
        }
    } else {
        const uint16_t bb = emu_ir_get(b, rv_rs2(insn));

        if (f7 == 0x04u && f3 == 4u) { /* pack; not zext.h, the caller's */
            v = emu_ir_alu(b, EMU_IR_OR, rv_ir_k(b, EMU_IR_SHLI, bb, 16u),
                           emu_ir_emit(b, EMU_IR_ZEXT16, 0u, a,
                                       EMU_IR_NO_TEMP, 0u, 0u));
        } else if (f7 == 0x04u && f3 == 7u) { /* packh */
            v = emu_ir_alu(
                b, EMU_IR_OR,
                rv_ir_k(b, EMU_IR_SHLI, rv_ir_k(b, EMU_IR_ANDI, bb, 0xFFu), 8u),
                rv_ir_k(b, EMU_IR_ANDI, a, 0xFFu));
        } else if (f3 == 0u && f7 >= 0x28u && f7 <= 0x2Fu &&
                   f7 != 0x2Cu && f7 != 0x2Du) {
            /* sha512*: the spec's shifts of rs1 and of rs2, XORed. */
            static const int8_t k[8][2][7] = {
                {{25, 30, -28, 0}, {-7, -2, 4, 0}},       /* sum0r */
                {{23, -14, -18, 0}, {-9, 18, 14, 0}},     /* sum1r */
                {{-1, -7, -8, 0}, {31, 25, 24, 0}},       /* sig0l */
                {{3, -6, -19, 0}, {-29, 26, 13, 0}},      /* sig1l */
                {{0}, {0}},
                {{0}, {0}},
                {{-1, -7, -8, 0}, {31, 24, 0}},           /* sig0h */
                {{3, -6, -19, 0}, {-29, 13, 0}},          /* sig1h */
            };

            v = rv_ir_shifts(b, a, k[f7 - 0x28u][0], bb, k[f7 - 0x28u][1]);
        } else if (f3 == 0u && ((f7 & 0x19u) == 0x11u)) {
            /* aes32esi 11, esmi 13, dsi 15, dsmi 17: bit 1 mix, bit 2 inverse */
            const bool mix = (f7 & 0x02u) != 0u;
            const bool inv = (f7 & 0x04u) != 0u;
            const uint32_t sh = (insn >> 30) * 8u;

            if (emu_ir_can_lower(EMU_IR_AESSB, inv ? 1u : 0u)) {
                const uint16_t x =
                    (sh != 0u) ? rv_ir_k(b, EMU_IR_SHRI, bb, sh) : bb;
                const uint16_t sv = emu_ir_emit(b, EMU_IR_AESSB, inv ? 1u : 0u,
                                                x, EMU_IR_NO_TEMP, 0u, 0u);
                uint16_t u = sv;

                if (mix && !inv) { /* {3, 1, 1, 2} */
                    const uint16_t s2 = rv_ir_xt2(b, sv);
                    const uint16_t s3 = emu_ir_alu(b, EMU_IR_XOR, sv, s2);

                    u = emu_ir_alu(
                        b, EMU_IR_OR,
                        emu_ir_alu(b, EMU_IR_OR,
                                   rv_ir_k(b, EMU_IR_SHLI, s3, 24u),
                                   rv_ir_k(b, EMU_IR_SHLI, sv, 16u)),
                        emu_ir_alu(b, EMU_IR_OR,
                                   rv_ir_k(b, EMU_IR_SHLI, sv, 8u), s2));
                } else if (mix) { /* {11, 13, 9, 14} */
                    const uint16_t s2 = rv_ir_xt2(b, sv);
                    const uint16_t s4 = rv_ir_xt2(b, s2);
                    const uint16_t s8 = rv_ir_xt2(b, s4);
                    const uint16_t s9 = emu_ir_alu(b, EMU_IR_XOR, s8, sv);
                    const uint16_t s11 = emu_ir_alu(b, EMU_IR_XOR, s9, s2);
                    const uint16_t s13 = emu_ir_alu(b, EMU_IR_XOR, s9, s4);
                    const uint16_t s14 = emu_ir_alu(
                        b, EMU_IR_XOR, emu_ir_alu(b, EMU_IR_XOR, s8, s4), s2);

                    u = emu_ir_alu(
                        b, EMU_IR_OR,
                        emu_ir_alu(b, EMU_IR_OR,
                                   rv_ir_k(b, EMU_IR_SHLI, s11, 24u),
                                   rv_ir_k(b, EMU_IR_SHLI, s13, 16u)),
                        emu_ir_alu(b, EMU_IR_OR,
                                   rv_ir_k(b, EMU_IR_SHLI, s9, 8u), s14));
                }
                v = emu_ir_alu(b, EMU_IR_XOR, a,
                               (sh != 0u) ? rv_ir_k(b, EMU_IR_ROTLI, u, sh) : u);
            }
        } else if (f3 == 0u && ((f7 & 0x1Du) == 0x18u)) {
            /*
             * sm4ed 18, sm4ks 1A: EMU_IR_SM4SB where the host has it, then
             * rv_crypto.c's linear map as (mask, shift) terms of the
             * S-box's byte -- a mask of 0xFF is none -- XORed, rotated to
             * the byte select and XORed into rs1.
             */
            static const uint8_t k[2][6][2] = {
                {{0xFFu, 0u}, {0xFFu, 8u}, {0xFFu, 2u}, {0xFFu, 18u},
                 {0x3Fu, 26u}, {0xC0u, 10u}},                    /* sm4ed */
                {{0xFFu, 0u}, {0x07u, 29u}, {0xFEu, 7u}, {0x01u, 23u},
                 {0xF8u, 13u}, {0u, 0u}},                        /* sm4ks */
            };
            const uint32_t ks = (f7 >> 1) & 1u;
            const uint32_t sh = (insn >> 30) * 8u;

            if (emu_ir_can_lower(EMU_IR_SM4SB, 0u)) {
                const uint16_t x =
                    (sh != 0u) ? rv_ir_k(b, EMU_IR_SHRI, bb, sh) : bb;
                const uint16_t sv = emu_ir_emit(b, EMU_IR_SM4SB, 0u, x,
                                                EMU_IR_NO_TEMP, 0u, 0u);
                uint16_t u = EMU_IR_NO_TEMP;

                for (unsigned i = 0u; i < 6u && k[ks][i][0] != 0u; i++) {
                    uint16_t t = sv;

                    if (k[ks][i][0] != 0xFFu) {
                        t = rv_ir_k(b, EMU_IR_ANDI, t, k[ks][i][0]);
                    }
                    if (k[ks][i][1] != 0u) {
                        t = rv_ir_k(b, EMU_IR_SHLI, t, k[ks][i][1]);
                    }
                    u = (u == EMU_IR_NO_TEMP) ? t
                                              : emu_ir_alu(b, EMU_IR_XOR, u, t);
                }
                v = emu_ir_alu(b, EMU_IR_XOR, a,
                               (sh != 0u) ? rv_ir_k(b, EMU_IR_ROTLI, u, sh) : u);
            }
        }
    }
    if (v == EMU_IR_NO_TEMP) { /* the helper: brev8, zip, xperm, SM4 without SM4SB */
        (void)emu_ir_emit(b, EMU_IR_HELPER, 0u, emu_ir_const(b, insn),
                          EMU_IR_NO_TEMP, RV_IR_HELPER_CRYPTO, 0u);
        return true;
    }
    emu_ir_put(b, rv_rd(insn), v);
    return true;
}
#endif

#if RV_EXT_ZVE32X
/*
 * The vector instructions a block lowers inline, and the vtype it knows.
 *
 * **vtype is known only after a vsetvli this block lowered**: the
 * immediate form names it, and from there to the block's end nothing
 * else in a block can change it -- vtype has no CSR write, and the
 * helper's vsetvl forgets it (RV_IR_VT_UNKNOWN). Nothing a block is
 * specialised on is vtype, so a block entered with another vtype from
 * outside never assumed one. vstart is 0 there too: vset clears it, an
 * access that completes leaves it 0, one that faults leaves the block,
 * and a CSR write ends the block before it.
 *
 * The rest goes to the helper: vill, vsetvl, a masked operation, a
 * register group that is not aligned, a form the host cannot lower
 * (emu_ir_can_lower), and everything without a mapping below.
 */
#define RV_IR_VT_UNKNOWN 0x80000000u /* vill: no vtype a block may assume */

static EMU_PER_CORE uint32_t g_vt;
static EMU_PER_CORE bool g_vs_marked;
/*
 * Whether vstart is known to be 0 here. Not at a block's start: a block
 * can begin at an access resumed after a fault, vstart where it stopped.
 * From an inline vset on, and after any vector helper that returned --
 * rv_hart_vec leaves vstart 0 whenever it does not trap -- until the
 * block ends. The inline accesses assume it.
 */
static EMU_PER_CORE bool g_vstart0;

/* VS Dirty at the block's first inline vector instruction, as fput marks
 * FS: Spike's rule, that every vector instruction that runs marks it. */
static void vs_mark(emu_ir_block_t *b)
{
    if (!g_vs_marked) {
        g_vs_marked = true;
        emu_ir_put(b, RV_IR_MSTATUS,
                   emu_ir_emit(b, EMU_IR_ORI, 0u, emu_ir_get(b, RV_IR_MSTATUS),
                               EMU_IR_NO_TEMP, MSTATUS_VS_MASK, 0u));
    }
}

static bool h_vs_off(const emu_cpu_t *cpu)
{
    return (((const rv_hart_t *)cpu)->mstatus & MSTATUS_VS_MASK) == 0u;
}

/* vsetvli and vsetivli with a vtype this unit holds: vl = min(AVL, VLMAX). */
static bool rv_ir_vset(emu_ir_block_t *b, uint32_t insn)
{
    const uint32_t rd = rv_rd(insn);
    const uint32_t rs1 = rv_rs1(insn);
    const bool imm = (insn >> 30) == 3u;
    uint32_t vt;

    if ((insn >> 31) == 0u) {
        vt = (insn >> 20) & 0x7FFu;
    } else if (imm) {
        vt = (insn >> 20) & 0x3FFu;
    } else {
        return false; /* vsetvl: vtype from a register */
    }
    const uint32_t vlmax = rv_vec_vlmax(vt);

    if (vlmax == 0u) {
        return false; /* vill */
    }

    uint16_t vl;

    if (imm) {
        vl = emu_ir_const(b, (rs1 < vlmax) ? rs1 : vlmax);
    } else if (rs1 != 0u) {
        /* avl - ((avl - vlmax) & -(avl > vlmax)): no select on RISC-V. */
        const uint16_t avl = emu_ir_get(b, rs1);
        const uint16_t max = emu_ir_const(b, vlmax);
        const uint16_t over = emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_GTU, avl,
                                          max, 0u, 0u);
        const uint16_t mask = emu_ir_emit(b, EMU_IR_NEG, 0u, over,
                                          EMU_IR_NO_TEMP, 0u, 0u);

        vl = emu_ir_alu(b, EMU_IR_SUB, avl,
                        emu_ir_alu(b, EMU_IR_AND,
                                   emu_ir_alu(b, EMU_IR_SUB, avl, max), mask));
    } else if (rd != 0u) {
        vl = emu_ir_const(b, vlmax);
    } else if (g_vt != RV_IR_VT_UNKNOWN && rv_vec_vlmax(g_vt) == vlmax) {
        vl = emu_ir_get(b, RV_IR_VL); /* keep vl: VLMAX the same */
    } else {
        return false; /* keep vl under a VLMAX this block cannot see */
    }
    vs_mark(b);
    emu_ir_put(b, RV_IR_VL, vl);
    emu_ir_put(b, RV_IR_VTYPE, emu_ir_const(b, vt));
    emu_ir_put(b, RV_IR_VSTART, emu_ir_const(b, 0u));
    if (rd != 0u) {
        emu_ir_put(b, rd, vl);
    }
    g_vt = vt;
    g_vstart0 = true;
    return true;
}

/*
 * An OP-V arithmetic instruction to EMU_IR_VOP: which operation, and
 * which of .vv (1), .vx (2) and .vi (4) it has -- the same forms
 * rv_vec.c's opi_forms allows, so a form that does not exist still
 * reaches the helper and its illegal instruction.
 */
static bool rv_ir_vop_of(uint32_t f3, uint32_t f6, uint32_t vxrm,
                         uint32_t *op, uint32_t *forms)
{
    if (f3 == 2u || f3 == 6u) { /* OPMVV, OPMVX: .vv and .vx each */
        *forms = 3u;
        switch (f6) {
        case 0x25u: *op = EMU_IR_V_MUL; return true;
        case 0x2Du: *op = EMU_IR_V_MACC; return true;
        case 0x2Fu: *op = EMU_IR_V_NMSAC; return true;
        case 0x29u: *op = EMU_IR_V_MADD; return true;
        case 0x2Bu: *op = EMU_IR_V_NMSUB; return true;
        /*
         * The averaging adds round by vxrm, which the block is
         * specialised on (rv_jit_ctx_key), so the mode is a constant
         * here; only rnu has an operation.
         */
        case 0x08u: *op = EMU_IR_V_AADDU_RNU; return vxrm == 0u;
        case 0x09u: *op = EMU_IR_V_AADD_RNU; return vxrm == 0u;
        /* widening: vd twice SEW in twice the registers (rv_ir_vec) */
        case 0x3Cu: *op = EMU_IR_V_WMACCU; return true;
        case 0x3Du: *op = EMU_IR_V_WMACC; return true;
        case 0x3Fu: *op = EMU_IR_V_WMACCSU; return true;
        case 0x3Eu: *op = EMU_IR_V_WMACCUS; *forms = 2u; return true;
        default: return false;
        }
    }
    /*
     * Only OPIVV, OPIVI and OPIVX from here. OPFVV and OPFVF (1 and 5)
     * share the funct6 values below and are Zve32f's, which this unit
     * has not: they must reach the helper and its illegal instruction.
     * fuzz-rv32-jit with the unit built found an OPFVV run as vadd.vx.
     */
    if (f3 != 0u && f3 != 3u && f3 != 4u) {
        return false;
    }
    switch (f6) {
    case 0x00u: *op = EMU_IR_V_ADD; *forms = 7u; return true;
    case 0x02u: *op = EMU_IR_V_SUB; *forms = 3u; return true;
    case 0x03u: *op = EMU_IR_V_RSUB; *forms = 6u; return true;
    case 0x04u: *op = EMU_IR_V_MINU; *forms = 3u; return true;
    case 0x05u: *op = EMU_IR_V_MIN; *forms = 3u; return true;
    case 0x06u: *op = EMU_IR_V_MAXU; *forms = 3u; return true;
    case 0x07u: *op = EMU_IR_V_MAX; *forms = 3u; return true;
    case 0x09u: *op = EMU_IR_V_AND; *forms = 7u; return true;
    case 0x0Au: *op = EMU_IR_V_OR; *forms = 7u; return true;
    case 0x0Bu: *op = EMU_IR_V_XOR; *forms = 7u; return true;
    case 0x17u: *op = EMU_IR_V_MV; *forms = 7u; return true; /* vmv.v */
    case 0x25u: *op = EMU_IR_V_SLL; *forms = 7u; return true;
    case 0x28u: *op = EMU_IR_V_SRL; *forms = 7u; return true;
    case 0x29u: *op = EMU_IR_V_SRA; *forms = 7u; return true;
    /* narrowing: vs2 twice SEW in twice the registers (rv_ir_vec) */
    case 0x2Cu: *op = EMU_IR_V_NSRL; *forms = 7u; return true;
    case 0x2Du: *op = EMU_IR_V_NSRA; *forms = 7u; return true;
    case 0x2Eu: *op = EMU_IR_V_NCLIPU_RNU; *forms = 7u; return vxrm == 0u;
    case 0x2Fu: *op = EMU_IR_V_NCLIP_RNU; *forms = 7u; return vxrm == 0u;
    default: return false;
    }
}

/*
 * A unit-stride vector load or store, unmasked, of one field, to
 * EMU_IR_VMEM: inline where vl fills the group and the bytes lie in the
 * block's RAM window, rv_hart_vec through the helper where not -- the
 * last trip of a strip-mined loop, an address outside, a fault. Its
 * group is VLMAX elements of EEW, which this block knows from its
 * vsetvli; a fractional EMUL, under 16 bytes, keeps the helper, as does
 * a build where a misaligned element must trap.
 */
static bool rv_ir_vmem(emu_cpu_t *cpu, emu_ir_block_t *b, uint32_t insn,
                       uint32_t pc)
{
#if !RV_MISALIGNED_OK
    (void)cpu;
    (void)b;
    (void)insn;
    (void)pc;
    return false;
#else
    const bool store = (insn & 0x7Fu) == 0x27u;
    const uint32_t width = (insn >> 12) & 7u;
    const uint32_t vd = (insn >> 7) & 31u;
    uint32_t eew;

    /*
     * A whole-register access -- vl<n>re<eew>.v, vs<n>r.v -- moves n
     * registers whatever vtype and vl are, even vill: the group is fixed
     * by the encoding, so the copy needs no vtype from this block, and vl
     * goes in as the group's element count, which the inline path always
     * takes. A compiler spills and reloads through these.
     */
    const uint32_t nf = insn >> 29;

    if (!h_vs_off(cpu) && b->has_fast && g_vstart0 &&
        ((insn >> 25) & 0xFu) == 1u &&
        ((insn >> 20) & 31u) == 0x08u /* lumop: whole registers */ &&
        (nf == 0u || nf == 1u || nf == 3u || nf == 7u) &&
        vd % (nf + 1u) == 0u && (!store || width == 0u) &&
        (width == 0u || width >= 5u) && width != 7u &&
        emu_ir_can_lower(EMU_IR_VMEM, 0u)) {
        const uint32_t ew = (width == 0u) ? 0u : width - 4u; /* log2 bytes */
        const uint32_t glg = (nf == 0u) ? 0u : (nf == 1u) ? 1u : (nf == 3u) ? 2u : 3u;

        vs_mark(b);
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        emu_ir_vmem(b, (uint8_t)RV_IR_HELPER_VEC, emu_ir_get(b, rv_rs1(insn)),
                    emu_ir_const(b, ((nf + 1u) * RV_VLENB) >> ew),
                    emu_ir_const(b, insn), EMU_IR_VMEM_IMM(vd, glg, ew, store));
        return true;
    }
    if (h_vs_off(cpu) || g_vt == RV_IR_VT_UNKNOWN || !b->has_fast) {
        return false;
    }
    switch (width) {
    case 0u: eew = 0u; break;
    case 5u: eew = 1u; break;
    case 6u: eew = 2u; break;
    default: return false; /* EEW 64: past ELEN */
    }
    const uint32_t bytes = rv_vec_vlmax(g_vt) << eew;

    /*
     * Strided (mop 2), unmasked, one field: EMU_IR_VMEMS, inline where
     * every element lies in the window. vd, rs1 and rs2 go in its imm,
     * from which the helper rebuilds this instruction.
     */
    if ((insn >> 25) == 0x05u /* nf 0, mew 0, mop 2, vm 1 */) {
        const uint32_t rs2 = (insn >> 20) & 31u;

        if ((bytes != 16u && bytes != 32u && bytes != 64u && bytes != 128u) ||
            vd % (bytes / RV_VLENB) != 0u || !emu_ir_can_lower(EMU_IR_VMEMS, 0u)) {
            return false;
        }
        vs_mark(b);
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        emu_ir_vmems(b, (uint8_t)RV_IR_HELPER_VMEMS, emu_ir_get(b, rv_rs1(insn)),
                     emu_ir_get(b, RV_IR_VL), emu_ir_get(b, rs2),
                     EMU_IR_VMEMS_IMM(vd, eew, store, rv_rs1(insn) | (rs2 << 5)));
        return true;
    }
    if ((insn >> 25) != 1u /* nf, mew, mop 0, vm 1 */ ||
        ((insn >> 20) & 31u) != 0u /* a plain unit-stride access */) {
        return false;
    }
    uint32_t glg;

    switch (bytes) {
    case 16u: glg = 0u; break;
    case 32u: glg = 1u; break;
    case 64u: glg = 2u; break;
    case 128u: glg = 3u; break;
    default: return false;
    }
    if (vd % (bytes / RV_VLENB) != 0u || !emu_ir_can_lower(EMU_IR_VMEM, 0u)) {
        return false;
    }
    vs_mark(b);
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, pc,
                      0u);
    emu_ir_vmem(b, (uint8_t)RV_IR_HELPER_VEC, emu_ir_get(b, rv_rs1(insn)),
                emu_ir_get(b, RV_IR_VL), emu_ir_const(b, insn),
                EMU_IR_VMEM_IMM(vd, glg, eew, store));
    return true;
#endif
}

/*
 * The reductions -- vredsum, vredand, vredor, vredxor, vredminu,
 * vredmin, vredmaxu, vredmax (OPMVV, funct6 0-7), and vwredsumu and
 * vwredsum (OPIVV, 0x30 and 0x31) -- to EMU_IR_VRED: vd[0] from vs1[0]
 * and vs2's group, masked or not, vd any register, v0 included. vstart
 * is 0 wherever this block knows vtype. vs2's group misaligned, and a
 * widening sum from 32 bits (64 is past ELEN), are the helper's illegal
 * instruction.
 */
static bool rv_ir_vred(emu_ir_block_t *b, uint32_t f3, uint32_t f6, bool vm,
                       uint32_t vd, uint32_t vs2, uint32_t vs1)
{
    const uint32_t sew = (g_vt >> 3) & 7u;
    const int32_t lg = ((g_vt & 7u) >= 4u) ? (int32_t)(g_vt & 7u) - 8
                                           : (int32_t)(g_vt & 7u);
    const uint32_t nlg = (lg > 0) ? (uint32_t)lg : 0u;
    const uint32_t rop = (f3 == 2u) ? f6
                         : (f6 == 0x30u) ? EMU_IR_R_WSUMU : EMU_IR_R_WSUM;
    const uint8_t aux = EMU_IR_VRED_AUX(rop, sew, !vm);

    if (vs2 % (1u << nlg) != 0u || (EMU_IR_R_IS_WIDE(rop) && sew > 1u) ||
        !emu_ir_can_lower(EMU_IR_VRED, aux)) {
        return false;
    }
    vs_mark(b);
    (void)emu_ir_emit(b, EMU_IR_VRED, aux, EMU_IR_NO_TEMP, emu_ir_get(b, RV_IR_VL),
                      EMU_IR_VOPM_IMM(vd, vs2, vs1, nlg, 0u), 0u);
    return true;
}

static bool rv_ir_vec(emu_cpu_t *cpu, emu_ir_block_t *b, uint32_t insn)
{
    const uint32_t f3 = (insn >> 12) & 7u;
    const uint32_t f6 = insn >> 26;
    const bool vm = ((insn >> 25) & 1u) != 0u;
    const uint32_t vs2 = (insn >> 20) & 31u;
    const uint32_t r1 = (insn >> 15) & 31u;
    const uint32_t vd = (insn >> 7) & 31u;

    if (h_vs_off(cpu)) {
        return false;
    }
    if (f3 == 7u) {
        return rv_ir_vset(b, insn);
    }
    if (g_vt != RV_IR_VT_UNKNOWN &&
        ((f3 == 2u && f6 <= 0x07u) || (f3 == 0u && (f6 == 0x30u || f6 == 0x31u)))) {
        return rv_ir_vred(b, f3, f6, vm, vd, vs2, r1);
    }
    /*
     * Masked (vm 0) forms are EMU_IR_VOPM, under v0; a masked operation
     * may not write v0, which holds its mask -- the helper's illegal
     * instruction.
     */
    if (g_vt == RV_IR_VT_UNKNOWN || (!vm && vd == 0u)) {
        return false;
    }

    uint32_t vop;
    uint32_t forms;

    if (!rv_ir_vop_of(f3, f6, ((const rv_hart_t *)cpu)->vxrm & 3u, &vop,
                      &forms)) {
        return false;
    }
    const uint32_t form = (f3 == 0u || f3 == 2u) ? 1u : (f3 == 3u) ? 4u : 2u;
    const uint32_t sew = (g_vt >> 3) & 7u; /* log2 bytes: 0-2, vill aside */
    const int32_t lg = ((g_vt & 7u) >= 4u) ? (int32_t)(g_vt & 7u) - 8
                                           : (int32_t)(g_vt & 7u);
    const uint32_t nlg = (lg > 0) ? (uint32_t)lg : 0u;
    const uint32_t grp = 1u << nlg;
    const bool shift = vop == EMU_IR_V_SLL || vop == EMU_IR_V_SRL ||
                       vop == EMU_IR_V_SRA || EMU_IR_V_IS_NARROW(vop);

    /*
     * Narrowing: LMUL 1 to 4 and SEW 8 or 16, vs2's group of twice the
     * registers aligned to it, and vd disjoint from it or its first
     * register -- the overlap the specification allows; any other is the
     * helper's.
     */
    if (EMU_IR_V_IS_NARROW(vop) &&
        (lg < 0 || lg > 2 || sew > 1u || vs2 % (2u * grp) != 0u ||
         vd % grp != 0u ||
         (vd != vs2 && vd < vs2 + 2u * grp && vs2 < vd + grp))) {
        return false;
    }
    /* vs2's group: twice the registers for a narrowing operation */
    const uint32_t sgrp = EMU_IR_V_IS_NARROW(vop) ? 2u * grp : grp;

    if ((forms & form) == 0u || vd % grp != 0u ||
        (vop != EMU_IR_V_MV && vs2 % sgrp != 0u) ||
        (vop == EMU_IR_V_MV && vm && vs2 != 0u) ||
        (vop == EMU_IR_V_MV && !vm && vs2 % grp != 0u) ||
        (form == 1u && r1 % grp != 0u)) {
        return false;
    }
    /*
     * Widening: LMUL 1 to 4 and SEW 8 or 16, vd's group of twice the
     * registers aligned to it, and disjoint from the sources -- the
     * overlap the specification allows is the helper's to have, as is
     * every one it reserves.
     */
    if (EMU_IR_V_IS_WIDE(vop) &&
        (lg < 0 || lg > 2 || sew > 1u || vd % (2u * grp) != 0u ||
         (vd < vs2 + grp && vs2 < vd + 2u * grp) ||
         (form == 1u && vd < r1 + grp && r1 < vd + 2u * grp))) {
        return false;
    }
    const uint8_t aux = EMU_IR_VOP_AUX(vop, sew, form != 1u);

    /* vmv.v under a mask is vmerge; the rest keep their operation. */
    if (!vm && vop == EMU_IR_V_MV) {
        vop = EMU_IR_V_MERGE;
    }
    const uint8_t maux = EMU_IR_VOP_AUX(vop, sew, form != 1u);

    if (!vm && (EMU_IR_V_IS_WIDE(vop) || EMU_IR_V_IS_NARROW(vop))) {
        return false; /* not under a mask yet */
    }
    if (!emu_ir_can_lower(vm ? EMU_IR_VOP : EMU_IR_VOPM, vm ? aux : maux)) {
        return false;
    }

    uint16_t scalar = EMU_IR_NO_TEMP;

    if (form == 2u) {
        scalar = emu_ir_get(b, r1);
    } else if (form == 4u) {
        /* simm5, but a shift's is unsigned: uimm5 */
        scalar = emu_ir_const(b, shift ? r1
                                       : (uint32_t)((int32_t)(r1 << 27) >> 27));
    }
    vs_mark(b);
    if (!vm) {
        (void)emu_ir_emit(b, EMU_IR_VOPM, maux, scalar, emu_ir_get(b, RV_IR_VL),
                          EMU_IR_VOPM_IMM(vd, vs2, r1, nlg, 0u), 0u);
        return true;
    }
    (void)emu_ir_emit(b, EMU_IR_VOP, aux, scalar, emu_ir_get(b, RV_IR_VL),
                      EMU_IR_VOP_IMM(vd, vs2, r1, nlg), 0u);
    return true;
}
#endif

static bool lower_one(emu_cpu_t *cpu, emu_ir_block_t *b, uint32_t insn,
                      uint32_t pc, uint32_t len, bool *ends_block,
                      bool *counted)
{
#if !RV_EXT_F
    (void)cpu;
#endif
    const uint32_t op = insn & 0x7Fu;
    const uint32_t rd = rv_rd(insn);
    const uint32_t rs1 = rv_rs1(insn);
    const uint32_t rs2 = rv_rs2(insn);
    const uint32_t f3 = rv_funct3(insn);

    *ends_block = false;
    *counted = false;

#if RV_EXT_ZK
    /*
     * The scalar crypto, through one helper, asked before the opcode's own
     * decode -- a translate-time question only. Not zext.h, which is
     * pack with rs2 zero and lowered natively below.
     */
    if ((op == 0x13u || op == 0x33u) && (insn & 0xFFF0707Fu) != 0x08004033u &&
        rv_crypto_is(insn)) {
        return rv_ir_crypto(b, insn);
    }
#endif

#if RV_EXT_ZVE32X
    /*
     * The vector unit, before the FP group's FS gate: VS gates these,
     * and rv_hart_vec asks it at run time. A trap leaves through the
     * helper's return, so nothing the block holds is assumed past it.
     * What can be lowered is (rv_ir_vec), and the rest is the helper.
     */
    if (op == 0x57u ||
        ((op == 0x07u || op == 0x27u) && rv_vec_is_mem(insn))) {
        if (op == 0x57u ? rv_ir_vec(cpu, b, insn)
                        : rv_ir_vmem(cpu, b, insn, pc)) {
            return true;
        }
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, emu_ir_const(b, insn),
                          EMU_IR_NO_TEMP, RV_IR_HELPER_VEC, 0u);
        if (op == 0x57u && f3 == 7u) {
            g_vt = RV_IR_VT_UNKNOWN; /* the helper's vset: anything */
        }
        g_vstart0 = true; /* the helper returned: vstart is 0 */
        return true;
    }
#endif

    switch (op) {
    case 0x37u: /* LUI   */
        emu_ir_put(b, rd, emu_ir_const(b, (uint32_t)rv_imm_u(insn)));
        return true;

    case 0x17u: /* AUIPC */
        emu_ir_put(b, rd, emu_ir_const(b, pc + (uint32_t)rv_imm_u(insn)));
        return true;

    case 0x0Fu: /* MISC-MEM */
        /*
         * **Plain FENCE is a no-op here, and declining it was costing
         * whole blocks.** One hart, no store buffer, no cache between
         * the core and its devices -- the interpreter says so and does
         * nothing. The translator did not lower it at all, so every
         * barrier ended a block *and* took a fallback, which under
         * Linux is constant: memory barriers are everywhere in kernel
         * code, and blocks were averaging under three instructions
         * because of it.
         *
         * Only f3 == 0. FENCE.I means the guest wrote instructions and
         * a translating backend must discard what it built from them,
         * which is not something a block can do to itself; CBO is a
         * real operation on real memory. Both still decline, and
         * declining is correct for them.
         */
        if (f3 != 0u) {
            return false;
        }
        return true;

    case 0x13u: { /* OP-IMM */
        const uint16_t x = emu_ir_get(b, rs1);
        const uint32_t imm = (uint32_t)rv_imm_i(insn);
        const uint32_t sh = insn >> 25;

        switch (f3) {
        case 0u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, EMU_IR_ADD, x, emu_ir_const(b, imm)));
            return true;
        case 4u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, EMU_IR_XOR, x, emu_ir_const(b, imm)));
            return true;
        case 6u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, EMU_IR_OR, x, emu_ir_const(b, imm)));
            return true;
        case 7u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, EMU_IR_AND, x, emu_ir_const(b, imm)));
            return true;
        case 2u:
            emu_ir_put(b, rd,
                       emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LT, x,
                                   emu_ir_const(b, imm), 0u, 0u));
            return true;
        case 3u:
            emu_ir_put(b, rd,
                       emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LTU, x,
                                   emu_ir_const(b, imm), 0u, 0u));
            return true;
        case 1u:
            /*
             * Shift-left, and under every other funct7 the Zbb unary
             * operations and Zbs's immediate forms, which share the
             * slot. The whole slot is decoded here, in the
             * interpreter's order and under the interpreter's gates: an
             * encoding it would trap on is declined, so that it does.
             */
            if (sh == 0u) {
                emu_ir_put(b, rd,
                           emu_ir_emit(b, EMU_IR_SHLI, 0u, x, EMU_IR_NO_TEMP,
                                       insn >> 20 & 31u, 0u));
                return true;
            }
#if RV_EXT_ZBB
            if (sh == 0x30u) {
                switch (rs2) {
                case 0u: /* clz */
                case 1u: /* ctz */ {
                    /*
                     * Both IR operations answer 32 for a zero input,
                     * which is Zbb's definition and is the awkward
                     * case: x86's BSR leaves its destination undefined
                     * there and ARM's CLZ does not, so it is the
                     * lowering's to get right and isatest asks.
                     */
                    const emu_ir_op_t count =
                        (rs2 == 0u) ? EMU_IR_CLZ : EMU_IR_CTZ;

                    if (!emu_ir_can_lower(count, 0u)) {
                        return false;
                    }
                    emu_ir_put(b, rd, rv_ir_unary(b, count, x));
                    return true;
                }
                case 2u: /* cpop */
                    return rv_ir_bitop_call(b, insn);
                case 4u: /* sext.b */
                    emu_ir_put(b, rd, rv_ir_unary(b, EMU_IR_SEXT8, x));
                    return true;
                case 5u: /* sext.h */
                    emu_ir_put(b, rd, rv_ir_unary(b, EMU_IR_SEXT16, x));
                    return true;
                default:
                    return false;
                }
            }
#endif
#if RV_EXT_ZBS
            /*
             * bseti, bclri, binvi: the mask is a constant, so each is
             * the ordinary logical operation with an immediate -- and
             * the optimiser folds the constant into the immediate form
             * where the host has one.
             */
            if (sh == 0x14u || sh == 0x24u || sh == 0x34u) {
                const uint32_t bit = 1u << rs2;

                emu_ir_put(
                    b, rd,
                    (sh == 0x14u)
                        ? emu_ir_alu(b, EMU_IR_OR, x, emu_ir_const(b, bit))
                    : (sh == 0x24u)
                        ? emu_ir_alu(b, EMU_IR_AND, x, emu_ir_const(b, ~bit))
                        : emu_ir_alu(b, EMU_IR_XOR, x, emu_ir_const(b, bit)));
                return true;
            }
#endif
            return false;
        case 5u:
            if (sh == 0u) {
                emu_ir_put(b, rd,
                           emu_ir_emit(b, EMU_IR_SHRI, 0u, x, EMU_IR_NO_TEMP,
                                       insn >> 20 & 31u, 0u));
                return true;
            }
            if (sh == 0x20u) {
                emu_ir_put(b, rd,
                           emu_ir_emit(b, EMU_IR_SARI, 0u, x, EMU_IR_NO_TEMP,
                                       insn >> 20 & 31u, 0u));
                return true;
            }
            /*
             * Zbb's rori, which is 4.34% of the crypto guest -- 21,710
             * of 500,000 executed instructions, all inside SHA-256's
             * compress. Untranslated until now, so every one of them
             * ended a block and fell to the interpreter.
             *
             * **It rotates right and EMU_IR_ROTLI rotates left**, so the
             * amount is complemented. A rotate by zero must stay zero
             * rather than becoming 32: `(32 - n) & 31` gets both, where
             * `32 - n` alone would turn the identity into a full turn --
             * which on ARM is the RRX encoding and not a rotation at
             * all. Same trap as the imm5 == 0 note in t2_shift_imm.
             */
#if RV_EXT_ZBB
            if (sh == 0x30u) {
                const uint32_t n = (insn >> 20) & 31u;

                emu_ir_put(b, rd,
                           emu_ir_emit(b, EMU_IR_ROTLI, 0u, x, EMU_IR_NO_TEMP,
                                       (32u - n) & 31u, 0u));
                return true;
            }
            /* orc.b and rev8 are single encodings: the rs2 field is
             * part of the opcode, and any other value is reserved. */
            if (sh == 0x14u && rs2 == 7u) {
                return rv_ir_bitop_call(b, insn);
            }
            if (sh == 0x34u && rs2 == 24u) {
                emu_ir_put(b, rd, rv_ir_unary(b, EMU_IR_BSWAP32, x));
                return true;
            }
#endif
#if RV_EXT_ZBS
            if (sh == 0x24u) { /* bexti */
                const uint16_t t =
                    emu_ir_emit(b, EMU_IR_SHRI, 0u, x, EMU_IR_NO_TEMP, rs2, 0u);

                emu_ir_put(b, rd,
                           emu_ir_alu(b, EMU_IR_AND, t, emu_ir_const(b, 1u)));
                return true;
            }
#endif
            return false;
        default:
            return false;
        }
    }

    case 0x33u: { /* OP */
        const uint32_t f7 = insn >> 25;

        if (f7 == 1u) { /* M extension */
            /*
             * The multiplies are lowered natively; divide, remainder
             * and MULHSU go to a helper.
             *
             * They are not open-coded, and the reason is the one this
             * comment used to give for declining them outright: RISC-V
             * defines divide by zero and signed overflow as producing
             * values where x86 traps, so emitting them inline means
             * reproducing two special cases per host. A call reproduces
             * none, and -- unlike declining -- keeps the block whole.
             */
            static const uint8_t k_mul[4] = {EMU_IR_MUL, EMU_IR_MULHS, 0u,
                                             EMU_IR_MULHU};
            if (f3 == 2u) {
                /*
                 * MULHSU alone stays on the helper: it is the one
                 * multiply with no single host instruction, and it is
                 * rare enough that open-coding the widening mixed-sign
                 * product would be three copies of arithmetic for no
                 * measured gain.
                 */
                (void)emu_ir_emit(b, EMU_IR_HELPER, 0u, emu_ir_const(b, insn),
                                  EMU_IR_NO_TEMP, RV_IR_HELPER_MULDIV, 0u);
                return true;
            }
            if (f3 > 3u) {
                return rv_ir_lower_divrem(b, insn, f3, rd, rs1, rs2);
            }
            emu_ir_put(b, rd,
                       emu_ir_alu(b, (emu_ir_op_t)k_mul[f3], emu_ir_get(b, rs1),
                                  emu_ir_get(b, rs2)));
            return true;
        }
#if RV_EXT_ZICOND
        if (f7 == 0x07u && (f3 == 5u || f3 == 7u)) {
            /*
             * czero.eqz / czero.nez: rd = rs1, or zero when rs2 is --
             * respectively is not -- zero.
             *
             * `rs1 & -(keep)`, where keep is the 0-or-1 answer to "is
             * rs2 the value that lets rs1 through". Three operations
             * every backend already lowers, and no EMU_IR_SELECT: that
             * one reads the guest's *flags*, of which RISC-V has none,
             * and Thumb-2 declines it besides -- so using it would have
             * lowered this on the host and left the board where it was.
             * The G4MH translator made the same choice for CMOV, for the
             * same reason.
             *
             * Branch-free is also the point of the extension: a guest
             * compiled with Zicond is asking not to have a branch here.
             */
            const uint16_t keep =
                emu_ir_emit(b, EMU_IR_SETCC,
                            (f3 == 5u) ? EMU_IR_C_NE : EMU_IR_C_EQ,
                            emu_ir_get(b, rs2), emu_ir_const(b, 0u), 0u, 0u);
            const uint16_t mask = emu_ir_emit(b, EMU_IR_NEG, 0u, keep,
                                              EMU_IR_NO_TEMP, 0u, 0u);

            emu_ir_put(b, rd,
                       emu_ir_alu(b, EMU_IR_AND, emu_ir_get(b, rs1), mask));
            return true;
        }
#endif
        const uint16_t x = emu_ir_get(b, rs1);
        const uint16_t y = emu_ir_get(b, rs2);
        const bool alt = (f7 == 0x20u);

        /*
         * Zba, Zbb, Zbc and Zbs, which is every remaining funct7.
         *
         * **Declining this slot was 98.8% of what the JIT still
         * interpreted on CoreMark and 99.6% on Dhrystone**, and nearly
         * all of that is one family: sh1add, sh2add and sh3add, which
         * the compiler emits for every array index. Each one ended its
         * block. Two IR operations.
         *
         * Decoded in one place and under the interpreter's own gates --
         * min/max and clmul share funct7 0x05, bclr and bext share 0x24
         * -- and whatever is not named declines, so a reserved encoding
         * still reaches the interpreter and traps there.
         *
         * Everything open-coded uses operations *both* backends lower:
         * shifts, the three logical operations, NOT. The IR has
         * EMU_IR_BSET and its three siblings and x86-64 has bt/bts for
         * them, but Thumb-2 declines all four, and an operation one
         * backend refuses costs that host the whole block. The variable
         * shifts mask their count to five bits in every consumer of the
         * IR, which is Zbs's definition of the bit index.
         */
        if (f7 != 0u && !alt) {
            switch (f7) {
#if RV_EXT_ZBA
            case 0x10u: /* sh1add, sh2add, sh3add: (rs1 << n) + rs2 */
                if (f3 != 2u && f3 != 4u && f3 != 6u) {
                    return false;
                }
                emu_ir_put(b, rd,
                           emu_ir_alu(b, EMU_IR_ADD,
                                      emu_ir_emit(b, EMU_IR_SHLI, 0u, x,
                                                  EMU_IR_NO_TEMP, f3 >> 1, 0u),
                                      y));
                return true;
#endif
#if RV_EXT_ZBB || RV_EXT_ZBC
            case 0x05u: /* clmul* below 4, min/max from 4 */
#if !RV_EXT_ZBC
                if (f3 < 4u) {
                    return false;
                }
#endif
#if !RV_EXT_ZBB
                if (f3 >= 4u) {
                    return false;
                }
#endif
#if RV_EXT_ZBC
                if (f3 >= 1u && f3 <= 3u) {
                    /* clmul, clmulr, clmulh: the low half, bits 62:31,
                     * the high half -- where the host has the instruction */
                    const uint8_t part =
                        (f3 == 1u) ? 0u : ((f3 == 3u) ? 1u : 2u);

                    if (emu_ir_can_lower(EMU_IR_CLMUL, part)) {
                        emu_ir_put(b, rd,
                                   emu_ir_emit(b, EMU_IR_CLMUL, part, x, y, 0u,
                                               0u));
                        return true;
                    }
                }
#endif
                return (f3 != 0u) && rv_ir_bitop_call(b, insn);
#endif
#if RV_EXT_ZBB
            case 0x30u: /* rol, ror */
                return (f3 == 1u || f3 == 5u) && rv_ir_bitop_call(b, insn);
            case 0x04u: /* zext.h: rs2 is part of the encoding */
                if (f3 != 4u || rs2 != 0u) {
                    return false;
                }
                emu_ir_put(b, rd, rv_ir_unary(b, EMU_IR_ZEXT16, x));
                return true;
#endif
#if RV_EXT_ZBS
            case 0x14u: /* bset */
            case 0x34u: /* binv */
            case 0x24u: /* bclr, and bext at funct3 5 */ {
                if (f7 == 0x24u && f3 == 5u) {
                    emu_ir_put(b, rd,
                               emu_ir_alu(b, EMU_IR_AND,
                                          emu_ir_alu(b, EMU_IR_SHR, x, y),
                                          emu_ir_const(b, 1u)));
                    return true;
                }
                if (f3 != 1u) {
                    return false;
                }
                const uint16_t bit =
                    emu_ir_alu(b, EMU_IR_SHL, emu_ir_const(b, 1u), y);

                emu_ir_put(b, rd,
                           (f7 == 0x14u) ? emu_ir_alu(b, EMU_IR_OR, x, bit)
                           : (f7 == 0x34u)
                               ? emu_ir_alu(b, EMU_IR_XOR, x, bit)
                               : emu_ir_alu(b, EMU_IR_AND, x,
                                            rv_ir_unary(b, EMU_IR_NOT, bit)));
                return true;
            }
#endif
            default:
                return false;
            }
        }

        switch (f3) {
        case 0u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, alt ? EMU_IR_SUB : EMU_IR_ADD, x, y));
            return true;
        case 1u:
            if (alt) {
                return false;
            }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_SHL, x, y));
            return true;
        case 2u:
            if (alt) {
                return false;
            }
            emu_ir_put(b, rd,
                       emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LT, x, y, 0u, 0u));
            return true;
        case 3u:
            if (alt) {
                return false;
            }
            emu_ir_put(
                b, rd,
                emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LTU, x, y, 0u, 0u));
            return true;
        case 4u:
            if (alt) { /* xnor */
#if RV_EXT_ZBB
                emu_ir_put(b, rd,
                           rv_ir_unary(b, EMU_IR_NOT,
                                       emu_ir_alu(b, EMU_IR_XOR, x, y)));
                return true;
#else
                return false;
#endif
            }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_XOR, x, y));
            return true;
        case 5u:
            emu_ir_put(b, rd,
                       emu_ir_alu(b, alt ? EMU_IR_SAR : EMU_IR_SHR, x, y));
            return true;
        case 6u:
            if (alt) { /* orn */
#if RV_EXT_ZBB
                emu_ir_put(
                    b, rd,
                    emu_ir_alu(b, EMU_IR_OR, x, rv_ir_unary(b, EMU_IR_NOT, y)));
                return true;
#else
                return false;
#endif
            }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_OR, x, y));
            return true;
        case 7u:
            if (alt) { /* andn */
#if RV_EXT_ZBB
                emu_ir_put(b, rd,
                           emu_ir_alu(b, EMU_IR_AND, x,
                                      rv_ir_unary(b, EMU_IR_NOT, y)));
                return true;
#else
                return false;
#endif
            }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_AND, x, y));
            return true;
        default:
            return false;
        }
    }

    case 0x03u: { /* LOAD */
        /*
         * LB LH LW LBU LHU, and nothing else: 3 is LD, 6 LWU and 7
         * nothing, and RV32 has none of them. Declined, so that the
         * interpreter raises illegal-instruction. Until fuzz-rv32-jit
         * asked, the spec below took them as an 8-byte load, a LW and
         * an 8-byte unsigned load -- on both hosts, since this
         * translator is theirs.
         */
        if (f3 == 3u || f3 >= 6u) {
            return false;
        }
        /* pc first: a fault records it. */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);
        const uint8_t spec = EMU_IR_MEM_AUX(1u << (f3 & 3u), (f3 & 4u) == 0u);

        emu_ir_put(b, rd,
                   emu_ir_emit(b, EMU_IR_LOAD, spec, base, EMU_IR_NO_TEMP,
                               (uint32_t)rv_imm_i(insn), 0u));
        return true;
    }

    case 0x23u: { /* STORE */
        /* SB SH SW only; f3 & 3 made 4-7 into those and 3 into SD. */
        if (f3 > 2u) {
            return false;
        }
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);
        const uint16_t val = emu_ir_get(b, rs2);
        const uint8_t spec = EMU_IR_MEM_AUX(1u << (f3 & 3u), 0u);

        (void)emu_ir_emit(b, EMU_IR_STORE, spec, base, val,
                          (uint32_t)rv_imm_s(insn), 0u);
        return true;
    }

    case 0x63u: { /* BRANCH */
        static const uint8_t k_cond[8] = {
            EMU_IR_C_EQ, EMU_IR_C_NE, 0u,           0u,
            EMU_IR_C_LT, EMU_IR_C_GE, EMU_IR_C_LTU, EMU_IR_C_GEU};
        if (f3 == 2u || f3 == 3u) {
            return false; /* reserved */
        }
        const uint16_t x = emu_ir_get(b, rs1);
        const uint16_t y = emu_ir_get(b, rs2);

        /*
         * Retire before the exit, not after. A taken branch *does*
         * retire -- it is the trapping helper above that does not -- and
         * a RETIRE placed after EXIT_IF is skipped on exactly the taken
         * path, which undercounts every loop back edge in the guest.
         */
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, k_cond[f3], x, y,
                          pc + (uint32_t)rv_imm_b(insn), 0u);
        *counted = true;
        return true;
    }

    case 0x6Fu: /* JAL */
        /*
         * The link is the address after *this* instruction, which for an
         * expanded C.JAL is two bytes on, not four. Passed in rather
         * than assumed, because the expansion produces a 32-bit encoding
         * that looks four bytes long.
         */
        emu_ir_put(b, rd, emu_ir_const(b, pc + len));
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc + (uint32_t)rv_imm_j(insn), 0u);
        *ends_block = true;
        *counted = true;
        return true;

    case 0x67u: { /* JALR */
        if (f3 != 0u) {
            return false; /* reserved: the interpreter traps */
        }
        /*
         * The target is computed *before* the link register is written,
         * because `jalr ra, ra` -- an indirect call through a saved
         * pointer -- names the same register twice and would otherwise
         * jump to the return address it just wrote.
         */
        const uint16_t base = emu_ir_get(b, rs1);
        const uint16_t sum = emu_ir_alu(
            b, EMU_IR_ADD, base, emu_ir_const(b, (uint32_t)rv_imm_i(insn)));
        const uint16_t tgt =
            emu_ir_alu(b, EMU_IR_AND, sum, emu_ir_const(b, ~1u));

        emu_ir_put(b, rd, emu_ir_const(b, pc + len));
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT, EMU_IR_EXIT_JUMP, tgt, EMU_IR_NO_TEMP,
                          0u, 0u);
        *ends_block = true;
        *counted = true;
        return true;
    }

#if RV_EXT_F
    /*
     * The floating-point group.
     *
     * Everything here is gated on mstatus.FS being on *at translation*,
     * and that is only half a guard on its own -- a block outlives the
     * check. The other half is rv_jit_ctx_key, which carries FS
     * off-ness, so a block that assumed the unit was on is never entered
     * while it is off.
     *
     * The rounding mode is resolved here rather than left dynamic, for
     * the reason the IR states: a host without an encoding for one --
     * neither x86-64 nor ARM has ties-away -- must be able to decline,
     * and it cannot decline something it only discovers at run time.
     * frm is in the generation key for the same reason FS is.
     */
    case 0x07u: /* LOAD-FP  */
    case 0x27u: { /* STORE-FP */
        if (h_fs_off(cpu)) {
            return false;
        }
        /*
         * FLD, FSD, FLH and FSH: rv_hart_fp, which does the access the
         * interpreter does. These ended the block -- every double load
         * in a D guest was a block boundary and an interpreted
         * instruction -- and a helper call is a translation, declining
         * is not.
         */
        if (f3 != 2u) {
            return rv_ir_fp_fallback(b, pc, insn);
        }
        /*
         * **With D, FLEN is 64 and a float in an f register carries a
         * box.** These were sent to the helper for a while because
         * EMU_IR_FGET/FPUT moved 32 bits and could not say so: lowering
         * FLW wrote an unboxed register and lowering FSW read one, and
         * neither could tell. The whole point of the boxing is that the
         * *next* reader of that register sees a canonical NaN, so
         * nothing about the instruction's own result looks wrong; it
         * fails somewhere else. 93 of 378 tests, and F-fsub.s among
         * them.
         *
         * EMU_IR_FP_BOX is what lets them come back. Note the asymmetry,
         * which is the architecture's and not an oversight: FLW *writes*
         * a single and boxes it, FSW stores the low half **raw**. A
         * store is bits, and putting it through the unboxing read would
         * turn an unboxed register into a canonical NaN in memory.
         */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);

        if (op == 0x07u) {
            const uint16_t v =
                emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(4u, 0u), base,
                            EMU_IR_NO_TEMP, (uint32_t)rv_imm_i(insn), 0u);
            fput(b, RV_IR_BOX, v, rd);
        } else {
            const uint16_t v = emu_ir_emit(b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                           EMU_IR_NO_TEMP, rs2, 0u);
            (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u), base, v,
                              (uint32_t)rv_imm_s(insn), 0u);
        }
        return true;
    }

    case 0x43u: /* FMADD.S  */
    case 0x47u: /* FMSUB.S  */
    case 0x4Bu: /* FNMSUB.S */
    case 0x4Fu: { /* FNMADD.S */
        /*
         * The fused multiply-adds. The IR models these now -- it grew a
         * third operand for them and nothing else -- so a host with an
         * FMA instruction gets one, and a host without declines to the
         * helper that was the only path before.
         *
         * **RISC-V's "n" forms negate the product, not the addend**,
         * which reads backwards from the mnemonic:
         *
         *   FMADD    a*b + c
         *   FMSUB    a*b - c
         *   FNMSUB  -a*b + c
         *   FNMADD  -a*b - c
         *
         * Only the rounding mode has to be checked here: the block is
         * specialised on frm, which rv_jit_ctx_key already carries, and
         * RMM has no ARM equivalent so it stays on the helper.
         */
        if (h_fs_off(cpu)) {
            return false;
        }

        /*
         * The rounding mode, resolved here as everywhere else: f3 == 7
         * means "dynamic", which the IR resolves at *translation* so a
         * backend can decline a mode it lacks. The block is specialised
         * on frm and rv_jit_ctx_key carries that, which is what makes
         * resolving it here sound.
         */
        const uint32_t rm =
            (rv_funct3(insn) == 7u) ? RV_IR_FRM(cpu) : rv_funct3(insn);
        const uint32_t op5 = op >> 2;
        uint8_t fma_aux = (uint8_t)(rm & 7u);

        if (op5 == (0x4Bu >> 2) || op5 == (0x4Fu >> 2)) {
            fma_aux |= EMU_IR_FMA_NEG_MUL;
        }
        if (op5 == (0x47u >> 2) || op5 == (0x4Fu >> 2)) {
            fma_aux |= EMU_IR_FMA_NEG_ADD;
        }

        if (!emu_ir_can_lower(EMU_IR_FMA, fma_aux)) {
            return rv_ir_fp_fallback(b, pc, insn);
        }

        const uint16_t fa =
            emu_ir_emit(b, EMU_IR_FGET, EMU_IR_FP_BOX, EMU_IR_NO_TEMP,
                        EMU_IR_NO_TEMP, rv_rs1(insn), 0u);
        const uint16_t fb =
            emu_ir_emit(b, EMU_IR_FGET, EMU_IR_FP_BOX, EMU_IR_NO_TEMP,
                        EMU_IR_NO_TEMP, rv_rs2(insn), 0u);
        const uint16_t fc =
            emu_ir_emit(b, EMU_IR_FGET, EMU_IR_FP_BOX, EMU_IR_NO_TEMP,
                        EMU_IR_NO_TEMP, rv_rs3(insn), 0u);
        const uint16_t r = emu_ir_emit3(b, EMU_IR_FMA, fma_aux, fa, fb, fc);

        fput(b, EMU_IR_FP_BOX, r, rv_rd(insn));
        return true;
    }

    case 0x53u: { /* OP-FP */
        if (h_fs_off(cpu)) {
            return false;
        }
        const uint32_t f7 = insn >> 25;

        /*
         * Everything that rounds, classifies or reports a flag goes to
         * rv_hart_fp, which is Berkeley SoftFloat.
         *
         * SoftFloat *is* the FP unit here -- see the note in
         * CMakeLists.txt -- so lowering an FP operation to the host's
         * own instructions makes a second implementation of semantics
         * the core already owns. The two then disagree exactly where the
         * architecture is fussiest: NaN propagation, subnormals, and
         * which of fflags an operation may raise. Measured on the
         * official suite, rv32i/F: the interpreter passes 78/78 on
         * SoftFloat, and the same binary with --jit lowering natively
         * passed 55/78. Twenty-three tests, every one of them the host
         * FPU being asked to be a RISC-V FPU.
         *
         * This is the rule the fused multiply-adds above already follow,
         * and the one CLAUDE.md states for FMIN/FMAX and FCLASS on
         * Thumb-2: a helper call is a translation, declining is not, and
         * open-coding is a second copy of the semantics. The block stays
         * whole either way -- only the arithmetic moves.
         *
         * What is left here is what cannot round. FMV.X.W and FMV.W.X
         * move bits between the register files and involve no
         * arithmetic at all, so they stay lowered; FP loads and stores
         * are memory and are lowered in their own cases above.
         *
         * To make this faster, bring operations back one at a time, each
         * measured against the F suite -- not the whole table on the
         * argument that the host has an FPU. FMUL.S was the first and
         * FADD.S the second, and the reason either can be is that the
         * objection above was never about *arithmetic*: add, subtract,
         * multiply and divide are the four operations IEEE 754 specifies
         * exactly, so a compliant host computes the same bits SoftFloat
         * does. What differed was everything around them -- which NaN
         * comes out, and which fflags get raised -- and both are now the
         * backend's job: canonicalisation after the operation, MXCSR
         * framed across the block. The rest of the table stays on the
         * helper because the rest of the table is where hosts genuinely
         * disagree.
         */
        if (f7 == 0x70u && f3 == 0u && rs2 == 0u) { /* FMV.X.W */
            /*
             * No box on the read. FMV.X.W moves bits, and putting it
             * through the unboxing form would turn an unboxed register
             * into the canonical NaN -- "no interpretation" is the whole
             * instruction. rv_fpu.c says the same thing beside its own
             * copy of this.
             */
            emu_ir_put(b, rd,
                       emu_ir_emit(b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                   EMU_IR_NO_TEMP, rs1, 0u));
            return true;
        }
        if (f7 == 0x78u && f3 == 0u && rs2 == 0u) { /* FMV.W.X */
            fput(b, RV_IR_BOX, emu_ir_get(b, rs1), rd);
            return true;
        }
        /*
         * The single-precision arithmetic that is lowered so far. Bit 2
         * of funct7 is the operation and bits 1..0 are the format, so
         * `.D` is the same value with bit 0 set and falls through to the
         * helper -- there is no host lowering for double here, and the
         * IR is 32 bits wide besides.
         *
         * A table rather than a switch, because each entry was one line
         * *and one measurement*: they went in one at a time, each run
         * against the F suite and timed, in the order that put the
         * biggest first. FSQRT.S is the fifth and is not in the table
         * because it is unary -- the loop below reads rs2 as an operand,
         * and for FSQRT that field is an opcode extension that must be
         * zero. It is in its own case underneath.
         */
        static const struct {
            uint32_t f7;
            uint8_t op;
        } k_fp[] = {
            {0x00u, (uint8_t)EMU_IR_FADD},
            {0x04u, (uint8_t)EMU_IR_FSUB},
            {0x08u, (uint8_t)EMU_IR_FMUL},
            {0x0Cu, (uint8_t)EMU_IR_FDIV},
        };

        for (unsigned i = 0; i < sizeof k_fp / sizeof k_fp[0]; i++) {
            if (f7 != k_fp[i].f7) {
                continue;
            }
            /*
             * "dyn" is resolved here, at translation, which is the
             * deliberate arrangement: it lets a backend decline a mode
             * it has no encoding for -- RMM, which neither host has --
             * instead of silently rounding some other way. frm is in
             * rv_jit_ctx_key, so a block specialised on it is never entered
             * under another. Modes 5 and 6 are reserved and 7 is
             * only meaningful in the instruction, so anything that is
             * not one of the five real ones falls through to the helper,
             * which is where illegal-instruction is decided.
             */
            const uint32_t rm = (f3 == 7u) ? RV_IR_FRM(cpu) : f3;

            if (rm <= EMU_IR_FRM_RMM &&
                emu_ir_can_lower((emu_ir_op_t)k_fp[i].op, (uint8_t)rm)) {
                const uint16_t x =
                    emu_ir_emit(b, EMU_IR_FGET, RV_IR_BOX, EMU_IR_NO_TEMP,
                                EMU_IR_NO_TEMP, rs1, 0u);
                const uint16_t y =
                    emu_ir_emit(b, EMU_IR_FGET, RV_IR_BOX, EMU_IR_NO_TEMP,
                                EMU_IR_NO_TEMP, rs2, 0u);
                const uint16_t r = emu_ir_emit(b, (emu_ir_op_t)k_fp[i].op,
                                               (uint8_t)rm, x, y, 0u, 0u);

                fput(b, RV_IR_BOX, r, rd);
                return true;
            }
            break;
        }

        if (f7 == 0x2Cu && rs2 == 0u) { /* FSQRT.S */
            /*
             * The fifth exact operation, and the one whose rs2 field is
             * an opcode extension rather than an operand -- a non-zero
             * rs2 here is not FSQRT at all and goes to the helper, which
             * is where illegal-instruction is decided.
             */
            const uint32_t rm = (f3 == 7u) ? RV_IR_FRM(cpu) : f3;

            if (rm <= EMU_IR_FRM_RMM &&
                emu_ir_can_lower(EMU_IR_FSQRT, (uint8_t)rm)) {
                const uint16_t x =
                    emu_ir_emit(b, EMU_IR_FGET, RV_IR_BOX, EMU_IR_NO_TEMP,
                                EMU_IR_NO_TEMP, rs1, 0u);
                const uint16_t r = emu_ir_emit(b, EMU_IR_FSQRT, (uint8_t)rm, x,
                                               EMU_IR_NO_TEMP, 0u, 0u);

                fput(b, RV_IR_BOX, r, rd);
                return true;
            }
        }
        return rv_ir_fp_fallback(b, pc, insn);
    }
#endif /* RV_EXT_F */

#if RV_EXT_A
    case 0x2Fu:
        /*
         * Every atomic, to the helper. It keeps the block whole across a
         * lock, which is what this is for; the helper itself does the
         * decoding, the validity tests and the trap, so nothing about
         * atomics is written twice.
         *
         * pc first, because the helper may trap and rv_hart_trap records
         * the address it finds. Same shape as the FP fallback above.
         */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, emu_ir_const(b, insn),
                          EMU_IR_NO_TEMP, RV_IR_HELPER_AMO, 0u);
        return true;
#endif

    case 0x73u: { /* SYSTEM */
        /*
         * Almost all of it stays declined, and deliberately: the
         * translator refusing SYSTEM is what makes the interpreter
         * fallback the *single* place frm, mstatus.FS, the PMP
         * configuration and satp can change, which is why the
         * generation key only has to be re-derived there. Lowering a
         * CSR *write* would break that invariant, and the failure is
         * the staleness class this project has five separate entries
         * about.
         *
         * `time` is the exception, and it is worth one: a Linux boot
         * executes it **335,195,899 times** -- 99.2% of all CSR
         * accesses and half of every interpreted instruction -- because
         * the kernel polls it in delay loops. Each one declined a
         * block, so the loop around it paid a dispatch and an
         * interpreter entry per iteration.
         *
         * Narrowed to exactly what is safe:
         *
         *   - a *read*, so nothing in the hart changes. CSRRS/CSRRC
         *     with rs1 == x0 and the immediate forms with uimm == 0 are
         *     the encodings that do not write; CSRRW always writes.
         *   - `time` and `timeh` only. Not cycle or instret, which are
         *     maintained per retired instruction: a block accumulates
         *     its retirements and commits them at the end, so reading
         *     either mid-block would see a stale count. They are 0.8%
         *     of the traffic and all of the risk.
         */
        const uint32_t csr = insn >> 20;
        const bool reads_only = ((f3 == 2u || f3 == 3u) && rs1 == 0u) ||
                                ((f3 == 6u || f3 == 7u) && rs1 == 0u);

        if (!reads_only || (csr != 0xC01u && csr != 0xC81u)) {
            return false;
        }
        /* pc first: the helper can trap, and rv_hart_trap records what
         * it finds. Same shape as the FP and AMO fallbacks above. */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, emu_ir_const(b, insn),
                          EMU_IR_NO_TEMP, RV_IR_HELPER_CSR, 0u);
        return true;
    }

    default:
        return false;
    }
}

/*
 * Fetch one halfword the way the interpreter fetches it.
 *
 * **The translator reads guest memory at a virtual address, so under
 * paging it has to walk the page tables like everything else.** Reading
 * the bus directly compiles whatever physical memory happens to sit at
 * that number, which is not the guest's code and is not even reliably
 * memory.
 *
 * Fetch permission is checked here too, per halfword, which gets the
 * straddle rule for a 32-bit instruction crossing a page or a PMP
 * boundary for free: the two halves are translated and checked
 * independently, exactly as the interpreter does.
 *
 * A failure of any kind ends the block rather than raising anything.
 * That is not a shortcut -- the translator must never take a trap,
 * because the instruction may never execute. The interpreter picks the
 * address up and raises the architectural fault with the right cause and
 * the right tval.
 *
 * The TLB fill this may cause is the one side effect, and it is the same
 * fill the interpreter would do. Svade means the walk never *writes* A
 * or D -- it faults instead -- so translating ahead cannot dirty a page
 * the guest has not touched.
 */
static bool ir_fetch16(rv_hart_t *h, uint32_t va, uint16_t *out)
{
    uint32_t pa = va;

#if RV_HAVE_DTLB
    /* The interpreter's fetch table answers the walk and PMP for the
     * translator too. */
    if (rv_itlb_lookup(h, va, &pa)) {
        return emu_bus_fetch16(h->bus, pa, out) == EMU_FAULT_NONE;
    }
#endif

#if RV_EXT_SV32
    if (h->vm_active &&
        rv_mmu_translate(h, va, EMU_ACC_FETCH, &pa) != RV_EXC_NONE) {
        return false;
    }
#endif
#if RV_EXT_PMP
    if (h->pmp_active && !rv_pmp_check(h, pa, 2u, EMU_ACC_FETCH)) {
        return false;
    }
#endif
    if (emu_bus_fetch16(h->bus, pa, out) != EMU_FAULT_NONE) {
        return false;
    }
#if RV_HAVE_DTLB
    if (h->fetch_guard) {
        rv_itlb_fill(h, va, pa);
    }
#endif
    return true;
}

uint32_t rv_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emu_ir_reset(b);
#if RV_EXT_F
    g_fs_marked = false;
#endif
#if RV_EXT_ZVE32X
    g_vt = RV_IR_VT_UNKNOWN;
    g_vs_marked = false;
    g_vstart0 = false;
#endif
    b->start_pc = pc;
    b->has_fast = rv_ir_fast_mem(cpu, &b->fast);
    b->has_dtlb = !b->has_fast && rv_ir_dtlb(cpu, &b->dtlb);
    /* Under exactly the conditions the RAM window is, and once armed. */
    b->has_pt = b->has_fast && ((rv_hart_t *)cpu)->pt_armed &&
                emu_ir_pt_window(((rv_hart_t *)cpu)->bus, &b->pt);
    b->has_rom = b->has_fast && h->rom_armed &&
                 emu_ir_rom_window(h->bus, EMU_GUEST_ROM_BASE, &b->rom);

    while (count < RV_IR_MAX_BLOCK_INSNS && !b->overflow) {
        uint32_t insn;

        uint16_t lo, hi;
        uint32_t len;

        /*
         * Before a pc a block already begins at, or did: exit to it, which
         * chains, rather than translate a second copy of the code there
         * (emu_jit_entry_known).
         */
        if (count != 0u && emu_jit_entry_known(cur)) {
            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP,
                              EMU_IR_NO_TEMP, cur, 0u);
            break;
        }
        if (!ir_fetch16(h, cur, &lo)) {
            break;
        }
        if (rv_is_32bit(lo)) {
            if (!ir_fetch16(h, cur + 2u, &hi)) {
                break;
            }
            insn = (uint32_t)lo | ((uint32_t)hi << 16);
            len = 4u;
        } else {
            /*
             * Compressed encodings are expanded to their 32-bit
             * equivalent and lowered by the same code, which is what the
             * expander exists for. Skipping them instead is not a
             * correctness problem -- the interpreter picks them up --
             * but it is a coverage one: this guest is built with C, so
             * ending the block at every compressed instruction left 57%
             * of CoreMark interpreted against the direct translator's
             * 8%, and a backend that mostly falls back proves mostly
             * nothing.
             *
             * A zero expansion is a permanently-illegal encoding, so it
             * falls out through lower_one's default and ends the block.
             */
            insn = rv_decode_expand_c(lo);
            if (insn == 0u) {
                break;
            }
            len = 2u;
        }

        const uint32_t mark = b->count;
        bool ends = false;
        bool counted = false;

        if (!lower_one(cpu, b, insn, cur, len, &ends, &counted)) {
            /*
             * Discard whatever the attempt emitted: a lowering can emit
             * operands before reaching the funct7 that tells it to
             * decline, and a half-instruction left behind does not
             * fault -- it quietly computes something else.
             */
            b->count = mark;
            break;
        }

        cur += len;
        count++;
        if (!counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                              EMU_IR_NO_TEMP, 0u, 0u);
        }

        /*
         * pc after every instruction, so a trap, an interrupt or the
         * interpreter resuming sees the right address. A block whose pc
         * only became correct at the end would send all three to the
         * wrong place.
         */
        if (ends) {
            /*
             * The instruction wrote pc itself. Emitting the usual
             * pc-after-this-instruction store here would overwrite a
             * jump target with the fall-through address.
             */
            break;
        }
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          cur, 0u);
    }

    if (b->overflow) {
        return 0u;
    }
    b->guest_insns = count;
    return count;
}

/* ------------------------------------------------------------------ */
/* What the host's jit.c needs from this frontend                      */
/* ------------------------------------------------------------------ */

/*
 * What, when it changes, makes every existing block possibly wrong.
 *
 * The framework compares this on each block entry and flushes the cache
 * when it moves, so it has to change whenever something a block baked in
 * stops being true -- and must *not* change for anything else, because
 * every spurious change throws away the whole cache. That second half is
 * the one that went wrong: see rv_jit_ctx_key for the FP state that used
 * to be here.
 *
 * What is left is the mappings and the PMP configuration, both of which
 * really do invalidate: a page remapped under a block keyed on its
 * virtual address, and a region made no-execute after a block was built
 * in it. Without the second, isatest's `pmpx-exec-noeffect` reported
 * 0xBAD -- the store really did run inside a region that forbade
 * execution.
 *
 * Summed rather than given a field each: both are monotonic counters and
 * only their *changing* matters, so one word carries both and a change in
 * either is a change in the sum.
 */
static uint32_t rv_ir_gen_key(const rv_hart_t *h)
{
    return h->vm_gen + h->pmp_gen;
}

/*
 * Re-derived here rather than in `generation` itself, which is read on
 * every block entry -- CoreMark enters blocks 2.9 million times a run.
 *
 * Once per interpreted instruction is enough, and is exactly the right
 * place: frm and mstatus.FS move only through a CSR write, a SYSTEM
 * instruction this translator declines, so every such write lands on the
 * interpreter fallback. Nothing else alters either -- a trap does not
 * touch FS, and mret restores privilege rather than extension state.
 */
static void rv_jit_after_interp(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    h->jit_gen = rv_ir_gen_key(h);
    /*
     * Privilege moves on an xRET, which the translator declines, and on
     * a trap -- which is *not* always an instruction this fallback ran:
     * rv_ir_load raises one from inside a block and the interrupt hook
     * between blocks. So rv_hart_trap refreshes the context too, and
     * this covers everything else, frm and mstatus.FS included.
     */
    h->jit_ctx = rv_jit_ctx_key(h);
}

static void rv_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    /*
     * Arm the peripheral window, once, between slices: nothing is running
     * here, so flushing is safe, and every block translated from now on
     * inlines the window. Not from the helper that counted the access --
     * a block would be running -- and not per dispatch, which is the hot
     * path. A slice late is a few thousand instructions late.
     */
    if (EMU_UNLIKELY(!h->pt_armed) && h->bus != NULL &&
        h->bus->pt_accesses >= EMU_JIT_PT_ARM_AT) {
        h->pt_armed = true;
        emu_jit_flush();
    }
    /* And the image window, by the same rule: see EMU_JIT_ROM_ARM_AT. */
    if (EMU_UNLIKELY(!h->rom_armed) && h->rom_reads >= EMU_JIT_ROM_ARM_AT) {
        h->rom_armed = true;
        emu_jit_flush();
    }

    /*
     * Seeded here as well as maintained by after_interp, so the very
     * first block entry compares against the state it was translated
     * under rather than against zero.
     */
    h->jit_gen = rv_ir_gen_key(h);

    /*
     * The privilege a block was translated for, as part of its identity.
     *
     * Fetch permission depends on it -- a supervisor page is not
     * executable from U-mode -- and the translator checks that
     * permission once, at translation. Without this, a guest could
     * branch to a kernel address, find the block the kernel left there,
     * and run it instead of taking the fault the architecture requires:
     * the dispatch looks a block up by address and enters it, and the
     * emitted code re-checks nothing.
     *
     * In `context` rather than in `generation` because a privilege
     * change does not invalidate anything -- the kernel's blocks are
     * still the kernel's. Putting it in `generation` would be correct
     * and useless: every trap and every return would flush the cache,
     * which under Linux is thousands of times a second.
     */
    h->jit_ctx = rv_jit_ctx_key(h);

    out->pc = &h->pc;
    out->state = &h->state;
    /*
     * **Sdtrig alone, not the whole fetch guard.**
     *
     * `fetch_guard` folds Sdtrig, PMP and paging together for the
     * interpreter's benefit, where it is one branch on the hot fetch
     * path. Pointing `blocked` at it meant the JIT stopped translating
     * the moment any of the three was armed -- which for an operating
     * system is from the first page table onwards, so Linux ran 96.7%
     * interpreted and the JIT was 12% *slower* than no JIT at all.
     *
     * PMP and paging are handled now: ir_fetch16 walks and checks, and
     * the loads and stores already go through rv_ir_load/rv_ir_store,
     * which check exactly as the interpreter does. Sdtrig is not: an
     * execute trigger has to be evaluated per instruction against the
     * virtual address, and there is nowhere in a translated block to do
     * that.
     */
#if RV_EXT_SDTRIG
    out->blocked = &h->trig_active;
#else
    out->blocked = NULL;
#endif
    out->context = &h->jit_ctx;
    out->generation = &h->jit_gen;
#if RV_LAZY_IRQ_CHECK
    out->irq_pending = &h->irq_dirty;
    out->charged = &h->ir_traps;
#endif
}

static bool rv_jit_is_idle(emu_cpu_t *cpu)
{
    return ((rv_hart_t *)cpu)->state == EMU_STATE_WFI;
}

static bool rv_jit_wake(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    if (!rv_hart_wfi_wake(h)) {
        return false;
    }
    h->state = EMU_STATE_RUNNING;
#if RV_LAZY_IRQ_CHECK
    h->irq_dirty = true;
#endif
    return true;
}

static bool rv_jit_take_irq(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

#if RV_LAZY_IRQ_CHECK
    if (!h->irq_dirty) {
        return false;
    }
    h->irq_dirty = false;
#endif
    const rv_exc_t irq = rv_hart_pending_irq(h);
    if (irq == RV_EXC_NONE) {
        return false;
    }
    rv_hart_trap(h, RV_CAUSE_INTERRUPT | irq, 0u);
    return true;
}

/* Once a block, from emu_jit_run: in ITCM beside it (EMU_HOT_TEXT). */
EMU_HOT_TEXT static void rv_jit_count(emu_cpu_t *cpu, uint32_t n)
{
#if RV_EXT_ZICNTR
    rv_hart_t *const h = (rv_hart_t *)cpu;

    if ((h->mcountinhibit & 0x1u) == 0u) {
        h->mcycle += n;
    }
    if ((h->mcountinhibit & 0x4u) == 0u) {
        h->minstret += n;
    }
#else
    (void)cpu;
    (void)n;
#endif
}

const emu_ir_frontend_t rv_ir_frontend = {
    .name = "rv32",
    .translate = rv_ir_translate,
    .target = &rv_ir_target,
    .bind = rv_jit_bind,
    .interp = &rv_backend_interp,
    .is_idle = rv_jit_is_idle,
    .wake = rv_jit_wake,
    .take_irq = rv_jit_take_irq,
    .count = rv_jit_count,
    .after_interp = rv_jit_after_interp,
    .code_bytes = RV_JIT_HOST_CODE_BYTES,
    /* x[0..31] and pc; everything past it is bus pointers and counters. */
    .diff_state_bytes = (uint32_t)offsetof(rv_hart_t, pc) + 4u,
};
