/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_kcrc.h - Data CRC Function K, RH850/U2B section 57.
 *
 * A CRC engine with a programmable polynomial of 8, 15, 16, 32 or 64
 * bits, input and output reflection, an output XOR mask, and an input
 * register that takes 8, 16 or 32 bits a write. The U2B6 has four
 * units, KCRC0 to KCRC3, at two strides in two bus groups; they are
 * shared, not per core, and raise nothing.
 *
 * **The state is the CRC register before the output XOR.** A write to
 * DOUTn installs the initial value as it is, every DIN write folds the
 * input into it, and a read of DOUTn returns it XORed with the mask --
 * "when reading from this register, the value is XORed by hardware".
 * That is the standard parameterised CRC with init, refin, refout and
 * xorout, and it is why DOUT0 reads 0 out of reset: the register holds
 * FFFF_FFFFH and so does the mask.
 *
 * Three things the manual says only by example, each pinned by a test:
 *
 *   - A wider DIN write is bytes, **least significant first**. The
 *     manual's CCITT16 sequence writes 34333231H, 38373635H and 39H and
 *     expects 29B1H, the check value of "123456789", so 31H ('1') is
 *     the first byte the engine sees.
 *   - CMD1 reflects each input byte and CMD0 the whole result, which is
 *     refin and refout.
 *   - CMD2 ("mode L") reflects the polynomial, the input and the output
 *     together: the CRC32 row with the reversed polynomial EDB88320H and
 *     only CMD2 set has the same check value as the plain one with CMD0
 *     and CMD1. It is modelled as flipping refin and refout and
 *     reflecting the polynomial, so CMD1 and CMD2 together cancel --
 *     a combination the setting table never uses and the manual says
 *     must not be used.
 *
 * Table 57.15's check values are the test: every row, from the
 * polynomial, mask, initial value and control word it lists.
 */
#ifndef G4MH_KCRC_H
#define G4MH_KCRC_H

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_dev.h"

/* Four units on the U2B6 (table 57.1); the larger parts have six. */
#define G4MH_KCRC_COUNT 4u

/* Table 57.3: even units in P-Bus group 3, odd ones in group 4. */
#define G4MH_KCRC_BASE(n)                                                      \
    ((((n) & 1u) != 0u ? 0xFFD8E000u : 0xFFD40000u) + 0x200u * ((n) >> 1))
#define G4MH_KCRC_SIZE 0x100u

#define G4MH_KCRC_DIN 0x00u   /* 32, 16 or 8 bits                     */
#define G4MH_KCRC_DOUT0 0x80u /* the result's low half, XORed on read */
#define G4MH_KCRC_DOUT1 0x84u /* and its high half                    */
#define G4MH_KCRC_CTL 0x90u
#define G4MH_KCRC_POLY0 0xA0u
#define G4MH_KCRC_POLY1 0xA4u
#define G4MH_KCRC_XOR0 0xB0u
#define G4MH_KCRC_XOR1 0xB4u

/* KCRCnCTL */
#define G4MH_KCRC_CTL_PSIZE_SHIFT 16u
#define G4MH_KCRC_CTL_PSIZE_MASK (0x3Fu << 16)
#define G4MH_KCRC_CTL_CMD0 (1u << 8) /* reflect the output      */
#define G4MH_KCRC_CTL_CMD1 (1u << 5) /* reflect the input       */
#define G4MH_KCRC_CTL_CMD2 (1u << 4) /* all three, LSB first    */
#define G4MH_KCRC_CTL_DW_MASK 7u     /* 0: 32, 1: 16, 3: 8 bits */
#define G4MH_KCRC_CTL_WRITABLE                                                 \
    (G4MH_KCRC_CTL_PSIZE_MASK | G4MH_KCRC_CTL_CMD0 | G4MH_KCRC_CTL_CMD1 |      \
     G4MH_KCRC_CTL_CMD2 | G4MH_KCRC_CTL_DW_MASK)
#define G4MH_KCRC_CTL_RESET 0x001F0000u

typedef struct g4mh_kcrc {
    uint64_t state; /* the CRC register, before the output XOR       */
    uint64_t poly;
    uint64_t mask;  /* KCRCnXOR1:KCRCnXOR0                            */
    uint32_t ctl;
    uint32_t din;   /* the last value written, which is what DIN reads */
} g4mh_kcrc_t;

void g4mh_kcrc_init(g4mh_kcrc_t *k);

/* A context is one g4mh_kcrc_t, mapped at G4MH_KCRC_BASE(n). */
extern const emu_dev_ops_t g4mh_kcrc_ops;

#endif /* G4MH_KCRC_H */
