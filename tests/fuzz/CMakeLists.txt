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
# And every input that has ever made the two backends disagree, or hung
# them, replayed, from regressions/<isa>-jit/. Each file is named for
# what it found; docs/fuzzing.md has the list.
#
# Both with a ten-second limit on any one input: libFuzzer's default is
# twenty minutes, and a hang is a finding to report, not a wait.
#
# And the dictionary's claim, where it has one (dict/<isa>-jit.dict,
# which scripts/fuzz.sh passes to every run): its `#!` tokens, in that
# order, must reach a device and take an interrupt the two backends
# compare. A harness change that moves an initial register or a device
# would leave every token valid and the dictionary meaning nothing.
#
find_package(Python3 COMPONENTS Interpreter QUIET)

function(emu_fuzz_jit_target isa)
    set(t fuzz-${isa}-jit)
    add_executable(${t} fuzz_${isa}_jit.c)
    target_link_libraries(${t} PRIVATE emucore)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wundef)

    add_test(NAME ${t}-smoke
             COMMAND ${CMAKE_COMMAND} -E env FUZZ_STATS=1
                     $<TARGET_FILE:${t}> -runs=3000 -seed=1 -max_len=512
                     -timeout=10)
    set_tests_properties(${t}-smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "[1-9][0-9]* compared.*[1-9][0-9]* translations"
        LABELS fuzz)

    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${isa}-jit)
    file(GLOB saved ${dir}/*)
    if(saved)
        add_test(NAME ${t}-regressions
                 COMMAND $<TARGET_FILE:${t}> -runs=0 -timeout=10 ${dir})
        set_tests_properties(${t}-regressions PROPERTIES
            PASS_REGULAR_EXPRESSION "Done [0-9]+ runs"
            LABELS fuzz)
    endif()

    set(dict ${CMAKE_CURRENT_SOURCE_DIR}/dict/${isa}-jit.dict)
    set(in ${CMAKE_CURRENT_BINARY_DIR}/dict-${isa})
    if(EXISTS ${dict} AND Python3_FOUND)
        add_test(NAME ${t}-dict
                 COMMAND sh -c "\"$1\" \"$2\" --compose \"$3\" ${isa} && FUZZ_STATS=1 exec \"$4\" -runs=0 -timeout=10 \"$3/${isa}-jit-taken\""
                         sh ${Python3_EXECUTABLE}
                         ${PROJECT_SOURCE_DIR}/scripts/fuzz-dict.py ${in}
                         $<TARGET_FILE:${t}>)
        set_tests_properties(${t}-dict PROPERTIES
            PASS_REGULAR_EXPRESSION
                "1 inputs, 1 compared \\(1 touched a device, 1 took an interrupt\\)"
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

#
# The IR itself, with no frontend: random blocks through the passes and
# the x86-64 lowering, against the IR interpreter on the block as built
# (fuzz_ir.c). The smoke test must show blocks compared *and* the passes
# doing something to them -- a generator whose blocks no pass touched
# would compare two copies of one program. Not `mac`: x86-64 lowers no
# multiply-accumulate, so pass_mac forms none here.
#
add_executable(fuzz-ir fuzz_ir.c)
target_link_libraries(fuzz-ir PRIVATE emucore)
target_link_options(fuzz-ir PRIVATE -fsanitize=fuzzer)
target_compile_options(fuzz-ir PRIVATE -Wall -Wextra -Wundef)
# From a copy of the regressions, as scripts/fuzz.sh starts: from nothing,
# three thousand runs never grow past the inputs that build one-line
# blocks. A copy, because libFuzzer writes what it finds into the first
# corpus directory it is given.
add_test(NAME fuzz-ir-smoke
         COMMAND sh -c "rm -rf ir-smoke && mkdir ir-smoke && cp \"$1\"/* ir-smoke/ && FUZZ_STATS=1 exec \"$2\" -runs=3000 -seed=1 -max_len=512 -timeout=10 ir-smoke"
                 sh ${CMAKE_CURRENT_SOURCE_DIR}/regressions/ir
                 $<TARGET_FILE:fuzz-ir>)
set_tests_properties(fuzz-ir-smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "[1-9][0-9]* compared.*[1-9][0-9]* looped.*[1-9][0-9]* trapped.*\n.*puts [1-9][0-9]* .*folded [1-9][0-9]* addr [1-9][0-9]* .*retires [1-9]"
    LABELS fuzz)
file(GLOB _ir_saved ${CMAKE_CURRENT_SOURCE_DIR}/regressions/ir/*)
if(_ir_saved)
    add_test(NAME fuzz-ir-regressions
             COMMAND $<TARGET_FILE:fuzz-ir> -runs=0 -timeout=10
                     ${CMAKE_CURRENT_SOURCE_DIR}/regressions/ir)
    set_tests_properties(fuzz-ir-regressions PROPERTIES
        PASS_REGULAR_EXPRESSION "Done [0-9]+ runs"
        LABELS fuzz)
endif()

#
# RV32's translation and protection -- Sv32, PMP and the data TLB in front
# of both -- against a walk written from the privileged spec, which shares
# nothing with rv_mmu.c or rv_pmp.c (fuzz_rv32_mmu.c). The smoke test must
# show loads and stores done *and* refused, and data TLB entries checked:
# a run that never filled one asked the cache nothing.
#
if(EMU_GUEST_ARCH_RV32)
    add_executable(fuzz-rv32-mmu fuzz_rv32_mmu.c)
    target_link_libraries(fuzz-rv32-mmu PRIVATE emucore)
    target_link_options(fuzz-rv32-mmu PRIVATE -fsanitize=fuzzer)
    target_compile_options(fuzz-rv32-mmu PRIVATE -Wall -Wextra -Wundef)
    add_test(NAME fuzz-rv32-mmu-smoke
             COMMAND ${CMAKE_COMMAND} -E env FUZZ_STATS=1
                     $<TARGET_FILE:fuzz-rv32-mmu> -runs=20000 -seed=1
                     -max_len=512 -timeout=10)
    set_tests_properties(fuzz-rv32-mmu-smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "[1-9][0-9]* loads and [1-9][0-9]* stores done, [1-9][0-9]* faults\\), [1-9][0-9]* data TLB entries checked"
        LABELS fuzz)
    file(GLOB _mmu_saved ${CMAKE_CURRENT_SOURCE_DIR}/regressions/rv32-mmu/*)
    if(_mmu_saved)
        add_test(NAME fuzz-rv32-mmu-regressions
                 COMMAND $<TARGET_FILE:fuzz-rv32-mmu> -runs=0 -timeout=10
                         ${CMAKE_CURRENT_SOURCE_DIR}/regressions/rv32-mmu)
        set_tests_properties(fuzz-rv32-mmu-regressions PROPERTIES
            PASS_REGULAR_EXPRESSION "Done [0-9]+ runs"
            LABELS fuzz)
    endif()
endif()
