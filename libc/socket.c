/*
 * libc/socket.c — the BSD socket interface, over this system's calls.
 *
 * Three jobs, and it is worth naming them because everything here is one
 * of the three:
 *
 *   Unpack a sockaddr. What crosses the system call boundary is four
 *   bytes of address and a port in host order; what a C program hands
 *   over is a struct sockaddr_in with the port in network order. The
 *   swap happens here, once, on this side — so that a mistake in it is a
 *   program's own bug rather than a kernel that has to guess which way
 *   round the last caller decided to send something.
 *
 *   Flatten setsockopt. The (level, name) pair is a numbering two
 *   systems can disagree about; three options actually do anything here,
 *   so three numbers cross the boundary and the mapping is done in one
 *   readable switch.
 *
 *   Resolve names. getaddrinfo and gethostbyname over one system call,
 *   with the two narrowings stated in <netdb.h> — every answer is IPv4,
 *   and there is always exactly one of them.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Mirrors of src/syscall.h. Two copies deliberately: a header a program
 * can edit is not one the kernel may read its constants from. */
#define VX_OPT_RCVTIMEO  1
#define VX_OPT_SNDTIMEO  2
#define VX_OPT_NODELAY   3

int socket(int domain, int type, int protocol) {
    return (int)__syscall_ret(__syscall3(SYS_SOCKET, domain, type, protocol));
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!addr || len < (socklen_t)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /* s_addr is in network order, which is to say the first byte of the
     * dotted quad is the low byte in memory. Copying the four bytes out
     * in memory order is therefore already the order the kernel wants,
     * and no swap is involved -- which is worth saying, because the
     * instinct is to reach for ntohl here and that would reverse it. */
    unsigned char ip[4];
    memcpy(ip, &sin->sin_addr.s_addr, 4);

    const unsigned port = ntohs(sin->sin_port);
    return (int)__syscall_ret(__syscall3(SYS_CONNECT, fd,
                                         (long)(uintptr_t)ip, (long)port));
}

int vx_connect_host(int fd, const char *host, unsigned short port) {
    return (int)__syscall_ret(__syscall3(SYS_CONNECT_HOST, fd,
                                         (long)(uintptr_t)host, (long)port));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    return (ssize_t)__syscall_ret(__syscall4(SYS_SEND, fd,
                                             (long)(uintptr_t)buf,
                                             (long)len, (long)flags));
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return (ssize_t)__syscall_ret(__syscall4(SYS_RECV, fd,
                                             (long)(uintptr_t)buf,
                                             (long)len, (long)flags));
}

int shutdown(int fd, int how) {
    return (int)__syscall_ret(__syscall2(SYS_SHUTDOWN, fd, (long)how));
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
    if (!val) { errno = EFAULT; return -1; }

    long opt, value;

    if (level == SOL_SOCKET &&
        (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        /* A struct timeval, which is what BSD defined and what ported
         * code passes; the call underneath takes milliseconds, because
         * that is the unit lwIP's own option takes and converting twice
         * would only introduce a rounding to argue about. */
        if (len < (socklen_t)sizeof(struct timeval)) { errno = EINVAL; return -1; }
        const struct timeval *tv = (const struct timeval *)val;
        value = tv->tv_sec * 1000L + tv->tv_usec / 1000L;
        opt = (name == SO_RCVTIMEO) ? VX_OPT_RCVTIMEO : VX_OPT_SNDTIMEO;
    } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        if (len < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
        value = *(const int *)val ? 1 : 0;
        opt = VX_OPT_NODELAY;
    } else {
        /*
         * Everything else is refused rather than accepted.
         *
         * SO_REUSEADDR, SO_KEEPALIVE, SO_SNDBUF: each is a real option
         * on a real system and none of them is reachable from here — the
         * seam the kernel exports carries three. Accepting them and
         * doing nothing would leave a program believing it had set a
         * keepalive, and discovering otherwise as a connection that
         * hangs for an hour.
         */
        errno = ENOPROTOOPT;
        return -1;
    }

    return (int)__syscall_ret(__syscall3(SYS_SOCKOPT, fd, opt, value));
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
    /*
     * SO_ERROR is the one that matters and the one that can be answered.
     * Every call here is blocking, so a connection either succeeded or
     * reported its failure at the time -- there is no pending error to
     * collect later, which is the case SO_ERROR exists for. Answering
     * zero is therefore true rather than a placeholder.
     */
    if (level == SOL_SOCKET && name == SO_ERROR &&
        val && len && *len >= (socklen_t)sizeof(int)) {
        *(int *)val = 0;
        *len = sizeof(int);
        return 0;
    }
    (void)fd;
    errno = ENOPROTOOPT;
    return -1;
}

/* ===== addresses as text ===== */

int inet_aton(const char *s, struct in_addr *out) {
    if (!s || !out) return 0;
    unsigned char b[4];
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return 0;
            s++;
        }
        b[part] = (unsigned char)v;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s != '\0') return 0;
    memcpy(&out->s_addr, b, 4);
    return 1;
}

in_addr_t inet_addr(const char *s) {
    struct in_addr a;
    if (!inet_aton(s, &a)) return INADDR_NONE;
    return a.s_addr;
}

char *inet_ntoa(struct in_addr in) {
    /* Static storage, as the interface has always specified and as is
     * the reason inet_ntop exists. */
    static char buf[INET_ADDRSTRLEN];
    const unsigned char *b = (const unsigned char *)&in.s_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    struct in_addr a;
    if (!inet_aton(src, &a)) return 0;
    memcpy(dst, &a.s_addr, 4);
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return 0; }
    if (!dst || size < INET_ADDRSTRLEN) { errno = ENOSPC; return 0; }
    const unsigned char *b = (const unsigned char *)src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}

