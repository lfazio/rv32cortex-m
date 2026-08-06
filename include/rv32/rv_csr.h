/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_csr.h - Control and status register numbers and field layouts.
 *
 * Machine mode only. The hart stores the handful of CSRs that actually have
 * state as named fields rather than a 4096-entry array; everything else is
 * either hardwired or synthesised on read.
 */
#ifndef RV32_RV_CSR_H
#define RV32_RV_CSR_H

#include "rv_types.h"
#include "rv_config.h"

struct rv_hart;

/* ------------------------------------------------------------------ */
/* CSR addresses                                                       */
/* ------------------------------------------------------------------ */

/* Floating-point CSRs (F). fflags and frm are windows onto fcsr. */
#define CSR_FFLAGS          0x001
#define CSR_FRM             0x002
#define CSR_FCSR            0x003

#define FFLAG_NX            0x01u   /* inexact          */
#define FFLAG_UF            0x02u   /* underflow        */
#define FFLAG_OF            0x04u   /* overflow         */
#define FFLAG_DZ            0x08u   /* divide by zero   */
#define FFLAG_NV            0x10u   /* invalid operation*/

#define FRM_RNE 0u  /* nearest, ties to even */
#define FRM_RTZ 1u  /* toward zero           */
#define FRM_RDN 2u  /* down, toward -inf     */
#define FRM_RUP 3u  /* up, toward +inf       */
#define FRM_RMM 4u  /* nearest, ties to max magnitude */
#define FRM_DYN 7u  /* use fcsr.frm          */

/* Unprivileged counters (Zicntr), read-only shadows of the M-mode ones. */
#define CSR_CYCLE           0xC00
#define CSR_TIME            0xC01
#define CSR_INSTRET         0xC02
#define CSR_CYCLEH          0xC80
#define CSR_TIMEH           0xC81
#define CSR_INSTRETH        0xC82

/* Machine information. */
#define CSR_MVENDORID       0xF11
#define CSR_MARCHID         0xF12
#define CSR_MIMPID          0xF13
#define CSR_MHARTID         0xF14
#define CSR_MCONFIGPTR      0xF15

/* Machine trap setup. */
#define CSR_MSTATUS         0x300
#define CSR_MISA            0x301
#define CSR_MEDELEG         0x302
#define CSR_MIDELEG         0x303
#define CSR_MIE             0x304
#define CSR_MTVEC           0x305
#define CSR_MCOUNTEREN      0x306
#define CSR_MSTATUSH        0x310
#define CSR_MENVCFG         0x30A
#define CSR_MENVCFGH        0x31A
#define CSR_SENVCFG         0x10A

/*
 * The environment-configuration registers, which say what a *lower*
 * privilege level may do. Only the cache-block bits mean anything here:
 * FIOM (fence-implies-IO) is vacuous with no distinct IO ordering, and
 * every field in the high half belongs to an extension this core does not
 * have -- PBMTE to Svpbmt, STCE to Sstc, and ADUE to Svadu, whose absence
 * is precisely the statement that A and D are software's to set.
 */
#define ENVCFG_FIOM         (1u << 0)
#define ENVCFG_CBIE         (3u << 4)
#define ENVCFG_CBCFE        (1u << 6)
#define ENVCFG_CBZE         (1u << 7)
#define ENVCFG_WMASK        (ENVCFG_FIOM | ENVCFG_CBIE | ENVCFG_CBCFE | \
                             ENVCFG_CBZE)

/* Machine trap handling. */
#define CSR_MSCRATCH        0x340
#define CSR_MEPC            0x341
#define CSR_MCAUSE          0x342
#define CSR_MTVAL           0x343
#define CSR_MIP             0x344

/* Supervisor trap setup. sstatus/sie/sip are restricted views of the
 * machine registers, not storage of their own. */
#define CSR_SSTATUS         0x100
#define CSR_SIE             0x104
#define CSR_STVEC           0x105
#define CSR_SCOUNTEREN      0x106

