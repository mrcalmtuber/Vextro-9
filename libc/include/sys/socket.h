#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

/* C++ reaches these now.
 *
 * libcxx/ compiles against this same library, and a C++ compiler mangles
 * every name it sees unless told not to -- so without this the C++ side
 * would fail to link against `malloc` and find `_Z6mallocm` missing.
 * Placed immediately after the include guard rather than after the
 * #includes below it, which is safe here because everything this header
 * includes is either one of the compiler's own type-only headers or one
 * of ours, and both want the same treatment. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * sys/socket.h — the network, from ring 3.
 *
 * Underneath is lwIP, which the kernel has run since src/vxnet.h was
 * written and which until now nothing outside the kernel could reach.
 * What is here is the BSD interface over it, narrowed to what this
 * system can actually do and refusing loudly rather than quietly
 * elsewhere.
 *
 * ============================================================
 *  WHAT EXISTS
 * ============================================================
 *
 * AF_INET and SOCK_STREAM. TCP over IPv4, blocking, one connection per
 * descriptor. socket(), connect(), send(), recv(), shutdown(), close(),
 * and setsockopt for three options.
 *
 * SOCK_DGRAM is refused with EOPNOTSUPP and AF_INET6 with
 * EAFNOSUPPORT, at socket() rather than later, because a descriptor
 * that could be created and could never carry anything is a failure
 * discovered several layers into a port.
 *
 * There is no bind, listen or accept. Nothing in ring 3 is a server —
 * the one server on this machine is the remote desktop, and it is in the
 * kernel. Those three are absent rather than stubbed so that a port
 * fails to link against a name that does not work instead of calling one
 * that does nothing.
 *
 * ============================================================
 *  IPPROTO_TLS, AND ITS CAVEAT
 * ============================================================
 *
 * A protocol number this system invented. Pass it to socket() and the
 * descriptor is a TLS 1.3 session rather than a plain connection —
 * driven by Mbed TLS in the kernel, through the same send() and recv().
 *
 * It must be connected with vx_connect_host() rather than connect(),
 * because a TLS session needs the *name* of the host: the handshake
 * carries it, and resolution, connection and handshake are one operation
 * rather than three.
 *
 * The caveat, which is repeated everywhere this is mentioned because it
 * is exactly the kind of thing that gets discovered rather than read:
 *
 *     Certificates are not verified.
 *
 * There is no certificate authority store on this volume. The chain is
 * parsed and the server's signature over the handshake is checked
 * against the key in the leaf certificate, and *nothing establishes that
 * the leaf belongs to the host that was asked for*. That stops somebody
 * listening on the wire. It does not stop somebody in the middle of it.
 *
 * ============================================================
 *  WHAT A CONNECTION COSTS TO MAKE
 * ============================================================
 *
 * The first connection a program makes to anywhere other than loopback
 * puts a question on the screen naming the address, and the answer is
 * remembered for the life of that program — including a refusal, so a
 * program told no is not asked again a millisecond later. With nobody
 * signed in the answer is no, immediately.
 *
 * Loopback is never asked about. A connection to 127.0.0.0/8 does not
 * leave the machine.
 */

#include <sys/types.h>

#define AF_UNSPEC   0
/*
 * A socket in the filesystem, which this system does not have.
 *
 * Named here for the same reason AF_INET6 is: a program that mentions
 * the constant in a branch it does not take should compile, and one that
 * passes it to socket() should be refused *at the call*, by name, rather
 * than fail to build against a constant that every other Unix defines.
 * The kernel answers EAFNOSUPPORT — see the note at the top of this file
 * and struct sockaddr_un in <sys/un.h>.
 */
#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX
#define AF_INET     2
#define AF_INET6   10
#define PF_INET     AF_INET
#define PF_UNSPEC   AF_UNSPEC

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP 17

/* This system's own, and not a number anybody else uses. See above. */
#define IPPROTO_TLS 256

#define SOL_SOCKET  1

#define SO_REUSEADDR  2
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/*
 * The generic address, present so that the calls below can have the
 * signatures ported code expects. What actually crosses the system call
 * boundary is four bytes and a port in host order — see libc/socket.c —
 * because a structure layout and a byte order are two more things for
 * the two sides of a privilege boundary to disagree about.
 */
struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __pad[126];
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int shutdown(int fd, int how);

ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);

/* No message flags are supported and passing one is EOPNOTSUPP rather
 * than ignored: a caller that asked not to block and was blocked anyway
 * has been lied to. */
#define MSG_OOB      0x01
#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

int setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);

/*
 * Connect by name, which is the only way to open a TLS session and the
 * easy way to open a plain one.
 *
 * Not a standard function — the standard way is getaddrinfo followed by
 * connect, and that works here too. This exists because a TLS session
 * needs the name rather than the address, and because the resolver lives
 * on the other side of the system call boundary anyway.
 */
int vx_connect_host(int fd, const char *host, unsigned short port);


#ifdef __cplusplus
}
#endif

/*
 * A connected pair that never leaves the machine — two descriptors over
 * two rings, which is what a socketpair is and all anything here uses
 * one for. Declared beside the network calls because that is where a
 * program looks for it; implemented beside pipe() in libc/process.c
 * because that is what it is.
 */
int socketpair(int domain, int type, int protocol, int sv[2]);

#endif /* _SYS_SOCKET_H */