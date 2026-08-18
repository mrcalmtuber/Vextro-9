/*
 * libc/string.c — the string and memory routines, for user space.
 *
 * Written plainly rather than cleverly. Word-at-a-time copies and the
 * bit tricks that find a zero byte in eight are worth real time on a
 * large buffer, and they are also where a C library goes wrong in ways
 * that only appear on unaligned input at the end of a page. The programs
 * this serves move a few kilobytes at a time; correctness is the whole
 * of what they need from it.
 *
 * memcpy and memset in particular are not optional. The compiler emits
 * calls to them from ordinary C — a structure assignment, an array
 * initialiser — whether or not the program mentions them.
 */

#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
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

/* Pads to n with NULs, and does not terminate if src is longer — both of
 * which are what the standard says and neither of which is what anyone
 * expects. It is here because code that already exists calls it. */
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
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
    d[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
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

static int in_set(char c, const char *set) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

size_t strspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && in_set(s[n], set)) n++;
    return n;
}

size_t strcspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && !in_set(s[n], set)) n++;
    return n;
}

/* The one function in here with memory between calls, which is why it is
 * the one function in here that cannot be used from two threads. That is
 * the standard's design, not this implementation's. */
char *strtok(char *s, const char *sep) {
    static char *save;
    if (!s) s = save;
    if (!s) return 0;
    s += strspn(s, sep);
    if (!*s) { save = 0; return 0; }
    char *tok = s;
    s += strcspn(s, sep);
    if (*s) { *s = '\0'; save = s + 1; }
    else      save = 0;
    return tok;
}
