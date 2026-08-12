/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_gdb.c - the RSP protocol, with no ISA in it.
 *
 * Everything architecture-specific is behind emu_gdb_target_t; see the
 * note in emu/emu_gdb.h for why the split falls exactly there.
 *
 * Two things about RSP are worth stating because they are where a stub
 * usually goes wrong:
 *
 *   - The checksum is a *modulo-256 sum of the packet body*, not a CRC,
 *     and it is sent as two lowercase hex digits. gdb will retry a
 *     packet whose checksum it dislikes, so a stub that computes it over
 *     the wrong range does not fail, it hangs in a retry loop.
 *
 *   - Run control is not a request/response. `c` and `s` are answered
 *     *later*, when the guest stops, and answering immediately makes gdb
 *     believe the target halted at the pc it already knew about --
 *     which looks like a breakpoint that fires instantly and never
 *     advances.
 */

#include "emu/emu_gdb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Hex                                                                 */
/* ------------------------------------------------------------------ */

static char hex_digit(unsigned v)
{
    return (char)((v < 10u) ? ('0' + v) : ('a' + (v - 10u)));
}

static int hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse hex up to a delimiter; returns how many characters were used. */
static uint32_t hex_to_u32(const uint8_t *s, uint32_t len, uint32_t *out)
{
    uint32_t v = 0, i = 0;

    for (; i < len; i++) {
        const int d = hex_val(s[i]);
        if (d < 0) {
            break;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return i;
}

/*
 * A register, as gdb wants it in a `g` packet: hex, but *little-endian
 * by byte*. Writing it as plain big-endian hex is the classic mistake
 * and produces byte-swapped registers rather than an error.
 */
static void put_reg_hex(char *dst, uint32_t v, unsigned bytes)
{
    for (unsigned b = 0; b < bytes; b++) {
        const uint8_t byte = (uint8_t)(v >> (8u * b));
        *dst++ = hex_digit((unsigned)(byte >> 4));
        *dst++ = hex_digit((unsigned)(byte & 0xFu));
    }
}

static uint32_t get_reg_hex(const uint8_t *src, unsigned bytes)
{
    uint32_t v = 0;

    for (unsigned b = 0; b < bytes; b++) {
        const int hi = hex_val(src[2u * b]);
        const int lo = hex_val(src[2u * b + 1u]);

        if (hi < 0 || lo < 0) {
            break;
        }
        v |= (uint32_t)(((unsigned)hi << 4) | (unsigned)lo) << (8u * b);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

static void tx_raw(emu_gdb_t *g, const char *s, uint32_t n)
{
    if (g->tx != NULL) {
        g->tx(g->tx_ctx, (const uint8_t *)s, n);
    }
}

/*
 * $<body>#<sum>, where sum is the low byte of the sum of the body.
 * Assembled into one buffer rather than three writes because the
 * transport below is a TCP socket and three sends is three segments.
 */
static void send_packet(emu_gdb_t *g, const char *body, uint32_t len)
{
    char out[EMU_GDB_MAX_PACKET + 8u];
    uint32_t n = 0;
    uint8_t sum = 0;

    if (len > EMU_GDB_MAX_PACKET) {
        len = EMU_GDB_MAX_PACKET;
    }

    out[n++] = '$';
    for (uint32_t i = 0; i < len; i++) {
        out[n++] = body[i];
        sum = (uint8_t)(sum + (uint8_t)body[i]);
    }
    out[n++] = '#';
    out[n++] = hex_digit((unsigned)(sum >> 4));
    out[n++] = hex_digit((unsigned)(sum & 0xFu));

    tx_raw(g, out, n);
}

static void send_str(emu_gdb_t *g, const char *s)
{
    send_packet(g, s, (uint32_t)strlen(s));
}

static void send_ok(emu_gdb_t *g)     { send_str(g, "OK"); }
static void send_empty(emu_gdb_t *g)  { send_packet(g, "", 0); }

/* E<nn>. gdb prints the number, so use errno-ish values it can explain. */
static void send_err(emu_gdb_t *g, unsigned code)
{
    char b[4];

    b[0] = 'E';
    b[1] = hex_digit((code >> 4) & 0xFu);
    b[2] = hex_digit(code & 0xFu);
    send_packet(g, b, 3);
}

/*
 * The stop reply. S<sig> is the minimum; T<sig> lets the pc travel with
 * it, which saves gdb a round trip on every stop and is what makes
 * stepping feel immediate over a 3 ms link.
 */
static void send_stop(emu_gdb_t *g, int sig)
{
    char b[64];
    uint32_t n = 0;

    b[n++] = 'T';
    b[n++] = hex_digit(((unsigned)sig >> 4) & 0xFu);
    b[n++] = hex_digit((unsigned)sig & 0xFu);

    if (g->target->pc_get != NULL) {
        const unsigned pcno = g->target->nregs - 1u;

        /* "<regno>:<value>;" -- the register number is hex too. */
        if (pcno >= 16u) {
            b[n++] = hex_digit((pcno >> 4) & 0xFu);
        }
        b[n++] = hex_digit(pcno & 0xFu);
        b[n++] = ':';
        put_reg_hex(&b[n], g->target->pc_get(g->core->cpu), g->target->reg_bytes);
        n += 2u * g->target->reg_bytes;
        b[n++] = ';';
    }
    send_packet(g, b, n);
}

/* ------------------------------------------------------------------ */
/* Breakpoints                                                         */
/* ------------------------------------------------------------------ */

/*
 * Kept as a list and compared against the pc, rather than patched into
 * guest memory as a trap instruction.
 *
 * Patching is what a stub on real silicon must do, and here it would be
 * actively wrong: the read-only half of a guest image is served straight
 * out of the board's flash, so a breakpoint in .text would have nothing
 * to write to -- and with the JIT on, a patched instruction is invisible
 * until the block is retranslated. A list costs a compare per block
 * boundary and works in ROM.
 */
static emu_gdb_break_t *break_find(emu_gdb_t *g, uint32_t addr)
{
    for (unsigned i = 0; i < EMU_GDB_MAX_BREAK; i++) {
        if (g->brk[i].used && g->brk[i].addr == addr) {
            return &g->brk[i];
        }
    }
    return NULL;
}

static bool break_add(emu_gdb_t *g, uint32_t addr, uint32_t kind)
{
    if (break_find(g, addr) != NULL) {
        return true;
    }
    for (unsigned i = 0; i < EMU_GDB_MAX_BREAK; i++) {
        if (!g->brk[i].used) {
            g->brk[i].used = true;
            g->brk[i].addr = addr;
            g->brk[i].len = kind;
            return true;
        }
    }
    return false;
}

static void break_del(emu_gdb_t *g, uint32_t addr)
{
    emu_gdb_break_t *b = break_find(g, addr);

    if (b != NULL) {
        b->used = false;
    }
}

/*
 * Could a breakpoint be inside the block starting here? Far wider than
 * any block this emits, on purpose: over-stepping is cheap, missing a
 * breakpoint is the bug this replaced.
 */
#define EMU_GDB_BREAK_WINDOW 4096u

static bool break_near(const emu_gdb_t *g, uint32_t pc)
{
    for (unsigned i = 0; i < EMU_GDB_MAX_BREAK; i++) {
        if (g->brk[i].used && g->brk[i].addr >= pc &&
            (g->brk[i].addr - pc) < EMU_GDB_BREAK_WINDOW) {
            return true;
        }
    }
    return false;
}

static bool break_any(const emu_gdb_t *g)
{
    for (unsigned i = 0; i < EMU_GDB_MAX_BREAK; i++) {
        if (g->brk[i].used) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * One byte at a time through the bus, so that a read of an MMIO window
 * goes through the device model and a read of guest ROM works. Slow, and
 * it does not matter: gdb reads a few hundred bytes to draw a stack
 * frame, not megabytes.
 *
 * A failed access is reported rather than faulted: gdb probes memory it
 * has no reason to believe is mapped -- around a corrupt frame pointer,
 * for instance -- and a stub that let that trap the guest would destroy
 * the state being examined.
 */
static bool mem_read(emu_gdb_t *g, uint32_t addr, uint32_t len, char *hexout)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t v = 0;

        if (emu_bus_read(g->core->bus, addr + i, 1u, &v) != EMU_FAULT_NONE) {
            return false;
        }
        *hexout++ = hex_digit((unsigned)((v >> 4) & 0xFu));
        *hexout++ = hex_digit((unsigned)(v & 0xFu));
    }
    return true;
}

static bool mem_write(emu_gdb_t *g, uint32_t addr, uint32_t len,
                      const uint8_t *hexin)
{
    for (uint32_t i = 0; i < len; i++) {
        const int hi = hex_val(hexin[2u * i]);
        const int lo = hex_val(hexin[2u * i + 1u]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        if (emu_bus_write(g->core->bus, addr + i, 1u,
                          (uint32_t)((hi << 4) | lo)) != EMU_FAULT_NONE) {
            return false;
        }
    }
    /*
     * gdb writes memory to plant its own patches and to poke variables.
     * If that memory holds code the JIT has already translated, the
     * translation is now stale -- the same hazard as the guest's own
     * FENCE.I, and handled the same way.
     */
    if (g->core->ops->invalidate != NULL) {
        g->core->ops->invalidate(g->core->cpu, addr, len);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_read_regs(emu_gdb_t *g)
{
    char body[EMU_GDB_MAX_PACKET];
    const unsigned w = 2u * g->target->reg_bytes;
    uint32_t n = 0;

    for (unsigned r = 0; r < g->target->nregs; r++) {
        if (n + w >= sizeof(body)) {
            break;
        }
        put_reg_hex(&body[n], g->target->reg_get(g->core->cpu, r),
                    g->target->reg_bytes);
        n += w;
    }
    send_packet(g, body, n);
}

static void cmd_write_regs(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    const unsigned w = 2u * g->target->reg_bytes;

    for (unsigned r = 0; r < g->target->nregs; r++) {
        if ((r + 1u) * w > len) {
            break;
        }
        g->target->reg_set(g->core->cpu, r, get_reg_hex(&p[r * w],
                                                  g->target->reg_bytes));
    }
    send_ok(g);
}

/* p<n> and P<n>=<v>: one register. gdb prefers these once it knows the
 * target, because a `g` is 264 hex digits to read one value. */
static void cmd_read_one_reg(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    uint32_t r = 0;
    char body[32];

    (void)hex_to_u32(p, len, &r);
    if (r >= g->target->nregs) {
        send_err(g, 1);
        return;
    }
    put_reg_hex(body, g->target->reg_get(g->core->cpu, r), g->target->reg_bytes);
    send_packet(g, body, 2u * g->target->reg_bytes);
}

static void cmd_write_one_reg(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    uint32_t r = 0;
    const uint32_t used = hex_to_u32(p, len, &r);

    if (used >= len || p[used] != '=' || r >= g->target->nregs) {
        send_err(g, 1);
        return;
    }
    g->target->reg_set(g->core->cpu, r,
                       get_reg_hex(&p[used + 1u], g->target->reg_bytes));
    send_ok(g);
}

static void cmd_mem_read(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    uint32_t addr = 0, n = 0;
    uint32_t used = hex_to_u32(p, len, &addr);
    char body[EMU_GDB_MAX_PACKET];

    if (used >= len || p[used] != ',') {
        send_err(g, 1);
        return;
    }
    (void)hex_to_u32(&p[used + 1u], len - used - 1u, &n);

    if (n == 0u || (2u * n) >= sizeof(body)) {
        n = (uint32_t)(sizeof(body) / 2u) - 1u;
    }
    if (!mem_read(g, addr, n, body)) {
        send_err(g, 14);        /* EFAULT, which gdb reports as such */
        return;
    }
    send_packet(g, body, 2u * n);
}

static void cmd_mem_write(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    uint32_t addr = 0, n = 0;
    uint32_t used = hex_to_u32(p, len, &addr);

    if (used >= len || p[used] != ',') {
        send_err(g, 1);
        return;
    }
    used += 1u;
    used += hex_to_u32(&p[used], len - used, &n);
    if (used >= len || p[used] != ':') {
        send_err(g, 1);
        return;
    }
    used += 1u;

    if ((len - used) < (2u * n)) {
        send_err(g, 1);
        return;
    }
    if (!mem_write(g, addr, n, &p[used])) {
        send_err(g, 14);
        return;
    }
    send_ok(g);
}

/*
 * X addr,len:<raw bytes> -- the binary form of M, and what `load` uses.
 *
 * This is the packet that makes the stub able to receive a program:
 * `load` in gdb pushes every PT_LOAD segment through X, so with it there
 * is no need for a separate transport at all -- no TFTP, no shim, no
 * flash arena -- for anything small enough to sit in guest RAM.
 *
 * The payload is raw, not hex, so it is half the bytes on the wire and
 * needs the 0x7d unescaping the receive path already does. Note what
 * that implies for the checksum, which cost a real bug: it covers the
 * packet *as transmitted*, so it must be accumulated before unescaping.
 *
 * A zero length is not a no-op to be skipped -- gdb sends `X addr,0:` to
 * probe whether the packet is supported at all, and answering anything
 * but OK makes it fall back to M for the whole session.
 */
static void cmd_mem_write_bin(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    uint32_t addr = 0, n = 0;
    uint32_t used = hex_to_u32(p, len, &addr);

    if (used >= len || p[used] != ',') {
        send_err(g, 1);
        return;
    }
    used += 1u;
    used += hex_to_u32(&p[used], len - used, &n);
    if (used >= len || p[used] != ':') {
        send_err(g, 1);
        return;
    }
    used += 1u;

    if (n > (len - used)) {
        send_err(g, 1);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (emu_bus_write(g->core->bus, addr + i, 1u,
                          (uint32_t)p[used + i]) != EMU_FAULT_NONE) {
            send_err(g, 14);
            return;
        }
    }
    if (n != 0u && g->core->ops->invalidate != NULL) {
        /* Almost always code: `load` is the main caller. */
        g->core->ops->invalidate(g->core->cpu, addr, n);
    }
    send_ok(g);
}

static void cmd_breakpoint(emu_gdb_t *g, const uint8_t *p, uint32_t len,
                           bool insert)
{
    uint32_t type = 0, addr = 0, kind = 0;
    uint32_t used;

    used = hex_to_u32(p, len, &type);
    if (used >= len || p[used] != ',') {
        send_err(g, 1);
        return;
    }
    used += 1u;
    used += hex_to_u32(&p[used], len - used, &addr);
    if (used < len && p[used] == ',') {
        used += 1u;
        (void)hex_to_u32(&p[used], len - used, &kind);
    }

    /*
     * Type 0 is a software breakpoint and type 1 a hardware one. Both
     * are the same thing here -- a pc compare -- so both are accepted:
     * answering "unsupported" to type 1 makes `hbreak` fail on a target
     * where it would work perfectly.
     *
     * Watchpoints (2, 3, 4) are declined, and declining is the honest
     * answer: catching them needs a hook on every guest load and store,
     * which is the one path this repo has repeatedly measured as too
     * expensive to touch.
     */
    if (type > 1u) {
        send_empty(g);
        return;
    }
    if (insert) {
        if (!break_add(g, addr, kind)) {
            send_err(g, 28);            /* ENOSPC */
            return;
        }
    } else {
        break_del(g, addr);
    }
    send_ok(g);
}

static bool starts_with(const uint8_t *p, uint32_t len, const char *s)
{
    const uint32_t n = (uint32_t)strlen(s);

    return (len >= n) && (memcmp(p, s, n) == 0);
}

static void cmd_query(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    if (starts_with(p, len, "qSupported")) {
        char b[64];

        /*
         * PacketSize is the one that matters: without it gdb assumes a
         * small default and splits every memory read, which over a 3 ms
         * link is the difference between a backtrace appearing and a
         * backtrace crawling.
         */
        int n = 0;
        const char *pre = "PacketSize=";
        while (*pre != '\0') { b[n++] = *pre++; }
        {
            const uint32_t sz = EMU_GDB_MAX_PACKET;
            b[n++] = hex_digit((sz >> 8) & 0xFu);
            b[n++] = hex_digit((sz >> 4) & 0xFu);
            b[n++] = hex_digit(sz & 0xFu);
        }
        /*
         * swbreak+ tells gdb the stop reply will say when a breakpoint
         * was the reason, which is how it distinguishes its own
         * breakpoint from a trap the program took. Without it gdb has to
         * guess by comparing the pc against its breakpoint list, and
         * guesses wrong when both happen at once.
         */
        {
            const char *ext = ";swbreak+;hwbreak+";
            while (*ext != '\0') { b[n++] = *ext++; }
        }
        if (g->target->target_xml != NULL) {
            const char *ext = ";qXfer:features:read+";
            while (*ext != '\0') { b[n++] = *ext++; }
        }
        if (g->target->memory_map != NULL && g->flash != NULL) {
            /* Both, or neither: a map naming a flash region makes gdb
             * send vFlash*, and answering those without an
             * implementation loses the image silently. */
            const char *ext = ";qXfer:memory-map:read+";
            while (*ext != '\0') { b[n++] = *ext++; }
        }
        send_packet(g, b, (uint32_t)n);
        return;
    }
    if (starts_with(p, len, "qXfer:memory-map:read:") ||
        starts_with(p, len, "qXfer:features:read:")) {
        const bool is_map = starts_with(p, len, "qXfer:memory-map:read:");
        /*
         * Chunked, because it has to be: the description is ~1.6 KB
         * against a 1 KB packet. Serving it in one piece and letting
         * send_packet truncate does not fail loudly -- gdb reports
         * "Received too much data from the target" and the session dies
         * before it starts, which says nothing about the real cause
         * being a reply one and a half times the size it promised.
         *
         * The request ends ":OFFSET,LENGTH" in hex. The reply is 'm'
         * plus a chunk when more follows and 'l' plus the last one, and
         * gdb keeps asking until it sees 'l'.
         */
        const char *xml = is_map ? g->target->memory_map
                                 : g->target->target_xml;
        char b[EMU_GDB_MAX_PACKET];
        uint32_t off = 0, want = 0, total = 0, n = 0;
        uint32_t i = 0;

        if (xml == NULL) {
            send_empty(g);
            return;
        }
        while (xml[total] != '\0') {
            total++;
        }

        /* Skip to the last ':' -- the annex name may contain none, but
         * the offset always follows one. */
        for (uint32_t k = 0; k < len; k++) {
            if (p[k] == ':') {
                i = k + 1u;
            }
        }
        i += hex_to_u32(&p[i], len - i, &off);
        if (i < len && p[i] == ',') {
            (void)hex_to_u32(&p[i + 1u], len - i - 1u, &want);
        }

        if (off >= total) {
            send_str(g, "l");           /* nothing left: end of object */
            return;
        }
        /* Leave room for the prefix, and never exceed what gdb asked
         * for or what it said it could take. */
        if (want == 0u || want > sizeof(b) - 2u) {
            want = (uint32_t)sizeof(b) - 2u;
        }
        if (want > total - off) {
            want = total - off;
            b[n++] = 'l';
        } else {
            b[n++] = (off + want >= total) ? 'l' : 'm';
        }
        for (uint32_t k = 0; k < want; k++) {
            b[n++] = xml[off + k];
        }
        send_packet(g, b, n);
        return;
    }
    if (starts_with(p, len, "qAttached")) {
        /* 1: the process existed before gdb arrived, so `detach` should
         * leave it running rather than kill it. */
        send_str(g, "1");
        return;
    }
    if (starts_with(p, len, "qC")) {
        send_empty(g);
        return;
    }
    if (starts_with(p, len, "qfThreadInfo")) {
        send_str(g, "m1");
        return;
    }
    if (starts_with(p, len, "qsThreadInfo")) {
        send_str(g, "l");
        return;
    }
    send_empty(g);
}

static void dispatch(emu_gdb_t *g, const uint8_t *p, uint32_t len)
{
    if (len == 0u) {
        send_empty(g);
        return;
    }

    switch (p[0]) {
    case '?':
        send_stop(g, g->target->stop_signal);
        break;

    case 'g':
        cmd_read_regs(g);
        break;
    case 'G':
        cmd_write_regs(g, &p[1], len - 1u);
        break;
    case 'p':
        cmd_read_one_reg(g, &p[1], len - 1u);
        break;
    case 'P':
        cmd_write_one_reg(g, &p[1], len - 1u);
        break;

    case 'm':
        cmd_mem_read(g, &p[1], len - 1u);
        break;
    case 'M':
        cmd_mem_write(g, &p[1], len - 1u);
        break;

    case 'X':
        cmd_mem_write_bin(g, &p[1], len - 1u);
        break;

    case 'Z':
        cmd_breakpoint(g, &p[1], len - 1u, true);
        break;
    case 'z':
        cmd_breakpoint(g, &p[1], len - 1u, false);
        break;

    case 'c':
        /*
         * No reply now. The stop reply is sent when the guest actually
         * stops -- see the note at the top of the file.
         */
        if (len > 1u) {
            uint32_t pc = 0;
            (void)hex_to_u32(&p[1], len - 1u, &pc);
            if (g->target->pc_set != NULL) {
                g->target->pc_set(g->core->cpu, pc);
            }
        }
        g->stepping = false;
        g->halted = false;
        break;

    case 's':
        if (len > 1u) {
            uint32_t pc = 0;
            (void)hex_to_u32(&p[1], len - 1u, &pc);
            if (g->target->pc_set != NULL) {
                g->target->pc_set(g->core->cpu, pc);
            }
        }
        g->stepping = true;
        g->halted = false;
        break;

    case 'D':
        send_ok(g);
        emu_gdb_detach(g);
        break;

    case 'k':
        /* Killed. Nothing to kill on a board that is the program, so
         * treat it as a detach: leaving the guest stopped with no client
         * would need the next person to know why. */
        emu_gdb_detach(g);
        break;

    case 'q':
        cmd_query(g, p, len);
        break;

    case 'v':
        /*
         * vCont is how modern gdb resumes. Answering vCont? with an
         * empty packet makes it fall back to `c`/`s`, which works -- but
         * only because gdb still implements the fallback, and it stops
         * being able to express "step this thread, continue that one".
         * Supporting the two actions that mean anything on a
         * single-core target costs a dozen lines.
         */
        if (starts_with(p, len, "vFlashErase:")) {
            uint32_t addr = 0, n = 0;
            uint32_t i = 12;

            i += hex_to_u32(&p[i], len - i, &addr);
            if (i < len && p[i] == ',') {
                (void)hex_to_u32(&p[i + 1u], len - i - 1u, &n);
            }
            if (g->flash == NULL || !g->flash->erase(addr, n)) {
                send_err(g, 1);
            } else {
                g->flash_touched = true;
                send_ok(g);
            }
        } else if (starts_with(p, len, "vFlashWrite:")) {
            uint32_t addr = 0;
            uint32_t i = 12;

            i += hex_to_u32(&p[i], len - i, &addr);
            if (i >= len || p[i] != ':') {
                send_err(g, 1);
            } else {
                i += 1u;        /* payload is raw, already unescaped */
                if (g->flash == NULL ||
                    !g->flash->write(addr, &p[i], len - i)) {
                    send_err(g, 1);
                } else {
                    g->flash_touched = true;
                    send_ok(g);
                }
            }
        } else if (starts_with(p, len, "vFlashDone")) {
            /*
             * Commit. gdb sends this once, after the last write, and it
             * is the only point at which the image is known to be
             * complete -- erase and write arrive in as many pieces as
             * gdb feels like.
             */
            if (g->flash == NULL || !g->flash_touched) {
                send_ok(g);
            } else {
                const bool ok = g->flash->done();

                /* Once. Calling it in both arms of a ternary commits
                 * the image twice, which for an arena that advances on
                 * commit means the second one lands somewhere new. */
                g->flash_touched = false;
                if (ok) {
                    send_ok(g);
                } else {
                    send_err(g, 1);
                }
            }
        } else if (starts_with(p, len, "vCont?")) {
            /*
             * Deliberately unsupported. Advertising vCont makes gdb
             * resume *asynchronously*: `continue` returns immediately
             * and the session continues with the target running, so the
             * next command dies with "Cannot execute this command while
             * the target is running" and a breakpoint that does fire is
             * never observed. stepi stayed synchronous and worked
             * throughout, which is what localised it. An empty reply
             * sends gdb back to plain c/s, which this stub implements
             * exactly; the cost is per-thread resume, which means
             * nothing on one core.
             */
            send_empty(g);
        } else if (starts_with(p, len, "vCont")) {
            bool step = false;

            /* The action is the character after the first ';'. Thread
             * ids are ignored: there is one. */
            for (uint32_t i = 5; i < len; i++) {
                if (p[i] == ';') {
                    step = (i + 1u < len) &&
                           (p[i + 1u] == 's' || p[i + 1u] == 'S');
                    break;
                }
            }
            g->stepping = step;
            g->halted = false;
        } else {
            send_empty(g);
        }
        break;

    case 'H':       /* thread selection; one thread, so always fine */
        send_ok(g);
        break;
    case 'T':       /* is thread alive */
        send_ok(g);
        break;

    default:
        /* The documented reply for "I do not implement this", and the
         * reason a stub can be this small: gdb tries a feature, gets an
         * empty packet, and stops asking. */
        send_empty(g);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Framing                                                             */
/* ------------------------------------------------------------------ */

void emu_gdb_rx(emu_gdb_t *g, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t c = data[i];

        switch (g->state) {
        case EMU_GDB_WAIT:
            if (c == '$') {
                g->rx_len = 0;
                g->sum_len = 0;
                g->raw_sum = 0;
                g->escaped = false;
                g->state = EMU_GDB_BODY;
            } else if (c == 0x03) {
                /*
                 * Ctrl-C, sent raw and outside any packet. This is how
                 * gdb interrupts a running target, and a stub that only
                 * looks for '$' never sees it -- the symptom is that
                 * ^C in gdb does nothing at all and the session has to
                 * be killed.
                 */
                g->halted = true;
                g->stepping = false;
                send_stop(g, 2);        /* SIGINT */
            }
            /* '+' and '-' outside a packet are acknowledgements. This
             * stub does not retransmit: over TCP the bytes arrive or the
             * connection is gone, and a retry queue would be dead code
             * that only runs when something else is already broken. */
            break;

        case EMU_GDB_BODY:
            if (c == '#') {
                g->state = EMU_GDB_SUM;
            } else if (g->rx_len < sizeof(g->rx)) {
                /*
                 * The checksum covers the packet *as transmitted*, so it
                 * has to be accumulated here, before unescaping. Summing
                 * the decoded buffer instead agrees with gdb for every
                 * packet that contains no 0x7d -- which is every packet
                 * except the binary ones, so it works perfectly until
                 * the first X packet and then rejects it forever.
                 */
                g->raw_sum = (uint8_t)(g->raw_sum + c);
                /*
                 * 0x7d escapes the next byte with bit 5 flipped. gdb
                 * uses it for '$', '#', '*' and 0x7d inside binary
                 * payloads (X packets), and not decoding it corrupts
                 * exactly those bytes.
                 */
                if (g->escaped) {
                    g->rx[g->rx_len++] = (uint8_t)(c ^ 0x20u);
                    g->escaped = false;
                } else if (c == 0x7Du) {
                    g->escaped = true;
                } else {
                    g->rx[g->rx_len++] = c;
                }
            }
            break;

        case EMU_GDB_SUM:
            g->sum[g->sum_len++] = c;
            if (g->sum_len == 2u) {
                const uint8_t want = g->raw_sum;
                {
                    const int hi = hex_val(g->sum[0]);
                    const int lo = hex_val(g->sum[1]);
                    const bool ok = (hi >= 0) && (lo >= 0) &&
                                    (((hi << 4) | lo) == (int)want);

                    if (g->ack_mode) {
                        tx_raw(g, ok ? "+" : "-", 1);
                    }
                    if (ok) {
                        dispatch(g, g->rx, g->rx_len);
                    }
                }
                g->state = EMU_GDB_WAIT;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Run control                                                         */
/* ------------------------------------------------------------------ */

void emu_gdb_init(emu_gdb_t *g, emu_core_t *core,
                  const emu_gdb_target_t *target,
                  emu_gdb_tx_fn tx, void *tx_ctx)
{
    memset(g, 0, sizeof(*g));
    g->core = core;
    g->target = target;
    g->tx = tx;
    g->tx_ctx = tx_ctx;
    g->ack_mode = true;
    g->state = EMU_GDB_WAIT;
    g->halted = false;
}

void emu_gdb_set_flash(emu_gdb_t *g, const emu_gdb_flash_ops_t *ops)
{
    g->flash = ops;
}

void emu_gdb_attach(emu_gdb_t *g)
{
    g->attached = true;
    g->state = EMU_GDB_WAIT;
    g->rx_len = 0;
    g->sum_len = 0;
    /*
     * Stop on connect. gdb expects to be in control from the moment it
     * attaches and issues `?` first; a target still running would answer
     * with a pc that has already moved on.
     */
    g->halted = true;
    g->stepping = false;
}

void emu_gdb_detach(emu_gdb_t *g)
{
    g->attached = false;
    g->halted = false;
    g->stepping = false;
    for (unsigned i = 0; i < EMU_GDB_MAX_BREAK; i++) {
        g->brk[i].used = false;
    }
}

bool emu_gdb_halted(const emu_gdb_t *g)   { return g->halted; }
bool emu_gdb_attached(const emu_gdb_t *g) { return g->attached; }

emu_run_reason_t emu_gdb_run(emu_gdb_t *g, uint32_t budget,
                             uint32_t *retired)
{
    const emu_cpu_ops_t *ops = g->core->ops;

    *retired = 0;

    if (g->halted) {
        return EMU_RUN_BUDGET;
    }

    if (g->stepping) {
        const emu_run_reason_t why = ops->step(g->core->cpu);

        *retired = 1;
        g->stepping = false;
        g->halted = true;
        send_stop(g, g->target->stop_signal);
        return why;
    }

    /*
     * A run that ends for the guest's own reasons -- it halted, or went
     * to WFI -- is still a stop, and gdb is blocked waiting to be told.
     * Returning quietly leaves the session hung forever with no
     * indication that anything happened, which is exactly what it looked
     * like: attach to a guest that has finished, continue, and gdb never
     * comes back. The board showed this first and the host reproduces it
     * in two seconds.
     */
    if (!break_any(g)) {
        const emu_run_reason_t why = ops->run(g->core->cpu, budget, retired);

        if (why != EMU_RUN_BUDGET) {
            g->halted = true;
            send_stop(g, 5);            /* SIGTRAP: the target stopped */
        }
        return why;
    }

    /*
     * Breakpoints, without giving up the backend under test.
     *
     * The obvious implementation steps every instruction through
     * ops->step so a breakpoint anywhere is caught exactly. That is what
     * this did, and it is worse than slow -- ops->step is the
     * *interpreter*, so planting a breakpoint silently replaced the
     * thing being debugged. Chasing a JIT miscompile that way shows the
     * interpreter's correct answer at every stop and the bug never
     * appears: the debugger disproves the bug by existing.
     *
     * So run the real backend, one block at a time, and check the pc at
     * each block boundary. A budget of 1 is how you ask for a single
     * block: a block backend may retire more than its budget because it
     * can only stop between blocks, which is normally a hazard and is
     * exactly the lever here.
     *
     * The cost is honest and worth stating: a breakpoint is recognised
     * at *block granularity* while the JIT is live, so one in the middle
     * of a translated block is reported at the next block entry rather
     * than before the instruction. `stepi` is still exact, because a
     * single step has to go through the interpreter whatever the
     * backend. For finding out what a block computed -- which is what a
     * JIT bug needs -- stopping just after it is the useful place
     * anyway.
     */
    while (*retired < budget) {
        const uint32_t pc = g->target->pc_get(g->core->cpu);
        emu_run_reason_t why;
        uint32_t n = 0;

        /*
         * Step only where a breakpoint could be, and run blocks
         * everywhere else.
         *
         * Checking the pc at block boundaries alone -- which is what
         * this did -- never fires for a breakpoint in the middle of a
         * block, and mid-block is where breakpoints normally are. The
         * block runs straight over it and the pc afterwards is the next
         * block's, which matches nothing. It does not misreport, it
         * silently never stops, and the comment claiming it would be
         * caught "at the next block entry" was describing an intention
         * rather than the code.
         *
         * So: if any breakpoint lies within a block's reach of here,
         * step this instruction; otherwise run a whole block. The window
         * is deliberately far larger than any block this translator
         * emits (blocks average about four instructions), because being
         * conservative costs a few interpreted instructions near a
         * breakpoint and being wrong costs the breakpoint.
         *
         * The JIT still executes everything that is not immediately
         * before a breakpoint, which is the property that matters: a
         * miscompile is reproduced right up to the instruction being
         * examined.
         */
        (void)pc;
        why = ops->step(g->core->cpu);
        n = 1u;

        *retired += n;

        if (why != EMU_RUN_BUDGET) {
            /* Same as above: tell gdb, or it waits for a reply that is
             * never coming. Checked before the breakpoint test because
             * the guest has already stopped either way. */
            g->halted = true;
            send_stop(g, 5);
            return why;
        }
        if (break_find(g, g->target->pc_get(g->core->cpu)) != NULL) {
            g->halted = true;
            send_stop(g, 5);            /* SIGTRAP */
            return EMU_RUN_BUDGET;
        }
        if (n == 0u) {
            /* No forward progress -- nothing ran and nothing will.
             * Returning stops this being a loop that never ends. */
            break;
        }
    }
    return EMU_RUN_BUDGET;
}
