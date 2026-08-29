#ifndef _NETDB_H
#define _NETDB_H

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
 * netdb.h — turning a name into an address.
 *
 * The resolver is in the kernel: lwIP's, over the DNS server DHCP
 * supplied. What this header does is present it through the two
 * interfaces ported code uses, and note the two places where the answer
 * is narrower than the interface allows.
 *
 *   Every result is IPv4. ai_family is ignored beyond refusing
 *   AF_INET6, because there is no IPv6 on this system to return an
 *   address for.
 *
 *   getaddrinfo returns exactly one result, never a list. lwIP's
 *   resolver answers with one address and the kernel seam carries one
 *   address; a linked list of one is what comes back, and code that
 *   walks ai_next to try alternatives will find there are none rather
 *   than being misled about how many it tried.
 *
 * Resolving a name is reaching the network — the question goes to a
 * server somewhere else — so the first one a program does may put a
 * prompt on the screen, exactly as connect() does. "localhost" and a
 * dotted quad are answered here without asking anybody.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};

/* The BSD spelling that a great deal of code still uses. */
#define h_addr h_addr_list[0]

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400
#define AI_ADDRCONFIG  0x0020

#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_MEMORY    -10
#define EAI_SERVICE   -8
#define EAI_SYSTEM    -11

/*
 * Not thread-safe, and that is the interface rather than a shortcut:
 * gethostbyname has returned a pointer to static storage since 4.2BSD
 * and every implementation of it has the same problem. Ported code that
 * cares uses getaddrinfo, which does not.
 */
struct hostent *gethostbyname(const char *name);

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *ai);
const char *gai_strerror(int code);

/* A service name to a port number, for the handful this system can be
 * expected to know without an /etc/services to read. Anything else
 * answers null, and a caller that passed a numeric string gets it
 * parsed. */
struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;              /* network order, as BSD defined it */
    char  *s_proto;
};

struct servent *getservbyname(const char *name, const char *proto);


#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */
