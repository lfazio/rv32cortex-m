# SPDX-License-Identifier: Apache-2.0
#
# Nucleo-F746ZG firmware.
#
# The vendor driver pack supplies the startup code, the device headers,
# system_stm32f7xx.c and the HAL drivers; only the emulator glue is local.

# Which vendor pack to fetch. Stated by every platform rather than left to
# the default in stm32cube.cmake: the failure mode of getting it wrong is a
# pack whose headers do not match the part, which surfaces as a wall of
# unrelated compile errors.
#
# It has to be set *before* the include -- that is when the pack is chosen
# and fetched. Setting it afterwards is a no-op that reads like a fix.
set(STM32CUBE_FAMILY "f7")
include(${CMAKE_SOURCE_DIR}/cmake/stm32cube.cmake)

set(RV32_GUEST "isatest" CACHE STRING "Guest image to embed in the firmware")
set(guest_src "${CMAKE_BINARY_DIR}/guest/${RV32_GUEST}.bin")

# The image is produced in tests/guest, and a cross-directory file
# dependency is where the two generators diverge: OBJECT_DEPENDS on a path
# in another directory's scope leaves the Makefile generator with "No rule
# to make target". Copying it here with a local custom command makes the
# dependency intra-directory, which both handle identically.
set(guest_bin "${CMAKE_CURRENT_BINARY_DIR}/guest_image.bin")
#
# Both the file and the target are named, and both are needed. The target
# supplies the rule that builds the image; the *file* supplies staleness.
# Depending on the target alone is an ordering constraint only, so the copy
# ran once and never again: editing a guest source rebuilt the .bin, left
# the staged copy untouched, and produced firmware still carrying the
# previous guest. Nothing failed, which is what made it dangerous.
add_custom_command(
    OUTPUT "${guest_bin}.ro" "${guest_bin}.rw"
    COMMAND ${CMAKE_COMMAND} -E copy "${guest_src}.ro" "${guest_bin}.ro"
    COMMAND ${CMAKE_COMMAND} -E copy "${guest_src}.rw" "${guest_bin}.rw"
    DEPENDS "${guest_src}.ro" "${guest_src}.rw" guest-${RV32_GUEST}
    COMMENT "Staging guest image ${RV32_GUEST}.bin (split)"
    VERBATIM)
add_custom_target(guest-image-staged
    DEPENDS "${guest_bin}.ro" "${guest_bin}.rw")

# ---------------------------------------------------------------------
# Vendor sources
# ---------------------------------------------------------------------

set(hal_src
    stm32f7xx_hal.c
    stm32f7xx_hal_cortex.c
    stm32f7xx_hal_dma.c
    stm32f7xx_hal_exti.c
    stm32f7xx_hal_flash.c
    stm32f7xx_hal_flash_ex.c
    stm32f7xx_hal_gpio.c
    stm32f7xx_hal_pwr.c
    stm32f7xx_hal_pwr_ex.c
    stm32f7xx_hal_rcc.c
    stm32f7xx_hal_rcc_ex.c
    stm32f7xx_hal_uart.c
)
list(TRANSFORM hal_src PREPEND "${STM32_HAL_DIR}/Src/")

add_library(stm32cube STATIC
    ${hal_src}
    "${CMSIS_DEVICE_DIR}/Source/Templates/system_stm32f7xx.c"
    "${CMSIS_DEVICE_DIR}/Source/Templates/gcc/startup_stm32f746xx.s"
)

target_include_directories(stm32cube PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"          # our stm32f7xx_hal_conf.h
    "${CMSIS_CORE_DIR}/Include"
    "${CMSIS_DEVICE_DIR}/Include"
    "${STM32_HAL_DIR}/Inc"
)

target_compile_definitions(stm32cube PUBLIC
    STM32F746xx
    USE_HAL_DRIVER
)

