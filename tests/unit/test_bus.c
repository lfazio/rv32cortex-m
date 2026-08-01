/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_bus.c - Region lookup and access control.
 *
 * The passthrough window is what lets a guest fault reach real ARM
 * hardware, so the checks that stop it going somewhere it should not are
 * worth testing directly rather than only through a guest.
 */

#include "tests.h"

#include "rv32/rv_bus.h"

static uint8_t ram_buf[256];
static uint8_t rom_buf[256];
static uint32_t passthru_target[16];

void test_bus(void)
{
    rv_bus_t bus;
    uint32_t v;

    rv_bus_init(&bus);

    CHECK(rv_bus_add_ram(&bus, "ram", 0x80000000u, ram_buf, sizeof(ram_buf)));
    CHECK(rv_bus_add_rom(&bus, "rom", 0x20000000u, rom_buf, sizeof(rom_buf)));

    /* --- overlap must be rejected at configuration time --- */
    CHECK(!rv_bus_add_ram(&bus, "overlap", 0x80000080u, ram_buf, 0x100u));
    /* Adjacent but non-overlapping is fine. */
    CHECK(rv_bus_add_ram(&bus, "adjacent", 0x80000100u, ram_buf, 0x100u));

    /* --- basic read/write --- */
    CHECK_EQ(rv_bus_write(&bus, 0x80000000u, 4u, 0xDEADBEEFu), RV_EXC_NONE);
    CHECK_EQ(rv_bus_read(&bus, 0x80000000u, 4u, &v), RV_EXC_NONE);
    CHECK_EQ(v, 0xDEADBEEFu);

    /* Sub-word access must hit the right bytes (little-endian). */
    CHECK_EQ(rv_bus_read(&bus, 0x80000000u, 1u, &v), RV_EXC_NONE);
    CHECK_EQ(v, 0xEFu);
    CHECK_EQ(rv_bus_read(&bus, 0x80000002u, 2u, &v), RV_EXC_NONE);
    CHECK_EQ(v, 0xDEADu);

    /* --- unmapped addresses fault, and with the right cause --- */
    CHECK_EQ(rv_bus_read(&bus, 0x90000000u, 4u, &v), RV_EXC_LOAD_ACCESS_FAULT);
    CHECK_EQ(rv_bus_write(&bus, 0x90000000u, 4u, 0u), RV_EXC_STORE_ACCESS_FAULT);

    /* --- ROM rejects writes but allows reads and fetches --- */
    CHECK_EQ(rv_bus_write(&bus, 0x20000000u, 4u, 1u), RV_EXC_STORE_ACCESS_FAULT);
    CHECK_EQ(rv_bus_read(&bus, 0x20000000u, 4u, &v), RV_EXC_NONE);
    uint16_t parcel;
    CHECK_EQ(rv_bus_fetch16(&bus, 0x20000000u, &parcel), RV_EXC_NONE);

    /* --- an access must not straddle the end of its region --- */
    CHECK_EQ(rv_bus_read(&bus, 0x800000FDu, 4u, &v), RV_EXC_LOAD_ACCESS_FAULT);
    CHECK_EQ(rv_bus_read(&bus, 0x800000FCu, 4u, &v), RV_EXC_NONE);

    /* --- passthrough --- */
    rv_bus_t pt;
    rv_bus_init(&pt);
    CHECK(rv_bus_add_passthru(&pt, "periph", 0x40000000u,
                              sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              RV_PERM_RW, RV_WANY));

    passthru_target[0] = 0x12345678u;
    CHECK_EQ(rv_bus_read(&pt, 0x40000000u, 4u, &v), RV_EXC_NONE);
    CHECK_EQ(v, 0x12345678u);

    CHECK_EQ(rv_bus_write(&pt, 0x40000004u, 4u, 0xCAFEBABEu), RV_EXC_NONE);
    CHECK_EQ(passthru_target[1], 0xCAFEBABEu);

    /* Execution out of a peripheral window is never legitimate. */
    CHECK_EQ(rv_bus_fetch16(&pt, 0x40000000u, &parcel),
             RV_EXC_INSN_ACCESS_FAULT);

    /* --- per-region width restriction --- */
    rv_bus_t wb;
    rv_bus_init(&wb);
    /* A 32-bit-only peripheral: byte and halfword accesses must fault. */
    CHECK(rv_bus_add_passthru(&wb, "w32only", 0x40000000u,
                              sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              RV_PERM_RW, RV_W32));
    CHECK_EQ(rv_bus_read(&wb, 0x40000000u, 4u, &v), RV_EXC_NONE);
    CHECK_EQ(rv_bus_read(&wb, 0x40000000u, 1u, &v), RV_EXC_LOAD_ACCESS_FAULT);
    CHECK_EQ(rv_bus_read(&wb, 0x40000000u, 2u, &v), RV_EXC_LOAD_ACCESS_FAULT);
    CHECK_EQ(rv_bus_write(&wb, 0x40000000u, 1u, 0u),
             RV_EXC_STORE_ACCESS_FAULT);

    /* --- read-only passthrough (e.g. a peripheral the guest may observe
           but must not reconfigure) --- */
    rv_bus_t ro;
    rv_bus_init(&ro);
    CHECK(rv_bus_add_passthru(&ro, "ro", 0x40000000u, sizeof(passthru_target),
                              (uintptr_t)passthru_target,
                              RV_PERM_R, RV_WANY));
    CHECK_EQ(rv_bus_read(&ro, 0x40000000u, 4u, &v), RV_EXC_NONE);
    CHECK_EQ(rv_bus_write(&ro, 0x40000000u, 4u, 0u), RV_EXC_STORE_ACCESS_FAULT);

    /* --- bulk load/dump --- */
    static const uint8_t pattern[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t back[8] = { 0 };
    CHECK(rv_bus_load(&bus, 0x80000010u, pattern, sizeof(pattern)));
    CHECK(rv_bus_dump(&bus, 0x80000010u, back, sizeof(back)));
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        CHECK_EQ(back[i], pattern[i]);
    }
    /* Bulk access to an unmapped address must fail rather than truncate. */
    CHECK(!rv_bus_load(&bus, 0x90000000u, pattern, sizeof(pattern)));
}
