# SPDX-License-Identifier: Apache-2.0
#
# Nucleo-N657X0-Q firmware.
#
# The vendor driver pack supplies the startup code, the device headers,
# system_stm32n6xx_fsbl.c and the HAL drivers; only the emulator glue is
# local.
#
# Three things differ from the F4/F7 platforms, and each is a fact about
# this part rather than a preference. They are called out where they
# happen.

# Which vendor pack to fetch. Stated by every platform rather than left to
# the default in stm32cube.cmake: the failure mode of getting it wrong is a
# pack whose headers do not match the part, which surfaces as a wall of
# unrelated compile errors.
#
# It has to be set *before* the include -- that is when the pack is chosen
# and fetched. Setting it afterwards is a no-op that reads like a fix.
set(STM32CUBE_FAMILY "n6")
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
add_custom_command(
    OUTPUT "${guest_bin}"
    COMMAND ${CMAKE_COMMAND} -E copy "${guest_src}" "${guest_bin}"
    DEPENDS "${guest_src}" guest-${RV32_GUEST}
    COMMENT "Staging guest image ${RV32_GUEST}.bin"
    VERBATIM)
add_custom_target(guest-image-staged DEPENDS "${guest_bin}")

# ---------------------------------------------------------------------
# Vendor sources
# ---------------------------------------------------------------------

set(hal_src
    stm32n6xx_hal.c
    stm32n6xx_hal_cortex.c
    stm32n6xx_hal_dma.c
    stm32n6xx_hal_exti.c
    stm32n6xx_hal_gpio.c
    stm32n6xx_hal_pwr.c
    stm32n6xx_hal_pwr_ex.c
    stm32n6xx_hal_rcc.c
    stm32n6xx_hal_rcc_ex.c
    stm32n6xx_hal_uart.c
    stm32n6xx_hal_uart_ex.c
)
list(TRANSFORM hal_src PREPEND "${STM32_HAL_DIR}/Src/")

#
# **The FSBL variants, not the application ones.** This part has no
# internal flash: the boot ROM loads an image into AXISRAM and jumps to
# it, so what runs here is a first-stage boot loader by the pack's
# reckoning even though it is the whole firmware by ours. The pack ships a
# *separate* system_stm32n6xx_fsbl.c and startup_stm32n657xx_fsbl.s for
# that case, so this cannot point at Templates/system_stm32n6xx.c the way
# the F4 and F7 platforms point at theirs -- there is no such file.
#
# What the FSBL variant does that the others do not: it sets VTOR to where
# the boot ROM placed us, clears the eight SAU regions, applies the
# compensation-cell settings from errata ES0620, and resets the RNG and
# XSPI blocks the boot ROM left running.
#
add_library(stm32cube STATIC
    ${hal_src}
    "${CMSIS_DEVICE_DIR}/Source/Templates/system_stm32n6xx_fsbl.c"
    "${CMSIS_DEVICE_DIR}/Source/Templates/gcc/startup_stm32n657xx_fsbl.s"
)

target_include_directories(stm32cube PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"          # our stm32n6xx_hal_conf.h
    "${CMSIS_CORE_DIR}/Include"
    "${CMSIS_DEVICE_DIR}/Include"
    "${STM32_HAL_DIR}/Inc"
)

target_compile_definitions(stm32cube PUBLIC
    STM32N657xx
    USE_HAL_DRIVER
)

#
# **-mcmse is required, and it must be PUBLIC.**
#
# Required because system_stm32n6xx_fsbl.c reaches SAU and SCB_NS, which
# CMSIS only declares when __ARM_FEATURE_CMSE == 3. Without it the build
# stops at "'SAU' undeclared", which at least says so.
#
# PUBLIC because of the part that would *not* say so. stm32n657xx.h
# defines CPU_IN_SECURE_STATE off __ARM_FEATURE_CMSE, and that macro
# decides whether `USART1` means the non-secure alias at 0x5200_1000 or
# the secure one at 0x4200_1000. Compiling the vendor half with -mcmse and
# our own board.c without it would leave the HAL configuring one USART
# while board_console_putc wrote to the other -- a console that is silent
# with every register correct, which is a failure this project has already
# spent a day on for a different reason. PUBLIC propagates the flag to
# emu-stm32n6's own sources, so both halves agree by construction.
#
# Do *not* also define CPU_IN_SECURE_STATE here: the device header defines
# it itself, and doing both is a redefinition warning at best.
#
# emucore is unaffected -- it does not link stm32cube and never includes a
# device header, which is the src/emu/ rule holding.
#
target_compile_options(stm32cube PUBLIC -mcmse)

