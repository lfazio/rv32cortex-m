/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RV32_TESTS_H
#define RV32_TESTS_H

#include <stdint.h>
#include <stdio.h>

extern int g_checks;
extern int g_failures;

#define CHECK_EQ(got, want)  check_eq(__FILE__, __LINE__, #got, \
                                      (uint32_t)(got), (uint32_t)(want))
#define CHECK(cond)          check_eq(__FILE__, __LINE__, #cond, \
                                      (cond) ? 1u : 0u, 1u)

void check_eq(const char *file, int line, const char *expr,
              uint32_t got, uint32_t want);

#if EMU_FRONTEND_RV32
void test_decode(void);
#endif
void test_bus(void);
#if EMU_FRONTEND_RV32
void test_fpu(void);
#endif
#if EMU_FRONTEND_G4MH
void test_g4mh(void);
#endif

#endif /* RV32_TESTS_H */
