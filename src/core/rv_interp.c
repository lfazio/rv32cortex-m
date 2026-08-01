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

static rv_run_reason_t interp_run(rv_hart_t *h, uint32_t budget,
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

        if (RV_UNLIKELY(h->state == RV_STATE_HALTED)) {
            reason = RV_RUN_HALTED;
            break;
        }

        /* --- 1. interrupts ------------------------------------------- */
        {
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
            if (RV_UNLIKELY(h->state == RV_STATE_WFI)) {
                /*
                 * Nothing pending. Hand control back so the host can run
                 * its own timers and devices; re-entering will retry.
                 */
                reason = RV_RUN_WFI;
                break;
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

            uint16_t lo;
            rv_exc_t exc = rv_bus_fetch16(h->bus, pc, &lo);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, pc);
            }

            if (rv_is_32bit(lo)) {
                uint16_t hi;
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

            case 1:  /* SLLI */
                if (RV_UNLIKELY(rv_funct7(insn) != 0u)) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
                wr(h, rd, a << rv_rs2(insn));
                break;

            case 5:  /* SRLI / SRAI */
                if (rv_funct7(insn) == 0u) {
                    wr(h, rd, a >> rv_rs2(insn));
                } else if (rv_funct7(insn) == 0x20u) {
                    wr(h, rd, (uint32_t)((int32_t)a >> rv_rs2(insn)));
                } else {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
                break;

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
                } else {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }
            }
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
                 * in the block; the block itself is what gets operated on.
                 */
                if (RV_UNLIKELY(rv_rd(insn) != 0u)) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
                }

                const uint32_t base =
                    h->x[rv_rs1(insn)] & ~(RV_CACHE_BLOCK_SIZE - 1u);

                switch (insn >> 20) {
#if RV_EXT_ZICBOZ
                case 4u: {   /* cbo.zero */
                    /*
                     * Unlike the maintenance operations, this one has an
                     * architecturally visible effect and must go through
                     * the permission-checked store path.
                     */
                    for (uint32_t i = 0; i < RV_CACHE_BLOCK_SIZE; i += 4u) {
                        const rv_exc_t exc = rv_hart_store(h, base + i, 4u, 0u);
                        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                            TRAP(exc, base + i);
                        }
                    }
                    break;
                }
#endif
#if RV_EXT_ZICBOM
                case 0u:     /* cbo.inval */
                case 1u:     /* cbo.clean */
                case 2u: {   /* cbo.flush */
                    /*
                     * Maintenance applies to the host memory backing the
                     * block, so a guest cleaning a DMA buffer cleans the
                     * ARM cache lines that really hold it.
                     */
                    void *host = rv_bus_host_ptr(h->bus, base,
                                                 RV_CACHE_BLOCK_SIZE);
                    if (RV_UNLIKELY(host == NULL)) {
                        TRAP(RV_EXC_STORE_ACCESS_FAULT, base);
                    }
                    if (h->cache != NULL && h->cache->maint != NULL) {
                        static const rv_cbo_op_t map[3] = {
                            RV_CBO_INVAL, RV_CBO_CLEAN, RV_CBO_FLUSH
                        };
                        h->cache->maint(h->cache->ctx, host,
                                        RV_CACHE_BLOCK_SIZE,
                                        map[insn >> 20]);
                    }
                    /* No cache maintenance configured: retiring without
                     * doing anything is architecturally legal. */
                    break;
                }
#endif
                default:
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);
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
            if (RV_UNLIKELY(rv_funct3(insn) != 2u)) {
                TRAP(RV_EXC_ILLEGAL_INSN, insn);   /* only 32-bit AMOs on RV32 */
            }

            const uint32_t addr = h->x[rv_rs1(insn)];
            const uint32_t src = h->x[rv_rs2(insn)];
            const uint32_t rd = rv_rd(insn);
            const uint32_t funct5 = rv_funct7(insn) >> 2;

            /* All AMOs require a naturally aligned address. */
            if (RV_UNLIKELY((addr & 3u) != 0u)) {
                TRAP(funct5 == 0x02u ? RV_EXC_LOAD_MISALIGNED
                                     : RV_EXC_STORE_MISALIGNED, addr);
            }

            if (funct5 == 0x02u) {           /* LR.W */
                if (RV_UNLIKELY(rv_rs2(insn) != 0u)) {
                    TRAP(RV_EXC_ILLEGAL_INSN, insn);   /* rs2 must be zero */
                }
                uint32_t v;
                const rv_exc_t exc = rv_bus_read(h->bus, addr, 4u, &v);
                if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                    TRAP(exc, addr);
                }
                h->resv_addr = addr;
                h->resv_valid = true;
                wr(h, rd, v);
                break;
            }

            if (funct5 == 0x03u) {           /* SC.W */
                if (!h->resv_valid || h->resv_addr != addr) {
                    wr(h, rd, 1u);           /* non-zero: store did not occur */
                    break;
                }
                const rv_exc_t exc = rv_bus_write(h->bus, addr, 4u, src);
                if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                    h->resv_valid = false;
                    TRAP(exc, addr);
                }
                h->resv_valid = false;
                wr(h, rd, 0u);               /* zero: success */
                break;
            }

            /* Read-modify-write AMOs. */
            uint32_t old;
            rv_exc_t exc = rv_bus_read(h->bus, addr, 4u, &old);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                /* An AMO reports load and store faults alike as store. */
                TRAP(RV_EXC_STORE_ACCESS_FAULT, addr);
            }

            uint32_t val;
            switch (funct5) {
            case 0x00u: val = old + src; break;                     /* AMOADD  */
            case 0x01u: val = src; break;                           /* AMOSWAP */
            case 0x04u: val = old ^ src; break;                     /* AMOXOR  */
            case 0x08u: val = old | src; break;                     /* AMOOR   */
            case 0x0Cu: val = old & src; break;                     /* AMOAND  */
            case 0x10u: val = ((int32_t)old < (int32_t)src) ? old : src; break;  /* AMOMIN  */
            case 0x14u: val = ((int32_t)old > (int32_t)src) ? old : src; break;  /* AMOMAX  */
            case 0x18u: val = (old < src) ? old : src; break;       /* AMOMINU */
            case 0x1Cu: val = (old > src) ? old : src; break;       /* AMOMAXU */
            default: TRAP(RV_EXC_ILLEGAL_INSN, insn);
            }

            exc = rv_bus_write(h->bus, addr, 4u, val);
            if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
                TRAP(exc, addr);
            }
            /* rd gets the value read, after the write, in case rd == rs2. */
            wr(h, rd, old);
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
                    TRAP(RV_EXC_ECALL_M, 0u);

                case 0x001u:                    /* EBREAK */
                    TRAP(RV_EXC_BREAKPOINT, pc);

                case 0x302u: {                  /* MRET */
                    /*
                     * Pop the interrupt-enable stack. MPP would select the
                     * privilege to return to; with M-mode only it stays M
                     * and is reset to M (the least-privileged supported
                     * mode) as the spec requires.
                     */
                    const uint32_t mpie =
                        (h->mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0u;
                    h->mstatus = (h->mstatus & ~(MSTATUS_MIE | MSTATUS_MPIE |
                                                 MSTATUS_MPP_MASK))
                               | mpie
                               | MSTATUS_MPIE
                               | ((uint32_t)RV_PRIV_M << MSTATUS_MPP_SHIFT);
                    h->priv = RV_PRIV_M;
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
