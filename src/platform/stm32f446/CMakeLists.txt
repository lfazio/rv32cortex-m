# SPDX-License-Identifier: Apache-2.0
#
# Nucleo-F446RE firmware.
#
# The vendor driver pack supplies the startup code, the device headers,
# system_stm32f4xx.c and the HAL drivers; only the emulator glue is local.

include(${CMAKE_SOURCE_DIR}/cmake/stm32cube.cmake)

set(RV32_GUEST "isatest" CACHE STRING "Guest image to embed in the firmware")
set(guest_src "${CMAKE_BINARY_DIR}/guest/${RV32_GUEST}.bin")

# The image is produced in tests/guest, and a cross-directory file
# dependency is where the two generators diverge: OBJECT_DEPENDS on a path
# in another directory's scope leaves the Makefile generator with "No rule
# to make target". Copying it here with a local custom command makes the
# dependency intra-directory, which both handle identically.
set(guest_bin "${CMAKE_CURRENT_BINARY_DIR}/guest_image.bin")
add_custom_command(
    OUTPUT "${guest_bin}"
    COMMAND ${CMAKE_COMMAND} -E copy "${guest_src}" "${guest_bin}"
    DEPENDS guest-${RV32_GUEST}
    COMMENT "Staging guest image ${RV32_GUEST}.bin"
    VERBATIM)
add_custom_target(guest-image-staged DEPENDS "${guest_bin}")

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

add_executable(rv32-stm32f446
    main.c
    stm32f4xx_it.c
    guest_image.S
)

target_link_libraries(rv32-stm32f446 PRIVATE rv32core stm32cube)

# guest_image.S pulls the binary in with .incbin.
set_source_files_properties(guest_image.S PROPERTIES
    COMPILE_DEFINITIONS "RV32_GUEST_IMAGE=${guest_bin}"
    OBJECT_DEPENDS "${guest_bin}")

if(TARGET guest-${RV32_GUEST})
    add_dependencies(rv32-stm32f446 guest-image-staged)
else()
    message(WARNING
        "Guest image '${RV32_GUEST}' is not built (no RISC-V toolchain?); "
        "the firmware link will fail until ${guest_bin} exists.")
endif()

target_compile_options(rv32-stm32f446 PRIVATE -Wall -Wextra)

# Native CoreMark baseline: the same sources the guest runs, compiled for
# the ARM host. core_main.c defines main(), so it is renamed at compile
# time and called from our own main.
option(RV32_NATIVE_COREMARK "Run CoreMark natively instead of the emulator" OFF)
if(RV32_NATIVE_COREMARK AND EXISTS "${COREMARK_DIR}/core_main.c")
    target_sources(rv32-stm32f446 PRIVATE
        "${COREMARK_DIR}/core_main.c"
        "${COREMARK_DIR}/core_list_join.c"
        "${COREMARK_DIR}/core_matrix.c"
        "${COREMARK_DIR}/core_state.c"
        "${COREMARK_DIR}/core_util.c"
        coremark_native.c)
    target_include_directories(rv32-stm32f446 PRIVATE
        "${COREMARK_DIR}" "${CMAKE_SOURCE_DIR}/tests/guest/coremark")
    target_compile_definitions(rv32-stm32f446 PRIVATE
        RV32_NATIVE_COREMARK=1 PERFORMANCE_RUN=1
        ITERATIONS=${COREMARK_ITERATIONS})
    # The main= rename must apply to CoreMark's core_main.c alone; applied
    # target-wide it would also rename the firmware's own entry point.
    set_source_files_properties("${COREMARK_DIR}/core_main.c" PROPERTIES
        COMPILE_DEFINITIONS "main=coremark_native_main")
    target_compile_options(rv32-stm32f446 PRIVATE
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
target_link_options(rv32-stm32f446 PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32f446retx.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/rv32-stm32f446.map
    -Wl,--print-memory-usage
)

set_target_properties(rv32-stm32f446 PROPERTIES
    OUTPUT_NAME "rv32-stm32f446"
    SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32f446retx.ld")

# LTO across main.c and the core lets the run loop inline the bus fast
# paths and the interrupt check, which is where the interpreter's time goes.
if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET rv32-stm32f446 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ---------------------------------------------------------------------
# Artefacts
# ---------------------------------------------------------------------

add_custom_command(TARGET rv32-stm32f446 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:rv32-stm32f446>
            "${CMAKE_BINARY_DIR}/rv32-stm32f446.bin"
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:rv32-stm32f446>
            "${CMAKE_BINARY_DIR}/rv32-stm32f446.hex"
    COMMENT "Generating .bin and .hex")

if(CMAKE_SIZE)
    add_custom_command(TARGET rv32-stm32f446 POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:rv32-stm32f446>)
endif()

# ---------------------------------------------------------------------
# Flash and debug
# ---------------------------------------------------------------------

add_custom_target(flash
    COMMAND probe-rs download --chip STM32F446RETx
            $<TARGET_FILE:rv32-stm32f446>
    COMMAND probe-rs reset --chip STM32F446RETx
    DEPENDS rv32-stm32f446
    USES_TERMINAL
    COMMENT "Flashing the Nucleo-F446RE over ST-LINK")

add_custom_target(flash-openocd
    COMMAND openocd -f board/st_nucleo_f4.cfg
            -c "program $<TARGET_FILE:rv32-stm32f446> verify reset exit"
    DEPENDS rv32-stm32f446
    USES_TERMINAL
    COMMENT "Flashing with OpenOCD")

# GDB server on :3333. Attach with the helper in scripts/gdb-attach.sh.
add_custom_target(gdbserver
    COMMAND openocd -f board/st_nucleo_f4.cfg
    USES_TERMINAL
    COMMENT "Starting OpenOCD GDB server on port 3333")
