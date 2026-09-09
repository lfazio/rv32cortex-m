/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_input.c - keyboard and mouse, as a ring a guest drains.
 *
 * One implementation for both devices: the mechanism is a ring of event
 * words and only the payload differs, so a keyboard and a mouse are the
 * same code with a different id. They are separate *instances* for the
 * reason a host binds a separate evdev node to each -- see emu_dev.h.
 *
 * Nothing here knows where events come from. The platform posts them;
 * on a host that is SDL, on a board it is nothing at all, and a guest
 * polling a device nobody posts to reads an empty ring rather than
 * hanging.
 */

#include "emu/emu_dev.h"

void emu_input_init(emu_input_t *in, uint32_t id)
{
    in->id = id;
    in->head = 0u;
    in->count = 0u;
    in->lost = 0u;
    in->abs_x = 0u;
    in->abs_y = 0u;
    in->buttons = 0u;
}

void emu_input_post(emu_input_t *in, uint32_t code, uint32_t value)
{
    /*
     * Full: drop the *newest* and count it.
     *
     * Keeping the oldest is the choice that matters. A ring that
     * discarded the front to make room would drop the key-down and keep
     * the key-up, and a guest replaying that sees a key released it
     * never saw pressed -- which is the stuck-key bug a player feels
     * rather than a message anyone reads. Dropping the newest loses the
     * end of a burst, which a guest recovers from at the next poll.
     */
    if (in->count >= EMU_INPUT_RING) {
        in->lost++;
        return;
    }

    /*
     * Buttons are also state, so the mouse's summary register tracks
     * them here rather than making a guest reconstruct it from the ring
     * -- the same reason the pointer position is not an event.
     */
    if (code >= EMU_INPUT_BTN_LEFT && code <= EMU_INPUT_BTN_MIDDLE) {
        const uint32_t bit = 1u << (code - EMU_INPUT_BTN_LEFT);

        if (value != 0u) {
            in->buttons |= bit;
        } else {
            in->buttons &= ~bit;
        }
    }

    in->ring[(in->head + in->count) % EMU_INPUT_RING] =
        EMU_INPUT_EV_VALID | ((value & 0x7FFFu) << 16) | (code & 0xFFFFu);
    in->count++;
}

void emu_input_motion(emu_input_t *in, uint32_t x, uint32_t y)
{
    in->abs_x = x;
    in->abs_y = y;
}

static emu_fault_t input_read(void *ctx, uint32_t off, uint32_t size,
                              uint32_t *out)
{
    emu_input_t *in = (emu_input_t *)ctx;

    /*
     * Word accesses only, as with the framebuffer, and EMU_FAULT_LOAD
     * because the bus reports the direction refused and leaves naming
     * the fault to the frontend.
     */
    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case EMU_INPUT_ID:
        *out = in->id;
        break;
    case EMU_INPUT_VERSION:
        *out = EMU_INPUT_VERSION_1;
        break;
    case EMU_INPUT_PENDING:
        *out = in->count;
        break;

    case EMU_INPUT_EVENT:
        /*
         * Dequeue. An empty ring reads 0, which has EMU_INPUT_EV_VALID
         * clear and so cannot be mistaken for an event -- including for
         * a key-up, whose value is zero but whose word is not.
         *
         * **Reading is destructive, which makes this register unlike
         * every other one here.** That is what a ring is, but it means
         * a guest must read it once and keep the value; reading it twice
         * to look at the code and then the value consumes two events and
         * throws one away.
         */
        if (in->count == 0u) {
            *out = 0u;
        } else {
            *out = in->ring[in->head];
            in->head = (in->head + 1u) % EMU_INPUT_RING;
            in->count--;
        }
        break;

    case EMU_INPUT_LOST:
        /*
         * Cleared on read, so a guest sees each overflow once. A count
         * that accumulated for ever would be a number nobody could act
         * on -- what a guest wants to know is whether it fell behind
         * *since it last looked*.
         */
        *out = in->lost;
        in->lost = 0u;
        break;

    case EMU_INPUT_ABS_X:
        *out = in->abs_x;
        break;
    case EMU_INPUT_ABS_Y:
        *out = in->abs_y;
        break;
    case EMU_INPUT_BUTTONS:
        *out = in->buttons;
        break;

    default:
        /* Unassigned reads zero rather than faulting, so a guest can
         * probe for a later version's register. */
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t input_write(void *ctx, uint32_t off, uint32_t size,
                               uint32_t val)
{
    (void)ctx;
    (void)off;
    (void)val;

    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_STORE;
    }
    /*
     * Nothing here is writable. Ignored rather than faulted, for the
     * reason the framebuffer's geometry is: a guest that writes has made
     * a mistake this device cannot fix, and killing it teaches less than
     * letting it read back what it did not change.
     */
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t emu_input_ops = {
    .read = input_read,
    .write = input_write,
    .tick = NULL,
};
