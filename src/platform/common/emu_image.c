/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_image.c - a guest image arriving at run time, for every platform.
 *
 * Two ways in, one store, one restart:
 *
 *   TFTP        emu_net_image_begin/data/end, driven by the server in
 *               src/net/
 *   gdb         emu_image_gdb_flash, driven by vFlashErase / vFlashWrite /
 *               vFlashDone
 *
 * Both program into the arena board_api.h describes and neither installs
 * anything: they set a flag, and emu_image_take_pending() does the
 * restart later, from a place where no guest instruction is in flight.
 *
 * **This was written twice and the copies were not the same.** The board's
 * had the gdb path, the erase-and-retry when the arena fills, and the
 * commit-on-success rule; the host's was a realloc-as-you-go buffer with
 * none of those, so `load` over the runner's own gdb stub silently did
 * nothing and the host could not exercise the recovery path that once
 * needed a power cycle to clear on the board. What made one copy possible
 * is that the host now answers board_flash_arena_* with a buffer -- see
 * the note there about why that is worth doing rather than special-casing
 * the host here.
 *
 * The platform still owns *where the first image comes from*: a board has
 * it linked in with .incbin, a runner reads the file named on its command
 * line. That arrives through emu_image_set().
 */

#include "board_api.h"
#include "emu_board.h"
#include "emu_console.h"
#include "emu/emu_gdb.h"
#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Where the guest image currently lives: the one the platform started
 * with, until an upload repoints it at the arena.
 *
 * A variable rather than the platform's own symbols, which is what makes
 * "which image is running" a run-time fact instead of a link-time one.
 * Everything downstream reads board_img, so the two cases are one case.
 */
static const uint8_t *g_cur;
static uint32_t       g_cur_size;

void emu_image_set(const uint8_t *img, uint32_t len)
{
    g_cur      = img;
    g_cur_size = len;
    board_img      = img;
    board_img_size = len;
}

/*
 * The store, gdb's `load` and the restart are unconditional; only the
 * three entry points src/net/ calls are not, because without EMU_NET
 * there is no TFTP server to call them and the symbols would be
 * unreferenced. Everything else is reached by a platform whether or not
 * it has a link -- which is what lets a board.c stop mentioning the
 * option at all.
 */
/* ------------------------------------------------------------------ */
/* Images arriving over TFTP                                           */
/* ------------------------------------------------------------------ */

/*
 * The emulator is already suspended whenever these run: they are reached
 * from emu_net_poll(), which the run loop calls between guest slices, so
 * no guest instruction is in flight. Nothing has to be stopped -- but the
 * restart does have to be explicit, because the bus regions and the reset
 * vector were built from the old image.
 *
 * One file, whole, into the arena. It used to be two -- "rom" then "ram"
 * -- because the guest was linked entirely in RAM and the board learned
 * where its read-only part ended from the size of the first transfer. The
 * guest executes in place from flash now and copies its own .data, so
 * there is no boundary and no ordering contract.
 */
static uint32_t g_up_addr;
static bool     g_reload;

#if EMU_NET
bool emu_net_image_begin(void)
{
    if (board_flash_arena_size() == 0u) {
        return false;           /* this board takes no uploads */
    }
    g_up_addr = board_flash_arena_begin();
    return g_up_addr != 0u;
}

bool emu_net_image_data(const void *data, uint32_t len, uint32_t off)
{
    if (g_up_addr == 0u) {
        return false;
    }
    return board_flash_write(g_up_addr + off, data, len);
}

void emu_net_image_end(uint32_t len, bool ok)
{
    if (g_up_addr == 0u) {
        return;
    }
    if (!ok) {
        /*
         * The arena filling up is the *expected* failure, not an
         * exceptional one: TFTP carries no length, so running out is how
         * the end is discovered. Erasing here is what makes the client's
         * retry succeed rather than fail identically for ever.
         */
        (void)board_flash_arena_reset();
        g_up_addr = 0u;
        emu_console_printf("\nemu: upload failed\n");
        return;
    }

    board_flash_arena_commit(len);
    g_cur      = (const uint8_t *)g_up_addr;
    g_cur_size = len;
    g_up_addr  = 0u;

    /*
     * Flagged, not acted on. The address space has to be rebuilt around
     * the new image and the core reset, and neither can happen from
     * inside a TFTP callback -- which runs from emu_net_poll(), called
     * between guest slices, with the current guest's regions live.
     */
    g_reload = true;
}
#endif /* EMU_NET */

/* ------------------------------------------------------------------ */
/* Images arriving through gdb's `load`                                */
/* ------------------------------------------------------------------ */

/*
 * The same arena, driven by vFlashErase / vFlashWrite / vFlashDone.
 *
 * Worth having because it collapses the whole upload dance into one
 * command: `load` puts the image where it actually lives and leaves the
 * debugger attached and in control, which is exactly the position from
 * which a guest bug is worth looking at.
 *
 * gdb addresses these in *guest* space, so the arena offset is applied
 * here; the guest's view is what its ELF says and the arena is an
 * implementation detail of where that lands.
 */
static uint8_t  g_gf_carry[4];
static uint32_t g_gf_carry_len;
static uint32_t g_gf_carry_off;   /* guest offset of g_gf_carry[0] */
static uint32_t g_gf_len;         /* highest byte gdb has written  */

