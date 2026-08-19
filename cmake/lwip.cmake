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

# IPv6, 6LoWPAN, the socket and netconn APIs and every app but TFTP are
# left out, because each is RAM this part spends on the guest. netif is
# named file by file for the same reason: lwipnetif_SRCS also carries
# zepif and the 6LoWPAN interfaces, and only the link in use is wanted.
#
# The link layer is EMU_NET_LINK. Both are built from the upstream lists
# where there is one -- lwipppp_SRCS is defined by Filelists.cmake and
# carries the whole of PPP including the parts this port disables in
# lwipopts.h, which cost nothing once PPP_SUPPORT gates them out.
set(_emu_net_link_srcs)
if(EMU_NET_LINK STREQUAL "ppp")
    list(APPEND _emu_net_link_srcs ${lwipppp_SRCS})
elseif(EMU_NET_LINK STREQUAL "slip")
    list(APPEND _emu_net_link_srcs "${LWIP_DIR}/src/netif/slipif.c")
else()
    message(FATAL_ERROR "EMU_NET_LINK must be ppp or slip, not '${EMU_NET_LINK}'")
endif()

add_library(lwip STATIC
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    ${lwiptftp_SRCS}
    ${_emu_net_link_srcs}
)

target_include_directories(lwip PUBLIC
    "${CMAKE_SOURCE_DIR}/src/net"        # lwipopts.h and arch/cc.h, ours
    "${LWIP_DIR}/src/include"
)

#
# **PUBLIC, and on this target, not only on the firmware's.**
#
# lwipopts.h turns PPP_SUPPORT on from this, and lwIP's own sources are
# what read it -- so defining it only where net.c is compiled left every
# file in netif/ppp/ compiling to an empty object. The archive contained
# ppp.c.obj, pppos.c.obj and four more, all of them zero symbols, and the
# link failed on pppos_create with the sources plainly in the build.
#
# PUBLIC so the firmware target inherits it by linking, which keeps the
# two halves from being able to disagree about which link layer is in
# use -- the failure that would produce is a stack built for one framing
# driven by a netif expecting the other.
#
target_compile_definitions(lwip PUBLIC
    EMU_NET_LINK_PPP=$<STREQUAL:${EMU_NET_LINK},ppp>)

# Vendor code, not ours to make warning-clean under the emulator's flags.
target_compile_options(lwip PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-address-of-packed-member)
