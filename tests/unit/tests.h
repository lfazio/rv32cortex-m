/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RV32_TESTS_H
#define RV32_TESTS_H

#include <stdint.h>
#include <stdio.h>

extern int g_checks;
extern int g_failures;

#define CHECK_EQ(got, want)  check_eq(__FILE__, __LINE__, #got, \
                                      (uint32_t)(got), (uint32_t)(want))
/*
 * The 64-bit form, and it exists because the 32-bit one **silently
 * truncates**. CHECK_EQ casts both sides to uint32_t, so every
 * assertion about a double or a register pair compared only the low
 * half: reversing SUBF.D's operands turns 2.0 into -2.0, which differ
 * in the *sign bit* -- the top of the high word -- and six tests of it
 * went on passing.
 *
 * Use this for anything wider than a register. The truncation is not a
 * compiler warning, because the cast is written down.
 */
#define CHECK_EQ64(got, want) check_eq64(__FILE__, __LINE__, #got, \
                                      (uint64_t)(got), (uint64_t)(want))
#define CHECK(cond)          check_eq(__FILE__, __LINE__, #cond, \
                                      (cond) ? 1u : 0u, 1u)

void check_eq(const char *file, int line, const char *expr,
              uint32_t got, uint32_t want);
void check_eq64(const char *file, int line, const char *expr,
                uint64_t got, uint64_t want);

#if EMU_FRONTEND_RV32
void test_decode(void);
#endif
void test_bus(void);
void test_ir(void);
#if EMU_FRONTEND_RV32
void test_fpu(void);
#endif
#if EMU_FRONTEND_G4MH
void test_g4mh(void);
#endif
#if EMU_FRONTEND_PPC
void test_ppc(void);
#endif

#endif /* RV32_TESTS_H */
