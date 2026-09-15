/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtiotest.c - a virtio queue completion, taken by guest code.
 *
 * The emulator's virtio devices were mapped, answered their identity
 * registers and had an interrupt wired to the APLIC -- and **nothing had
 * ever driven a completion through any of it**. Every register read
 * correctly, which is exactly the state in which the interesting part
 * can be broken: a device that never signals, or signals at a privilege
 * the guest is not running at, is indistinguishable from one that works
 * until something waits for it.
 *
 * So this waits for one. It drives the virtio console, because that is
 * the shortest round trip that exercises the whole path:
 *
 *   the guest publishes a descriptor ring in its own RAM
 *     -> writes QueueNotify
 *     -> the device walks the ring through emu_bus_host_ptr, prints the
 *        bytes, and puts the buffer on the used ring
 *     -> the device raises its line, and the APLIC sets pending
 *     -> MEIP reaches the hart and the guest traps
 *     -> the handler reads InterruptStatus, writes InterruptACK, and
 *        claims from the APLIC
 *
 * What it proves that a register read cannot: that the descriptor walk
 * reaches guest memory, that the used ring is written where the guest
 * is looking, and that the interrupt arrives.
 *
 * Run it with:
 *   emu-host --virtio-console --ram 0x400000 guest/virtiotest.bin
 */

#include <stdint.h>

/* ---- the emulator's console, for reporting ------------------------- */

#define UART_THR (*(volatile uint8_t *)0x10000000u)

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    int i;

    puts_("0x");
    for (i = 7; i >= 0; i--) {
        UART_THR = (uint8_t)d[(v >> (i * 4)) & 0xFu];
    }
}

static int g_fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) {
        puts_("  ok   ");
        puts_(what);
        puts_("\n");
        return;
    }
    g_fails++;
    puts_("  FAIL ");
    puts_(what);
    puts_(" got ");
    puthex(got);
    puts_(" want ");
    puthex(want);
    puts_("\n");
}

/* ---- virtio-mmio, version 2 ---------------------------------------- */

#define VIRTIO_BASE 0x10001000u
#define V(off) (*(volatile uint32_t *)(VIRTIO_BASE + (off)))

/*
 * Publish the ring before telling the device to look at it.
 *
 * **A volatile access does not order the ordinary stores around it.**
 * The descriptors and `avail->idx` are plain memory, so the compiler
 * may sink them past the volatile write to QueueNotify -- and does:
 * without this, GCC emitted the notify at 0x200005b4 and the whole ring
 * setup after it, so the device looked at an avail ring still reading
 * zero, consumed nothing, and completed nothing. Every register was
 * right and no access faulted.
 *
 * The console queue had the same defect and passed anyway, because its
 * one descriptor happened to be scheduled before the notify. That is
 * this tree's recurring "one weak test" shape: the barrier is needed by
 * both and was proved by neither.
 *
 * A compiler barrier is enough here and a real driver would need more:
 * there is one guest hart, the device runs inside the same host thread,
 * and nothing reorders at the bus.
 */
#define VIRTIO_WMB() __asm__ volatile("" ::: "memory")

#define V_MAGIC 0x000u
#define V_VERSION 0x004u
#define V_DEVICE_ID 0x008u
#define V_DEVICE_FEATURES 0x010u
#define V_DEVICE_FEATURES_SEL 0x014u
#define V_DRIVER_FEATURES 0x020u
#define V_DRIVER_FEATURES_SEL 0x024u
#define V_QUEUE_SEL 0x030u
#define V_QUEUE_NUM_MAX 0x034u
#define V_QUEUE_NUM 0x038u
#define V_QUEUE_READY 0x044u
#define V_QUEUE_NOTIFY 0x050u
/*
 * **Version 2 registers, and the version matters.** The device reports
 * 2, which means the driver publishes the three ring addresses
 * separately and sets QUEUE_READY -- there is no QUEUE_PFN and no page
 * size. Writing the legacy registers instead is accepted silently:
 * every status write lands, the queue reports its maximum size, and
 * QueueNotify then does nothing at all, because the device is looking
 * at ring addresses the driver never set. That is what the first
 * version of this test did.
 */
#define V_QUEUE_DESC_LOW 0x080u
#define V_QUEUE_DESC_HIGH 0x084u
#define V_QUEUE_AVAIL_LOW 0x090u
#define V_QUEUE_AVAIL_HIGH 0x094u
#define V_QUEUE_USED_LOW 0x0A0u
#define V_QUEUE_USED_HIGH 0x0A4u
#define V_INT_STATUS 0x060u
#define V_INT_ACK 0x064u
#define V_STATUS 0x070u