/* ===== names ===== */

static int vx_resolve(const char *host, unsigned char out[4]) {
    return (int)__syscall_ret(__syscall2(SYS_RESOLVE, (long)(uintptr_t)host,
                                         (long)(uintptr_t)out));
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static unsigned char  addr[4];
    static char          *addr_list[2];
    static char           namebuf[128];
    static char          *aliases[1];

    if (!name) { errno = EINVAL; return 0; }
    if (vx_resolve(name, addr) != 0) return 0;

    strlcpy(namebuf, name, sizeof(namebuf));
    addr_list[0] = (char *)addr;
    addr_list[1] = 0;
    aliases[0]   = 0;

    he.h_name      = namebuf;
    he.h_aliases   = aliases;
    he.h_addrtype  = AF_INET;
    he.h_length    = 4;
    he.h_addr_list = addr_list;
    return &he;
}

/*
 * A service name to a port.
 *
 * There is no /etc/services on this volume, so this is a table of the
 * names a program is actually likely to pass — and a numeric string,
 * which getaddrinfo hands through here and which is the common case
 * anyway.
 */
static const struct { const char *name; int port; } vx_services[] = {
    { "http",   80 },  { "https", 443 }, { "ftp",     21 },
    { "ssh",    22 },  { "smtp",   25 }, { "domain",  53 },
    { "ntp",   123 },  { "imap",  143 }, { "pop3",   110 },
    { "telnet", 23 },  { "rdp",  3389 },
};

struct servent *getservbyname(const char *name, const char *proto) {
    static struct servent se;
    static char           namebuf[32];
    static char          *aliases[1];
    static char           protobuf[8];

    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(vx_services)/sizeof(vx_services[0]); i++) {
        if (strcmp(name, vx_services[i].name) != 0) continue;
        strlcpy(namebuf, name, sizeof(namebuf));
        strlcpy(protobuf, proto ? proto : "tcp", sizeof(protobuf));
        aliases[0]  = 0;
        se.s_name    = namebuf;
        se.s_aliases = aliases;
        se.s_port    = (int)htons((unsigned short)vx_services[i].port);
        se.s_proto   = protobuf;
        return &se;
    }
    return 0;
}

static int service_port(const char *service, unsigned short *out) {
    if (!service || !*service) { *out = 0; return 0; }
    if (service[0] >= '0' && service[0] <= '9') {
        long v = strtol(service, 0, 10);
        if (v < 0 || v > 65535) return EAI_SERVICE;
        *out = (unsigned short)v;
        return 0;
    }
    struct servent *se = getservbyname(service, "tcp");
    if (!se) return EAI_SERVICE;
    *out = ntohs((unsigned short)se->s_port);
    return 0;
}

/*
 * One allocation holding the addrinfo, the sockaddr it points at, and
 * the canonical name. Freeing it is freeing one block, which is what
 * makes freeaddrinfo correct without a walk — and correct is the word
 * that matters here, because a caller that frees a list built out of
 * separate allocations in the wrong order is a caller with a
 * use-after-free.
 */
typedef struct {
    struct addrinfo    ai;
    struct sockaddr_in sin;
    char               canon[128];
} vx_addrinfo_t;

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_SYSTEM;
    *res = 0;

    if (hints) {
        if (hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
            return EAI_FAMILY;
        if (hints->ai_socktype && hints->ai_socktype != SOCK_STREAM)
            return EAI_SERVICE;
    }

    unsigned short port = 0;
    int rc = service_port(service, &port);
    if (rc) return rc;

    unsigned char ip[4] = { 0, 0, 0, 0 };
    if (node) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) {
            struct in_addr a;
            if (!inet_aton(node, &a)) return EAI_NONAME;
            memcpy(ip, &a.s_addr, 4);
        } else if (vx_resolve(node, ip) != 0) {
            return errno == EPERM ? EAI_SYSTEM : EAI_NONAME;
        }
    } else {
        /* No node: a passive request, which on a system with no bind is
         * a question with no useful answer. Loopback is the honest one
         * -- it is the only address a program here could serve on if it
         * could serve at all. */
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
    }

    vx_addrinfo_t *a = (vx_addrinfo_t *)calloc(1, sizeof(vx_addrinfo_t));
    if (!a) return EAI_MEMORY;

    a->sin.sin_family = AF_INET;
    a->sin.sin_port   = htons(port);
    memcpy(&a->sin.sin_addr.s_addr, ip, 4);

    a->ai.ai_family   = AF_INET;
    a->ai.ai_socktype = SOCK_STREAM;
    a->ai.ai_protocol = IPPROTO_TCP;
    a->ai.ai_addrlen  = sizeof(struct sockaddr_in);
    a->ai.ai_addr     = (struct sockaddr *)&a->sin;
    a->ai.ai_next     = 0;

    if (node && hints && (hints->ai_flags & AI_CANONNAME)) {
        strlcpy(a->canon, node, sizeof(a->canon));
        a->ai.ai_canonname = a->canon;
    }

    *res = &a->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai) {
    /* The addrinfo is the first member of the block it was allocated in,
     * so its address is the block's address. */
    if (ai) free(ai);
}

const char *gai_strerror(int code) {
    switch (code) {
    case 0:            return "no error";
    case EAI_BADFLAGS: return "invalid flags";
    case EAI_NONAME:   return "no such host";
    case EAI_AGAIN:    return "temporary failure in name resolution";
    case EAI_FAIL:     return "name resolution failed";
    case EAI_FAMILY:   return "address family not supported";
    case EAI_MEMORY:   return "out of memory";
    case EAI_SERVICE:  return "no such service";
    case EAI_SYSTEM:   return "system error";
    default:           return "unknown error";
    }
}
