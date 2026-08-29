#ifndef VEXTRO_LWIPOPTS_H
#define VEXTRO_LWIPOPTS_H

/*
 * third_party/lwip-port/lwipopts.h — lwIP 2.2.1, configured for this
 * machine.
 *
 * The system already had a TCP/IP stack: src/netstack.h, about twelve
 * hundred lines, and it worked. What it could not do is hold two
 * conversations at once. `tcp_state` was a single global, so a page
 * fetch and a package download could not overlap, DNS had one query in
 * flight, and "parallel" was not a matter of tuning -- there was one
 * connection's worth of state in the whole kernel and no second copy to
 * be had.
 *
 * That is what this replaces. lwIP is what a real TCP is: reassembly of
 * out-of-order segments, retransmission with backoff, Nagle, delayed
 * ACKs, window scaling, and as many connections as the pools below
 * allow.
 *
 * ---- NO_SYS = 0, and why it matters here ----
 *
 * lwIP can run without an operating system, as a poll loop. It is
 * configured the other way -- with threads, mailboxes and semaphores --
 * because the brief asked for parallel sockets, and the socket API
 * exists only in this mode. A socket call blocks the calling thread and
 * the tcpip thread keeps running; that *is* the parallelism, and it is
 * not available to a NO_SYS build at any setting.
 *
 * It is also the harder configuration to get right, because it means
 * everything below has to be thread-safe, and the port
 * (src/lwipglue.c) has to supply real blocking primitives rather than
 * spin loops. Those come from the scheduler's wait channels.
 */

/* ===== the operating system ===== */
#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_TCPIP_CORE_LOCKING     1

/* We supply real mutexes, so lwIP must not fall back to emulating them
 * out of binary semaphores -- which it does silently, and which is not
 * recursive-safe under core locking. */
#define LWIP_COMPAT_MUTEX           0
#define LWIP_COMPAT_MUTEX_ALLOWED   0

/* ===== memory =====
 *
 * The heap is the kernel's own. lwIP's alternative is a static array it
 * carves up itself, which would mean a fixed slice of RAM reserved for
 * networking whether or not anything was connected, and a second
 * allocator to account for separately. Routing mem_malloc at the slab
 * allocator instead is what makes a TLS session and a window title show
 * up in the same `free` figure.
 */
#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               8
#define mem_clib_malloc             vx_alloc
#define mem_clib_free               vx_free
#define mem_clib_calloc             vx_calloc

/* ===== pools =====
 *
 * Sized for the brief: eight simultaneous secure connections, each of
 * which is one TCP PCB and one netconn, plus the listener and resolver
 * traffic around them. Doubling that leaves room for the connections
 * that are closing -- a PCB sits in TIME_WAIT long after the
 * application has forgotten it, and a pool sized to the *active* count
 * runs out under exactly the workload it was sized for.
 */
#define MEMP_NUM_TCP_PCB            24
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_TCP_SEG            96
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_NETCONN            32
#define MEMP_NUM_NETBUF             32
#define MEMP_NUM_TCPIP_MSG_API      32
#define MEMP_NUM_TCPIP_MSG_INPKT    32
#define MEMP_NUM_SYS_TIMEOUT        16
#define MEMP_NUM_REASSDATA          8
#define MEMP_NUM_FRAG_PBUF          16
#define MEMP_NUM_ARP_QUEUE          16

#define PBUF_POOL_SIZE              64
#define PBUF_POOL_BUFSIZE           1536

/* ===== protocols ===== */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

/*
 * ===== the loopback interface =====
 *
 * On because ring 3 has sockets now, and a machine has to be able to
 * talk to itself.
 *
 * Two reasons, and the second is the one that made it necessary. A
 * program that connects to 127.0.0.1 is asking for something no network
 * is involved in, and answering "no route" would be wrong. And the boot
 * self-test that proves the new socket system calls actually carry bytes
 * has to do it without a server on the far side -- a test that needs the
 * outside world is a test that fails in a room with no network, which is
 * exactly the room a headless harness runs in.
 *
 * LWIP_HAVE_LOOPIF follows from this by default and is named anyway,
 * because what it does -- create the 127.0.0.1 interface at
 * initialisation -- is the part being relied on.
 *
 * LWIP_NETIF_LOOPBACK_MULTITHREADING is lwIP's own default when NO_SYS
 * is 0 and is what makes this safe here: a looped frame is handed to the
 * tcpip thread through tcpip_try_callback rather than being delivered on
 * the caller's stack, so a program sending to itself does not re-enter
 * the stack underneath its own send.
 */
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1
#define LWIP_LOOPBACK_MAX_PBUFS     16

/* ===== TCP tuning =====
 *
 * MSS is 1460: 1500 of Ethernet payload less twenty of IP and twenty of
 * TCP. Getting this wrong is not a performance question -- announce
 * more than the path carries and every full segment is dropped by a
 * router that will not fragment it, which presents as a connection that
 * completes its handshake and then hangs on the first large response.
 *
 * The window is eight segments. Smaller stalls a transfer waiting for
 * ACKs; much larger, on a stack whose receive path is a copy out of the
 * DMA ring, only buys retransmissions.
 */