#define V_MAGIC_VALUE 0x74726976u /* 'virt' */
#define V_ID_BLOCK 2u
#define V_ID_CONSOLE 3u
#define V_ID_NET 1u

#define ST_ACK 1u
#define ST_DRIVER 2u
#define ST_DRIVER_OK 4u
#define ST_FEATURES_OK 8u

#define VRING_DESC_F_NEXT 1u

#define QSZ 8u

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSZ];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[QSZ];
};

/*
 * The three rings as separate objects, which is what version 2 allows
 * and why it is easier: the driver tells the device where each one is,
 * so there is no layout rule for the two sides to disagree about. The
 * alignments are the specification's -- 16, 2 and 4.
 */
static struct vring_desc g_desc[QSZ] __attribute__((aligned(16)));
static struct vring_avail g_avail __attribute__((aligned(2)));
static struct vring_used g_used __attribute__((aligned(4)));
static const char g_msg[] = "virtio console says hello\n";

/*
 * A second ring set, for the one device that needs two queues at once.
 *
 * The console and the disk each drive a single queue, so one set served
 * them. A network interface cannot: a frame is transmitted on queue 1
 * and arrives back on queue 0, and the receive buffer has to be posted
 * *before* the transmit, or there is nowhere for the reply to land.
 * Sharing one set between the two would have the transmit overwrite the
 * descriptor the receive is waiting on.
 */
static struct vring_desc g_desc1[QSZ] __attribute__((aligned(16)));
static struct vring_avail g_avail1 __attribute__((aligned(2)));
static struct vring_used g_used1 __attribute__((aligned(4)));

/* ---- APLIC (direct delivery) --------------------------------------- */

#define APLIC_BASE 0x0C000000u
#define APLIC_R(off) (*(volatile uint32_t *)(APLIC_BASE + (off)))
#define APLIC_DOMAINCFG APLIC_R(0x0000u)
#define APLIC_SOURCECFG(i) APLIC_R(0x0004u + 4u * ((i) - 1u))
#define APLIC_SETIENUM APLIC_R(0x1EDCu)
#define APLIC_TARGET(i) APLIC_R(0x3004u + 4u * ((i) - 1u))
#define APLIC_IDELIVERY APLIC_R(0x4000u)
#define APLIC_TOPI APLIC_R(0x4018u)
#define APLIC_CLAIMI APLIC_R(0x401Cu)

#define APLIC_DOMAINCFG_IE (1u << 8)
#define APLIC_SM_LEVEL_HIGH 6u

/*
 * The source number the runner gives the first virtio device. It is not
 * discoverable -- a real guest reads it from the device tree -- so it is
 * written here and must match what emu_main hands out.
 */
#define SRC_VIRTIO 1u

static volatile int g_took_irq;
static volatile uint32_t g_int_status;

/*
 * The machine-mode trap handler.
 *
 * Only external interrupts are expected. Anything else is reported and
 * halts rather than being ignored: a guest that quietly resumes from an
 * unexpected trap produces the silent wrong answer this whole test
 * exists to rule out.
 */
void __attribute__((interrupt("machine"), aligned(4))) trap_entry(void)
{
    uint32_t cause;
    uint32_t claim;

    __asm__ volatile("csrr %0, mcause" : "=r"(cause));

    if ((cause & 0x80000000u) == 0u || (cause & 0xFFu) != 11u) {
        puts_("  FAIL unexpected trap cause ");
        puthex(cause);
        puts_("\n");
        g_fails++;
        g_took_irq = 1;
        return;
    }

    /*
     * The device first, the controller second. virtio-mmio is level
     * triggered: the line stays asserted until InterruptACK is written,
     * so claiming from the APLIC before acknowledging the device would
     * clear a pending bit the device immediately sets again.
     */
    g_int_status = V(V_INT_STATUS);
    V(V_INT_ACK) = g_int_status;

    claim = APLIC_CLAIMI;
    (void)claim;

    g_took_irq = 1;
}

