/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_uart.c - the console UART's interrupt, which nothing polled ever
 * needed and an operating system cannot work without.
 *
 * **The failure this covers is silence, not a wrong value.** Linux
 * writes kernel messages through the 8250 driver's *polled* console
 * path and everything userspace writes through the tty layer, which
 * fills a buffer and waits to be told the transmitter is free. With no
 * interrupt the kernel's own output is perfect and every byte a process
 * writes is queued for ever -- write() returns success and userspace is
 * simply mute. Nothing in the emulator is wrong, no access faults, and
 * the symptom looks like the program not running.
 *
 * The thing that makes this fiddly enough to be worth a test is that
 * **transmission here is instantaneous**, so THRE is permanently true.
 * An interrupt conditioned on it alone re-asserts the moment it is
 * acknowledged, which the guest cannot escape except by disabling the
 * interrupt -- so the acknowledgement has to be modelled: reading IIR
 * clears the transmit indication, and writing THR sets it again. Both
 * directions are checked below, because getting either one wrong gives
 * a UART that works for one byte.
 */

#include "tests.h"

#include "emu/emu_dev.h"

#include <string.h>

static emu_uart_t g_u;

/* What the platform would do: a line with a level, and a count. */
static int g_level;
static int g_edges;

static void line(void *ctx, int level)
{
    (void)ctx;
    if (level != g_level) {
        g_edges++;
    }
    g_level = level;
}

/* A transport that emits nowhere and can be fed input. */
static int g_rx_next = -1;

static void tx_sink(void *ctx, uint8_t c)
{
    (void)ctx;
    (void)c;
}

static int rx_src(void *ctx)
{
    const int c = g_rx_next;

    (void)ctx;
    g_rx_next = -1;
    return c;
}

static uint32_t rd(uint32_t off)
{
    uint32_t v = 0;

    CHECK_EQ(emu_uart_ops.read(&g_u, off, 1u, &v), EMU_FAULT_NONE);
    return v;
}

static void wr(uint32_t off, uint32_t v)
{
    CHECK_EQ(emu_uart_ops.write(&g_u, off, 1u, v), EMU_FAULT_NONE);
}

static void reset(void)
{
    emu_uart_init(&g_u, tx_sink, rx_src, NULL);
    emu_uart_set_irq(&g_u, line, NULL);
    g_level = 0;
    g_edges = 0;
    g_rx_next = -1;
}

