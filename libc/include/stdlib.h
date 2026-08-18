#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

/* The heap. Every one of these ends up at the kernel's sbrk. */
void   *malloc(size_t n);
void   *calloc(size_t count, size_t size);
void   *realloc(void *p, size_t n);
void    free(void *p);

/* Process control. exit() does not return; abort() is exit(134), which
 * is what a shell would report for a program killed by SIGABRT on a
 * system that had signals. */
void    exit(int status) __attribute__((noreturn));
void    abort(void) __attribute__((noreturn));

int     atoi(const char *s);
long    atol(const char *s);
long    strtol(const char *s, char **end, int base);

int     abs(int v);
long    labs(long v);

/* A linear congruential generator, deterministic and seedable — which is
 * what the graphical demos in apps/store want, since a picture that is
 * different every run is a picture that cannot be compared. */
int     rand(void);
void    srand(unsigned int seed);
#define RAND_MAX 0x7FFFFFFF

#endif /* _STDLIB_H */
