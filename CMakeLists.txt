# SPDX-License-Identifier: Apache-2.0
#
# rv32cortex-m - a retargetable 32-bit ISA emulator for ARM Cortex-M hosts.
#
# Three axes, independent of each other:
#
#   platform    where it runs        EMU_PLATFORM=host|stm32f446|stm32f746|stm32n6
#   frontend    what it emulates     EMU_GUEST_ARCH_RV32, EMU_GUEST_ARCH_G4MH
#   backend     how it executes      EMU_JIT=ON|OFF, per frontend
#
# The ISA-agnostic runtime (src/emu) owns the bus, the passthrough window,
# the console and the ELF loader; each frontend (src/frontend/<isa>) owns an
# instruction set and meets the runtime at emu_cpu_ops_t. See
# include/emu/emu_cpu.h for what adding a frontend involves.
#
#   cmake -B build/host -DRV32_PLATFORM=host
#   cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

cmake_minimum_required(VERSION 3.20)

project(rv32cortex-m
    VERSION 1.0.0
    DESCRIPTION "Retargetable 32-bit ISA emulator for ARM Cortex-M"
    LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # The interpreter's speed depends heavily on optimisation, and a debug
    # build of the emulator is rarely what anyone wants.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(EMU_PLATFORM "host" CACHE STRING
    "Target platform (host|stm32f446|stm32f746|stm32n6)")
set_property(CACHE EMU_PLATFORM PROPERTY STRINGS
    host stm32f446 stm32f746 stm32n6)

# ---------------------------------------------------------------------
# Frontends
# ---------------------------------------------------------------------
#
# Which guest architectures to build in. More than one may be selected; the
# host runner picks between them with --frontend or from the image's ELF
# header, and the first one listed here is the default for an image that
# says nothing -- a flat binary carries no architecture.
#
# **On the host every frontend is on**: one emu-host takes an RV32, G4MH,
# PowerPC or ARMv7-M guest without reconfiguring, and the frontends
# nobody had asked for are no longer the ones nothing exercises. A
# firmware builds exactly one, because there a frontend costs flash the
# guest could have had. Either default yields to an explicit -D, and a
# build directory configured before this keeps what its cache says --
# `cmake --fresh`, or name the four.
if(EMU_PLATFORM STREQUAL "host")
    set(_fe_default ON)
else()
    set(_fe_default OFF)
endif()

option(EMU_GUEST_ARCH_RV32 "RISC-V RV32 frontend" ON)
option(EMU_GUEST_ARCH_G4MH "Renesas RH850 G4MH frontend" ${_fe_default})
# NXP PowerPC e200z7 (Power ISA 2.06 embedded, VLE). **Big-endian**, the
# first of those in this tree: see the note by EMU_BUS_ORDER in emu_bus.h
# for what that costs and, more importantly, for which region kinds it
# does *not* apply to.
option(EMU_GUEST_ARCH_PPC "NXP PowerPC e200z7 frontend (VLE, big-endian)"
       ${_fe_default})
#
# ARMv7E-M as a *guest*, which is a different thing from the Thumb-2
# backend and exists for a different reason: the backend can only be
# validated on hardware today, and a Cortex-M guest lets an x86 host run
# the firmware that contains it. Where an instruction is not implemented
# it says so by faulting rather than by skipping.
#
option(EMU_GUEST_ARCH_ARMV7M "ARM Cortex-M frontend (ARMv7E-M, Thumb-2)"
       ${_fe_default})

#
# A list a new frontend falls off, which is why it is written as a count
# rather than as a chain of ANDs: adding ARMV7M to the options and
# forgetting this line rejects an ARMV7M-only build as having no
# frontend at all. This tree has the same shape recorded three times for
# `default:` cases that silently decline.
#
set(_frontend_count 0)
foreach(_fe RV32 G4MH PPC ARMV7M)
    if(EMU_GUEST_ARCH_${_fe})
        math(EXPR _frontend_count "${_frontend_count} + 1")
    endif()
endforeach()
if(_frontend_count EQUAL 0)
    message(FATAL_ERROR "at least one EMU_GUEST_ARCH_* must be enabled")
endif()

# ---------------------------------------------------------------------
# Runtime configuration (frontend-independent)
# ---------------------------------------------------------------------

# Regions in the bus table. Passthrough platforms need more than they first
# appear to: protecting individual peripheral registers splits a window.


option(EMU_ENABLE_TRACE "Per-instruction trace hook (slow)" OFF)
option(EMU_ENABLE_STATS "Collect run statistics" ON)

# ---------------------------------------------------------------------
# RV32 frontend configuration
# ---------------------------------------------------------------------

# Guest RAM size the guests are *linked* for: the link script's RAM
# region, so a guest whose .data and .bss do not fit is a link error;
# see tests/guest/guest.ld.in. It is a ceiling, not a guest's need: the
# boards give the guest whatever their firmware leaves, and start.S
# checks at run time that this guest fits what it was given. The F446
# leaves 33.7 KiB beside a 32 KB code cache, so its guests are linked
# for 32 and one that would not fit there fails to link.
if(EMU_PLATFORM STREQUAL "stm32f446")
    set(_guest_ram_kib_default "32")
else()
    set(_guest_ram_kib_default "48")
endif()
set(EMU_GUEST_RAM_KIB "${_guest_ram_kib_default}" CACHE STRING
    "Guest RAM size in KiB that guests are linked for")

#
# The guest's read-only window, in MiB, and a *link-time bound* rather
# than a mapping: the host maps whatever the image turns out to be and a
# board serves it from its own flash, so this only decides when the
# linker refuses. 16 covers every guest here but Quake, which carries an
# 18.7 MiB PAK in .rodata.
#
set(EMU_GUEST_ROM_MIB "16" CACHE STRING "Guest flash window in MiB")

# Link-time optimisation. The interpreter calls into rv_hart and rv_bus on
# every emulated instruction; without cross-TU inlining those stay real
# calls, and they are the hottest code in the project.
option(RV32_LTO "Link-time optimisation" ON)
# Where GCC cuts the LTO program into units. Empty is GCC's own choice,
# which follows the sizes of everything linked, so a change to one
# frontend can move another's code between units: `one` is what a host
# A/B builds both runners with, and nothing else does -- it links in
# 30 s rather than 5, and on the board it reorders .bss, which is a
# layout change (docs/performance.md, "One LTO partition, for A/Bs").
set(EMU_LTO_PARTITION "" CACHE STRING
    "GCC's -flto-partition= (one, balanced, ...); empty for GCC's own")
