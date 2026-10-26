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
 * none), FSQRT and FMA (the reference declines them), and the Thumb-2
 * lowering, which needs a board.
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
#include <sys/mman.h>

#if !defined(EMU_HOST_JIT_X86_64)
#error "fuzz-ir runs the x86-64 lowering"
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
static uint32_t h_mix(emu_cpu_t *cpu, uint32_t a, uint32_t b)
{
    fcpu_t *const c = (fcpu_t *)(void *)cpu;

    g_st.helpers++;
    c->hlog = (c->hlog ^ a ^ (b << 1)) * 0x01000193u;
    return a * 31u + b;
}

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
    c->flags ^= (b ^ (b >> 28)) & 0xF000000Fu;
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

static const void *const k_helpers[] = {
    (const void *)h_mix, (const void *)h_peek, (const void *)h_poke,
    (const void *)h_trap};
#define H_TRAP 3u

static void f_fp_flags(emu_cpu_t *cpu, uint32_t flags)
{
    ((fcpu_t *)(void *)cpu)->fe |= flags;
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

static uint32_t off_freg(uint32_t n)
{
    return (uint32_t)(offsetof(fcpu_t, f) + 8u * n);
}

/* Three flag layouts: low bits, ARM's N Z C V at the top, and none. */
#define TGT(ro, rz, z, s, v, cy)                                               \
    {                                                                          \
        .reg_offset = (ro), .flags_offset = offsetof(fcpu_t, flags),          \
        .flag_bit = {(z), (s), (v), (cy)}, .reg_is_zero = (rz),                \
        .pc_offset = offsetof(fcpu_t, pc), .helpers = k_helpers,               \
        .helper_count = 4u, .load = f_load, .store = f_store,                  \
        .freg_offset = off_freg, .fp_flags = f_fp_flags,                       \
    }
#define TGT_FLAGS(ro, rz)                                                      \
    TGT(ro, rz, 1u, 2u, 4u, 8u),                                               \
        TGT(ro, rz, 1u << 30, 1u << 31, 1u << 28, 1u << 29),                   \
        TGT(ro, rz, 0u, 0u, 0u, 0u)

static const emu_ir_target_t k_targets[12] = {
    TGT_FLAGS(off_reg, NULL), TGT_FLAGS(off_reg, zero_r0),
    TGT_FLAGS(off_reg_pc, NULL), TGT_FLAGS(off_reg_pc, zero_r0)};

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
    const uint32_t kind = pick(in, 20u);
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

        if (lowerable((emu_ir_op_t)op, 0u)) {
            d = E((emu_ir_op_t)op, 0u, val(in), EMU_IR_NO_TEMP, 0u, 0u);
        }
        break;
    }
    case 5: { /* compare to a value */
        const uint16_t a = val(in), b = val(in);

        d = E(EMU_IR_SETCC, (uint8_t)pick(in, 10u), a, b, 0u, 0u);
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
            const uint8_t cond = (uint8_t)pick(in, 11u);
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
            p = E(EMU_IR_GETCOND, (uint8_t)pick(in, 11u), EMU_IR_NO_TEMP,
                  EMU_IR_NO_TEMP, 0u, 0u);
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
        d = E(EMU_IR_GETCOND, (uint8_t)pick(in, 11u), EMU_IR_NO_TEMP,
              EMU_IR_NO_TEMP, 0u, 0u);
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
            d = E(EMU_IR_HELPER, 0u, a, b, h, 0u);
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

        (void)E(EMU_IR_EXIT_IF, (uint8_t)pick(in, 10u), a, b, to, 0u);
    }
    return true;
}

/*
 * The configuration bytes: which target (zero register, pc register,
 * flag layout), how memory is reached -- and which window, all of RAM,
 * part of it, or part at an odd base -- and the store guard.
 */
static const emu_ir_target_t *build(in_t *in, fcpu_t *c)
{
    const uint32_t cfg = byte(in);
    const uint32_t cfg2 = byte(in);
    const emu_ir_target_t *const t = &k_targets[(cfg & 3u) * 3u +
                                                (cfg2 % 3u)];

    memset(c, 0, sizeof(*c));
    for (uint32_t i = 0; i < 16u; i++) {
        c->r[i] = interesting(byte(in));
    }
    c->flags = word(in);
    c->pc = START_PC;
    for (uint32_t i = 0; i < 8u; i++) {
        c->f[i] = interesting_f(byte(in));
    }
    c->guard = (cfg & 0x10u) ? 1u : 0u;
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

#define CODE_BYTES (256u * 1024u)
static uint8_t *g_code;

static const char *op_name(uint8_t op)
{
#define N(x) [EMU_IR_##x] = #x
    static const char *const k[EMU_IR_OP_COUNT] = {
        N(NOP),     N(GET),       N(PUT),       N(CONST),     N(MOV),
        N(ADD),     N(SUB),       N(AND),       N(OR),        N(XOR),
        N(SHL),     N(SHR),       N(SAR),       N(ROTL),      N(MUL),
        N(MULHS),   N(MULHU),     N(DIVS),      N(DIVU),      N(REMS),
        N(REMU),    N(MAC),       N(ADDI),      N(ANDI),      N(ORI),
        N(XORI),    N(SHLI),      N(SHRI),      N(SARI),      N(ROTLI),
        N(NEG),     N(NOT),       N(BSWAP32),   N(BSWAP16),   N(HSWAP),
        N(CLZ),     N(CTZ),       N(POPCNT),    N(BEXT),      N(BSET),
        N(BCLR),    N(BINV),      N(BITOP_SET), N(BITOP_CLR), N(BITOP_INV),
        N(BITOP_TST), N(SEXT8),   N(SEXT16),    N(ZEXT8),     N(ZEXT16),
        N(LOAD),    N(STORE),     N(SETF),      N(GETCOND),   N(SETCC),
        N(SELECT),  N(FGET),      N(FPUT),      N(FADD),      N(FSUB),
        N(FMUL),    N(FDIV),      N(FSQRT),     N(FMA),       N(FMIN),
        N(FMAX),    N(FSGNJ),     N(FCMP),      N(FCVT_TO_I), N(FCVT_FROM_I),
        N(FCLASS),  N(HELPER),    N(HELPER_TRAP), N(RETIRE),  N(SETPC),
        N(EXIT),    N(EXIT_IF)};
#undef N
    return (op < EMU_IR_OP_COUNT && k[op] != NULL) ? k[op] : "?";
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
            "retires %u\n",
            (unsigned)o.blocks, (unsigned)o.gets_removed,
            (unsigned)o.puts_removed, (unsigned)o.setpcs_removed,
            (unsigned)o.flags_removed, (unsigned)o.dead_removed,
            (unsigned)o.folded, (unsigned)o.addr_folded,
            (unsigned)o.identities, (unsigned)o.macs,
            (unsigned)o.retires_merged);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static side_t want, opt, got;
    static fcpu_t cpu;
    in_t in = {data, size};

    if (g_show < 0) {
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
    }
    g_st.inputs++;

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
    n = ((uint32_t(*)(void *, const void *))(void *)g_code)(&cpu, NULL);
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
    abort();
}
