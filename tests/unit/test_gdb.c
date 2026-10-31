/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_gdb.c - the stub, driven by RSP packets, with no ISA behind it.
 *
 * Two things only a packet can show:
 *
 *   - registers of different widths in one `g` packet. A target that is
 *     all one width cannot tell an offset computed as `regno * width`
 *     from the sum of the widths before it; this one has a 64-bit
 *     register between two 32-bit ones, which is ARMv7-M's VFP feature in
 *     miniature.
 *   - the memory map, built from the bus and the flash ops. It used to be
 *     a string in each target, and RV32's named flash where guest code
 *     had not lived since images moved to execute in place.
 *
 * gdb-check.sh runs a real gdb against each frontend on the host, where
 * there are no flash ops and so no map; this is the map's only test.
 */

#include "tests.h"

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* A target with a 64-bit register between two 32-bit ones             */
/* ------------------------------------------------------------------ */

static uint64_t g_regs[3];

static uint64_t t_get(const emu_cpu_t *cpu, unsigned n)
{
    (void)cpu;
    return (n < 3u) ? g_regs[n] : 0u;
}

static void t_set(emu_cpu_t *cpu, unsigned n, uint64_t v)
{
    (void)cpu;
    if (n < 3u) {
        g_regs[n] = (n == 1u) ? v : (uint32_t)v;
    }
}

static uint32_t t_pc_get(const emu_cpu_t *cpu)
{
    (void)cpu;
    return (uint32_t)g_regs[0];
}

static void t_pc_set(emu_cpu_t *cpu, uint32_t pc)
{
    (void)cpu;
    g_regs[0] = pc;
}

static const uint8_t k_widths[3] = {4u, 8u, 4u};

static const emu_gdb_target_t k_target = {
    .nregs = 3u,
    .reg_bytes = 4u,
    .reg_width = k_widths,
    .pc_regno = 0u,
    .reg_get = t_get,
    .reg_set = t_set,
    .pc_get = t_pc_get,
    .pc_set = t_pc_set,
    .stop_signal = 5,
    .arch = "test",
    .target_xml = NULL,
};

/* ------------------------------------------------------------------ */
/* Flash ops that accept nothing: only the window they describe matters */
/* ------------------------------------------------------------------ */

static bool f_erase(uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
    return false;
}

static bool f_write(uint32_t addr, const void *data, uint32_t len)
{
    (void)addr;
    (void)data;
    (void)len;
    return false;
}

static bool f_done(void)
{
    return false;
}

static uint32_t f_size(void)
{
    return 0x40000u;
}

static const emu_gdb_flash_ops_t k_flash = {
    f_erase, f_write, f_done, 0x20000000u, f_size,
};

/* ------------------------------------------------------------------ */
/* The wire                                                            */
/* ------------------------------------------------------------------ */

static char g_out[8192];
static uint32_t g_out_len;

static void tx(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    for (uint32_t i = 0; i < len && g_out_len + 1u < sizeof(g_out); i++) {
        g_out[g_out_len++] = (char)data[i];
    }
    g_out[g_out_len] = '\0';
}

/* Send one packet with its checksum, and return the body of the reply. */
static const char *rsp(emu_gdb_t *g, const char *body)
{
    static char reply[4096];
    char pkt[1024];
    unsigned sum = 0;
    size_t n = 0;

    for (const char *p = body; *p != '\0'; p++) {
        sum += (unsigned char)*p;
    }
    pkt[n++] = '$';
    for (const char *p = body; *p != '\0'; p++) {
        pkt[n++] = *p;
    }
    n += (size_t)snprintf(&pkt[n], sizeof(pkt) - n, "#%02x", sum & 0xFFu);
    g_out_len = 0;
    g_out[0] = '\0';
    emu_gdb_rx(g, (const uint8_t *)pkt, (uint32_t)n);

    const char *const s = strchr(g_out, '$');
    const char *const e = (s != NULL) ? strchr(s, '#') : NULL;

    reply[0] = '\0';
    if (s != NULL && e != NULL && (size_t)(e - s - 1) < sizeof(reply)) {
        memcpy(reply, s + 1, (size_t)(e - s - 1));
        reply[e - s - 1] = '\0';
    }
    return reply;
}

static void test_mixed_widths(emu_gdb_t *g)
{
    g_regs[0] = 0x11223344u;
    g_regs[1] = 0x8877665544332211ull;
    g_regs[2] = 0xAABBCCDDu;

    /* Little-endian bytes, each register at its own width: 8 + 16 + 8
     * digits. A stub that took every register as four bytes sends 24
     * and loses the top half of the second. */
    CHECK(strcmp(rsp(g, "g"), "44332211"
                              "1122334455667788"
                              "ddccbbaa") == 0);
    CHECK(strcmp(rsp(g, "p1"), "1122334455667788") == 0);
    CHECK(strcmp(rsp(g, "p2"), "ddccbbaa") == 0);

    /* G: the third register starts at digit 24, not 16. */
    CHECK(strcmp(rsp(g, "G"
                        "01000000"
                        "0807060504030201"
                        "0a000000"),
                 "OK") == 0);
    CHECK_EQ64(g_regs[0], 1u);
    CHECK_EQ64(g_regs[1], 0x0102030405060708ull);
    CHECK_EQ64(g_regs[2], 10u);

    CHECK(strcmp(rsp(g, "P1=ffeeddccbbaa9988"), "OK") == 0);
    CHECK_EQ64(g_regs[1], 0x8899AABBCCDDEEFFull);
}

