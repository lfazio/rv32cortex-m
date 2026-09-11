/* SPDX-License-Identifier: Apache-2.0 */
/*
 * iomem.h - the name TinyEMU's virtio.h includes.
 *
 * A shim, for the reason cutils.h beside it is one. TinyEMU's real
 * iomem.h carries a whole physical memory map with dirty-bit tracking
 * and RAM registration; virtio.c uses four things from it, and those
 * are in virtio_glue.h.
 */
#ifndef EMU_VIRTIO_IOMEM_SHIM_H
#define EMU_VIRTIO_IOMEM_SHIM_H
#include "virtio_glue.h"
#endif
