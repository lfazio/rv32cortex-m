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

    /* --- VBE mode setting ----------------------------------------- */

    /*
     * A buffer big enough for every offered mode, which is what a
     * platform is expected to allocate -- the point of emu_fb_max_bytes
     * is that a device should not enumerate modes it must then refuse.
     */
    static uint8_t big[1024u * 768u * 4u];

    CHECK(emu_fb_max_bytes() == sizeof(big));
    CHECK(emu_fb_init(&fb, 320u, 200u, EMU_FB_FMT_IDX8, 0u, big, 0x30100000u,
                      (uint32_t)sizeof(big), NULL, NULL));

    /* The starting geometry is recognised as a mode, so a guest reading
     * MODE_SET without having written one gets a real number. */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_SET, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_FB_MODE_320X200X8);

    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_COUNT, 4u, &v), EMU_FAULT_NONE);
    CHECK(v >= 8u);

    /* Enumeration: index 0 is the smallest, which is Doom's. */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_MODE_INDEX, 4u, 0u), EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_NUMBER, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_FB_MODE_320X200X8);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 320u);

    /*
     * An index past the end clamps rather than faulting: a guest
     * walking the table until it runs out is a reasonable way to
     * enumerate, and a loop bound one too large should not be a crash.
     */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_MODE_INDEX, 4u, 9999u),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_INDEX, 4u, &v), EMU_FAULT_NONE);
    CHECK(v < 64u); /* clamped to the last entry, not stored raw */

    /* Setting a real mode takes, and the geometry follows it. */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_MODE_SET, 4u, EMU_FB_MODE_640X480X32),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_OK, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 1u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 640u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_HEIGHT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 480u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_FORMAT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, (uint32_t)EMU_FB_FMT_XRGB8888);
    /*
     * **The stride follows the new width and is not carried over.** It
     * was 320 for the previous mode; keeping it would describe rows a
     * fifth of their real length, which is exactly how a picture comes
     * out sheared rather than absent.
     */
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_STRIDE, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 640u * 4u);

    /* An unknown mode is refused, says so, and changes nothing. */
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_MODE_SET, 4u, 0xBEEFu),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_OK, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 640u); /* still the mode that was taken */

    /*
     * A mode that does not fit the platform's buffer is refused too --
     * the case a platform creates by allocating less than
     * emu_fb_max_bytes. Checked with a deliberately small buffer,
     * because it is the only way this path is reachable.
     */
    CHECK(emu_fb_init(&fb, 320u, 200u, EMU_FB_FMT_IDX8, 0u, big, 0u,
                      320u * 200u, NULL, NULL));
    CHECK_EQ(emu_fb_ops.write(&fb, EMU_FB_MODE_SET, 4u,
                              EMU_FB_MODE_1024X768X32),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_MODE_OK, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);
    CHECK_EQ(emu_fb_ops.read(&fb, EMU_FB_WIDTH, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 320u);
}

/*
 * The input devices.
 *
 * Same file because they share the framebuffer's arrangement -- a
 * portable device with the platform supplying the events -- and the same
 * reason it needs no display: a test posts its own and reads them back
 * exactly as a guest would.
 */
void test_input(void)
{
    emu_input_t in;
    uint32_t v;

    emu_input_init(&in, EMU_INPUT_ID_KEYBOARD);

    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ID, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_INPUT_ID_KEYBOARD);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_PENDING, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* An empty ring reads 0, which cannot be a valid event. */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* --- an event round-trips, and the encoding is decodable ------ */
    emu_input_post(&in, 30u, 1u); /* 'a' down */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_PENDING, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 1u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v), EMU_FAULT_NONE);
    CHECK(  (v & EMU_INPUT_EV_VALID) != 0u);
    CHECK_EQ(EMU_INPUT_EV_CODE(v), 30u);
    CHECK_EQ(EMU_INPUT_EV_VALUE(v), 1u);

    /*
     * **A key-up must be distinguishable from an empty ring**, and it is
     * the one case where they could be confused: its value is zero, so
     * only the VALID bit separates "the key was released" from "there is
     * nothing here". A device that returned a bare 0 for a key-up would
     * make every release invisible.
     */
    emu_input_post(&in, 30u, 0u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v), EMU_FAULT_NONE);
    CHECK(  (v & EMU_INPUT_EV_VALID) != 0u);
    CHECK_EQ(EMU_INPUT_EV_CODE(v), 30u);
    CHECK_EQ(EMU_INPUT_EV_VALUE(v), 0u);

    /* Drained again. */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* --- order is preserved, which a ring can get wrong ----------- */
    for (uint32_t i = 0; i < 8u; i++) {
        emu_input_post(&in, 100u + i, 1u);
    }
    for (uint32_t i = 0; i < 8u; i++) {
        CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v),
                 EMU_FAULT_NONE);
        CHECK_EQ(EMU_INPUT_EV_CODE(v), 100u + i);
    }

    /*
     * --- overflow keeps the oldest and counts the rest -------------
     *
     * The discriminating case. A ring that dropped the *front* to make
     * room would keep the newest, which sounds equally reasonable and is
     * not: it discards the key-down and keeps the key-up, so a guest
     * sees a release it never saw pressed. That is the stuck key a
     * player feels rather than a message anyone reads.
     */
    emu_input_init(&in, EMU_INPUT_ID_KEYBOARD);
    for (uint32_t i = 0; i < EMU_INPUT_RING + 10u; i++) {
        emu_input_post(&in, 200u + i, 1u);
    }
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_PENDING, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_INPUT_RING);

    /* The first event still there is the first one posted. */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_EVENT, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(EMU_INPUT_EV_CODE(v), 200u);

    /* LOST reports the ten that did not fit, and clears on read. */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_LOST, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 10u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_LOST, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);

    /* --- the mouse: position is state, buttons are both ----------- */
    emu_input_init(&in, EMU_INPUT_ID_MOUSE);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ID, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, EMU_INPUT_ID_MOUSE);

    /*
     * Motion does not fill the ring. A guest that missed three motions
     * still wants to know where the pointer is, and reconstructing that
     * by summing deltas it may have dropped is how a cursor drifts.
     */
    for (uint32_t i = 0; i < 100u; i++) {
        emu_input_motion(&in, 10u + i, 20u + i);
    }
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_PENDING, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0u); /* 100 motions, no events, no overflow */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_LOST, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ABS_X, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 109u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ABS_Y, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 119u);

    /* A button is an event *and* a bit in the summary. */
    emu_input_post(&in, EMU_INPUT_BTN_LEFT, 1u);
    emu_input_post(&in, EMU_INPUT_BTN_RIGHT, 1u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_BUTTONS, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0x3u);
    emu_input_post(&in, EMU_INPUT_BTN_LEFT, 0u);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_BUTTONS, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 0x2u);
    /* And all three are still in the ring. */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_PENDING, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, 3u);

    /* --- word accesses only, naming the direction ----------------- */
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ID, 1u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ID + 1u, 4u, &v),
             EMU_FAULT_LOAD);
    CHECK_EQ(emu_input_ops.write(&in, EMU_INPUT_ID, 2u, 0u), EMU_FAULT_STORE);

    /* Nothing is writable; a write is ignored rather than fatal. */
    CHECK_EQ(emu_input_ops.write(&in, EMU_INPUT_ABS_X, 4u, 999u),
             EMU_FAULT_NONE);
    CHECK_EQ(emu_input_ops.read(&in, EMU_INPUT_ABS_X, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 109u);
}
