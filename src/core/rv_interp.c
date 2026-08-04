/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_interp.c - Threaded RV32IMAC interpreter.
 *
 * One instruction per loop iteration:
 *
 *   1. take a pending interrupt, if any
 *   2. fetch (one or two 16-bit parcels)
 *   3. expand compressed encodings to their 32-bit equivalent
 *   4. execute
 *
 * Dispatch is a switch on inst[6:2]. Because the low two bits of a 32-bit
 * instruction are always 0b11, dropping them yields a dense 0..31 index
 * that GCC lowers to a jump table, which is what a hand-written computed
 * goto would produce anyway.
 *
 * Traps unwind through the TRAP() macro rather than by returning error
 * codes up a call chain: it restores pc to the faulting instruction (so
 * mepc is right), enters the trap, and continues the loop.
 */

#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_hart.h"

/* ------------------------------------------------------------------ */
/* Register file helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * x0 reads as zero and discards writes. Writing unconditionally and then
 * clearing x0 costs one extra store but avoids a mispredictable branch on
 * every register write, which is the better trade on an in-order M-class
 * core.
 */
static RV_ALWAYS_INLINE void wr(rv_hart_t *h, uint32_t rd, uint32_t v)
{
    h->x[rd] = v;
    h->x[0] = 0u;
}

/* ------------------------------------------------------------------ */
/* M extension helpers                                                 */
/* ------------------------------------------------------------------ */

#if RV_EXT_M
static RV_ALWAYS_INLINE uint32_t mulh_ss(int32_t a, int32_t b)
{
    return (uint32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static RV_ALWAYS_INLINE uint32_t mulh_su(int32_t a, uint32_t b)
{
    /* b is zero-extended before the multiply, so the product fits in 64 bits. */
    return (uint32_t)(((int64_t)a * (int64_t)(uint64_t)b) >> 32);
}

static RV_ALWAYS_INLINE uint32_t mulh_uu(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

/*
 * RISC-V defines division by zero and signed overflow as producing specific
 * values rather than trapping, so these cases are handled explicitly. C
 * would treat INT32_MIN / -1 as undefined behaviour.
 */
static RV_ALWAYS_INLINE uint32_t div_s(int32_t a, int32_t b)
{
    if (RV_UNLIKELY(b == 0)) {
        return 0xFFFFFFFFu;                 /* -1 */
    }
    if (RV_UNLIKELY(a == INT32_MIN && b == -1)) {
        return (uint32_t)INT32_MIN;         /* overflow wraps to the dividend */
    }
    return (uint32_t)(a / b);
}

static RV_ALWAYS_INLINE uint32_t rem_s(int32_t a, int32_t b)
{
    if (RV_UNLIKELY(b == 0)) {
        return (uint32_t)a;
    }
    if (RV_UNLIKELY(a == INT32_MIN && b == -1)) {
        return 0u;
    }
    return (uint32_t)(a % b);
}

static RV_ALWAYS_INLINE uint32_t div_u(uint32_t a, uint32_t b)
{
    return RV_UNLIKELY(b == 0u) ? 0xFFFFFFFFu : (a / b);
}

static RV_ALWAYS_INLINE uint32_t rem_u(uint32_t a, uint32_t b)
{
    return RV_UNLIKELY(b == 0u) ? a : (a % b);
}
#endif /* RV_EXT_M */

/* ------------------------------------------------------------------ */
/* Zbb helpers                                                         */
/* ------------------------------------------------------------------ */

#if RV_EXT_ZBB
/* __builtin_clz/ctz are undefined for zero; Zbb defines both as 32. */
static RV_ALWAYS_INLINE uint32_t zbb_clz(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_clz(v);
}

static RV_ALWAYS_INLINE uint32_t zbb_ctz(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_ctz(v);
}

/* Rotates by a multiple of 32 must not shift by 32, which is UB in C. */
static RV_ALWAYS_INLINE uint32_t zbb_ror(uint32_t v, uint32_t n)
{
    n &= 31u;
    return (n == 0u) ? v : ((v >> n) | (v << (32u - n)));
}

static RV_ALWAYS_INLINE uint32_t zbb_rol(uint32_t v, uint32_t n)
{
    n &= 31u;
    return (n == 0u) ? v : ((v << n) | (v >> (32u - n)));
}

/* orc.b: each byte becomes 0xFF if any of its bits are set, else 0x00. */
static RV_ALWAYS_INLINE uint32_t zbb_orcb(uint32_t v)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        if ((v & (0xFFu << (i * 8u))) != 0u) {
            r |= 0xFFu << (i * 8u);
        }
    }
    return r;
}
#endif /* RV_EXT_ZBB */

#if RV_EXT_ZBC
/*
 * Carry-less multiply: the same shift-and-accumulate as an ordinary
 * multiply with XOR in place of addition, so there are no carries between
 * bit positions. clmulh takes the high half of the 64-bit product and
 * clmulr the middle 32 bits (63:31), which is why their shifts differ by
 * one.
 */
static uint32_t zbc_clmul(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 32u; i++) {
        if ((b >> i) & 1u) {
            r ^= a << i;
        }
    }
    return r;
}

