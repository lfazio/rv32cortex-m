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

void test_decode(void);
void test_bus(void);
void test_fpu(void);

#endif /* RV32_TESTS_H */
