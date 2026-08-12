# SPDX-License-Identifier: Apache-2.0
#
# Cross toolchain for ARM Cortex-M targets.
#
#   cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# The CPU flags are set here rather than in a target's CMakeLists because
# they have to reach *every* compilation, including the emulator core
# defined in the top-level directory. add_compile_options() would only
# affect targets created after it in the same directory tree, which
# silently leaves other objects built for the wrong architecture.
#
# Retarget with -DRV32_ARM_CPU=... ; the core is portable to ARMv6-M
# (cortex-m0plus, no FPU) and ARMv8.1-M (cortex-m85).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# There is no libc to link against when probing the compiler, so ask CMake
# to build a static library rather than an executable for its ABI test.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(RV32_ARM_TOOLCHAIN_PREFIX "arm-none-eabi-" CACHE STRING "Cross toolchain prefix")

# Defaults follow EMU_PLATFORM, which is already in the cache by the time
# the toolchain file runs because it comes from the command line. Getting
# this wrong is not a build error: -mcpu=cortex-m4 code runs on a Cortex-M7
# and simply never uses its caches or its wider pipeline, so the port would
# look like it worked and quietly leave most of the part behind.
if(EMU_PLATFORM STREQUAL "stm32f746")
    set(_def_cpu "cortex-m7")
    # The F7 FPU is single precision only, like the M4's -- fpv5-d16 would
    # emit double-precision instructions this part does not have.
    set(_def_fpu "fpv5-sp-d16")
else()
    set(_def_cpu "cortex-m4")
    set(_def_fpu "fpv4-sp-d16")
endif()

set(RV32_ARM_CPU       "${_def_cpu}" CACHE STRING "Target CPU (-mcpu)")
set(RV32_ARM_FPU       "${_def_fpu}" CACHE STRING "Target FPU (-mfpu); empty for soft-float")
set(RV32_ARM_FLOAT_ABI "hard"        CACHE STRING "Float ABI (-mfloat-abi)")

find_program(CMAKE_C_COMPILER   ${RV32_ARM_TOOLCHAIN_PREFIX}gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER ${RV32_ARM_TOOLCHAIN_PREFIX}g++)
find_program(CMAKE_ASM_COMPILER ${RV32_ARM_TOOLCHAIN_PREFIX}gcc REQUIRED)
find_program(CMAKE_OBJCOPY      ${RV32_ARM_TOOLCHAIN_PREFIX}objcopy REQUIRED)
find_program(CMAKE_OBJDUMP      ${RV32_ARM_TOOLCHAIN_PREFIX}objdump)
find_program(CMAKE_SIZE         ${RV32_ARM_TOOLCHAIN_PREFIX}size)

set(_arch "-mcpu=${RV32_ARM_CPU} -mthumb")
if(RV32_ARM_FPU)
    string(APPEND _arch " -mfpu=${RV32_ARM_FPU} -mfloat-abi=${RV32_ARM_FLOAT_ABI}")
else()
    string(APPEND _arch " -mfloat-abi=soft")
endif()

set(CMAKE_C_FLAGS_INIT   "${_arch} -Os -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${_arch} -Os -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${_arch}")

# No --specs=nano.specs/nosys.specs here on purpose: those ship with ARM's
# and ST's toolchain distributions but not with Debian's arm-none-eabi
# packaging, and requiring them makes the build non-portable. This firmware
# calls nothing that needs a syscall backend -- the only libc symbols it
# pulls in are __libc_init_array (from ST's startup) plus memcpy/memset --
# so the default link works everywhere.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_arch} -Wl,--gc-sections")

# Only look for target artefacts in the sysroot, but keep host programs
# (the RISC-V compiler that builds guest images) findable.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