# The Cortex-M7 has instruction and data caches, which is what makes the
# JIT's write-then-execute need real maintenance rather than a barrier. The
# backend cannot infer this: -mcpu=cortex-m4 and -mcpu=cortex-m7 both define
# __ARM_ARCH_7EM__, so the platform has to say so.
target_compile_definitions(rv32core PUBLIC RV_ARM_HAS_CACHES=1)

# Vendor code is not warning-clean under our flags and is not ours to fix,
# so it is compiled without the strict set applied to the emulator.
target_compile_options(stm32cube PRIVATE -Wno-unused-parameter)

# ---------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------

add_executable(rv32-stm32f746
    main.c
    board.c
    stm32f7xx_it.c
    guest_image.S
)

target_link_libraries(rv32-stm32f746 PRIVATE rv32core stm32cube)

# ---------------------------------------------------------------------
# Network transport
# ---------------------------------------------------------------------
#
# Off by default, because it costs guest RAM. The link script gives the
# guest whatever SRAM is left after the firmware's .bss, so lwIP's pools
# come straight out of the largest architecture test's headroom -- read
# the `ram` line in the boot banner after turning it on.
#
# What it buys is not having to reflash between tests: with it on, the
# board takes a guest image over TFTP and reports over telnet, which is
# the difference between minutes and seconds per test across 274 of them.
option(EMU_NET "SLIP/TFTP/telnet transport over the console UART" OFF)

if(EMU_NET)
    include(${CMAKE_SOURCE_DIR}/cmake/lwip.cmake)

    target_sources(rv32-stm32f746 PRIVATE
        "${CMAKE_SOURCE_DIR}/src/net/net.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_sio.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_telnet.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_tftp.c")

    # The port sources are built into the firmware, not into lwip: they
    # reach the board through board.h, which lives in this directory, and
    # that is the whole of their platform dependency -- which is what
    # lets the same stack build for either Nucleo. lwip itself never
    # includes board.h.
    #
    # Linking lwip PUBLIC-exports src/net (for lwipopts.h and arch/cc.h)
    # and lwIP's own headers onto the firmware, so nothing else needs
    # naming here.
    target_link_libraries(rv32-stm32f746 PRIVATE lwip)

    set(EMU_NET_ADDR "192.168.7.2"   CACHE STRING "Board IP address")
    set(EMU_NET_PEER "192.168.7.1"   CACHE STRING "Host IP address")
    set(EMU_NET_MASK "255.255.255.0" CACHE STRING "Netmask")

    target_compile_definitions(rv32-stm32f746 PRIVATE
        EMU_NET=1
        EMU_NET_ADDR="${EMU_NET_ADDR}"
        EMU_NET_PEER="${EMU_NET_PEER}"
        EMU_NET_MASK="${EMU_NET_MASK}")
endif()

# guest_image.S pulls the binary in with .incbin.
set_source_files_properties(guest_image.S PROPERTIES
    COMPILE_DEFINITIONS "RV32_GUEST_IMAGE_RO=${guest_bin}.ro;RV32_GUEST_IMAGE_RW=${guest_bin}.rw"
    OBJECT_DEPENDS "${guest_bin}.ro;${guest_bin}.rw")

if(TARGET guest-${RV32_GUEST})
    add_dependencies(rv32-stm32f746 guest-image-staged)
else()
    message(WARNING
        "Guest image '${RV32_GUEST}' is not built (no RISC-V toolchain?); "
        "the firmware link will fail until ${guest_bin} exists.")
endif()

target_compile_options(rv32-stm32f746 PRIVATE -Wall -Wextra)

