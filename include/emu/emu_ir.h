/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir.h - The intermediate representation between a frontend and a host.
 *
 *   frontend -> IR -> common optimisation -> backend
 *
 * A frontend is then unique per emulated machine and a backend unique per
 * host, and neither knows the other exists. That is the point of the
 * layer: with three frontends (RV32, G4MH, e200z7) and three hosts
 * (x86-64, ARMv7-M, ARMv8-M) the direct arrangement is nine translators,
 * and every optimisation has to be written nine times or not at all.
 *
 * What this buys that the direct translators could not have
 * ---------------------------------------------------------
 * The framework in emu_jit.h already shares the block cache, the dispatch
 * loop and staleness tracking, and emu_x86_64.h already shares the host
 * encoding. What was left unshared -- and what this adds -- is everything
 * that needs to see *more than one instruction at a time*:
 *
 *   - dead flag elimination. G4MH and e200z7 set condition flags on
 *     almost every arithmetic instruction, and on x86-64 materialising
 *     them costs a `seto`/`lahf` pair plus masking. Most of those
 *     definitions are dead, because the next flag-setting instruction
 *     overwrites them before any conditional reads them. Seeing one
 *     instruction ahead deletes the whole sequence. RISC-V has no flags,
 *     so this never came up while RV32 was the only frontend.
 *
 *   - guest register traffic. With the register file in memory, a
 *     dependent pair emits a store and then an immediate reload of the
 *     same slot. Measured on this project's own guests, a quarter to a
 *     third of adjacent instruction pairs are data dependent, and the
 *     round trip is 5-7% of executed host instructions and a similar
 *     share of code size -- which counts twice, because code size is what
 *     sets JIT performance when the cache is 12 KB.
 *
 *   - fusion. Not the textbook RISC-V pairs, which this project measured
 *     and found absent (lui+addi is 0.2% of CoreMark pairs), but the ones
 *     a host actually offers: shift-and-add into one ARM operand, or one
 *     x86 LEA.
 *
 * Deliberate limits
 * -----------------
 * Every value is 32 bits. Both current guests are 32-bit and the third
 * is too, and a type lattice that nothing needs is a cost paid on every
 * translation -- so a float is its IEEE-754 bit pattern in an ordinary
 * temp, and the *operation* says how to read it. That is what lets the
 * optimiser, the register allocator and the dead-store rule apply to
 * floating-point values without knowing they are floats.
 *
 * There is no allocation. Blocks are fixed-size arrays sized for the
 * target, because on a microcontroller these bytes are the guest's.
 *
 * The IR is *not* SSA. Values are numbered temporaries written once by
 * construction -- the builder never reuses a temp within a block -- which
 * gives the passes what SSA would without a phi node or a dominance
 * computation, since a block has one entry and no joins.
 */
#ifndef EMU_IR_H
#define EMU_IR_H

#include "emu_jit.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

/*
 * A block's worth of IR. Sized from the block length the translators
 * already use (64 guest instructions) times the widest lowering, and
 * kept small enough on a target that the buffer is not the reason the
 * guest runs out of RAM.
 */
#if defined(EMU_JIT_THUMB2)
#  define EMU_IR_MAX_INSNS  512u
#  define EMU_IR_MAX_TEMPS  256u
#else
#  define EMU_IR_MAX_INSNS  2048u
#  define EMU_IR_MAX_TEMPS  1024u
#endif

/* ------------------------------------------------------------------ */
/* Guest condition flags                                               */
/* ------------------------------------------------------------------ */

/*
 * The flags a guest defines and uses, as a bitmask, in *architecture
 * neutral* terms. G4MH's Z/S/OV/CY and PowerPC's CR0 LT/GT/EQ/SO both map
 * onto these; RISC-V defines none of them and its lowering never sets a
 * flag mask at all, which is what makes the dead-flag pass free for it.
 *
 * The mapping is the frontend's business. What the IR guarantees is only
 * that a definition of a flag is dead if every later instruction in the
 * block redefines it before reading it, and that the block boundary
 * counts as a read of everything -- a flag can always be examined by the
 * next block or by an interrupt handler.
 */
