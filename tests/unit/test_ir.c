/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ir.c - The optimisation passes between a frontend and a host.
 *
 * These are tested on hand-built IR rather than through a frontend, for
 * the same reason the G4MH tests are hand-assembled: a test that drove
 * the passes through the G4MH lowering would pass whenever the lowering
 * and the pass were wrong in the same direction, and the whole value of
 * a shared optimiser is that it is shared -- a bug here is a bug for
 * every frontend and every host at once.
 *
 * Each test states what the pass must *not* do as well as what it must,
 * because every one of these passes deletes code, and a pass that
 * deletes too much produces a wrong answer some distance away rather
 * than a crash.
 */

#include "tests.h"

#include "emu/emu_ir.h"

#include <string.h>

/*
 * Forward declaration. The pass tests below run emu_ir_optimise, which
 * takes the target so the register-traffic pass can see which guest
 * register is hardwired to zero. Defined with the rest of the fake
 * machine further down.
 */
static const emu_ir_target_t g_fake_target;

static emu_ir_block_t g_b;

/* Instructions the backend would actually emit. */
static uint32_t live_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_b.count; i++) {
        if (!g_b.insn[i].dead) {
            n++;
        }
    }
    return n;
}

static uint32_t count_op(emu_ir_op_t op)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_b.count; i++) {
        if (!g_b.insn[i].dead && g_b.insn[i].op == (uint8_t)op) {
            n++;
        }
    }
    return n;
}

/*
 * Two flag-setting operations in a row with no conditional between them.
 * The first definition is dead: nothing can observe it.
 *
 * This is the case the IR exists for on a flag machine. G4MH sets Z/S/OV/CY
 * on nearly every arithmetic instruction and x86-64 needs `seto`+`lahf`
 * plus masking to materialise them, so deleting the ones nobody reads is
 * the largest single saving available to that frontend.
 */
static void test_dead_flags_removed(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s1, b, 0u,
                      EMU_IR_F_ALL);              /* dead */

    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_SUB, s1, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, s2, b, 0u,
                      EMU_IR_F_ALL);              /* live: block exit */
    emu_ir_put(&g_b, 3u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_SETF), 1u);
    /* The arithmetic itself must survive -- only the flags were dead. */
    CHECK_EQ(count_op(EMU_IR_ADD), 1u);
    CHECK_EQ(count_op(EMU_IR_SUB), 1u);
}

/*
 * The same shape with a conditional in between. Now the first definition
 * is read, and deleting it would take a branch the wrong way.
 *
 * Without this case the pass above could be implemented as "delete every
 * SETF but the last" and still pass.
 */
static void test_flags_kept_when_read(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_SUB, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, s1, b, 0u,
                      EMU_IR_F_ALL);
    const uint16_t c = emu_ir_emit(&g_b, EMU_IR_GETCOND, EMU_IR_C_EQ,
                                   EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
    emu_ir_put(&g_b, 4u, c);

    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s2, b, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 3u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_SETF), 2u);
}

/*
 * A flag definition at the end of a block is live even though nothing in
 * the block reads it: the next block, an interrupt handler and the
 * debugger can all see the guest's flag word.
 *
 * Checked by asking for a live-out set of nothing and seeing the count
 * change -- which is what proves the parameter is honoured rather than
 * ignored.
 */
static void test_flags_live_out(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s, a, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 2u, s);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);
    CHECK_EQ(st.flags_removed, 0u);

    /* Same block, but the frontend proved no later reader exists. */
    emu_ir_reset(&g_b);
    const uint16_t a2 = emu_ir_get(&g_b, 1u);
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_ADD, a2, a2);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s2, a2, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 2u, s2);

    emu_ir_optimise(&g_b, &g_fake_target, 0u, &st);
    CHECK_EQ(st.flags_removed, 1u);
}

/*
 * A guest register read straight after it was written comes from the
 * temp, not from memory.
 *
 * This is the measured register-file round trip: a quarter to a third of
 * adjacent instruction pairs in this project's guests are data
 * dependent, and each emits a store immediately followed by a load of
 * the same slot.
 */
static void test_redundant_get_elided(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);

    /* The next guest instruction reads r3 back. */
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_XOR, reread, a);
    emu_ir_put(&g_b, 4u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.gets_removed, 1u);
    /* Two real loads remain: r1 and r2. */
    CHECK_EQ(count_op(EMU_IR_GET), 2u);
    /*
     * And the reload became nothing at all rather than a MOV -- the
     * value pass sweeps it up once the operand is taken directly.
     */
    CHECK_EQ(count_op(EMU_IR_MOV), 0u);
}

/*
 * A guest register written twice with nothing observable in between
 * needs only the second store...
 */
static void test_dead_put_removed(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t x = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    emu_ir_put(&g_b, 5u, x);          /* dead: overwritten below */
    const uint16_t y = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 5u, y);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_PUT), 1u);
}

/*
 * ...and neither may a *faulting* memory operation be stepped over.
 *
 * A store to a register followed by a second store to the same register
 * looks like a dead first store -- unless what sits between them can
 * trap, because then the second never runs and the first is the value
 * the trap handler reads.
 *
 * riscv-tests' rv32mi/ma_addr is exactly this shape:
 *
 *     addi t1, s0, 1     ; t1 = the address about to fault
 *     lh   t1, 1(s0)     ; traps; never writes t1
 *
 * and its handler compares mtval against t1. Deleting the first store
 * left t1 stale and failed one sub-test, while the other 76 riscv-tests,
 * the architecture suite, isatest and every unit test here passed.
 */
static void test_put_kept_across_faulting_load(void)
{
    emu_ir_reset(&g_b);

    const uint16_t base = emu_ir_get(&g_b, 1u);
    const uint16_t addr = emu_ir_alu(&g_b, EMU_IR_ADD, base,
                                     emu_ir_const(&g_b, 1u));
    emu_ir_put(&g_b, 6u, addr);                  /* must survive */

    /* The load writes the same guest register, and can fault. */
    emu_ir_put(&g_b, 6u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(2u, 1u), base,
                                     EMU_IR_NO_TEMP, 1u, 0u));

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_PUT), 2u);
}

/*
 * ...but a helper call in between can read the register file, so the
 * first store has to stay.
 *
 * This is the case that separates a correct pass from one that just
 * counts stores per register, and it is the direction that produces a
 * wrong answer rather than slow code.
 */
static void test_put_kept_across_helper(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t x = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    emu_ir_put(&g_b, 5u, x);
    (void)emu_ir_emit(&g_b, EMU_IR_HELPER, 0u, a, EMU_IR_NO_TEMP, 0u, 0u);
    const uint16_t y = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 5u, y);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_PUT), 2u);
}

