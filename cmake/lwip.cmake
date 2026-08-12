# SPDX-License-Identifier: Apache-2.0
#
# lwIP, as ST ships it.
#
# The transport for running a test suite on the board without reflashing
# between tests: an IP stack over SLIP on the same USART the console
# already uses, carrying TFTP for the guest image and telnet for its
# output. There is no second wire -- the Nucleo has an Ethernet PHY, but
# using it would mean an ETH driver, DMA descriptors in uncached memory
# and PHY bring-up, where SLIP needs a UART that already works.
#
# Fetched exactly as the Cube packs are, from ST's own mirror rather than
# upstream lwIP, so the version matches what ST validates against these
# parts. STM32CUBE_LOCAL_DIR works here too, under the name "lwip".
#
# What is *not* used from that repo is its port layer: system/arch/cc.h
# routes assertions through printf and seeds from rand(), and this
# firmware deliberately links nothing from libc but memcpy, memset and
# __libc_init_array. src/net/arch/cc.h replaces it.

include(FetchContent)

set(LWIP_TAG "v2.2.1_20250804" CACHE STRING "ST lwIP middleware tag")

# Unlike the Cube packs, lwIP *does* ship a top-level CMakeLists.txt, and
# letting FetchContent add it as a subdirectory is wrong twice over: it
# builds targets this firmware has no use for (the docs, mbedTLS, every
# app), and its Filelists.cmake opens with include_guard(GLOBAL), so the
# include() below then silently does nothing and every source list comes
# out empty. That failure mode is a link against a library holding one
# object file -- which is exactly what it produced.
#
# SOURCE_SUBDIR naming a directory that does not exist is the documented
# way to say "populate, do not configure".
if(STM32CUBE_LOCAL_DIR AND EXISTS "${STM32CUBE_LOCAL_DIR}/lwip")
    message(STATUS "lwIP: using local checkout")
    FetchContent_Declare(lwip
        SOURCE_DIR    "${STM32CUBE_LOCAL_DIR}/lwip"
        SOURCE_SUBDIR "not-a-directory")
else()
    FetchContent_Declare(lwip
        GIT_REPOSITORY "https://github.com/STMicroelectronics/stm32_mw_lwip.git"
        GIT_TAG        "${LWIP_TAG}"
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SOURCE_SUBDIR  "not-a-directory")
endif()

FetchContent_MakeAvailable(lwip)

set(LWIP_DIR "${lwip_SOURCE_DIR}")

# Defines lwipcore_SRCS, lwipcore4_SRCS, lwiptftp_SRCS and friends. Using
# the upstream lists rather than enumerating the files here is what keeps
# a version bump from being a silent partial build: a source added
# upstream arrives on its own, and one removed stops being named.
include("${LWIP_DIR}/src/Filelists.cmake")

# IPv6, PPP, 6LoWPAN, the socket and netconn APIs and every app but TFTP
# are left out, because each is RAM this part spends on the guest. netif
# is named file by file for the same reason: lwipnetif_SRCS also carries
# zepif and the 6LoWPAN interfaces, and slipif.c is the only one wanted.
add_library(lwip STATIC
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    ${lwiptftp_SRCS}
    "${LWIP_DIR}/src/netif/slipif.c"
)

target_include_directories(lwip PUBLIC
    "${CMAKE_SOURCE_DIR}/src/net"        # lwipopts.h and arch/cc.h, ours
    "${LWIP_DIR}/src/include"
)

# Vendor code, not ours to make warning-clean under the emulator's flags.
target_compile_options(lwip PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-address-of-packed-member)