# Native CoreMark baseline: the same sources the guest runs, compiled for
# the ARM host. core_main.c defines main(), so it is renamed at compile
# time and called from our own main.
option(RV32_NATIVE_COREMARK "Run CoreMark natively instead of the emulator" OFF)
if(RV32_NATIVE_COREMARK AND EXISTS "${COREMARK_DIR}/core_main.c")
    target_sources(rv32-stm32f746 PRIVATE
        "${COREMARK_DIR}/core_main.c"
        "${COREMARK_DIR}/core_list_join.c"
        "${COREMARK_DIR}/core_matrix.c"
        "${COREMARK_DIR}/core_state.c"
        "${COREMARK_DIR}/core_util.c"
        coremark_native.c)
    target_include_directories(rv32-stm32f746 PRIVATE
        "${COREMARK_DIR}" "${CMAKE_SOURCE_DIR}/tests/guest/coremark")
    target_compile_definitions(rv32-stm32f746 PRIVATE
        RV32_NATIVE_COREMARK=1 PERFORMANCE_RUN=1
        ITERATIONS=${COREMARK_ITERATIONS})
    # The main= rename must apply to CoreMark's core_main.c alone; applied
    # target-wide it would also rename the firmware's own entry point.
    set_source_files_properties("${COREMARK_DIR}/core_main.c" PROPERTIES
        COMPILE_DEFINITIONS "main=coremark_native_main")
    target_compile_options(rv32-stm32f746 PRIVATE
        -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough)
endif()

# Guest RAM size is not a compile definition: the link script computes it
# from what is left over, and main.c reads it from __guest_ram_start/end.

# ---------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------
#
# CPU and float-ABI flags come from the toolchain file so they apply to
# every object, including the emulator core built in the top-level
# directory. Only the link-time specifics belong here.

# The linker script path is passed unquoted: CMake treats each list item as
# one argument already, and embedded quotes reach the driver literally.
target_link_options(rv32-stm32f746 PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32f746zgtx.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/rv32-stm32f746.map
    -Wl,--print-memory-usage
)

set_target_properties(rv32-stm32f746 PROPERTIES
    OUTPUT_NAME "rv32-stm32f746"
    SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32f746zgtx.ld")

# LTO across main.c and the core lets the run loop inline the bus fast
# paths and the interrupt check, which is where the interpreter's time goes.
if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET rv32-stm32f746 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ---------------------------------------------------------------------
# Artefacts
# ---------------------------------------------------------------------

add_custom_command(TARGET rv32-stm32f746 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:rv32-stm32f746>
            "${CMAKE_BINARY_DIR}/rv32-stm32f746.bin"
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:rv32-stm32f746>
            "${CMAKE_BINARY_DIR}/rv32-stm32f746.hex"
    COMMENT "Generating .bin and .hex")

if(CMAKE_SIZE)
    add_custom_command(TARGET rv32-stm32f746 POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:rv32-stm32f746>)
endif()

# ---------------------------------------------------------------------
# Flash and debug
# ---------------------------------------------------------------------

# --connect-under-reset is not optional here, and the reason is this
# firmware in particular: it never idles. It runs a guest flat out and
# then parks in __WFI with interrupts arriving, so the debug port is
# contended from reset onwards and a plain attach times out --
#
#   Error: Connecting to the chip was unsuccessful.
#   Caused by: Timeout occurred during operation.
#
# which reads like a broken probe or a dead board rather than "the part
# is busy". Holding reset while the probe attaches removes the race
# entirely. It costs nothing when the board is idle, so it is the
# default rather than a flag to remember after the failure.
add_custom_target(flash
    COMMAND probe-rs download --chip STM32F746ZGTx --connect-under-reset
            $<TARGET_FILE:rv32-stm32f746>
    COMMAND probe-rs reset --chip STM32F746ZGTx --connect-under-reset
    DEPENDS rv32-stm32f746
    USES_TERMINAL
    COMMENT "Flashing the Nucleo-F746ZG over ST-LINK")

add_custom_target(flash-openocd
    COMMAND openocd -f board/st_nucleo_f7.cfg
            -c "program $<TARGET_FILE:rv32-stm32f746> verify reset exit"
    DEPENDS rv32-stm32f746
    USES_TERMINAL
    COMMENT "Flashing with OpenOCD")

# GDB server on :3333. Attach with the helper in scripts/gdb-attach.sh.
add_custom_target(gdbserver
    COMMAND openocd -f board/st_nucleo_f7.cfg
    USES_TERMINAL
    COMMENT "Starting OpenOCD GDB server on port 3333")
