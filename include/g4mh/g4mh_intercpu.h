/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_intercpu.h - The three inter-CPU peripherals: BARR, IPIR and TPTM.
 *
 * Section 3.3 of the U2B hardware manual (R01UH0923EJ0130) groups these
 * as one subsystem on the inter-cluster bus, and they are here for the
 * same reason: they share a shape. Each is one global state object, and
 * each presents a *self region* whose meaning depends on which PE is
 * doing the access -- "when PE1 accesses the IPI0REQS register, PE1 can
 * also access the IPI0REQ1 register".
 *
 * That shape is why they cost so little here. Every core already has its
 * own bus, so a self region is an ordinary MMIO region bound to a
 * per-core context that names the PE; nothing on the access path learns
 * that multicore exists. The same trick already carries INTC1-self and
 * LRAM-self.
 *
 * **BarrierSync is not an instruction.** It is easy to go looking for an
 * `HBARR` opcode: there is none, CC-RH rejects the mnemonic at
 * `-Xcpu=g4mh`, and the manual lists BARR beside IPIR and the TPTM as
 * modules with register maps. It needs an emu_dev_ops_t and a base
 * address, not a decoder case.
 *
 * What is *not* modelled, in all three: the guard registers
 * (BRGPROT/IPIGPROT/TPTGPROT), address EDC and data ECC, and the
 * debug-mode counter stop signals. Those describe faults and access
 * control this emulator has no way to raise, and inventing them would
 * be modelling the manual rather than the part.
 */
#ifndef G4MH_G4MH_INTERCPU_H
#define G4MH_G4MH_INTERCPU_H

#include "emu/emu_bus.h"

#include "g4mh_config.h"
#include "g4mh_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct g4mh_intc;

/*
 * The manual's register files are sized for six PEs whatever the part
 * populates, and the "hidden PE" notes describe reads returning 0 for
 * the absent ones. The arrays here follow the manual rather than
 * G4MH_PE_COUNT so that an access to PE5's register does what the part
 * does -- reads zero -- instead of running off the end.
 */
#define G4MH_INTERCPU_PES       6u

/* ------------------------------------------------------------------ */
/* BARR - Barrier-Synchronization, 16 channels                         */
/* ------------------------------------------------------------------ */

#define G4MH_BARR_CHANNELS      16u

#define G4MH_BARR_INIT          0x000u    /* + 0x10 * n, W             */
#define G4MH_BARR_EN            0x004u    /* + 0x10 * n, R/W           */
#define G4MH_BARR_CHKS          0x100u    /* + 0x10 * n, self          */
#define G4MH_BARR_SYNCS         0x104u    /* + 0x10 * n, self          */
#define G4MH_BARR_CHK           0x800u    /* + 0x10 * n + 0x100 * m    */
#define G4MH_BARR_SYNC          0x804u    /* + 0x10 * n + 0x100 * m    */

typedef struct g4mh_barrier {
    uint8_t en[G4MH_BARR_CHANNELS];                        /* BRnEN    */
    uint8_t chk[G4MH_BARR_CHANNELS];                       /* bit m    */
    uint8_t sync[G4MH_BARR_CHANNELS];                      /* bit m    */
} g4mh_barrier_t;

void g4mh_barrier_init(g4mh_barrier_t *b);

/* ------------------------------------------------------------------ */
/* IPIR - Inter-Processor Interrupt, 4 channels                        */
/* ------------------------------------------------------------------ */

#define G4MH_IPIR_CHANNELS      4u

#define G4MH_IPIR_ENS           0x000u    /* + 0x20 * n, self          */
#define G4MH_IPIR_FLGS          0x004u
#define G4MH_IPIR_FCLRS         0x008u
#define G4MH_IPIR_REQS          0x010u
#define G4MH_IPIR_RCLRS         0x014u
#define G4MH_IPIR_EN            0x800u    /* + 0x20 * n + 0x100 * m    */
#define G4MH_IPIR_FLG           0x804u
#define G4MH_IPIR_FCLR          0x808u
#define G4MH_IPIR_REQ           0x810u
#define G4MH_IPIR_RCLR          0x814u

/*
 * INTIPIRn is EIINTn -- channel 0 of the IPIR is interrupt channel 0.
 * Table 3.147; the coincidence is the hardware's, not a simplification.
 */
#define G4MH_IPIR_CHANNEL(n)    (n)

typedef struct g4mh_ipir {
    /* [n][m]: the register of channel n belonging to PE m. */
    uint8_t en[G4MH_IPIR_CHANNELS][G4MH_INTERCPU_PES];
    uint8_t flg[G4MH_IPIR_CHANNELS][G4MH_INTERCPU_PES];
    uint8_t req[G4MH_IPIR_CHANNELS][G4MH_INTERCPU_PES];

    /* Where a raised channel goes. One per PE, since each has its own. */
    struct g4mh_intc *intc[G4MH_PE_COUNT];
} g4mh_ipir_t;

void g4mh_ipir_init(g4mh_ipir_t *p);
void g4mh_ipir_bind(g4mh_ipir_t *p, unsigned pe, struct g4mh_intc *ic);

/* ------------------------------------------------------------------ */
/* TPTM - Time Protection Timer                                        */
/* ------------------------------------------------------------------ */

/*
 * One timer set per PE: two interval timers (down, reload from ILD on
 * start, underflow flag and one shared interrupt), one free-run timer
 * (up), and two up timers (up, four compare values each with their own
 * interrupt).
 *
 * The self block is at offset 0 and PEn's at 0x100 + 0x100 * n, so the
 * self region is the same layout at a different base -- which is why
 * one register decoder serves both.
 */
#define G4MH_TPTM_SELF          0x000u
#define G4MH_TPTM_PE(n)         (0x100u + 0x100u * (n))
#define G4MH_TPTM_BLOCK         0x100u

