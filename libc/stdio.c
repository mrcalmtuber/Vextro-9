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
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

/* write() moved to libc/file.c when descriptors arrived. It is the same
 * system call it always was; what it gained is a table to look the
 * descriptor up in, and an error number to report. */
#include <unistd.h>
#include <fcntl.h>

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

/*
 * puts writes straight to the descriptor rather than through the stream
 * layer, which is what it has always done here and is why it is up at
 * this end of the file with printf rather than down with fwrite. fputs
 * moved to the stream half when it was given its standard signature —
 * see the note in stdio.h — and the two no longer share an
 * implementation.
 */
int puts(const char *s) {
    size_t n = strlen(s);
    if (write(STDOUT_FILENO, s, n) < 0) return EOF;
    return write(STDOUT_FILENO, "\n", 1) < 0 ? EOF : 1;
}

int putchar(int c) {
    char ch = (char)c;
    return write(STDOUT_FILENO, &ch, 1) < 0 ? EOF : c;
}

/* ===== STREAMS =====
 *
 * There are files now, so a FILE is a real object rather than a name for
 * one of two destinations.
 *
 * ---- what the buffer is for ----
 *
 * Every read and every write here is a system call, and a system call on
 * this machine is a privilege transition with interrupts masked at the
 * far end. A getc() loop over a hundred-kilobyte file, unbuffered, is a
 * hundred thousand of them. So a stream opened by fopen carries four
 * kilobytes and fills or drains it in one call, which turns that same
 * loop into twenty-five.
 *
 * ---- and what it is not for ----
 *
 * stdout and stderr are unbuffered, deliberately and not by oversight.
 * They reach the terminal window, and the reason a program prints to the
 * terminal is usually to say what it is doing *while* it does it. A
 * buffered stderr is a diagnostic that arrives after the crash it was
 * describing. setvbuf can change it for a program that would rather have
 * the throughput.
 *
 * ---- and the one asymmetry worth knowing ----
 *
 * A stream open for writing does not reach the disk when it is flushed;
 * it reaches the *kernel*, which holds the file's image until the
 * descriptor is closed. fflush therefore means "this library is no
 * longer holding anything", and fsync (or fclose) is what means "it is
 * on the disk". The reason is in <fcntl.h>: the filesystem underneath
 * replaces whole files rather than updating them in place.
 */

#define VXF_READ   0x0001
#define VXF_WRITE  0x0002
#define VXF_EOF    0x0004
#define VXF_ERR    0x0008
#define VXF_OWNBUF 0x0010   /* the buffer came from malloc               */
#define VXF_NOCLOSE 0x0020  /* the standard streams: never close their fd */
#define VXF_APPEND 0x0040
#define VXF_DIRTY  0x0080   /* the buffer holds bytes not yet written out */

#define VXF_BUFSZ  4096

struct _VX_FILE {
    int            fd;
    unsigned short flags;
    short          mode;        /* _IOFBF, _IOLBF, _IONBF */
    unsigned char *buf;
    size_t         cap;
    size_t         len;         /* bytes valid (reading) or pending (writing) */
    size_t         pos;         /* cursor within buf, reading only */
    int            ungot;       /* a pushed-back byte, or -1 */
};

static struct _VX_FILE vx_stdin_obj = {
    0, VXF_READ | VXF_NOCLOSE, _IONBF, 0, 0, 0, 0, -1
};
static struct _VX_FILE vx_stdout_obj = {
    1, VXF_WRITE | VXF_NOCLOSE, _IONBF, 0, 0, 0, 0, -1
};
static struct _VX_FILE vx_stderr_obj = {
    2, VXF_WRITE | VXF_NOCLOSE, _IONBF, 0, 0, 0, 0, -1
};

FILE *stdin  = &vx_stdin_obj;
FILE *stdout = &vx_stdout_obj;
FILE *stderr = &vx_stderr_obj;

int fileno(FILE *f) {
    if (!f) { errno = EBADF; return -1; }
    return f->fd;
}

/* Push everything the stream is holding at the descriptor. Returns 0 or
 * EOF, and leaves the error flag set on failure so that a caller which
 * only checks at fclose still finds out. */
