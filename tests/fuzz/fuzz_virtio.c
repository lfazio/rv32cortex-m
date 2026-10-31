/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_virtio.c - virtio devices, driven by a guest that writes anything.
 *
 * A guest's virtio driver hands the devices descriptor tables and rings
 * in its own memory, and TinyEMU's devices (third_party/tinyemu, through
 * src/emu/virtio/) follow them: addresses, lengths, flags and `next`
 * links are all the guest's. This is that guest, written by the fuzzer:
 * it programs each device's virtio-mmio registers, sets up queues, writes
 * descriptors and rings into guest RAM, posts buffers and notifies, and
 * sends key and mouse events -- on a console, a read-only disk, a
 * loopback network interface, a keyboard and a mouse.
 *
 * Held to ASan and UBSan -- a device reaching past guest RAM into the
 * host's memory is the failure that matters -- and to libFuzzer's
 * timeout: a device that follows a chain for ever hangs the emulator.
 * Guest RAM ends at an address no page or word boundary falls on, as
 * `--ram` allows.
 *
 * Not here: virtio-9p, which carries out a guest's file operations on a
 * host directory; fuzzing it is fuzzing the host's filesystem.
 *
 * The devices are made once and reset before each input the way a
 * driver resets one, by writing 0 to Status; the loopback's and the
 * input devices' queues are drained by that reset too.
 *
 * FUZZ_STATS=1 prints what the run covered at exit.
 */

#include "emu/emu_bus.h"
#include "emu/emu_virtio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0xF9FEu /* ends on no word and no page */
#define DEV_BASE 0x10001000u
#define DEV_STRIDE 0x1000u
#define NDEV 5u

/* virtio-mmio, version 2. */
#define R_DEV_FEAT_SEL 0x014u
#define R_DRV_FEAT 0x020u
#define R_DRV_FEAT_SEL 0x024u
#define R_QUEUE_SEL 0x030u
#define R_QUEUE_NUM 0x038u
#define R_QUEUE_READY 0x044u
#define R_QUEUE_NOTIFY 0x050u
#define R_INT_ACK 0x064u
#define R_STATUS 0x070u
#define R_DESC_LO 0x080u
#define R_AVAIL_LO 0x090u
#define R_USED_LO 0x0A0u

static uint8_t *g_ram;
static emu_bus_t g_bus;

static struct {
    uint64_t inputs, mmio, queues, posts, chains, keys, irqs;
} g_st;

static void stats_at_exit(void)
{
    fprintf(stderr,
            "fuzz-virtio: %llu inputs, %llu register writes, %llu queues "
            "set up, %llu buffers posted (%llu with a chain), %llu input "
            "events, %llu interrupts raised\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.mmio,
            (unsigned long long)g_st.queues, (unsigned long long)g_st.posts,
            (unsigned long long)g_st.chains, (unsigned long long)g_st.keys,
            (unsigned long long)g_st.irqs);
}

static void irq(void *ctx, int num, int level)
{
    (void)ctx;
    (void)num;
    g_st.irqs += level != 0;
}

typedef struct {
    const uint8_t *p;
    size_t n;
} in_t;

static uint32_t take(in_t *in, unsigned bytes)
{
    uint32_t v = 0u;

    for (unsigned i = 0u; i < bytes; i++) {
        v |= (uint32_t)(in->n != 0u ? (in->n--, *in->p++) : 0u) << (8u * i);
    }
    return v;
}

static void reg(unsigned dev, uint32_t off, uint32_t v)
{
    g_st.mmio++;
    (void)emu_bus_write(&g_bus, DEV_BASE + dev * DEV_STRIDE + off, 4u, v);
}

static void ram_put(uint32_t off, const void *src, uint32_t n)
{
    if (off < RAM_SIZE) {
        memcpy(g_ram + off, src, (n < RAM_SIZE - off) ? n : RAM_SIZE - off);
    }
}

static void put16(uint32_t off, uint16_t v)
{
    ram_put(off, &v, 2u);
}

/* A guest address: mostly in RAM, near its end often, sometimes not. */
static uint32_t pick_addr(in_t *in)
{
    const uint32_t sel = take(in, 1u);

    switch (sel % 4u) {
    case 0u:
        return RAM_BASE + take(in, 2u) % RAM_SIZE;
    case 1u:
        return RAM_BASE + RAM_SIZE - (take(in, 1u) % 64u);
    case 2u:
        return RAM_BASE + (take(in, 2u) & ~0xFu);
    default:
        return take(in, 4u);
    }
}

/* Each queue's rings, as the fuzzer last set them, so a post can find
 * them; nothing stops the input from moving them meanwhile. */
static struct {
    uint32_t num, desc, avail, used;
    uint16_t idx;
} g_q[NDEV][2];

static void reset_devices(void)
{
    for (unsigned d = 0u; d < NDEV; d++) {
        reg(d, R_STATUS, 0u);
    }
    memset(g_ram, 0, RAM_SIZE);
    memset(g_q, 0, sizeof(g_q));
}

