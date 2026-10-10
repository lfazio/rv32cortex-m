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

#
# **Before the RISC-V early return below, not after it.**
#
# That return skips the rest of this file when the RISC-V toolchain is
# absent, which is right for RV32 images and wrong for these: they are
# built with the ARM toolchain and have nothing to do with RISC-V.
# Placed after the return, an ARMv7-M-only tree builds no guest at all
# and nothing says so -- the same shape as the three RISC-V tests this
# tree once queued in a PowerPC-only build.
#
# hello.S is hand-written assembly and 16-bit only; alu.c is compiled C,
# which is what found three decode defects the hand-written one could
# not. Both go through the same link script and start-up, so a guest
# installs its own .data rather than depending on the loader.
#
if(EMU_GUEST_ARCH_ARMV7M)
    find_program(ARM_GCC arm-none-eabi-gcc)
    find_program(ARM_OBJCOPY arm-none-eabi-objcopy)
    if(ARM_GCC AND ARM_OBJCOPY)
        set(_m7_dir "${CMAKE_BINARY_DIR}/guest")
        set(_m7_src "${CMAKE_CURRENT_SOURCE_DIR}/armv7m")
        file(MAKE_DIRECTORY "${_m7_dir}")

        #
        # SOURCES are relative to tests/guest/armv7m; EXTRA_SOURCES are
        # absolute, for a benchmark whose code lives elsewhere. OPT is
        # -O1 unless a guest says otherwise: the test guests want the
        # compiler to leave their instructions recognisable, and a
        # benchmark wants what a product would ship.
        #
        function(add_armv7m_guest name)
            cmake_parse_arguments(A "" "OPT"
                                  "SOURCES;EXTRA_SOURCES;FLAGS;LIBS" ${ARGN})
            set(srcs "")
            foreach(f IN LISTS A_SOURCES)
                list(APPEND srcs "${_m7_src}/${f}")
            endforeach()
            list(APPEND srcs ${A_EXTRA_SOURCES})
            if(NOT A_OPT)
                set(A_OPT "-O1")
            endif()
            add_custom_command(
                OUTPUT "${_m7_dir}/armv7m-${name}.bin"
                COMMAND ${ARM_GCC} -mthumb -mcpu=cortex-m4 ${A_OPT} -g
                        -ffreestanding -nostdlib -fno-builtin -fno-common
                        -Wall -Wextra -I "${_m7_src}" ${A_FLAGS}
                        -T "${_m7_src}/guest.ld" ${srcs} ${A_LIBS}
                        -o "${_m7_dir}/armv7m-${name}.elf"
                COMMAND ${ARM_OBJCOPY} -O binary
                        "${_m7_dir}/armv7m-${name}.elf"
                        "${_m7_dir}/armv7m-${name}.bin"
                DEPENDS ${srcs} "${_m7_src}/guest.ld"
                COMMENT "Building ARMv7-M guest armv7m-${name}.bin"
                VERBATIM)
            add_custom_target(guest-armv7m-${name} ALL
                              DEPENDS "${_m7_dir}/armv7m-${name}.bin")
        endfunction()

        add_armv7m_guest(hello SOURCES hello.S)
        add_armv7m_guest(alu   SOURCES start.S alu.c)
        add_armv7m_guest(itblock SOURCES start.S itblock.c)
        add_armv7m_guest(nvic  SOURCES start.S nvic.c frame.S)

        #
        # **The Thumb-2 encoder, as a guest, executing what it emits.**
        #
        # This is the one that justifies the frontend: the backend's
        # emitters can only be executed on hardware today, and three of
        # its defects were live for months because no host suite could
        # reach them. Here the real `src/backend/thumb2/encode.c` is
        # compiled into a Cortex-M guest, so an x86 host runs it.
        #
        # It reaches outside tests/guest for that source, which is
        # deliberate: a copy would be a second encoder and would agree
        # with itself.
        #
        add_custom_command(
            OUTPUT "${_m7_dir}/armv7m-t2exec.bin"
            COMMAND ${ARM_GCC} -mthumb -mcpu=cortex-m4 -O1 -g
                    -ffreestanding -nostdlib -fno-builtin -fno-common
                    -Wall -Wextra
                    -I "${CMAKE_SOURCE_DIR}/include"
                    -DEMU_HOST_JIT_THUMB2=1
                    -T "${_m7_src}/guest.ld"
                    "${_m7_src}/start.S" "${_m7_src}/t2exec.c"
                    "${CMAKE_SOURCE_DIR}/src/backend/thumb2/encode.c"
                    -o "${_m7_dir}/armv7m-t2exec.elf"
            COMMAND ${ARM_OBJCOPY} -O binary
                    "${_m7_dir}/armv7m-t2exec.elf"
                    "${_m7_dir}/armv7m-t2exec.bin"
            DEPENDS "${_m7_src}/t2exec.c" "${_m7_src}/start.S"
                    "${_m7_src}/guest.ld"
                    "${CMAKE_SOURCE_DIR}/src/backend/thumb2/encode.c"
            COMMENT "Building ARMv7-M guest armv7m-t2exec.bin"
            VERBATIM)
        add_custom_target(guest-armv7m-t2exec ALL
                          DEPENDS "${_m7_dir}/armv7m-t2exec.bin")
    endif()