if(RV32_LTO AND NOT EMU_LTO_PARTITION STREQUAL ""
   AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_link_options(-flto-partition=${EMU_LTO_PARTITION})
endif()

#
# Fuzzing: tests/fuzz, libFuzzer targets, clang on the host only. The
# whole tree is built with ASan and UBSan and with libFuzzer's coverage
# instrumentation -- the emulator's own decoder compares are what steer
# the fuzzer towards valid encodings -- so this is a build directory of
# its own (scripts/fuzz.sh), never the one a figure is measured in.
# LTO is off because the instrumentation and clang's LTO need lld.
#
option(EMU_FUZZ "libFuzzer targets under ASan/UBSan (clang, host)" OFF)
if(EMU_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "EMU_FUZZ needs clang for libFuzzer: "
                            "configure with CC=clang")
    endif()
    if(NOT EMU_PLATFORM STREQUAL "host")
        message(FATAL_ERROR "EMU_FUZZ is for the host platform")
    endif()
    set(RV32_LTO OFF CACHE BOOL "Link-time optimisation" FORCE)
    # Not -fsanitize=function: it checks an indirect call by reading a
    # signature word ahead of the callee, and a translated block has
    # none -- the first call into the code buffer reads below its start.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                        -fno-sanitize=function
                        -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Run the interpreter loop from SRAM.
#
# Off by default because it was measured *slower* on the STM32F446: 162 vs
# 122 host cycles per guest instruction, while also taking 8 KiB from the
# guest. Executing from flash lets the Cortex-M4 fetch over the I-bus while
# data goes to SRAM over the D-bus, and the ART accelerator keeps a hot
# loop effectively wait-state free; moving code into SRAM puts fetch and
# data on the same interface and serialises them.
#
# Kept as an option because that trade-off is part-specific: it can pay off
# on a device with slower flash, no accelerator, or true TCM.
option(RV32_INTERP_IN_RAM "Place the interpreter loop in SRAM" OFF)

# Evaluate interrupt delivery only when something could have changed it.
#
# No measurable gain on the interrupt-free benchmark, and that is expected:
# with mstatus.MIE clear, rv_hart_pending_irq already returns after one
# load and one test, which the dirty flag cannot beat. It pays when a guest
# actually enables interrupts, where the eager check costs three loads.
# Left on because that is the realistic case and the cost otherwise is
# below the noise floor.
option(RV32_LAZY_IRQ "Lazy interrupt-delivery evaluation" ON)

# Bytes of RAM for the JIT code cache, taken from what the guest would get.
# Setting this very small is also how the compaction path gets exercised.
#
# **32 KB, measured, not guessed.** `bench` on the F746, one flash apart:
#
#            12 KB          32 KB
#   cycles   1,185,619,446  181,029,971     6.55x
#   ratio    930.24         142.03
#   xlat     10,192         850
#   evicted  10,188         821
#
# At 12 KB every translation evicted another -- the working set is 26,828
# bytes and the cache could not hold a fifth of it, so the board spent its
# time retranslating. It costs the guest 20 KiB of the F746's 264, which
# is the trade this knob has always been, and it is worth it by a factor
# of six.
#
# It also did nothing at all until this release: the value reached only
# the hand-written translator's RV_JIT_CODE_SIZE, and the IR backend that
# replaced it sized its static buffer from EMU_IR_JIT_STATIC_BYTES, which
# nothing set. -DEMU_JIT_CODE_BYTES=32768 reported `code 1728/12288` and
# figures identical to the digit.
#
# **The default follows the platform**, because what makes 32 KB right is
# scarcity rather than sufficiency. On a 320 KB part every one of those
# bytes is the guest's; the STM32N6 has 256 KB of AXISRAM2 sitting unused
# after .bss, and CoreMark on that board measures:
#
#              32 KB        128 KB
#   ratio      201.67       90.24   host cycles per guest instruction
#   KIPS       2975         6648
#   evicted    16969        136
#   xlat       17052        375
#
# 2.23x, with crcfinal 0xa69a either way -- the same answer, faster. The
# eviction count is the whole story: at 32 KB every translation threw
# another away.
#
# **The N6 takes 128 KB unless G4MH is compiled in**, in AXISRAM2 rather
# than ITCM, and the guest pays nothing: its RAM is AXISRAM1. Measured
# again with the dispatch path in ITCM, RV32 CoreMark x120, host cycles
# an instruction at 600 MHz:
#
#              32 KB ITCM   64 KB    96 KB    128 KB
#   ratio      175.46       110.05   36.60    20.21
#   xlat       63,462       35,336   6,885    301
#
# 8.7x, and at 128 KB the working set fits: two compactions in a run. A
# 650-iteration run takes 5.33 s by the host's clock against 5.30 s by
# the firmware's. The G4MH frontend's .bss does not leave room -- at
# 64 KB that build overflows AXISRAM2, and the part's other 1792 KB is
# unreachable because the boot ROM leaves AXISRAM3..6 powered down (see
# the note in stm32n657xx_lrun.ld) -- so a G4MH build keeps 32 KB in
# ITCM. Raising that means bringing those banks up, which is RCC and
# RAMCFG work rather than a linker change.
#
# It has to be decided here rather than in the platform's own
# CMakeLists, which is added later: by then this cache entry exists and a
# set() there is a no-op that reads exactly like a setting. Same trap as
# STM32CUBE_FAMILY having to precede the include.
#
# **The F746 takes 64 KB for RV32**, and 96 KB with PowerPC or ARMv7-M
# compiled in. It took 96 KB for everything first, when that was the
# smallest size RV32 CoreMark fitted: at 32 KB
# the run was translation-bound -- 63,733 translations, 70% of the time
# in the translator by a PC profile, and slower than the interpreter
# (152.7 against 107.4) -- and at 96 KB it was 32.5, 4.7x faster.
# docs/jit/tuning.md has both measurements. The Thumb-2 emitters have
# since taken a guest instruction from 60.8 bytes to 33.9, and at
# ce02bd0 RV32 CoreMark x120 is 14.55 at 64 KB against 14.59 at 96 KB
# (333 and 297 translations), with 193 KiB of guest RAM against 161.
#
# **PowerPC and ARMv7-M CoreMark do not fit 64 KB**, so they keep 96.
# PowerPC's is 148.35 host cycles an instruction at 64 KB against 53.63
# at 96, ARMv7-M's 106.41 against 34.7. Both cryptos fit either size
# (29.57 and 29.14, 32.39 and 31.3), so the choice is made for the
# CoreMarks: 32 KiB of guest RAM against a 2.8x and a 3.1x.
#
# **With G4MH compiled in, 64 KB on the F746 too and still 128 KB on the
# N6**, now that a board's G4MH cluster and local RAM default to 32 and
# 16 KiB (below) instead of 128 and 64. At the old sizes there was no
# room: a G4MH build kept 32 KB, and on G4MH CoreMark x40
# (scripts/g4mh-build-coremark.sh) that cache was a queue -- 22,369
# translations, and on the F746 a JIT barely ahead of the interpreter
# with 32 KiB left to the guest:
#
#                     cache   guest RAM   host cycles / insn   xlat
#   F746, 128/64 KiB  32 KB    32 KiB        92.38            22,369
#   F746,  32/16 KiB  64 KB   144 KiB        53.66               525
#   F746,  32/16 KiB  96 KB   112 KiB        54.75               455
#   N6,   128/64 KiB  32 KB  (1535 KiB)      93.96            23,023
#   N6,    32/16 KiB 128 KB  (1535 KiB)      50.59               458
#
# For G4MH more than 64 KB buys nothing and costs the guest 32 KiB a
# step, and its RAM is the tightest, so it decides when it is compiled
# in beside the others. The N6's guest RAM is elsewhere, so it takes
# the size RV32 does.
#
if(EMU_PLATFORM STREQUAL "stm32f746" AND EMU_GUEST_ARCH_G4MH)
    set(_jit_bytes_default "65536")
elseif(EMU_PLATFORM STREQUAL "stm32f746" AND
       (EMU_GUEST_ARCH_PPC OR EMU_GUEST_ARCH_ARMV7M))
    set(_jit_bytes_default "98304")
elseif(EMU_PLATFORM STREQUAL "stm32f746")
    set(_jit_bytes_default "65536")
elseif(EMU_PLATFORM STREQUAL "stm32n6")
    set(_jit_bytes_default "131072")
else()
    set(_jit_bytes_default "32768")
endif()
set(EMU_JIT_CODE_BYTES "${_jit_bytes_default}" CACHE STRING
    "JIT code cache size in bytes")

#
# Which linker section the JIT's code buffer lands in.
#
# Empty means "an ordinary .bss array", which is what a host and a part
# without a tightly-coupled memory both want. A platform whose linker
# script provides somewhere better names it here.
#
# **It has to be set at this level, because this is where emu_ir_jit.c is
# compiled.** Setting it on the platform's own executable target reaches
# nothing: the file is in `emucore`, so the define never arrives and the
# buffer stays in .bss -- which links, runs, and reports figures
# identical to the digit. Exactly the shape of EMU_JIT_CODE_BYTES
# reaching only the translator that no longer existed, and caught the
# same way, by reading `nm` for the symbol's address rather than
# believing the build.
#
#
# The N6 has 64 KiB of ITCM that nothing else uses, so the translator's
# output buffer goes there: it is the only memory this core fetches from
# without crossing a bus, and .jitcode is NOLOAD so the image does not
# grow by a byte.
#
# Worth 1.36% on CoreMark, which is small and is *measured* rather than
# argued -- 2,261,843/2,261,576 ticks with it off against
# 2,231,496/2,230,417 with it on, same crcfinal 0xa69a and the same
# 6,270,489 retired. The board's counter is repeatable to about 0.05%
# between runs of one binary, which is what makes a 1.36% difference
# readable at all; see CLAUDE.md on layout noise before quoting a
# smaller one.
#
# It needs board_itcm_init's ECC scrub to work at all. See
# docs/stm32n6/itcm.md.
#
# **Only where the buffer fits**: 32 KB or less, which no default asks
# for any more -- a G4MH build's was the last. A 128 KB buffer cannot be
# in 64 KiB of ITCM, and is worth 8.7x where ITCM was worth 1.36%, so the
# larger buffer goes to AXISRAM2 and ITCM keeps the dispatch path.
#
if(EMU_PLATFORM STREQUAL "stm32n6" AND EMU_JIT_CODE_BYTES LESS_EQUAL 32768)
    set(_jit_section_default ".jitcode")
else()
    set(_jit_section_default "")
endif()
set(EMU_JIT_CODE_SECTION "${_jit_section_default}" CACHE STRING
    "linker section for the JIT code buffer (empty: .bss)")

#
# And the same question for the emulator's own per-dispatch code.
#
# Separate from the buffer on purpose: they compete for the same 64 KiB
# of ITCM, and which of them is worth the space is a measurement rather
# than a principle. This file already records interpreter-in-SRAM being
# built on a sound argument and measured *worse*, so both are options
# and neither is assumed.
#
# The per-dispatch path joins it: emu_jit_run and the two IR memory
# helpers, 1256 bytes, which with the 32 KiB buffer is 34,024 of the
# 64 KiB.
#
# The F746 too: its 16 KiB of ITCM held only the flash driver, and the
# dispatch path there is 1.6% on CoreMark with the figure unmoved across
# five .text layouts (150.33-150.42) -- see docs/jit/tuning.md. Its
# buffer stays in SRAM: at 64 KB it would be the whole 64 KiB DTCM,
# which holds other things.
#
if(EMU_PLATFORM STREQUAL "stm32n6" OR EMU_PLATFORM STREQUAL "stm32f746")
    set(_hot_section_default ".itcm")
else()
    set(_hot_section_default "")
endif()
set(EMU_HOT_TEXT_SECTION "${_hot_section_default}" CACHE STRING
    "linker section for the per-dispatch path (empty: ordinary .text)")

#
# And an interpreter's run loop, which is a third question with its own
# answer on each part. ARMv7-M's interpreted CoreMark on the F746 went
# 161.9-172.9 host cycles an instruction to 143.6-144.3 with it in ITCM
# -- 8.6 KB, twice the part's 4 KB I-cache, fetched from flash -- and the
# spread across three function alignments from 11 cycles to under 1. On
# the N6, whose M55 has a 32 KB I-cache, the same move was 4.8% *slower*
# at every alignment, so it is the F746's alone. RV32's loop (-6.6%) and
# G4MH's (up to -5.1%) are there too; PowerPC's is not, because it calls
# into flash for every instruction and was 2-5% slower -- see
# docs/performance.md. Each frontend that opts in measures its own.
#
if(EMU_PLATFORM STREQUAL "stm32f746")
    set(_interp_section_default ".itcm")
else()
    set(_interp_section_default "")
endif()
set(EMU_INTERP_TEXT_SECTION "${_interp_section_default}" CACHE STRING
    "linker section for an interpreter's run loop (empty: ordinary .text)")

#
# How many times a pc must miss the block cache before it is translated;
# until then it is interpreted. 1 translates on the first miss, which is
# the default because nothing measured gains from more -- see
# docs/jit/tuning.md before raising it. `--jit-hot N` sets it per run on
# the host.
#
set(EMU_JIT_HOT_THRESHOLD 1 CACHE STRING
    "misses before a pc is translated (1: translate on the first)")

# Build and use the Thumb-2 JIT. Turning this off is how the same guest
# image is measured on the interpreter, which is also how a suspected JIT
# miscompile gets isolated from a core bug.
# The Thumb-2 JIT is built from the IR, and only from the IR: the
# hand-written translator it used to share this slot with has been
# removed. It failed the architecture suite -- I-nop-00, 2374
# instructions, passes on the interpreter and on the IR path and ran away
# emitting garbage under the hand-written one -- and maintaining two
# translators for one host is how a fix lands in the one nobody is
# running. The option is kept as a no-op so an old build directory with
# it cached still configures.
set(EMU_JIT_IR ON CACHE INTERNAL "Thumb-2 JIT is IR-based (no longer optional)" FORCE)
option(EMU_JIT_DIFF
    "Check every translated block against the IR interpreter" OFF)
#
# The interpreter, by default, on the F446 with G4MH compiled in -- the
# one pairing where the JIT is the wrong default. The part has 128 KiB,
# a JIT build spends some 77 KiB before the guest gets a byte, and what
# is left after G4MH's cluster and local RAM fits a 12 KB cache at most:
# G4MH CoreMark x40 there ran at 204.4 host cycles an instruction with
# 65,142 evictions and 28 KiB for the guest, against 143.6 and 62 KiB
# interpreted. With the JIT on, a default build did not link at all.
#
if(EMU_PLATFORM STREQUAL "stm32f446" AND EMU_GUEST_ARCH_G4MH)
    set(_jit_default OFF)
else()
    set(_jit_default ON)
endif()
option(EMU_JIT "Thumb-2 JIT backend" ${_jit_default})

# EMU_JIT_LOOP_CHAIN, EMU_JIT_INLINE_PERIPH, EMU_JIT_ELIDE_LD and
# EMU_JIT_ELIDE_ST were options here. Each configured the hand-written
# Thumb-2 translator, each was still declared, forwarded as a compile
# definition and defaulted in rv_config.h -- and **read by nothing**
# since the IR backend replaced that translator. Setting one changed no
# byte of the build. They are gone rather than kept as no-ops: a knob
# that reads like a setting and is not one is how a measurement comes to
# compare two identical binaries. What they did, and what survived as
# behaviour rather than as an option, is in docs/jit/tuning.md.
set(EMU_JIT_LOOP_CAP "128" CACHE STRING
    "Guest instructions a chained loop runs before returning to the dispatcher")

# Histogram how often a block reads sp/ra/a0/a1, to decide whether a
# per-block register cache could pay. Measurement scaffolding, off by default.
option(EMU_JIT_HOT_REG_STATS "Count hot-register reads per block" OFF)

# Emitted bytes per IR operation, summed over every block translated and
# printed in the run report -- on a board as well as a host, because code
# size is what the board's cache pays for. Measurement scaffolding, off
# by default; see include/emu/emu_ir_bytes.h.
option(EMU_JIT_BYTE_STATS "Count emitted bytes per IR operation" OFF)

# Histogram adjacent executed instruction pairs, to decide which are worth
# fusing in the translator. Measurement scaffolding, off by default.
#
# Not RV32_: the histogram is in src/emu/ and knows nothing about an
# instruction set -- a frontend supplies seven small functions and gets it.
# The two frontends without a reference model are exactly the ones where
# "what does this guest actually execute" is hardest to answer any other
# way, so tying this to one of them was backwards.
option(EMU_PAIR_STATS "Count adjacent executed instruction pairs" OFF)

# Berkeley SoftFloat is the F extension's arithmetic. Not an option.
#
# It is the implementation the RISC-V FP spec was written against: its
# rounding modes and exception flags are numerically identical to frm and
# fflags, and it ships a RISCV specialization with the right canonical-NaN
# behaviour. The host-FPU path via <fenv.h> is smaller and faster and gets
# the wrong answer -- the flags it can report are the ones the hardware
# happens to raise, which are not RISC-V's rules on the fused
# multiply-adds or around subnormals.
#
# Measured on the official suite, interpreter, rv32i/F: SoftFloat 78/78,
# host FPU 26/78. That is not a tuning knob, so it is not offered as one.
# It was an option defaulting to OFF, which meant every default build had
# an FP unit that failed two thirds of the architecture tests -- and the
# documented way to turn it on named a variable that does not exist
# (EMU_FPU_SOFTFLOAT, against the real RV32_FPU_SOFTFLOAT), so setting it
# changed nothing and said nothing.
set(SOFTFLOAT_DIR "${CMAKE_BINARY_DIR}/softfloat" CACHE PATH "Berkeley SoftFloat source")

option(RV32_EXT_SDTRIG "Sdtrig debug triggers" ON)
option(RV32_EXT_PMP "Physical memory protection" ON)

#
# Misaligned loads and stores, emulated by splitting rather than raising.
#
# **The C library needs it.** picolibc's strcmp and friends are
# word-at-a-time with no alignment preamble -- they read `lw` straight
# off the pointer -- so any guest that links them and compares an
# odd-addressed string takes a load-address-misaligned trap inside the
# library. Quake does it on its first file lookup; DOOM happened not to,
# which is the only reason this was not found sooner.
#
# ON by default for that reason: a guest built with this toolchain is
# far more likely to need it than to be testing a core that traps. The
# RISC-V spec permits either, and embedded cores usually raise -- so
# turn it OFF to model one of those.
#
option(RV32_MISALIGNED "Emulate misaligned load/store instead of trapping" ON)
option(RV32_EXT_ZCB "Zcb compressed instructions" ON)
#
# The bit-manipulation set. Declared here and not only in rv_config.h,
# because two places were reading them and only one of them existed:
# rv32_frontend.c builds RV32_ISA_STRING from the C macros and got
# "RV32IMAFCB_zbc" right, while the configure summary below tested CMake
# variables of the same name that nothing declared -- so it printed
# "RV32IMAFC" and understated the core it had just built. misa says B.
#
# B is the union of Zba, Zbb and Zbs, which is what the 2024 ratification
# folded into it; Zbc was moved out before ratification and is separate.
option(RV32_EXT_ZBA "Zba address generation" ON)
option(RV32_EXT_ZBB "Zbb basic bit manipulation" ON)
option(RV32_EXT_ZBC "Zbc carry-less multiply" ON)
option(RV32_EXT_ZBS "Zbs single-bit operations" ON)
option(RV32_EXT_F "Single-precision floating point" ON)
# Double precision. FLEN becomes 64 and single values are NaN-boxed, so
# this changes what *F* reads and writes as well as adding instructions
# -- see the note on rv_hart_t.f.
option(RV32_EXT_D "Double-precision floating point (requires F)" ON)
option(RV32_EXT_M "Integer multiply/divide" ON)
option(RV32_EXT_A "Atomics" ON)
option(RV32_EXT_C "Compressed instructions" ON)
option(RV32_ENABLE_DISASM "Build the RV32 disassembler" ON)

# ---------------------------------------------------------------------
# G4MH frontend configuration
# ---------------------------------------------------------------------

# The G4MH FPU is an option on real parts, and single precision only here.
#
# On by default because the arithmetic is Berkeley SoftFloat, which the
# tree builds anyway for the RISC-V frontend, so the marginal cost is the
# decode and the semantics rather than a library. Turning it off puts the
# whole encoding range back on RIE, which is what a part without the
# option does -- see src/frontend/g4mh/g4mh_fpu.c for what "single
# precision only" excludes.
option(G4MH_EXT_FPU "G4MH single-precision FPU" ON)

# The memory protection unit. On by default, unlike the FPU: a real G4MH
# reports 32 entries in MPCFG and does not omit it, and a guest that never
# sets MPM.MPE pays one predicted branch per fetch and per data access.
option(G4MH_EXT_MPU "G4MH memory protection unit" ON)

# MPU entries. 32 is what MPCFG.NMPUE reports and the register is
# read-only, so a smaller value makes the emulator disagree with itself
# unless it is what the part has -- it exists for a build where those
# 96 words are the guest's.
set(G4MH_MPU_ENTRIES "32" CACHE STRING "G4MH MPU entries (1..32)")

option(G4MH_ENABLE_DISASM "Build the G4MH disassembler" ON)
option(ARMV7M_ENABLE_DISASM "Build the ARMv7-M disassembler" ON)
option(PPC_ENABLE_DISASM "Build the e200z7 disassembler" ON)

# e200z7 cores to model. The MPC5675K has two, and in its decoupled
# parallel mode core 1 is held in reset until core 0 writes SSCM_DPMBOOT
# and the two SSCM_DPMKEY keys (Reference Manual, 5.6, in docs/nxp), so a
# guest that never does runs on core 0 alone and pays only a core skipped
# each round. A held core costs a small structure and a bus, so unlike
# G4MH's PEs -- 64 KiB of local RAM each -- this is not a host-only
# number. See docs/frontend/ppc.md.
set(PPC_CORE_COUNT "2" CACHE STRING "e200z7 cores (1..2)")

# Processing elements to model. The RH850/U2B6 has three; the default is 1
# because the constraint is memory -- 64 KiB of local RAM per PE does not
# fit on an F446 -- so multicore is a host-platform feature.
# See docs/host/g4mh/multicore.md.
set(G4MH_PE_COUNT "1" CACHE STRING "G4MH processing elements (1..3)")

# Regions in the bus table, per core.
#
# 16 was enough for one core and is *not* enough for a multicore G4MH: each
# bus carries ram, rom, periph, uart, intc1-self, intc1-pe0..n-1, intc2,
# intif, ostm, barrier, ipir, tptm, lram-self and lram-pe0..n-1, which
# passes 16 at two PEs and 20 at three. docs/host/g4mh/multicore.md said
# "raise it to 24" when the design was written and nobody did, so
# -DG4MH_PE_COUNT=3 failed at start-up with "could not bring up 3 g4mh
# cores" -- a message about cores, for a full region table.
#
# The default therefore follows the PE count rather than being a constant
# somebody has to remember to change with it. Still a cache variable, so a
# firmware build that knows its map can set it lower and get the RAM back.
#
# **The platform's carve-outs and the frontend's devices are added, not
# chosen between.** This was an if/elseif chain, so an N6 build with the
# G4MH frontend took the N6 branch and got a table sized for RV32's five
# devices -- and failed at start-up with "could not bring the cores up",
# a message about cores, for a full region table. Third time. It needed
# 30 where it had 24, and the two terms below are what those 30 are: 15
# for the N6's policy and 15 for G4MH with one PE.
#
if(EMU_PLATFORM STREQUAL "stm32n6")
    set(_regions_platform 16)
else()
    set(_regions_platform 10)
endif()
if(EMU_GUEST_ARCH_G4MH)
    # Its shared devices, plus the INTC and timers each PE gets. The
    # shared ones include the four KCRC units, the three OS timers and
    # the three watchdogs.
    math(EXPR _regions_frontend "22 + 4 * ${G4MH_PE_COUNT}")
else()
    set(_regions_frontend 6)
endif()
math(EXPR _regions_default "${_regions_platform} + ${_regions_frontend}")
set(EMU_MAX_REGIONS "${_regions_default}" CACHE STRING "Bus region table size")

# How much of the U2B6's memory map this build actually backs.
#
# The architectural sizes -- 3 MiB of code flash, 384 KiB of cluster RAM,
# 64 KiB of local RAM per PE -- are 3.44 MiB, which is .bss the frontend
# allocated unconditionally and which no microcontroller has. Sizing them
# here is the same tension EMU_JIT_CODE_BYTES has: on a target every byte
# is a byte the guest does not get.
#
# Flash is the one that is not a trade-off. It is where code is *executed
# from*, so a platform serves the guest image out of its own flash through
# g4mh_set_flash() and pays nothing -- which is what the RV32 side already
# does with its ROM region. G4MH_FLASH_KIB only sizes the fallback arena
# the host runner needs, because its ELF loader writes the image into
# guest memory; firmware sets it to 0 and the arena does not exist.
if(EMU_PLATFORM STREQUAL "host")
    set(_g4mh_flash_kib 256)
else()
    set(_g4mh_flash_kib 0)
endif()
set(G4MH_FLASH_KIB "${_g4mh_flash_kib}" CACHE STRING
    "G4MH code-flash arena, KiB (0 = the platform supplies it)")
#
# Cluster and local RAM are .bss too, and on a board the defaults are 32
# and 16 KiB -- what the F446's G4MH notes already used -- not the 128
# and 64 a host backs. At those sizes a G4MH build could not link on the
# F446 at all, kept 32 KiB of guest RAM and a 32 KB code cache on the
# F746, and overflowed AXISRAM2 on the N6 with anything larger; see
# EMU_JIT_CODE_BYTES above for what the room bought. What it costs is
# room for a U2B6 program linked to keep its data there: every guest in
# this tree links its data into guest RAM.
#
if(EMU_PLATFORM STREQUAL "host")
    set(_g4mh_cram_kib 128)
    set(_g4mh_lram_kib 64)
else()
    set(_g4mh_cram_kib 32)
    set(_g4mh_lram_kib 16)
endif()
set(G4MH_CRAM_KIB "${_g4mh_cram_kib}" CACHE STRING "G4MH cluster RAM backed, KiB")
set(G4MH_LRAM_KIB "${_g4mh_lram_kib}" CACHE STRING "G4MH local RAM per PE backed, KiB")

# ---------------------------------------------------------------------
# Runtime library: portable C11, no platform and no ISA dependencies
# ---------------------------------------------------------------------
#
# Named emucore, not rv32core: it holds the ISA-agnostic runtime plus
# whichever frontends were selected above, and has held two of them since
# G4MH landed. The old name was kept for a while so existing build
# directories would not have to change, which is exactly the kind of
# convenience that leaves a library called after one of the things inside
# it. (The *project* is still rv32cortex-m; renaming that would move the
# repository.)

add_library(emucore STATIC
    src/emu/emu_bus.c
    src/emu/emu_jit.c
    src/emu/emu_lock.c
    src/emu/emu_ir.c
    src/emu/emu_gdb.c
    src/emu/emu_pairstats.c
    # Self-guarding on EMU_JIT_HOT_REG_STATS, as emu_pairstats.c is on
    # EMU_PAIR_STATS: listed always, compiling to nothing when off.
    src/emu/emu_regstats.c
    # Self-guarding on EMU_JIT_BYTE_STATS, likewise.
    src/emu/emu_ir_bytes.c
    # The IR JIT framework and its two host backends. Every one of these
    # guards itself on the host it emits for, so all of them can stay in
    # the list and exactly one backend compiles to anything. They belong
    # here rather than under a frontend: emu_ir_jit.c defines *both*
    # frontends' backends from one macro, so putting it under
    # EMU_GUEST_ARCH_RV32 left g4mh_backend_jit undefined in a G4MH-only
    # tree -- which the G4MH frontend then referenced unconditionally.
    src/backend/x86_64/encode.c
    src/backend/common/interp.c
    src/backend/x86_64/ir_lower.c
    src/backend/thumb2/encode.c
    src/backend/thumb2/ir_lower.c
    src/emu/emu_ir_jit.c
    src/emu/emu_cpu.c
    src/emu/emu_elf.c
    src/emu/emu_uart.c
    src/emu/emu_finisher.c
    src/emu/emu_fb.c
    src/emu/emu_input.c
)

#
# virtio-mmio, over TinyEMU's implementation.
#
# **Host only, and off elsewhere by construction.** virtio walks the
# guest's descriptor rings through host pointers and TinyEMU's 9p
# backend calls openat, statfs and friends -- neither is available on a
# microcontroller, and neither would fit in its flash if it were.
#
# The vendored sources are compiled with warnings relaxed: they are not
# this project's code to tidy, and warnings nobody will act on train
# people to ignore the ones that matter. The glue beside them is built
# with the project's usual flags, because that part *is* ours.
#
option(EMU_VIRTIO "virtio-mmio devices (host only)" ON)

#
# **Asked for, and possible, are two different things, and the macro has
# to mean the second.**
#
# EMU_HAVE_VIRTIO used to be defined from the option, which defaults ON
# for every platform, while the sources below are compiled only for the
# host. So every board firmware compiled emu_main.c's `#if
# EMU_HAVE_VIRTIO` block and then failed to link against six functions
# nothing provided -- stm32f446, stm32f746 and stm32n6 alike, from the
# commit that attached virtio to the host until this one. Nobody had
# built a board with the default options in between.
#
# This is the same reconciliation rv_config.h does for the JIT, and it
# is here for the same reason recorded in CLAUDE.md: a capability that
# depends on nobody selecting it is not a capability that is off.
#
if(EMU_VIRTIO AND EMU_PLATFORM STREQUAL "host")
    set(EMU_VIRTIO_BUILT ON)
else()
    set(EMU_VIRTIO_BUILT OFF)
    # Say so, rather than silently doing less than was asked. Only when
    # it was actually requested: the default being ON, an unqualified
    # note would fire on every board build and say nothing.
    if(EMU_VIRTIO)
        message(STATUS
            "virtio: not built for platform '${EMU_PLATFORM}' (host only)")
    endif()
endif()

if(EMU_VIRTIO_BUILT)
    # virtio.c is compiled through virtio_tinyemu.c, which includes it
    # unmodified and completes the block requests it drops -- see there.
    target_sources(emucore PRIVATE
        src/emu/virtio/virtio_tinyemu.c
        third_party/tinyemu/fs.c
        third_party/tinyemu/fs_disk.c
        src/emu/virtio/virtio_glue.c
        src/emu/virtio/virtio_nopci.c
        src/emu/virtio/emu_virtio.c)

    target_include_directories(emucore PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyemu
        ${CMAKE_CURRENT_SOURCE_DIR}/src/emu/virtio)

    set_source_files_properties(
        third_party/tinyemu/fs.c
        third_party/tinyemu/fs_disk.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_definitions(emucore PUBLIC
    EMU_HAVE_VIRTIO=$<BOOL:${EMU_VIRTIO_BUILT}>
    EMU_JIT_LOOP_CAP=${EMU_JIT_LOOP_CAP}u)

target_include_directories(emucore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_definitions(emucore PUBLIC
    EMU_GUEST_ARCH_RV32=$<BOOL:${EMU_GUEST_ARCH_RV32}>
    EMU_GUEST_ARCH_G4MH=$<BOOL:${EMU_GUEST_ARCH_G4MH}>
    EMU_GUEST_ARCH_PPC=$<BOOL:${EMU_GUEST_ARCH_PPC}>
    EMU_GUEST_ARCH_ARMV7M=$<BOOL:${EMU_GUEST_ARCH_ARMV7M}>
    EMU_MAX_REGIONS=${EMU_MAX_REGIONS}
    EMU_ENABLE_TRACE=$<BOOL:${EMU_ENABLE_TRACE}>
    EMU_ENABLE_STATS=$<BOOL:${EMU_ENABLE_STATS}>
    EMU_PAIR_STATS=$<BOOL:${EMU_PAIR_STATS}>
    EMU_JIT_HOT_REG_STATS=$<BOOL:${EMU_JIT_HOT_REG_STATS}>
    EMU_JIT_BYTE_STATS=$<BOOL:${EMU_JIT_BYTE_STATS}>
    #
    # The IR backend's static code cache.
    #
    # This was missing, and EMU_JIT_CODE_BYTES -- which BUILD.md and
    # CLAUDE.md both describe as dominating JIT performance on the board
    # -- reached only the hand-written translator's RV_JIT_CODE_SIZE.
    # Since the IR backend replaced it, setting the option changed
    # nothing: -DEMU_JIT_CODE_BYTES=32768 still reported `code 1728/12288`
    # and produced figures identical to the digit.
    EMU_IR_JIT_STATIC_BYTES=${EMU_JIT_CODE_BYTES}u
    #
    # And where it lives. Quoted twice on purpose: the C source uses the
    # macro as a string literal in __attribute__((section(...))), so the
    # quotes have to survive CMake.
    #
    $<$<BOOL:${EMU_JIT_CODE_SECTION}>:EMU_JIT_CODE_SECTION="${EMU_JIT_CODE_SECTION}">
    $<$<BOOL:${EMU_HOT_TEXT_SECTION}>:EMU_HOT_TEXT_SECTION="${EMU_HOT_TEXT_SECTION}">
    $<$<BOOL:${EMU_INTERP_TEXT_SECTION}>:EMU_INTERP_TEXT_SECTION="${EMU_INTERP_TEXT_SECTION}">
    EMU_JIT_HOT_THRESHOLD=${EMU_JIT_HOT_THRESHOLD}u
    #
    # Whether a JIT was *asked for*. emu_jit.h reconciles this with what
    # the host can actually emit; before it existed, EMU_HOST_JIT_X86_64 and
    # EMU_HOST_JIT_THUMB2 were derived from the host alone, so -DEMU_JIT=OFF
    # still compiled the JIT sources -- and on both hosts that meant
    # referring to types the same option had removed. Two whole
    # configurations did not build and nothing said so.
    #
    EMU_JIT_REQUESTED=$<BOOL:${EMU_JIT}>
)

# ---------------------------------------------------------------------
# Frontend: RISC-V RV32
# ---------------------------------------------------------------------

if(EMU_GUEST_ARCH_RV32)
    target_sources(emucore PRIVATE
        src/frontend/rv32/rv32_frontend.c
        src/frontend/rv32/rv_aplic.c
        src/frontend/rv32/rv_clint.c
        src/frontend/rv32/rv_csr.c
        src/frontend/rv32/rv_decode.c
        src/frontend/rv32/rv_disasm.c
        src/frontend/rv32/rv_fpu.c
        src/frontend/rv32/rv_hart.c
        src/frontend/rv32/rv_interp.c
        src/frontend/rv32/rv_pmp.c
        src/frontend/rv32/rv_mmu.c
        src/frontend/rv32/rv_dtlb.c
        src/frontend/rv32/rv_trig.c
        src/frontend/rv32/rv_gdb.c
    )
    #
    # The IR translator is JIT-only: it calls emu_ir_can_lower, which the
    # backend lowering files define, and with EMU_JIT=OFF none of those
    # compile to anything. Listing it unconditionally left an undefined
    # reference in a configuration nobody built.
    #
    if(EMU_JIT)
        target_sources(emucore PRIVATE src/frontend/rv32/rv_ir.c)
    endif()

    # rv_fpu.c was built -ffp-contract=off here, for a 2Product/2Sum
    # error term that GCC's default contraction fused into a VFMA on the
    # Cortex-M7 and so computed as zero. That arithmetic went when the FP
    # unit became SoftFloat; the flag stayed, with a comment describing
    # code that was no longer in the file. Compiled with and without it --
    # on x86-64, on x86-64 with -mfma, and for the M7 -- the object code
    # is identical instruction for instruction, with no fused multiply in
    # any of them. An error-free transformation written in host floating
    # point needs the flag back; nothing here is one.

    target_compile_definitions(emucore PUBLIC
        RV_EXT_F=$<BOOL:${RV32_EXT_F}>
        RV_EXT_D=$<BOOL:${RV32_EXT_D}>
        RV_EXT_ZCB=$<BOOL:${RV32_EXT_ZCB}>
        RV_EXT_ZBA=$<BOOL:${RV32_EXT_ZBA}>
        RV_EXT_ZBB=$<BOOL:${RV32_EXT_ZBB}>
        RV_EXT_ZBC=$<BOOL:${RV32_EXT_ZBC}>
        RV_EXT_ZBS=$<BOOL:${RV32_EXT_ZBS}>
        RV_EXT_PMP=$<BOOL:${RV32_EXT_PMP}>
        RV_MISALIGNED_OK=$<BOOL:${RV32_MISALIGNED}>
        RV_EXT_SDTRIG=$<BOOL:${RV32_EXT_SDTRIG}>
        RV_FPU_SOFTFLOAT=1
        RV_EXT_M=$<BOOL:${RV32_EXT_M}>
        RV_EXT_A=$<BOOL:${RV32_EXT_A}>
        RV_EXT_C=$<BOOL:${RV32_EXT_C}>
        RV_ENABLE_DISASM=$<BOOL:${RV32_ENABLE_DISASM}>
        RV_INTERP_RAMFUNC=$<BOOL:${RV32_INTERP_IN_RAM}>
        RV_LAZY_IRQ_CHECK=$<BOOL:${RV32_LAZY_IRQ}>
        RV_JIT_CODE_SIZE=${EMU_JIT_CODE_BYTES}u
        $<$<BOOL:${EMU_JIT_DIFF}>:EMU_JIT_DIFF>
        RV_JIT_HOT_REG_STATS=$<BOOL:${EMU_JIT_HOT_REG_STATS}>
    )

    if(EMU_PAIR_STATS)
        target_sources(emucore PRIVATE src/frontend/rv32/rv_pairstats.c)
    endif()
endif()

# ---------------------------------------------------------------------
# Frontend: NXP PowerPC e200z7
# ---------------------------------------------------------------------

if(EMU_GUEST_ARCH_PPC)
    target_sources(emucore PRIVATE
        src/frontend/ppc/ppc_cpu.c
        src/frontend/ppc/ppc_decode.c
        src/frontend/ppc/ppc_disasm.c
        src/frontend/ppc/ppc_fpu.c
        src/frontend/ppc/ppc_frontend.c
        src/frontend/ppc/ppc_gdb.c
        src/frontend/ppc/ppc_interp.c
        src/frontend/ppc/ppc_intc.c
        src/frontend/ppc/ppc_mmu.c
        src/frontend/ppc/ppc_sscm.c
        src/frontend/ppc/ppc_stm.c
        src/frontend/ppc/ppc_swt.c
        $<$<BOOL:${EMU_PAIR_STATS}>:src/frontend/ppc/ppc_pairstats.c>
    )

    # JIT-only, as for the other three.
    if(EMU_JIT)
        target_sources(emucore PRIVATE src/frontend/ppc/ppc_ir.c)
    endif()

    target_compile_definitions(emucore PUBLIC
        PPC_ENABLE_DISASM=$<BOOL:${PPC_ENABLE_DISASM}>
        PPC_CORE_COUNT=${PPC_CORE_COUNT}u
    )
endif()

# ---------------------------------------------------------------------
# Frontend: ARM Cortex-M (ARMv7E-M)
# ---------------------------------------------------------------------

if(EMU_GUEST_ARCH_ARMV7M)
    target_sources(emucore PRIVATE
        $<$<BOOL:${EMU_PAIR_STATS}>:src/frontend/armv7m/armv7m_pairstats.c>
        src/frontend/armv7m/armv7m_frontend.c
        src/frontend/armv7m/armv7m_gdb.c
        src/frontend/armv7m/armv7m_interp.c
        src/frontend/armv7m/armv7m_decode.c
        src/frontend/armv7m/armv7m_disasm.c
        src/frontend/armv7m/armv7m_exc.c
        src/frontend/armv7m/armv7m_fpu.c
        src/frontend/armv7m/armv7m_nvic.c
    )

    # JIT-only, as for RV32 and G4MH.
    if(EMU_JIT)
        target_sources(emucore PRIVATE src/frontend/armv7m/armv7m_ir.c)
    endif()

    target_compile_definitions(emucore PUBLIC
        ARMV7M_ENABLE_DISASM=$<BOOL:${ARMV7M_ENABLE_DISASM}>
    )
endif()

# ---------------------------------------------------------------------
# Frontend: Renesas RH850 G4MH
# ---------------------------------------------------------------------

if(EMU_GUEST_ARCH_G4MH)
    target_sources(emucore PRIVATE
        $<$<BOOL:${EMU_PAIR_STATS}>:src/frontend/g4mh/g4mh_pairstats.c>
        src/frontend/g4mh/g4mh_barrier.c
        src/frontend/g4mh/g4mh_cpu.c
        src/frontend/g4mh/g4mh_decode.c
        src/frontend/g4mh/g4mh_disasm.c
        src/frontend/g4mh/g4mh_frontend.c
        src/frontend/g4mh/g4mh_gdb.c
        src/frontend/g4mh/g4mh_boot.c
        src/frontend/g4mh/g4mh_intc.c
        src/frontend/g4mh/g4mh_mpu.c
        src/frontend/g4mh/g4mh_ipir.c
        src/frontend/g4mh/g4mh_interp.c
        src/frontend/g4mh/g4mh_fpu.c
        src/frontend/g4mh/g4mh_tptm.c
        src/frontend/g4mh/g4mh_ltsc.c
        src/frontend/g4mh/g4mh_kcrc.c
        src/frontend/g4mh/g4mh_ostm.c
        src/frontend/g4mh/g4mh_wdtb.c
    )
    # JIT-only, as for RV32 above.
    if(EMU_JIT)
        target_sources(emucore PRIVATE src/frontend/g4mh/g4mh_ir.c)
    endif()

    target_compile_definitions(emucore PUBLIC
        G4MH_ENABLE_DISASM=$<BOOL:${G4MH_ENABLE_DISASM}>
        G4MH_PE_COUNT=${G4MH_PE_COUNT}u
        G4MH_EXT_FPU=$<BOOL:${G4MH_EXT_FPU}>
        G4MH_EXT_MPU=$<BOOL:${G4MH_EXT_MPU}>
        G4MH_MPU_ENTRIES=${G4MH_MPU_ENTRIES}u
        G4MH_FLASH_KIB=${G4MH_FLASH_KIB}u
        G4MH_CRAM_KIB=${G4MH_CRAM_KIB}u
        G4MH_LRAM_KIB=${G4MH_LRAM_KIB}u
    )
endif()

# ARMv7-M's FPv5-SP is the third FP unit built on it, and the reason the
# half-precision conversions are listed: VCVTB/VCVTT.
if((EMU_GUEST_ARCH_RV32 AND RV32_EXT_F) OR (EMU_GUEST_ARCH_G4MH AND G4MH_EXT_FPU)
   OR EMU_GUEST_ARCH_ARMV7M OR EMU_GUEST_ARCH_PPC)
    if(NOT EXISTS "${SOFTFLOAT_DIR}/source/include/softfloat.h")
        find_package(Git QUIET)
        if(GIT_FOUND)
            message(STATUS "Fetching Berkeley SoftFloat into ${SOFTFLOAT_DIR}")
            execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 -q
                https://github.com/ucb-bar/berkeley-softfloat-3.git
                "${SOFTFLOAT_DIR}")
        endif()
    endif()
    # Fail here rather than later. SoftFloat *is* the FP unit now, so a
    # missing checkout is a broken build and not a reason to quietly fall
    # back to something that fails two thirds of the F suite. Point
    # SOFTFLOAT_DIR at a checkout when building offline.
    if(NOT EXISTS "${SOFTFLOAT_DIR}/source/include/softfloat.h")
        message(FATAL_ERROR
            "Berkeley SoftFloat not found in ${SOFTFLOAT_DIR} and could not "
            "be fetched. It is required for RV32_EXT_F. Either clone "
            "https://github.com/ucb-bar/berkeley-softfloat-3.git there, "
            "point -DSOFTFLOAT_DIR=<path> at a checkout, or build with "
            "-DRV32_EXT_F=OFF.")
    endif()

    # Only the single-precision entry points and the primitives they need.
    # Pulling in the whole library would drag f64, f128 and extF80 along for
    # an extension this core does not implement.
    set(_sf_names
        f32_add f32_sub f32_mul f32_div f32_sqrt f32_mulAdd
        f32_eq f32_le f32_lt f32_eq_signaling f32_le_quiet f32_lt_quiet
        f32_to_i32 f32_to_ui32 f32_to_i32_r_minMag f32_to_ui32_r_minMag
        f32_roundToInt f32_isSignalingNaN
        i32_to_f32 ui32_to_f32
        s_addMagsF32 s_subMagsF32 s_mulAddF32
        s_normSubnormalF32Sig s_roundPackToF32 s_normRoundPackToF32
        s_shiftRightJam32 s_shiftRightJam64
        s_countLeadingZeros8 s_countLeadingZeros32
        s_approxRecip_1Ks s_approxRecipSqrt_1Ks s_approxRecipSqrt32_1
        s_approxRecip32_1 s_countLeadingZeros64 s_roundToI32 s_roundToUI32
        s_shortShiftRightJam64
        softfloat_state softfloat_raiseFlags)

    #
    # Double precision, and only where a frontend implements it.
    #
    # Named rather than globbed, for the reason the comment above gives:
    # source/ also holds the f128 and extF80 implementations, which want
    # the opposite SOFTFLOAT_FAST_INT64 setting and do not compile here.
    # The 128-bit primitives below are not f128 -- they are what the
    # f64 path uses for its intermediate products.
    #
    #
    # Double precision, wanted by either frontend that has it: RV32's D
    # and G4MH's .D operations are the same SoftFloat entry points.
    #
    #
    # Half precision, converted and never computed: ARMv7-M's VCVTB/VCVTT
    # and RV32's Zfhmin, which rv_config.h turns on with F.
    #
    set(_need_f16 OFF)
    if(EMU_GUEST_ARCH_ARMV7M)
        set(_need_f16 ON)
    endif()
    if(EMU_GUEST_ARCH_RV32 AND RV32_EXT_F)
        set(_need_f16 ON)
    endif()
    if(_need_f16)
        list(APPEND _sf_names f32_to_f16 f16_to_f32 s_roundPackToF16
                              s_normSubnormalF16Sig s_countLeadingZeros16)
    endif()
    set(_need_f64 OFF)
    if(EMU_GUEST_ARCH_G4MH AND G4MH_EXT_FPU)
        set(_need_f64 ON)
    endif()
    if(EMU_GUEST_ARCH_RV32 AND RV32_EXT_F AND RV32_EXT_D)
        set(_need_f64 ON)
    endif()
    # The e200's EFPU2 is single precision, and computes in double: that
    # is where its guard and sticky bits come from. See ppc_fpu.c.
    if(EMU_GUEST_ARCH_PPC)
        set(_need_f64 ON)
    endif()
    if(_need_f64)
        list(APPEND _sf_names
            f64_add f64_sub f64_mul f64_div f64_sqrt f64_mulAdd
            f64_le f64_lt s_mulAddF64
            f64_eq f64_lt_quiet
            f64_to_i32 f64_to_ui32 f64_to_i64 f64_to_ui64
            i32_to_f64 ui32_to_f64 i64_to_f64 ui64_to_f64
            f32_to_f64 f64_to_f32
            f32_to_i64 f32_to_ui64 i64_to_f32 ui64_to_f32
            s_addMagsF64 s_subMagsF64
            s_normSubnormalF64Sig s_roundPackToF64 s_normRoundPackToF64
            s_roundToI64 s_roundToUI64
            s_shiftRightJam64Extra s_shortShiftRightJam64Extra
            s_add128 s_sub128 s_mul64To128 s_shortShiftLeft128
            s_shiftRightJam128 s_shortShiftRightJam128
            s_shiftRightJam128Extra s_shortShiftRightJam128Extra
            s_le128 s_lt128)
    endif()
    # Zfhmin's FCVT.D.H and FCVT.H.D.
    if(_need_f16 AND _need_f64)
        list(APPEND _sf_names f16_to_f64 f64_to_f16)
    endif()

    set(_sf_src "")
    foreach(_n IN LISTS _sf_names)
        foreach(_d "source" "source/RISCV")
            if(EXISTS "${SOFTFLOAT_DIR}/${_d}/${_n}.c")
                list(APPEND _sf_src "${SOFTFLOAT_DIR}/${_d}/${_n}.c")
            endif()
        endforeach()
    endforeach()
    # Only the f32 parts of the RISCV specialization. Globbing the whole
    # directory drags in the f128M and extF80 variants, which need the
    # opposite SOFTFLOAT_FAST_INT64 setting and do not compile here.
    set(_sf_riscv softfloat_raiseFlags s_f32UIToCommonNaN s_commonNaNToF32UI
                  s_propagateNaNF32UI)
    if(_need_f64)
        list(APPEND _sf_riscv s_f64UIToCommonNaN s_commonNaNToF64UI
                              s_propagateNaNF64UI)
    endif()
    if(_need_f16)
        list(APPEND _sf_riscv s_f16UIToCommonNaN s_commonNaNToF16UI)
    endif()
    foreach(_n IN LISTS _sf_riscv)
        if(EXISTS "${SOFTFLOAT_DIR}/source/RISCV/${_n}.c")
            list(APPEND _sf_src "${SOFTFLOAT_DIR}/source/RISCV/${_n}.c")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _sf_src)

    target_sources(emucore PRIVATE ${_sf_src})
    target_include_directories(emucore PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/rv32/softfloat"
        "${SOFTFLOAT_DIR}/source/include"
        "${SOFTFLOAT_DIR}/source/RISCV")
    # Third-party code, not warning-clean under our flags.
    set_source_files_properties(${_sf_src} PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_options(emucore PRIVATE
    -Wall -Wextra -Wshadow -Wundef
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fomit-frame-pointer>
)

if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET emucore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ---------------------------------------------------------------------
# Guest images (built with the RISC-V toolchain, on any host)
# ---------------------------------------------------------------------
#
# Before the platform, not after. The firmware embeds one of these and
# asks `if(TARGET guest-${RV32_GUEST})` to find out whether it exists --
# and if(TARGET) only sees targets already defined, so with this
# directory added afterwards the answer was always no. The firmware
# linked correctly anyway, because guest_image.S carries an OBJECT_DEPENDS
# on the staged files, and the only visible effect was a configure-time
# warning saying the link would fail when it would not.
#
# A warning that is always wrong is worse than none: it is the one that
# teaches you to skip reading them.
add_subdirectory(tests/guest)

# The e200z7 guest, which needs a different toolchain and so has its own
# file -- tests/guest/CMakeLists.txt returns early without the RISC-V one
# and would skip this for an unrelated reason.
if(EMU_GUEST_ARCH_PPC)
    add_subdirectory(tests/guest/ppc)
endif()

# ---------------------------------------------------------------------
# Platform
# ---------------------------------------------------------------------

# FreeRTOS, on every platform: the runner is a task, and so is each
# further core of a multicore guest. Not an option -- one execution model
# is what keeps a board and a runner comparable. See cmake/freertos.cmake.
include(cmake/freertos.cmake)

if(EMU_PLATFORM STREQUAL "host")
    add_subdirectory(src/platform/host)
elseif(EMU_PLATFORM MATCHES "^stm32(f446|f746|n6)$")
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "EMU_PLATFORM=${EMU_PLATFORM} needs the cross toolchain:\n"
            "  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake")
    endif()
    #
    # Where a board stops a guest that never halts. 0 never stops it,
    # which is what a run long enough to cross the cycle counter's wrap
    # needs: CoreMark at 650 iterations is 162M instructions.
    #
    set(EMU_MAX_INSN 100000000 CACHE STRING
        "board: instructions before a guest is stopped (0: never)")
    add_compile_definitions(EMU_MAX_INSN=${EMU_MAX_INSN}u)
    add_subdirectory(src/platform/${EMU_PLATFORM})
else()
    message(FATAL_ERROR "unknown EMU_PLATFORM '${EMU_PLATFORM}'")
endif()

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

if(EMU_PLATFORM STREQUAL "host")
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests/unit)
    endif()
    include(cmake/validation.cmake)
    if(EMU_FUZZ)
        add_subdirectory(tests/fuzz)
    endif()