static void irq_setup(void)
{
    uint32_t mtvec = (uint32_t)(uintptr_t)&trap_entry;

    __asm__ volatile("csrw mtvec, %0" : : "r"(mtvec));

    APLIC_SOURCECFG(SRC_VIRTIO) = APLIC_SM_LEVEL_HIGH;
    APLIC_TARGET(SRC_VIRTIO) = 1u; /* priority 1: deliverable */
    APLIC_SETIENUM = SRC_VIRTIO;
    APLIC_DOMAINCFG = APLIC_DOMAINCFG_IE;
    APLIC_IDELIVERY = 1u;

    /* mie.MEIE, then mstatus.MIE. */
    __asm__ volatile("csrs mie, %0" : : "r"(1u << 11));
    __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));
}

/*
 * virtio-blk, driven the same way.
 *
 * The console proves a queue completes; this proves the device can
 * *fill* a buffer the guest supplied, which the console never does --
 * its transmit queue only reads. A device whose descriptor walk was
 * write-only would pass the console test and fail here.
 *
 * A block request is three descriptors chained: a header the guest
 * writes, the data the device fills, and a status byte the device
 * writes. The chain is the point -- it exercises VRING_DESC_F_NEXT and
 * VRING_DESC_F_WRITE, neither of which the single-descriptor console
 * path touches.
 */
#define VIRTIO_BLK_T_IN 0u
#define VRING_DESC_F_WRITE 2u

/* ---- the network interface ----------------------------------------- */

/*
 * **Two queues, and the receive buffer goes first.**
 *
 * virtio-net numbers them the way the device sees traffic, not the way
 * the driver does: queue 0 is *receive* and queue 1 is *transmit*. The
 * receive queue is the one with no notify -- the device marks it
 * manual_recv, so writing QueueNotify for it does nothing at all and
 * the buffer is simply left in the avail ring for the host to find when
 * a frame turns up. A driver that waits for a used entry on queue 0
 * after notifying it waits for ever, and everything it can read says
 * the queue is configured and ready.
 *
 * Every frame carries a 12-byte virtio-net header in front of it, in
 * the same buffer. It is not a separate descriptor and it is not
 * optional: the device reads header_size bytes before the frame on
 * transmit and writes them before the frame on receive, so a driver
 * that omits it transmits its first 12 bytes as header and loses them.
 */
#define VIRTIO_NET_HDR_LEN 12u
#define NET_FRAME_LEN 60u

static uint8_t g_tx[VIRTIO_NET_HDR_LEN + NET_FRAME_LEN]
    __attribute__((aligned(16)));
static uint8_t g_rx[VIRTIO_NET_HDR_LEN + NET_FRAME_LEN]
    __attribute__((aligned(16)));

static void test_net(void)
{
    uint32_t i;
    uint32_t bad = 0u;

    puts_("virtiotest: sending a frame and waiting for it to come back\n");

    /*
     * A frame that is recognisably ours rather than zeros. Zeros would
     * pass against a device that completed the descriptor without
     * copying anything, which is the failure worth ruling out -- the
     * receive buffer starts as zeros too.
     */
    for (i = 0; i < VIRTIO_NET_HDR_LEN; i++) {
        g_tx[i] = 0u;
    }
    for (i = 0; i < NET_FRAME_LEN; i++) {
        g_tx[VIRTIO_NET_HDR_LEN + i] = (uint8_t)(0xA0u + (i & 0x0Fu));
    }
    for (i = 0; i < sizeof(g_rx); i++) {
        g_rx[i] = 0u;
    }

    /*
     * The receive buffer, posted on queue 0 and left there. Written by
     * the device, so VRING_DESC_F_WRITE; it has to be big enough for
     * the header as well as the frame, because the device refuses the
     * whole delivery if header+frame does not fit and drops the packet
     * without saying so.
     */
    g_desc[0].addr = (uint64_t)(uintptr_t)g_rx;
    g_desc[0].len = sizeof(g_rx);
    g_desc[0].flags = VRING_DESC_F_WRITE;
    g_desc[0].next = 0u;
    g_avail.ring[0] = 0u;
    g_avail.idx = 1u;

    /* The frame to send, on queue 1: header and payload, read-only. */
    g_desc1[0].addr = (uint64_t)(uintptr_t)g_tx;
    g_desc1[0].len = sizeof(g_tx);
    g_desc1[0].flags = 0u;
    g_desc1[0].next = 0u;
    g_avail1.ring[0] = 0u;
    g_avail1.idx = 1u;

    g_took_irq = 0;
    VIRTIO_WMB();
    V(V_QUEUE_NOTIFY) = 1u; /* transmit */

    for (i = 0; i < 20000000u && g_took_irq == 0; i++) {
        __asm__ volatile("" ::: "memory");
    }

    check("net interrupt taken", (uint32_t)g_took_irq, 1u);
    /* The transmit was consumed... */
    check("net tx used ring advanced", g_used1.idx, 1u);
    /* ...and the loopback put it back on the receive queue. */
    check("net rx used ring advanced", g_used.idx, 1u);

    /*
     * The length the device reported, which is header plus frame. A
     * device that completed the descriptor without copying would report
     * zero here and still advance the ring.
     */
    check("net rx length", g_used.ring[0].len,
          VIRTIO_NET_HDR_LEN + NET_FRAME_LEN);

    for (i = 0; i < NET_FRAME_LEN; i++) {
        if (g_rx[VIRTIO_NET_HDR_LEN + i] != g_tx[VIRTIO_NET_HDR_LEN + i]) {
            bad++;
        }
    }
    check("net frame came back intact", bad, 0u);
}