#define G4MH_TPTM_IRUN          0x00u   /* W: start interval ch, load ILD */
#define G4MH_TPTM_IRRUN         0x04u   /* W: restart (reload, keep run)  */
#define G4MH_TPTM_ISTP          0x08u   /* W: stop                        */
#define G4MH_TPTM_ISTR          0x0Cu   /* R: running                     */
#define G4MH_TPTM_IIEN          0x10u   /* R/W: underflow interrupt enable */
#define G4MH_TPTM_IUSTR         0x14u   /* R/W0C: underflow flags         */
#define G4MH_TPTM_IDIV          0x18u   /* R/W: clock / (IDIV + 1)        */
#define G4MH_TPTM_FRUN          0x20u
#define G4MH_TPTM_FRRUN         0x24u
#define G4MH_TPTM_FSTP          0x28u
#define G4MH_TPTM_FSTR          0x2Cu
#define G4MH_TPTM_FDIV          0x30u
#define G4MH_TPTM_URUN          0x40u
#define G4MH_TPTM_URRUN         0x44u
#define G4MH_TPTM_USTP          0x48u
#define G4MH_TPTM_USTR          0x4Cu
#define G4MH_TPTM_UIEN          0x50u
#define G4MH_TPTM_UCSTR         0x54u
#define G4MH_TPTM_UDIV          0x58u
#define G4MH_TPTM_UTRG          0x5Cu
#define G4MH_TPTM_UICFG         0x60u
#define G4MH_TPTM_ICNT0         0x80u
#define G4MH_TPTM_ILD0          0x84u
#define G4MH_TPTM_ICNT1         0x88u
#define G4MH_TPTM_ILD1          0x8Cu
#define G4MH_TPTM_FCNT          0xA0u
#define G4MH_TPTM_UCNT0         0xC0u
#define G4MH_TPTM_UCMP0(i)      (0xC4u + 4u * (i))
#define G4MH_TPTM_UCNT1         0xE0u
#define G4MH_TPTM_UCMP1(i)      (0xE4u + 4u * (i))

#define G4MH_TPTM_INTERVALS     2u
#define G4MH_TPTM_UPTIMERS      2u
#define G4MH_TPTM_COMPARES      4u

/*
 * The interval interrupt is EIINT31 when TPTMSEL selects EI, and FEINT
 * otherwise -- see g4mh_intc.h, which owns TPTMSEL.
 *
 * The up-timer interrupts are EIINT413..416 for PE0 and rise in blocks
 * from there, which is past G4MH_INT_CHANNELS in every configuration
 * this frontend builds. They are raised through g4mh_intc_raise anyway,
 * which drops an out-of-range channel; that is a deliberate no-op and
 * not an oversight, and the flags in UCSTR are set either way, so a
 * guest polling them sees the timer work.
 */
#define G4MH_TPTM_EI_CHANNEL    31u
#define G4MH_TPTM_U_CHANNEL(pe, m, i) \
    (413u + 128u * (unsigned)(pe) + 4u * (unsigned)(m) + (unsigned)(i))

typedef struct g4mh_tptm_pe {
    uint32_t icnt[G4MH_TPTM_INTERVALS];
    uint32_t ild[G4MH_TPTM_INTERVALS];
    uint32_t fcnt;
    uint32_t ucnt[G4MH_TPTM_UPTIMERS];
    uint32_t ucmp[G4MH_TPTM_UPTIMERS][G4MH_TPTM_COMPARES];

    uint8_t  irun;                  /* ISTR: bit per interval channel  */
    uint8_t  iien;
    uint8_t  iustr;
    uint8_t  idiv;
    uint8_t  frun;                  /* FSTR: bit 0                     */
    uint8_t  fdiv;
    uint8_t  urun;                  /* USTR: bit per up-timer channel  */
    uint8_t  udiv;
    uint16_t uien;                  /* U0IEN0-3 in 3:0, U1IEN0-3 in 11:8 */
    uint16_t ucstr;
    uint16_t utrg;
    uint8_t  uicfg;

    /*
     * Fractional carry for each divider, so a divider of 3 advances the
     * counter once every 4 ticks *across* calls rather than losing the
     * remainder at every slice boundary. Guest time arrives in chunks of
     * a run budget, not one tick at a time, which is exactly when
     * dropping the remainder stops being a rounding error and becomes a
     * clock that runs slow by a factor of the slice length.
     */
    uint32_t iacc, facc, uacc;
} g4mh_tptm_pe_t;

typedef struct g4mh_tptm {
    g4mh_tptm_pe_t pe[G4MH_PE_COUNT];
    struct g4mh_intc *intc[G4MH_PE_COUNT];
} g4mh_tptm_t;

void g4mh_tptm_init(g4mh_tptm_t *t);
void g4mh_tptm_bind(g4mh_tptm_t *t, unsigned pe, struct g4mh_intc *ic);

/* Advance every running counter by `ticks` of cpu_clk. */
void g4mh_tptm_advance(g4mh_tptm_t *t, uint32_t ticks);

/* ------------------------------------------------------------------ */
/* Devices                                                             */
/* ------------------------------------------------------------------ */

/*
 * Each region is bound to one of these rather than to the state itself,
 * because the self registers need to know who is asking. `pe` is the
 * accessing PE for a self region; the absolute windows ignore it.
 */
typedef struct g4mh_intercpu_port {
    void    *state;
    unsigned pe;
} g4mh_intercpu_port_t;

extern const emu_dev_ops_t g4mh_barrier_ops;
extern const emu_dev_ops_t g4mh_ipir_ops;
extern const emu_dev_ops_t g4mh_tptm_ops;

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_INTERCPU_H */
