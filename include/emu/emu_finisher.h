/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_finisher.h - a register a guest writes to stop the machine.
 *
 * The SiFive test finisher's semantics (`sifive,test0`): one 32-bit
 * register, where a write of 0x5555 in the low half ends the run with a
 * pass, 0x3333 ends it with a failure whose code is the high half, and
 * 0x7777 asks for a reset. Reads return zero. Any other write is
 * ignored.
 *
 * It exists so that an operating system can power the machine off. An
 * OS does not exit; it asks its firmware to, and OpenSBI's generic
 * platform does that by writing a value to a `syscon` register named in
 * the device tree (`syscon-poweroff`, `syscon-reboot`): this is that
 * register. Before it, a Linux run ended with init exiting and the
 * kernel panicking, and had to reach its instruction cap or a timeout
 * before it printed its figures.
 *
 * Portable and ISA-agnostic, like the rest of src/emu: what "pass",
 * "fail" and "reset" do is the frontend's to say, through `finish`.
 */
#ifndef EMU_FINISHER_H
#define EMU_FINISHER_H

#include "emu/emu_dev.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_FINISHER_SIZE 0x1000u

#define EMU_FINISHER_PASS 0x5555u
#define EMU_FINISHER_FAIL 0x3333u
#define EMU_FINISHER_RESET 0x7777u

typedef enum {
    EMU_FINISH_EXIT, /* stop, with `code`: 0 for a pass */
    EMU_FINISH_RESET, /* the guest asked for a reset */
} emu_finish_t;

typedef struct emu_finisher {
    void (*finish)(void *ctx, emu_finish_t how, uint32_t code);
    void *ctx;
} emu_finisher_t;

/* EMU_FINISHER_SIZE bytes; the context is an emu_finisher_t. */
extern const emu_dev_ops_t emu_finisher_ops;

#ifdef __cplusplus
}
#endif

#endif /* EMU_FINISHER_H */
