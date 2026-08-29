/*
 * libc/stdlib2.c — the conversions, the sort, and the string helpers
 * that ported code takes for granted.
 *
 * A second file rather than more of libc/stdlib.c's contents in
 * libc/malloc.c, where the original handful of stdlib functions live
 * beside the allocator for historical reasons. Nothing here allocates
 * except strdup, and keeping them apart means the allocator's file stays
 * about the allocator.
 */

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>

/* ===== INTEGER CONVERSION =====
 *
 * One parser, four entry points. The differences between strtoul and
 * strtoull are the width of the accumulator and where it saturates, and
 * writing the parse four times is how three of the four end up with a
 * subtly different idea of what counts as a valid prefix.
 */
static unsigned long long strtox(const char *s, char **end, int base,
                                 unsigned long long limit, int *negp) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    const char *digits = p;
    unsigned long long v = 0;
    int overflow = 0;

    for (;; p++) {
        int d;
        if      (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;

        /* Detected before it happens rather than after. Checking whether
         * the result came out smaller would work for base ten and not
         * for base sixteen, where a wrap can land on a larger value. */
        if (v > (limit - (unsigned long long)d) / (unsigned long long)base)
            overflow = 1;
        else
            v = v * (unsigned long long)base + (unsigned long long)d;
    }

    /*
     * No digits at all: the standard says the end pointer is set to the
     * *original* string, not to wherever the sign and prefix scanning
     * stopped. A caller distinguishes "parsed zero" from "parsed
     * nothing" only by that pointer, so getting it wrong turns a
     * malformed input into a silent zero.
     */
    if (p == digits) {
        if (end) *end = (char *)s;
        *negp = 0;
        return 0;
    }
    if (end) *end = (char *)p;
    if (overflow) { errno = ERANGE; *negp = neg; return limit; }
    *negp = neg;
    return v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtox(s, end, base, ULONG_MAX, &neg);
    return neg ? (unsigned long)(-(unsigned long)v) : (unsigned long)v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtox(s, end, base, 9223372036854775807ull + 1, &neg);
    if (!neg && v > 9223372036854775807ull) { errno = ERANGE; return 9223372036854775807ll; }
    if (neg && v > 9223372036854775807ull + 1) { errno = ERANGE; return -9223372036854775807ll - 1; }
    return neg ? (long long)(-(unsigned long long)v) : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtox(s, end, base, 18446744073709551615ull, &neg);
    return neg ? (unsigned long long)(-(unsigned long long)v) : v;
}

long long atoll(const char *s) { return strtoll(s, 0, 10); }
long long llabs(long long v)   { return v < 0 ? -v : v; }

div_t   div(int n, int d)             { div_t r   = { n / d, n % d }; return r; }
ldiv_t  ldiv(long n, long d)          { ldiv_t r  = { n / d, n % d }; return r; }
lldiv_t lldiv(long long n, long long d) { lldiv_t r = { n / d, n % d }; return r; }

int rand_r(unsigned int *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return (int)((*seed >> 1) & 0x7FFFFFFF);
}

/* ===== FLOATING-POINT CONVERSION =====
 *
 * strtod, and what it does and does not promise.
 *
 * A correctly rounded decimal-to-binary conversion is a genuinely hard
 * problem: the nearest double to "0.1" cannot be found by any fixed
 * amount of floating-point arithmetic, because the decimal digits and
 * the binary exponent do not share a base and the error accumulates.
 * Doing it properly means big-integer arithmetic — David Gay's algorithm
 * is about two thousand lines — and the payoff is the last bit.
 *
 * What is done instead: accumulate the digits into a 64-bit integer,
 * which is exact for the first nineteen of them, and then apply the
 * decimal exponent by multiplying or dividing by a power of ten taken
 * from an exact table. That is one rounding for the mantissa and one for
 * the scale, so the result is within an ulp or two rather than within
 * half of one.
 *
 * The consequence, stated because it is the kind of thing that surfaces
 * as a mysterious test failure: a round trip through printf("%.17g") and
 * back is not guaranteed to reproduce the original double. Nothing in
 * this system depends on that, and a program that does — a JSON parser
 * that must preserve numbers exactly, say — needs the real algorithm.
 */
