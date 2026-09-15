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

/*
 * Mode setting, with VBE's numbers.
 *
 * **This is not a VBE BIOS and cannot be one.** VBE is reached through
 * INT 10h, a real-mode x86 software interrupt, and a RISC-V or RH850
 * guest has no such thing to call. What survives the move is the part
 * that was never x86-specific: the *mode numbers*, and VBE 2.0's linear
 * framebuffer -- which is the arrangement this device already has, and
 * the reason VBE rather than VGA is the right ancestor. See
 * docs/vga.md.
 *
 * So a guest sets a mode by writing its VBE number to MODE_SET rather
 * than by calling a BIOS, and enumerates what is available by writing an
 * index to MODE_INDEX and reading the four registers below it. A porting
 * layer that already knows `0x101 is 640x480x8` keeps that knowledge.
 *
 * Setting a mode does not move the pixels: FB_BASE and FB_BYTES describe
 * a buffer the platform allocated once, large enough for every mode it
 * offers, and a mode that would not fit is refused. That is what makes
 * mode setting cheap here where on real hardware it is not.
 */
#define EMU_FB_MODE_COUNT 0x28u /* R: how many modes are offered   */
#define EMU_FB_MODE_INDEX 0x2Cu /* W: which one to describe below  */
#define EMU_FB_MODE_NUMBER 0x30u /* R: its VBE number               */
#define EMU_FB_MODE_WIDTH 0x34u /* R                               */
#define EMU_FB_MODE_HEIGHT 0x38u /* R                               */
#define EMU_FB_MODE_FORMAT 0x3Cu /* R: emu_fb_format_t              */
#define EMU_FB_MODE_SET 0x40u /* W: a VBE number; R: the current */
#define EMU_FB_MODE_OK 0x44u /* R: 1 if the last SET was taken  */

#define EMU_FB_PALETTE 0x400u /* R/W: 256 entries, 0x00RRGGBB       */

/*
 * The VBE mode numbers this device answers to. Standard values, so a
 * guest that already has a VBE table does not need a new one; the 8-bit
 * modes are indexed and the 32-bit modes are XRGB, which are the two
 * formats above.
 *
 * 0x13 is the odd one out: it is a *VGA* mode number rather than a VBE
 * one, and it is here because 320x200x8 is what Doom asks for and the
 * number every port of it already uses.
 */
#define EMU_FB_MODE_320X200X8 0x013u
#define EMU_FB_MODE_640X400X8 0x100u
#define EMU_FB_MODE_640X480X8 0x101u
#define EMU_FB_MODE_800X600X8 0x103u
#define EMU_FB_MODE_1024X768X8 0x105u
#define EMU_FB_MODE_640X480X32 0x112u
#define EMU_FB_MODE_800X600X32 0x115u
#define EMU_FB_MODE_1024X768X32 0x118u

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

    /* Mode enumeration: which entry MODE_INDEX last selected, and
     * whether the last MODE_SET was accepted. */
    uint32_t mode_index;
    uint32_t mode_number;
    uint32_t mode_ok;
} emu_fb_t;

/* One row of the mode table, in the order a guest enumerates them. */
typedef struct emu_fb_mode {
    uint16_t number; /* the VBE number a guest writes to MODE_SET */
    uint16_t width;
    uint16_t height;
    uint8_t format; /* emu_fb_format_t                           */
} emu_fb_mode_t;

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

/*
 * The largest buffer any offered mode needs, so a platform can allocate
 * once and never refuse a mode for want of memory. A platform that
 * allocates less is not wrong -- modes that do not fit are refused at
 * MODE_SET -- but it will offer a guest a mode list it cannot honour.
 */
uint32_t emu_fb_max_bytes(void);

/*
 * Whether the device offers this geometry at this format.
 *
 * For a *platform* choosing the mode it starts in. emu_fb_init accepts
 * any geometry that fits the buffer, which is right for it -- the
 * buffer is the only thing it can check -- but a platform that started
 * the device at a size no mode produces would report that size from the
 * geometry registers while MODE_GET matched nothing in the list, so a
 * guest enumerating modes could not find the one it was already in.
 */
bool emu_fb_has_mode(uint32_t width, uint32_t height, emu_fb_format_t format);

/* ------------------------------------------------------------------ */
/* Input: keyboard and mouse                                           */
/* ------------------------------------------------------------------ */