static void event(in_t *in)
{
    const uint32_t op = take(in, 1u) % 8u;
    const unsigned d = take(in, 1u) % NDEV;
    const unsigned q = take(in, 1u) % 2u;

    switch (op) {
    case 0u: { /* any register, any value */
        static const uint32_t regs[] = {
            R_DEV_FEAT_SEL, R_DRV_FEAT, R_DRV_FEAT_SEL, R_QUEUE_SEL,
            R_QUEUE_NUM,    R_QUEUE_READY, R_QUEUE_NOTIFY, R_INT_ACK,
            R_STATUS,       R_DESC_LO,  R_AVAIL_LO,     R_USED_LO,
        };
        const uint32_t r = take(in, 1u);
        const uint32_t off =
            (r < 0xC0u) ? regs[r % (sizeof(regs) / sizeof(regs[0]))] : r * 4u;

        reg(d, off, (take(in, 1u) & 1u) ? pick_addr(in) : take(in, 4u));
        break;
    }
    case 1u: { /* a queue, set up as a driver would */
        const uint32_t num = 1u << (take(in, 1u) % 9u);

        g_q[d][q].num = (take(in, 1u) == 0xFFu) ? take(in, 2u) : num;
        g_q[d][q].desc = pick_addr(in) & ~15u;
        g_q[d][q].avail = pick_addr(in) & ~1u;
        g_q[d][q].used = pick_addr(in) & ~3u;
        g_q[d][q].idx = 0u;
        reg(d, R_STATUS, 0xBu); /* ACKNOWLEDGE | DRIVER | FEATURES_OK */
        reg(d, R_QUEUE_SEL, q);
        reg(d, R_QUEUE_NUM, g_q[d][q].num);
        reg(d, R_DESC_LO, g_q[d][q].desc);
        reg(d, R_AVAIL_LO, g_q[d][q].avail);
        reg(d, R_USED_LO, g_q[d][q].used);
        reg(d, R_QUEUE_READY, 1u);
        reg(d, R_STATUS, 0xFu); /* and DRIVER_OK */
        g_st.queues++;
        break;
    }
    case 2u:
    case 3u: { /* a descriptor */
        const uint32_t i = take(in, 1u);
        uint8_t desc[16];
        uint32_t a = pick_addr(in);
        const uint32_t len = (take(in, 1u) & 1u) ? take(in, 4u) : take(in, 2u);
        const uint16_t flags = (uint16_t)(take(in, 1u) & 7u);
        const uint16_t next = (uint16_t)take(in, 1u);

        memcpy(desc, &a, 4u);
        memset(desc + 4, 0, 4u);
        memcpy(desc + 8, &len, 4u);
        memcpy(desc + 12, &flags, 2u);
        memcpy(desc + 14, &next, 2u);
        ram_put(g_q[d][q].desc - RAM_BASE + i * 16u, desc, 16u);
        g_st.chains += (flags & 1u) != 0u;
        break;
    }
    case 4u: { /* post a chain's head and notify */
        const uint32_t num = g_q[d][q].num != 0u ? g_q[d][q].num : 1u;
        const uint32_t a = g_q[d][q].avail - RAM_BASE;

        put16(a + 4u + (g_q[d][q].idx % num) * 2u, (uint16_t)take(in, 1u));
        g_q[d][q].idx++;
        put16(a + 2u, g_q[d][q].idx);
        reg(d, R_QUEUE_NOTIFY, q);
        g_st.posts++;
        break;
    }
    case 5u: { /* bytes into guest RAM: a request header, data */
        uint8_t buf[64];
        const uint32_t off = take(in, 2u) % RAM_SIZE;
        const uint32_t n = take(in, 1u) % sizeof(buf);

        for (uint32_t i = 0u; i < n; i++) {
            buf[i] = (uint8_t)take(in, 1u);
        }
        ram_put(off, buf, n);
        break;
    }
    case 6u:
        g_st.keys++;
        if (take(in, 1u) & 1u) {
            emu_virtio_key_event(take(in, 1u) & 1u, (uint16_t)take(in, 2u));
        } else {
            emu_virtio_mouse_event((int8_t)take(in, 1u), (int8_t)take(in, 1u),
                                   (int8_t)take(in, 1u), take(in, 1u));
        }
        break;
    default:
        emu_virtio_net_poll();
        reg(d, R_INT_ACK, take(in, 1u));
        break;
    }
}

static bool setup(void)
{
    char disk[] = "/tmp/fuzz-virtio-disk-XXXXXX";
    static uint8_t zeros[8192];
    const int fd = mkstemp(disk);

    if (fd < 0 || write(fd, zeros, sizeof(zeros)) != (ssize_t)sizeof(zeros)) {
        return false;
    }
    (void)close(fd);

    /* Exactly RAM_SIZE on the heap, so a byte past it is ASan's. */
    g_ram = malloc(RAM_SIZE);
    if (g_ram == NULL) {
        return false;
    }
    emu_bus_init(&g_bus);
    if (!emu_bus_add_ram(&g_bus, "ram", RAM_BASE, g_ram, RAM_SIZE) ||
        !emu_virtio_init(&g_bus, irq, NULL) ||
        !emu_virtio_add_console(DEV_BASE, 1) ||
        !emu_virtio_add_block(DEV_BASE + DEV_STRIDE, 2, disk, false) ||
        !emu_virtio_add_net(DEV_BASE + 2u * DEV_STRIDE, 3, "loop") ||
        !emu_virtio_add_keyboard(DEV_BASE + 3u * DEV_STRIDE, 4) ||
        !emu_virtio_add_mouse(DEV_BASE + 4u * DEV_STRIDE, 5)) {
        return false;
    }
    /* Opened, so the name can go; the device keeps its descriptor. */
    (void)unlink(disk);
    /* The console's output is the guest's, and there is a lot of it. */
    if (freopen("/dev/null", "w", stdout) == NULL) {
        return false;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    in_t in = {data, size};

    if (!once) {
        once = true;
        if (!setup()) {
            fprintf(stderr, "fuzz-virtio: could not set the devices up\n");
            abort();
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(stats_at_exit);
        }
    }
    g_st.inputs++;
    reset_devices();
    while (in.n != 0u) {
        event(&in);
    }
    return 0;
}
