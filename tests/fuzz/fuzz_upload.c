/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_upload.c - a guest image arriving at run time, both ways in.
 *
 * TFTP and gdb's `load` put the next guest into the flash arena from the
 * network, and whatever they commit is run (src/platform/common/
 * emu_image.c). This drives both as their clients can:
 *
 *   TFTP   through src/net/net_tftp.c itself, its callbacks called the
 *          way lwIP's server calls them -- open, a write per DATA block
 *          as a pbuf chain, close when a short block completes the
 *          transfer or a write fails or lwIP's timer gives up, error on
 *          an ERROR packet -- and its own idle watchdog, by a clock the
 *          input advances. lwIP itself is not here (tests/fuzz/lwip-stub).
 *   gdb    vFlashErase, vFlashWrite and vFlashDone, at any address and
 *          length a client can send, through emu_image_gdb_flash.
 *
 * interleaved, with emu_image_take_pending() after every event as the
 * run loop calls it between slices. The arena is the F746's -- begin
 * refuses once it is used up, a byte is programmed only while erased,
 * commit trusts its length -- in a heap buffer of exactly its size, so a
 * read past it is ASan's. Held to:
 *
 *   - a reload only after a transfer that completed, and always after
 *     one: a short last block with every write accepted, or a vFlashDone
 *     that said yes;
 *   - the image it reloads inside the arena;
 *   - and its bytes: the blocks in order for TFTP, and for gdb the bytes
 *     of every accepted write at its offset, erased bytes between, to
 *     the highest written -- unless gdb was refused a write or told
 *     vFlashDone failed, after which nothing is promised. One upload at a
 *     time: a client that begins takes over, and the other's later
 *     calls are refused.
 *
 * Both sources are #included rather than linked, so that their statics
 * can be put back before each input: libFuzzer runs every input in one
 * process, and a finding that depended on the inputs before it could
 * not be replayed. A static added to either file belongs in reset().
 *
 * FUZZ_STATS=1 prints what the run covered at exit; FUZZ_SHOW=1 traces
 * each event.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/platform/common/emu_image.c"
#include "../../src/net/net_tftp.c"

/* ------------------------------------------------------------------ */
/* The platform, as much of it as these two reach                      */
/* ------------------------------------------------------------------ */

const uint8_t *board_img;
uint32_t board_img_size;

void emu_console_printf(const char *fmt, ...)
{
    (void)fmt;
}

void emu_net_console_putc(uint8_t c)
{
    (void)c;
}

static uint32_t g_now;

u32_t sys_now(void)
{
    return g_now;
}

static const struct tftp_context *g_ctx;
static bool g_lw_session; /* lwIP holds a handle */
static void *g_lw_handle;

err_t tftp_init_server(const struct tftp_context *ctx)
{
    g_ctx = ctx;
    return ERR_OK;
}

/* lwIP's: close_handle() on whatever session is open, then forget. */
void tftp_cleanup(void)
{
    if (g_lw_session) {
        g_lw_session = false;
        g_ctx->close(g_lw_handle);
    }
}

/* The F746's arena (src/platform/stm32f746/board.c), in a heap buffer. */
#define ARENA_SIZE 4096u

static uint8_t *g_arena;
static uint32_t g_used;
static bool g_erased;

uintptr_t board_flash_arena_base(void)
{
    return (uintptr_t)g_arena;
}

uint32_t board_flash_arena_size(void)
{
    return ARENA_SIZE;
}

static bool arena_ensure_erased(void)
{
    if (!g_erased) {
        memset(g_arena, 0xFF, ARENA_SIZE);
        g_used = 0u;
        g_erased = true;
    }
    return true;
}

uintptr_t board_flash_arena_begin(void)
{
    if (!arena_ensure_erased() || g_used >= ARENA_SIZE) {
        return 0u;
    }
    return (uintptr_t)g_arena + g_used;
}

void board_flash_arena_commit(uint32_t len)
{
    g_used += (len + 3u) & ~3u;
}

bool board_flash_arena_reset(void)
{
    g_erased = false;
    return arena_ensure_erased();
}

bool board_flash_write(uintptr_t addr, const void *data, uint32_t len)
{
    const uintptr_t base = (uintptr_t)g_arena;

    if (addr < base || addr - base > ARENA_SIZE ||
        (addr - base) + len > ARENA_SIZE) {
        return false;
    }
    /* NOR: a byte is programmed once, from erased. */
    for (uint32_t i = 0u; i < len; i++) {
        if (g_arena[addr - base + i] != 0xFFu) {
            return false;
        }
    }
    memcpy(g_arena + (addr - base), data, len);
    return true;
}