/*
 * A helper may also write the register file, so a read after one cannot
 * be served from a temp captured before it.
 */
static void test_get_not_elided_across_helper(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 3u, a);
    (void)emu_ir_emit(&g_b, EMU_IR_HELPER, 0u, a, EMU_IR_NO_TEMP, 0u, 0u);
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    emu_ir_put(&g_b, 4u, reread);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.gets_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_GET), 2u);
}

/*
 * A computed value nothing consumes goes, but one consumed only by a
 * store stays. The first is what makes the other passes pay; the second
 * is the way a dead-value pass most easily becomes a wrong-answer bug.
 */
static void test_dead_values(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    (void)emu_ir_alu(&g_b, EMU_IR_ADD, a, a);     /* nothing reads it */
    const uint16_t keep = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 2u, keep);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.dead_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_ADD), 0u);
    CHECK_EQ(count_op(EMU_IR_XOR), 1u);
    CHECK_EQ(count_op(EMU_IR_PUT), 1u);
}

/*
 * A frontend with no condition flags at all -- RISC-V -- must pay
 * nothing. No SETF is ever emitted, so there is nothing for the flag
 * pass to find, and the register passes still apply.
 */
static void test_flagless_frontend(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    emu_ir_put(&g_b, 4u, reread);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 0u);
    CHECK_EQ(st.gets_removed, 1u);
    CHECK(live_count() < g_b.count);
}

/*
 * Reader counts, which a backend needs before it can fuse.
 *
 * The property that matters is that the count is taken *after*
 * everything that deletes code: a value whose only reader was itself
 * removed must come out at zero, not one. A fusion trusting a stale
 * count would fold a value into an instruction and leave the original
 * emitted too -- computing it twice, which is worse than not fusing.
 */
static void test_use_counts(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    /* One reader: the shift below. */
    const uint16_t sh = emu_ir_emit(&g_b, EMU_IR_SHLI, 0u, a,
                                    EMU_IR_NO_TEMP, 4u, 0u);
    emu_ir_put(&g_b, 2u, emu_ir_alu(&g_b, EMU_IR_ADD, a, sh));

    /* Two readers. */
    const uint16_t t = emu_ir_get(&g_b, 3u);
    emu_ir_put(&g_b, 4u, emu_ir_alu(&g_b, EMU_IR_XOR, t, t));

    /*
     * A value with one live reader and one dead one. This is the case
     * that discriminates: counted before the deletions it has two
     * readers, after them one -- and a fusion that trusted the former
     * would decline to fold something it safely could, or worse, fold
     * something it could not.
     */
    const uint16_t shared = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    emu_ir_put(&g_b, 5u, shared);                     /* live reader */
    (void)emu_ir_alu(&g_b, EMU_IR_SUB, shared, a);    /* dead reader */

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    uint32_t sh_uses = 0u, t_uses = 0u, shared_uses = 0u;

    for (uint32_t i = 0; i < g_b.count; i++) {
        const emu_ir_insn_t *const in = &g_b.insn[i];

        if (in->dead) {
            continue;
        }
        if (in->dst == sh) { sh_uses = in->uses; }
        if (in->dst == t)  { t_uses = in->uses; }
        if (in->dst == shared) { shared_uses = in->uses; }
    }

    CHECK_EQ(sh_uses, 1u);      /* exactly the fusion candidate */
    CHECK_EQ(t_uses, 2u);       /* read twice by one instruction */
    /* One live reader, not two: the dead one must not be counted. */
    CHECK_EQ(shared_uses, 1u);
    CHECK(st.single_use > 0u);
}

/*
 * Overflowing the block must be reported and must leave the passes
 * alone: a partially built block is not a correct one, and optimising it
 * would produce something that looked runnable.
 */
static void test_overflow_not_optimised(void)
{
    emu_ir_reset(&g_b);
    for (uint32_t i = 0; i < EMU_IR_MAX_INSNS + 4u; i++) {
        (void)emu_ir_const(&g_b, i);
    }
    CHECK(g_b.overflow);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);
    CHECK_EQ(st.dead_removed, 0u);
}

/* ------------------------------------------------------------------ */
/* Lowering, executed                                                  */
/* ------------------------------------------------------------------ */

#if defined(EMU_JIT_X86_64)

#include "emu/emu_x86_64.h"

#include <stddef.h>
#include <sys/mman.h>

/*
 * A stand-in guest, laid out the way a real frontend's state is: a
 * register file, a flag word and a pc, reached by byte offset.
 *
 * The point of these tests is that the emitted bytes are *run*. A
 * lowering can be read and reviewed and still encode the wrong ModRM
 * byte -- and a wrong ModRM does not fail to assemble, it addresses
 * something else. Nothing short of executing it says otherwise.
 */
typedef struct {
    uint32_t r[16];
    uint32_t flags;
    uint32_t pc;
    /*
     * The FP register file, 64 bits wide -- as RV32's is once D widens
     * FLEN, and which is the only width at which EMU_IR_FP_BOX means
     * anything. A test whose file was 32 bits could set the flag and
     * observe nothing, which is the shape of a test that passes against
     * the bug it covers.
     */
    uint64_t f[16];
    uint32_t fe;           /* accumulated EMU_IR_FE_* flags */
} fake_cpu_t;

/* All-ones in the upper half: what a single-precision value carries. */
#define F_BOXED(v)  (UINT64_C(0xFFFFFFFF00000000) | (v))

#define FAKE_F_Z (1u << 0)
#define FAKE_F_S (1u << 1)
#define FAKE_F_V (1u << 2)
#define FAKE_F_C (1u << 3)

static uint32_t fake_reg_offset(uint32_t n)
{
    return (uint32_t)offsetof(fake_cpu_t, r) + n * 4u;
}

static bool fake_reg_is_zero(uint32_t n) { return n == 0u; }

static uint32_t fake_freg_offset(uint32_t n)
{
    return (uint32_t)(offsetof(fake_cpu_t, f) + n * sizeof(uint64_t));
}

static void fake_fp_flags(emu_cpu_t *cpu, uint32_t flags)
{
    ((fake_cpu_t *)(void *)cpu)->fe |= flags;
}

/*
 * A tiny guest memory, so the memory operations can be lowered and run
 * rather than declined. Deliberately not the real bus: what is under
 * test is the lowering's call sequence and its trap path, not any
 * frontend's address map.
 */
static uint8_t g_mem[256];

