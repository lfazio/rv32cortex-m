# SPDX-License-Identifier: Apache-2.0
#
# Nucleo-F446RE firmware.
#
# The vendor driver pack supplies the startup code, the device headers,
# system_stm32f4xx.c and the HAL drivers; only the emulator glue is local.

# Which vendor pack to fetch; see the note in stm32f746/CMakeLists.txt.
set(STM32CUBE_FAMILY "f4")
include(${CMAKE_SOURCE_DIR}/cmake/stm32cube.cmake)

# The guest image, by the function the F746 and the N6 use. This board
# staged its own until 2026-10 -- always ${RV32_GUEST}, whatever frontend
# was built -- so a G4MH firmware embedded RV32's isatest, ran it as
# RH850 code from 0x20000000, and faulted at its first fetch: the reason
# docs/stm32f446/g4mh said the pair had never run.
include(${CMAKE_SOURCE_DIR}/cmake/guest-image.cmake)
emu_stage_guest_image(guest_bin)

# ---------------------------------------------------------------------
# Vendor sources
# ---------------------------------------------------------------------

set(hal_src
    stm32f4xx_hal.c
    stm32f4xx_hal_cortex.c
    stm32f4xx_hal_dma.c
    stm32f4xx_hal_exti.c
    stm32f4xx_hal_flash.c
    stm32f4xx_hal_flash_ex.c
    stm32f4xx_hal_gpio.c
    stm32f4xx_hal_pwr.c
    stm32f4xx_hal_pwr_ex.c
    stm32f4xx_hal_rcc.c
    stm32f4xx_hal_rcc_ex.c
    stm32f4xx_hal_uart.c
)
list(TRANSFORM hal_src PREPEND "${STM32_HAL_DIR}/Src/")

add_library(stm32cube STATIC
    ${hal_src}
    "${CMSIS_DEVICE_DIR}/Source/Templates/system_stm32f4xx.c"
    "${CMSIS_DEVICE_DIR}/Source/Templates/gcc/startup_stm32f446xx.s"
)

target_include_directories(stm32cube PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"          # our stm32f4xx_hal_conf.h
    "${CMSIS_CORE_DIR}/Include"
    "${CMSIS_DEVICE_DIR}/Include"
    "${STM32_HAL_DIR}/Inc"
)

target_compile_definitions(stm32cube PUBLIC
    STM32F446xx
    USE_HAL_DRIVER
)

# Vendor code is not warning-clean under our flags and is not ours to fix,
# so it is compiled without the strict set applied to the emulator.
target_compile_options(stm32cube PRIVATE -Wno-unused-parameter)

# ---------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------

add_executable(emu-stm32f446
    platform.c
    board.c
    stm32f4xx_it.c
    guest_image.S
)

# The shared runner pieces -- see src/platform/common/emu_console.h.
target_sources(emu-stm32f446 PRIVATE
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_main.c"
    # The runner as FreeRTOS tasks, one per emulated core -- emu_os.h.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_os.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_debug.c"
    "${CMAKE_SOURCE_DIR}/src/platform/stm32/board.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_console.c"
    "${CMAKE_SOURCE_DIR}/src/platform/stm32/cache.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_syscall.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_stats.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_diag.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_address_space.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_args.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_image.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_session.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_run.c")
target_include_directories(emu-stm32f446 PRIVATE
    # This directory too: the part's own hal_conf.h lives here. board.h
    # is one directory up, shared by all three STM32 platforms -- the
    # shared console reaches the wire through it.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/platform/stm32"
    "${CMAKE_SOURCE_DIR}/src/platform/common")

target_link_libraries(emu-stm32f446 PRIVATE emucore emu_freertos stm32cube)

# guest_image.S pulls the binary in with .incbin.
set_source_files_properties(guest_image.S PROPERTIES
    COMPILE_DEFINITIONS "EMU_GUEST_IMAGE=${guest_bin}"
    OBJECT_DEPENDS "${guest_bin}")

add_dependencies(emu-stm32f446 guest-image-staged)
# Where the guest runs, when it is not in place -- cmake/guest-image.cmake.
if(EMU_GUEST_LOAD_ADDR)
    target_compile_definitions(emu-stm32f446 PRIVATE
        EMU_GUEST_LOAD=${EMU_GUEST_LOAD_ADDR}u)
endif()

# -Wundef with the rest, and it earns its place here rather than being
# tidiness: it is the warning that catches a capability macro read
# without including what defines it, which #if silently treats as 0.
# emucore has had it for a while; the platform targets did not, and the
# JIT statistics block in emu_main.c stopped printing entirely the moment an
# unrelated include was removed. Nothing failed and no test could see it.
# A terminal on a serial line expects CRLF; a host's stdout does not, and
# putting \r into guest output there would break the two suites that
# compare it. See emu_console_putchar.
target_compile_definitions(emu-stm32f446 PRIVATE EMU_CONSOLE_CRLF=1)

