# SPDX-License-Identifier: Apache-2.0
#
# FreeRTOS: one task per emulated core, on every platform.
#
# The kernel is MIT-licensed and fetched rather than vendored, as SoftFloat
# is: a checkout under the build directory, cloned at configure time if it
# is missing, and a configure error -- not a fallback -- if it cannot be.
# Point FREERTOS_KERNEL_DIR at a checkout to build offline.
#
# The port follows the platform, and it is the only thing that does:
#
#   host        portable/ThirdParty/GCC/Posix -- tasks are pthreads, and
#               the kernel runs exactly one of them at a time. A multicore
#               development machine gains nothing from the threads until
#               there is an SMP-capable FreeRTOS host; see docs/os.md.
#   stm32f746   portable/GCC/ARM_CM7/r0p1
#   stm32f446   portable/GCC/ARM_CM4F
#   stm32n6     portable/GCC/ARM_CM55_NTZ/non_secure, run secure-only:
#               this firmware is a secure image built with -mcmse.
#
# FreeRTOSConfig.h is src/platform/common/FreeRTOSConfig.h, one file for
# all four, keyed on EMU_OS_PORT_* defined here.

set(FREERTOS_KERNEL_TAG "V11.1.0" CACHE STRING "FreeRTOS-Kernel release")
set(FREERTOS_KERNEL_DIR "${CMAKE_BINARY_DIR}/freertos-kernel"
    CACHE PATH "FreeRTOS-Kernel source")

if(NOT EXISTS "${FREERTOS_KERNEL_DIR}/tasks.c")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Fetching FreeRTOS-Kernel ${FREERTOS_KERNEL_TAG} into ${FREERTOS_KERNEL_DIR}")
        execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 -q
            --branch "${FREERTOS_KERNEL_TAG}"
            https://github.com/FreeRTOS/FreeRTOS-Kernel.git
            "${FREERTOS_KERNEL_DIR}")
    endif()
endif()
if(NOT EXISTS "${FREERTOS_KERNEL_DIR}/tasks.c")
    message(FATAL_ERROR
        "FreeRTOS-Kernel not found in ${FREERTOS_KERNEL_DIR} and could not "
        "be fetched. Clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git "
        "(tag ${FREERTOS_KERNEL_TAG}) there, or point "
        "-DFREERTOS_KERNEL_DIR=<path> at a checkout.")
endif()

set(_frt "${FREERTOS_KERNEL_DIR}")
set(_frt_port_srcs)
set(_frt_port_inc)
set(_frt_defs)

if(EMU_PLATFORM STREQUAL "host")
    set(_frt_port "${_frt}/portable/ThirdParty/GCC/Posix")
    list(APPEND _frt_port_srcs "${_frt_port}/port.c"
                               "${_frt_port}/utils/wait_for_event.c")
    list(APPEND _frt_port_inc "${_frt_port}" "${_frt_port}/utils")
    list(APPEND _frt_defs EMU_OS_PORT_POSIX=1)
elseif(EMU_PLATFORM STREQUAL "stm32f746")
    set(_frt_port "${_frt}/portable/GCC/ARM_CM7/r0p1")
    list(APPEND _frt_port_srcs "${_frt_port}/port.c")
    list(APPEND _frt_port_inc "${_frt_port}")
    list(APPEND _frt_defs EMU_OS_PORT_ARMV7M=1)
elseif(EMU_PLATFORM STREQUAL "stm32f446")
    set(_frt_port "${_frt}/portable/GCC/ARM_CM4F")
    list(APPEND _frt_port_srcs "${_frt_port}/port.c")
    list(APPEND _frt_port_inc "${_frt_port}")
    list(APPEND _frt_defs EMU_OS_PORT_ARMV7M=1)
elseif(EMU_PLATFORM STREQUAL "stm32n6")
    set(_frt_port "${_frt}/portable/GCC/ARM_CM55_NTZ/non_secure")
    list(APPEND _frt_port_srcs "${_frt_port}/port.c" "${_frt_port}/portasm.c")
    list(APPEND _frt_port_inc "${_frt_port}")
    list(APPEND _frt_defs EMU_OS_PORT_ARMV8M=1)
    #
    # The ARMv8-M port *defines* SysTick_Handler, where the v7-M ports
    # export xPortSysTickHandler for the application's handler to call.
    # This board's handler has to advance the HAL's tick as well, so the
    # port's is renamed to the v7-M name in that one file and called
    # from stm32n6xx_it.c like the others.
    #
    set_source_files_properties("${_frt_port}/port.c" PROPERTIES
        COMPILE_DEFINITIONS "SysTick_Handler=xPortSysTickHandler")
else()
    message(FATAL_ERROR "freertos.cmake: no FreeRTOS port for EMU_PLATFORM=${EMU_PLATFORM}")
endif()

#
# tasks.c and list.c are the scheduler; queue.c is what tasks.c needs for
# its notifications' sibling primitives. No timers, no event groups, no
# stream buffers, and no heap: every task and its stack is static, so a
# board's RAM is decided at link time and the guest gets the rest.
#
add_library(emu_freertos STATIC
    "${_frt}/tasks.c"
    "${_frt}/list.c"
    "${_frt}/queue.c"
    ${_frt_port_srcs})

target_include_directories(emu_freertos SYSTEM PUBLIC
    "${_frt}/include"
    ${_frt_port_inc}
    "${CMAKE_SOURCE_DIR}/src/platform/common")
target_compile_definitions(emu_freertos PUBLIC ${_frt_defs})

#
# How many emulated cores this build can run as tasks, so that emu_os.c
# allocates stacks for those and no more: on a board each is RAM the
# guest would otherwise have.
#
set(_emu_os_cores 1)
if(EMU_GUEST_ARCH_G4MH AND G4MH_PE_COUNT GREATER 1)
    set(_emu_os_cores ${G4MH_PE_COUNT})
endif()
target_compile_definitions(emu_freertos PUBLIC EMU_OS_MAX_CORES=${_emu_os_cores}u)

# Third-party code: built, not linted. The project's -Werror-adjacent
# warnings are for the project's code.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emu_freertos PRIVATE -w)
endif()

# The ports need the board's CMSIS view of the part (SystemCoreClock,
# __NVIC_PRIO_BITS) only through FreeRTOSConfig.h, which declares what it
# uses itself, so there is no dependency on the Cube packs here.
