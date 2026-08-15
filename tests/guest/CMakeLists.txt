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

# Every extension the emulator implements, so the default guest exercises
# what the core actually supports and a new extension reaches the images by
# editing one line. D is absent deliberately: the emulator implements F
# only, matching the single-precision FPU of the Cortex-M4F/M7 targets.
#
# Zcb is supported by the emulator but deliberately absent here. Building
# the guest with it cost ~9% on CoreMark at an identical instruction count:
# the compiler swaps 32-bit encodings for compressed ones, and under
# expand-then-execute a compressed instruction is never cheaper to
# interpret than the 32-bit form it replaces, only cheaper to store. Add
# _zcb back if guest flash is the binding constraint.
#
# This is a CACHE variable, so an existing build tree keeps its old value:
# pass -DRV_GUEST_MARCH=... explicitly when changing it.
set(RV_GUEST_MARCH
    "rv32imafc_zicsr_zifencei_zicbom_zicboz_zba_zbb_zbc_zbs_zacas"
    CACHE STRING "ISA for guest images")
# ilp32 rather than ilp32f: the guest ABI stays soft-float so images built
# with and without F are interchangeable, and F is exercised explicitly by
# the self-test rather than implicitly by the calling convention.
set(RV_GUEST_MABI  "ilp32" CACHE STRING "ABI for guest images")

set(guest_dir "${CMAKE_CURRENT_SOURCE_DIR}")

# Headers are named explicitly in every image's DEPENDS. A custom command
# depends on what it lists and nothing else, so without this a header edit
# rebuilt nothing and the image kept the previous definitions -- silently,
# which is how the same omission in the firmware's image staging produced
# firmware carrying a stale guest.
file(GLOB_RECURSE guest_headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
set(guest_out "${CMAKE_BINARY_DIR}/guest")
file(MAKE_DIRECTORY "${guest_out}")

# The link script is generated so the guest's idea of how much RAM exists
# always matches what the platform maps. See guest.ld.in.
set(guest_ld "${guest_out}/guest.ld")
configure_file("${guest_dir}/guest.ld.in" "${guest_ld}" @ONLY)

# Build one guest image, producing both .elf (for debugging) and .bin
# (what the target actually loads).
function(add_guest_image name)
    # LIBS is separate from FLAGS because position matters: a linker
    # resolves -l against the objects *already* seen, so a library given
    # with the compile flags -- which come first -- is scanned before
    # anything needs it and silently contributes nothing.
    cmake_parse_arguments(G "" "" "SOURCES;EXTRA_SOURCES;FLAGS;LIBS" ${ARGN})

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
                ${G_LIBS}
                -o "${elf}"
        DEPENDS ${srcs} ${guest_headers} "${guest_ld}"
        COMMENT "Building guest image ${name}.elf"
        VERBATIM)

    add_custom_command(
        OUTPUT "${bin}"
        COMMAND ${RISCV_OBJCOPY} -O binary "${elf}" "${bin}"
        DEPENDS "${elf}"
        COMMENT "Building guest image ${name}.bin"
        VERBATIM)

    # The same image split at __guest_ro_end, so the firmware can serve
    # the read-only half straight out of the part's flash instead of
    # copying it into guest RAM. Concatenated the two are byte-identical
    # to ${bin} -- the link script aligns the boundary to 4, so nothing
    # is padded between them -- which is what lets guest_image.S embed
    # them consecutively and still hand out one base pointer.
    #
    # A split rather than extracting the symbol with `nm`: the sizes then
    # come from the assembler's own end-minus-start, exactly as
    # rv_guest_image_size already does, and there is no third place for
    # the boundary to be computed differently.
    add_custom_command(
        OUTPUT "${bin}.ro" "${bin}.rw"
        COMMAND ${RISCV_OBJCOPY} -O binary
                -j .text -j .rodata "${elf}" "${bin}.ro"
        COMMAND ${RISCV_OBJCOPY} -O binary
                -j .data "${elf}" "${bin}.rw"
        # And prove it, because the paragraph above was wrong for two of
        # the four guests and nothing said so. The invariant is what lets
        # guest_image.S embed the halves consecutively and hand out one
        # base pointer, and what lets an uploaded image be reassembled in
        # flash the same way -- when it does not hold, .data is placed
        # below where the guest reads it and the guest runs anyway, on
        # zeros.
        COMMAND ${CMAKE_COMMAND} -E cat "${bin}.ro" "${bin}.rw" > "${bin}.cat"
        COMMAND ${CMAKE_COMMAND} -E compare_files "${bin}.cat" "${bin}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${bin}.cat"
        DEPENDS "${elf}" "${bin}"
        COMMENT "Splitting guest image ${name} at __guest_ro_end"
        VERBATIM)

    add_custom_target(guest-${name} ALL
        DEPENDS "${bin}" "${bin}.ro" "${bin}.rw")
