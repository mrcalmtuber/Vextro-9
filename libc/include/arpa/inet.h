#ifndef _ARPA_INET_H
#define _ARPA_INET_H

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
 * arpa/inet.h — addresses as text.
 *
 * inet_addr's error return is the reason this header has a comment at
 * all. It answers INADDR_NONE — 255.255.255.255 — for a string it
 * cannot parse, which is also a perfectly valid address, so the two
 * cases are indistinguishable. That is a defect of the interface rather
 * than of this implementation and it is why inet_aton and inet_pton
 * exist; both are here, and new code should use inet_pton.
 */

#include <netinet/in.h>
#include <sys/types.h>

in_addr_t   inet_addr(const char *s);
int         inet_aton(const char *s, struct in_addr *out);
char       *inet_ntoa(struct in_addr in);

/* Only AF_INET. AF_INET6 answers -1 with errno EAFNOSUPPORT rather
 * than parsing an address this system has no way to reach. */
int         inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);


#ifdef __cplusplus
}
#endif

#endif /* _ARPA_INET_H */
