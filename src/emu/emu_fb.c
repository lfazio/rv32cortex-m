/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_fb.c - the framebuffer's control block.
 *
 * The pixels are not here. They are an ordinary RAM region the platform
 * registers on the bus, and the guest stores into them at memory speed;
 * this file is only the handful of registers that say how large that
 * region is and when to show it. See the note in emu_dev.h for why that
 * split is the whole design.
 *
 * Nothing in this file knows what a window is. `present` is the
 * platform's, exactly as the console UART's `tx` is, and a platform with
 * no display supplies NULL rather than a stub -- which is also what a
 * unit test does when it only wants the register semantics.
 */

#include "emu/emu_dev.h"

uint32_t emu_fb_bpp(emu_fb_format_t format)
{
    switch (format) {
    case EMU_FB_FMT_IDX8:
        return 1u;
    case EMU_FB_FMT_XRGB8888:
        return 4u;
    default:
        return 0u;
    }
}

/*
 * The modes on offer, in the order a guest enumerates them.
 *
 * VBE's numbers, so a porting layer that already knows `0x101 is
 * 640x480x8` keeps that knowledge -- see the note in emu_dev.h for why
 * this is VBE's numbering and not a VBE BIOS, which a guest with no real
 * mode could not call.
 *
 * Smallest first, because a guest walking the list for "the first mode
 * that fits" wants the cheapest, and because 320x200 is Doom's and is
 * the one most likely to be asked for by number.
 */
static const emu_fb_mode_t k_modes[] = {
    {EMU_FB_MODE_320X200X8, 320u, 200u, EMU_FB_FMT_IDX8},
    {EMU_FB_MODE_640X400X8, 640u, 400u, EMU_FB_FMT_IDX8},
    {EMU_FB_MODE_640X480X8, 640u, 480u, EMU_FB_FMT_IDX8},
    {EMU_FB_MODE_800X600X8, 800u, 600u, EMU_FB_FMT_IDX8},
    {EMU_FB_MODE_1024X768X8, 1024u, 768u, EMU_FB_FMT_IDX8},
    {EMU_FB_MODE_640X480X32, 640u, 480u, EMU_FB_FMT_XRGB8888},
    {EMU_FB_MODE_800X600X32, 800u, 600u, EMU_FB_FMT_XRGB8888},
    {EMU_FB_MODE_1024X768X32, 1024u, 768u, EMU_FB_FMT_XRGB8888},
};

#define EMU_FB_MODE_TOTAL (sizeof(k_modes) / sizeof(k_modes[0]))

bool emu_fb_has_mode(uint32_t width, uint32_t height, emu_fb_format_t format)
{
    for (uint32_t i = 0; i < EMU_FB_MODE_TOTAL; i++) {
        if ((uint32_t)k_modes[i].width == width &&
            (uint32_t)k_modes[i].height == height &&
            (emu_fb_format_t)k_modes[i].format == format) {
            return true;
        }
    }
    return false;
}

uint32_t emu_fb_max_bytes(void)
{
    uint32_t most = 0u;

    for (uint32_t i = 0; i < EMU_FB_MODE_TOTAL; i++) {
        const uint32_t need = (uint32_t)k_modes[i].width *
                              (uint32_t)k_modes[i].height *
                              emu_fb_bpp((emu_fb_format_t)k_modes[i].format);

        if (need > most) {
            most = need;
        }
    }
    return most;
}

/*
 * Take a mode by its VBE number. False if there is no such mode, or if
 * the platform's buffer cannot hold it.
 *
 * **The stride is recomputed, not kept.** A platform may have asked for
 * a wider one than its width needed -- that is how a guest gets row
 * alignment -- but that number described the *old* geometry, and
 * carrying it into a mode 640 pixels wider is how a picture comes out
 * sheared. Every mode here is naturally strided.
 */
