#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H

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
 * netinet/tcp.h — the one TCP option this system can act on.
 *
 * TCP_NODELAY turns off Nagle's algorithm, which is the option ported
 * code actually sets: a request-response protocol on a Nagled socket
 * waits for an acknowledgement that the peer is holding back because it
 * has nothing to say, and the round trip that should take a millisecond
 * takes forty.
 *
 * TCP_KEEPIDLE and the rest are named so that code referring to them
 * compiles; setsockopt answers EINVAL for them, rather than accepting a
 * value that would never take effect.
 */

#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6


#ifdef __cplusplus
}
#endif

#endif /* _NETINET_TCP_H */