static int vx_drain(FILE *f) {
    if (!(f->flags & VXF_DIRTY) || !f->buf || !f->len) {
        f->len = 0;
        f->flags &= (unsigned short)~VXF_DIRTY;
        return 0;
    }
    size_t off = 0;
    while (off < f->len) {
        ssize_t n = write(f->fd, f->buf + off, f->len - off);
        if (n <= 0) {
            f->flags |= VXF_ERR;
            /* What could not be written stays in the buffer rather than
             * being discarded: a caller that retries after clearing the
             * condition should not have lost the bytes in between. */
            if (off) {
                memmove(f->buf, f->buf + off, f->len - off);
                f->len -= off;
            }
            return EOF;
        }
        off += (size_t)n;
    }
    f->len = 0;
    f->flags &= (unsigned short)~VXF_DIRTY;
    return 0;
}

/* Fill an empty read buffer. Returns the number of bytes available, 0 at
 * the end of the file, or -1. */
static long vx_fill(FILE *f) {
    if (!f->buf) return -1;
    f->pos = 0;
    f->len = 0;
    ssize_t n = read(f->fd, f->buf, f->cap);
    if (n < 0) { f->flags |= VXF_ERR; return -1; }
    if (n == 0) { f->flags |= VXF_EOF; return 0; }
    f->len = (size_t)n;
    return n;
}

static int vx_ensure_buf(FILE *f) {
    if (f->buf || f->mode == _IONBF) return 0;
    f->buf = (unsigned char *)malloc(VXF_BUFSZ);
    if (!f->buf) { f->mode = _IONBF; return -1; }
    f->cap = VXF_BUFSZ;
    f->flags |= VXF_OWNBUF;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) { errno = EINVAL; return 0; }

    int oflags = 0;
    unsigned short sflags = 0;
    const int plus = strchr(mode, '+') != 0;

    switch (mode[0]) {
    case 'r':
        oflags = plus ? O_RDWR : O_RDONLY;
        sflags = plus ? (VXF_READ | VXF_WRITE) : VXF_READ;
        break;
    case 'w':
        oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        sflags = plus ? (VXF_READ | VXF_WRITE) : VXF_WRITE;
        break;
    case 'a':
        oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        sflags = (plus ? (VXF_READ | VXF_WRITE) : VXF_WRITE) | VXF_APPEND;
        break;
    default:
        errno = EINVAL;
        return 0;
    }
    /* 'b' is accepted and means nothing, as it has on every system
     * without a text mode since the last one stopped shipping. */

    int fd = open(path, oflags, 0666);
    if (fd < 0) return 0;

    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) { close(fd); errno = ENOMEM; return 0; }
    f->fd    = fd;
    f->flags = sflags;
    f->mode  = _IOFBF;
    f->ungot = -1;
    vx_ensure_buf(f);
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    if (!mode) { errno = EINVAL; return 0; }
    unsigned short sflags = 0;
    const int plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': sflags = plus ? (VXF_READ | VXF_WRITE) : VXF_READ;  break;
    case 'w': sflags = plus ? (VXF_READ | VXF_WRITE) : VXF_WRITE; break;
    case 'a': sflags = (plus ? (VXF_READ | VXF_WRITE) : VXF_WRITE)
                       | VXF_APPEND; break;
    default:  errno = EINVAL; return 0;
    }

    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) { errno = ENOMEM; return 0; }
    f->fd    = fd;
    f->flags = sflags;
    /* A descriptor that reaches the console stays unbuffered, for the
     * reason the standard streams do. */
    f->mode  = (fd >= 0 && fd <= 2) ? _IONBF : _IOFBF;
    f->ungot = -1;
    vx_ensure_buf(f);
    return f;
}

int fclose(FILE *f) {
    if (!f) { errno = EBADF; return EOF; }
    int rc = vx_drain(f);
    if (!(f->flags & VXF_NOCLOSE)) {
        if (close(f->fd) != 0) rc = EOF;
    }
    if (f->buf && (f->flags & VXF_OWNBUF)) free(f->buf);
    if (f->flags & VXF_NOCLOSE) {
        /* One of the three standard streams: it is static storage and
         * outlives every close of it. Reset rather than freed. */
        f->buf = 0; f->cap = f->len = f->pos = 0;
        f->flags &= (unsigned short)~(VXF_OWNBUF | VXF_DIRTY);
        return rc;
    }
    free(f);
    return rc;
}

