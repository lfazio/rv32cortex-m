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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

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

const char *board_name(void)
{
    return g_name;
}

void board_console_putc(uint8_t c)
{
    if (g_fd < 0) {
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