#define EMU_IR_F_Z   (1u << 0)   /* result was zero          */
#define EMU_IR_F_S   (1u << 1)   /* result was negative      */
#define EMU_IR_F_V   (1u << 2)   /* signed overflow          */
#define EMU_IR_F_C   (1u << 3)   /* carry or borrow          */
#define EMU_IR_F_ALL (EMU_IR_F_Z | EMU_IR_F_S | EMU_IR_F_V | EMU_IR_F_C)

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

typedef enum emu_ir_op {
    EMU_IR_NOP = 0,

    /*
     * Guest register file access, explicit so the optimiser can see it.
     * GET reads guest register `a` into `dst`; PUT writes `a`'s value
     * into guest register `imm`. Making these ordinary instructions
     * rather than an implicit part of every operand is the whole reason
     * the redundant round trip is visible at all.
     */
    EMU_IR_GET,
    EMU_IR_PUT,

    EMU_IR_CONST,          /* dst = imm                                */
    EMU_IR_MOV,            /* dst = a                                  */

    /* Arithmetic and logic: dst = a op b. */
    EMU_IR_ADD, EMU_IR_SUB, EMU_IR_AND, EMU_IR_OR, EMU_IR_XOR,
    EMU_IR_SHL, EMU_IR_SHR, EMU_IR_SAR, EMU_IR_ROTL,
    EMU_IR_MUL, EMU_IR_MULHS, EMU_IR_MULHU,

    /* Same, with `imm` as the right operand. */
    EMU_IR_ADDI, EMU_IR_ANDI, EMU_IR_ORI, EMU_IR_XORI,
    EMU_IR_SHLI, EMU_IR_SHRI, EMU_IR_SARI, EMU_IR_ROTLI,

    EMU_IR_NEG,            /* dst = -a                                 */
    EMU_IR_NOT,            /* dst = ~a                                 */

    /*
     * Bit and byte manipulation. Present as first-class operations
     * rather than as open-coded shift-and-mask sequences because every
     * host has them -- x86 has bswap/bsf/bsr, ARMv7-M has REV/REV16/CLZ
     * and RBIT -- and recovering them from a shift sequence afterwards
     * is pattern matching that would have to be written per host.
     */
    EMU_IR_BSWAP32,        /* reverse all four bytes                   */
    EMU_IR_BSWAP16,        /* reverse bytes within each halfword       */
    EMU_IR_HSWAP,          /* exchange the two halfwords               */
    EMU_IR_CLZ,            /* count leading zeros, 32 for a zero input */
    EMU_IR_CTZ,            /* count trailing zeros, 32 for zero        */
    EMU_IR_POPCNT,

    /*
     * Extract or deposit one bit of a *value*. `b` holds the bit number;
     * using a value rather than an immediate keeps the register and
     * immediate forms of a guest's bit instructions on one path.
     */
    EMU_IR_BEXT,           /* dst = (a >> b) & 1                       */
    EMU_IR_BSET,           /* dst = a | (1 << b)                       */
    EMU_IR_BCLR,           /* dst = a & ~(1 << b)                      */
    EMU_IR_BINV,           /* dst = a ^ (1 << b)                       */

    /*
     * Bit operations on *memory*: the read-modify-write class.
     *
     * A separate class from the value forms above rather than a LOAD, a
     * BSET and a STORE, for three reasons that all come from what the
     * guests actually define:
     *
     *   - it is one access as far as the guest is concerned. Spelling it
     *     as three IR instructions invites a later pass to reorder or
     *     eliminate one half of it, and on an address that is a
     *     peripheral register a spurious extra read is a side effect.
     *   - the Z flag reports the bit as it was *before* the change, which
     *     is a value that exists nowhere in a load/modify/store
     *     decomposition once the modify has happened.
     *   - hosts implement it directly. x86 has bts/btr/btc with a memory
     *     operand and ARM has the load-exclusive pair, so a backend that
     *     was handed three instructions would have to pattern-match them
     *     back into one.
     *
     * `a` is the address, `b` the bit number, `imm` a byte displacement
     * added to the address, and `aux` the access width. All four define
     * EMU_IR_F_Z and nothing else. TST alone does not write memory --
     * which also means it alone cannot raise a store-side fault.
     *
     * G4MH's SET1/CLR1/NOT1/TST1 are exactly this, in both their
     * immediate-bit and register-bit encodings.
     */
    EMU_IR_BITOP_SET,
    EMU_IR_BITOP_CLR,
    EMU_IR_BITOP_INV,
    EMU_IR_BITOP_TST,

    /* Sign and zero extension from a narrower width. */
    EMU_IR_SEXT8, EMU_IR_SEXT16, EMU_IR_ZEXT8, EMU_IR_ZEXT16,

    /*
     * Memory. `imm` is a byte displacement added to `a`; `b` is the data
     * for a store. Width and signedness travel in `aux`.
     */
    EMU_IR_LOAD,
    EMU_IR_STORE,

    /*
     * Flags, split from the operation that produced the value.
     *
     * This split is what makes the dead-flag pass possible: an ADD that
     * also sets flags is an EMU_IR_ADD followed by an EMU_IR_SETF, and
     * deleting the SETF leaves a perfectly good ADD. Fusing them into
     * one flag-setting opcode would mean the pass had to rewrite the
     * opcode instead, once per opcode per host.
     *
     * `aux` says how to derive them (see emu_ir_flagsrc_t), `a` is the
     * result and `b` and `imm` the operands where the derivation needs
     * them.
     */
    EMU_IR_SETF,

    /*
     * dst = condition `aux` evaluated against the current flags. The
     * backend may fold this into a host setcc.
     */
    EMU_IR_GETCOND,

    /*
     * dst = (a `aux` b) ? 1 : 0, comparing two values directly rather
     * than consulting the guest's flags.
     *
     * Separate from GETCOND because a guest without condition flags has
     * no flags to consult: RISC-V's SLT and SLTU are exactly this, and
     * expressing them as "set flags, then read them" would invent a flag
     * word the architecture does not have and defeat the dead-flag pass
     * for the one frontend that should pay nothing for it.
     */
    EMU_IR_SETCC,

    /* dst = c ? a : b, with the condition in `aux`. */
    EMU_IR_SELECT,

    /*
     * Floating point, single precision.
     *
     * The register file is separate from the integer one on both guests
     * that have floats, so it gets its own accessors rather than an
     * offset trick -- and everything between them is an ordinary temp,
     * which is what makes FMV.X.W an FGET and a PUT with nothing in the
     * middle.
     *
     * `aux` carries the rounding mode (EMU_IR_FRM_*) on every operation
     * whose result depends on one, resolved by the *frontend* at
     * translation: a guest's "dynamic" mode is a register read, and
     * baking it in is what lets a backend with no equivalent for a
     * particular mode decline that block instead of quietly rounding
     * differently. This project has made that mistake once already, on
     * ARM, where the old run-time table mapped RISC-V's ties-away to
     * ties-even and no test noticed.
     *
     * A block that contains any of these accumulates the host's sticky
     * exception flags and hands them to the frontend once, at the block
     * epilogue -- see emu_ir_target_t.fp_flags. Per-operation would be
     * correct and is what the flags mean, but every path out of a block
     * already passes through the epilogue, so once is enough and the
     * difference is invisible to a guest that can only read them with a
     * CSR instruction no backend translates.
     */
    EMU_IR_FGET,           /* dst = freg[imm]                          */
    EMU_IR_FPUT,           /* freg[imm] = a                            */

    EMU_IR_FADD, EMU_IR_FSUB, EMU_IR_FMUL, EMU_IR_FDIV,
    EMU_IR_FSQRT,          /* dst = sqrt(a)                            */

    /*
     * Minimum and maximum, which are *not* a compare and a select: both
     * guests define the result for a NaN operand as the other operand,
     * where every host's instruction of that name returns its second.
     */
    EMU_IR_FMIN, EMU_IR_FMAX,

    /* Sign injection; `aux` is EMU_IR_FSGNJ_*. Pure bit manipulation. */
    EMU_IR_FSGNJ,

    /*
     * dst = (a `aux` b) ? 1 : 0 with `aux` an EMU_IR_C_EQ/LT/LE, and
     * unordered always false. Separate from EMU_IR_SETCC because the
     * unordered case has no integer analogue.
     */
    EMU_IR_FCMP,

    /* Conversions. `aux` is a rounding mode plus EMU_IR_F_UNSIGNED. */
    EMU_IR_FCVT_TO_I,
    EMU_IR_FCVT_FROM_I,

    /* The ten-bit classification both guests spell the same way. */
    EMU_IR_FCLASS,

    /*
     * Anything the IR does not model: a call to a frontend helper with
     * the cpu pointer and up to three arguments. The escape hatch that
     * keeps a block whole -- this project has measured that declining an
     * instruction costs more than translating it badly, because ending
     * the block fragments hot code.
     */
    EMU_IR_HELPER,

    /*
     * A helper that may take a guest trap.
     *
     * Same call as EMU_IR_HELPER, but a non-zero return means the helper
     * entered a trap and the block must stop *there*: pc already points
     * at the handler, and running the rest of the block would execute
     * instructions the trap preempted. `dst` receives the return value.
     *
     * This is why loads and stores cannot simply be EMU_IR_HELPER. The
     * distinction is invisible until a guest actually faults, which is
     * the sort of thing that survives a whole test suite.
     */
    EMU_IR_HELPER_TRAP,

    /*
     * Control flow. All of these end a block.
     *
     * EXIT writes the guest pc from `a` (or `imm` when `a` is
     * EMU_IR_NO_TEMP) and returns. EXIT_IF does the same under a
     * condition, falling through otherwise.
     */
    /*
     * Write the guest pc without ending the block, from `a` or from
     * `imm` when `a` is EMU_IR_NO_TEMP.
     *
     * Distinct from EMU_IR_EXIT because a block that folds several guest
     * instructions still has to leave the pc right after each one: a
     * trap, an interrupt or the interpreter picking up where the block
     * stopped all read it, and a pc that only became correct at the end
     * of the block would send every one of them to the wrong address.
     */
    /*
     * One guest instruction retired.
     *
     * Emitted by the frontend after each instruction it lowers, and
     * lowered to whatever the host uses to bump the block's retired
     * count. It has to be per instruction rather than a total added at
     * the end of the block, because a block can leave early -- a taken
     * branch, a trapping helper -- and a count placed after the exit
     * label charges the guest for instructions it never ran. That reads
     * as the guest retiring nearly twice what it executed, while
     * computing entirely correct answers.
     */
    EMU_IR_RETIRE,

    EMU_IR_SETPC,

    EMU_IR_EXIT,
    EMU_IR_EXIT_IF,

    EMU_IR_OP_COUNT
} emu_ir_op_t;

