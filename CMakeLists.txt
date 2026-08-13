# SPDX-License-Identifier: Apache-2.0
#
# rv32cortex-m - a retargetable 32-bit ISA emulator for ARM Cortex-M hosts.
#
# Three axes, independent of each other:
#
#   platform    where it runs        EMU_PLATFORM=host|stm32f446
#   frontend    what it emulates     EMU_FRONTEND_RV32, EMU_FRONTEND_G4MH
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

set(EMU_PLATFORM "host" CACHE STRING "Target platform (host|stm32f446|stm32f746)")
set_property(CACHE EMU_PLATFORM PROPERTY STRINGS host stm32f446 stm32f746)

# ---------------------------------------------------------------------
# Frontends
# ---------------------------------------------------------------------
#
# Which guest architectures to build in. More than one may be selected; the
# host runner picks between them with --frontend or from the image's ELF
# header, and the first one listed here is the default.
#
# A frontend costs flash only when it is on, so the firmware normally
# builds exactly one.

option(EMU_FRONTEND_RV32 "RISC-V RV32 frontend" ON)
option(EMU_FRONTEND_G4MH "Renesas RH850 G4MH frontend" OFF)

if(NOT EMU_FRONTEND_RV32 AND NOT EMU_FRONTEND_G4MH)
    message(FATAL_ERROR "at least one EMU_FRONTEND_* must be enabled")
endif()

# ---------------------------------------------------------------------
# Runtime configuration (frontend-independent)
# ---------------------------------------------------------------------

# Regions in the bus table. Passthrough platforms need more than they first
# appear to: protecting individual peripheral registers splits a window.
set(EMU_MAX_REGIONS "16" CACHE STRING "Bus region table size")

option(EMU_ENABLE_TRACE "Per-instruction trace hook (slow)" OFF)
option(EMU_ENABLE_STATS "Collect run statistics" ON)

# ---------------------------------------------------------------------
# RV32 frontend configuration
# ---------------------------------------------------------------------

# Guest RAM size. Shared between the guest link script and the firmware's
# static carve-out so the two can never disagree; see tests/guest/guest.ld.in.
set(EMU_GUEST_RAM_KIB "48" CACHE STRING "Guest RAM size in KiB")

# Link-time optimisation. The interpreter calls into rv_hart and rv_bus on
# every emulated instruction; without cross-TU inlining those stay real
# calls, and they are the hottest code in the project.
option(RV32_LTO "Link-time optimisation" ON)

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
set(EMU_JIT_CODE_BYTES "12288" CACHE STRING "JIT code cache size in bytes")

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
option(EMU_JIT "Thumb-2 JIT backend" ON)

# Chain loop back edges inside translated code: 35% on bench, 7% on CoreMark.
option(EMU_JIT_LOOP_CHAIN "Chain loop back edges in the JIT" ON)
option(EMU_JIT_INLINE_PERIPH "Inline the peripheral window in the JIT" ON)
# Both measured neutral at 48 KB and a large regression at 12 KB; see
# the note in include/rv32/rv_config.h before turning either on.
option(EMU_JIT_ELIDE_LD "Elide the reload of a value already in R1" OFF)
option(EMU_JIT_ELIDE_ST "Elide a store the next instruction makes dead" OFF)
set(EMU_JIT_LOOP_CAP "128" CACHE STRING
    "Guest instructions a chained loop runs before returning to the dispatcher")

# Histogram how often a block reads sp/ra/a0/a1, to decide whether a
# per-block register cache could pay. Measurement scaffolding, off by default.
option(EMU_JIT_HOT_REG_STATS "Count hot-register reads per block" OFF)

# Histogram adjacent executed instruction pairs, to decide which are worth
# fusing in the translator. Measurement scaffolding, off by default.
option(RV32_PAIR_STATS "Count adjacent executed instruction pairs" OFF)

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
option(RV32_EXT_ZCB "Zcb compressed instructions" ON)
option(RV32_EXT_F "Single-precision floating point" ON)
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

option(G4MH_ENABLE_DISASM "Build the G4MH disassembler" ON)

# Processing elements to model. The RH850/U2B6 has three; the default is 1
# because the constraint is memory -- 64 KiB of local RAM per PE does not
# fit on an F446 -- so multicore is a host-platform feature.
# See docs/host/g4mh/multicore.md.
set(G4MH_PE_COUNT "1" CACHE STRING "G4MH processing elements (1..3)")