endfunction()

add_guest_image(isatest  SOURCES start.S isatest.c)
add_guest_image(hello    SOURCES start.S hello.c)

# Peripheral drivers running as guest code, talking to real silicon
# through the passthrough window. Only meaningful on the STM32 target; on
# the host runner the peripheral window is backed by plain memory, so it
# runs but drives nothing.
add_guest_image(stm32drv SOURCES start.S stm32drv.c)

# A real peripheral interrupt taken by guest code. Like stm32drv, only
# meaningful on the board; on the host the peripheral window is plain
# memory, so it detects the absent timer and reports rather than hanging.
add_guest_image(irqtest SOURCES start.S irqtest.c)

# The CMSIS-Core shim, taking the same interrupt through the vendor API
# rather than through APLIC registers.
add_guest_image(cmsistest
    SOURCES start.S cmsistest.c cmsis/cmsis_rv32.c
    FLAGS -I "${guest_dir}/cmsis")

# Compute-bound workload for throughput measurement.
add_guest_image(bench    SOURCES start.S bench.c)

# Floating point, and deliberately no PMP: isatest arms one early, after
# which fetch_guard sends everything to the interpreter and no FP change
# can be measured on the JIT at all. See CLAUDE.md.
add_guest_image(fptest   SOURCES start.S fptest.c)

# Driver-shaped workload for the passthrough window. Unlike bench, this one
# only measures anything on the STM32: the host runner backs the peripheral
# window with plain memory, so both halves of each comparison take the same
# path and the ratio is 1 by construction.
add_guest_image(mmiobench SOURCES start.S mmiobench.c)

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

#
# netlib's Dhrystone 2.1, unmodified except for the run count -- see the
# note in dhry_portme.c, which supplies the libc it expects.
#
# TIMES, which is Dhrystone's default and is left selected by *not*
# defining TIME. The other path takes time()'s return value for a count of
# seconds and never references HZ at all, so a 1 MHz tick read straight out
# of mtime came back as 470016000 microseconds per run. Under TIMES the
# tick rate is declared instead, and HZ below is the CLINT's -- 1 MHz on
# every platform, which is what makes it a constant here.
set(DHRY_RUNS "20000" CACHE STRING "Dhrystone runs through the benchmark")

if(EXISTS "${guest_dir}/dhrystone/dhry_1.c")
    add_guest_image(dhrystone
        SOURCES start.S
        EXTRA_SOURCES
            "${guest_dir}/dhrystone/dhry_1.c"
            "${guest_dir}/dhrystone/dhry_2.c"
            "${guest_dir}/dhrystone/dhry_portme.c"
        FLAGS
            # First, so that <sys/times.h> and <sys/types.h> find the
            # shims in dhrystone/sys/ rather than the toolchain's -- which
            # is what lets dhry.h keep its unmodified includes.
            -I "${guest_dir}/dhrystone"
            -DHZ=1000000
            -DDHRY_RUNS=${DHRY_RUNS}
            -DREG=register
            #
            # gnu89, because these sources are 1988 K&R and a modern
            # default rejects them outright -- old-style definitions,
            # implicit int, and `extern long time();` with no parameter
            # list. Modernising them would mean editing the measured
            # code, which is the one thing a benchmark must not have done
            # to it, so the standard moves instead of the source.
            #
            -std=gnu89
            -Wno-implicit-fallthrough -Wno-implicit-int
            -Wno-builtin-declaration-mismatch -Wno-return-type
            -Wno-implicit-function-declaration
            #
            # libgcc, for the soft-float double routines. Dhrystone's
            # *reporting* arithmetic -- the DMIPS and per-second figures
            # at the end -- is float, and K&R promotion makes it double,
            # so it pulls in __muldf3 and friends. This emulator's FPU is
            # single-precision by design, so those are libgcc's.
            #
            # It does not touch the measurement: the timed loop is pure
            # integer, and this runs once after it. Worth stating,
            # because "the benchmark links soft-float" reads like it
            # would.
            #
        LIBS -lgcc)
endif()
