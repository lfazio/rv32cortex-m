/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_mpu.h - RH850 G4MH memory protection unit.
 *
 * Register numbers, bit positions and the check rules are from the
 * RH850/U2B hardware manual R01UH0923EJ0130, section 3 "CPU System":
 * tables 3.65 (MPM), 3.66 (MPCFG), 3.74 (MPLA), 3.75 (MPUA), 3.76 (MPAT)
 * and 3.77 (MPIDn), with the violation rules from 3.2.5.1(4) and (5).
 * The virtualization manual R01UH0865EJ0140 gives the same register map
 * in its table 3.12 and defers every bit layout to the above.
 *
 * The shape of it
 * ---------------
 * Thirty-two entries, each three registers -- MPLA (minimum address),
 * MPUA (maximum address) and MPAT (attributes) -- reached through a
 * *window*: MPIDX selects the entry and the three registers refer to
 * whichever one that is. "It is impossible to manipulate the minimum
 * address of area n without using the MPIDX register and this register."
 *
 * That is the same arrangement the INTC has with EICn/EEICn/IMRm, and
 * the same rule applies: the entry array is the state and the three
 * system registers are a view of one element of it. Storing MPLA as a
 * system register in its own right would make thirty-two entries share
 * one slot.
 *
 * What it costs when off
 * ----------------------
 * One predicted branch on the fetch path and one per data access,
 * because `mpu_active` is false until a guest sets MPM.MPE. This project
 * has measured what the alternative costs: an unguarded second test in
 * the RISC-V fetch sequence was 9.3% of CoreMark. Do not add a second
 * condition beside `mpu_active` -- fold it in, the way fetch_guard does
 * on the other side.
 */
#ifndef G4MH_G4MH_MPU_H
#define G4MH_G4MH_MPU_H

#include "g4mh_config.h"
#include "g4mh_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct g4mh_cpu;

/* ------------------------------------------------------------------ */
/* Registers (selID 5)                                                 */
/* ------------------------------------------------------------------ */

#define G4MH_SR_MPM             0u    /* operation mode                 */
#define G4MH_SR_MPCFG           2u    /* configuration, read-only       */
#define G4MH_SR_MCA             8u    /* setting-check address          */
#define G4MH_SR_MCS             9u    /* setting-check size             */
#define G4MH_SR_MCC             10u   /* setting-check command          */
#define G4MH_SR_MCR             11u   /* setting-check result           */
#define G4MH_SR_MCI             12u   /* setting-check SPID             */
#define G4MH_SR_MPIDX           16u   /* which entry the window shows   */
#define G4MH_SR_MPBK            17u   /* bank select                    */
#define G4MH_SR_MPLA            20u   /* entry: minimum address         */
#define G4MH_SR_MPUA            21u   /* entry: maximum address         */
#define G4MH_SR_MPAT            22u   /* entry: attributes              */
#define G4MH_SR_MPID0           24u   /* .. MPID7 at 31                 */

#define G4MH_SR_SEL_MPU         5u    /* the selID all of them live in  */

/* MPM. Everything above bit 1 is reserved and reads zero. */
#define G4MH_MPM_MPE            (1u << 0)   /* protection enabled       */
#define G4MH_MPM_SVP            (1u << 1)   /* apply SX/SW/SR in SV mode */

/*
 * MPAT. The permission bits come in two independent groups and both have
 * to allow an access:
 *
 *   the mode group   SR/SW/SX for supervisor, UR/UW/UX for user
 *   the SPID group   RMPIDn/WMPIDn, indexed by which MPIDn holds the
 *                    accessing SPID -- or bypassed entirely by RG/WG
 *
 * RMPIDn covers *execution and reading* together, which is why there is
 * no XMPIDn: the SPID group has no separate execute permission.
 */
#define G4MH_MPAT_UR            (1u << 0)
#define G4MH_MPAT_UW            (1u << 1)
#define G4MH_MPAT_UX            (1u << 2)
#define G4MH_MPAT_SR            (1u << 3)
#define G4MH_MPAT_SW            (1u << 4)
#define G4MH_MPAT_SX            (1u << 5)
#define G4MH_MPAT_E             (1u << 7)   /* entry enabled            */
#define G4MH_MPAT_RG            (1u << 14)  /* read/execute: any SPID   */
#define G4MH_MPAT_WG            (1u << 15)  /* write: any SPID          */
#define G4MH_MPAT_RMPID_SHIFT   16u
#define G4MH_MPAT_WMPID_SHIFT   24u

/* MPCFG is read-only and reports what this build has. */
#define G4MH_MPCFG_ARCH         2u    /* RH850 v2.1 MPU                 */
#define G4MH_MPCFG_VALUE                                                \
    (((G4MH_MPU_ENTRIES - 1u) & 0x1Fu) | (G4MH_MPCFG_ARCH << 16))

/* MPIDn holds a five-bit SPID. */
#define G4MH_MPID_SPID_MASK     0x1Fu

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct g4mh_mpu {
    /*
     * The entries. Three parallel arrays rather than an array of structs
     * because the window reads and writes one register at a time and the
     * check reads all three of one entry -- neither layout helps the
     * other, and this one keeps the hot loop's three loads adjacent per
     * array rather than strided.
     */
    uint32_t mpla[G4MH_MPU_ENTRIES];
    uint32_t mpua[G4MH_MPU_ENTRIES];
    uint32_t mpat[G4MH_MPU_ENTRIES];

    uint32_t mpid[8];
    uint32_t mpm;
    uint32_t mpidx;

    /* The setting-check function, which is state and not a check. */
    uint32_t mca, mcs, mcc, mcr, mci;
} g4mh_mpu_t;

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

/*
 * What an access is for. Kept separate from emu_fault_t because that
 * says what *went wrong* and this says what was attempted -- and because
 * a fetch and a read differ here: RMPIDn covers both, but SX and UX are
 * not SR and UR.
 */
typedef enum g4mh_mpu_acc {
    G4MH_MPU_FETCH = 0,
    G4MH_MPU_READ,
    G4MH_MPU_WRITE
} g4mh_mpu_acc_t;

void g4mh_mpu_reset(g4mh_mpu_t *m);

/*
 * True if the access is permitted.
 *
 * Only called when `mpu_active`, so it does not re-test MPM.MPE: the
 * caller's branch is the guard, and testing twice is how a second
 * condition creeps onto the fetch path.
 *
 * `size` is in bytes and the whole span must be permitted -- an access
 * straddling the top of an area is a violation even if its first byte is
 * inside, which is the case a single-address check gets wrong.
 */
bool g4mh_mpu_permits(const g4mh_mpu_t *m, uint32_t addr, uint32_t size,
                      g4mh_mpu_acc_t acc, bool user_mode, uint32_t spid);

/*
 * Recompute what the fetch and access paths test. Call after anything
 * that could change MPM -- there is exactly one writer, the system
 * register path.
 */
bool g4mh_mpu_is_active(const g4mh_mpu_t *m);

/*
 * The selID-5 window. Return false for a register this unit does not
 * own, so the caller can fall through to the generic system-register
 * array rather than this file having to know what else lives there.
 */
bool g4mh_mpu_sr_read(const g4mh_mpu_t *m, unsigned reg, uint32_t *out);
bool g4mh_mpu_sr_write(g4mh_mpu_t *m, unsigned reg, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_MPU_H */
