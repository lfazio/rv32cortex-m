/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_aplic.h - Advanced Platform-Level Interrupt Controller (AIA), the
 * subset an M-mode single-hart embedded target needs.
 *
 * This is what lets a guest driver take an interrupt from real silicon.
 * Everything else the guest touches goes straight to the STM32 through the
 * passthrough window, but interrupts cannot: the ARM NVIC vectors into the
 * *emulator*, not into the guest, so something has to carry the event
 * across. That is this device, and `rv_aplic_raise` is the crossing point.
 *
 * Direct delivery mode only. MSI mode targets an IMSIC, which needs a
 * second privilege level's worth of CSRs and has nothing to deliver to on a
 * core with one hart and no S-mode.
 *
 * Sources are 1..RV_APLIC_SOURCES-1; source 0 does not exist, which is the
 * spec's convention and is what makes 0 usable as "nothing pending".
 *
 * Registers implemented, at the offsets the AIA specification gives them:
 *
 *   0x0000  domaincfg    IE (bit 8) gates the whole domain
 *   0x0004  sourcecfg[1] .. one per source, SM in bits 2:0
 *   0x1C00  setip[0]     pending bitmap, write-1-to-set
 *   0x1CDC  setipnum     set the pending bit named by the value written
 *   0x1D00  in_clrip[0]  read: rectified input; write-1-to-clear pending
 *   0x1DDC  clripnum     clear the pending bit named by the value written
 *   0x1E00  setie[0]     enable bitmap, write-1-to-set
 *   0x1EDC  setienum     set the enable bit named by the value written
 *   0x1F00  clrie[0]     write-1-to-clear enable
 *   0x1FDC  clrienum     clear the enable bit named by the value written
 *   0x3004  target[1] ..  IPRIO in bits 7:0 (hart index is always 0 here)
 *   0x4000  idc0.idelivery
 *   0x4004  idc0.iforce
 *   0x4008  idc0.ithreshold
 *   0x4018  idc0.topi     read-only: highest-priority pending and enabled
 *   0x401C  idc0.claimi   reading it also clears that source's pending bit
 */
#ifndef RV32_RV_APLIC_H
#define RV32_RV_APLIC_H

#include "rv_types.h"
#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rv_hart;

/*
 * Enough sources that a source number can *be* the host's interrupt number.
 * The STM32F446 has 97 NVIC lines, and making the two numbering spaces the
 * same removes a translation table from the firmware and another from every
 * guest driver: a guest that would call HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn)
 * writes that same number to setienum instead.
 */
#ifndef RV_APLIC_SOURCES
#  define RV_APLIC_SOURCES 128u
#endif
#define RV_APLIC_WORDS  (RV_APLIC_SOURCES / 32u)

#define RV_APLIC_SIZE       0x8000u

/* Domain offsets. */
#define RV_APLIC_DOMAINCFG  0x0000u
#define RV_APLIC_SOURCECFG  0x0004u
#define RV_APLIC_SETIP      0x1C00u
#define RV_APLIC_SETIPNUM   0x1CDCu
#define RV_APLIC_IN_CLRIP   0x1D00u
#define RV_APLIC_CLRIPNUM   0x1DDCu
#define RV_APLIC_SETIE      0x1E00u
#define RV_APLIC_SETIENUM   0x1EDCu
#define RV_APLIC_CLRIE      0x1F00u
#define RV_APLIC_CLRIENUM   0x1FDCu
#define RV_APLIC_TARGET     0x3004u
#define RV_APLIC_IDC        0x4000u

#define RV_APLIC_IDC_IDELIVERY  0x00u
#define RV_APLIC_IDC_IFORCE     0x04u
#define RV_APLIC_IDC_ITHRESHOLD 0x08u
#define RV_APLIC_IDC_TOPI       0x18u
#define RV_APLIC_IDC_CLAIMI     0x1Cu

/* domaincfg */
#define RV_APLIC_DOMAINCFG_IE   (1u << 8)
/* The upper half reads back as 0x80 so software can identify the register. */
#define RV_APLIC_DOMAINCFG_RO   0x80000000u

/* sourcecfg SM values. */
#define RV_APLIC_SM_INACTIVE    0u
#define RV_APLIC_SM_DETACHED    1u
#define RV_APLIC_SM_EDGE_RISE   4u
#define RV_APLIC_SM_EDGE_FALL   5u
#define RV_APLIC_SM_LEVEL_HIGH  6u
#define RV_APLIC_SM_LEVEL_LOW   7u

/*
 * Called when a source's pending bit is cleared, which is the guest saying
 * it has serviced the device. On a platform bridging real interrupt lines
 * this is where the line is unmasked again; see rv_aplic_raise.
 */
/* Same shape as emu_unmask_fn, and assigned from it: the platform's
 * unmask hook arrives through emu_cpu_ops_t::set_unmask_hook. */
typedef emu_unmask_fn rv_aplic_eoi_fn;

typedef struct rv_aplic {
    uint32_t domaincfg;
    uint8_t  sourcecfg[RV_APLIC_SOURCES];
    uint8_t  target[RV_APLIC_SOURCES];    /* IPRIO; 0 means "never deliver" */
    uint32_t pending[RV_APLIC_WORDS];
    uint32_t enabled[RV_APLIC_WORDS];
    uint32_t idelivery;
    uint32_t iforce;
    uint32_t ithreshold;

    struct rv_hart *hart;
    rv_aplic_eoi_fn eoi;
    void           *eoi_ctx;
} rv_aplic_t;

extern const emu_dev_ops_t rv_aplic_ops;

void rv_aplic_init(rv_aplic_t *a, struct rv_hart *hart);

/* Install the completion callback used to unmask a real interrupt line. */
void rv_aplic_set_eoi(rv_aplic_t *a, rv_aplic_eoi_fn fn, void *ctx);

/*
 * Raise a source. Safe to call from a host interrupt handler: it touches
 * only the pending word and the hart's mip, both of which the run loop
 * re-reads rather than caches.
 *
 * The caller is expected to have masked the underlying line first. Nothing
 * here can service the device -- only the guest's driver can, through the
 * passthrough window -- so a level-triggered source left unmasked would
 * re-enter the host handler forever without the guest ever running.
 */
void rv_aplic_raise(rv_aplic_t *a, uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_APLIC_H */
