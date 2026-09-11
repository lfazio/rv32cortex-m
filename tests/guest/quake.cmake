# SPDX-License-Identifier: Apache-2.0
#
# Build Quake as a guest image.
#
# **Fetched, never vendored**, on the same terms as CoreMark, TinyCrypt
# and DOOM beside it, and for the same reason it matters here: the port
# is GPL-2.0 and this tree is Apache-2.0. Nothing of Quake's enters this
# repository, and this file is not a derivative of it -- it is a build
# recipe, and it describes rather than contains.
#
# The board layer Quake needs -- the framebuffer, the two input devices,
# the timebase and a file system that is one compiled-in PAK -- lives in
# the port itself, as `port/boards/rv32cortexm/`, because a file
# implementing that port's interface and linking into its binary is part
# of that GPL work. The emulator side needs nothing Quake-shaped: its
# devices are generic, and a framebuffer with anything of a game in it
# would be the wrong device.
#
#   -DEMU_QUAKE=ON
#
# Off by default. The fetch is large and the game data larger still.

option(EMU_QUAKE "fetch and build Quake as a guest image" OFF)

if(NOT EMU_QUAKE)
    return()
endif()

set(QUAKE_REPO "https://github.com/lfazio/quake-embedded.git"
    CACHE STRING "Quake port to build")
set(QUAKE_BRANCH "master" CACHE STRING "branch carrying the board")
set(QUAKE_DIR "${CMAKE_BINARY_DIR}/quake-src" CACHE PATH "Quake source")

#
# The game data, which this repository cannot carry and does not fetch.
#
# pak0.pak from the Quake shareware distribution is what the engine
# loads; id Software made it freely redistributable, and it is still not
# ours to vendor or to download on someone's behalf. Point this at a
# copy.
#
set(QUAKE_PAK "" CACHE FILEPATH "path to pak0.pak (shareware is enough)")

if(NOT EXISTS "${QUAKE_DIR}/winquake/host.c")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Fetching Quake into ${QUAKE_DIR}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 -b "${QUAKE_BRANCH}"
                    -q "${QUAKE_REPO}" "${QUAKE_DIR}"
            RESULT_VARIABLE _quake_rc)
    endif()
endif()

if(NOT EXISTS "${QUAKE_DIR}/port/boards/rv32cortexm/display.c")
    message(STATUS
        "Quake source or its rv32cortex-m board is not available; "
        "skipping the quake image")
    return()
endif()

#
# The PAK, as a C array.
#
# Generated rather than carried, and *named* as the missing thing when
# it is absent: "cannot open pak0.pak" from a linker three steps later
# is how a first-time builder ends up reading this file to find out what
# it wanted. The guest has no file system, so the data is in the image;
# see the board's fio.c.
#
set(_quake_pak_c "${CMAKE_CURRENT_BINARY_DIR}/quake_pak.c")

if(NOT QUAKE_PAK)
    message(STATUS
        "Quake: no game data -- set -DQUAKE_PAK=/path/to/pak0.pak "
        "(the shareware pak is enough); skipping the quake image")
    return()
endif()

if(NOT EXISTS "${QUAKE_PAK}")
    message(FATAL_ERROR "Quake: QUAKE_PAK is set to ${QUAKE_PAK}, which "
                        "does not exist")
endif()

#
# By file timestamp, not by target: this tree has already been caught
# once by a guest image that kept the previous data because the
# dependency named a target rather than a file.
#
add_custom_command(
    OUTPUT "${_quake_pak_c}"
    COMMAND ${CMAKE_COMMAND}
            -DIN=${QUAKE_PAK} -DOUT=${_quake_pak_c}
            -DSYM=quake_pak
            -P "${CMAKE_CURRENT_SOURCE_DIR}/bin2c.cmake"
    DEPENDS "${QUAKE_PAK}" "${CMAKE_CURRENT_SOURCE_DIR}/bin2c.cmake"
    COMMENT "Embedding ${QUAKE_PAK}"
    VERBATIM)

#
# What to compile.
#
# The engine's own list lives in winquake/CMakeLists.txt and is not
# copied here -- that would be the one part of this file that *was* a
# derivative. Globbing and excluding gives the same answer: the five
# files it leaves out are all alternative network drivers, and the port
# selects one of them (net_loop) in the list it does build.
#
file(GLOB _quake_srcs
     "${QUAKE_DIR}/winquake/*.c"
     "${QUAKE_DIR}/port/*.c"
     "${QUAKE_DIR}/port/boards/rv32cortexm/*.c")

#
# net_none.c is **kept**, and it is the one that looks like it should go:
# the name reads as "no networking, omit it" and it is in fact the
# driver *table* for a build with none -- net_drivers[] with the
# loopback in it, which net_main.c indexes unconditionally. Dropping it
# links cleanly right up to `undefined reference to net_drivers'.
#
list(FILTER _quake_srcs EXCLUDE REGEX
     "/(net_bsd|net_dgrm|net_udp|net_vcr)\\.c$")

set(_quake_flags
    #
    # Quake is from 1996 and has the same dialect problems DOOM does:
    # implicit int, implicit declarations, and int/pointer conversion in
    # both directions were legal then and are errors in GCC 14.
    #
    -std=gnu99
    -fpermissive
    #
    # **-fsigned-char, for the reason DOOM needed it.** Plain char is
    # signed on x86 and unsigned on RISC-V and ARM, and this code was
    # written for the former. It has not been shown to bite here, and
    # that is exactly why it is set: the DOOM port lost a session to it
    # presenting as mis-mapped input rather than as arithmetic.
    #
    -fsigned-char
    -DWINQUAKE_ENABLE_LOGGING
    -DWINQUAKE_LOGGING_EXTERNAL
    -fno-common
    -I "${QUAKE_DIR}/include"
    -I "${QUAKE_DIR}/winquake"
    -w)

#
# Linked against the C library rather than -nostdlib, as DOOM is: the
# engine uses malloc, sprintf, the string functions and libm across
# dozens of files, and writing those is a libc rather than a port.
#
add_guest_image(quake
    SOURCES start.S
    EXTRA_SOURCES ${_quake_srcs} ${_quake_pak_c}
    FLAGS ${_quake_flags}
    LIBS -lm -lc -lgcc)
