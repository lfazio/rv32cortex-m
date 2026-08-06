/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_cpu.h - The ISA frontend contract.
 *
 * Everything a platform needs from a guest core, and nothing about which
 * guest core it is. A frontend supplies one emu_cpu_ops_t; the host runner
 * and the firmware drive it through this table and never name rv_hart_t,
 * g4mh_cpu_t or any other frontend's state.
 *
 * What is deliberately *not* here
 * ------------------------------
 *
 * There is no per-instruction entry point. `run` executes a whole budget
 * (EMU_DEFAULT_BUDGET, 4096 instructions) behind one indirect call, and
 * every other hook is either setup or fires on a trap. That is the rule
 * this interface lives by: an indirect call anywhere on the fetch or
 * execute path would cost more than the entire abstraction saves. The
 * measurements in CLAUDE.md are about exactly that -- 9.3% on CoreMark for
 * one extra *direct* branch on the fetch path. Adding a member here is
 * cheap; adding one that a frontend has to consult per instruction is not.
 *
 * There is also no register-file layout. rv_hart_t keeps x[] and pc at
 * offset 0 because Thumb-2 encodes small displacements off a base register
 * in 16 bits, and a shared header prefix would push them out of range. So
 * emu_cpu_t is opaque, the frontend casts it back, and state is reached
 * through reg_read/reg_write when a platform genuinely needs it.
 *
 * Adding a frontend
 * -----------------
 *
 *   1. include/<isa>/          public headers, <isa>_ prefixed
 *   2. src/frontend/<isa>/     state, decoder, interpreter, devices
 *   3. one emu_cpu_ops_t, registered in src/emu/emu_cpu.c
 *   4. -DEMU_FRONTEND_<ISA>=ON in the top-level CMakeLists
 *
 * The ISA-agnostic runtime it gets for free: the bus and its region table,
 * passthrough onto real peripherals, the NS16550 console, the ELF loader,
 * platform cache maintenance, and both platforms.
 */
#ifndef EMU_CPU_H
#define EMU_CPU_H

#include "emu_types.h"
#include "emu_bus.h"
#include "emu_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A guest core. Opaque: the frontend defines the real type and casts.
 * Never dereferenced outside the frontend that owns it.
 */
typedef struct emu_cpu emu_cpu_t;

/*
 * ELF e_machine numbers, so a frontend can name the images it accepts and
 * the host runner can pick a frontend from the file it was handed. Here
 * rather than in emu_elf.h because the loader is host-only and a frontend
 * declares this whether or not it was built into one.
 */
#define EMU_EM_V850         87u   /* Renesas V850 / RH850 */
#define EMU_EM_RISCV        243u

/* ------------------------------------------------------------------ */
/* Callbacks the platform installs                                     */
/* ------------------------------------------------------------------ */

/* Emit a string. The target has no stdio, so this is the whole interface. */
typedef void (*emu_print_fn)(void *ctx, const char *s);

/*
 * A guest system call, in ABI-neutral form.
 *
 * RISC-V puts the number in a7 and the arguments in a0-a3; RH850 encodes a
 * vector in the TRAP instruction. The frontend unpacks whichever it has
 * into this, so a platform's syscall handler -- the newlib write/exit pair
 * the test harnesses need -- is written once and works for any frontend.
 */
typedef struct emu_syscall {
    uint32_t nr;
    uint32_t arg[4];
    uint32_t ret;        /* handler's return value, written back by the frontend */
} emu_syscall_t;

/*
 * Return true to consume the call: execution resumes after the
 * instruction and `ret` goes to the ABI's return register. Return false to
 * take the architectural trap, which is what software with its own handler
 * expects.
 */
typedef bool (*emu_syscall_fn)(emu_cpu_t *cpu, emu_syscall_t *sc, void *user);

/* Per-instruction trace. Only called when the frontend is built with
 * EMU_ENABLE_TRACE, which is off by default because it is slow. */
typedef void (*emu_trace_fn)(emu_cpu_t *cpu, uint32_t pc, uint32_t insn,
                             void *user);

