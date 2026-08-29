#ifndef _STDLIB_H
#define _STDLIB_H

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

/* Re-entrant, because two threads sharing one generator get one
 * sequence between them and neither gets a reproducible one. */
int     rand_r(unsigned int *seed);

/* ---- the rest of the conversions ----
 *
 * strtod is the one that matters and the one that is easy to get subtly
 * wrong; see the note on it in libc/stdlib.c for what "correctly
 * rounded" costs and what is done instead.
 */
unsigned long      strtoul(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double             strtod(const char *s, char **end);
float              strtof(const char *s, char **end);
long double        strtold(const char *s, char **end);
double             atof(const char *s);
long long          atoll(const char *s);

long long llabs(long long v);

typedef struct { int quot, rem; }             div_t;
typedef struct { long quot, rem; }            ldiv_t;
typedef struct { long long quot, rem; }       lldiv_t;
div_t   div(int n, int d);
ldiv_t  ldiv(long n, long d);
lldiv_t lldiv(long long n, long long d);

/* ---- searching and sorting ---- */
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

/* ---- aligned allocation ----
 *
 * Needed by anything that hands a buffer to a vector instruction or to a
 * device, and by C++'s operator new for over-aligned types.
 */
void *aligned_alloc(size_t align, size_t size);

/*
 * Whether this pointer came from aligned_alloc, and where its underlying
 * block starts. For free() and realloc() and nothing else -- an
 * allocation's provenance is not something a program should have to
 * know, and this exists because the two of them genuinely do.
 */
int   __vx_aligned_block(const void *p, size_t *out_offset, size_t *out_size);
int   posix_memalign(void **out, size_t align, size_t size);
void *memalign(size_t align, size_t size);

/* ---- the environment ----
 *
 * There is none. getenv answers null for every name, which is the same
 * answer a real system gives for a variable that is not set, so ported
 * code takes its default path rather than a failure path. setenv reports
 * success and stores nothing: a program that sets a variable and reads
 * it back will not see it, and that is stated here rather than
 * discovered.
 */
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *s);

/* Registered by crt0 and run in reverse at exit; see libc/crt0.c. */
int   atexit(void (*fn)(void));

/* Ends the process without running any handler, which is what a child
 * that has forked and failed should do rather than flushing its
 * parent's state a second time. */
void  _Exit(int status) __attribute__((noreturn));

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1


#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