endif()

#
# PowerPC, compiled. Before the early return for the same reason as the
# ARMv7-M guests above, and *classic Book E*, because GCC has no VLE
# code generator -- so these are the only guests that exercise that half
# of the e200z7 decoder, and the assembly guest in ppc/ is the only one
# that exercises VLE.
#
# They are run as ELF. The frontend picks its decoder from the image's
# PF_PPC_VLE segment flag, and a raw binary has nowhere to carry one.
#
# -mcpu=8540 is the nearest thing GCC has to an e200: Book E, isel, no
# classic FPU with -msoft-float, and none of the POWER-only forms
# (mfocrf, popcntb) that the e200 decodes as illegal. -msdata=none keeps
# r2 and r13 out of it, so crt0 has no small-data bases to set up.
#
# There is no 32-bit libgcc beside a powerpc64 cross compiler, so a
# guest cannot use 64-bit division or soft-float: Dhrystone, whose
# report is in doubles, is not built for this frontend.
#
if(EMU_GUEST_ARCH_PPC)
    find_program(PPC_GCC NAMES powerpc-linux-gnu-gcc powerpc64-linux-gnu-gcc
                               powerpc-eabi-gcc)
    if(PPC_GCC)
        set(_ppc_dir "${CMAKE_BINARY_DIR}/guest")
        set(_ppc_src "${CMAKE_CURRENT_SOURCE_DIR}/ppc")
        file(MAKE_DIRECTORY "${_ppc_dir}")

        function(add_ppc_guest name)
            cmake_parse_arguments(A "" "OPT" "SOURCES;EXTRA_SOURCES;FLAGS" ${ARGN})
            set(srcs "${_ppc_src}/crt0.S")
            foreach(f IN LISTS A_SOURCES)
                list(APPEND srcs "${_ppc_src}/${f}")
            endforeach()
            list(APPEND srcs ${A_EXTRA_SOURCES})
            if(NOT A_OPT)
                set(A_OPT "-O1")
            endif()
            add_custom_command(
                OUTPUT "${_ppc_dir}/ppc-${name}.elf"
                COMMAND ${PPC_GCC} -m32 -mbig-endian -mcpu=8540 -msoft-float
                        -msdata=none ${A_OPT} -g
                        -ffreestanding -nostdlib -fno-builtin -fno-common
                        -fno-pic -static -Wl,--build-id=none
                        -Wall -Wextra -I "${_ppc_src}" ${A_FLAGS}
                        -T "${_ppc_src}/cguest.ld" ${srcs}
                        -o "${_ppc_dir}/ppc-${name}.elf"
                DEPENDS ${srcs} "${_ppc_src}/cguest.ld" "${_ppc_src}/ppc_clock.h"
                COMMENT "Building PowerPC guest ppc-${name}.elf"
                VERBATIM)
            add_custom_target(guest-ppc-${name} ALL
                              DEPENDS "${_ppc_dir}/ppc-${name}.elf")
        endfunction()

        add_ppc_guest(alu SOURCES alu.c)
    else()
        message(STATUS "No PowerPC compiler: the compiled e200z7 guests will not be built")
    endif()
endif()

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

    add_custom_target(guest-${name} ALL DEPENDS "${bin}")
endfunction()

#
# isatest is told how the *emulator* was configured, because one of the
# behaviours it checks is a build option: whether a misaligned access
# completes or raises. The architecture permits either, so the test has
# to assert what this build chose -- and passing the flag here is what
# keeps the two from drifting.
#
add_guest_image(isatest  SOURCES start.S isatest.c
                FLAGS -DRV32_MISALIGNED=$<BOOL:${RV32_MISALIGNED}>)
add_guest_image(hello    SOURCES start.S hello.c)