/*
 * Two devices, not one, and the reason is the same one that makes a
 * host bind a separate evdev node to each: a guest that wants only a
 * keyboard should not have to decode mouse motion to find its
 * keystrokes, and a driver written for one should not need a descriptor
 * claiming both. They share this file and this implementation because
 * the *mechanism* is identical -- a ring of events a guest drains -- and
 * only the payload differs.
 *
 * **A ring, not a single register.** A guest polls at whatever rate its
 * frame loop runs, and a mouse moving under a 35 Hz game produces events
 * far faster than that; a device holding "the last event" would drop
 * most of them and, worse, would drop them silently. The ring holds
 * what arrived between polls, and says so when it could not.
 *
 * **Overflow is reported, not hidden.** When the ring fills, the oldest
 * events are kept and the newest dropped -- keeping the oldest is what
 * preserves a key-down whose key-up would otherwise be lost, which is
 * the failure a player feels as a stuck key. LOST counts what did not
 * fit, so a guest can tell "nothing happened" from "I could not keep
 * up".
 *
 * The event codes are Linux's evdev values, because they are the ones
 * every port already has a table for, and inventing a third numbering
 * would mean writing that table again. This is not an evdev protocol --
 * there is no input_event struct, no timestamps, no SYN -- only the
 * numbering.
 */
#define EMU_INPUT_SIZE 0x100u

#define EMU_INPUT_ID 0x00u /* R: 'INPK' or 'INPM'              */
#define EMU_INPUT_VERSION 0x04u /* R                                */
#define EMU_INPUT_PENDING 0x08u /* R: events waiting               */
#define EMU_INPUT_EVENT 0x0Cu /* R: dequeue one; 0 if empty      */
#define EMU_INPUT_LOST 0x10u /* R: events dropped, cleared on read */

/*
 * A mouse's absolute position, which is *not* in the ring.
 *
 * Position is state, not an event: a guest that missed three motions
 * still wants to know where the pointer is now, and making it
 * reconstruct that by summing deltas it may have dropped is how a
 * cursor drifts. The ring carries buttons and motion *notifications*;
 * these two registers carry the truth.
 */
#define EMU_INPUT_ABS_X 0x14u /* R: mouse only                    */
#define EMU_INPUT_ABS_Y 0x18u /* R: mouse only                    */
#define EMU_INPUT_BUTTONS 0x1Cu /* R: mouse only, bit per button    */

#define EMU_INPUT_ID_KEYBOARD 0x4B504E49u /* 'INPK' */
#define EMU_INPUT_ID_MOUSE 0x4D504E49u /* 'INPM' */
#define EMU_INPUT_VERSION_1 1u

/*
 * One event word: the code in the low 16 bits, the value above it, and
 * bit 31 marking the word as real -- so a guest that reads an empty ring
 * gets 0 and can test for it without a separate register access. A
 * legitimate event always has bit 31 set, including a key-up whose value
 * is zero.
 */
#define EMU_INPUT_EV_VALID 0x80000000u
#define EMU_INPUT_EV_CODE(w) ((w) & 0xFFFFu)
#define EMU_INPUT_EV_VALUE(w) (((w) >> 16) & 0x7FFFu)

/*
 * Mouse buttons, as bits in EMU_INPUT_BUTTONS and as codes in the ring.
 * The codes are evdev's BTN_LEFT and its neighbours.
 */
#define EMU_INPUT_BTN_LEFT 0x110u
#define EMU_INPUT_BTN_RIGHT 0x111u
#define EMU_INPUT_BTN_MIDDLE 0x112u

#define EMU_INPUT_RING 64u

typedef struct emu_input {
    uint32_t id; /* EMU_INPUT_ID_KEYBOARD or _MOUSE           */

    uint32_t ring[EMU_INPUT_RING];
    uint32_t head; /* next to read                              */
    uint32_t count; /* how many are waiting                      */
    uint32_t lost;

    /* Mouse state. Meaningless on a keyboard and read as zero. */
    uint32_t abs_x;
    uint32_t abs_y;
    uint32_t buttons;
} emu_input_t;

extern const emu_dev_ops_t emu_input_ops;

void emu_input_init(emu_input_t *in, uint32_t id);

/*
 * Post an event. Called by the platform from wherever its events come
 * from -- SDL on a host -- and safe to call when nothing is draining:
 * the ring fills, LOST counts, and no memory is touched beyond it.
 */
void emu_input_post(emu_input_t *in, uint32_t code, uint32_t value);

/* Update the pointer, which is state rather than an event. */
void emu_input_motion(emu_input_t *in, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif /* EMU_DEV_H */
