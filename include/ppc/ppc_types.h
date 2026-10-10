/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_types.h - NXP PowerPC e200z7 architectural constants.
 *
 * Power ISA 2.06 embedded (Book E) with VLE, the core in the MPC57xx
 * automotive parts. **Big-endian**, which is the first in this tree; see
 * the note by EMU_BUS_ORDER in emu/emu_bus.h for what that does and does
 * not change.
 *
 * Bit numbering is the trap for anyone arriving from RISC-V or ARM.
 * PowerPC numbers bits from the *most* significant: bit 0 is 0x80000000
 * and bit 31 is 0x1. Every field position in the manual is in that
 * convention, so the shifts here convert once, at the point of decode,
 * and everything downstream is ordinary C.
 */
#ifndef PPC_TYPES_H
#define PPC_TYPES_H

#include "emu/emu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_NGPR 32u

/*
 * Machine State Register, every bit the e200z759n3 implements (manual
 * table 7-4). Named by their PowerPC bit number in the comment and given
 * as masks, because the manual's numbers are from the left and mixing
 * the two conventions is how MSR[EE] and MSR[PR] end up swapped.
 */
#define PPC_MSR_UCLE (1u << 26) /* bit  5: user cache lock enable */
#define PPC_MSR_SPE (1u << 25)  /* bit  6: SPE/EFPU available     */
#define PPC_MSR_WE (1u << 18)   /* bit 13: wait state enable      */
#define PPC_MSR_CE (1u << 17)   /* bit 14: critical enable        */
#define PPC_MSR_EE (1u << 15)   /* bit 16: external enable        */
#define PPC_MSR_PR (1u << 14)   /* bit 17: problem state (user)   */
#define PPC_MSR_FP (1u << 13)   /* bit 18: FP available (ignored) */
#define PPC_MSR_ME (1u << 12)   /* bit 19: machine check enable   */
#define PPC_MSR_FE0 (1u << 11)  /* bit 20: unused by this core    */
#define PPC_MSR_DE (1u << 9)    /* bit 22: debug enable           */
#define PPC_MSR_FE1 (1u << 8)   /* bit 23: unused by this core    */
#define PPC_MSR_IS (1u << 5)    /* bit 26: instruction space      */
#define PPC_MSR_DS (1u << 4)    /* bit 27: data space             */
#define PPC_MSR_PMM (1u << 2)   /* bit 29: performance mark       */
#define PPC_MSR_RI (1u << 1)    /* bit 30: recoverable interrupt  */

/* The bits that exist; the rest read as zero and ignore writes. */
#define PPC_MSR_IMPL                                                           \
    (PPC_MSR_UCLE | PPC_MSR_SPE | PPC_MSR_WE | PPC_MSR_CE | PPC_MSR_EE |       \
     PPC_MSR_PR | PPC_MSR_FP | PPC_MSR_ME | PPC_MSR_FE0 | PPC_MSR_DE |         \
     PPC_MSR_FE1 | PPC_MSR_IS | PPC_MSR_DS | PPC_MSR_PMM | PPC_MSR_RI)

/*
 * Exception Syndrome Register (table 7-3). An interrupt that sets ESR
 * clears every bit it does not name -- the register describes the last
 * synchronous exception, not a history.
 */
#define PPC_ESR_PIL (1u << 27)   /* bit  4: illegal instruction     */
#define PPC_ESR_PPR (1u << 26)   /* bit  5: privileged instruction  */
#define PPC_ESR_PTR (1u << 25)   /* bit  6: trap                    */
#define PPC_ESR_ST (1u << 23)    /* bit  8: the access was a store  */
#define PPC_ESR_SPE (1u << 7)    /* bit 24: SPE/EFPU operation      */
#define PPC_ESR_VLEMI (1u << 5)  /* bit 26: a VLE instruction       */
#define PPC_ESR_MIF (1u << 1)    /* bit 30: misaligned fetch        */

/*
 * SPEFSCR, the embedded floating-point status and control register
 * (table 5-1). The "H" bits describe the high element of a vector
 * operation, which this model does not execute; a scalar instruction
 * clears them, which is all they ever do here.
 */
