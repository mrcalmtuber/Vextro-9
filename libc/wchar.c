/*
 * libc/wchar.c — wide characters, and the UTF-8 codec under them.
 *
 * See libc/include/wchar.h for what a wchar_t is here and why the
 * multibyte encoding is UTF-8.
 *
 * The string half is mechanical: the same functions as <string.h> with a
 * wider element and no encoding involved, because a wchar_t string is
 * already one code point per element. The interesting half is the codec,
 * and the interesting part of the codec is what it refuses.
 *
 * ---- what "malformed" means, and why it is not tolerated ----
 *
 * A UTF-8 decoder that accepts everything is a security problem, not a
 * lenient one. Three classes of input are rejected here:
 *
 *   Overlong forms. 0xC0 0x80 encodes U+0000 in two bytes. It decodes to
 *   the same character as a single 0x00 byte, so a check for "does this
 *   path contain a null" that looks at bytes disagrees with one that
 *   looks at characters. Every code point has exactly one encoding, and
 *   anything else is rejected.
 *
 *   Surrogates, U+D800..U+DFFF. They exist only as a UTF-16 escape
 *   mechanism and are not characters; encoded in UTF-8 they are a way to
 *   smuggle an unpaired half through something that only checks the
 *   decoded value.
 *
 *   Anything above U+10FFFF, which is not a code point at all.
 *
 * The standard's answer for all three is the same and this is it:
 * (size_t)-1, with errno set to EILSEQ, and nothing written.
 */

#include <wchar.h>
#include <errno.h>
#include <string.h>

/* ============================================================
 *  the string functions
 * ============================================================ */

size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t wcsnlen(const wchar_t *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src) {
    wchar_t *d = dst;
    while ((*d++ = *src++) != 0) {}
    return dst;
}

/*
 * wcsncpy, with the two behaviours people forget: it pads the whole
 * remainder with nulls rather than writing one, and it writes no
 * terminator at all if the source fills the buffer exactly.
 */
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src) {
    wchar_t *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != 0) {}
    return dst;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n) {
    wchar_t *d = dst;
    while (*d) d++;
    for (size_t i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = 0;
    return dst;
}

/*
 * Comparison is by value, not by locale, and the values are code
 * points -- so this is code point order. That is a well-defined order
 * and it is not alphabetical order in any language; anything that needs
 * the second one wants a collator, which is what ICU is for.
 */
int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return 0;
    return (*a < *b) ? -1 : 1;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (;; s++) {
        if (*s == c) return (wchar_t *)s;   /* finds the terminator too */
        if (!*s) return 0;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = 0;
    for (;; s++) {
        if (*s == c) found = s;
        if (!*s) break;
    }
    return (wchar_t *)found;
}

wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle) {
    if (!*needle) return (wchar_t *)hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && hay[i] == needle[i]) i++;
        if (!needle[i]) return (wchar_t *)hay;
    }
    return 0;
}

static int in_set(wchar_t c, const wchar_t *set) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set) {
    for (; *s; s++) if (in_set(*s, set)) return (wchar_t *)s;
    return 0;
}

size_t wcsspn(const wchar_t *s, const wchar_t *set) {
    size_t n = 0;
    while (s[n] && in_set(s[n], set)) n++;
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *set) {
    size_t n = 0;
    while (s[n] && !in_set(s[n], set)) n++;
    return n;
}

/* The state is the caller's, so this is the re-entrant form and the
 * only form: a hidden static would make two threads tokenising two
 * strings destroy each other's position. */
wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **state) {
    if (!s) s = *state;
    if (!s) return 0;

    s += wcsspn(s, sep);
    if (!*s) { *state = 0; return 0; }

    wchar_t *end = s + wcscspn(s, sep);
    if (*end) { *end = 0; *state = end + 1; }
    else      { *state = 0; }
    return s;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    return dst;
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n) {
    if (dst < src) {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (size_t i = n; i-- > 0;) dst[i] = src[i];
    }
    return dst;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) s[i] = c;
    return s;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == c) return (wchar_t *)&s[i];
    return 0;
}

/* ============================================================
 *  the codec
 * ============================================================ */

#define VX_EILSEQ_RETURN ((size_t)-1)
#define VX_INCOMPLETE    ((size_t)-2)

