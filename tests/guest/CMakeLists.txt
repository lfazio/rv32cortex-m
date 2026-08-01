# SPDX-License-Identifier: Apache-2.0
#
# Guest images. These are RISC-V binaries that run *inside* the emulator,
# so they are built with the RISC-V toolchain regardless of what the
# emulator itself is being built for. If that toolchain is missing the
# targets are skipped rather than failing the build: the emulator is still
# perfectly buildable without them.

find_program(RISCV_GCC
    NAMES riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc riscv64-elf-gcc
    DOC "RISC-V cross compiler for guest images")
find_program(RISCV_OBJCOPY
    NAMES riscv64-unknown-elf-objcopy riscv32-unknown-elf-objcopy
          riscv64-elf-objcopy)

if(NOT RISCV_GCC OR NOT RISCV_OBJCOPY)
    message(STATUS "RISC-V toolchain not found: guest images will not be built")
    return()
endif()

set(RV_GUEST_MARCH "rv32imac_zicsr_zicbom_zicboz" CACHE STRING "ISA for guest images")
set(RV_GUEST_MABI  "ilp32"          CACHE STRING "ABI for guest images")

set(guest_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(guest_out "${CMAKE_BINARY_DIR}/guest")
file(MAKE_DIRECTORY "${guest_out}")

# The link script is generated so the guest's idea of how much RAM exists
# always matches what the platform maps. See guest.ld.in.
set(guest_ld "${guest_out}/guest.ld")
configure_file("${guest_dir}/guest.ld.in" "${guest_ld}" @ONLY)

# Build one guest image, producing both .elf (for debugging) and .bin
# (what the target actually loads).
function(add_guest_image name)
    cmake_parse_arguments(G "" "" "SOURCES;FLAGS" ${ARGN})

    set(elf "${guest_out}/${name}.elf")
    set(bin "${guest_out}/${name}.bin")

    set(srcs "")
    foreach(s IN LISTS G_SOURCES)
        list(APPEND srcs "${guest_dir}/${s}")
    endforeach()

    add_custom_command(
        OUTPUT "${elf}"
        COMMAND ${RISCV_GCC}
                -march=${RV_GUEST_MARCH} -mabi=${RV_GUEST_MABI}
                -O2 -g -ffreestanding -nostdlib -fno-builtin
                -fno-common -Wall -Wextra
                ${G_FLAGS}
                -T "${guest_ld}"
                ${srcs}
                -o "${elf}"
        DEPENDS ${srcs} "${guest_ld}"
        COMMENT "Building guest image ${name}.elf"
        VERBATIM)

    add_custom_command(
        OUTPUT "${bin}"
        COMMAND ${RISCV_OBJCOPY} -O binary "${elf}" "${bin}"
        DEPENDS "${elf}"
        COMMENT "Building guest image ${name}.bin"
        VERBATIM)

    add_custom_target(guest-${name} ALL DEPENDS "${bin}")
endfunction()

add_guest_image(isatest  SOURCES start.S isatest.c)
add_guest_image(hello    SOURCES start.S hello.c)

# Peripheral drivers running as guest code, talking to real silicon
# through the passthrough window. Only meaningful on the STM32 target; on
# the host runner the peripheral window is backed by plain memory, so it
# runs but drives nothing.
add_guest_image(stm32drv SOURCES start.S stm32drv.c)

# Compute-bound workload for throughput measurement.
add_guest_image(bench    SOURCES start.S bench.c)