static uint32_t fake_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                          uint32_t *out)
{
    (void)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    if (addr + size > sizeof(g_mem)) {
        return 1u;                       /* "trapped" */
    }
    uint32_t v = 0u;
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint32_t)g_mem[addr + i] << (8u * i);
    }
    if ((spec & EMU_IR_MEM_SIGNED) != 0u && size < 4u) {
        const uint32_t sign = 1u << (size * 8u - 1u);
        if ((v & sign) != 0u) {
            v |= ~((sign << 1u) - 1u);
        }
    }
    *out = v;
    return 0u;
}

static uint32_t fake_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                           uint32_t val)
{
    (void)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    if (addr + size > sizeof(g_mem)) {
        return 1u;
    }
    for (uint32_t i = 0; i < size; i++) {
        g_mem[addr + i] = (uint8_t)(val >> (8u * i));
    }
    return 0u;
}

static const emu_ir_target_t g_fake_target = {
    .reg_offset   = fake_reg_offset,
    .flags_offset = (uint32_t)offsetof(fake_cpu_t, flags),
    .flag_bit     = { FAKE_F_Z, FAKE_F_S, FAKE_F_V, FAKE_F_C },
    .reg_is_zero  = fake_reg_is_zero,
    .pc_offset    = (uint32_t)offsetof(fake_cpu_t, pc),
    .helpers      = NULL,
    .helper_count = 0u,
    .load         = fake_load,
    .store        = fake_store,
    .freg_offset  = fake_freg_offset,
    .fp_flags     = fake_fp_flags,
};

#define IR_TEST_CODE_BYTES 8192u

/*
 * Lower the current block into an executable mapping and call it.
 *
 * Its own mmap rather than the framework's buffer: that one belongs to
 * the dispatch loop, and this test has no block cache behind it.
 */
static bool lower_and_run(fake_cpu_t *cpu)
{
    static uint8_t *exec;

    if (exec == NULL) {
        void *const p = mmap(NULL, IR_TEST_CODE_BYTES,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        exec = (uint8_t *)p;
    }

    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, NULL);

    /*
     * No prologue or epilogue here: emu_ir_lower emits the whole block
     * including its frame, which is what lets emu_ir_jit.c name no host.
     * Emitting them here as well pushed twice and popped once.
     */
    emu_jit_emit_begin(exec, IR_TEST_CODE_BYTES);
    const bool ok = emu_ir_lower(&g_b, &g_fake_target);
    if (!ok || emu_jit_overflowed()) {
        return false;
    }

    ((uint32_t (*)(void *))(void *)exec)(cpu);
    return true;
}

/*
 * The register file round trip, executed: read two guest registers, add
 * them, write a third.
 */
static void test_lower_add(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 40u;
    cpu.r[2] = 2u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 42u);
}

/* Register 0 reads as zero and discards writes, as both guests define. */
static void test_lower_zero_register(void)
{
    emu_ir_reset(&g_b);
    const uint16_t z = emu_ir_get(&g_b, 0u);
    const uint16_t c = emu_ir_const(&g_b, 0x1234u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_OR, z, c);
    emu_ir_put(&g_b, 4u, s);
    emu_ir_put(&g_b, 0u, c);          /* discarded */

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[0] = 0xDEADBEEFu;           /* must not be read */

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[4], 0x1234u);
    CHECK_EQ(cpu.r[0], 0xDEADBEEFu);  /* the write went nowhere */
}

/*
 * The same register, written *before* it is read.
 *
 * The test above reads r0 and then writes it, and passed for the whole
 * life of a bug that made 36 of 39 architecture tests fail: the
 * register-traffic pass recorded the discarded write and rewrote the
 * next read into a MOV of the value written, so a guest saw its own
 * discarded result where the architecture guarantees zero. Every
 * lowering handles the hardwired register correctly -- but the pass runs
 * first and rewrites the IR, so all three faithfully compiled the wrong
 * thing.
 *
 * The order is the entire test. Writing r0 is not a corner case: every
 * discarded result and every canonical NOP is one.
 *
 * It is invisible to the differential checker by construction, because
 * the reference interpreter runs the same optimised IR and agrees.
 */
static void test_zero_register_write_then_read(void)
{
    emu_ir_reset(&g_b);

    /* Something the guest computed and threw away, as `add r0, ...`. */
    const uint16_t junk = emu_ir_const(&g_b, 0xA5A5A5A5u);
    emu_ir_put(&g_b, 0u, junk);

    /* ...and then a genuine read of the zero register. */
    const uint16_t z = emu_ir_get(&g_b, 0u);
    const uint16_t v = emu_ir_get(&g_b, 2u);
    emu_ir_put(&g_b, 3u, emu_ir_alu(&g_b, EMU_IR_ADD, z, v));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[0] = 0xDEADBEEFu;
    cpu.r[2] = 7u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 7u);           /* 0 + 7, not junk + 7 */
    CHECK_EQ(cpu.r[0], 0xDEADBEEFu);
}

/*
 * And the same thing one level down, on the IR the pass leaves behind:
 * the read must not have become a MOV of the discarded value.
 *
 * Checked separately from the result above because the two fail for
 * different reasons -- this one says the pass declined to track the
 * register, the one above says the whole pipeline agrees on zero.
 */
static void test_zero_register_not_forwarded(void)
{
    emu_ir_reset(&g_b);

    emu_ir_put(&g_b, 0u, emu_ir_const(&g_b, 0xA5A5A5A5u));
    const uint16_t z = emu_ir_get(&g_b, 0u);
    emu_ir_put(&g_b, 3u, emu_ir_alu(&g_b, EMU_IR_ADD, z,
                                    emu_ir_get(&g_b, 2u)));

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, &st);

    /*
     * Nothing was elided: the only candidate was the hardwired register,
     * and r2 is read once. A pass that forwarded r0 would report 1 here
     * and leave a MOV behind for the lowering to compile.
     */
    CHECK_EQ(st.gets_removed, 0u);
}

/*
 * The bit and byte group, which is the reason these are IR operations
 * rather than shift sequences. Each lowers to one host instruction, and
 * each is checked on the input that separates it from its neighbours.
 */
static void test_lower_bit_ops(void)
{
    emu_ir_reset(&g_b);
    const uint16_t v = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_BSWAP32, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 3u, emu_ir_emit(&g_b, EMU_IR_HSWAP, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_BSWAP16, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x11223344u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[2], 0x44332211u);   /* BSWAP32 */
    CHECK_EQ(cpu.r[3], 0x33441122u);   /* HSWAP   */
    CHECK_EQ(cpu.r[4], 0x22114433u);   /* BSWAP16 */
}