static const double pow10_tab[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    /* The named values, which are part of the format and which a parser
     * that only looks for digits turns into zero. */
    if ((p[0] == 'i' || p[0] == 'I') && !strncasecmp(p, "inf", 3)) {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        if (end) *end = (char *)p;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if ((p[0] == 'n' || p[0] == 'N') && !strncasecmp(p, "nan", 3)) {
        p += 3;
        if (*p == '(') { while (*p && *p != ')') p++; if (*p) p++; }
        if (end) *end = (char *)p;
        return neg ? -nan("") : nan("");
    }

    /* Hexadecimal floats: 0x1.8p3. C99's spelling, and the only one that
     * round-trips a double exactly — which is why printf("%a") exists and
     * why anything that writes one expects this to read it back. */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || p[2] == '.')) {
        p += 2;
        double v = 0.0;
        int any = 0, bexp = 0;
        for (; isxdigit((unsigned char)*p); p++, any = 1) {
            int d = isdigit((unsigned char)*p) ? *p - '0'
                  : (tolower((unsigned char)*p) - 'a' + 10);
            v = v * 16.0 + (double)d;
        }
        if (*p == '.') {
            p++;
            for (; isxdigit((unsigned char)*p); p++, any = 1) {
                int d = isdigit((unsigned char)*p) ? *p - '0'
                      : (tolower((unsigned char)*p) - 'a' + 10);
                v = v * 16.0 + (double)d;
                bexp -= 4;
            }
        }
        if (!any) { if (end) *end = (char *)s; return 0.0; }
        if (*p == 'p' || *p == 'P') {
            const char *q = p + 1;
            int esign = 1;
            if (*q == '-') { esign = -1; q++; }
            else if (*q == '+') q++;
            if (isdigit((unsigned char)*q)) {
                int e = 0;
                for (; isdigit((unsigned char)*q); q++)
                    if (e < 100000) e = e * 10 + (*q - '0');
                bexp += esign * e;
                p = q;
            }
        }
        if (end) *end = (char *)p;
        double r = scalbn(v, bexp);
        return neg ? -r : r;
    }

    /*
     * The mantissa, into an integer.
     *
     * Only the first nineteen significant digits are accumulated —
     * beyond that a 64-bit integer would overflow, and beyond seventeen
     * they cannot change a double anyway. Digits past the cut still move
     * the exponent when they are before the point, which is what keeps
     * "12345678901234567890" from being read as if it were twenty times
     * smaller.
     */
    uint64_t mant = 0;
    int digits = 0, dexp = 0, any = 0, seen_point = 0;

    for (;; p++) {
        if (*p == '.' && !seen_point) { seen_point = 1; continue; }
        if (!isdigit((unsigned char)*p)) break;
        any = 1;
        if (digits < 19) {
            mant = mant * 10u + (uint64_t)(*p - '0');
            if (mant) digits++;
            if (seen_point) dexp--;
        } else {
            if (!seen_point) dexp++;
        }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }

    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int esign = 1;
        if (*q == '-') { esign = -1; q++; }
        else if (*q == '+') q++;
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            for (; isdigit((unsigned char)*q); q++)
                if (e < 100000) e = e * 10 + (*q - '0');
            dexp += esign * e;
            p = q;
        }
    }
    if (end) *end = (char *)p;

    if (mant == 0) return neg ? -0.0 : 0.0;

    double v = (double)mant;

    /*
     * Applying the exponent.
     *
     * Powers of ten up to 1e22 are exactly representable, so a single
     * multiply or divide by one of them is a single rounding. Larger
     * exponents are split into chunks of 22, which costs one rounding
     * per chunk — but an exponent that large has already lost more than
     * that to the mantissa cut above.
     *
     * Multiplying by a negative power rather than dividing would be one
     * operation either way and is *not* equivalent: 1e-22 is not exactly
     * representable, so multiplying by it rounds twice. Dividing by the
     * exact 1e22 rounds once.
     */
    if (dexp > 0) {
        while (dexp > 22) { v *= 1e22; dexp -= 22; if (isinf(v)) break; }
        if (dexp > 0) v *= pow10_tab[dexp];
    } else if (dexp < 0) {
        while (dexp < -22) { v /= 1e22; dexp += 22; if (v == 0.0) break; }
        if (dexp < 0) v /= pow10_tab[-dexp];
    }

    if (isinf(v) || v == 0.0) errno = ERANGE;
    return neg ? -v : v;
}

float strtof(const char *s, char **end)       { return (float)strtod(s, end); }
long double strtold(const char *s, char **end) { return (long double)strtod(s, end); }
double atof(const char *s)                     { return strtod(s, 0); }

