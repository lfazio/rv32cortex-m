/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_args.h - the command line, and reading the image it names.
 *
 * Split out because it is 188 of main()'s 477 lines and none of it is
 * about running a guest. It is also exactly the half emu_session.h says
 * stays per-platform: *acquisition*. A board has its image linked in and
 * takes replacements over the wire; a runner is told where to find one
 * and how to place it. Neither shape helps the other, and this is the
 * whole of the difference.
 */
#ifndef EMU_HOST_ARGS_H
#define EMU_HOST_ARGS_H

#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef struct host_args {
    const char *path;            /* the image to run                    */
    const char *frontend;        /* NULL: from the ELF, else the first  */

    /*
     * Where a flat binary goes, and where the guest starts.
     *
     * The default is the flash window, where the image is mapped
     * read-only and there is nothing to copy. A guest linked to run from
     * RAM needs --load 0x80000000 -- riscv-tests are exactly that, and
     * they also *write to their own image*, which the read-only window
     * would fault. `entry` 0 means "from the ELF, or the load address".
     */
    uint32_t load_addr;
    uint32_t entry;

    uint32_t ram_size;
    uint32_t quantum;
    uint64_t max_insn;
    uint32_t timer_div;

    bool     want_jit;
    bool     quiet;
    bool     dump;

    int      gdb_port;           /* 0: no stub                          */
    bool     ppp;                /* the IP stack, over a pty            */
    const char *ppp_dev;         /* an existing device instead          */

    uint64_t trace_skip;
    uint64_t trace_count;
} host_args_t;

/*
 * Parse argv. False means the runner should exit with *status* -- 0 for
 * a request that was answered, 2 for a usage error, and the caller does
 * not need to know which was which.
 */
bool host_args_parse(int argc, char **argv, host_args_t *out, int *status);

/*
 * The whole file, into a malloc'd buffer. NULL on failure, having said
 * why.
 *
 * The caller does *not* free it: the flash window points into it for the
 * lifetime of the run, and even an ELF's segments -- which are copied
 * out -- leave the window referring to it. Freeing left the guest
 * executing out of a freed buffer, which read correctly because nothing
 * had reused the allocation yet.
 */
uint8_t *host_read_file(const char *path, size_t *out_len);

/* The frontends this build has, comma separated, for a message that has
 * to say what was possible as well as what was asked for. */
void host_list_frontends(FILE *f);

#endif /* EMU_HOST_ARGS_H */
