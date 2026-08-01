/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core_portme.h - CoreMark port configuration for an rv32cortex-m guest.
 *
 * Derived from CoreMark's barebones port. The differences that matter:
 *
 *   HAS_FLOAT 0     guest images link -nostdlib and RV32IMAC has no FPU,
 *                   so avoiding float avoids needing libgcc's soft-float
 *   HAS_TIME_H 0    no C library
 *   MEM_METHOD      MEM_STATIC, because there is no allocator
 *   CLOCKS_PER_SEC  the emulated CLINT runs mtime at 1 MHz
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

/* ---- compiler/platform capabilities ---- */
#define HAS_FLOAT   0
#define HAS_TIME_H  0
#define USE_CLOCK   0
#define HAS_STDIO   0
#define HAS_PRINTF  0

#define COMPILER_VERSION "GCC" __VERSION__
#define COMPILER_FLAGS   "-O2 -march=rv32imac"
#define MEM_LOCATION     "STATIC"

/* ---- data types ---- */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef float          ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef unsigned int   ee_size_t;

#define NULL ((void *)0)

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* ---- timing ---- */
/* The CLINT's mtime advances at 1 MHz on this platform. */
#define CLOCKS_PER_SEC    1000000
#define CORETIMETYPE      ee_u32
typedef ee_u32 CORE_TICKS;
typedef ee_u32 secs_ret;          /* HAS_FLOAT=0: whole seconds */

#define TIMER_RES_DIVIDER          1
#define SAMPLE_TIME_IMPLEMENTATION 1
#define EE_TICKS_PER_SEC           (CLOCKS_PER_SEC / TIMER_RES_DIVIDER)

/* ---- benchmark configuration ---- */
#define SEED_METHOD    SEED_VOLATILE
#define MEM_METHOD     MEM_STATIC
#define MULTITHREAD    1
#define MAIN_HAS_NOARGC 1
#define MAIN_HAS_NORETURN 0

/* Small-system data size, the value CoreMark documents for MCUs. */
#ifndef TOTAL_DATA_SIZE
#define TOTAL_DATA_SIZE 2000
#endif

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

/* Both implemented in core_portme.c; see the note there on why CoreMark's
 * own ee_printf.c is not used. */
int  ee_printf(const char *fmt, ...);
void uart_send_char(char c);

/* Declared here because coremark.h only declares it for MULTITHREAD > 1. */
extern ee_u32 default_num_contexts;

#endif /* CORE_PORTME_H */
