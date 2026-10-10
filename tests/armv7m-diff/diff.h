/* SPDX-License-Identifier: Apache-2.0 */
/*
 * diff.h - what a generated test case and the harness agree on.
 *
 * The ARMv7-M frontend is checked against **a real Cortex-M7**, not
 * against another emulator: the same cases are built once as an emulator
 * guest and once as native firmware for the Nucleo-F746ZG, and the two
 * outputs are compared line by line. The board is the reference, which
 * makes the implementation-defined choices -- four priority bits, eight
 * MPU regions, FPv5-SP -- the silicon's rather than a reading of the
 * manual.
 *
 * Each case is a function taking a pointer to this state. It loads r0-r12
 * and APSR (and the FP registers when it uses them), runs its instruction,
 * and stores everything back. The offsets are fixed because the generated
 * assembly hard-codes them.
 */
#ifndef ARMV7M_DIFF_H
#define ARMV7M_DIFF_H

#include <stdint.h>

typedef struct {
    uint32_t r[13];  /* r0-r12; sp and pc are never operands */
    uint32_t apsr;   /* NZCVQ and GE, as MSR APSR_nzcvqg takes them */
    uint32_t s[32];  /* only when the case says it uses the FPU */
    uint32_t fpscr;
} diff_state_t;

#define DIFF_OFF_APSR 52
#define DIFF_OFF_S 56
#define DIFF_OFF_FPSCR 184

/*
 * Per case, from the generator: which registers hold addresses into
 * diff_mem afterwards (printed relative to it, because the two memory
 * maps differ), which window of diff_mem the case may touch (hashed
 * afterwards), and whether it uses the FPU.
 */
typedef struct {
    uint32_t addr_mask;
    uint32_t mem_lo;
    uint32_t mem_hi;
    uint32_t flags;
    uint32_t probe; /* a raw case: where its one instruction is */
} diff_meta_t;

#define DIFF_FLAG_FP 1u
/*
 * A raw case is an *encoding* rather than an instruction: four bytes the
 * assembler was told to emit, which the core may run, or refuse with
 * UNDEFINSTR or NOCP. The harness takes the fault, records what it was,
 * and resumes after the four bytes -- so "this encoding is UNDEFINED" is
 * a result like any other and is compared like any other.
 */
#define DIFF_FLAG_RAW 2u

#define DIFF_MEM_BYTES 16384u

extern uint8_t diff_mem[DIFF_MEM_BYTES];

typedef void (*diff_case_fn)(diff_state_t *);
extern const diff_case_fn diff_cases[];
extern const diff_meta_t diff_meta[];
extern const uint32_t diff_ncases;
extern const uint32_t diff_seed;

/* Supplied by the target: the emulator's UART, or the board's USART3. */
void diff_putc(char c);

/* Supplied by the target: the run is over and cannot continue. */
void diff_stop(void) __attribute__((noreturn));

#endif /* ARMV7M_DIFF_H */
