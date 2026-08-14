/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_types.h - RH850 G4MH exception causes and PSW layout.
 *
 * The ISA-agnostic vocabulary lives in emu/emu_types.h; this header holds
 * only what is specific to the G4MH frontend.
 */
#ifndef G4MH_G4MH_TYPES_H
#define G4MH_G4MH_TYPES_H

#include "emu/emu_types.h"

#include "g4mh_config.h"

/* ------------------------------------------------------------------ */
/* Exception causes (EIIC / FEIC)                                      */
/* ------------------------------------------------------------------ */

/*
 * The architecture calls these "exception cause codes" and puts them in
 * EIIC or FEIC depending on the level the exception is taken at. As with
 * rv_exc_t, 0 is a real cause, so the sentinel sits outside the encodable
 * range and callers branch on != G4MH_EXC_NONE.
 */
typedef uint32_t g4mh_exc_t;

#define G4MH_EXC_NONE           0xFFFFFFFFu

/* FE level: taken to FEPC/FEPSW/FEIC, and not maskable by PSW.ID. */
#define G4MH_EXC_SYSERR         0x0010u   /* system error                 */
#define G4MH_EXC_MIP            0x0030u   /* instruction-fetch protection */
#define G4MH_EXC_MDP            0x0031u   /* data protection              */
#define G4MH_EXC_RIE            0x0060u   /* reserved instruction         */
#define G4MH_EXC_MAE            0x0062u   /* misaligned access            */
#define G4MH_EXC_FPP            0x0071u   /* FP operation                 */
#define G4MH_EXC_UCPOP          0x0080u   /* coprocessor unusable         */

/* EI level: taken to EIPC/EIPSW/EIIC, maskable by PSW.ID. */
#define G4MH_EXC_TRAP0          0x0040u   /* TRAP 0..15,  +vector         */
#define G4MH_EXC_TRAP1          0x0050u   /* TRAP 16..31, +vector         */
#define G4MH_EXC_SYSCALL        0x8000u   /* SYSCALL, +vector             */
/*
 * FETRAP 1..15. The architecture leaves the cause code to the part ("see
 * the hardware manual of the product used"), and this frontend's codes are
 * internal identifiers anyway -- handler_address maps them to the compact
 * offsets it uses. 0x0090 is chosen only because it collides with nothing:
 * the obvious reading of the real table would put FETRAP at 0x0031..0x003F,
 * on top of MDP.
 */
#define G4MH_EXC_FETRAP         0x0090u   /* FETRAP 1..15, +vector        */

/*
 * An EI interrupt's cause is 0x1000 + channel. The frontend's INTC raises
 * these; the platform sees only a channel number.
 */
#define G4MH_EXC_EIINT_BASE     0x1000u

/*
 * Map a bus fault onto the G4MH exception that reports it.
 *
 * MIP for a fetch and MDP for data: RH850 has no "access fault" separate
 * from a protection violation, so an unmapped address is reported as the
 * memory-protection exception, which is what a part with the MPU covering
 * all of memory would raise. Only ever reached on a fault, so the table
 * lookup stays off the hot path.
 */
static EMU_ALWAYS_INLINE g4mh_exc_t g4mh_exc_from_fault(emu_fault_t f)
{
    static const uint16_t cause[] = {
        [EMU_FAULT_NONE]  = 0u,   /* never read; the test below shorts it */
        [EMU_FAULT_FETCH] = G4MH_EXC_MIP,
        [EMU_FAULT_LOAD]  = G4MH_EXC_MDP,
        [EMU_FAULT_STORE] = G4MH_EXC_MDP,
    };
    return (f == EMU_FAULT_NONE) ? G4MH_EXC_NONE : cause[f];
}

/* ------------------------------------------------------------------ */
/* PSW                                                                 */
/* ------------------------------------------------------------------ */

#define G4MH_PSW_Z          (1u << 0)    /* result was zero              */
#define G4MH_PSW_S          (1u << 1)    /* result was negative          */
#define G4MH_PSW_OV         (1u << 2)    /* signed overflow              */
#define G4MH_PSW_CY         (1u << 3)    /* carry / borrow               */
#define G4MH_PSW_SAT        (1u << 4)    /* saturated (sticky)           */
#define G4MH_PSW_ID         (1u << 5)    /* EI interrupts disabled       */
#define G4MH_PSW_EP         (1u << 6)    /* in an exception, not an int  */
#define G4MH_PSW_NP         (1u << 7)    /* in an FE-level exception     */
#define G4MH_PSW_EBV        (1u << 15)   /* vectors from EBASE, not RBASE*/
#define G4MH_PSW_CU0        (1u << 16)
#define G4MH_PSW_CU1        (1u << 17)
#define G4MH_PSW_CU2        (1u << 18)
#define G4MH_PSW_UM         (1u << 30)   /* user mode                    */

/* The condition-code bits, as a group: what an arithmetic result sets. */
#define G4MH_PSW_FLAGS      (G4MH_PSW_Z | G4MH_PSW_S | G4MH_PSW_OV | \
                             G4MH_PSW_CY)

/* ------------------------------------------------------------------ */
/* Register conventions                                                */
/* ------------------------------------------------------------------ */

/*
 * r0 is hardwired zero. The rest are conventions the ABI fixes rather
 * than the hardware, but the ones below are architectural enough to name:
 * EP is the base register the 16-bit SLD/SST forms address through, and
 * LP is where JARL leaves the return address by default.
 */
