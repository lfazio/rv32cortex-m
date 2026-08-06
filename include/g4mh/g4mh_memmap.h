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

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * Taken from the Y-ASK-RH850U2B board package's linker scripts
 * (Source/Make/CSP/r7f7025*.csp.ld), which is the authority a real guest
 * is actually built against -- more useful here than the manual's prose,
 * because it is the file the vendor's own toolchain reads.
 *
 * The three device variants differ only in how much of each region
 * exists, never in where it starts:
 *
 *            flash banks        local RAM   cluster RAM   PEs
 *   U2B6     2 x 3M             3 x 64K     384K          3
 *   U2B10    3M,3M,2M,2M        4 x 64K     1M            4
 *   U2B24    6 x 4M             6 x 64K     1.5M + 2.5M   6
 *
 * So the bases below are shared and the sizes follow G4MH_PE_COUNT. What
 * is modelled is one flash bank, the local RAMs and one cluster RAM --
 * enough for a guest linked by the vendor's scripts to load and run. The
 * extra flash banks and the retention/ERAM regions are address space this
 * emulator has nothing to put behind yet.
 */

/* Code flash, bank A. Reset fetches from 0 unless RBASE says otherwise. */
#define G4MH_FLASH_BASE         0x00000000u
#define G4MH_FLASH_SIZE         0x00300000u    /* 3 MiB, the U2B6's bank A */

/* Boot cluster. Two banks, for the A/B update scheme. */
#define G4MH_BOOT_A_BASE        0x08000000u
#define G4MH_BOOT_B_BASE        0x08300000u
#define G4MH_BOOT_SIZE          0x00010000u    /* 64 KiB each */

/*
 * Local RAM, and the reason this needs a bus per core.
 *
 * Every PE sees its *own* local RAM at the SELF address, so 0xFDE00000
 * means different memory depending on which core is executing -- the same
 * shape as INTC1 SELF above, and the same reason core-local startup code
 * can be one image shared by every PE.
 *
 * The absolute windows run *downwards* from PE0 at 0xFDC00000, 2 MiB
 * apart, so PE n is at PE0_BASE - n * STRIDE. That is the one thing here
 * easy to get backwards, and getting it backwards puts PE1's RAM where
 * nothing is mapped rather than producing a wrong answer.
 */
#define G4MH_LRAM_SELF_BASE     0xFDE00000u
#define G4MH_LRAM_PE0_BASE      0xFDC00000u
#define G4MH_LRAM_PE_STRIDE     0x00200000u
#define G4MH_LRAM_SIZE          0x00010000u    /* 64 KiB per PE */

/* PE n's local RAM, seen from any core. */
#define G4MH_LRAM_PE_BASE(n) \
    (G4MH_LRAM_PE0_BASE - (uint32_t)(n) * G4MH_LRAM_PE_STRIDE)

/*
 * Cluster RAM: one region every PE shares, at the same address on all of
 * them. The U2B6 splits its 384 KiB into 352 KiB plus a 32 KiB retention
 * area at 0xFE058000; retention is a power-domain property with nothing to
 * model here, so it is one region.
 */
#define G4MH_CRAM_BASE          0xFE000000u
#define G4MH_CRAM_SIZE          0x00060000u    /* 384 KiB */

#endif /* G4MH_G4MH_MEMMAP_H */
