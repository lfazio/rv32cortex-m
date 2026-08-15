/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_cpu.h - Architectural state of one RH850 G4MH core.
 *
 * Laid out on the same principle as rv_hart_t: the fields the interpreter
 * touches every instruction (r[], pc) sit at offset 0, so that on Thumb-2
 * they stay inside the 5-bit immediate range of LDR/STR and a guest
 * register access is a single instruction off the core pointer. That
 * matters here for the same reason it matters there, and it is why
 * emu_cpu_t is an opaque pointer rather than a struct with a common
 * header: a shared prefix would push these out of range.
 *
 * Scope
 * -----
 *
 * This frontend implements the G4MH integer core: the 16-bit and 32-bit
 * base formats, the exception model at both EI and FE level, the system
 * register file and the interrupt controller. It does *not* implement the
 * FPU, the MPU, or the 48-bit and bit-string instruction groups; those
 * encodings raise a reserved-instruction exception, which is the correct
 * report for something not implemented, and each is a self-contained
 * addition to g4mh_interp.c rather than a change to anything around it.
 */
#ifndef G4MH_G4MH_CPU_H
#define G4MH_G4MH_CPU_H

#include "emu/emu_backend.h"
#include "emu/emu_bus.h"
#include "emu/emu_cache.h"
#include "emu/emu_cpu.h"
/* For EMU_JIT_X86_64, which decides whether there is a JIT to declare.
 * Included here rather than left to the includer: a translation unit that
 * got this header first would silently see G4MH_HAVE_JIT as 0 and declare
 * no backend, which is a link error at best and a wrong default at worst. */
#include "emu/emu_jit.h"

#include "g4mh_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct g4mh_intc;

typedef struct g4mh_cpu {
    /* --- hot state: keep first --- */
    uint32_t r[32];              /* r0 is hardwired zero, written then ignored */
    uint32_t pc;

    struct emu_bus *bus;

    /* --- program status --- */
    uint32_t psw;

    /*
     * The system register file, as banks of 32 indexed by selID. Held as a
     * plain array rather than as named fields because LDSR and STSR name
     * registers by number, so every access would otherwise be a switch;
     * the named ones (PSW especially) are shadowed into their own fields
     * where the hot path needs them.
     */
    uint32_t sr[G4MH_SR_BANKS][G4MH_SR_PER_BANK];

    /* --- counters --- */
    uint64_t cycles;
    uint64_t retired;

    /*
     * Guest time, owned by the INTC's timer the way the RISC-V frontend's
     * is owned by the CLINT. Points at whatever the platform is driving.
     */
    const volatile uint64_t *time;

    /*
     * The LL/SC reservation, per core.
     *
     * LDL.W sets it, STC.W consumes it, CLL and any exception drop it, and
     * a store from *any* core to the reserved word clears it -- which is
     * the whole point of the primitive and the one piece of cross-core
     * state in the model. Word granularity, which the architecture leaves
     * to the implementation ("for the link operation, see the hardware
     * manual of the product used").
     */
    uint32_t ll_addr;
    bool     ll_valid;

    /* --- interrupts --- */
    struct g4mh_intc *intc;

    /*
     * Set whenever something may have made an interrupt deliverable: a
     * channel being raised, a write to PSW.ID, or a RETI restoring it. The
     * run loop evaluates only when this is set and clears it when the
     * check comes back empty.
     *
     * volatile because a platform may raise a channel from an ARM
     * interrupt handler while the run loop is executing. The loop must
     * clear this *before* evaluating, not after, or a set performed during
     * the evaluation is lost.
     */
    volatile bool irq_dirty;

    uint32_t coreid;
    uint8_t  state;              /* emu_state_t */

    /*
     * True while any performance channel is enabled.
     *
     * Kept so the interpreter pays one flag test per instruction rather
     * than a loop over eight channels -- the same shape as the RISC-V
     * frontend's fetch_guard, and for the same reason: this is on the
     * retire path, which every guest pays whether it uses the feature or
     * not. Maintained by g4mh_sr_write, the only thing that can enable a
     * channel.
     */
    bool     pm_active;

#if EMU_ENABLE_STATS
    uint32_t exc_count;
#endif

#if EMU_ENABLE_TRACE
    emu_trace_fn trace;
    void        *trace_user;
#endif

    /*
     * Optional interception of TRAP and SYSCALL before they take the
     * architectural exception, so the host test harness can offer the same
     * write/exit services it offers a RISC-V guest. Return true to consume
     * the call. When NULL, TRAP always takes its exception.
     */
    emu_syscall_fn syscall;
    void          *syscall_user;

    const emu_cache_ops_t *cache;

    void *user;                  /* opaque platform pointer */
} g4mh_cpu_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void g4mh_cpu_init(g4mh_cpu_t *c, emu_bus_t *bus, uint32_t coreid);
void g4mh_cpu_reset(g4mh_cpu_t *c, uint32_t reset_pc);

/*
 * Boot protocol, the G4MH counterpart of rv_hart_boot: a stack pointer at
 * the top of guest RAM in r3 (sp), the core id in r6 and the RAM size in
 * r7 -- the first two argument registers of the RH850 calling convention.
 */
void g4mh_cpu_boot(g4mh_cpu_t *c, uint32_t ram_base, uint32_t ram_size);

/* ------------------------------------------------------------------ */
/* Exceptions and interrupts                                           */
/* ------------------------------------------------------------------ */

