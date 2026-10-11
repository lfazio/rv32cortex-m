/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_cpu.c - the frontend contract, for every frontend compiled in.
 *
 * The other files here test one frontend each. What belongs in this one
 * is a property the contract states about *all* of them, where the
 * defect is a frontend that did not hear about it.
 */

#include "tests.h"

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

#include <string.h>

/*
 * A core that has done nothing has not faulted.
 *
 * `emu_cpu_status_t` grew `faulted`, `fault_pc` and `fault_insn` for a
 * frontend that halts on an instruction it cannot execute, with the
 * contract saying everyone else "leaves them clear". The callers pass a
 * stack variable, and two of the four frontends assigned only the
 * members they had always assigned -- so every RV32 run was reported as
 * stopping on an instruction it could not execute, at a pc that was
 * whatever the stack held.
 *
 * The struct is poisoned first because that is the bug: zero-initialised
 * storage passes against it, and so did every run where the stack
 * happened to hold a zero in that byte.
 */
static void test_status_starts_clear(void)
{
    static emu_bus_t bus;

    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        emu_core_t core;
        emu_cpu_status_t st;

        emu_bus_init(&bus);
        if (!emu_core_open(&core, *p, &bus, 0u)) {
            CHECK(false);
            continue;
        }

        memset(&st, 0xA5, sizeof(st));
        emu_core_status(&core, &st);

        CHECK(!st.faulted);
        CHECK_EQ(st.fault_pc, 0u);
        CHECK_EQ(st.fault_insn, 0u);
        /* And it really did answer: the name is the frontend's own. */
        CHECK(st.backend != NULL);
    }
}

void test_cpu(void)
{
    test_status_starts_clear();
}
