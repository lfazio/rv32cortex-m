# SPDX-License-Identifier: Apache-2.0
#
# rv32cortex-m - a RISC-V RV32IMAC emulator for ARM Cortex-M hosts.
#
# Two build configurations share one core:
#
#   host        native binary for development and the ISA test suite
#   stm32f446   firmware for the Nucleo-F446RE, built with the ST driver pack
#
# Select with -DRV32_PLATFORM=... ; the ARM build additionally needs the
# cross toolchain file:
#
#   cmake -B build/host -DRV32_PLATFORM=host
#   cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

cmake_minimum_required(VERSION 3.20)

project(rv32cortex-m
    VERSION 1.0.0
    DESCRIPTION "RV32IMAC emulator for ARM Cortex-M"
    LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # The interpreter's speed depends heavily on optimisation, and a debug
    # build of the emulator is rarely what anyone wants.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(RV32_PLATFORM "host" CACHE STRING "Target platform (host|stm32f446)")
set_property(CACHE RV32_PLATFORM PROPERTY STRINGS host stm32f446)

# ---------------------------------------------------------------------
# ISA configuration
# ---------------------------------------------------------------------

# Guest RAM size. Shared between the guest link script and the firmware's
# static carve-out so the two can never disagree; see tests/guest/guest.ld.in.
set(RV32_GUEST_RAM_KIB "48" CACHE STRING "Guest RAM size in KiB")

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
set(RV32_JIT_CODE_BYTES "12288" CACHE STRING "JIT code cache size in bytes")

# Build and use the Thumb-2 JIT. Turning this off is how the same guest
# image is measured on the interpreter, which is also how a suspected JIT
# miscompile gets isolated from a core bug.
option(RV32_JIT "Thumb-2 JIT backend" ON)

# Chain loop back edges inside translated code: 35% on bench, 7% on CoreMark.
option(RV32_JIT_LOOP_CHAIN "Chain loop back edges in the JIT" ON)
option(RV32_JIT_INLINE_PERIPH "Inline the peripheral window in the JIT" ON)

# Histogram how often a block reads sp/ra/a0/a1, to decide whether a
# per-block register cache could pay. Measurement scaffolding, off by default.
option(RV32_JIT_HOT_REG_STATS "Count hot-register reads per block" OFF)

# Use Berkeley SoftFloat for the F extension instead of the host FPU.
#
# The host-FPU path via <fenv.h> is smaller and faster but passes 165 of the
# 217 official F tests: the flags it can report are the ones the hardware
# happens to raise, which differ from RISC-V's rules on the fused
# multiply-adds and around subnormals. SoftFloat is the implementation the
# RISC-V FP spec was written against -- its rounding modes and exception
# flags are numerically identical to frm and fflags, and it ships a RISCV
# specialization with the right canonical-NaN behaviour.
option(RV32_FPU_SOFTFLOAT "Use Berkeley SoftFloat for F" OFF)
set(SOFTFLOAT_DIR "${CMAKE_BINARY_DIR}/softfloat" CACHE PATH "Berkeley SoftFloat source")

option(RV32_EXT_SDTRIG "Sdtrig debug triggers" ON)
option(RV32_EXT_PMP "Physical memory protection" ON)
option(RV32_EXT_ZCB "Zcb compressed instructions" ON)
option(RV32_EXT_F "Single-precision floating point" ON)
option(RV32_EXT_M "Integer multiply/divide" ON)
option(RV32_EXT_A "Atomics" ON)
option(RV32_EXT_C "Compressed instructions" ON)
option(RV32_ENABLE_DISASM "Build the disassembler" ON)
option(RV32_ENABLE_TRACE "Per-instruction trace hook (slow)" OFF)
option(RV32_ENABLE_STATS "Collect run statistics" ON)

# ---------------------------------------------------------------------
# Core library: portable C11, no platform dependencies
# ---------------------------------------------------------------------

add_library(rv32core STATIC
    src/core/rv_bus.c
    src/core/rv_csr.c
    src/core/rv_decode.c
    src/core/rv_disasm.c
    src/core/rv_fpu.c
    src/core/rv_hart.c
    src/core/rv_interp.c
    src/core/rv_pmp.c
    src/core/rv_trig.c
    src/devices/rv_clint.c
    src/devices/rv_uart.c
    # Compiles to nothing unless RV_ENABLE_JIT is set, which needs a
    # Thumb-2 host; the file guards itself so it can stay in the list.
    src/backend/rv_jit_thumb2.c
)

target_include_directories(rv32core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_definitions(rv32core PUBLIC
    RV_EXT_F=$<BOOL:${RV32_EXT_F}>
    RV_EXT_ZCB=$<BOOL:${RV32_EXT_ZCB}>
    RV_EXT_PMP=$<BOOL:${RV32_EXT_PMP}>
    RV_EXT_SDTRIG=$<BOOL:${RV32_EXT_SDTRIG}>
    RV_FPU_SOFTFLOAT=$<BOOL:${RV32_FPU_SOFTFLOAT}>
    RV_EXT_M=$<BOOL:${RV32_EXT_M}>
    RV_EXT_A=$<BOOL:${RV32_EXT_A}>
    RV_EXT_C=$<BOOL:${RV32_EXT_C}>
    RV_ENABLE_DISASM=$<BOOL:${RV32_ENABLE_DISASM}>
    RV_ENABLE_TRACE=$<BOOL:${RV32_ENABLE_TRACE}>
    RV_ENABLE_STATS=$<BOOL:${RV32_ENABLE_STATS}>
    RV_INTERP_RAMFUNC=$<BOOL:${RV32_INTERP_IN_RAM}>
    RV_LAZY_IRQ_CHECK=$<BOOL:${RV32_LAZY_IRQ}>
    RV_JIT_CODE_SIZE=${RV32_JIT_CODE_BYTES}u
    RV_ENABLE_JIT=$<BOOL:${RV32_JIT}>
    RV_JIT_LOOP_CHAIN=$<BOOL:${RV32_JIT_LOOP_CHAIN}>
    RV_JIT_INLINE_PERIPH=$<BOOL:${RV32_JIT_INLINE_PERIPH}>
    RV_JIT_HOT_REG_STATS=$<BOOL:${RV32_JIT_HOT_REG_STATS}>
)

# <fenv.h> is the FPU control/status interface the F extension uses; on both
# glibc and newlib its implementation lives in libm.
if(RV32_EXT_F AND NOT RV32_FPU_SOFTFLOAT)
    target_link_libraries(rv32core PUBLIC m)
endif()

if(RV32_EXT_F AND RV32_FPU_SOFTFLOAT)
    if(NOT EXISTS "${SOFTFLOAT_DIR}/source/include/softfloat.h")
        find_package(Git QUIET)
        if(GIT_FOUND)
            message(STATUS "Fetching Berkeley SoftFloat into ${SOFTFLOAT_DIR}")
            execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 -q
                https://github.com/ucb-bar/berkeley-softfloat-3.git
                "${SOFTFLOAT_DIR}")
        endif()
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/softfloat"
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
# Platform
# ---------------------------------------------------------------------

if(RV32_PLATFORM STREQUAL "host")
    add_subdirectory(src/platform/host)
elseif(RV32_PLATFORM STREQUAL "stm32f446")
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "RV32_PLATFORM=stm32f446 needs the cross toolchain:\n"
            "  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake")
    endif()
    add_subdirectory(src/platform/stm32f446)
else()
    message(FATAL_ERROR "unknown RV32_PLATFORM '${RV32_PLATFORM}'")
endif()

# ---------------------------------------------------------------------
# Guest images (built with the RISC-V toolchain, on any host)
# ---------------------------------------------------------------------

add_subdirectory(tests/guest)

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

if(RV32_PLATFORM STREQUAL "host")
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
set(_isa "RV32I")
foreach(_ext M A C)
    if(RV32_EXT_${_ext})
        string(APPEND _isa "${_ext}")
    endif()
endforeach()

message(STATUS "")
message(STATUS "rv32cortex-m ${PROJECT_VERSION}")
message(STATUS "  platform    ${RV32_PLATFORM}")
message(STATUS "  build type  ${CMAKE_BUILD_TYPE}")
message(STATUS "  ISA         ${_isa}_zicsr_zicntr_zifencei")
message(STATUS "")
