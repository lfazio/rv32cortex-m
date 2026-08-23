# SPDX-License-Identifier: Apache-2.0
#
# CoreMark compiled for the ARM host, instead of for the guest.
#
# The baseline the emulated figure is read against: the *same* CoreMark
# sources the guest runs, built for the part the emulator runs on. Without
# it "8ms on the F746" is a number with nothing to divide by.
#
#   include(${CMAKE_SOURCE_DIR}/cmake/coremark_native.cmake)
#   emu_coremark_native_attach(emu-stm32f746)
#
# One copy, called by each board, in the shape emu_net_attach already has.
# There were two, differing only in the target name -- and beside them two
# byte-identical copies of coremark_native.c, which is now in
# src/platform/common/ for the same reason. A block that is duplicated per
# platform while containing nothing per-platform is a block that will
# eventually be edited in one place.

# COREMARK_DIR and COREMARK_ITERATIONS come from tests/guest/CMakeLists.txt,
# which the top level adds before any platform -- deliberately, so the
# native baseline and the guest build are the same checkout at the same
# iteration count. Re-declaring them here would be a second docstring for
# one cache entry, and the two would disagree the moment one is edited.
function(emu_coremark_native_attach tgt)
    if(NOT EXISTS "${COREMARK_DIR}/core_main.c")
        message(WARNING
            "EMU_NATIVE_COREMARK is on but CoreMark was not fetched; "
            "the native baseline is not built.")
        return()
    endif()

    target_sources(${tgt} PRIVATE
        "${COREMARK_DIR}/core_main.c"
        "${COREMARK_DIR}/core_list_join.c"
        "${COREMARK_DIR}/core_matrix.c"
        "${COREMARK_DIR}/core_state.c"
        "${COREMARK_DIR}/core_util.c"
        "${CMAKE_SOURCE_DIR}/src/platform/common/coremark_native.c")

    target_include_directories(${tgt} PRIVATE
        "${COREMARK_DIR}" "${CMAKE_SOURCE_DIR}/tests/guest/coremark")

    target_compile_definitions(${tgt} PRIVATE
        EMU_NATIVE_COREMARK=1 PERFORMANCE_RUN=1
        ITERATIONS=${COREMARK_ITERATIONS})

    #
    # **The rename must apply to core_main.c alone.** Applied target-wide
    # it also renames the firmware's own entry point, and the link then
    # fails somewhere that does not mention CoreMark.
    #
    # set_source_files_properties is directory-scoped, and a function does
    # not introduce a directory -- so this lands in whichever platform
    # called us, which is where the file is compiled. That is the
    # behaviour being relied on rather than a coincidence: DIRECTORY would
    # be needed if this were called from the top level.
    #
    set_source_files_properties("${COREMARK_DIR}/core_main.c" PROPERTIES
        COMPILE_DEFINITIONS "main=coremark_native_main")

    # CoreMark is not warning-clean under our flags and is not ours to fix.
    target_compile_options(${tgt} PRIVATE
        -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough)
endfunction()
