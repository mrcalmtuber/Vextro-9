#ifndef VX_FREESTANDING_STRING_H
#define VX_FREESTANDING_STRING_H

/*
 * third_party/include/string.h — the string functions, for code that was
 * written expecting a C library.
 *
 * This directory is on the include path of the *vendored* translation
 * units only. kernel.c never sees it, and that separation is load
 * bearing: src/klibc.h has its own strlen as a `static inline`, and two
 * definitions of strlen in one translation unit is an error, while two
 * in one program -- one static, one not -- is fine and is what happens
 * here.
 *
 * memcpy, memset, memmove and memcmp are declared but not implemented in
 * third_party/vxport.c. kernel.c already exports all four as global
 * symbols, because GCC emits calls to them for structure assignment and
 * loop idioms whether the source mentions them or not, and a freestanding
 * kernel that does not provide them does not link. Defining them again
 * here would be a duplicate symbol at link time.
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
char   *strdup(const char *s);
size_t  strspn(const char *s, const char *accept);
size_t  strcspn(const char *s, const char *reject);
char   *strtok_r(char *s, const char *delim, char **save);

#endif /* VX_FREESTANDING_STRING_H */
