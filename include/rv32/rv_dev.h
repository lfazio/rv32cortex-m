/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_dev.h - Virtual devices.
 *
 * These are the pieces of a RISC-V platform that have no ARM equivalent to
 * pass through to, so they are emulated: the CLINT (timer and software
 * interrupt) and a console UART. Everything else the guest touches goes
 * straight to real STM32 hardware through the passthrough window.
 */
#ifndef RV32_RV_DEV_H
#define RV32_RV_DEV_H

#include "rv_types.h"
#include "rv_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rv_hart;

/* ------------------------------------------------------------------ */
/* CLINT - core local interruptor                                      */
/* ------------------------------------------------------------------ */

/*
 * Register layout matches the de-facto SiFive CLINT that every RISC-V
 * toolchain and RTOS already targets:
 *
 *   +0x0000  msip       software interrupt pending (bit 0)
 *   +0x4000  mtimecmp   64-bit compare value
 *   +0xBFF8  mtime      64-bit free-running counter
 */
#define RV_CLINT_SIZE       0xC000u
#define RV_CLINT_MSIP       0x0000u
#define RV_CLINT_MTIMECMP   0x4000u
#define RV_CLINT_MTIME      0xBFF8u

typedef struct rv_clint {
    /*
     * mtime is written by the platform's timer interrupt and read by the
     * emulator loop, so it is volatile even though there is only one hart.
     */
    volatile uint64_t mtime;
    uint64_t          mtimecmp;
    uint32_t          msip;
    struct rv_hart   *hart;
} rv_clint_t;

extern const rv_dev_ops_t rv_clint_ops;

/* Attach the CLINT to a hart and wire the hart's `time` CSR to mtime. */
void rv_clint_init(rv_clint_t *c, struct rv_hart *hart);

/*
 * Set the current time, then re-evaluate MTIP. Call this from whatever
 * drives time on the host (a hardware timer ISR, SysTick, or the emulator
 * loop itself). mtimecmp comparison is unsigned, per the spec.
 */
void rv_clint_set_time(rv_clint_t *c, uint64_t now);

/* Convenience: advance mtime by `delta` ticks. */
void rv_clint_advance(rv_clint_t *c, uint32_t delta);

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
#define RV_UART_SIZE        0x100u
#define RV_UART_RBR_THR     0x00u
#define RV_UART_IER         0x01u
#define RV_UART_IIR_FCR     0x02u
#define RV_UART_LCR         0x03u
#define RV_UART_MCR         0x04u
#define RV_UART_LSR         0x05u
#define RV_UART_MSR         0x06u
#define RV_UART_SCR         0x07u

#define RV_UART_LSR_DR      0x01u
#define RV_UART_LSR_THRE    0x20u
#define RV_UART_LSR_TEMT    0x40u

typedef struct rv_uart {
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
} rv_uart_t;

extern const rv_dev_ops_t rv_uart_ops;

void rv_uart_init(rv_uart_t *u,
                  void (*tx)(void *ctx, uint8_t c),
                  int (*rx)(void *ctx),
                  void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_DEV_H */
