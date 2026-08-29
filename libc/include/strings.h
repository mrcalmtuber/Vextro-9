#ifndef _STRINGS_H
#define _STRINGS_H

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

/* strings.h — the case-insensitive comparisons, which the standard puts
 * in a header of their own for historical reasons that have long since
 * stopped applying. Ported code includes it by name. */

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int ffs(int v);

/* The BSD names for memset and memcpy. Present because ported code uses
 * them; note that bcopy takes its arguments the other way round from
 * memcpy, which is exactly why they were deprecated. */
void bzero(void *p, size_t n);
void bcopy(const void *src, void *dst, size_t n);
int  bcmp(const void *a, const void *b, size_t n);


#ifdef __cplusplus
}
#endif

#endif /* _STRINGS_H */
