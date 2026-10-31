/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_gdb.c - the gdb stub, fed what a TCP peer can send.
 *
 * On a board with the network up the stub listens on port 1234, and
 * whatever connects is parsed by src/emu/emu_gdb.c. This feeds it a
 * stream of packets -- framed with a correct checksum, or not, escaped
 * as gdb escapes them, or not, up to past the receive buffer, with raw
 * bytes and ^C between -- and lets the guest run where a packet says
 * continue or step. It is configured as a board's stub is: a real
 * frontend's core and target description (the first input byte picks
 * the frontend), and flash ops, which is what makes it advertise and
 * serve the memory map and vFlash*.
 *
 * Held to:
 *
 *   - ASan and UBSan, which see the stub's stack buffers;
 *   - every reply framed: '+', '-', or $<body>#<sum> with the right sum,
 *     a body no longer than the PacketSize it advertises and with no
 *     unescaped '$' or '#';
 *   - a packet of L bytes writing at most L bytes of guest memory (a
 *     device stands in for memory here and counts what is written to
 *     it), and a flash write handed no more than the packet carried.
 *
 * Packets begin with a command from a table -- the stub's own, and
 * gdb's -- since a checksum and a command letter are two things the
 * fuzzer would otherwise take a long time to agree on.
 *
 * FUZZ_STATS=1 prints what the run covered at exit; FUZZ_SHOW=1 prints
 * each packet and reply.
 */

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"
#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define ROM_BASE EMU_GUEST_ROM_BASE
#define ROM_SIZE 0x4000u
#define RAM_BASE EMU_GUEST_RAM_BASE
#define RAM_SIZE 0x10000u
#define WATCH_BASE 0x30000000u /* memory gdb may write, counted */
#define WATCH_SIZE 0x1000u

static uint8_t g_rom[ROM_SIZE];
static uint8_t g_ram[RAM_SIZE];
static uint8_t g_watch[WATCH_SIZE];
static uint32_t g_watch_writes;

