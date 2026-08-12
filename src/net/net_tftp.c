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

#include "lwip/apps/tftp_server.h"
#include "lwip/pbuf.h"

#include <string.h>

/*
 * One transfer at a time. TFTP has no notion of concurrent access to the
 * same file and this server has exactly one client by construction --
 * the other end of a point-to-point serial line.
 */
static emu_net_image_t g_target;
static uint32_t        g_written;
static bool            g_busy;
static bool            g_failed;

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

    return (which == EMU_NET_IMAGE_ROM) ? (void *)&g_handle_rom
                                        : (void *)&g_handle_ram;
}

static int tftp_write(void *handle, struct pbuf *p)
{
    LWIP_UNUSED_ARG(handle);

    if (!g_busy) {
        return -1;
    }

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

bool emu_net_tftp_init(void)
{
    return tftp_init_server(&k_ctx) == ERR_OK;
}