#define PPC_SPEFSCR_SOVH (1u << 31)
#define PPC_SPEFSCR_OVH (1u << 30)
#define PPC_SPEFSCR_FGH (1u << 29)
#define PPC_SPEFSCR_FXH (1u << 28)
#define PPC_SPEFSCR_FINVH (1u << 27)
#define PPC_SPEFSCR_FDBZH (1u << 26)
#define PPC_SPEFSCR_FUNFH (1u << 25)
#define PPC_SPEFSCR_FOVFH (1u << 24)
#define PPC_SPEFSCR_FINXS (1u << 21)
#define PPC_SPEFSCR_FINVS (1u << 20)
#define PPC_SPEFSCR_FDBZS (1u << 19)
#define PPC_SPEFSCR_FUNFS (1u << 18)
#define PPC_SPEFSCR_FOVFS (1u << 17)
#define PPC_SPEFSCR_MODE (1u << 16) /* only mode 0 exists: reads 0 */
#define PPC_SPEFSCR_SOV (1u << 15)
#define PPC_SPEFSCR_OV (1u << 14)
#define PPC_SPEFSCR_FG (1u << 13)
#define PPC_SPEFSCR_FX (1u << 12)
#define PPC_SPEFSCR_FINV (1u << 11)
#define PPC_SPEFSCR_FDBZ (1u << 10)
#define PPC_SPEFSCR_FUNF (1u << 9)
#define PPC_SPEFSCR_FOVF (1u << 8)
#define PPC_SPEFSCR_FINXE (1u << 6)
#define PPC_SPEFSCR_FINVE (1u << 5)
#define PPC_SPEFSCR_FDBZE (1u << 4)
#define PPC_SPEFSCR_FUNFE (1u << 3)
#define PPC_SPEFSCR_FOVFE (1u << 2)
#define PPC_SPEFSCR_FRMC 3u /* 0 RN, 1 RZ, 2 RP, 3 RM */

/* Every implemented bit: bits 8:9 and 24 are reserved, MODE reads 0. */
#define PPC_SPEFSCR_IMPL 0xFF3EFF7Fu

/*
 * Condition register. Eight 4-bit fields, CR0..CR7, numbered from the
 * left: CR0 is bits 0:3, i.e. the *top* nibble of the 32-bit register.
 * Within a field the order is LT, GT, EQ, SO.
 */
#define PPC_CR_LT 0x8u
#define PPC_CR_GT 0x4u
#define PPC_CR_EQ 0x2u
#define PPC_CR_SO 0x1u

/* XER, the fixed-point exception register. Bits 25:31 are the string
 * byte count, which exists as storage although the string instructions
 * do not. */
#define PPC_XER_SO (1u << 31)
#define PPC_XER_OV (1u << 30)
#define PPC_XER_CA (1u << 29)
#define PPC_XER_IMPL (PPC_XER_SO | PPC_XER_OV | PPC_XER_CA | 0x7Fu)

/*
 * Special purpose registers, by SPR number (manual table 16). The
 * privileged ones have bit 5 of the *split* field set, which in the
 * swapped number is 0x10 -- the rule mfspr and mtspr apply to an
 * unimplemented number as well.
 */
#define PPC_SPR_XER 1u
#define PPC_SPR_LR 8u
#define PPC_SPR_CTR 9u
#define PPC_SPR_DEC 22u
#define PPC_SPR_SRR0 26u
#define PPC_SPR_SRR1 27u
#define PPC_SPR_PID0 48u
#define PPC_SPR_DECAR 54u
#define PPC_SPR_CSRR0 58u
#define PPC_SPR_CSRR1 59u
#define PPC_SPR_DEAR 61u
#define PPC_SPR_ESR 62u
#define PPC_SPR_IVPR 63u
#define PPC_SPR_USPRG0 256u
#define PPC_SPR_SPRG4_R 260u /* .. SPRG7_R at 263, user read-only   */
#define PPC_SPR_TBL_R 268u
#define PPC_SPR_TBU_R 269u
#define PPC_SPR_SPRG0 272u   /* .. SPRG7 at 279                     */
#define PPC_SPR_TBL_W 284u
#define PPC_SPR_TBU_W 285u
#define PPC_SPR_PIR 286u
#define PPC_SPR_PVR 287u
#define PPC_SPR_DBSR 304u
#define PPC_SPR_DBCR0 308u   /* .. DBCR2 at 310                     */
#define PPC_SPR_IAC1 312u    /* .. IAC4 at 315                      */
#define PPC_SPR_DAC1 316u    /* .. DAC2 at 317                      */
#define PPC_SPR_DVC1 318u    /* .. DVC2 at 319                      */
#define PPC_SPR_TSR 336u
#define PPC_SPR_TCR 340u
#define PPC_SPR_IVOR0 400u   /* .. IVOR15 at 415                    */
#define PPC_SPR_SPEFSCR 512u
#define PPC_SPR_L1CFG0 515u
#define PPC_SPR_L1CFG1 516u
#define PPC_SPR_IVOR32 528u  /* .. IVOR35 at 531                    */
#define PPC_SPR_DBCR3 561u
#define PPC_SPR_DBCNT 562u
#define PPC_SPR_DBCR4 563u
#define PPC_SPR_DBCR5 564u
#define PPC_SPR_IAC5 565u    /* .. IAC8 at 568                      */
#define PPC_SPR_DBERC0 569u
#define PPC_SPR_MCSRR0 570u
#define PPC_SPR_MCSRR1 571u
#define PPC_SPR_MCSR 572u
#define PPC_SPR_MCAR 573u
#define PPC_SPR_DSRR0 574u
#define PPC_SPR_DSRR1 575u
#define PPC_SPR_DDAM 576u
#define PPC_SPR_DBCR6 603u
#define PPC_SPR_SPRG8 604u
#define PPC_SPR_SPRG9 605u
#define PPC_SPR_L1FINV1 959u
#define PPC_SPR_DEVENT 975u
#define PPC_SPR_HID0 1008u
#define PPC_SPR_HID1 1009u
#define PPC_SPR_L1CSR0 1010u
#define PPC_SPR_L1CSR1 1011u
#define PPC_SPR_BUCSR 1013u
#define PPC_SPR_L1FINV0 1016u
#define PPC_SPR_SVR 1023u