# ---------------------------------------------------------------------
# Runtime library: portable C11, no platform and no ISA dependencies
# ---------------------------------------------------------------------
#
# The target keeps the name rv32core so existing build directories and
# scripts do not have to change. What is in it is the ISA-agnostic runtime
# plus whichever frontends were selected above.

add_library(rv32core STATIC
    src/emu/emu_bus.c
    src/emu/emu_jit.c
    src/emu/emu_ir.c
    src/emu/emu_gdb.c
    src/frontend/rv32/rv_gdb.c
    # The IR JIT framework and its two host backends. Every one of these
    # guards itself on the host it emits for, so all of them can stay in
    # the list and exactly one backend compiles to anything. They belong
    # here rather than under a frontend: emu_ir_jit.c defines *both*
    # frontends' backends from one macro, so putting it under
    # EMU_FRONTEND_RV32 left g4mh_backend_jit undefined in a G4MH-only
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
)

target_include_directories(rv32core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_definitions(rv32core PUBLIC
    EMU_FRONTEND_RV32=$<BOOL:${EMU_FRONTEND_RV32}>
    EMU_FRONTEND_G4MH=$<BOOL:${EMU_FRONTEND_G4MH}>
    EMU_MAX_REGIONS=${EMU_MAX_REGIONS}
    EMU_ENABLE_TRACE=$<BOOL:${EMU_ENABLE_TRACE}>
    EMU_ENABLE_STATS=$<BOOL:${EMU_ENABLE_STATS}>
)

# ---------------------------------------------------------------------
# Frontend: RISC-V RV32
# ---------------------------------------------------------------------

if(EMU_FRONTEND_RV32)
    target_sources(rv32core PRIVATE
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
        src/frontend/rv32/rv_trig.c
        src/frontend/rv32/rv_ir.c
    )

    # The fused multiply-adds recover their rounding error with
    # 2Product/2Sum, which is only exact if `a * b - p` is evaluated as
    # written. GCC's default -ffp-contract=fast fuses exactly that into a
    # single VFMA wherever the host has one, so the error term computes as
    # zero and the FMA rounds twice instead of once. It is silent: the
    # answer is merely a little wrong, and only on the host with hardware
    # FMA -- which is why the same guest binary gave one answer on x86-64
    # and another on the Cortex-M7, in the one kernel that emitted an
    # FMADD. Confirmed by finding eleven VFMA/VFNMS in rv_hart_fp.
    set_source_files_properties(src/frontend/rv32/rv_fpu.c
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

    target_compile_definitions(rv32core PUBLIC
        RV_EXT_F=$<BOOL:${RV32_EXT_F}>
        RV_EXT_ZCB=$<BOOL:${RV32_EXT_ZCB}>
        RV_EXT_PMP=$<BOOL:${RV32_EXT_PMP}>
        RV_EXT_SDTRIG=$<BOOL:${RV32_EXT_SDTRIG}>
        RV_FPU_SOFTFLOAT=1
        RV_EXT_M=$<BOOL:${RV32_EXT_M}>
        RV_EXT_A=$<BOOL:${RV32_EXT_A}>
        RV_EXT_C=$<BOOL:${RV32_EXT_C}>
        RV_ENABLE_DISASM=$<BOOL:${RV32_ENABLE_DISASM}>
        RV_INTERP_RAMFUNC=$<BOOL:${RV32_INTERP_IN_RAM}>
        RV_LAZY_IRQ_CHECK=$<BOOL:${RV32_LAZY_IRQ}>
        RV_JIT_CODE_SIZE=${EMU_JIT_CODE_BYTES}u
        RV_ENABLE_JIT=$<BOOL:${EMU_JIT}>
        EMU_IR_JIT_ON_THUMB2
        $<$<BOOL:${EMU_JIT_DIFF}>:EMU_JIT_DIFF>
        RV_JIT_LOOP_CHAIN=$<BOOL:${EMU_JIT_LOOP_CHAIN}>
        RV_JIT_INLINE_PERIPH=$<BOOL:${EMU_JIT_INLINE_PERIPH}>
        RV_JIT_ELIDE_LD=$<BOOL:${EMU_JIT_ELIDE_LD}>
        RV_JIT_ELIDE_ST=$<BOOL:${EMU_JIT_ELIDE_ST}>
        RV_JIT_LOOP_CAP=${EMU_JIT_LOOP_CAP}u
        RV_JIT_HOT_REG_STATS=$<BOOL:${EMU_JIT_HOT_REG_STATS}>
        RV_PAIR_STATS=$<BOOL:${RV32_PAIR_STATS}>
    )

    if(RV32_PAIR_STATS)
        target_sources(rv32core PRIVATE src/frontend/rv32/rv_pairstats.c)
    endif()
endif()

# ---------------------------------------------------------------------
# Frontend: Renesas RH850 G4MH
# ---------------------------------------------------------------------

if(EMU_FRONTEND_G4MH)
    target_sources(rv32core PRIVATE
        src/frontend/g4mh/g4mh_cpu.c
        src/frontend/g4mh/g4mh_decode.c
        src/frontend/g4mh/g4mh_disasm.c
        src/frontend/g4mh/g4mh_frontend.c
        src/frontend/g4mh/g4mh_intc.c
        src/frontend/g4mh/g4mh_ir.c
        src/frontend/g4mh/g4mh_interp.c
        src/frontend/g4mh/g4mh_fpu.c
    )

    target_compile_definitions(rv32core PUBLIC
        G4MH_ENABLE_DISASM=$<BOOL:${G4MH_ENABLE_DISASM}>
        G4MH_PE_COUNT=${G4MH_PE_COUNT}u
        G4MH_EXT_FPU=$<BOOL:${G4MH_EXT_FPU}>
    )
endif()

if((EMU_FRONTEND_RV32 AND RV32_EXT_F) OR (EMU_FRONTEND_G4MH AND G4MH_EXT_FPU))
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
    foreach(_n softfloat_raiseFlags s_f32UIToCommonNaN s_commonNaNToF32UI
               s_propagateNaNF32UI)
        if(EXISTS "${SOFTFLOAT_DIR}/source/RISCV/${_n}.c")
            list(APPEND _sf_src "${SOFTFLOAT_DIR}/source/RISCV/${_n}.c")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _sf_src)

    target_sources(rv32core PRIVATE ${_sf_src})
    target_include_directories(rv32core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/rv32/softfloat"
        "${SOFTFLOAT_DIR}/source/include"
        "${SOFTFLOAT_DIR}/source/RISCV")
    # Third-party code, not warning-clean under our flags.
    set_source_files_properties(${_sf_src} PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_options(rv32core PRIVATE
    -Wall -Wextra -Wshadow -Wundef
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fomit-frame-pointer>
)

if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET rv32core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
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

# ---------------------------------------------------------------------
# Platform
# ---------------------------------------------------------------------

if(EMU_PLATFORM STREQUAL "host")
    add_subdirectory(src/platform/host)
elseif(EMU_PLATFORM STREQUAL "stm32f446" OR EMU_PLATFORM STREQUAL "stm32f746")
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "EMU_PLATFORM=${EMU_PLATFORM} needs the cross toolchain:\n"
            "  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake")
    endif()
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
endif()

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------

# Built up as a plain string: message() does not evaluate generator
# expressions, so $<BOOL:...> would be printed verbatim.
set(_frontends "")
if(EMU_FRONTEND_RV32)
    set(_isa "RV32I")
    foreach(_ext M A F C B)
        if(RV32_EXT_${_ext})
            string(APPEND _isa "${_ext}")
        endif()
    endforeach()
    # B is the union of Zba, Zbb and Zbs rather than an option of its own,
    # and misa advertises it on exactly that basis, so the banner has to
    # agree.
    if(NOT RV32_EXT_B AND RV32_EXT_ZBA AND RV32_EXT_ZBB AND RV32_EXT_ZBS)
        string(APPEND _isa "B")
    endif()
    if(RV32_EXT_U)
        string(APPEND _isa "U")
    endif()
    if(RV32_EXT_S)
        string(APPEND _isa "S")
    endif()
    list(APPEND _frontends "rv32 (${_isa}_zicsr_zicntr_zifencei)")
endif()
if(EMU_FRONTEND_G4MH)
    list(APPEND _frontends "g4mh (RH850 G4MH, ${G4MH_PE_COUNT} PE)")
endif()

message(STATUS "")
message(STATUS "rv32cortex-m ${PROJECT_VERSION}")
message(STATUS "  platform    ${EMU_PLATFORM}")
message(STATUS "  build type  ${CMAKE_BUILD_TYPE}")
foreach(_f IN LISTS _frontends)
    message(STATUS "  frontend    ${_f}")
endforeach()
message(STATUS "")