/*
 * The send buffer is larger than the window because this stack now has
 * a bulk *sender* in it as well as clients: the remote desktop in
 * src/net/rdp.c pushes a two-megabyte screen refresh as fast as the
 * link will take it. At eight segments the buffer held one screen tile
 * and part of a second, so every third write met a full buffer and the
 * session spent its time stalled rather than sending.
 */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (24 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_QUEUE_OOSEQ             1
#define LWIP_TCP_SACK_OUT           1
#define TCP_LISTEN_BACKLOG          1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_WND_SCALE              1
#define TCP_RCV_SCALE               2

/* ===== the socket API ===== */
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1
/* Off deliberately. With this on, lwIP macro-defines read(), write(),
 * close() and select() over the top of whatever else is in scope --
 * which in a kernel that has its own read() and close() is not a name
 * clash so much as a silent rebinding of the filesystem. */
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET_SELECT          1
#define LWIP_SOCKET_POLL            0
#define LWIP_SO_RCVTIMEO            1

/*
 * SO_RCVTIMEO and SO_SNDTIMEO take a plain integer of milliseconds.
 *
 * On, and it fixes a bug rather than choosing a taste. lwIP's default is
 * to take a `struct timeval`, and vxnet_timeout() in src/lwipglue.c has
 * always passed an `int` -- so every call it made was rejected for a
 * wrong option length, and every caller ignored the return value.
 *
 * The consequences were real and silent. The HTTPS path asks for a
 * fifteen-second timeout before it sends a request, and never got one:
 * a peer that vanished without a FIN held that thread until the
 * connection was torn down some other way. The remote desktop's poll
 * timeout was in the same position. Nothing reported anything, because
 * a timeout that is never set looks exactly like a peer that is simply
 * slow.
 *
 * It surfaced when ring 3 got sockets and a test finally checked the
 * return value of setsockopt.
 *
 * Fixed here rather than in the glue because milliseconds is the unit
 * that seam already speaks, and this is the switch lwIP provides for
 * exactly that preference.
 */
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_SO_RCVBUF              1
#define LWIP_TCP_INFO               0
#define LWIP_NETBUF_RECVINFO        0
#define MEMP_NUM_SELECT_CB          16

/* There is no errno.h here and no per-thread errno; lwIP carries its
 * own rather than us inventing one. */
#define LWIP_PROVIDE_ERRNO          1

/* ===== threads =====
 *
 * The tcpip thread sits one priority level above the workers that feed
 * it. Equal priority is the subtle failure: eight threads all blocking
 * on socket calls hand the processor round between themselves, and the
 * thread that has to run for any of them to make progress waits its
 * turn behind all eight.
 *
 * Stack sizes are ignored -- every kernel thread here gets the
 * scheduler's 32 KB with an unmapped guard page under it -- and are
 * left at lwIP's defaults so that the numbers in its documentation
 * still mean something to anyone reading this.
 */
#define TCPIP_THREAD_NAME           "lwip"
#define TCPIP_THREAD_STACKSIZE      4096
#define TCPIP_THREAD_PRIO           5
#define TCPIP_MBOX_SIZE             32
#define DEFAULT_THREAD_STACKSIZE    4096
#define DEFAULT_THREAD_PRIO         4
#define DEFAULT_RAW_RECVMBOX_SIZE   16
#define DEFAULT_UDP_RECVMBOX_SIZE   16
#define DEFAULT_TCP_RECVMBOX_SIZE   16
#define DEFAULT_ACCEPTMBOX_SIZE     16

/* ===== checksums =====
 *
 * All in software. The e1000 can offload them, but the driver hands
 * lwIP a copy out of the DMA ring rather than the descriptor's status
 * word, so the flag that says "the hardware checked this" is not
 * available at the point the decision is made. Claiming the offload
 * without it would mean accepting corrupt frames.
 */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define LWIP_CHECKSUM_ON_COPY       1

/* ===== statistics and diagnostics ===== */
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   0    /* the kernel heap counts this */
#define MEMP_STATS                  1
#define LINK_STATS                  1
#define IP_STATS                    1
#define TCP_STATS                   1
#define UDP_STATS                   1

/* Off by default; `make iso EXTRA=-DVX_LWIP_DEBUG` turns the lot on. */
#ifdef VX_LWIP_DEBUG
#define LWIP_DEBUG                  1
#define TCP_DEBUG                   LWIP_DBG_ON
#define ETHARP_DEBUG                LWIP_DBG_ON
#define DHCP_DEBUG                  LWIP_DBG_ON
#define DNS_DEBUG                   LWIP_DBG_ON
#define SOCKETS_DEBUG               LWIP_DBG_ON
#endif

/* No assert-and-die in a kernel that has a desktop to keep drawing:
 * LWIP_PLATFORM_ASSERT in arch/cc.h names the failure on the serial
 * line and returns. */
#define LWIP_NOASSERT_ON_ERROR      0

#endif /* VEXTRO_LWIPOPTS_H */
