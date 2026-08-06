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

        case 0x03:                                  /* JMP [reg1]       */
            if (r2 != 0u) {
                EXC(G4MH_EXC_RIE);   /* SLD.BU and friends: not implemented */
            }
            /* Bit 0 of the target is ignored: instructions are halfword
             * aligned and the architecture defines the low bit as zero
             * rather than as a mode bit. */
            pc = c->r[r1] & ~1u;
            goto retired_insn;

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
        case 0x10:                                  /* MOV imm5 / CALLT */
            if (r2 == 0u) {
                /* CALLT imm6 shares this slot. Not implemented -- and it
                 * must not fall through to "MOV into r0", which would
                 * retire silently instead of calling. */
                EXC(G4MH_EXC_RIE);
            }
            wr(c, r2, (uint32_t)g4mh_imm5(w0));
            break;
        case 0x11:                                  /* SATADD imm5      */
            wr(c, r2, do_satadd(c, c->r[r2], (uint32_t)g4mh_imm5(w0)));
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
        case 0x17: {                                /* MULH imm5        */
            if (r2 == 0u) {
                /* JR / JARL disp32, a 48-bit form. Not implemented. */
                EXC(G4MH_EXC_RIE);
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
                /* DISPOSE straddles both slots with reg2 == 0. Not
                 * implemented, and it must not fall through to a write
                 * into r0 -- a silently skipped stack-frame pop is far
                 * worse than a clean exception. */
                EXC(G4MH_EXC_RIE);
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
        case 0x37: {                                /* MULHI imm16      */
            if (r2 == 0u) {
                /* JMP disp32[reg1], a 48-bit form. Not implemented. */
                EXC(G4MH_EXC_RIE);
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
            const uint32_t sub = w1 & 0x7FFu;
            const uint32_t sel = (w1 >> 11) & 0x1Fu;

            switch (sub) {
            case 0x000:                             /* SETF cccc, reg2  */
                wr(c, r2, g4mh_cond(r1, c->psw) ? 1u : 0u);
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

            case 0x080:                             /* SHR reg1, reg2   */
                wr(c, r2, do_shr(c, c->r[r2], c->r[r1]));
                break;
            case 0x0A0:                             /* SAR reg1, reg2   */
                wr(c, r2, do_sar(c, c->r[r2], c->r[r1]));
                break;
            case 0x0C0:                             /* SHL reg1, reg2   */
                wr(c, r2, do_shl(c, c->r[r2], c->r[r1]));
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

            case 0x378: {                           /* LDL.W [reg1],r3  */
                const uint32_t r3 = sel;
                const uint32_t adr = c->r[r1];
                uint32_t v;
                const g4mh_exc_t e = g4mh_load(c, adr, 4u, false, &v);
                if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) { EXC(e); }
                wr(c, r3, v);
                g4mh_ll_take(c, adr);
                break;
            }

            case 0x37A: {                           /* STC.W r3,[reg1]  */
                /*
                 * The store happens only if this core still holds the
                 * reservation, and reg3 reports which: 1 stored, 0 did
                 * not. Either way the reservation is gone afterwards, so a
                 * retry loop must re-run its LDL.W.
                 */
                const uint32_t r3 = sel;
                const uint32_t adr = c->r[r1];
                const bool held = c->ll_valid && c->ll_addr == (adr & ~3u);

                if (held) {
                    const g4mh_exc_t e = g4mh_store(c, adr, 4u, c->r[r3]);
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
                 * to a RISC-V guest. The vector selects, and r6/r7/r8/r9
                 * carry the arguments -- the RH850 calling convention's
                 * first four.
                 */
                if (c->syscall != NULL) {
                    c->pc = pc;
                    emu_syscall_t sc = {
                        .nr  = r1,
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
                case 0x00u:                         /* DI               */
                    c->psw |= G4MH_PSW_ID;
                    break;
                case 0x10u:                         /* EI               */
                    c->psw &= ~G4MH_PSW_ID;
                    c->irq_dirty = true;
                    break;
                case 0x1Fu:                         /* CLL              */
                    if (sel != 0x1Eu) {
                        EXC(G4MH_EXC_RIE);
                    }
                    g4mh_ll_drop(c);
                    break;
                default:                            /* PUSHSP / POPSP   */
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

                case 0x2C0: {                       /* DIV / DIVU       */
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

                default:
                    EXC(G4MH_EXC_RIE);
                }
                break;
            }
            break;
        }

        /* ---------------- Format V: JR / JARL disp22 --------------- */
        case 0x3C:
        case 0x3D: {
            if (r2 == 0u && (w1 & 1u) != 0u) {
                /*
                 * PREPARE, and the 48-bit disp23 loads and stores. Both
                 * live in this slot with reg2 == 0 and are told from JR by
                 * bit 0 of the second halfword. Not implemented.
                 */
                EXC(G4MH_EXC_RIE);
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

        /* ------------------------------------------------------------
         * Everything not named above: the bit-manipulation group
         * (Format VIII), the FP instructions, and the 48-bit forms other
         * than MOV imm32. See the scope note in g4mh_cpu.h.
         * ---------------------------------------------------------- */
        case 0x3E:
            EXC(G4MH_EXC_RIE);
        }

        pc = next;

    retired_insn:
        done++;
        c->retired++;
        c->cycles++;
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
