# SPDX-License-Identifier: Apache-2.0
#
# emu_net.cmake - the IP stack, for any platform that wants it.
#
# This was inside the F746's CMakeLists, which made networking a property
# of one board. It is not: the port in src/net reaches the hardware
# through board.h and nothing else -- the comment there already said that
# "is the whole of their platform dependency, which is what lets the same
# stack build for either Nucleo" -- and lwIP itself never includes
# board.h. The F446 simply had no EMU_NET option to turn on, and the N6
# would have needed a third copy of this block to get one.
#
# What *is* per-platform is the upload path behind it: emu_net_image_*
# writes to a flash arena, and gdb's `load` writes through the same, so a
# part with no internal flash implements those differently or not at all.
# Those are functions in the platform's main.c, not build wiring.
#
# Usage, after the target exists:
#
#   include(${CMAKE_SOURCE_DIR}/cmake/emu_net.cmake)
#   emu_net_attach(emu-stm32f746)
#
# The caller declares option(EMU_NET ...) with whatever default suits the
# board, because whether a board *should* default to giving its console
# away is a board question.

#
# Which link layer carries it.
#
# **PPP is the default and SLIP is kept buildable**, because the two
# differ in what the *host* end needs rather than in what the board does:
# SLIP is a framing rule and scripts/slip-tun.py implements it in thirty
# lines over a TUN device, needing no privilege after a one-time setup;
# PPP is a negotiation (LCP, then IPCP) and the host end is pppd, which
# wants root every session. That was the exact thing slip-tun.py was
# written to escape, so the choice is a real one and not a migration.
#
# What PPP buys is that both ends *agree* -- addresses are negotiated
# rather than compiled in on one side and configured on the other, a
# dropped link is detected instead of silently swallowing frames, and
# any host with pppd can talk to the board without a bespoke script.
#
set(EMU_NET_LINK "ppp" CACHE STRING "Serial link layer: ppp or slip")
set_property(CACHE EMU_NET_LINK PROPERTY STRINGS ppp slip)

set(EMU_NET_ADDR "192.168.7.2"   CACHE STRING "Board IP address")
set(EMU_NET_PEER "192.168.7.1"   CACHE STRING "Host IP address")
set(EMU_NET_MASK "255.255.255.0" CACHE STRING "Netmask")

function(emu_net_attach tgt)
    include(${CMAKE_SOURCE_DIR}/cmake/lwip.cmake)

    # The port sources build into the firmware, not into lwip: they reach
    # the board through board.h, which lives in the platform's directory.
    target_sources(${tgt} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/net/net.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_sio.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_telnet.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_tftp.c"
        "${CMAKE_SOURCE_DIR}/src/net/net_gdb.c")

    # emu_gdb.c and the frontends' register maps are already in emucore,
    # which this links: naming them again compiled a second copy and tied
    # the network build to RV32, so a G4MH firmware served the wrong `g`
    # packet layout.
    #
    # Linking lwip PUBLIC-exports src/net (for lwipopts.h and arch/cc.h)
    # and lwIP's own headers onto the firmware, and carries
    # EMU_NET_LINK_PPP so the stack and the firmware cannot disagree about
    # the framing.
    target_link_libraries(${tgt} PRIVATE lwip)

    target_compile_definitions(${tgt} PRIVATE
        EMU_NET=1
        EMU_NET_ADDR="${EMU_NET_ADDR}"
        EMU_NET_PEER="${EMU_NET_PEER}"
        EMU_NET_MASK="${EMU_NET_MASK}")
endfunction()
