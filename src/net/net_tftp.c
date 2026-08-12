/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net_tftp.c - two pseudo-files that are the guest image.
 *
 * There is no filesystem here and nothing is stored under a name. The
 * server answers exactly two, which are the two halves a guest image is
 * built as:
 *
 *   rom   .text and .rodata, programmed into the flash arena and
 *         executed from there. It is in flash because it has to be: the
 *         largest architecture tests need ~345 KiB of which ~140 is
 *         read-only, and only the writable remainder fits in the SRAM
 *         the guest gets.
 *
 *   ram   .data, copied straight into guest RAM.
 *
 * Uploading either suspends the emulator, lands the image, and restarts
 * it -- which is the whole point, because it turns "one reflash per
 * architecture test" into "one UDP transfer per architecture test".
 *
 * The suspend is free and worth understanding rather than trusting. This
 * runs inside emu_net_poll(), which the run loop calls *between* guest
 * slices, so no guest instruction is executing while a block is being
 * written. Nothing had to be stopped; the structure already guaranteed
 * it. What does have to be explicit is the restart, because the bus
 * regions and the core's reset vector are built from the old image and
 * have to be rebuilt from the new one -- emu_net_image_reload() is what
 * the platform implements for that.
 *
 * Reads are refused. A pseudo-file that could be read back would be a
 * pleasant way to verify an upload, and it is not worth the code: the
 * guest itself is the verification, and a corrupted image fails its own
 * self-test far more informatively than a byte comparison would.
 */

#include "emu_net.h"

#include "lwip/apps/tftp_common.h"
#include "lwip/apps/tftp_server.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

#include <string.h>

/*
 * How long a session may sit with no packet before it is torn down.
 *
 * lwIP has its own timeout for this and it does not fire here -- see
 * emu_net_tftp_poll(). Eight seconds is longer than any gap a working
 * client leaves between blocks on this link and short enough that a
 * suite runner's own retry outlives it.
 */
#define EMU_TFTP_IDLE_MS 8000u

/*
 * One transfer at a time. TFTP has no notion of concurrent access to the
 * same file and this server has exactly one client by construction --
 * the other end of a point-to-point serial line.
 */
static emu_net_image_t g_target;
static uint32_t        g_written;
static bool            g_busy;
static bool            g_failed;

/* When this session last saw a packet, for the watchdog below. */
static uint32_t        g_last_ms;

/* Whether tftp_init_server() has succeeded; tftp_cleanup() asserts on it. */
static bool            g_up;

/* How many times the watchdog below has rebuilt the server. */
static uint32_t        g_reclaims;

/* A distinct non-NULL handle per file, so close() knows which ended. */
static uint8_t g_handle_rom;
static uint8_t g_handle_ram;

static void *tftp_open(const char *fname, const char *mode, u8_t write)
{
    LWIP_UNUSED_ARG(mode);

    if (!write) {
        /* Read refused -- see the note at the top. */
        return NULL;
    }
    if (g_busy) {
        /*
         * A transfer is already open. This is not a client racing
         * itself: it is the previous transfer never having been closed,
         * which happens when a harness is killed mid-upload. Refusing
         * would leave the board unusable until reset, so the old one is
         * abandoned instead -- nothing was committed, so it leaves no
         * trace.
         */
        g_busy = false;
    }

    emu_net_image_t which;

    if (strcmp(fname, "rom") == 0) {
        which = EMU_NET_IMAGE_ROM;
    } else if (strcmp(fname, "ram") == 0) {
        which = EMU_NET_IMAGE_RAM;
    } else {
        return NULL;
    }

    if (!emu_net_image_begin(which)) {
        return NULL;
    }

    g_target = which;
    g_written = 0u;
    g_busy = true;
    g_failed = false;
    g_last_ms = sys_now();

    return (which == EMU_NET_IMAGE_ROM) ? (void *)&g_handle_rom
                                        : (void *)&g_handle_ram;
}

static int tftp_write(void *handle, struct pbuf *p)
{
    LWIP_UNUSED_ARG(handle);

    if (!g_busy) {
        return -1;
    }
    g_last_ms = sys_now();

    /*
     * A pbuf may be chained, and the pieces have to be handed over in
     * order and contiguously -- flash is programmed sequentially and
     * guest RAM is a straight copy, so neither tolerates a gap.
     */
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (!emu_net_image_data(g_target, q->payload, q->len, g_written)) {
            /*
             * Remembered rather than returned immediately. The usual
             * cause is the flash arena filling up, and the caller's
             * recovery is to erase and retry -- which it can only do
             * once it has been told, and TFTP's way of telling it is the
             * error reply this return produces.
             */
            g_failed = true;
            return -1;
        }
        g_written += q->len;
    }
    return 0;
}

