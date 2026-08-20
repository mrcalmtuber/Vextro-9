/*
 * third_party/vxport.c — the C library the vendored code was written
 * against, in the two hundred lines of it that lwIP and Mbed TLS
 * actually use.
 *
 * This is not a libc. It is the closure of what a quarter of a million
 * lines of foreign code turned out to call, arrived at by compiling
 * them and reading the undefined symbols -- which is why there is a
 * strstr and no fopen, and why strtok_r is here and strtok is not.
 *
 * Nothing here is clever. Every function is the obvious implementation,
 * because the alternative is a subtle bug in the foundation of a TLS
 * stack, and none of these is on a path where the difference could be
 * measured. The one thing they all do is stay inside their bounds: a
 * string function in a certificate parser is reachable by anyone who
 * can get this machine to open a connection.
 *
 * memcpy, memset, memmove and memcmp are deliberately absent -- kernel.c
 * exports those, and defining them again would be a duplicate symbol.
 * See third_party/include/string.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "time.h"

#include "vxport.h"

int errno = 0;

/* ===== strings ===== */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

/*
 * The standard's strncpy, faithfully -- including the part everyone
 * gets wrong: it does *not* terminate if the source fills the buffer,
 * and it pads with zeros if the source is shorter. Callers in the
 * vendored code rely on both halves.
 */
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return 0;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)vx_alloc(n);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    return p;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) if (!strchr(accept, s[n])) break;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) if (strchr(reject, s[n])) break;
    return n;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *save = 0; return 0; }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) { *s = 0; *save = s + 1; } else { *save = 0; }
    return tok;
}

/* ===== allocation =====
 *
 * Straight through to the kernel's slab allocator. realloc has to know
 * how big the old block was, which the heap can answer -- without that
 * it would have to copy the *new* size out of the old block and read
 * past its end.
 */

void *malloc(size_t n)              { return vx_alloc((uint64_t)n); }
void *calloc(size_t n, size_t size) { return vx_calloc((uint64_t)n, (uint64_t)size); }
void  free(void *p)                 { vx_free(p); }

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { vx_free(p); return 0; }
    uint64_t old = vx_alloc_size(p);
    if (old >= (uint64_t)n) return p;
    void *q = vx_alloc((uint64_t)n);
    if (!q) return 0;
    const uint8_t *s = (const uint8_t *)p;
    uint8_t *d = (uint8_t *)q;
    for (uint64_t i = 0; i < old; i++) d[i] = s[i];
    vx_free(p);
    return q;
}

/* ===== numbers ===== */

int abs(int v) { return v < 0 ? -v : v; }

unsigned long strtoul(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)s;
    return v;
}

long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    unsigned long v = strtoul(s, end, base);
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }

/*
 * rand() is here because a couple of vendored files call it in code
 * paths that are compiled out but still have to link. It is not, and
 * must never become, the source of anything cryptographic: every key,
 * nonce and sequence number in this system comes from vx_random, which
 * is RDRAND. This is a linear congruential generator and it is
 * predictable from two outputs.
 */
static unsigned long rand_state = 1;
int  rand(void) {
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((rand_state >> 33) & 0x7FFFFFFF);
}
void srand(unsigned seed) { rand_state = seed; }

/* ===== formatting =====
 *
 * Shared with the one in src/tlsglue.c by declaration rather than by
 * copy: that file has the implementation, this exposes it under the
 * names the vendored code expects.
 */

int vx_mbed_snprintf(char *buf, size_t n, const char *fmt, ...);
int vx_mbed_printf(const char *fmt, ...);
int vx_vsnprintf_pub(char *buf, size_t size, const char *fmt, va_list ap);

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vx_vsnprintf_pub(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    return vx_vsnprintf_pub(buf, n, fmt, ap);
}

int printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int r = vx_vsnprintf_pub(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vx_log(buf);
    return r;
}

int puts(const char *s) { vx_log(s); vx_log("\n"); return 0; }

/* ===== time =====
 *
 * There is a real clock in this system, but not one shaped like time_t,
 * and Mbed TLS is built with MBEDTLS_HAVE_TIME off so nothing calls
 * these. They exist to satisfy the link, and they return a fixed value
 * rather than a plausible one so that anything which starts depending on
 * them fails obviously instead of drifting.
 */
time_t time(time_t *t) {
    time_t v = 0;
    if (t) *t = v;
    return v;
}

static struct tm tm_zero;
struct tm *gmtime(const time_t *t) { (void)t; return &tm_zero; }
struct tm *gmtime_r(const time_t *t, struct tm *out) {
    (void)t;
    if (out) *out = tm_zero;
    return out;
}

/* ===== the two that must not stop the machine ===== */

void abort(void) {
    vx_log("[port] a library called abort()\n");
    for (;;) vx_sleep_ms(1000);
}

void exit(int status) {
    (void)status;
    vx_log("[port] a library called exit()\n");
    for (;;) vx_sleep_ms(1000);
}
