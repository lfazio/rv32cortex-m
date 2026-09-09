/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_fb.c - the framebuffer's control block.
 *
 * The point of this file is that it needs no display. The device is
 * portable C with the presenter supplied by the platform, so a test can
 * supply its own and inspect exactly what a real one would be handed --
 * which is the arrangement that lets `src/emu/` stay free of a windowing
 * system it cannot have on a board.
 *
 * The inputs here are chosen for the ones that discriminate rather than
 * representative ones: a geometry whose byte count overflows 32 bits, a
 * stride narrower than the width, and a present callback that is NULL.
 * Every plausible mode behaves identically under a correct and an
 * incorrect implementation of all three.
 */

#include "emu/emu_dev.h"
#include "tests.h"

#include <string.h>

static uint8_t fb_buf[64u * 32u];

/* What the presenter last saw, so the test can check it was told the
 * truth rather than merely called. */
static struct {
    unsigned calls;
    const void *pixels;
    uint32_t width, height, stride, format;
    uint32_t pal0, pal255;
} g_seen;

static void record_present(void *ctx, const emu_fb_frame_t *f)
{
    *(int *)ctx += 1;
    g_seen.calls++;
    g_seen.pixels = f->pixels;
    g_seen.width = f->width;
    g_seen.height = f->height;
    g_seen.stride = f->stride;
    g_seen.format = f->format;
    g_seen.pal0 = (f->palette != NULL) ? f->palette[0] : 0xFFFFFFFFu;
    g_seen.pal255 = (f->palette != NULL) ? f->palette[255] : 0xFFFFFFFFu;
}