/*
 * CLZ and CTZ, on zero as well as on a normal input. Zero is the case a
 * lowering built on bsr/bsf gets wrong -- those leave the destination
 * untouched -- and it is exactly the input a bit search is most often
 * handed.
 */
static void test_lower_bit_counts(void)
{
    emu_ir_reset(&g_b);
    const uint16_t v = emu_ir_get(&g_b, 1u);
    const uint16_t z = emu_ir_get(&g_b, 2u);
    emu_ir_put(&g_b, 3u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_CTZ, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, z,
                                     EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x00100000u;   /* bit 20 */
    cpu.r[2] = 0u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 11u);
    CHECK_EQ(cpu.r[4], 20u);
    CHECK_EQ(cpu.r[5], 32u);   /* defined for zero */
}

/*
 * Flags, and the thing the whole IR was built for: a SETF whose
 * definition is dead emits nothing at all, while the live one writes the
 * guest's flag word without disturbing the bits around it.
 */
static void test_lower_flags(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s1,
                      EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);   /* dead */
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_SUB, a, a);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s2,
                      EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);   /* live */
    emu_ir_put(&g_b, 3u, s2);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 7u;
    cpu.r[2] = 5u;
    /* A bit outside the four the IR knows about, which must survive. */
    cpu.flags = 0x80000000u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 0u);
    CHECK((cpu.flags & FAKE_F_Z) != 0u);        /* 7 - 7 == 0 */
    CHECK((cpu.flags & FAKE_F_S) == 0u);
    CHECK_EQ(cpu.flags & 0x80000000u, 0x80000000u);
}

#endif /* EMU_JIT_X86_64 */

/*
 * The backend's two execution strategies, run against each other.
 *
 * emu_ir_interp evaluates the IR in C; emu_ir_lower emits x86-64 and the
 * result is called. They share nothing but emu_ir.h, so a disagreement
 * is a real bug in one of them rather than a stale expectation -- the
 * same reason this project runs SoftFloat against the VFP path.
 *
 * This is the check that would have caught lzcnt-decodes-as-bsr without
 * anyone having worked out by hand what CLZ(0x00100000) should be.
 */
static void diff_one(uint32_t seed_r1, uint32_t seed_r2)
{
    fake_cpu_t a, b;

    memset(&a, 0, sizeof(a));
    a.r[1] = seed_r1;
    a.r[2] = seed_r2;
    a.flags = 0x80000000u;
    b = a;

    /* The block is optimised in place, so interpret first. */
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, NULL);
    if (!emu_ir_interp(&g_b, (emu_cpu_t *)&a, &g_fake_target)) {
        CHECK(false);
        return;
    }
    if (!lower_and_run(&b)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(memcmp(&a, &b, sizeof(a)), 0);
}

/*
 * Every flag at once, on an operation that sets more than one of them.
 *
 * test_lower_flags above expects S, V and C to be *clear* and so cannot
 * see a flag word that comes out empty. It passed for the whole life of a
 * bug that made the x86-64 emitter produce exactly that: it emitted one
 * `setcc` per guest flag and folded each into an accumulator with `shl`
 * and `or`, both of which write ZF, SF, CF and OF -- so only the first
 * flag in the loop saw the operation's real flags and the rest read the
 * previous `or`'s.
 *
 * 0 - 8 is the discriminating input: Z clear, S *set*, V clear, C *set*.
 * Under the bug S and C came out clear, which made a G4MH `blt` fall
 * through and a `for (i = 0; i < 8; i++)` loop run zero times.
 *
 * Checked through lower_and_run rather than by inspecting the emitted
 * bytes, because what is being asserted is agreement with the IR
 * interpreter, which computes the same four flags from the same operands.
 */
static void test_lower_flags_all_four(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, a, b, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 3u, emu_ir_alu(&g_b, EMU_IR_SUB, a, b));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;
    cpu.r[2] = 8u;
    cpu.flags = 0x80000000u;            /* must survive the update */

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(cpu.r[3], (uint32_t)-8);
    CHECK((cpu.flags & FAKE_F_Z) == 0u);        /* 0 - 8 != 0        */
    CHECK((cpu.flags & FAKE_F_S) != 0u);        /* the result is < 0 */
    CHECK((cpu.flags & FAKE_F_V) == 0u);        /* no signed overflow*/
    CHECK((cpu.flags & FAKE_F_C) != 0u);        /* borrow            */
    CHECK_EQ(cpu.flags & 0x80000000u, 0x80000000u);
}

/*
 * The sign flag is the sign of the *result*, not "signed less than".
 *
 * They differ exactly when the subtraction overflows, which is why this
 * needs its own case: 0x80000000 - 1 is a positive result (0x7FFFFFFF)
 * with V set, so S must be *clear* while `setl` -- SF != OF -- would say
 * set. The IR interpreter computes `d & 0x80000000` and is the reference.
 */
static void test_lower_flags_sign_not_less_than(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, a, b, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 3u, emu_ir_alu(&g_b, EMU_IR_SUB, a, b));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x80000000u;
    cpu.r[2] = 1u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }

    CHECK_EQ(cpu.r[3], 0x7FFFFFFFu);
    CHECK((cpu.flags & FAKE_F_S) == 0u);        /* result is positive */
    CHECK((cpu.flags & FAKE_F_V) != 0u);        /* and it overflowed  */
}

static void test_interp_matches_jit(void)
{
    static const uint32_t k_seeds[][2] = {
        { 0x00000000u, 0x00000000u },
        { 0x00000001u, 0xFFFFFFFFu },
        { 0x80000000u, 0x80000000u },
        { 0x11223344u, 0x0000000Fu },
        { 0x00100000u, 0x00000001u },
    };

    for (unsigned i = 0; i < sizeof(k_seeds) / sizeof(k_seeds[0]); i++) {
        /* A block touching the value ops, the bit group and the flags. */
        emu_ir_reset(&g_b);
        const uint16_t r1 = emu_ir_get(&g_b, 1u);
        const uint16_t r2 = emu_ir_get(&g_b, 2u);

        const uint16_t sum = emu_ir_alu(&g_b, EMU_IR_ADD, r1, r2);
        (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, sum, r2, 0u,
                          EMU_IR_F_ALL);
        emu_ir_put(&g_b, 3u, sum);

        emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_BSWAP32, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_HSWAP, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 6u, emu_ir_emit(&g_b, EMU_IR_BSWAP16, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 7u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 8u, emu_ir_emit(&g_b, EMU_IR_CTZ, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));

        const uint16_t d = emu_ir_alu(&g_b, EMU_IR_SUB, r1, r2);
        (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, d,
                          EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);
        emu_ir_put(&g_b, 9u, d);

        emu_ir_put(&g_b, 10u, emu_ir_alu(&g_b, EMU_IR_SHL, r1, r2));
        emu_ir_put(&g_b, 11u, emu_ir_alu(&g_b, EMU_IR_SAR, r1, r2));
        emu_ir_put(&g_b, 12u, emu_ir_emit(&g_b, EMU_IR_BEXT, 0u, r1, r2,
                                          0u, 0u));
        emu_ir_put(&g_b, 13u, emu_ir_emit(&g_b, EMU_IR_BINV, 0u, r1, r2,
                                          0u, 0u));

        diff_one(k_seeds[i][0], k_seeds[i][1]);
    }
}

