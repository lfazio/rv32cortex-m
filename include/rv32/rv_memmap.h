/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_memmap.h - Where the RISC-V platform devices sit in the guest map.
 *
 * The RAM, ROM, console and passthrough windows are the same for every
 * frontend and live in emu/emu_memmap.h, which this header includes. What
 * is here is the part only a RISC-V guest has:
 *
 *   0x0200_0000  CLINT          virtual   timer + software interrupt
 *   0x0C00_0000  APLIC          virtual   external interrupts from real IRQs
 */
#ifndef RV32_RV_MEMMAP_H
#define RV32_RV_MEMMAP_H

#include "emu/emu_memmap.h"

#define RV_GUEST_CLINT_BASE     0x02000000u
#define RV_GUEST_ACLINT_MSWI_BASE   RV_GUEST_CLINT_BASE
#define RV_GUEST_ACLINT_MTIMER_BASE (RV_GUEST_CLINT_BASE + 0x4000u)

#define RV_GUEST_APLIC_BASE     0x0C000000u

#endif /* RV32_RV_MEMMAP_H */