uint32_t board_flash_last_error(void)
{
    return 0u;
}

/* ------------------------------------------------------------------ */
/* What should be run next                                             */
/* ------------------------------------------------------------------ */

#define MODEL_CAP (2u * ARENA_SIZE)

typedef enum { EXPECT_NONE, EXPECT_ANY, EXPECT_TFTP, EXPECT_GDB } expect_t;

static struct {
    expect_t expect;
    bool reloaded;

    /* TFTP: the bytes of this transfer, while lwIP holds it. */
    uint8_t tftp[MODEL_CAP];
    uint32_t tftp_len;
    bool tftp_dirty; /* the model overflowed */

    /* gdb: one load, from the erase that begins it to vFlashDone, or to
     * a TFTP write request that takes the upload over. */
    bool gdb_active;
    bool gdb_dirty; /* a write was refused, or vFlashDone said no */
    uint8_t gdb[MODEL_CAP];
    uint32_t gdb_len;
} M;

static struct {
    uint64_t inputs, events, opens, blocks, completed, failed, abandoned,
        reclaims, erases, writes, refused, dones, reloads, checked;
} g_st;

static void stats_at_exit(void)
{
    fprintf(stderr,
            "fuzz-upload: %llu inputs, %llu events; tftp %llu opens, %llu "
            "blocks, %llu completed, %llu failed, %llu abandoned, %llu "
            "reclaimed; gdb %llu erases, %llu writes (%llu refused), %llu "
            "done; %llu reloads, %llu checked byte for byte\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.events,
            (unsigned long long)g_st.opens, (unsigned long long)g_st.blocks,
            (unsigned long long)g_st.completed,
            (unsigned long long)g_st.failed,
            (unsigned long long)g_st.abandoned,
            (unsigned long long)g_st.reclaims,
            (unsigned long long)g_st.erases, (unsigned long long)g_st.writes,
            (unsigned long long)g_st.refused, (unsigned long long)g_st.dones,
            (unsigned long long)g_st.reloads,
            (unsigned long long)g_st.checked);
}

static bool g_show;

static void fail(const char *what)
{
    fprintf(stderr, "fuzz-upload: %s\n", what);
    abort();
}

/* The platform's restart, which reads the image it was given. */
bool emu_main_reload(void)
{
    static uint8_t copy[ARENA_SIZE];
    const uintptr_t a = (uintptr_t)g_arena;
    const uintptr_t img = (uintptr_t)board_img;

    g_st.reloads++;
    M.reloaded = true;
    if (g_show) {
        fprintf(stderr, "  reload: %u bytes at arena+%ld\n",
                (unsigned)board_img_size, (long)(img - a));
    }
    if (M.expect == EXPECT_NONE) {
        fail("an image was run with no upload completed");
    }
    if (img < a || img > a + ARENA_SIZE ||
        (uint64_t)(img - a) + board_img_size > ARENA_SIZE) {
        fprintf(stderr, "  image at arena%+ld, %u bytes; the arena is %u\n",
                (long)(img - a), (unsigned)board_img_size,
                (unsigned)ARENA_SIZE);
        fail("the image run is not inside the arena");
    }
    memcpy(copy, board_img, board_img_size);
    if (M.expect == EXPECT_TFTP &&
        (board_img_size != M.tftp_len ||
         memcmp(copy, M.tftp, M.tftp_len) != 0)) {
        fprintf(stderr, "  %u bytes, the transfer sent %u\n",
                (unsigned)board_img_size, (unsigned)M.tftp_len);
        fail("the image run is not the one TFTP sent");
    }
    if (M.expect == EXPECT_GDB &&
        (board_img_size != M.gdb_len || memcmp(copy, M.gdb, M.gdb_len) != 0)) {
        fprintf(stderr, "  %u bytes, gdb wrote to %u\n",
                (unsigned)board_img_size, (unsigned)M.gdb_len);
        fail("the image run is not the one gdb loaded");
    }
    g_st.checked += M.expect == EXPECT_TFTP || M.expect == EXPECT_GDB;
    M.expect = EXPECT_NONE;
    return true;
}

