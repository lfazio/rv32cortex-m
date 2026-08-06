/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_clint.h - Core local interruptor.
 *
 * Part of the RISC-V privileged platform rather than of the emulator: what
 * it does is drive mip.MSIP and mip.MTIP, which only a RISC-V hart has. It
 * therefore lives with the frontend, alongside the APLIC, while the
 * architecture-neutral devices sit in emu/emu_dev.h.
 */
#ifndef RV32_RV_CLINT_H
#define RV32_RV_CLINT_H

#include "emu/emu_bus.h"

#include "rv_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rv_hart;

/*
 * Register layout matches the de-facto SiFive CLINT that every RISC-V
 * toolchain and RTOS already targets:
 *
 *   +0x0000  msip       software interrupt pending (bit 0)
 *   +0x4000  mtimecmp   64-bit compare value
 *   +0xBFF8  mtime      64-bit free-running counter
 */
#define RV_CLINT_SIZE       0xC000u
#define RV_CLINT_MSIP       0x0000u
#define RV_CLINT_MTIMECMP   0x4000u
#define RV_CLINT_MTIME      0xBFF8u

typedef struct rv_clint {
    /*
     * mtime is written by the platform's timer interrupt and read by the
     * emulator loop, so it is volatile even though there is only one hart.
     */
    volatile uint64_t mtime;
    uint64_t          mtimecmp;
    uint32_t          msip;
    struct rv_hart   *hart;
} rv_clint_t;

extern const emu_dev_ops_t rv_clint_ops;

/*
 * ACLINT presents the same state as two devices that a platform may place
 * independently: MSWI (msip) and MTIMER (mtimecmp, and mtime at 0x7FF8).
 * Mapped at RV_GUEST_CLINT_BASE and +0x4000 they occupy exactly the legacy
 * CLINT window, which is why guests written for either layout work.
 */
#define RV_ACLINT_MSWI_SIZE     0x4000u
#define RV_ACLINT_MTIMER_SIZE   0x8000u
#define RV_ACLINT_MTIMER_MTIME  0x7FF8u

extern const emu_dev_ops_t rv_aclint_mswi_ops;
extern const emu_dev_ops_t rv_aclint_mtimer_ops;

/* Attach the CLINT to a hart and wire the hart's `time` CSR to mtime. */
void rv_clint_init(rv_clint_t *c, struct rv_hart *hart);

/*
 * Set the current time, then re-evaluate MTIP. Call this from whatever
 * drives time on the host (a hardware timer ISR, SysTick, or the emulator
 * loop itself). mtimecmp comparison is unsigned, per the spec.
 */
void rv_clint_set_time(rv_clint_t *c, uint64_t now);

/* Convenience: advance mtime by `delta` ticks. */
void rv_clint_advance(rv_clint_t *c, uint32_t delta);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_CLINT_H */
