/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_ir.c - random IR blocks: the passes and the x86-64 lowering
 * against the IR interpreter on the block as it was built.
 *
 * The other targets reach the IR through a frontend, so they build only
 * what a translator emits and check the optimised block against the
 * frontend's interpreter. This one builds the IR itself, from the
 * input, in every order the contract in emu_ir.h allows -- a GET after
 * a PUT of the zero register, a load whose result nothing reads, a
 * register that is the pc, a helper that writes the flags -- and runs
 * each block three ways from one initial state:
 *
 *   want  emu_ir_interp on the block as built, before any pass
 *   opt   emu_ir_interp on the block after emu_ir_optimise
 *   got   the optimised block lowered to x86-64 and called
 *
 * `want` against `got` is the verdict, compared on the register file,
 * the flags, the pc, the FP registers, memory, a device, every helper
 * call in order, the trap and the retired count. `opt` says which half
 * is wrong: a pass when it differs from `want`, the lowering when it
 * differs from `got`. Running the reference on the *optimised* IR, as
 * EMU_JIT_DIFF does, cannot see a defect in a pass -- which is how the
 * zero-register forwarding passed it (docs/lessons.md).
 *
 * The machine is made up, and small on purpose: a page of RAM, a page
 * of read-only image, a four-byte device whose reads count and whose
 * writes are logged -- so a deleted or repeated access shows -- and
 * four helpers. Memory is reached through the target's load and store,
 * through the RAM window inlined in full or in part, with the image
 * window, or through a four-entry data TLB, as the input chooses.
 *
 * What it cannot see: the FP exception flags (the reference computes
 * none), FSQRT and FMA (the reference declines them), how EMU_IR_VFOP
 * rounds (its reference is C's floats on the same FPU, f_vfop), and the Thumb-2
 * lowering, which needs a board -- where this same file runs the corpus
 * built here, compiled with EMU_IR_BOARD into a firmware of its own
 * (tests/ir-board, scripts/ir-board.sh): the Thumb-2 lowering against
 * the same reference, on the inputs that reached what they reached on
 * x86-64.
 */

#include "emu/emu_dtlb.h"
#include "emu/emu_ir.h"
#include "emu/emu_jit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(EMU_IR_BOARD)
#if !defined(EMU_HOST_JIT_THUMB2)
#error "the board's fuzz-ir runs the Thumb-2 lowering"
#endif
/* The replay's own, in tests/ir-board/main.c: a difference is counted
 * and the next input runs, where on the host the process stops. */
void fuzz_ir_board_differ(void);
void fuzz_ir_stats(void);
#else
#include <sys/mman.h>
#if !defined(EMU_HOST_JIT_X86_64)
#error "fuzz-ir runs the x86-64 lowering"
#endif
#endif

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define PAGE 4096u
#define RAM_BASE 0x00001000u
#define DEV_BASE 0x00003000u
#define ROM_BASE 0x00008000u
#define START_PC 0x00000400u
#define TRAP_PC 0x0000E000u /* where a faulting access sends the pc */
#define HTRAP_PC 0x0000E100u /* and a trapping helper */

/* The frontend's own access bit: this one must be naturally aligned. */
#define F_ALIGNED (1u << 4)

typedef struct {
    uint32_t r[16];
    uint32_t flags;
    uint32_t pc;
    uint64_t f[8];
    uint32_t cause; /* address of the access, or argument, that trapped */
    uint32_t cause_pc; /* the pc it trapped at                          */
    uint32_t hlog; /* every helper call, hashed in order               */
    uint32_t fe; /* FP flags; only compiled code reports them       */
    uint8_t guard; /* the store guard: inlined stores are illegal    */
    uint8_t pad[7];
    emu_dtlb_entry_t tlb[4];
    uint8_t v[32][EMU_IR_VREG_BYTES]; /* EMU_IR_VOP's registers */
    uint32_t vsat; /* the narrowing clips' saturation word */
} fcpu_t;

static uint8_t g_ram[PAGE];
static uint8_t g_ram_odd[PAGE - 3u]; /* RAM when the input asks for odd */
static uint8_t g_rom[PAGE];
static uint8_t *g_mem = g_ram; /* the RAM this input has */
static uint32_t g_mem_size = PAGE;
static uint32_t g_dev_reads; /* a read returns this, then it counts */
static uint32_t g_dev_log; /* every write, hashed in order         */

static struct {
    unsigned long long inputs, compared, declined, unlowered, loops, traps,
        helpers, mem_helper, dev;
} g_st;

static uint32_t fault(fcpu_t *c, uint32_t addr)
{
    c->cause = addr;
    c->cause_pc = c->pc;
    c->pc = TRAP_PC;
    return 1u;
}

/* RAM for both, the image for loads only; NULL if any byte is outside. */
static uint8_t *backing(uint32_t addr, uint32_t size, bool store)
{
    if (addr >= RAM_BASE && size <= g_mem_size &&
        addr - RAM_BASE <= g_mem_size - size) {
        return &g_mem[addr - RAM_BASE];
    }
    if (!store && addr >= ROM_BASE && addr - ROM_BASE <= PAGE - size) {
        return &g_rom[addr - ROM_BASE];
    }
    return NULL;
}

static bool is_dev(uint32_t addr, uint32_t size)
{
    return addr >= DEV_BASE && addr - DEV_BASE <= 4u - size;
}

static bool bad_access(uint32_t addr, uint32_t spec, uint32_t size)
{
    if (size != 1u && size != 2u && size != 4u) {
        return true;
    }
    return (spec & F_ALIGNED) != 0u && (addr & (size - 1u)) != 0u;
}

static uint32_t f_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                       uint32_t *out)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);
    uint32_t v = 0u;

    g_st.mem_helper++;
    if (bad_access(addr, spec, size)) {
        return fault(c, addr);
    }
    if (is_dev(addr, size)) {
        g_st.dev++;
        v = (0xD15C0000u + g_dev_reads++) >> (8u * (addr - DEV_BASE));
    } else {
        const uint8_t *const p = backing(addr, size, false);

        if (p == NULL) {
            return fault(c, addr);
        }
        for (uint32_t i = 0; i < size; i++) {
            v |= (uint32_t)p[i] << (8u * i);
        }
    }
    if (size < 4u) {
        v &= (1u << (8u * size)) - 1u;
        if ((spec & EMU_IR_MEM_SIGNED) != 0u &&
            (v & (1u << (8u * size - 1u))) != 0u) {
            v |= ~((1u << (8u * size)) - 1u);
        }
    }
    *out = v;
    return 0u;
}

static uint32_t f_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                        uint32_t val)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    g_st.mem_helper++;
    if (bad_access(addr, spec, size)) {
        return fault(c, addr);
    }
    if (is_dev(addr, size)) {
        g_st.dev++;
        g_dev_log = (g_dev_log ^ addr ^ (val << 3) ^ size) * 0x01000193u;
        return 0u;
    }
    uint8_t *const p = backing(addr, size, true);

    if (p == NULL) {
        return fault(c, addr);
    }
    for (uint32_t i = 0; i < size; i++) {
        p[i] = (uint8_t)(val >> (8u * i));
    }
    return 0u;
}

/*
 * Helpers, called as both lowerings call them. Between them they read
 * the register file, the pc and the flags, and write a register and the
 * flags -- what a frontend's helper may do, and what the passes must
 * therefore treat a call as doing.
 */
/* And the third argument, `aux`: emitted non-zero, so it is passed. */
static uint32_t h_mix(emu_cpu_t *cpu, uint32_t a, uint32_t b, uint32_t aux)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;

    g_st.helpers++;
    c->hlog = (c->hlog ^ a ^ (b << 1) ^ (aux << 9)) * 0x01000193u;
    return a * 31u + b + aux;
}

static const emu_ir_target_t *g_tgt; /* this input's, for h_vmems and h_poke */

static uint32_t h_peek(emu_cpu_t *cpu, uint32_t a, uint32_t b)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;

    g_st.helpers++;
    c->hlog = (c->hlog ^ c->pc ^ (c->flags << 7)) * 0x01000193u;
    return c->r[a & 15u] + b;
}

static uint32_t h_poke(emu_cpu_t *cpu, uint32_t a, uint32_t b)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;

    g_st.helpers++;
    c->hlog = (c->hlog ^ 0x55u ^ a) * 0x01000193u;
    c->r[a & 15u] = b;
    /* the flag word's own bits only, where the target says it holds no other */
    c->flags ^= (b ^ (b >> 28)) & ((g_tgt != NULL && g_tgt->flags_alone) ? 0xF0000000u : 0xF000000Fu);
    return a ^ b;
}

/* The trapping one: non-zero means it entered a trap. */
static uint32_t h_trap(emu_cpu_t *cpu, uint32_t a, uint32_t b)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;

    g_st.helpers++;
    c->hlog = (c->hlog ^ 0xAAu ^ b) * 0x01000193u;
    if ((a & 7u) == 0u) {
        c->cause = a;
        c->cause_pc = c->pc;
        c->pc = HTRAP_PC;
        return 1u;
    }
    return 0u;
}

/*
 * EMU_IR_VMEM's helper and its reference: vl elements of the group at
 * vd, a byte at a time through the target's own accessors -- so a device,
 * the image or a fault is whatever a byte load or store there is. c is
 * the layout: vd, the element's bytes in bits 8-15, a store in bit 16.
 * A fault stops it where it was, as a frontend's leaves the elements
 * before the one that faulted done.
 */
static uint32_t h_vmem(emu_cpu_t *cpu, uint32_t c, uint32_t addr, uint32_t vl)
{
    fcpu_t *const f = (fcpu_t *)(void *)cpu;
    uint8_t *const reg = (uint8_t *)f->v + (c & 31u) * EMU_IR_VREG_BYTES;
    const uint32_t eb = (c >> 8) & 0xFFu;
    const bool store = (c & 0x10000u) != 0u;

    /*
     * Not logged: the inline copy is this access without the call, so
     * the call itself must leave nothing a comparison can see.
     */
    g_st.helpers++;
    for (uint32_t i = 0; i < vl * eb; i++) {
        uint32_t v = reg[i];

        if (store) {
            if (f_store(cpu, addr + i, EMU_IR_MEM_AUX(1u, 0u), v) != 0u) {
                return 1u;
            }
        } else {
            if (f_load(cpu, addr + i, EMU_IR_MEM_AUX(1u, 0u), &v) != 0u) {
                return 1u;
            }
            reg[i] = (uint8_t)v;
        }
    }
    return 0u;
}

