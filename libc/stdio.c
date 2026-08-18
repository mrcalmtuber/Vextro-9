/*
 * libc/stdio.c — one formatter, and everything else on top of it.
 *
 * vsnprintf does the work; printf is that plus a syscall, sprintf is
 * that with no bound. Writing it once means the three cannot disagree
 * about what "%08.3f" means, which is the usual way a small C library
 * ends up with two subtly different printfs.
 *
 * The bound is honoured properly: vsnprintf returns the length it
 * *would* have produced, so a caller can measure first and allocate
 * once, and it never writes past the buffer while doing so.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>

long write(int fd, const void *buf, size_t len) {
    return __syscall3(SYS_WRITE, (long)fd, (long)(uintptr_t)buf, (long)len);
}

/* ---- the sink ----
 *
 * A cursor that either writes or only counts, so the same loop serves a
 * bounded buffer and a length measurement. */
typedef struct {
    char  *buf;
    size_t cap;      /* 0 means: count only */
    size_t len;      /* what has been produced, bound or no bound */
} sink_t;

static void emit(sink_t *s, char c) {
    if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void emit_str(sink_t *s, const char *p, size_t n) {
    while (n--) emit(s, *p++);
}

static void emit_pad(sink_t *s, char c, int n) {
    while (n-- > 0) emit(s, c);
}

/* Unsigned to text in any base up to 16, written backwards into a local
 * and reversed by the caller's padding logic. */
static int utoa_rev(char *tmp, uint64_t v, unsigned base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    return n;
}

#define FLAG_LEFT   1
#define FLAG_ZERO   2
#define FLAG_PLUS   4
#define FLAG_SPACE  8
#define FLAG_ALT    16

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    sink_t s = { out, cap, 0 };

    while (*fmt) {
        if (*fmt != '%') { emit(&s, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { emit(&s, '%'); fmt++; continue; }

        int flags = 0;
        for (;; fmt++) {
            if      (*fmt == '-') flags |= FLAG_LEFT;
            else if (*fmt == '0') flags |= FLAG_ZERO;
            else if (*fmt == '+') flags |= FLAG_PLUS;
            else if (*fmt == ' ') flags |= FLAG_SPACE;
            else if (*fmt == '#') flags |= FLAG_ALT;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++;
                           if (width < 0) { flags |= FLAG_LEFT; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* Length modifiers. `long long` and `long` are the same width
         * here and `size_t` is one of them, so they collapse. */
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j') {
            if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j') lng = 1;
            fmt++;
        }

        char conv = *fmt++;
        char tmp[32];
        int  ndig = 0;
        const char *prefix = "";
        char sign = 0;

        switch (conv) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!(flags & FLAG_LEFT)) emit_pad(&s, ' ', pad);
            emit(&s, c);
            if (flags & FLAG_LEFT) emit_pad(&s, ' ', pad);
            continue;
        }
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            size_t n = prec >= 0 ? strnlen(p, (size_t)prec) : strlen(p);
            int pad = width - (int)n;
            if (!(flags & FLAG_LEFT)) emit_pad(&s, ' ', pad);
            emit_str(&s, p, n);
            if (flags & FLAG_LEFT) emit_pad(&s, ' ', pad);
            continue;
        }
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, long) : (int64_t)va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            if (v < 0)                 sign = '-';
            else if (flags & FLAG_PLUS)  sign = '+';
            else if (flags & FLAG_SPACE) sign = ' ';
            ndig = utoa_rev(tmp, mag, 10, 0);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int);
            ndig = utoa_rev(tmp, v, 10, 0);
            break;
        }
        case 'o': {
            uint64_t v = lng ? va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int);
            ndig = utoa_rev(tmp, v, 8, 0);
            if (flags & FLAG_ALT) prefix = "0";
            break;
        }
        case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int);
            ndig = utoa_rev(tmp, v, 16, conv == 'X');
            if ((flags & FLAG_ALT) && v) prefix = conv == 'X' ? "0X" : "0x";
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            ndig = utoa_rev(tmp, v, 16, 0);
            prefix = "0x";
            break;
        }
        case 'f': case 'F': {
            /*
             * Fixed notation, by splitting rather than by a rounding
             * library. The integer part is exact for anything a program
             * would print; the fraction is scaled by ten to the
             * precision and rounded once, which is where the halfway
             * cases are decided and the only place they are.
             */
            double d = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (d < 0) { sign = '-'; d = -d; }
            else if (flags & FLAG_PLUS)  sign = '+';
            else if (flags & FLAG_SPACE) sign = ' ';

            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            uint64_t whole = (uint64_t)d;
            double   frac  = (d - (double)whole) * scale + 0.5;
            uint64_t fracn = (uint64_t)frac;
            if (prec > 0 && (double)fracn >= scale) { fracn = 0; whole++; }

            char wtmp[32];
            int wn = utoa_rev(wtmp, whole, 10, 0);
            int total = wn + (prec > 0 ? prec + 1 : 0) + (sign ? 1 : 0);
            int pad = width - total;

            if (!(flags & FLAG_LEFT) && !(flags & FLAG_ZERO))
                emit_pad(&s, ' ', pad);
            if (sign) emit(&s, sign);
            if (!(flags & FLAG_LEFT) && (flags & FLAG_ZERO))
                emit_pad(&s, '0', pad);
            while (wn) emit(&s, wtmp[--wn]);
            if (prec > 0) {
                emit(&s, '.');
                char ftmp[32];
                int fn = utoa_rev(ftmp, fracn, 10, 0);
                emit_pad(&s, '0', prec - fn);
                while (fn) emit(&s, ftmp[--fn]);
            }
            if (flags & FLAG_LEFT) emit_pad(&s, ' ', pad);
            continue;
        }
        default:
            emit(&s, '%');
            emit(&s, conv);
            continue;
        }

        /* Shared tail for every integer conversion. Precision on an
         * integer is a minimum digit count, and it cancels the zero
         * flag — which is the rule everyone forgets. */
        int zeros = prec > ndig ? prec - ndig : 0;
        if (prec >= 0) flags &= ~FLAG_ZERO;
        int plen  = (int)strlen(prefix);
        int total = ndig + zeros + plen + (sign ? 1 : 0);
        int pad   = width - total;

        if (!(flags & FLAG_LEFT) && !(flags & FLAG_ZERO))
            emit_pad(&s, ' ', pad);
        if (sign) emit(&s, sign);
        emit_str(&s, prefix, (size_t)plen);
        if (!(flags & FLAG_LEFT) && (flags & FLAG_ZERO))
            emit_pad(&s, '0', pad);
        emit_pad(&s, '0', zeros);
        while (ndig) emit(&s, tmp[--ndig]);
        if (flags & FLAG_LEFT) emit_pad(&s, ' ', pad);
    }

    if (s.buf && s.cap) s.buf[s.len < s.cap ? s.len : s.cap - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* No bound, because the caller said so. The size is what a caller who
 * has thought about it would pass to snprintf and nothing here can
 * check it. */
int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

/* One line at a time through a stack buffer. A larger one would be a
 * larger stack frame in every program that prints, and the terminal
 * takes a bounded string anyway. */
int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = strlen(buf);
    write(STDOUT_FILENO, buf, len);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fputs(const char *s, int fd) {
    size_t n = strlen(s);
    return write(fd, s, n) < 0 ? EOF : (int)n;
}

int puts(const char *s) {
    if (fputs(s, STDOUT_FILENO) == EOF) return EOF;
    return write(STDOUT_FILENO, "\n", 1) < 0 ? EOF : 1;
}

int putchar(int c) {
    char ch = (char)c;
    return write(STDOUT_FILENO, &ch, 1) < 0 ? EOF : c;
}
