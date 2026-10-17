/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_intc.h - Interrupt controller (INTC1 + INTC2).
 *
 * The G4MH counterpart of the RISC-V frontend's CLINT and APLIC, and the
 * split is the same one for the same reason:
 *
 *   INTC1   core-local. One per PE, at a per-PE base plus a SELF alias
 *           onto whichever PE is executing -- the CLINT's role. Owns
 *           channels 0..31.
 *   INTC2   global, shared by every PE -- the APLIC's role. Owns channels
 *           32..1023.
 *
 * What makes that split invisible to software is that `EICn` is one 16-bit
 * register at `base + 0x02 * n` in *both* units, so the channel array is a
 * single address space that happens to be implemented by two devices. A
 * guest computes the address from the channel number and does not care
 * which side of the boundary it lands on.
 *
 * Register layout and bit positions are from the RH850/U2B hardware manual
 * (R01UH0923EJ0130), Section 6.3, "Registers".
 *
 * A channel number is also the *host's* interrupt number, exactly as an
 * APLIC source is, so a guest driver names TIM6's interrupt by the same 54
 * the host's vector table uses and no translation table exists on either
 * side of the bridge.
 */
#ifndef G4MH_G4MH_INTC_H
#define G4MH_G4MH_INTC_H

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

#include "g4mh_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct g4mh_cpu;

/* ------------------------------------------------------------------ */
/* Register offsets                                                    */
/* ------------------------------------------------------------------ */

/* INTC1, from <INTC1_PEx_base>. Channels 0..31. */
#define G4MH_INTC1_EIC 0x0000u /* + 0x02 * n, n = 0..31      */
#define G4MH_INTC1_IMR0 0x00F0u
#define G4MH_INTC1_EIBD 0x0100u /* + 0x04 * n                 */
#define G4MH_INTC1_FIBD 0x01C0u
#define G4MH_INTC1_EEIC 0x0200u /* + 0x04 * n                 */
#define G4MH_INTC1_EIBG 0x0280u
#define G4MH_INTC1_FIBG 0x02C0u
#define G4MH_INTC1_IHVCFG 0x02F0u

/* INTC2, from <INTC2_base>. Channels 32..1023. */
#define G4MH_INTC2_EIC 0x0000u /* + 0x02 * n, n = 32..1023   */
#define G4MH_INTC2_IMR 0x1000u /* + 0x04 * n, n = 1..31      */
#define G4MH_INTC2_I2EIBG 0x1FE0u /* + 0x04 * m, m = 0..5       */
#define G4MH_INTC2_EIBD 0x2000u /* + 0x04 * n                 */
#define G4MH_INTC2_EEIC 0x4000u /* + 0x04 * n                 */

/*
 * INTIF, from <INTIF_base> = 0xFF09_0000. A separate block from either
 * INTC unit, and only one register of it is modelled.
 */
#define G4MH_INTIF_BASE 0xFF090000u
#define G4MH_INTIF_SIZE 0x00000400u
#define G4MH_INTIF_TPTMSEL 0x0200u

/* The channel at which INTC2 takes over from INTC1. */
#define G4MH_INTC1_CHANNELS 32u

/*
 * EIBDn fields. PEID names the PE the channel is bound to -- 000 is PE0,
 * 001 PE1, 010 PE2 -- which is what turns INTC2 from a shared pending
 * array into a router. GPID and the GM/CST/BCP bits belong to the
 * virtualization and broadcast features and are stored but not acted on.
 */
#define G4MH_EIBD_PEID_MASK 0x00000007u
#define G4MH_EIBD_GPID_SHIFT 8u

/* ------------------------------------------------------------------ */
/* Channel state: one set of bits, three windows onto it                */
/* ------------------------------------------------------------------ */

/*
 * **EICn, EEICn and IMRm are not three registers. They are three views
 * of one per-channel state**, and the manual says so in as many words:
 *
 *   "All bits except EIP[5:4] of EEICn are shared with EICn."   (6.3.7)
 *   "[IMR registers] are an aggregation of the EIMK bits from the EIC
 *    registers. Setting of the EIMK bit in the EIC register is reflected
 *    in this register. Also the setting of a bit in this register is
 *    reflected in the EIMK bit of the corresponding EIC register."
 *                                                               (6.3.3)
 *
 * Holding them as separate arrays -- which this file used to do -- lets
 * them disagree, and they disagreed in the direction that matters:
 * IMR was stored, never consulted when choosing a channel, and reset to
 * "everything unmasked" against an architectural reset of FFFFFFFFH.
 * A guest that masked a channel through IMR was not masked at all.
 *
 * So the canonical form below is **EEICn's layout**, because it is the
 * superset: 6 priority bits where EICn shows 4. EICn is the same word
 * seen through a narrower window, and writing it leaves EIP[5:4] alone
 * rather than clearing the two bits it cannot express.
 *
 * Layout and reset values: RH850/U2B hardware manual R01UH0923EJ0130,
 * tables 6.15 (EICn), 6.16 (IMRm) and 6.20 (EEICn).
 */