/*
 * The guest enabled an interrupt source, so the host may need to unmask
 * the line behind it (on the STM32, an NVIC IRQ). `source` is the host
 * interrupt number: the guest names a peripheral's interrupt by the number
 * the host's own vector table uses, so no translation table exists on
 * either side.
 */
typedef void (*emu_unmask_fn)(void *ctx, uint32_t source);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

/*
 * Filled by `status`. One call rather than an accessor per field: the
 * monitor and the exit summary want all of it at once, and a frontend can
 * gather it more cheaply than five indirect calls can.
 */
typedef struct emu_cpu_status {
    /*
     * Which backend is executing. A frontend may ship several -- rv32 has
     * an interpreter and a JIT -- and which one came up is the first thing
     * a performance number has to be read against.
     */
    const char *backend;
    uint32_t    pc;
    uint64_t    retired;      /* instructions retired since reset */
    uint32_t    traps;        /* 0 unless built with EMU_ENABLE_STATS */
    emu_state_t state;
    /*
     * False when the core is parked and nothing could ever wake it -- no
     * interrupt source enabled. The host runner uses it to stop rather
     * than spin forever on a guest that fell off the end of main().
     */
    bool        wakeable;
} emu_cpu_status_t;

/* ------------------------------------------------------------------ */
/* The contract                                                        */
/* ------------------------------------------------------------------ */