static bool fb_set_mode(emu_fb_t *fb, uint32_t number)
{
    for (uint32_t i = 0; i < EMU_FB_MODE_TOTAL; i++) {
        if (k_modes[i].number != number) {
            continue;
        }

        const uint32_t bpp = emu_fb_bpp((emu_fb_format_t)k_modes[i].format);
        const uint32_t stride = (uint32_t)k_modes[i].width * bpp;

        if ((uint64_t)stride * (uint64_t)k_modes[i].height >
            (uint64_t)fb->bytes) {
            return false;
        }

        fb->width = k_modes[i].width;
        fb->height = k_modes[i].height;
        fb->format = k_modes[i].format;
        fb->stride = stride;
        fb->mode_number = number;
        return true;
    }
    return false;
}

bool emu_fb_init(emu_fb_t *fb, uint32_t width, uint32_t height,
                 emu_fb_format_t format, uint32_t stride, void *pixels,
                 uint32_t base, uint32_t bytes,
                 void (*present)(void *ctx, const emu_fb_frame_t *frame),
                 void *ctx)
{
    const uint32_t bpp = emu_fb_bpp(format);

    if (bpp == 0u || width == 0u || height == 0u || pixels == NULL) {
        return false;
    }

    /*
     * A stride of zero means the natural one. A caller's own is allowed
     * to be *wider* -- that is how a guest gets row alignment -- but not
     * narrower, because then rows overlap and the geometry no longer
     * describes the buffer.
     */
    if (stride == 0u) {
        stride = width * bpp;
    } else if (stride < width * bpp) {
        return false;
    }

    /*
     * The frame has to fit. Checked here rather than trusted, because
     * the alternative is a read off the end of the buffer on the first
     * present -- in the platform, one call away from anything that could
     * explain it.
     *
     * In 64-bit so the multiply cannot wrap: a guest-supplied height
     * times a plausible stride overflows 32 bits long before it stops
     * looking like a reasonable mode.
     */
    if ((uint64_t)stride * (uint64_t)height > (uint64_t)bytes) {
        return false;
    }

    fb->present = present;
    fb->ctx = ctx;
    fb->pixels = pixels;
    fb->base = base;
    fb->bytes = bytes;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->format = (uint32_t)format;
    fb->frames = 0u;
    fb->mode_index = 0u;
    fb->mode_ok = 1u;

    /*
     * The VBE number for the geometry the platform asked for, if it is
     * one of the offered modes; zero if it is not. A platform is allowed
     * a mode outside the table -- the device's geometry is whatever it
     * was given -- and a guest reading MODE_SET then gets 0, which is
     * not a VBE mode and is the honest answer.
     */
    fb->mode_number = 0u;
    for (uint32_t i = 0; i < EMU_FB_MODE_TOTAL; i++) {
        if (k_modes[i].width == width && k_modes[i].height == height &&
            k_modes[i].format == (uint8_t)format) {
            fb->mode_number = k_modes[i].number;
            break;
        }
    }

    /*
     * A greyscale ramp, so an indexed guest that never writes a palette
     * still produces a picture rather than a black screen. It is not a
     * plausible default for any real content -- Doom loads its own from
     * the WAD -- but "nothing appeared" and "the palette is all zeros"
     * are otherwise the same symptom.
     */
    for (uint32_t i = 0; i < EMU_FB_PALETTE_ENTRIES; i++) {
        fb->palette[i] = (i << 16) | (i << 8) | i;
    }
    return true;
}

