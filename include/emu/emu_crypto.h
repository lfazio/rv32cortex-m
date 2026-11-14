/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_crypto.h - the cryptographic primitives more than one consumer
 * computes: the AES S-box and the carry-less multiply.
 *
 * Here and not in a frontend because the IR has operations for them
 * (EMU_IR_AESSB, EMU_IR_CLMUL), which a host with the instructions --
 * x86-64's AES-NI and PCLMULQDQ -- lowers directly, and whose reference
 * semantics the IR interpreter needs; RV32's scalar crypto uses the same
 * S-box. Nothing here knows an instruction set.
 *
 * **Data-independent time**, for the RISC-V Zkt promise the callers
 * make: the S-box reads all 256 entries and keeps one with a mask, and
 * the multiply masks each partial product rather than testing a bit.
 */
#ifndef EMU_CRYPTO_H
#define EMU_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>

/* AES's S-box at x & 0xFF, or its inverse; 0 to 255. */
uint32_t emu_aes_sbox(uint32_t x, bool inverse);

/* The 64-bit carry-less product of a and b. */
uint64_t emu_clmul64(uint32_t a, uint32_t b);

#endif /* EMU_CRYPTO_H */
