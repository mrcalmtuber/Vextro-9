#ifndef _STRING_H
#define _STRING_H

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

#endif /* _STRING_H */
