#ifndef _NETINET_IN_H
#define _NETINET_IN_H

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
 * netinet/in.h — an IPv4 address, in the shape everything expects.
 *
 * The one thing worth knowing about this header is where the byte
 * swapping happens. sin_port and sin_addr are in *network* order, as
 * they have been since 4.2BSD, and the system call underneath takes a
 * port in host order and four bytes in address order. The conversion is
 * done once, in libc/socket.c, on this side of the boundary — so a
 * mistake in it is a program's own bug rather than a kernel that has to
 * be told which way round the last caller decided to send something.
 */

#include <sys/types.h>
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;          /* network order */
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;   /* network order */
    struct in_addr sin_addr;
    char           sin_zero[8];
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

#define INET_ADDRSTRLEN  16

/*
 * The four conversions.
 *
 * This machine is little-endian and always will be — the ARM tree is
 * frozen and this kernel is x86-64 throughout — so these could be plain
 * byte swaps. They are written as swaps guarded by nothing rather than
 * as identity functions, because a header that says "host to network"
 * and does nothing is a trap for whoever ports this next.
 *
 * Static inline because they appear in inner loops of anything that
 * parses a packet, and a function call to swap two bytes is the kind of
 * cost that is invisible in a profile and real in a total.
 */
static inline uint16_t __vx_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t __vx_bswap32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
}

static inline uint16_t htons(uint16_t v) { return __vx_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __vx_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __vx_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __vx_bswap32(v); }


#ifdef __cplusplus
}
#endif

#endif /* _NETINET_IN_H */
