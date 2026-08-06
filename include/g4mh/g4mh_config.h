/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_config.h - Compile-time configuration of the RH850 G4MH frontend.
 *
 * Knobs that belong to the emulator rather than to the ISA -- the bus
 * region table, the run budget, the diagnostics -- live in emu/emu_config.h
 * and are shared with every other frontend.
 */
#ifndef G4MH_G4MH_CONFIG_H
#define G4MH_G4MH_CONFIG_H

#include "emu/emu_config.h"

/*
 * Processing elements to model.
 *
 * The RH850/U2B6 has three (PE0 to PE2; the larger U2B parts have six, and
 * the manual states plainly that "CPU3, CPU4, CPU5 are not implemented in
 * RH850/U2B6"). The default is 1 because the constraint is memory, not the
 * architecture: each PE has 64 KiB of local RAM on the part, so three cores
 * want ~192 KiB before any emulator state, against 128 KiB of SRAM total on
 * an F446. The host build raises it; the firmware cannot.
 *
 * See docs/host/g4mh/multicore.md.
 */
#ifndef G4MH_PE_COUNT
#  define G4MH_PE_COUNT 1u
#endif
#if G4MH_PE_COUNT < 1u || G4MH_PE_COUNT > 3u
#  error "G4MH_PE_COUNT must be 1..3 (the U2B6 has three PEs)"
#endif

/*
 * Interrupt channels the INTC implements. A channel number is the host's
 * interrupt number, exactly as it is for the RISC-V frontend's APLIC, so
 * this has to be wide enough for the host's vector table rather than for
 * any particular RH850 device.
 */
#ifndef G4MH_INT_CHANNELS
#  define G4MH_INT_CHANNELS 128u
#endif

/*
 * Coprocessor usage. The G4MH FPU is an option on real parts and is not
 * implemented here: an FP instruction raises a coprocessor-unusable
 * exception, which is what a part without the option does when PSW.CU1 is
 * clear. Kept as a macro so the check has a name rather than being an
 * unexplained "always trap".
 */
#ifndef G4MH_EXT_FPU
#  define G4MH_EXT_FPU 0
#endif

/* Build the disassembler (useful for tracing; costs flash). */
#ifndef G4MH_ENABLE_DISASM
#  define G4MH_ENABLE_DISASM 1
#endif

#endif /* G4MH_G4MH_CONFIG_H */
