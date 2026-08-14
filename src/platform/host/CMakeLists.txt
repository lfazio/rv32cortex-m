# SPDX-License-Identifier: Apache-2.0
#
# Host runner: the same core the firmware runs, on a development machine.

add_executable(rv32-host
    main.c
    # The gdb stub's transport. The protocol and the register map are in
    # emucore; only the sockets are platform.
    host_gdb.c
)

target_link_libraries(rv32-host PRIVATE emucore)

target_compile_options(rv32-host PRIVATE
    -Wall -Wextra -Wshadow
)

# The test scripts look for the binary at a stable path.
set_target_properties(rv32-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
