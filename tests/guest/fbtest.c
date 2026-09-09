/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fbtest.c - the framebuffer, from the guest's side.
 *
 * The unit test drives the device through its ops table directly. This
 * one goes the whole way: through the bus, at the addresses a real guest
 * uses, with pixels written as ordinary stores rather than through any
 * device call. It is the difference between "the device works" and "the
 * device is reachable", and this project has been caught by that gap
 * often enough to spend a guest on it.
 *
 * It checks what a guest would actually rely on: that the thing at
 * EMU_GUEST_FB_BASE identifies itself, that the geometry it reports
 * describes the buffer it points at, that the buffer is writable memory
 * at the advertised address, and that FLUSH is counted.
 */

#include <stdint.h>

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#define FB_BASE 0x30000000u
#define FB_REG(off) (*(volatile uint32_t *)(FB_BASE + (off)))

#define FB_ID 0x00u
#define FB_VERSION 0x04u
#define FB_WIDTH 0x08u
#define FB_HEIGHT 0x0Cu
#define FB_FORMAT 0x10u
#define FB_STRIDE 0x14u
#define FB_PIXBASE 0x18u
#define FB_BYTES 0x1Cu
#define FB_FLUSH 0x20u
#define FB_FRAMES 0x24u
#define FB_PALETTE 0x400u

#define FB_ID_MAGIC 0x46425546u /* 'FBUF' */

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        const uint32_t d = (v >> i) & 0xFu;

        UART_THR = (uint8_t)(d < 10u ? ('0' + d) : ('a' + d - 10u));
    }
}

static uint32_t g_fail;

static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        g_fail++;
        puts_("  FAIL ");
        puts_(name);
        puts_(" got 0x");
        puthex(got);
        puts_(" want 0x");
        puthex(want);
        puts_("\n");
    } else {
        puts_("  ok   ");
        puts_(name);
        puts_("\n");
    }
}

int main(void)
{
    puts_("\nFBTEST-START\n");

    /*
     * Identify it first. A guest that assumed a display was there and
     * wrote into an unmapped window would fault; the ID register is how
     * it asks instead, and it is the reason the device has one.
     */
    check("id", FB_REG(FB_ID), FB_ID_MAGIC);
    check("version", FB_REG(FB_VERSION), 1u);

    const uint32_t w = FB_REG(FB_WIDTH);
    const uint32_t h = FB_REG(FB_HEIGHT);
    const uint32_t stride = FB_REG(FB_STRIDE);
    const uint32_t pix = FB_REG(FB_PIXBASE);
    const uint32_t bytes = FB_REG(FB_BYTES);

    check("format-idx8", FB_REG(FB_FORMAT), 0u);

    /*
     * The geometry has to describe the buffer, not merely be plausible.
     * A stride that does not span the width, or a buffer too small for
     * the rows, is the shape of error that shows up later as a picture
     * that shears -- so it is checked here where it names itself.
     */
    check("stride-spans-width", (stride >= w) ? 1u : 0u, 1u);
    check("buffer-holds-frame", (stride * h <= bytes) ? 1u : 0u, 1u);

    /*
     * The pixels are ordinary memory at the advertised address. This is
     * the claim the whole design rests on: written as stores, with no
     * device call between the guest and the buffer.
     */
    volatile uint8_t *const fb = (volatile uint8_t *)pix;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            fb[y * stride + x] = (uint8_t)(x ^ y);
        }
    }

    /* Read back two corners: if this were MMIO discarding writes, or a
     * region shorter than advertised, one of them would not hold. */
    check("pixel-first", fb[0], 0u);
    check("pixel-last", fb[(h - 1u) * stride + (w - 1u)],
          (uint8_t)((w - 1u) ^ (h - 1u)));

    /* The palette is device state, so it round-trips through the
     * registers rather than through memory. */
    FB_REG(FB_PALETTE + 4u * 7u) = 0x00C0FFEEu;
    check("palette", FB_REG(FB_PALETTE + 4u * 7u), 0x00C0FFEEu);

    /*
     * Present. The count must move whether or not anything is on a
     * screen -- this runs headless, and a guest measuring its own frame
     * rate cannot be made to depend on the platform having a window.
     */
    const uint32_t before = FB_REG(FB_FRAMES);

    /*
     * Sixty frames of something moving, so a run with a window attached
     * shows a picture that is obviously alive rather than a still that
     * could equally be a stuck buffer. A colour ramp scrolling under a
     * moving bar exercises the two things a display can get wrong
     * independently: the palette, and the stride.
     */
    for (uint32_t i = 0; i < 60u; i++) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t bar = ((x + i * 4u) % w < 8u) ? 255u : 0u;

                fb[y * stride + x] = (uint8_t)(bar | ((x + y + i) & 0x7Fu));
            }
        }
        FB_REG(FB_FLUSH) = 1u;
    }
    check("frames", FB_REG(FB_FRAMES) - before, 60u);

    /* Geometry is read-only; a write must be ignored rather than taken. */
    FB_REG(FB_WIDTH) = 1234u;
    check("width-read-only", FB_REG(FB_WIDTH), w);

    puts_("  failures 0x");
    puthex(g_fail);
    puts_("\nFBTEST-END\n");

    return (int)g_fail;
}