static uint32_t zbc_clmulh(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 1; i < 32u; i++) {
        if ((b >> i) & 1u) {
            r ^= a >> (32u - i);
        }
    }
    return r;
}

static uint32_t zbc_clmulr(uint32_t a, uint32_t b)
{
    uint32_t r = 0u;
    for (unsigned i = 0; i < 32u; i++) {
        if ((b >> i) & 1u) {
            r ^= a >> (31u - i);
        }
    }
    return r;
}
#endif /* RV_EXT_ZBC */

/* ------------------------------------------------------------------ */
/* Control-transfer target validation                                  */
/* ------------------------------------------------------------------ */

/* With C, targets need 2-byte alignment; without it, 4-byte. */
#if RV_EXT_C
#  define TARGET_ALIGN_MASK 1u
#else
#  define TARGET_ALIGN_MASK 3u
#endif

/* ------------------------------------------------------------------ */
/* The interpreter                                                     */
/* ------------------------------------------------------------------ */

/*
 * When RV_INTERP_RAMFUNC is set the run loop is placed in .ramfunc, which
 * the link script maps into SRAM with a load address in flash. noinline
 * keeps it from being folded back into a caller that lives in flash.
 */
#if RV_INTERP_RAMFUNC
#  define RV_INTERP_SECTION __attribute__((section(".ramfunc"), noinline))
#else
#  define RV_INTERP_SECTION
#endif

static RV_INTERP_SECTION rv_run_reason_t interp_run(rv_hart_t *h,
                                                    uint32_t budget,
                                                    uint32_t *retired)
{
    uint32_t done = 0;
    rv_run_reason_t reason = RV_RUN_BUDGET;

    /*
     * pc is kept in a local across the loop and written back to the hart
     * only on exit or on a trap. `pc` is the address of the instruction
     * being executed; `next` is where control goes if it completes.
     */
    uint32_t pc = h->pc;
    uint32_t next;

    /*
     * A parked hart is handled once here rather than tested inside the
     * dispatch loop: it has nothing to execute, so carrying the test
     * through the loop would cost a load per instruction to serve a case
     * that cannot occur while the loop is running.
     */
    if (RV_UNLIKELY(h->state == RV_STATE_WFI)) {
        if (rv_hart_pending_irq(h) == RV_EXC_NONE) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return RV_RUN_WFI;
        }
        h->state = RV_STATE_RUNNING;
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;   /* let the loop below deliver it */
#endif
    }

