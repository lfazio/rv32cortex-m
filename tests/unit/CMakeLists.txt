# SPDX-License-Identifier: Apache-2.0

add_executable(emu-unit
    test_main.c
    test_bus.c
    test_ir.c
)

# Frontend tests follow the frontends: a build without one must not try to
# link tests for it. test_fpu.c is here rather than above because it drives
# rv_hart_fp directly -- the FP unit is shared, but the only way in is
# through an RV32 hart.
if(EMU_FRONTEND_RV32)
    target_sources(emu-unit PRIVATE test_decode.c test_fpu.c)
endif()
if(EMU_FRONTEND_G4MH)
    target_sources(emu-unit PRIVATE test_g4mh.c)
endif()
if(EMU_FRONTEND_PPC)
    target_sources(emu-unit PRIVATE test_ppc.c)
endif()

target_link_libraries(emu-unit PRIVATE emucore)
target_include_directories(emu-unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(emu-unit PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND emu-unit)
set_tests_properties(unit PROPERTIES LABELS fast)

# The guest ISA self-test, run through the host runner. This is the check
# that actually exercises the RV32 interpreter end to end.
if(TARGET guest-isatest)
    add_test(NAME guest-isatest
             COMMAND emu-host --quiet --load 0x80000000
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
                 COMMAND emu-host --jit --quiet --load 0x80000000
                         "${CMAKE_BINARY_DIR}/guest/isatest.bin")
        set_tests_properties(guest-isatest-jit PROPERTIES LABELS fast)
    endif()
endif()