int fflush(FILE *f) {
    if (!f) {
        /* fflush(NULL) flushes every stream, and the three that can be
         * named without a registry are the standard ones. A stream this
         * library did not open is not reachable from here, which is why
         * fclose drains rather than relying on this. */
        int rc = 0;
        if (vx_drain(stdout) != 0) rc = EOF;
        if (vx_drain(stderr) != 0) rc = EOF;
        return rc;
    }
    return vx_drain(f);
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (!f) { errno = EBADF; return -1; }
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }
    /* Only before anything has been read or written, as the standard
     * requires -- swapping the buffer out from under pending bytes is
     * how they get lost. */
    if (f->len || f->pos) { errno = EBUSY; return -1; }

    if (f->buf && (f->flags & VXF_OWNBUF)) free(f->buf);
    f->buf = 0; f->cap = 0;
    f->flags &= (unsigned short)~VXF_OWNBUF;
    f->mode = (short)mode;

    if (mode != _IONBF) {
        if (buf && size) {
            f->buf = (unsigned char *)buf;
            f->cap = size;
        } else {
            vx_ensure_buf(f);
        }
    }
    return 0;
}

void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, VXF_BUFSZ);
}

/* ---- writing ---- */

static int vx_putc(FILE *f, unsigned char c) {
    if (!(f->flags & VXF_WRITE)) { f->flags |= VXF_ERR; errno = EBADF; return EOF; }

    if (f->mode == _IONBF || !f->buf) {
        if (write(f->fd, &c, 1) != 1) { f->flags |= VXF_ERR; return EOF; }
        return c;
    }
    if (f->len == f->cap && vx_drain(f) != 0) return EOF;
    f->buf[f->len++] = c;
    f->flags |= VXF_DIRTY;
    if (f->mode == _IOLBF && c == '\n' && vx_drain(f) != 0) return EOF;
    return c;
}

int fputc(int c, FILE *f) {
    if (!f) { errno = EBADF; return EOF; }
    return vx_putc(f, (unsigned char)c);
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
    if (!s || !f) { errno = EINVAL; return EOF; }
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

/* The name this function had while a descriptor-taking `fputs` sat
 * beside it. Kept because libcxx/ and tools/cxx_hostshim.h call it, and
 * renaming those would be churn with nothing behind it. */
int fputs_stream(const char *s, FILE *f) { return fputs(s, f); }

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!buf || !size || !count || !f) return 0;
    size_t total = size * count;
    /* An overflowed product would write far less than the caller asked
     * and report success for all of it. */
    if (total / size != count) { f->flags |= VXF_ERR; return 0; }
    if (!(f->flags & VXF_WRITE)) { f->flags |= VXF_ERR; errno = EBADF; return 0; }

    const unsigned char *p = (const unsigned char *)buf;

    /*
     * A write larger than the buffer goes straight out rather than
     * through it. Copying a megabyte into a four-kilobyte buffer two
     * hundred and fifty times, to write it out two hundred and fifty
     * times, is all of the cost of buffering and none of the benefit.
     */
    if (f->mode == _IONBF || !f->buf || total >= f->cap) {
        if (vx_drain(f) != 0) return 0;
        size_t off = 0;
        while (off < total) {
            ssize_t n = write(f->fd, p + off, total - off);
            if (n <= 0) { f->flags |= VXF_ERR; break; }
            off += (size_t)n;
        }
        return off / size;
    }

    if (f->len + total > f->cap && vx_drain(f) != 0) return 0;
    memcpy(f->buf + f->len, p, total);
    f->len += total;
    f->flags |= VXF_DIRTY;
    if (f->mode == _IOLBF && memchr(p, '\n', total)) {
        if (vx_drain(f) != 0) return 0;
    }
    return count;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    /*
     * Formatted into a buffer and written once.
     *
     * A character at a time would be simpler and would cost a system
     * call per character on an unbuffered stream, which stdout and
     * stderr are. Anything longer than the buffer is measured and
     * allocated rather than truncated -- which the previous version of
     * this did, silently, at a thousand and twenty-four characters.
     */
    if (!f) { errno = EBADF; return -1; }

    char stack[1024];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);
    if (n < 0) return n;

    if ((size_t)n < sizeof(stack)) {
        return fwrite(stack, 1, (size_t)n, f) == (size_t)n ? n : -1;
    }

    char *big = (char *)malloc((size_t)n + 1);
    if (!big) { f->flags |= VXF_ERR; errno = ENOMEM; return -1; }
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    size_t got = fwrite(big, 1, (size_t)n, f);
    free(big);
    return got == (size_t)n ? n : -1;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- reading ---- */

