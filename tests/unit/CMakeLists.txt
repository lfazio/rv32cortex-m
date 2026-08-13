# SPDX-License-Identifier: Apache-2.0

add_executable(rv32-unit
    test_main.c
    test_bus.c
    test_ir.c
)

# Frontend tests follow the frontends: a build without one must not try to
# link tests for it. test_fpu.c is here rather than above because it drives
# rv_hart_fp directly -- the FP unit is shared, but the only way in is
# through an RV32 hart.
if(EMU_FRONTEND_RV32)
    target_sources(rv32-unit PRIVATE test_decode.c test_fpu.c)
endif()
if(EMU_FRONTEND_G4MH)
    target_sources(rv32-unit PRIVATE test_g4mh.c)
endif()

target_link_libraries(rv32-unit PRIVATE rv32core)
target_include_directories(rv32-unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(rv32-unit PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND rv32-unit)
set_tests_properties(unit PROPERTIES LABELS fast)

# The guest ISA self-test, run through the host runner. This is the check
# that actually exercises the RV32 interpreter end to end.
if(TARGET guest-isatest)
    add_test(NAME guest-isatest
             COMMAND rv32-host --quiet --load 0x80000000
                     "${CMAKE_BINARY_DIR}/guest/isatest.bin")
    set_tests_properties(guest-isatest PROPERTIES LABELS fast)

    # The same self-test through the JIT.
    #
    # This is the check the Thumb-2 backend can never have: every JIT bug
    # this project has found needed a board, a flash cycle and a UART, and
    # three of them were live for months first. On a host with a JIT
    # backend, translated code is covered by `ctest -L fast`.
    if(EMU_JIT)
        add_test(NAME guest-isatest-jit
                 COMMAND rv32-host --jit --quiet --load 0x80000000
                         "${CMAKE_BINARY_DIR}/guest/isatest.bin")
        set_tests_properties(guest-isatest-jit PROPERTIES LABELS fast)
    endif()
endif()
