#ifndef _LIMITS_H
#define _LIMITS_H

/* C++ reaches these now.
 *
 * libcxx/ compiles against this same library, and a C++ compiler mangles
 * every name it sees unless told not to -- so without this the C++ side
 * would fail to link against `malloc` and find `_Z6mallocm` missing.
 * Placed immediately after the include guard rather than after the
 * #includes below it, which is safe here because everything this header
 * includes is either one of the compiler's own type-only headers or one
 * of ours, and both want the same treatment. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * limits.h — the ranges of the integer types on this target.
 *
 * x86-64 System V: int is 32 bits, long and pointers are 64, and char is
 * signed. Those are the only choices this file has to record, and they
 * are the same in the kernel and in user space because there is one
 * compiler and one target.
 */

#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX
#define MB_LEN_MAX  4

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535

#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MIN    (-9223372036854775807L - 1)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL

#define LLONG_MIN   (-9223372036854775807LL - 1)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

/*
 * ---- the same widths again, counted in bits ----
 *
 * WORD_BIT and LONG_BIT are the number of bits in an `int` and in a
 * `long`. They say nothing the three blocks above do not already say —
 * they are the same facts in a form that can be used as a shift count,
 * which is what makes them worth having: a hash that rotates a value
 * wants `h >> (WORD_BIT - 9)`, and computing that from INT_MAX is not
 * something a preprocessor can do.
 *
 * POSIX puts them in <limits.h> under the XSI option; glibc hides them
 * behind __USE_XOPEN and musl defines them unconditionally, which is
 * what this does. Added for libtasn1, whose parser_aux.c includes this
 * header with the comment `/`* WORD_BIT *`/` beside it and hashes every
 * node name that way.
 */
#define WORD_BIT    32
#define LONG_BIT    64

#define SSIZE_MAX   LONG_MAX
#define PATH_MAX    256
#define NAME_MAX    255
#define PAGE_SIZE   4096


#ifdef __cplusplus
}
#endif

#endif /* _LIMITS_H */
