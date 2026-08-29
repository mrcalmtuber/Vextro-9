#ifndef VXNET_H
#define VXNET_H

/*
 * src/vxnet.h — the network, as the rest of this system sees it.
 *
 * Underneath are lwIP and Mbed TLS, about a quarter of a million lines
 * between them. Above is this: thirty functions in native types, no
 * external header, nothing that has to be included in a particular
 * order. kernel.c includes this and gets sockets and TLS; it never sees
 * `struct sockaddr`, `fd_set`, `mbedtls_ssl_context`, or lwIP's habit
 * of macro-defining htons() over the one in netstack.h.
 *
 * ---- what changed ----
 *
 * The system had a TCP/IP stack before this and it worked, for one
 * connection. `tcp_state` was a global; so were the sequence numbers,
 * the receive buffer and the DNS query in flight. Two things could not
 * use the network at once -- not slowly, not at all -- so downloading a
 * package and loading a page were mutually exclusive, and neither could
 * happen while the clock synchronised.
 *
 * Here, eight can, and eight of them can be encrypted.
 *
 * ---- what TLS here does not do ----
 *
 * It does not verify certificates. There is no certificate authority
 * store on the volume, so the chain is parsed and the server's
 * signature over the handshake is checked against the key in the leaf,
 * and *nothing* establishes that the leaf belongs to the host that was
 * asked for. That stops an eavesdropper and does not stop a machine in
 * the middle. vxsec_verifies_certificates() returns 0 and is what the
 * interface reports; it is not a detail to be discovered later.
 */

#include <stdint.h>

/* The receive thread sits above ordinary work and below the compositor:
 * frames must be drained promptly or the ring overruns, but not at the
 * cost of a dropped display frame. */
#define VXNET_PRIO_RX   6

/* ===== bring-up ===== */
int      vxnet_init(void);
int      vxnet_up(void);
void     vxnet_addr(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
void     vxnet_log_ip(uint32_t addr_net_order);

/* ===== plain sockets =====
 *
 * Blocking, one thread per connection. That is the shape the socket API
 * was designed for and the reason the port runs lwIP with an operating
 * system under it rather than as a poll loop: a thread waiting on a
 * socket costs nothing, so eight of them cost nothing eight times.
 */
int      vxnet_socket(void);
int      vxnet_connect(int s, const uint8_t ip[4], uint16_t port);

/* ===== the listening half =====
 *
 * A server socket, for the one thing in this system that is a server:
 * the remote desktop in src/net/rdp.c. vxnet_accept() blocks until
 * somebody connects and hands back a socket that behaves exactly like
 * a connected one from vxnet_connect(). */
int      vxnet_listen(uint16_t port, int backlog);
int      vxnet_accept(int s, uint8_t peer[4]);

int      vxnet_send(int s, const void *buf, int len);
int      vxnet_recv(int s, void *buf, int len);
void     vxnet_close(int s);

/*
 * Half-close. `how` is 0 for the read side, 1 for the write side, 2 for
 * both, which is what shutdown(2) has meant everywhere since 4.2BSD.
 *
 * Exposed because ring 3 has sockets now and this is the only way to
 * say "I have finished sending" without also saying "I have finished
 * listening" -- which is precisely the sequence an HTTP client that
 * sends a request and then reads a response until end-of-stream
 * depends on. Closing instead would discard the answer.
 */
int      vxnet_shutdown(int s, int how);
int      vxnet_timeout(int s, uint32_t ms);
int      vxnet_rcv_timeout(int s, uint32_t ms);
int      vxnet_snd_timeout(int s, uint32_t ms);
int      vxnet_nodelay(int s, int on);
int      vxnet_resolve(const char *host, uint8_t out[4]);

typedef struct {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_drop;
    uint32_t tcp_active;
    uint32_t tcp_listen;
    uint32_t tcp_tw;
} vxnet_stats_t;

void     vxnet_stats(vxnet_stats_t *out);

/* ===== secure connections =====
 *
 * Eight slots, which is the number the brief asked for. Each is an
 * independent TLS 1.3 session with its own record state, and each may
 * be driven from its own thread.
 *
 * The pool is fixed rather than grown on demand because a TLS context
 * with its buffers is about thirty kilobytes: eight is a quarter of a
 * megabyte of permanently resident kernel heap, and unbounded is a
 * machine that runs out of memory when a page has enough images on it.
 */
#define VXSEC_MAX 8

int      vxsec_init(void);
int      vxsec_ready(void);

/* Open a TLS 1.3 connection. Returns a slot index, or -1. */
int      vxsec_open(const char *host, uint16_t port);
int      vxsec_write(int slot, const void *buf, int len);
int      vxsec_read(int slot, void *buf, int len);
void     vxsec_close(int slot);

int      vxsec_active(void);
int      vxsec_slot_used(int slot);
const char *vxsec_cipher(int slot);
const char *vxsec_version(int slot);
const char *vxsec_peer_name(int slot);
int      vxsec_last_error(int slot);

/* Always 0, and deliberately so. See the note at the top. */
int      vxsec_verifies_certificates(void);

/* One HTTPS GET, start to finish, on a thread of its own if `background`
 * is set. The convenience the browser and the package store actually
 * use; returns the number of body bytes written to `out`. */
int      vxsec_https_get(const char *host, const char *path,
                         uint8_t *out, int max);

#ifdef NET_SELFTEST
/* Proves the four things a screenshot cannot show. See src/lwipglue.c. */
void     vxnet_selftest(void);
#endif

/* How much of a quarter megabyte the pool is presently holding. */
uint32_t vxsec_heap_kb(void);

#endif /* VXNET_H */
