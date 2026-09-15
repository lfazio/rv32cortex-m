/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_args.h - the command line, and reading the image it names.
 *
 * Split out because it is 188 of main()'s 477 lines and none of it is
 * about running a guest. It is also exactly the half emu_session.h says
 * stays per-platform: *acquisition*. A board has its image linked in and
 * takes replacements over the wire; a runner is told where to find one
 * and how to place it. Neither shape helps the other, and this is the
 * whole of the difference.
 */
#ifndef EMU_EMU_ARGS_H
#define EMU_EMU_ARGS_H

#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct emu_args {
    const char *path; /* the image to run                    */
    const char *frontend; /* NULL: from the ELF, else the first  */

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

    /*
     * The framebuffer geometry the device comes up in, as --fb WxH.
     *
     * Only the *starting* mode. The buffer is always allocated for the
     * largest mode in the table, so a guest that sets a different one
     * through the mode registers gets it whatever this said -- which is
     * why this is a runtime option rather than a build one: it changes
     * what a guest finds at reset, not what the machine can do.
     *
     * Rejected unless it is a mode the device actually has. A geometry
     * the table does not hold would leave the device reporting a size
     * no mode can produce, and a guest that enumerates modes to pick
     * one would never find the one it is already in.
     */
    uint32_t fb_width;
    uint32_t fb_height;

    /*
     * Path to a flattened device tree, or NULL. A supervisor payload --
     * OpenSBI, and Linux behind it -- reads the machine's shape out of
     * this rather than being compiled for one, so it is the difference
     * between running a kernel and only running bare metal.
     */
    const char *dtb_path;

    /*
     * A host directory the guest mounts over virtio-9p, and the tag it
     * mounts by. NULL means no filesystem device, which is the default:
     * a guest that does not ask for one should not be given a device
     * its device tree does not describe.
     */
    /*
     * The guest boots an OS that runs in supervisor mode. See
     * emu_boot_info_t::supervisor for what it decides and why guessing
     * it fails silently.
     */
    bool supervisor;

    /*
     * Present a virtio keyboard and mouse as well as the simple polled
     * devices. An operating system's driver binds to these; a
     * bare-metal guest uses the others.
     */
    /*
     * Force the performance trace on. It is shown automatically when
     * stderr is a terminal; this is for a run whose output is
     * redirected or piped, where there is no cursor to rewrite but the
     * numbers are still wanted.
     */
    bool rate;

    /*
     * A virtio console, in addition to the NS16550. An OS can move its
     * console to it once drivers are up; a test guest can use it to
     * drive a queue end to end without needing a filesystem.
     */
    /*
     * A disk image for virtio-blk. Attached read-write; `disk_ro` keeps
     * the file untouched, which is what a shared or golden image wants.
     */
    const char *disk_path;
    bool disk_ro;

    /*
     * The virtio network backend, or NULL for no interface: "loop", or
     * "tap:NAME". See emu_virtio_add_net -- the two differ in whether
     * they touch the host's network at all, which is why the loopback
     * one exists.
     */
    const char *net_spec;

    bool virtio_console;

    bool virtio_input;

    const char *p9_root;
    const char *p9_tag;

    uint32_t quantum;
    uint64_t max_insn;
    uint32_t timer_div;

    bool want_jit;
    bool quiet;
    bool dump;

    int gdb_port; /* 0: no stub                          */
    bool ppp; /* the IP stack, over a pty            */
    const char *ppp_dev; /* an existing device instead          */

    uint64_t trace_skip;
    uint64_t trace_count;
} emu_args_t;

/*
 * Parse argv. False means the runner should exit with *status* -- 0 for
 * a request that was answered, 2 for a usage error, and the caller does
 * not need to know which was which.
 */
bool emu_args_parse(int argc, char **argv, emu_args_t *out, int *status);

/* The --help text. Public because "an image is required" is enforced by
 * the host rather than by the parser -- see the note at the end of
 * emu_args_parse -- and that check wants the same output. */
void emu_args_usage(void);

/* The frontends this build has, comma separated, for a message that has
 * to say what was possible as well as what was asked for. */
void emu_args_list_frontends(void);

#endif /* EMU_EMU_ARGS_H */