/* Supervisor trap handling. */
#define CSR_SSCRATCH        0x140
#define CSR_SEPC            0x141
#define CSR_SCAUSE          0x142
#define CSR_STVAL           0x143
#define CSR_SIP             0x144

/* Supervisor address translation. */
#define CSR_SATP            0x180
#define SATP_MODE_SV32      0x80000000u
#define SATP_ASID_SHIFT     22
#define SATP_ASID_MASK      (0x1FFu << SATP_ASID_SHIFT)
/*
 * 20 bits, not the 22 Sv32 allows. satp.PPN is WARL and its width follows
 * the physical address space: this core's bus is 32-bit, so a page number
 * is PA[31:12] and the top two bits of the architectural field cannot name
 * anything. Masking them off is what makes that visible to software --
 * keeping them would let satp report a root table that the walk then
 * silently truncates when it shifts the PPN back up.
 */
#define SATP_PPN_MASK       0x000FFFFFu

/* Sdtrig debug triggers. */
#define CSR_TSELECT         0x7A0
#define CSR_TDATA1          0x7A1
#define CSR_TDATA2          0x7A2
#define CSR_TDATA3          0x7A3
#define CSR_TINFO           0x7A4

/* Physical memory protection. */
#define CSR_PMPCFG0         0x3A0
#define CSR_PMPADDR0        0x3B0

/* Machine counters. */
#define CSR_MCYCLE          0xB00
#define CSR_MINSTRET        0xB02
#define CSR_MCYCLEH         0xB80
#define CSR_MINSTRETH       0xB82
#define CSR_MCOUNTINHIBIT   0x320

/* ------------------------------------------------------------------ */
/* mstatus fields                                                      */
/* ------------------------------------------------------------------ */

#define MSTATUS_SIE         (1u << 1)
#define MSTATUS_MIE         (1u << 3)
#define MSTATUS_SPIE        (1u << 5)
#define MSTATUS_MPIE        (1u << 7)
#define MSTATUS_SPP         (1u << 8)
#define MSTATUS_MPP_SHIFT   11
#define MSTATUS_MPP_MASK    (3u << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPRV        (1u << 17)
#define MSTATUS_SUM         (1u << 18)
#define MSTATUS_MXR         (1u << 19)
#define MSTATUS_TVM         (1u << 20)
#define MSTATUS_TW          (1u << 21)
#define MSTATUS_TSR         (1u << 22)
#define MSTATUS_FS_SHIFT    13
#define MSTATUS_FS_MASK     (3u << MSTATUS_FS_SHIFT)
#define MSTATUS_SD          (1u << 31)

/* Bits software is allowed to change in mstatus on this implementation. */
#if RV_EXT_F
/* FS must be writable, or software cannot enable the FPU at all. */
#  define MSTATUS_WMASK_F   MSTATUS_FS_MASK
#else
#  define MSTATUS_WMASK_F   0u
#endif
/* MPRV is meaningless without a second privilege level to borrow. */
#if RV_EXT_U
#  define MSTATUS_WMASK_U   MSTATUS_MPRV
#else
#  define MSTATUS_WMASK_U   0u
#endif
/*
 * The S bank.
 *
 * SUM, MXR and TVM belong to address translation, and software probes for
 * paging by setting SUM and MXR and reading them back -- rv32mi/illegal
 * makes exactly that probe and changes what it then demands of SFENCE.VMA.
 * So they are writable exactly when Sv32 is built in, and read as zero
 * when it is not, which is the honest answer to that probe either way.
 */
#if RV_EXT_SV32
#  define MSTATUS_WMASK_VM  (MSTATUS_SUM | MSTATUS_MXR | MSTATUS_TVM)
#else
#  define MSTATUS_WMASK_VM  0u
#endif
#if RV_EXT_S
#  define MSTATUS_WMASK_S   (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                             MSTATUS_TW | MSTATUS_TSR | MSTATUS_WMASK_VM)
#else
#  define MSTATUS_WMASK_S   0u
#endif

#define MSTATUS_WMASK       (MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK | \
                             MSTATUS_WMASK_F | MSTATUS_WMASK_U | \
                             MSTATUS_WMASK_S)

