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
static uint32_t g_cur_size;

void emu_image_set(const uint8_t *img, uint32_t len)
{
    g_cur = img;
    g_cur_size = len;
    board_img = img;
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
/*
 * uintptr_t, as board_flash_arena_begin returns. It was uint32_t, which
 * is the same thing on every board and drops the top half of a host
 * pointer: the host's arena is a 512 KB malloc, mapped far above 4 GB,
 * so every board_flash_write to the truncated address fell outside the
 * arena and failed -- which is also what a *full* arena reports, so an
 * upload to a host network build would have been erased and retried
 * for ever. A compiler warning said so, beside two others nobody read.
 */
static uintptr_t g_up_addr;
static bool g_reload;

/*
 * Whose upload is under way. TFTP and gdb share the arena and the
 * address an upload starts at, and nothing said which of them had it:
 * gdb wrote 64 bytes into a TFTP transfer's space, the transfer ended
 * without committing them, and the next gdb load began on that
 * "erased" space and ran with them inside its image (fuzz-upload). A
 * client that begins now takes the upload over -- the other's later
 * calls are refused -- and what the abandoned one programmed is skipped.
 */
typedef enum { UP_NONE, UP_TFTP, UP_GDB } up_owner_t;
static up_owner_t g_up_owner;
/* How far past g_up_addr this upload has programmed, or tried to. */
static uint32_t g_up_hwm;

/* gdb's load: see "Images arriving through gdb's `load`" below. */
static uint8_t g_gf_carry[4];
static uint32_t g_gf_carry_len;
static uint32_t g_gf_carry_off; /* guest offset of g_gf_carry[0] */
static uint32_t g_gf_len; /* highest byte gdb has written  */

/*
 * Whether `len` bytes from the upload's start lie inside the arena. A
 * gdb client chooses its addresses, and a write of nothing at offset
 * 0x1501 was accepted -- no flash written, so nothing refused it -- and
 * set the image's length: vFlashDone then committed 5,377 bytes of a
 * 4,096-byte arena and the next guest was read from past its end
 * (fuzz-upload). On the F746 the arena ends where flash does.
 */
static bool fits_arena(uint64_t len)
{
    const uintptr_t end = board_flash_arena_base() + board_flash_arena_size();

    return g_up_addr != 0u && g_up_addr <= end && len <= end - g_up_addr;
}

/* Program `len` bytes at `off` from the upload's start. */
static bool up_write(uint32_t off, const void *data, uint32_t len)
{
    if (!fits_arena((uint64_t)off + len)) {
        return false;
    }
    if (off + len > g_up_hwm) {
        g_up_hwm = off + len; /* tried: a refused program may have begun */
    }
    return board_flash_write(g_up_addr + off, data, len);
}

/*
 * Give up an upload that did not finish. Programmed cells cannot be
 * programmed again, so its bytes are skipped rather than reused, and the
 * next erase reclaims them.
 */
static void up_abandon(void)
{
    if (g_up_owner != UP_NONE) {
        board_flash_arena_commit(g_up_hwm);
    }
    g_up_owner = UP_NONE;
    g_up_addr = 0u;
    g_up_hwm = 0u;
    g_gf_carry_len = 0u;
    g_gf_len = 0u;
}

/*
 * Start an upload for `who`, abandoning any under way. A full arena
 * refuses, and nothing else would ever erase it -- TFTP's erase-and-retry
 * is reached from a write that fails, and there is no write without a
 * begin -- so a full one is erased here and asked again. gdb's `load`
 * had no such retry, and could not load into a full arena at all.
 */
static bool up_begin(up_owner_t who)
{
    up_abandon();
    if (board_flash_arena_size() == 0u) {
        return false; /* this board takes no uploads */
    }
    g_up_addr = board_flash_arena_begin();
    if (g_up_addr == 0u && board_flash_arena_reset()) {
        g_up_addr = board_flash_arena_begin();
    }
    g_up_owner = (g_up_addr != 0u) ? who : UP_NONE;
    return g_up_addr != 0u;
}

/*
 * The upload is complete and its first `len` bytes are the next image.
 *
 * Flagged, not acted on. The address space has to be rebuilt around the
 * new image and the core reset, and neither can happen from inside a
 * TFTP callback or a gdb packet -- which run between guest slices, with
 * the current guest's regions live.
 */
static void up_commit(uint32_t len)
{
    board_flash_arena_commit(len > g_up_hwm ? len : g_up_hwm);
    g_cur = (const uint8_t *)g_up_addr;
    g_cur_size = len;
    g_up_owner = UP_NONE;
    g_up_addr = 0u;
    g_up_hwm = 0u;
    g_reload = true;
}

#if EMU_NET
bool emu_net_image_begin(void)
{
    return up_begin(UP_TFTP);
}

bool emu_net_image_data(const void *data, uint32_t len, uint32_t off)
{
    return g_up_owner == UP_TFTP && up_write(off, data, len);
}

void emu_net_image_end(uint32_t len, bool ok)
{
    if (g_up_owner != UP_TFTP) {
        return; /* gdb took the upload over; this one is gone */
    }
    if (!ok) {
        /*
         * The arena filling up is the *expected* failure, not an
         * exceptional one: TFTP carries no length, so running out is how
         * the end is discovered. Erasing here is what makes the client's
         * retry succeed rather than fail identically for ever.
         */
        (void)board_flash_arena_reset();
        g_up_owner = UP_NONE; /* nothing to skip: all of it is erased */
        g_up_addr = 0u;
        g_up_hwm = 0u;
        emu_console_printf("\nemu: upload failed\n");
        return;
    }
    if (!fits_arena(len)) {
        /* Not reachable from net_tftp.c, whose length is what it wrote;
         * refused rather than run past the arena if that changes. */
        up_abandon();
        return;
    }
    up_commit(len);
}

void emu_net_image_abandon(uint32_t len)
{
    if (g_up_owner != UP_TFTP) {
        return;
    }
    up_abandon();
    emu_console_printf("\nemu: upload abandoned after %u bytes; nothing run\n",
                       (unsigned)len);
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

static bool gdb_flash_erase(uint32_t addr, uint32_t len)
{
    (void)len;

    if (addr < EMU_GUEST_ROM_BASE) {
        return false;
    }
    /*
     * gdb erases before writing, and it is the first erase that decides
     * where this image starts -- abandoning any other upload, TFTP's or
     * a load gdb never finished, whose length and carry go with it. Later
     * ones inside the same load are already covered: the arena is handed
     * out erased.
     */
    if (g_up_owner != UP_GDB) {
        return up_begin(UP_GDB);
    }
    return true;
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
        ok = up_write(g_gf_carry_off, g_gf_carry, g_gf_carry_len);
        g_gf_carry_len = 0u;
    }
    return ok;
}

static bool gdb_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *const src = (const uint8_t *)data;
    const uint32_t off = addr - EMU_GUEST_ROM_BASE;
    uint32_t pos = 0u;

    if (g_up_owner != UP_GDB || addr < EMU_GUEST_ROM_BASE ||
        !fits_arena((uint64_t)off + len)) {
        return false;
    }

    /*
     * The highest byte seen is the image's length: gdb writes segments in
     * whatever order it likes and never says how much there is. Counted
     * first: a write of one to three bytes that only tops up the carry
     * returns early, and counted at the end its bytes were cut off the
     * image -- gdb's ~975-byte packets leave two carried after the
     * second, so a one-byte third that ends the image lost it
     * (fuzz-upload).
     */
    if (off + len > g_gf_len) {
        g_gf_len = off + len;
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
            return true; /* still short of a word */
        }
        if (!up_write(g_gf_carry_off, g_gf_carry, 4u)) {
            return false;
        }
        g_gf_carry_len = 0u;
    }

    {
        const uint32_t rest = len - pos;
        const uint32_t whole = rest & ~3u;
        const uint32_t tail = rest - whole;

        if (whole != 0u && !up_write(off + pos, &src[pos], whole)) {
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
    return true;
}

static bool gdb_flash_done(void)
{
    if (g_up_owner != UP_GDB || g_gf_len == 0u || !fits_arena(g_gf_len)) {
        return false;
    }
    if (!gf_flush()) { /* the last partial word */
        return false;
    }
    up_commit(g_gf_len);
    g_gf_len = 0u;
    return true;
}

const emu_gdb_flash_ops_t emu_image_gdb_flash = {
    gdb_flash_erase,    gdb_flash_write,        gdb_flash_done,
    EMU_GUEST_ROM_BASE, board_flash_arena_size,
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
    board_img = g_cur;
    board_img_size = g_cur_size;

    if (!emu_main_reload()) {
        emu_console_printf("emu: uploaded image does not fit guest RAM\n");
        return false;
    }

    emu_console_printf("\nemu: running uploaded image, %u bytes\n",
                       (unsigned)g_cur_size);
    return true;
}