/*
 * EMU_IR_VMEMS's helper and its reference: element i at addr + i *
 * stride, a byte at a time through the target's accessors, the stride
 * read from the register imm names -- as RV32's rebuilds its instruction
 * and reads rs2 -- which the generator wrote just before. Not logged,
 * as h_vmem is not.
 */
static uint32_t h_vmems(emu_cpu_t *cpu, uint32_t imm, uint32_t addr,
                        uint32_t vl)
{
    fcpu_t *const f = (fcpu_t *)(void *)cpu;
    uint8_t *const reg = (uint8_t *)f->v + EMU_IR_VMEM_VD(imm) * EMU_IR_VREG_BYTES;
    const uint32_t eb = 1u << EMU_IR_VMEM_EEW(imm);
    const bool store = EMU_IR_VMEM_STORE(imm);
    /*
     * The register through the target, as the IR's GET read it: r[n]
     * directly disagreed with the inline path wherever a target puts a
     * register elsewhere -- r12, the flag word on ARMv7-M's targets.
     */
    uint32_t stride;

    memcpy(&stride, (const uint8_t *)f + g_tgt->reg_offset(EMU_IR_VMEMS_ARG(imm) & 15u), 4u);

    g_st.helpers++;
    for (uint32_t i = 0; i < vl; i++) {
        for (uint32_t k = 0; k < eb; k++) {
            const uint32_t a = addr + i * stride + k;
            uint32_t v = reg[i * eb + k];

            if (store) {
                if (f_store(cpu, a, EMU_IR_MEM_AUX(1u, 0u), v) != 0u) {
                    return 1u;
                }
            } else {
                if (f_load(cpu, a, EMU_IR_MEM_AUX(1u, 0u), &v) != 0u) {
                    return 1u;
                }
                reg[i * eb + k] = (uint8_t)v;
            }
        }
    }
    return 0u;
}

static const void *const k_helpers[] = {
    (const void *)h_mix, (const void *)h_peek, (const void *)h_poke,
    (const void *)h_trap, (const void *)h_vmem, (const void *)h_vmems};
#define H_TRAP 3u
#define H_VMEM 4u
#define H_VMEMS 5u

static void f_fp_flags(emu_cpu_t *cpu, uint32_t flags)
{
    ((fcpu_t *)(void *)cpu)->fe |= flags;
}

/*
 * EMU_IR_VFOP's element, for the reference: C's float arithmetic under
 * round to nearest, a NaN made the canonical one. It is the host's FPU,
 * which the lowering uses too -- so this sees how an operation is
 * lowered (its operands, the mask, vl, the fused forms' signs, the NaN)
 * and not how the unit rounds, which test_lower_vfop asks of x86-64 and
 * the guests of the board. No flags, as for the scalar operations; the
 * generator asks only for RNE.
 */
static uint32_t f_vfop(uint32_t op, uint32_t a, uint32_t b, uint32_t c,
                       uint32_t rm, uint32_t *flags)
{
    float x, y, z, r;
    uint32_t u;

    (void)rm;
    (void)flags;
    memcpy(&x, &a, 4u);
    memcpy(&y, &b, 4u);
    memcpy(&z, &c, 4u);
    switch (op) {
    case EMU_IR_VF_ADD: r = x + y; break;
    case EMU_IR_VF_SUB: r = x - y; break;
    case EMU_IR_VF_RSUB: r = y - x; break;
    case EMU_IR_VF_MUL: r = x * y; break;
    case EMU_IR_VF_DIV: r = x / y; break;
    case EMU_IR_VF_RDIV: r = y / x; break;
    case EMU_IR_VF_SQRT: r = __builtin_sqrtf(x); break;
    case EMU_IR_VF_MACC: r = __builtin_fmaf(y, x, z); break;
    case EMU_IR_VF_NMACC: r = __builtin_fmaf(-y, x, -z); break;
    case EMU_IR_VF_MSAC: r = __builtin_fmaf(y, x, -z); break;
    case EMU_IR_VF_NMSAC: r = __builtin_fmaf(-y, x, z); break;
    case EMU_IR_VF_MADD: r = __builtin_fmaf(y, z, x); break;
    case EMU_IR_VF_NMADD: r = __builtin_fmaf(-y, z, -x); break;
    case EMU_IR_VF_MSUB: r = __builtin_fmaf(y, z, -x); break;
    default: r = __builtin_fmaf(-y, z, x); break; /* NMSUB */
    }
    memcpy(&u, &r, 4u);
    return ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0u) ? 0x7FC00000u : u;
}

static uint32_t off_reg(uint32_t n)
{
    return (uint32_t)(offsetof(fcpu_t, r) + 4u * n);
}

/* r15 is the pc, as ARMv7-M's is. */
static uint32_t off_reg_pc(uint32_t n)
{
    return (n == 15u) ? (uint32_t)offsetof(fcpu_t, pc) : off_reg(n);
}

static bool zero_r0(uint32_t n)
{
    return n == 0u;
}

/* r12 is the flag word, as ARMv7-M's R_FLAGS is: a GET or PUT of it. */
static uint32_t off_reg_fl(uint32_t n)
{
    return (n == 12u) ? (uint32_t)offsetof(fcpu_t, flags) : off_reg(n);
}

static uint32_t off_reg_pc_fl(uint32_t n)
{
    return (n == 12u) ? (uint32_t)offsetof(fcpu_t, flags) : off_reg_pc(n);
}

static uint32_t off_freg(uint32_t n)
{
    return (uint32_t)(offsetof(fcpu_t, f) + 8u * n);
}

static uint32_t off_vreg(uint32_t n)
{
    return (uint32_t)(offsetof(fcpu_t, v) + EMU_IR_VREG_BYTES * n);
}

/* Three flag layouts: low bits, ARM's N Z C V at the top, and none. */
#define TGT(ro, rz, z, s, v, cy)                                               \
    {                                                                          \
        .reg_offset = (ro), .flags_offset = offsetof(fcpu_t, flags),          \
        .flag_bit = {(z), (s), (v), (cy)}, .reg_is_zero = (rz),                \
        .pc_offset = offsetof(fcpu_t, pc), .helpers = k_helpers,               \
        .helper_count = 6u, .load = f_load, .store = f_store,                  \
        .freg_offset = off_freg, .fp_flags = f_fp_flags,                       \
        .vreg_offset = off_vreg, .vsat_offset = offsetof(fcpu_t, vsat),       \
        .vfop = f_vfop,                                                        \
    }
#define TGT_FLAGS(ro, rz)                                                      \
    TGT(ro, rz, 1u, 2u, 4u, 8u),                                               \
        TGT(ro, rz, 1u << 30, 1u << 31, 1u << 28, 1u << 29),                   \
        TGT(ro, rz, 0u, 0u, 0u, 0u)
/*
 * And ARMv7-M's: the APSR's layout, ARM's carry (flags_arm_carry), and a
 * guest register that is the flag word -- SETF and GETCOND beside GET and
 * PUT of the same word, which three passes must see as one thing.
 */
#define TGT_ARM(ro, rz, alone)                                                 \
    {                                                                          \
        .reg_offset = (ro), .flags_offset = offsetof(fcpu_t, flags),          \
        .flag_bit = {1u << 30, 1u << 31, 1u << 28, 1u << 29},                  \
        .flags_arm_carry = true, .flags_alone = (alone), .reg_is_zero = (rz),  \
        .pc_offset = offsetof(fcpu_t, pc), .helpers = k_helpers,               \
        .helper_count = 6u, .load = f_load, .store = f_store,                  \
        .freg_offset = off_freg, .fp_flags = f_fp_flags,                       \
        .vreg_offset = off_vreg, .vsat_offset = offsetof(fcpu_t, vsat),       \
        .vfop = f_vfop,                                                        \
    }

/*
 * G4MH's: the APSR's places, a borrow for C, and a word that holds the
 * flags alone (flags_alone) -- jit_flags.
 */
#define TGT_G4(ro, rz)                                                         \
    {                                                                          \
        .reg_offset = (ro), .flags_offset = offsetof(fcpu_t, flags),          \
        .flag_bit = {1u << 30, 1u << 31, 1u << 28, 1u << 29},                  \
        .flags_alone = true, .reg_is_zero = (rz),                              \
        .pc_offset = offsetof(fcpu_t, pc), .helpers = k_helpers,               \
        .helper_count = 6u, .load = f_load, .store = f_store,                  \
        .freg_offset = off_freg, .fp_flags = f_fp_flags,                       \
        .vreg_offset = off_vreg, .vsat_offset = offsetof(fcpu_t, vsat),       \
        .vfop = f_vfop,                                                        \
    }

static const emu_ir_target_t k_targets[24] = {
    TGT_FLAGS(off_reg, NULL), TGT_FLAGS(off_reg, zero_r0),
    TGT_FLAGS(off_reg_pc, NULL), TGT_FLAGS(off_reg_pc, zero_r0),
    TGT_ARM(off_reg_fl, NULL, false), TGT_ARM(off_reg_fl, zero_r0, false),
    TGT_ARM(off_reg_pc_fl, NULL, false), TGT_ARM(off_reg_pc_fl, zero_r0, false),
    /*
     * A word that holds the flags alone (flags_alone): no register reaches
     * it, and it starts with nothing else in it -- ARMv7-M's jit_nzcv,
     * then G4MH's jit_flags.
     */
    TGT_ARM(off_reg, NULL, true), TGT_ARM(off_reg, zero_r0, true),
    TGT_ARM(off_reg_pc, NULL, true), TGT_ARM(off_reg_pc, zero_r0, true),
    TGT_G4(off_reg, NULL), TGT_G4(off_reg, zero_r0),
    TGT_G4(off_reg_pc, NULL), TGT_G4(off_reg_pc, zero_r0)};

/* ------------------------------------------------------------------ */
/* Reading the input                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t n;
} in_t;

static uint32_t byte(in_t *in)
{
    if (in->n == 0u) {
        return 0u;
    }
    in->n--;
    return *in->p++;
}

static uint32_t pick(in_t *in, uint32_t n)
{
    return byte(in) % n;
}

/*
 * A condition comparing two values: any but ALWAYS, which SETCC and
 * EXIT_IF do not take. One byte, as every draw, so that widening the
 * set (MI/PL/VS/VC, 2026-11-07) left older inputs' streams aligned.
 */
static uint8_t value_cond(in_t *in)
{
    const uint32_t c = pick(in, (uint32_t)EMU_IR_C_COUNT - 1u);

    return (uint8_t)((c < (uint32_t)EMU_IR_C_ALWAYS) ? c : c + 1u);
}

