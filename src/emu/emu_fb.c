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
