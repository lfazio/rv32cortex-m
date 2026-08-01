/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_main.c - Host unit tests for the pieces that are awkward to reach
 * from a guest program: RVC expansion of encodings a compiler never emits,
 * and bus permission enforcement.
 */

#include "tests.h"

int g_checks;
int g_failures;

void check_eq(const char *file, int line, const char *expr,
              uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        fprintf(stderr, "%s:%d: %s\n    got  0x%08x\n    want 0x%08x\n",
                file, line, expr, got, want);
    }
}

int main(void)
{
    test_decode();
    test_bus();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