static uint32_t word(in_t *in)
{
    uint32_t v = byte(in);

    v |= byte(in) << 8;
    v |= byte(in) << 16;
    return v | (byte(in) << 24);
}

/*
 * A value worth starting a register at: the edges of every operation,
 * and addresses at and around every edge of the machine. The high bits
 * of the byte move it a little, so that most of these stay addresses.
 */
static uint32_t interesting(uint32_t b)
{
    static const uint32_t k[32] = {
        0u,          1u,          2u,          3u,
        7u,          8u,          31u,         32u,
        33u,         0xFFu,       0x7FFFu,     0x8000u,
        0xFFFFu,     0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
        RAM_BASE,    RAM_BASE + 4u, RAM_BASE + 0x7FEu, RAM_BASE + PAGE - 4u,
        RAM_BASE + PAGE - 2u, ROM_BASE, ROM_BASE + 0x10u, ROM_BASE + PAGE - 4u,
        DEV_BASE,    DEV_BASE + 2u, START_PC,  0x12345678u,
        0xDEADBEEFu, 0x00000100u, 0xFFFFFF80u, 0x40000000u};
    const uint32_t v = k[b & 31u];

    switch (b >> 5) {
    case 1:
        return v + 4u;
    case 2:
        return v + 1u;
    case 3:
        return v - 4u;
    case 4:
        return v ^ 0x80000000u;
    case 5:
        return v * 3u;
    case 6:
        return ~v;
    default:
        return v;
    }
}

/* Single-precision values, and the two ways of holding one. */
static uint64_t interesting_f(uint32_t b)
{
    static const uint32_t k[16] = {
        0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
        0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00001u,
        0x00000001u, 0x007FFFFFu, 0x7F7FFFFFu, 0x3F000000u,
        0x3FC00000u, 0x40200000u, 0x4F000000u, 0xCF000001u};
    const uint64_t v = k[b & 15u];

    /* Most boxed, some not -- an unboxed one reads as the canonical NaN. */
    return ((b & 0x30u) != 0u) ? (UINT64_C(0xFFFFFFFF00000000) | v)
                               : (v | ((uint64_t)(b >> 6) << 32));
}

/* ------------------------------------------------------------------ */
/* Building a block                                                    */
/* ------------------------------------------------------------------ */

static emu_ir_block_t g_built;
static emu_ir_block_t g_opt;

#define POOL 32u
static uint16_t g_pool[POOL];
static uint32_t g_npool;

static void keep(uint16_t t)
{
    if (t != EMU_IR_NO_TEMP) {
        g_pool[g_npool % POOL] = t;
        g_npool++;
    }
}

static uint16_t E(emu_ir_op_t op, uint8_t aux, uint16_t a, uint16_t b,
                  uint32_t imm, uint8_t defs)
{
    return emu_ir_emit(&g_built, op, aux, a, b, imm, defs);
}

/* An operand: mostly a value already computed, sometimes a fresh one. */
static uint16_t val(in_t *in)
{
    const uint32_t sel = byte(in);

    if (g_npool == 0u || sel < 48u) {
        if ((sel & 1u) != 0u) {
            return emu_ir_const(&g_built, interesting(byte(in)));
        }
        const uint16_t t = emu_ir_get(&g_built, pick(in, 16u));

        keep(t);
        return t;
    }
    const uint32_t have = (g_npool < POOL) ? g_npool : POOL;
    const uint32_t back = sel % have;

    return g_pool[(g_npool - 1u - back) % POOL];
}

static uint8_t mem_aux(in_t *in, bool load)
{
    static const uint8_t k_size[4] = {1u, 2u, 4u, 4u};
    const uint32_t b = byte(in);
    uint8_t aux = EMU_IR_MEM_AUX(k_size[b & 3u], load && (b & 4u) != 0u);

    if ((b & 0x18u) == 0x18u) {
        aux |= F_ALIGNED;
    }
    return aux;
}

/* An address: a value, or a base plus an ADDI the pass may fold. */
static uint16_t addr_of(in_t *in, uint32_t *disp)
{
    static const int32_t k_disp[8] = {0, 0, 4, -4, 2, 1, 8, -8};
    const uint32_t b = byte(in);
    uint16_t a = val(in);

    *disp = (uint32_t)k_disp[b & 7u];
    if ((b & 0x40u) != 0u) {
        a = E(EMU_IR_ADDI, 0u, a, EMU_IR_NO_TEMP,
              (uint32_t)k_disp[(b >> 3) & 7u], 0u);
    }
    return a;
}

/*
 * Ask, as a frontend does, before emitting what a backend may lack --
 * and only then. emu_ir_can_lower answers for the operations a frontend
 * asks about; its default is false, so asked about ADD it says no. The
 * rest are what every frontend emits unasked, and BEXT and its three
 * siblings, which x86-64 lowers and does not list (a lie in the safe
 * direction: a frontend that asked would use a helper). POPCNT and ROTL
 * are lowered by neither backend and SELECT only for EQ and NE by
 * x86-64, so no frontend emits them and neither does this.
 */
static bool lowerable(emu_ir_op_t op, uint8_t aux)
{
    switch (op) {
    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR:
    case EMU_IR_MUL:
    case EMU_IR_NEG:
    case EMU_IR_NOT:
    case EMU_IR_MOV:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16:
    case EMU_IR_BEXT:
    case EMU_IR_BSET:
    case EMU_IR_BCLR:
    case EMU_IR_BINV:
        return true;
    case EMU_IR_POPCNT:
    case EMU_IR_ROTL:
    case EMU_IR_SELECT:
        return false;
    default:
        return emu_ir_can_lower(op, aux);
    }
}

/* c ? x : y for c in {0, 1}, as ARMv7-M's IT blocks select: a mask. */
static uint16_t choose(uint16_t c, uint16_t x, uint16_t y)
{
    const uint16_t m = E(EMU_IR_SUB, 0u, emu_ir_const(&g_built, 0u), c, 0u, 0u);
    const uint16_t diff = E(EMU_IR_XOR, 0u, x, y, 0u, 0u);

    return E(EMU_IR_XOR, 0u, y, E(EMU_IR_AND, 0u, diff, m, 0u, 0u), 0u, 0u);
}

static const uint8_t k_rr[] = {
    EMU_IR_ADD, EMU_IR_SUB,  EMU_IR_AND,  EMU_IR_OR,   EMU_IR_XOR,
    EMU_IR_SHL, EMU_IR_SHR,  EMU_IR_SAR,  EMU_IR_ROTL, EMU_IR_MUL,
    EMU_IR_MULHS, EMU_IR_MULHU, EMU_IR_BEXT, EMU_IR_BSET, EMU_IR_BCLR,
    EMU_IR_BINV};
static const uint8_t k_ri[] = {EMU_IR_ADDI, EMU_IR_ANDI, EMU_IR_ORI,
                               EMU_IR_XORI, EMU_IR_SHLI, EMU_IR_SHRI,
                               EMU_IR_SARI, EMU_IR_ROTLI};
static const uint8_t k_un[] = {
    EMU_IR_NEG,   EMU_IR_NOT,    EMU_IR_BSWAP32, EMU_IR_BSWAP16,
    EMU_IR_HSWAP, EMU_IR_CLZ,    EMU_IR_CTZ,     EMU_IR_POPCNT,
    EMU_IR_SEXT8, EMU_IR_SEXT16, EMU_IR_ZEXT8,   EMU_IR_ZEXT16,
    EMU_IR_MOV};
static const uint8_t k_fbin[] = {EMU_IR_FADD, EMU_IR_FSUB, EMU_IR_FMUL,
                                 EMU_IR_FDIV, EMU_IR_FMIN, EMU_IR_FMAX};

/* Flags from a value, as a frontend's flag-setting instruction does. */
static void setf(in_t *in, uint16_t a, uint16_t b)
{
    const uint32_t k = byte(in);
    uint8_t defs = (uint8_t)(k & EMU_IR_F_ALL);

    if (defs == 0u) {
        defs = EMU_IR_F_ALL;
    }
    (void)E(EMU_IR_SETF, (uint8_t)((k >> 4) % 5u), a, b, 0u, defs);
}

/* A divisor the IR may be handed: never zero, never -1 (see emu_ir.h). */
static uint16_t divisor(uint16_t d)
{
    const uint16_t zero = emu_ir_const(&g_built, 0u);
    const uint16_t is0 = E(EMU_IR_SETCC, EMU_IR_C_EQ, d, zero, 0u, 0u);
    const uint16_t nz = E(EMU_IR_ADD, 0u, d, is0, 0u, 0u);
    const uint16_t m1 = emu_ir_const(&g_built, 0xFFFFFFFFu);
    const uint16_t ism1 = E(EMU_IR_SETCC, EMU_IR_C_EQ, nz, m1, 0u, 0u);

    return E(EMU_IR_SHL, 0u, nz, ism1, 0u, 0u);
}

/*
 * One guest instruction: a pc written first where it can be observed,
 * then its operations, then a RETIRE -- before any exit, as every
 * frontend orders them, since a RETIRE after a taken branch is skipped.
 * Returns false when the instruction ended the block.
 */