/* Between slices: run what is pending, and say whether it should have. */
static void between_slices(void)
{
    const expect_t want = M.expect;

    M.reloaded = false;
    (void)emu_image_take_pending();
    if ((want == EXPECT_TFTP || want == EXPECT_GDB) && !M.reloaded) {
        fail("an upload completed and was not run");
    }
    if (want == EXPECT_ANY) {
        M.expect = EXPECT_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* The events                                                          */
/* ------------------------------------------------------------------ */

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

/* Bytes for a payload: from the input while it lasts, then a pattern. */
static void fill(in_t *in, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        buf[i] = (uint8_t)(in->n != 0u ? (in->n--, *in->p++) : (i * 7u + 1u));
    }
}

static void tftp_closed(void)
{
    g_lw_session = false;
}

/* A DATA block, of `len` bytes in `pieces` pbufs, as lwIP hands it on. */
static void tftp_block(in_t *in)
{
    static uint8_t buf[512];
    struct pbuf p[3];
    const uint32_t len = take(in, 2u) % 513u;
    const uint32_t pieces = 1u + take(in, 1u) % 3u;
    uint32_t at = 0u;

    if (!g_lw_session) {
        return;
    }
    fill(in, buf, len);
    for (uint32_t i = 0u; i < pieces; i++) {
        const uint32_t n = (i + 1u == pieces) ? len - at : (len - at) / 2u;

        p[i].next = (i + 1u < pieces) ? &p[i + 1u] : NULL;
        p[i].payload = buf + at;
        p[i].len = (u16_t)n;
        p[i].tot_len = (u16_t)(len - at);
        at += n;
    }
    g_st.blocks++;
    const int r = g_ctx->write(g_lw_handle, p);

    if (g_show) {
        fprintf(stderr, "tftp block %u bytes in %u: %d\n", (unsigned)len,
                (unsigned)pieces, r);
    }
    if (r < 0) {
        /* lwIP sends an ERROR and closes the session. */
        g_st.failed++;
        tftp_closed();
        g_ctx->close(g_lw_handle);
        return;
    }
    if (M.tftp_len + len <= MODEL_CAP) {
        memcpy(M.tftp + M.tftp_len, buf, len);
        M.tftp_len += len;
    } else {
        M.tftp_dirty = true;
    }
    if (len < 512u) {
        /* The short block ends the transfer: lwIP closes it. */
        g_st.completed++;
        tftp_closed();
        M.expect = M.tftp_dirty ? EXPECT_ANY : EXPECT_TFTP;
        g_ctx->close(g_lw_handle);
    }
}

static uint32_t gdb_addr(in_t *in)
{
    const uint32_t sel = take(in, 1u);

    switch (sel % 4u) {
    case 0u:
        return EMU_GUEST_ROM_BASE + take(in, 2u) % (MODEL_CAP + 64u);
    case 1u:
        return EMU_GUEST_ROM_BASE + take(in, 4u);
    case 2u:
        return take(in, 4u);
    default:
        return EMU_GUEST_ROM_BASE + (take(in, 1u) & 0x3Fu);
    }
}

static void event(in_t *in)
{
    static uint8_t buf[1100];
    const emu_gdb_flash_ops_t *const f = &emu_image_gdb_flash;
    const uint32_t op = take(in, 1u) % 11u;

    g_st.events++;
    switch (op) {
    case 0u: /* RRQ or WRQ */
    {
        const u8_t write = (take(in, 1u) & 7u) != 0u;
        void *h;

        if (g_lw_session) {
            break; /* lwIP refuses a second session itself */
        }
        g_st.opens++;
        if (write) {
            /* A write request begins an upload, which takes over from a
             * gdb load under way: that load is over. */
            M.gdb_active = false;
            M.gdb_dirty = false;
        }
        h = g_ctx->open("guest.bin", "octet", write);
        if (g_show) {
            fprintf(stderr, "tftp open %s: %s\n", write ? "write" : "read",
                    h != NULL ? "yes" : "no");
        }
        if (h != NULL) {
            if (!write) {
                fail("a read was accepted");
            }
            g_lw_session = true;
            g_lw_handle = h;
            M.tftp_len = 0u;
            M.tftp_dirty = false;
        }
        break;
    }
    case 1u:
    case 2u:
    case 3u:
        tftp_block(in);
        break;
    case 4u: /* an ERROR packet from the client */
        if (g_lw_session) {
            g_st.abandoned++;
            tftp_closed();
            g_ctx->error(g_lw_handle, 0, "x", 1);
        }
        break;
    case 5u: /* lwIP's own timer gives up on the client */
        if (g_lw_session) {
            g_st.abandoned++;
            tftp_closed();
            g_ctx->close(g_lw_handle);
        }
        break;
    case 6u: /* time passes; net_tftp's watchdog may rebuild the server */
    {
        const uint32_t before = emu_net_tftp_reclaims();

        g_now += take(in, 2u) * 16u;
        emu_net_tftp_poll();
        g_st.reclaims += emu_net_tftp_reclaims() - before;
        break;
    }
    case 7u: /* vFlashErase */
    {
        const uint32_t addr = gdb_addr(in);
        const bool ok = f->erase(addr, take(in, 4u));

        g_st.erases++;
        if (g_show) {
            fprintf(stderr, "gdb erase %08x: %d\n", (unsigned)addr, ok);
        }
        /* The first erase of a load begins it, taking over from a TFTP
         * transfer under way, whose blocks are refused from now on. */
        if (ok && !M.gdb_active) {
            M.gdb_active = true;
            M.gdb_dirty = false;
            memset(M.gdb, 0xFF, sizeof(M.gdb));
            M.gdb_len = 0u;
        }
        break;
    }
    case 8u:
    case 9u: /* vFlashWrite */
    {
        const uint32_t addr = gdb_addr(in);
        const uint32_t len = take(in, 2u) % (sizeof(buf) + 1u);
        const uint32_t off = addr - EMU_GUEST_ROM_BASE;
        bool ok;

        fill(in, buf, len);
        ok = f->write(addr, buf, len);
        g_st.writes++;
        g_st.refused += !ok;
        if (g_show) {
            fprintf(stderr, "gdb write %08x, %u bytes: %d\n", (unsigned)addr,
                    (unsigned)len, ok);
        }
        if (!M.gdb_active) {
            break;
        }
        if (!ok) {
            M.gdb_dirty = true;
        } else if (addr >= EMU_GUEST_ROM_BASE &&
                   (uint64_t)off + len <= MODEL_CAP) {
            memcpy(M.gdb + off, buf, len);
            if (off + len > M.gdb_len) {
                M.gdb_len = off + len;
            }
        } else {
            M.gdb_dirty = true; /* past what the model holds */
        }
        break;
    }
    default: /* vFlashDone */
    {
        const bool ok = f->done();

        g_st.dones++;
        if (g_show) {
            fprintf(stderr, "gdb done: %d\n", ok);
        }
        if (ok) {
            M.expect = (M.gdb_active && !M.gdb_dirty) ? EXPECT_GDB : EXPECT_ANY;
            M.gdb_active = false;
            M.gdb_dirty = false;
        } else {
            /*
             * gdb was told the load failed -- a byte held back as part of
             * a word could not be programmed when it was flushed -- and
             * nothing is promised about a load a client goes on with.
             */
            M.gdb_dirty = true;
        }
        break;
    }
    }
    between_slices();
}

/* ------------------------------------------------------------------ */

static void reset(void)
{
    static const uint8_t initial[64];

    /* emu_image.c */
    g_up_addr = 0u;
    g_reload = false;
    g_gf_carry_len = 0u;
    g_gf_carry_off = 0u;
    g_gf_len = 0u;
    memset(g_gf_carry, 0, sizeof(g_gf_carry));
    /* net_tftp.c */
    g_written = 0u;
    g_busy = false;
    g_failed = false;
    g_complete = false;
    g_last_ms = 0u;
    g_up = false;
    g_reclaims = 0u;
    /* here */
    g_now = 0u;
    g_lw_session = false;
    g_lw_handle = NULL;
    g_erased = false;
    g_used = 0u;
    memset(&M, 0, sizeof(M));

    emu_image_set(initial, sizeof(initial));
    if (!emu_net_tftp_init() || g_ctx == NULL) {
        fail("the TFTP server did not start");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    in_t in = {data, size};

    if (!once) {
        once = true;
        g_arena = malloc(ARENA_SIZE);
        if (g_arena == NULL) {
            abort();
        }
        g_show = getenv("FUZZ_SHOW") != NULL;
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(stats_at_exit);
        }
    }
    g_st.inputs++;
    reset();
    while (in.n != 0u) {
        event(&in);
    }
    return 0;
}