/* Enter a trap. mepc must be the faulting instruction, so restore pc. */
#define TRAP(cause_, tval_)                     \
    do {                                        \
        h->pc = pc;                             \
        rv_hart_trap(h, (cause_), (tval_));     \
        pc = h->pc;                             \
        goto retired_insn;                      \
    } while (0)

    while (done < budget) {
        /*
         * Set when this instruction wrote mcycle or minstret. The spec is
         * explicit that for counters updated as a side effect of execution,
         * "if a CSR access instruction writes such a CSR, the write is done
         * instead of the increment" -- so a write has to suppress this
         * instruction's own increment, or `csrw minstret, 0` would read
         * back as 1.
         */
        uint32_t ctr_written = 0u;
#define CTR_CYCLE   1u
#define CTR_INSTRET 2u

        /* One test covers both halt and WFI; neither can continue here. */
        if (RV_UNLIKELY(h->state != RV_STATE_RUNNING)) {
            reason = (h->state == RV_STATE_HALTED) ? RV_RUN_HALTED : RV_RUN_WFI;
            break;
        }

        /* --- 1. interrupts ------------------------------------------- */
        /*
         * Evaluating delivery means reading mstatus, mip and mie and
         * combining them; doing that per instruction was measurable. The
         * dirty flag reduces it to one load and a predictable branch in
         * the common case.
         *
         * The flag is cleared before evaluating, not after: a device or an
         * ARM interrupt handler can set it while the evaluation runs, and
         * clearing afterwards would discard that.
         */
#if RV_LAZY_IRQ_CHECK
        if (RV_UNLIKELY(h->irq_dirty))
#endif
        {
#if RV_LAZY_IRQ_CHECK
            h->irq_dirty = false;
#endif
            const rv_exc_t irq = rv_hart_pending_irq(h);
            if (RV_UNLIKELY(irq != RV_EXC_NONE)) {
                /*
                 * An interrupt resumes a WFI-parked hart, and mepc must
                 * point at the instruction *after* the WFI so it is not
                 * re-executed on mret.
                 */
                h->state = RV_STATE_RUNNING;
                h->pc = pc;
                rv_hart_trap(h, RV_CAUSE_INTERRUPT | irq, 0u);
                pc = h->pc;
                goto retired_insn;
            }
        }

        /* --- 2. fetch ------------------------------------------------ */
        uint32_t insn;
        unsigned len;
        {
            if (RV_UNLIKELY((pc & TARGET_ALIGN_MASK) != 0u)) {
                /* Every control transfer validates its target, so this can
                 * only happen if something wrote a bad pc directly. */
                TRAP(RV_EXC_INSN_MISALIGNED, pc);
            }

            /*
             * Everything that can refuse a fetch, behind one test.
             *
             * Both of these are almost always false, and this is the
             * hottest sequence in the emulator -- one instruction pays it
             * per fetch whether or not the guest uses either feature.
             * Testing them separately measured 9.3% on CoreMark, which arms
             * neither; folding them into one flag puts the second feature
             * on the first one's branch for nothing.
             */
            if (RV_UNLIKELY(h->fetch_guard)) {
#if RV_EXT_SDTRIG
                /*
                 * An execute trigger fires before the instruction runs, so
                 * mepc points at it rather than past it -- the handler is
                 * expected to step over or resume it deliberately.
                 */
                if (h->trig_active && rv_trig_check(h, pc, RV_ACC_FETCH)) {
                    TRAP(RV_EXC_BREAKPOINT, pc);
                }
#endif
#if RV_EXT_PMP
                if (h->pmp_active &&
                    !rv_pmp_check(h, pc, 2u, RV_ACC_FETCH)) {
                    TRAP(RV_EXC_INSN_ACCESS_FAULT, pc);
                }
#endif
            }

            uint16_t lo;
            rv_exc_t exc = rv_bus_fetch16(h->bus, pc, &lo);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, pc);
            }

            if (rv_is_32bit(lo)) {
                uint16_t hi;
#if RV_EXT_PMP
                /*
                 * The second halfword needs its own check only when it can
                 * fall in a different entry from the first. Every PMP bound
                 * is a multiple of four bytes, so that is exactly when the
                 * instruction is not 4-byte aligned -- which C code almost
                 * never is, so this costs an already-live register test
                 * rather than a second walk.
                 */
                if (RV_UNLIKELY(h->pmp_active && (pc & 2u) != 0u) &&
                    !rv_pmp_check(h, pc + 2u, 2u, RV_ACC_FETCH)) {
                    TRAP(RV_EXC_INSN_ACCESS_FAULT, pc + 2u);
                }
#endif
                exc = rv_bus_fetch16(h->bus, pc + 2u, &hi);
                if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                    TRAP(exc, pc + 2u);
                }
                insn = (uint32_t)lo | ((uint32_t)hi << 16);
                len = 4u;
            } else {
#if RV_EXT_C
                insn = rv_decode_expand_c(lo);
                len = 2u;
                if (RV_UNLIKELY(insn == 0u)) {
                    /* Reserved or unsupported encoding. mtval carries the
                     * original 16-bit instruction, not the expansion. */
                    TRAP(RV_EXC_ILLEGAL_INSN, lo);
                }
#else
                TRAP(RV_EXC_ILLEGAL_INSN, lo);
#endif
            }
        }

#if RV_ENABLE_TRACE
        if (h->trace != NULL) {
            h->trace(h, pc, insn, h->trace_user);
        }