static bool one(in_t *in, uint32_t pc, uint32_t next)
{
    const uint32_t kind = pick(in, 23u);
    uint16_t d = EMU_IR_NO_TEMP;

    if (byte(in) < 200u) {
        (void)E(EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, pc, 0u);
    }

    switch (kind) {
    case 0:
    case 1: { /* register-register, perhaps setting flags */
        const uint8_t op = k_rr[pick(in, sizeof(k_rr))];
        const uint16_t a = val(in), b = val(in);

        if (lowerable((emu_ir_op_t)op, 0u)) {
            d = E((emu_ir_op_t)op, 0u, a, b, 0u, 0u);
        }
        if (byte(in) < 96u) {
            setf(in, a, b);
        }
        break;
    }
    case 2: { /* against a constant, which pass_fuse folds */
        const uint8_t op = k_rr[pick(in, 9u)];
        const uint16_t a = val(in);
        /* A rotate by a constant is only ever ROTLI once folded. */
        const uint32_t kv = interesting(byte(in));
        const uint16_t k = emu_ir_const(&g_built, (op == EMU_IR_ROTL)
                                                      ? (kv & 31u)
                                                      : kv);

        if (op == EMU_IR_ROTL) {
            d = E(EMU_IR_ROTL, 0u, a, k, 0u, 0u);
        } else if (lowerable((emu_ir_op_t)op, 0u)) {
            d = (byte(in) & 1u) ? E((emu_ir_op_t)op, 0u, k, a, 0u, 0u)
                                : E((emu_ir_op_t)op, 0u, a, k, 0u, 0u);
        }
        break;
    }
    case 3: { /* an immediate form */
        const uint8_t op = k_ri[pick(in, sizeof(k_ri))];
        uint32_t imm = interesting(byte(in));

        if (op >= EMU_IR_SHLI) {
            imm &= 31u;
        }
        if (lowerable((emu_ir_op_t)op, 0u)) {
            d = E((emu_ir_op_t)op, 0u, val(in), EMU_IR_NO_TEMP, imm, 0u);
        }
        break;
    }
    case 4: { /* one operand */
        const uint8_t op = k_un[pick(in, sizeof(k_un))];

        if (op == EMU_IR_POPCNT) {
            /*
             * Declined by both hosts, so this slot built nothing; it
             * builds the crypto operations instead, which x86-64 lowers
             * when its CPU has AES-NI, PCLMULQDQ and (for SM4) SSSE3.
             * Here rather than a kind of their own so that every saved
             * input not landing on it decodes as it did; one byte picks
             * the operation and its aux, so those landing on it keep
             * their streams.
             */
            const uint32_t w = byte(in);
            static const emu_ir_op_t k_c[3] = {EMU_IR_CLMUL, EMU_IR_AESSB,
                                               EMU_IR_SM4SB};
            static const uint8_t k_aux[3] = {3u, 2u, 1u};
            const emu_ir_op_t c = k_c[w % 3u];
            const uint8_t aux = (uint8_t)((w / 3u) % k_aux[w % 3u]);

            if (lowerable(c, aux)) {
                const uint16_t a = val(in);

                d = E(c, aux, a, (c == EMU_IR_CLMUL) ? val(in) : EMU_IR_NO_TEMP,
                      0u, 0u);
            }
        } else if (lowerable((emu_ir_op_t)op, 0u)) {
            d = E((emu_ir_op_t)op, 0u, val(in), EMU_IR_NO_TEMP, 0u, 0u);
        }
        break;
    }
    case 5: { /* compare to a value */
        const uint16_t a = val(in), b = val(in);
        const uint8_t c = value_cond(in);

        /*
         * VC's draws make a three-way compare instead, signed or not by
         * b's parity: no byte more, so a saved input decodes as it did
         * up to here.
         */
        if (c == (uint8_t)EMU_IR_C_VC &&
            lowerable(EMU_IR_CMP3, (b & 1u) ? EMU_IR_C_LTU : EMU_IR_C_LT)) {
            d = E(EMU_IR_CMP3, (b & 1u) ? EMU_IR_C_LTU : EMU_IR_C_LT, a, b, 0u, 0u);
        } else {
            d = E(EMU_IR_SETCC, c, a, b, 0u, 0u);
        }
        break;
    }
    case 6: { /* divide, with the divisor the contract asks for */
        static const uint8_t k_div[4] = {EMU_IR_DIVS, EMU_IR_DIVU,
                                         EMU_IR_REMS, EMU_IR_REMU};
        const uint8_t op = k_div[pick(in, 4u)];
        const uint16_t a = val(in);

        if (lowerable((emu_ir_op_t)op, 0u)) {
            d = E((emu_ir_op_t)op, 0u, a, divisor(val(in)), 0u, 0u);
        }
        break;
    }
    case 7:
    case 8: { /* a load, perhaps under a condition */
        uint32_t disp;
        const uint8_t aux = mem_aux(in, true);
        const uint16_t a = addr_of(in, &disp);

        if (byte(in) < 64u) {
            const uint8_t cond = (uint8_t)pick(in, (uint32_t)EMU_IR_C_COUNT);
            const uint16_t old = val(in);
            const uint16_t p =
                E(EMU_IR_GETCOND, cond, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
            const uint16_t v = emu_ir_mem_if(&g_built, EMU_IR_LOAD, aux, a,
                                             EMU_IR_NO_TEMP, disp, p);
            /* The skipped load's result is unspecified: select it away. */
            d = choose(p, v, old);
        } else {
            d = E(EMU_IR_LOAD, aux, a, EMU_IR_NO_TEMP, disp, 0u);
        }
        break;
    }
    case 9: { /* a store, perhaps under a condition */
        uint32_t disp;
        const uint8_t aux = mem_aux(in, false);
        const uint16_t a = addr_of(in, &disp);
        const uint16_t v = val(in);
        uint16_t p = EMU_IR_NO_TEMP;

        if (byte(in) < 64u) {
            p = E(EMU_IR_GETCOND, (uint8_t)pick(in, (uint32_t)EMU_IR_C_COUNT),
                  EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
        }
        (void)emu_ir_mem_if(&g_built, EMU_IR_STORE, aux, a, v, disp, p);
        break;
    }
    case 10: { /* a read-modify-write of one bit */
        static const uint8_t k_bit[4] = {EMU_IR_BITOP_SET, EMU_IR_BITOP_CLR,
                                         EMU_IR_BITOP_INV, EMU_IR_BITOP_TST};
        const uint8_t op = k_bit[pick(in, 4u)];
        uint32_t disp;
        const uint16_t a = addr_of(in, &disp);

        if (lowerable((emu_ir_op_t)op, 1u)) {
            emu_ir_bitop(&g_built, (emu_ir_op_t)op, a, val(in), disp, 1u);
        }
        break;
    }
    case 11: { /* flags, then something that reads them */
        const uint16_t a = val(in), b = val(in);

        setf(in, a, b);
        d = E(EMU_IR_GETCOND, (uint8_t)pick(in, (uint32_t)EMU_IR_C_COUNT),
              EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
        if (byte(in) & 1u) {
            const uint16_t x = val(in), y = val(in);

            d = choose(d, x, y);
        }
        break;
    }
    case 12: { /* register traffic, the zero register and the pc too */
        const uint32_t r = pick(in, 16u);
        const uint32_t s = (byte(in) & 1u) ? r : pick(in, 16u);

        emu_ir_put(&g_built, r, val(in));
        d = emu_ir_get(&g_built, s);
        break;
    }
    case 13: { /* single precision */
        const uint32_t op = pick(in, 10u);
        const uint8_t box = (byte(in) & 1u) ? EMU_IR_FP_BOX : 0u;
        const uint16_t x = E(EMU_IR_FGET, box, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                             pick(in, 8u), 0u);
        const uint16_t y = E(EMU_IR_FGET, box, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                             pick(in, 8u), 0u);
        uint16_t res = EMU_IR_NO_TEMP;
        uint8_t aux = 0u;

        if (op < 6u) {
            aux = (uint8_t)pick(in, 5u); /* a rounding mode */
            if (lowerable((emu_ir_op_t)k_fbin[op], aux)) {
                res = E((emu_ir_op_t)k_fbin[op], aux, x, y, 0u, 0u);
            }
        } else if (op == 6u) {
            aux = (uint8_t)pick(in, 3u);
            res = E(EMU_IR_FSGNJ, aux, x, y, 0u, 0u);
        } else if (op == 7u) {
            static const uint8_t k_fc[3] = {EMU_IR_C_EQ, EMU_IR_C_LT,
                                            EMU_IR_C_LE};
            aux = k_fc[pick(in, 3u)];
            if (lowerable(EMU_IR_FCMP, aux)) {
                d = E(EMU_IR_FCMP, aux, x, y, 0u, 0u);
            }
        } else if (op == 8u) {
            aux = (uint8_t)(pick(in, 5u) | ((byte(in) & 1u) ? EMU_IR_F_UNSIGNED
                                                           : 0u));
            if (byte(in) & 1u) {
                if (lowerable(EMU_IR_FCVT_TO_I, aux)) {
                    d = E(EMU_IR_FCVT_TO_I, aux, x, EMU_IR_NO_TEMP, 0u, 0u);
                }
            } else if (lowerable(EMU_IR_FCVT_FROM_I, aux)) {
                res = E(EMU_IR_FCVT_FROM_I, aux, val(in), EMU_IR_NO_TEMP, 0u,
                        0u);
            }
        } else if (lowerable(EMU_IR_FCLASS, 0u)) {
            d = E(EMU_IR_FCLASS, 0u, x, EMU_IR_NO_TEMP, 0u, 0u);
        }
        if (res != EMU_IR_NO_TEMP) {
            (void)E(EMU_IR_FPUT, (byte(in) & 1u) ? EMU_IR_FP_BOX : 0u, res,
                    EMU_IR_NO_TEMP, pick(in, 8u), 0u);
        }
        break;
    }
    case 14: { /* a helper, which may read and write anything */
        const uint32_t h = pick(in, 4u);
        const uint16_t a = val(in), b = val(in);

        if (h == H_TRAP) {
            (void)E(EMU_IR_HELPER_TRAP, 0u, a, b, H_TRAP, 0u);
        } else {
            /*
             * h_mix's third argument, never 0, from what was drawn
             * rather than another byte, so older inputs keep their
             * streams. The others are emitted without one.
             */
            const uint8_t aux =
                (h == 0u) ? (uint8_t)(1u + ((a * 7u + b) & 0x7Fu)) : 0u;

            d = E(EMU_IR_HELPER, aux, a, b, h, 0u);
        }
        break;
    }
    case 15: { /* a multiply whose one reader adds it: pass_mac */
        const uint16_t a = val(in), b = val(in), c = val(in);
        const uint16_t m = E(EMU_IR_MUL, 0u, a, b, 0u, 0u);

        d = (byte(in) & 1u) ? E(EMU_IR_SUB, 0u, c, m, 0u, 0u)
                            : E(EMU_IR_ADD, 0u, m, c, 0u, 0u);
        break;
    }
    case 16: /* the pc, as a value */
        (void)E(EMU_IR_SETPC, 0u, val(in), EMU_IR_NO_TEMP, 0u, 0u);
        break;
    case 18: { /* a vector access of a whole group, or of part of one */
        const uint32_t glg = pick(in, 4u);
        const uint32_t eew = pick(in, 3u);
        const bool store = (byte(in) & 1u) != 0u;
        const uint32_t bytes = 16u << glg;
        const uint32_t vd = pick(in, 32u >> glg) << glg;
        const uint32_t full = bytes >> eew;
        /* vl mostly the whole group, the inline case; else shorter */
        const uint32_t vl = (byte(in) < 176u) ? full : pick(in, full + 1u);
        const uint32_t sel = byte(in);
        uint16_t a;

        if (!emu_ir_can_lower(EMU_IR_VMEM, 0u)) {
            break;
        }
        if (sel < 96u) { /* in RAM, anywhere */
            a = emu_ir_const(&g_built, RAM_BASE + pick(in, PAGE));
        } else if (sel < 192u) { /* at RAM's end, give or take */
            a = emu_ir_const(&g_built, RAM_BASE + PAGE - bytes +
                                           (uint32_t)((int32_t)pick(in, 9u) - 4));
        } else {
            a = val(in);
        }
        emu_ir_vmem(&g_built, (uint8_t)H_VMEM, a, emu_ir_const(&g_built, vl),
                    emu_ir_const(&g_built, vd | ((1u << eew) << 8) |
                                               (store ? 0x10000u : 0u)),
                    EMU_IR_VMEM_IMM(vd, glg, eew, store));
        break;
    }
    case 20: { /* a reduction: vd and vs1 anywhere, vs2's group aligned */
        const uint32_t rop = pick(in, EMU_IR_R_COUNT);
        const uint32_t sew = pick(in, 3u);
        const uint32_t lg = pick(in, 4u);
        const bool masked = (byte(in) & 1u) != 0u;
        const uint8_t aux = EMU_IR_VRED_AUX(rop, sew, masked);
        const uint32_t vs2 = pick(in, 32u >> lg) << lg;
        const uint32_t vd = (byte(in) < 64u) ? vs2 : pick(in, 32u);
        const uint32_t vs1 = pick(in, 32u);
        const uint32_t vmr = pick(in, 32u);
        const uint32_t lanes = (EMU_IR_VREG_BYTES << lg) >> sew;
        const uint32_t vl = (byte(in) < 64u) ? lanes : pick(in, lanes + 1u);

        if (!emu_ir_can_lower(EMU_IR_VRED, aux)) {
            break;
        }
        (void)E(EMU_IR_VRED, aux, EMU_IR_NO_TEMP, emu_ir_const(&g_built, vl),
                EMU_IR_VOPM_IMM(vd, vs2, vs1, lg, vmr), 0u);
        break;
    }
    case 21: { /* a strided access: its stride written to a register first */
        static const int32_t k_strides[] = {
            0, 1, 2, 4, -4, 16, -12, 3, -1, 0x00FFFFFF, -0x01000000, 0x01000000,
        };
        const uint32_t glg = pick(in, 4u);
        const uint32_t eew = pick(in, 3u);
        const bool store = (byte(in) & 1u) != 0u;
        const uint32_t vd = pick(in, 32u >> glg) << glg;
        const uint32_t full = (16u << glg) >> eew;
        const uint32_t vl = (byte(in) < 128u) ? full : pick(in, full + 1u);
        const uint32_t sr = 1u + pick(in, 14u); /* neither r0 nor the pc */
        const uint32_t sel = byte(in);
        uint32_t stride = (uint32_t)k_strides[pick(in, sizeof(k_strides) / sizeof(k_strides[0]))];
        uint16_t a;

        if (!emu_ir_can_lower(EMU_IR_VMEMS, 0u)) {
            break;
        }
        if (byte(in) < 32u) {
            stride = (uint32_t)byte(in) | ((uint32_t)byte(in) << 8) |
                     ((uint32_t)byte(in) << 24);
        }
        if (sel < 96u) { /* in RAM, anywhere */
            a = emu_ir_const(&g_built, RAM_BASE + pick(in, PAGE));
        } else if (sel < 192u) { /* the last element near RAM's end */
            a = emu_ir_const(&g_built,
                             RAM_BASE + PAGE - (full - 1u) * (stride & 0xFFu) -
                                 (1u << eew) + (uint32_t)((int32_t)pick(in, 9u) - 4));
        } else {
            a = val(in);
        }
        emu_ir_put(&g_built, sr, emu_ir_const(&g_built, stride));
        emu_ir_vmems(&g_built, (uint8_t)H_VMEMS, a, emu_ir_const(&g_built, vl),
                     emu_ir_get(&g_built, sr), EMU_IR_VMEMS_IMM(vd, eew, store, sr));
        break;
    }
    case 22: { /* single precision on the vector registers, under RNE */
        static const uint32_t k_special[16] = {
            0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
            0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu,
            0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
            0xFFC12345u, 0x3F7FFFFEu, 0x00800001u, 0x40400000u,
        };
        const uint32_t op = pick(in, EMU_IR_VF_COUNT);
        const uint32_t lg = pick(in, 4u);
        const uint32_t vd = pick(in, 32u >> lg) << lg;
        const uint32_t vs2 = (byte(in) < 64u) ? vd : (pick(in, 32u >> lg) << lg);
        const uint32_t vs1 = pick(in, 32u >> lg) << lg;
        const uint32_t vm = pick(in, 32u);
        /* the mask register outside vd's group, as the contract asks */
        const bool masked = (byte(in) & 1u) != 0u && (vm >> lg) != (vd >> lg);
        const bool vf = (byte(in) & 1u) != 0u;
        const uint32_t lanes = (EMU_IR_VREG_BYTES / 4u) << lg;
        const uint32_t vl = (byte(in) < 128u) ? lanes : pick(in, lanes + 1u);
        const uint8_t aux = EMU_IR_VFOP_AUX(op, EMU_IR_FRM_RNE);
        const uint8_t mv = EMU_IR_VOP_AUX(EMU_IR_V_MV, 2u, true);
        const uint32_t fill = byte(in);

        if (!emu_ir_can_lower(EMU_IR_VFOP, aux)) {
            break;
        }
        /* a special value in a source group first, sometimes */
        if (fill < 128u && emu_ir_can_lower(EMU_IR_VOP, mv)) {
            const uint32_t to = (fill & 2u) ? vs1 : (fill & 1u) ? vs2 : vd;

            (void)E(EMU_IR_VOP, mv, emu_ir_const(&g_built, k_special[pick(in, 16u)]),
                    emu_ir_const(&g_built, pick(in, lanes + 1u)),
                    EMU_IR_VOP_IMM(to, 0u, 0u, lg), 0u);
        }
        (void)E(EMU_IR_VFOP, aux,
                vf ? ((byte(in) & 1u) ? emu_ir_const(&g_built, k_special[pick(in, 16u)])
                                      : val(in))
                   : EMU_IR_NO_TEMP,
                emu_ir_const(&g_built, vl),
                EMU_IR_VFOP_IMM(vd, vs2, vs1, lg, vm, masked, vf), 0u);
        break;
    }
    case 17: { /* the guest's vector registers, within the op's contract */
        const uint32_t vop = pick(in, EMU_IR_V_COUNT);
        const uint32_t sew = pick(in, 3u);
        const bool vx = (byte(in) & 1u) != 0u;
        const bool wide = EMU_IR_V_IS_WIDE(vop);
        const bool narrow = EMU_IR_V_IS_NARROW(vop);
        const uint32_t lg = pick(in, (wide || narrow) ? 3u : 4u);
        const uint32_t dlg = wide ? lg + 1u : lg; /* vd's group */
        const uint32_t slg = narrow ? lg + 1u : lg; /* vs2's */
        const uint8_t aux = EMU_IR_VOP_AUX(vop, sew, vx);
        /* each the first register of a group: same or disjoint */
        const uint32_t vd = pick(in, 32u >> dlg) << dlg;
        const uint32_t vs2 = (narrow && (byte(in) & 3u) == 0u)
                                 ? (vd & ~((2u << lg) - 1u)) /* vd at its start */
                                 : (pick(in, 32u >> slg) << slg);
        const uint32_t vs1 = pick(in, 32u >> lg) << lg;
        const uint32_t lanes = (EMU_IR_VREG_BYTES << lg) >> sew;

        if (vop != EMU_IR_V_MERGE && !emu_ir_can_lower(EMU_IR_VOP, aux)) {
            break; /* a frontend would have kept its helper */
        }
        if (wide && ((vd < vs2 + (1u << lg) && vs2 < vd + (2u << lg)) ||
                     (!vx && vd < vs1 + (1u << lg) && vs1 < vd + (2u << lg)))) {
            break; /* the contract: vd disjoint from the sources */
        }
        if (narrow && vd != vs2 && vd < vs2 + (2u << lg) && vs2 < vd + (1u << lg)) {
            break; /* the contract: disjoint from vs2's group, or its start */
        }
        /* vl at most the group's elements; often a register's edge */
        uint32_t vl = pick(in, lanes + 1u);

        if (byte(in) < 64u) {
            vl = ((EMU_IR_VREG_BYTES >> sew) * pick(in, (1u << lg) + 1u));
        }
        /*
         * Under a mask, half the time where the operation has a masked
         * form: the mask register anywhere outside vd's group.
         */
        const uint32_t vmr = pick(in, 32u);
        const bool masked = (byte(in) & 1u) != 0u &&
                            !(vmr >= vd && vmr < vd + (1u << dlg));
        const uint32_t mop = (masked && vop == EMU_IR_V_MV && (byte(in) & 1u))
                                 ? EMU_IR_V_MERGE
                                 : vop;
        const uint8_t maux = EMU_IR_VOP_AUX(mop, sew, vx);
        /* the averages and clips under any rounding mode, drawn only for them */
        const uint32_t rmi = (vop == EMU_IR_V_AADDU || vop == EMU_IR_V_AADD ||
                              vop == EMU_IR_V_NCLIPU || vop == EMU_IR_V_NCLIP)
                                 ? EMU_IR_VOP_IMM_RM(pick(in, 4u))
                                 : 0u;

        /*
         * A narrowing count of 0 to 3 half the time: rounding differs most
         * at the smallest shifts -- rne from rnu at a count of 1 for a
         * quarter of all values -- which a random word reaches once in 32.
         */
        const uint16_t sc = !vx ? EMU_IR_NO_TEMP
                            : (narrow && (byte(in) & 1u) != 0u)
                                ? emu_ir_const(&g_built, pick(in, 4u))
                                : val(in);

        if (masked && emu_ir_can_lower(EMU_IR_VOPM, maux)) {
            (void)E(EMU_IR_VOPM, maux, sc,
                    emu_ir_const(&g_built, vl),
                    EMU_IR_VOPM_IMM(vd, vs2, vs1, lg, vmr) | rmi, 0u);
            break;
        }
        if (vop == EMU_IR_V_MERGE) {
            break; /* a merge has no unmasked form */
        }
        (void)E(EMU_IR_VOP, aux, sc, emu_ir_const(&g_built, vl),
                EMU_IR_VOP_IMM(vd, vs2, vs1, lg) | rmi, 0u);
        break;
    }
    default: /* a write of the result to a register, below */
        d = val(in);
        break;
    }

    if (d != EMU_IR_NO_TEMP) {
        keep(d);
        if (byte(in) < 160u) {
            emu_ir_put(&g_built, pick(in, 16u), d);
        }
    }
    (void)E(EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);

    /*
     * A branch: back to the start (a loop), on, or away. On a high byte,
     * because an input that has run out reads zeros, and a zero that
     * meant "branch" made every short input a loop.
     */
    if (byte(in) >= 232u) {
        static const uint32_t k_to[4] = {START_PC, 0u, 0x00000900u,
                                         START_PC};
        const uint32_t sel = k_to[byte(in) & 3u];
        const uint32_t to = (sel == 0u) ? next : sel;
        const uint16_t a = val(in), b = val(in);

        (void)E(EMU_IR_EXIT_IF, value_cond(in), a, b, to, 0u);
    }
    return true;
}

/*
 * The configuration bytes: which target (zero register, pc register,
 * flag layout), how memory is reached -- and which window, all of RAM,
 * part of it, or part at an odd base -- and the store guard.
 */
/*
 * A 16-bit dot product that loops on itself, the shape a kernel takes
 * (emu_ir_kernel.c, docs/jit/autovectorisation.md): random words never
 * spell one. Each parameter is drawn where the kernel's checks decide:
 * the element signed, sign-extended, byte-swapped first, or unsigned
 * (which no kernel may take); x walking up or down; y by a constant or
 * by a register shifted; the back edge on x against an end, or on a
 * counter down to zero; an end a whole number of steps away or not;
 * arrays placed anywhere in the RAM, past its end and misaligned among
 * them. The kernel's run is checked as every block is: as built -- the
 * scalar loop -- against as optimised, and against the compiled code.
 */
static uint16_t dot_elem(uint16_t addr, uint32_t off, uint32_t form)
{
    const bool sgn = form == 0u;
    const uint16_t l = E(EMU_IR_LOAD, EMU_IR_MEM_AUX(2u, sgn), addr, EMU_IR_NO_TEMP, off, 0u);

    switch (form) {
    case 1u:
        return E(EMU_IR_SEXT16, 0u, l, EMU_IR_NO_TEMP, 0u, 0u);
    case 2u:
        return E(EMU_IR_SEXT16, 0u, E(EMU_IR_BSWAP16, 0u, l, EMU_IR_NO_TEMP, 0u, 0u),
                 EMU_IR_NO_TEMP, 0u, 0u);
    default:
        return l; /* 0: signed load; 3: unsigned, as a kernel must refuse */
    }
}

/*
 * DOT16_F's map of the product: none, a chain of it, or a chain times
 * another of the same product (CoreMark's bit_extract(p, 2, 4) *
 * bit_extract(p, 5, 7)). Drawn after everything build_dot drew before
 * maps existed, so an input that ends there -- reading 0 -- keeps its
 * meaning. Shifts past 31 and constants with no immediate form included:
 * the IR masks the first, Thumb-2 needs a register for the second.
 */
static uint16_t dot_chain(in_t *in, uint16_t v)
{
    static const uint8_t k_ops[14] = {
        EMU_IR_ADDI,  EMU_IR_ANDI,  EMU_IR_ORI,   EMU_IR_XORI,  EMU_IR_SHLI,
        EMU_IR_SHRI,  EMU_IR_SARI,  EMU_IR_ROTLI, EMU_IR_SEXT8, EMU_IR_SEXT16,
        EMU_IR_ZEXT8, EMU_IR_ZEXT16, EMU_IR_NOT,  EMU_IR_NEG,
    };
    const uint32_t len = byte(in) % 5u; /* the recogniser takes 4 */

    for (uint32_t i = 0u; i < len; i++) {
        const uint8_t op = k_ops[byte(in) % 14u];
        const uint32_t b = byte(in);
        uint32_t imm = 0u;

        if (op == (uint8_t)EMU_IR_SHLI || op == (uint8_t)EMU_IR_SHRI ||
            op == (uint8_t)EMU_IR_SARI || op == (uint8_t)EMU_IR_ROTLI) {
            imm = (b & 0x80u) ? b : (b & 31u);
        } else if (op == (uint8_t)EMU_IR_ADDI || op == (uint8_t)EMU_IR_ANDI ||
                   op == (uint8_t)EMU_IR_ORI || op == (uint8_t)EMU_IR_XORI) {
            imm = (b & 0x80u) ? interesting(b) : ((1u << (b & 7u)) - 1u); /* masks */
        }
        v = E((emu_ir_op_t)op, 0u, v, EMU_IR_NO_TEMP, imm, 0u);
    }
    return v;
}

static uint16_t dot_map(in_t *in, uint16_t p)
{
    switch (byte(in) & 3u) {
    case 0u:
        return p;
    case 1u:
        return dot_chain(in, p);
    default: {
        const uint16_t a = dot_chain(in, p);

        return E(EMU_IR_MUL, 0u, a, dot_chain(in, p), 0u, 0u);
    }
    }
}

static void build_dot(in_t *in, fcpu_t *c)
{
    const uint32_t f = byte(in);
    const uint32_t xo = byte(in), yo = byte(in);
    const uint32_t tb = byte(in);
    const uint32_t trip = 1u + (tb & 63u);
    const uint32_t ys = byte(in);
    const int32_t xstep = (f & 0x10u) ? -2 : 2;
    const uint32_t xoff = (uint32_t)(int32_t)(int8_t)(xo & 0x86u); /* 0, 2, 4, 6, -128... */
    const uint32_t yoff = (uint32_t)(int32_t)(int8_t)(yo & 0x86u);

    /* x anywhere, near the end, or misaligned; y likewise */
    c->r[1] = RAM_BASE + ((xo >> 3) * 16u) + ((f & 1u) ? (PAGE - 2u * trip) : 0u) +
              ((f & 2u) ? 1u : 0u);
    c->r[2] = RAM_BASE + ((yo >> 3) * 8u) + ((yo & 1u) ? (PAGE / 2u) : 0u);
    c->r[3] = interesting(byte(in));
    c->r[5] = (ys >> 4) & 7u; /* y's stride register: 0 to 7, shifted by (ys & 3) */
    c->r[8] = trip;           /* the counter, for a back edge on it */
    c->r[4] = c->r[1] + (uint32_t)xstep * trip + ((f & 0x40u) ? 1u : 0u);

    (void)E(EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, START_PC, 0u);
    const uint16_t tx = emu_ir_get(&g_built, 1u);
    const uint16_t lx = dot_elem(tx, xoff, (f >> 2) & 3u);
    const uint16_t ty = emu_ir_get(&g_built, 2u);
    const uint16_t ly = dot_elem(ty, yoff, (ys & 0x80u) ? ((f >> 2) & 3u) : 0u);
    const uint16_t tx2 = E(EMU_IR_ADDI, 0u, tx, EMU_IR_NO_TEMP, (uint32_t)xstep, 0u);
    uint16_t ty2;

    emu_ir_put(&g_built, 1u, tx2);
    if ((ys & 0x08u) != 0u) {
        ty2 = E(EMU_IR_ADDI, 0u, ty, EMU_IR_NO_TEMP, (uint32_t)(int32_t)(int8_t)(ys & 0x8Eu), 0u);
    } else {
        uint16_t s = emu_ir_get(&g_built, 5u);

        if ((ys & 3u) != 0u) {
            s = E(EMU_IR_SHLI, 0u, s, EMU_IR_NO_TEMP, ys & 3u, 0u);
        }
        ty2 = (ys & 0x04u) ? E(EMU_IR_ADD, 0u, s, ty, 0u, 0u) : E(EMU_IR_ADD, 0u, ty, s, 0u, 0u);
    }
    emu_ir_put(&g_built, 2u, ty2);

    /* the constant-stride factor first or second (ARMv7-M's smulbb has it second) */
    const uint16_t p = (tb & 0x40u) ? E(EMU_IR_MUL, 0u, ly, lx, 0u, 0u)
                                    : E(EMU_IR_MUL, 0u, lx, ly, 0u, 0u);

    emu_ir_put(&g_built, 6u, p); /* a register each iteration writes */
    emu_ir_put(&g_built, 3u, E(EMU_IR_ADD, 0u, emu_ir_get(&g_built, 3u), dot_map(in, p), 0u, 0u));
    (void)E(EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 3u + (f >> 7), 0u);
    if ((f & 0x20u) != 0u) {
        /* PowerPC's bdnz: a counter down to zero */
        const uint16_t k = E(EMU_IR_SUB, 0u, emu_ir_get(&g_built, 8u),
                             emu_ir_const(&g_built, 1u), 0u, 0u);

        emu_ir_put(&g_built, 8u, k);
        (void)E(EMU_IR_EXIT_IF, EMU_IR_C_NE, k, emu_ir_const(&g_built, 0u), START_PC, 0u);
    } else {
        (void)E(EMU_IR_EXIT_IF, EMU_IR_C_NE, emu_ir_get(&g_built, 4u), tx2, START_PC, 0u);
    }
    (void)E(EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
    (void)E(EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, START_PC + 0x40u, 0u);
}

/*
 * A byte loop that loops on itself: strcpy's copy to the NUL, memcpy's
 * copy, memset's fill -- the kernels' other shapes. The source in RAM or
 * in the image; the destination over the source a byte or a few behind
 * or ahead, or apart; the NUL anywhere, or past where the loop could
 * reach this entry; an end near the window's; the store guard as the
 * window's configuration leaves it.
 */
static void build_bytes(in_t *in, fcpu_t *c)
{
    const uint32_t f = byte(in);
    const uint32_t kind = f % 3u; /* 0 copy to NUL, 1 copy, 2 fill */
    const uint32_t so = byte(in), dd = byte(in);
    const uint32_t trip = 1u + (byte(in) & 63u);
    const uint32_t z = byte(in) & 127u;

    if ((f & 4u) != 0u && g_built.has_rom) {
        c->r[1] = ROM_BASE + so * 8u;
        g_rom[(c->r[1] - ROM_BASE + z) % PAGE] = 0u;
    } else {
        c->r[1] = RAM_BASE + so * 8u + ((f & 8u) ? (PAGE - trip - 2u) : 0u);
        g_mem[(c->r[1] - RAM_BASE + z) % g_mem_size] = 0u;
    }
    /* the destination: over the source, near it either side, or apart */
    c->r[2] = ((f & 0x30u) == 0x10u) ? c->r[1] + (dd & 7u)
              : ((f & 0x30u) == 0x20u) ? c->r[1] - (dd & 7u)
                                       : RAM_BASE + (dd * 8u) % PAGE;
    if (c->r[2] - RAM_BASE >= PAGE) {
        c->r[2] = RAM_BASE + (dd & 0x7Fu);
    }
    c->r[4] = c->r[2] + trip + ((f & 0x40u) ? 1u : 0u); /* a counted loop's end */
    c->r[5] = interesting(byte(in));                    /* a fill's value */

    (void)E(EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, START_PC, 0u);
    uint16_t v;

    if (kind == 2u) {
        v = emu_ir_get(&g_built, 5u);
    } else {
        const uint16_t ts = emu_ir_get(&g_built, 1u);

        v = E(EMU_IR_LOAD, EMU_IR_MEM_AUX(1u, false), ts, EMU_IR_NO_TEMP, 0u, 0u);
        emu_ir_put(&g_built, 6u, v); /* the byte, a register each iteration writes */
        emu_ir_put(&g_built, 1u, E(EMU_IR_ADDI, 0u, ts, EMU_IR_NO_TEMP, 1u, 0u));
    }
    const uint16_t td = emu_ir_get(&g_built, 2u);
    const uint16_t td2 = E(EMU_IR_ADDI, 0u, td, EMU_IR_NO_TEMP, 1u, 0u);

    emu_ir_put(&g_built, 2u, td2);
    if ((f & 0x80u) != 0u) {
        (void)E(EMU_IR_STORE, EMU_IR_MEM_AUX(1u, false), td2, v, 0xFFFFFFFFu, 0u);
    } else {
        (void)E(EMU_IR_STORE, EMU_IR_MEM_AUX(1u, false), td, v, 0u, 0u);
    }
    (void)E(EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 4u, 0u);
    if (kind == 0u) {
        (void)E(EMU_IR_EXIT_IF, EMU_IR_C_NE, v, emu_ir_const(&g_built, 0u), START_PC, 0u);
    } else {
        (void)E(EMU_IR_EXIT_IF, EMU_IR_C_NE, emu_ir_get(&g_built, 4u), td2, START_PC, 0u);
    }
    (void)E(EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
    (void)E(EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, START_PC + 0x40u, 0u);
}

static const emu_ir_target_t *build(in_t *in, fcpu_t *c)
{
    const uint32_t cfg = byte(in);
    const uint32_t cfg2 = byte(in);
    /*
     * ARMv7-M's targets take half of the ARM layout's inputs, keyed on a
     * bit of cfg2 nothing else reads there; no regression input has the
     * ARM layout, so none changed meaning.
     */
    const bool arm = (cfg2 % 3u) == 1u && ((cfg2 / 3u) & 1u) != 0u;
    /*
     * And the word alone, ARM's or G4MH's, on the next bit, which no
     * regression input with either layout sets: vmems-stride-in-a-moved-
     * register keeps its register on the flag word.
     */
    const bool alone = (cfg2 % 3u) == 1u && ((cfg2 / 6u) & 1u) != 0u;
    const emu_ir_target_t *const t =
        (arm && alone) ? &k_targets[16u + (cfg & 3u)]
        : arm          ? &k_targets[12u + (cfg & 3u)]
        : alone        ? &k_targets[20u + (cfg & 3u)]
                       : &k_targets[(cfg & 3u) * 3u + (cfg2 % 3u)];

    g_tgt = t;

    memset(c, 0, sizeof(*c));
    for (uint32_t i = 0; i < 16u; i++) {
        c->r[i] = interesting(byte(in));
    }
    c->flags = word(in);
    if (t->flags_alone) { /* the promise flags_alone makes */
        c->flags &= t->flag_bit[0] | t->flag_bit[1] | t->flag_bit[2] | t->flag_bit[3];
    }
    c->pc = START_PC;
    for (uint32_t i = 0; i < 8u; i++) {
        c->f[i] = interesting_f(byte(in));
    }
    c->guard = (cfg & 0x10u) ? 1u : 0u;
    /* The vector registers from a pattern, so a lane written shows. */
    for (uint32_t i = 0; i < sizeof(c->v); i++) {
        ((uint8_t *)c->v)[i] = (uint8_t)(0x5Au ^ (i * 29u) ^ (i >> 4));
    }
    for (uint32_t i = 0; i < 4u; i++) {
        c->tlb[i].read = EMU_DTLB_NONE;
        c->tlb[i].write = EMU_DTLB_NONE;
    }

    uint32_t seed = word(in) | 1u;

    g_mem = g_ram;
    g_mem_size = PAGE;
    for (uint32_t i = 0; i < PAGE; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        g_ram[i] = (uint8_t)seed;
        g_rom[i] = (uint8_t)(seed >> 8);
        if (i < sizeof(g_ram_odd)) {
            g_ram_odd[i] = (uint8_t)seed;
        }
    }
    g_dev_reads = 0u;
    g_dev_log = 0u;

    emu_ir_reset(&g_built);
    g_built.start_pc = START_PC;
    g_npool = 0u;

    switch ((cfg >> 2) & 3u) {
    case 1: /* the RAM window, whole or a part of it, and the image */
    case 3:
        g_built.has_fast = true;
        if ((cfg & 0xC0u) == 0xC0u) {
            /*
             * RAM of a size that is not a multiple of four, which nothing
             * forbids, in a buffer that ends where it does. With a size
             * that is a multiple, "the access's last byte is inside" and
             * "an aligned access starting inside" are the same test, so
             * a guard that checked the second would pass; and the window
             * has to end where its host memory does, or a guard that
             * reads past it reads the same bytes the helper would.
             */
            g_mem = g_ram_odd;
            g_mem_size = sizeof(g_ram_odd);
            g_built.fast.base = RAM_BASE;
            g_built.fast.size = g_mem_size;
        } else if ((cfg & 0x40u) != 0u) {
            g_built.fast.base = RAM_BASE + 0x100u;
            g_built.fast.size = 0x200u;
        } else {
            g_built.fast.base = RAM_BASE;
            g_built.fast.size = PAGE;
        }
        g_built.fast.host = &g_mem[g_built.fast.base - RAM_BASE];
        g_built.fast.store_guard_offset =
            (cfg & 0x20u) ? (uint32_t)offsetof(fcpu_t, guard)
                          : EMU_IR_NO_GUARD;
        if (((cfg >> 2) & 3u) == 3u) {
            g_built.has_rom = true;
            g_built.rom.base = ROM_BASE;
            g_built.rom.size = PAGE;
            g_built.rom.host = g_rom;
            g_built.rom.store_guard_offset = EMU_IR_NO_GUARD;
        }
        break;
    case 2: /* the data TLB: RAM, perhaps read-only, and the image */
        g_built.has_dtlb = true;
        g_built.dtlb.table_offset = (uint32_t)offsetof(fcpu_t, tlb);
        g_built.dtlb.index_mask = 3u;
        g_built.dtlb.store_guard_offset =
            (cfg & 0x20u) ? (uint32_t)offsetof(fcpu_t, guard)
                          : EMU_IR_NO_GUARD;
        c->tlb[1].read = RAM_BASE >> EMU_DTLB_PAGE_SHIFT;
        c->tlb[1].write = (cfg & 0x40u) ? EMU_DTLB_NONE
                                        : RAM_BASE >> EMU_DTLB_PAGE_SHIFT;
        c->tlb[1].addend = (uintptr_t)g_ram - RAM_BASE;
        c->tlb[0].read = ROM_BASE >> EMU_DTLB_PAGE_SHIFT;
        c->tlb[0].addend = (uintptr_t)g_rom - ROM_BASE;
        break;
    default: /* every access through the target's load and store */
        break;
    }

    /*
     * At least one instruction, so at least one RETIRE before any exit:
     * a back edge that retires nothing loops for ever in compiled code,
     * whose bound is the retired count. Every frontend's block retires
     * an instruction before it can leave.
     */
    if (cfg2 >= 240u && g_built.has_fast) {
        if (cfg2 >= 248u) {
            build_bytes(in, c);
        } else {
            build_dot(in, c);
        }
        g_built.guest_insns = 4u;
        return t;
    }

    uint32_t pc = START_PC;
    const uint32_t n = 1u + pick(in, 40u);

    for (uint32_t i = 0; i < n && (i == 0u || in->n > 0u); i++) {
        (void)one(in, pc, pc + 4u);
        pc += 4u;
    }

    /* The end: on, back to the start, or to a computed address. */
    const uint32_t how = pick(in, 4u);

    if (how == 3u) {
        (void)E(EMU_IR_EXIT, 0u, val(in), EMU_IR_NO_TEMP, 0u, 0u);
    } else {
        (void)E(EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                (how == 1u) ? START_PC : pc, 0u);
    }
    g_built.guest_insns = n;
    return t;
}

/* ------------------------------------------------------------------ */
/* Running it three ways                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    fcpu_t cpu;
    uint8_t ram[PAGE];
    uint32_t dev_reads, dev_log, retired;
} side_t;

static fcpu_t g_init;
static uint8_t g_init_ram[PAGE];

static void restore(fcpu_t *c)
{
    *c = g_init;
    memcpy(g_mem, g_init_ram, g_mem_size);
    g_dev_reads = 0u;
    g_dev_log = 0u;
}

static void save(side_t *s, const fcpu_t *c, uint32_t retired)
{
    s->cpu = *c;
    memcpy(s->ram, g_mem, g_mem_size);
    s->dev_reads = g_dev_reads;
    s->dev_log = g_dev_log;
    s->retired = retired;
}

#if defined(EMU_IR_BOARD)
/* One block at a time; RAM the M7 executes from. */
#define CODE_BYTES (32u * 1024u)
static uint8_t g_code_board[CODE_BYTES] __attribute__((aligned(8)));
static uint8_t *g_code = g_code_board;
#else
#define CODE_BYTES (256u * 1024u)
static uint8_t *g_code;
#endif

static const char *op_name(uint8_t op)
{
    return emu_ir_op_name(op);
}

static void dump(const char *what, const emu_ir_block_t *b)
{
    fprintf(stderr, "-- %s: %u instructions, %u temps%s%s%s\n", what,
            (unsigned)b->count, (unsigned)b->next_temp,
            b->has_fast ? ", RAM window" : "", b->has_rom ? ", image" : "",
            b->has_dtlb ? ", data TLB" : "");
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];
        char t[3][8];
        const uint16_t o[3] = {in->a, in->b, in->c};

        for (unsigned k = 0; k < 3u; k++) {
            if (o[k] == EMU_IR_NO_TEMP) {
                strcpy(t[k], "-");
            } else {
                snprintf(t[k], sizeof(t[k]), "t%u", (unsigned)o[k]);
            }
        }
        fprintf(stderr, "  %3u %s%-11s aux %02x  ", (unsigned)i,
                in->dead ? "x " : "  ", op_name(in->op), in->aux);
        if (in->dst != EMU_IR_NO_TEMP) {
            fprintf(stderr, "t%-4u= ", (unsigned)in->dst);
        } else {
            fprintf(stderr, "       ");
        }
        fprintf(stderr, "%-5s %-5s %-5s imm 0x%08x defs %x live %x\n", t[0],
                t[1], t[2], (unsigned)in->imm, in->defs, in->live);
    }
}

static int g_show = -1;

/* Every word two sides may disagree on; `fe` alone is not compared. */
static bool same(const side_t *x, const side_t *y, bool print,
                 const char *xn, const char *yn)
{
    bool ok = true;

#define CMP(name, idx, a, b)                                                   \
    do {                                                                       \
        if ((uint64_t)(a) != (uint64_t)(b)) {                                  \
            ok = false;                                                        \
            if (print) {                                                       \
                fprintf(stderr, "  %-9s[%2u] %s 0x%016llx %s 0x%016llx\n",     \
                        name, (unsigned)(idx), xn,                             \
                        (unsigned long long)(a), yn,                           \
                        (unsigned long long)(b));                              \
            }                                                                  \
        }                                                                      \
    } while (0)

    for (unsigned i = 0; i < 16u; i++) {
        CMP("r", i, x->cpu.r[i], y->cpu.r[i]);
    }
    CMP("flags", 0, x->cpu.flags, y->cpu.flags);
    CMP("pc", 0, x->cpu.pc, y->cpu.pc);
    for (unsigned i = 0; i < 8u; i++) {
        CMP("f", i, x->cpu.f[i], y->cpu.f[i]);
    }
    CMP("cause", 0, x->cpu.cause, y->cpu.cause);
    CMP("cause_pc", 0, x->cpu.cause_pc, y->cpu.cause_pc);
    CMP("helpers", 0, x->cpu.hlog, y->cpu.hlog);
    CMP("guard", 0, x->cpu.guard, y->cpu.guard);
    CMP("vsat", 0, x->cpu.vsat, y->cpu.vsat);
    for (unsigned i = 0; i < sizeof(x->cpu.v) / 4u; i++) {
        uint32_t a, b;

        memcpy(&a, (const uint8_t *)x->cpu.v + 4u * i, 4u);
        memcpy(&b, (const uint8_t *)y->cpu.v + 4u * i, 4u);
        CMP("v word", i, a, b);
    }
    CMP("retired", 0, x->retired, y->retired);
    CMP("dev reads", 0, x->dev_reads, y->dev_reads);
    CMP("dev log", 0, x->dev_log, y->dev_log);
    for (unsigned i = 0; i < g_mem_size; i++) {
        CMP("ram byte", i, x->ram[i], y->ram[i]);
    }
#undef CMP
    return ok;
}

static void print_stats(void)
{
    emu_ir_opt_stats_t o;

    emu_ir_opt_totals(&o);
    fprintf(stderr,
            "fuzz-ir: %llu inputs, %llu compared, %llu declined by the "
            "reference, %llu not lowered; %llu looped, %llu trapped, "
            "%llu helper calls, %llu accesses by the target's functions "
            "(%llu to the device)\n",
            g_st.inputs, g_st.compared, g_st.declined, g_st.unlowered,
            g_st.loops, g_st.traps, g_st.helpers, g_st.mem_helper, g_st.dev);
    fprintf(stderr,
            "fuzz-ir: passes over %u blocks: gets %u puts %u setpcs %u "
            "flags %u dead %u folded %u addr %u identities %u mac %u "
            "retires %u zero %u cond %u branch %u\n",
            (unsigned)o.blocks, (unsigned)o.gets_removed,
            (unsigned)o.puts_removed, (unsigned)o.setpcs_removed,
            (unsigned)o.flags_removed, (unsigned)o.dead_removed,
            (unsigned)o.folded, (unsigned)o.addr_folded,
            (unsigned)o.identities, (unsigned)o.macs,
            (unsigned)o.retires_merged, (unsigned)o.zero_gets,
            (unsigned)o.conds_forwarded, (unsigned)o.branch_folded);
    {
        uint32_t loops = 0u, kernels = 0u;

        for (uint32_t k = 0; k < EMU_IR_LOOP_KINDS; k++) {
            loops += o.loop_shape[k];
            kernels += (k < EMU_IR_LOOP_AFFINE) ? o.loop_shape[k] : 0u;
        }
        uint32_t kn = 0u, kc = 0u, kr = 0u, ki = 0u, kf = 0u;

#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE
        emu_ir_kernel_stats(&kn, &kc, &kr, &ki, &kf);
#endif
        fprintf(stderr,
                "fuzz-ir: loops analysed %u, a kernel's shape %u, kernels %u (%u made, "
                "%u calls, %u ran %u iterations, %u left scalar by a full table)\n",
                (unsigned)loops, (unsigned)kernels, (unsigned)o.kernels, (unsigned)kn,
                (unsigned)kc, (unsigned)kr, (unsigned)ki, (unsigned)kf);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static side_t want, opt, got;
    static fcpu_t cpu;
    in_t in = {data, size};

    if (g_show < 0) {
#if defined(EMU_IR_BOARD)
        g_show = 0; /* the replay prints the differences it finds */
#else
        g_show = getenv("FUZZ_SHOW") != NULL;
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(print_stats);
        }
        g_code = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_code == MAP_FAILED) {
            fprintf(stderr, "fuzz-ir: no executable mapping\n");
            abort();
        }
#endif
    }
    g_st.inputs++;
#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE
    emu_ir_kernel_reset(); /* the last input's blocks are gone */
    emu_ir_kernel_kinds = 0xFFFFFFFFu; /* every kernel, used or not by default */
#endif

    const emu_ir_target_t *const t = build(&in, &g_init);

    memcpy(g_init_ram, g_mem, g_mem_size);
    if (g_built.overflow) {
        g_st.declined++;
        return 0;
    }

    /* want: the block as built. */
    uint32_t n = 0u;

    restore(&cpu);
    if (!emu_ir_interp(&g_built, (emu_cpu_t *)(void *)&cpu, t, &n)) {
        g_st.declined++;
        return 0;
    }
    save(&want, &cpu, n);

    /* opt: the same reference on what the passes made of it. */
    memcpy(&g_opt, &g_built, sizeof(g_opt));
    emu_ir_optimise(&g_opt, t, EMU_IR_F_ALL, NULL);
    restore(&cpu);
    n = 0u;
    const bool opt_ran =
        emu_ir_interp(&g_opt, (emu_cpu_t *)(void *)&cpu, t, &n);
    save(&opt, &cpu, n);

    /* got: the optimised block, compiled and called. */
    bool lowered = true;

    memset(&emu_jit_layout, 0, sizeof(emu_jit_layout));
    emu_jit_emit_begin(g_code, CODE_BYTES);
    if (!emu_ir_lower(&g_opt, t) || emu_jit_overflowed()) {
        g_st.unlowered++;
        lowered = false;
        if (g_show) {
            dump("optimised, not lowered", &g_opt);
            for (uint32_t i = 0; i < g_opt.count; i++) {
                const emu_ir_insn_t *const x = &g_opt.insn[i];

                if (!x->dead &&
                    !emu_ir_can_lower((emu_ir_op_t)x->op, x->aux)) {
                    fprintf(stderr, "  can_lower says no to %u: %s aux %02x\n",
                            (unsigned)i, op_name(x->op), x->aux);
                }
            }
            /* Each instruction alone, to name the one it refused. */
            static emu_ir_block_t one_insn;

            for (uint32_t i = 0; i < g_opt.count; i++) {
                if (g_opt.insn[i].dead) {
                    continue;
                }
                one_insn = g_opt;
                one_insn.count = 1u;
                one_insn.insn[0] = g_opt.insn[i];
                memset(&emu_jit_layout, 0, sizeof(emu_jit_layout));
                emu_jit_emit_begin(g_code, CODE_BYTES);
                if (!emu_ir_lower(&one_insn, t) || emu_jit_overflowed()) {
                    fprintf(stderr, "  the lowering refuses %u alone: %s\n",
                            (unsigned)i, op_name(g_opt.insn[i].op));
                }
            }
        }
        if (!opt_ran || !same(&want, &opt, false, "", "")) {
            goto differ; /* a pass is wrong whatever the lowering does */
        }
        return 0;
    }
    restore(&cpu);
#if defined(EMU_IR_BOARD)
    /*
     * The Thumb bit, and the memory stub in r1, which every block's
     * slow path reaches through r11 (emu_jit_block_arg). The caches are
     * off in this firmware, so what was written is what is fetched.
     */
    n = ((uint32_t(*)(void *, const void *))((uintptr_t)g_code | 1u))(
        &cpu, emu_jit_block_arg);
#else
    n = ((uint32_t(*)(void *, const void *))(void *)g_code)(&cpu, NULL);
#endif
    /* A constant exit leaves unlinked here, and says so in the count. */
    n &= ~EMU_JIT_RET_UNLINKED;
    save(&got, &cpu, n);

    g_st.compared++;
    if (got.cpu.pc == TRAP_PC || got.cpu.pc == HTRAP_PC) {
        g_st.traps++;
    }
    if (want.retired > g_built.guest_insns) {
        g_st.loops++;
    }

    if (g_show) {
        dump("built", &g_built);
        dump("optimised", &g_opt);
        fprintf(stderr, "-- want against got (retired %u, %u)\n",
                (unsigned)want.retired, (unsigned)got.retired);
        (void)same(&want, &got, true, "want", "got");
    }
    if (opt_ran && same(&want, &got, false, "", "") &&
        same(&want, &opt, false, "", "")) {
        return 0;
    }

differ:
    fprintf(stderr, "fuzz-ir: the block as built and as compiled disagree\n");
    dump("built", &g_built);
    dump("optimised", &g_opt);
    if (!opt_ran) {
        fprintf(stderr, "-- the reference declined the optimised block\n");
    } else {
        fprintf(stderr, "-- want (as built) against opt (as optimised): "
                        "a difference here is a pass\n");
        (void)same(&want, &opt, true, "want", "opt");
    }
    if (lowered) {
        fprintf(stderr, "-- opt against got (compiled): a difference here "
                        "is the lowering\n");
        (void)same(&opt, &got, true, "opt", "got");
    } else {
        fprintf(stderr, "-- (the lowering declined the block)\n");
    }
#if defined(EMU_IR_BOARD)
    fuzz_ir_board_differ();
    return 0;
#else
    abort();
#endif
}

#if defined(EMU_IR_BOARD)
void fuzz_ir_stats(void)
{
    print_stats();
}
#endif