static void test_memory_map(emu_gdb_t *g)
{
    static char xml[4096];
    size_t n = 0;

    CHECK(strstr(rsp(g, "qSupported"), "qXfer:memory-map:read+") != NULL);

    /* Read it whole, in the chunks gdb would ask for. */
    for (unsigned off = 0;;) {
        char req[64];
        const char *r;

        (void)snprintf(req, sizeof(req), "qXfer:memory-map:read::%x,100", off);
        r = rsp(g, req);
        if (r[0] != 'm' && r[0] != 'l') {
            CHECK(false);
            return;
        }
        const size_t len = strlen(r + 1);

        if (n + len >= sizeof(xml)) {
            CHECK(false);
            return;
        }
        memcpy(&xml[n], r + 1, len);
        n += len;
        off += (unsigned)len;
        if (r[0] == 'l') {
            break;
        }
    }
    xml[n] = '\0';

    /* The flash window, from the ops, with a block size. */
    CHECK(strstr(xml, "<memory type=\"flash\" start=\"0x20000000\" "
                      "length=\"0x00040000\"><property name=\"blocksize\">"
                      "0x1000</property></memory>") != NULL);
    /* The image's ROM region is inside it, so it is not listed again. */
    CHECK(strstr(xml, "type=\"rom\"") == NULL);
    /* RAM, and the device -- readable by a debugger. */
    CHECK(strstr(xml, "<memory type=\"ram\" start=\"0x80000000\" "
                      "length=\"0x00010000\"/>") != NULL);
    CHECK(strstr(xml, "<memory type=\"ram\" start=\"0x10000000\" "
                      "length=\"0x00001000\"/>") != NULL);
    /* Ascending, though the regions were added in another order. */
    {
        const char *const dev = strstr(xml, "0x10000000");
        const char *const fl = strstr(xml, "0x20000000");
        const char *const ram = strstr(xml, "0x80000000");

        CHECK(dev != NULL && fl != NULL && ram != NULL && dev < fl && fl < ram);
    }
    CHECK(strstr(xml, "</memory-map>") != NULL);
}

static emu_fault_t dev_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    (void)ctx;
    (void)off;
    (void)size;
    *out = 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t dev_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    (void)ctx;
    (void)off;
    (void)size;
    (void)val;
    return EMU_FAULT_NONE;
}

/*
 * Lengths a packet states and does not carry. "M...,80000000:" doubled
 * its count past 32 bits and wrote what an earlier packet had left in
 * the buffer; "P1=11" read a 64-bit register's other fourteen digits
 * from there. Both are refused now, and the earlier packet that leaves
 * hex behind is part of each case.
 */
static void test_short_packets(emu_gdb_t *g, uint8_t *ram)
{
    char fill[600];

    memset(fill, 'a', sizeof(fill) - 1u);
    fill[0] = 'm';
    fill[sizeof(fill) - 1u] = '\0';
    ram[0x100] = 0x5A;

    (void)rsp(g, fill);
    CHECK(strcmp(rsp(g, "M80000100,80000000:"), "E01") == 0);
    CHECK_EQ(ram[0x100], 0x5Au); /* nothing written */
    (void)rsp(g, fill);
    CHECK(strcmp(rsp(g, "M80000100,2:aa"), "E01") == 0);
    CHECK(strcmp(rsp(g, "M80000100,1:a5"), "OK") == 0);
    CHECK_EQ(ram[0x100], 0xA5u);

    /* A read whose length doubles past 32 bits is capped, not trusted. */
    CHECK_EQ((uint32_t)strlen(rsp(g, "m80000000,80000080")),
             2u * (EMU_GDB_MAX_PACKET / 2u - 1u));

    g_regs[1] = 7u;
    (void)rsp(g, fill);
    CHECK(strcmp(rsp(g, "P1=11"), "E01") == 0);
    CHECK_EQ64(g_regs[1], 7u);
}

void test_gdb(void)
{
    static emu_bus_t bus;
    static uint8_t ram[0x10000];
    static const uint8_t rom[0x1000] = {0};
    static const emu_dev_ops_t dev = {dev_read, dev_write, NULL};
    static emu_gdb_t g;
    /* No hooks, but an ops table: a successful M write asks it for
     * invalidate, and no case before test_short_packets made one. */
    static const emu_cpu_ops_t ops = {0};
    emu_core_t core = {&ops, NULL, &bus};

    emu_bus_init(&bus);
    CHECK(emu_bus_add_ram(&bus, "ram", 0x80000000u, ram, sizeof(ram)));
    CHECK(emu_bus_add_rom(&bus, "image", 0x20000000u, rom, sizeof(rom)));
    CHECK(emu_bus_add_mmio(&bus, "dev", 0x10000000u, 0x1000u, &dev, NULL));

    emu_gdb_init(&g, &core, &k_target, tx, NULL);
    emu_gdb_set_flash(&g, &k_flash);
    emu_gdb_attach(&g);
    (void)rsp(&g, "QStartNoAckMode");

    test_mixed_widths(&g);
    test_memory_map(&g);
    test_short_packets(&g, ram);
}