target_compile_options(emu-stm32f446 PRIVATE -Wall -Wextra -Wundef)

# Native CoreMark baseline: the same sources the guest runs, compiled for
# the ARM host, so the emulated figure has something to be read against.
option(EMU_NATIVE_COREMARK "Run CoreMark natively instead of the emulator" OFF)
if(EMU_NATIVE_COREMARK)
    include(${CMAKE_SOURCE_DIR}/cmake/coremark_native.cmake)
    emu_coremark_native_attach(emu-stm32f446)
endif()

# Guest RAM size is not a compile definition: the link script computes it
# from what is left over, and platform.c reads it from __guest_ram_start/end.

# ---------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------
#
# CPU and float-ABI flags come from the toolchain file so they apply to
# every object, including the emulator core built in the top-level
# directory. Only the link-time specifics belong here.

# The linker script path is passed unquoted: CMake treats each list item as
# one argument already, and embedded quotes reach the driver literally.
target_link_options(emu-stm32f446 PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32f446retx.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/emu-stm32f446.map
    -Wl,--print-memory-usage
)

set_target_properties(emu-stm32f446 PROPERTIES
    OUTPUT_NAME "emu-stm32f446"
    SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32f446retx.ld")

# LTO across emu_main.c and the core lets the run loop inline the bus fast
# paths and the interrupt check, which is where the interpreter's time goes.
if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET emu-stm32f446 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ---------------------------------------------------------------------
# Artefacts
# ---------------------------------------------------------------------

add_custom_command(TARGET emu-stm32f446 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:emu-stm32f446>
            "${CMAKE_BINARY_DIR}/emu-stm32f446.bin"
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:emu-stm32f446>
            "${CMAKE_BINARY_DIR}/emu-stm32f446.hex"
    COMMENT "Generating .bin and .hex")

if(CMAKE_SIZE)
    add_custom_command(TARGET emu-stm32f446 POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:emu-stm32f446>)
endif()

# ---------------------------------------------------------------------
# Flash and debug
# ---------------------------------------------------------------------

# Which probe, when more than one is attached: EMU_PROBE, as on the F746
# (src/platform/stm32f746/CMakeLists.txt says why it is needed). The
# F446 had no such option because it was the only board when this was
# written, and with three ST-LINKs attached its flash target failed --
# leaving whatever firmware was there running.
set(EMU_PROBE "" CACHE STRING "probe-rs --probe selector (VID:PID:serial)")
if(EMU_PROBE)
    set(_probe_arg --probe ${EMU_PROBE})
else()
    set(_probe_arg)
endif()

add_custom_target(flash
    COMMAND probe-rs download --chip STM32F446RETx --connect-under-reset
            ${_probe_arg} $<TARGET_FILE:emu-stm32f446>
    COMMAND probe-rs reset --chip STM32F446RETx --connect-under-reset
            ${_probe_arg}
    DEPENDS emu-stm32f446
    USES_TERMINAL
    COMMENT "Flashing the Nucleo-F446RE over ST-LINK")

add_custom_target(flash-openocd
    COMMAND openocd -f board/st_nucleo_f4.cfg
            -c "program $<TARGET_FILE:emu-stm32f446> verify reset exit"
    DEPENDS emu-stm32f446
    USES_TERMINAL
    COMMENT "Flashing with OpenOCD")

# GDB server on :3333. Attach with the helper in scripts/gdb-attach.sh.
add_custom_target(gdbserver
    COMMAND openocd -f board/st_nucleo_f4.cfg
    USES_TERMINAL
    COMMENT "Starting OpenOCD GDB server on port 3333")

# ---------------------------------------------------------------------
# Network transport
# ---------------------------------------------------------------------
#
# Nothing about the stack is F7-specific -- it reaches the board through
# board.h and this part supplies every entry point it needs, including
# the interrupt-driven receive the wire requires. It was simply never
# offered here, because the option lived inside the other platform's
# CMakeLists rather than in cmake/emu_net.cmake.
#
# **Off by default on this board**, unlike the F746. emu_net_init() is a
# one-way handover of the console, and this part is the one used for
# bring-up where a serial console is worth more than a network; a board
# whose console is worth giving away should say so itself.
option(EMU_NET "IP/TFTP/telnet/gdb transport over the console UART" OFF)

#
# EMU_NET is defined either way, never left undefined. `#if` on a macro
# that does not exist is 0 without a word, which is how the JIT statistics
# block silently stopped printing -- and -Wundef only catches it because
# the macro is expected to exist. Defining it 0 here is what makes that
# warning meaningful rather than noise.
if(EMU_NET)
    include(${CMAKE_SOURCE_DIR}/cmake/emu_net.cmake)
    emu_net_attach(emu-stm32f446)
else()
    target_compile_definitions(emu-stm32f446 PRIVATE EMU_NET=0)
endif()
