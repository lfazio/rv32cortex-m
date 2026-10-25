# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer targets. Configured only with -DEMU_FUZZ=ON, which also
# builds the rest of the tree with ASan, UBSan and coverage
# instrumentation (see the top-level CMakeLists). scripts/fuzz.sh is
# the way in; docs/fuzzing.md says what each target compares.

if(NOT EMU_GUEST_ARCH_RV32 OR NOT EMU_JIT)
    message(FATAL_ERROR "fuzz-rv32-jit needs EMU_GUEST_ARCH_RV32 and EMU_JIT")
endif()

add_executable(fuzz-rv32-jit fuzz_rv32_jit.c)
target_link_libraries(fuzz-rv32-jit PRIVATE emucore)
target_link_options(fuzz-rv32-jit PRIVATE -fsanitize=fuzzer)
target_compile_options(fuzz-rv32-jit PRIVATE -Wall -Wextra -Wundef)

#
# A smoke run, so ctest in a fuzz tree proves the target runs, compares
# and translates -- not that it finds anything. It must say it compared
# runs and translated blocks: a harness whose JIT declined everything,
# or whose runs never finished, passes every input and tests nothing.
#
add_test(NAME fuzz-rv32-jit-smoke
         COMMAND ${CMAKE_COMMAND} -E env FUZZ_STATS=1
                 $<TARGET_FILE:fuzz-rv32-jit> -runs=3000 -seed=1
                 -max_len=512)
set_tests_properties(fuzz-rv32-jit-smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "[1-9][0-9]* compared.*[1-9][0-9]* translations"
    LABELS fuzz)

#
# Every input that has ever made the two backends disagree, replayed.
# Each file is named for what it found; docs/fuzzing.md has the list.
#
add_test(NAME fuzz-rv32-jit-regressions
         COMMAND $<TARGET_FILE:fuzz-rv32-jit> -runs=0
                 ${CMAKE_CURRENT_SOURCE_DIR}/regressions/rv32-jit)
set_tests_properties(fuzz-rv32-jit-regressions PROPERTIES
    PASS_REGULAR_EXPRESSION "Done [0-9]+ runs"
    LABELS fuzz)
