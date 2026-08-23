# SPDX-License-Identifier: Apache-2.0
#
# Host runner: the same core the firmware runs, on a development machine.

#
# The IP stack on the host, over a pty.
#
# Off by default, unlike the F746 where it is on: a board's console is a
# wire it can give away, and a runner's is a terminal a person is reading.
# What this buys is that the same lwIP, the same PPPoS, the same telnet
# and TFTP servers become debuggable with the tools a development machine
# already has -- every network defect this project has had needed a board,
# a flash cycle and a UART to find.
option(EMU_NET "IP/TFTP/telnet/gdb transport over a pty" OFF)

add_executable(emu-host
    # This platform's two ends: its "hardware" (a heap, stdout, a pty)
    # and how a run starts and stops here. The sequence between them is
    # src/platform/common/emu_main.c, the same one the boards run.
    runner.c

    # The command line and the file it names: 188 of main()'s 477 lines,
    # and the half emu_session.h calls acquisition -- the one thing a
    # runner and a board genuinely cannot share.
    host_args.c
    # The gdb stub's transport. The protocol and the register map are in
    # emucore; only the sockets are platform.
    host_gdb.c

    # The guest execution loop, shared with the firmware -- see
    # src/platform/common/emu_run.c. What a CLI and a board differ in is
    # four hooks, not a loop each.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_run.c"

    # Bringing a guest up and reporting what it did -- the middle of both
    # runners, which was the same code twice. See emu_session.h.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_session.c"

    # The run summary and the framework's JIT counters, which the shared
    # report prints. emu_console.c is deliberately *not* here: this runner
    # has two sinks where a board has one, so it supplies the console
    # primitives itself -- see emu_console_puts below.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_stats.c"

    # The guest's address space, which is the same four regions on every
    # platform plus whatever that platform adds -- see emu_board.h. This
    # file had its own copy under the name build_buses.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_address_space.c"
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_main.c"

    # The newlib write(64)/exit(93) pair, also shared. This file had its
    # own copy of the same ABI until it did not need to.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_syscall.c"

    # The one jit-diff reporter, so a divergence reported from here and
    # from a board are the same text.
    "${CMAKE_SOURCE_DIR}/src/platform/common/emu_diag.c"
)

target_include_directories(emu-host PRIVATE
    "${CMAKE_SOURCE_DIR}/src/platform/common")

target_link_libraries(emu-host PRIVATE emucore)

#
# EMU_NET is defined either way, never left undefined. `#if` on a macro
# that does not exist is 0 without a word, which is how the JIT statistics
# block silently stopped printing -- and -Wundef only catches it because
# the macro is expected to exist. Defining it 0 here is what makes that
# warning meaningful rather than noise.
if(EMU_NET)
    # board.c is the pty, the clock and two counters: everything src/net/
    # asks of a platform. See src/platform/host/board.h.
    target_sources(emu-host PRIVATE board.c)
    target_include_directories(emu-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    include(${CMAKE_SOURCE_DIR}/cmake/emu_net.cmake)
    emu_net_attach(emu-host)
else()
    target_compile_definitions(emu-host PRIVATE EMU_NET=0)
endif()

target_compile_options(emu-host PRIVATE
    -Wall -Wextra -Wshadow -Wundef
)

# The test scripts look for the binary at a stable path.
set_target_properties(emu-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