# Vendor code is not warning-clean under our flags and is not ours to fix,
# so it is compiled without the strict set applied to the emulator.
target_compile_options(stm32cube PRIVATE -Wno-unused-parameter)

# ---------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------

add_executable(emu-stm32n6
    platform.c
    board.c
    stm32n6xx_it.c
    guest_image.S

    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_main.c"
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

# The shared runner pieces -- see src/platform/common/emu_console.h.
target_include_directories(emu-stm32n6 PRIVATE
    # This directory too: the part's own hal_conf.h lives here. board.h
    # is one directory up, shared by all three STM32 platforms -- the
    # shared console reaches the wire through it.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/platform/stm32"
    "${CMAKE_SOURCE_DIR}/src/platform/common")

target_link_libraries(emu-stm32n6 PRIVATE emucore stm32cube)

# ---------------------------------------------------------------------
# Network transport
# ---------------------------------------------------------------------
#
# **Off by default here, where it is on for the F746.** Not a judgement
# about the transport -- it is the thing that turns a suite run from
# minutes into seconds -- but about what it is *for*. Its whole purpose is
# taking a guest image without reflashing, and this part has no internal
# flash to put one in: board_flash_arena_size() returns 0, so a TFTP
# upload has nowhere to land. Turning it on would give a board that
# negotiates a link, serves telnet, and refuses every upload, while having
# handed away the serial console that would have said so.
#
# It becomes worth turning on the moment the arena is backed by AXISRAM
# instead, which this part has 4 MiB of and is the obvious next step.
option(EMU_NET "IP/TFTP/telnet/gdb transport over the console UART" OFF)

# EMU_NET is defined either way, never left undefined. `#if` on a macro
# that does not exist is 0 without a word, which is how the JIT statistics
# block silently stopped printing.
if(EMU_NET)
    include(${CMAKE_SOURCE_DIR}/cmake/emu_net.cmake)
    emu_net_attach(emu-stm32n6)
else()
    target_compile_definitions(emu-stm32n6 PRIVATE EMU_NET=0)
endif()

# guest_image.S pulls the binary in with .incbin.
set_source_files_properties(guest_image.S PROPERTIES
    COMPILE_DEFINITIONS "EMU_GUEST_IMAGE=${guest_bin}"
    OBJECT_DEPENDS "${guest_bin}")

if(TARGET guest-${RV32_GUEST})
    add_dependencies(emu-stm32n6 guest-image-staged)
else()
    message(WARNING
        "Guest image '${RV32_GUEST}' is not built (no RISC-V toolchain?); "
        "the firmware link will fail until ${guest_bin} exists.")
endif()

# A terminal on a serial line expects CRLF; a host's stdout does not, and
# putting \r into guest output there would break the two suites that
# compare it. See emu_console_putchar.
target_compile_definitions(emu-stm32n6 PRIVATE EMU_CONSOLE_CRLF=1)

# -Wundef with the rest, and it earns its place: it is the warning that
# catches a capability macro read without including what defines it,
# which #if silently treats as 0.
target_compile_options(emu-stm32n6 PRIVATE -Wall -Wextra -Wundef)

# Native CoreMark baseline: the same sources the guest runs, compiled for
# the ARM host, so the emulated figure has something to be read against.
option(EMU_NATIVE_COREMARK "Run CoreMark natively instead of the emulator" OFF)
if(EMU_NATIVE_COREMARK)
    include(${CMAKE_SOURCE_DIR}/cmake/coremark_native.cmake)
    emu_coremark_native_attach(emu-stm32n6)
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
target_link_options(emu-stm32n6 PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32n657xx_lrun.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/emu-stm32n6.map
    -Wl,--print-memory-usage
)

set_target_properties(emu-stm32n6 PROPERTIES
    OUTPUT_NAME "emu-stm32n6"
    SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32n657xx_lrun.ld")

# LTO across emu_main.c and the core lets the run loop inline the bus fast
# paths and the interrupt check, which is where the interpreter's time goes.
if(RV32_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set_property(TARGET emu-stm32n6 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ---------------------------------------------------------------------
# Artefacts
# ---------------------------------------------------------------------

add_custom_command(TARGET emu-stm32n6 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:emu-stm32n6>
            "${CMAKE_BINARY_DIR}/emu-stm32n6.bin"
    COMMENT "Generating .bin")

if(CMAKE_SIZE)
    add_custom_command(TARGET emu-stm32n6 POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:emu-stm32n6>)
endif()

# ---------------------------------------------------------------------
# Flash
# ---------------------------------------------------------------------
#
# **The board must be in development boot**, and that is a physical
# change no tool can make. UM3417 table 11, for this board (MB1940):
#
#     BOOT0   BOOT1   boot source
#       -       1     development boot
#       0       0     flash boot
#       1       0     serial boot
#
# and "BOOT0 and BOOT1 might be configured manually by pushing the
# mechanical parts: JP1 (BOOT0) and JP2 (BOOT1)". So BOOT1 has to go
# high; BOOT0 does not matter once it is.
#
# **The board ships in the wrong position for this**, which is why it
# looks broken out of the box. UM3417 figure 3, "Default board
# configuration", is JP1 [1-2] and JP2 [1-2], and the jumper table spells
# JP2 [1-2] as "Boot in flash mode" -- BOOT1 = 0. Moving JP2 off [1-2] is
# the change; the manual does not name the other position in prose, but
# the [1-2] notation is a three-pin block.
#
# What the wrong position looks like, measured rather than guessed --
# every one of these was seen on this board in flash boot:
#
#   probe-rs, plain attach     JtagNoDeviceConnected
#   probe-rs --connect-under-reset
#                              SwdApWdataError
#   CubeProgrammer HOTPLUG     "Unable to get core ID / No STM32 target
#                              found! If your product embeds Debug
#                              Authentication..."  -- which sends you
#                              looking at debug authentication, and it is
#                              not that
#   CubeProgrammer mode=UR     *works*: reports STM32N6xx, Cortex-M55 --
#                              the DAP is open before the boot ROM closes
#                              it -- but AXISRAM is not powered yet, so
#                              reading 0x34080000 fails and a download
#                              dies with "failed to download Sector[0]"
#
# That last pair is the tell, and it is why "the debug port is broken" is
# the wrong reading: the port is fine, the *memory* is not up. Only the
# boot ROM enables AXISRAM, and only development boot leaves it enabled
# with the port still open.
#
# probe-rs 0.32 does list five STM32N6 targets, so this could move back to
# it once the board can be attached at all; it is CubeProgrammer here
# because mode=UR is the invocation that was shown to reach the core.
#
# The .bin at an explicit address rather than the .elf: the linker aligns
# the first LOAD segment down to its 64 KB page size, so the ELF advertises
# 0x34000000 while .isr_vector really sits at 0x34000400, and a programmer
# that believes the segment header writes a kilobyte too low.
#
set(EMU_ST_TOOLS
    "/opt/st/stm32cubeide_2.1.0/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_2.2.400.202601091506/tools/bin"
    CACHE PATH "STM32CubeProgrammer / SigningTool directory")

#
# **The image needs a boot-ROM header, and that is what the 0x400 in the
# link script is for.** The ROM does not jump to a bare vector table: it
# validates a header and takes the entry point from it. So the payload is
# signed -- `-nk`, an empty header with no keys, which is what an OPEN
# part accepts -- with `-hv 2.3` and `-align`, the latter documented as
# "align the payload to the 0x400 offset ... applicable only with header
# v2.3 for MCUs (STM32N6)".
#
# The tool reads the entry point out of the vector table by itself and
# says so: "Entry point value : 0x340167d1". Header plus payload goes to
# 0x34000000, which puts our .isr_vector at 0x34000400 exactly where the
# link script placed it.
#
# **This gets as far as "File download complete" and the guest does not
# run.** Verified as far as it goes: the memory regions match ST's own
# STM32N657XX_LRUN.ld, every symbol its startup copies with resolves
# sanely (_sdata 0x34080000, _sidata 0x34020c08, 0x3c bytes), the vector
# table reads back correctly from RAM, and the signing tool finds the
# right entry point. What does not happen is execution: after every
# start, .data at its destination is still zero and USART1's registers
# are still zero, so the reset handler has not run. Starting the ROM's
# way (-g 0x34000000) and by hand (-g at the reset handler) both report
# success and both leave it unexecuted.
#
# What is missing is ST's documented development-boot start sequence, and
# it is not in any package installed here -- CubeIDE ships no STM32N6
# example projects and the CubeN6 firmware package is not present. The
# next step is that package's FSBL example, not more guessing at the
# tools.
#
#
# **Read the Voltage line before the error text.** With the target
# unpowered CubeProgrammer says "Unable to get core ID / No STM32 target
# found! If your product embeds Debug Authentication..." -- word for word
# what it says for a wrong boot mode, and for a part that is genuinely
# locked. The only line that tells them apart is `Voltage`, which reads
# 0,22V for a dark board and 3,28V for a live one. CN9 selects where the
# 5V comes from and [1-2] is "5V from STLINK-V3EC"; move it and the
# ST-LINK keeps enumerating and keeps printing "Board :
# NUCLEO-N657X0-Q", so everything looks connected while the MCU has no
# power at all.
#
# **JP3 (STLK_RST) is not the answer, and fitting it makes things worse.**
# The name is ST-LINK reset: fitting it holds the *on-board debugger* in
# reset and the probe disappears from USB entirely. It is OFF by default
# for that reason. This comment previously recommended fitting it, on the
# reasoning below, which is wrong about the jumper and right about the
# symptom:
#
#   ST-LINK_gdbserver   "Target not halted after reset. Force halt /
#                       Failed to halt target / Error in initializing
#                       ST-LINK device. Reason: Target not halted."
#   mode=HWRSTPULSE     ST-LINK error (DEV_CONNECT_ERR) -- the mode that
#                       drives NRST cannot connect at all
#   mode=UR             connects and reads memory, because the AHB-AP
#                       works; it is the core debug that does not
#
# So -g reports success and nothing runs, and reads of the core's private
# peripheral bus come back as nonsense -- VTOR as 0x18, which is not a
# legal value, because bits 6:0 of VTOR are reserved. Both live access
# ports answer identically, so it is not an AP selection problem either.
#
# Fit JP3 before concluding anything else about this board.
#
add_custom_target(flash
    COMMAND ${CMAKE_COMMAND} -E echo
            "N6: JP2 (BOOT1) at 2-3 = development boot. See UM3417 table 11."
    COMMAND ${EMU_ST_TOOLS}/STM32_SigningTool_CLI
            -bin "${CMAKE_BINARY_DIR}/emu-stm32n6.bin"
            -nk -t fsbl -hv 2.3 -align
            -o "${CMAKE_BINARY_DIR}/emu-stm32n6-signed.bin"
    COMMAND ${EMU_ST_TOOLS}/STM32_Programmer_CLI -c port=SWD mode=HOTPLUG
            -w "${CMAKE_BINARY_DIR}/emu-stm32n6-signed.bin" 0x34000000
            -g 0x34000000
    DEPENDS emu-stm32n6
    USES_TERMINAL
    COMMENT "Signing and loading the Nucleo-N657X0-Q into AXISRAM")
