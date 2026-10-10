/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_tinyemu.c - TinyEMU's virtio.c, compiled here, plus the block
 * request it drops.
 *
 * `third_party/tinyemu/virtio.c` stays byte-identical to upstream -- an
 * update is a copy, not a merge -- so this file does not patch it. It
 * *includes* it. That puts the device struct and the queue helpers, all
 * `static` there, in this translation unit, where one function can be
 * wrapped without a copy of either. If upstream renames or reshapes
 * them, this stops compiling, which is the right failure: the same one
 * virtio_glue.h relies on for the names virtio.c calls out to.
 *
 * **What it adds is completing every block request.** TinyEMU handles
 * IN and OUT and lets anything else fall through a `default: break;`:
 * no status written, descriptor never returned, used ring never moved.
 * The driver waits for ever, and nothing on either side says why.
 *
 * Linux sends one of those on every boot. Reading
 * /sys/block/vda/serial issues VIRTIO_BLK_T_GET_ID whatever features
 * were negotiated, and udev's persistent-storage rules read it for every
 * virtio disk. So the udev worker for vda blocked in the kernel,
 * `udevadm settle` waited out its 120 s, and boot carried on without
 * it. Measured: one dropped request per boot, type 8, a 16-byte header
 * and 21 writable bytes -- the 20-byte ID and its status.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "virtio.c"
#pragma GCC diagnostic pop

#include "virtio_tinyemu.h"

/* virtio 1.1, 5.2.6: the ID is up to 20 bytes, NUL-padded. */
#define VIRTIO_BLK_T_GET_ID 8u
#define VIRTIO_BLK_ID_BYTES 20

/*
 * GET_ID answers with an empty serial -- twenty zero bytes -- which is
 * what QEMU reports for a disk configured without one. Nothing here has
 * a serial to give, and an invented one would be read by udev as the
 * identity of a physical disk.
 *
 * Every other type IN and OUT do not cover is refused with UNSUPP, the
 * status the specification provides for exactly this, rather than
 * dropped. The driver then gets an error it can act on; Linux turns it
 * into -EOPNOTSUPP. None of FLUSH, DISCARD or WRITE_ZEROES is offered as
 * a feature, so a conforming driver should not send them -- but "should
 * not" is the case where a request that never completes is hardest to
 * find.
 */
static int block_recv(VIRTIODevice *s, int queue_idx, int desc_idx,
                      int read_size, int write_size)
{
    VIRTIOBlockDevice *const s1 = (VIRTIOBlockDevice *)s;
    BlockRequestHeader h;

    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0) {
        return 0;
    }
    if (h.type == VIRTIO_BLK_T_IN || h.type == VIRTIO_BLK_T_OUT) {
        return virtio_block_recv_request(s, queue_idx, desc_idx, read_size,
                                         write_size);
    }

    /*
     * The same rule upstream applies to IN and OUT: one request at a
     * time, and the queue is retried when the one in flight completes.
     */
    if (s1->req_in_progress) {
        return -1;
    }

    /*
     * The status is the last writable byte. A chain with nothing
     * writable has nowhere to put one, so it is returned unanswered
     * rather than kept -- keeping it is the defect this file exists for.
     */
    if (write_size < 1) {
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
        return 0;
    }

    /*
     * Offsets count from the first writable descriptor, so the ID starts
     * at 0 and the status is at write_size - 1 however the driver split
     * the chain.
     */
    if (h.type == VIRTIO_BLK_T_GET_ID) {
        static const uint8_t empty[VIRTIO_BLK_ID_BYTES];
        const uint8_t ok = VIRTIO_BLK_S_OK;
        const int id = (write_size - 1 < VIRTIO_BLK_ID_BYTES)
                           ? write_size - 1
                           : VIRTIO_BLK_ID_BYTES;

        memcpy_to_queue(s, queue_idx, desc_idx, 0, empty, id);
        memcpy_to_queue(s, queue_idx, desc_idx, write_size - 1, &ok, 1);
        virtio_consume_desc(s, queue_idx, desc_idx, id + 1);
        return 0;
    }

    {
        const uint8_t status = VIRTIO_BLK_S_UNSUPP;

        memcpy_to_queue(s, queue_idx, desc_idx, write_size - 1, &status, 1);
        virtio_consume_desc(s, queue_idx, desc_idx, 1);
    }
    return 0;
}

VIRTIODevice *emu_virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs)
{
    VIRTIODevice *const s = virtio_block_init(bus, bs);

    if (s != NULL) {
        s->device_recv = block_recv;
    }
    return s;
}