int fgetc(FILE *f) {
    if (!f) { errno = EBADF; return EOF; }
    if (!(f->flags & VXF_READ)) { f->flags |= VXF_ERR; errno = EBADF; return EOF; }

    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        return c;
    }
    /* Anything this stream is holding to write goes out before a read
     * moves the descriptor's position underneath it. */
    if (f->flags & VXF_DIRTY) { if (vx_drain(f) != 0) return EOF; }

    if (f->mode == _IONBF || !f->buf) {
        unsigned char c;
        ssize_t n = read(f->fd, &c, 1);
        if (n < 0) { f->flags |= VXF_ERR; return EOF; }
        if (n == 0) { f->flags |= VXF_EOF; return EOF; }
        return c;
    }
    if (f->pos >= f->len && vx_fill(f) <= 0) return EOF;
    return f->buf[f->pos++];
}

int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    /* One byte of pushback, which is all the standard guarantees and all
     * any parser written against it uses. */
    if (f->ungot >= 0) return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= (unsigned short)~VXF_EOF;
    return (unsigned char)c;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (!buf || !size || !count || !f) return 0;
    size_t total = size * count;
    if (total / size != count) { f->flags |= VXF_ERR; return 0; }
    if (!(f->flags & VXF_READ)) { f->flags |= VXF_ERR; errno = EBADF; return 0; }
    if (f->flags & VXF_DIRTY) { if (vx_drain(f) != 0) return 0; }

    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

    if (f->ungot >= 0) {
        p[got++] = (unsigned char)f->ungot;
        f->ungot = -1;
    }

    /* Whatever is already in the buffer, first. */
    while (got < total && f->buf && f->pos < f->len) {
        size_t n = f->len - f->pos;
        if (n > total - got) n = total - got;
        memcpy(p + got, f->buf + f->pos, n);
        f->pos += n;
        got += n;
    }

    /* Then straight into the caller's buffer, bypassing ours: a read of
     * a megabyte should not be four kilobytes at a time through an
     * intermediate copy. */
    while (got < total) {
        size_t want = total - got;
        if (want >= f->cap || f->mode == _IONBF || !f->buf) {
            ssize_t n = read(f->fd, p + got, want);
            if (n < 0) { f->flags |= VXF_ERR; break; }
            if (n == 0) { f->flags |= VXF_EOF; break; }
            got += (size_t)n;
        } else {
            if (vx_fill(f) <= 0) break;
            size_t n = f->len - f->pos;
            if (n > want) n = want;
            memcpy(p + got, f->buf + f->pos, n);
            f->pos += n;
            got += n;
        }
    }
    return got / size;
}

