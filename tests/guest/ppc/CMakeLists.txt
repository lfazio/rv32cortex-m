# SPDX-License-Identifier: Apache-2.0
#
# The e200z7 guest image.
#
# Its own file rather than a block in tests/guest/CMakeLists.txt because
# that one is about RISC-V and returns early when the RISC-V toolchain is
# missing -- a PowerPC guest hanging off the end of it would be skipped
# for the wrong reason, and silently.
#
# Built from source, unlike the G4MH guest beside it: that one is a
# checked-in .bin because Renesas CC-RH is not something a checkout can
# assume, whereas Debian's binutils-powerpc-linux-gnu is one apt install
# and is already what scripts/ppc-check-encodings.sh needs.
#
# **Assembly, not C, and not by choice.** powerpc-linux-gnu-gcc is not
# built with VLE support -- `-mvle` is rejected -- so the compiled-guest
# arrangement that found three G4MH defects is unavailable here. The
# assembler and objdump do handle VLE, so this still gets the important
# half: an encoder that is not this emulator's decoder.

#
# powerpc64-linux-gnu is the same binutils driven in 32-bit mode, and it
# is what Debian ships now: binutils-powerpc-linux-gnu went away, the
# cached path to it kept configuring, and a fresh tree printed "not
# found" and quietly stopped building -- and so testing -- this guest.
# -a32 and -m elf32ppc are what make the 64-bit tools emit this image;
# both are harmless to the 32-bit tools, so one set of flags serves both.
#
#
# **And an old tree has to notice too.** find_program does not look again
# once its result is cached, so a build directory configured while
# powerpc-linux-gnu-as existed went on naming it after the package was
# gone -- and failed at build time with "not found", in the one row of
# the build matrix that had been configured longest. A cached path that
# no longer exists is forgotten here, so the search below runs again.
#
foreach(_tool PPC_AS PPC_LD PPC_OBJCOPY)
    if(${_tool} AND NOT EXISTS "${${_tool}}")
        unset(${_tool} CACHE)
    endif()
endforeach()

find_program(PPC_AS      NAMES powerpc-linux-gnu-as powerpc-eabi-as
                               powerpc64-linux-gnu-as)
find_program(PPC_LD      NAMES powerpc-linux-gnu-ld powerpc-eabi-ld
                               powerpc64-linux-gnu-ld)
find_program(PPC_OBJCOPY NAMES powerpc-linux-gnu-objcopy powerpc-eabi-objcopy
                               powerpc64-linux-gnu-objcopy)

if(NOT PPC_AS OR NOT PPC_LD OR NOT PPC_OBJCOPY)
    message(STATUS "PowerPC binutils not found: the e200z7 guest will not be built")
    return()
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/isatest.S")
set(_ld  "${CMAKE_CURRENT_SOURCE_DIR}/guest.ld")
set(_out "${CMAKE_CURRENT_BINARY_DIR}")

# -mregnames so the source may write %r3 rather than 3, which is what
# makes a wrong register a syntax error instead of a different
# instruction.
add_custom_command(
    OUTPUT "${_out}/isatest.bin"
    COMMAND "${PPC_AS}" -a32 -mbig -mvle -mregnames -o "${_out}/isatest.o" "${_src}"
    COMMAND "${PPC_LD}" -m elf32ppc -T "${_ld}" -o "${_out}/isatest.elf" "${_out}/isatest.o"
    COMMAND "${PPC_OBJCOPY}" -O binary "${_out}/isatest.elf" "${_out}/isatest.bin"
    # Both named, because DEPENDS on a target alone is ordering and not
    # staleness -- the trap this project already paid for once, when a
    # staged guest image stopped tracking its own source.
    DEPENDS "${_src}" "${_ld}"
    COMMENT "Assembling the e200z7 guest (VLE)"
    VERBATIM)

add_custom_target(guest-ppc-isatest ALL DEPENDS "${_out}/isatest.bin")
