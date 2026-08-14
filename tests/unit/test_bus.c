/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_bus.c - Region lookup and access control.
 *
 * The passthrough window is what lets a guest fault reach real ARM
 * hardware, so the checks that stop it going somewhere it should not are
 * worth testing directly rather than only through a guest.
 */

#include "tests.h"

#include "emu/emu_bus.h"

static uint8_t ram_buf[256];
static uint8_t rom_buf[256];
static uint32_t passthru_target[16];


/*
 * Guest byte order.
 *
 * The whole difficulty is *which* region kinds it applies to. Byte order
 * describes how bytes in memory compose into a value, so RAM and ROM
 * reverse and MMIO and passthrough do not -- a device callback hands
 * back a value and a passthrough read takes a real register's value, and
 * neither composed any bytes.
 *
 * Both halves are checked here, because both failure modes are silent
 * and neither is a crash: swap everything and every peripheral register
 * reads byte-reversed, swap nothing and a big-endian guest's own data is
 * garbage.
 *
 * Compiled only when a big-endian frontend is in the build, because
 * otherwise emu_bus_set_big_endian has no flag to set -- and that is
 * itself the point of the #if: a little-endian build pays nothing.
 */
#if EMU_BUS_ANY_BE
static emu_fault_t be_dev_read(void *ctx, uint32_t off, uint32_t size,
                               uint32_t *out)
{
    (void)ctx; (void)off; (void)size;
    *out = 0x11223344u;
    return EMU_FAULT_NONE;
}

static uint32_t g_be_dev_wrote;

static emu_fault_t be_dev_write(void *ctx, uint32_t off, uint32_t size,
                                uint32_t val)
{
    (void)ctx; (void)off; (void)size;
    g_be_dev_wrote = val;
    return EMU_FAULT_NONE;
}

static const emu_dev_ops_t k_be_dev = { be_dev_read, be_dev_write, NULL };

