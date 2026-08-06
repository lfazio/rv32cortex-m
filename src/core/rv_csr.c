/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_csr.c - Machine-mode control and status registers.
 *
 * Only the CSRs this core actually implements are
 * implemented. Anything else raises an illegal-instruction exception, which
 * is the architecturally correct response to an access to a non-existent
 * CSR and is also how guest software probes for optional features.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#if RV_EXT_ZICSR

/* ------------------------------------------------------------------ */

/* csr[11:10] == 0b11 marks a read-only CSR. */
static RV_ALWAYS_INLINE bool csr_is_readonly(uint32_t csr)
{
    return ((csr >> 10) & 0x3u) == 0x3u;
}

/* csr[9:8] is the lowest privilege level allowed to access it. */
static RV_ALWAYS_INLINE uint32_t csr_min_priv(uint32_t csr)
{
    return (csr >> 8) & 0x3u;
}

static uint64_t read_time(const rv_hart_t *h)
{
    /*
     * `time` is architecturally the memory-mapped CLINT counter, not hart
     * state. If no timer is wired up, fall back to the cycle counter so
     * guests that busy-wait on time still make progress.
     */
    if (h->mtime != NULL) {
        return *h->mtime;
    }
    return h->mcycle;
}

/*
 * mstatus as software sees it.
 *
 * SD is read-only and summarises the extension state fields: it reads as
 * one exactly when some of them is Dirty. It is not stored, because
 * storing it would mean every write to FS had to remember to maintain it,
 * and the one place that cannot forget is the read itself.
 */