char *fgets(char *out, int cap, FILE *f) {
    if (!out || cap <= 0 || !f) return 0;
    int i = 0;
    while (i < cap - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        out[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;                 /* nothing read: end, or error */
    out[i] = '\0';
    return out;
}

/* ---- position ---- */

int fseek(FILE *f, long off, int whence) {
    if (!f) { errno = EBADF; return -1; }
    if (vx_drain(f) != 0) return -1;

    /* Anything buffered ahead of the cursor has to be given back before
     * the descriptor is moved, or the next read continues from a
     * position the caller has just changed. The descriptor's own offset
     * is ahead of the caller's by exactly what is left unread. */
    if (whence == SEEK_CUR && f->buf && f->pos < f->len)
        off -= (long)(f->len - f->pos);
    if (whence == SEEK_CUR && f->ungot >= 0) off -= 1;

    f->pos = f->len = 0;
    f->ungot = -1;
    f->flags &= (unsigned short)~VXF_EOF;

    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f) { errno = EBADF; return -1; }
    if (f->flags & VXF_DIRTY) { if (vx_drain(f) != 0) return -1; }
    long at = (long)lseek(f->fd, 0, SEEK_CUR);
    if (at < 0) return -1;
    /* Less whatever was read ahead and not yet handed to the caller. */
    if (f->buf && f->pos < f->len) at -= (long)(f->len - f->pos);
    if (f->ungot >= 0) at -= 1;
    return at;
}

int  fseeko(FILE *f, off_t off, int whence) { return fseek(f, (long)off, whence); }
off_t ftello(FILE *f) { return (off_t)ftell(f); }

void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    if (f) f->flags &= (unsigned short)~VXF_ERR;
}

int feof(FILE *f)   { return f && (f->flags & VXF_EOF) ? 1 : 0; }
int ferror(FILE *f) { return f && (f->flags & VXF_ERR) ? 1 : 0; }

void clearerr(FILE *f) {
    if (f) f->flags &= (unsigned short)~(VXF_EOF | VXF_ERR);
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    /* Unbounded, as the standard specifies and as is the reason it
     * should not be used. The bound below is what this library's
     * formatter will write in any case. */
    return vsnprintf(out, (size_t)-1, fmt, ap);
}

int vasprintf(char **out, const char *fmt, va_list ap) {
    if (!out) return -1;
    /*
     * Measured first, then formatted.
     *
     * vsnprintf with a null destination and a zero size returns the
     * length it *would* have written, which is what makes one pass to
     * measure and one to fill possible — and the alternative, guessing a
     * size and growing, would format the arguments twice in the common
     * case anyway. The argument list is consumed by the first pass, so
     * it has to be copied.
     */
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(0, 0, fmt, copy);
    va_end(copy);
    if (n < 0) { *out = 0; return -1; }

    char *p = (char *)malloc((size_t)n + 1);
    if (!p) { *out = 0; return -1; }
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    *out = p;
    return n;
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else         fprintf(stderr, "%s\n", strerror(errno));
}

int perror_prefix(const char *s) {
    perror(s);
    return 0;
}

/* ===== THE SCANNER =====
 *
 * scanf, and the four functions that are the same scanner over different
 * sources. It is here because ported code parses with it — a
 * configuration file, a header, a number out of a string — and a library
 * that formats but cannot read back is half a library.
 *
 * ---- one scanner, two sources ----
 *
 * The difference between sscanf and fscanf is where a character comes
 * from and where it goes back to when it turns out not to belong to the
 * field being read. Everything else — the widths, the suppression, the
 * length modifiers, the scan sets — is identical, and writing it twice
 * is how two of them end up disagreeing about whether "%d" accepts a
 * leading plus.
 *
 * ---- the pushback is one character, and that is enough ----
 *
 * Every conversion below reads until it sees something that cannot be
 * part of the field, and then puts that one character back. No
 * conversion needs two, which is the same reason ungetc only promises
 * one.
 */

typedef struct {
    FILE       *f;          /* exactly one of these two is set */
    const char *s;
    size_t      si;
    int         pushed;     /* -1, or the character put back */
    long        consumed;   /* for %n */
} scan_src_t;

static int sc_get(scan_src_t *sc) {
    if (sc->pushed >= 0) {
        int c = sc->pushed;
        sc->pushed = -1;
        sc->consumed++;
        return c;
    }
    int c;
    if (sc->f) {
        c = fgetc(sc->f);
    } else {
        c = sc->s[sc->si] ? (unsigned char)sc->s[sc->si] : EOF;
        if (c != EOF) sc->si++;
    }
    if (c != EOF) sc->consumed++;
    return c;
}

static void sc_unget(scan_src_t *sc, int c) {
    if (c == EOF) return;
    sc->pushed = c;
    sc->consumed--;
}