/*
 * What sstatus exposes of mstatus. Reads outside this mask return zero and
 * writes to it leave the rest of mstatus alone, which is the whole of what
 * makes sstatus a view rather than a register.
 */
#define SSTATUS_RMASK       (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                             MSTATUS_SUM | MSTATUS_MXR | \
                             MSTATUS_FS_MASK | MSTATUS_SD)
#define SSTATUS_WMASK       (SSTATUS_RMASK & MSTATUS_WMASK)

/* ------------------------------------------------------------------ */
/* mie / mip fields                                                    */
/* ------------------------------------------------------------------ */

#define MIP_SSIP            (1u << RV_INT_S_SOFT)
#define MIP_STIP            (1u << RV_INT_S_TIMER)
#define MIP_SEIP            (1u << RV_INT_S_EXT)
#define MIP_MSIP            (1u << RV_INT_M_SOFT)
#define MIP_MTIP            (1u << RV_INT_M_TIMER)
#define MIP_MEIP            (1u << RV_INT_M_EXT)

#define MIE_SSIE            MIP_SSIP
#define MIE_STIE            MIP_STIP
#define MIE_SEIE            MIP_SEIP
#define MIE_MSIE            MIP_MSIP
#define MIE_MTIE            MIP_MTIP
#define MIE_MEIE            MIP_MEIP

/* The S bits, as a set. Everything delegable to S-mode. */
#define MIP_S_ALL           (MIP_SSIP | MIP_STIP | MIP_SEIP)

/*
 * mip bits are driven by devices, not by CSR writes -- except the S ones,
 * which have no device behind them here. SSIP and STIP are how M-mode
 * software posts work to a supervisor, so they have to be writable; SEIP
 * is writable in M-mode for the same reason.
 */
#if RV_EXT_S
#  define MIP_WMASK         MIP_S_ALL
#  define MIE_WMASK         (MIE_MSIE | MIE_MTIE | MIE_MEIE | MIP_S_ALL)
#else
#  define MIP_WMASK         0u
#  define MIE_WMASK         (MIE_MSIE | MIE_MTIE | MIE_MEIE)
#endif

/*
 * What may be delegated. Only S-mode interrupts: an M-mode interrupt
 * delegated to S could never be taken, since it is precisely the one the
 * supervisor is not trusted with.
 */
#define MIDELEG_WMASK       MIP_S_ALL
/*
 * Every exception this core can raise except ECALL from M, which cannot be
 * delegated to a mode less privileged than the one that took it. That is
 * causes 0 through 9; the page faults are left out because with satp Bare
 * nothing can raise them, and medeleg is WARL, so a bit that could only
 * ever describe an impossible trap is better read back as zero.
 */
#define MEDELEG_WMASK       0x000003FFu

/* ------------------------------------------------------------------ */
/* mtvec                                                               */
/* ------------------------------------------------------------------ */

#define MTVEC_MODE_MASK     3u
#define MTVEC_MODE_DIRECT   0u
#define MTVEC_MODE_VECTORED 1u

/* ------------------------------------------------------------------ */
/* misa                                                                */
/* ------------------------------------------------------------------ */

#define MISA_MXL_32         (1u << 30)
#define MISA_EXT(c)         (1u << ((c) - 'A'))

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Read/write a CSR on behalf of a CSR instruction. Both enforce the
 * privilege and read-only encodings and return RV_EXC_ILLEGAL_INSN for a
 * bad access, which the interpreter turns into a trap.
 */
rv_exc_t rv_csr_read(struct rv_hart *h, uint32_t csr, uint32_t *out);
rv_exc_t rv_csr_write(struct rv_hart *h, uint32_t csr, uint32_t val);

/* True if the CSR number exists on this implementation. */
bool rv_csr_exists(uint32_t csr);

/* Human-readable CSR name, or NULL. Used by the disassembler and monitor. */
const char *rv_csr_name(uint32_t csr);

#endif /* RV32_RV_CSR_H */
