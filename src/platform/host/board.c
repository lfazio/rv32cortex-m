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

#include "board.h"
#include "board_api.h"
#include "emu_debug.h"
#include "emu_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "emu/emu_cpu.h"
#include "emu_console.h"   /* the shared syscall handler and its context */
#include "emu_run.h"
#include "emu_board.h"
#include "host_args.h"
#include "emu_session.h"
#include "emu/emu_gdb.h"
#include "emu/emu_dev.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"
#include "emu/emu_jit.h"
#include "emu/emu_ir.h"
#include <stdarg.h>

static int  g_fd = -1;
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
        fflush(stdout);         /* survive a guest that faults next */
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
        return;                 /* the other end is gone */
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
void board_poll(void) { }

/* ------------------------------------------------------------------ */
/* The image store, the clocks, and the two ends of a run             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Guest memory                                                        */
/* ------------------------------------------------------------------ */

#define DEFAULT_RAM_SIZE  (1u << 20)   /* 1 MiB */

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
#define PERIPH_SIM_SIZE   0x24000u

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
#define g_sys  (*emu_main_system())

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
void board_irqs_init(void) { }

void board_irq_unmask(void *ctx, uint32_t source)
{
    (void)ctx;
    (void)source;
}




/* ------------------------------------------------------------------ */
/* System-call services                                                */
/* ------------------------------------------------------------------ */

/*
 * The guest's exit status. `exited` distinguishes a guest that called
 * exit() from one that halted or hit the cap, because a code of 0 means
 * nothing if the syscall was never reached.
 */

/*
 * The syscall handler is emu_guest_syscall, shared with the firmware.
 *
 * This file used to carry its own -- the same newlib write(64)/exit(93)
 * pair, differing only in how it reached the bus. Two implementations of
 * one ABI, and the drift had already started: the shared one reads the
 * buffer a byte at a time *through the bus* so a buffer spanning two
 * regions works and a bad pointer faults rather than reaching into host
 * memory, and this copy did the same thing without the reasoning and
 * would not have kept doing it.
 */


/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* emu_print_fn onto stderr, for the frontend's own state dump. */

/*
 * The diagnostic sink the shared files print through.
 *
 * Two functions rather than linking emu_console.c, because the host has
 * two sinks where a board has one: guest output goes to stdout (above)
 * and diagnostics go to stderr and the telnet ring (below). A board gives
 * its only wire away and has no such choice to make.
 */



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

static uint8_t *g_arena;        /* lazily allocated: most runs never upload */
static uint32_t g_arena_used;
static bool     g_arena_erased;

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
    g_arena_used   = 0u;
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
        return false;           /* full: how a transfer's end is found */
    }
    memcpy(g_arena + (addr - base), data, len);
    return true;
}

uint32_t board_flash_last_error(void)
{
    return 0u;                  /* no programming hardware to complain */
}


/* Where emu_session reports a failure: the same sink as everything else
 * this runner says about itself. */



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

