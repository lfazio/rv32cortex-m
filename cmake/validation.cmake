# SPDX-License-Identifier: Apache-2.0
#
# Validation targets.
#
#   make arch-test          official riscv-arch-test, the full set
#   make arch-test-quick    same, base integer set only (fast smoke check)
#   make riscv-tests        the older Berkeley riscv-tests suite
#   make validate           unit + guest self-test + arch-test
#
# The heavy suites are deliberately *not* wired into `make all`: they clone
# external repositories, download the Sail golden model and build several
# hundred test images. They are on-demand targets, and `ctest` keeps the
# fast checks separate from them via labels.

if(NOT EMU_PLATFORM STREQUAL "host")
    return()
endif()

set(_scripts "${CMAKE_SOURCE_DIR}/scripts")

# ---------------------------------------------------------------------
# Official RISC-V architecture test suite (riscv/riscv-arch-test)
# ---------------------------------------------------------------------

add_custom_target(arch-test
    COMMAND "${_scripts}/run-arch-test.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS rv32-host
    USES_TERMINAL
    COMMENT "Running the official RISC-V architecture tests")

add_custom_target(arch-test-quick
    COMMAND "${_scripts}/run-arch-test.sh" --extensions I
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS rv32-host
    USES_TERMINAL
    COMMENT "Running the official RISC-V architecture tests (base integer)")

# ---------------------------------------------------------------------
# riscv-tests (riscv-software-src/riscv-tests)
# ---------------------------------------------------------------------

add_custom_target(riscv-tests
    COMMAND "${_scripts}/run-riscv-tests.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS rv32-host
    USES_TERMINAL
    COMMENT "Running the riscv-tests ISA suite")

# ---------------------------------------------------------------------
# Everything
# ---------------------------------------------------------------------

add_custom_target(validate
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L fast
    COMMAND "${_scripts}/run-arch-test.sh"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS rv32-host rv32-unit
    USES_TERMINAL
    COMMENT "Running unit tests, the guest self-test and the architecture suite")

# ---------------------------------------------------------------------
# ctest integration
# ---------------------------------------------------------------------
#
# Registered so `ctest -L conformance` can drive the suites too, with a
# generous timeout: a cold run clones the suite and fetches the model.

if(BUILD_TESTING)
    add_test(NAME arch-test-I
             COMMAND "${_scripts}/run-arch-test.sh" --extensions I
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(arch-test-I PROPERTIES
        LABELS conformance
        TIMEOUT 3600)
endif()
