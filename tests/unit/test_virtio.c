/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_virtio.c - that a virtio-mmio device answers on the bus.
 *
 * The devices themselves are TinyEMU's and are not this project's to
 * test: virtio.c has been exercised by every guest TinyEMU has ever
 * run. What is ours is the **glue** -- four functions turning its host
 * interface into this emulator's bus -- and the way that fails is
 * silent: a device registered at the wrong width, or a read that
 * returns the wrong thing, produces a guest driver that probes and
 * finds nothing.
 *
 * So this checks the one thing a driver checks first, and the one thing
 * that proves the whole path: the magic value at offset 0. Reaching it
 * means cpu_register_device mapped the device, the bus routed the
 * access, the size_log2 conversion was right, and virtio.c's read
 * handler ran.
 */

#include "tests.h"

#include "emu/emu_bus.h"
#include "emu/emu_virtio.h"

#include <string.h>

/* Registers every virtio-mmio driver reads before anything else. */
#define VIRTIO_MMIO_MAGIC 0x000u
#define VIRTIO_MMIO_VERSION 0x004u
#define VIRTIO_MMIO_DEVICE_ID 0x008u
#define VIRTIO_MMIO_CONFIG 0x100u

#define VIRTIO_MAGIC_VALUE 0x74726976u /* 'virt', little-endian */
#define VIRTIO_ID_9P 9u
#define VIRTIO_ID_INPUT 18u

#define TEST_VIRTIO_BASE 0x10001000u

/* And the ones a driver writes to set up a queue (virtio 1.1, 4.2.2). */
#define VIRTIO_MMIO_QUEUE_SEL 0x030u
#define VIRTIO_MMIO_QUEUE_NUM 0x038u
#define VIRTIO_MMIO_QUEUE_READY 0x044u
#define VIRTIO_MMIO_STATUS 0x070u
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080u
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090u
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0A0u

/*
 * The bus needs RAM as well as the device: virtio walks its descriptor
 * rings through emu_bus_host_ptr, so a map with no host-backed RAM
 * cannot carry one at all -- and a device that came up regardless would
 * be the interesting kind of wrong.
 */
static uint8_t g_ram[4096];

static int g_irq_calls;

static void count_irq(void *ctx, int irq_num, int level)
{
    (void)ctx;
    (void)irq_num;
    (void)level;
    g_irq_calls++;
}

static void put16(uint32_t off, uint16_t v)
{
    g_ram[off] = (uint8_t)v;
    g_ram[off + 1u] = (uint8_t)(v >> 8);
}

static void put32(uint32_t off, uint32_t v)
{
    put16(off, (uint16_t)v);
    put16(off + 2u, (uint16_t)(v >> 16));
}

static void wr(emu_bus_t *bus, uint32_t addr, uint32_t v)
{
    CHECK_EQ(emu_bus_write(bus, addr, 4u, v), EMU_FAULT_NONE);
}

static uint32_t rd(emu_bus_t *bus, uint32_t addr)
{
    uint32_t v = 0xDEADBEEFu;

    CHECK_EQ(emu_bus_read(bus, addr, 4u, &v), EMU_FAULT_NONE);
    return v;
}

/*
 * A reload: a guest uploaded over TFTP or gdb replaces the running one.
 * emu_main_reload rebuilds the bus from empty, which forgets the
 * devices, and TinyEMU cannot free one to make it again -- so the same
 * devices have to go back on the new bus (emu_virtio_remap).
 *
 * Back, and **reset**: the previous guest's queues point into RAM the
 * new guest now owns. So the old guest here sets up the keyboard's
 * event queue with four buffers and takes a key press (two events,
 * checked to have arrived, so the queue is shown to work), leaving two
 * buffers posted. After the reload, a key press must write nothing
 * anywhere in RAM -- the case a frame or a key arriving between the
 * upload and the new driver's probe would be.
 */
