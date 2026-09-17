# SPDX-License-Identifier: Apache-2.0
#
# Which guest image a firmware carries, and staging it where .incbin can
# reach it.
#
# **The image has to follow the frontend, and it did not.** Both boards
# baked `${RV32_GUEST}` whatever was compiled in, so a G4MH-only build --
# `-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON` -- carried a
# RISC-V binary and ran it through the RH850 decoder. That does not fail:
# the firmware boots, prints its banner, reports `backend jit-ir-thumb2`,
# and then produces nothing, because the decoder is being fed a program
# in another instruction set. It reads as the frontend being broken.
#
# The two frontends supply their images differently, which is why this is
# a function rather than a variable:
#
#   RV32   built here, by tests/guest, so the staging depends on a target
#          *and* on the file. Both are needed -- depending on the target
#          alone is an ordering constraint only, and that is how the copy
#          once ran a single time and left every later build carrying the
#          previous guest with nothing failing.
#
#   G4MH   checked in prebuilt, because it is compiled by Renesas CC-RH
#          and this tree cannot assume that exists. So there is no target
#          to depend on, only the file.

function(emu_stage_guest_image out_bin)
    #
    # The copy lands in *this* directory's binary dir on purpose. A
    # cross-directory file dependency is where the two generators
    # diverge: OBJECT_DEPENDS on a path in another directory's scope
    # leaves the Makefile generator with "No rule to make target".
    #
    set(guest_bin "${CMAKE_CURRENT_BINARY_DIR}/guest_image.bin")

    #
    # An image from outside the tree, named by path.
    #
    # For anything this build does not produce -- OpenSBI's fw_jump.bin,
    # a kernel, a binary from another toolchain. There is no target to
    # depend on, only the file, exactly as for the prebuilt G4MH guest
    # below; and it is the *file* that is named, because a target-only
    # dependency is an ordering constraint and that is how one build came
    # to carry the previous guest with nothing failing.
    #
    # A path, not a name, so nothing here has to guess where it came
    # from:
    #
    #   -DEMU_GUEST_BIN=/path/to/fw_jump.bin
    #
    if(EMU_GUEST_BIN)
        if(NOT EXISTS "${EMU_GUEST_BIN}")
            message(FATAL_ERROR "EMU_GUEST_BIN does not exist: ${EMU_GUEST_BIN}")
        endif()
        set(_src "${EMU_GUEST_BIN}")
        set(_dep "${EMU_GUEST_BIN}")
        get_filename_component(_name "${EMU_GUEST_BIN}" NAME)
    elseif(EMU_GUEST_ARCH_RV32)
        set(RV32_GUEST "isatest"
            CACHE STRING "Guest image to embed in the firmware")
        set(_src "${CMAKE_BINARY_DIR}/guest/${RV32_GUEST}.bin")
        set(_dep "${_src}" "guest-${RV32_GUEST}")
        set(_name "${RV32_GUEST}.bin")
    elseif(EMU_GUEST_ARCH_G4MH)
        set(G4MH_GUEST "guest"
            CACHE STRING "G4MH guest image to embed in the firmware")
        set(_src "${CMAKE_SOURCE_DIR}/tests/guest/g4mh/${G4MH_GUEST}.bin")
        set(_dep "${_src}")
        set(_name "g4mh/${G4MH_GUEST}.bin")

        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR
                "G4MH guest image ${_src} does not exist. These are "
                "checked in prebuilt -- CC-RH is not assumed -- so a "
                "missing one is a wrong G4MH_GUEST rather than a missing "
                "toolchain.")
        endif()
    else()
        #
        # Loud, because the alternative is a firmware that links against
        # whatever guest_image.bin happens to be left in the build
        # directory from a previous configuration.
        #
        message(FATAL_ERROR
            "No frontend selected, so there is no guest image to embed.")
    endif()

    add_custom_command(
        OUTPUT "${guest_bin}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${guest_bin}"
        DEPENDS ${_dep}
        COMMENT "Staging guest image ${_name}"
        VERBATIM)
    add_custom_target(guest-image-staged DEPENDS "${guest_bin}")

    set(${out_bin} "${guest_bin}" PARENT_SCOPE)
endfunction()
