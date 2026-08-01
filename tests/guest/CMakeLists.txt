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

set(RV_GUEST_MARCH "rv32imac_zicsr_zicbom_zicboz_zba_zbb_zbc_zbs" CACHE STRING "ISA for guest images")
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
    cmake_parse_arguments(G "" "" "SOURCES;EXTRA_SOURCES;FLAGS" ${ARGN})

    set(elf "${guest_out}/${name}.elf")
    set(bin "${guest_out}/${name}.bin")

    # SOURCES are relative to this directory; EXTRA_SOURCES are absolute,
    # so third-party benchmark sources can be pulled in from anywhere.
    set(srcs "")
    foreach(s IN LISTS G_SOURCES)
        list(APPEND srcs "${guest_dir}/${s}")
    endforeach()
    list(APPEND srcs ${G_EXTRA_SOURCES})

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

# ---------------------------------------------------------------------
# CoreMark
# ---------------------------------------------------------------------
#
# Built from an upstream checkout rather than vendored, because CoreMark's
# licence governs how results may be reported and keeping the source
# pristine keeps that unambiguous. Point COREMARK_DIR at a checkout, or let
# it be fetched.

set(COREMARK_DIR "${CMAKE_BINARY_DIR}/coremark-src" CACHE PATH "CoreMark source")
set(COREMARK_ITERATIONS "2" CACHE STRING "CoreMark iterations")

if(NOT EXISTS "${COREMARK_DIR}/core_main.c")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Fetching CoreMark into ${COREMARK_DIR}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 -q
                    https://github.com/eembc/coremark.git "${COREMARK_DIR}"
            RESULT_VARIABLE _cm_rc)
    endif()
endif()

if(EXISTS "${COREMARK_DIR}/core_main.c")
    add_guest_image(coremark
        SOURCES start.S
        EXTRA_SOURCES
            "${COREMARK_DIR}/core_main.c"
            "${COREMARK_DIR}/core_list_join.c"
            "${COREMARK_DIR}/core_matrix.c"
            "${COREMARK_DIR}/core_state.c"
            "${COREMARK_DIR}/core_util.c"
            "${guest_dir}/coremark/core_portme.c"
        FLAGS
            -I "${COREMARK_DIR}"
            -I "${guest_dir}/coremark"
            -DPERFORMANCE_RUN=1
            -DITERATIONS=${COREMARK_ITERATIONS}
            -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough)
else()
    message(STATUS "CoreMark source not available; skipping the coremark image")
endif()

# ---------------------------------------------------------------------
# Dhrystone
# ---------------------------------------------------------------------

if(EXISTS "${guest_dir}/dhrystone/dhry_1.c")
    add_guest_image(dhrystone
        SOURCES start.S
        EXTRA_SOURCES
            "${guest_dir}/dhrystone/dhry_1.c"
            "${guest_dir}/dhrystone/dhry_2.c"
            "${guest_dir}/dhrystone/dhry_portme.c"
        FLAGS
            -I "${guest_dir}/dhrystone"
            -Wno-implicit-fallthrough)
endif()