/* How many bytes the sequence starting with this byte occupies, or -1
 * if it cannot start one -- a continuation byte, or one of the values
 * UTF-8 never uses at all. */
static int utf8_seq_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return -1;                         /* 0x80..0xBF or 0xF8..0xFF */
}

/*
 * The lowest code point each sequence length is allowed to encode, which
 * is what makes overlong forms detectable: a three-byte sequence that
 * decodes below 0x800 was written the long way round and is rejected.
 */
static const unsigned long utf8_lowest[5] = { 0, 0, 0x80, 0x800, 0x10000 };

/*
 * Decode one character from up to n bytes.
 *
 * Returns the number of bytes consumed, VX_INCOMPLETE if n is too small
 * to tell, or VX_EILSEQ_RETURN if the bytes are not a valid encoding.
 * A decoded U+0000 returns 0, which is what mbrtowc must report.
 */
static size_t utf8_decode(wchar_t *out, const char *s, size_t n) {
    if (n == 0) return VX_INCOMPLETE;

    const unsigned char *p = (const unsigned char *)s;
    const int len = utf8_seq_len(p[0]);
    if (len < 0) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
    if ((size_t)len > n) {
        /* Could still be malformed; check what is there before saying
         * "incomplete", so that a caller feeding a broken stream one
         * byte at a time is told it is broken rather than kept waiting. */
        for (size_t i = 1; i < n; i++)
            if ((p[i] & 0xC0) != 0x80) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
        return VX_INCOMPLETE;
    }

    unsigned long cp;
    if (len == 1) {
        cp = p[0];
    } else {
        cp = (unsigned long)(p[0] & (0xFF >> (len + 1)));
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
            cp = (cp << 6) | (unsigned long)(p[i] & 0x3F);
        }
    }

    if (cp < utf8_lowest[len]) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
    if (cp >= 0xD800 && cp <= 0xDFFF) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
    if (cp > 0x10FFFF) { errno = EILSEQ; return VX_EILSEQ_RETURN; }

    if (out) *out = (wchar_t)cp;
    return cp == 0 ? 0 : (size_t)len;
}

/* Encode one code point. Returns bytes written, or (size_t)-1. `dst`
 * must have room for MB_LEN_MAX. */