/* The canonical word, in EEICn's layout. */
#define G4MH_EEIC_EICT (1u << 31) /* level detection; READ ONLY */
#define G4MH_EEIC_EIRF (1u << 28) /* request pending           */
#define G4MH_EEIC_EIMK (1u << 23) /* masked                    */
#define G4MH_EEIC_EITB (1u << 22) /* table reference method    */
#define G4MH_EEIC_EIOV (1u << 15) /* overflow                  */
#define G4MH_EEIC_EIP_MASK 0x0000003Fu /* 0 highest, 63 lowest      */

/* Masked, lowest priority, edge detection -- 0080_000FH. */
#define G4MH_EEIC_RESET 0x0080000Fu

/*
 * EICn's window. Bit 4 and bits 14:13 and 11:8 are reserved and read
 * back their reset value, which is zero for all of them.
 */
#define G4MH_EIC_EIP_MASK 0x000Fu /* EIP[3:0] only               */
#define G4MH_EIC_EIOV 0x0020u
#define G4MH_EIC_EITB 0x0040u
#define G4MH_EIC_EIMK 0x0080u
#define G4MH_EIC_EIRF 0x1000u
#define G4MH_EIC_EICT 0x8000u

#define G4MH_EIC_RESET 0x008Fu

/* IMRm resets to every channel masked, which is EIMK's reset repeated. */
#define G4MH_IMR_RESET 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define G4MH_INTC2_SLOTS (G4MH_INT_CHANNELS - G4MH_INTC1_CHANNELS)
#define G4MH_INTC2_WORDS ((G4MH_INTC2_SLOTS + 31u) / 32u)

/* Cores INTC2 can notify; EIBD's PEID is three bits. */
#define G4MH_INTC2_PES 8u

/*
 * INTC2: channels 32 and up, one for the whole system.
 *
 * **Held once, and reached the same way from every core.** It used to be
 * the upper part of an array in every core's controller, of which only
 * PE0's was ever written -- so a request acknowledged, or a priority
 * looked up, through PE1's controller read PE1's untouched copy, and the
 * request stayed pending in PE0's after PE1 had taken it.
 */
typedef struct g4mh_intc2 {
    uint32_t chan[G4MH_INTC2_SLOTS]; /* channel 32 + i, EEICn's layout   */
    uint32_t eibd[G4MH_INTC2_SLOTS]; /* bind: which PE/VM takes it       */

    /*
     * Bit i of word w: channel 32 + 32 * w + i has EIRF set and EIMK
     * clear. Kept by the one function that writes a channel word, and
     * held equal to the words by test_intc_pending_summary.
     */
    uint32_t pend[G4MH_INTC2_WORDS];

    /*
     * The cores whose controllers share this, by PE number. A change to
     * a global channel is news to whichever core it is bound to, and to
     * the one it was bound to before an EIBD write, so all of them are
     * told: a spurious search costs a bitmap walk.
     */
    struct g4mh_cpu *cpu[G4MH_INTC2_PES];
} g4mh_intc2_t;

typedef struct g4mh_intc {
    /*
     * The controller holding the global INTC2 state, which every core's
     * shares. Self for the instance that *is* the global one (PE0's), so
     * a lookup never needs a null check on the hot side.
     */
    struct g4mh_intc *global;
    g4mh_intc2_t *intc2; /* global->intc2, cached: the same for every core */

    /*
     * This PE's own channels, 0..31, in EEICn's layout -- the superset
     * -- so EICn and IMRm are computed views rather than stored copies.
     * There is no imr[] array and there must not be one: two stores of
     * the same architectural bit is exactly how the mask came to be
     * ignored. The same holds for INTC2's half.
     */
    uint32_t chan[G4MH_INTC1_CHANNELS];
    uint32_t eibd[G4MH_INTC1_CHANNELS]; /* bind: which PE/VM takes it */
    uint32_t pend; /* bit n: chan[n] requested and unmasked, as INTC2's */

    uint32_t fibd;
    uint32_t eibg;
    uint32_t fibg;
    uint32_t ihvcfg;

    /*
     * TPTMSEL: bit n picks EIINT31 over FEINT for PE n's TPTM interval
     * interrupt. One register for the whole system, so it lives on the
     * global instance and is reached through `global` from any core --
     * the same arrangement as the INTC2 half.
     *
     * **Its reset value selects FEINT**, so the default path is the one
     * that needs an FE-level delivery. A frontend that implemented only
     * the EI half would work perfectly for a guest that sets the bit and
     * do nothing at all for one that does not, which is the worse of the
     * two failures: silence where the guest did the ordinary thing.
     */
    uint32_t tptmsel;

    /*
     * Pending FEINT, one bit per PE. FE-level interrupts ignore PSW.ID
     * and are refused only by PSW.NP, so they cannot go through the
     * EIINT machinery -- eic[] is priority and masking that does not
     * apply. A bitmask rather than a channel because the only FEINT
     * source here is the TPTM, and inventing FEINTF/FEINTMSK/FEINTC
     * around one source would be modelling the manual rather than the
     * part.
     */
    volatile uint8_t feint;

    struct g4mh_cpu *cpu;

    /*
     * Called when the guest unmasks a channel, so the platform can unmask
     * the host line behind it. Same handshake as the APLIC's, and for the
     * same reason: the host handler cannot service the device, only the
     * guest's driver can, so the line stays masked until the guest says it
     * has dealt with it.
     */
    emu_unmask_fn unmask;
    void *unmask_ctx;
} g4mh_intc_t;