/*
 * Loads, stores and the memory bit ops, lowered and executed, and then
 * run again through the IR interpreter for comparison.
 *
 * These were the four the lowering used to decline, and declining is
 * exactly the failure that hides: a frontend emitting them silently
 * loses its whole block to the interpreter while every suite stays
 * green.
 */
static void test_lower_memory(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x40] = 0x12u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    /* r2 = load.b [r1 + 0x40], sign-extended */
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(1u, 1u), base,
                                     EMU_IR_NO_TEMP, 0x40u, 0u));
    /* store.w r3 -> [r1 + 0x10] */
    (void)emu_ir_emit(&g_b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u), base,
                      emu_ir_get(&g_b, 3u), 0x10u, 0u);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;
    cpu.r[3] = 0xDEADBEEFu;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[2], 0x12u);
    CHECK_EQ((uint32_t)g_mem[0x10], 0xEFu);
    CHECK_EQ((uint32_t)g_mem[0x13], 0xDEu);
}

/*
 * A load whose address the target refuses. The block must stop there --
 * the instructions after it belong to whatever the trap preempted, and
 * running them is not a wrong value but a wrong program.
 */
static void test_lower_memory_trap(void)
{
    memset(g_mem, 0, sizeof(g_mem));

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(4u, 0u), base,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    /* Must not run. */
    emu_ir_put(&g_b, 4u, emu_ir_const(&g_b, 0xABCDu));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x1000u;                 /* out of range -> refused */

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[4], 0u);
}

/*
 * The bit ops on memory, both encodings' worth of behaviour: Z reports
 * the bit before the change, TST does not write back, and the bits of
 * the flag word outside Z are left alone.
 */
static void test_lower_bitop_memory(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x20] = 0x01u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    const uint16_t bit1 = emu_ir_const(&g_b, 1u);

    emu_ir_bitop(&g_b, EMU_IR_BITOP_SET, base, bit1, 0x20u, 0u);
    emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_GETCOND, EMU_IR_C_EQ,
                                     EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;
    cpu.flags = 0x80000000u | FAKE_F_C;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    /* bit 1 was 0 -> set, and Z reports that it had been clear. */
    CHECK_EQ((uint32_t)g_mem[0x20], 0x03u);
    CHECK_EQ(cpu.r[5], 1u);
    /* Only Z moved. */
    CHECK_EQ(cpu.flags & FAKE_F_C, FAKE_F_C);
    CHECK_EQ(cpu.flags & 0x80000000u, 0x80000000u);
}

/* TST must not write, checked on a clear bit so a stray write shows. */
static void test_lower_bitop_tst(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x20] = 0x80u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);

    emu_ir_bitop(&g_b, EMU_IR_BITOP_TST, base, emu_ir_const(&g_b, 0u),
                 0x20u, 0u);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ((uint32_t)g_mem[0x20], 0x80u);
    CHECK((cpu.flags & FAKE_F_Z) != 0u);   /* bit 0 was clear */
}

/*
 * The extended registers, executed.
 *
 * REX support is otherwise dead code: no lowering uses r8-r15 yet, so
 * every suite passes whether the prefix is right or absent. A wrong or
 * missing REX does not fault -- it names a *different* register, which
 * is the quietest possible failure.
 *
 * r8-r11 are caller-saved in System V, so this needs no save/restore
 * beyond rbx, which carries the cpu pointer.
 */
static void test_encode_rex(void)
{
    static uint8_t *exec;

    if (exec == NULL) {
        void *const m = mmap(NULL, IR_TEST_CODE_BYTES,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            CHECK(false);
            return;
        }
        exec = (uint8_t *)m;
    }

    emu_jit_emit_begin(exec, IR_TEST_CODE_BYTES);
    emu_jit_emit8(0x53);                          /* push rbx      */
    emu_jit_emit8(0x48); emu_jit_emit8(0x89);
    emu_jit_emit8(0xFB);                          /* mov rbx, rdi  */

    x86_mov_imm32(X86_R8, 0x00001000u);
    x86_mov_imm32(X86_R9, 0x00000234u);
    x86_alu_rr(X86_ADD, X86_R8, X86_R9);          /* r8 = 0x1234   */
    x86_st_cpu(X86_R8, (uint32_t)offsetof(fake_cpu_t, r) + 3u * 4u);

    x86_ld_cpu(X86_R10, (uint32_t)offsetof(fake_cpu_t, r) + 1u * 4u);
    x86_shift_imm(X86_R10, X86_SHL, 4u);
    x86_mov_rr(X86_R11, X86_R10);
    x86_st_cpu(X86_R11, (uint32_t)offsetof(fake_cpu_t, r) + 4u * 4u);

    emu_jit_emit8(0x5B);                          /* pop rbx       */
    emu_jit_emit8(0xC3);                          /* ret           */
    if (emu_jit_overflowed()) {
        CHECK(false);
        return;
    }

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x0000000Fu;
    ((void (*)(void *))(void *)exec)(&cpu);

    CHECK_EQ(cpu.r[3], 0x00001234u);   /* imm, add, store via r8/r9 */
    CHECK_EQ(cpu.r[4], 0x000000F0u);   /* load, shift, mov via r10/r11 */
}

/* ------------------------------------------------------------------ */

/*
 * The floating-point semantics that are *not* what a host instruction of
 * the same name does.
 *
 * Every case here is one where the obvious lowering is wrong:
 *
 *   FMIN/FMAX  both guests return the other operand for a NaN; every
 *              host minss/maxss returns its second operand regardless,
 *              so "emit minss" is wrong for exactly the input a test
 *              using ordinary numbers never supplies
 *   FCMP       unordered is false for all three comparisons, including
 *              the one spelled "not equal" -- so it cannot be an
 *              inverted equality
 *   FCVT       NaN gives the target's *maximum*, where x86 and ARM both
 *              give zero, and out-of-range saturates rather than wraps
 *   FSGNJ      operates on bits, so it has to work on a NaN
 *
 * A test built from finite, ordered, in-range values passes against an
 * implementation that gets all four wrong.
 */