#endif

        next = pc + len;

        /* --- 3. execute ---------------------------------------------- */
        switch ((insn >> 2) & 0x1Fu) {

        /* ---------------- LUI / AUIPC ---------------- */
        case OP_LUI >> 2:
            wr(h, rv_rd(insn), rv_imm_u(insn));
            break;

        case OP_AUIPC >> 2:
            wr(h, rv_rd(insn), pc + rv_imm_u(insn));
            break;

        /* ---------------- jumps ---------------- */
        case OP_JAL >> 2: {
            const uint32_t target = pc + (uint32_t)rv_imm_j(insn);
            if (RV_UNLIKELY((target & TARGET_ALIGN_MASK) != 0u)) {
                TRAP(RV_EXC_INSN_MISALIGNED, target);
            }
            wr(h, rv_rd(insn), next);
            next = target;
            break;
        }

        case OP_JALR >> 2: {
            if (RV_UNLIKELY(rv_funct3(insn) != 0u)) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }
            /* The low bit of the computed target is cleared, not faulted. */
            const uint32_t target =
                (h->x[rv_rs1(insn)] + (uint32_t)rv_imm_i(insn)) & ~1u;
            if (RV_UNLIKELY((target & TARGET_ALIGN_MASK) != 0u)) {
                TRAP(RV_EXC_INSN_MISALIGNED, target);
            }
            /* rd is written after rs1 is read: they may be the same register. */
            wr(h, rv_rd(insn), next);
            next = target;
            break;
        }

        /* ---------------- branches ---------------- */
        case OP_BRANCH >> 2: {
            const uint32_t a = h->x[rv_rs1(insn)];
            const uint32_t b = h->x[rv_rs2(insn)];
            bool taken;

            switch (rv_funct3(insn)) {
            case 0: taken = (a == b); break;                       /* BEQ  */
            case 1: taken = (a != b); break;                       /* BNE  */
            case 4: taken = ((int32_t)a <  (int32_t)b); break;     /* BLT  */
            case 5: taken = ((int32_t)a >= (int32_t)b); break;     /* BGE  */
            case 6: taken = (a <  b); break;                       /* BLTU */
            case 7: taken = (a >= b); break;                       /* BGEU */
            default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            if (taken) {
                const uint32_t target = pc + (uint32_t)rv_imm_b(insn);
                if (RV_UNLIKELY((target & TARGET_ALIGN_MASK) != 0u)) {
                    TRAP(RV_EXC_INSN_MISALIGNED, target);
                }
                next = target;
            }
            break;
        }

        /* ---------------- loads ---------------- */
        case OP_LOAD >> 2: {
            const uint32_t addr = h->x[rv_rs1(insn)] + (uint32_t)rv_imm_i(insn);
            uint32_t size, v;
            bool sx;

            switch (rv_funct3(insn)) {
            case 0: size = 1u; sx = true;  break;   /* LB  */
            case 1: size = 2u; sx = true;  break;   /* LH  */
            case 2: size = 4u; sx = false; break;   /* LW  */
            case 4: size = 1u; sx = false; break;   /* LBU */
            case 5: size = 2u; sx = false; break;   /* LHU */
            default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            const rv_exc_t exc = rv_hart_load(h, addr, size, sx, &v);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, addr);
            }
            wr(h, rv_rd(insn), v);
            break;
        }

        /* ---------------- stores ---------------- */
        case OP_STORE >> 2: {
            const uint32_t addr = h->x[rv_rs1(insn)] + (uint32_t)rv_imm_s(insn);
            const uint32_t v = h->x[rv_rs2(insn)];
            uint32_t size;

            switch (rv_funct3(insn)) {
            case 0: size = 1u; break;   /* SB */
            case 1: size = 2u; break;   /* SH */
            case 2: size = 4u; break;   /* SW */
            default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            const rv_exc_t exc = rv_hart_store(h, addr, size, v);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, addr);
            }
            break;
        }

        /* ---------------- register-immediate ALU ---------------- */
        case OP_IMM >> 2: {
            const uint32_t a = h->x[rv_rs1(insn)];
            const int32_t imm = rv_imm_i(insn);
            const uint32_t rd = rv_rd(insn);

            switch (rv_funct3(insn)) {
            case 0: wr(h, rd, a + (uint32_t)imm); break;                  /* ADDI  */
            case 2: wr(h, rd, (int32_t)a < imm); break;                   /* SLTI  */
            case 3: wr(h, rd, a < (uint32_t)imm); break;                  /* SLTIU */
            case 4: wr(h, rd, a ^ (uint32_t)imm); break;                  /* XORI  */
            case 6: wr(h, rd, a | (uint32_t)imm); break;                  /* ORI   */
            case 7: wr(h, rd, a & (uint32_t)imm); break;                  /* ANDI  */

            case 1: {  /* SLLI, and the Zbb unary ops sharing its slot */
                /*
                 * SLLI is tested first because it is overwhelmingly the
                 * common case: putting the Zbb decode ahead of it made
                 * every shift pay an extra funct7 compare, which cost more
                 * than Zbb saved.
                 */
                const uint32_t f7i = rv_funct7(insn);
                if (RV_LIKELY(f7i == 0u)) {
                    wr(h, rd, a << rv_rs2(insn));
                    break;
                }
#if RV_EXT_ZBB
                if (f7i == 0x30u) {
                    switch (rv_rs2(insn)) {
                    case 0: wr(h, rd, zbb_clz(a)); break;             /* clz    */
                    case 1: wr(h, rd, zbb_ctz(a)); break;             /* ctz    */
                    case 2: wr(h, rd, (uint32_t)__builtin_popcount(a)); break;
                    case 4: wr(h, rd, (uint32_t)(int8_t)a); break;    /* sext.b */
                    case 5: wr(h, rd, (uint32_t)(int16_t)a); break;   /* sext.h */
                    default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
                    }
                    break;
                }
#endif
#if RV_EXT_ZBS
                /* Single-bit immediate forms; shamt is the rs2 field. */
                if (f7i == 0x14u) { wr(h, rd, a | (1u << rv_rs2(insn))); break; }
                if (f7i == 0x24u) { wr(h, rd, a & ~(1u << rv_rs2(insn))); break; }
                if (f7i == 0x34u) { wr(h, rd, a ^ (1u << rv_rs2(insn))); break; }
#endif
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            case 5: {  /* SRLI / SRAI, plus Zbb rori / orc.b / rev8 */
                const uint32_t f7i = rv_funct7(insn);
                if (RV_LIKELY(f7i == 0u)) {
                    wr(h, rd, a >> rv_rs2(insn));
                    break;
                }
                if (f7i == 0x20u) {
                    wr(h, rd, (uint32_t)((int32_t)a >> rv_rs2(insn)));
                    break;
                }
#if RV_EXT_ZBB
                /* orc.b is imm 0x287 and rev8 is 0x698; neither collides
                 * with the shift funct7 values, so one compare each. */
                if (f7i == 0x30u) {
                    wr(h, rd, zbb_ror(a, rv_rs2(insn)));             /* rori  */
                    break;
                }
                if (f7i == 0x14u && rv_rs2(insn) == 7u) {
                    wr(h, rd, zbb_orcb(a));                          /* orc.b */
                    break;
                }
                if (f7i == 0x34u && rv_rs2(insn) == 24u) {
                    wr(h, rd, __builtin_bswap32(a));                 /* rev8  */
                    break;
                }
#endif
#if RV_EXT_ZBS
                if (f7i == 0x24u) {
                    wr(h, rd, (a >> rv_rs2(insn)) & 1u);             /* bexti */
                    break;
                }
#endif
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            default:
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }
            break;
        }

        /* ---------------- register-register ALU ---------------- */
        case OP_OP >> 2: {
            const uint32_t a = h->x[rv_rs1(insn)];
            const uint32_t b = h->x[rv_rs2(insn)];
            const uint32_t rd = rv_rd(insn);
            const uint32_t f7 = rv_funct7(insn);
            const uint32_t f3 = rv_funct3(insn);

            if (f7 == 0u) {
                switch (f3) {
                case 0: wr(h, rd, a + b); break;                        /* ADD  */
                case 1: wr(h, rd, a << (b & 0x1Fu)); break;             /* SLL  */
                case 2: wr(h, rd, (int32_t)a < (int32_t)b); break;      /* SLT  */
                case 3: wr(h, rd, a < b); break;                        /* SLTU */
                case 4: wr(h, rd, a ^ b); break;                        /* XOR  */
                case 5: wr(h, rd, a >> (b & 0x1Fu)); break;             /* SRL  */
                case 6: wr(h, rd, a | b); break;                        /* OR   */
                default: wr(h, rd, a & b); break;                       /* AND  */
                }
            } else if (f7 == 0x20u) {
                if (f3 == 0u) {
                    wr(h, rd, a - b);                                   /* SUB */
                } else if (f3 == 5u) {
                    wr(h, rd, (uint32_t)((int32_t)a >> (b & 0x1Fu)));   /* SRA */
                }
#if RV_EXT_ZBB
                else if (f3 == 7u) { wr(h, rd, a & ~b); }               /* andn */
                else if (f3 == 6u) { wr(h, rd, a | ~b); }               /* orn  */
                else if (f3 == 4u) { wr(h, rd, ~(a ^ b)); }             /* xnor */
#endif
                else {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
            }
#if RV_EXT_ZBB
            else if (f7 == 0x05u) {
                /* Zbb's min/max and Zbc's clmul share this funct7, so they
                 * are decoded together: a separate branch further down the
                 * chain would never be reached. */
                switch (f3) {
                case 4: wr(h, rd, ((int32_t)a < (int32_t)b) ? a : b); break; /* min  */
                case 5: wr(h, rd, (a < b) ? a : b); break;                   /* minu */
                case 6: wr(h, rd, ((int32_t)a > (int32_t)b) ? a : b); break; /* max  */
                case 7: wr(h, rd, (a > b) ? a : b); break;                   /* maxu */
#if RV_EXT_ZBC
                case 1: wr(h, rd, zbc_clmul(a, b)); break;
                case 2: wr(h, rd, zbc_clmulr(a, b)); break;
                case 3: wr(h, rd, zbc_clmulh(a, b)); break;
#endif
                default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
            }
            else if (f7 == 0x30u) {
                if (f3 == 1u)      { wr(h, rd, zbb_rol(a, b)); }        /* rol */
                else if (f3 == 5u) { wr(h, rd, zbb_ror(a, b)); }        /* ror */
                else               { TRAP(RV_EXC_ILLEGAL_INSN, insn); }
            }
            else if (f7 == 0x04u && f3 == 4u && rv_rs2(insn) == 0u) {
                wr(h, rd, a & 0xFFFFu);                                 /* zext.h */
            }
#endif
#if RV_EXT_ZBA
            else if (f7 == 0x10u && (f3 == 2u || f3 == 4u || f3 == 6u)) {
                /* sh1add / sh2add / sh3add: rd = (rs1 << n) + rs2 */
                wr(h, rd, (a << (f3 >> 1)) + b);
            }
#endif
#if RV_EXT_ZBS
            else if (f7 == 0x14u && f3 == 1u) { wr(h, rd, a | (1u << (b & 31u))); }
            else if (f7 == 0x24u && f3 == 1u) { wr(h, rd, a & ~(1u << (b & 31u))); }
            else if (f7 == 0x34u && f3 == 1u) { wr(h, rd, a ^ (1u << (b & 31u))); }
            else if (f7 == 0x24u && f3 == 5u) { wr(h, rd, (a >> (b & 31u)) & 1u); }
#endif
#if RV_EXT_M
            else if (f7 == 1u) {
                switch (f3) {
                case 0: wr(h, rd, a * b); break;                        /* MUL    */
                case 1: wr(h, rd, mulh_ss((int32_t)a, (int32_t)b)); break;  /* MULH   */
                case 2: wr(h, rd, mulh_su((int32_t)a, b)); break;       /* MULHSU */
                case 3: wr(h, rd, mulh_uu(a, b)); break;                /* MULHU  */
                case 4: wr(h, rd, div_s((int32_t)a, (int32_t)b)); break;/* DIV    */
                case 5: wr(h, rd, div_u(a, b)); break;                  /* DIVU   */
                case 6: wr(h, rd, rem_s((int32_t)a, (int32_t)b)); break;/* REM    */
                default: wr(h, rd, rem_u(a, b)); break;                 /* REMU   */
                }
            }
#endif
            else {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }
            break;
        }

        /* ---------------- fences and cache blocks ---------------- */
        case OP_MISC_MEM >> 2: {
            const uint32_t f3 = rv_funct3(insn);

            /*
             * A single hart with no store buffer and no caches between the
             * core and its devices: FENCE is architecturally a no-op.
             * FENCE.I still matters, because a JIT backend must discard
             * translations for code the guest just wrote.
             */
            if (f3 == 1u) {
                rv_invalidate(h, 0u, 0xFFFFFFFFu);
                break;
            }

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ
            if (f3 == 2u) {
                /*
                 * CBO.*: the operation is selected by the whole 12-bit
                 * immediate, and rd must be zero. The address is any byte
                 * in the block; the block itself is what is operated on.
                 */
                const uint32_t op = insn >> 20;
                if (RV_UNLIKELY(rv_rd(insn) != 0u || !rv_cbo_valid(op))) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }

                uint32_t fault_addr;
                const rv_exc_t exc = rv_hart_cbo(h, op, h->x[rv_rs1(insn)],
                                                 &fault_addr);
                if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                    TRAP(exc, fault_addr);
                }
                break;
            }
#endif /* RV_EXT_ZICBOM || RV_EXT_ZICBOZ */

            /* Plain FENCE (f3 == 0) and any other encoding: no-op. */
            break;
        }

        /* ---------------- atomics ---------------- */
#if RV_EXT_A
        case OP_AMO >> 2: {
            const uint32_t funct5 = rv_funct7(insn) >> 2;

#if RV_EXT_ZACAS
            if (rv_funct3(insn) == 3u && funct5 == RV_AMO_CAS) {
                /* amocas.d: even-odd register pairs, so odd operands are
                 * not encodable and must raise illegal instruction. */
                if (RV_UNLIKELY((rv_rd(insn) & 1u) || (rv_rs2(insn) & 1u))) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
                const uint32_t daddr = h->x[rv_rs1(insn)];
                const rv_exc_t dexc =
                    rv_hart_amocas_d(h, rv_rd(insn), rv_rs2(insn), daddr);
                if (RV_UNLIKELY(dexc != RV_EXC_NONE)) {
                    TRAP(dexc, daddr);
                }
                h->x[0] = 0u;
                break;
            }
#endif
            if (RV_UNLIKELY(rv_funct3(insn) != 2u)) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);   /* only 32/64-bit AMOs */
            }

            if (RV_UNLIKELY(!rv_amo_valid(funct5))) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }
            /* LR takes no source operand; a non-zero rs2 is not an LR. */
            if (RV_UNLIKELY(funct5 == RV_AMO_LR && rv_rs2(insn) != 0u)) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            const uint32_t addr = h->x[rv_rs1(insn)];
            const rv_exc_t exc = rv_hart_amo(h, funct5, rv_rd(insn), addr,
                                             h->x[rv_rs2(insn)]);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, addr);
            }
            h->x[0] = 0u;   /* rv_hart_amo skips rd==0; keep x0 canonical */
            break;
        }
