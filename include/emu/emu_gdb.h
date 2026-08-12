/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_gdb.h - a GDB remote-serial-protocol stub for the emulated guest.
 *
 * This debugs the *guest*, not the emulator. A debug probe on the SWD
 * port shows Cortex-M state -- the interpreter's dispatch loop, the
 * JIT's emitted blocks -- which is the right tool for a firmware bug and
 * useless for a guest one. This attaches gdb to the RISC-V (or G4MH)
 * program instead, so `bt`, `info registers` and a breakpoint mean what
 * the guest's source says they mean.
 *
 * Split the way the rest of the tree is split, and for the same reason:
 *
 *   src/emu/emu_gdb.c   the protocol. Packet framing, checksums,
 *                       acknowledgement, the command dispatch, software
 *                       breakpoints and run control. Knows about
 *                       registers only as a count and a width, and about
 *                       memory only through emu_bus. No ISA anywhere.
 *
 *   src/frontend/<isa>/  one emu_gdb_target_t: how many registers gdb
 *                       expects for this architecture, in what order,
 *                       which of them is the PC, and what a trap
 *                       instruction looks like. That is the whole of
 *                       what differs.
 *
 * The register *order* is not ours to choose -- gdb's `g` packet is a
 * fixed concatenation defined per architecture, and getting it wrong
 * does not produce an error, it produces plausible wrong values in
 * `info registers`. For RV32 it is x0..x31 then pc, all 32-bit,
 * little-endian, which is what rv_gdb.c implements.
 *
 * Transport is anything that can move bytes: emu_gdb_rx() takes what
 * arrived and the tx callback is handed what to send. src/net/net_gdb.c
 * binds that to a TCP socket on port 1234, so this needs no lwIP header
 * and builds for a platform with no network at all.
 */
#ifndef EMU_GDB_H
#define EMU_GDB_H

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef EMU_GDB_MAX_PACKET
/*
 * Big enough for `g`/`G` on the widest target here (33 registers x 8 hex
 * digits = 264) with room for an `m` reply, and small enough that two of
 * them are not a meaningful share of guest RAM. gdb is told this via
 * PacketSize in the qSupported reply, so it will not send more.
 */
#define EMU_GDB_MAX_PACKET   1024u
#endif

#ifndef EMU_GDB_MAX_BREAK
/* Software breakpoints. gdb reports "Too many" past this rather than
 * silently losing one. */
#define EMU_GDB_MAX_BREAK    16u
#endif

/* ------------------------------------------------------------------ */
/* What a frontend has to say about itself                             */
/* ------------------------------------------------------------------ */

typedef struct emu_gdb_target {
    /*
     * Registers as gdb numbers them for this architecture, which is not
     * necessarily how the frontend numbers them: reg_get/reg_set take a
     * gdb register number and do the mapping. `nregs` is the length of
     * the `g` packet in registers.
     */
    unsigned nregs;
    unsigned reg_bytes;                 /* per register; 4 on both targets */

    uint32_t (*reg_get)(const emu_cpu_t *cpu, unsigned gdb_regno);
    void     (*reg_set)(emu_cpu_t *cpu, unsigned gdb_regno, uint32_t v);

    /* Where execution is, for breakpoint matching and the stop reply. */
    uint32_t (*pc_get)(const emu_cpu_t *cpu);
    void     (*pc_set)(emu_cpu_t *cpu, uint32_t pc);

    /*
     * Reported to gdb in the initial stop reply. 5 is SIGTRAP, which is
     * what an attach or a breakpoint should look like; a frontend may
     * map a guest trap onto something more specific.
     */
    int stop_signal;

    /* Name for qXfer / the log; not required to be a gdb arch string. */
    const char *arch;

    /*
     * gdb's target description, served through
     * qXfer:features:read:target.xml. Optional, and worth supplying:
     * without it gdb cannot know what it is talking to and every session
     * has to begin with `set architecture riscv:rv32` or an ELF, which
     * is the difference between the stub being usable and being a thing
     * you have to remember how to use.
     */
    const char *target_xml;
} emu_gdb_target_t;

/* ------------------------------------------------------------------ */
/* The stub                                                            */
/* ------------------------------------------------------------------ */

typedef void (*emu_gdb_tx_fn)(void *ctx, const uint8_t *data, uint32_t len);

typedef struct {
    uint32_t addr;
    uint32_t len;                       /* kind, as gdb sends it */
    bool     used;
} emu_gdb_break_t;

typedef struct {
    /*
     * The core, not a bare cpu pointer: emu_cpu_t is opaque here by
     * design -- src/emu/ knows no ISA -- so reaching its ops means going
     * through emu_core_t, which carries the ops, the cpu and the bus
     * together.
     */
    emu_core_t               *core;
    const emu_gdb_target_t   *target;

    emu_gdb_tx_fn tx;
    void         *tx_ctx;

    /* Receive framing: everything between '$' and '#', plus the sum. */
    uint8_t  rx[EMU_GDB_MAX_PACKET];
    uint32_t rx_len;
    uint8_t  sum[2];
    uint8_t  sum_len;
    enum { EMU_GDB_WAIT, EMU_GDB_BODY, EMU_GDB_SUM } state;
    bool     escaped;

    bool attached;                      /* a client is connected        */
    bool halted;                        /* guest is stopped for gdb     */
    bool stepping;                      /* one instruction, then stop   */
    bool ack_mode;                      /* +/- acknowledgement in use   */

    emu_gdb_break_t brk[EMU_GDB_MAX_BREAK];
} emu_gdb_t;

/*
 * Bind the stub to a core. The guest starts *running*: a stub that
 * halted at reset would make a board with no debugger attached look
 * hung, and this one is always compiled in.
 */
void emu_gdb_init(emu_gdb_t *g, emu_core_t *core,
                  const emu_gdb_target_t *target,
                  emu_gdb_tx_fn tx, void *tx_ctx);

/* A client connected or went away. Disconnecting resumes the guest --
 * leaving it stopped would need the next person to know why. */
void emu_gdb_attach(emu_gdb_t *g);
void emu_gdb_detach(emu_gdb_t *g);

/* Bytes off the wire. */
void emu_gdb_rx(emu_gdb_t *g, const uint8_t *data, uint32_t len);

/*
 * True while the guest must not be run. The run loop tests this instead
 * of calling ops->run, which is the entire integration on the emulator
 * side: no hook on the execute path, and so nothing paid per
 * instruction by a guest nobody is debugging.
 */
bool emu_gdb_halted(const emu_gdb_t *g);

/* True if a client is connected. */
bool emu_gdb_attached(const emu_gdb_t *g);

/*
 * Run one budget's worth under the stub's control, honouring
 * breakpoints and single-step. Returns what ops->run would have. Call
 * this in place of ops->run when a client is attached; when none is,
 * skip it entirely and run normally.
 */
emu_run_reason_t emu_gdb_run(emu_gdb_t *g, uint32_t budget,
                             uint32_t *retired);

#endif /* EMU_GDB_H */