#define F_NAN   0x7FC00000u
#define F_SNAN  0x7F800001u
#define F_ONE   0x3F800000u
#define F_TWO   0x40000000u
#define F_NEG1  0xBF800000u
#define F_PZERO 0x00000000u
#define F_NZERO 0x80000000u
#define F_PINF  0x7F800000u

static uint32_t fp_eval(emu_ir_op_t op, uint8_t aux, uint32_t x, uint32_t y)
{
    fake_cpu_t cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = x;
    cpu.f[2] = y;

    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, op, aux, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, 0u, r, EMU_IR_NO_TEMP, 3u, 0u);
    }
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, NULL);

    if (!emu_ir_interp(&g_b, (emu_cpu_t *)(void *)&cpu, &g_fake_target)) {
        return 0xDEADBEEFu;
    }
    return (uint32_t)cpu.f[3];
}

/*
 * The box, through the reference interpreter rather than through emitted
 * code.
 *
 * It matters that this is checked separately. ir_diff_ref compares the
 * compiled block against emu_ir_interp on the *same* IR, so a box the
 * reference also ignored would agree with a backend that ignored it --
 * two consumers of the same misreading, agreeing perfectly. That is the
 * shape of the pass_reg_traffic defect this project already has written
 * down: a checker that compares two consumers validates the consumers,
 * not the input.
 */
static void test_fp_box_reference(void)
{
    fake_cpu_t cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = UINT64_C(0x3FF0000000000000);      /* unboxed */
    cpu.f[2] = F_BOXED(F_TWO);

    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 2u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, EMU_IR_FP_BOX, a,
                          EMU_IR_NO_TEMP, 3u, 0u);
        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, EMU_IR_FP_BOX, b,
                          EMU_IR_NO_TEMP, 4u, 0u);
    }
    emu_ir_optimise(&g_b, &g_fake_target, EMU_IR_F_ALL, NULL);
    if (!emu_ir_interp(&g_b, (emu_cpu_t *)(void *)&cpu, &g_fake_target)) {
        CHECK(false);
        return;
    }
    CHECK_EQ64(cpu.f[3], F_BOXED(F_NAN));
    CHECK_EQ64(cpu.f[4], F_BOXED(F_TWO));
}

static void test_fp_semantics(void)
{
    /* Arithmetic, to establish the plumbing works at all. */
    CHECK_EQ(fp_eval(EMU_IR_FADD, 0u, F_ONE, F_ONE), F_TWO);
    CHECK_EQ(fp_eval(EMU_IR_FMUL, 0u, F_TWO, F_TWO), 0x40800000u);  /* 4 */
    CHECK_EQ(fp_eval(EMU_IR_FDIV, 0u, F_TWO, F_ONE), F_TWO);

    /* A NaN operand gives the *other* one; two NaNs give the canonical. */
    CHECK_EQ(fp_eval(EMU_IR_FMIN, 0u, F_NAN, F_ONE), F_ONE);
    CHECK_EQ(fp_eval(EMU_IR_FMIN, 0u, F_ONE, F_NAN), F_ONE);
    CHECK_EQ(fp_eval(EMU_IR_FMAX, 0u, F_NAN, F_ONE), F_ONE);
    CHECK_EQ(fp_eval(EMU_IR_FMAX, 0u, F_NAN, F_NAN), F_NAN);
    /* Signed zeros compare equal, and min must still prefer the negative. */
    CHECK_EQ(fp_eval(EMU_IR_FMIN, 0u, F_PZERO, F_NZERO), F_NZERO);
    CHECK_EQ(fp_eval(EMU_IR_FMAX, 0u, F_PZERO, F_NZERO), F_PZERO);

    /* Unordered is false for every comparison, "not equal" included. */
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_EQ, F_NAN, F_NAN), 0u);
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_LT, F_NAN, F_ONE), 0u);
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_LE, F_ONE, F_NAN), 0u);
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_EQ, F_ONE, F_ONE), 1u);
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_LT, F_NEG1, F_ONE), 1u);
    CHECK_EQ(fp_eval(EMU_IR_FCMP, EMU_IR_C_LE, F_ONE, F_ONE), 1u);

    /* Sign injection is bit manipulation, so a NaN goes through it. */
    CHECK_EQ(fp_eval(EMU_IR_FSGNJ, EMU_IR_FSGNJ_J, F_ONE, F_NEG1), F_NEG1);
    CHECK_EQ(fp_eval(EMU_IR_FSGNJ, EMU_IR_FSGNJ_N, F_ONE, F_NEG1), F_ONE);
    CHECK_EQ(fp_eval(EMU_IR_FSGNJ, EMU_IR_FSGNJ_X, F_NEG1, F_NEG1), F_ONE);
    CHECK_EQ(fp_eval(EMU_IR_FSGNJ, EMU_IR_FSGNJ_J, F_NAN, F_NEG1),
             F_NAN | 0x80000000u);

    /* NaN converts to the maximum, not to zero; range saturates. */
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, F_NAN, 0u),
             0x7FFFFFFFu);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I,
                     EMU_IR_FRM_RTZ | EMU_IR_F_UNSIGNED, F_NAN, 0u),
             0xFFFFFFFFu);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, F_PINF, 0u),
             0x7FFFFFFFu);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I,
                     EMU_IR_FRM_RTZ | EMU_IR_F_UNSIGNED, F_NEG1, 0u), 0u);

    /* Rounding: 2.5 and 3.5 are the pair that separates the modes. */
    const uint32_t f2h = 0x40200000u;      /* 2.5 */
    const uint32_t f3h = 0x40600000u;      /* 3.5 */
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RNE, f2h, 0u), 2u);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RNE, f3h, 0u), 4u);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RMM, f2h, 0u), 3u);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, f2h, 0u), 2u);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RUP, f2h, 0u), 3u);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RDN, f2h, 0u), 2u);

    CHECK_EQ(fp_eval(EMU_IR_FCVT_FROM_I, 0u, 0xFFFFFFFFu, 0u), F_NEG1);
    CHECK_EQ(fp_eval(EMU_IR_FCVT_FROM_I, EMU_IR_F_UNSIGNED, 1u, 0u), F_ONE);

    /* The classification bits. */
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_PINF, 0u), 1u << 7);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_PINF | 0x80000000u, 0u), 1u << 0);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_PZERO, 0u), 1u << 4);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_NZERO, 0u), 1u << 3);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_ONE, 0u), 1u << 6);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_NEG1, 0u), 1u << 1);
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_NAN, 0u), 1u << 9);   /* quiet   */
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, F_SNAN, 0u), 1u << 8);  /* signal  */
    CHECK_EQ(fp_eval(EMU_IR_FCLASS, 0u, 1u, 0u), 1u << 5);      /* subnorm */

    /* The reference declines square root rather than approximating it. */
    CHECK_EQ(fp_eval(EMU_IR_FSQRT, 0u, F_ONE, 0u), 0xDEADBEEFu);
}