#define G4MH_REG_ZERO       0u
#define G4MH_REG_SP         3u
#define G4MH_REG_GP         4u
#define G4MH_REG_TP         5u
#define G4MH_REG_EP         30u
#define G4MH_REG_LP         31u

/* ------------------------------------------------------------------ */
/* System registers (selID 0 unless noted)                             */
/* ------------------------------------------------------------------ */

#define G4MH_SR_EIPC        0u
#define G4MH_SR_EIPSW       1u
#define G4MH_SR_FEPC        2u
#define G4MH_SR_FEPSW       3u
#define G4MH_SR_PSW         5u
#define G4MH_SR_EIIC        13u
#define G4MH_SR_FEIC        14u
#define G4MH_SR_CTPC        16u
#define G4MH_SR_CTPSW       17u
#define G4MH_SR_CTBP        20u
#define G4MH_SR_EIWR        28u
#define G4MH_SR_FEWR        29u
#define G4MH_SR_BSEL        31u

/* selID 1 */
#define G4MH_SR_MCFG0       0u
#define G4MH_SR_RBASE       2u
#define G4MH_SR_EBASE       3u
#define G4MH_SR_INTBP       4u
#define G4MH_SR_MCTL        5u
#define G4MH_SR_PID         6u
#define G4MH_SR_SCCFG       11u
#define G4MH_SR_SCBP        12u

/* selID 2 */
#define G4MH_SR_HTCFG0      0u
#define G4MH_SR_MEA         6u
#define G4MH_SR_ASID        7u
#define G4MH_SR_MEI         8u
#define G4MH_SR_ISPR        10u
#define G4MH_SR_PMR         11u
#define G4MH_SR_ICSR        12u
#define G4MH_SR_INTCFG      13u

/*
 * The FPU system registers, selID 0. Numbering from the U2B hardware
 * manual Table 3.50 -- the *software* manual describes the instructions
 * and never says where these live, which is why the reference here is a
 * different document from the one the encodings came from.
 *
 * FPST, FPCC and FPCFG are windows onto FPSR rather than storage: each
 * reflects a subset of its bits, so a write through one has to land in
 * FPSR or the two disagree. g4mh_sr_read/write route them.
 */
#define G4MH_SR_FPSR        6u
#define G4MH_SR_FPEPC       7u
#define G4MH_SR_FPST        8u
#define G4MH_SR_FPCC        9u
#define G4MH_SR_FPCFG       10u

/*
 * FPSR, from the same table.
 *
 *   31..24  CC[7:0]  comparison results, written only by CMPF and LDSR
 *   23      FN       flush-to-nearest
 *   22      IF       an input operand was flushed (sticky)
 *   21      reserved, reads 1
 *   19..18  RM       rounding mode, 00 RN / 01 RZ / 10 RP / 11 RM
 *   17      FS       flush subnormals; 1 out of reset
 *   15..10  XC       cause bits      E V Z O U I
 *    9..5   XE       enable bits       V Z O U I
 *    4..0   XP       preservation      V Z O U I
 *
 * The three exception groups are *not* the same width: the cause group
 * has an E (unimplemented operation) bit that the enable and
 * preservation groups do not, so they are six and five bits and the
 * shifts below differ by one. Deriving one from the other by shifting a
 * single five-bit mask puts every cause bit one place out.
 */
#define G4MH_FPSR_CC_SHIFT  24u
#define G4MH_FPSR_CC_MASK   0xFF000000u
#define G4MH_FPSR_FN        (1u << 23)
#define G4MH_FPSR_IF        (1u << 22)
#define G4MH_FPSR_RSV1      (1u << 21)
#define G4MH_FPSR_RM_SHIFT  18u
#define G4MH_FPSR_RM_MASK   (3u << 18)
#define G4MH_FPSR_FS        (1u << 17)
#define G4MH_FPSR_XC_SHIFT  10u
#define G4MH_FPSR_XC_MASK   (0x3Fu << 10)
#define G4MH_FPSR_XE_SHIFT  5u
#define G4MH_FPSR_XE_MASK   (0x1Fu << 5)
#define G4MH_FPSR_XP_SHIFT  0u
#define G4MH_FPSR_XP_MASK   0x1Fu

/*
 * The five exception bits, in the order the XE and XP groups use them:
 * bit 0 inexact, 1 underflow, 2 overflow, 3 divide-by-zero, 4 invalid.
 * The cause group adds E above them, at bit 5 of the group.
 */
#define G4MH_FPX_I          (1u << 0)    /* inexact                     */
#define G4MH_FPX_U          (1u << 1)    /* underflow                   */
#define G4MH_FPX_O          (1u << 2)    /* overflow                    */
#define G4MH_FPX_Z          (1u << 3)    /* divide by zero              */
#define G4MH_FPX_V          (1u << 4)    /* invalid operation           */
#define G4MH_FPX_E          (1u << 5)    /* unimplemented; cause only   */

/* Rounding modes, as FPSR.RM encodes them. */
#define G4MH_RM_RN          0u
#define G4MH_RM_RZ          1u
#define G4MH_RM_RP          2u
#define G4MH_RM_RM          3u

/* Number of selID banks this implementation decodes. */
#define G4MH_SR_BANKS       3u
#define G4MH_SR_PER_BANK    32u

#endif /* G4MH_G4MH_TYPES_H */