/* The length modifiers, as one value rather than a pair of flags,
 * because they are mutually exclusive and treating them otherwise
 * invites "%hhl". */
enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_CAP_L, LEN_Z, LEN_J, LEN_T };

/*
 * Macros rather than functions, and for a reason that is easy to get
 * wrong: va_list is an array type on this ABI, so a `va_list` parameter
 * is already a pointer and `&ap` inside a function that has one is a
 * pointer to a pointer. Passing the list to a helper that consumes an
 * argument therefore does not do what it reads as. Expanding in place
 * sidesteps the question entirely.
 */
#define STORE_SIGNED(len, v) do {                                          \
    long long __v = (long long)(v);                                        \
    switch (len) {                                                         \
    case LEN_HH: *va_arg(ap, signed char *) = (signed char)__v; break;     \
    case LEN_H:  *va_arg(ap, short *)       = (short)__v;       break;     \
    case LEN_L:  *va_arg(ap, long *)        = (long)__v;        break;     \
    case LEN_LL: *va_arg(ap, long long *)   = __v;              break;     \
    case LEN_Z:  *va_arg(ap, size_t *)      = (size_t)__v;      break;     \
    case LEN_J:                                                            \
    case LEN_T:  *va_arg(ap, long *)        = (long)__v;        break;     \
    default:     *va_arg(ap, int *)         = (int)__v;         break;     \
    }                                                                      \
} while (0)

#define STORE_UNSIGNED(len, v) do {                                        \
    unsigned long long __v = (unsigned long long)(v);                      \
    switch (len) {                                                         \
    case LEN_HH: *va_arg(ap, unsigned char *)  = (unsigned char)__v;  break;\
    case LEN_H:  *va_arg(ap, unsigned short *) = (unsigned short)__v; break;\
    case LEN_L:  *va_arg(ap, unsigned long *)  = (unsigned long)__v;  break;\
    case LEN_LL: *va_arg(ap, unsigned long long *) = __v;             break;\
    case LEN_Z:  *va_arg(ap, size_t *)         = (size_t)__v;         break;\
    case LEN_J:                                                            \
    case LEN_T:  *va_arg(ap, unsigned long *)  = (unsigned long)__v;  break;\
    default:     *va_arg(ap, unsigned int *)   = (unsigned int)__v;   break;\
    }                                                                      \
} while (0)

static int sc_skip_space(scan_src_t *sc) {
    int c;
    do { c = sc_get(sc); } while (c != EOF && isspace(c));
    if (c == EOF) return EOF;
    sc_unget(sc, c);
    return 0;
}

/* Read an integer in `base`, honouring a field width. Returns 0, or EOF
 * if the input ended before anything was read, or -1 for a field that
 * did not match. */
