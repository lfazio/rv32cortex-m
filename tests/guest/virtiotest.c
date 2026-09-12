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
#define V_ID_CONSOLE 3u

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

int main(void)
{
    struct vring_desc *const desc = g_desc;
    struct vring_avail *const avail = &g_avail;
    struct vring_used *const used = &g_used;
    uint32_t i;

    puts_("virtiotest: driving a console queue to completion\n");

    check("magic", V(V_MAGIC), V_MAGIC_VALUE);
    check("device id", V(V_DEVICE_ID), V_ID_CONSOLE);

    if (g_fails != 0) {
        puts_("virtiotest: no device -- run with --virtio-console\n");
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

    /* Queue 1 is the console's transmit queue; 0 is receive. */
    V(V_QUEUE_SEL) = 1u;
    check("queue max", (V(V_QUEUE_NUM_MAX) >= QSZ) ? 1u : 0u, 1u);
    V(V_QUEUE_NUM) = QSZ;
    V(V_QUEUE_DESC_LOW) = (uint32_t)(uintptr_t)g_desc;
    V(V_QUEUE_DESC_HIGH) = 0u;
    V(V_QUEUE_AVAIL_LOW) = (uint32_t)(uintptr_t)&g_avail;
    V(V_QUEUE_AVAIL_HIGH) = 0u;
    V(V_QUEUE_USED_LOW) = (uint32_t)(uintptr_t)&g_used;
    V(V_QUEUE_USED_HIGH) = 0u;
    V(V_QUEUE_READY) = 1u;
    V(V_STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK;

    /* One descriptor pointing at the message, and one available entry. */
    desc[0].addr = (uint64_t)(uintptr_t)g_msg;
    desc[0].len = (uint32_t)(sizeof(g_msg) - 1u);
    desc[0].flags = 0u;
    desc[0].next = 0u;

    avail->ring[0] = 0u;
    /*
     * The index last, after the descriptor it refers to. The device
     * reads idx to decide there is work, so publishing it first is a
     * race it would win -- and on a host that reorders, a barrier
     * belongs here. There is none on this emulator: the device runs
     * only when the guest stops, at QueueNotify below.
     */
    avail->idx = 1u;

    puts_("virtiotest: notifying\n");
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

    if (g_fails == 0) {
        puts_("VIRTIOTEST-PASS\n");
    } else {
        puts_("VIRTIOTEST-FAIL\n");
    }
    return g_fails;
}
