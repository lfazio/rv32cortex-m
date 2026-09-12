/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.c - a pty, a clock and two counters: the host's whole "board".
 *
 * See board.h for why this exists. Everything here is the smallest thing
 * that satisfies what src/net/ asks for, and nothing more.
 */

/*
 * _DEFAULT_SOURCE, not _XOPEN_SOURCE: cfmakeraw and CRTSCTS are BSD
 * extensions that the strict XOPEN feature set hides, and the failure is
 * an implicit declaration rather than a missing header -- which under
 * -Werror is a build error and without it is a call through a guessed
 * prototype. posix_openpt and ptsname need _XOPEN_SOURCE, so both.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

/* This platform: its own header, and the contract it implements. */
#include "board.h"

/* The shared runner pieces this file talks to. */
#include "emu_args.h"
#include "emu_board.h"
#include "emu_console.h"
#include "emu_run.h"
#include "emu_session.h"

/* emucore. */
#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"
#include "emu/emu_memmap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_fd = -1;
static char g_name[64];

/*
 * A nominal core clock, so board_cycles() and board_clock_hz() have the
 * ratio sys_now() needs. The value is arbitrary and only its consistency
 * with board_cycles matters; 1 MHz makes a "cycle" a microsecond, which
 * is exactly what clock_gettime gives and avoids a multiply that could
 * overflow the 32 bits the counter is declared as.
 */
#define HOST_CLOCK_HZ 1000000u

bool board_console_open(const char *dev, char *slave_out, unsigned n)
{
    int fd;

    if (dev != NULL) {
        fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "emu: %s: %s\n", dev, strerror(errno));
            return false;
        }
        snprintf(g_name, sizeof(g_name), "%s", dev);
    } else {
        /*
         * A pty pair. pppd opens the slave; this end holds the master,
         * and the two are a serial line in every way that matters to
         * either -- including that closing one end makes the other
         * report EIO, which is how a dropped link presents.
         */
        fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
            fprintf(stderr, "emu: could not allocate a pty: %s\n",
                    strerror(errno));
            if (fd >= 0) {
                close(fd);
            }
            return false;
        }

        const char *const slave = ptsname(fd);

        if (slave == NULL) {
            close(fd);
            return false;
        }
        snprintf(g_name, sizeof(g_name), "%s", slave);
        (void)fcntl(fd, F_SETFL, O_NONBLOCK);
    }

    /*
     * Raw, and every transformation off.
     *
     * This is not tidiness. CLAUDE.md records a full day lost to
     * slattach leaving the line at **cs5** -- five data bits against the
     * board's eight -- because it cleared CSIZE without setting it, and
     * `stty raw` does not touch CSIZE either. A framing byte that is
     * silently mangled makes every frame fail its checksum and the link
     * simply never comes up, with nothing anywhere saying why. cfmakeraw
     * sets CS8 explicitly, and the whole of that failure mode is
     * unavailable here.
     */
    struct termios tio;

    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= (unsigned)~CRTSCTS;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        (void)tcsetattr(fd, TCSANOW, &tio);
    }

    g_fd = fd;
    if (slave_out != NULL && n != 0u) {
        snprintf(slave_out, n, "%s", g_name);
    }
    return true;
}

/*
 * The core this runs on, for the banner -- the counterpart of the boards'
 * "Cortex-M7". board_api.h asks every platform for it and this one did
 * not have it, because the only reader was in stm32/board.c.
 */
const char *const board_core_name = "x86-64";

static uint8_t *g_dtb;
static uint32_t g_dtb_size;