static bool gdb_flash_erase(uint32_t addr, uint32_t len)
{
    (void)len;

    if (board_flash_arena_size() == 0u || addr < EMU_GUEST_ROM_BASE) {
        return false;
    }
    /*
     * gdb erases before writing, and it is the first erase that decides
     * where this image starts. Later ones inside the same load are
     * already covered: the arena is handed out erased.
     */
    if (g_up_addr == 0u) {
        g_up_addr = board_flash_arena_begin();
        g_gf_carry_len = 0u;
    }
    return g_up_addr != 0u;
}

/*
 * gdb does not send word-aligned chunks, and board_flash_write requires
 * them.
 *
 * Its contract is "sequential and word aligned in length except for the
 * last", which the TFTP path satisfies for free -- 512-byte blocks. gdb
 * sends whatever fits its packet, ~975 bytes. Each chunk had its tail
 * padded to a word with 0xFF and the next then began at a non-aligned
 * flash address, so `load` reported success, the image landed corrupted,
 * and the guest ran away without reaching the first breakpoint. The
 * transfer looks perfect from both ends; only the guest disagrees.
 *
 * So carry the 1-3 byte remainder into the next call and hand the flash
 * only whole words. The carry is flushed when a write arrives that is not
 * contiguous with it -- gdb moves between sections, and the gap between
 * .text and .data is exactly that -- and again at vFlashDone.
 */
static bool gf_flush(void)
{
    bool ok = true;

    if (g_gf_carry_len != 0u) {
        /* board_flash_write pads a short tail with 0xFF, which is the
         * erased state, so a final partial word is safe here. */
        ok = board_flash_write(g_up_addr + g_gf_carry_off,
                               g_gf_carry, g_gf_carry_len);
        g_gf_carry_len = 0u;
    }
    return ok;
}

static bool gdb_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *const src = (const uint8_t *)data;
    const uint32_t off = addr - EMU_GUEST_ROM_BASE;
    uint32_t pos = 0u;

    if (g_up_addr == 0u || addr < EMU_GUEST_ROM_BASE) {
        return false;
    }

    /* A jump to a new section abandons whatever partial word was held for
     * the old one; it belongs at its own address, not this one. */
    if (g_gf_carry_len != 0u && (g_gf_carry_off + g_gf_carry_len) != off) {
        if (!gf_flush()) {
            return false;
        }
    }

    if (g_gf_carry_len != 0u) {
        while (g_gf_carry_len < 4u && pos < len) {
            g_gf_carry[g_gf_carry_len++] = src[pos++];
        }
        if (g_gf_carry_len < 4u) {
            return true;                /* still short of a word */
        }
        if (!board_flash_write(g_up_addr + g_gf_carry_off, g_gf_carry, 4u)) {
            return false;
        }
        g_gf_carry_len = 0u;
    }

    {
        const uint32_t rest  = len - pos;
        const uint32_t whole = rest & ~3u;
        const uint32_t tail  = rest - whole;

        if (whole != 0u &&
            !board_flash_write(g_up_addr + off + pos, &src[pos], whole)) {
            return false;
        }
        if (tail != 0u) {
            for (uint32_t i = 0; i < tail; i++) {
                g_gf_carry[i] = src[pos + whole + i];
            }
            g_gf_carry_len = tail;
            g_gf_carry_off = off + pos + whole;
        }
    }

    /* The highest byte seen is the image's length: gdb writes segments in
     * whatever order it likes and never says how much there is. */
    if (off + len > g_gf_len) {
        g_gf_len = off + len;
    }
    return true;
}

static bool gdb_flash_done(void)
{
    if (g_up_addr == 0u || g_gf_len == 0u) {
        return false;
    }
    if (!gf_flush()) {              /* the last partial word */
        return false;
    }
    board_flash_arena_commit(g_gf_len);
    g_cur      = (const uint8_t *)g_up_addr;
    g_cur_size = g_gf_len;
    g_gf_len   = 0u;
    g_up_addr  = 0u;
    g_reload   = true;
    return true;
}

const emu_gdb_flash_ops_t emu_image_gdb_flash = {
    gdb_flash_erase, gdb_flash_write, gdb_flash_done,
};

/*
 * Take a freshly uploaded image, if one is waiting. True when the guest
 * was restarted from it.
 *
 * One function because there are two callers that must not drift: between
 * guest slices, and after a guest has halted. The second is the one that
 * matters for a test harness and was the one missing -- a harness runs a
 * test, waits for it to halt, then pushes the next, by which time the run
 * loop has exited. Both transfers completed, the server said so, and
 * nothing happened.
 */
bool emu_image_take_pending(void)
{
    if (!g_reload) {
        return false;
    }
    g_reload = false;

    /*
     * The whole bring-up, not just the bus: a new image needs the
     * frontend's devices re-added, RAM cleared and the core reset.
     * Skipping that leaves the previous guest's core state in place,
     * which presents as the new guest retiring zero instructions.
     */
    board_img      = g_cur;
    board_img_size = g_cur_size;

    if (!emu_main_reload()) {
        emu_console_printf("emu: uploaded image does not fit guest RAM\n");
        return false;
    }

    emu_console_printf("\nemu: running uploaded image, %u bytes\n",
                   (unsigned)g_cur_size);
    return true;
}