static void test_virtio_reload(emu_bus_t *bus)
{
    static uint8_t before[sizeof(g_ram)];
    const uint32_t kb = TEST_VIRTIO_BASE + 0x1000u;
    const uint32_t ram = 0x80000000u;
    uint32_t v = 0;

    /* Descriptors at 0, avail at 0x100, used at 0x200, buffers at 0x400. */
    memset(g_ram, 0, sizeof(g_ram));
    for (uint32_t i = 0; i < 4u; i++) {
        put32(i * 16u, ram + 0x400u + i * 8u); /* addr, low half */
        put32(i * 16u + 8u, 8u);               /* len */
        put16(i * 16u + 12u, 2u);              /* VRING_DESC_F_WRITE */
        put16(0x104u + i * 2u, (uint16_t)i);   /* avail ring[i] */
    }
    put16(0x102u, 4u); /* avail idx: four buffers posted */

    wr(bus, kb + VIRTIO_MMIO_STATUS, 0x3u); /* ACKNOWLEDGE | DRIVER */
    wr(bus, kb + VIRTIO_MMIO_QUEUE_SEL, 0u);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_NUM, 16u);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_DESC_LOW, ram);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_AVAIL_LOW, ram + 0x100u);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_USED_LOW, ram + 0x200u);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_READY, 1u);
    wr(bus, kb + VIRTIO_MMIO_STATUS, 0xFu); /* ... | FEATURES_OK | DRIVER_OK */

    emu_virtio_key_event(true, 30u);
    CHECK_EQ(g_ram[0x400], 1u);  /* EV_KEY */
    CHECK_EQ(g_ram[0x402], 30u); /* KEY_A */
    CHECK_EQ(g_ram[0x404], 1u);  /* down */
    CHECK_EQ(g_ram[0x202], 2u);  /* used idx: KEY and SYN */

    /* The reload: the bus from empty, then RAM, as emu_main_reload. */
    emu_bus_init(bus);
    if (!emu_bus_add_ram(bus, "ram", ram, g_ram, sizeof(g_ram))) {
        CHECK(false);
        return;
    }
    /* That it forgot them, or the remap below would prove nothing. */
    CHECK(emu_bus_read(bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_MAGIC, 4u, &v) !=
          EMU_FAULT_NONE);

    CHECK(emu_virtio_remap(bus));

    /* All three back, each where it was. */
    CHECK_EQ(rd(bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_MAGIC),
             VIRTIO_MAGIC_VALUE);
    CHECK_EQ(rd(bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_DEVICE_ID), VIRTIO_ID_9P);
    CHECK_EQ(rd(bus, kb + VIRTIO_MMIO_DEVICE_ID), VIRTIO_ID_INPUT);
    CHECK_EQ(rd(bus, TEST_VIRTIO_BASE + 0x2000u + VIRTIO_MMIO_DEVICE_ID),
             VIRTIO_ID_INPUT);

    /* Reset: nothing the old driver wrote survives. */
    CHECK_EQ(rd(bus, kb + VIRTIO_MMIO_STATUS), 0u);
    wr(bus, kb + VIRTIO_MMIO_QUEUE_SEL, 0u);
    CHECK_EQ(rd(bus, kb + VIRTIO_MMIO_QUEUE_READY), 0u);
    CHECK_EQ(rd(bus, kb + VIRTIO_MMIO_QUEUE_DESC_LOW), 0u);

    /* And a key press after it reaches nothing: all of RAM compared. */
    memcpy(before, g_ram, sizeof(g_ram));
    emu_virtio_key_event(true, 31u);
    emu_virtio_key_event(false, 31u);
    CHECK(memcmp(before, g_ram, sizeof(g_ram)) == 0);
}