uint8_t *host_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        emu_console_printf("emu: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        emu_console_printf("emu: %s: not seekable\n", path);
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        emu_console_printf("emu: %s: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        emu_console_printf("emu: out of memory\n");
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        emu_console_printf("emu: %s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/*
 * **Not the core name**, despite what board_api.h's comment for it used
 * to say. On this platform it is the wire: the pty the link runs over, or
 * the device --ppp was pointed at, which is what a person needs printed
 * in order to attach pppd to it. The boards have one name for both
 * because their wire is not something you choose.
 */
const char *board_name(void)
{
    return g_name;
}

/*
 * Nothing to do, and the reason is specific rather than "a host does not
 * need this".
 *
 * x86-64 keeps its instruction cache coherent with data writes in
 * hardware, so a block written into the JIT's buffer is fetchable
 * immediately -- which is exactly the assumption that was wrong once the
 * same `.sync` macro started serving Thumb-2 as well, and it is written
 * here so the next reader knows which half of that sentence is doing the
 * work.
 *
 * This is where a real implementation would go if the runner were ever
 * built for an ARM host, and it would look like
 * src/platform/stm32/cache.c's. Defined rather than left out because the
 * contract asks every board for it -- see board_api.h.
 */
void board_sync_icache(const void *addr, uint32_t len)
{
    (void)addr;
    (void)len;
}

/*
 * The console wire.
 *
 * Two devices on this platform where a board has one: stdout, and the pty
 * that carries PPP once --ppp has opened it. net_sio.c moves link bytes
 * through here, so with a pty open this *is* the link -- and the
 * console's own output goes to the telnet ring instead, exactly as it
 * does on a board after the handover. Without one, stdout.
 *
 * Falling through to stdout rather than returning matters: a board with
 * no UART yet drops the byte, and a runner that dropped its output would
 * simply print nothing and look like a hang.
 */
void board_console_putc(uint8_t c)
{
    if (g_fd < 0) {
        fputc((int)c, stdout);
        fflush(stdout); /* survive a guest that faults next */
        return;
    }

    /*
     * Retry a short write rather than dropping the byte. pppos_output
     * takes a partial write as a dropped frame, and a pty master's
     * buffer is finite -- so a burst larger than it, which is any IP
     * packet of consequence, would lose its tail and the link would
     * appear to work for small frames only.
     */
    for (;;) {
        const ssize_t w = write(g_fd, &c, 1);

        if (w == 1) {
            return;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        return; /* the other end is gone */
    }
}

/*
 * A byte in, from the link. Never stdin: this is a batch runner and a
 * guest blocking on input it will never get is a hang, not a prompt.
 */
int board_console_getc(void)
{
    uint8_t c;

    if (g_fd < 0) {
        return -1;
    }

    const ssize_t r = read(g_fd, &c, 1);

    return (r == 1) ? (int)c : -1;
}

void board_console_rx_irq_enable(void)
{
    /* Nothing to arm: the kernel already buffers this line. */
}

uint32_t board_console_rx_overruns(void)
{
    return 0u;
}

uint32_t board_cycles(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * HOST_CLOCK_HZ +
                      (uint64_t)ts.tv_nsec / 1000u);
}

uint32_t board_clock_hz(void)
{
    return HOST_CLOCK_HZ;
}

static uint32_t g_led[2];

void board_led_toggle(board_led_t led)
{
    g_led[(led == BOARD_LED_TX) ? 1u : 0u]++;
}

uint32_t board_led_count(board_led_t led)
{
    return g_led[(led == BOARD_LED_TX) ? 1u : 0u];
}

static int g_gdb_port;

void board_gdb_configure(int port)
{
    g_gdb_port = port;
}

/* ------------------------------------------------------------------ */
/* The rest of the board contract -- see board_api.h                   */
/* ------------------------------------------------------------------ */

/*
 * Here rather than in runner.c, which is the file that *calls* the
 * contract. One file doing both gives a reader no way to tell which half
 * they are reading.
 */

/*
 * Under --gdb only. A runner's guest is over in milliseconds, so a stub
 * nobody asked for would be a socket nobody connects to; a board's is
 * always worth having because its guest is still going.
 */
bool board_gdb_wanted(void)
{
    /*
     * This platform's *own* transport, which is the loopback socket
     * behind --gdb. Serving the stub over --ppp is the network transport
     * and belongs to emu_debug.c, which takes precedence when the link is
     * up -- a person who asked for a network debugging session asked for
     * the stub with it, and a socket beside it would be a second way in
     * to one stub.
     */
    return g_gdb_port != 0;
}
bool board_gdb_start(emu_core_t *core, const emu_gdb_target_t *target,
                     const emu_gdb_flash_ops_t **flash)
{
    /*
     * No flash ops either way: this platform's image is a malloc'd buffer
     * the ELF loader writes into directly, so gdb's `load` has nothing to
     * program. A board's writes to its arena.
     */
    *flash = NULL;

    return host_gdb_start(core, target, g_gdb_port);
}

const char *board_gdb_where(void)
{
    static char buf[32];

    (void)snprintf(buf, sizeof(buf), "localhost:%d", g_gdb_port);
    return buf;
}

/*
 * Wait, and this is the platform that must.
 *
 * The whole guest is over in milliseconds, so a debugger that connects
 * "immediately" still arrives after the run finished: without this,
 * --gdb attaches to a guest that has already stopped and every breakpoint
 * is behind it.
 */
void board_gdb_wait(void)
{
    host_gdb_wait();
}

void board_gdb_poll(void)
{
    host_gdb_poll();
}

bool board_gdb_attached(void)
{
    return g_gdb_port != 0 && host_gdb_attached();
}

uint32_t board_gdb_run(uint32_t budget, uint32_t *retired)
{
    return host_gdb_run(budget, retired);
}

/*
 * Nothing of this platform's own between slices. Driving the IP stack is
 * emu_board_poll's, on every platform that has one -- it was here and in
 * the board's copy behind an #if, which made a build option look like a
 * property of the machine.
 */
void board_poll(void) {}

/* ------------------------------------------------------------------ */
/* The image store, the clocks, and the two ends of a run             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Guest memory                                                        */
/* ------------------------------------------------------------------ */

#define DEFAULT_RAM_SIZE (1u << 20) /* 1 MiB */

/*
 * The host has no ARM peripherals to pass through to, so the peripheral
 * window is backed by plain memory. Guest drivers still run; they just talk
 * to nothing. This keeps guest images identical across host and target.
 *
 * Sized to reach 0x40024000, the end of the STM32F446's APB1/APB2/AHB1
 * block, because that is where RCC lives at 0x40023800. A driver's first
 * act is to ungate its own peripheral's clock, so a window that stops short
 * of RCC faults on the first store every real guest driver makes -- which
 * is exactly what a 64 KiB window did.
 */
#define PERIPH_SIM_SIZE 0x24000u

static uint8_t *g_ram;
static uint8_t *g_periph;

/*
 * The machine: a frontend, its cores, and a bus each.
 *
 * One bus per core rather than one shared: a core-relative window like the
 * RH850 INTC1 SELF alias means the same guest address resolves to
 * different memory depending on who is executing, and giving each core its
 * own region table expresses that with no cost on the access path. It also
 * means each core gets its own fast-path caches, which one shared bus
 * would have them thrashing on every switch.
 *
 * `g_core` is core 0, kept as a name because the syscall and trace hooks
 * only ever need the bus, and every core's bus maps the shared regions
 * identically.
 */

/* ------------------------------------------------------------------ */
/* Console transport                                                   */

/*
 * A byte of *guest* output, for the shared syscall handler.
 *
 * Deliberately not the firmware's emu_console_putchar, which expands LF to
 * CRLF: that is a property of a terminal on a serial line, not of a
 * console. Doing it here would put a \r into every line of every guest's
 * output on stdout, which two test suites compare and one figure script
 * parses.
 *
 * The same byte as host_tx, which is the guest's virtual UART -- one
 * console, so a guest that writes through the UART and one that writes
 * through the syscall interleave in the order they happened.
 */

/*
 * No real interrupt lines to bridge: a host has no peripherals, so
 * nothing raises one and nothing needs unmasking. The hooks exist because
 * a board's do, and answering them with nothing is how a platform says
 * it has none.
 */
void board_irqs_init(void) {}

void board_irq_unmask(void *ctx, uint32_t source)
{
    (void)ctx;
    (void)source;
}

/* ------------------------------------------------------------------ */
/* The guest-image arena, in RAM                                       */
/* ------------------------------------------------------------------ */

/*
 * The same contract the boards satisfy with a flash sector, satisfied
 * here with a buffer -- which is the whole difference, and the point.
 *
 * Everything built on it is one implementation rather than two: the TFTP
 * server's begin/data/end, gdb's vFlashErase/vFlashWrite/vFlashDone, the
 * commit-on-success rule, and the erase-and-retry that makes a client's
 * retry work when the arena fills. Before this the host had its own
 * realloc-as-you-go copy of the first of those and none of the rest, so
 * `load` over the runner's gdb stub did nothing and the upload paths
 * could -- and did -- diverge.
 *
 * **Fixed size, deliberately, and it is the fidelity that matters.**
 * Growing on demand would be the natural thing for a host and would
 * remove the one behaviour worth reproducing: TFTP carries no length, so
 * running out of arena is *how the end of a transfer is discovered*, and
 * the recovery is to erase and let the client retry. A host that never
 * runs out cannot exercise that path, and CLAUDE.md records what happens
 * when it is wrong -- the recovery was wired to one of two symmetric
 * cases for a long time, and the board needed a power cycle to take
 * another image. 512 KiB is comfortably more than any guest here and
 * small enough that a suite run reaches the end of it.
 *
 * Erase writes 0xFF rather than freeing, because that is what a NOR
 * sector does and because a guest image read out of a partly-written
 * arena should look the same on both.
 */
#define HOST_ARENA_BYTES (512u * 1024u)

static uint8_t *g_arena; /* lazily allocated: most runs never upload */
static uint32_t g_arena_used;
static bool g_arena_erased;

static bool arena_alloc(void)
{
    if (g_arena == NULL) {
        g_arena = malloc(HOST_ARENA_BYTES);
        if (g_arena == NULL) {
            return false;
        }
        g_arena_erased = false;
    }
    return true;
}

uintptr_t board_flash_arena_base(void)
{
    return arena_alloc() ? (uintptr_t)g_arena : 0u;
}

uint32_t board_flash_arena_size(void)
{
    return HOST_ARENA_BYTES;
}

bool board_flash_arena_reset(void)
{
    if (!arena_alloc()) {
        return false;
    }
    memset(g_arena, 0xFF, HOST_ARENA_BYTES);
    g_arena_used = 0u;
    g_arena_erased = true;
    return true;
}

uintptr_t board_flash_arena_begin(void)
{
    if (!arena_alloc()) {
        return 0u;
    }
    if (!g_arena_erased && !board_flash_arena_reset()) {
        return 0u;
    }
    return (uintptr_t)(g_arena + g_arena_used);
}

void board_flash_arena_commit(uint32_t len)
{
    /* Word-align the next image, as a flash arena's programming
     * granularity does for free. */
    g_arena_used += (len + 3u) & ~3u;
}

bool board_flash_write(uintptr_t addr, const void *data, uint32_t len)
{
    if (g_arena == NULL) {
        return false;
    }

    const uintptr_t base = (uintptr_t)g_arena;

    if (addr < base || addr - base > HOST_ARENA_BYTES ||
        (addr - base) + len > HOST_ARENA_BYTES) {
        return false; /* full: how a transfer's end is found */
    }
    memcpy(g_arena + (addr - base), data, len);
    return true;
}

uint32_t board_flash_last_error(void)
{
    return 0u; /* no programming hardware to complain */
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

/*
 * Guest time advances with instructions retired: there is no wall clock
 * worth tracking here, and a deterministic time base is what makes two
 * runs of the same guest comparable -- which is the whole point of the
 * architecture suite. One tick per instruction matches the rate the cycle
 * counter advances at, which is what its Sail config declares.
 *
 * Once per round, not once per core, or it would run N times fast. The
 * firmware answers this hook from a real cycle counter instead, because
 * its guest drives real peripherals.
 */
static uint32_t g_timer_div = 1u;

/*
 * Guest time, from the host's monotonic clock.
 *
 * **It used to advance by instructions retired, and that is wrong for
 * anything that measures time.** A guest was told a second had passed
 * every `--timer-hz` instructions, so its clock and its progress were
 * two unrelated quantities: a Linux kernel booted with dmesg timestamps
 * past 1500 seconds during driver init and its watchdogs fired on a
 * machine running perfectly -- `BUG: workqueue lockup ... stuck for
 * 56s`, RCU stalls -- while raising the device tree's
 * timebase-frequency to compensate only moved the problem, because then
 * a timer interrupt the kernel programmed for one jiffy needed a
 * hundred times more instructions to arrive.
 *
 * The board platform has answered this correctly since it existed: it
 * reads a real cycle counter, because its guest drives real peripherals
 * and a timer interrupt has to bear some relation to the wall clock.
 * The comment beside it said the host "answers the opposite way, and
 * both are right for what they are". Only one of them is: a guest that
 * cannot tell how long anything took is broken on both.
 *
 * So this is the board's arrangement, with clock_gettime where the M7
 * has DWT. board_cycles() is already CLOCK_MONOTONIC in microseconds,
 * which makes the divisor exact and the epoch the first slice, so a
 * guest's clock starts near zero rather than at the host's uptime.
 *
 * --timer-hz still divides, which is how a guest is deliberately given
 * a slower clock than the host's.
 */
/*
 * The display.
 *
 * 1024x768 indexed, which is what the guests here ask for. A guest
 * that wants something else sets it -- the mode registers are the point
 * of the device -- and every mode in its table fits the buffer, which
 * is sized by emu_fb_max_bytes() rather than by this geometry.
 *
 * It was 320x200, Doom's mode and the cheapest thing to draw: every
 * pixel is a guest store the emulator has to execute, so a frame is now
 * 768 KiB against 64. That cost is paid by the guest, in emulated
 * instructions, rather than by the host.
 *
 * `present` is host_display_present, which draws into an SDL3 window
 * when the build has one and does nothing when it does not. The device
 * is deliberately usable either way -- the frame counter still advances,
 * so a guest can measure its own rate and a test can prove the whole
 * path without a window. See emu_dev.h.
 */
void host_display_present(void *ctx, const emu_fb_frame_t *frame);
void host_display_shutdown(void);
#define HOST_FB_WIDTH 1024u
#define HOST_FB_HEIGHT 768u

static emu_fb_t g_fb;
static uint8_t *g_fb_pixels;
static bool g_fb_ready;

emu_fb_t *board_fb(void)
{
    return g_fb_ready ? &g_fb : NULL;
}

/*
 * Keyboard and mouse.
 *
 * Present unconditionally, like the framebuffer and for the same reason:
 * a guest probing for input should not get a different answer depending
 * on whether a window happens to be open. Without SDL nothing posts to
 * them and a guest reads an empty ring, which is exactly what a board
 * with no input does.
 */
static emu_input_t g_kbd;
static emu_input_t g_mouse;

emu_input_t *board_keyboard(void)
{
    return &g_kbd;
}

emu_input_t *board_mouse(void)
{
    return &g_mouse;
}

static uint32_t g_time_epoch;

uint64_t board_time_now(void)
{
    const uint32_t div = (g_timer_div != 0u) ? g_timer_div : 1u;

    return (uint64_t)(board_cycles() - g_time_epoch) / div;
}

/*
 * How fast the guest is going, on one line that rewrites itself.
 *
 * **\r and never \n**, so a long run leaves one line rather than a
 * screenful -- and on *stderr*, so a guest's own output on stdout stays
 * clean and a redirected run is unaffected.
 *
 * Only when stderr is a terminal. A pipe or a file has no cursor to
 * return to, so the carriage returns would accumulate as one enormous
 * line; and a build log full of progress meters is the thing nobody
 * reads. --quiet turns it off explicitly, on the same reasoning that
 * already suppresses the exit summary.
 *
 * The rate is measured over the interval rather than since the start:
 * what a reader wants to know is whether it is going faster or slower
 * *now* -- a JIT warming up, a guest entering a different phase -- and
 * a running average hides exactly that.
 */
static bool g_rate_on;
static bool g_rate_printed;
static uint64_t g_rate_last_us;
static uint64_t g_rate_last_retired;

/* The line is left open, so something has to close it before anything
 * else prints -- otherwise the exit summary lands on top of it. */
static void rate_finish(void)
{
    if (g_rate_printed) {
        (void)fputc('\n', stderr);
        g_rate_printed = false;
    }
}

void host_rate_init(bool quiet)
{
    g_rate_on = !quiet && isatty(fileno(stderr));
    g_rate_last_us = board_time_now();
    g_rate_last_retired = 0u;
    if (g_rate_on) {
        (void)atexit(rate_finish);
    }
}

static void rate_report(uint64_t retired_total, uint64_t now_us)
{
    /*
     * Twice a second. Often enough to watch, rare enough that the
     * printing itself is not part of what is being measured -- this is
     * called once per run slice, which is thousands of times a second.
     */
    const uint64_t interval_us = 500000u;
    uint64_t dt;
    uint64_t dn;
    double mips;

    if (!g_rate_on) {
        return;
    }

    dt = now_us - g_rate_last_us;
    if (dt < interval_us) {
        return;
    }

    dn = retired_total - g_rate_last_retired;
    g_rate_last_us = now_us;
    g_rate_last_retired = retired_total;

    /* instructions per microsecond is already millions per second. */
    mips = (double)dn / (double)dt;

    (void)fprintf(stderr, "\r  %8.2f MIPS   %12llu retired ",
                  mips, (unsigned long long)retired_total);
    (void)fflush(stderr);
    g_rate_printed = true;
}

static void advance_guest_time(emu_system_t *sys, uint64_t retired_total,
                               uint32_t did)
{
    const uint64_t now = board_time_now();

    (void)did;

    rate_report(retired_total, now);

    if (sys->ops->set_time != NULL) {
        sys->ops->set_time(sys->core[0].cpu, now);
    }
}

/*
 * What this platform adds to a guest's address space beyond the four
 * every platform has -- see emu_board.h.
 *
 * A board's is its passthrough windows onto real peripherals. A host has
 * no peripherals, so this is plain RAM standing in for them: a guest
 * driver written against the STM32 reference manual runs here and reads
 * back what it wrote, which is enough to exercise everything except the
 * hardware itself.
 */

/*
 * One window, and it is the other kind: memory the emulator owns, where a
 * board's entries are passthrough onto real hardware. A host has no
 * peripherals to identity-map, so a guest driver poking at the window
 * reads and writes a buffer -- which is enough to exercise the driver's
 * code paths and is exactly not enough to be a peripheral.
 *
 * Not const, because g_periph is allocated in board_init.
 */
static board_region_t g_regions[1];

const board_region_t *board_regions(unsigned *count)
{
    g_regions[0] = (board_region_t){
        .name = "periph-sim",
        .base = EMU_GUEST_PERIPH_BASE,
        .size = PERIPH_SIM_SIZE,
        .perm = EMU_PERM_RW,
        .host = g_periph,
    };
    *count = 1u;
    return g_regions;
}

/*
 * The image and RAM extents the shared builder reads. Mutable here for
 * the same reason as on a board: an image arriving over TFTP replaces
 * them and the address space is rebuilt around the new numbers.
 */
const uint8_t *board_img = NULL;
uint32_t board_img_size = 0u;
uint8_t *board_ram = NULL;
uint32_t board_ram_size = 0u;

/* ------------------------------------------------------------------ */
/* The two ends of a run -- see emu_board.h                            */
/* ------------------------------------------------------------------ */

/*
 * A runner's "hardware": the command line, the file it names, and RAM
 * from malloc.
 *
 * That is the whole of what makes this a different platform rather than a
 * different program. A board brings up clocks and a UART and has its
 * image linked in; this brings up a heap and a pty and is told where to
 * find one. Everything after is the same sequence, in emu_main.c.
 */
static emu_args_t g_opt;

/*
 * The real command line, which is what a hosted platform has.
 * emu_main.c parses it and hands the result back through board_init.
 */
char *const *board_argv(int *argc)
{
    (void)argc;
    return NULL;
}

/*
 * Bring this "board" up: a heap, an image read from the file the command
 * line names, and a pty if a link was asked for.
 *
 * The rest of what used to be here is emu_main's now -- the frontend,
 * which is chosen the same way from any image; the banner; the handover.
 * Choosing the frontend in particular had drifted: this file had the full
 * three-way choice and a board had emu_frontend_default(), so a board
 * could not honour a --frontend its own board_argv named.
 */
bool board_init(const emu_args_t *args, emu_session_cfg_t *cfg,
                emu_run_env_t *env)
{
    g_opt = *args;

    /*
     * **Where "an image is required" is enforced**, rather than in the
     * parser: a board's is linked in and its equivalent command line
     * names none, so a shared parser cannot insist. This is the one
     * caller that cannot proceed without a path.
     */
    if (g_opt.path == NULL) {
        emu_args_usage();
        return false;
    }

    size_t len = 0;
    uint8_t *const image = host_read_file(g_opt.path, &len);

    if (image == NULL) {
        return false;
    }

    /*
     * The device tree, if one was named. Read here beside the image
     * because both are files this platform knows how to open and the
     * session takes them as bytes -- src/emu/ has no filesystem.
     */
    if (g_opt.dtb_path != NULL) {
        size_t dlen = 0;
        uint8_t *const dtb = host_read_file(g_opt.dtb_path, &dlen);

        if (dtb == NULL) {
            free(image);
            return false;
        }
        g_dtb = dtb;
        g_dtb_size = (uint32_t)dlen;
    }

    g_ram = calloc(g_opt.ram_size, 1u);
    g_periph = calloc(PERIPH_SIM_SIZE, 1u);

    /*
     * The framebuffer is allocated whether or not a guest uses one, and
     * at the size the *largest* offered mode needs rather than the size
     * of the starting one. A guest that sets a bigger mode gets it; a
     * platform that allocated only for 320x200 would enumerate modes it
     * then refuses, which is a worse answer than not offering them.
     *
     * 3 MB on a host, which is nothing, and unconditional for the same
     * reason as before: a guest probing for a display should not get a
     * different answer depending on a flag nobody passed.
     */
    emu_input_init(&g_kbd, EMU_INPUT_ID_KEYBOARD);
    emu_input_init(&g_mouse, EMU_INPUT_ID_MOUSE);

    const uint32_t fb_bytes = emu_fb_max_bytes();

    g_fb_pixels = calloc(fb_bytes, 1u);
    if (g_fb_pixels != NULL) {
        g_fb_ready = emu_fb_init(&g_fb, HOST_FB_WIDTH, HOST_FB_HEIGHT,
                                 EMU_FB_FMT_IDX8, 0u, g_fb_pixels,
                                 EMU_GUEST_FB_PIXELS, fb_bytes,
                                 host_display_present, NULL);
    }

    if (g_ram == NULL || g_periph == NULL) {
        emu_console_printf("emu: cannot allocate guest memory\n");
        free(image);
        return false;
    }

    board_ram = g_ram;
    board_ram_size = g_opt.ram_size;
    g_time_epoch = board_cycles();

    /* Found, not installed -- emu_main calls emu_image_set. */
    cfg->image = image;
    cfg->image_size = (uint32_t)len;
    cfg->dtb = g_dtb;
    cfg->dtb_size = g_dtb_size;

    board_gdb_configure(g_opt.gdb_port);
    cfg->ram_host = NULL;
    env->advance_time = advance_guest_time;

    /*
     * The wire, before emu_main hands it to the IP stack. Opening it is
     * this platform's -- a pty, or a device named on the command line --
     * and the handover itself is not, so emu_board_link_start() is called
     * one layer up for both platforms.
     *
     * Not fatal if it fails: a runner that cannot get a pty is still a
     * runner, and saying so beats refusing to run the guest.
     */
    if (g_opt.ppp) {
        char slave[64] = "";

        if (!board_console_open(g_opt.ppp_dev, slave, sizeof(slave))) {
            emu_console_printf("emu: --ppp: no serial device; continuing "
                               "without a network\n");
        } else {
            fprintf(stderr,
                    "emu: ppp on %s\n"
                    "emu:   scripts/ppp-host.sh %s\n",
                    slave, slave);
        }
    }
    return true;
}

/*
 * A runner can simply stop: there is a shell to report to, and the
 * message has already gone to stderr. The board's version never returns.
 */
void board_fatal(int *status)
{
    *status = 1;
}

/*
 * No cycle counter worth quoting for the *ratio*.
 *
 * Returning 0 suppresses it rather than deriving one from a clock that
 * means something else -- board_cycles() here is wall time for lwIP, and
 * reporting that as "host cycles per guest instruction" gave 2.01 for a
 * board that spends 429.
 */
uint32_t board_perf_cycles(void)
{
    return 0u;
}

/*
 * A runner has a shell to return an exit status to, so it does not park
 * -- unless a link is up, in which case there may still be a client that
 * wants the report or means to push another image.
 *
 * The loop is emu_board_after_run's; this is the one bit the two
 * platforms disagreed about.
 */
bool board_parks_after_run(void)
{
    return false;
}

/*
 * A millisecond, inside the park loop.
 *
 * The board spins there because it has nothing else to do with the
 * cycles; a process on a shared machine does. lwIP's finest timeout is
 * coarser than this by orders of magnitude, and sleeping any longer would
 * slow the stack's clock the way __WFI did on the board.
 */
void board_idle(void)
{
    struct timespec ts = {0, 1000000L};

    (void)nanosleep(&ts, NULL);
}