static size_t utf8_encode(char *dst, wchar_t wc) {
    const unsigned long cp = (unsigned long)wc;

    if ((unsigned long)wc > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        errno = EILSEQ;
        return VX_EILSEQ_RETURN;
    }

    unsigned char *d = (unsigned char *)dst;
    if (cp < 0x80)    { d[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)   { d[0] = (unsigned char)(0xC0 | (cp >> 6));
                        d[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { d[0] = (unsigned char)(0xE0 | (cp >> 12));
                        d[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        d[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
    d[0] = (unsigned char)(0xF0 | (cp >> 18));
    d[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    d[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

int mbsinit(const mbstate_t *st) { return !st || st->__count == 0; }

size_t mbrtowc(wchar_t *out, const char *src, size_t n, mbstate_t *st) {
    static mbstate_t internal;
    if (!st) st = &internal;

    if (!src) { st->__count = 0; return 0; }

    /*
     * With bytes already buffered, the new ones are appended and the
     * whole thing decoded together -- and the count returned is only the
     * bytes taken *this* call, which is what a caller advancing its own
     * pointer needs.
     */
    if (st->__count) {
        unsigned char buf[8];
        const size_t have = st->__count;
        for (size_t i = 0; i < have; i++) buf[i] = st->__bytes[i];

        size_t take = n;
        if (take > sizeof(buf) - have) take = sizeof(buf) - have;
        for (size_t i = 0; i < take; i++) buf[have + i] = (unsigned char)src[i];

        const size_t r = utf8_decode(out, (const char *)buf, have + take);
        if (r == VX_EILSEQ_RETURN) { st->__count = 0; return r; }
        if (r == VX_INCOMPLETE) {
            if (have + take > sizeof(st->__bytes)) { errno = EILSEQ; st->__count = 0;
                                                     return VX_EILSEQ_RETURN; }
            for (size_t i = 0; i < take; i++) st->__bytes[have + i] = (unsigned char)src[i];
            st->__count = (unsigned char)(have + take);
            return VX_INCOMPLETE;
        }
        const size_t used = (r == 0 ? 1 : r);   /* a decoded NUL took one byte */
        st->__count = 0;
        return r == 0 ? 0 : used - have;
    }

    const size_t r = utf8_decode(out, src, n);
    if (r == VX_INCOMPLETE) {
        if (n > sizeof(st->__bytes)) { errno = EILSEQ; return VX_EILSEQ_RETURN; }
        for (size_t i = 0; i < n; i++) st->__bytes[i] = (unsigned char)src[i];
        st->__count = (unsigned char)n;
    }
    return r;
}

size_t wcrtomb(char *dst, wchar_t c, mbstate_t *st) {
    char scratch[MB_LEN_MAX];
    if (st) st->__count = 0;
    if (!dst) { dst = scratch; c = 0; }
    return utf8_encode(dst, c);
}

/*
 * mbstowcs and wcstombs, with the null-destination convention: dst == 0
 * means "how many would there be", and n is ignored.
 */
size_t mbstowcs(wchar_t *dst, const char *src, size_t n) {
    size_t written = 0;

    for (;;) {
        if (dst && written >= n) break;

        wchar_t wc = 0;
        /* The remaining length is unknown -- src is a C string -- so
         * MB_LEN_MAX is the bound, and a truncated sequence at the end
         * decodes as malformed rather than incomplete because the
         * terminator cannot be a continuation byte. */
        const size_t r = utf8_decode(&wc, src, MB_LEN_MAX);
        if (r == VX_EILSEQ_RETURN || r == VX_INCOMPLETE) {
            errno = EILSEQ;
            return VX_EILSEQ_RETURN;
        }
        if (r == 0) {                     /* the terminator */
            if (dst) dst[written] = 0;
            return written;
        }
        if (dst) dst[written] = wc;
        written++;
        src += r;
    }
    return written;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n) {
    size_t written = 0;
    char   one[MB_LEN_MAX];

    for (; *src; src++) {
        const size_t r = utf8_encode(one, *src);
        if (r == VX_EILSEQ_RETURN) return VX_EILSEQ_RETURN;

        if (!dst) { written += r; continue; }

        /* A character that would not fit whole is not written at all;
         * half a UTF-8 sequence in a buffer is worse than a short one. */
        if (written + r > n) return written;
        for (size_t i = 0; i < r; i++) dst[written + i] = one[i];
        written += r;
    }

    if (dst && written < n) dst[written] = 0;
    return written;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *st) {
    (void)st;
    if (!src || !*src) return 0;

    size_t written = 0;
    const char *p = *src;

    for (;;) {
        if (dst && written >= n) { *src = p; return written; }

        wchar_t wc = 0;
        const size_t r = utf8_decode(&wc, p, MB_LEN_MAX);
        if (r == VX_EILSEQ_RETURN || r == VX_INCOMPLETE) {
            errno = EILSEQ;
            if (dst) *src = p;
            return VX_EILSEQ_RETURN;
        }
        if (r == 0) {
            if (dst) { dst[written] = 0; *src = 0; }
            return written;
        }
        if (dst) dst[written] = wc;
        written++;
        p += r;
    }
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *st) {
    (void)st;
    if (!src || !*src) return 0;

    size_t written = 0;
    const wchar_t *p = *src;
    char one[MB_LEN_MAX];

    for (; *p; p++) {
        const size_t r = utf8_encode(one, *p);
        if (r == VX_EILSEQ_RETURN) { if (dst) *src = p; return VX_EILSEQ_RETURN; }

        if (!dst) { written += r; continue; }
        if (written + r > n) { *src = p; return written; }
        for (size_t i = 0; i < r; i++) dst[written + i] = one[i];
        written += r;
    }

    if (dst) {
        if (written < n) dst[written] = 0;
        *src = 0;
    }
    return written;
}

int mblen(const char *s, size_t n) {
    if (!s) return 0;                 /* the encoding is stateless */
    const size_t r = utf8_decode(0, s, n);
    if (r == VX_EILSEQ_RETURN || r == VX_INCOMPLETE) return -1;
    return r == 0 ? 0 : (int)r;
}
