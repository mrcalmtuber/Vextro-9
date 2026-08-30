#ifndef _WCHAR_H
#define _WCHAR_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wchar.h — wide characters, which on this target are whole code points.
 *
 * ---- what a wchar_t is here ----
 *
 * Four bytes, signed, holding one Unicode code point. That is the same
 * choice every Unix makes and the opposite of Windows, where wchar_t is
 * sixteen bits and a "wide character" may be half of a surrogate pair.
 * The distinction matters to ported code: on this system wcslen returns
 * a count of characters, and there is no encoding hiding inside a wide
 * string.
 *
 * The type itself is the compiler's -- __WCHAR_TYPE__ in C, a keyword in
 * C++ -- so this header never defines it and cannot disagree with the
 * ABI about it.
 *
 * ---- and what the multibyte encoding is ----
 *
 * UTF-8, and that is a real decision rather than a default.
 *
 * The standard says the conversion between char strings and wchar_t
 * strings uses the current locale's multibyte encoding, and this system
 * has one locale: "C". What the C locale's encoding *is* is left to the
 * implementation, and the honest answer here is UTF-8, because that is
 * what every byte string in this system already holds -- the browser's
 * documents, the font stack's text, NTFS names converted from UTF-16,
 * the encyclopedia. Choosing ASCII instead would make mbstowcs fail on
 * text this machine displays correctly every day.
 *
 * The conversion functions below are therefore a UTF-8 decoder and
 * encoder, and they reject malformed input rather than producing
 * replacement characters, which is what the standard requires: (size_t)-1
 * and EILSEQ.
 *
 * ---- ICU is why this exists now ----
 *
 * ICU's ustr_wcs.cpp implements u_strToWCS and u_strFromWCS on top of
 * mbstowcs and wcstombs, and its cwchar.h includes <wchar.h> whenever
 * U_HAVE_WCHAR_H, which is on by default. Setting that to zero was the
 * alternative and would have been a claim this system cannot support
 * wide characters, which is not true -- it simply had not been asked.
 */

#include <stddef.h>
#include <stdarg.h>

typedef __WCHAR_TYPE__ __vx_wchar_t;

#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

/*
 * wint_t must hold every wchar_t value plus WEOF, so it cannot be the
 * same type as a signed 32-bit wchar_t and still have a spare value.
 * unsigned int is the usual answer and is what this uses.
 */
typedef unsigned int wint_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * The conversion state.
 *
 * A partial multibyte character is what this is for: a caller feeding
 * bytes one at a time needs somewhere to keep the two bytes of a
 * three-byte sequence it has seen so far. Held as a byte count and the
 * bytes themselves rather than as an opaque word, because the whole
 * point of the type is that it is the implementation's to define and
 * this is the shape the decoder wants.
 */
typedef struct {
    unsigned char __count;      /* bytes buffered so far, 0 when idle */
    unsigned char __bytes[4];
} mbstate_t;

/* ===== length, copying, comparison ===== */

size_t   wcslen(const wchar_t *s);
size_t   wcsnlen(const wchar_t *s, size_t n);

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);

int      wcscmp(const wchar_t *a, const wchar_t *b);
int      wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);

/* ===== searching ===== */

wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set);
size_t   wcsspn(const wchar_t *s, const wchar_t *set);
size_t   wcscspn(const wchar_t *s, const wchar_t *set);
wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **state);

/* ===== the array forms, which do not stop at a null ===== */

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
int      wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

/* ===== conversion, which is UTF-8 both ways ===== */

size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

size_t mbrtowc(wchar_t *out, const char *src, size_t n, mbstate_t *st);
size_t wcrtomb(char *dst, wchar_t c, mbstate_t *st);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *st);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *st);

int    mbsinit(const mbstate_t *st);
int    mblen(const char *s, size_t n);

/* The longest UTF-8 sequence is four bytes. Named as the standard names
 * it so that ported code sizing a buffer gets the right answer. */
#ifndef MB_LEN_MAX
#define MB_LEN_MAX 4
#endif
#ifndef MB_CUR_MAX
#define MB_CUR_MAX ((size_t)4)
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WCHAR_H */
