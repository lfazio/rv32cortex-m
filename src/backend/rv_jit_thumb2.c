/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_jit_thumb2.c - Thumb-2 JIT backend.
 *
 * See include/rv32/rv_jit.h for the design rationale. In short: RV32 basic
 * blocks are translated into Thumb-2 held in a RAM code cache, the guest
 * register file stays in the hart struct (so guest state is coherent at
 * every instruction boundary), and anything not translated ends the block
 * and is executed by the interpreter.
 *
 * All encodings below are from the ARMv7-M Architecture Reference Manual.
 * Each emitter names the encoding it uses (T1, T3, ...) because the same
 * mnemonic often has several, and picking the wrong one produces code that
 * assembles into something plausible and behaves subtly differently.
 */

#include "rv32/rv_jit.h"

#if RV_ENABLE_JIT

#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_hart.h"

#include <stddef.h>
#include <string.h>

/*
 * The translated code addresses the guest register file as [r4, #n*4]
 * using the 16-bit LDR/STR encoding, which requires x[] at offset 0 and a
 * maximum byte offset of 124. Both hold by construction; assert it so a
 * struct reshuffle fails the build instead of miscompiling guests.
 */
_Static_assert(offsetof(rv_hart_t, x) == 0,
               "translated code assumes hart->x is at offset 0");
_Static_assert(sizeof(((rv_hart_t *)0)->x) == 32u * 4u,
               "translated code assumes 32 32-bit guest registers");

#define HART_PC_OFF   ((uint32_t)offsetof(rv_hart_t, pc))
#if RV_EXT_A
#define HART_RESV_OFF ((uint32_t)offsetof(rv_hart_t, resv_valid))
#endif
#if RV_EXT_F
#define HART_F_OFF    ((uint32_t)offsetof(rv_hart_t, f))
/* The f file sits past the 124-byte reach of the 16-bit LDR/STR form, so
 * it is addressed with the 32-bit imm12 encodings below. */
_Static_assert(offsetof(rv_hart_t, f) + 31u * 4u < 4096u,
               "translated code addresses hart->f with a 12-bit offset");
#endif

/* ARM registers used by translated code. */
#define R0  0u
#define R1  1u
#define R2  2u
#define R3  3u
#define R4  4u   /* hart pointer, callee-saved so it survives helper calls */
/*
 * Guest RAM described in callee-saved registers for the whole block, so an
 * inlined memory access is a subtract, a compare and a register-offset
 * load rather than a helper call. Materialising these three constants
 * costs ~6 halfwords once per block entry and saves ~30 cycles on every
 * load and store that hits RAM.
 */
#define R5  5u   /* guest RAM base   */
#define R6  6u   /* guest RAM size   */
#define R7  7u   /* host pointer to guest RAM base */
#define R12 12u  /* helper address before BLX */

/*
 * PUSH {r4-r8, lr} / POP {r4-r8, pc}, the 32-bit T2 forms.
 *
 * r8 holds the running count of instructions retired by this block. It has
 * to be callee-saved: a chained loop passes its exits more than once, so
 * the count must accumulate, and the helper calls clobber r0-r3. One extra
 * word of stack traffic per block buys the loop chaining below.
 */
#if RV_JIT_LOOP_CHAIN
#define PUSH_HW1 0xE92Du
#define PUSH_HW2 0x41F0u   /* {r4-r8, lr} */
#define POP_HW1  0xE8BDu
#define POP_HW2  0x81F0u   /* {r4-r8, pc} */
#else
/* Without chaining the count is a per-path constant, so r8 is not needed
 * and the 16-bit forms suffice. */
#define PUSH_HW1 0xB5F0u   /* PUSH {r4-r7, lr}, 16-bit  */
#define PUSH_HW2 0u
#define POP_HW1  0xBDF0u   /* POP  {r4-r7, pc}, 16-bit  */
#define POP_HW2  0u
#endif
#define R8 8u

/* Condition codes. */
#define C_EQ 0u
#define C_NE 1u
#define C_CS 2u   /* unsigned >= */
#define C_CC 3u   /* unsigned <  */
#define C_GE 10u
#define C_LT 11u

/* ------------------------------------------------------------------ */
/* Code cache                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  guest_pc;
    uint16_t *code;        /* first halfword of the translated block  */
    uint16_t  code_len;    /* bytes, needed to relocate during compaction */
    uint16_t  insns;       /* guest instructions the block retires    */
    uint16_t  hits;        /* executions since the last ageing pass   */
    int16_t   next;        /* hash chain, -1 terminates               */
} jit_block_t;

/*
 * Hit counts are bucketed rather than compared directly, so choosing what
 * to retain is a histogram walk instead of a sort.
 */
#define HIT_BINS 16u

static uint32_t hit_bin(uint32_t hits)
{
    return (hits >= HIT_BINS) ? (HIT_BINS - 1u) : hits;
}

/*
 * Space kept free so a compaction leaves room to translate. A block is at
 * most RV_JIT_MAX_BLOCK_INSNS instructions and the largest translation is
 * well under 48 bytes per instruction, but the emitter bounds-checks every
 * write anyway, so this only avoids wasted work.
 */
#define JIT_HEADROOM 512u

static uint8_t     *g_code;
static uint32_t     g_code_size;
static uint32_t     g_code_used;
static jit_block_t  g_blocks[RV_JIT_MAX_BLOCKS];
static uint32_t     g_block_count;
static int16_t      g_hash[RV_JIT_HASH_SIZE];
static bool         g_hash_ready;
static rv_jit_stats_t g_stats;

/*
 * Guest RAM for the block being translated, and whether r5/r6/r7 have been
 * loaded with it yet.
 *
 * The constants cost six halfwords, and a block with no memory access has
 * no use for them -- which at an average block of about seven instructions
 * is a real share of the prologue. They are therefore emitted lazily, at
 * the first access that needs them rather than on entry.
 *
 * Lazy emission is safe here without a pre-pass: a block has one entry, and
 * every path that reaches a given instruction has flowed through everything
 * emitted before it. Earlier exits (a taken branch, a faulting helper) leave
 * before the emission point and never read the registers.
 */
/* The hart being translated for, so translate_one can read mstatus. */
static rv_hart_t *g_xlate_hart;

#if RV_EXT_PMP
/*
 * The value of hart->pmp_active the cached blocks were translated against.
 *
 * The inlined memory path writes guest RAM directly and so cannot consult
 * PMP. That is fine while PMP cannot deny anything, which is the state
 * until a guest locks an entry -- but blocks are translated once and reused,
 * so a block emitted before the lock would keep bypassing the check
 * afterwards. Comparing this on each block dispatch and flushing on a
 * change costs one load and one compare per block, and nothing per
 * instruction.
 */
static bool g_pmp_seen;
#endif

/*
 * Loop chaining state for the block being translated.
 *
 * g_loop_start is the first instruction after the prologue, and g_block_pc
 * the guest pc it corresponds to. A backward jump landing exactly there is
 * a loop whose body is this block, and can branch within the translated
 * code instead of returning to the dispatcher.
 */
static uint16_t *g_loop_start;
static uint32_t  g_block_pc;

#if RV_JIT_LOOP_CHAIN
/*
 * Where each guest instruction of this block starts in the emitted code.
 *
 * Matching only the block's first pc caught just the loops whose body is
 * exactly one block beginning at the loop head, which was 1% of block
 * entries. Recording every instruction lets a backward jump into the middle
 * of a block chain too, which is the common shape once forward branches
 * have been merged into the block.
 */
static uint32_t  g_pc_map[RV_JIT_MAX_BLOCK_INSNS];
static uint16_t *g_code_map[RV_JIT_MAX_BLOCK_INSNS];
static uint32_t  g_map_len;

/*
 * Instructions already added to the accumulator along the path being
 * emitted. Exits add only what they retired since this point, which is what
 * makes the count right once an edge has accumulated mid-block.
 */
static uint32_t  g_acc_base;

/* Emitted code for `pc` in this block, or NULL. */
static uint16_t *chain_target(uint32_t pc)
{
    for (uint32_t i = 0; i < g_map_len; i++) {
        if (g_pc_map[i] == pc) {
            return g_code_map[i];
        }
    }
    return NULL;
}
#endif

static bool     g_regions_scanned;
static uint32_t g_ram_base;
static uint32_t g_ram_size;
static uint32_t g_ram_host;
static bool     g_ram_live;

#if RV_JIT_INLINE_PERIPH
/*
 * The inlinable part of the peripheral window: one contiguous run of
 * identity-mapped passthrough regions, minus the sub-ranges a store must
 * not take. Holes are held as offsets from the window base rather than as
 * absolute addresses, which is not just tidiness -- 0x40023800 is not a
 * Thumb-2 modified immediate but 0x23800 is, so the offset form makes each
 * hole test two instructions instead of five.
 *
 * A size of zero disables the path, which is what the host build gets:
 * there the window is ordinary simulated RAM, not passthrough at all.
 */
static uint32_t g_pt_base;
static uint32_t g_pt_size;
static struct { uint32_t off, size; } g_pt_hole[RV_JIT_PT_MAX_HOLES];
static uint32_t g_pt_holes;
static bool     g_pt_store_ok;

/*
 * Whether to emit the window test at all, and the count of passthrough
 * accesses that have gone through the helper so far.
 *
 * This is not emitted unconditionally because the code it adds is not free:
 * about 18 bytes per load and 48 per store, which on CoreMark grew the
 * translated image from 39.6 KB to 48.5 KB against a 48 KB cache. The
 * result was constant compaction -- evictions went from 27k to 42k -- and
 * CoreMark ran 53% slower. Driver code gained 2-3x and compute code lost
 * half its speed, which is not a trade worth making in either direction.
 *
 * So the guest decides. A guest that touches the window arms the path and
 * pays one flush; a guest that never does never pays anything, and that
 * includes every compute benchmark, whose console is a virtual device
 * outside the window rather than real silicon inside it.
 */
static bool     g_pt_armed;
static uint32_t g_pt_hits;
#endif /* RV_JIT_INLINE_PERIPH */

/* Emission cursor, valid only while translating. */
static uint16_t *g_emit;
static uint16_t *g_emit_end;
static bool      g_emit_overflow;

static uint32_t pc_hash(uint32_t pc)
{
    /* pc is at least 2-byte aligned, so the low bit carries nothing. */
    return (pc >> 1) & (RV_JIT_HASH_SIZE - 1u);
}

void rv_jit_set_code_buffer(void *buf, uint32_t size)
{
    g_code = (uint8_t *)buf;
    g_code_size = size;
    rv_jit_flush();
}