static void test_bus_big_endian(void)
{
    static uint8_t ram[64];
    static volatile uint32_t periph = 0x11223344u;
    emu_bus_t bus;
    uint32_t v;

    emu_bus_init(&bus);
    CHECK(emu_bus_add_ram(&bus, "ram", 0x80000000u, ram, sizeof(ram)));
    CHECK(emu_bus_add_mmio(&bus, "dev", 0x10000000u, 0x100u, &k_be_dev, NULL));
    CHECK(emu_bus_add_passthru(&bus, "pt", 0x40000000u, 4u,
                               (uintptr_t)&periph, EMU_PERM_RW, EMU_WANY));

    /* Little-endian first, so the comparison below means something. */
    CHECK_EQ(emu_bus_write(&bus, 0x80000000u, 4u, 0x11223344u), EMU_FAULT_NONE);
    CHECK_EQ(ram[0], 0x44u);
    CHECK_EQ(ram[3], 0x11u);

    emu_bus_set_big_endian(&bus, true);

    /* --- RAM: bytes compose in the guest's order --- */
    CHECK_EQ(emu_bus_write(&bus, 0x80000000u, 4u, 0x11223344u), EMU_FAULT_NONE);
    CHECK_EQ(ram[0], 0x11u);            /* most significant byte first */
    CHECK_EQ(ram[3], 0x44u);
    CHECK_EQ(emu_bus_read(&bus, 0x80000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x11223344u);           /* and reads back as written   */

    /* Halfwords too, and bytes are unaffected by definition. */
    CHECK_EQ(emu_bus_write(&bus, 0x80000008u, 2u, 0xABCDu), EMU_FAULT_NONE);
    CHECK_EQ(ram[8], 0xABu);
    CHECK_EQ(ram[9], 0xCDu);
    CHECK_EQ(emu_bus_read(&bus, 0x80000008u, 2u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0xABCDu);
    CHECK_EQ(emu_bus_write(&bus, 0x8000000Cu, 1u, 0x5Au), EMU_FAULT_NONE);
    CHECK_EQ(ram[12], 0x5Au);

    /* --- MMIO: a value, not bytes. Must NOT be reversed --- */
    CHECK_EQ(emu_bus_read(&bus, 0x10000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x11223344u);
    CHECK_EQ(emu_bus_write(&bus, 0x10000000u, 4u, 0xDEADBEEFu), EMU_FAULT_NONE);
    CHECK_EQ(g_be_dev_wrote, 0xDEADBEEFu);

    /* --- passthrough: a real register. Also not reversed --- */
    CHECK_EQ(emu_bus_read(&bus, 0x40000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x11223344u);
    CHECK_EQ(emu_bus_write(&bus, 0x40000000u, 4u, 0xCAFEF00Du), EMU_FAULT_NONE);
    CHECK_EQ(periph, 0xCAFEF00Du);

    /* --- instruction fetch follows the image, so it reverses --- */
    ram[16] = 0x7Cu; ram[17] = 0x08u;
    uint16_t parcel = 0u;
    CHECK(emu_bus_add_rom(&bus, "rom", 0x20000000u, ram, sizeof(ram)));
    CHECK_EQ(emu_bus_fetch16(&bus, 0x20000010u, &parcel), EMU_FAULT_NONE);
    CHECK_EQ(parcel, 0x7C08u);

    /* And back: the flag is not one-way. */
    emu_bus_set_big_endian(&bus, false);
    CHECK_EQ(emu_bus_read(&bus, 0x80000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x44332211u);
}
#endif /* EMU_BUS_ANY_BE */

void test_bus(void)
{
    emu_bus_t bus;
    uint32_t v;

    emu_bus_init(&bus);

    CHECK(emu_bus_add_ram(&bus, "ram", 0x80000000u, ram_buf, sizeof(ram_buf)));
    CHECK(emu_bus_add_rom(&bus, "rom", 0x20000000u, rom_buf, sizeof(rom_buf)));

    /* --- overlap must be rejected at configuration time --- */
    CHECK(!emu_bus_add_ram(&bus, "overlap", 0x80000080u, ram_buf, 0x100u));
    /* Adjacent but non-overlapping is fine. */
    CHECK(emu_bus_add_ram(&bus, "adjacent", 0x80000100u, ram_buf, 0x100u));

    /* --- basic read/write --- */
    CHECK_EQ(emu_bus_write(&bus, 0x80000000u, 4u, 0xDEADBEEFu), EMU_FAULT_NONE);
    CHECK_EQ(emu_bus_read(&bus, 0x80000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0xDEADBEEFu);

    /* Sub-word access must hit the right bytes (little-endian). */
    CHECK_EQ(emu_bus_read(&bus, 0x80000000u, 1u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0xEFu);
    CHECK_EQ(emu_bus_read(&bus, 0x80000002u, 2u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0xDEADu);

    /* --- unmapped addresses fault, and with the right cause --- */
    CHECK_EQ(emu_bus_read(&bus, 0x90000000u, 4u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_bus_write(&bus, 0x90000000u, 4u, 0u), EMU_FAULT_STORE);

    /* --- ROM rejects writes but allows reads and fetches --- */
    CHECK_EQ(emu_bus_write(&bus, 0x20000000u, 4u, 1u), EMU_FAULT_STORE);
    CHECK_EQ(emu_bus_read(&bus, 0x20000000u, 4u, &v), EMU_FAULT_NONE);
    uint16_t parcel;
    CHECK_EQ(emu_bus_fetch16(&bus, 0x20000000u, &parcel), EMU_FAULT_NONE);

    /* --- an access must not straddle the end of its region --- */
    CHECK_EQ(emu_bus_read(&bus, 0x800000FDu, 4u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_bus_read(&bus, 0x800000FCu, 4u, &v), EMU_FAULT_NONE);

    /* --- passthrough --- */
    emu_bus_t pt;
    emu_bus_init(&pt);
    CHECK(emu_bus_add_passthru(&pt, "periph", 0x40000000u,
                              sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              EMU_PERM_RW, EMU_WANY));

    passthru_target[0] = 0x12345678u;
    CHECK_EQ(emu_bus_read(&pt, 0x40000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(v, 0x12345678u);

    CHECK_EQ(emu_bus_write(&pt, 0x40000004u, 4u, 0xCAFEBABEu), EMU_FAULT_NONE);
    CHECK_EQ(passthru_target[1], 0xCAFEBABEu);

    /* Execution out of a peripheral window is never legitimate. */
    CHECK_EQ(emu_bus_fetch16(&pt, 0x40000000u, &parcel),
             EMU_FAULT_FETCH);

    /* --- per-region width restriction --- */
    emu_bus_t wb;
    emu_bus_init(&wb);
    /* A 32-bit-only peripheral: byte and halfword accesses must fault. */
    CHECK(emu_bus_add_passthru(&wb, "w32only", 0x40000000u,
                              sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              EMU_PERM_RW, EMU_W32));
    CHECK_EQ(emu_bus_read(&wb, 0x40000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(emu_bus_read(&wb, 0x40000000u, 1u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_bus_read(&wb, 0x40000000u, 2u, &v), EMU_FAULT_LOAD);
    CHECK_EQ(emu_bus_write(&wb, 0x40000000u, 1u, 0u),
             EMU_FAULT_STORE);

    /* --- read-only passthrough (e.g. a peripheral the guest may observe
           but must not reconfigure) --- */
    emu_bus_t ro;
    emu_bus_init(&ro);
    CHECK(emu_bus_add_passthru(&ro, "ro", 0x40000000u, sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              EMU_PERM_R, EMU_WANY));
    CHECK_EQ(emu_bus_read(&ro, 0x40000000u, 4u, &v), EMU_FAULT_NONE);
    CHECK_EQ(emu_bus_write(&ro, 0x40000000u, 4u, 0u), EMU_FAULT_STORE);

    /* --- bulk load/dump --- */
    static const uint8_t pattern[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t back[8] = { 0 };
    CHECK(emu_bus_load(&bus, 0x80000010u, pattern, sizeof(pattern)));
    CHECK(emu_bus_dump(&bus, 0x80000010u, back, sizeof(back)));
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        CHECK_EQ(back[i], pattern[i]);
    }
    /* Bulk access to an unmapped address must fail rather than truncate. */
    CHECK(!emu_bus_load(&bus, 0x90000000u, pattern, sizeof(pattern)));

#if EMU_BUS_ANY_BE
    test_bus_big_endian();
#endif
}