typedef struct emu_cpu_ops {
    /* Selector for --frontend and for EMU_FRONTEND in the build. */
    const char *name;
    /* Human-readable, for the banner: "RISC-V RV32IMAFC_Zicsr". */
    const char *desc;
    /* ELF e_machine this frontend accepts. 0 accepts anything. */
    uint16_t    elf_machine;

    /*
     * Cores this frontend models. A platform asks rather than assumes,
     * because the answer is a build-time property of the frontend -- the
     * G4MH one is 3 on a host and 1 on the firmware, where 64 KiB of local
     * RAM per core does not fit.
     */
    unsigned    ncores;

    /* --- lifecycle ------------------------------------------------- */

    /*
     * The frontend's core state. Statically allocated by the frontend, so
     * neither platform needs an allocator and the firmware's footprint is
     * known at link time. Returns NULL for an index the frontend does not
     * implement; today every frontend is single-core and answers only 0.
     */
    emu_cpu_t *(*instance)(unsigned index);

    void (*init)(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid);
    void (*reset)(emu_cpu_t *cpu, uint32_t reset_pc);

    /*
     * Hand the guest what only the platform knows: a stack pointer at the
     * top of guest RAM, its core id, and how much RAM it actually has, in
     * whichever registers the architecture's boot convention uses. Guests
     * that set up their own stack from a link script and ignore this still
     * work, which is why it is separate from reset.
     */
    void (*boot)(emu_cpu_t *cpu, uint32_t ram_base, uint32_t ram_size);

    /* --- execution ------------------------------------------------- */

    /*
     * Execute at most `budget` instructions through whichever backend the
     * frontend selected. The one hot entry point in this table, and the
     * reason everything else in it can afford to be indirect.
     */
    emu_run_reason_t (*run)(emu_cpu_t *cpu, uint32_t budget,
                            uint32_t *retired);

    /* Guest memory changed underneath any translated code. */
    void (*invalidate)(emu_cpu_t *cpu, uint32_t addr, uint32_t len);

    /* Execute exactly one instruction. For tests and the debug monitor. */
    emu_run_reason_t (*step)(emu_cpu_t *cpu);

    /* --- the architecture's own platform devices -------------------- */

    /*
     * The architecture's own devices, split by what they belong to.
     *
     * `add_shared_devices` maps the ones every core sees the same way -- a
     * RISC-V APLIC, an RH850 INTC2 -- and is called once per bus, because
     * with more than one core there is more than one bus.
     *
     * `add_core_devices` maps the ones that are *per core* and, crucially,
     * may be visible at a core-relative address: a RISC-V CLINT, an RH850
     * INTC1 with its SELF alias. That alias is why each core needs its own
     * bus rather than all of them sharing one -- the same guest address
     * has to resolve to different memory depending on who is executing,
     * and doing that with one bus would mean either an indirection on
     * every access or mutable global state that could not survive
     * threading. See docs/host/g4mh/multicore.md.
     *
     * Either may be NULL. False if the map is full.
     */
    bool (*add_shared_devices)(emu_bus_t *bus);
    bool (*add_core_devices)(emu_cpu_t *cpu, emu_bus_t *bus, unsigned index);

    /*
     * Raise or lower an external interrupt line, from a host ISR.
     *
     * System-wide rather than per core: an interrupt arrives at a
     * *channel*, and which core takes it is the interrupt controller's
     * routing decision (an RH850 INTC2 binds channels to PEs through
     * EIBD), not the platform's. `cpu` is any core of the system.
     */
    void (*set_irq)(emu_cpu_t *cpu, uint32_t source, bool level);

    /* Called when the guest enables a source. May be left uninstalled. */
    void (*set_unmask_hook)(emu_cpu_t *cpu, emu_unmask_fn fn, void *ctx);

    /*
     * Advance the architecture's timer. The host runner ticks it with
     * retired instructions so runs are reproducible; the firmware ticks it
     * from a real hardware timer.
     *
     * One clock for the system, not one per core: called once per
     * scheduling round, or time runs N times fast.
     */
    void (*advance_time)(emu_cpu_t *cpu, uint32_t ticks);
    void (*set_time)(emu_cpu_t *cpu, uint64_t now);

    /* --- platform services ----------------------------------------- */

    void (*set_syscall)(emu_cpu_t *cpu, emu_syscall_fn fn, void *user);
    void (*set_trace)(emu_cpu_t *cpu, emu_trace_fn fn, void *user);
    void (*set_cache)(emu_cpu_t *cpu, const emu_cache_ops_t *ops);

    /* Stop the core. What a syscall handler calls to implement exit(). */
    void (*halt)(emu_cpu_t *cpu);

    /* --- introspection --------------------------------------------- */

    void (*status)(const emu_cpu_t *cpu, emu_cpu_status_t *out);

    /* General-purpose register file, for the monitor and syscall glue. */
    unsigned    nregs;
    const char *(*reg_name)(unsigned r);
    uint32_t    (*reg_read)(const emu_cpu_t *cpu, unsigned r);
    void        (*reg_write)(emu_cpu_t *cpu, unsigned r, uint32_t v);

    /*
     * Print the architectural state a post-mortem needs: pc, the trap
     * cause decoded into words, and the register file. Formatting is the
     * frontend's job because only it knows what its registers are called
     * and which status registers matter.
     */
    void (*dump)(const emu_cpu_t *cpu, emu_print_fn out, void *ctx);

    /*
     * Disassemble one instruction. `insn` is the encoding as fetched.
     * Returns characters written, excluding the NUL. NULL when the
     * frontend was built without its disassembler.
     */
    size_t (*disasm)(char *buf, size_t buflen, uint32_t pc, uint32_t insn);
} emu_cpu_ops_t;

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/*
 * Every frontend compiled into this build, NULL-terminated. Populated in
 * src/emu/emu_cpu.c from the EMU_FRONTEND_* build flags, which is the one
 * place that needs editing when a frontend is added.
 */
extern const emu_cpu_ops_t *const emu_frontends[];

/* Look one up by name, or NULL. */
const emu_cpu_ops_t *emu_frontend_find(const char *name);

/* The first one compiled in; what a platform uses when not told otherwise. */
const emu_cpu_ops_t *emu_frontend_default(void);

/* The frontend whose ELF e_machine matches, or NULL. */
const emu_cpu_ops_t *emu_frontend_for_elf(uint16_t machine);

/* ------------------------------------------------------------------ */
/* Binding                                                             */
/* ------------------------------------------------------------------ */