/* How EMU_IR_SETF derives the flags it defines. */
typedef enum emu_ir_flagsrc {
    EMU_IR_FS_LOGIC = 0,   /* Z and S from the result; V and C cleared */
    EMU_IR_FS_ADD,         /* full add semantics, needs both operands  */
    EMU_IR_FS_SUB,         /* full subtract; C is a borrow             */
    EMU_IR_FS_ZS           /* Z and S only, leaving V and C alone      */
} emu_ir_flagsrc_t;

/* Conditions, for GETCOND, SELECT and EXIT_IF. */
typedef enum emu_ir_cond {
    EMU_IR_C_EQ = 0, EMU_IR_C_NE,
    EMU_IR_C_LT, EMU_IR_C_GE,      /* signed   */
    EMU_IR_C_LTU, EMU_IR_C_GEU,    /* unsigned */
    EMU_IR_C_LE, EMU_IR_C_GT,
    EMU_IR_C_LEU, EMU_IR_C_GTU,
    EMU_IR_C_ALWAYS
} emu_ir_cond_t;

/*
 * Rounding modes, in the `aux` of every FP operation that has one. The
 * numbering is RISC-V's because it is the guest that has to name them;
 * a host without an equivalent for one declines the operation.
 */
#define EMU_IR_FRM_RNE  0u     /* to nearest, ties to even             */
#define EMU_IR_FRM_RTZ  1u     /* toward zero                          */
#define EMU_IR_FRM_RDN  2u     /* toward -inf                          */
#define EMU_IR_FRM_RUP  3u     /* toward +inf                          */
#define EMU_IR_FRM_RMM  4u     /* to nearest, ties away -- x86 and ARM
                                * have no mode for this               */
