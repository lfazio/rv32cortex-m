# SPDX-License-Identifier: Apache-2.0

add_executable(emu-unit
    test_main.c
    test_bus.c
    test_fb.c
    test_ir.c
)

# Frontend tests follow the frontends: a build without one must not try to
# link tests for it. test_fpu.c is here rather than above because it drives
# rv_hart_fp directly -- the FP unit is shared, but the only way in is
# through an RV32 hart.
if(EMU_GUEST_ARCH_RV32)
    target_sources(emu-unit PRIVATE test_decode.c test_fpu.c)
endif()
if(EMU_GUEST_ARCH_G4MH)
    target_sources(emu-unit PRIVATE test_g4mh.c)
endif()
if(EMU_GUEST_ARCH_PPC)
    target_sources(emu-unit PRIVATE test_ppc.c)
endif()

target_link_libraries(emu-unit PRIVATE emucore)
target_include_directories(emu-unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# -Wundef with the rest, and it earns its place here rather than being
# tidiness: it is the warning that catches a capability macro read
# without including what defines it, which #if silently treats as 0.
# emucore has had it for a while; the platform targets did not, and the
# JIT statistics block in main.c stopped printing entirely the moment an
# unrelated include was removed. Nothing failed and no test could see it.
target_compile_options(emu-unit PRIVATE -Wall -Wextra -Wundef)

add_test(NAME unit COMMAND emu-unit)
set_tests_properties(unit PROPERTIES LABELS fast)

# The guest ISA self-test, run through the host runner. This is the check
# that actually exercises the RV32 interpreter end to end.
#
# `TARGET guest-isatest` alone is not the right condition and used to be:
# the guest images are built whenever the RISC-V toolchain is present,
# which is independent of which frontends were compiled in. So a
# PowerPC-only build declared this test, handed an RV32 image to an
# emulator that decodes VLE, and -- with no instruction cap -- ran until
# something killed it. The image existing and the emulator being able to
# run it are two different questions.
if(TARGET guest-isatest AND EMU_GUEST_ARCH_RV32)
    #
    # The PASS line, not just the exit status. The status is the failure
    # count, so zero is also what a guest that ran *nothing* returns --
    # which is not hypothetical: the PowerPC case below passed for three
    # commits while its guest stopped after 212 instructions.
    add_test(NAME guest-isatest
             COMMAND emu-host "${CMAKE_BINARY_DIR}/guest/isatest.bin")
    set_tests_properties(guest-isatest PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "PASS")

    # The same self-test through the JIT.
    #
    # This is the check the Thumb-2 backend can never have: every JIT bug
    # this project has found needed a board, a flash cycle and a UART, and
    # three of them were live for months first. On a host with a JIT
    # backend, translated code is covered by `ctest -L fast`.
    if(EMU_JIT)
        add_test(NAME guest-isatest-jit
                 COMMAND emu-host --jit
                         "${CMAKE_BINARY_DIR}/guest/isatest.bin")
        set_tests_properties(guest-isatest-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "PASS")
    endif()
endif()

#
# TinyCrypt: AES-128 and SHA-256 against their published vectors.
#
# **A different question from isatest.** That one checks the instructions
# an ISA test thought to write; this runs a real library that was not
# written with an emulator in mind, and checks the answer against FIPS
# rather than against itself. Crypto fails catastrophically -- one wrong
# bit in a rotation changes every byte after it -- so the vectors are a
# far stronger assertion than a checksum.
#
# It is also the only guest here that executes rotations at all, which is
# what the Zbb lowerings need in order to be measurable.
#
# CRYPTO-END rather than a failure count, for the reason the PowerPC test
# above records: an exit status of zero is also what a guest that ran
# nothing returns.
if(TARGET guest-crypto AND EMU_GUEST_ARCH_RV32)
    add_test(NAME guest-crypto
             COMMAND emu-host "${CMAKE_BINARY_DIR}/guest/crypto.bin")
    set_tests_properties(guest-crypto PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "failures 0x00000000")

    if(EMU_JIT)
        add_test(NAME guest-crypto-jit
                 COMMAND emu-host --jit
                         "${CMAKE_BINARY_DIR}/guest/crypto.bin")
        set_tests_properties(guest-crypto-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "failures 0x00000000")
    endif()
endif()

# The framebuffer, from the guest's side.
#
# The unit test drives the device's ops table directly; this goes through
# the bus at the addresses a real guest uses, with pixels written as
# ordinary stores. "The device works" and "the device is reachable" are
# different claims, and this tree has been caught by the gap between them
# more than once -- a capability nothing exercises is not a capability.
#
# FBTEST-END rather than a failure count, for the reason recorded above:
# an exit status of zero is also what a guest that ran nothing returns.
if(TARGET guest-fbtest AND EMU_GUEST_ARCH_RV32)
    add_test(NAME guest-fbtest
             COMMAND emu-host "${CMAKE_BINARY_DIR}/guest/fbtest.bin")
    set_tests_properties(guest-fbtest PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "FBTEST-END")

    if(EMU_JIT)
        add_test(NAME guest-fbtest-jit
                 COMMAND emu-host --jit
                         "${CMAKE_BINARY_DIR}/guest/fbtest.bin")
        set_tests_properties(guest-fbtest-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "FBTEST-END")
    endif()
endif()

# The A extension.
#
# Two different questions, and they need two different assertions.
#
# **Correctness cannot guard the lowering.** Both backends call the same
# rv_hart_amo, so an atomic that stops being translated still computes the
# right answer -- it just ends the block and interprets. Measured, not
# argued: with the lowering removed this guest still reports
# `failures 0x00000000`, and only the `declined` counter moves, from 1 to
# 6017. A test that checked the answers alone would have passed against
# the very regression it was written for -- which is this project's
# most-repeated mistake.
#
# So the JIT run is checked twice: once for the answers, once for the
# coverage. The bound comes from the guest's own design rather than from
# a number the implementation produced -- it issues 3 * ATOMIC_ROUNDS
# atomics, so a translator that declined them again cannot come in under
# a thousand. The 1 that remains is the ecall the guest exits through,
# which the translator declines deliberately.
if(TARGET guest-atomics AND EMU_GUEST_ARCH_RV32)
    add_test(NAME guest-atomics
             COMMAND emu-host "${CMAKE_BINARY_DIR}/guest/atomics.bin")
    set_tests_properties(guest-atomics PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "failures 0x00000000")

    if(EMU_JIT)
        add_test(NAME guest-atomics-jit
                 COMMAND emu-host --jit
                         "${CMAKE_BINARY_DIR}/guest/atomics.bin")
        set_tests_properties(guest-atomics-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "failures 0x00000000")

        add_test(NAME guest-atomics-jit-lowered
                 COMMAND emu-host --jit
                         "${CMAKE_BINARY_DIR}/guest/atomics.bin")
        set_tests_properties(guest-atomics-jit-lowered PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "declined [0-9][0-9]?[0-9]? overflow")
    endif()
endif()

# The e200z7 guest, which is the only thing that runs this frontend as a
# program rather than as hand-assembled arrays.
#
# The exit status is the number of failed checks, and 99 means the guest
# took an unexpected trap -- so a decode gap fails this test by name
# rather than by running to the instruction cap. That distinction is the
# whole reason the guest carries a vector table; without one an
# unimplemented encoding vectors to address 0, restarts the program, and
# presents as a hang that no exit status can describe.
#
# Gated on the frontend, not only on the image, for the reason spelled out
# above -- and --load because this guest is *not* execute-in-place: its
# link script puts it at 0x80000000 and it writes to its own image, so the
# runner's default flash window would map it read-only.
#
# PASS_REGULAR_EXPRESSION and not the exit status alone. The status is the
# number of failed checks, so zero means "nothing failed" -- which is also
# what "nothing ran" looks like, and that is exactly what happened when the
# load address moved: the guest stopped after 212 instructions, printed no
# marker, and the test passed for three commits. Requiring the line the
# guest prints when it reaches the end makes running a precondition of
# passing.
if(TARGET guest-ppc-isatest AND EMU_GUEST_ARCH_PPC)
    add_test(NAME guest-ppc-isatest
             COMMAND emu-host --load 0x80000000 --max-insn 200000
                     "${CMAKE_BINARY_DIR}/tests/guest/ppc/isatest.bin")
    set_tests_properties(guest-ppc-isatest PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "PPC-ISATEST-END")
endif()