static emu_fault_t fb_read(void *ctx, uint32_t off, uint32_t size,
                           uint32_t *out)
{
    const emu_fb_t *fb = (const emu_fb_t *)ctx;

    /*
     * Word accesses only, and aligned. Every register here is a 32-bit
     * value read once at start-up; accepting byte reads would mean
     * deciding what half of FB_BASE means, which no guest needs and
     * which would be one more thing to get subtly wrong.
     *
     * The refusal is EMU_FAULT_LOAD because the bus reports the
     * *direction* that was refused and lets the frontend name the fault
     * -- there is no "bad size" code, and inventing one here would put
     * an architecture's vocabulary in a file that must not have one.
     */
    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_LOAD;
    }

    if (off >= EMU_FB_PALETTE &&
        off < EMU_FB_PALETTE + EMU_FB_PALETTE_ENTRIES * 4u) {
        *out = fb->palette[(off - EMU_FB_PALETTE) >> 2];
        return EMU_FAULT_NONE;
    }

    switch (off) {
    case EMU_FB_ID:
        *out = EMU_FB_ID_MAGIC;
        break;
    case EMU_FB_VERSION:
        *out = EMU_FB_VERSION_1;
        break;
    case EMU_FB_WIDTH:
        *out = fb->width;
        break;
    case EMU_FB_HEIGHT:
        *out = fb->height;
        break;
    case EMU_FB_FORMAT:
        *out = fb->format;
        break;
    case EMU_FB_STRIDE:
        *out = fb->stride;
        break;
    case EMU_FB_BASE:
        *out = fb->base;
        break;
    case EMU_FB_BYTES:
        *out = fb->bytes;
        break;
    case EMU_FB_FRAMES:
        *out = (uint32_t)fb->frames;
        break;
    case EMU_FB_MODE_COUNT:
        *out = (uint32_t)EMU_FB_MODE_TOTAL;
        break;
    case EMU_FB_MODE_NUMBER:
        *out = k_modes[fb->mode_index].number;
        break;
    case EMU_FB_MODE_WIDTH:
        *out = k_modes[fb->mode_index].width;
        break;
    case EMU_FB_MODE_HEIGHT:
        *out = k_modes[fb->mode_index].height;
        break;
    case EMU_FB_MODE_FORMAT:
        *out = k_modes[fb->mode_index].format;
        break;
    case EMU_FB_MODE_INDEX:
        *out = fb->mode_index;
        break;
    case EMU_FB_MODE_SET:
        *out = fb->mode_number;
        break;
    case EMU_FB_MODE_OK:
        *out = fb->mode_ok;
        break;
    default:
        /*
         * Reads of unassigned registers are zero rather than a fault.
         * A guest probing for a feature this version does not have gets
         * an answer it can test, which is what the ID and VERSION
         * registers are for.
         */
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t fb_write(void *ctx, uint32_t off, uint32_t size,
                            uint32_t val)
{
    emu_fb_t *fb = (emu_fb_t *)ctx;

    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_STORE;
    }

    if (off >= EMU_FB_PALETTE &&
        off < EMU_FB_PALETTE + EMU_FB_PALETTE_ENTRIES * 4u) {
        fb->palette[(off - EMU_FB_PALETTE) >> 2] = val & 0x00FFFFFFu;
        return EMU_FAULT_NONE;
    }

    if (off == EMU_FB_FLUSH) {
        /*
         * The count moves whether or not anything is displayed. It is
         * the guest's own measure of frames drawn -- a frame rate is
         * read from here -- and making it depend on the platform having
         * a window would mean a headless run reporting that it rendered
         * nothing.
         */
        fb->frames++;

        if (fb->present != NULL) {
            const emu_fb_frame_t frame = {
                .pixels = fb->pixels,
                .width = fb->width,
                .height = fb->height,
                .stride = fb->stride,
                .format = fb->format,
                .palette = fb->palette,
            };

            fb->present(fb->ctx, &frame);
        }
        return EMU_FAULT_NONE;
    }

    if (off == EMU_FB_MODE_INDEX) {
        /*
         * Clamped rather than refused. A guest walking the table until
         * it runs out is a reasonable way to enumerate, and the
         * alternative -- faulting on the entry past the end -- turns a
         * loop bound that is one too many into a crash.
         */
        fb->mode_index = (val < (uint32_t)EMU_FB_MODE_TOTAL)
                             ? val
                             : (uint32_t)(EMU_FB_MODE_TOTAL - 1u);
        return EMU_FAULT_NONE;
    }

    if (off == EMU_FB_MODE_SET) {
        /*
         * A refused mode leaves the current one untouched and says so in
         * MODE_OK. Not a fault: a guest asking for a mode the device
         * does not have has done nothing illegal, and it needs to be
         * able to try the next one on its list.
         */
        fb->mode_ok = fb_set_mode(fb, val) ? 1u : 0u;
        return EMU_FAULT_NONE;
    }

    /*
     * Everything else is read-only geometry. Ignored rather than
     * faulted: a guest that writes it has made a mistake this device
     * cannot fix, and killing it teaches less than letting it read the
     * value back unchanged.
     */
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t emu_fb_ops = {
    .read = fb_read,
    .write = fb_write,
    .tick = NULL,
};
