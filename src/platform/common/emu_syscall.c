/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_syscall.c - the two guest services every board offers, shared.
 *
 * newlib's `write` and `exit`, which is all a test guest needs and all
 * this offers: a guest that wants more is a guest that has outgrown
 * being a test. The numbers are newlib's (64 and 93) and reach here
 * through the frontend's syscall hook, which is why nothing in this file
 * knows what instruction raised them -- RISC-V's ECALL and G4MH's TRAP
 * both arrive the same way.
 *
 * It was identical in both runners. The four things it needs from the
 * board -- the bus to read the guest's buffer from, the core to halt,
 * and somewhere to record the exit status -- come through the `user`
 * pointer that emu_syscall_fn already carries, so sharing it needed a
 * context rather than a rewrite.
 */

#include "emu_console.h"

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

bool emu_guest_syscall(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    emu_syscall_ctx_t *const ctx = (emu_syscall_ctx_t *)user;

    switch (sc->nr) {
    case 64: {
        /*
         * write(fd, buf, len). The fd is ignored: there is one console
         * and a guest writing to stderr wants it on the same wire as one
         * writing to stdout.
         *
         * Read a byte at a time through the bus rather than reaching into
         * guest RAM, because the buffer may span regions -- and because
         * a bad pointer must fault here rather than in the firmware.
         * Stopping at the first fault reports the prefix, which is more
         * useful than reporting nothing.
         */
        const uint32_t buf = sc->arg[1];
        const uint32_t len = sc->arg[2];

        for (uint32_t i = 0; i < len; i++) {
            uint32_t byte;

            if (emu_bus_read(ctx->bus, buf + i, 1u, &byte) != EMU_FAULT_NONE) {
                break;
            }
            emu_console_putb((uint8_t)byte);
        }
        sc->ret = len;
        return true;
    }

    case 93:
        /*
         * exit(status). Recorded *and* halted: the run loop stops
         * because the core is halted, and the status is what the harness
         * reads afterwards. A guest that halts without calling this
         * leaves the previous status behind, which is why the platform
         * clears both before each run.
         */
        ctx->exit->code = sc->arg[0];
        ctx->exit->exited = true;
        ctx->core->ops->halt(cpu);
        return true;

    default:
        return false;
    }
}