#if defined(EMU_JIT_X86_64)
/*
 * The same FP cases as the reference, but *executed* -- the emitted SSE
 * is run and its answer compared against the semantics, which is the
 * only thing that catches a wrong ModRM or a mandatory prefix in the
 * wrong order relative to REX.
 *
 * Only the operations emu_ir_can_lower claims are checked here. The rest
 * are the frontend's problem, and asking for one would discard the block
 * rather than fail an assertion, which reads as a passing test.
 */
static uint32_t fp_run(emu_ir_op_t op, uint8_t aux, uint32_t x, uint32_t y)
{
    fake_cpu_t cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = x;
    cpu.f[2] = y;

    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, op, aux, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, 0u, r, EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) {
        return 0xDEADBEEFu;
    }
    return (uint32_t)cpu.f[3];
}

/*
 * The NaN convention, which is the whole of what separates a host FPU
 * from a guest one for the four exact operations.
 *
 * RISC-V has no NaN payloads: every operation producing a NaN produces
 * 0x7FC00000. x86 propagates an operand's payload and *quietens* a
 * signalling operand rather than replacing it, so each of these is a
 * different wrong answer without the canonicalisation -- and each was a
 * failure in the F suite with it removed.
 *
 * The awkward inputs are the point. An operand NaN with a payload, a
 * signalling NaN, and the invalid operations that manufacture one from
 * finite inputs: a test using ordinary numbers reaches none of them and
 * passes against every version of this.
 */
static void test_lower_fp_nan_canonical(void)
{
    /* A quiet NaN with a payload, which x86 would propagate verbatim. */
    const uint32_t payload = 0x7FC0BEEFu;
    /* A signalling NaN, which x86 quietens by setting bit 22 and keeps. */
    const uint32_t snan = 0x7F800001u;

    CHECK_EQ(fp_run(EMU_IR_FADD, 0u, payload, F_ONE), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FADD, 0u, F_ONE, payload), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FSUB, 0u, payload, F_ONE), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FMUL, 0u, payload, F_ONE), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FDIV, 0u, payload, F_ONE), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FSQRT, 0u, payload, 0u), F_NAN);

    CHECK_EQ(fp_run(EMU_IR_FADD, 0u, snan, F_ONE), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FMUL, 0u, snan, F_ONE), F_NAN);

    /* Invalid operations, where the NaN is made rather than propagated. */
    CHECK_EQ(fp_run(EMU_IR_FSUB, 0u, F_PINF, F_PINF), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FMUL, 0u, 0u, F_PINF), F_NAN);
    CHECK_EQ(fp_run(EMU_IR_FDIV, 0u, 0u, 0u), F_NAN);
    /*
     * The square root of a negative. x86's answer is the *real
     * indefinite* 0xFFC00000, which differs from what the guests want in
     * the sign bit alone -- one bit, and the only operation here where
     * the wrong answer is still a canonical-looking NaN.
     */
    CHECK_EQ(fp_run(EMU_IR_FSQRT, 0u, F_NEG1, 0u), F_NAN);

    /* And an ordinary result is not disturbed by the check. */
    CHECK_EQ(fp_run(EMU_IR_FADD, 0u, F_ONE, F_ONE), F_TWO);
}

/*
 * NaN boxing, EMU_IR_FP_BOX.
 *
 * Only meaningful because fake_cpu_t's file is 64 bits wide. The awkward
 * input is a register whose upper half is *not* all ones: reading it as
 * a single must give the canonical NaN rather than the bits that are
 * there, and nothing about the instruction doing the read looks wrong if
 * it does not -- the failure lands on whatever consumes the value.
 */
static void test_lower_fp_box(void)
{
    fake_cpu_t cpu;

    /* A boxed register reads as its low half. */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = F_BOXED(F_TWO);
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 1u, 0u);
        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, EMU_IR_FP_BOX, a,
                          EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) { CHECK(false); return; }
    /* The write boxes too, so the whole 64 bits are checked. */
    CHECK_EQ64(cpu.f[3], F_BOXED(F_TWO));

    /* An unboxed one reads as the canonical NaN, whatever it holds. */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = UINT64_C(0x3FF0000000000000);      /* 1.0 as a double */
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 1u, 0u);
        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, EMU_IR_FP_BOX, a,
                          EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) { CHECK(false); return; }
    CHECK_EQ64(cpu.f[3], F_BOXED(F_NAN));

    /*
     * Without the flag the same register reads raw, which is what FSW
     * and FMV.X.W need and is the reason this is per-instruction rather
     * than a property of the register file.
     */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = UINT64_C(0x3FF0000000000000);
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 1u, 0u);
        emu_ir_put(&g_b, 3u, a);
    }
    if (!lower_and_run(&cpu)) { CHECK(false); return; }
    CHECK_EQ(cpu.r[3], 0u);                       /* the low half, raw */

    /*
     * An unboxed *operand* reaching the arithmetic gives a canonical NaN
     * result, which is the case that made 87 of 378 architecture tests
     * fail when the box was dropped. Two boxed operands multiply
     * normally; one unboxed poisons the result.
     */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = F_BOXED(F_TWO);
    cpu.f[2] = (uint64_t)F_TWO;                   /* no box */
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, EMU_IR_FP_BOX,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, EMU_IR_FMUL, 0u, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, EMU_IR_FP_BOX, r,
                          EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) { CHECK(false); return; }
    CHECK_EQ64(cpu.f[3], F_BOXED(F_NAN));
}