static void advance_guest_time(uint64_t retired_total, uint32_t did)
{
    (void)retired_total;
    emu_system_t *const sys = emu_main_system();

    if (g_timer_div != 0u && sys->ops->advance_time != NULL) {
        sys->ops->advance_time(sys->core[0].cpu, did / g_timer_div);
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

bool board_add_regions(emu_bus_t *bus)
{
    return emu_bus_add_ram(bus, "periph-sim", EMU_GUEST_PERIPH_BASE,
                           g_periph, PERIPH_SIM_SIZE);
}

/*
 * The image and RAM extents the shared builder reads. Mutable here for
 * the same reason as on a board: an image arriving over TFTP replaces
 * them and the address space is rebuilt around the new numbers.
 */
const uint8_t *board_img      = NULL;
uint32_t       board_img_size = 0u;
uint8_t       *board_ram      = NULL;
uint32_t       board_ram_size = 0u;



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
static host_args_t g_opt;

bool board_startup(int argc, char **argv, int *status,
                       emu_session_cfg_t *cfg, emu_run_env_t *env)
{

    /* Instructions per timer tick. 1 keeps guest time in step with the
     * cycle counter, which is what the reference model assumes. */

    if (!host_args_parse(argc, argv, &g_opt, status)) {
        return false;
    }

    /* --- read the image first, so its ELF header can pick a frontend - */
    size_t len = 0;
    uint8_t *image = host_read_file(g_opt.path, &len);
    if (image == NULL) {
        *status = 1;
        return false;
    }

    const emu_cpu_ops_t *ops;
    if (g_opt.frontend != NULL) {
        ops = emu_frontend_find(g_opt.frontend);
        if (ops == NULL) {
            emu_console_printf("emu: no frontend '%s'; this build has: ",
                    g_opt.frontend);
            host_list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            *status = 2;
            return false;
        }
    } else if (emu_elf_is_elf(image, len)) {
        const uint16_t m = emu_elf_machine(image, len);
        ops = emu_frontend_for_elf(m);
        if (ops == NULL) {
            emu_console_printf("emu: no frontend for ELF machine %u; this build has: ", m);
            host_list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            *status = 2;
            return false;
        }
    } else {
        /* A flat binary says nothing about its architecture. */
        ops = emu_frontend_default();
    }


    /* --- guest memory, which is this platform's "hardware" -------- */
    g_ram    = calloc(g_opt.ram_size, 1u);
    g_periph = calloc(PERIPH_SIM_SIZE, 1u);
    if (g_ram == NULL || g_periph == NULL) {
        emu_console_printf("emu: cannot allocate guest memory\n");
        *status = 1;
        return false;
    }

    board_ram      = g_ram;
    board_ram_size = g_opt.ram_size;
    emu_image_set(image, (uint32_t)len);

    /*
     * What only this platform decides. The runner already filled in the
     * buses, the UART and the syscall handler before calling here -- see
     * emu_board.h on why these are the two structs rather than a hook
     * each.
     */
    cfg->ops       = ops;
    cfg->load_addr = g_opt.load_addr;
    cfg->entry     = g_opt.entry;
    cfg->want_jit  = g_opt.want_jit;
    /* 378 architecture tests do not each want a register dump. */
    cfg->dump_state = g_opt.dump;
    board_gdb_configure(g_opt.gdb_port);
    /*
     * NULL: this runner calloc'd guest RAM and an ELF's segments are
     * written into it by the loader. Zeroing it again would erase them.
     */
    cfg->ram_host  = NULL;

    env->slice    = g_opt.quantum;
    env->max_insn = (uint32_t)g_opt.max_insn;
    env->advance_time = advance_guest_time;
    /*
     * The link, before the guest runs and after the cores exist -- the
     * gdb stub needs one to describe.
     *
     * The pty is this platform's; the handover is not, so it goes through
     * emu_board_link_start() exactly as the boards' does. Not fatal if it
     * fails: a runner that cannot get one is still a runner, and saying
     * so beats refusing to run the guest.
     */
    if (g_opt.ppp) {
        char slave[64] = "";

        if (!board_console_open(g_opt.ppp_dev, slave, sizeof(slave))) {
            emu_console_printf("emu: --ppp: no serial device; continuing "
                               "without a network\n");
        } else if (emu_board_link_start()) {
            fprintf(stderr,
                    "emu: ppp on %s\n"
                    "emu:   scripts/ppp-host.sh %s\n",
                    slave, slave);
        }
    }


    /* --- run --------------------------------------------------------- */
    env->take_upload = emu_image_take_pending;
    /*
     * Run control comes from board_gdb_*, which picks the transport --
     * this platform has two and a board has one. Set unconditionally
     * because they answer false when no stub is listening, which is one
     * predictable branch per slice against a NULL check that had to be
     * kept in step with three other places.
     */
#if EMU_ENABLE_TRACE
    emu_trace_configure(g_opt.trace_skip, g_opt.trace_count);
#endif
    return true;
}

/* ------------------------------------------------------------------ */
/* The gdb transport -- see emu_debug.h                                */
/* ------------------------------------------------------------------ */








/*
 * No cycle counter worth quoting. Returning 0 suppresses the ratio rather
 * than printing one derived from a clock that means something else --
 * which is exactly the mistake the board made when this was wired to
 * guest time.
 */
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
 * A runner exits; a board parks. With --ppp it does both: the link is
 * still worth serving after the guest stops, because the report is
 * sitting in a ring with nobody connected and the next image has not
 * arrived yet.
 */
bool board_after_run(const emu_guest_exit_t *exit, bool capped,
                         int *status)
{
    (void)capped;

    /*
     * With a link up, do not exit: serve it.
     *
     * This is the board's park loop, and it is what makes --ppp useful
     * rather than a demonstration. A guest is over in milliseconds; the
     * report is sitting in the telnet ring with nobody connected, and the
     * next image has not been uploaded yet. The board stays up because it
     * has nowhere to go, and here it is a deliberate choice with the same
     * consequence: a harness can push image after image at one process.
     *
     * ^C is the way out, which is why there is no clever exit condition.
     * Anything cleverer would have to guess whether a client that has not
     * connected yet is coming.
     */
    if (emu_board_link_up()) {
        fprintf(stderr, "emu: guest finished; serving the link (^C to quit)\n");
        for (;;) {
            emu_board_poll();

            if (emu_image_take_pending()) {
                return true;            /* run the new image */
            }
            if (emu_debug_parked_step(g_opt.quantum)) {
                continue;
            }

            /*
             * A millisecond. The board spins because it has nothing else
             * to do with the cycles; a process on a shared machine does,
             * and lwIP's finest timeout is coarser than this by orders of
             * magnitude. Sleeping any longer would slow the stack's clock
             * the way __WFI did on the board.
             */
            struct timespec ts = { 0, 1000000L };

            (void)nanosleep(&ts, NULL);
        }
    }

    *status = exit->exited ? (int)exit->code : 0;
    return false;
}
