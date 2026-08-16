/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_interp.c - Threaded RH850 G4MH interpreter.
 *
 * One instruction per loop iteration:
 *
 *   1. take a pending EI interrupt, if any
 *   2. fetch the first halfword and work out the length
 *   3. fetch the rest
 *   4. execute
 *
 * Dispatch is a switch on the 6-bit opcode in bits[10:5], which GCC lowers
 * to a jump table. Formats III and IV overlay that range with a 4-bit
 * opcode in bits[10:7], so those entries fan out a second time -- which is
 * cheaper than dispatching on four bits and re-testing six, because the
 * 16-bit ALU forms are the common case and they get the direct path.
 *
 * Exceptions unwind through the EXC() macro rather than by returning error
 * codes up a call chain: it restores pc to the faulting instruction (so
 * EIPC/FEPC are right), enters the handler, and continues the loop.
 *
 * Coverage: the 16-bit formats I-IV, the 32-bit formats V-VII, and the
 * Format X system group (LDSR, STSR, TRAP, RETI, HALT, DI, EI, the
 * register-form shifts, MUL/DIV, SETF and CMOV). Anything else raises
 * RIE, which is the correct report for an unimplemented encoding, and
 * adding one is a case in the switch below rather than a change anywhere
 * else. See the scope note in g4mh_cpu.h.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_intc.h"

/* ------------------------------------------------------------------ */
/* Register file                                                       */
/* ------------------------------------------------------------------ */

/*
 * r0 reads as zero and discards writes. Written unconditionally and then
 * cleared: one extra store against a mispredictable branch on every
 * register write, which is the same trade the RISC-V interpreter makes and
 * the same answer on an in-order M-class core.
 */
static EMU_ALWAYS_INLINE void wr(g4mh_cpu_t *c, uint32_t rd, uint32_t v)
{
    c->r[rd] = v;
    c->r[0] = 0u;
}

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