static uint32_t mstatus_read(const rv_hart_t *h)
{
#if RV_EXT_F
    if ((h->mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_MASK) {
        return h->mstatus | MSTATUS_SD;
    }
#endif
    return h->mstatus & ~MSTATUS_SD;
}

/*
 * May the current privilege read this unprivileged counter shadow?
 *
 * mcounteren gates S-mode, and scounteren gates U-mode *in addition* --
 * a supervisor can withhold from its users what M-mode granted it, but
 * cannot grant what it was not given. M-mode is never gated.
 */
static bool counter_enabled(const rv_hart_t *h, uint32_t csr)
{
    if (h->priv == RV_PRIV_M) {
        return true;
    }
    /* cycle/time/instret are bits 0/1/2 of both registers. */
    const uint32_t bit = 1u << (csr & 3u);

    if ((h->mcounteren & bit) == 0u) {
        return false;
    }
#if RV_EXT_S
    if (h->priv == RV_PRIV_U && (h->scounteren & bit) == 0u) {
        return false;
    }
#endif
    return true;
}

/* ------------------------------------------------------------------ */
/* Read                                                                */
/* ------------------------------------------------------------------ */

rv_exc_t rv_csr_read(rv_hart_t *h, uint32_t csr, uint32_t *out)
{
    if (RV_UNLIKELY(csr_min_priv(csr) > h->priv)) {
        return RV_EXC_ILLEGAL_INSN;
    }

    switch (csr) {
#if RV_EXT_F
    case CSR_FFLAGS:        *out = h->fcsr & 0x1Fu; break;
    case CSR_FRM:           *out = (h->fcsr >> 5) & 0x7u; break;
    case CSR_FCSR:          *out = h->fcsr & 0xFFu; break;
#endif

    /* --- machine information --- */
    case CSR_MVENDORID:     *out = RV_MVENDORID; break;
    case CSR_MARCHID:       *out = RV_MARCHID;   break;
    case CSR_MIMPID:        *out = RV_MIMPID;    break;
    case CSR_MHARTID:       *out = h->hartid;    break;
    case CSR_MCONFIGPTR:    *out = 0u;           break;

    /* --- trap setup --- */
    case CSR_MSTATUS:       *out = mstatus_read(h);              break;
    case CSR_MSTATUSH:      *out = 0u;           break;
    case CSR_MISA:          *out = rv_hart_misa(); break;
    case CSR_MIE:           *out = h->mie;       break;
    case CSR_MTVEC:         *out = h->mtvec;     break;
#if RV_EXT_S
    case CSR_MEDELEG:       *out = h->medeleg;   break;
    case CSR_MIDELEG:       *out = h->mideleg;   break;
#else
    /* Nothing to delegate to, so both read as zero (WARL). */
    case CSR_MEDELEG:
    case CSR_MIDELEG:       *out = 0u;           break;
#endif
    case CSR_MCOUNTEREN:    *out = h->mcounteren; break;
    case CSR_MENVCFG:       *out = h->menvcfg;    break;
    /* The high half is entirely fields of extensions this core lacks. */
    case CSR_MENVCFGH:      *out = 0u;            break;

#if RV_EXT_S
    /* --- supervisor trap setup --- */
    case CSR_SSTATUS:       *out = mstatus_read(h) & SSTATUS_RMASK; break;
    /*
     * sie and sip show only the delegated interrupts. An interrupt M-mode
     * has kept is not the supervisor's to see, let alone to mask.
     */
    case CSR_SIE:           *out = h->mie & h->mideleg;          break;
    case CSR_SIP:           *out = h->mip & h->mideleg;          break;
    case CSR_STVEC:         *out = h->stvec;     break;
    case CSR_SCOUNTEREN:    *out = h->scounteren; break;
    case CSR_SENVCFG:       *out = h->senvcfg;    break;

    /* --- supervisor trap handling --- */
    case CSR_SSCRATCH:      *out = h->sscratch;  break;
    case CSR_SEPC:          *out = h->sepc;      break;
    case CSR_SCAUSE:        *out = h->scause;    break;
    case CSR_STVAL:         *out = h->stval;     break;

    case CSR_SATP:
#if RV_EXT_SV32
        /*
         * TVM lets M-mode trap a supervisor touching address translation,
         * which is the hook a hypervisor needs. It covers reads as well as
         * writes, and never applies to M-mode itself.
         */
        if (h->priv == RV_PRIV_S && (h->mstatus & MSTATUS_TVM) != 0u) {
            return RV_EXC_ILLEGAL_INSN;
        }
        *out = h->satp;
#else
        /* Bare is the only mode, so satp has no state -- but it must still
         * read rather than trap: software reads it to discover what
         * translation exists, and an illegal instruction is not an answer
         * to that question. */
        *out = 0u;
#endif
        break;
#endif

    /* --- trap handling --- */
    case CSR_MSCRATCH:      *out = h->mscratch;  break;
    case CSR_MEPC:          *out = h->mepc;      break;
    case CSR_MCAUSE:        *out = h->mcause;    break;
    case CSR_MTVAL:         *out = h->mtval;     break;
    case CSR_MIP:           *out = h->mip;       break;

#if RV_EXT_SDTRIG
    /* --- debug triggers --- */
    case CSR_TSELECT: *out = h->tselect; break;
    case CSR_TDATA1:
        *out = (h->tselect < RV_TRIG_COUNT) ? h->tdata1[h->tselect] : 0u;
        break;
    case CSR_TDATA2:
        *out = (h->tselect < RV_TRIG_COUNT) ? h->tdata2[h->tselect] : 0u;
        break;
    case CSR_TDATA3: *out = 0u; break;
    case CSR_TINFO:
        /* Bit n set means trigger type n is supported; only mcontrol. */
        *out = 1u << 2;
        break;
#endif

#if RV_EXT_PMP
    /* --- physical memory protection --- */
    case CSR_PMPCFG0: case CSR_PMPCFG0 + 1: case CSR_PMPCFG0 + 2:
    case CSR_PMPCFG0 + 3:
        *out = h->pmpcfg[csr - CSR_PMPCFG0];
        break;
    default:
        if (csr >= CSR_PMPADDR0 && csr < CSR_PMPADDR0 + RV_PMP_ENTRIES) {
            *out = h->pmpaddr[csr - CSR_PMPADDR0];
            break;
        }
        return RV_EXC_ILLEGAL_INSN;
#endif

    /* --- counters --- */
#if RV_EXT_ZICNTR
    /*
     * The unprivileged shadows are readable only where mcounteren (for
     * S-mode) and then scounteren (for U-mode) allow it. The machine
     * numbers above have their own privilege from csr_min_priv and are not
     * affected; this is only about cycle/time/instret.
     */
    case CSR_CYCLE: case CSR_CYCLEH:
    case CSR_TIME:  case CSR_TIMEH:
    case CSR_INSTRET: case CSR_INSTRETH:
        if (!counter_enabled(h, csr)) {
            return RV_EXC_ILLEGAL_INSN;
        }
        switch (csr) {
        case CSR_CYCLE:    *out = (uint32_t)h->mcycle;           break;
        case CSR_CYCLEH:   *out = (uint32_t)(h->mcycle >> 32);   break;
        case CSR_INSTRET:  *out = (uint32_t)h->minstret;         break;
        case CSR_INSTRETH: *out = (uint32_t)(h->minstret >> 32); break;
        case CSR_TIME:     *out = (uint32_t)read_time(h);        break;
        default:           *out = (uint32_t)(read_time(h) >> 32); break;
        }
        break;

    case CSR_MCYCLE:        *out = (uint32_t)h->mcycle;          break;
    case CSR_MCYCLEH:       *out = (uint32_t)(h->mcycle >> 32);  break;
    case CSR_MINSTRET:      *out = (uint32_t)h->minstret;        break;
    case CSR_MINSTRETH:     *out = (uint32_t)(h->minstret >> 32); break;
    case CSR_MCOUNTINHIBIT: *out = h->mcountinhibit;              break;
#endif

    }

    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* Write                                                               */
/* ------------------------------------------------------------------ */

rv_exc_t rv_csr_write(rv_hart_t *h, uint32_t csr, uint32_t val)
{
    if (RV_UNLIKELY(csr_min_priv(csr) > h->priv || csr_is_readonly(csr))) {
        return RV_EXC_ILLEGAL_INSN;
    }

    switch (csr) {
#if RV_EXT_F
    case CSR_FFLAGS:
        h->fcsr = (h->fcsr & ~0x1Fu) | (val & 0x1Fu);
        break;
    case CSR_FRM:
        h->fcsr = (h->fcsr & 0x1Fu) | ((val & 0x7u) << 5);
        break;
    case CSR_FCSR:
        h->fcsr = val & 0xFFu;
        break;
#endif

    case CSR_MSTATUS:
        /* WARL: keep the bits we implement, ignore the rest. */
        h->mstatus = (h->mstatus & ~MSTATUS_WMASK) | (val & MSTATUS_WMASK);
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;   /* MIE may have been set */
#endif
#if RV_EXT_U && RV_EXT_PMP
        /*
         * MPRV and MPP together decide the privilege a load or store is
         * checked at, so writing mstatus can arm PMP without touching a
         * single pmpcfg. Missing this leaves the access path skipping the
         * check that MPRV just made necessary.
         */
        rv_pmp_refresh(h);
#endif
#if RV_EXT_SV32
        /* MPRV and MPP decide the privilege data accesses translate at, so
         * writing mstatus can start or stop translation on its own. */
        rv_mmu_refresh(h);
#endif
        break;

    case CSR_MSTATUSH:
        break;   /* all implemented fields are read-only zero */

#if RV_EXT_S
    case CSR_SSTATUS:
        /*
         * A view, not a register: only the S-visible bits move, and the
         * machine bits sharing the word are left exactly as they were.
         * Writing mstatus wholesale here would let a supervisor clear MIE.
         */
        h->mstatus = (h->mstatus & ~SSTATUS_WMASK) | (val & SSTATUS_WMASK);
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;   /* SIE may have been set */
#endif
        break;

    case CSR_SIE:
        /* Only the delegated bits, and only those, are the supervisor's. */
        h->mie = (h->mie & ~h->mideleg) | (val & h->mideleg & MIE_WMASK);
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;
#endif
        break;

    case CSR_SIP:
        /*
         * Of the delegated interrupts a supervisor may only clear its own
         * software one; the timer and external bits belong to whatever
         * raises them.
         */
        h->mip = (h->mip & ~(h->mideleg & MIP_SSIP))
               | (val & h->mideleg & MIP_SSIP);
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;
#endif
        break;

    case CSR_STVEC:
        /* Same WARL rule as mtvec: only direct and vectored exist. */
        if ((val & MTVEC_MODE_MASK) <= MTVEC_MODE_VECTORED) {
            h->stvec = val & ~2u;
        }
        break;

    case CSR_SCOUNTEREN:  h->scounteren = val & 0x7u; break;
    case CSR_SENVCFG:     h->senvcfg = val & ENVCFG_WMASK; break;
    case CSR_SSCRATCH:    h->sscratch = val;          break;
    case CSR_SEPC:        h->sepc = val & ~1u;        break;
    case CSR_SCAUSE:      h->scause = val;            break;
    case CSR_STVAL:       h->stval = val;             break;

    case CSR_SATP:
#if RV_EXT_SV32
        if (h->priv == RV_PRIV_S && (h->mstatus & MSTATUS_TVM) != 0u) {
            return RV_EXC_ILLEGAL_INSN;
        }
        /*
         * MODE is WARL over {Bare, Sv32}; on RV32 those are the only two
         * encodings, so every value is legal and none has to be rejected.
         * Changing the root table or the ASID invalidates everything
         * cached -- the conservative reading of the spec's "no ordering
         * guarantee without SFENCE.VMA", and cheaper than being clever
         * about it.
         */
        h->satp = val & (SATP_MODE_SV32 | SATP_ASID_MASK | SATP_PPN_MASK);
        rv_mmu_flush(h);
        rv_mmu_refresh(h);
#endif
        break;

    case CSR_MEDELEG:
        h->medeleg = val & MEDELEG_WMASK;
        break;

    case CSR_MIDELEG:
        h->mideleg = val & MIDELEG_WMASK;
#if RV_LAZY_IRQ_CHECK
        /* Moving an interrupt between M and S changes who may take it. */
        h->irq_dirty = true;
#endif
        break;
#endif /* RV_EXT_S */

    case CSR_MCOUNTEREN:
        h->mcounteren = val & 0x7u;
        break;

    case CSR_MENVCFG:
        h->menvcfg = val & ENVCFG_WMASK;
        break;
    case CSR_MENVCFGH:
        break;   /* every field belongs to an absent extension */

    case CSR_MISA:
        break;   /* the extension set is fixed at build time */

    case CSR_MIE:
        h->mie = val & MIE_WMASK;
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;
#endif
        break;

    case CSR_MTVEC:
        /*
         * Modes other than direct (0) and vectored (1) are reserved; WARL
         * lets us keep the previous mode rather than adopt a bad one.
         */
        if ((val & MTVEC_MODE_MASK) > MTVEC_MODE_VECTORED) {
            val = (val & ~MTVEC_MODE_MASK) | (h->mtvec & MTVEC_MODE_MASK);
        }
        h->mtvec = val;
        break;

#if !RV_EXT_S
    case CSR_MEDELEG:
    case CSR_MIDELEG:
        break;   /* nothing to delegate to; hardwired zero */
#endif

    case CSR_MSCRATCH:
        h->mscratch = val;
        break;

    case CSR_MEPC:
        /*
         * mepc always holds a legal instruction address. With C the low bit
         * is masked; without it, the low two bits are.
         */
#if RV_EXT_C
        h->mepc = val & ~1u;
#else
        h->mepc = val & ~3u;
#endif
        break;

    case CSR_MCAUSE:
        h->mcause = val;
        break;

    case CSR_MTVAL:
        h->mtval = val;
        break;

    case CSR_MIP:
        /*
         * Every implemented mip bit is driven by a device (CLINT, external
         * controller), so software writes are dropped.
         */
        h->mip = (h->mip & ~MIP_WMASK) | (val & MIP_WMASK);
        break;

#if RV_EXT_SDTRIG
    case CSR_TSELECT:
        /* WARL: an out-of-range index leaves the previous selection. */
        if (val < RV_TRIG_COUNT) {
            h->tselect = val;
        }
        break;
    case CSR_TDATA1:
        rv_trig_write_tdata1(h, val);
        break;
    case CSR_TDATA2:
        if (h->tselect < RV_TRIG_COUNT) {
            h->tdata2[h->tselect] = val;
        }
        break;
    case CSR_TDATA3:
        break;
#endif

#if RV_EXT_PMP
    case CSR_PMPCFG0: case CSR_PMPCFG0 + 1: case CSR_PMPCFG0 + 2:
    case CSR_PMPCFG0 + 3: {
        /*
         * A locked entry is locked until reset: neither its cfg byte nor
         * its address register may be changed, which is what makes PMP
         * usable to constrain M-mode itself.
         */
        const uint32_t idx = csr - CSR_PMPCFG0;
        uint32_t cur = h->pmpcfg[idx];
        for (uint32_t b = 0; b < 4u; b++) {
            const uint32_t sh = b * 8u;
            if ((cur >> sh) & 0x80u) {
                continue;                       /* locked byte */
            }
            /* A == 2 (NA4) is reserved when the grain exceeds 4 bytes;
             * this implementation supports it, so nothing is masked out
             * beyond the bits that have no meaning. */
            const uint32_t nb = (val >> sh) & 0x9Fu;
            cur = (cur & ~(0xFFu << sh)) | (nb << sh);
        }
        h->pmpcfg[idx] = cur;
        rv_pmp_refresh(h);
        break;
    }
#endif


#if RV_EXT_ZICNTR
    case CSR_MCYCLE:
        h->mcycle = (h->mcycle & 0xFFFFFFFF00000000ull) | val;
        break;
    case CSR_MCYCLEH:
        h->mcycle = (h->mcycle & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    case CSR_MINSTRET:
        h->minstret = (h->minstret & 0xFFFFFFFF00000000ull) | val;
        break;
    case CSR_MINSTRETH:
        h->minstret = (h->minstret & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    case CSR_MCOUNTINHIBIT:
        /* Only CY (bit 0) and IR (bit 2) exist; TM (bit 1) is hardwired 0. */
        h->mcountinhibit = val & 0x5u;
        break;
#endif

    default:
#if RV_EXT_PMP
        if (csr >= CSR_PMPADDR0 && csr < CSR_PMPADDR0 + RV_PMP_ENTRIES) {
            const uint32_t i = csr - CSR_PMPADDR0;
            /*
             * An address register is read-only while its own entry is
             * locked, and also while the *next* entry is a locked TOR --
             * that entry uses this register as its lower bound, so allowing
             * a write here would let software move a locked range.
             */
            const uint32_t own = (h->pmpcfg[i / 4u] >> ((i % 4u) * 8u)) & 0xFFu;
            if ((own & 0x80u) != 0u) {
                break;
            }
            if (i + 1u < RV_PMP_ENTRIES) {
                const uint32_t j = i + 1u;
                const uint32_t nxt =
                    (h->pmpcfg[j / 4u] >> ((j % 4u) * 8u)) & 0xFFu;
                if ((nxt & 0x80u) != 0u && ((nxt & 0x18u) >> 3) == 1u) {
                    break;
                }
            }
            h->pmpaddr[i] = val;
            break;
        }
#endif
        return RV_EXC_ILLEGAL_INSN;
    }

    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */

bool rv_csr_exists(uint32_t csr)
{
    /* The name table below lists exactly the implemented CSRs. */
    return rv_csr_name(csr) != NULL;
}

const char *rv_csr_name(uint32_t csr)
{
    switch (csr) {
#if RV_EXT_F
    case CSR_FFLAGS:        return "fflags";
    case CSR_FRM:           return "frm";
    case CSR_FCSR:          return "fcsr";
#endif
    case CSR_CYCLE:         return "cycle";
    case CSR_TIME:          return "time";
    case CSR_INSTRET:       return "instret";
    case CSR_CYCLEH:        return "cycleh";
    case CSR_TIMEH:         return "timeh";
    case CSR_INSTRETH:      return "instreth";
    case CSR_MVENDORID:     return "mvendorid";
    case CSR_MARCHID:       return "marchid";
    case CSR_MIMPID:        return "mimpid";
    case CSR_MHARTID:       return "mhartid";
    case CSR_MCONFIGPTR:    return "mconfigptr";
    case CSR_MSTATUS:       return "mstatus";
    case CSR_MISA:          return "misa";
    case CSR_MEDELEG:       return "medeleg";
    case CSR_MIDELEG:       return "mideleg";
    case CSR_MIE:           return "mie";
    case CSR_MTVEC:         return "mtvec";
    case CSR_MCOUNTEREN:    return "mcounteren";
    case CSR_MENVCFG:       return "menvcfg";
    case CSR_MENVCFGH:      return "menvcfgh";
#if RV_EXT_S
    case CSR_SSTATUS:       return "sstatus";
    case CSR_SIE:           return "sie";
    case CSR_STVEC:         return "stvec";
    case CSR_SCOUNTEREN:    return "scounteren";
    case CSR_SENVCFG:       return "senvcfg";
    case CSR_SSCRATCH:      return "sscratch";
    case CSR_SEPC:          return "sepc";
    case CSR_SCAUSE:        return "scause";
    case CSR_STVAL:         return "stval";
    case CSR_SIP:           return "sip";
    case CSR_SATP:          return "satp";
#endif
    case CSR_MSTATUSH:      return "mstatush";
    case CSR_MSCRATCH:      return "mscratch";
    case CSR_MEPC:          return "mepc";
    case CSR_MCAUSE:        return "mcause";
    case CSR_MTVAL:         return "mtval";
    case CSR_MIP:           return "mip";
    case CSR_MCYCLE:        return "mcycle";
    case CSR_MINSTRET:      return "minstret";
    case CSR_MCYCLEH:       return "mcycleh";
    case CSR_MINSTRETH:     return "minstreth";
    case CSR_MCOUNTINHIBIT: return "mcountinhibit";
#if RV_EXT_SDTRIG
    case CSR_TSELECT:       return "tselect";
    case CSR_TDATA1:        return "tdata1";
    case CSR_TDATA2:        return "tdata2";
    case CSR_TDATA3:        return "tdata3";
    case CSR_TINFO:         return "tinfo";
#endif
#if RV_EXT_PMP
    case CSR_PMPCFG0: case CSR_PMPCFG0 + 1: case CSR_PMPCFG0 + 2:
    case CSR_PMPCFG0 + 3: return "pmpcfg";
#endif
    default:
#if RV_EXT_PMP
        if (csr >= CSR_PMPADDR0 && csr < CSR_PMPADDR0 + RV_PMP_ENTRIES) {
            return "pmpaddr";
        }
#endif
        return NULL;
    }
}

#endif /* RV_EXT_ZICSR */