#define EMU_IR_FRM(aux)     ((aux) & 7u)

/* Conversion is to or from an unsigned integer. */
#define EMU_IR_F_UNSIGNED   (1u << 3)

/* EMU_IR_FSGNJ variants, in `aux`. */
#define EMU_IR_FSGNJ_J   0u    /* take b's sign            */
#define EMU_IR_FSGNJ_N   1u    /* take b's sign, negated   */
#define EMU_IR_FSGNJ_X   2u    /* xor the two signs        */

/*
 * The host's sticky exception flags, as emu_ir_target_t.fp_flags
 * receives them. Architecture-neutral, because x86 and ARM order their
 * own differently from each other and from both guests -- and this
 * project has already spent a debugging session on exactly that: ARM is
 * IOC,DZC,OFC,UFC,IXC from bit 0 and RISC-V is NX,UF,OF,DZ,NV, so a
 * backend that passed its own encoding through would be right on one
 * host and wrong on the other.
 */
#define EMU_IR_FE_INEXACT   (1u << 0)
#define EMU_IR_FE_UNDERFLOW (1u << 1)
#define EMU_IR_FE_OVERFLOW  (1u << 2)
#define EMU_IR_FE_DIVBYZERO (1u << 3)
#define EMU_IR_FE_INVALID   (1u << 4)

