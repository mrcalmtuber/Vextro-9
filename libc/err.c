/*
 * libc/err.c — BSD's err/warn family.
 *
 * See the note in libc/include/err.h for who asked: libepoxy includes
 * the header and calls none of it, and providing the header without the
 * functions would have moved the failure to whichever port called one
 * first.
 *
 * There is no program name to print. BSD's version prints
 * getprogname(), which comes from argv[0], and a program on this system
 * is entered as `_start` with argv only when it was reached through
 * execve — so there is nothing reliable to name. "error" is printed
 * instead, which is honest about what it is rather than wrong about
 * which program it came from.
 */

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void err_body(const char *fmt, va_list ap, int with_errno) {
    /* Captured before anything else runs, because fprintf below is
     * entitled to change it and BSD's contract is that the text belongs
     * to the errno at the point of the call. */
    const int saved = errno;

    fputs("error: ", stderr);
    if (fmt) vfprintf(stderr, fmt, ap);
    if (with_errno) {
        fputs(": ", stderr);
        fputs(strerror(saved), stderr);
    }
    fputc('\n', stderr);
}

void verr(int status, const char *fmt, va_list ap) {
    err_body(fmt, ap, 1);
    exit(status);
}

void verrx(int status, const char *fmt, va_list ap) {
    err_body(fmt, ap, 0);
    exit(status);
}

void vwarn(const char *fmt, va_list ap)  { err_body(fmt, ap, 1); }
void vwarnx(const char *fmt, va_list ap) { err_body(fmt, ap, 0); }

void err(int status, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    err_body(fmt, ap, 1);
    va_end(ap);
    exit(status);
}

void errx(int status, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    err_body(fmt, ap, 0);
    va_end(ap);
    exit(status);
}

void warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    err_body(fmt, ap, 1);
    va_end(ap);
}

void warnx(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    err_body(fmt, ap, 0);
    va_end(ap);
}
