#ifndef VX_FREESTANDING_STDLIB_H
#define VX_FREESTANDING_STDLIB_H

/*
 * third_party/include/stdlib.h
 *
 * The allocation functions are declared and defined, and they go to the
 * kernel's slab allocator through src/vxport.h. Mbed TLS is configured
 * to reach them through mbedtls_calloc/mbedtls_free instead -- the hook
 * the brief asked for -- and lwIP through mem_clib_malloc; these
 * declarations exist for the handful of places inside the vendored code
 * that call malloc directly, so that such a call lands on the kernel
 * heap rather than failing to link.
 *
 * exit() and abort() are the interesting ones. In a kernel there is
 * nothing to exit *to*, so they do not terminate: they name themselves
 * on the serial line and park the calling thread. A library that
 * decides to abort must not take the desktop with it.
 */

#include <stddef.h>

void  *malloc(size_t n);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

int    atoi(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int    abs(int v);

void   abort(void);
void   exit(int status);

#define RAND_MAX 0x7FFFFFFF
int    rand(void);
void   srand(unsigned seed);

#endif
