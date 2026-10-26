# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer targets. Configured only with -DEMU_FUZZ=ON, which also
# builds the rest of the tree with ASan, UBSan and coverage
# instrumentation (see the top-level CMakeLists). scripts/fuzz.sh is
# the way in; docs/fuzzing.md says what each target compares.

if(NOT EMU_JIT)
    message(FATAL_ERROR "the fuzz targets compare against the JIT: EMU_JIT=ON")
endif()

#
# One interpreter-against-JIT target per frontend: fuzz-<isa>-jit, from
# fuzz_<isa>_jit.c and fuzz_harness.h, with two tests.
#
# A smoke run, so ctest in a fuzz tree proves the target runs, compares
# and translates -- not that it finds anything. It must say it compared
# runs and translated blocks: a harness whose JIT declined everything,
# or whose runs never finished, passes every input and tests nothing.
#
# And every input that has ever made the two backends disagree,
# replayed, from regressions/<isa>-jit/. Each file is named for what it
# found; docs/fuzzing.md has the list.
#
function(emu_fuzz_jit_target isa)
    set(t fuzz-${isa}-jit)
    add_executable(${t} fuzz_${isa}_jit.c)
    target_link_libraries(${t} PRIVATE emucore)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wundef)

    add_test(NAME ${t}-smoke
             COMMAND ${CMAKE_COMMAND} -E env FUZZ_STATS=1
                     $<TARGET_FILE:${t}> -runs=3000 -seed=1 -max_len=512)
    set_tests_properties(${t}-smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "[1-9][0-9]* compared.*[1-9][0-9]* translations"
        LABELS fuzz)

    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${isa}-jit)
    file(GLOB saved ${dir}/*)
    if(saved)
        add_test(NAME ${t}-regressions
                 COMMAND $<TARGET_FILE:${t}> -runs=0 ${dir})
        set_tests_properties(${t}-regressions PROPERTIES
            PASS_REGULAR_EXPRESSION "Done [0-9]+ runs"
            LABELS fuzz)
    endif()
endfunction()

if(EMU_GUEST_ARCH_RV32)
    emu_fuzz_jit_target(rv32)
endif()
if(EMU_GUEST_ARCH_ARMV7M)
    emu_fuzz_jit_target(armv7m)
endif()
if(EMU_GUEST_ARCH_G4MH)
    emu_fuzz_jit_target(g4mh)
endif()
if(EMU_GUEST_ARCH_PPC)
    emu_fuzz_jit_target(ppc)
endif()