#
# The framebuffer from the guest's side, through the bus at the addresses
# a real guest uses. The unit test drives the device's ops table
# directly; this proves the path is *reachable*, which is a different
# claim and the one this project has most often found missing.
#
# DOOM, fetched rather than vendored -- it is GPL-2.0 and this tree is
# Apache-2.0. See doom.cmake for the whole arrangement.
include(${CMAKE_CURRENT_SOURCE_DIR}/doom.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/quake.cmake)

set(FBTEST_FRAMES "60" CACHE STRING "frames the fbtest guest draws")
add_guest_image(fbtest   SOURCES start.S fbtest.c
    FLAGS -DFBTEST_FRAMES=${FBTEST_FRAMES}u)

#
# The A extension, as a guest the JIT actually sees.
#
# isatest covers the same instructions and cannot guard their *lowering*:
# it arms PMP early, and the JIT is gated on fetch_guard, so every atomic
# in it is interpreted whatever the translator can do. This one arms
# nothing and stays in M-mode.
#
set(ATOMIC_ROUNDS "2000" CACHE STRING "atomic iterations in the atomics guest")
add_guest_image(atomics
    SOURCES start.S atomics.c
    FLAGS -DATOMIC_ROUNDS=${ATOMIC_ROUNDS})

# Peripheral drivers running as guest code, talking to real silicon
# through the passthrough window. Only meaningful on the STM32 target; on
# the host runner the peripheral window is backed by plain memory, so it
# runs but drives nothing.
add_guest_image(stm32drv SOURCES start.S stm32drv.c)

# A real peripheral interrupt taken by guest code. Like stm32drv, only
# meaningful on the board; on the host the peripheral window is plain
# memory, so it detects the absent timer and reports rather than hanging.
add_guest_image(irqtest SOURCES start.S irqtest.c)
#
# Drives a virtio queue to completion and waits for the interrupt. Needs
# the emulator started with --virtio-console; without one it reports
# that and exits rather than hanging.
#
add_guest_image(virtiotest SOURCES start.S virtiotest.c)

# The CMSIS-Core shim, taking the same interrupt through the vendor API
# rather than through APLIC registers.
add_guest_image(cmsistest
    SOURCES start.S cmsistest.c cmsis/cmsis_rv32.c
    FLAGS -I "${guest_dir}/cmsis")

# Compute-bound workload for throughput measurement.
add_guest_image(bench    SOURCES start.S bench.c)

# What the inlined memory path is worth, and what turns it off.
#
# Two images from one source, differing in one -D: the kernel between
# the markers is identical and only the privilege and the translation
# change. `rv_ir_fast_mem` inlines guest RAM accesses in the first and
# declines in the second -- for *two* reasons, since `pmp_active` is set
# by privilege alone and is true for anything below M-mode whether or
# not a PMP entry exists. See the file header.
add_guest_image(sv32bench-bare
    SOURCES start.S sv32bench.c FLAGS -DSV32BENCH_PAGING=0)
add_guest_image(sv32bench-paged
    SOURCES start.S sv32bench.c FLAGS -DSV32BENCH_PAGING=1)

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
    #
    # The same benchmark as an ARMv7-M guest, for the one question the
    # generated cases cannot answer: is translated code *faster*. They
    # say whether each instruction is right; this is thousands of them
    # in the proportions a compiler emits, with a SysTick interrupt
    # landing every ten thousand.
    #
    #
    # Its own iteration count, and not COREMARK_ITERATIONS: that one is
    # sized for a microcontroller running an interpreter, and at its
    # default of 2 CoreMark declares the run invalid -- "Must execute for
    # at least 10 secs" -- and prints "Errors detected". This guest's
    # second is a million instructions, so forty iterations is eleven of
    # them and a fraction of a real one on a host.
    #
    set(ARMV7M_COREMARK_ITERATIONS "40" CACHE STRING
        "CoreMark iterations in the ARMv7-M guest")
    if(EMU_GUEST_ARCH_ARMV7M AND ARM_GCC AND ARM_OBJCOPY)
        add_armv7m_guest(coremark
            OPT -O2
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
                -DITERATIONS=${ARMV7M_COREMARK_ITERATIONS}
                -Wno-unused-parameter -Wno-sign-compare
                -Wno-implicit-fallthrough)
    endif()
    # And for the e200z7, with the time base for a clock.
    if(EMU_GUEST_ARCH_PPC AND PPC_GCC)
        add_ppc_guest(coremark
            OPT -O2
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
                -DITERATIONS=${ARMV7M_COREMARK_ITERATIONS}
                -Wno-unused-parameter -Wno-sign-compare
                -Wno-implicit-fallthrough)
    endif()
