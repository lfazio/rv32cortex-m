/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_image.h - the guest image in force, and how a new one arrives.
 *
 * The implementation is emu_image.c and is the same on every platform,
 * because it is written entirely in terms of the arena in board_api.h --
 * a flash sector on the boards, a buffer on the host. What a platform
 * still owns is where the *first* image comes from, which is the one
 * genuinely per-platform fact: linked in with .incbin, or read from a
 * file named on the command line.
 */
#ifndef EMU_PLATFORM_IMAGE_H
#define EMU_PLATFORM_IMAGE_H

#include "emu/emu_gdb.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The image to run, and the only way board_img / board_img_size are set.
 * Called once by the platform at start-up with whatever it brought.
 */
void emu_image_set(const uint8_t *img, uint32_t len);

/*
 * Take a freshly uploaded image, if one is waiting. True when the guest
 * was restarted from it.
 *
 * One function because there are two callers that must not drift:
 * between guest slices, and after a guest has halted. **The second is
 * the one that matters for a test harness and was the one missing** -- a
 * harness runs a test, waits for it to halt, then pushes the next, by
 * which time the run loop has exited. Both transfers completed, the
 * server said so, and nothing happened, which is the most convincing
 * kind of failure because every visible signal is success.
 */
bool emu_image_take_pending(void);

/*
 * gdb's `load`, onto the same arena the TFTP path uses.
 *
 * Worth having because it collapses the whole upload dance into one
 * command: `load` puts the image where it actually lives and leaves the
 * debugger attached and in control, which is exactly the position from
 * which a guest bug is worth looking at.
 */
extern const emu_gdb_flash_ops_t emu_image_gdb_flash;

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_IMAGE_H */
