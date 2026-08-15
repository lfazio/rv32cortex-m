/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_intc.h - Interrupt controller (INTC1 + INTC2) and a time base.
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
#define G4MH_INTC1_EIC          0x0000u   /* + 0x02 * n, n = 0..31      */
#define G4MH_INTC1_IMR0         0x00F0u
#define G4MH_INTC1_EIBD         0x0100u   /* + 0x04 * n                 */
#define G4MH_INTC1_FIBD         0x01C0u
#define G4MH_INTC1_EEIC         0x0200u   /* + 0x04 * n                 */
#define G4MH_INTC1_EIBG         0x0280u
#define G4MH_INTC1_FIBG         0x02C0u
#define G4MH_INTC1_IHVCFG       0x02F0u

/* INTC2, from <INTC2_base>. Channels 32..1023. */
#define G4MH_INTC2_EIC          0x0000u   /* + 0x02 * n, n = 32..1023   */
#define G4MH_INTC2_IMR          0x1000u   /* + 0x04 * n, n = 1..31      */
#define G4MH_INTC2_I2EIBG       0x1FE0u   /* + 0x04 * m, m = 0..5       */
#define G4MH_INTC2_EIBD         0x2000u   /* + 0x04 * n                 */
#define G4MH_INTC2_EEIC         0x4000u   /* + 0x04 * n                 */

/*
 * INTIF, from <INTIF_base> = 0xFF09_0000. A separate block from either
 * INTC unit, and only one register of it is modelled.
 */
#define G4MH_INTIF_BASE         0xFF090000u
#define G4MH_INTIF_SIZE         0x00000400u
#define G4MH_INTIF_TPTMSEL      0x0200u

/* The channel at which INTC2 takes over from INTC1. */
#define G4MH_INTC1_CHANNELS     32u

/*
 * EIBDn fields. PEID names the PE the channel is bound to -- 000 is PE0,
 * 001 PE1, 010 PE2 -- which is what turns INTC2 from a shared pending
 * array into a router. GPID and the GM/CST/BCP bits belong to the
 * virtualization and broadcast features and are stored but not acted on.
 */
#define G4MH_EIBD_PEID_MASK     0x00000007u
#define G4MH_EIBD_GPID_SHIFT    8u

/* ------------------------------------------------------------------ */
/* EICn bits                                                           */
/* ------------------------------------------------------------------ */

/*
 * Reset is 0x008F: masked, lowest priority, edge detection. So a channel
 * delivers nothing until software has both lowered EIMK and set a
 * priority, which is why nothing is delivered before a guest configures
 * its controller.
 */
#define G4MH_EIC_EIP_MASK       0x000Fu   /* priority; 0 highest, 15 lowest */
#define G4MH_EIC_EIOV           0x0020u   /* overflow                    */
#define G4MH_EIC_EITB           0x0040u   /* table reference enable      */
#define G4MH_EIC_EIMK           0x0080u   /* masked                      */
#define G4MH_EIC_EIRF           0x1000u   /* request pending             */
#define G4MH_EIC_EICT           0x8000u   /* level detection (else edge) */

#define G4MH_EIC_RESET          0x008Fu

/* ------------------------------------------------------------------ */
/* OS timer                                                            */
/* ------------------------------------------------------------------ */

/*
 * A minimal free-running counter with a compare match, standing in for the
 * RH850 OSTM. Enough for a guest to have a time base and a periodic
 * interrupt; not the real OSTM register set, which is why it is documented
 * as a stand-in rather than presented as the peripheral.
 */
#define G4MH_OSTM_CNT           0x0000u   /* 64-bit, read-only          */
#define G4MH_OSTM_CMP           0x0008u   /* 64-bit compare             */

#ifndef G4MH_OSTM_CHANNEL
#  define G4MH_OSTM_CHANNEL     0u
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct g4mh_intc {
    /*
     * The global INTC2 state, which every core's controller shares. Self
     * for the instance that *is* the global one (PE0's), so a lookup never
     * needs a null check on the hot side.
     *
     * The split is the hardware's: EICn is one 16-bit register at
     * base + 0x02 * n across both units, so channels 0-31 come from this
     * core's own array and 32 and up from the shared one -- the same array
     * index, a different object.
     */
    struct g4mh_intc *global;

    /*
     * One entry per channel, spanning both units: 0..31 are answered by
     * INTC1 and the rest by INTC2, but they are the same array because
     * that is what the register layout says they are.
     */
    uint16_t eic[G4MH_INT_CHANNELS];
    uint32_t eibd[G4MH_INT_CHANNELS];   /* bind: which PE/VM takes it   */

    uint32_t imr[32];                   /* IMR0 in INTC1, IMR1.. in INTC2 */
    uint32_t fibd;
    uint32_t eibg;
    uint32_t fibg;
    uint32_t ihvcfg;

    /*
     * Written by the platform's timer interrupt and read by the emulator
     * loop, so volatile even though there is only one core.
     */
    volatile uint64_t ostm_cnt;
    uint64_t          ostm_cmp;

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
    void         *unmask_ctx;
} g4mh_intc_t;

/*
 * Three devices over one state. INTC1 is mapped twice -- once at the SELF
 * alias and once at this PE's absolute base -- because software uses both.
 */
extern const emu_dev_ops_t g4mh_intc1_ops;
extern const emu_dev_ops_t g4mh_intc2_ops;
extern const emu_dev_ops_t g4mh_ostm_ops;
extern const emu_dev_ops_t g4mh_intif_ops;

/*
 * `global` is the instance holding the INTC2 half -- PE0's. Pass NULL when
 * initialising that one itself.
 */
void g4mh_intc_init(g4mh_intc_t *ic, struct g4mh_cpu *cpu,
                    g4mh_intc_t *global);
void g4mh_intc_set_unmask(g4mh_intc_t *ic, emu_unmask_fn fn, void *ctx);

/*
 * Raise a channel. Safe to call from a host interrupt handler: it sets one
 * pending bit and a flag the run loop polls, and touches nothing the
 * interpreter could be midway through.
 */
void g4mh_intc_raise(g4mh_intc_t *ic, uint32_t channel);

/* Set or advance the time base, then re-evaluate the compare match. */
void g4mh_intc_set_time(g4mh_intc_t *ic, uint64_t now);
void g4mh_intc_advance(g4mh_intc_t *ic, uint32_t delta);

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

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_INTC_H */
