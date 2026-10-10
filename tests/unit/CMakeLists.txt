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
    #
    # The JIT framework, driven through a stub frontend: the negative
    # cache is bookkeeping that belongs to this layer, and a test written
    # against a real frontend would assert its lowering decisions too.
    #
    test_jit.c
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
if(EMU_GUEST_ARCH_ARMV7M)
    target_sources(emu-unit PRIVATE test_armv7m.c)
endif()

#
# The ARMv7-M frontend's first guest, run through the host runner.
#
# **This is the test that matters, and the unit test is not.** The
# length decoder's property test proves one function; this proves the
# frontend fetches, decodes, executes, stores to a device and takes a
# backward conditional branch. Every frontend in this tree has had
# defects that only a running program found -- G4MH three, PowerPC
# four -- and this one was no different: a prefix read two bits wide
# instead of three aliased MOVS onto LSL, and no unit test would have
# seen it.
#
# `OKAAA` asserts the loop ran the right number of times, not merely
# that something printed: a broken Z flag gives the wrong number of
# As, which names its own cause.
#
#
# The compiled-C guest, which is the one that matters: it found three
# decode defects the hand-written assembly could not, because a
# compiler reaches for MOVW/MOVT, the modified immediate and shifts by
# register without being asked.
#
# It asserts ARMV7M-ALU-OK, a line only a *completed* run prints, and
# its expected values are constants worked out independently of the
# implementation.
#
if(TARGET guest-armv7m-alu)
    add_test(NAME guest-armv7m-alu
             COMMAND emu-host --max-insn 200000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-alu.bin")
    set_tests_properties(guest-armv7m-alu PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "ARMV7M-ALU-OK")
endif()

#
# IT blocks: the only stateful thing in this decoder, and the one place
# a half-correct implementation is right for the first instruction of a
# block and wrong for the rest.
#
#
# SysTick and the exception model.
#
# **Only waiting for an interrupt can prove one works.** Every NVIC
# register can be written and read back by a controller that never
# signals -- which this project has already been caught by once, when
# the first virtio guest accepted every write and the device did
# nothing. So the guest asserts a handler that *ran*, a counter that
# moved, and a register value that survived the frame.
#
#
# **The Thumb-2 backend's emitters, executed.**
#
# This is the test that justifies the ARMv7-M frontend. The emitters
# could only be *run* on hardware until now -- three of the backend's
# defects were live for months because no host suite could reach them,
# and both of the ones this guest reproduces produced a working program
# that did something else rather than a wrong answer.
#
# scripts/t2-check-encodings.sh compares emitted bytes against
# arm-none-eabi-as, which catches a wrong encoding. It cannot catch a
# wrong *choice* of encoding. Executing them does.
#
if(TARGET guest-armv7m-t2exec)
    add_test(NAME guest-armv7m-t2exec
             COMMAND emu-host --max-insn 2000000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-t2exec.bin")
    set_tests_properties(guest-armv7m-t2exec PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "T2EXEC-OK")
endif()

if(TARGET guest-armv7m-nvic)
    add_test(NAME guest-armv7m-nvic
             COMMAND emu-host --max-insn 5000000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-nvic.bin")
    set_tests_properties(guest-armv7m-nvic PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "ARMV7M-NVIC-OK")
endif()

if(TARGET guest-armv7m-itblock)
    add_test(NAME guest-armv7m-itblock
             COMMAND emu-host --max-insn 200000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-itblock.bin")
    set_tests_properties(guest-armv7m-itblock PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "ARMV7M-IT-OK")
endif()

if(TARGET guest-armv7m-hello)
    add_test(NAME guest-armv7m-hello
             COMMAND emu-host --max-insn 1000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-hello.bin")
    set_tests_properties(guest-armv7m-hello PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "OKAAA")
endif()

#
# The same guests through the JIT, and the JIT against the interpreter.
#
# Separate tests rather than a loop over the ones above, because they
# make a different claim. Interpreted, a guest checks the frontend;
# translated, it checks the translator -- and a translator that declined
# everything would pass the first kind unchanged, which is why the
# differential test below also requires that blocks were entered.
#
if(EMU_GUEST_ARCH_ARMV7M AND EMU_JIT)
    foreach(_g alu:ARMV7M-ALU-OK:200000 itblock:ARMV7M-IT-OK:200000
               nvic:ARMV7M-NVIC-OK:5000000 t2exec:T2EXEC-OK:2000000
               hello:OKAAA:1000)
        string(REPLACE ":" ";" _p "${_g}")
        list(GET _p 0 _name)
        list(GET _p 1 _want)
        list(GET _p 2 _cap)
        if(TARGET guest-armv7m-${_name})
            add_test(NAME guest-armv7m-${_name}-jit
                     COMMAND emu-host --jit --max-insn ${_cap}
                             "${CMAKE_BINARY_DIR}/guest/armv7m-${_name}.bin")
            set_tests_properties(guest-armv7m-${_name}-jit PROPERTIES
                LABELS fast
                PASS_REGULAR_EXPRESSION "${_want}")
        endif()
    endforeach()

    #
    # Generated instructions, interpreted and translated, compared line
    # by line. The interpreter's answers were checked against a
    # Cortex-M7; this carries that verdict to the translator without the
    # board being attached.
    #
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND AND TARGET guest-armv7m-alu)
        add_test(NAME armv7m-jit-vs-interp
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_SOURCE_DIR}/tests/armv7m-diff/run.py"
                         --self --seed 1 --count 3000
                         --emu $<TARGET_FILE:emu-host>
                         --out "${CMAKE_BINARY_DIR}/armv7m-self")
        set_tests_properties(armv7m-jit-vs-interp PROPERTIES LABELS fast)
    endif()
endif()

#
# CoreMark as an ARMv7-M guest, both ways. "Correct operation validated"
# is CoreMark's own verdict on its CRCs *and* on having run long enough,
# so the image is built with enough iterations for the second -- a run
# CoreMark calls invalid prints "Errors detected", which reads as a
# miscompile and is not.
#
if(TARGET guest-armv7m-coremark)
    add_test(NAME guest-armv7m-coremark
             COMMAND emu-host --max-insn 400000000
                     "${CMAKE_BINARY_DIR}/guest/armv7m-coremark.bin")
    set_tests_properties(guest-armv7m-coremark PROPERTIES
        LABELS fast
        PASS_REGULAR_EXPRESSION "Correct operation validated")
    if(EMU_JIT)
        add_test(NAME guest-armv7m-coremark-jit
                 COMMAND emu-host --jit --max-insn 400000000
                         "${CMAKE_BINARY_DIR}/guest/armv7m-coremark.bin")
        set_tests_properties(guest-armv7m-coremark-jit PROPERTIES
            LABELS fast
            PASS_REGULAR_EXPRESSION "Correct operation validated")
    endif()
endif()

# AES and SHA-256 against their FIPS vectors, and Dhrystone, the same
# two ways. Dhrystone prints no verdict of its own, so its test is the
# last line it prints -- which a run that stopped early never reaches.
foreach(_g crypto:failures\ 0x00000000 dhrystone:Dhrystones\ per\ Second)
    string(REPLACE ":" ";" _p "${_g}")
    list(GET _p 0 _name)
    list(GET _p 1 _want)
    if(TARGET guest-armv7m-${_name})
        add_test(NAME guest-armv7m-${_name}
                 COMMAND emu-host --max-insn 400000000
                         "${CMAKE_BINARY_DIR}/guest/armv7m-${_name}.bin")
        set_tests_properties(guest-armv7m-${_name} PROPERTIES
            LABELS fast PASS_REGULAR_EXPRESSION "${_want}")
        if(EMU_JIT)
            add_test(NAME guest-armv7m-${_name}-jit
                     COMMAND emu-host --jit --max-insn 400000000
                             "${CMAKE_BINARY_DIR}/guest/armv7m-${_name}.bin")
            set_tests_properties(guest-armv7m-${_name}-jit PROPERTIES
                LABELS fast PASS_REGULAR_EXPRESSION "${_want}")
        endif()
    endif()
endforeach()

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
    # isatest used to exercise Sv32 and PMP on the interpreter only: it
    # arms a PMP entry early, and `blocked` pointed at fetch_guard. It
    # points at Sdtrig alone now, and isatest --jit interprets 436 of
    # 45,799 instructions -- its jit-ctx-* checks depend on that. This
    # guest is still the one that runs a memory-bound kernel below
    # M-mode, which is the configuration every operating system uses
    # and the one the inlined memory path declines.
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
            # The stats line gained a `(cached N)` between the count and
            # `overflow` when the negative cache was added. The bound is
            # what this test is about -- three digits, i.e. the AMOs are
            # being lowered rather than declined -- so it is the shape
            # around it that had to move, not the assertion.
            PASS_REGULAR_EXPRESSION
                "declined [0-9][0-9]?[0-9]? \\(cached [0-9]+\\) overflow")
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

#
# The compiled e200z7 guests: classic Book E, as ELF, because that is
# where the image says which of the core's two encodings it is in.
#
# Each expression is something only a completed run prints. For alu that
# includes the *number* of checks, so a run that skipped half of them is
# not a pass, and "ticked", so the decrementer interrupt really did land
# in the middle of it.
#
if(EMU_GUEST_ARCH_PPC)
    foreach(_g "alu:PPC-ALU-END checks=00000044 fails=00000000 ticked"
               "coremark:Correct operation validated"
               "crypto:failures 0x00000000")
        string(FIND "${_g}" ":" _at)
        string(SUBSTRING "${_g}" 0 ${_at} _name)
        math(EXPR _at "${_at} + 1")
        string(SUBSTRING "${_g}" ${_at} -1 _want)
        if(TARGET guest-ppc-${_name})
            add_test(NAME guest-ppc-${_name}
                     COMMAND emu-host --max-insn 400000000
                             "${CMAKE_BINARY_DIR}/guest/ppc-${_name}.elf")
            set_tests_properties(guest-ppc-${_name} PROPERTIES
                LABELS fast
                PASS_REGULAR_EXPRESSION "${_want}")
        endif()
    endforeach()

    #
    # The decoder and the disassembler against binutils: the 16-bit VLE
    # space exhaustively and random 32-bit words in both encodings, and
    # then every instruction the compiler put in the guests above, none
    # of which may be one this core does not have. The full sweeps
    # (--sets vle32,booke: four million encodings, a minute) are run by
    # hand; see tests/ppc-check/check.py.
    #
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(PPC_CHECK_AS NAMES powerpc-linux-gnu-as powerpc64-linux-gnu-as)
    #
    # Not `if(TARGET ppc-dis)`: that target is defined after this
    # directory is read, and if(TARGET) only sees what already exists --
    # the answer would always be no, and nothing would say so. The
    # generator expression below is resolved later, when it does exist.
    #
    if(Python3_FOUND AND PPC_CHECK_AS AND EMU_PLATFORM STREQUAL "host")
        add_test(NAME ppc-decode-vs-binutils
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_SOURCE_DIR}/tests/ppc-check/check.py"
                         --dis $<TARGET_FILE:ppc-dis> --sets vle16,random)
        set_tests_properties(ppc-decode-vs-binutils PROPERTIES LABELS fast)
        if(TARGET guest-ppc-alu AND TARGET guest-ppc-coremark AND
           TARGET guest-ppc-crypto)
            add_test(NAME ppc-decode-compiled
                     COMMAND ${Python3_EXECUTABLE}
                             "${CMAKE_SOURCE_DIR}/tests/ppc-check/check.py"
                             --dis $<TARGET_FILE:ppc-dis>
                             --elf "${CMAKE_BINARY_DIR}/guest/ppc-alu.elf"
                                   "${CMAKE_BINARY_DIR}/guest/ppc-coremark.elf"
                                   "${CMAKE_BINARY_DIR}/guest/ppc-crypto.elf")
            set_tests_properties(ppc-decode-compiled PROPERTIES LABELS fast)
        endif()
    endif()
endif()