static emu_fault_t watch_read(void *ctx, uint32_t off, uint32_t size,
                              uint32_t *out)
{
    (void)ctx;
    *out = 0u;
    for (uint32_t i = 0u; i < size && off + i < WATCH_SIZE; i++) {
        *out |= (uint32_t)g_watch[off + i] << (8u * i);
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t watch_write(void *ctx, uint32_t off, uint32_t size,
                               uint32_t val)
{
    (void)ctx;
    for (uint32_t i = 0u; i < size && off + i < WATCH_SIZE; i++) {
        g_watch[off + i] = (uint8_t)(val >> (8u * i));
    }
    g_watch_writes += size;
    return EMU_FAULT_NONE;
}

static const emu_dev_ops_t k_watch = {watch_read, watch_write, NULL};

/* Flash ops that keep what they are given, inside their window. */
#define FLASH_SIZE 0x10000u

static uint8_t g_flash[FLASH_SIZE];
static uint32_t g_flash_max; /* the most one write may carry */

static bool f_erase(uint32_t addr, uint32_t len)
{
    (void)len;
    return addr >= ROM_BASE && addr - ROM_BASE < FLASH_SIZE;
}

static bool f_write(uint32_t addr, const void *data, uint32_t len)
{
    if (len > g_flash_max) {
        fprintf(stderr, "fuzz-gdb: vFlashWrite of %u bytes from a packet of %u\n",
                (unsigned)len, (unsigned)g_flash_max);
        abort();
    }
    if (addr < ROM_BASE || addr - ROM_BASE > FLASH_SIZE ||
        len > FLASH_SIZE - (addr - ROM_BASE)) {
        return false;
    }
    memcpy(&g_flash[addr - ROM_BASE], data, len);
    return true;
}

static bool f_done(void)
{
    return true;
}

static uint32_t f_size(void)
{
    return FLASH_SIZE;
}

static const emu_gdb_flash_ops_t k_flash = {f_erase, f_write, f_done,
                                            ROM_BASE, f_size};

/* ------------------------------------------------------------------ */
/* The wire                                                            */
/* ------------------------------------------------------------------ */

static uint8_t g_out[1u << 16];
static uint32_t g_out_len;
static bool g_show;

static void tx(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (g_out_len + len > sizeof(g_out)) {
        fprintf(stderr, "fuzz-gdb: %u bytes of reply to one packet\n",
                (unsigned)(g_out_len + len));
        abort();
    }
    memcpy(&g_out[g_out_len], data, len);
    g_out_len += len;
}

static int hexv(uint8_t c)
{
    return (c >= '0' && c <= '9')   ? c - '0'
           : (c >= 'a' && c <= 'f') ? c - 'a' + 10
           : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                    : -1;
}

static struct {
    uint64_t inputs, packets, replies, bad_sums, long_packets, ctrl_c, runs,
        steps, watched;
} g_st;

static void stats_at_exit(void)
{
    fprintf(stderr,
            "fuzz-gdb: %llu inputs, %llu packets (%llu bad checksums, %llu "
            "past the buffer, %llu ^C), %llu replies; %llu runs (%llu "
            "stepping); %llu packets wrote memory\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.packets,
            (unsigned long long)g_st.bad_sums,
            (unsigned long long)g_st.long_packets,
            (unsigned long long)g_st.ctrl_c, (unsigned long long)g_st.replies,
            (unsigned long long)g_st.runs, (unsigned long long)g_st.steps,
            (unsigned long long)g_st.watched);
}

/* Everything sent since the last check is '+', '-' or a whole packet. */
static void check_replies(void)
{
    uint32_t i = 0u;

    while (i < g_out_len) {
        const uint8_t c = g_out[i];

        if (c == '+' || c == '-') {
            i++;
            continue;
        }
        if (c != '$') {
            fprintf(stderr, "fuzz-gdb: reply byte 0x%02x outside a packet\n",
                    c);
            abort();
        }
        uint32_t j = i + 1u;
        uint8_t sum = 0u;

        while (j < g_out_len && g_out[j] != '#') {
            if (g_out[j] == '$') {
                fprintf(stderr, "fuzz-gdb: '$' inside a reply\n");
                abort();
            }
            sum = (uint8_t)(sum + g_out[j]);
            j++;
        }
        if (j + 2u >= g_out_len) {
            fprintf(stderr, "fuzz-gdb: a reply with no checksum\n");
            abort();
        }
        if (j - i - 1u > EMU_GDB_MAX_PACKET) {
            fprintf(stderr, "fuzz-gdb: a reply of %u bytes, past PacketSize\n",
                    (unsigned)(j - i - 1u));
            abort();
        }
        if (hexv(g_out[j + 1u]) < 0 || hexv(g_out[j + 2u]) < 0 ||
            (uint8_t)((hexv(g_out[j + 1u]) << 4) | hexv(g_out[j + 2u])) !=
                sum) {
            fprintf(stderr, "fuzz-gdb: a reply with the wrong checksum\n");
            abort();
        }
        if (g_show) {
            fprintf(stderr, "  <- %.*s\n", (int)(j - i - 1u),
                    (const char *)&g_out[i + 1u]);
        }
        g_st.replies++;
        i = j + 3u;
    }
    g_out_len = 0u;
}

/* ------------------------------------------------------------------ */
/* Packets                                                             */
/* ------------------------------------------------------------------ */

static const char *const k_cmds[] = {
    "",
    "?",
    "g",
    "G",
    "p",
    "P",
    "m",
    "M",
    "X",
    "m30000000,",
    "M30000000,",
    "X30000000,",
    "m20000000,",
    "M80000000,",
    "X80000000,",
    "Z0,",
    "z0,",
    "Z1,",
    "Z2,",
    "c",
    "s",
    "D",
    "k",
    "H",
    "T",
    "qSupported",
    "qSupported:multiprocess+;swbreak+;hwbreak+;xmlRegisters=i386",
    "qXfer:features:read:target.xml:",
    "qXfer:memory-map:read::",
    "qAttached",
    "qC",
    "qfThreadInfo",
    "qsThreadInfo",
    "QStartNoAckMode",
    "vFlashErase:",
    "vFlashErase:20000000,",
    "vFlashWrite:",
    "vFlashWrite:20000000:",
    "vFlashDone",
    "vCont?",
    "vCont;s",
    "vCont;c",
};

#define NCMDS (sizeof(k_cmds) / sizeof(k_cmds[0]))

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

static emu_gdb_t g_gdb;

/*
 * One packet: a command, then `extra` bytes of the input, cycled out to
 * `len` when a longer packet is wanted; escaped as gdb escapes binary
 * data unless `raw`; with the right checksum unless `bad_sum`.
 */
static void send_packet(in_t *in, uint32_t len_wanted, bool raw, bool bad_sum)
{
    static uint8_t body[2048], wire[4200];
    const char *const cmd = k_cmds[take(in, 1u) % NCMDS];
    const uint32_t extra = take(in, 1u);
    uint32_t n = (uint32_t)strlen(cmd), w = 0u;
    uint8_t sum = 0u;

    memcpy(body, cmd, n);
    {
        const uint8_t *const src = in->p;
        const uint32_t have = (uint32_t)(extra < in->n ? extra : in->n);

        for (uint32_t i = 0u; i < have; i++) {
            body[n++] = src[i];
        }
        in->p += have;
        in->n -= have;
        while (have != 0u && n < len_wanted && n < sizeof(body)) {
            body[n] = body[n - have];
            n++;
        }
    }
    wire[w++] = '$';
    for (uint32_t i = 0u; i < n; i++) {
        const uint8_t c = body[i];

        if (!raw && (c == '$' || c == '#' || c == '}' || c == '*')) {
            wire[w++] = 0x7Du;
            sum = (uint8_t)(sum + 0x7Du);
            wire[w++] = (uint8_t)(c ^ 0x20u);
            sum = (uint8_t)(sum + (uint8_t)(c ^ 0x20u));
        } else {
            wire[w++] = c;
            sum = (uint8_t)(sum + c);
        }
    }
    if (bad_sum) {
        sum = (uint8_t)~sum;
    }
    wire[w++] = '#';
    wire[w++] = (uint8_t)"0123456789abcdef"[sum >> 4];
    wire[w++] = (uint8_t)"0123456789abcdef"[sum & 15u];

    g_st.packets++;
    g_st.bad_sums += bad_sum;
    g_st.long_packets += n > EMU_GDB_MAX_PACKET;
    if (g_show) {
        fprintf(stderr, "-> %.*s%s\n", (int)(n < 200u ? n : 200u),
                (const char *)body, n < 200u ? "" : "...");
    }
    /* A packet's own bytes are all a write may use: M's hex is two a
     * byte, X's one, vFlashWrite's one. */
    g_watch_writes = 0u;
    g_flash_max = n;
    emu_gdb_rx(&g_gdb, wire, w);
    if (g_watch_writes > n) {
        fprintf(stderr, "fuzz-gdb: a packet of %u bytes wrote %u to memory\n",
                (unsigned)n, (unsigned)g_watch_writes);
        abort();
    }
    g_st.watched += g_watch_writes != 0u;
    check_replies();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    static emu_bus_t bus;
    static emu_system_t sys;
    static unsigned nfrontends;
    in_t in = {data, size};

    if (!once) {
        once = true;
        g_show = getenv("FUZZ_SHOW") != NULL;
        while (emu_frontends[nfrontends] != NULL) {
            nfrontends++;
        }
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(stats_at_exit);
        }
    }
    g_st.inputs++;

    const emu_cpu_ops_t *ops = NULL;
    for (unsigned k = 0u; k < nfrontends && ops == NULL; k++) {
        const emu_cpu_ops_t *const o =
            emu_frontends[(take(&in, 1u) + k) % nfrontends];

        if (o->gdb_target != NULL && o->gdb_target() != NULL) {
            ops = o;
        }
    }
    if (ops == NULL) {
        return 0;
    }

    memset(g_rom, 0, sizeof(g_rom));
    memset(g_ram, 0, sizeof(g_ram));
    memset(g_watch, 0, sizeof(g_watch));
    emu_bus_init(&bus);
    if (!emu_bus_add_rom(&bus, "rom", ROM_BASE, g_rom, ROM_SIZE) ||
        !emu_bus_add_ram(&bus, "ram", RAM_BASE, g_ram, RAM_SIZE) ||
        !emu_bus_add_mmio(&bus, "watch", WATCH_BASE, WATCH_SIZE, &k_watch,
                          NULL) ||
        !emu_system_open(&sys, ops, &bus, 1u)) {
        abort();
    }
    emu_system_reset(&sys, ROM_BASE);

    emu_gdb_init(&g_gdb, &sys.core[0], ops->gdb_target(), tx, NULL);
    emu_gdb_set_flash(&g_gdb, &k_flash);
    emu_gdb_attach(&g_gdb);
    check_replies();

    while (in.n != 0u) {
        const uint32_t k = take(&in, 1u) % 10u;

        switch (k) {
        case 0u:
        case 1u:
        case 2u:
        case 3u:
            send_packet(&in, 0u, false, false);
            break;
        case 4u: /* unescaped */
            send_packet(&in, 0u, true, false);
            break;
        case 5u: /* past the receive buffer, perhaps */
            send_packet(&in, take(&in, 2u) % 1400u, false, false);
            break;
        case 6u:
            send_packet(&in, 0u, false, true);
            break;
        case 7u: { /* bytes off the wire, framed or not */
            const uint32_t n = take(&in, 1u) % 32u;
            const uint32_t have = (uint32_t)(n < in.n ? n : in.n);

            emu_gdb_rx(&g_gdb, in.p, have);
            in.p += have;
            in.n -= have;
            check_replies();
            break;
        }
        case 8u:
            g_st.ctrl_c++;
            emu_gdb_rx(&g_gdb, (const uint8_t *)"\x03", 1u);
            check_replies();
            break;
        default: /* the run loop's turn */
            if (emu_gdb_attached(&g_gdb) && !emu_gdb_halted(&g_gdb)) {
                uint32_t retired = 0u;

                g_st.runs++;
                g_st.steps += g_gdb.stepping;
                (void)emu_gdb_run(&g_gdb, 1u + take(&in, 1u), &retired);
                check_replies();
            }
            break;
        }
    }
    return 0;
}