/* Access widths for LOAD and STORE, in `aux`. */
#define EMU_IR_MEM_SIZE(aux)   ((aux) & 7u)
#define EMU_IR_MEM_SIGNED      (1u << 3)
#define EMU_IR_MEM_AUX(sz, sg) (uint8_t)((sz) | ((sg) ? EMU_IR_MEM_SIGNED : 0u))

/* No operand in this slot. */
#define EMU_IR_NO_TEMP 0xFFFFu

typedef struct emu_ir_insn {
    uint8_t  op;        /* emu_ir_op_t                                 */
    uint8_t  aux;       /* op-specific: width, flag source, condition  */
    uint16_t dst;       /* temp written, or EMU_IR_NO_TEMP             */
    uint16_t a, b;      /* temps read, or EMU_IR_NO_TEMP               */
    uint32_t imm;

    /*
     * Which guest flags this instruction defines, and -- filled in by
     * the optimiser, not by the frontend -- which of those any later
     * instruction actually reads. A SETF whose `live` is zero is dead.
     */
    uint8_t  defs;
    uint8_t  live;

    /*
     * How many surviving instructions read this one's result, saturating
     * at 255. Filled in by the optimiser, after everything that deletes
     * code has run, so it counts real readers rather than ones about to
     * disappear.
     *
     * A backend needs this to fuse safely. Folding a shift into the
     * operand of the instruction that consumes it only pays if the shift
     * itself is then not emitted, and dropping it is sound exactly when
     * nothing else reads it -- `uses == 1`. Without the count a fusion
     * computes the value twice, which is larger and slower than not
     * fusing at all.
     */
    uint8_t  uses;

    /* Set by the optimiser; the backend skips these. */
    bool     dead;
} emu_ir_insn_t;