/*
 * Take an exception.
 *
 * `cause` goes to EIIC or FEIC and `ret_pc` to EIPC or FEPC, chosen by
 * whether the cause is an FE-level one -- which is the whole of what
 * separates the two levels: FE has its own save registers so that an
 * exception occurring inside an EI handler does not destroy the EI return
 * state, and it ignores PSW.ID.
 */
void g4mh_cpu_exception(g4mh_cpu_t *c, g4mh_exc_t cause, uint32_t ret_pc);

/* True if `cause` is taken at FE level rather than EI. */
bool g4mh_exc_is_fe(g4mh_exc_t cause);

/*
 * The channel of a pending, enabled EI interrupt, or -1. Honours PSW.ID
 * and PSW.NP, so it reports nothing while interrupts are masked.
 */
int g4mh_cpu_pending_irq(const g4mh_cpu_t *c);

/* ------------------------------------------------------------------ */
/* Memory (permission-checked, used by the interpreter)                */
/* ------------------------------------------------------------------ */

/*
 * Alignment is checked here rather than in the bus so a misaligned access
 * can be reported as MAE, and so the bus can assume aligned accesses on
 * its fast paths. G4MH allows misaligned data accesses on most parts, but
 * reporting them is the conservative choice: a guest that relies on them
 * gets a clear exception rather than a silently split access.
 */
g4mh_exc_t g4mh_load(g4mh_cpu_t *c, uint32_t addr, uint32_t size,
                     bool sign_extend, uint32_t *out);
g4mh_exc_t g4mh_store(g4mh_cpu_t *c, uint32_t addr, uint32_t size,
                      uint32_t val);

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

/*
 * Break any core's reservation covering `addr`.
 *
 * Called by every store that could be observed by another core. The
 * check is gated on g4mh_any_reservation() so a guest that never takes
 * one pays a single predictable branch -- the same shape as the RISC-V
 * frontend's fetch_guard, and for the same reason: this is on the store
 * path, which every guest pays whether it uses the feature or not.
 */
void g4mh_ll_break(uint32_t addr);

/* True while any core holds a reservation. */
bool g4mh_any_reservation(void);

/* Register a core with the reservation tracker at init. */
void g4mh_ll_register(g4mh_cpu_t *c);

/* LDL.W takes a reservation; STC.W, CLL and every exception drop one. */
void g4mh_ll_take(g4mh_cpu_t *c, uint32_t addr);
void g4mh_ll_drop(g4mh_cpu_t *c);

/* ------------------------------------------------------------------ */
/* System registers                                                    */
/* ------------------------------------------------------------------ */

/*
 * LDSR/STSR. `bank` is the selID. Reads of an unimplemented register give
 * zero and writes are dropped, which is what the architecture specifies
 * for a reserved system register rather than an exception.
 */
uint32_t g4mh_sr_read(const g4mh_cpu_t *c, unsigned bank, unsigned reg);
void     g4mh_sr_write(g4mh_cpu_t *c, unsigned bank, unsigned reg,
                       uint32_t val);

/* ------------------------------------------------------------------ */
/* Floating point                                                      */
/* ------------------------------------------------------------------ */

#if G4MH_EXT_FPU
/*
 * Execute one floating-point instruction. `sub` is instruction bits
 * 26..16, which is what the interpreter's Format X switch already keys
 * on, and reg3 is the field the manual calls reg3 (instruction bits
 * 31..27).
 *
 * Single precision only -- see the note at the top of g4mh_fpu.c for
 * exactly what is and is not implemented, and why the double-precision
 * and 64-bit-integer encodings still raise RIE.
 */
g4mh_exc_t g4mh_fpu_exec(g4mh_cpu_t *c, uint32_t sub, uint32_t reg1,
                         uint32_t reg2, uint32_t reg3);
#endif

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

/*
 * The threaded interpreter, and the IR JIT where the host has a backend
 * for it. g4mh_frontend.c prefers the JIT, which is right for firmware:
 * there it is a speed choice and the backend falls back per instruction.
 *
 * On a host it is a *coverage* choice and the caller must be able to say
 * which it wants -- see the same note in rv32_frontend.c. It matters more
 * here than there: G4MH has no reference model, so the interpreter is the
 * only thing that defines what an answer should be, and a run that
 * silently translated cannot be compared against one that did not.
 */
extern const emu_backend_t g4mh_backend_interp;
#if defined(RV_JIT_X86_64) || defined(EMU_JIT_X86_64)
#  define G4MH_HAVE_JIT 1
extern const emu_backend_t g4mh_backend_jit;
#else
#  define G4MH_HAVE_JIT 0
#endif
extern const emu_backend_t *g4mh_backend;

emu_run_reason_t g4mh_step(g4mh_cpu_t *c);

/* Advance the enabled performance channels by `insns` events. */
void g4mh_pm_tick(g4mh_cpu_t *c, uint32_t insns);

/* ------------------------------------------------------------------ */
/* Code flash                                                          */
/* ------------------------------------------------------------------ */

/*
 * Say where code flash is backed, before bringing a core up.
 *
 * Flash is where code is *executed from*, so on a target it is the host
 * part's own flash holding the guest image: pass that pointer with
 * `writable` false and the region costs no RAM at all, which is the
 * arrangement the RV32 side already uses for its ROM region. A guest
 * write then faults, which is what a real part does and what this
 * frontend cannot otherwise model without a flash sequencer.
 *
 * A platform that does not call this gets a writable arena of
 * G4MH_FLASH_KIB instead -- the host runner needs one, because its ELF
 * loader writes the image straight into guest memory. Firmware builds set
 * G4MH_FLASH_KIB=0 so that arena does not exist.
 */
void g4mh_set_flash(const void *base, uint32_t size, bool writable);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_CPU_H */