else()
    message(STATUS "CoreMark source not available; skipping the coremark image")
endif()

# ---------------------------------------------------------------------
# TinyCrypt: AES-128 and SHA-256
# ---------------------------------------------------------------------
#
# Intel's TinyCrypt, BSD-3-Clause, fetched rather than vendored -- same
# arrangement as CoreMark.
#
# **This is the only guest that executes bit manipulation.** A trace
# histogram of CoreMark, Dhrystone and bench over 400,000 sampled
# instructions each found zero rotations, bit-extracts or
# count-leading-zeros: compilers emit shifts and masks for ordinary C, and
# the Zbb lowerings had nothing to measure against. SHA-256 is built from
# rotations and AES from byte permutes, so this is what makes them visible.
#
# It checks itself against FIPS-197 appendix B and FIPS-180-2 before it
# measures anything, which matters more here than elsewhere: a crypto
# primitive fails catastrophically, so one wrong bit in a rotation
# changes every byte downstream and cannot be mistaken for noise.
set(TINYCRYPT_DIR "${CMAKE_BINARY_DIR}/tinycrypt-src"
    CACHE PATH "TinyCrypt source")
set(CRYPTO_ROUNDS "200" CACHE STRING "AES+SHA rounds in the crypto guest")

if(NOT EXISTS "${TINYCRYPT_DIR}/lib/source/aes_encrypt.c")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Fetching TinyCrypt into ${TINYCRYPT_DIR}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 -q
                    https://github.com/intel/tinycrypt.git "${TINYCRYPT_DIR}"
            RESULT_VARIABLE _tc_rc)
    endif()
endif()

if(EXISTS "${TINYCRYPT_DIR}/lib/source/aes_encrypt.c")
    add_guest_image(crypto
        SOURCES start.S crypto.c
        EXTRA_SOURCES
            "${TINYCRYPT_DIR}/lib/source/aes_encrypt.c"
            "${TINYCRYPT_DIR}/lib/source/aes_decrypt.c"
            "${TINYCRYPT_DIR}/lib/source/sha256.c"
            "${TINYCRYPT_DIR}/lib/source/utils.c"
        FLAGS
            -I "${TINYCRYPT_DIR}/lib/include"
            -DCRYPTO_ROUNDS=${CRYPTO_ROUNDS}
            -Wno-sign-compare -Wno-unused-parameter)
    #
    # The same guest for ARMv7-M. It needs nothing but a console, and it
    # is the harshest of the compiled guests on a translator: one wrong
    # bit in a rotation or a carry changes every byte downstream, and it
    # checks itself against FIPS-197 and FIPS-180-2 before it reports.
    #
    if(EMU_GUEST_ARCH_ARMV7M AND ARM_GCC AND ARM_OBJCOPY)
        add_armv7m_guest(crypto
            OPT -O2
            SOURCES start.S
            EXTRA_SOURCES
                "${guest_dir}/crypto.c"
                "${TINYCRYPT_DIR}/lib/source/aes_encrypt.c"
                "${TINYCRYPT_DIR}/lib/source/aes_decrypt.c"
                "${TINYCRYPT_DIR}/lib/source/sha256.c"
                "${TINYCRYPT_DIR}/lib/source/utils.c"
            FLAGS
                -I "${TINYCRYPT_DIR}/lib/include"
                -DCRYPTO_ROUNDS=${CRYPTO_ROUNDS}
                -Wno-sign-compare -Wno-unused-parameter)
    endif()
    if(EMU_GUEST_ARCH_PPC AND PPC_GCC)
        add_ppc_guest(crypto
            OPT -O2
            EXTRA_SOURCES
                "${guest_dir}/crypto.c"
                "${TINYCRYPT_DIR}/lib/source/aes_encrypt.c"
                "${TINYCRYPT_DIR}/lib/source/aes_decrypt.c"
                "${TINYCRYPT_DIR}/lib/source/sha256.c"
                "${TINYCRYPT_DIR}/lib/source/utils.c"
            FLAGS
                -I "${TINYCRYPT_DIR}/lib/include"
                -DCRYPTO_ROUNDS=${CRYPTO_ROUNDS}
                -Wno-sign-compare -Wno-unused-parameter)
    endif()
