/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_dev.h - Virtual devices that belong to no particular architecture.
 *
 * A console UART is a console UART whatever the guest core is, so it lives
 * here and every frontend gets it. Devices that are part of an
 * architecture's own privileged design -- the RISC-V CLINT and APLIC, an
 * RH850 INTC2 -- belong to that frontend instead, because raising an
 * interrupt means writing that architecture's pending register.
 *
 * Everything else the guest touches goes straight to real hardware through
 * the passthrough window.
 */
#ifndef EMU_DEV_H
#define EMU_DEV_H

#include "emu_types.h"
#include "emu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Console UART                                                        */
/* ------------------------------------------------------------------ */

/*
 * A minimal NS16550 subset: enough for a guest's putchar/getchar and for
 * anything that pokes THR and polls LSR, which covers the usual bare-metal
 * console code and OpenSBI-style output.
 *
 *   +0x00  RBR (read) / THR (write)
 *   +0x05  LSR: bit 0 = data ready, bit 5 = THR empty, bit 6 = TX idle
 */
#define EMU_UART_SIZE        0x100u
#define EMU_UART_RBR_THR     0x00u
#define EMU_UART_IER         0x01u
#define EMU_UART_IIR_FCR     0x02u
#define EMU_UART_LCR         0x03u
#define EMU_UART_MCR         0x04u
#define EMU_UART_LSR         0x05u
#define EMU_UART_MSR         0x06u
#define EMU_UART_SCR         0x07u

#define EMU_UART_LSR_DR      0x01u
#define EMU_UART_LSR_THRE    0x20u
#define EMU_UART_LSR_TEMT    0x40u

typedef struct emu_uart {
    /* Backing transport, supplied by the platform. */
    void (*tx)(void *ctx, uint8_t c);
    /* Return the next byte, or -1 if none is available. */
    int  (*rx)(void *ctx);
    void *ctx;

    /*
     * Reporting LSR.DR requires knowing whether a byte is available
     * without consuming it, but the transport only offers a destructive
     * read. One byte of lookahead bridges the two.  -1 means empty.
     */
    int     pending;

    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
} emu_uart_t;

extern const emu_dev_ops_t emu_uart_ops;

void emu_uart_init(emu_uart_t *u,
                   void (*tx)(void *ctx, uint8_t c),
                   int (*rx)(void *ctx),
                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EMU_DEV_H */
