/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_csr.c - Machine-mode control and status registers.
 *
 * Only the CSRs that a machine-mode-only RV32IMAC core actually needs are
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

/* ------------------------------------------------------------------ */
/* Read                                                                */
/* ------------------------------------------------------------------ */

rv_exc_t rv_csr_read(rv_hart_t *h, uint32_t csr, uint32_t *out)
{
    if (RV_UNLIKELY(csr_min_priv(csr) > h->priv)) {
        return RV_EXC_ILLEGAL_INSN;
    }

    switch (csr) {
    /* --- machine information --- */
    case CSR_MVENDORID:     *out = RV_MVENDORID; break;
    case CSR_MARCHID:       *out = RV_MARCHID;   break;
    case CSR_MIMPID:        *out = RV_MIMPID;    break;
    case CSR_MHARTID:       *out = h->hartid;    break;
    case CSR_MCONFIGPTR:    *out = 0u;           break;

    /* --- trap setup --- */
    case CSR_MSTATUS:       *out = h->mstatus;   break;
    case CSR_MSTATUSH:      *out = 0u;           break;
    case CSR_MISA:          *out = rv_hart_misa(); break;
    case CSR_MIE:           *out = h->mie;       break;
    case CSR_MTVEC:         *out = h->mtvec;     break;
    /*
     * No S or U mode, so nothing can be delegated and no counters can be
     * enabled for a lower privilege level. These read as zero (WARL).
     */
    case CSR_MEDELEG:
    case CSR_MIDELEG:
    case CSR_MCOUNTEREN:    *out = 0u;           break;

    /* --- trap handling --- */
    case CSR_MSCRATCH:      *out = h->mscratch;  break;
    case CSR_MEPC:          *out = h->mepc;      break;
    case CSR_MCAUSE:        *out = h->mcause;    break;
    case CSR_MTVAL:         *out = h->mtval;     break;
    case CSR_MIP:           *out = h->mip;       break;

    /* --- counters --- */
#if RV_EXT_ZICNTR
    case CSR_MCYCLE:
    case CSR_CYCLE:         *out = (uint32_t)h->mcycle;          break;
    case CSR_MCYCLEH:
    case CSR_CYCLEH:        *out = (uint32_t)(h->mcycle >> 32);  break;
    case CSR_MINSTRET:
    case CSR_INSTRET:       *out = (uint32_t)h->minstret;        break;
    case CSR_MINSTRETH:
    case CSR_INSTRETH:      *out = (uint32_t)(h->minstret >> 32); break;
    case CSR_TIME:          *out = (uint32_t)read_time(h);        break;
    case CSR_TIMEH:         *out = (uint32_t)(read_time(h) >> 32); break;
    case CSR_MCOUNTINHIBIT: *out = h->mcountinhibit;              break;
#endif

    default:
        return RV_EXC_ILLEGAL_INSN;
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
    case CSR_MSTATUS:
        /* WARL: keep the bits we implement, ignore the rest. */
        h->mstatus = (h->mstatus & ~MSTATUS_WMASK) | (val & MSTATUS_WMASK);
        break;

    case CSR_MSTATUSH:
        break;   /* all implemented fields are read-only zero */

    case CSR_MISA:
        break;   /* the extension set is fixed at build time */

    case CSR_MIE:
        h->mie = val & MIE_WMASK;
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

    case CSR_MEDELEG:
    case CSR_MIDELEG:
    case CSR_MCOUNTEREN:
        break;   /* no lower privilege levels; hardwired zero */

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
    default:                return NULL;
    }
}

#endif /* RV_EXT_ZICSR */
