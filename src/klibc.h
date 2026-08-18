#ifndef KLIBC_H
#define KLIBC_H

/*
 * src/klibc.h — the standard names, inside the kernel.
 *
 * This kernel has always had its own vocabulary: str_copy, str_eq,
 * str_len, uint_to_str. They work, thousands of lines depend on them,
 * and none of that is worth churning. What it did not have was the
 * names the *language* uses — the ones GCC emits calls to on its own,
 * and the ones any code brought in from outside expects to link
 * against. memcpy and memset were already defined in kernel.c for
 * exactly that reason; this is the rest of the set.
 *
 * malloc and free are the interesting ones. They are the kernel heap
 * under their standard names, so a translation unit written against a
 * hosted C library — a decoder, a parser, anything lifted from
 * elsewhere — links and runs without a shim per call site.
 *
 * Everything here is a real function rather than a macro. A macro named
 * `free` is a trap that goes off in whatever unrelated header happens to
 * have a member called that.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kheap.h"

/* ---- memory ----
 *
 * memcpy, memset, memmove and memcmp are defined in kernel.c, before any
 * header that might call them, and are declared there. Only the
 * remainder belongs here.
 */
static inline void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    while (n--) { if (*p == (uint8_t)c) return (void *)p; p++; }
    return 0;
}

/* ---- the heap under its usual names ---- */
static inline void *malloc(size_t n)                { return kmalloc(n); }
static inline void  free(void *p)                   { kfree(p); }
static inline void *calloc(size_t c, size_t s)      { return kcalloc(c, s); }
static inline void *realloc(void *p, size_t n)      { return krealloc(p, n); }

/* ---- strings ---- */
static inline size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static inline size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static inline char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

static inline char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

static inline char *strcat(char *d, const char *s) {
    char *r = d;
    d += strlen(d);
    while ((*d++ = *s++)) { }
    return r;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static inline char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

static inline char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

static inline char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

/*
 * ---- formatting ----
 *
 * A bounded formatter, because an unbounded one in a kernel is a way of
 * writing "corrupt the stack later". snprintf returns the length it
 * would have produced, so callers that need to measure can, and it never
 * writes past the buffer while measuring.
 *
 * The conversions are the ones this kernel actually has anything to say
 * with: signed and unsigned decimal, hex, characters, strings, pointers,
 * and — new, because there is now floating point to print — fixed-point
 * decimals. There is no %e and no %g; nothing here has ever wanted one.
 */
static inline int k_utoa_rev(char *tmp, uint64_t v, unsigned base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    return n;
}

static inline int kvsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    size_t len = 0;
    #define KPUT(c) do { \
        if (out && len + 1 < cap) out[len] = (c); \
        len++; \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') { KPUT(*fmt++); continue; }
        fmt++;
        if (*fmt == '%') { KPUT('%'); fmt++; continue; }

        int zero = 0, left = 0;
        for (;; fmt++) {
            if      (*fmt == '0') zero = 1;
            else if (*fmt == '-') left = 1;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'z') { lng = 1; fmt++; }

        char conv = *fmt++;
        char tmp[32];
        int  n = 0;
        char sign = 0;
        const char *str = 0;
        size_t slen = 0;

        switch (conv) {
        case 'c': tmp[n++] = (char)va_arg(ap, int); break;
        case 's':
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            slen = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str);
            break;
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, long) : (int64_t)va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            if (v < 0) sign = '-';
            n = k_utoa_rev(tmp, mag, 10, 0);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int);
            n = k_utoa_rev(tmp, v, 10, 0);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int);
            n = k_utoa_rev(tmp, v, 16, conv == 'X');
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            KPUT('0'); KPUT('x');
            n = k_utoa_rev(tmp, v, 16, 0);
            break;
        }
        case 'f': case 'F': {
            double d = va_arg(ap, double);
            if (prec < 0) prec = 3;
            if (d < 0) { sign = '-'; d = -d; }
            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            uint64_t whole = (uint64_t)d;
            uint64_t frac  = (uint64_t)((d - (double)whole) * scale + 0.5);
            if (prec > 0 && (double)frac >= scale) { frac = 0; whole++; }

            char wt[32];
            int wn = k_utoa_rev(wt, whole, 10, 0);
            if (sign) KPUT(sign);
            while (wn) KPUT(wt[--wn]);
            if (prec > 0) {
                KPUT('.');
                char ft[32];
                int fn = k_utoa_rev(ft, frac, 10, 0);
                for (int i = fn; i < prec; i++) KPUT('0');
                while (fn) KPUT(ft[--fn]);
            }
            continue;
        }
        default:
            KPUT('%'); KPUT(conv);
            continue;
        }

        int total = conv == 's' ? (int)slen : n + (sign ? 1 : 0);
        int pad = width - total;
        if (!left) while (pad-- > 0) KPUT(zero && conv != 's' ? '0' : ' ');
        if (sign) KPUT(sign);
        if (conv == 's') { for (size_t i = 0; i < slen; i++) KPUT(str[i]); }
        else             { while (n) KPUT(tmp[--n]); }
        if (left) while (pad-- > 0) KPUT(' ');
    }

    if (out && cap) out[len < cap ? len : cap - 1] = '\0';
    #undef KPUT
    return (int)len;
}

static inline int ksnprintf(char *out, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline int ksnprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* Straight to the debug port, which is where kernel diagnostics have
 * always gone. Bounded by a stack buffer for the same reason as above. */
static inline int kprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline int kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(buf);
    return n;
}

#endif /* KLIBC_H */
