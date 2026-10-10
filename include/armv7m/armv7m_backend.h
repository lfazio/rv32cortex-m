/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_backend.h - The ARMv7-M frontend's execution engines.
 *
 * The same shape as g4mh/g4mh_backend.h, deliberately: an interpreter
 * that is always there and is the one checked against a Cortex-M7, and
 * an IR JIT built where the host has an emitter. The JIT is a fast path
 * over the interpreter, not a replacement -- what it does not lower it
 * hands to the interpreter one instruction at a time, inside the block
 * where it can and by ending the block where it cannot.
 */
#ifndef ARMV7M_BACKEND_H
#define ARMV7M_BACKEND_H

#include "emu/emu_backend.h"
#include "emu/emu_jit.h"

#include "armv7m/armv7m_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interpreter. Always available. */
extern const emu_backend_t armv7m_backend_interp;

#if EMU_HAVE_JIT
/* The IR JIT; see armv7m_ir.c for what it lowers and what it does not. */
extern const emu_backend_t armv7m_backend_jit;
#endif

/*
 * The backend in use. The platform chooses -- `--jit` on a host -- and
 * the default is the interpreter, because on a host the difference is
 * what a passing run *proves*, not how fast it ran.
 */
extern const emu_backend_t *armv7m_backend;

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_BACKEND_H */
