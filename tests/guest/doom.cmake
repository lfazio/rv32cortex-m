# SPDX-License-Identifier: Apache-2.0
#
# Build DOOM as a guest image.
#
# **Fetched, never vendored**, which is how CoreMark and TinyCrypt are
# handled beside it and matters more here: DOOM is GPL-2.0 and this tree
# is Apache-2.0. Nothing of DOOM's enters this repository, and this file
# is not a derivative of it -- it is a build recipe, and it describes
# rather than contains.
#
# The platform layer DOOM needs -- `i_video_rv32.c`, which drives the
# framebuffer, the two input devices and the CLINT -- lives in the DOOM
# port itself, because a file implementing DOOM's interface and linking
# into DOOM's binary is part of that GPL work. The emulator side needs
# nothing DOOM-shaped: its devices are generic, and a framebuffer with
# anything of a game in it would be the wrong device.
#
#   -DEMU_DOOM=ON
#
# Off by default. The fetch is large -- the shareware WAD is compiled in
# as a C array of about 25 MB -- and a checkout that does not ask for
# DOOM should not pay for it.

option(EMU_DOOM "fetch and build DOOM as a guest image" OFF)

if(NOT EMU_DOOM)
    return()
endif()

set(DOOM_REPO "https://github.com/lfazio/embeddedDOOM.git"
    CACHE STRING "DOOM port to build")
set(DOOM_BRANCH "rv32cortex-m"
    CACHE STRING "branch carrying the rv32 platform layer")
set(DOOM_DIR "${CMAKE_BINARY_DIR}/doom-src" CACHE PATH "DOOM source")

if(NOT EXISTS "${DOOM_DIR}/src/d_main.c")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Fetching DOOM into ${DOOM_DIR}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 -b "${DOOM_BRANCH}"
                    -q "${DOOM_REPO}" "${DOOM_DIR}"
            RESULT_VARIABLE _doom_rc)
    endif()
endif()

if(NOT EXISTS "${DOOM_DIR}/src/i_video_rv32.c")
    message(STATUS
        "DOOM source or its rv32 platform layer is not available; "
        "skipping the doom image")
    return()
endif()

#
# What to compile.
#
# A glob with exclusions rather than the Makefile's list, because that
# list is DOOM's and copying it here would be the one part of this file
# that *was* a derivative -- and because a port that gains a file should
# not need this edited.
#
# The exclusions are all platform layers that do not apply here. Only one
# video driver may be linked: they define the same nine functions, and
# picking two is a duplicate-symbol error rather than a choice. i_net.c
# and i_sound.c reach for sys/socket.h and linux/soundcard.h, which a
# bare-metal guest has not got -- and the port already replaces what they
# provide with the STUB_NET and STUB_SOUND macros in its stubs.h, so
# excluding them removes files whose contents were already unused.
# os_generic.c goes for the same reason one step further: it is a
# threading and timing shim wanting pthread.h, and nothing outside it
# calls any of the helpers it defines.
#
#
# support/rawwad.c is the shareware WAD as a C array and is not optional
# -- w_wad.c reads the game's data straight out of it, so leaving it out
# links cleanly right up to `undefined reference to rawwad`. The rest of
# support/ is host tooling that builds the array in the first place and
# must not be compiled into a guest.
#
file(GLOB _doom_srcs "${DOOM_DIR}/src/*.c")
list(APPEND _doom_srcs "${DOOM_DIR}/src/support/rawwad.c")
list(FILTER _doom_srcs EXCLUDE REGEX
     "/(i_video|i_video_console|XDriver|i_net|i_sound|os_generic)\\.c$")

#
# DOOM is from 1993 and does not compile as C23: `typedef enum {false,
# true} boolean;` is an error once `false` is a keyword. gnu99 is the
# newest standard it builds under, and saying so here is better than
# discovering it as a syntax error in someone else's code.
#
# -Wno-* rather than -Wall: this is not our code to tidy, and warnings
# nobody will act on train people to ignore the ones that matter.
#
set(_doom_flags
    -std=gnu99
    #
    # **-fpermissive, and it is not laziness.** GCC 14 turned four
    # things this code does on every page into errors rather than
    # warnings: implicit int, implicit function declarations, and
    # int/pointer conversion in both directions. They were legal C in
    # 1993 and the code is not ours to modernise -- 43 errors in a first
    # build, none of them a defect in DOOM as it was written. This
    # switch is exactly the one GCC documents for the purpose.
    #
    -fpermissive
    #
    # **Not -DLINUX.** It looks right and is not: DOOM uses it to reach
    # for values.h, a legacy Unix header newlib does not have, and the
    # #else branch beside that include defines the same constants
    # itself. Defining it costs a header that does not exist to gain
    # nothing.
    #
    -DNORMALUNIX
    #
    # Selects the WAD that is *present*. w_wad.c picks between
    # support/rawwad.h and support/rawwad_use.h, and only the first
    # exists in a fresh checkout -- the second is produced by the port's
    # own shrinking tools. The name reads like a code generator and is
    # not: it chooses a header.
    #
    -DGENERATE_BAKED
    #
    # alloca is a compiler builtin rather than a library function, and
    # DOOM calls it without declaring it -- which older toolchains
    # resolved for free and this one leaves to the linker.
    #
    -Dalloca=__builtin_alloca
    -I "${DOOM_DIR}/src"
    -w)

#
# Linked against newlib rather than -nostdlib, unlike every other guest
# here. DOOM uses malloc, sprintf and the string functions across
# twenty-odd files, and writing those is a libc, not a port. The
# multilib for this march exists, which is what makes it possible:
#   riscv64-unknown-elf/lib/rv32imafc/ilp32f/libc.a
#
# Its syscall stubs are the port's, in the DOOM repo beside the video
# driver, because what `_write` should do is a property of the guest
# rather than of the emulator.
#
add_guest_image(doom
    SOURCES start.S
    EXTRA_SOURCES ${_doom_srcs}
    FLAGS ${_doom_flags}
    LIBS -lc -lgcc)