static void test_lower_fp(void)
{
    CHECK_EQ(fp_run(EMU_IR_FADD, 0u, F_ONE, F_ONE), F_TWO);
    CHECK_EQ(fp_run(EMU_IR_FSUB, 0u, F_TWO, F_ONE), F_ONE);
    CHECK_EQ(fp_run(EMU_IR_FMUL, 0u, F_TWO, F_TWO), 0x40800000u);
    CHECK_EQ(fp_run(EMU_IR_FDIV, 0u, F_TWO, F_ONE), F_TWO);
    CHECK_EQ(fp_run(EMU_IR_FSQRT, 0u, 0x40800000u, 0u), F_TWO);  /* 4 -> 2 */

    /* Bit manipulation, so a NaN must pass through unquietened. */
    CHECK_EQ(fp_run(EMU_IR_FSGNJ, EMU_IR_FSGNJ_J, F_ONE, F_NEG1), F_NEG1);
    CHECK_EQ(fp_run(EMU_IR_FSGNJ, EMU_IR_FSGNJ_N, F_ONE, F_NEG1), F_ONE);
    CHECK_EQ(fp_run(EMU_IR_FSGNJ, EMU_IR_FSGNJ_X, F_NEG1, F_NEG1), F_ONE);
    CHECK_EQ(fp_run(EMU_IR_FSGNJ, EMU_IR_FSGNJ_J, F_NAN, F_NEG1),
             F_NAN | 0x80000000u);

    /*
     * Unordered is false for all three. Equality is the one that cannot
     * come from the operand-order trick, because a NaN sets ZF -- so
     * `sete` alone answers true, and only the parity flag says otherwise.
     */
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_EQ, F_ONE, F_ONE), 1u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_EQ, F_ONE, F_TWO), 0u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_EQ, F_NAN, F_NAN), 0u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_EQ, F_NAN, F_ONE), 0u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_LT, F_NEG1, F_ONE), 1u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_LT, F_ONE, F_NEG1), 0u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_LT, F_NAN, F_ONE), 0u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_LE, F_ONE, F_ONE), 1u);
    CHECK_EQ(fp_run(EMU_IR_FCMP, EMU_IR_C_LE, F_ONE, F_NAN), 0u);

    /*
     * The conversion fixup. x86 reports NaN, both infinities and every
     * out-of-range value as the one "integer indefinite" 0x80000000, and
     * the guests want the maximum for all of those except a large
     * negative -- so all four inputs below have to be separated by
     * re-examining the operand, and a lowering that trusted the
     * conversion gives 0x80000000 for three of them.
     */
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, F_NAN, 0u),
             0x7FFFFFFFu);
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, F_PINF, 0u),
             0x7FFFFFFFu);
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ,
                    F_PINF | 0x80000000u, 0u), 0x80000000u);
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, 0x40200000u, 0u), 2u);
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RTZ, 0xC0200000u, 0u),
             0xFFFFFFFEu);                              /* -2.5 -> -2 */
    /* The MXCSR the block sets is round-to-nearest, ties to even. */
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RNE, 0x40200000u, 0u), 2u);
    CHECK_EQ(fp_run(EMU_IR_FCVT_TO_I, EMU_IR_FRM_RNE, 0x40600000u, 0u), 4u);

    CHECK_EQ(fp_run(EMU_IR_FCVT_FROM_I, 0u, 0xFFFFFFFFu, 0u), F_NEG1);
    CHECK_EQ(fp_run(EMU_IR_FCVT_FROM_I, 0u, 2u, 0u), F_TWO);

    /* What this backend declines, it must decline consistently. */
    CHECK(!emu_ir_can_lower(EMU_IR_FMIN, 0u));
    CHECK(!emu_ir_can_lower(EMU_IR_FCLASS, 0u));
    CHECK(!emu_ir_can_lower(EMU_IR_FADD, EMU_IR_FRM_RMM));
    CHECK(!emu_ir_can_lower(EMU_IR_FCVT_TO_I,
                            EMU_IR_FRM_RTZ | EMU_IR_F_UNSIGNED));
    CHECK(emu_ir_can_lower(EMU_IR_FADD, EMU_IR_FRM_RNE));
}

/*
 * The flags a block accumulated reach the frontend once, at the exit --
 * and the sticky bits are *cleared* on entry, which is the half that a
 * test running one block cannot see. Two blocks are run: the first
 * raises inexact, the second raises nothing, and the second must report
 * nothing.
 */
static void test_lower_fp_flags(void)
{
    fake_cpu_t cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = F_ONE;
    cpu.f[2] = 0x40400000u;                 /* 3.0; 1/3 is inexact */

    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, EMU_IR_FDIV, 0u, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, 0u, r, EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK((cpu.fe & EMU_IR_FE_INEXACT) != 0u);
    CHECK((cpu.fe & EMU_IR_FE_INVALID) == 0u);

    /* Divide by zero, in its own block, reports itself. */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = F_ONE;
    cpu.f[2] = 0u;
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, EMU_IR_FDIV, 0u, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, 0u, r, EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK((cpu.fe & EMU_IR_FE_DIVBYZERO) != 0u);

    /*
     * And a block that raises nothing reports nothing, which only holds
     * because the prologue clears MXCSR's sticky bits. Without that it
     * would inherit whatever the two blocks above left.
     */
    memset(&cpu, 0, sizeof(cpu));
    cpu.f[1] = F_ONE;
    cpu.f[2] = F_ONE;
    emu_ir_reset(&g_b);
    {
        const uint16_t a = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t b = emu_ir_emit(&g_b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                       EMU_IR_NO_TEMP, 2u, 0u);
        const uint16_t r = emu_ir_emit(&g_b, EMU_IR_FADD, 0u, a, b, 0u, 0u);

        (void)emu_ir_emit(&g_b, EMU_IR_FPUT, 0u, r, EMU_IR_NO_TEMP, 3u, 0u);
    }
    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.f[3], F_TWO);
    CHECK_EQ(cpu.fe, 0u);
}
#endif /* EMU_JIT_X86_64 */

void test_ir(void)
{
    test_dead_flags_removed();
    test_flags_kept_when_read();
    test_flags_live_out();
    test_redundant_get_elided();
    test_dead_put_removed();
    test_put_kept_across_helper();
    test_put_kept_across_faulting_load();
    test_get_not_elided_across_helper();
    test_dead_values();
    test_flagless_frontend();
    test_use_counts();
    test_overflow_not_optimised();
    test_fp_semantics();
    test_fp_box_reference();
#if defined(EMU_JIT_X86_64)
    test_lower_add();
    test_lower_zero_register();
    test_zero_register_write_then_read();
    test_zero_register_not_forwarded();
    test_lower_bit_ops();
    test_lower_bit_counts();
    test_lower_flags();
    test_lower_flags_all_four();
    test_lower_flags_sign_not_less_than();
    test_interp_matches_jit();
    test_encode_rex();
    test_lower_fp();
    test_lower_fp_nan_canonical();
    test_lower_fp_box();
    test_lower_fp_flags();
    test_lower_memory();
    test_lower_memory_trap();
    test_lower_bitop_memory();
    test_lower_bitop_tst();
#endif
}