void test_fb(void)
{
    emu_fb_t fb;
    uint32_t v;
    int calls = 0;

    memset(&g_seen, 0, sizeof(g_seen));

    /* --- what emu_fb_bpp must answer, including for a non-format --- */
    CHECK_EQ(emu_fb_bpp(EMU_FB_FMT_IDX8), 1u);
    CHECK_EQ(emu_fb_bpp(EMU_FB_FMT_XRGB8888), 4u);
    CHECK_EQ(emu_fb_bpp((emu_fb_format_t)99), 0u);

    /* --- geometry that does not fit is refused ------------------- */

    /* One row too many for the buffer. */
    CHECK(!emu_fb_init(&fb, 64u, 33u, EMU_FB_FMT_IDX8, 0u, fb_buf, 0x40000000u,
                       sizeof(fb_buf), NULL, NULL));

    /*
     * **The awkward one: stride * height overflows 32 bits.** 0x40000000
     * by 4 is exactly 2^32, which in 32-bit arithmetic is zero -- so a
     * check done in the guest's own width would compute "0 bytes needed"
     * and accept it, and the first present would read a gigabyte from a
     * 2 KB buffer. Every sane geometry passes either way; only this one
     * tells the two implementations apart.
     */
    CHECK(!emu_fb_init(&fb, 4u, 4u, EMU_FB_FMT_XRGB8888, 0x40000000u, fb_buf,
                       0x40000000u, sizeof(fb_buf), NULL, NULL));

    /* A stride narrower than the row would make rows overlap. */
    CHECK(!emu_fb_init(&fb, 16u, 4u, EMU_FB_FMT_XRGB8888, 32u, fb_buf,
                       0x40000000u, sizeof(fb_buf), NULL, NULL));
    /* Wider is legal: that is how a guest gets row alignment. */
    CHECK(emu_fb_init(&fb, 8u, 4u, EMU_FB_FMT_XRGB8888, 64u, fb_buf,
                      0x40000000u, sizeof(fb_buf), NULL, NULL));
    CHECK_EQ(fb.stride, 64u);

    /* Zero anything, or no buffer, is not a mode. */
    CHECK(!emu_fb_init(&fb, 0u, 4u, EMU_FB_FMT_IDX8, 0u, fb_buf, 0u,
                       sizeof(fb_buf), NULL, NULL));
    CHECK(!emu_fb_init(&fb, 4u, 0u, EMU_FB_FMT_IDX8, 0u, fb_buf, 0u,
                       sizeof(fb_buf), NULL, NULL));
    CHECK(!emu_fb_init(&fb, 4u, 4u, EMU_FB_FMT_IDX8, 0u, NULL, 0u, 64u, NULL,
                       NULL));

    /* --- a real mode, with a presenter ---------------------------- */

    CHECK(emu_fb_init(&fb, 64u, 32u, EMU_FB_FMT_IDX8, 0u, fb_buf, 0x40000000u,
                      sizeof(fb_buf), record_present, &calls));
    CHECK_EQ(fb.stride, 64u); /* the natural one for IDX8 */

    /* --- the registers a guest reads at start-up ------------------ */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_ID, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_FB_ID_MAGIC);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_VERSION, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_FB_VERSION_1);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 64u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_HEIGHT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 32u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_FORMAT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, (uint32_t)EMU_FB_FMT_IDX8);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_BASE, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x40000000u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_BYTES, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, (uint32_t)sizeof(fb_buf));

    /* An unassigned register reads zero rather than faulting, so a
     * guest can probe for a later version's feature. */
    CHECK_EQ(emu_fb_ops.read(&fb, 0x100u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* --- word accesses only, and the fault names the direction ---- */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 1u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH + 1u, 4u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_FLUSH, 2u, 0u), EMU_FAULT_STORE);
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_FLUSH + 2u, 4u, 0u),
             EMU_FAULT_STORE);

    /* --- the palette round-trips, and drops the top byte ---------- */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_PALETTE, 4u, 0xFF123456u),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_PALETTE, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x00123456u); /* the alpha byte is not colour */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_PALETTE + 255u * 4u, 4u, 0x00ABCDEFu),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_PALETTE + 255u * 4u, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0x00ABCDEFu);

    /* The entry past the last one is not palette; it must not write
     * off the end of the array. */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_PALETTE + 256u * 4u, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* --- FLUSH presents, and describes the buffer truthfully ------ */
    CHECK_EQ(fb.frames, 0u);
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_FLUSH, 4u, 1u), EMU_FAULT_NONE);
    CHECK_EQ(calls, 1);
    CHECK_EQ(g_seen.calls, 1u);
    CHECK(g_seen.pixels == fb_buf);
    CHECK_EQ(g_seen.width, 64u);
    CHECK_EQ(g_seen.height, 32u);
    CHECK_EQ(g_seen.stride, 64u);
    CHECK_EQ(g_seen.format, (uint32_t)EMU_FB_FMT_IDX8);
    /* The presenter sees the palette the guest wrote, not the default. */
    CHECK_EQ(g_seen.pal0, 0x00123456u);
    CHECK_EQ(g_seen.pal255, 0x00ABCDEFu);

    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_FRAMES, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 1u);

    /* --- geometry is read-only, and writing it is ignored --------- */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_WIDTH, 4u, 1234u), EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 64u);

    /*
     * --- headless: no presenter, and the guest cannot tell ---------
     *
     * This is the case the board is in, and the one that would make a
     * game hang if FLUSH waited for something. The frame count must
     * still advance, because that is how a guest measures its own frame
     * rate and a headless run reporting zero would read as "it drew
     * nothing".
     */
    CHECK(emu_fb_init(&fb, 64u, 32u, EMU_FB_FMT_IDX8, 0u, fb_buf, 0u,
                      sizeof(fb_buf), NULL, NULL));
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_FLUSH, 4u, 0u), EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_FLUSH, 4u, 0u), EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_FRAMES, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 2u);
    /* And the earlier presenter was not called again. */
    CHECK_EQ(calls, 1);

    /* The default palette is a ramp rather than zeros, so an indexed
     * guest that writes none still produces a picture. */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_PALETTE + 4u, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0x00010101u);
}