static void tftp_close(void *handle)
{
    LWIP_UNUSED_ARG(handle);

    if (!g_busy) {
        return;
    }
    g_busy = false;

    /*
     * close() is called on the error path too, and committing there
     * would advance the flash arena past a half-written image and then
     * try to run it. A failed upload has to leave the board exactly as
     * it was.
     */
    emu_net_image_end(g_target, g_written, !g_failed);
}

static void tftp_error(void *handle, int err, const char *msg, int size)
{
    LWIP_UNUSED_ARG(handle);
    LWIP_UNUSED_ARG(err);
    LWIP_UNUSED_ARG(msg);
    LWIP_UNUSED_ARG(size);

    /*
     * An error from the client. lwIP does not call close() after this,
     * so the transfer has to be abandoned here or the next open() would
     * find the server still busy.
     */
    if (g_busy) {
        g_busy = false;
        emu_net_image_end(g_target, g_written, false);
    }
}

static const struct tftp_context k_ctx = {
    tftp_open,
    tftp_close,
    NULL,               /* read: refused at open, never reached */
    tftp_write,
    tftp_error,
};

/*
 * Reclaim a TFTP session whose client stopped talking.
 *
 * lwIP has exactly this timeout already and it does not fire here. Its
 * timer is armed with sys_timeout() from the request handler, and
 * sys_timeout() fails silently when MEMP_NUM_SYS_TIMEOUT is exhausted --
 * so the one path that would ever close an abandoned session is also the
 * path that stops working once a slot leaks. Measured on the board: a
 * client killed after one data block left the session refusing every
 * later request past 42 seconds, against a 10-second timeout, and only a
 * reset cleared it.
 *
 * The trigger is idleness alone, and deliberately *not* this file's own
 * g_busy. The wedge has more than one shape -- lwIP refuses a request
 * from tftp_recv() before ctx->open() is ever reached, so whether the
 * server is stuck is not something these callbacks can observe, and a
 * watchdog gated on g_busy sat silent through a board that was refusing
 * every upload. What can be observed is that nothing has arrived for a
 * while, and that is enough, because tearing an *idle* server down and
 * rebuilding it costs a udp_remove and a udp_new and changes nothing.
 * So it is done on idleness whether or not anything looks wrong.
 *
 * That is blunt on purpose: it depends on none of lwIP's private state,
 * on which of close_handle()'s callers ran, or on the timer pool having
 * had a slot. It also frees the leaked slot, via close_handle()'s
 * sys_untimeout().
 *
 * Once per idle period, not once per poll -- g_quiet says the reclaim
 * for this silence has already happened, and any real traffic clears it.
 *
 * Nothing is lost. A transfer idle this long has a client that has gone
 * away, and no upload commits until its second half lands, so abandoning
 * one leaves the board exactly as it was -- which is what
 * emu_net_image_end() is careful about.
 *
 * Called from emu_net_poll(), so it needs no timer of its own.
 */
void emu_net_tftp_poll(void)
{
    if (!g_up) {
        return;                 /* server never started */
    }
    if ((sys_now() - g_last_ms) < EMU_TFTP_IDLE_MS) {
        return;
    }

    /*
     * Re-arm rather than latch. An earlier version fired once per silence
     * and stayed quiet until real traffic cleared the flag -- which meant
     * the single firing happened seconds after boot, before anything had
     * gone wrong, and the wedge that arrived later was never revisited.
     * Repeating is the whole value: whatever state the server reaches,
     * it is at most EMU_TFTP_IDLE_MS from being rebuilt.
     */
    g_last_ms = sys_now();
    g_reclaims++;

    if (g_busy) {
        for (const char *m = "\nemu: tftp session abandoned; reclaiming\n";
             *m != '\0'; m++) {
            emu_net_console_putc((uint8_t)*m);
        }
    }

    /*
     * Order matters. tftp_cleanup() calls close_handle(), which calls
     * tftp_close() above, and that needs g_busy still set to abandon the
     * part-written image properly -- so clear nothing before it.
     */
    tftp_cleanup();
    g_busy = false;
    (void)tftp_init_server(&k_ctx);
}

uint32_t emu_net_tftp_reclaims(void)
{
    return g_reclaims;
}

bool emu_net_tftp_init(void)
{
    g_up = (tftp_init_server(&k_ctx) == ERR_OK);
    g_last_ms = sys_now();
    return g_up;
}