void test_uart(void)
{
    /*
     * A UART nobody armed must not interrupt.
     *
     * Asserted rather than assumed, because every bare-metal guest in
     * this tree polls LSR and leaves IER at zero. A device that raised
     * its line at reset would leave a level-triggered input stuck high
     * before any guest had touched it.
     */
    reset();
    wr(EMU_UART_RBR_THR, 'x');
    g_rx_next = 'q';
    (void)rd(EMU_UART_LSR);
    CHECK_EQ((uint32_t)g_level, 0u);
    CHECK_EQ((uint32_t)g_edges, 0u);

    /*
     * Enabling the transmit interrupt raises it immediately, with no
     * write to THR first.
     *
     * **This is how the tty layer starts a transmission**: it fills its
     * buffer, sets ETBEI and waits to be told there is room -- which
     * there always is here. A device that waited for a THR write to
     * raise the first interrupt would never send anything at all,
     * because the driver is waiting for the interrupt before it writes.
     */
    reset();
    wr(EMU_UART_IER, EMU_UART_IER_THRE);
    CHECK_EQ((uint32_t)g_level, 1u);
    CHECK_EQ(rd(EMU_UART_IIR_FCR), EMU_UART_IIR_THRE);

    /*
     * ...and reading IIR acknowledged it. Without this the line stays
     * up on a permanently-empty transmitter and the guest re-enters its
     * handler for ever.
     */
    CHECK_EQ((uint32_t)g_level, 0u);
    CHECK_EQ(rd(EMU_UART_IIR_FCR), EMU_UART_IIR_NONE);

    /*
     * Writing THR re-arms it. This is the half that makes a *second*
     * byte possible: the driver writes what it can, returns, and is
     * called again when there is room.
     */
    wr(EMU_UART_RBR_THR, 'a');
    CHECK_EQ((uint32_t)g_level, 1u);
    CHECK_EQ(rd(EMU_UART_IIR_FCR), EMU_UART_IIR_THRE);
    CHECK_EQ((uint32_t)g_level, 0u);

    /*
     * Disabling it drops the line even though THRE is still true --
     * which is how a driver with nothing left to send stops the
     * interrupt, and the only way it can.
     */
    wr(EMU_UART_RBR_THR, 'b');
    CHECK_EQ((uint32_t)g_level, 1u);
    wr(EMU_UART_IER, 0u);
    CHECK_EQ((uint32_t)g_level, 0u);

    /*
     * Receive. Nothing is pending until a byte arrives, and it is
     * emu_uart_poll that notices -- the transport is polled, so without
     * that call from the run loop a guest waits for an interrupt
     * nothing can cause.
     */
    reset();
    wr(EMU_UART_IER, EMU_UART_IER_RDA);
    CHECK_EQ((uint32_t)g_level, 0u);

    g_rx_next = 'k';
    emu_uart_poll(&g_u);
    CHECK_EQ((uint32_t)g_level, 1u);
    CHECK_EQ(rd(EMU_UART_IIR_FCR), EMU_UART_IIR_RDA);

    /*
     * **Reading IIR must not acknowledge a receive interrupt.** Only
     * taking the byte does. A device that cleared it on the IIR read
     * would let a driver acknowledge input it has not collected, and
     * the byte would sit in the lookahead slot for ever.
     */
    CHECK_EQ((uint32_t)g_level, 1u);
    CHECK_EQ(rd(EMU_UART_RBR_THR), (uint32_t)'k');
    CHECK_EQ((uint32_t)g_level, 0u);

    /*
     * Receive outranks transmit, which is the 16550's own priority
     * order. A driver that is told about the lower-priority cause first
     * can service it, return, and leave the higher one asserted.
     */
    reset();
    wr(EMU_UART_IER, EMU_UART_IER_RDA | EMU_UART_IER_THRE);
    g_rx_next = 'z';
    emu_uart_poll(&g_u);
    CHECK_EQ(rd(EMU_UART_IIR_FCR), EMU_UART_IIR_RDA);

    /*
     * The modem lines say "connected and clear to send".
     *
     * Zero here is an unplugged cable: a tty opened without CLOCAL
     * blocks waiting for carrier, and one with hardware flow control
     * never starts a transmission -- both of which present as a console
     * that accepts every byte and emits none.
     */
    reset();
    CHECK_EQ(rd(EMU_UART_MSR) & EMU_UART_MSR_CTS, EMU_UART_MSR_CTS);
    CHECK_EQ(rd(EMU_UART_MSR) & EMU_UART_MSR_DSR, EMU_UART_MSR_DSR);
    CHECK_EQ(rd(EMU_UART_MSR) & EMU_UART_MSR_DCD, EMU_UART_MSR_DCD);
    /* The delta bits mean "changed since last read" and nothing does. */
    CHECK_EQ(rd(EMU_UART_MSR) & 0x0Fu, 0u);

    /*
     * A UART with no line configured must behave exactly as it did
     * before there was one. This is every board in the tree, and the
     * check is that nothing faults rather than that something happens.
     */
    emu_uart_init(&g_u, tx_sink, rx_src, NULL);
    g_level = 0;
    g_edges = 0;
    wr(EMU_UART_IER, EMU_UART_IER_THRE);
    wr(EMU_UART_RBR_THR, 'c');
    (void)rd(EMU_UART_IIR_FCR);
    CHECK_EQ((uint32_t)g_edges, 0u);
}