void rv_jit_flush(void)
{
    g_regions_scanned = false;
    g_code_used = 0u;
    g_block_count = 0u;
    for (uint32_t i = 0; i < RV_JIT_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    g_hash_ready = true;
    g_stats.flushes++;
}

void rv_jit_get_stats(rv_jit_stats_t *out)
{
    *out = g_stats;
    out->blocks = g_block_count;
    out->code_used = g_code_used;
    out->code_size = g_code_size;
}

static jit_block_t *lookup(uint32_t pc)
{
    for (int16_t i = g_hash[pc_hash(pc)]; i >= 0; i = g_blocks[i].next) {
        if (g_blocks[i].guest_pc == pc) {
            /* Saturating: what matters is the ordering, not the magnitude. */
            if (g_blocks[i].hits != 0xFFFFu) {
                g_blocks[i].hits++;
            }
            return &g_blocks[i];
        }
    }
    return NULL;
}

static void rebuild_hash(void)
{
    for (uint32_t i = 0; i < RV_JIT_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    for (uint32_t i = 0; i < g_block_count; i++) {
        const uint32_t hidx = pc_hash(g_blocks[i].guest_pc);
        g_blocks[i].next = g_hash[hidx];
        g_hash[hidx] = (int16_t)i;
    }
}

/* Make the code just written visible to the instruction side. */
static void sync_icache(void)
{
#if defined(__ARM_ARCH)
    __asm__ volatile ("dsb 0xF" ::: "memory");
    __asm__ volatile ("isb 0xF" ::: "memory");
#endif
}

/*
 * Reclaim space by discarding the least-used blocks and sliding the rest
 * down, instead of throwing the whole cache away.
 *
 * This is only possible because translated blocks are position
 * independent: every guest pc and helper address is materialised as an
 * absolute constant with MOVW/MOVT, and the sole pc-relative branch is the
 * CBZ that skips a block's own early-exit. So a block can simply be moved.
 *
 * Retention is by hit count, with a budget so compaction leaves room to
 * translate afterwards, and an ageing pass so a block that was hot long
 * ago cannot hold its place forever.
 */
static void compact(void)
{
    uint32_t bytes[HIT_BINS];
    memset(bytes, 0, sizeof(bytes));

    for (uint32_t i = 0; i < g_block_count; i++) {
        bytes[hit_bin(g_blocks[i].hits)] += g_blocks[i].code_len;
    }

    /* Walk from hottest to coldest, taking bins while they fit. */
    const uint32_t budget = g_code_size - (g_code_size / 4u) - JIT_HEADROOM;
    uint32_t acc = 0u;
    uint32_t threshold = HIT_BINS;      /* nothing retained by default */
    for (int32_t b = (int32_t)HIT_BINS - 1; b >= 0; b--) {
        if (acc + bytes[b] > budget) {
            break;
        }
        acc += bytes[b];
        threshold = (uint32_t)b;
    }

    /*
     * Never retain blocks that have run only once: they are as likely to
     * be first-execution noise as working set, and keeping them is what
     * fills the cache in the first place.
     */
    if (threshold < 2u) {
        threshold = 2u;
    }

    uint8_t *dst = g_code;
    uint32_t kept = 0u;

    /* Blocks are appended by a bump allocator, so this array is already in
     * increasing code-address order and dst never overtakes the source. */
    for (uint32_t i = 0; i < g_block_count; i++) {
        jit_block_t b = g_blocks[i];
        if (hit_bin(b.hits) < threshold) {
            g_stats.evictions++;
            continue;
        }
        dst = (uint8_t *)(((uintptr_t)dst + 3u) & ~(uintptr_t)3u);
        if (dst != (uint8_t *)b.code) {
            memmove(dst, b.code, b.code_len);
        }
        b.code = (uint16_t *)(void *)dst;
        b.hits >>= 1;                   /* age */
        dst += b.code_len;
        g_blocks[kept++] = b;
    }

    g_block_count = kept;
    g_code_used = (uint32_t)(dst - g_code);
    rebuild_hash();
    sync_icache();
    g_stats.compactions++;
}

/* ------------------------------------------------------------------ */
/* Emitters                                                            */
/* ------------------------------------------------------------------ */

static void emit16(uint16_t hw)
{
    if (RV_UNLIKELY(g_emit >= g_emit_end)) {
        g_emit_overflow = true;
        return;
    }
    *g_emit++ = hw;
}

/* Thumb-2 32-bit instructions are stored as two halfwords, high first. */
static void emit32(uint16_t hw1, uint16_t hw2)
{
    emit16(hw1);
    emit16(hw2);
}

/* LDR Rt,[R4,#reg*4]  -- T1, requires Rt and Rn low and offset <= 124. */
static void emit_ld_greg(uint32_t rt, uint32_t greg)
{
    emit16((uint16_t)(0x6800u | (greg << 6) | (R4 << 3) | rt));
}

/* STR Rt,[R4,#reg*4] -- T1. Writes to x0 are dropped by the caller. */
static void emit_st_greg(uint32_t greg, uint32_t rt)
{
    emit16((uint16_t)(0x6000u | (greg << 6) | (R4 << 3) | rt));
}

/* STR<c>.W Rt,[Rn,#imm12] -- T3, for offsets the 16-bit form cannot reach. */
static void emit_str_imm12(uint32_t rt, uint32_t rn, uint32_t imm12)
{
    emit32((uint16_t)(0xF8C0u | rn), (uint16_t)((rt << 12) | imm12));
}

/* MOVW Rd,#imm16 -- T3. */
static void emit_movw(uint32_t rd, uint32_t imm16)
{
    const uint32_t imm4 = (imm16 >> 12) & 0xFu;
    const uint32_t i    = (imm16 >> 11) & 0x1u;
    const uint32_t imm3 = (imm16 >> 8) & 0x7u;
    const uint32_t imm8 = imm16 & 0xFFu;
    emit32((uint16_t)(0xF240u | (i << 10) | imm4),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

/* MOVT Rd,#imm16 -- T1. */
static void emit_movt(uint32_t rd, uint32_t imm16)
{
    const uint32_t imm4 = (imm16 >> 12) & 0xFu;
    const uint32_t i    = (imm16 >> 11) & 0x1u;
    const uint32_t imm3 = (imm16 >> 8) & 0x7u;
    const uint32_t imm8 = imm16 & 0xFFu;
    emit32((uint16_t)(0xF2C0u | (i << 10) | imm4),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

/* Materialise a 32-bit constant, skipping MOVT when the top half is zero. */
static void emit_imm32(uint32_t rd, uint32_t val)
{
    emit_movw(rd, val & 0xFFFFu);
    if ((val >> 16) != 0u) {
        emit_movt(rd, val >> 16);
    }
}

/*
 * Thumb-2 "modified immediate": the 12-bit field shared by the 32-bit
 * data-processing encodings. It represents either an 8-bit value splatted
 * into one, two or four bytes, or an 8-bit value with bit 7 set rotated
 * right by 8..31. Returns false for constants it cannot express, which the
 * callers handle by materialising the value into a register instead.
 *
 * Worth having because the two constants that matter here -- 0x40000000 and
 * 0x20000000, the base and size of the peripheral window -- both encode,
 * turning a four-instruction range test into two.
 */
static bool thumb_imm12(uint32_t val, uint32_t *out)
{
    const uint32_t b = val & 0xFFu;

    if (val < 0x100u) {
        *out = val;
        return true;
    }
    if ((val & 0xFF00FF00u) == 0u && b == ((val >> 16) & 0xFFu)) {
        *out = (1u << 8) | b;                       /* 0x00XY00XY */
        return true;
    }
    if ((val & 0x00FF00FFu) == 0u &&
        ((val >> 8) & 0xFFu) == ((val >> 24) & 0xFFu)) {
        *out = (2u << 8) | ((val >> 8) & 0xFFu);    /* 0xXY00XY00 */
        return true;
    }
    if (b == ((val >> 8) & 0xFFu) && b == ((val >> 16) & 0xFFu) &&
        b == ((val >> 24) & 0xFFu)) {
        *out = (3u << 8) | b;                       /* 0xXYXYXYXY */
        return true;
    }

    for (uint32_t rot = 8u; rot < 32u; rot++) {
        /* Rotating the target left by rot must recover the 8-bit source. */
        const uint32_t v = (val << rot) | (val >> (32u - rot));
        if (v <= 0xFFu && (v & 0x80u) != 0u) {
            *out = (rot << 7) | (v & 0x7Fu);
            return true;
        }
    }
    return false;
}

/* SUB.W Rd,Rn,#imm -- T3, taking an already-encoded modified immediate. */
static void emit_sub_imm_w(uint32_t rd, uint32_t rn, uint32_t enc)
{
    emit32((uint16_t)(0xF1A0u | (((enc >> 11) & 1u) << 10) | rn),
           (uint16_t)((((enc >> 8) & 7u) << 12) | (rd << 8) | (enc & 0xFFu)));
}

/* CMP.W Rn,#imm -- T2, which is SUBS with the result discarded into PC. */
static void emit_cmp_imm_w(uint32_t rn, uint32_t enc)
{
    emit32((uint16_t)(0xF1B0u | (((enc >> 11) & 1u) << 10) | rn),
           (uint16_t)((((enc >> 8) & 7u) << 12) | (0xFu << 8) | (enc & 0xFFu)));
}

/* MOV Rd,Rm -- T1, works across the full register file. */
static void emit_mov(uint32_t rd, uint32_t rm)
{
    emit16((uint16_t)(0x4600u | ((rd & 0x8u) << 4) | (rm << 3) | (rd & 0x7u)));
}

/* MOVS Rd,#imm8 -- T1. Inside an IT block this does not set flags. */
static void emit_mov_imm8(uint32_t rd, uint32_t imm8)
{
    emit16((uint16_t)(0x2000u | (rd << 8) | imm8));
}

/* ADD/SUB Rd,Rn,Rm -- T1, low registers only. */
static void emit_add_reg(uint32_t rd, uint32_t rn, uint32_t rm)
{
    emit16((uint16_t)(0x1800u | (rm << 6) | (rn << 3) | rd));
}

static void emit_sub_reg(uint32_t rd, uint32_t rn, uint32_t rm)
{
    emit16((uint16_t)(0x1A00u | (rm << 6) | (rn << 3) | rd));
}

/* Data-processing Rdn,Rm -- T1 (AND/EOR/ORR/shifts/CMP/MVN share a form). */
static void emit_dp_reg(uint16_t op, uint32_t rdn, uint32_t rm)
{
    emit16((uint16_t)(op | (rm << 3) | rdn));
}

/*
 * CMP Rn,Rm -- T2, the form that reaches the high registers.
 *
 * The T1 form above encodes three bits of Rn, so passing it r8 sets a bit
 * that belongs to Rm and it assembles as a comparison of two entirely
 * different registers. That is not hypothetical: the loop-chain cap did
 * exactly this, comparing r0 against the limit instead of the accumulator
 * in r8, so a chained loop never saw its cap and ran to completion in one
 * block entry -- 3700 guest instructions per entry where 64 was intended.
 * It looked like a win on throughput and was really the interrupt-latency
 * bound being silently discarded.
 */
static void emit_cmp_hi(uint32_t rn, uint32_t rm)
{
    emit16((uint16_t)(0x4500u | ((rn & 0x8u) << 4) | (rm << 3) | (rn & 0x7u)));
}

#define DP_AND 0x4000u
#define DP_EOR 0x4040u
#define DP_LSL 0x4080u
#define DP_LSR 0x40C0u
#define DP_ASR 0x4100u
#define DP_CMP 0x4280u
#define DP_ORR 0x4300u
#define DP_MUL 0x4340u
#define DP_BIC 0x4380u   /* Rdn &= ~Rm            */
#define DP_MVN 0x43C0u   /* Rd  = ~Rm             */
#define DP_ROR 0x41C0u   /* Rdn = Rdn ROR Rm[7:0] */
#define DP_RSB 0x4240u   /* Rd  = -Rm  (RSBS #0)  */

#if RV_EXT_ZBA
/* ADD.W Rd,Rn,Rm,LSL #imm5 -- T3. Exactly what sh1add/sh2add/sh3add want. */
static void emit_add_lsl(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t sh)
{
    emit32((uint16_t)(0xEB00u | rn),
           (uint16_t)((((sh >> 2) & 7u) << 12) | (rd << 8) |
                      ((sh & 3u) << 6) | rm));
}
#endif

#if RV_EXT_ZBB
/* Sign/zero extension, T1: SXTB/SXTH/UXTH Rd,Rm. */
#define XT_SXTH 0xB200u
#define XT_SXTB 0xB240u
#define XT_UXTH 0xB280u
#define XT_REV  0xBA00u   /* REV Rd,Rm: byte-reverse a word */

static void emit_xt(uint16_t op, uint32_t rd, uint32_t rm)
{
    emit16((uint16_t)(op | (rm << 3) | rd));
}

/* CLZ Rd,Rm -- T1, 32-bit. */
static void emit_clz(uint32_t rd, uint32_t rm)
{
    emit32((uint16_t)(0xFAB0u | rm), (uint16_t)(0xF080u | (rd << 8) | rm));
}

/* RBIT Rd,Rm -- T1, 32-bit. Reversing the bits turns ctz into clz. */
static void emit_rbit(uint32_t rd, uint32_t rm)
{
    emit32((uint16_t)(0xFA90u | rm), (uint16_t)(0xF0A0u | (rd << 8) | rm));
}
#endif

/* Shift Rd,Rm,#imm5 -- T1. */
static void emit_shift_imm(uint16_t op, uint32_t rd, uint32_t rm, uint32_t imm5)
{
    emit16((uint16_t)(op | (imm5 << 6) | (rm << 3) | rd));
}

#define SH_LSL 0x0000u
#define SH_LSR 0x0800u
#define SH_ASR 0x1000u

/* ADDW/SUBW Rd,Rn,#imm12 -- T4, no flags, full 0..4095 range. */
static void emit_addw(uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i    = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 7u;
    const uint32_t imm8 = imm12 & 0xFFu;
    emit32((uint16_t)(0xF200u | (i << 10) | rn),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

static void emit_subw(uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i    = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 7u;
    const uint32_t imm8 = imm12 & 0xFFu;
    emit32((uint16_t)(0xF2A0u | (i << 10) | rn),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

/* Add a signed 12-bit RISC-V immediate to a register, in place. */
static void emit_add_simm12(uint32_t rd, uint32_t rn, int32_t imm)
{
    if (imm >= 0) {
        emit_addw(rd, rn, (uint32_t)imm);
    } else {
        emit_subw(rd, rn, (uint32_t)(-imm));
    }
}

/* UBFX Rd,Rn,#0,#5 -- isolate a RISC-V shift amount. */
static void emit_ubfx5(uint32_t rd, uint32_t rn)
{
    emit32((uint16_t)(0xF3C0u | rn), (uint16_t)((rd << 8) | 4u));
}

/* BIC Rd,Rn,#1 -- T1 modified-immediate form, used to mask a JALR target. */
static void emit_bic1(uint32_t rd, uint32_t rn)
{
    emit32((uint16_t)(0xF020u | rn), (uint16_t)((rd << 8) | 1u));
}

/* IT <cond> -- one conditional instruction follows. */
static void emit_it(uint32_t cond)
{
    emit16((uint16_t)(0xBF08u | (cond << 4)));
}

/* CBZ Rn,<+4 bytes> -- skips the two-instruction early exit below. */
static void emit_cbz_skip4(uint32_t rn)
{
    emit16((uint16_t)(0xB100u | (1u << 3) | rn));
}

/* B<cond> with the displacement filled in later; returns where to patch. */
static uint16_t *emit_bcond_fwd(uint32_t cond)
{
    uint16_t *at = g_emit;
    emit16((uint16_t)(0xD000u | (cond << 8)));
    return at;
}

/*
 * Backward B<cond> to an already-emitted point, choosing the encoding by
 * reach: the 16-bit T1 form where it fits, the 32-bit T3 form (+/-1 MB)
 * where it does not.
 *
 * The widening matters more than the two bytes suggest. Loop chaining is
 * only emitted when the back edge is reachable, so before this existed a
 * block that outgrew T1's +/-254 bytes did not get a longer branch -- it
 * silently stopped chaining and went back through the dispatcher every
 * iteration. Inlining the peripheral window pushed two-access loop bodies
 * over that line and cost them 2.4x. The common case still gets T1, so
 * nothing is paid for the safety.
 */
static void emit_bcond_back(uint32_t cond, const uint16_t *dst)
{
    /* PC reads as the instruction address plus 4, for both encodings. */
    const int32_t off = (int32_t)((const uint8_t *)dst - (const uint8_t *)g_emit) - 4;

    if (off >= -252) {
        emit16((uint16_t)(0xD000u | (cond << 8) |
                          (((uint32_t)(off >> 1)) & 0xFFu)));
        return;
    }
    const uint32_t val = (uint32_t)off >> 1;   /* S:J2:J1:imm6:imm11 */
    emit32((uint16_t)(0xF000u | (((val >> 19) & 1u) << 10) | (cond << 6) |
                      ((val >> 11) & 0x3Fu)),
           (uint16_t)(0x8000u | (((val >> 17) & 1u) << 13) |
                      (((val >> 18) & 1u) << 11) | (val & 0x7FFu)));
}

/* Unconditional B, likewise. */
static uint16_t *emit_b_fwd(void)
{
    uint16_t *at = g_emit;
    emit16(0xE000u);
    return at;
}

static void patch_fwd(uint16_t *at, bool conditional)
{
    if (at == NULL || at >= g_emit_end) {
        return;                     /* the block overflowed and is discarded */
    }
    /* Thumb branch displacements are relative to PC, which reads as the
     * instruction address plus 4. */
    const int32_t off =
        (int32_t)((uint8_t *)g_emit - (uint8_t *)at) - 4;
    if (conditional) {
        *at = (uint16_t)((*at & 0xFF00u) | (((uint32_t)(off >> 1)) & 0xFFu));
    } else {
        *at = (uint16_t)(0xE000u | (((uint32_t)(off >> 1)) & 0x7FFu));
    }
}

static void emit_push(void)
{
#if RV_JIT_LOOP_CHAIN
    emit32(PUSH_HW1, PUSH_HW2);
#else
    emit16(PUSH_HW1);
#endif
}

static void emit_pop(void)
{
#if RV_JIT_LOOP_CHAIN
    emit32(POP_HW1, POP_HW2);
#else
    emit16(POP_HW1);
#endif
}

static void emit_prologue(void)
{
    emit_push();
    emit_mov(R4, R0);     /* hart pointer into a callee-saved register */
#if RV_JIT_LOOP_CHAIN
    emit_mov_imm8(R0, 0u);
    emit_mov(R8, R0);     /* retired-instruction accumulator */
#endif
}

/* Materialise the guest-RAM registers, once per block, on first use. */
static void emit_ram_regs(void)
{
    if (g_ram_live) {
        return;
    }
    emit_imm32(R5, g_ram_base);
    emit_imm32(R6, g_ram_size);
    emit_imm32(R7, g_ram_host);
    g_ram_live = true;
}

/* Add this path's instruction count to the running total and return it. */
static void emit_epilogue(uint32_t insns)
{
#if RV_JIT_LOOP_CHAIN
    /* ADD.W r8, r8, #delta, then return it in r0. */
    emit32(0xF108u, (uint16_t)((R8 << 8) | ((insns - g_acc_base) & 0xFFu)));
    emit_mov(R0, R8);
#else
    emit_mov_imm8(R0, insns);
#endif
    emit_pop();
}

/* Write a constant guest pc into hart->pc using r`scratch`. */
static void emit_set_pc(uint32_t scratch, uint32_t pc)
{
    emit_imm32(scratch, pc);
    emit_str_imm12(scratch, R4, HART_PC_OFF);
}

/* ------------------------------------------------------------------ */
/* Helpers called from translated code                                 */
/* ------------------------------------------------------------------ */

/*
 * Memory access stays in C: it needs the region walk, the permission and
 * width checks and the fault path, none of which is worth open-coding.
 * The helper does the whole instruction including the register write, so
 * the emitted sequence is just argument setup, a call and a fault test.
 *
 * `spec` packs the operand register, the access size and (for loads) the
 * sign-extension flag. Returns 0 to continue, or 1 meaning the helper has
 * already entered a trap and the block must exit immediately.
 */
#define SPEC_REG(s)   ((s) & 0x1Fu)
#define SPEC_SIZE(s)  (((s) >> 8) & 0x7u)
#define SPEC_SIGNED(s) (((s) >> 12) & 1u)

#if RV_JIT_INLINE_PERIPH
/*
 * Note a helper access that landed in the peripheral window, and arm the
 * inlined path once the guest has made enough of them to show it is driving
 * hardware rather than computing.
 *
 * Flushing from inside a helper is safe, and worth stating because it looks
 * like it should not be. rv_jit_flush only resets bookkeeping: the block
 * currently executing stays intact in the code buffer, runs to its end and
 * returns to the dispatcher normally. Only the *next* translation reuses
 * that memory, and translation happens in the dispatch loop, never
 * underneath a running block.
 *
 * The alternative -- a pending-flush flag tested at every block entry --
 * would put the cost on the hot path, where an earlier version of the
 * trigger check cost 16% on CoreMark for exactly that kind of per-dispatch
 * bookkeeping. Here the cost sits in the helper, which is already slow.
 */
static void pt_note(uint32_t addr)
{
    if (g_pt_armed || g_pt_size == 0u || (addr - g_pt_base) >= g_pt_size) {
        return;
    }
    if (++g_pt_hits >= RV_JIT_PT_ARM_AT) {
        g_pt_armed = true;
        rv_jit_flush();
    }
    g_stats.pt_hits = g_pt_hits;
    g_stats.pt_armed = g_pt_armed ? 1u : 0u;
}
#endif

static uint32_t jit_helper_load(rv_hart_t *h, uint32_t addr, uint32_t spec,
                                uint32_t pc)
{
    uint32_t v;
    const rv_exc_t exc = rv_hart_load(h, addr, SPEC_SIZE(spec),
                                      SPEC_SIGNED(spec) != 0u, &v);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
    const uint32_t rd = SPEC_REG(spec);
    if (rd != 0u) {
        h->x[rd] = v;
    }
#if RV_JIT_INLINE_PERIPH
    pt_note(addr);
#endif
    return 0u;
}

static uint32_t jit_helper_store(rv_hart_t *h, uint32_t addr, uint32_t spec,
                                 uint32_t pc)
{
    const rv_exc_t exc = rv_hart_store(h, addr, SPEC_SIZE(spec),
                                       h->x[SPEC_REG(spec)]);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
#if RV_JIT_INLINE_PERIPH
    pt_note(addr);
#endif
    return 0u;
}

#if RV_EXT_A
/*
 * Atomics. The reservation bookkeeping and the read-modify-write live in
 * rv_hart_amo, shared with the interpreter, so the two cannot drift apart.
 * `spec` packs rd, rs2 and funct5; the translator has already checked that
 * funct5 names an implemented operation.
 */
#define AMO_SPEC(rd, rs2, f5)  ((rd) | ((rs2) << 5) | ((f5) << 10))
#define AMO_RD(s)              ((s) & 0x1Fu)
#define AMO_RS2(s)             (((s) >> 5) & 0x1Fu)
#define AMO_F5(s)              (((s) >> 10) & 0x1Fu)

static uint32_t jit_helper_amo(rv_hart_t *h, uint32_t addr, uint32_t spec,
                               uint32_t pc)
{
    const rv_exc_t exc = rv_hart_amo(h, AMO_F5(spec), AMO_RD(spec), addr,
                                     h->x[AMO_RS2(spec)]);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
    h->x[0] = 0u;   /* rv_hart_amo skips rd==0; keep x0 canonical */
    return 0u;
}
#endif

/*
 * Call a helper: r0=hart, r1=addr (already in place), r2=spec, r3=pc.
 * On a non-zero return the block exits with `insns` retired, which
 * includes the faulting instruction so the counters match the
 * interpreter's accounting.
 */
static void emit_helper_call(const void *fn, uint32_t spec, uint32_t pc,
                             uint32_t insns)
{
    emit_mov(R0, R4);
    emit_imm32(R2, spec);
    emit_imm32(R3, pc);
    emit_imm32(R12, (uint32_t)(uintptr_t)fn | 1u);   /* Thumb bit */
    emit16((uint16_t)(0x4780u | (R12 << 3)));        /* BLX r12 */

    /*
     * A faulting helper exits with the count so far. The test is on the
     * helper's return value in r0, which is why the total is only added
     * after the branch that skips this stub.
     */
    /*
     * A faulting helper exits with the count so far.
     *
     * The stub below is ADD.W (4) + MOV (2) + POP.W (4) = 10 bytes, and CBZ
     * is PC-relative with PC reading as the instruction address plus 4. The
     * branch is at addr, the instruction after the stub at addr+2+10, so
     * the displacement is 8 and imm5 is 4. Encoding it for the old 4-byte
     * stub lands in the middle of this one, which is what produced a guest
     * that ran off into nonsense.
     */
#if RV_JIT_LOOP_CHAIN
    emit16((uint16_t)(0xB100u | (4u << 3) | R0));   /* CBZ r0, over the stub */
    emit32(0xF108u, (uint16_t)((R8 << 8) | ((insns - g_acc_base) & 0xFFu)));
    emit_mov(R0, R8);
#else
    emit_cbz_skip4(R0);
    emit_mov_imm8(R0, insns);
#endif
    emit_pop();
}

#if RV_EXT_F
/* LDR.W/STR.W Rt,[r4,#f_off] -- T3, for the f register file. */
static void emit_ld_freg(uint32_t rt, uint32_t freg)
{
    const uint32_t off = HART_F_OFF + freg * 4u;
    emit32((uint16_t)(0xF8D0u | R4), (uint16_t)((rt << 12) | off));
}

static void emit_st_freg(uint32_t freg, uint32_t rt)
{
    const uint32_t off = HART_F_OFF + freg * 4u;
    emit32((uint16_t)(0xF8C0u | R4), (uint16_t)((rt << 12) | off));
}

/* Mark the FP state dirty, matching what rv_hart_fp does in C. */
static void emit_fp_dirty(void)
{
    const uint32_t off = (uint32_t)offsetof(rv_hart_t, mstatus);
    emit32((uint16_t)(0xF8D0u | R4), (uint16_t)((R3 << 12) | off));
    emit_imm32(R2, MSTATUS_FS_MASK);
    emit_dp_reg(DP_ORR, R3, R2);
    emit32((uint16_t)(0xF8C0u | R4), (uint16_t)((R3 << 12) | off));
}
#endif

#if RV_EXT_F
/* ------------------------------------------------------------------ */
/* VFP (ARMv7-M ARM A7.7)                                              */
/* ------------------------------------------------------------------ */
/*
 * Only the even single-precision registers s0, s2 and s4 are used, so the
 * D/N/M bits are always zero and the V field is simply reg/2. That keeps
 * the encodings below readable at the cost of three registers nothing else
 * wants.
 */
#define VS0 0u
#define VS2 1u
#define VS4 2u

/* VADD/VSUB/VMUL/VDIV.F32 Sd,Sn,Sm. */
static void emit_vfp3(uint16_t hw1_base, uint32_t vd, uint32_t vn,
                      uint32_t vm, bool sub)
{
    emit32((uint16_t)(hw1_base | vn),
           (uint16_t)((vd << 12) | 0x0A00u | (sub ? 0x40u : 0u) | vm));
}

#define VFP_ADD 0xEE30u
#define VFP_MUL 0xEE20u
#define VFP_DIV 0xEE80u

/*
 * Fused multiply-add, VFPv4 (present on FPv4-SP, so on the M4F and M7).
 * All four accumulate into Sd, which must therefore be preloaded with the
 * RISC-V rs3 operand.
 *
 *   VFMA   Sd =  Sd + Sn*Sm      VFMS   Sd =  Sd - Sn*Sm
 *   VFNMA  Sd = -Sd + Sn*Sm      VFNMS  Sd = -Sd - Sn*Sm
 *
 * against RISC-V's, which negate the product rather than the accumulator:
 *
 *   FMADD   rs1*rs2 + rs3  -> VFMA     FMSUB   rs1*rs2 - rs3  -> VFNMA
 *   FNMSUB -rs1*rs2 + rs3  -> VFMS     FNMADD -rs1*rs2 - rs3  -> VFNMS
 *
 * The pairing is not the one the names suggest: RISC-V FMSUB subtracts the
 * addend, which ARM expresses by negating the accumulator, so it becomes
 * VFNMA and not VFMS.
 */
#define VFP_FMA  0xEEA0u   /* VFMA / VFMS  */
#define VFP_FNMA 0xEE90u   /* VFNMS / VFNMA */

static void emit_vfma(uint16_t hw1_base, uint32_t vd, uint32_t vn,
                      uint32_t vm, bool op1)
{
    emit32((uint16_t)(hw1_base | vn),
           (uint16_t)((vd << 12) | 0x0A00u | (op1 ? 0x40u : 0u) | vm));
}

/* VSQRT.F32 Sd,Sm. */
static void emit_vsqrt(uint32_t vd, uint32_t vm)
{
    emit32(0xEEB1u, (uint16_t)((vd << 12) | 0x0AC0u | vm));
}

/*
 * VCMP/VCMPE.F32 Sd,Sm, followed by VMRS APSR_nzcv to move the result into
 * the condition flags.
 *
 * The quiet/signalling split maps exactly onto RISC-V's: VCMP raises the
 * invalid flag only for a signalling NaN, which is FEQ's rule, and VCMPE
 * raises it for any NaN, which is FLT's and FLE's.
 *
 * The resulting condition codes also line up, once unordered is accounted
 * for. Unordered leaves N=0, Z=0, C=1, V=1, so EQ is false for it, MI is
 * false because N is clear, and LS is false because C is set and Z clear --
 * which is what RISC-V wants, every comparison against NaN being false.
 */
static void emit_vcmp(uint32_t vd, uint32_t vm, bool signalling)
{
    emit32((uint16_t)(0xEEB4u),
           (uint16_t)((vd << 12) | 0x0A40u | (signalling ? 0x80u : 0u) | vm));
}

/*
 * VCVT.F32.S32 / VCVT.F32.U32 Sd,Sm -- integer to single precision.
 * `is_signed` selects between them.
 */
static void emit_vcvt_f32_from_int(uint32_t vd, uint32_t vm, bool is_signed)
{
    emit32(0xEEB8u,
           (uint16_t)((vd << 12) | 0x0A40u | (is_signed ? 0x80u : 0u) | vm));
}

/*
 * VCVTR.S32.F32 / VCVTR.U32.F32 Sd,Sm -- single precision to integer.
 *
 * The R form, which rounds by FPSCR.RMode. The plain VCVT would force
 * round-toward-zero no matter what `frm` asked for, which is right for
 * exactly one of the five RISC-V rounding modes.
 *
 * opc2 selects the target type: 100 unsigned, 101 signed. That is the
 * opposite sense to the integer-to-float direction above, where the type
 * lives in `op` instead.
 */
static void emit_vcvtr_int_from_f32(uint32_t vd, uint32_t vm, bool is_signed)
{
    emit32((uint16_t)(0xEEB8u | (is_signed ? 5u : 4u)),
           (uint16_t)((vd << 12) | 0x0A40u | vm));
}

static void emit_vmrs_apsr(void)
{
    emit32(0xEEF1u, 0xFA10u);
}

#define C_MI 4u   /* N set: strictly less, and false when unordered */
#define C_VS 6u   /* V set: unordered, which for VCMP means a NaN operand */
#define C_LS 9u   /* C clear or Z set: less or equal, false when unordered */

/* VMOV Sn,Rt  (core register into a VFP register). */
static void emit_vmov_s_r(uint32_t vn, uint32_t rt)
{
    emit32((uint16_t)(0xEE00u | vn), (uint16_t)((rt << 12) | 0x0A10u));
}

/* VMOV Rt,Sn  (VFP register back into a core register). */
static void emit_vmov_r_s(uint32_t rt, uint32_t vn)
{
    emit32((uint16_t)(0xEE10u | vn), (uint16_t)((rt << 12) | 0x0A10u));
}

static void emit_vmrs(uint32_t rt)   /* VMRS Rt,FPSCR */
{
    emit32(0xEEF1u, (uint16_t)((rt << 12) | 0x0A10u));
}

static void emit_vmsr(uint32_t rt)   /* VMSR FPSCR,Rt */
{
    emit32(0xEEE1u, (uint16_t)((rt << 12) | 0x0A10u));
}

/* emit_rbit is defined with the Zbb emitters above; RBIT is what turns
 * ARM's flag order into RISC-V's, so it is used by both. */

/*
 * ARM FPSCR.RMode for a RISC-V rounding mode, in its [23:22] position.
 *
 *   RNE -> RN (00)   RTZ -> RZ (11)   RDN -> RM (10)   RUP -> RP (01)
 *
 * RMM has no ARM equivalent and maps to RN, which differs only on an exact
 * tie; instructions using it are left to the helper rather than translated
 * with the wrong tie behaviour.
 */
static const uint8_t k_rmode[5] = { 0u, 3u, 2u, 1u, 0u };

/*
 * Emit the prologue of a rounding-mode-sensitive FP operation: put the ARM
 * RMode bits for `rm` into r3, then load FPSCR, replace RMode, clear the
 * cumulative exception bits, and set DN so a NaN result comes out as ARM's
 * default NaN -- which is bit-identical to RISC-V's canonical 0x7FC00000.
 * FZ is cleared because RISC-V requires real subnormals, not flush-to-zero.
 *
 * `rm` is FRM_DYN when the instruction defers to fcsr.frm, in which case
 * the mode is looked up at run time from a packed two-bits-per-entry table.
 */
static void emit_fp_setmode(uint32_t rm)
{
    if (rm == FRM_DYN) {
        /* r3 = k_rmode[(fcsr >> 5) & 7], from a packed constant. */
        const uint32_t off = (uint32_t)offsetof(rv_hart_t, fcsr);
        emit32((uint16_t)(0xF8D0u | R4), (uint16_t)((R3 << 12) | off));
        emit_shift_imm(SH_LSR, R3, R3, 5u);
        emit_mov_imm8(R2, 7u);
        emit_dp_reg(DP_AND, R3, R2);
        emit_shift_imm(SH_LSL, R3, R3, 1u);      /* two bits per entry */
        emit_imm32(R2, 0x0000006Cu);             /* 0,3,2,1,0 packed */
        emit_dp_reg(DP_LSR, R2, R3);
        emit_mov_imm8(R3, 3u);
        emit_dp_reg(DP_AND, R2, R3);
        emit_mov(R3, R2);
    } else {
        emit_mov_imm8(R3, k_rmode[rm]);
    }
    emit_shift_imm(SH_LSL, R3, R3, 22u);
    emit_imm32(R2, 1u << 25);                    /* DN: default NaN */
    emit_dp_reg(DP_ORR, R3, R2);

    emit_vmrs(R2);
    emit_imm32(R1, 0x03C0001Fu);   /* RMode | FZ | DN | cumulative flags */
    emit_dp_reg(DP_BIC, R2, R1);
    emit_dp_reg(DP_ORR, R2, R3);
    emit_vmsr(R2);
}

/*
 * Emit the epilogue: read back FPSCR and fold the cumulative exception bits
 * into hart->fcsr.
 *
 * ARM orders them IOC, DZC, OFC, UFC, IXC from bit 0; RISC-V orders them
 * NX, UF, OF, DZ, NV. That is the same five flags in exactly reversed
 * order, so a 32-bit RBIT followed by a shift down by 27 converts one to
 * the other -- no table, no branches.
 */
static void emit_fp_getflags(void)
{
    emit_vmrs(R2);
    emit_mov_imm8(R1, 0x1Fu);
    emit_dp_reg(DP_AND, R2, R1);
    emit_rbit(R2, R2);
    emit_shift_imm(SH_LSR, R2, R2, 27u);

    const uint32_t off = (uint32_t)offsetof(rv_hart_t, fcsr);
    emit32((uint16_t)(0xF8D0u | R4), (uint16_t)((R1 << 12) | off));
    emit_dp_reg(DP_ORR, R1, R2);
    emit32((uint16_t)(0xF8C0u | R4), (uint16_t)((R1 << 12) | off));
}
#endif /* RV_EXT_F */

/* Register-offset load/store, T1: LDR/STR Rt,[Rn,Rm]. */
#define LS_STR   0x5000u
#define LS_STRH  0x5200u
#define LS_STRB  0x5400u
#define LS_LDRSB 0x5600u
#define LS_LDR   0x5800u
#define LS_LDRH  0x5A00u
#define LS_LDRSH 0x5E00u
#define LS_LDRB  0x5C00u

static void emit_ls_reg(uint16_t op, uint32_t rt, uint32_t rn, uint32_t rm)
{
    emit16((uint16_t)(op | (rm << 6) | (rn << 3) | rt));
}

#if RV_JIT_INLINE_PERIPH
/*
 * Qualifies for the window: passthrough, identity-mapped, readable and
 * indifferent to access width.
 *
 * Identity is required rather than merely a uniform offset because it is
 * what makes the emitted access a bare load from the address register. A
 * platform that maps the window somewhere else keeps working; it just
 * keeps the helper call.
 */
static bool pt_region_ok(const rv_region_t *r)
{
    return r->kind == RV_MEM_PASSTHRU && r->size != 0u &&
           r->host_base == (uintptr_t)r->base &&
           (r->perm & RV_PERM_R) != 0u && r->widths == RV_WANY;
}

static void scan_passthru(const rv_bus_t *bus)
{
    g_pt_base = 0u;
    g_pt_size = 0u;
    g_pt_holes = 0u;
    g_pt_store_ok = true;

    /* The run starts at the lowest qualifying region. */
    const rv_region_t *first = NULL;
    for (uint32_t i = 0; i < bus->count; i++) {
        const rv_region_t *r = &bus->regions[i];
        if (pt_region_ok(r) && (first == NULL || r->base < first->base)) {
            first = r;
        }
    }
    if (first == NULL) {
        return;
    }

    /*
     * Extend upwards while some region begins exactly where the run ends.
     * The table is not required to be sorted, hence the rescan; it holds a
     * handful of entries and this runs once per translated block.
     */
    const uint32_t base = first->base;
    uint32_t end = base + first->size;
    for (bool grew = true; grew; ) {
        grew = false;
        for (uint32_t i = 0; i < bus->count; i++) {
            const rv_region_t *r = &bus->regions[i];
            if (pt_region_ok(r) && r->base == end && r->size <= 0xFFFFFFFFu - end) {
                end += r->size;
                grew = true;
                break;
            }
        }
    }

    /* Read-only members of the run become holes that stores must avoid. */
    for (uint32_t i = 0; i < bus->count; i++) {
        const rv_region_t *r = &bus->regions[i];
        uint32_t enc;

        if (!pt_region_ok(r) || r->base < base || r->base >= end ||
            (r->perm & RV_PERM_W) != 0u) {
            continue;
        }
        /*
         * A hole the emitter cannot express in one instruction pair costs
         * more to test than the helper call it avoids, so give up on
         * inlining stores rather than emit the long form. Loads are
         * unaffected: every region in the run is readable by construction.
         */
        if (g_pt_holes == RV_JIT_PT_MAX_HOLES ||
            !thumb_imm12(r->base - base, &enc) || !thumb_imm12(r->size, &enc)) {
            g_pt_store_ok = false;
            break;
        }
        g_pt_hole[g_pt_holes].off = r->base - base;
        g_pt_hole[g_pt_holes].size = r->size;
        g_pt_holes++;
    }

    g_pt_base = base;
    g_pt_size = end - base;
}

/*
 * Set flags so that CC (unsigned lower) means "inside [base, base+size)".
 * Leaves the offset into the range in `scratch`, which the hole tests then
 * reuse. Falls back to materialising the constants when they are not
 * modified immediates; R3 is free at every call site that can need it.
 */
static void emit_range_test(uint32_t scratch, uint32_t rn, uint32_t base,
                            uint32_t size)
{
    uint32_t enc;

    if (thumb_imm12(base, &enc)) {
        emit_sub_imm_w(scratch, rn, enc);
    } else {
        emit_imm32(scratch, base);
        emit_sub_reg(scratch, rn, scratch);
    }
    if (thumb_imm12(size, &enc)) {
        emit_cmp_imm_w(scratch, enc);
    } else {
        emit_imm32(R3, size);
        emit_dp_reg(DP_CMP, scratch, R3);
    }
}
#endif /* RV_JIT_INLINE_PERIPH */

/*
 * Inline a load or store, falling back to the helper for anything the
 * emitted tests cannot settle (virtual devices, ROM, faults).
 *
 * Two windows are inlined. Guest RAM is the one that matters for compute,
 * and the peripheral window is the one that matters for drivers; a guest
 * that talks to real silicon spends its accesses in the second, and before
 * it was inlined each of those cost around 165 host cycles more than a RAM
 * access on the F446.
 *
 * With r1 holding the address:
 *
 *     LSLS r3, r1, #30      alignment: Z set iff the low bits are clear
 *     BNE  slow
 *     SUB  r2, r1, r5       offset into guest RAM
 *     CMP  r2, r6
 *     BHS  periph           unsigned, so below-base wraps and fails too
 *     LDR  r2, [r7, r2]
 *     STR  r2, [r4, rd]
 *     B    done
 *   periph:
 *     SUB  r2, r1, #base    the passthrough window
 *     CMP  r2, #size
 *     BHS  slow
 *     SUB  r3, r2, #hole    stores only: a read-only sub-range
 *     CMP  r3, #hole_size
 *     BLO  slow
 *     MOVS r2, #0
 *     LDR  r3, [r2, r1]     identity map, so the guest address is the host's
 *     B    done
 *   slow:
 *     <helper call>
 *   done:
 *
 * One compare suffices for each upper bound because the alignment check has
 * already run and both windows are a whole number of words: an aligned
 * offset strictly below the size cannot have its last byte past the end.
 */
static void emit_mem_access(bool is_store, uint32_t size, uint32_t sign,
                            uint32_t reg, const void *helper, uint32_t spec,
                            uint32_t pc, uint32_t insns, bool is_fp)
{
    uint16_t *fail_align = NULL;

#if RV_EXT_PMP
    /*
     * With PMP able to deny, every access has to go through the helper,
     * which calls rv_hart_load/store and therefore checks it. The inlined
     * path writes memory directly and would silently ignore a locked entry.
     */
    uint16_t *pmp_slow = NULL;
    if (g_xlate_hart->pmp_active) {
        uint32_t plo, phi;
        if (!rv_pmp_simple(g_xlate_hart, &plo, &phi)) {
            /* More than one entry: the walk cannot be inlined. */
            emit_helper_call(helper, spec, pc, insns);
            return;
        }
        /*
         * One entry, so one range test: addresses inside it go to the
         * helper for the real check, everything else keeps the fast path.
         * That is the common shape -- a guest protecting one buffer -- and
         * it costs a subtract, a compare and a not-taken branch rather than
         * a call.
         */
        emit_imm32(R2, plo);
        emit_sub_reg(R2, R1, R2);
        emit_imm32(R3, phi - plo);
        emit_dp_reg(DP_CMP, R2, R3);
        pmp_slow = emit_bcond_fwd(C_CC);   /* unsigned below: inside */
    }
#endif

    emit_ram_regs();

    /* Alignment. A byte access is always aligned. */
    if (size == 4u) {
        emit_shift_imm(SH_LSL, R3, R1, 30u);
        fail_align = emit_bcond_fwd(C_NE);
    } else if (size == 2u) {
        emit_shift_imm(SH_LSL, R3, R1, 31u);
        fail_align = emit_bcond_fwd(C_NE);
    }

    emit_sub_reg(R2, R1, R5);
    emit_dp_reg(DP_CMP, R2, R6);
    uint16_t *fail_range = emit_bcond_fwd(C_CS);

    if (is_store) {
#if RV_EXT_F
        if (is_fp) { emit_ld_freg(R3, reg); } else
#endif
        emit_ld_greg(R3, reg);
        emit_ls_reg((size == 4u) ? LS_STR : (size == 2u) ? LS_STRH : LS_STRB,
                    R3, R7, R2);
#if RV_EXT_A
        /*
         * rv_hart_store() breaks an outstanding LR/SC reservation when the
         * store touches the reserved word; the inlined path bypasses it, so
         * the reservation has to be dropped here or a later SC would
         * wrongly succeed after an intervening store.
         *
         * Dropping it unconditionally rather than comparing against
         * resv_addr costs one store instead of a compare and a branch, and
         * is architecturally sound: the spec allows SC to fail spuriously,
         * and the constrained LR/SC sequence it must succeed for contains
         * no memory instructions between the pair, so nothing can reach
         * this code inside one.
         */
        emit_mov_imm8(R2, 0u);
        emit32((uint16_t)(0xF880u | R4), (uint16_t)((R2 << 12) | HART_RESV_OFF));
#endif
    } else {
        uint16_t op;
        if (size == 4u) {
            op = LS_LDR;
        } else if (size == 2u) {
            op = sign ? LS_LDRSH : LS_LDRH;
        } else {
            op = sign ? LS_LDRSB : LS_LDRB;
        }
        emit_ls_reg(op, R2, R7, R2);
#if RV_EXT_F
        if (is_fp) {
            /* f0 is a real register, so unlike x0 it is always written. */
            emit_st_freg(reg, R2);
        } else
#endif
        if (reg != 0u) {
            emit_st_greg(reg, R2);
        }
    }
    uint16_t *skip_slow = emit_b_fwd();

#if RV_JIT_INLINE_PERIPH
    uint16_t *skip_pt = NULL;
    uint16_t *pt_miss = NULL;
    uint16_t *pt_hole[RV_JIT_PT_MAX_HOLES] = { NULL };
    uint32_t  pt_holes = 0u;

    /*
     * periph: reached when the address is not in guest RAM. A store also
     * has to clear the read-only holes; a load does not, because every
     * region in the window is readable by construction.
     */
    if (g_pt_armed && g_pt_size != 0u && (!is_store || g_pt_store_ok)) {
        patch_fwd(fail_range, true);
        fail_range = NULL;

        emit_range_test(R2, R1, g_pt_base, g_pt_size);
        pt_miss = emit_bcond_fwd(C_CS);

        if (is_store) {
            for (uint32_t i = 0; i < g_pt_holes; i++) {
                uint32_t enc;
                /* Offsets from the window base, so r2 is what to test. */
                (void)thumb_imm12(g_pt_hole[i].off, &enc);
                emit_sub_imm_w(R3, R2, enc);
                (void)thumb_imm12(g_pt_hole[i].size, &enc);
                emit_cmp_imm_w(R3, enc);
                pt_hole[pt_holes++] = emit_bcond_fwd(C_CC);
            }

#if RV_EXT_F
            if (is_fp) { emit_ld_freg(R3, reg); } else
#endif
            emit_ld_greg(R3, reg);
            emit_mov_imm8(R2, 0u);
            emit_ls_reg((size == 4u) ? LS_STR : (size == 2u) ? LS_STRH : LS_STRB,
                        R3, R2, R1);
#if RV_EXT_A
            /*
             * Drop any LR/SC reservation, as the RAM path does. A guest
             * that reserved a peripheral address is a strange guest, but
             * the reservation address is not checked here and a store that
             * silently left it standing would let a later SC succeed across
             * an intervening write. r2 already holds zero.
             */
            emit32((uint16_t)(0xF880u | R4),
                   (uint16_t)((R2 << 12) | HART_RESV_OFF));
#endif
        } else {
            uint16_t op;
            if (size == 4u) {
                op = LS_LDR;
            } else if (size == 2u) {
                op = sign ? LS_LDRSH : LS_LDRH;
            } else {
                op = sign ? LS_LDRSB : LS_LDRB;
            }
            emit_mov_imm8(R2, 0u);
            emit_ls_reg(op, R3, R2, R1);
#if RV_EXT_F
            if (is_fp) {
                emit_st_freg(reg, R3);
            } else
#endif
            if (reg != 0u) {
                emit_st_greg(reg, R3);
            }
        }
        skip_pt = emit_b_fwd();
    }
#endif /* RV_JIT_INLINE_PERIPH */

    /* slow: */
    patch_fwd(fail_align, true);
    patch_fwd(fail_range, true);
#if RV_EXT_PMP
    patch_fwd(pmp_slow, true);
#endif
#if RV_JIT_INLINE_PERIPH
    patch_fwd(pt_miss, true);
    for (uint32_t i = 0; i < pt_holes; i++) {
        patch_fwd(pt_hole[i], true);
    }
#endif
    emit_helper_call(helper, spec, pc, insns);

    /* done: */
    patch_fwd(skip_slow, false);
#if RV_JIT_INLINE_PERIPH
    patch_fwd(skip_pt, false);
#endif
}

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ
/*
 * Cache-block operations. `spec` is the raw 12-bit CBO immediate, already
 * validated by the translator. The block base is derived inside
 * rv_hart_cbo, which is shared with the interpreter.
 */
static uint32_t jit_helper_cbo(rv_hart_t *h, uint32_t addr, uint32_t spec,
                               uint32_t pc)
{
    uint32_t fault_addr;
    const rv_exc_t exc = rv_hart_cbo(h, spec, addr, &fault_addr);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, fault_addr);
        return 1u;
    }
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* ALU helper                                                          */
/* ------------------------------------------------------------------ */

/*
 * Operations with no short Thumb-2 equivalent -- the M extension's high
 * multiplies and divides, and Zbc/Zbb's clmul, cpop and orc.b.
 *
 * These used to end the block and fall back to the interpreter, which cost
 * far more than the instruction: CoreMark alone took 175,305 fallbacks, and
 * every one of them fragmented otherwise-hot code. A plain call keeps the
 * block intact. None of them can fault, so there is no result to check.
 *
 * `spec` packs rd, rs1, rs2 and the operation into one 32-bit immediate.
 */
enum {
    ALU_MULH = 0, ALU_MULHSU, ALU_MULHU, ALU_DIV, ALU_DIVU, ALU_REM, ALU_REMU,
    ALU_CLMUL, ALU_CLMULR, ALU_CLMULH, ALU_CPOP, ALU_ORCB,
};

#define ALU_SPEC(rd, rs1, rs2, op) \
    ((rd) | ((rs1) << 5) | ((rs2) << 10) | ((uint32_t)(op) << 15))

static void jit_helper_alu(rv_hart_t *h, uint32_t spec)
{
    const uint32_t op = spec >> 15;
    if (op <= ALU_REMU)        { g_stats.alu_calls_muldiv++; }
    else if (op >= ALU_CLMUL && op <= ALU_CLMULH) { g_stats.alu_calls_clmul++; }
    else                       { g_stats.alu_calls_bit++; }

    const uint32_t rd = spec & 0x1Fu;
    const uint32_t a = h->x[(spec >> 5) & 0x1Fu];
    const uint32_t b = h->x[(spec >> 10) & 0x1Fu];
    uint32_t r;

    switch (spec >> 15) {
    case ALU_MULH:   r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
    case ALU_MULHSU: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
    case ALU_MULHU:  r = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
    /* RISC-V defines these rather than leaving them undefined as C does. */
    case ALU_DIV:
        r = (b == 0u) ? 0xFFFFFFFFu
          : ((a == 0x80000000u && b == 0xFFFFFFFFu) ? 0x80000000u
          : (uint32_t)((int32_t)a / (int32_t)b));
        break;
    case ALU_DIVU:   r = (b == 0u) ? 0xFFFFFFFFu : (a / b); break;
    case ALU_REM:
        r = (b == 0u) ? a
          : ((a == 0x80000000u && b == 0xFFFFFFFFu) ? 0u
          : (uint32_t)((int32_t)a % (int32_t)b));
        break;
    case ALU_REMU:   r = (b == 0u) ? a : (a % b); break;
    case ALU_CPOP:   r = (uint32_t)__builtin_popcount(a); break;
    case ALU_ORCB: {
        r = 0u;
        for (unsigned i = 0; i < 4u; i++) {
            if ((a & (0xFFu << (i * 8u))) != 0u) {
                r |= 0xFFu << (i * 8u);
            }
        }
        break;
    }
    case ALU_CLMUL: {
        r = 0u;
        for (unsigned i = 0; i < 32u; i++) { if ((b >> i) & 1u) { r ^= a << i; } }
        break;
    }
    case ALU_CLMULR: {
        r = 0u;
        for (unsigned i = 0; i < 32u; i++) { if ((b >> i) & 1u) { r ^= a >> (31u - i); } }
        break;
    }
    default: {   /* ALU_CLMULH */
        r = 0u;
        for (unsigned i = 1; i < 32u; i++) { if ((b >> i) & 1u) { r ^= a >> (32u - i); } }
        break;
    }
    }

    if (rd != 0u) {
        h->x[rd] = r;
    }
}

#if RV_EXT_ZBC
/*
 * clmul inline. ARMv7-M has no carry-less multiply, but the operation is
 * shift-and-XOR, which is six instructions in a loop:
 *
 *       MOVS r3, #0
 *   1:  CBZ  r2, 2f          ; stop as soon as no source bits remain
 *       LSRS r2, r2, #1      ; C = the bit just shifted out
 *       IT   CS
 *       EOR  r3, r1          ; accumulate when that bit was set
 *       LSLS r1, r1, #1
 *       B    1b
 *   2:
 *
 * The early exit is why this is worth doing rather than calling the helper:
 * the helper always runs 32 iterations, while this stops at the highest set
 * bit of the multiplier, which for CRC operands is usually well short of 32.
 *
 * Offsets are hard-coded because the sequence is fixed-size; they are
 * derived below from the byte layout, where PC reads as the instruction
 * address plus 4.
 */
static void emit_clmul(uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    emit_ld_greg(R1, rs1);              /* a */
    emit_ld_greg(R2, rs2);              /* b */
    emit_mov_imm8(R3, 0u);              /* @0  result = 0            */
    /* @2 CBZ r2 -> done(@14): PC=@6, offset 8, so (i:imm5) = 4. */
    emit16((uint16_t)(0xB100u | (4u << 3) | R2));
    emit_shift_imm(SH_LSR, R2, R2, 1u); /* @4  b >>= 1, C = old bit0 */
    emit_it(C_CS);                      /* @6                        */
    emit_dp_reg(DP_EOR, R3, R1);        /* @8  conditional in the IT  */
    emit_shift_imm(SH_LSL, R1, R1, 1u); /* @10 a <<= 1                */
    /* @12 B -> loop(@2): PC=@16, offset -14, imm11 = -7. */
    emit16((uint16_t)(0xE000u | (((uint32_t)-7) & 0x7FFu)));
    /* @14 done */
    if (rd != 0u) {
        emit_st_greg(rd, R3);
    }
}
#endif

/* Call jit_helper_alu; no fault is possible so nothing is checked. */
static void emit_alu_helper(uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t op)
{
    emit_mov(R0, R4);
    emit_imm32(R1, ALU_SPEC(rd, rs1, rs2, op));
    emit_imm32(R12, (uint32_t)(uintptr_t)jit_helper_alu | 1u);
    emit16((uint16_t)(0x4780u | (R12 << 3)));      /* BLX r12 */
}

#if RV_EXT_F
/*
 * Slow paths for FLW and FSW. The inlined guest-RAM path handles the common
 * case; anything else -- MMIO, the passthrough window, a fault, or an FPU
 * that is Off -- comes here, where rv_hart_fp re-executes the whole
 * instruction and reports exactly what the interpreter would.
 *
 * `spec` is the raw instruction rather than a packed descriptor, which is
 * why these take it in place of the integer helpers' spec word.
 */
static uint32_t jit_helper_fp_load(rv_hart_t *h, uint32_t addr, uint32_t insn,
                                   uint32_t pc)
{
    (void)addr;
    uint32_t tval = insn;
    const rv_exc_t exc = rv_hart_fp(h, insn, &tval);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, tval);
        return 1u;
    }
    return 0u;
}

static uint32_t jit_helper_fp_store(rv_hart_t *h, uint32_t addr, uint32_t insn,
                                    uint32_t pc)
{
    return jit_helper_fp_load(h, addr, insn, pc);
}

#endif

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

/*
 * Translate one instruction. Returns:
 *   0  translated, block continues
 *   1  translated, block ends here (control transfer)
 *  -1  not translated; the block must end *before* this instruction
 */
static int translate_one(uint32_t insn, uint32_t pc, unsigned len,
                         uint32_t insns_after, uint32_t *redirect)
{
#if RV_EXT_F
    /* FP state as it stands at translation time; see the FLW case below. */
    const bool h_fs_off = (g_xlate_hart->mstatus & MSTATUS_FS_MASK) == 0u;
#endif
    const uint32_t rd  = rv_rd(insn);
    const uint32_t rs1 = rv_rs1(insn);
    const uint32_t rs2 = rv_rs2(insn);
    const uint32_t f3  = rv_funct3(insn);
    const uint32_t f7  = rv_funct7(insn);
    const uint32_t next_pc = pc + len;

    switch (rv_opcode(insn)) {

    case OP_LUI:
        if (rd != 0u) {
            emit_imm32(R1, rv_imm_u(insn));
            emit_st_greg(rd, R1);
        }
        return 0;

    case OP_AUIPC:
        if (rd != 0u) {
            emit_imm32(R1, pc + rv_imm_u(insn));
            emit_st_greg(rd, R1);
        }
        return 0;

    case OP_IMM: {
        const int32_t imm = rv_imm_i(insn);

        if (rd == 0u) {
            return 0;   /* result discarded; nothing to emit */
        }
        emit_ld_greg(R1, rs1);

        switch (f3) {
        case 0:  /* ADDI */
            emit_add_simm12(R1, R1, imm);
            break;
        case 1:  /* SLLI, and the Zbb unary ops sharing its slot */
            if (f7 == 0u) {
                emit_shift_imm(SH_LSL, R1, R1, rs2);
                break;
            }
#if RV_EXT_ZBB
            if (f7 == 0x30u) {
                switch (rs2) {
                case 0: emit_clz(R1, R1); break;                    /* clz    */
                /* Reversing the bits turns a trailing-zero count into a
                 * leading-zero count, which ARM does have. */
                case 1: emit_rbit(R1, R1); emit_clz(R1, R1); break;  /* ctz    */
                case 4: emit_xt(XT_SXTB, R1, R1); break;            /* sext.b */
                case 5: emit_xt(XT_SXTH, R1, R1); break;            /* sext.h */
                /* cpop has no ARMv7-M equivalent, so it goes via a call
                 * rather than ending the block. */
                case 2: emit_alu_helper(rd, rs1, 0u, ALU_CPOP); return 0;
                default: return -1;
                }
                break;
            }
#endif
#if RV_EXT_ZBS
            /* bseti/bclri/binvi: the shift amount is known, so the mask is
             * a compile-time constant and needs no runtime shift. */
            if (f7 == 0x14u || f7 == 0x24u || f7 == 0x34u) {
                emit_imm32(R2, 1u << rs2);
                if (f7 == 0x14u)      { emit_dp_reg(DP_ORR, R1, R2); }
                else if (f7 == 0x24u) { emit_dp_reg(DP_BIC, R1, R2); }
                else                  { emit_dp_reg(DP_EOR, R1, R2); }
                break;
            }
#endif
            return -1;
        case 5:  /* SRLI / SRAI, plus Zbb rori / rev8 */
            if (f7 == 0u) {
                emit_shift_imm(SH_LSR, R1, R1, rs2);
            } else if (f7 == 0x20u) {
                emit_shift_imm(SH_ASR, R1, R1, rs2);
            }
#if RV_EXT_ZBB
            else if (f7 == 0x30u) {                       /* rori */
                emit_mov_imm8(R2, rs2);
                emit_dp_reg(DP_ROR, R1, R2);
            } else if (f7 == 0x34u && rs2 == 24u) {       /* rev8 */
                emit_xt(XT_REV, R1, R1);
            }
            else if (f7 == 0x14u && rs2 == 7u) {          /* orc.b */
                emit_alu_helper(rd, rs1, 0u, ALU_ORCB);
                return 0;
            }
#endif
#if RV_EXT_ZBS
            else if (f7 == 0x24u) {                       /* bexti */
                emit_shift_imm(SH_LSR, R1, R1, rs2);
                emit_mov_imm8(R2, 1u);
                emit_dp_reg(DP_AND, R1, R2);
            }
#endif
            else {
                return -1;
            }
            break;
        case 2:  /* SLTI */
        case 3:  /* SLTIU */
            emit_imm32(R2, (uint32_t)imm);
            emit_mov_imm8(R3, 0u);
            emit_dp_reg(DP_CMP, R1, R2);
            emit_it(f3 == 2u ? C_LT : C_CC);
            emit_mov_imm8(R3, 1u);
            emit_mov(R1, R3);
            break;
        case 4:  /* XORI */
            emit_imm32(R2, (uint32_t)imm);
            emit_dp_reg(DP_EOR, R1, R2);
            break;
        case 6:  /* ORI */
            emit_imm32(R2, (uint32_t)imm);
            emit_dp_reg(DP_ORR, R1, R2);
            break;
        default: /* ANDI */
            emit_imm32(R2, (uint32_t)imm);
            emit_dp_reg(DP_AND, R1, R2);
            break;
        }
        emit_st_greg(rd, R1);
        return 0;
    }

    case OP_OP: {
        if (rd == 0u) {
            return 0;
        }

        if (f7 == 0u || f7 == 0x20u) {
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);

            if (f7 == 0x20u) {
                if (f3 == 0u) {
                    emit_sub_reg(R1, R1, R2);          /* SUB */
                } else if (f3 == 5u) {
                    emit_ubfx5(R2, R2);
                    emit_dp_reg(DP_ASR, R1, R2);       /* SRA */
                } else {
                    return -1;
                }
            } else {
                switch (f3) {
                case 0: emit_add_reg(R1, R1, R2); break;          /* ADD  */
                case 1: emit_ubfx5(R2, R2);
                        emit_dp_reg(DP_LSL, R1, R2); break;       /* SLL  */
                case 4: emit_dp_reg(DP_EOR, R1, R2); break;       /* XOR  */
                case 5: emit_ubfx5(R2, R2);
                        emit_dp_reg(DP_LSR, R1, R2); break;       /* SRL  */
                case 6: emit_dp_reg(DP_ORR, R1, R2); break;       /* OR   */
                case 7: emit_dp_reg(DP_AND, R1, R2); break;       /* AND  */
                case 2:                                            /* SLT  */
                case 3:                                            /* SLTU */
                    emit_mov_imm8(R3, 0u);
                    emit_dp_reg(DP_CMP, R1, R2);
                    emit_it(f3 == 2u ? C_LT : C_CC);
                    emit_mov_imm8(R3, 1u);
                    emit_mov(R1, R3);
                    break;
                default:
                    return -1;
                }
            }
            emit_st_greg(rd, R1);
            return 0;
        }

#if RV_EXT_M
        if (f7 == 1u && f3 == 0u) {      /* MUL: the only M op worth inlining */
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            emit_dp_reg(DP_MUL, R1, R2); /* MULS r1, r2, r1 */
            emit_st_greg(rd, R1);
            return 0;
        }
#endif
#if RV_EXT_ZBB
        if (f7 == 0x20u && (f3 == 4u || f3 == 6u || f3 == 7u)) {
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            if (f3 == 7u) {                       /* andn */
                emit_dp_reg(DP_BIC, R1, R2);
            } else if (f3 == 6u) {                /* orn  */
                emit_dp_reg(DP_MVN, R2, R2);
                emit_dp_reg(DP_ORR, R1, R2);
            } else {                              /* xnor */
                emit_dp_reg(DP_EOR, R1, R2);
                emit_dp_reg(DP_MVN, R1, R1);
            }
            emit_st_greg(rd, R1);
            return 0;
        }

        if (f7 == 0x05u && f3 >= 4u) {            /* min / minu / max / maxu */
            static const uint32_t cond[4] = { C_LT, C_CC, C_GE, C_CS };
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            emit_dp_reg(DP_CMP, R1, R2);
            emit_it(cond[f3 - 4u]);
            emit_mov(R2, R1);                     /* keep rs1 when it wins */
            emit_st_greg(rd, R2);
            return 0;
        }

        if (f7 == 0x30u && (f3 == 1u || f3 == 5u)) {   /* rol / ror */
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            emit_ubfx5(R2, R2);                   /* RISC-V uses rs2[4:0] */
            if (f3 == 1u) {
                /*
                 * ARM has no rotate-left. Negating the amount works because
                 * ROR uses Rm[7:0] modulo 32, and (-n) mod 32 == 32-n for
                 * n in 1..31, while n==0 negates to 0 and rotates by none.
                 */
                emit_dp_reg(DP_RSB, R2, R2);
            }
            emit_dp_reg(DP_ROR, R1, R2);
            emit_st_greg(rd, R1);
            return 0;
        }

        if (f7 == 0x04u && f3 == 4u && rs2 == 0u) {    /* zext.h */
            emit_ld_greg(R1, rs1);
            emit_xt(XT_UXTH, R1, R1);
            emit_st_greg(rd, R1);
            return 0;
        }
#endif
#if RV_EXT_ZBA
        if (f7 == 0x10u && (f3 == 2u || f3 == 4u || f3 == 6u)) {
            /* sh1add/sh2add/sh3add: rd = (rs1 << n) + rs2, one instruction. */
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            emit_add_lsl(R1, R2, R1, f3 >> 1);
            emit_st_greg(rd, R1);
            return 0;
        }
#endif
#if RV_EXT_ZBS
        if ((f7 == 0x14u || f7 == 0x24u || f7 == 0x34u) &&
            (f3 == 1u || (f7 == 0x24u && f3 == 5u))) {
            emit_ld_greg(R1, rs1);
            emit_ld_greg(R2, rs2);
            emit_ubfx5(R2, R2);                   /* RISC-V uses rs2[4:0] */
            if (f7 == 0x24u && f3 == 5u) {        /* bext */
                emit_dp_reg(DP_LSR, R1, R2);
                emit_mov_imm8(R3, 1u);
                emit_dp_reg(DP_AND, R1, R3);
            } else {
                emit_mov_imm8(R3, 1u);
                emit_dp_reg(DP_LSL, R3, R2);      /* mask = 1 << n */
                if (f7 == 0x14u)      { emit_dp_reg(DP_ORR, R1, R3); } /* bset */
                else if (f7 == 0x24u) { emit_dp_reg(DP_BIC, R1, R3); } /* bclr */
                else                  { emit_dp_reg(DP_EOR, R1, R3); } /* binv */
            }
            emit_st_greg(rd, R1);
            return 0;
        }
#endif
#if RV_EXT_M
        if (f7 == 1u && f3 >= 1u) {
            /* MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU -> helper. */
            emit_alu_helper(rd, rs1, rs2, ALU_MULH + (f3 - 1u));
            return 0;
        }
#endif
#if RV_EXT_ZBC
        if (f7 == 0x05u && f3 >= 1u && f3 <= 3u) {
            if (f3 == 1u) {
                emit_clmul(rd, rs1, rs2);       /* the hot one; inlined */
                return 0;
            }
            /* clmulh/clmulr take the high and middle words of the product,
             * whose shifts do not fold into the same loop; they are rare
             * and stay on the helper. */
            emit_alu_helper(rd, rs1, rs2,
                            (f3 == 2u) ? ALU_CLMULR : ALU_CLMULH);
            return 0;
        }
#endif
        return -1;
    }

    case OP_LOAD: {
        uint32_t size, sign;
        switch (f3) {
        case 0: size = 1u; sign = 1u; break;   /* LB  */
        case 1: size = 2u; sign = 1u; break;   /* LH  */
        case 2: size = 4u; sign = 0u; break;   /* LW  */
        case 4: size = 1u; sign = 0u; break;   /* LBU */
        case 5: size = 2u; sign = 0u; break;   /* LHU */
        default: return -1;
        }
        emit_ld_greg(R1, rs1);
        emit_add_simm12(R1, R1, rv_imm_i(insn));
        emit_mem_access(false, size, sign, rd, (const void *)jit_helper_load,
                        rd | (size << 8) | (sign << 12), pc, insns_after, false);
        return 0;
    }

    case OP_STORE: {
        uint32_t size;
        switch (f3) {
        case 0: size = 1u; break;
        case 1: size = 2u; break;
        case 2: size = 4u; break;
        default: return -1;
        }
        emit_ld_greg(R1, rs1);
        emit_add_simm12(R1, R1, rv_imm_s(insn));
        emit_mem_access(true, size, 0u, rs2, (const void *)jit_helper_store,
                        rs2 | (size << 8), pc, insns_after, false);
        return 0;
    }

    case OP_JAL: {
        const uint32_t target = pc + (uint32_t)rv_imm_j(insn);
        if (rd != 0u) {
            emit_imm32(R1, next_pc);
            emit_st_greg(rd, R1);
        }
        /*
         * A forward jump can simply be followed: nothing needs to be
         * emitted for it at all, and the block keeps growing. Backward
         * jumps are left to end the block -- they are loop back edges, and
         * returning to the dispatcher there is what bounds interrupt
         * latency and stops translation looping over the same body.
         */
        if (target > pc) {
            *redirect = target;
            return 2;
        }
        /*
         * Backward to the block's own start: a loop. Branch back rather
         * than exiting, after adding this pass to the accumulator and
         * checking it against a cap.
         *
         * The cap is what keeps interrupt latency bounded. Delivery happens
         * between blocks, so looping without one would defer an interrupt
         * for the whole loop; 64 guest instructions is a few iterations of
         * a typical body, which amortises the dispatch while keeping the
         * worst case in the same order as before.
         */
#if RV_JIT_LOOP_CHAIN
        uint16_t *const back =
            (target == g_block_pc) ? g_loop_start : NULL;
        if (back != NULL) {
            emit32(0xF108u,
                   (uint16_t)((R8 << 8) | ((insns_after - g_acc_base) & 0xFFu)));
            emit_imm32(R1, RV_JIT_LOOP_CAP);
            emit_cmp_hi(R8, R1);

            /* Branch back if still under the cap. */
            emit_bcond_back(C_CC, back);

            /* Cap reached: fall out to the dispatcher at the loop target.
             * The accumulator already holds everything retired. */
            emit_set_pc(R1, target);
            emit_mov(R0, R8);
            emit_pop();
            return 1;
        }
#endif
        emit_set_pc(R1, target);
        emit_epilogue(insns_after);
        return 1;
    }

    case OP_JALR: {
        if (f3 != 0u) {
            return -1;
        }
        /* rs1 is read before rd is written: they may be the same register. */
        emit_ld_greg(R1, rs1);
        emit_add_simm12(R1, R1, rv_imm_i(insn));
        emit_bic1(R1, R1);
        if (rd != 0u) {
            emit_imm32(R2, next_pc);
            emit_st_greg(rd, R2);
        }
        emit_str_imm12(R1, R4, HART_PC_OFF);
        emit_epilogue(insns_after);
        return 1;
    }

    case OP_BRANCH: {
        if (f3 == 2u || f3 == 3u) {
            return -1;              /* no such branch condition */
        }
        /* Condition to skip the taken-path exit, i.e. the inverse. */
        static const uint32_t inv[8] = {
            C_NE, C_EQ, 0u, 0u, C_GE, C_LT, C_CS, C_CC
        };
        const uint32_t target = pc + (uint32_t)rv_imm_b(insn);

        /*
         * Only the taken path leaves the block; the fall-through carries on
         * being translated. This is what lengthens blocks: CoreMark is full
         * of forward if/else branches that would otherwise end one every
         * few instructions.
         *
         * The skip branch jumps over the exit stub, which is at most eight
         * halfwords, so the 16-bit conditional form always reaches.
         */
        emit_ld_greg(R1, rs1);
        emit_ld_greg(R2, rs2);

#if RV_JIT_LOOP_CHAIN
        /*
         * Only a branch back to the block's own start is chained: the edge
         * emits one constant, executed on the first pass and every
         * iteration alike, so the loop body must be the whole path.
         *
         * The accumulation goes *before* the conditional split, so both
         * paths account for the same instructions. Putting it on the taken
         * path while advancing the translate-time base for both made the
         * fall-through under-count by exactly the loop body. ADD.W does not
         * set flags, so it is safe ahead of the compare.
         */
        const bool chain = (target == g_block_pc) && (g_loop_start != NULL);
        if (chain) {
            emit32(0xF108u,
                   (uint16_t)((R8 << 8) | ((insns_after - g_acc_base) & 0xFFu)));
            g_acc_base = insns_after;
        }
#endif

        emit_dp_reg(DP_CMP, R1, R2);
        uint16_t *skip = emit_bcond_fwd(inv[f3]);

#if RV_JIT_LOOP_CHAIN
        if (chain) {
            emit_imm32(R1, RV_JIT_LOOP_CAP);
            emit_cmp_hi(R8, R1);
            emit_bcond_back(C_CC, g_loop_start);   /* BLO, still under cap */

            /* Cap reached: leave with the total. */
            emit_set_pc(R1, target);
            emit_mov(R0, R8);
            emit_pop();
            patch_fwd(skip, true);
            return 0;
        }
#endif

        emit_set_pc(R1, target);
        emit_epilogue(insns_after);

        patch_fwd(skip, true);
        return 0;
    }

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ
    case OP_MISC_MEM: {
        /*
         * FENCE (f3==0) is a no-op on this single-hart, cacheless-to-device
         * design, so it translates to nothing at all. FENCE.I (f3==1) must
         * discard translations, which cannot be done from inside a block --
         * it would be invalidating the very code that is executing -- so it
         * ends the block and the interpreter handles it.
         */
        if (f3 == 0u) {
            return 0;
        }
        if (f3 != 2u) {
            return -1;
        }
        const uint32_t op = insn >> 20;
        if (rd != 0u || !rv_cbo_valid(op)) {
            return -1;          /* let the interpreter raise illegal */
        }
        emit_ld_greg(R1, rs1);
        emit_helper_call((const void *)jit_helper_cbo, op, pc, insns_after);
        return 0;
    }
#endif

#if RV_EXT_A
    case OP_AMO: {
        if (f3 != 2u) {
            return -1;              /* only 32-bit AMOs exist on RV32 */
        }
        const uint32_t funct5 = f7 >> 2;
        if (!rv_amo_valid(funct5)) {
            return -1;              /* let the interpreter raise illegal */
        }
        if (funct5 == RV_AMO_LR && rs2 != 0u) {
            return -1;              /* rs2 must be zero for LR */
        }
        /* The address is rs1 with no offset. */
        emit_ld_greg(R1, rs1);
        emit_helper_call((const void *)jit_helper_amo,
                         AMO_SPEC(rd, rs2, funct5), pc, insns_after);
        return 0;
    }
#endif

#if RV_EXT_F
    /*
     * Only the operations that are independent of both the rounding mode
     * and the exception flags are emitted inline. Those are exactly the
     * ones whose semantics are bit manipulation rather than arithmetic:
     * the loads and stores, the register moves, and sign injection. They
     * cannot raise a flag, so no FPSCR handling is needed and the result
     * is provably identical to the interpreter's.
     *
     * Everything arithmetic -- add, multiply, divide, sqrt, the fused
     * multiply-adds, the conversions, and the comparisons, which do raise
     * NV on NaN -- goes to the shared helper. Emitting those inline means
     * driving FPSCR.RMode from frm and harvesting the exception bits back
     * into fflags on every operation, and getting that subtly wrong would
     * lose the flag semantics the interpreter currently gets right. It is
     * the obvious next step, not a shortcut taken here.
     */
    case OP_LOAD_FP: {
        if (f3 != 2u) {
            return -1;
        }
        /*
         * mstatus.FS gates the whole extension, so an FPU that is Off must
         * make even FLW and FSW trap. The inlined path cannot see FS, and a
         * block is translated once and then reused across changes to it --
         * so translation is refused unless FS is on at translate time, and
         * the interpreter, which does check, handles the rest.
         */
        if ((h_fs_off)) {
            return -1;
        }
        emit_ld_greg(R1, rs1);
        emit_add_simm12(R1, R1, rv_imm_i(insn));
        emit_mem_access(false, 4u, 0u, rd, (const void *)jit_helper_fp_load,
                        insn, pc, insns_after, true);
        emit_fp_dirty();
        return 0;
    }

    case OP_STORE_FP: {
        if (f3 != 2u || (h_fs_off)) {
            return -1;
        }
        emit_ld_greg(R1, rs1);
        emit_add_simm12(R1, R1, rv_imm_s(insn));
        emit_mem_access(true, 4u, 0u, rs2, (const void *)jit_helper_fp_store,
                        insn, pc, insns_after, true);
        return 0;
    }

    case OP_MADD:
    case OP_MSUB:
    case OP_NMSUB:
    case OP_NMADD: {
        if ((f7 & 3u) != 0u) {
            return -1;              /* fmt must be S */
        }
        const uint32_t rmf = f3;
        if (rmf == FRM_RMM || (rmf > FRM_RMM && rmf != FRM_DYN)) {
            return -1;              /* RMM has no ARM rounding mode */
        }
        const uint32_t rs3 = (insn >> 27) & 0x1Fu;

        emit_fp_setmode(rmf);

        /* s4 accumulates, so it takes rs3; s0 and s2 take the product. */
        emit_ld_freg(R1, rs3);
        emit_vmov_s_r(VS4, R1);
        emit_ld_freg(R1, rs1);
        emit_vmov_s_r(VS0, R1);
        emit_ld_freg(R1, rs2);
        emit_vmov_s_r(VS2, R1);

        switch (rv_opcode(insn)) {
        case OP_MADD:  emit_vfma(VFP_FMA,  VS4, VS0, VS2, false); break;
        case OP_MSUB:  emit_vfma(VFP_FNMA, VS4, VS0, VS2, true);  break;
        case OP_NMSUB: emit_vfma(VFP_FMA,  VS4, VS0, VS2, true);  break;
        default:       emit_vfma(VFP_FNMA, VS4, VS0, VS2, false); break;
        }

        emit_vmov_r_s(R1, VS4);
        emit_st_freg(rd, R1);
        emit_fp_getflags();
        emit_fp_dirty();
        return 0;
    }

    case OP_FP: {
        if ((f7 & 3u) != 0u) {
            return -1;              /* fmt must be S */
        }
        switch (f7 >> 2) {
        case 0x04u:                 /* FSGNJ.S / FSGNJN.S / FSGNJX.S */
            if (f3 > 2u) {
                return -1;
            }
            emit_ld_freg(R1, rs1);
            emit_ld_freg(R2, rs2);
            emit_imm32(R3, 0x80000000u);
            if (f3 == 2u) {         /* fsgnjx: xor in the sign of rs2 */
                emit_dp_reg(DP_AND, R2, R3);
                emit_dp_reg(DP_EOR, R1, R2);
            } else {
                if (f3 == 1u) {     /* fsgnjn: use the inverted sign */
                    emit_dp_reg(DP_MVN, R2, R2);
                }
                emit_dp_reg(DP_AND, R2, R3);
                emit_dp_reg(DP_BIC, R1, R3);
                emit_dp_reg(DP_ORR, R1, R2);
            }
            emit_st_freg(rd, R1);
            emit_fp_dirty();
            return 0;

        case 0x14u: {               /* FEQ.S / FLT.S / FLE.S */
            if (f3 > 2u) {
                return -1;
            }
            /* Rounding is irrelevant to a comparison, but the invalid flag
             * is not, so the FPSCR exception bits still have to be cleared
             * and harvested. RNE is passed simply because f_begin needs a
             * mode. */
            emit_fp_setmode(FRM_RNE);

            emit_ld_freg(R1, rs1);
            emit_vmov_s_r(VS0, R1);
            emit_ld_freg(R1, rs2);
            emit_vmov_s_r(VS2, R1);

            /*
             * The false result is set up *before* the comparison. MOVS on a
             * low register writes N and Z, so zeroing the destination after
             * VMRS would overwrite the very flags being tested -- leaving
             * Z set and N clear, which makes EQ always true, MI always
             * false and LS always true. The conditional MOV below is inside
             * an IT block and so does not set flags.
             */
            emit_mov_imm8(R1, 0u);

            /* FEQ is the quiet comparison; FLT and FLE are signalling. */
            emit_vcmp(VS0, VS2, f3 != 2u);
            emit_vmrs_apsr();

            emit_it((f3 == 2u) ? C_EQ : ((f3 == 1u) ? C_MI : C_LS));
            emit_mov_imm8(R1, 1u);

            if (rd != 0u) {
                emit_st_greg(rd, R1);
            }
            emit_fp_getflags();
            return 0;
        }

        case 0x18u: {               /* FCVT.W.S / FCVT.WU.S */
            /*
             * Float to integer. ARM and RISC-V agree on far more of this
             * than they disagree on: both saturate an out-of-range value to
             * the limit of the target type, both raise invalid when they do,
             * and neither raises inexact on top of it. The whole divergence
             * is one input -- ARM converts a NaN to zero, RISC-V to the
             * *maximum* value of the target type -- so a compare against
             * self, which is unordered exactly for a NaN, settles it.
             */
            if (rs2 > 1u) {
                return -1;
            }
            const uint32_t rmf18 = f3;
            if (rmf18 == FRM_RMM || (rmf18 > FRM_RMM && rmf18 != FRM_DYN)) {
                return -1;
            }
            const bool cvt_signed = (rs2 == 0u);

            emit_fp_setmode(rmf18);
            emit_ld_freg(R1, rs1);
            emit_vmov_s_r(VS0, R1);
            emit_vcvtr_int_from_f32(VS4, VS0, cvt_signed);
            emit_vmov_r_s(R1, VS4);

            /*
             * The NaN replacement is materialised *before* the compare.
             * MOVW/MOVT do not write flags so it would survive either way,
             * but the ordering is what the FEQ path above had to learn the
             * hard way and keeping it uniform is cheaper than re-deriving
             * which materialisations are safe.
             */
            emit_imm32(R2, cvt_signed ? 0x7FFFFFFFu : 0xFFFFFFFFu);

            /*
             * Quiet, not signalling: the conversion has already raised
             * invalid for a NaN, and VCMPE would raise it a second time for
             * a quiet NaN that RISC-V says nothing about here.
             */
            emit_vcmp(VS0, VS0, false);
            emit_vmrs_apsr();
            emit_it(C_VS);
            emit_mov(R1, R2);       /* MOV register: no flags, IT-safe */

            if (rd != 0u) {
                emit_st_greg(rd, R1);
            }
            /*
             * No emit_fp_dirty: the result goes to an X register, and the
             * interpreter does not mark FS dirty here either. The two
             * backends have to agree on this or a guest that checks FS sees
             * different state depending on which one ran.
             */
            emit_fp_getflags();
            return 0;
        }

        case 0x1Au: {               /* FCVT.S.W / FCVT.S.WU */
            if (rs2 > 1u) {
                return -1;
            }
            const uint32_t rmf = f3;
            if (rmf == FRM_RMM || (rmf > FRM_RMM && rmf != FRM_DYN)) {
                return -1;
            }

            emit_fp_setmode(rmf);
            emit_ld_greg(R1, rs1);
            emit_vmov_s_r(VS0, R1);
            emit_vcvt_f32_from_int(VS4, VS0, rs2 == 0u);
            emit_vmov_r_s(R1, VS4);
            emit_st_freg(rd, R1);
            emit_fp_getflags();
            emit_fp_dirty();
            return 0;
        }

        case 0x1Cu:                 /* FMV.X.W -- raw bits to an X register */
            if (f3 != 0u || rs2 != 0u) {
                return -1;          /* f3 == 1 is FCLASS: helper */
            }
            emit_ld_freg(R1, rs1);
            if (rd != 0u) {
                emit_st_greg(rd, R1);
            }
            return 0;

        case 0x1Eu:                 /* FMV.W.X -- raw bits from an X register */
            if (f3 != 0u || rs2 != 0u) {
                return -1;
            }
            emit_ld_greg(R1, rs1);
            emit_st_freg(rd, R1);
            emit_fp_dirty();
            return 0;

        case 0x00u:   /* FADD.S */
        case 0x01u:   /* FSUB.S */
        case 0x02u:   /* FMUL.S */
        case 0x03u:   /* FDIV.S */
        case 0x0Bu: { /* FSQRT.S */
            /*
             * RMM has no ARM rounding mode; translating it as
             * round-to-nearest would silently give ties-to-even. Leave it
             * to the helper, which implements it properly.
             */
            const uint32_t rmf = f3;
            if (rmf == FRM_RMM || (rmf > FRM_RMM && rmf != FRM_DYN)) {
                return -1;
            }
            const uint32_t f5 = f7 >> 2;
            if (f5 == 0x0Bu && rs2 != 0u) {
                return -1;
            }

            emit_fp_setmode(rmf);

            emit_ld_freg(R1, rs1);
            emit_vmov_s_r(VS0, R1);
            if (f5 != 0x0Bu) {
                emit_ld_freg(R1, rs2);
                emit_vmov_s_r(VS2, R1);
            }

            switch (f5) {
            case 0x00u: emit_vfp3(VFP_ADD, VS4, VS0, VS2, false); break;
            case 0x01u: emit_vfp3(VFP_ADD, VS4, VS0, VS2, true);  break;
            case 0x02u: emit_vfp3(VFP_MUL, VS4, VS0, VS2, false); break;
            case 0x03u: emit_vfp3(VFP_DIV, VS4, VS0, VS2, false); break;
            default:    emit_vsqrt(VS4, VS0);                     break;
            }

            emit_vmov_r_s(R1, VS4);
            emit_st_freg(rd, R1);
            emit_fp_getflags();
            emit_fp_dirty();
            return 0;
        }

        default:
            return -1;              /* conversions and compares: helper */
        }
    }
#endif

    default:
        /* SYSTEM, MISC-MEM and anything else: the interpreter's job. */
        return -1;
    }
}

/*
 * Translate the block starting at `pc`. Returns the block, or NULL if
 * nothing could be translated (the first instruction is unsupported) or
 * the caches are full.
 */
/*
 * Candidate registers for a per-block cache, and how often a block reads
 * them. Measurement only: nothing is cached yet, and the point is to find
 * out whether caching could pay before writing the invalidation logic that
 * would make it correct.
 */
#if RV_JIT_HOT_REG_STATS
static const uint8_t k_hot_regs[4] = { 2u, 1u, 10u, 11u };   /* sp ra a0 a1 */

/* Bit 0 set if the instruction reads rs1, bit 1 if it reads rs2. */
static uint32_t insn_reads(uint32_t insn)
{
    switch (rv_opcode(insn)) {
    case OP_OP:
    case OP_STORE:
    case OP_BRANCH:
    case OP_AMO:
        return 3u;                      /* both */
    case OP_IMM:
    case OP_LOAD:
    case OP_JALR:
    case OP_LOAD_FP:
    case OP_STORE_FP:                   /* rs2 here is an f register */
        return 1u;                      /* rs1 only */
    case OP_SYSTEM:
        /* The CSR immediate forms put a constant in the rs1 field. */
        return (rv_funct3(insn) != 0u && (rv_funct3(insn) & 4u) == 0u) ? 1u : 0u;
    default:
        return 0u;                      /* LUI, AUIPC, JAL, MISC-MEM, OP-FP */
    }
}

static void count_hot_reads(uint32_t insn, uint32_t *per_block)
{
    const uint32_t rd_mask = insn_reads(insn);

    for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t r = k_hot_regs[i];
        if ((rd_mask & 1u) && rv_rs1(insn) == r) { per_block[i]++; }
        if ((rd_mask & 2u) && rv_rs2(insn) == r) { per_block[i]++; }
    }
}

#else
static void count_hot_reads(uint32_t insn, uint32_t *per_block)
{
    (void)insn;
    (void)per_block;
}
#endif

/* True when the block table or the code buffer has no room for a block. */
static bool space_low(void)
{
    return g_block_count >= RV_JIT_MAX_BLOCKS ||
           g_code_used + JIT_HEADROOM >= g_code_size;
}

static jit_block_t *translate_once(rv_hart_t *h, uint32_t pc)
{
    if (space_low()) {
        return NULL;
    }

    /* 4-byte align so the block entry is well-formed. */
    g_code_used = (g_code_used + 3u) & ~3u;
    g_emit = (uint16_t *)(void *)(g_code + g_code_used);
    g_emit_end = (uint16_t *)(void *)(g_code + g_code_size);
    g_emit_overflow = false;

    uint16_t *const start = g_emit;

    /*
     * Describe the inlinable regions to the block. The region table is
     * fixed before execution begins and anything that changes it flushes
     * the JIT, so this is scanned once per flush rather than once per
     * block: doing it per block put a bus walk in front of every
     * translation, and a workload that re-translates as hard as CoreMark
     * does -- 26575 evictions in one run -- paid 8% for it.
     */
    g_ram_live = false;
    g_xlate_hart = h;
    if (!g_regions_scanned) {
        g_ram_base = 0u;
        g_ram_size = 0u;
        g_ram_host = 0u;
        for (uint32_t i = 0; i < h->bus->count; i++) {
            const rv_region_t *r = &h->bus->regions[i];
            if (r->kind == RV_MEM_RAM && r->perm == RV_PERM_RWX) {
                g_ram_base = r->base;
                g_ram_size = r->size;
                g_ram_host = (uint32_t)(uintptr_t)r->host;
                break;
            }
        }
#if RV_JIT_INLINE_PERIPH
        scan_passthru(h->bus);
#endif
        g_regions_scanned = true;
    }
    emit_prologue();
    /*
     * A backward jump landing exactly here re-enters the block, so it can
     * branch within the translated code instead of returning to the
     * dispatcher. Recorded after the prologue: the pushes and the
     * accumulator init must not run again.
     */
    g_loop_start = g_emit;
    g_block_pc = pc;
#if RV_JIT_LOOP_CHAIN
    g_map_len = 0u;
    g_acc_base = 0u;
#endif

    uint32_t cur = pc;
    uint32_t count = 0;
    bool ended = false;
    uint32_t hot[4] = { 0u, 0u, 0u, 0u };

    while (count < RV_JIT_MAX_BLOCK_INSNS && !g_emit_overflow) {
        /*
         * Fetch through the bus so permissions are honoured. A fetch fault
         * during translation simply ends the block; the interpreter will
         * re-fetch and raise the trap with the right cause and mtval.
         */
        uint16_t lo;
        if (rv_bus_fetch16(h->bus, cur, &lo) != RV_EXC_NONE) {
            break;
        }

        uint32_t insn;
        unsigned len;
        if (rv_is_32bit(lo)) {
            uint16_t hi;
            if (rv_bus_fetch16(h->bus, cur + 2u, &hi) != RV_EXC_NONE) {
                break;
            }
            insn = (uint32_t)lo | ((uint32_t)hi << 16);
            len = 4u;
        } else {
#if RV_EXT_C
            insn = rv_decode_expand_c(lo);
            len = 2u;
            if (insn == 0u) {
                break;   /* illegal: let the interpreter report it */
            }
#else
            break;
#endif
        }

        /* Snapshot the cursor so an untranslatable instruction can be
         * rolled back out of the block cleanly. */
#if RV_JIT_LOOP_CHAIN
        if (g_map_len < RV_JIT_MAX_BLOCK_INSNS) {
            g_pc_map[g_map_len] = cur;
            g_code_map[g_map_len] = g_emit;
            g_map_len++;
        }
#endif
        count_hot_reads(insn, hot);

        uint16_t *const before = g_emit;
        uint32_t redirect = 0u;
        const int r = translate_one(insn, cur, len, count + 1u, &redirect);

        if (r < 0) {
            g_emit = before;
            break;
        }
        count++;
        if (r == 1) {
            ended = true;
            break;
        }
        /* r == 2: a followed jump; carry on at the target. */
        cur = (r == 2) ? redirect : (cur + len);
    }

    if (g_emit_overflow || count == 0u) {
        return NULL;
    }

    /* A block that ran out of translatable instructions falls through: set
     * pc to where the interpreter should resume. */
    if (!ended) {
        emit_set_pc(R1, cur);
        emit_epilogue(count);
    }

    if (g_emit_overflow) {
        return NULL;
    }

    jit_block_t *b = &g_blocks[g_block_count];
    b->guest_pc = pc;
    b->code = start;
    b->code_len = (uint16_t)((uint8_t *)g_emit - (uint8_t *)start);
    b->insns = (uint16_t)count;
    b->hits = 1u;

    const uint32_t hidx = pc_hash(pc);
    b->next = g_hash[hidx];
    g_hash[hidx] = (int16_t)g_block_count;
    g_block_count++;
    g_code_used = (uint32_t)((uint8_t *)g_emit - g_code);
    g_stats.translations++;

    for (uint32_t i = 0; i < 4u; i++) {
        if (hot[i] != 0u) {
            g_stats.hot_reads[i] += hot[i];
            g_stats.hot_blocks[i]++;
        }
    }

    /*
     * The code was written as data. On ARMv7-M without a data cache a DSB
     * followed by an ISB is what makes it visible to the instruction side;
     * on a core with caches the platform's cache maintenance would also be
     * required before this point.
     */
    sync_icache();

    return b;
}

/*
 * Translate, reclaiming space if the caches are full.
 *
 * Without this, a guest whose working set outgrows the cache would fall
 * back to the interpreter permanently the first time it filled: nothing
 * ever freed anything, so every later lookup missed and every later
 * translation was refused. Compaction keeps the hot blocks and only
 * full-flushes when even that cannot make room.
 */
static jit_block_t *translate(rv_hart_t *h, uint32_t pc)
{
    if (!space_low()) {
        jit_block_t *b = translate_once(h, pc);
        if (b != NULL) {
            return b;
        }
        /*
         * Failing with space available means this pc simply starts with an
         * instruction the translator does not handle. That is the normal,
         * frequent case -- every interpreted DIV lands here -- and it must
         * not be confused with the cache being full. Reclaiming here would
         * compact, and often flush, the entire cache once per interpreted
         * instruction, which is exactly the pathology that made a
         * div-heavy loop translate the same blocks tens of thousands of
         * times.
         */
        if (!g_emit_overflow) {
            return NULL;
        }
    }

    /* Genuinely out of room. Keep the hot blocks, drop the rest. */
    compact();
    if (!space_low()) {
        jit_block_t *b = translate_once(h, pc);
        if (b != NULL) {
            return b;
        }
        if (!g_emit_overflow) {
            return NULL;
        }
    }

    /* Compaction could not free enough: start over. */
    rv_jit_flush();
    return translate_once(h, pc);
}

/* ------------------------------------------------------------------ */
/* Backend                                                             */
/* ------------------------------------------------------------------ */

typedef uint32_t (*block_fn_t)(rv_hart_t *h);

static bool jit_init(rv_hart_t *h)
{
    (void)h;
    if (!g_hash_ready) {
        rv_jit_flush();
    }
    return true;
}

static void jit_reset(rv_hart_t *h)
{
#if RV_EXT_PMP
    g_pmp_seen = h->pmp_active;
#else
    (void)h;
#endif
    rv_jit_flush();
}

static void jit_invalidate(rv_hart_t *h, uint32_t addr, uint32_t len)
{
    (void)h;
    (void)addr;
    (void)len;
    /* Whole-cache flush: translations are cheap to rebuild and tracking
     * which blocks covered a given range would cost more than it saves. */
    rv_jit_flush();
}

static rv_run_reason_t jit_run(rv_hart_t *h, uint32_t budget, uint32_t *retired)
{
    uint32_t done = 0;
    rv_run_reason_t reason = RV_RUN_BUDGET;

    if (RV_UNLIKELY(g_code == NULL)) {
        /* No code buffer configured: behave exactly like the interpreter. */
        return rv_backend_interp.run(h, budget, retired);
    }

    if (RV_UNLIKELY(h->state == RV_STATE_WFI)) {
        if (rv_hart_pending_irq(h) == RV_EXC_NONE) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return RV_RUN_WFI;
        }
        h->state = RV_STATE_RUNNING;
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;
#endif
    }

    while (done < budget) {
        if (RV_UNLIKELY(h->state != RV_STATE_RUNNING)) {
            reason = (h->state == RV_STATE_HALTED) ? RV_RUN_HALTED : RV_RUN_WFI;
            break;
        }

#if RV_EXT_SDTRIG
        /*
         * An armed trigger needs a check before every fetch, which a
         * translated block cannot express, and its load and store checks
         * live in rv_hart_load/store which the inlined path bypasses. So
         * while any trigger is armed the interpreter runs everything.
         *
         * No flush is needed either way, which is worth stating because the
         * first version did one and paid for it. Blocks are only ever
         * translated while unarmed and only ever executed while unarmed, so
         * a cached block cannot have been built under the wrong assumption.
         * That first version also stored a "seen" flag unconditionally on
         * every dispatch, which cost 16% on CoreMark for a guest that never
         * arms a trigger at all.
         */
        if (RV_UNLIKELY(h->trig_active)) {
            uint32_t n = 0;
            const rv_run_reason_t r = rv_backend_interp.run(h, 1u, &n);
            done += n;
            g_stats.interp_fallbacks += n;
            if (r == RV_RUN_HALTED || r == RV_RUN_WFI) {
                reason = r;
                break;
            }
            continue;
        }
#endif

#if RV_EXT_PMP
        /*
         * A guest that has just locked a PMP entry invalidates every block
         * translated while PMP was inert, because those inlined their
         * memory accesses.
         */
        if (RV_UNLIKELY(h->pmp_active != g_pmp_seen)) {
            g_pmp_seen = h->pmp_active;
            rv_jit_flush();
            continue;
        }
#endif

        /* Interrupts are delivered between blocks, which bounds latency by
         * the block length rather than by a chain of them. */
#if RV_LAZY_IRQ_CHECK
        if (RV_UNLIKELY(h->irq_dirty))
#endif
        {
#if RV_LAZY_IRQ_CHECK
            h->irq_dirty = false;
#endif
            const rv_exc_t irq = rv_hart_pending_irq(h);
            if (RV_UNLIKELY(irq != RV_EXC_NONE)) {
                rv_hart_trap(h, RV_CAUSE_INTERRUPT | irq, 0u);
                done++;
                continue;
            }
        }

        jit_block_t *b = lookup(h->pc);
        if (b == NULL) {
            b = translate(h, h->pc);
            if (b == NULL) {
                /*
                 * Nothing translatable here. Run a single instruction on
                 * the interpreter and try again; that covers SYSTEM, AMO,
                 * the M helpers and every encoding the translator skips.
                 */
                uint32_t n = 0;
                const rv_run_reason_t r = rv_backend_interp.run(h, 1u, &n);
                done += n;
                g_stats.interp_fallbacks += n;
                if (r == RV_RUN_HALTED || r == RV_RUN_WFI) {
                    reason = r;
                    break;
                }
                continue;
            }
        }

        /* Thumb bit set so BLX enters in Thumb state. */
        const block_fn_t fn =
            (block_fn_t)(uintptr_t)((uint32_t)(uintptr_t)b->code | 1u);
        const uint32_t n = fn(h);
        g_stats.block_entries++;
        done += n;

#if RV_EXT_ZICNTR
        if (RV_LIKELY((h->mcountinhibit & 0x1u) == 0u)) {
            h->mcycle += n;
        }
        if (RV_LIKELY((h->mcountinhibit & 0x4u) == 0u)) {
            h->minstret += n;
        }
#endif
    }

#if RV_ENABLE_STATS
    h->insn_retired_lo += done;
#endif
    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}

const rv_backend_t rv_backend_jit = {
    .name       = "jit-thumb2",
    .init       = jit_init,
    .reset      = jit_reset,
    .run        = jit_run,
    .invalidate = jit_invalidate,
};

#endif /* RV_ENABLE_JIT */
