/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net_sio.c - lwIP's serial interface, onto the board console UART.
 *
 * slipif.c needs three entry points and no more: sio_open to claim the
 * device, sio_send to put a byte, and sio_tryread to take whatever has
 * arrived without waiting. The blocking sio_read and the sio_write bulk
 * form are only reachable from the RX thread and the netconn API, both
 * of which NO_SYS=1 compiles out -- so they are not defined here, and a
 * build that starts needing them fails to link rather than deadlocking
 * in a firmware with no scheduler to switch to.
 *
 * There is one device, and it is the console UART that board.h already
 * abstracts. Nothing in this file knows which USART that is, which is
 * what lets the same stack run on either Nucleo.
 *
 * The receive side is the part that has to be right. At 921600 baud a
 * byte arrives every 10.8 us, and this family's USART has no receive
 * FIFO -- one byte of RDR, then an overrun. The run loop reaches
 * sio_tryread once per guest slice, and a slice is thousands of guest
 * instructions, so polling RDR here would drop most of every frame.
 * board_console_rx_irq_enable() moves reception into an interrupt that
 * fills a ring, and this reads the ring.
 */

#include "emu_net.h"

#include "lwip/opt.h"
#include "lwip/sio.h"

#include "board.h"

/*
 * sio_open returns a handle lwIP only ever compares against NULL and
 * hands back, so any non-NULL value will do. Using the address of a
 * static rather than a cast integer keeps it a real pointer, which is
 * what sio_fd_t is declared as.
 */
static uint8_t g_console_dev;

sio_fd_t sio_open(u8_t devnum)
{
    /*
     * There is exactly one serial device. Refusing the rest rather than
     * returning the same handle for all of them means a second netif
     * added later fails at init with ERR_IF, instead of silently
     * interleaving two SLIP streams on one wire.
     */
    if (devnum != 0u) {
        return NULL;
    }

    board_console_rx_irq_enable();
    return (sio_fd_t)&g_console_dev;
}

void sio_send(u8_t c, sio_fd_t fd)
{
    LWIP_UNUSED_ARG(fd);
    board_console_putc(c);
}

u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
    u32_t n = 0;

    LWIP_UNUSED_ARG(fd);

    while (n < len) {
        const int c = board_console_getc();

        if (c < 0) {
            break;
        }
        data[n++] = (u8_t)c;
    }
    return n;
}