/*
 * What a platform holds: a frontend, one of its cores, and the bus they
 * share. Passed around instead of a bare emu_cpu_t so the ops travel with
 * the state.
 */
typedef struct emu_core {
    const emu_cpu_ops_t *ops;
    emu_cpu_t           *cpu;
    emu_bus_t           *bus;
} emu_core_t;

/*
 * Bind `ops` to core `index` on `bus`. False if the frontend has no such
 * core. Does not reset it; call emu_core_reset next.
 */
bool emu_core_open(emu_core_t *core, const emu_cpu_ops_t *ops,
                   emu_bus_t *bus, unsigned index);

/* ------------------------------------------------------------------ */
/* Systems                                                             */
/* ------------------------------------------------------------------ */

#ifndef EMU_MAX_CORES
#  define EMU_MAX_CORES 4u
#endif

/*
 * A whole machine: one frontend, its cores, and a bus each.
 *
 * Single-core is the degenerate case rather than a separate path, so the
 * host runner and the firmware drive one and the same code whether the
 * frontend reports one core or three.
 */
typedef struct emu_system {
    const emu_cpu_ops_t *ops;
    unsigned    ncores;
    emu_core_t  core[EMU_MAX_CORES];
} emu_system_t;

/*
 * Open `ncores` cores, each on its own bus from `buses[]`. The caller has
 * already placed the shared regions -- RAM, ROM, console, passthrough --
 * in every one of them; this adds the frontend's own devices, shared ones
 * to each bus and per-core ones to the core they belong to.
 *
 * `ncores` of 0 means "whatever the frontend reports".
 */
bool emu_system_open(emu_system_t *sys, const emu_cpu_ops_t *ops,
                     emu_bus_t *buses, unsigned ncores);

void emu_system_reset(emu_system_t *sys, uint32_t reset_pc);
void emu_system_boot(emu_system_t *sys, uint32_t ram_base, uint32_t ram_size);
void emu_system_invalidate(emu_system_t *sys, uint32_t addr, uint32_t len);

/*
 * Run one scheduling round: up to `quantum` instructions on each core that
 * has anything to do, in order. Returns the number of instructions retired
 * across all of them, and stores through `all_idle` whether every core is
 * halted or parked with nothing that could wake it -- which is the
 * caller's signal to stop.
 *
 * Deterministic by construction: the same image runs the same way twice,
 * which is what makes a failure reproducible. `quantum` is the interleaving
 * knob -- 1 is instruction-interleaved lockstep, useful for shaking out
 * guest races; larger is faster. A guest whose behaviour changes with it
 * has a race.
 */
uint32_t emu_system_step(emu_system_t *sys, uint32_t quantum, bool *all_idle);

/* Convenience wrappers, so callers read as verbs rather than as tables. */

static inline emu_run_reason_t emu_core_run(emu_core_t *c, uint32_t budget,
                                            uint32_t *retired)
{
    return c->ops->run(c->cpu, budget, retired);
}

static inline void emu_core_reset(emu_core_t *c, uint32_t reset_pc)
{
    c->ops->reset(c->cpu, reset_pc);
}

static inline void emu_core_boot(emu_core_t *c, uint32_t base, uint32_t size)
{
    c->ops->boot(c->cpu, base, size);
}

static inline void emu_core_invalidate(emu_core_t *c, uint32_t addr,
                                       uint32_t len)
{
    if (c->ops->invalidate != NULL) {
        c->ops->invalidate(c->cpu, addr, len);
    }
}

static inline void emu_core_status(const emu_core_t *c, emu_cpu_status_t *out)
{
    c->ops->status(c->cpu, out);
}

static inline void emu_core_advance_time(emu_core_t *c, uint32_t ticks)
{
    if (c->ops->advance_time != NULL) {
        c->ops->advance_time(c->cpu, ticks);
    }
}

static inline void emu_core_set_irq(emu_core_t *c, uint32_t source, bool level)
{
    if (c->ops->set_irq != NULL) {
        c->ops->set_irq(c->cpu, source, level);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* EMU_CPU_H */
