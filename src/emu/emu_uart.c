/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_uart.c - NS16550-subset console UART.
 *
 * The guest sees a familiar 8250/16550 register block; the bytes are
 * handed to whatever transport the platform supplied (USART2 over the
 * ST-LINK virtual COM port on the Nucleo, stdout on the host build).
 *
 * Transmission is synchronous, so THR is always reported empty. That is a
 * legal 16550 behaviour (an infinitely fast transmitter) and it keeps
 * guests that poll LSR from spinning.
 *
 * It also makes the transmit interrupt need care, which is the whole of
 * the interrupt logic below: a condition that is permanently true would
 * re-assert the moment it was acknowledged. See emu_uart_t::thre_pending.
 */

#include "emu/emu_dev.h"

#include <stdbool.h>

/* Fetch a byte into the lookahead slot if it is empty. */
static int uart_peek(emu_uart_t *u)
{
    if (u->pending < 0 && u->rx != NULL) {
        u->pending = u->rx(u->ctx);
    }
    return u->pending;
}

void emu_uart_init(emu_uart_t *u, void (*tx)(void *ctx, uint8_t c),
                   int (*rx)(void *ctx), void *ctx)
{
    u->tx = tx;
    u->rx = rx;
    u->ctx = ctx;
    u->pending = -1;
    u->ier = 0u;
    u->lcr = 0u;
    u->mcr = 0u;
    u->scr = 0u;
    u->irq = NULL;
    u->irq_ctx = NULL;
    u->thre_pending = false;
    u->line = false;
}

void emu_uart_set_irq(emu_uart_t *u, void (*irq)(void *ctx, int level),
                      void *irq_ctx)
{
    u->irq = irq;
    u->irq_ctx = irq_ctx;
}

/*
 * What IIR would report, without the side effect of reading it.
 *
 * Receive wins over transmit, which is the 16550's own priority order
 * and matters here: a driver that services the lower-priority cause
 * first can leave the higher one asserted for ever.
 */
static uint32_t uart_cause(emu_uart_t *u)
{
    if ((u->ier & EMU_UART_IER_RDA) != 0u && uart_peek(u) >= 0) {
        return EMU_UART_IIR_RDA;
    }
    if ((u->ier & EMU_UART_IER_THRE) != 0u && u->thre_pending) {
        return EMU_UART_IIR_THRE;
    }
    return EMU_UART_IIR_NONE;
}

/*
 * Drive the line to match the state, and only on a change.
 *
 * The edge test is not an optimisation: the platform's handler reaches
 * an interrupt controller, and re-asserting a level that is already
 * asserted is at best wasted work and at worst a second entry into a
 * handler that has not finished the first.
 */
static void uart_update_irq(emu_uart_t *u)
{
    const bool want = (uart_cause(u) != EMU_UART_IIR_NONE);

    if (u->irq == NULL || want == u->line) {
        return;
    }
    u->line = want;
    u->irq(u->irq_ctx, want ? 1 : 0);
}

void emu_uart_poll(emu_uart_t *u)
{
    if (u->irq != NULL) {
        uart_update_irq(u);
    }
}

static emu_fault_t uart_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    emu_uart_t *u = (emu_uart_t *)ctx;

    /* Real 16550 registers are byte-wide; wider reads are a guest bug. */
    if (size != 1u && size != 4u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case EMU_UART_RBR_THR: {
        const int c = uart_peek(u);
        u->pending = -1; /* consume */
        *out = (c < 0) ? 0u : (uint32_t)c;
        uart_update_irq(u);
        break;
    }

    case EMU_UART_IER:
        *out = u->ier;
        break;
    case EMU_UART_IIR_FCR: {
        const uint32_t cause = uart_cause(u);

        *out = cause;
        /*
         * Reading IIR acknowledges a transmit interrupt, and only a
         * transmit one. Receive stays asserted until the byte is read
         * from RBR, which is what stops a driver from acknowledging
         * input it has not taken.
         */
        if (cause == EMU_UART_IIR_THRE) {
            u->thre_pending = false;
            uart_update_irq(u);
        }
        break;
    }
    case EMU_UART_LCR:
        *out = u->lcr;
        break;
    case EMU_UART_MCR:
        *out = u->mcr;
        break;

    case EMU_UART_LSR: {
        /* TX is synchronous, so THR is permanently empty. */
        uint32_t lsr = EMU_UART_LSR_THRE | EMU_UART_LSR_TEMT;
        if (uart_peek(u) >= 0) {
            lsr |= EMU_UART_LSR_DR;
        }
        *out = lsr;
        break;
    }

    case EMU_UART_MSR:
        *out = 0u;
        break;
    case EMU_UART_SCR:
        *out = u->scr;
        break;
    default:
        *out = 0u;
        break;
    }

    return EMU_FAULT_NONE;
}

static emu_fault_t uart_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    emu_uart_t *u = (emu_uart_t *)ctx;

    if (size != 1u && size != 4u) {
        return EMU_FAULT_STORE;
    }

    switch (off) {
    case EMU_UART_RBR_THR:
        if (u->tx != NULL) {
            u->tx(u->ctx, (uint8_t)val);
        }
        /*
         * The byte is already gone, so the holding register is empty
         * again the instant it was written -- which is exactly when a
         * 16550 raises THRE. Without this the driver sends one byte per
         * interrupt-enable rather than draining its buffer.
         */
        u->thre_pending = true;
        uart_update_irq(u);
        break;

    case EMU_UART_IER:
        u->ier = (uint8_t)val;
        /*
         * Enabling the transmit interrupt on a UART whose holding
         * register is already empty must raise it immediately. This is
         * how the tty layer starts a transmission: it fills its buffer,
         * sets ETBEI, and waits to be told there is room -- which there
         * always is here.
         */
        if ((u->ier & EMU_UART_IER_THRE) != 0u) {
            u->thre_pending = true;
        }
        uart_update_irq(u);
        break;
    case EMU_UART_LCR:
        u->lcr = (uint8_t)val;
        break;
    case EMU_UART_MCR:
        u->mcr = (uint8_t)val;
        break;
    case EMU_UART_SCR:
        u->scr = (uint8_t)val;
        break;
    default:
        break; /* FCR and the read-only registers */
    }

    return EMU_FAULT_NONE;
}

const emu_dev_ops_t emu_uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .tick = NULL,
};
