/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cutils.h - the name TinyEMU's virtio.c includes.
 *
 * A shim, so the vendored file stays byte-identical to upstream and a
 * future update is a copy rather than a merge. Everything it actually
 * needs from TinyEMU's cutils -- eight inline functions and a typedef --
 * is in virtio_glue.h.
 */
#ifndef EMU_VIRTIO_CUTILS_SHIM_H
#define EMU_VIRTIO_CUTILS_SHIM_H
#include "virtio_glue.h"
#endif