void test_virtio(void)
{
    emu_bus_t bus;
    uint32_t v = 0;

    emu_bus_init(&bus);
    if (!emu_bus_add_ram(&bus, "ram", 0x80000000u, g_ram, sizeof(g_ram))) {
        CHECK(false);
        return;
    }

    if (!emu_virtio_init(&bus, count_irq, NULL)) {
        CHECK(false);
        return;
    }

    /*
     * "." rather than a temporary directory: the 9p backend only has to
     * open the root for this test, and a path that always exists keeps
     * the test from failing for a reason that is not about virtio.
     */
    if (!emu_virtio_add_9p(TEST_VIRTIO_BASE, 1, "test", ".")) {
        CHECK(false);
        return;
    }
    CHECK_EQ(emu_virtio_count(), 1u);

    /*
     * The three registers a driver reads to decide whether anything is
     * there. Magic first, because it is the one that says the *glue* is
     * right -- a wrong address, a wrong width or a wrong size_log2 all
     * show up here as a value that is not 'virt'.
     */
    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_MAGIC, 4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, VIRTIO_MAGIC_VALUE);

    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_VERSION, 4u,
                          &v),
             EMU_FAULT_NONE);
    /* Legacy (1) or modern (2); either is a device, zero is not. */
    CHECK(v == 1u || v == 2u);

    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_DEVICE_ID, 4u,
                          &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, VIRTIO_ID_9P);

    /*
     * Narrow reads, which are **the case the adapter exists for**:
     * TinyEMU passes a log2 size and this bus passes a byte count, and
     * that conversion is the only arithmetic in the glue.
     *
     * They have to be aimed at the *config space*, not at the registers
     * above. Those are 32-bit only -- the specification says so and
     * virtio.c implements it by returning 0 for anything narrower -- so
     * a byte read of the magic proves nothing about the conversion and
     * would only be asserting TinyEMU's refusal.
     *
     * A 9p device's config space is the mount tag: a 16-bit length
     * followed by the characters. So this reads the length of "test" at
     * one width and its first character at another, and both go through
     * the size_log2 path.
     */
    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_CONFIG, 2u,
                          &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v & 0xFFFFu, 4u); /* strlen("test") */

    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + VIRTIO_MMIO_CONFIG + 2u,
                          1u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v & 0xFFu, (uint32_t)'t');

    /*
     * A keyboard and a mouse beside it, at the next addresses.
     *
     * Checked mostly for the *device id*: virtio's input device is id
     * 18 whatever it is configured as, so a keyboard and a mouse look
     * identical from outside and the thing that separates them is the
     * config space the driver reads afterwards. Getting the type
     * backwards would produce two devices that both enumerate and one
     * that reports the wrong events -- so what this pins is that two
     * distinct devices exist, at the addresses the device tree will
     * name.
     */
    if (!emu_virtio_add_keyboard(TEST_VIRTIO_BASE + 0x1000u, 2)) {
        CHECK(false);
        return;
    }
    if (!emu_virtio_add_mouse(TEST_VIRTIO_BASE + 0x2000u, 3)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(emu_virtio_count(), 3u);

    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + 0x1000u +
                                    VIRTIO_MMIO_DEVICE_ID,
                          4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, VIRTIO_ID_INPUT);

    CHECK_EQ(emu_bus_read(&bus, TEST_VIRTIO_BASE + 0x2000u +
                                    VIRTIO_MMIO_DEVICE_ID,
                          4u, &v),
             EMU_FAULT_NONE);
    CHECK_EQ(v, VIRTIO_ID_INPUT);

    /*
     * Posting with no driver attached must not fault.
     *
     * **This is the case that matters for the caller.** The SDL layer
     * posts every event to these unconditionally -- it does not ask
     * whether a device exists or whether a guest has configured one --
     * so an event arriving before any queue is set up is the normal
     * state of affairs for the whole of start-up, not an edge case.
     */
    emu_virtio_key_event(true, 30u);  /* 'a' */
    emu_virtio_key_event(false, 30u);
    emu_virtio_mouse_event(3, -2, 0, 1u);

    /*
     * Nothing has driven a queue, so nothing should have raised an
     * interrupt. Worth asserting rather than assuming: a device that
     * asserted its line at init would leave a level-triggered input
     * stuck high before the guest has even probed it.
     */
    CHECK_EQ((uint32_t)g_irq_calls, 0u);

    test_virtio_reload(&bus);
}
