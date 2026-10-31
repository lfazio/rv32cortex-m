/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_tinyemu.h - the TinyEMU entry points this tree wraps.
 *
 * Use this rather than virtio_block_init: it is the same device, except
 * that every request completes. See virtio_tinyemu.c.
 */

#ifndef EMU_VIRTIO_TINYEMU_H
#define EMU_VIRTIO_TINYEMU_H

#include "virtio.h"

VIRTIODevice *emu_virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

/* And virtio_net_init's: a frame shorter than its header is refused. */
VIRTIODevice *emu_virtio_net_init(VIRTIOBusDef *bus, EthernetDevice *es);

#endif /* EMU_VIRTIO_TINYEMU_H */