endif()

#
# g4mh-sweep: what the G4MH frontend cannot name, counted over a real
# image. Built only where the frontend is, since it links its decoder.
#
# It answers the question a mnemonic-by-mnemonic sweep does not: not
# whether an encoding exists, but whether the compiler emits it -- and
# it orders the answer by how often. Read the *slots*, not the total:
# a flat image is code and data together, so a gap is a sub-opcode that
# recurs thousands of times, and noise is scattered singletons.
#
if(EMU_PLATFORM STREQUAL "host" AND EMU_GUEST_ARCH_G4MH)
    add_executable(g4mh-sweep tests/tools/g4mh_sweep.c
                   src/frontend/g4mh/g4mh_disasm.c
                   src/frontend/g4mh/g4mh_decode.c)
    target_include_directories(g4mh-sweep PRIVATE include)
    target_compile_definitions(g4mh-sweep PRIVATE
                               EMU_MAX_REGIONS=${EMU_MAX_REGIONS})
endif()

#
# ppc-dis: the e200z7 decoder and disassembler, for tests/ppc-check, which
# assembles what it prints and compares the decode of the result.
#
if(EMU_PLATFORM STREQUAL "host" AND EMU_GUEST_ARCH_PPC)
    add_executable(ppc-dis tests/tools/ppc_dis.c
                   src/frontend/ppc/ppc_disasm.c
                   src/frontend/ppc/ppc_decode.c)
    target_include_directories(ppc-dis PRIVATE include)

    # ppc-fpu: the embedded FP unit, an operation per line, for
    # tests/ppc-check/efpu.py and its exact-arithmetic model.
    add_executable(ppc-fpu tests/tools/ppc_fpu.c)
    target_link_libraries(ppc-fpu PRIVATE emucore)
