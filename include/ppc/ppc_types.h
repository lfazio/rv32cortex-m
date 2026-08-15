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

#define PPC_NGPR            32u

/*
 * Machine State Register, the bits this core models. Named by their
 * PowerPC bit number in the comment and given as masks, because the
 * manual's numbers are from the left and mixing the two conventions is
 * how MSR[EE] and MSR[PR] end up swapped.
 */
#define PPC_MSR_CE          (1u << 17)   /* bit 14: critical enable      */
#define PPC_MSR_EE          (1u << 15)   /* bit 16: external enable      */
#define PPC_MSR_PR          (1u << 14)   /* bit 17: problem state (user) */
#define PPC_MSR_ME          (1u << 12)   /* bit 19: machine check enable */
#define PPC_MSR_DE          (1u << 9)    /* bit 22: debug enable         */
#define PPC_MSR_IS          (1u << 5)    /* bit 26: instruction space    */
#define PPC_MSR_DS          (1u << 4)    /* bit 27: data space           */
#define PPC_MSR_SPE         (1u << 25)   /* bit  6: SPE available        */

/*
 * Condition register. Eight 4-bit fields, CR0..CR7, numbered from the
 * left: CR0 is bits 0:3, i.e. the *top* nibble of the 32-bit register.
 * Within a field the order is LT, GT, EQ, SO.
 */
#define PPC_CR_LT           0x8u
#define PPC_CR_GT           0x4u
#define PPC_CR_EQ           0x2u
#define PPC_CR_SO           0x1u

/* XER, the fixed-point exception register. */
#define PPC_XER_SO          (1u << 31)
#define PPC_XER_OV          (1u << 30)
#define PPC_XER_CA          (1u << 29)

/*
 * Special purpose registers, by SPR number. The e200 has a great many;
 * these are the ones a guest cannot start without.
 */
#define PPC_SPR_XER         1u
#define PPC_SPR_LR          8u
#define PPC_SPR_CTR         9u
#define PPC_SPR_SRR0        26u
#define PPC_SPR_SRR1        27u
#define PPC_SPR_CSRR0       58u
#define PPC_SPR_CSRR1       59u
#define PPC_SPR_DEAR        61u
#define PPC_SPR_ESR         62u
#define PPC_SPR_IVPR        63u
#define PPC_SPR_PIR         286u
#define PPC_SPR_PVR         287u
#define PPC_SPR_DBSR        304u
#define PPC_SPR_DBCR0       308u
#define PPC_SPR_TSR         336u
#define PPC_SPR_TCR         340u
#define PPC_SPR_IVOR0       400u   /* .. IVOR15 at 415                   */
#define PPC_SPR_SPRG0       272u   /* .. SPRG7                           */

/* How many SPRs are storable. 1024 is the architectural space; a flat
 * array of that is 4 KiB, which is more than the sparse set justifies,
 * so the frontend maps the ones above onto a small table. */
#define PPC_NSPR            1024u

/*
 * Book E interrupts, as IVOR index. The handler address is
 * IVPR[0:15] || IVORn[16:27] || 0b0000 -- the vector is *in a register*,
 * not at a fixed offset, which is the main structural difference from
 * RISC-V's mtvec and from G4MH's RBASE table.
 */
typedef enum {
    PPC_IVOR_CRITICAL      = 0,
    PPC_IVOR_MACHINE_CHECK = 1,
    PPC_IVOR_DATA_STORAGE  = 2,
    PPC_IVOR_INST_STORAGE  = 3,
    PPC_IVOR_EXTERNAL      = 4,
    PPC_IVOR_ALIGNMENT     = 5,
    PPC_IVOR_PROGRAM       = 6,
    PPC_IVOR_FP_UNAVAIL    = 7,
    PPC_IVOR_SYSTEM_CALL   = 8,
    PPC_IVOR_AP_UNAVAIL    = 9,
    PPC_IVOR_DECREMENTER   = 10,
    PPC_IVOR_FIT           = 11,
    PPC_IVOR_WATCHDOG      = 12,
    PPC_IVOR_DTLB_ERROR    = 13,
    PPC_IVOR_ITLB_ERROR    = 14,
    PPC_IVOR_DEBUG         = 15,
    PPC_IVOR_COUNT         = 16,
} ppc_ivor_t;

/* No exception pending. Distinct from every valid IVOR index. */
#define PPC_EXC_NONE        0xFFFFFFFFu

typedef uint32_t ppc_exc_t;

#ifdef __cplusplus
}
#endif

#endif /* PPC_TYPES_H */
