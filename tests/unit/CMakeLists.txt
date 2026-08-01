# SPDX-License-Identifier: Apache-2.0

add_executable(rv32-unit
    test_main.c
    test_decode.c
    test_bus.c
)

target_link_libraries(rv32-unit PRIVATE rv32core)
target_include_directories(rv32-unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(rv32-unit PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND rv32-unit)
set_tests_properties(unit PROPERTIES LABELS fast)

# The guest ISA self-test, run through the host runner. This is the check
# that actually exercises the interpreter end to end.
if(TARGET guest-isatest)
    add_test(NAME guest-isatest
             COMMAND rv32-host --quiet --load 0x80000000
                     "${CMAKE_BINARY_DIR}/guest/isatest.bin")
    set_tests_properties(guest-isatest PROPERTIES LABELS fast)
endif()