endif()

#
# armv7m-dis: the ARMv7-M disassembler over the code ranges of an image.
# tests/armv7m-diff/disasm-check.py drives it and then assembles what it
# printed, which is the disassembler's test.
#
if(EMU_PLATFORM STREQUAL "host" AND EMU_GUEST_ARCH_ARMV7M)
    add_executable(armv7m-dis tests/tools/armv7m_dis.c
                   src/frontend/armv7m/armv7m_disasm.c
                   src/frontend/armv7m/armv7m_decode.c)
    target_include_directories(armv7m-dis PRIVATE include)
endif()

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------

# Built up as a plain string: message() does not evaluate generator
# expressions, so $<BOOL:...> would be printed verbatim.
set(_frontends "")
if(EMU_GUEST_ARCH_RV32)
    set(_isa "RV32I")
    foreach(_ext M A F D C B)
        if(RV32_EXT_${_ext})
            string(APPEND _isa "${_ext}")
        endif()
    endforeach()
    # B is the union of Zba, Zbb and Zbs rather than an option of its own,
    # and misa advertises it on exactly that basis, so the banner has to
    # agree. It did not: RV32_EXT_ZBA and friends were read here and
    # declared nowhere, so the condition was always false and this line
    # was dead.
    if(RV32_EXT_ZBA AND RV32_EXT_ZBB AND RV32_EXT_ZBS)
        string(APPEND _isa "B")
    endif()
    #
    # The multi-letter extensions, in canonical order -- which is the
    # order RV32_ISA_STRING uses in rv32_frontend.c, and that is the
    # thing to be equal to.
    #
    # **Two descriptions of one core, neither generated from the
    # other.** They have disagreed three times: this summary advertised
    # B from a condition that was always false, then advertised Zicsr,
    # Zifencei and Zicntr while the run-time banner did not, and then
    # both stopped there with four more extensions implemented. "Kept in
    # step by hand" was the arrangement each time. The `rv32-isa-string`
    # test below runs a guest and requires the banner to be this string,
    # so a change to one that misses the other fails a test instead of
    # waiting to be noticed.
    #
    # Most of these have no CMake option: rv_config.h defaults them on
    # and nothing here declares them, so they are unconditional for the
    # same reason they are there. Zalasr and Zawrs follow A, as their
    # gates do; Zacas is A's too.
    #
    # Zic64b is RV_CACHE_BLOCK_SIZE being 64, which has no option either.
    string(APPEND _isa "_zic64b_zicbom_zicbop_zicboz_zicntr_zicond_zicsr")
    string(APPEND _isa "_zifencei")
    string(APPEND _isa "_zihintntl_zihintpause_zihpm")
    if(RV32_EXT_A)
        string(APPEND _isa "_zacas_zalasr_zawrs")
    endif()
    # Zfhmin follows F, as rv_config.h has it.
    if(RV32_EXT_F)
        string(APPEND _isa "_zfhmin")
    endif()
    if(RV32_EXT_C AND RV32_EXT_ZCB)
        string(APPEND _isa "_zcb")
    endif()
    if(RV32_EXT_ZBC)
        string(APPEND _isa "_zbc")
    endif()
    # Zkt is a promise about timing with no option; rv_config.h makes it.
    string(APPEND _isa "_zkt")
    # Sscofpmf and Svinval follow S and Zihpm, which have no option:
    # rv_config.h turns them on.
    string(APPEND _isa "_sscofpmf_svinval")
    if(BUILD_TESTING AND TARGET emu-host AND TARGET guest-hello)
        add_test(NAME rv32-isa-string
                 COMMAND emu-host "${CMAKE_BINARY_DIR}/guest/hello.bin")
        set_tests_properties(rv32-isa-string PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "emu: ${_isa} on ")
    endif()
    # U and S are privilege modes rather than instruction-set
    # extensions, so they are reported here and deliberately absent from
    # the ISA string the banner prints.
    #
    # Unconditionally. This tested RV32_EXT_U and RV32_EXT_S, which
    # nothing declares -- both modes are rv_config.h defaults with no
    # CMake option -- so the summary had never once printed either.
    string(APPEND _isa " +U +S")
    list(APPEND _frontends "rv32 (${_isa})")
endif()
if(EMU_GUEST_ARCH_G4MH)
    list(APPEND _frontends "g4mh (RH850 G4MH, ${G4MH_PE_COUNT} PE)")
endif()
# Each of the four, or the summary under-reports what the runner accepts:
# it listed two until the host built all four by default.
if(EMU_GUEST_ARCH_PPC)
    if(PPC_CORE_COUNT EQUAL 1)
        set(_ppc_cores "1 core")
    else()
        set(_ppc_cores "${PPC_CORE_COUNT} cores")
    endif()
    list(APPEND _frontends "ppc (e200z7, Book E and VLE, big-endian, ${_ppc_cores})")
endif()
if(EMU_GUEST_ARCH_ARMV7M)
    list(APPEND _frontends "armv7m (ARMv7E-M, Thumb-2)")
endif()

message(STATUS "")
message(STATUS "rv32cortex-m ${PROJECT_VERSION}")
message(STATUS "  platform    ${EMU_PLATFORM}")
message(STATUS "  build type  ${CMAKE_BUILD_TYPE}")
foreach(_f IN LISTS _frontends)
    message(STATUS "  frontend    ${_f}")
endforeach()
message(STATUS "")