#endif /* RV_EXT_A */

        /* ---------------- system ---------------- */
        case OP_SYSTEM >> 2: {
            const uint32_t f3 = rv_funct3(insn);

            if (f3 == 0u) {
                /* Privileged instructions: rd and rs1 must both be zero. */
                if (RV_UNLIKELY(rv_rd(insn) != 0u || rv_rs1(insn) != 0u)) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }

                switch (insn >> 20) {
                case 0x000u:                    /* ECALL */
#if RV_ENABLE_ECALL_HOOK
                    if (h->ecall != NULL) {
                        h->pc = pc;
                        if (h->ecall(h, h->ecall_user)) {
                            /* Consumed by the platform: retire normally. */
                            pc = next;
                            goto retired_insn;
                        }
                    }
#endif
#if RV_EXT_U
                    TRAP((h->priv == RV_PRIV_U) ? RV_EXC_ECALL_U
                                                : RV_EXC_ECALL_M, 0u);
#else
                    TRAP(RV_EXC_ECALL_M, 0u);
#endif

                case 0x001u:                    /* EBREAK */
                    TRAP(RV_EXC_BREAKPOINT, pc);

                case 0x302u: {                  /* MRET */
                    /*
                     * Pop the interrupt-enable stack. MPP selects the
                     * privilege to return to, and is then reset to the
                     * least-privileged supported mode as the spec requires.
                     */
                    const uint32_t mpie =
                        (h->mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0u;
                    uint32_t clear = MSTATUS_MIE | MSTATUS_MPIE |
                                     MSTATUS_MPP_MASK;
#if RV_EXT_U
                    const uint32_t mpp =
                        (h->mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT;
                    /* MPP is WARL over the supported modes; S is not one. */
                    const uint32_t back =
                        (mpp == RV_PRIV_U) ? RV_PRIV_U : RV_PRIV_M;
                    /*
                     * Returning below M clears MPRV. Without this, M-mode
                     * could leave data accesses borrowing U's privilege and
                     * that setting would outlive the return -- the spec
                     * closes the hole here rather than trusting software to.
                     */
                    if (back != RV_PRIV_M) {
                        clear |= MSTATUS_MPRV;
                    }
#else
                    const uint32_t back = RV_PRIV_M;
#endif
                    h->mstatus = (h->mstatus & ~clear)
                               | mpie
                               | MSTATUS_MPIE
                               | ((uint32_t)RV_PRIV_LEAST << MSTATUS_MPP_SHIFT);
                    h->priv = (uint8_t)back;
#if RV_EXT_U && RV_EXT_PMP
                    /* See rv_hart_trap: the predicate depends on privilege. */
                    rv_pmp_refresh(h);
#endif
#if RV_LAZY_IRQ_CHECK
                    /* MIE was just restored from MPIE. */
                    h->irq_dirty = true;
#endif
                    next = h->mepc;
                    break;
                }

                case 0x105u:                    /* WFI */
                    /*
                     * Implemented as a hint that parks the hart. Retiring
                     * it first means mepc points past the WFI when the
                     * wake-up interrupt is taken.
                     */
                    h->state = RV_STATE_WFI;
                    break;

                default:
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
                break;
            }

#if RV_EXT_ZICSR
            if (RV_UNLIKELY(f3 == 4u)) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);   /* not a CSR encoding */
            }
            {
                const uint32_t csr = insn >> 20;
                const uint32_t rd = rv_rd(insn);
                const uint32_t rs1 = rv_rs1(insn);
                /* The immediate forms take the value from the rs1 field. */
                const uint32_t src = (f3 & 4u) ? rs1 : h->x[rs1];
                const bool is_write = (f3 & 3u) == 1u;       /* CSRRW/CSRRWI */
                uint32_t old = 0u;

                /*
                 * CSRRW with rd==x0 must not read the CSR, and CSRRS/CSRRC
                 * with an all-zero source must not write it. Both matter
                 * for CSRs with read or write side effects. The "source is
                 * zero" test is on the rs1 field either way: a register
                 * number for the register forms, the immediate itself for
                 * the immediate forms.
                 */
                const bool do_read = !(is_write && rd == 0u);
                const bool do_write = is_write || (rs1 != 0u);

                if (do_read) {
                    const rv_exc_t exc = rv_csr_read(h, csr, &old);
                    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                        TRAP(exc, insn);
                    }
                }

                if (do_write) {
                    uint32_t nv;
                    switch (f3 & 3u) {
                    case 1: nv = src; break;              /* CSRRW  */
                    case 2: nv = old | src; break;        /* CSRRS  */
                    default: nv = old & ~src; break;      /* CSRRC  */
                    }
                    const rv_exc_t exc = rv_csr_write(h, csr, nv);
                    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                        TRAP(exc, insn);
                    }
#if RV_EXT_ZICNTR
                    switch (csr) {
                    case CSR_MCYCLE:
                    case CSR_MCYCLEH:
                        ctr_written |= CTR_CYCLE;
                        break;
                    case CSR_MINSTRET:
                    case CSR_MINSTRETH:
                        ctr_written |= CTR_INSTRET;
                        break;
                    default:
                        break;
                    }
#endif
                }

                wr(h, rd, old);
            }
            break;
