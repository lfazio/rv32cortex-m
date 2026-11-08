# lwIP, as much of it as `src/net/net_tftp.c` and `net_telnet.c` see

`fuzz-upload` compiles `src/net/net_tftp.c` as it is and calls its
callbacks the way lwIP's TFTP server does. The real lwIP is fetched only
by a network build, and a fuzz tree should not need the network to
configure, so these headers declare the few things `net_tftp.c` uses.

**They are a reading of lwIP's API and must match it.** `net_tftp.c`
fills `struct tftp_context` positionally, so its member order is the
real one -- lwIP 2.2.1 (ST's `v2.2.1_20250804`, `cmake/lwip.cmake`):
`open, close, read, write, error`. `struct pbuf` keeps lwIP's first
four members in lwIP's order; nothing here reads the rest. Check both
against `lwip/apps/tftp_common.h` and `lwip/pbuf.h` when `LWIP_TAG`
changes.

`lwip/tcp.h` is the raw TCP API `net_telnet.c` uses, for `fuzz-telnet`,
with lwIP 2.2.1's signatures; `tcp_sndbuf` and `tcp_nagle_disable`,
macros reading the pcb there, are functions here.
