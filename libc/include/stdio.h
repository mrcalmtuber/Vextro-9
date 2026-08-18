#ifndef _STDIO_H
#define _STDIO_H

/*
 * stdio.h — formatting, and one place for output to go.
 *
 * There are no files and no FILE objects here. This machine's console is
 * the terminal window, reached by a syscall, and inventing a stream
 * abstraction over a single destination would be a header full of
 * promises nothing keeps. What programs actually need from stdio is
 * printf, and printf is here in full: the flags, the width, the
 * precision, and the length modifiers, for every conversion this
 * environment can mean anything by.
 *
 * %f and friends are supported — floating point is available to user
 * programs now — and they format through the ordinary integer path after
 * scaling, which is exact for the magnitudes a program prints and does
 * not drag in a rounding library.
 */

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

/* The two descriptors that exist. Both reach the same terminal. */
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintf(const char *fmt, va_list ap);

int  sprintf(char *out, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
int  snprintf(char *out, size_t cap, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);

int  puts(const char *s);
int  putchar(int c);
int  fputs(const char *s, int fd);

/* Raw, unformatted, straight to the terminal. */
long write(int fd, const void *buf, size_t len);

#endif /* _STDIO_H */
