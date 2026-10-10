/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_tinyemu.h - the one TinyEMU entry point this tree wraps.
 *
 * Use this rather than virtio_block_init: it is the same device, except
 * that every request completes. See virtio_tinyemu.c.
 */

#ifndef EMU_VIRTIO_TINYEMU_H
#define EMU_VIRTIO_TINYEMU_H

#include "virtio.h"

VIRTIODevice *emu_virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

#endif /* EMU_VIRTIO_TINYEMU_H */