/* HID0[ICR], interrupt inputs clear the reservation (bit 14). */
#define PPC_HID0_ICR (1u << 17)
/*
 * HID0[TBEN] and HID0[SEL_TBCLK] (bits 17 and 18): whether the time
 * base and the decrementer count at all, and what they count -- the
 * processor clock, or the p_tbclk input.
 */
#define PPC_HID0_TBEN (1u << 14)
#define PPC_HID0_SEL_TBCLK (1u << 13)
/* L1CSR0[DCE], the data cache enable (bit 31). */
#define PPC_L1CSR0_DCE 1u

/*
 * TSR and TCR, with Book E's bit numbers in the comments and masks in
 * the code -- the same discipline as MSR above, and for the same
 * reason: these are numbered from the left, so TSR[DIS] at bit 4 is
 * mask 1 << 27.
 */
#define PPC_TSR_ENW (1u << 31) /* bit  0: next watchdog        */
#define PPC_TSR_WIS (1u << 30) /* bit  1: watchdog             */
#define PPC_TSR_DIS (1u << 27) /* bit  4: decrementer          */
#define PPC_TSR_FIS (1u << 26) /* bit  5: fixed interval       */

#define PPC_TCR_WIE (1u << 27) /* bit  4: watchdog enable      */
#define PPC_TCR_DIE (1u << 26) /* bit  5: decrementer enable   */
#define PPC_TCR_FIE (1u << 23) /* bit  8: fixed-interval en    */
#define PPC_TCR_ARE (1u << 22) /* bit  9: auto-reload          */

/*
 * Book E interrupts, by IVOR number. The handler address is
 * IVPR[0:15] || IVORn[16:27] || 0b0000 -- the vector is *in a register*,
 * not at a fixed offset, which is the main structural difference from
 * RISC-V's mtvec and from G4MH's RBASE table.
 *
 * 32 to 35 are the e200's own: the SPE/EFPU unit and the performance
 * monitor. There is no IVOR16 to 31, and the array that holds them is
 * indexed by this number, so it has 36 slots of which 20 are used.
 */
typedef enum {
    PPC_IVOR_CRITICAL = 0,
    PPC_IVOR_MACHINE_CHECK = 1,
    PPC_IVOR_DATA_STORAGE = 2,
    PPC_IVOR_INST_STORAGE = 3,
    PPC_IVOR_EXTERNAL = 4,
    PPC_IVOR_ALIGNMENT = 5,
    PPC_IVOR_PROGRAM = 6,
    PPC_IVOR_FP_UNAVAIL = 7,
    PPC_IVOR_SYSTEM_CALL = 8,
    PPC_IVOR_AP_UNAVAIL = 9,
    PPC_IVOR_DECREMENTER = 10,
    PPC_IVOR_FIT = 11,
    PPC_IVOR_WATCHDOG = 12,
    PPC_IVOR_DTLB_ERROR = 13,
    PPC_IVOR_ITLB_ERROR = 14,
    PPC_IVOR_DEBUG = 15,
    PPC_IVOR_SPE_UNAVAIL = 32,
    PPC_IVOR_FP_DATA = 33,
    PPC_IVOR_FP_ROUND = 34,
    PPC_IVOR_PERF_MON = 35,
    PPC_IVOR_COUNT = 36,
} ppc_ivor_t;

/* No exception pending. Distinct from every valid IVOR index. */
#define PPC_EXC_NONE 0xFFFFFFFFu

typedef uint32_t ppc_exc_t;

#ifdef __cplusplus
}
#endif

#endif /* PPC_TYPES_H */
