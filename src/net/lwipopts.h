/* SPDX-License-Identifier: Apache-2.0 */
/*
 * lwipopts.h - what this firmware asks of lwIP.
 *
 * Every option here is chosen against one budget: the linker script hands
 * the guest whatever SRAM is left over after the firmware's .bss, so each
 * kilobyte the stack takes is a kilobyte the largest architecture test
 * does not get. The sizing work that made 274/274 fit left roughly 60 KiB
 * of headroom over the worst test's 222 KiB, and this is what spends part
 * of it. Read `ram` in the boot banner after changing anything below --
 * it prints what actually survived, not what was intended.
 *
 * Two structural decisions drive most of the rest:
 *
 *   NO_SYS = 1. There is no RTOS, so lwIP is a library the run loop calls
 *   between guest slices rather than a thread. That rules out the socket
 *   and netconn APIs and leaves the raw callback API, which is what
 *   net_telnet.c and net_tftp.c are written against.
 *
 *   The link is SLIP, which is point to point. There is no ARP, no
 *   Ethernet header and no broadcast, so all of that is off -- not as a
 *   size optimisation but because none of it means anything on a serial
 *   line with exactly two ends.
 */
#ifndef EMU_LWIPOPTS_H
#define EMU_LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TIMERS                 1

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * lwIP's own heap and pools, not libc's: this firmware has no malloc and
 * deliberately links nothing that would bring one in.
 */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4

/*
 * The heap serves outgoing data (PBUF_RAM), so it has to cover the TCP
 * send buffer of every connection that can be open at once plus the UDP
 * datagrams TFTP builds. One telnet client and one TFTP transfer.
 */
#define MEM_SIZE                    6144

/*
 * The pool serves incoming data. SLIP fills these one byte at a time and
 * chains them, so the buffer size is a granularity rather than a limit:
 * a full-MTU frame takes three. Eight is two such frames in flight.
 */
#define PBUF_POOL_SIZE              8
#define PBUF_POOL_BUFSIZE           512

/*
 * No link-layer header to leave room for -- SLIP frames its packets with
 * escape bytes rather than prefixing them, so an outgoing pbuf needs no
 * headroom at all. The default 14 is Ethernet's.
 */
#define PBUF_LINK_HLEN              0
#define PBUF_LINK_ENCAPSULATION_HLEN 0

#define MEMP_NUM_PBUF               8
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            8

/*
 * One timeout slot for the TFTP server, on top of what the stack itself
 * needs.
 *
 * The default is LWIP_NUM_SYS_TIMEOUT_INTERNAL, which counts exactly the
 * timers lwIP's own modules register and nothing an application adds.
 * Starting the TFTP server therefore overflows it, and the failure is
 * loud but arrives long after the cause -- the server comes up fine,
 * announces itself, and then the *first transfer* dies with
 *
 *   sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty
 *
 * which is a message about memory pools appearing while debugging a file
 * transfer. Anything else registering a timeout needs another slot here.
 *
 * Written in terms of a macro that is not defined yet: lwipopts.h is
 * included from the top of opt.h, and this is not expanded until
 * memp_std.h uses it, by which time it is. That is the documented idiom
 * rather than an accident.
 *
 * Four rather than one, and the headroom is deliberate. sys_timeout()
 * does not fail loudly enough to be worth running to the edge of: when
 * the pool is empty it asserts and then *returns*, leaving the caller's
 * timer simply not registered. For the TFTP server that timer is the
 * only thing that would ever close an abandoned session, so a single
 * leaked slot turns "a client was killed" into "no upload works again
 * until reset" -- which is what happened, and which took a board reset
 * per occurrence to clear. emu_net_tftp_poll() is the actual recovery;
 * this is so the situation is rarer to begin with. Each slot costs about
 * a dozen bytes.
 */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)

/* ------------------------------------------------------------------ */
/* Protocols                                                           */
/* ------------------------------------------------------------------ */

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ICMP                   1           /* ping is the bring-up test */
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0

/*
 * Point to point: the peer is whatever is on the other end of the wire,
 * and there is no address to resolve. Turning ARP off also drops the
 * whole Ethernet path, which is most of what LWIP_ETHERNET brings.
 */
#define LWIP_ARP                    0
#define LWIP_ETHERNET               0
#define LWIP_NETIF_HOSTNAME         0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0

/*
 * IP fragments would need a reassembly buffer as large as the datagram,
 * which is exactly the RAM this is trying not to spend. Nothing here
 * sends anything that needs fragmenting: the largest is a TFTP data
 * packet at 544 bytes, well inside the MTU.
 */
#define IP_REASSEMBLY               0
#define IP_FRAG                     0

/* ------------------------------------------------------------------ */
/* SLIP                                                                */
/* ------------------------------------------------------------------ */

#define LWIP_HAVE_SLIPIF            1
/*
 * Both default to !NO_SYS and would therefore already be 0, but stating
 * them is worth a line: the first says the byte loop is driven by
 * slipif_poll() from the run loop, and the second says sio_tryread is
 * called from thread context and may not be re-entered from an ISR. The
 * UART interrupt fills a ring in board.c and stops there.
 */
#define SLIP_USE_RX_THREAD          0
#define SLIP_RX_FROM_ISR            0

/*
 * The MTU, and with it the size of a fully received frame. 1500 matches
 * what the host end gets by default from `ip link set sl0 mtu 1500`;
 * both ends must agree, because SLIP carries no length and no way to
 * negotiate one -- a mismatch shows up as large transfers failing while
 * ping works.
 */
#define SLIP_MAX_SIZE               1500

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

/*
 * 536 is the segment size any IPv4 path must accept without fragmenting.
 * Telnet is a trickle of console output and TFTP does not use TCP at
 * all, so there is nothing here to gain from a larger one, and every
 * byte of TCP_SND_BUF comes out of MEM_SIZE above.
 */
#define TCP_MSS                     536
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_LISTEN_BACKLOG          0
#define LWIP_TCP_SACK_OUT           0

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/*
 * Statistics are counters in .bss and a debug build routes every message
 * through LWIP_PLATFORM_DIAG, which arch/cc.h has deliberately made a
 * no-op. Turn LWIP_DEBUG on together with a real DIAG when the stack
 * itself is under suspicion, not before.
 */
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/*
 * Checked at compile time rather than trusted: this port supplies no
 * sys_arch.c, so anything that pulled in the threading API would fail to
 * link with a name that does not obviously mean "NO_SYS was overridden".
 */
#if !NO_SYS
#error "this port has no sys_arch; NO_SYS must stay 1"
#endif

#endif /* EMU_LWIPOPTS_H */