typedef struct emu_ir_block {
    emu_ir_insn_t insn[EMU_IR_MAX_INSNS];
    uint32_t      count;
    uint32_t      next_temp;

    /* Guest instructions folded in so far, for the retired count. */
    uint32_t      guest_insns;

    /* Set when the block ran out of room; it is then discarded. */
    bool          overflow;
} emu_ir_block_t;

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

void     emu_ir_reset(emu_ir_block_t *b);
uint16_t emu_ir_temp(emu_ir_block_t *b);

/*
 * Append an instruction and return the temp it writes (or
 * EMU_IR_NO_TEMP). `dst` of EMU_IR_NO_TEMP asks for a fresh temp for the
 * ops that produce a value.
 */
uint16_t emu_ir_emit(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                     uint16_t a, uint16_t bb, uint32_t imm, uint8_t defs);

/* Shorthands for the shapes that appear most. */
uint16_t emu_ir_get(emu_ir_block_t *b, uint32_t guest_reg);
void     emu_ir_put(emu_ir_block_t *b, uint32_t guest_reg, uint16_t val);
uint16_t emu_ir_const(emu_ir_block_t *b, uint32_t v);
uint16_t emu_ir_alu(emu_ir_block_t *b, emu_ir_op_t op, uint16_t a,
                    uint16_t bb);

/*
 * A memory bit operation. `op` must be one of the EMU_IR_BITOP_* class.
 * Defines EMU_IR_F_Z from the bit's value before the operation.
 */
void emu_ir_bitop(emu_ir_block_t *b, emu_ir_op_t op, uint16_t addr,
                  uint16_t bit, uint32_t disp, uint8_t width);

/* ------------------------------------------------------------------ */
/* Optimisation                                                        */
/* ------------------------------------------------------------------ */

typedef struct emu_ir_opt_stats {
    uint32_t single_use;      /* values with exactly one reader        */
    uint32_t flags_removed;   /* dead EMU_IR_SETF deleted              */
    uint32_t gets_removed;    /* guest register reloads elided         */
    uint32_t puts_removed;    /* guest register stores made redundant  */
    uint32_t folded;          /* constant-folded instructions          */
    uint32_t dead_removed;    /* values nothing consumed               */
} emu_ir_opt_stats_t;

/* Defined under Lowering below; the passes read one field of it. */
typedef struct emu_ir_target emu_ir_target_t;

/*
 * Run every pass over the block. Host-independent by construction: a
 * pass that needed to know the host would belong in the backend.
 *
 * `live_out` names the flags that must survive to the end of the block.
 * Pass EMU_IR_F_ALL unless the frontend can prove otherwise -- a flag
 * examined by the next block or by an interrupt handler is live, and
 * getting this wrong deletes a flag definition a later block depends on,
 * which no test that runs one block at a time would catch.
 *
 * `t` is the *guest* description, not the host's: the passes need it for
 * one thing only, which is which guest registers are hardwired to zero.
 * That is a property of the architecture rather than of the machine the
 * block is being compiled for, so taking it here does not make the
 * passes host-dependent. It may be NULL, meaning no register is
 * hardwired -- but see the note on pass_reg_traffic before passing NULL
 * for an architecture that has one.
 */
void emu_ir_optimise(emu_ir_block_t *b, const emu_ir_target_t *t,
                     uint8_t live_out, emu_ir_opt_stats_t *stats);

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

/*
 * What a backend needs to know about the frontend it is lowering for:
 * where the guest register file and flags live, and what to call for
 * EMU_IR_HELPER. Supplied once, not per instruction.
 */
