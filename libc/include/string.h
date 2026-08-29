#ifndef _STRING_H
#define _STRING_H

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
 * string.h — the part of the C library a freestanding program cannot
 * actually do without.
 *
 * Not because programs call these by name especially often, but because
 * the compiler does. GCC turns a structure assignment into memcpy and a
 * zeroing loop into memset whether or not the source says so, which is
 * why every app in this tree used to be built with
 * -fno-tree-loop-distribute-patterns: there was nothing to link the
 * calls against. There is now, and that flag is gone.
 */

#include <stddef.h>

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

size_t  strlen(const char *s);
size_t  strnlen(const char *s, size_t max);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *hay, const char *needle);
size_t  strspn(const char *s, const char *set);
size_t  strcspn(const char *s, const char *set);
char   *strtok(char *s, const char *sep);

/* The re-entrant form. strtok keeps its position in a static, so two
 * threads tokenising two strings destroy each other's traversal; this
 * one keeps it in the caller's variable. Ported code uses it for exactly
 * that reason. */
char   *strtok_r(char *s, const char *sep, char **save);

char   *strdup(const char *s);
char   *strndup(const char *s, size_t n);

/* The OpenBSD forms, which return the length they *wanted* rather than
 * the length they wrote — so truncation is detectable, which is the
 * whole reason they exist and the reason strncpy's silent truncation is
 * a hazard. */
size_t  strlcpy(char *dst, const char *src, size_t cap);
size_t  strlcat(char *dst, const char *src, size_t cap);

char   *strpbrk(const char *s, const char *set);
void   *memrchr(const void *s, int c, size_t n);
void   *mempcpy(void *dst, const void *src, size_t n);
char   *strerror(int e);
char   *strcasestr(const char *hay, const char *needle);
char   *strchrnul(const char *s, int c);


#ifdef __cplusplus
}
#endif

#endif /* _STRING_H */