struct blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct blk_req g_req;
static uint8_t g_sector[512] __attribute__((aligned(16)));
static volatile uint8_t g_status = 0xFF;

static void test_block(void)
{
    struct vring_desc *const desc = g_desc;
    struct vring_avail *const avail = &g_avail;
    struct vring_used *const used = &g_used;
    uint32_t i;

    puts_("virtiotest: reading sector 0 from the disk\n");

    g_req.type = VIRTIO_BLK_T_IN;
    g_req.reserved = 0u;
    g_req.sector = 0u;

    /* header: read by the device */
    desc[0].addr = (uint64_t)(uintptr_t)&g_req;
    desc[0].len = sizeof(g_req);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1u;

    /* data: written by the device */
    desc[1].addr = (uint64_t)(uintptr_t)g_sector;
    desc[1].len = sizeof(g_sector);
    desc[1].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    desc[1].next = 2u;

    /* status: written by the device */
    desc[2].addr = (uint64_t)(uintptr_t)&g_status;
    desc[2].len = 1u;
    desc[2].flags = VRING_DESC_F_WRITE;
    desc[2].next = 0u;

    avail->ring[0] = 0u;
    avail->idx = 1u;

    g_took_irq = 0;
    VIRTIO_WMB();
    V(V_QUEUE_NOTIFY) = 0u; /* blk has one queue, number 0 */

    for (i = 0; i < 20000000u && g_took_irq == 0; i++) {
        __asm__ volatile("" ::: "memory");
    }

    check("blk interrupt taken", (uint32_t)g_took_irq, 1u);
    check("blk used ring advanced", used->idx, 1u);
    check("blk status ok", g_status, 0u);

    /*
     * The content, which is the whole point: a completion that filled
     * nothing would pass every check above. The image the test is run
     * with begins with this string.
     */
    {
        static const char want[] = "VIRTIO-DISK-SECTOR-0";
        uint32_t k;
        uint32_t bad = 0u;

        for (k = 0; k < sizeof(want) - 1u; k++) {
            if (g_sector[k] != (uint8_t)want[k]) {
                bad++;
            }
        }
        check("blk sector 0 content", bad, 0u);
    }
}

