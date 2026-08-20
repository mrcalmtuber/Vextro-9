#ifndef VX_FREESTANDING_STDIO_H
#define VX_FREESTANDING_STDIO_H

/*
 * third_party/include/stdio.h
 *
 * There are no files and no streams here, so this is the formatting
 * half of stdio and nothing else. printf and friends land on the serial
 * port; the FILE-based interface is absent entirely rather than stubbed,
 * so that any vendored file which genuinely needs it fails to compile
 * and gets configured off instead of silently doing nothing at runtime.
 */

#include <stddef.h>
#include <stdarg.h>

int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int puts(const char *s);

#endif