#else
            TRAP(RV_EXC_ILLEGAL_INSN, insn);
#endif
        }

#if RV_EXT_F
        /*
         * All of F routes to one place: OP-FP, the four fused multiply-adds
         * and FLW/FSW. Keeping it out of this switch means a JIT helper can
         * share the same implementation rather than restate the rounding
         * and flag rules.
         */
        case 0x07u >> 2:      /* OP-LOAD-FP  */
        case 0x27u >> 2:      /* OP-STORE-FP */
        case 0x43u >> 2:      /* FMADD.S     */
        case 0x47u >> 2:      /* FMSUB.S     */
        case 0x4Bu >> 2:      /* FNMSUB.S    */
        case 0x4Fu >> 2:      /* FNMADD.S    */
        case 0x53u >> 2: {    /* OP-FP       */
            uint32_t ftval = insn;
            const rv_exc_t exc = rv_hart_fp(h, insn, &ftval);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, ftval);
            }
            break;
        }
#endif

        default:
            TRAP(RV_EXC_ILLEGAL_INSN, insn);
        }

        pc = next;

    retired_insn:
        done++;
#if RV_EXT_ZICNTR
        if (RV_LIKELY(((h->mcountinhibit & 0x1u) | (ctr_written & CTR_CYCLE)) == 0u)) {
            h->mcycle++;
        }
        if (RV_LIKELY(((h->mcountinhibit & 0x4u) |
                       ((ctr_written & CTR_INSTRET) >> 1)) == 0u)) {
            h->minstret++;
        }
#endif
#undef CTR_CYCLE
#undef CTR_INSTRET
    }

#undef TRAP

    h->pc = pc;
#if RV_ENABLE_STATS
    h->insn_retired_lo += done;
#endif
    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}

/* ------------------------------------------------------------------ */
/* Backend plumbing                                                    */
/* ------------------------------------------------------------------ */

static void interp_reset(rv_hart_t *h)
{
    (void)h;   /* no translation state to discard */
}

const rv_backend_t rv_backend_interp = {
    .name       = "interp",
    .init       = NULL,
    .reset      = interp_reset,
    .run        = interp_run,
    .invalidate = NULL,
};

/* Platforms may override this; the interpreter is the default. */
const rv_backend_t *rv_backend = &rv_backend_interp;

rv_run_reason_t rv_step(rv_hart_t *h)
{
    return rv_backend->run(h, 1u, NULL);
}