typedef struct emu_ir_target {
    /* Byte offset of guest register `n` within emu_cpu_t. */
    uint32_t (*reg_offset)(uint32_t n);

    /* Byte offset of the word holding the guest's condition flags. */
    uint32_t flags_offset;

    /*
     * Where each IR flag sits in that word. Indexed by bit position of
     * EMU_IR_F_*; a guest that lacks a flag gives it 0.
     */
    uint32_t flag_bit[4];

    /* Guest register `n` reads as zero and ignores writes. */
    bool (*reg_is_zero)(uint32_t n);

    /* Byte offset of the guest pc. */
    uint32_t pc_offset;

    /* Helpers, indexed by the `imm` of EMU_IR_HELPER. */
    const void *const *helpers;
    uint32_t           helper_count;

    /*
     * Guest memory. A guest address is not a host address -- it goes
     * through a bus with regions, permissions and passthrough windows --
     * so a backend cannot emit a plain load. These are what let
     * EMU_IR_LOAD, EMU_IR_STORE and the EMU_IR_BITOP_* class be lowered
     * generically rather than each frontend routing them to a helper of
     * its own shape.
     *
     * `spec` is EMU_IR_MEM_AUX: size in bytes plus a sign-extend flag.
     * Both return non-zero if the access trapped, in which case the trap
     * has already been entered and the block must stop. Both go to the
     * same load and store the interpreter uses, so everything those do
     * beyond the access itself -- reservations, PMP, translation -- is
     * had for free.
     *
     * The guest pc must already point at the faulting instruction when
     * either is called, because that is what the trap records. Frontends
     * emit EMU_IR_SETPC before a memory operation for exactly that
     * reason, rather than only after each instruction.
     */
    uint32_t (*load)(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                     uint32_t *out);
    uint32_t (*store)(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                      uint32_t val);

    /*
     * Floating point. NULL where the guest has none, which is what a
     * backend tests before lowering any of the FP class -- the same way
     * a NULL `load` makes it decline memory.
     *
     * `freg_offset` locates the FP register file inside emu_cpu_t.
     * `fp_flags` accumulates one block's worth of host sticky flags,
     * given in EMU_IR_FE_* terms, into wherever the guest keeps them;
     * it is called once per block that used any FP operation, on the
     * single path every exit takes.
     *
     * `fp_dirty` is the hook for guests that track extension state --
     * RISC-V's mstatus.FS -- and is called from the same place. A
     * frontend without one leaves it NULL.
     */
    uint32_t (*freg_offset)(uint32_t n);
    void     (*fp_flags)(emu_cpu_t *cpu, uint32_t flags);
} emu_ir_target_t;

/*
 * Lower an optimised block to host code through emu_jit_emit*().
 * Returns false if the buffer overflowed, in which case the caller
 * discards the block exactly as it would a declined translation.
 */
bool emu_ir_lower(const emu_ir_block_t *b, const emu_ir_target_t *t);

/*
 * Can this backend lower this operation, with this `aux`?
 *
 * The frontend asks *before* emitting, so that an operation only some
 * hosts have becomes a helper call rather than a declined block. Without
 * it a frontend has two bad choices: emit the op and lose the whole
 * block wherever it is unsupported, or never emit it and lose the
 * lowering wherever it exists -- and this project has already measured
 * that ending a block costs more than translating one instruction badly.
 *
 * `aux` is part of the question, not decoration. A host may have an
 * instruction for an operation and no encoding for one of its rounding
 * modes: x86 and ARM both lack RISC-V's ties-away, and a backend that
 * answered on the opcode alone would be asked to emit it anyway.
 *
 * Answering `true` is a promise. emu_ir_lower returning false for an
 * operation this said it could do discards a block the frontend built on
 * the strength of the answer.
 */
bool emu_ir_can_lower(emu_ir_op_t op, uint8_t aux);

/* ------------------------------------------------------------------ */
/* Register allocation                                                 */
/* ------------------------------------------------------------------ */

/* This temp did not get a register and lives in its frame slot. */
#define EMU_IR_NO_REG      0xFFu

/* As many host registers as any backend here offers the allocator. */
#define EMU_IR_MAX_HOST_REGS 8u