static int sc_integer(scan_src_t *sc, int base, int width, int is_signed,
                      unsigned long long *out, int *negative) {
    if (width <= 0) width = 1 << 30;

    int c = sc_get(sc);
    int neg = 0;
    if (c == '+' || c == '-') {
        if (c == '-') neg = 1;
        if (--width <= 0) { sc_unget(sc, c); return -1; }
        c = sc_get(sc);
    }

    /* 0x for hexadecimal, and for base 0 the prefix is what chooses the
     * base -- which is what %i means and the whole difference between it
     * and %d. */
    if (c == '0' && width > 1) {
        int n = sc_get(sc);
        if ((n == 'x' || n == 'X') && (base == 16 || base == 0)) {
            base = 16;
            width -= 2;
            c = sc_get(sc);
        } else {
            if (base == 0) base = 8;
            sc_unget(sc, n);
            /* The leading zero is itself a valid digit, so the field has
             * matched even if nothing follows. */
            unsigned long long v = 0;
            width--;
            for (;;) {
                c = sc_get(sc);
                int d;
                if      (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
                else break;
                if (d >= base || width-- <= 0) break;
                v = v * (unsigned)base + (unsigned)d;
            }
            sc_unget(sc, c);
            *out = v;
            if (negative) *negative = neg;
            (void)is_signed;
            return 0;
        }
    }
    if (base == 0) base = 10;

    int digits = 0;
    unsigned long long v = 0;
    for (;;) {
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        digits++;
        if (--width <= 0) { c = sc_get(sc); break; }
        c = sc_get(sc);
    }
    sc_unget(sc, c);

    if (!digits) return c == EOF ? EOF : -1;
    *out = v;
    if (negative) *negative = neg;
    (void)is_signed;
    return 0;
}

static int vx_scan(scan_src_t *sc, const char *fmt, va_list ap) {
    int assigned = 0;

    for (const char *p = fmt; *p; p++) {
        /* Whitespace in the format matches any run of it, including
         * none -- which is why this cannot fail. */
        if (isspace((unsigned char)*p)) {
            int c;
            do { c = sc_get(sc); } while (c != EOF && isspace(c));
            sc_unget(sc, c);
            continue;
        }

        if (*p != '%') {
            int c = sc_get(sc);
            if (c == EOF) return assigned ? assigned : EOF;
            if (c != (unsigned char)*p) { sc_unget(sc, c); return assigned; }
            continue;
        }

        p++;
        if (*p == '%') {
            int c = sc_get(sc);
            if (c == EOF) return assigned ? assigned : EOF;
            if (c != '%') { sc_unget(sc, c); return assigned; }
            continue;
        }

        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }

        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');

        int len = LEN_NONE;
        if      (p[0] == 'h' && p[1] == 'h') { len = LEN_HH; p += 2; }
        else if (p[0] == 'h')                { len = LEN_H;  p += 1; }
        else if (p[0] == 'l' && p[1] == 'l') { len = LEN_LL; p += 2; }
        else if (p[0] == 'l')                { len = LEN_L;  p += 1; }
        else if (p[0] == 'L')                { len = LEN_CAP_L; p += 1; }
        else if (p[0] == 'z')                { len = LEN_Z;  p += 1; }
        else if (p[0] == 'j')                { len = LEN_J;  p += 1; }
        else if (p[0] == 't')                { len = LEN_T;  p += 1; }

        const char conv = *p;
        if (!conv) return assigned;

        switch (conv) {

        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            if (sc_skip_space(sc) == EOF) return assigned ? assigned : EOF;
            int base = conv == 'd' || conv == 'u' ? 10
                     : conv == 'o' ? 8
                     : conv == 'i' ? 0 : 16;
            unsigned long long v = 0;
            int neg = 0;
            int rc = sc_integer(sc, base, width,
                                conv == 'd' || conv == 'i', &v, &neg);
            if (rc == EOF) return assigned ? assigned : EOF;
            if (rc != 0) return assigned;
            if (!suppress) {
                if (conv == 'd' || conv == 'i')
                    STORE_SIGNED(len, neg ? -(long long)v : (long long)v);
                else
                    STORE_UNSIGNED(len, neg ? 0ULL - v : v);
                assigned++;
            }
            break;
        }

        case 'p': {
            if (sc_skip_space(sc) == EOF) return assigned ? assigned : EOF;
            unsigned long long v = 0;
            int neg = 0;
            int rc = sc_integer(sc, 16, width, 0, &v, &neg);
            if (rc == EOF) return assigned ? assigned : EOF;
            if (rc != 0) return assigned;
            if (!suppress) {
                *va_arg(ap, void **) = (void *)(uintptr_t)v;
                assigned++;
            }
            break;
        }

        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            if (sc_skip_space(sc) == EOF) return assigned ? assigned : EOF;
            /*
             * Collected into a token and handed to strtod, rather than
             * accumulated digit by digit. That is not laziness: getting
             * the rounding of a decimal string to a double right is what
             * strtod is *for*, and a second implementation here would be
             * a second implementation to be subtly wrong.
             */
            char tok[64];
            int n = 0;
            if (width <= 0 || width > (int)sizeof(tok) - 1)
                width = (int)sizeof(tok) - 1;
            int c = sc_get(sc);
            if (c == '+' || c == '-') { tok[n++] = (char)c; c = sc_get(sc); }
            int seen_digit = 0, seen_dot = 0, seen_exp = 0;
            while (c != EOF && n < width) {
                if (c >= '0' && c <= '9') { seen_digit = 1; }
                else if (c == '.' && !seen_dot && !seen_exp) { seen_dot = 1; }
                else if ((c == 'e' || c == 'E') && seen_digit && !seen_exp) {
                    seen_exp = 1;
                    tok[n++] = (char)c;
                    c = sc_get(sc);
                    if (c == '+' || c == '-') {
                        if (n < width) tok[n++] = (char)c;
                        c = sc_get(sc);
                    }
                    continue;
                } else break;
                tok[n++] = (char)c;
                c = sc_get(sc);
            }
            sc_unget(sc, c);
            tok[n] = '\0';
            if (!seen_digit) return c == EOF && !assigned ? EOF : assigned;

            double d = strtod(tok, 0);
            if (!suppress) {
                if (len == LEN_L || len == LEN_CAP_L)
                    *va_arg(ap, double *) = d;
                else
                    *va_arg(ap, float *) = (float)d;
                assigned++;
            }
            break;
        }

        case 'c': {
            /* No leading whitespace is skipped, which is the whole
             * difference between %c and %1s and the reason both exist. */
            int n = width > 0 ? width : 1;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            for (; got < n; got++) {
                int c = sc_get(sc);
                if (c == EOF) break;
                if (out) out[got] = (char)c;
            }
            if (got < n) return assigned ? assigned : EOF;
            if (!suppress) assigned++;
            break;
        }

        case 's': {
            if (sc_skip_space(sc) == EOF) return assigned ? assigned : EOF;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            int cap = width > 0 ? width : (1 << 30);
            for (;;) {
                int c = sc_get(sc);
                if (c == EOF || isspace(c)) { sc_unget(sc, c); break; }
                if (n >= cap) { sc_unget(sc, c); break; }
                if (out) out[n] = (char)c;
                n++;
            }
            if (!n) return assigned ? assigned : EOF;
            if (out) out[n] = '\0';
            if (!suppress) assigned++;
            break;
        }

        case '[': {
            /* A scan set. `^` inverts, a `]` immediately after either of
             * those is a literal, and a-b is a range. */
            p++;
            int invert = 0;
            if (*p == '^') { invert = 1; p++; }
            unsigned char set[256];
            memset(set, 0, sizeof(set));
            if (*p == ']') { set[(unsigned char)']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[0] == '-' && p[1] && p[1] != ']' && p[-1] != '[') {
                    for (int c = (unsigned char)p[-1];
                         c <= (unsigned char)p[1]; c++) set[c] = 1;
                    p += 2;
                    continue;
                }
                set[(unsigned char)*p] = 1;
                p++;
            }
            if (!*p) return assigned;         /* unterminated set */

            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            int cap = width > 0 ? width : (1 << 30);
            for (;;) {
                int c = sc_get(sc);
                if (c == EOF) { sc_unget(sc, c); break; }
                int in = set[(unsigned char)c] ? 1 : 0;
                if (invert) in = !in;
                if (!in || n >= cap) { sc_unget(sc, c); break; }
                if (out) out[n] = (char)c;
                n++;
            }
            if (!n) return assigned ? assigned : EOF;
            if (out) out[n] = '\0';
            if (!suppress) assigned++;
            break;
        }

        case 'n':
            /* Not an assignment, and so not counted -- which matters,
             * because a caller checking the return against the number of
             * fields would otherwise be off by one for every %n. */
            if (!suppress) STORE_SIGNED(len, sc->consumed);
            break;

        default:
            return assigned;
        }
    }
    return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    if (!s || !fmt) return EOF;
    scan_src_t sc = { 0, s, 0, -1, 0 };
    return vx_scan(&sc, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    if (!f || !fmt) return EOF;
    scan_src_t sc = { f, 0, 0, -1, 0 };
    int n = vx_scan(&sc, fmt, ap);
    /* The one character the scanner is holding belongs to the stream, not
     * to this call: without giving it back, every field would eat the
     * delimiter that ended the one before it. */
    if (sc.pushed >= 0) ungetc(sc.pushed, f);
    return n;
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return n;
}