else()
    message(STATUS "TinyCrypt source not available; skipping the crypto image")
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

    # And as an ARMv7-M guest, with SysTick for a clock.
    if(EMU_GUEST_ARCH_ARMV7M AND ARM_GCC AND ARM_OBJCOPY)
        add_armv7m_guest(dhrystone
            OPT -O2
            SOURCES start.S
            EXTRA_SOURCES
                "${guest_dir}/dhrystone/dhry_1.c"
                "${guest_dir}/dhrystone/dhry_2.c"
                "${guest_dir}/dhrystone/dhry_portme.c"
            FLAGS
                -I "${guest_dir}/dhrystone"
                -DHZ=1000000
                -DDHRY_RUNS=${DHRY_RUNS}
                -DREG=register
                -std=gnu89
                -Wno-implicit-fallthrough -Wno-implicit-int
                -Wno-builtin-declaration-mismatch -Wno-return-type
                -Wno-implicit-function-declaration
                -Wno-unused-parameter -Wno-old-style-definition
                -Wno-missing-prototypes -Wno-sign-compare
            LIBS -lgcc)
    endif()
endif()

# ---------------------------------------------------------------------
# Whetstone
# ---------------------------------------------------------------------

#
# The floating-point counterpart to Dhrystone and to CoreMark, both of
# which are integer. Vendored from kreier/benchmark's single-precision
# Arduino port -- single precision because that is the FPU this emulator
# has, on the host and on the M4F/M7 alike; the double-precision original
# would have measured libgcc's soft float instead.
#
# LOOP: upstream defaults to 1000, which is 100 million Whetstone
# instructions -- 800 million guest instructions, and more than an
# emulated core wants to run. 100 is a tenth of that: 86 million guest
# instructions, 2.0 s on the host interpreter and 0.4 s on the JIT.
#
# **The rate is not LOOP-invariant, so any figure has to name its LOOP.**
# Measured here: 113.6 KIPS at 10, 116.3 at 100, 125.1 at 1000. That is
# not a warm-up artefact. Modules 7 and 11 -- the trigonometric and the
# sqrt/exp/log one -- are 99.4% of the instruction count between them
# (confirmed by building with each N zeroed and differencing), and both
# get cheaper per iteration as their inputs converge: module 11's X rises
# toward 1.0, where logf's argument reduction has least to do. So a
# longer run is not simply a more accurate measurement of the same thing.
set(WHET_LOOPS "100" CACHE STRING "Whetstone LOOP count (100 = 10M Whetstone instructions)")

# Upstream's per-module reports. Off by default because upstream says
# timing runs want them off, and on because they are the only correctness
# check this benchmark has: the digest is what gets compared between
# backends, between frontends and between the host and the board. There
# is nothing to compare it against in advance -- see the note at the top
# of whetstone.c about the expected value that is not 1.0.
option(WHET_PRINTOUT "Whetstone: print each module's result" OFF)
if(WHET_PRINTOUT)
    set(_whet_printout -DPRINTOUT=1)
else()
    set(_whet_printout "")
endif()

if(EXISTS "${guest_dir}/whetstone/whetstone.c")
    add_guest_image(whetstone
        SOURCES start.S
        EXTRA_SOURCES
            "${guest_dir}/whetstone/whetstone.c"
            "${guest_dir}/whetstone/whetstone_portme.c"
        FLAGS
            -DWHET_LOOPS=${WHET_LOOPS}
            ${_whet_printout}
            #
            # **The hard-float ABI, and without it this benchmark does
            # not measure the FPU at all.**
            #
            # RV_GUEST_MABI is ilp32 for the reason stated at the top of
            # this file -- images built with and without F stay
            # interchangeable. But the ABI also picks the multilib, and
            # rv32imac/ilp32's libm is compiled *soft-float*: every
            # sinf, logf and float multiply in Whetstone becomes
            # __mulsf3 and friends. Sampling 200,002 instructions from
            # the middle of a run found **zero** FP instructions.
            #
            # So the FP benchmark was measuring libgcc's integer soft
            # float. ilp32f selects rv32imafc/ilp32f, whose libm uses
            # the F extension, which is what makes the number mean
            # something -- and it is why this is the one image that
            # overrides the shared ABI.
            #
            -mabi=ilp32f
            #
            # Modules 7 and 11 are the transcendental functions, so they
            # come from newlib rather than from anything written here --
            # a hand-rolled sinf would be what a third of the benchmark
            # measured. On this multilib they live in libc.a: libm.a
            # holds one empty object, which is why -lm is not what the
            # link names.
            #
        LIBS -lc -lgcc)
endif()
