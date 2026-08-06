/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_memmap.h - Where the G4MH platform devices sit in the guest map.
 *
 * The RAM, ROM, console and passthrough windows are the same for every
 * frontend and live in emu/emu_memmap.h, which this header includes. What
 * is here is the part only an RH850 guest has, and the addresses are the
 * real ones from the RH850/U2B hardware manual (R01UH0923EJ0130, Table 6.7,
 * "Register Base Address") rather than somewhere convenient:
 *
 *   0xFFFC_0000  INTC1 SELF     core-local interrupt controller, this PE
 *   0xFFFC_4000  INTC1 PE0      the same registers named absolutely
 *   0xFFF8_0000  INTC2          global interrupt controller
 *   0xFFEC_0000  OSTM0          a time base
 *
 * Modelled on the RH850/U2B6: three G4MH cores, PE0 to PE2. The manual's
 * base table runs to PE5 because the larger U2B parts have six, and
 * Section 40 states plainly that "CPU3, CPU4, CPU5 are not implemented in
 * RH850/U2B6". This frontend is single-core, so only SELF and PE0 are
 * mapped; the stride is here so adding the other two is arithmetic.
 *
 * Using the architectural addresses matters more than it looks: an RH850
 * guest's startup code and any vendor driver reach these by the numbers
 * printed in the manual, exactly as an STM32 driver reaches its
 * peripherals through the identity-mapped passthrough window. An invented
 * map would mean every guest needs porting, which is the thing this
 * emulator exists to avoid.
 */
#ifndef G4MH_G4MH_MEMMAP_H
#define G4MH_G4MH_MEMMAP_H

#include "emu/emu_memmap.h"

/*
 * INTC1 is per-PE. SELF is an alias window onto whichever PE is executing,
 * which is how core-local code addresses its own controller without
 * knowing its own number -- the same idea as the RISC-V CLINT being
 * hart-local. Single-core here, so SELF and PE0 are the same registers.
 */
#define G4MH_INTC1_SELF_BASE    0xFFFC0000u
#define G4MH_INTC1_PE0_BASE     0xFFFC4000u
#define G4MH_INTC1_PE_STRIDE    0x00004000u
/* PE0..PE2 on the U2B6; the U2B10 and above go to PE5. */
#define G4MH_INTC1_PE_COUNT     3u
#define G4MH_INTC1_SIZE         0x00000400u

#define G4MH_INTC2_BASE         0xFFF80000u
#define G4MH_INTC2_SIZE         0x00008000u

/* OS timer. Minimal model; see g4mh_intc.h. */
#define G4MH_OSTM0_BASE         0xFFEC0000u
#define G4MH_OSTM0_SIZE         0x00000100u

#endif /* G4MH_G4MH_MEMMAP_H */
