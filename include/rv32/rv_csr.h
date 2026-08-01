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

struct rv_hart;

/* ------------------------------------------------------------------ */
/* CSR addresses                                                       */
/* ------------------------------------------------------------------ */

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

/* Machine trap handling. */
#define CSR_MSCRATCH        0x340
#define CSR_MEPC            0x341
#define CSR_MCAUSE          0x342
#define CSR_MTVAL           0x343
#define CSR_MIP             0x344

/* Machine counters. */
#define CSR_MCYCLE          0xB00
#define CSR_MINSTRET        0xB02
#define CSR_MCYCLEH         0xB80
#define CSR_MINSTRETH       0xB82
#define CSR_MCOUNTINHIBIT   0x320

/* ------------------------------------------------------------------ */
/* mstatus fields                                                      */
/* ------------------------------------------------------------------ */

#define MSTATUS_MIE         (1u << 3)
#define MSTATUS_MPIE        (1u << 7)
#define MSTATUS_MPP_SHIFT   11
#define MSTATUS_MPP_MASK    (3u << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPRV        (1u << 17)

/* Bits software is allowed to change in mstatus on this implementation. */
#define MSTATUS_WMASK       (MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK)

/* ------------------------------------------------------------------ */
/* mie / mip fields                                                    */
/* ------------------------------------------------------------------ */

#define MIP_MSIP            (1u << RV_INT_M_SOFT)
#define MIP_MTIP            (1u << RV_INT_M_TIMER)
#define MIP_MEIP            (1u << RV_INT_M_EXT)

#define MIE_MSIE            MIP_MSIP
#define MIE_MTIE            MIP_MTIP
#define MIE_MEIE            MIP_MEIP

/* mip bits are driven by devices, not by CSR writes. */
#define MIP_WMASK           0u
#define MIE_WMASK           (MIE_MSIE | MIE_MTIE | MIE_MEIE)

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