/* ===== SORTING =====
 *
 * Shellsort with Ciura's gap sequence, extended by the usual factor of
 * 2.25 for the larger gaps.
 *
 * Not quicksort, and the reason is the environment rather than taste.
 * Quicksort's worst case is quadratic on an input an adversary chose,
 * and introsort's answer to that is to fall back to heapsort — two
 * algorithms and a depth counter. Shellsort has no worst case worth
 * naming, needs no recursion and therefore no stack depth on a system
 * where a thread's stack is a fixed mmap, and needs no scratch memory,
 * so it cannot fail. It is slower than a good quicksort by a constant
 * factor on large inputs; nothing here sorts a large input.
 */
static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
    static const size_t gaps[] = {
        1, 4, 10, 23, 57, 132, 301, 701, 1577, 3548, 7983, 17962,
        40414, 90931, 204595, 460339, 1035763, 2330467
    };
    if (!base || !cmp || size == 0 || n < 2) return;

    char *b = (char *)base;
    int gi = (int)(sizeof(gaps) / sizeof(gaps[0])) - 1;
    while (gi > 0 && gaps[gi] >= n) gi--;

    for (; gi >= 0; gi--) {
        size_t g = gaps[gi];
        for (size_t i = g; i < n; i++) {
            for (size_t j = i; j >= g; j -= g) {
                char *x = b + j * size;
                char *y = b + (j - g) * size;
                if (cmp(y, x) <= 0) break;
                swap_bytes(x, y, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    if (!key || !base || !cmp || size == 0) return 0;
    const char *b = (const char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        /* lo + (hi-lo)/2 rather than (lo+hi)/2: the sum overflows for a
         * large enough array, and the midpoint then lands outside it. */
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void *)(b + mid * size);
        if (r < 0) hi = mid;
        else       lo = mid + 1;
    }
    return 0;
}

/* ===== ALIGNED ALLOCATION =====
 *
 * malloc returns sixteen-byte alignment, which covers every ordinary
 * type and every SSE load. Anything stricter is obtained by taking more
 * than was asked for and moving the pointer up — and then the freeing
 * problem appears, because free() needs the pointer malloc returned.
 *
 * The offset is stored in the word immediately before the aligned
 * pointer. That word is inside the block malloc handed out, so writing
 * it is legal, and reading it back is how free() finds its way home.
 */
/*
 * ===== OVER-ALIGNED ALLOCATION, AND THE BUG THAT WAS HERE =====
 *
 * This used to record the offset back to the underlying block and hand
 * out the aligned address, and nothing else. The offset was written and
 * never read: free() takes the pointer it was given, steps back by the
 * header size, and reads a block header — which for an aligned pointer
 * is the middle of the padding, so it read a size and a next-pointer out
 * of whatever happened to be there and put that on the free list.
 *
 * It never showed, because until C++ arrived nothing in this system
 * over-aligned anything. `operator new(size, align_val_t)` made it
 * routine: the compiler emits it by itself for any type whose alignment
 * exceeds sixteen, so `new` of a struct holding an aligned(64) array was
 * enough. The symptom was a page fault reading through 0x900000014 — a
 * pointer assembled from the padding — one line after the allocation
 * that caused it.
 *
 * ---- how free() tells the two apart ----
 *
 * Three words immediately below the aligned pointer: a magic, the offset
 * back to the block malloc returned, and the usable size. free() checks
 * the magic before it does anything else.
 *
 * The check is exact rather than probable, and that is worth showing
 * rather than asserting. For an ordinary malloc'd pointer p, the two
 * words below it are the second half of block_t — the `free` flag and
 * the `pad` word — so p[-2] read as a 64-bit value can only ever be 0,
 * 1, or one of those two with BIG_MAGIC in the upper half. ALIGN_MAGIC
 * is none of the four, and cannot become one of them, because those
 * fields hold a boolean and a tag this file chooses.
 */
#define ALIGN_MAGIC ((size_t)0x414C474E5F565800ull)   /* "ALGN_VX\0" */

void *aligned_alloc(size_t align, size_t size) {
    if (align == 0 || (align & (align - 1))) { errno = EINVAL; return 0; }

    /* malloc already returns sixteen-byte aligned memory -- every block
     * is carved at a sixteen-byte multiple from a sixteen-aligned base
     * -- so anything up to that is an ordinary allocation and frees like
     * one. */
    if (align <= 16) return malloc(size);

    const size_t tag = 3 * sizeof(size_t);
    size_t total = size + align + tag;
    if (total < size) { errno = ENOMEM; return 0; }

    char *raw = (char *)malloc(total);
    if (!raw) return 0;

    /* At least the tag has to fit between the block and the aligned
     * address, which is why the search starts past it rather than at
     * `raw`. */
    uintptr_t from = (uintptr_t)raw + tag;
    uintptr_t a = (from + align - 1) & ~(uintptr_t)(align - 1);

    ((size_t *)a)[-1] = size;
    ((size_t *)a)[-2] = (size_t)(a - (uintptr_t)raw);
    ((size_t *)a)[-3] = ALIGN_MAGIC;
    return (void *)a;
}

/* What free() and realloc() ask before touching a pointer. Declared in
 * <stdlib.h> only for those two; nothing else has any business knowing
 * how an allocation was made. */
int __vx_aligned_block(const void *p, size_t *out_offset, size_t *out_size) {
    if (!p) return 0;
    const size_t *w = (const size_t *)p;
    if (w[-3] != ALIGN_MAGIC) return 0;
    if (out_offset) *out_offset = w[-2];
    if (out_size)   *out_size   = w[-1];
    return 1;
}

int posix_memalign(void **out, size_t align, size_t size) {
    if (!out) return EINVAL;
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    void *p = aligned_alloc(align, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

void *memalign(size_t align, size_t size) { return aligned_alloc(align, size); }

/* ===== THE ENVIRONMENT =====
 *
 * There is none, and these say so consistently. getenv answering null is
 * indistinguishable from a variable that is simply not set, so ported
 * code takes its default path; setenv reporting success and storing
 * nothing is the compromise, because reporting failure makes libraries
 * that configure themselves through the environment abort at startup.
 */
char *getenv(const char *name) { (void)name; return 0; }
int setenv(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite; return 0;
}
int unsetenv(const char *name) { (void)name; return 0; }
int putenv(char *s) { (void)s; return 0; }

void _Exit(int status) { _exit(status); }

/* ===== STRING HELPERS ===== */

char *strdup(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    if (!s) return 0;
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);
    if (!p) return 0;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/*
 * The OpenBSD copies, which return what they *wanted* to write.
 *
 * That is the whole difference from strncpy and strncat, and it is the
 * point: truncation is detectable. strncpy silently drops the tail and,
 * worse, does not terminate the result when it fills the buffer exactly.
 */
size_t strlcpy(char *dst, const char *src, size_t cap) {
    size_t n = strlen(src);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t cap) {
    size_t d = strnlen(dst, cap);
    size_t s = strlen(src);
    if (d == cap) return cap + s;
    size_t room = cap - d - 1;
    size_t c = s < room ? s : room;
    memcpy(dst + d, src, c);
    dst[d + c] = '\0';
    return d + s;
}

char *strtok_r(char *s, const char *sep, char **save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, sep);
    if (!*s) { *save = 0; return 0; }
    char *tok = s;
    s += strcspn(s, sep);
    if (*s) { *s = '\0'; *save = s + 1; }
    else    { *save = 0; }
    return tok;
}

char *strpbrk(const char *s, const char *set) {
    s += strcspn(s, set);
    return *s ? (char *)s : 0;
}

char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return 0;
}

void *mempcpy(void *dst, const void *src, size_t n) {
    return (char *)memcpy(dst, src, n) + n;
}

char *strcasestr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return (char *)hay;
    return 0;
}

/* ===== <inttypes.h> =====
 *
 * The four functions that header declares. They are here rather than in
 * a file of their own because each is one line over something this file
 * already has: intmax_t is `long` on this machine, so strtoimax *is*
 * strtoll and imaxabs *is* llabs. Written out anyway rather than as
 * macros, because a library that takes their address — and ported code
 * does — needs a symbol.
 */
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }

imaxdiv_t imaxdiv(intmax_t num, intmax_t den) {
    imaxdiv_t r;
    r.quot = num / den;
    r.rem  = num % den;
    return r;
}

intmax_t strtoimax(const char *s, char **end, int base) {
    return (intmax_t)strtoll(s, end, base);
}

uintmax_t strtoumax(const char *s, char **end, int base) {
    return (uintmax_t)strtoull(s, end, base);
}
