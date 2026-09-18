# SPDX-License-Identifier: Apache-2.0

add_executable(emu-unit
    test_main.c
    test_bus.c
    test_uart.c
    test_fb.c
    #
    # Built unconditionally and empty when virtio is off, rather than
    # conditionally added: a test file that disappears from the list is
    # a test nobody notices has stopped running.
    #
    test_virtio.c
    test_ir.c
)

# Frontend tests follow the frontends: a build without one must not try to
# link tests for it. test_fpu.c is here rather than above because it drives
# rv_hart_fp directly -- the FP unit is shared, but the only way in is
# through an RV32 hart.
#
# test_aplic.c is here and not in the unconditional list above, which is
# where it sat for six days: the APLIC is `src/frontend/rv32/rv_aplic.c`,
# so a PowerPC-only build compiled the test and linked against a device
# that was not built -- ten undefined references, in exactly the
# configuration `docs/frontend/ppc.md` tells the reader to use.
#
# **This file already records the same lesson twice**, both times about
# tests being *queued* in a tree that cannot run them. This is the link
# step finding it instead, and it went unnoticed for the same reason:
# nobody builds the PowerPC-only configuration.
#
if(EMU_GUEST_ARCH_RV32)
    target_sources(emu-unit PRIVATE
        test_decode.c test_fpu.c test_ecall.c test_aplic.c)
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

    # A virtio queue, driven to completion by guest code.
    #
    # **This is the only test that proves an interrupt reaches a guest.**
    # Everything else about virtio can be checked by reading registers,
    # and registers read correctly in exactly the state where the
    # interesting part is broken -- a device that never signals, or
    # signals at a privilege the guest is not running at, is
    # indistinguishable from one that works until something waits for it.
    #
    # Asserting VIRTIOTEST-PASS rather than the exit status, because the
    # status is a failure count and zero is also what a guest that ran
    # nothing returns.
    #
    if(EMU_VIRTIO)
        add_test(NAME guest-virtiotest
                 COMMAND emu-host --virtio-console --ram 0x400000
                         "${CMAKE_BINARY_DIR}/guest/virtiotest.bin")
        set_tests_properties(guest-virtiotest PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "VIRTIOTEST-PASS")

        # The same, against a disk.
        #
        # Worth its own test rather than folding into the one above,
        # because the two reach the device by different routes: the
        # console's queue is fed by the *host* and completes when the
        # emulator has somewhere to put the bytes, while the block
        # device's is fed by the guest and completes only when a backend
        # has read a file. A console that works says nothing about
        # whether a request ever reaches storage.
        #
        # The image is generated here, and its first sector is the
        # string the guest compares against -- so the test asserts
        # *content*, not merely that a completion arrived. A device that
        # signalled and filled nothing would pass every other check in
        # the guest.
        string(REPEAT "-" 492 _virtio_disk_pad)
        file(WRITE "${CMAKE_BINARY_DIR}/guest/disk.img"
             "VIRTIO-DISK-SECTOR-0${_virtio_disk_pad}")
        unset(_virtio_disk_pad)

        add_test(NAME guest-virtiotest-blk
                 COMMAND emu-host --disk
                         "${CMAKE_BINARY_DIR}/guest/disk.img"
                         --ram 0x400000
                         "${CMAKE_BINARY_DIR}/guest/virtiotest.bin")
        set_tests_properties(guest-virtiotest-blk PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "VIRTIOTEST-PASS")

        # And against a network interface.
        #
        # The loopback backend, which is why it can be a test at all: a
        # tap needs a device node, a persistent interface and an address
        # on it, none of which a suite can assume. What this covers that
        # the other two do not is **two queues at once** -- a frame is
        # transmitted on queue 1 and arrives on queue 0, and the receive
        # buffer has to be posted before the transmit or there is
        # nowhere for it to land.
        #
        # It asserts the frame's *bytes*, not the interrupt. The
        # interrupt is raised by the transmit completing, so a device
        # that delivered nothing to the guest still takes one -- checked
        # by breaking it, and "net interrupt taken" passed while the
        # three checks about the frame failed.
        add_test(NAME guest-virtiotest-net
                 COMMAND emu-host --net loop --ram 0x400000
                         "${CMAKE_BINARY_DIR}/guest/virtiotest.bin")
        set_tests_properties(guest-virtiotest-net PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "VIRTIOTEST-PASS")
    endif()

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

    #
    # A translated block running in S-mode, under Sv32 and with a PMP
    # entry armed -- which nothing else here covers.
    #
    # isatest exercises Sv32 and PMP on the *interpreter*: it arms a PMP
    # entry early, and from then on fetch_guard sends everything to the
    # interpreter, so the JIT's behaviour under either is untested by it.
    # This guest runs a memory-bound kernel below M-mode instead, which
    # is the configuration every operating system uses and the one the
    # inlined memory path declines.
    #
    # It asserts SV32BENCH-END, the line only a completed run prints --
    # not the absence of a failure. A trap in S-mode prints
    # SV32BENCH-TRAP with its cause and returns 1, and the first version
    # of this guest needed exactly that: it read `minstret`, which is
    # illegal below M-mode, and printed both markers with no kernel
    # between them.
    add_test(NAME guest-sv32bench
             COMMAND emu-host --ram 0x800000
                     "${CMAKE_BINARY_DIR}/guest/sv32bench-paged.bin")
    set_tests_properties(guest-sv32bench PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "SV32BENCH-END")

    if(EMU_JIT)
        add_test(NAME guest-sv32bench-jit
                 COMMAND emu-host --jit --ram 0x800000
                         "${CMAKE_BINARY_DIR}/guest/sv32bench-paged.bin")
        set_tests_properties(guest-sv32bench-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "SV32BENCH-END")
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

    # The same, in a mode that is not the default.
    #
    # --fb only moves the mode the device *starts* in, and everything
    # downstream reads the geometry from the device -- so a build that
    # ignored the option entirely would still pass guest-fbtest above.
    # 800x600 is 61% of 1024x768's pixels and the guest touches every
    # one, which is why this is worth a run: the count it retires says
    # whether the option reached the device.
    add_test(NAME guest-fbtest-800x600
             COMMAND emu-host --fb 800x600
                     "${CMAKE_BINARY_DIR}/guest/fbtest.bin")
    set_tests_properties(guest-fbtest-800x600 PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "FBTEST-END")

    # And that a geometry the device does not have is refused rather
    # than started. Accepting it would leave the device reporting a size
    # no mode in its table produces.
    add_test(NAME fb-mode-rejected
             COMMAND emu-host --fb 1280x1024
                     "${CMAKE_BINARY_DIR}/guest/fbtest.bin")
    set_tests_properties(fb-mode-rejected PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "is not a mode this device has")

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