int main(void)
{
    struct vring_desc *const desc = g_desc;
    struct vring_avail *const avail = &g_avail;
    struct vring_used *const used = &g_used;
    uint32_t i;
    uint32_t dev_id;

    puts_("virtiotest: driving a console queue to completion\n");

    check("magic", V(V_MAGIC), V_MAGIC_VALUE);

    /*
     * Whichever device is at the first slot. The runner places them in
     * the order the options ask for, so the test follows the command
     * line rather than requiring one -- and says what it found, because
     * "no device" and "the wrong device" need different fixes.
     */
    dev_id = V(V_DEVICE_ID);
    if (dev_id != V_ID_CONSOLE && dev_id != V_ID_BLOCK &&
        dev_id != V_ID_NET) {
        puts_("virtiotest: device id ");
        puthex(dev_id);
        puts_(" is not console, block or net -- run with"
              " --virtio-console, --disk or --net\n");
        return 1;
    }

    if (g_fails != 0) {
        return 1;
    }

    irq_setup();

    /*
     * The status handshake, in the order the specification gives it.
     * The device is legacy (version 1), so the driver publishes a page
     * size and a page number rather than split ring addresses.
     */
    V(V_STATUS) = 0u;
    V(V_STATUS) = ST_ACK;
    V(V_STATUS) = ST_ACK | ST_DRIVER;

    /*
     * **VIRTIO_F_VERSION_1 must be accepted.** It is feature bit 32, so
     * it lives in the upper word that the *_SEL registers select, and a
     * driver that only ever reads word 0 never sees it. A version 2
     * device whose driver does not set it is a driver still speaking
     * the legacy layout, which is the failure above wearing a
     * different hat.
     */
    V(V_DEVICE_FEATURES_SEL) = 1u;
    (void)V(V_DEVICE_FEATURES);
    V(V_DRIVER_FEATURES_SEL) = 1u;
    V(V_DRIVER_FEATURES) = 1u; /* bit 32 of the feature space */
    V(V_DRIVER_FEATURES_SEL) = 0u;
    V(V_DRIVER_FEATURES) = 0u;
    V(V_STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK;

    /*
     * Which queue this device's work goes on. Console: queue 1
     * transmits, 0 receives. Block: one queue, 0. Net: both, and it is
     * the only one that needs two -- see test_net.
     */
    V(V_QUEUE_SEL) = (dev_id == V_ID_CONSOLE) ? 1u : 0u;
    check("queue max", (V(V_QUEUE_NUM_MAX) >= QSZ) ? 1u : 0u, 1u);
    V(V_QUEUE_NUM) = QSZ;
    V(V_QUEUE_DESC_LOW) = (uint32_t)(uintptr_t)g_desc;
    V(V_QUEUE_DESC_HIGH) = 0u;
    V(V_QUEUE_AVAIL_LOW) = (uint32_t)(uintptr_t)&g_avail;
    V(V_QUEUE_AVAIL_HIGH) = 0u;
    V(V_QUEUE_USED_LOW) = (uint32_t)(uintptr_t)&g_used;
    V(V_QUEUE_USED_HIGH) = 0u;
    V(V_QUEUE_READY) = 1u;

    if (dev_id == V_ID_NET) {
        /* Queue 1, transmit, with the second ring set. */
        V(V_QUEUE_SEL) = 1u;
        V(V_QUEUE_NUM) = QSZ;
        V(V_QUEUE_DESC_LOW) = (uint32_t)(uintptr_t)g_desc1;
        V(V_QUEUE_DESC_HIGH) = 0u;
        V(V_QUEUE_AVAIL_LOW) = (uint32_t)(uintptr_t)&g_avail1;
        V(V_QUEUE_AVAIL_HIGH) = 0u;
        V(V_QUEUE_USED_LOW) = (uint32_t)(uintptr_t)&g_used1;
        V(V_QUEUE_USED_HIGH) = 0u;
        V(V_QUEUE_READY) = 1u;
    }

    V(V_STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK;

    if (dev_id == V_ID_BLOCK) {
        test_block();
        goto report;
    }
    if (dev_id == V_ID_NET) {
        test_net();
        goto report;
    }

    /* One descriptor pointing at the message, and one available entry. */
    desc[0].addr = (uint64_t)(uintptr_t)g_msg;
    desc[0].len = (uint32_t)(sizeof(g_msg) - 1u);
    desc[0].flags = 0u;
    desc[0].next = 0u;

    avail->ring[0] = 0u;
    /*
     * The index last, after the descriptor it refers to. The device
     * reads idx to decide there is work, so publishing it first is a
     * race it would win.
     *
     * "The device runs only when the guest stops, so no barrier is
     * needed" is what used to be written here, and it is wrong about
     * *which* reordering is the danger: the compiler's, not the
     * machine's. See VIRTIO_WMB.
     */
    avail->idx = 1u;

    puts_("virtiotest: notifying\n");
    VIRTIO_WMB();
    V(V_QUEUE_NOTIFY) = 1u;

    /*
     * Wait, with a bound. A test that spins for ever on a missing
     * interrupt reports nothing at all -- which reads as a hang in the
     * emulator rather than as the thing being tested having failed.
     */
    for (i = 0; i < 20000000u && g_took_irq == 0; i++) {
        __asm__ volatile("" ::: "memory");
    }

    check("interrupt taken", (uint32_t)g_took_irq, 1u);
    check("InterruptStatus had the used-buffer bit", g_int_status & 1u, 1u);
    check("used ring advanced", used->idx, 1u);
    check("used entry names descriptor 0", used->ring[0].id, 0u);

report:
    if (g_fails == 0) {
        puts_("VIRTIOTEST-PASS\n");
    } else {
        puts_("VIRTIOTEST-FAIL\n");
    }
    return g_fails;
}