/*
 * Three devices over one state. INTC1 is mapped twice -- once at the SELF
 * alias and once at this PE's absolute base -- because software uses both.
 */
extern const emu_dev_ops_t g4mh_intc1_ops;
extern const emu_dev_ops_t g4mh_intc2_ops;
extern const emu_dev_ops_t g4mh_intif_ops;

/*
 * `global` is the instance holding the INTC2 half -- PE0's. Pass NULL
 * when initialising that one itself, with the storage for INTC2 in
 * `intc2`, which is reset; it is ignored otherwise.
 */
void g4mh_intc_init(g4mh_intc_t *ic, struct g4mh_cpu *cpu, g4mh_intc_t *global,
                    g4mh_intc2_t *intc2);
void g4mh_intc_set_unmask(g4mh_intc_t *ic, emu_unmask_fn fn, void *ctx);

/*
 * Raise a channel. Safe to call from a host interrupt handler: it sets one
 * pending bit and a flag the run loop polls, and touches nothing the
 * interpreter could be midway through.
 */
void g4mh_intc_raise(g4mh_intc_t *ic, uint32_t channel);

/*
 * The highest-priority pending, unmasked channel for PE `pe`, or -1.
 *
 * Looks in two places, because the hardware does: this core's own INTC1
 * for channels 0-31, and the shared INTC2 for 32 and up, where only
 * channels whose EIBD.PEID names `pe` are candidates. EIP 0 is the highest
 * priority and a tie breaks towards the lower channel number.
 */
int g4mh_intc_pending(const g4mh_intc_t *ic, unsigned pe);

/*
 * The same search, also reporting the winner's EIP.
 *
 * The core needs the priority to apply its ceiling (ISPR, PSW.EIMASK,
 * PLMR), and only the controller knows it -- the channel number says
 * nothing about priority. Checking the *best* candidate alone is
 * sufficient and not a shortcut: every other pending channel has a
 * numerically larger EIP, so a ceiling that refuses the best refuses all
 * of them.
 */
int g4mh_intc_pending_pri(const g4mh_intc_t *ic, unsigned pe,
                          unsigned *priority);

/*
 * Is `channel` set to the table reference method (EEICn.EITB)?
 *
 * The core asks this when vectoring, because which of the two handler
 * address methods applies is a property of the *channel* and not of the
 * core -- and the answer lives in the one word this controller keeps per
 * channel, which EICn, EEICn and IMRm are three views of.
 *
 * Out of range reads false, so a core that somehow names a channel this
 * controller does not have takes the direct vector rather than reading a
 * table entry that is not there.
 */
bool g4mh_intc_chan_is_table(const g4mh_intc_t *ic, uint32_t channel);

/*
 * `channel`'s configured priority (EIP), which the core needs to pick a
 * direct vector offset. False and untouched if the channel is out of
 * range -- distinct from priority 0, which is the *highest*.
 */
bool g4mh_intc_chan_priority(const g4mh_intc_t *ic, uint32_t channel,
                             unsigned *priority);

/*
 * The TPTM's interval interrupt for PE `pe`, routed by TPTMSEL: EIINT31
 * when its bit is set, FEINT otherwise.
 *
 * The routing belongs here rather than in the timer because TPTMSEL is
 * an interrupt-controller register (manual section 6.3.15) and because
 * only this side can deliver an FE-level request.
 */
void g4mh_intc_raise_tptm(g4mh_intc_t *ic, unsigned pe);

/*
 * True if an FE-level interrupt is pending for `pe`. Cleared by
 * g4mh_intc_ack_fe once the core has taken it.
 */
bool g4mh_intc_fe_pending(const g4mh_intc_t *ic, unsigned pe);
void g4mh_intc_ack_fe(g4mh_intc_t *ic, unsigned pe);

/* Acknowledge a channel: clears its request flag. */
void g4mh_intc_ack(g4mh_intc_t *ic, uint32_t channel);

/*
 * Could anything wake PE `pe` from HALT: is any channel it could take --
 * its own, or a global one bound to it -- unmasked?
 */
bool g4mh_intc_wakeable(const g4mh_intc_t *ic, unsigned pe);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_INTC_H */
