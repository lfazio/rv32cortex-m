# SPDX-License-Identifier: Apache-2.0
#
# Host runner: the same core the firmware runs, on a development machine.

add_executable(rv32-host
    main.c
)

target_link_libraries(rv32-host PRIVATE rv32core)

target_compile_options(rv32-host PRIVATE
    -Wall -Wextra -Wshadow
)

# The test scripts look for the binary at a stable path.
set_target_properties(rv32-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