/*
 * Assign temps to host registers, linear scan over live intervals.
 *
 * Host-independent because the only thing it needs from the host is how
 * many registers there are: which ones they are, and what it costs to
 * reach them, is the backend's business. `assign[t]` comes back as an
 * index below `nregs`, or EMU_IR_NO_REG for a temp that stays in the
 * frame. The return value is how many registers were touched, and the
 * set is always 0..n-1 -- the scan takes the lowest free index, so a
 * backend can save exactly that many and no more.
 *
 * Linear scan is not a compromise here, it is the shape of the problem.
 * A temp is written once by construction and a block has one entry and
 * no joins, so a live range is a single interval [def, last use] and the
 * intervals arrive already sorted by start point. Graph colouring would
 * build an interference graph that is by construction an interval graph
 * and colour it optimally -- for the same answer, at translation time,
 * which is already 48-54% of host cycles at the code-cache size a
 * microcontroller gets.
 *
 * Two rules deserve stating because they are where this could go wrong:
 *
 *   - a register is reused only once its previous occupant's last use is
 *     *strictly* before the new definition. Sharing on the boundary
 *     instruction would be sound only if every lowering read its
 *     operands before writing its destination, which is true today and
 *     is not a property the IR guarantees.
 *   - the registers a backend offers must be callee-saved on its host.
 *     Every value being in the frame is what made a helper call clobber
 *     nothing, and allocation gives that up; the alternative -- spilling
 *     around calls -- would cost more than it saves in blocks that
 *     average four guest instructions.
 *
 * Temps whose single reader is the very next live instruction are
 * deliberately left unallocated: the backends' reload elision already
 * hands those over in the scratch register for nothing, and spending one
 * of four registers on a value that lives for one instruction is exactly
 * the wrong trade.
 */
uint32_t emu_ir_regalloc(const emu_ir_block_t *b, uint32_t nregs,
                         uint8_t *assign);

/* ------------------------------------------------------------------ */
/* What a host's jit.c needs from a frontend                           */
/* ------------------------------------------------------------------ */

/*
 * Everything that differs between one frontend/host pair and another,
 * gathered so the pair itself does not need a file.
 *
 * Before this, each pair had its own translator: the same three-line
 * pipeline, the same block cache adapters, and a handful of genuinely
 * frontend-specific callbacks, copied per pair. Three frontends and
 * three hosts is nine copies of that. What is left below is the handful
 * -- everything a host cannot know and the IR does not carry -- and one
 * jit.c per host consumes it.
 */
typedef struct emu_ir_frontend {
    const char *name;

    /* Guest instructions to IR; see the per-frontend header. */
    uint32_t (*translate)(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

    /* Where this guest keeps its registers, flags, pc and memory. */
    const emu_ir_target_t *target;

    /*
     * The parts of emu_jit_ops_t only the frontend can answer. They are
     * held here rather than built by each pair, which is what stops the
     * dispatch loop's contract from being restated once per pair -- and
     * that contract is where this project has found most of its bugs.
     */
    void (*bind)(emu_cpu_t *cpu, emu_jit_hot_t *out);
    const emu_backend_t *interp;
    bool (*is_idle)(emu_cpu_t *cpu);
    bool (*wake)(emu_cpu_t *cpu);
    bool (*take_irq)(emu_cpu_t *cpu);
    void (*count)(emu_cpu_t *cpu, uint32_t n);
    void (*after_interp)(emu_cpu_t *cpu);

    /* Code cache to ask the framework for. */
    uint32_t code_bytes;

    /*
     * How much of this guest's state a differential check compares --
     * enough to cover the register file, pc and flags, and no more. Not
     * the whole struct: that carries bus pointers, cycle counters and
     * scratch that the two paths are not required to agree on.
     */
    uint32_t diff_state_bytes;
} emu_ir_frontend_t;

/*
 * Execute an optimised block directly, without emitting anything.
 *
 * The backend's other execution strategy, and its own reference: it
 * shares nothing with emu_ir_lower but this header, so running a block
 * both ways and comparing guest state is a differential check between
 * two independent implementations rather than against a hand-written
 * expectation.
 *
 * Returns false for a block containing something it does not model, in
 * which case the caller has made no progress and nothing was changed.
 */
bool emu_ir_interp(const emu_ir_block_t *b, emu_cpu_t *cpu,
                   const emu_ir_target_t *t);

#ifdef __cplusplus
}
#endif

#endif /* EMU_IR_H */