static EMU_ALWAYS_INLINE void set_zs(g4mh_cpu_t *c, uint32_t res)
{
    uint32_t psw = c->psw & ~(G4MH_PSW_Z | G4MH_PSW_S);
    if (res == 0u) {
        psw |= G4MH_PSW_Z;
    }
    if ((res & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_S;
    }
    c->psw = psw;
}

/* Logical operations define OV as 0 and leave CY alone. */
static EMU_ALWAYS_INLINE void set_logic(g4mh_cpu_t *c, uint32_t res)
{
    set_zs(c, res);
    c->psw &= ~G4MH_PSW_OV;
}

static EMU_ALWAYS_INLINE uint32_t do_add(g4mh_cpu_t *c, uint32_t a, uint32_t b)
{
    const uint32_t res = a + b;
    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

    if (res == 0u) {
        psw |= G4MH_PSW_Z;
    }
    if ((res & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_S;
    }
    /* Carry out of bit 31: the sum wrapped below either operand. */
    if (res < a) {
        psw |= G4MH_PSW_CY;
    }
    /* Signed overflow: both operands the same sign, result the other. */
    if ((~(a ^ b) & (a ^ res) & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_OV;
    }
    c->psw = psw;
    return res;
}

/* a - b, which is also what CMP evaluates without writing a result. */
static EMU_ALWAYS_INLINE uint32_t do_sub(g4mh_cpu_t *c, uint32_t a, uint32_t b)
{
    const uint32_t res = a - b;
    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

    if (res == 0u) {
        psw |= G4MH_PSW_Z;
    }
    if ((res & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_S;
    }
    /* CY is borrow on RH850: set when the subtraction underflowed. */
    if (a < b) {
        psw |= G4MH_PSW_CY;
    }
    if (((a ^ b) & (a ^ res) & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_OV;
    }
    c->psw = psw;
    return res;
}

/*
 * Saturating add and subtract. SAT is sticky -- it is only ever cleared by
 * writing PSW -- which is what lets a DSP loop test it once at the end
 * rather than after every operation.
 */
static EMU_ALWAYS_INLINE uint32_t do_satadd(g4mh_cpu_t *c, uint32_t a,
                                            uint32_t b)
{
    const uint32_t res = do_add(c, a, b);
    if ((c->psw & G4MH_PSW_OV) != 0u) {
        c->psw |= G4MH_PSW_SAT;
        return (a & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    }
    return res;
}

static EMU_ALWAYS_INLINE uint32_t do_satsub(g4mh_cpu_t *c, uint32_t a,
                                            uint32_t b)
{
    const uint32_t res = do_sub(c, a, b);
    if ((c->psw & G4MH_PSW_OV) != 0u) {
        c->psw |= G4MH_PSW_SAT;
        return (a & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* Data manipulation                                                   */
/* ------------------------------------------------------------------ */

/*
 * The swap group -- BSW, BSH, HSW, HSH -- all write reg3 and all set the
 * same four flags, but each on a *different* width of the result, which is
 * the whole point of them: they exist so an endian conversion can test in
 * one instruction whether the converted value contains a zero element.
 *
 *   BSW  a zero byte anywhere in the word     Z on the word
 *   BSH  a zero byte in the lower halfword    Z on the lower halfword
 *   HSW  a zero halfword anywhere in the word Z on the word
 *   HSH  the lower halfword is zero           Z on the lower halfword
 *
 * S is the word MSB throughout and OV is always 0. Getting the width wrong
 * gives the right register result with the wrong flags, which no test that
 * only checks reg3 would notice -- so the tests check PSW.
 */
static EMU_ALWAYS_INLINE void set_swap_flags(g4mh_cpu_t *c, uint32_t res,
                                             bool cy, bool zero)
{
    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

    if (cy) {
        psw |= G4MH_PSW_CY;
    }
    if (zero) {
        psw |= G4MH_PSW_Z;
    }
    if ((res & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_S;
    }
    c->psw = psw;
}

/* True if any byte of `v` is zero, without a loop or a branch. */
static EMU_ALWAYS_INLINE bool has_zero_byte(uint32_t v)
{
    return ((v - 0x01010101u) & ~v & 0x80808080u) != 0u;
}

/*
 * Bit search: SCH0L/SCH1L scan from the MSB, SCH0R/SCH1R from the LSB.
 *
 * The result is a *one-based* distance to the first matching bit, counted
 * from the end the search started at, and 0 when there is no match -- so
 * "found at the first bit examined" is 1, not 0, and the not-found case is
 * distinguishable from it. Z reports not-found. CY reports that the match
 * was at the far end, i.e. that the search examined all 32 bits, which is
 * how the manual's "if the bit found is the LSB" (searching from the MSB)
 * and "is the MSB" (searching from the LSB) both read.
 *
 * S is defined as 0 here, not as the result's MSB: the result never
 * exceeds 32.
 */
static EMU_ALWAYS_INLINE void do_sch(g4mh_cpu_t *c, uint32_t rd, uint32_t v,
                                     bool want_one, bool from_msb)
{
    /* Searching for a zero is searching for a one in the complement. */
    uint32_t bits = want_one ? v : ~v;
    uint32_t n = 0u;

    if (bits != 0u) {
        n = from_msb ? (uint32_t)__builtin_clz(bits) + 1u
                     : (uint32_t)__builtin_ctz(bits) + 1u;
    }

    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;
    if (n == 0u) {
        psw |= G4MH_PSW_Z;
    } else if (n == 32u) {
        psw |= G4MH_PSW_CY;
    }
    c->psw = psw;
    wr(c, rd, n);
}

/* ------------------------------------------------------------------ */
/* Bit manipulation on memory                                          */
/* ------------------------------------------------------------------ */

enum { BITOP_SET, BITOP_NOT, BITOP_CLR, BITOP_TST };

/*
 * SET1 / NOT1 / CLR1 / TST1, which differ only in what they write back.
 *
 * Shared between the two encodings -- Format VIII takes the bit number
 * from the opcode and the address from a disp16, Format IX takes both
 * from registers -- because the semantics below are the whole
 * instruction and having them in one place is what keeps the two forms
 * from drifting.
 *
 * Three things here are easy to get wrong and none of them shows up as a
 * wrong register value:
 *
 *   - Z is the *old* value of the bit, complemented, and is set before
 *     the modification. Computing it from the result makes SET1 always
 *     clear Z and CLR1 always set it.
 *   - only Z moves. CY, OV and S keep whatever the previous instruction
 *     left, which is unlike almost everything else in this file.
 *   - the access is a byte, and it is a read-modify-write even for NOT1.
 *     TST1 alone does not store, which also means it alone cannot raise
 *     a store-side fault.
 */
static g4mh_exc_t do_bitop(g4mh_cpu_t *c, unsigned op, uint32_t adr,
                           uint32_t bit)
{
    const uint32_t mask = 1u << (bit & 7u);
    uint32_t token;

    g4mh_exc_t e = g4mh_load(c, adr, 1u, false, &token);
    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
        return e;
    }

    if ((token & mask) != 0u) {
        c->psw &= ~G4MH_PSW_Z;
    } else {
        c->psw |= G4MH_PSW_Z;
    }

    if (op == BITOP_TST) {
        return G4MH_EXC_NONE;
    }

    uint32_t out = token;
    if (op == BITOP_SET) {
        out |= mask;
    } else if (op == BITOP_CLR) {
        out &= ~mask;
    } else {
        out ^= mask;
    }
    return g4mh_store(c, adr, 1u, out);
}

/*
 * Shifts. A shift of zero leaves the operand alone and clears CY, which is
 * why the count is tested rather than fed straight to the C operator --
 * shifting a uint32_t by 32 is undefined, and the architecture defines
 * only the low five bits of the count as significant.
 */
static EMU_ALWAYS_INLINE uint32_t do_shl(g4mh_cpu_t *c, uint32_t v, uint32_t n)
{
    n &= 31u;
    uint32_t res = v;
    uint32_t cy = 0u;
    if (n != 0u) {
        cy = (v >> (32u - n)) & 1u;
        res = v << n;
    }
    set_zs(c, res);
    c->psw = (c->psw & ~(G4MH_PSW_OV | G4MH_PSW_CY)) |
             (cy ? G4MH_PSW_CY : 0u);
    return res;
}

static EMU_ALWAYS_INLINE uint32_t do_shr(g4mh_cpu_t *c, uint32_t v, uint32_t n)
{
    n &= 31u;
    uint32_t res = v;
    uint32_t cy = 0u;
    if (n != 0u) {
        cy = (v >> (n - 1u)) & 1u;
        res = v >> n;
    }
    set_zs(c, res);
    c->psw = (c->psw & ~(G4MH_PSW_OV | G4MH_PSW_CY)) |
             (cy ? G4MH_PSW_CY : 0u);
    return res;
}

/*
 * CLIP.B/.BU/.H/.HU -- narrow a word to a byte or halfword, saturating.
 *
 * The signed and unsigned forms differ in how they read the *source*, not
 * only in where they clamp: CLIP.B treats reg1 as signed word data and
 * clamps to [-128, 127], while CLIP.BU "regards the word data in reg1 as
 * unsigned" and clamps to [0, 255]. So 0xFFFFFFFF gives -1 through CLIP.B
 * and 255 through CLIP.BU -- not the same number narrowed two ways.
 *
 * S is the result's sign for the signed forms and a hard zero for the
 * unsigned ones. SAT is sticky: set when OV is, never cleared here, and
 * only an LDSR to PSW puts it back.
 */
static EMU_ALWAYS_INLINE uint32_t do_clip(g4mh_cpu_t *c, uint32_t v,
                                          bool sgn, unsigned bits)
{
    uint32_t res;
    bool ov;

    if (sgn) {
        const int32_t lo = -(int32_t)(1u << (bits - 1u));
        const int32_t hi = (int32_t)((1u << (bits - 1u)) - 1u);
        const int32_t a = (int32_t)v;

        res = (uint32_t)((a < lo) ? lo : ((a > hi) ? hi : a));
        ov = (a < lo) || (a > hi);
    } else {
        const uint32_t hi = (1u << bits) - 1u;

        res = (v > hi) ? hi : v;
        ov = v > hi;
    }

    uint32_t psw = c->psw & ~(G4MH_PSW_Z | G4MH_PSW_S | G4MH_PSW_OV |
                              G4MH_PSW_CY);
    if (res == 0u)                       { psw |= G4MH_PSW_Z; }
    if (sgn && ((int32_t)res < 0))       { psw |= G4MH_PSW_S; }
    if (ov)                              { psw |= G4MH_PSW_OV | G4MH_PSW_SAT; }
    c->psw = psw;
    return res;
}

static EMU_ALWAYS_INLINE uint32_t do_sar(g4mh_cpu_t *c, uint32_t v, uint32_t n)
{
    n &= 31u;
    uint32_t res = v;
    uint32_t cy = 0u;
    if (n != 0u) {
        cy = (v >> (n - 1u)) & 1u;
        res = (uint32_t)((int32_t)v >> n);
    }
    set_zs(c, res);
    c->psw = (c->psw & ~(G4MH_PSW_OV | G4MH_PSW_CY)) |
             (cy ? G4MH_PSW_CY : 0u);
    return res;
}

/* ------------------------------------------------------------------ */
/* list12, for PREPARE and DISPOSE                                     */
/* ------------------------------------------------------------------ */

/*
 * Which bit of the 32-bit instruction word names each of r20-r31.
 *
 * The architecture packs twelve register bits into space an encoding had
 * left over, so the mapping is neither monotonic nor contiguous: eleven
 * are bits 31 down to 21 in an order that puts r24-r27 ahead of r20-r23,
 * and r30 is bit 0 of the *first* halfword, four bits away from the
 * rest. No arithmetic produces this, which is why it is a table -- and
 * why deriving it from the register number, the obvious thing to try,
 * silently saves the wrong registers.
 */
static const uint8_t k_list12_bit[12] = {
    27u, 26u, 25u, 24u,      /* r20 r21 r22 r23 */
    31u, 30u, 29u, 28u,      /* r24 r25 r26 r27 */
    23u, 22u,  0u, 21u       /* r28 r29 r30 r31 */
};

static EMU_ALWAYS_INLINE bool list12_has(uint32_t list, unsigned reg)
{
    return (list & (1u << k_list12_bit[reg - 20u])) != 0u;
}

/*
 * PREPARE's register save. Ascending register order, each one four bytes
 * below the last, so r20 lands highest and r31 lowest -- and DISPOSE
 * therefore walks *descending* to undo it. The manual states the two
 * orders in separate places and they are not the same; reading one and
 * assuming the other restores every register into its neighbour, which
 * is a wrong answer rather than a fault.
 */
static g4mh_exc_t do_prepare_save(g4mh_cpu_t *c, uint32_t list,
                                  uint32_t *sp_out)
{
    uint32_t tmp = c->r[3];

    for (unsigned reg = 20u; reg <= 31u; reg++) {
        if (!list12_has(list, reg)) {
            continue;
        }
        tmp -= 4u;
        const g4mh_exc_t e = g4mh_store(c, tmp & ~3u, 4u, c->r[reg]);
        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
            return e;
        }
    }
    *sp_out = tmp;
    return G4MH_EXC_NONE;
}

static g4mh_exc_t do_dispose_load(g4mh_cpu_t *c, uint32_t list, uint32_t imm5,
                                  uint32_t *sp_out)
{
    uint32_t tmp = c->r[3] + (imm5 << 2);

    for (unsigned reg = 32u; reg-- > 20u;) {
        if (!list12_has(list, reg)) {
            continue;
        }
        uint32_t v;
        const g4mh_exc_t e = g4mh_load(c, tmp & ~3u, 4u, false, &v);
        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
            return e;
        }
        c->r[reg] = v;
        tmp += 4u;
    }
    *sp_out = tmp;
    return G4MH_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

/*
 * Take an exception and continue. pc is restored to the faulting
 * instruction first, because that -- not the next one -- is what
 * EIPC/FEPC must hold for a handler that fixes the fault and returns.
 */
#define EXC(cause)                                        \
    do {                                                  \
        c->pc = pc;                                       \
        g4mh_cpu_exception(c, (cause), pc);               \
        pc = c->pc;                                       \
        goto next_insn;                                   \
    } while (0)

/* An exception whose return address is the *following* instruction:
 * TRAP and SYSCALL, which are meant to resume after the call. */
#define EXC_AFTER(cause)                                  \
    do {                                                  \
        c->pc = pc;                                       \
        g4mh_cpu_exception(c, (cause), next);             \
        pc = c->pc;                                       \
        goto next_insn;                                   \
    } while (0)

static emu_run_reason_t interp_run(g4mh_cpu_t *c, uint32_t budget,
                                   uint32_t *retired)
{
    emu_run_reason_t reason = EMU_RUN_BUDGET;
    uint32_t pc = c->pc;
    uint32_t done = 0u;

    if (EMU_UNLIKELY(c->state == EMU_STATE_HALTED)) {
        if (retired != NULL) {
            *retired = 0u;
        }
        return EMU_RUN_HALTED;
    }

    while (done < budget) {
        /*
         * A halt can arrive from outside the switch -- the syscall hook
         * calls it to implement exit() -- so it is tested here rather than
         * only where HALT is executed. Before the interrupt check, because
         * a halted core does not take interrupts.
         */
        if (EMU_UNLIKELY(c->state == EMU_STATE_HALTED)) {
            reason = EMU_RUN_HALTED;
            break;
        }

        /* --- interrupts ------------------------------------------- */
        if (EMU_UNLIKELY(c->irq_dirty)) {
            /*
             * Cleared before the evaluation, not after: a host ISR that
             * raises a channel while we are deciding would otherwise have
             * its set overwritten and the interrupt would go unnoticed
             * until something else happened to dirty the flag again.
             */
            c->irq_dirty = false;

            /*
             * FE level first: it outranks every EI interrupt and PSW.ID
             * does not reach it, so a guest running with interrupts
             * masked still takes this one. Only the TPTM raises it, and
             * only when TPTMSEL has not routed it to EIINT31.
             */
            if (EMU_UNLIKELY(g4mh_cpu_pending_fe(c))) {
                c->state = EMU_STATE_RUNNING;
                c->pc = pc;
                g4mh_intc_ack_fe(c->intc, c->coreid);
                g4mh_cpu_exception(c, G4MH_EXC_FEINT, pc);
                pc = c->pc;
                continue;
            }

            const int ch = g4mh_cpu_pending_irq(c);
            if (ch >= 0) {
                c->state = EMU_STATE_RUNNING;
                c->pc = pc;
                g4mh_intc_ack(c->intc, (uint32_t)ch);
                g4mh_cpu_exception(c,
                                   G4MH_EXC_EIINT_BASE + (uint32_t)ch, pc);
                pc = c->pc;
                continue;
            }
        }

        if (EMU_UNLIKELY(c->state == EMU_STATE_WFI)) {
            reason = EMU_RUN_WFI;
            break;
        }

        /* --- fetch ------------------------------------------------- */
        uint16_t w0;
        emu_fault_t f = emu_bus_fetch16(c->bus, pc, &w0);
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            EXC(g4mh_exc_from_fault(f));
        }

        unsigned len = g4mh_insn_len(w0);
        uint32_t w1 = 0u;
        uint32_t w2 = 0u;
        uint32_t w3 = 0u;
        if (len >= 4u) {
            uint16_t h;
            f = emu_bus_fetch16(c->bus, pc + 2u, &h);
            if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                EXC(g4mh_exc_from_fault(f));
            }
            w1 = h;
            /* Only now can a 48-bit form be told from a 32-bit one. */
            if (g4mh_insn_is_48(w0, w1)) {
                len = 6u;
                f = emu_bus_fetch16(c->bus, pc + 4u, &h);
                if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                    EXC(g4mh_exc_from_fault(f));
                }
                w2 = h;
                /* And only PREPARE's imm32 form goes further. */
                if (EMU_UNLIKELY(g4mh_insn_is_64(w0, w1))) {
                    len = 8u;
                    f = emu_bus_fetch16(c->bus, pc + 6u, &h);
                    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                        EXC(g4mh_exc_from_fault(f));
                    }
                    w3 = h;
                }
            }
        }

        const uint32_t next = pc + len;

#if EMU_ENABLE_TRACE
        if (c->trace != NULL) {
            c->trace((emu_cpu_t *)c, pc,
                     (uint32_t)w0 | (w1 << 16), c->trace_user);
        }
#endif

        const uint32_t r1 = g4mh_reg1(w0);
        const uint32_t r2 = g4mh_reg2(w0);
        const uint32_t op = g4mh_op6(w0);

        switch (op) {
        /* ---------------- Format I: reg-reg, 16-bit ---------------- */
        case 0x00:
            /*
             * MOV reg1, reg2, and -- with reg2 == 0, where a move would
             * write r0 and be discarded -- the barriers and NOP:
             *
             *   reg1 = 0        NOP
             *   reg1 = 28..31   SYNCI, SYNCE, SYNCM, SYNCP
             *
             * The barriers are no-ops in this model and would be even
             * without this case, since they decode as a discarded move.
             * They are named anyway: a barrier that silently means nothing
             * because of an encoding accident is not the same as one that
             * means nothing because the execution model gives the guest a
             * total order, and only the second stays true if this ever
             * grows threads. See docs/host/g4mh/multicore.md.
             */
            if (r2 == 0u) {
                if (r1 != 0u && (r1 < 28u)) {
                    EXC(G4MH_EXC_RIE);
                }
                break;      /* NOP, or a barrier with nothing to order */
            }
            wr(c, r2, c->r[r1]);
            break;

        case 0x01:                                  /* NOT              */
            set_logic(c, ~c->r[r1]);
            wr(c, r2, ~c->r[r1]);
            break;

        case 0x02:                                  /* DIVH / SWITCH / RIE */
            if (r2 == 0u && r1 == 0u) {
                /* The architectural RIE encoding: 0x0040, a deliberate
                 * "raise reserved instruction" rather than a hole. */
                EXC(G4MH_EXC_RIE);
            }
            if (r1 == 0u) {
                /*
                 * FETRAP vector4, which is 0vvvv00001000000 -- reg1 zero
                 * and reg2 the vector. It shares this opcode with DIVH,
                 * whose own encoding forbids r0 in *either* field for
                 * exactly this reason, and the split was not being made:
                 * every FETRAP ran as a DIVH by r0, which divides by zero,
                 * sets OV and retires. No trap, no diagnostic.
                 *
                 * FE level, so it saves to FEPC/FEPSW and ignores PSW.ID;
                 * the return pc is the *next* instruction.
                 */
                c->pc = pc;
                g4mh_cpu_exception(c, G4MH_EXC_FETRAP + r2, next);
                pc = c->pc;
                goto retired_insn;
            }
            if (r2 == 0u) {
                /*
                 * SWITCH: a jump-table dispatch. The table of 16-bit
                 * signed halfword displacements follows the instruction,
                 * and the entry is doubled and added to that address.
                 */
                uint32_t ent;
                const uint32_t tbl = next + c->r[r1] * 2u;
                const g4mh_exc_t e = g4mh_load(c, tbl, 2u, true, &ent);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
                    EXC(e);
                }
                pc = next + (ent * 2u);
                goto retired_insn;
            } else {
                /* DIVH: 32-by-16 signed divide, quotient to reg2. */
                const int32_t div = (int32_t)emu_sext(c->r[r1] & 0xFFFFu, 16);
                if (div == 0) {
                    /* Division by zero leaves the dividend and sets OV,
                     * rather than trapping. */
                    c->psw |= G4MH_PSW_OV;
                    break;
                }
                const int32_t a = (int32_t)c->r[r2];
                if (a == INT32_MIN && div == -1) {
                    c->psw |= G4MH_PSW_OV;
                    break;
                }
                const int32_t q = a / div;
                set_zs(c, (uint32_t)q);
                c->psw &= ~G4MH_PSW_OV;
                wr(c, r2, (uint32_t)q);
            }
            break;

        case 0x03: {                                /* JMP / SLD.BU/.HU */
            if (r2 != 0u) {
                /*
                 * The unsigned short loads. Their opcode is seven bits
                 * where this switch dispatches on six, so bit 4 -- which
                 * for JMP is part of reg1 -- picks the width, and only
                 * reg2 separates the two groups at all.
                 *
                 * The sign-extending SLD.B/.H are a different opcode
                 * entirely, in the Format IV overlay at 0x18..0x2F.
                 * These are not a variant of those.
                 */
                uint32_t v;
                const bool half = (w0 & 0x10u) != 0u;
                const uint32_t disp = half ? ((uint32_t)(w0 & 0xFu) << 1)
                                           : (uint32_t)(w0 & 0xFu);
                const g4mh_exc_t e = g4mh_load(c, c->r[30] + disp,
                                               half ? 2u : 1u, false, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r2, v);
                break;
            }
            /* Bit 0 of the target is ignored: instructions are halfword
             * aligned and the architecture defines the low bit as zero
             * rather than as a mode bit. */
            pc = c->r[r1] & ~1u;
            goto retired_insn;
        }

        case 0x04:                                  /* SATSUBR / ZXB    */
            if (r2 == 0u) {
                wr(c, r1, c->r[r1] & 0xFFu);
            } else {
                /* Reversed operands: reg2 = reg2 - reg1 is SATSUB; this
                 * is reg2 = reg1 - reg2. */
                wr(c, r2, do_satsub(c, c->r[r1], c->r[r2]));
            }
            break;

        case 0x05:                                  /* SATSUB / SXB     */
            if (r2 == 0u) {
                wr(c, r1, (uint32_t)emu_sext(c->r[r1], 8));
            } else {
                wr(c, r2, do_satsub(c, c->r[r2], c->r[r1]));
            }
            break;

        case 0x06:                                  /* SATADD / ZXH     */
            if (r2 == 0u) {
                wr(c, r1, c->r[r1] & 0xFFFFu);
            } else {
                wr(c, r2, do_satadd(c, c->r[r2], c->r[r1]));
            }
            break;

        case 0x07:                                  /* MULH / SXH       */
            if (r2 == 0u) {
                wr(c, r1, (uint32_t)emu_sext(c->r[r1], 16));
            } else {
                /* Signed 16x16 into 32, taking the low halves. MULH does
                 * not affect the flags. */
                const int32_t a = emu_sext(c->r[r2] & 0xFFFFu, 16);
                const int32_t b = emu_sext(c->r[r1] & 0xFFFFu, 16);
                wr(c, r2, (uint32_t)(a * b));
            }
            break;

        case 0x08: {                                /* OR               */
            const uint32_t v = c->r[r2] | c->r[r1];
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x09: {                                /* XOR              */
            const uint32_t v = c->r[r2] ^ c->r[r1];
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x0A: {                                /* AND              */
            const uint32_t v = c->r[r2] & c->r[r1];
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x0B:                                  /* TST              */
            set_logic(c, c->r[r2] & c->r[r1]);
            break;

        case 0x0C:                                  /* SUBR             */
            wr(c, r2, do_sub(c, c->r[r1], c->r[r2]));
            break;
        case 0x0D:                                  /* SUB              */
            wr(c, r2, do_sub(c, c->r[r2], c->r[r1]));
            break;
        case 0x0E:                                  /* ADD              */
            wr(c, r2, do_add(c, c->r[r2], c->r[r1]));
            break;
        case 0x0F:                                  /* CMP              */
            (void)do_sub(c, c->r[r2], c->r[r1]);
            break;

        /* ---------------- Format II: imm5-reg, 16-bit -------------- */
        /*
         * CALLT straddles both of the next two slots, because its imm6
         * is one bit wider than the field this switch dispatches on: the
         * opcode's low bit is imm6[5]. So it hides in MOV imm5 for a low
         * vector and in SATADD imm5 for a high one, and only reg2 == 0
         * tells it from either.
         *
         * SATADD did not test reg2 at all, so half of every CALLT
         * retired as a saturating add into r0 -- discarded, with the
         * call never made. That is the failure this architecture keeps
         * offering: a register field reused as an opcode extension does
         * not announce itself.
         */
        case 0x10:                                  /* MOV imm5 / CALLT  */
        case 0x11:                                  /* SATADD imm5/CALLT */
            if (r2 == 0u) {
                const uint32_t ctbp = c->sr[0][G4MH_SR_CTBP];
                uint32_t ent;
                const g4mh_exc_t e =
                    g4mh_load(c, ctbp + ((uint32_t)(w0 & 0x3Fu) << 1), 2u,
                              false, &ent);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                /* CTPSW keeps PSW's low five bits, not the whole word. */
                c->sr[0][G4MH_SR_CTPC]  = next;
                c->sr[0][G4MH_SR_CTPSW] = c->psw & 0x1Fu;
                pc = ctbp + ent;
                goto retired_insn;
            }
            if (op == 0x10u) {
                wr(c, r2, (uint32_t)g4mh_imm5(w0));
            } else {
                wr(c, r2, do_satadd(c, c->r[r2], (uint32_t)g4mh_imm5(w0)));
            }
            break;
        case 0x12:                                  /* ADD imm5         */
            wr(c, r2, do_add(c, c->r[r2], (uint32_t)g4mh_imm5(w0)));
            break;
        case 0x13:                                  /* CMP imm5         */
            (void)do_sub(c, c->r[r2], (uint32_t)g4mh_imm5(w0));
            break;
        /* The shift forms take an unsigned count, not a sign-extended
         * immediate: shifting by "-1" is not a thing the encoding means. */
        case 0x14:                                  /* SHR imm5         */
            wr(c, r2, do_shr(c, c->r[r2], w0 & 0x1Fu));
            break;
        case 0x15:                                  /* SAR imm5         */
            wr(c, r2, do_sar(c, c->r[r2], w0 & 0x1Fu));
            break;
        case 0x16:                                  /* SHL imm5         */
            wr(c, r2, do_shl(c, c->r[r2], w0 & 0x1Fu));
            break;
        case 0x17: {                                /* MULH imm5 / JR   */
            if (r2 == 0u) {
                /*
                 * JR and JARL disp32, one encoding: reg1 names the link
                 * register and zero makes it JR, exactly as reg2 does
                 * for the disp22 pair, so the write happens either way
                 * and lands in r0 when it is not wanted.
                 *
                 * Unlike disp22 the displacement's high half is in the
                 * *third* halfword. The 48-bit forms append their extra
                 * word; disp22 put its high bits first. Assuming one
                 * layout from the other gives plausible small jumps and
                 * garbage for everything else.
                 */
                wr(c, r1, next);
                pc = pc + ((w2 << 16) | (w1 & 0xFFFEu));
                goto retired_insn;
            }
            const int32_t a = emu_sext(c->r[r2] & 0xFFFFu, 16);
            wr(c, r2, (uint32_t)(a * g4mh_imm5(w0)));
            break;
        }

        /* ------- Formats III and IV: the 0x18..0x2F overlay --------- */
        /*
         * These share the 6-bit opcode range but are really distinguished
         * by bits[10:7]. Falling into one arm of the switch and testing
         * four bits again is one compare on a path the 16-bit ALU forms
         * above never take.
         */
        default: {
            const uint32_t op4 = g4mh_op4(w0);

            if (op4 == 0x0Bu) {                     /* Bcond disp9      */
                /*
                 * disp[8:4] in bits[15:11], disp[3:1] in bits[6:4], and
                 * bit 0 is always zero. Sign-extended from 9 bits and
                 * added to the address of the *branch*, not of the next
                 * instruction.
                 */
                const uint32_t d = (((uint32_t)w0 >> 11) & 0x1Fu) << 4 |
                                   (((uint32_t)w0 >> 4) & 0x7u) << 1;
                if (g4mh_cond(w0 & 0xFu, c->psw)) {
                    pc = pc + (uint32_t)emu_sext(d, 9);
                    goto retired_insn;
                }
                break;
            }

            /*
             * Format IV: the short load/store forms, addressed through EP
             * with an unsigned displacement. What makes them worth an
             * encoding of their own is that a compiler parks the frame or
             * a hot structure in EP and then reaches its fields in two
             * bytes rather than four.
             */
            uint32_t addr;
            uint32_t v;
            g4mh_exc_t e;

            switch (op4) {
            case 0x06:                              /* SLD.B disp7      */
                addr = c->r[G4MH_REG_EP] + (w0 & 0x7Fu);
                e = g4mh_load(c, addr, 1u, true, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r2, v);
                break;

            case 0x07:                              /* SST.B disp7      */
                addr = c->r[G4MH_REG_EP] + (w0 & 0x7Fu);
                e = g4mh_store(c, addr, 1u, c->r[r2]);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                break;

            case 0x08:                              /* SLD.H disp8      */
                addr = c->r[G4MH_REG_EP] + ((w0 & 0x7Fu) << 1);
                e = g4mh_load(c, addr, 2u, true, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r2, v);
                break;

            case 0x09:                              /* SST.H disp8      */
                addr = c->r[G4MH_REG_EP] + ((w0 & 0x7Fu) << 1);
                e = g4mh_store(c, addr, 2u, c->r[r2]);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                break;

            case 0x0A:
                /*
                 * SLD.W and SST.W share this opcode and are told apart by
                 * bit 0, which the displacement cannot use: a word access
                 * is 4-byte aligned, so disp[1:0] are always zero and the
                 * encoding reclaims bit 0 as the direction.
                 */
                addr = c->r[G4MH_REG_EP] + ((w0 & 0x7Eu) << 1);
                if ((w0 & 1u) == 0u) {              /* SLD.W            */
                    e = g4mh_load(c, addr, 4u, false, &v);
                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                    wr(c, r2, v);
                } else {                            /* SST.W            */
                    e = g4mh_store(c, addr, 4u, c->r[r2]);
                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                }
                break;

            default:
                EXC(G4MH_EXC_RIE);
            }
            break;
        }

        /* ---------------- Format VI: imm16 ALU, 32-bit ------------- */
        case 0x30:                                  /* ADDI imm16       */
            wr(c, r2, do_add(c, c->r[r1], (uint32_t)emu_sext(w1, 16)));
            break;
        case 0x31:
            if (r2 == 0u) {                         /* MOV imm32, reg1  */
                /* The only 48-bit form implemented; see g4mh_insn_len. */
                wr(c, r1, w1 | (w2 << 16));
            } else {                                /* MOVEA imm16      */
                /* Address arithmetic: no flags, which is what makes it a
                 * separate instruction from ADDI. */
                wr(c, r2, c->r[r1] + (uint32_t)emu_sext(w1, 16));
            }
            break;
        case 0x32:                                  /* MOVHI / DISPOSE  */
        case 0x33:                                  /* SATSUBI / DISPOSE*/
            if (r2 == 0u) {
                /*
                 * DISPOSE straddles both slots for the same reason
                 * CALLT straddles two: its imm5 is five bits where this
                 * switch dispatches on six, so the opcode's low bit is
                 * imm5[4].
                 *
                 * reg1 doubles as the return target. Zero means "just
                 * pop"; anything else means pop and jump, which is how a
                 * leaf epilogue and a tail return share one encoding.
                 * The pc write has to come *after* the loads, because
                 * one of them may restore the register it reads.
                 */
                const uint32_t list = ((uint32_t)w1 << 16) | w0;
                const uint32_t imm5 = (w0 >> 1) & 0x1Fu;
                const uint32_t rt = w1 & 0x1Fu;
                uint32_t sp;
                const g4mh_exc_t e = do_dispose_load(c, list, imm5, &sp);

                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                const uint32_t target = c->r[rt] & ~1u;
                c->r[3] = sp;
                if (rt != 0u) {
                    pc = target;
                    goto retired_insn;
                }
                break;
            }
            if (op == 0x32u) {
                wr(c, r2, c->r[r1] + (w1 << 16));
            } else {
                wr(c, r2, do_satsub(c, c->r[r1], (uint32_t)emu_sext(w1, 16)));
            }
            break;
        case 0x34: {                                /* ORI imm16        */
            /* The logical forms zero-extend, where the arithmetic ones
             * sign-extend. */
            const uint32_t v = c->r[r1] | w1;
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x35: {                                /* XORI imm16       */
            const uint32_t v = c->r[r1] ^ w1;
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x36: {                                /* ANDI imm16       */
            const uint32_t v = c->r[r1] & w1;
            set_logic(c, v);
            wr(c, r2, v);
            break;
        }
        case 0x37: {                                /* MULHI / JMP/LOOP */
            if (r2 == 0u) {
                /*
                 * Two instructions of different *lengths* share this
                 * slot, separated only by bit 0 of the second halfword:
                 * clear is the 48-bit JMP disp32[reg1], set is the
                 * 32-bit LOOP. g4mh_insn_is_48 makes the same test, and
                 * the two have to agree or the pc moves by the wrong
                 * amount.
                 */
                if ((w1 & 1u) != 0u) {
                    /*
                     * LOOP: decrement and branch *backwards* if the
                     * result is non-zero. The displacement is unsigned
                     * and subtracted -- there is no forward form -- and
                     * the flags are the ones an ADD of -1 would leave,
                     * which is what lets a loop test them afterwards.
                     */
                    const uint32_t v = do_add(c, c->r[r1], 0xFFFFFFFFu);
                    wr(c, r1, v);
                    if (v != 0u) {
                        pc = pc - (uint32_t)(w1 & 0xFFFEu);
                        goto retired_insn;
                    }
                    break;
                }
                pc = (c->r[r1] + ((w2 << 16) | (w1 & 0xFFFEu))) & ~1u;
                goto retired_insn;
            }
            const int32_t a = emu_sext(c->r[r1] & 0xFFFFu, 16);
            wr(c, r2, (uint32_t)(a * emu_sext(w1, 16)));
            break;
        }

        /* ---------------- Format VII: LD / ST, 32-bit -------------- */
        case 0x38: {                                /* LD.B disp16      */
            uint32_t v;
            const uint32_t addr = c->r[r1] + (uint32_t)emu_sext(w1, 16);
            const g4mh_exc_t e = g4mh_load(c, addr, 1u, true, &v);
            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
            wr(c, r2, v);
            break;
        }
        case 0x39: {                                /* LD.H / LD.W      */
            /*
             * As with SLD.W: a halfword access is 2-byte aligned and a
             * word access 4-byte, so disp bit 0 is free and the encoding
             * uses it to pick the width.
             */
            uint32_t v;
            const uint32_t size = (w1 & 1u) ? 4u : 2u;
            const uint32_t addr = c->r[r1] +
                                  (uint32_t)emu_sext(w1 & 0xFFFEu, 16);
            const g4mh_exc_t e = g4mh_load(c, addr, size, size == 2u, &v);
            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
            wr(c, r2, v);
            break;
        }
        case 0x3A: {                                /* ST.B disp16      */
            const uint32_t addr = c->r[r1] + (uint32_t)emu_sext(w1, 16);
            const g4mh_exc_t e = g4mh_store(c, addr, 1u, c->r[r2]);
            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
            break;
        }
        case 0x3B: {                                /* ST.H / ST.W      */
            const uint32_t size = (w1 & 1u) ? 4u : 2u;
            const uint32_t addr = c->r[r1] +
                                  (uint32_t)emu_sext(w1 & 0xFFFEu, 16);
            const g4mh_exc_t e = g4mh_store(c, addr, size, c->r[r2]);
            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
            break;
        }

        /* ---------------- Format X: the system group --------------- */
        case 0x3F: {
            /*
             * LD.HU comes first because it is not a sub-opcode at all:
             * its second halfword is a displacement, and the only thing
             * separating it from the whole Format X/XI group is bit 0.
             * Every sub-opcode below is even, so the test is exact.
             */
            if ((w1 & 1u) != 0u) {                  /* LD.HU disp16     */
                uint32_t v;
                const uint32_t adr = c->r[r1] +
                                     (uint32_t)emu_sext(w1 & 0xFFFEu, 16);
                const g4mh_exc_t e = g4mh_load(c, adr, 2u, false, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r2, v);
                break;
            }
            const uint32_t sub = w1 & 0x7FFu;
            const uint32_t sel = (w1 >> 11) & 0x1Fu;

            /*
             * The floating-point group, taken whole before the integer
             * switch rather than as cases inside it.
             *
             * Every FP sub-opcode is >= 0x400 and every integer one is
             * below it, so one comparison separates them exactly -- and
             * that is a checked fact, not a convenient assumption: the
             * integer cases in the switch below run from 0x000 to 0x3E0.
             * Splitting the group here keeps the FP encodings decoded in
             * *one* place, which is the rule this frontend already learned
             * the hard way for the Zbb/Zbc-style shared slots.
             *
             * Below is where G4MH_EXT_FPU being off lands: the whole range
             * falls through to RIE, exactly as it did before the FPU
             * existed.
             */
#if G4MH_EXT_FPU
            if (sub >= 0x400u) {
                const g4mh_exc_t e = g4mh_fpu_exec(c, sub, r1, r2, sel);

                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                break;
            }
#endif

            switch (sub) {
            case 0x000:                             /* SETF cccc, reg2  */
                wr(c, r2, g4mh_cond(r1, c->psw) ? 1u : 0u);
                break;

            /*
             * The saturating narrowings. reg1 is the source and reg2 the
             * destination -- the opposite sense to most of this group, and
             * the second halfword is entirely fixed, so reg3 has to read
             * as zero or this is some other encoding.
             */
            case 0x008:                             /* CLIP.B  r1, r2   */
            case 0x00A:                             /* CLIP.BU r1, r2   */
            case 0x00C:                             /* CLIP.H  r1, r2   */
            case 0x00E:                             /* CLIP.HU r1, r2   */
                if (sel != 0u) { EXC(G4MH_EXC_RIE); }
                wr(c, r2, do_clip(c, c->r[r1],
                                  (sub & 0x2u) == 0u,
                                  ((sub & 0x4u) != 0u) ? 16u : 8u));
                break;

            /*
             * LDSR and STSR use the two register fields in opposite
             * senses, which is the thing to get right here:
             *
             *   LDSR  bits[15:11] = regID (destination system register)
             *         bits[4:0]   = reg2  (source general register)
             *   STSR  bits[15:11] = reg2  (destination general register)
             *         bits[4:0]   = regID (source system register)
             *
             * So the *same* field is a system register in one and a
             * general register in the other. Implementing LDSR by analogy
             * with STSR gets it backwards, which is what this did.
             */
            case 0x020:                             /* LDSR reg2, regID */
                g4mh_sr_write(c, sel, r2, c->r[r1]);
                break;

            case 0x040:                             /* STSR regID, reg2 */
                wr(c, r2, g4mh_sr_read(c, sel, r1));
                break;

            /*
             * The register forms of the bit-manipulation group. The bit
             * number comes from the *value* in reg2 rather than from the
             * opcode, and the address from reg1 with no displacement.
             *
             * Their sub-opcode order is not the same as the Format VIII
             * operation-selector order above (there SET, NOT, CLR, TST;
             * here SET, NOT, CLR, TST at 0x0E0, 0x0E2, 0x0E4, 0x0E6), so
             * the two tables are written out separately rather than
             * derived from one another.
             */
            /*
             * SASF shifts a condition in at the bottom, one bit per
             * execution -- the idiom for accumulating a bitmask of test
             * results without branching. It reads the flags and defines
             * none, which is what makes a run of them composable.
             */
            case 0x200:                             /* SASF cccc, reg2  */
                wr(c, r2, (c->r[r2] << 1) |
                          (g4mh_cond(r1, c->psw) ? 1u : 0u));
                break;

            case 0x0C4:                             /* ROTL imm5, r2,r3 */
            case 0x0C6: {                           /* ROTL reg1, r2,r3 */
                /*
                 * CY comes from bit 0 of the *result*, including for a
                 * rotate of zero -- so it is the bit that was rotated
                 * round, and a zero count still redefines it rather than
                 * leaving it alone.
                 */
                const uint32_t n = ((sub == 0x0C4u) ? (w0 & 0x1Fu)
                                                    : c->r[r1]) & 0x1Fu;
                const uint32_t v = c->r[r2];
                const uint32_t res = (n == 0u) ? v
                                               : ((v << n) | (v >> (32u - n)));
                uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

                if ((res & 1u) != 0u)          { psw |= G4MH_PSW_CY; }
                if (res == 0u)                 { psw |= G4MH_PSW_Z;  }
                if ((res & 0x80000000u) != 0u) { psw |= G4MH_PSW_S;  }
                c->psw = psw;
                wr(c, sel, res);
                break;
            }

            case 0x0E0:                             /* SET1 reg2, [reg1] */
            case 0x0E2:                             /* NOT1 reg2, [reg1] */
            case 0x0E4:                             /* CLR1 reg2, [reg1] */
            case 0x0E6: {                           /* TST1 reg2, [reg1] */
                static const uint8_t k_regbitop[4] = {
                    BITOP_SET, BITOP_NOT, BITOP_CLR, BITOP_TST
                };
                const g4mh_exc_t e =
                    do_bitop(c, k_regbitop[(sub - 0x0E0u) >> 1],
                             c->r[r1], c->r[r2]);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                break;
            }

            /*
             * The swap and bit-search group, sub-opcodes 0x340-0x366.
             *
             * These read reg2 from the *first* halfword and write reg3
             * from the second, so the source is `r2` and the destination
             * is `sel`. reg1 is a fixed zero in every one of them; a
             * non-zero there is not this instruction, and the project's
             * rule is that an encoding we do not know raises RIE rather
             * than doing something plausible.
             */
            case 0x340: {                           /* BSW reg2, reg3   */
                const uint32_t v = c->r[r2];
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                const uint32_t res = __builtin_bswap32(v);
                set_swap_flags(c, res, has_zero_byte(res), res == 0u);
                wr(c, sel, res);
                break;
            }
            case 0x342: {                           /* BSH reg2, reg3   */
                const uint32_t v = c->r[r2];
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                /* Byte swap within each halfword, halfwords left alone. */
                const uint32_t res = ((v & 0x00FF00FFu) << 8) |
                                     ((v & 0xFF00FF00u) >> 8);
                const bool cy = ((res & 0x00FFu) == 0u) ||
                                ((res & 0xFF00u) == 0u);
                set_swap_flags(c, res, cy, (res & 0xFFFFu) == 0u);
                wr(c, sel, res);
                break;
            }
            case 0x344: {                           /* HSW reg2, reg3   */
                const uint32_t v = c->r[r2];
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                const uint32_t res = (v << 16) | (v >> 16);
                const bool cy = ((res & 0xFFFFu) == 0u) ||
                                ((res >> 16) == 0u);
                set_swap_flags(c, res, cy, res == 0u);
                wr(c, sel, res);
                break;
            }
            case 0x346: {                           /* HSH reg2, reg3   */
                const uint32_t res = c->r[r2];
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                /*
                 * A move, not a swap -- the value is unchanged and only
                 * the flags are the point. CY and Z are both the lower
                 * halfword being zero, which is what makes HSH the
                 * halfword counterpart of the tests above.
                 */
                const bool lo_zero = (res & 0xFFFFu) == 0u;
                set_swap_flags(c, res, lo_zero, lo_zero);
                wr(c, sel, res);
                break;
            }

            case 0x360:                             /* SCH0R reg2, reg3 */
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                do_sch(c, sel, c->r[r2], false, false);
                break;
            case 0x362:                             /* SCH1R reg2, reg3 */
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                do_sch(c, sel, c->r[r2], true, false);
                break;
            case 0x364:                             /* SCH0L reg2, reg3 */
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                do_sch(c, sel, c->r[r2], false, true);
                break;
            case 0x366:                             /* SCH1L reg2, reg3 */
                if (r1 != 0u) { EXC(G4MH_EXC_RIE); }
                do_sch(c, sel, c->r[r2], true, true);
                break;

            /*
             * The register-form shifts come in two shapes, and the
             * three-operand one is what a compiler actually emits: CC-RH
             * turns `v >> n` into `shr reg1, reg2, reg3` every time. Only
             * the two-operand forms were decoded, so every one of those
             * raised RIE -- and because a flat guest has no vector table,
             * the RIE handler address (RBASE + 0x60) landed on an ordinary
             * instruction further down the same loop. Execution carried on
             * with the shift skipped and no diagnostic, which presented as
             * an arithmetic answer that was merely wrong.
             *
             * The three-operand encoding is the two-operand one with bit 1
             * of the sub-opcode set, and reg3 in bits[31:27] -- the same
             * bit that separates ROTL's two forms at 0x0C4/0x0C6 below.
             */
            case 0x080:                             /* SHR reg1, reg2   */
                wr(c, r2, do_shr(c, c->r[r2], c->r[r1]));
                break;
            case 0x082:                             /* SHR r1, r2, r3   */
                wr(c, sel, do_shr(c, c->r[r2], c->r[r1]));
                break;
            case 0x0A0:                             /* SAR reg1, reg2   */
                wr(c, r2, do_sar(c, c->r[r2], c->r[r1]));
                break;
            case 0x0A2:                             /* SAR r1, r2, r3   */
                wr(c, sel, do_sar(c, c->r[r2], c->r[r1]));
                break;
            case 0x0C0:                             /* SHL reg1, reg2   */
                wr(c, r2, do_shl(c, c->r[r2], c->r[r1]));
                break;
            case 0x0C2:                             /* SHL r1, r2, r3   */
                wr(c, sel, do_shl(c, c->r[r2], c->r[r1]));
                break;

            case 0x0EE: {                           /* CAXI [reg1],r2,r3 */
                /*
                 * Compare and exchange. Note that it stores in *both*
                 * cases -- on a mismatch it writes the token back -- so it
                 * is a read-modify-write either way, and either way it
                 * breaks other cores' reservations on the word. That falls
                 * out of routing both through g4mh_store rather than being
                 * special-cased.
                 *
                 * Atomic here because nothing preempts inside an
                 * instruction and only one core runs at a time. Under
                 * threads this would need a real lock; see
                 * docs/host/g4mh/multicore.md.
                 */
                const uint32_t r3 = sel;
                const uint32_t adr = c->r[r1];
                uint32_t token;
                g4mh_exc_t e = g4mh_load(c, adr, 4u, false, &token);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }

                const bool match = (c->r[r2] == token);
                e = g4mh_store(c, adr, 4u, match ? c->r[r3] : token);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r3, token);

                if (!match) {
                    /* Compared unequal: this core is waiting on another,
                     * so give the quantum back rather than spin it out. */
                    pc = next;
                    done++;
                    c->retired++;
                    c->pc = pc;
                    reason = EMU_RUN_YIELD;
                    goto out;
                }
                break;
            }

            /*
             * The link/conditional-store group is six encodings, not two:
             * byte at 0x370/0x372, halfword at 0x374/0x376 and word at
             * 0x378/0x37A. The narrow loads are zero-extending only --
             * there is no LDL.B or LDL.H -- which is why they are spelled
             * LDL.BU and LDL.HU and why `false` is right for every one.
             */
            case 0x370:                             /* LDL.BU [reg1],r3 */
            case 0x374:                             /* LDL.HU [reg1],r3 */
            case 0x378: {                           /* LDL.W  [reg1],r3 */
                const uint32_t r3 = sel;
                const uint32_t adr = c->r[r1];
                const uint32_t w = (sub == 0x370u) ? 1u
                                 : ((sub == 0x374u) ? 2u : 4u);
                uint32_t v;
                const g4mh_exc_t e = g4mh_load(c, adr, w, false, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r3, v);
                g4mh_ll_take(c, adr);
                break;
            }

            case 0x372:                             /* STC.B r3,[reg1]  */
            case 0x376:                             /* STC.H r3,[reg1]  */
            case 0x37A: {                           /* STC.W r3,[reg1]  */
                /*
                 * The store happens only if this core still holds the
                 * reservation, and reg3 reports which: 1 stored, 0 did
                 * not. Either way the reservation is gone afterwards, so a
                 * retry loop must re-run its LDL.
                 *
                 * The reservation granule is a word for all three widths --
                 * g4mh_ll_take masks the address -- so a byte STC pairs
                 * with a byte LDL anywhere in the same word. That is
                 * coarser than a real part need be and is the safe
                 * direction: it can only make a conditional store fail
                 * that hardware would have let through, which a retry loop
                 * already has to cope with.
                 */
                const uint32_t r3 = sel;
                const uint32_t adr = c->r[r1];
                const uint32_t w = (sub == 0x372u) ? 1u
                                 : ((sub == 0x376u) ? 2u : 4u);
                const bool held = c->ll_valid && c->ll_addr == (adr & ~3u);

                if (held) {
                    const g4mh_exc_t e = g4mh_store(c, adr, w, c->r[r3]);
                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                }
                g4mh_ll_drop(c);
                wr(c, r3, held ? 1u : 0u);

                if (!held) {
                    /* Lost the reservation to another core: waiting. */
                    pc = next;
                    done++;
                    c->retired++;
                    c->pc = pc;
                    reason = EMU_RUN_YIELD;
                    goto out;
                }
                break;
            }

            case 0x100: {                           /* TRAP vector5     */
                /*
                 * The platform's syscall hook gets first refusal, so the
                 * host test harness can offer write/exit the way it does
                 * to a RISC-V guest. Arguments are r6-r9, the RH850
                 * calling convention's first four, and the result goes to
                 * r10, its return register.
                 *
                 * The number comes from r11, *not* from the trap vector.
                 * The vector is five bits, so it can only say 0-31, while
                 * the numbers the harness answers to are newlib's 64 and
                 * 93 -- a guest could never name them, and every syscall
                 * fell through to the architectural trap instead. Real
                 * ccrh-built code found this; the unit tests could not,
                 * because they assemble their own instruction words and
                 * had encoded the same misunderstanding.
                 *
                 * r11 is caller-saved and is not an argument register, so
                 * it is free at a call boundary -- the same role a7 plays
                 * on RISC-V. A hook that does not recognise the number
                 * returns false and the architectural trap happens after
                 * all, which is what keeps TRAP usable for its own sake.
                 */
                if (c->syscall != NULL) {
                    c->pc = pc;
                    emu_syscall_t sc = {
                        .nr  = c->r[11],
                        .arg = { c->r[6], c->r[7], c->r[8], c->r[9] },
                        .ret = 0u,
                    };
                    if (c->syscall((emu_cpu_t *)c, &sc, c->syscall_user)) {
                        c->r[10] = sc.ret;
                        pc = next;
                        goto retired_insn;
                    }
                }
                const uint32_t base = (r1 < 16u) ? G4MH_EXC_TRAP0
                                                 : G4MH_EXC_TRAP1;
                EXC_AFTER(base + (r1 & 0xFu));
            }

            case 0x120:                             /* HALT / SNOOZE    */
                if (w0 == 0x0FE0u) {
                    /*
                     * SNOOZE: a hint that this core is waiting on another
                     * and has nothing to do until one runs. It retires
                     * normally -- unlike HALT it does not stop the core --
                     * but there is no point draining the rest of the
                     * budget, so hand the quantum back.
                     */
                    c->pc = next;
                    pc = next;
                    done++;
                    c->retired++;
                    reason = EMU_RUN_YIELD;
                    goto out;
                }
                if (w0 != 0x07E0u) {
                    EXC(G4MH_EXC_RIE);
                }
                c->state = EMU_STATE_WFI;
                c->pc = next;
                pc = next;
                done++;
                reason = EMU_RUN_WFI;
                goto out;

            /*
             * G4MH has no RETI. V850's single return-from-trap was split
             * into three instructions that each name their level
             * explicitly, so which save registers to restore is in the
             * opcode rather than inferred from PSW.EP/NP -- and inferring
             * it, which is what this used to do, is wrong even when it
             * happens to pick the same pair.
             */
            case 0x144:                             /* CTRET            */
                pc = c->sr[0][G4MH_SR_CTPC];
                g4mh_sr_write(c, 0u, G4MH_SR_PSW, c->sr[0][G4MH_SR_CTPSW]);
                goto retired_insn;

            case 0x148:                             /* EIRET            */
                pc = c->sr[0][G4MH_SR_EIPC];
                g4mh_sr_write(c, 0u, G4MH_SR_PSW, c->sr[0][G4MH_SR_EIPSW]);
                goto retired_insn;

            case 0x14A:                             /* FERET            */
                pc = c->sr[0][G4MH_SR_FEPC];
                g4mh_sr_write(c, 0u, G4MH_SR_PSW, c->sr[0][G4MH_SR_FEPSW]);
                goto retired_insn;

            case 0x160:
                /*
                 * A crowded sub-opcode: DI, EI, PUSHSP, POPSP and CLL all
                 * live here and are told apart by the whole reg2 field,
                 * not by its top bit. Testing bit 15 alone -- which is
                 * what this did -- reads PUSHSP (01000) and POPSP (01100)
                 * as DI, and CLL (11111) as EI.
                 */
                switch (r2) {
                case 0x00u:
                    /*
                     * DI and RESBANK share reg2 == 0 and are told apart by
                     * reg3 -- 0 and 16. Decoding on reg2 alone ran RESBANK
                     * as DI: it masked interrupts and returned, having
                     * restored no bank at all, which is the silent wrong
                     * answer this slot keeps producing. Register banks are
                     * not modelled, so RESBANK is reported unimplemented
                     * rather than approximated.
                     */
                    if (sel == 0x10u) {             /* RESBANK          */
                        EXC(G4MH_EXC_RIE);
                    }
                    if (sel != 0u) { EXC(G4MH_EXC_RIE); }
                    c->psw |= G4MH_PSW_ID;          /* DI               */
                    break;
                case 0x10u:                         /* EI               */
                    if (sel != 0u) { EXC(G4MH_EXC_RIE); }
                    c->psw &= ~G4MH_PSW_ID;
                    c->irq_dirty = true;
                    break;

                /*
                 * CACHE and PREF are hints. This model has no cache to
                 * manage and no prefetch to start, so they retire without
                 * effect -- which is what SYNCE/SYNCM/SYNCP/SYNCI already
                 * do here. Decoding them matters anyway: undecoded they
                 * raised RIE, and in a flat guest RIE is not a report.
                 */
                case 0x1Cu:                         /* CACHE op,[reg1]  */
                case 0x1Bu:                         /* PREF  op,[reg1]  */
                    break;
                case 0x1Au: {                       /* SYSCALL vector8  */
                    /*
                     * The only exception here whose handler address is
                     * *read from memory* rather than computed from RBASE,
                     * which is why it does not go through
                     * g4mh_cpu_exception: the table lives at SCBP, the
                     * entry is a word offset from SCBP, and the result is
                     * added back to SCBP.
                     *
                     * vector8 is split -- the low five bits sit in reg1 and
                     * the high three in reg3 -- so reading reg1 alone gets
                     * the first 32 vectors right and every one above wrong.
                     * A vector past SCCFG.SIZE uses entry zero rather than
                     * faulting, which is the architecture's way of giving
                     * an out-of-range call a default handler.
                     */
                    const uint32_t vec = ((sel & 0x7u) << 5) | r1;
                    const uint32_t scbp = c->sr[1][G4MH_SR_SCBP];
                    const uint32_t size = c->sr[1][G4MH_SR_SCCFG] & 0xFFu;
                    const uint32_t adr = (vec <= size) ? (scbp + (vec << 2))
                                                       : scbp;
                    const uint32_t tmp = c->psw;
                    uint32_t ent;
                    const g4mh_exc_t e = g4mh_load(c, adr, 4u, false, &ent);

                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }

                    c->sr[0][G4MH_SR_EIPC]  = next;
                    c->sr[0][G4MH_SR_EIPSW] = tmp;
                    c->sr[0][G4MH_SR_EIIC]  = G4MH_EXC_SYSCALL + vec;
                    c->psw = (tmp & ~G4MH_PSW_UM) | G4MH_PSW_EP |
                             G4MH_PSW_ID;
                    c->sr[0][G4MH_SR_PSW] = c->psw;
                    pc = scbp + ent;
                    goto retired_insn;
                }

                case 0x1Fu:                         /* CLL              */
                    if (sel != 0x1Eu) {
                        EXC(G4MH_EXC_RIE);
                    }
                    g4mh_ll_drop(c);
                    break;
                /*
                 * PUSHSP and POPSP move a *range* of registers, rh
                 * through rt, rather than a list -- so unlike PREPARE
                 * there is no table, and unlike PREPARE they leave sp
                 * where the transfer ended with no frame adjustment.
                 * A range with rh > rt transfers nothing at all; that is
                 * defined behaviour and not an error.
                 */
                case 0x08u: {                       /* PUSHSP rh-rt     */
                    uint32_t tmp = c->r[3];

                    for (uint32_t cur = r1; cur <= sel; cur++) {
                        tmp -= 4u;
                        const g4mh_exc_t e =
                            g4mh_store(c, tmp & ~3u, 4u, c->r[cur]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                    }
                    c->r[3] = tmp;
                    break;
                }

                case 0x0Cu: {                       /* POPSP rh-rt      */
                    uint32_t tmp = c->r[3];

                    if (r1 <= sel) {
                        for (uint32_t cur = sel + 1u; cur-- > r1;) {
                            uint32_t v;
                            const g4mh_exc_t e =
                                g4mh_load(c, tmp & ~3u, 4u, false, &v);
                            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                            c->r[cur] = v;
                            tmp += 4u;
                        }
                    }
                    c->r[0] = 0u;
                    c->r[3] = tmp;
                    break;
                }

                case 0x18u:                         /* JARL [reg1],reg3 */
                    /*
                     * The register-indirect call. Written before the
                     * jump because reg1 and reg3 may name the same
                     * register -- the same hazard RISC-V's `jalr ra, ra`
                     * has, and the reason the target is read first.
                     */
                    {
                        const uint32_t target = c->r[r1] & ~1u;
                        wr(c, sel, next);
                        c->sr[0][G4MH_SR_PSW] = c->psw;
                        pc = target;
                    }
                    goto retired_insn;

                default:
                    EXC(G4MH_EXC_RIE);
                }
                c->sr[0][G4MH_SR_PSW] = c->psw;
                break;

            default:
                /*
                 * The three-operand group: MUL, DIV and friends put their
                 * third register in bits[15:11] of the second halfword,
                 * so the sub-opcode has to be matched on the low bits
                 * alone rather than on the whole word.
                 */
                /*
                 * The groups that carry an operand in their low bits, so
                 * that no exact case above can match them: a condition
                 * for CMOV, SBF and ADF, a register for MAC, and the
                 * bit positions for BINS. Each is matched on the width
                 * of field it actually leaves fixed -- 0x7E0 where four
                 * condition bits vary, 0x7F0 where three do -- because a
                 * mask that is one bit too wide swallows a neighbour.
                 * BINS at 0x090 is exactly that case: 0x7E0 would fold
                 * it into SHR at 0x080.
                 */
                switch (sub & 0x7E0u) {
                case 0x300:                         /* CMOV imm5        */
                case 0x320:                         /* CMOV reg1        */
                    wr(c, sel, g4mh_cond((sub >> 1) & 0xFu, c->psw)
                                 ? ((sub & 0x20u) != 0u
                                        ? c->r[r1]
                                        : (uint32_t)g4mh_imm5(w0))
                                 : c->r[r2]);
                    goto sub_done;

                case 0x380: {                       /* SBF cccc         */
                    /*
                     * reg2 - reg1 - cond, with a borrow the ordinary
                     * subtract helper cannot express, so the flags are
                     * computed here. CY is a borrow on RH850, and the
                     * overflow test is the two-operand one, which stays
                     * correct with a borrow in.
                     */
                    const uint32_t a = c->r[r1];
                    const uint32_t b = c->r[r2];
                    const uint32_t k =
                        g4mh_cond((sub >> 1) & 0xFu, c->psw) ? 1u : 0u;
                    const uint32_t res = b - a - k;
                    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

                    if ((uint64_t)b < (uint64_t)a + k) { psw |= G4MH_PSW_CY; }
                    if (res == 0u)                 { psw |= G4MH_PSW_Z; }
                    if ((res & 0x80000000u) != 0u) { psw |= G4MH_PSW_S; }
                    if (((a ^ b) & (b ^ res) & 0x80000000u) != 0u) {
                        psw |= G4MH_PSW_OV;
                    }
                    c->psw = psw;
                    wr(c, sel, res);
                    goto sub_done;
                }

                case 0x3A0: {                       /* ADF cccc         */
                    const uint32_t a = c->r[r1];
                    const uint32_t b = c->r[r2];
                    const uint32_t k =
                        g4mh_cond((sub >> 1) & 0xFu, c->psw) ? 1u : 0u;
                    const uint64_t wide = (uint64_t)a + b + k;
                    const uint32_t res = (uint32_t)wide;
                    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

                    if ((wide >> 32) != 0u)        { psw |= G4MH_PSW_CY; }
                    if (res == 0u)                 { psw |= G4MH_PSW_Z; }
                    if ((res & 0x80000000u) != 0u) { psw |= G4MH_PSW_S; }
                    if ((~(a ^ b) & (a ^ res) & 0x80000000u) != 0u) {
                        psw |= G4MH_PSW_OV;
                    }
                    c->psw = psw;
                    wr(c, sel, res);
                    goto sub_done;
                }

                case 0x3C0:                         /* MAC  reg1,r2,r3,r4 */
                case 0x3E0: {                       /* MACU               */
                    /*
                     * A 64-bit accumulate across a register pair. Both
                     * pairs are named by four bits and are therefore
                     * always even -- reg3 in the top of the second
                     * halfword, reg4 in the same bits the conditions use
                     * elsewhere -- so the odd half is reg+1 and never
                     * needs encoding.
                     */
                    const uint32_t r3 = sel & ~1u;
                    const uint32_t r4 = ((sub >> 1) & 0xFu) << 1;
                    const uint64_t acc = ((uint64_t)c->r[r3 + 1u] << 32) |
                                         c->r[r3];
                    uint64_t res;

                    if ((sub & 0x20u) == 0u) {      /* MAC: signed      */
                        res = (uint64_t)(((int64_t)(int32_t)c->r[r2] *
                                          (int64_t)(int32_t)c->r[r1]) +
                                         (int64_t)acc);
                    } else {                        /* MACU: unsigned   */
                        res = (uint64_t)c->r[r2] * (uint64_t)c->r[r1] + acc;
                    }
                    wr(c, r4, (uint32_t)res);
                    wr(c, r4 + 1u, (uint32_t)(res >> 32));
                    goto sub_done;
                }

                default:
                    break;
                }

                switch (sub & 0x7F0u) {
                case 0x090:                         /* BINS msb>=16 lsb>=16 */
                case 0x0B0:                         /* BINS msb>=16 lsb<16  */
                case 0x0D0: {                       /* BINS msb<16  lsb<16  */
                    /*
                     * Insert reg1's low bits into reg2 at [msb:lsb].
                     * Only the low four bits of each position are
                     * encoded; bit 4 of each comes from *which* of the
                     * three sub-opcodes this is, which is the whole
                     * reason there are three.
                     */
                    const uint32_t msb = ((w1 >> 12) & 0xFu) |
                                         ((sub & 0x7F0u) != 0x0D0u ? 16u : 0u);
                    const uint32_t lsb = (((w1 >> 8) & 0x8u) |
                                          ((w1 >> 1) & 0x7u)) |
                                         ((sub & 0x7F0u) == 0x090u ? 16u : 0u);
                    if (msb < lsb) {
                        EXC(G4MH_EXC_RIE);
                    }
                    const uint32_t width = msb - lsb + 1u;
                    const uint32_t mask = (width >= 32u)
                                            ? 0xFFFFFFFFu
                                            : (((1u << width) - 1u) << lsb);
                    const uint32_t res = (c->r[r2] & ~mask) |
                                         ((c->r[r1] << lsb) & mask);
                    set_logic(c, res);
                    wr(c, r2, res);
                    goto sub_done;
                }
                default:
                    break;
                }

                switch (sub & 0x7FDu) {
                case 0x220: {                       /* MUL / MULU       */
                    const uint32_t r3 = sel;
                    if ((sub & 0x2u) == 0u) {       /* MUL: signed      */
                        const int64_t p = (int64_t)(int32_t)c->r[r2] *
                                          (int64_t)(int32_t)c->r[r1];
                        wr(c, r2, (uint32_t)p);
                        wr(c, r3, (uint32_t)((uint64_t)p >> 32));
                    } else {                        /* MULU: unsigned   */
                        const uint64_t p = (uint64_t)c->r[r2] *
                                           (uint64_t)c->r[r1];
                        wr(c, r2, (uint32_t)p);
                        wr(c, r3, (uint32_t)(p >> 32));
                    }
                    break;
                }

                /*
                 * DIVQ and DIVQU are DIV and DIVU. The manual's only
                 * difference is the cycle count -- "the minimum number of
                 * steps required is determined from the values in reg1 and
                 * reg2" -- and an emulator has no steps to save. They get
                 * the same label rather than a copy of the body, because a
                 * second copy is a second thing to get the INT32_MIN / -1
                 * overflow rule wrong in. CC-RH emits DIVQ for ordinary C
                 * integer division, so this is the form a real guest hits.
                 */
                case 0x2C0:                         /* DIV / DIVU       */
                case 0x2FC: {                       /* DIVQ / DIVQU     */
                    const uint32_t r3 = sel;
                    const uint32_t d = c->r[r1];
                    if (d == 0u) {
                        /* Divide by zero sets OV and leaves the operands
                         * alone; RH850 does not trap on it. */
                        c->psw |= G4MH_PSW_OV;
                        break;
                    }
                    if ((sub & 0x2u) == 0u) {       /* DIV: signed      */
                        const int32_t a = (int32_t)c->r[r2];
                        const int32_t b = (int32_t)d;
                        if (a == INT32_MIN && b == -1) {
                            c->psw |= G4MH_PSW_OV;
                            break;
                        }
                        const int32_t q = a / b;
                        const int32_t r = a % b;
                        set_zs(c, (uint32_t)q);
                        c->psw &= ~G4MH_PSW_OV;
                        wr(c, r2, (uint32_t)q);
                        wr(c, r3, (uint32_t)r);
                    } else {                        /* DIVU: unsigned   */
                        const uint32_t q = c->r[r2] / d;
                        const uint32_t r = c->r[r2] % d;
                        set_zs(c, q);
                        c->psw &= ~G4MH_PSW_OV;
                        wr(c, r2, q);
                        wr(c, r3, r);
                    }
                    break;
                }

                case 0x280: {                       /* DIVH / DIVHU     */
                    /*
                     * Only the *lower halfword* of reg1 is the divisor --
                     * sign-extended for DIVH, zero-extended for DIVHU --
                     * so a 32-bit divisor whose low half is zero divides
                     * by zero however large it is. The three-operand form
                     * stores the remainder in reg3; the two-operand one is
                     * Format I at op 0x02 and has no remainder.
                     */
                    const uint32_t r3 = sel;
                    const bool sgn = (sub & 0x2u) == 0u;
                    const uint32_t lo = c->r[r1] & 0xFFFFu;
                    const uint32_t d = sgn ? (uint32_t)(int32_t)(int16_t)lo
                                           : lo;
                    if (d == 0u) {
                        c->psw |= G4MH_PSW_OV;
                        break;
                    }
                    uint32_t q;
                    uint32_t rem;
                    if (sgn) {
                        const int32_t a = (int32_t)c->r[r2];
                        const int32_t b = (int32_t)d;
                        if (a == INT32_MIN && b == -1) {
                            c->psw |= G4MH_PSW_OV;
                            break;
                        }
                        q = (uint32_t)(a / b);
                        rem = (uint32_t)(a % b);
                    } else {
                        q = c->r[r2] / d;
                        rem = c->r[r2] % d;
                    }
                    set_zs(c, q);
                    c->psw &= ~G4MH_PSW_OV;
                    wr(c, r2, q);
                    wr(c, r3, rem);
                    break;
                }

                default:
                    /*
                     * The imm9 multiplies are last because their immediate
                     * is *split across the sub-opcode*: bits[8:5] sit in
                     * sub bits[5:2] and bits[4:0] in the reg1 field, so no
                     * exact case can match them and the mask has to be
                     * 0x7C0 rather than the 0x7FD used above. That mask
                     * would also swallow DIV and DIVQ, which is why this
                     * runs only after they have had their exact cases.
                     */
                    if ((sub & 0x7C0u) == 0x240u) {  /* MUL/MULU imm9   */
                        const uint32_t r3 = sel;
                        const uint32_t imm9 = (((sub >> 2) & 0xFu) << 5) | r1;
                        if ((sub & 0x2u) == 0u) {   /* MUL: sign-extend */
                            const int64_t p = (int64_t)(int32_t)c->r[r2] *
                                              (int64_t)emu_sext(imm9, 9);
                            wr(c, r2, (uint32_t)p);
                            wr(c, r3, (uint32_t)((uint64_t)p >> 32));
                        } else {                    /* MULU: zero-extend */
                            const uint64_t p = (uint64_t)c->r[r2] *
                                               (uint64_t)imm9;
                            wr(c, r2, (uint32_t)p);
                            wr(c, r3, (uint32_t)(p >> 32));
                        }
                        break;
                    }
                    EXC(G4MH_EXC_RIE);
                }
                break;
            }
        sub_done:
            break;
        }

        /* ---------------- Format V: JR / JARL disp22 --------------- */
        case 0x3C:
        case 0x3D: {
            if ((w1 & 1u) != 0u) {
                /*
                 * Bit 0 of the second halfword separates JR/JARL disp22
                 * -- whose displacement is even, so the bit is free --
                 * from everything else in this slot. What is left is
                 * told apart by reg2 and then by the low bits of the
                 * second halfword, in the same order g4mh_insn_is_48
                 * uses. The two must agree: it decides the length and
                 * this decides the meaning.
                 */
                if (r2 != 0u) {                     /* LD.BU disp16     */
                    /*
                     * The one load whose displacement is not naturally
                     * aligned, so its bit 0 has nowhere to live in the
                     * second halfword and is carried in the *opcode*
                     * instead -- which is why 0x3C and 0x3D are one
                     * instruction here and two everywhere else.
                     */
                    const uint32_t disp = (uint32_t)(w1 & 0xFFFEu) |
                                          (op & 1u);
                    uint32_t v;
                    const g4mh_exc_t e =
                        g4mh_load(c, c->r[r1] + (uint32_t)emu_sext(disp, 16),
                                  1u, false, &v);
                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                    wr(c, r2, v);
                    break;
                }

                if ((w1 & 0x1Fu) == 0x01u || (w1 & 0x07u) == 0x03u) {
                    /*
                     * PREPARE: save the list, then cut the frame. The
                     * order matters -- the saved words go below the old
                     * sp and the frame below those -- and so does doing
                     * the sp write only after every store has succeeded,
                     * because an MDP fault partway through must leave sp
                     * and ep at their old values for the handler.
                     */
                    const uint32_t list = ((uint32_t)w1 << 16) | w0;
                    const uint32_t imm5 = (w0 >> 1) & 0x1Fu;
                    const uint32_t ff = (w1 >> 3) & 3u;
                    uint32_t sp;

                    const g4mh_exc_t e = do_prepare_save(c, list, &sp);
                    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                    sp -= imm5 << 2;
                    c->r[3] = sp;

                    if ((w1 & 0x07u) == 0x03u) {
                        /*
                         * ff names what reaches ep: 00 the new sp, 01 a
                         * sign-extended imm16, 10 that imm16 shifted up,
                         * and 11 a full imm32 in the two halfwords after
                         * w1 -- the ISA's only 64-bit encoding, and the
                         * one the length decoder now reaches.
                         */
                        c->r[30] = (ff == 0u) ? sp
                                 : (ff == 1u) ? (uint32_t)emu_sext(w2, 16)
                                 : (ff == 2u) ? (w2 << 16)
                                              : ((w3 << 16) | w2);
                    }
                    break;
                }

                /*
                 * Format XIV: the 48-bit disp23 loads and stores.
                 *
                 *   w0  00000 1111 0x RRRRR      reg2 = 0, reg1 = base
                 *   w1  wwwww ddddddd ssss       reg3, disp[6:0], opcode
                 *   w2  DDDDDDDDDDDDDDDD        disp[22:7]
                 *
                 * The manual draws the aligned forms with a *five*-bit
                 * opcode and six displacement bits, because their disp[0]
                 * is architecturally zero -- LD.DW is `dddddd01001`
                 * against LD.B's `ddddddd0101`. Read as one rule, that
                 * bit is disp[0] for the byte forms and required-zero for
                 * the rest, which is what the check below says. Both
                 * readings agree on everything an assembler emits; they
                 * differ only on reserved encodings, and there RIE is the
                 * architectural answer rather than the misaligned-address
                 * exception a uniform reading would produce.
                 *
                 * Confirmed against CC-RH, which is the only thing here
                 * that can say an opcode constant is wrong: the whole
                 * table below came out of scripts/g4mh-check-encodings.sh
                 * and not out of the manual's diagrams.
                 */
                {
                    const uint32_t r3   = (w1 >> 11) & 0x1Fu;
                    const uint32_t sub  = w1 & 0x0Fu;
                    const bool     is_b = (sub == 0x5u) ||
                                          (sub == 0xDu && op == 0x3Cu);
                    const uint32_t d0   = (w1 >> 4) & 1u;
                    uint32_t disp;
                    uint32_t addr;
                    uint32_t v;
                    g4mh_exc_t e;

                    if (!is_b && d0 != 0u) {
                        EXC(G4MH_EXC_RIE);      /* opcode bit, not disp */
                    }

                    disp = (w2 << 7) | ((w1 >> 4) & 0x7Fu);
                    addr = c->r[r1] + (uint32_t)emu_sext(disp, 23);

                    switch ((sub << 1) | (op & 1u)) {
                    case (0x5u << 1) | 0u:      /* LD.B  disp23 */
                        e = g4mh_load(c, addr, 1u, true, &v);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, r3, v);
                        break;
                    case (0x5u << 1) | 1u:      /* LD.BU disp23 */
                        e = g4mh_load(c, addr, 1u, false, &v);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, r3, v);
                        break;
                    case (0x7u << 1) | 0u:      /* LD.H  disp23 */
                        e = g4mh_load(c, addr, 2u, true, &v);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, r3, v);
                        break;
                    case (0x7u << 1) | 1u:      /* LD.HU disp23 */
                        e = g4mh_load(c, addr, 2u, false, &v);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, r3, v);
                        break;
                    case (0x9u << 1) | 0u:      /* LD.W  disp23 */
                        e = g4mh_load(c, addr, 4u, false, &v);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, r3, v);
                        break;
                    case (0x9u << 1) | 1u: {    /* LD.DW disp23 */
                        /*
                         * "reg3 must be an even-numbered register. If an
                         * odd-numbered register is specified, bit 0 of
                         * the register number is ignored" -- so this
                         * masks rather than raising RIE. CC-RH aligns it
                         * down with a warning, so the case is unreachable
                         * from compiled code and the manual is the only
                         * statement of what it does.
                         *
                         * Two word accesses, not one eight-byte one:
                         * the caution under LD.DW says no MAE occurs when
                         * the address is on a *word* boundary, so the
                         * alignment required is 4 and each half checks
                         * it. Both loads complete before either register
                         * is written, so a fault on the second leaves the
                         * first untouched for the handler.
                         */
                        const uint32_t rd = r3 & ~1u;
                        uint32_t lo, hi;
                        e = g4mh_load(c, addr, 4u, false, &lo);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        e = g4mh_load(c, addr + 4u, 4u, false, &hi);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        wr(c, rd, lo);
                        wr(c, rd + 1u, hi);
                        break;
                    }
                    case (0xDu << 1) | 0u:      /* ST.B  disp23 */
                        e = g4mh_store(c, addr, 1u, c->r[r3]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        break;
                    case (0xDu << 1) | 1u:      /* ST.H  disp23 */
                        e = g4mh_store(c, addr, 2u, c->r[r3]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        break;
                    case (0xFu << 1) | 0u:      /* ST.W  disp23 */
                        e = g4mh_store(c, addr, 4u, c->r[r3]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        break;
                    case (0xFu << 1) | 1u: {    /* ST.DW disp23 */
                        const uint32_t rs = r3 & ~1u;
                        e = g4mh_store(c, addr, 4u, c->r[rs]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        e = g4mh_store(c, addr + 4u, 4u, c->r[rs + 1u]);
                        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                        break;
                    }
                    default:
                        EXC(G4MH_EXC_RIE);
                    }
                    break;
                }
            }
            /*
             * bits[10:6] are the opcode, so bit 5 -- the low bit of the
             * 6-bit field this switch dispatches on -- is disp[5], which
             * is why both values arrive here and are handled alike.
             *
             * The displacement is split with its *high* bits in the first
             * halfword: disp[21:16] in w0[5:0] and disp[15:1] in w1[15:1],
             * with disp[0] hardwired zero. Relative to the address of the
             * instruction, not of the next one.
             *
             * Worth stating because the natural assumption is the other
             * order -- low bits first, as RISC-V does it -- and that is
             * what this originally implemented. It gave a plausible-looking
             * displacement for small forward jumps and garbage for
             * everything else.
             *
             * reg2 == 0 makes it JR: the link register is r0, so the
             * return address is written and discarded, and one encoding
             * serves both.
             */
            const uint32_t d = ((uint32_t)(w0 & 0x3Fu) << 16) |
                               ((uint32_t)w1 & 0xFFFEu);
            wr(c, r2, next);
            pc = pc + (uint32_t)emu_sext(d, 22);
            goto retired_insn;
        }

        /*
         * Format VIII: the bit-manipulation group on memory.
         *
         *   ooBBB111110RRRRR  dddddddddddddddd
         *
         * where oo picks the operation and BBB is the bit number. The
         * operation selector is bits[15:14] -- the *top* of the field
         * that every other 32-bit format uses for reg2 -- so this whole
         * opcode is one where reading a register number out of bits
         * [15:11] gets nonsense.
         *
         * All four are byte-sized read-modify-writes at a sign-extended
         * disp16 from reg1, all four set Z from the bit *before* the
         * change and touch no other flag, and TST1 alone does not write
         * back.
         */
        case 0x3E: {
            static const uint8_t k_bitop[4] = {
                BITOP_SET, BITOP_NOT, BITOP_CLR, BITOP_TST
            };
            const uint32_t op  = (w0 >> 14) & 0x3u;
            const uint32_t bit = (w0 >> 11) & 0x7u;
            const uint32_t adr = c->r[r1] + (uint32_t)(int32_t)(int16_t)w1;
            const g4mh_exc_t e = do_bitop(c, k_bitop[op], adr, bit);
            if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
            break;
        }
        }

        pc = next;

    retired_insn:
        done++;
        c->retired++;
        c->cycles++;
        /*
         * The performance counters are *not* ticked here. They are ticked
         * once per run slice by g4mh_ops_run, from the retired delta,
         * because a tick on this path counts only interpreted
         * instructions -- and under the JIT that is a small and
         * arbitrary subset. See the note there.
         */
        continue;

    next_insn:
        /* Reached after an exception; pc already points at the handler. */
        done++;
        c->cycles++;
    }

out:
    c->pc = pc;
    if (retired != NULL) {
        *retired = done;
    }
    if (c->state == EMU_STATE_HALTED) {
        reason = EMU_RUN_HALTED;
    }
    return reason;
}

/* ------------------------------------------------------------------ */
/* Backend plumbing                                                    */
/* ------------------------------------------------------------------ */

static void interp_reset(g4mh_cpu_t *c)
{
    (void)c;   /* no translation state to discard */
}

/* Adapters onto emu_backend_t; the cast is paid once per budget. */
static void interp_reset_cpu(emu_cpu_t *cpu)
{
    interp_reset((g4mh_cpu_t *)cpu);
}

static emu_run_reason_t interp_run_cpu(emu_cpu_t *cpu, uint32_t budget,
                                       uint32_t *retired)
{
    return interp_run((g4mh_cpu_t *)cpu, budget, retired);
}

const emu_backend_t g4mh_backend_interp = {
    .name       = "interp",
    .init       = NULL,
    .reset      = interp_reset_cpu,
    .run        = interp_run_cpu,
    .invalidate = NULL,
};

const emu_backend_t *g4mh_backend = &g4mh_backend_interp;

emu_run_reason_t g4mh_step(g4mh_cpu_t *c)
{
    return g4mh_backend->run((emu_cpu_t *)c, 1u, NULL);
}
