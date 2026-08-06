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
 */

#include "emu/emu_dev.h"

/* Fetch a byte into the lookahead slot if it is empty. */
static int uart_peek(emu_uart_t *u)
{
    if (u->pending < 0 && u->rx != NULL) {
        u->pending = u->rx(u->ctx);
    }
    return u->pending;
}

void emu_uart_init(emu_uart_t *u,
                  void (*tx)(void *ctx, uint8_t c),
                  int (*rx)(void *ctx),
                  void *ctx)
{
    u->tx = tx;
    u->rx = rx;
    u->ctx = ctx;
    u->pending = -1;
    u->ier = 0u;
    u->lcr = 0u;
    u->mcr = 0u;
    u->scr = 0u;
}

static emu_fault_t uart_read(void *ctx, uint32_t off, uint32_t size, uint32_t *out)
{
    emu_uart_t *u = (emu_uart_t *)ctx;

    /* Real 16550 registers are byte-wide; wider reads are a guest bug. */
    if (size != 1u && size != 4u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case EMU_UART_RBR_THR: {
        const int c = uart_peek(u);
        u->pending = -1;                     /* consume */
        *out = (c < 0) ? 0u : (uint32_t)c;
        break;
    }

    case EMU_UART_IER: *out = u->ier; break;
    case EMU_UART_IIR_FCR: *out = 0x01u; break;   /* no interrupt pending */
    case EMU_UART_LCR: *out = u->lcr; break;
    case EMU_UART_MCR: *out = u->mcr; break;

    case EMU_UART_LSR: {
        /* TX is synchronous, so THR is permanently empty. */
        uint32_t lsr = EMU_UART_LSR_THRE | EMU_UART_LSR_TEMT;
        if (uart_peek(u) >= 0) {
            lsr |= EMU_UART_LSR_DR;
        }
        *out = lsr;
        break;
    }

    case EMU_UART_MSR: *out = 0u; break;
    case EMU_UART_SCR: *out = u->scr; break;
    default: *out = 0u; break;
    }

    return EMU_FAULT_NONE;
}

static emu_fault_t uart_write(void *ctx, uint32_t off, uint32_t size, uint32_t val)
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
        break;

    case EMU_UART_IER: u->ier = (uint8_t)val; break;
    case EMU_UART_LCR: u->lcr = (uint8_t)val; break;
    case EMU_UART_MCR: u->mcr = (uint8_t)val; break;
    case EMU_UART_SCR: u->scr = (uint8_t)val; break;
    default: break;   /* FCR and the read-only registers */
    }

    return EMU_FAULT_NONE;
}

const emu_dev_ops_t emu_uart_ops = {
    .read  = uart_read,
    .write = uart_write,
    .tick  = NULL,
};
