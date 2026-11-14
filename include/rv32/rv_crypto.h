/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_crypto.h - RISC-V scalar cryptography, RV32: Zbkb's own
 * instructions, Zbkx, Zknd, Zkne, Zknh, Zksed and Zksh.
 *
 * One executor for both backends, as rv_hart_amo is one for the atomics:
 * the interpreter calls it from its decode, and the translator hands it
 * the instruction through a helper (rv_ir.c), so the two cannot drift.
 * What Zbkb shares with Zbb -- the rotates, andn, orn, xnor, rev8 -- and
 * Zbkc with Zbc -- clmul, clmulh -- stays where Zbb and Zbc decode it;
 * this file has the rest.
 *
 * **Data-independent latency (Zkt), by construction.** Zkt covers every
 * one of these, and an S-box read at an index taken from the data is a
 * cache-timing channel on the host. So each S-box lookup reads all 256
 * entries and keeps one with a mask, and nothing here tests a value:
 * docs/frontend/rv32.md has the examination.
 */
#ifndef RV_CRYPTO_H
#define RV_CRYPTO_H

#include "rv32/rv_config.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Whether `insn` is one of this file's instructions in this build. A
 * decode only, for the translator, which routes what it claims to
 * rv_crypto_exec at run time.
 */
bool rv_crypto_is(uint32_t insn);

/*
 * Execute `insn` on its operands: `a` is rs1's value and `b` rs2's (or
 * ignored). False, and *out untouched, when it is not one of this
 * file's instructions in this build -- the caller's illegal instruction.
 */
bool rv_crypto_exec(uint32_t insn, uint32_t a, uint32_t b, uint32_t *out);

#endif /* RV_CRYPTO_H */
