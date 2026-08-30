#ifndef _ERR_H
#define _ERR_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * err.h — BSD's four "print and exit" helpers.
 *
 * Written because libepoxy includes it and never uses it: its
 * dispatch_common.c has `#include <err.h>` at the top and not one call
 * to errx, warnx, err or warn anywhere in the file. That is a header a
 * port needs to *have* rather than a facility it needs to work, which is
 * exactly the case where the smallest honest thing is to provide the
 * four functions properly rather than an empty file — an empty header
 * would compile this library and then fail to link the next port that
 * actually called one of them.
 *
 * The behaviour is BSD's, which is worth stating because it surprises
 * people: err and warn append the strerror() text for the *current*
 * errno, and errx and warnx do not. The x is for "no errno".
 */

#include <stdarg.h>

/* Print the program's name, the formatted message, and — for the two
 * without the x — a colon and the current errno's text. err and errx
 * then end the process with `status`; warn and warnx return. */
void err(int status, const char *fmt, ...) __attribute__((noreturn));
void errx(int status, const char *fmt, ...) __attribute__((noreturn));
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);

void verr(int status, const char *fmt, va_list ap) __attribute__((noreturn));
void verrx(int status, const char *fmt, va_list ap) __attribute__((noreturn));
void vwarn(const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* _ERR_H */
