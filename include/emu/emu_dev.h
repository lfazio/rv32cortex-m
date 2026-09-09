/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_dev.h - Virtual devices that belong to no particular architecture.
 *
 * A console UART is a console UART whatever the guest core is, so it lives
 * here and every frontend gets it. Devices that are part of an
 * architecture's own privileged design -- the RISC-V CLINT and APLIC, an
 * RH850 INTC2 -- belong to that frontend instead, because raising an
 * interrupt means writing that architecture's pending register.
 *
 * Everything else the guest touches goes straight to real hardware through
 * the passthrough window.
 */
#ifndef EMU_DEV_H
#define EMU_DEV_H

#include "emu_types.h"
#include "emu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Console UART                                                        */
/* ------------------------------------------------------------------ */

/*
 * A minimal NS16550 subset: enough for a guest's putchar/getchar and for
 * anything that pokes THR and polls LSR, which covers the usual bare-metal
 * console code and OpenSBI-style output.
 *
 *   +0x00  RBR (read) / THR (write)
 *   +0x05  LSR: bit 0 = data ready, bit 5 = THR empty, bit 6 = TX idle
 */
#define EMU_UART_SIZE 0x100u
#define EMU_UART_RBR_THR 0x00u
#define EMU_UART_IER 0x01u
#define EMU_UART_IIR_FCR 0x02u
#define EMU_UART_LCR 0x03u
#define EMU_UART_MCR 0x04u
#define EMU_UART_LSR 0x05u
#define EMU_UART_MSR 0x06u
#define EMU_UART_SCR 0x07u

#define EMU_UART_LSR_DR 0x01u
#define EMU_UART_LSR_THRE 0x20u
#define EMU_UART_LSR_TEMT 0x40u

typedef struct emu_uart {
    /* Backing transport, supplied by the platform. */
    void (*tx)(void *ctx, uint8_t c);
    /* Return the next byte, or -1 if none is available. */
    int (*rx)(void *ctx);
    void *ctx;

    /*
     * Reporting LSR.DR requires knowing whether a byte is available
     * without consuming it, but the transport only offers a destructive
     * read. One byte of lookahead bridges the two.  -1 means empty.
     */
    int pending;

    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
} emu_uart_t;

extern const emu_dev_ops_t emu_uart_ops;

void emu_uart_init(emu_uart_t *u, void (*tx)(void *ctx, uint8_t c),
                   int (*rx)(void *ctx), void *ctx);

/* ------------------------------------------------------------------ */
/* Framebuffer                                                         */
/* ------------------------------------------------------------------ */

/*
 * A framebuffer the guest draws into and asks to have shown.
 *
 * **The pixels are RAM, not MMIO, and that is the whole design.** A
 * device `write` per pixel would put a function call and a region walk
 * between the guest and every dot on the screen; at 320x200 that is
 * 64,000 calls a frame before anything is drawn twice. So the pixel
 * buffer is an ordinary RAM region -- the guest stores to it exactly as
 * it stores to anything -- and only the *control* block is a device.
 * The register set below is therefore small and cold: read the geometry
 * once at start-up, write FLUSH once a frame.
 *
 * The split is the console's, one layer up: this file owns the register
 * semantics and knows nothing about how a frame reaches a human, and the
 * platform supplies `present`. A host build hands it to SDL; the board
 * has no screen and supplies nothing, which must keep working -- the
 * device is portable C and `src/emu/` may not depend on a windowing
 * system it cannot have.
 *
 * Indexed colour is supported because it is what the games use: Doom is
 * 8-bit paletted, and at 320x200 that is 64 KB a frame against 256 KB
 * for 32-bit. The conversion is *not* done here. `present` receives the
 * format and the palette and does what its display can do natively --
 * SDL takes an indexed surface directly -- because converting in this
 * file would mean a second full-size buffer and a per-pixel loop on
 * every platform, including the ones that would then throw it away.
 */
#define EMU_FB_CTRL_SIZE 0x1000u

#define EMU_FB_ID 0x00u /* R: 'FBUF', so a guest can detect it */
#define EMU_FB_VERSION 0x04u /* R                                   */
#define EMU_FB_WIDTH 0x08u /* R: pixels                           */
#define EMU_FB_HEIGHT 0x0Cu /* R: pixels                           */
#define EMU_FB_FORMAT 0x10u /* R: emu_fb_format_t                  */
#define EMU_FB_STRIDE 0x14u /* R: bytes per row                    */
#define EMU_FB_BASE 0x18u /* R: guest address of the pixels      */
#define EMU_FB_BYTES 0x1Cu /* R: size of the pixel region         */
#define EMU_FB_FLUSH 0x20u /* W: any value presents a frame       */
#define EMU_FB_FRAMES 0x24u /* R: frames presented, low 32 bits    */
#define EMU_FB_PALETTE 0x400u /* R/W: 256 entries, 0x00RRGGBB       */

#define EMU_FB_ID_MAGIC 0x46425546u /* 'FBUF' */
#define EMU_FB_VERSION_1 1u
#define EMU_FB_PALETTE_ENTRIES 256u

typedef enum emu_fb_format {
    /*
     * One byte per pixel, an index into the palette. The games' native
     * format, and a quarter of the memory traffic of the one below.
     */
    EMU_FB_FMT_IDX8 = 0,
    /* Four bytes per pixel, 0x00RRGGBB; the top byte is ignored. */
    EMU_FB_FMT_XRGB8888 = 1,
} emu_fb_format_t;

/* What `present` is handed. Borrowed for the duration of the call. */
typedef struct emu_fb_frame {
    const void *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per row                              */
    uint32_t format; /* emu_fb_format_t                            */
    /* 256 entries of 0x00RRGGBB. Meaningful for IDX8 only. */
    const uint32_t *palette;
} emu_fb_frame_t;

typedef struct emu_fb {
    /*
     * Show a frame. NULL is legal and means nothing is displayed: a
     * board with no screen, or a test that only wants the register
     * semantics. The guest cannot tell, which is deliberate -- a guest
     * that hangs waiting for a display it does not have is worse than
     * one that draws into the void.
     */
    void (*present)(void *ctx, const emu_fb_frame_t *frame);
    void *ctx;

    /* The pixel region, which the platform allocates and also
     * registers on the bus as RAM. This is the same memory. */
    void *pixels;
    uint32_t base; /* guest address of `pixels`                   */
    uint32_t bytes;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;

    uint32_t palette[EMU_FB_PALETTE_ENTRIES];
    uint64_t frames;
} emu_fb_t;

extern const emu_dev_ops_t emu_fb_ops;

/*
 * `stride` may be 0, meaning "the natural one for this format and
 * width". A caller that passes its own is trusted with it: a stride
 * wider than the width is how a guest gets row alignment, and one
 * narrower would make rows overlap, so it is rejected.
 *
 * Returns false if the geometry does not fit `bytes`, which is the one
 * error this can make that would otherwise be a read off the end of the
 * buffer on the first present.
 */
bool emu_fb_init(emu_fb_t *fb, uint32_t width, uint32_t height,
                 emu_fb_format_t format, uint32_t stride, void *pixels,
                 uint32_t base, uint32_t bytes,
                 void (*present)(void *ctx, const emu_fb_frame_t *frame),
                 void *ctx);

/* Bytes per pixel for a format; 0 if the format is not one. */
uint32_t emu_fb_bpp(emu_fb_format_t format);

#ifdef __cplusplus
}
#endif

#endif /* EMU_DEV_H */
