#ifndef _CTYPE_H
#define _CTYPE_H

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
 * ctype.h — character classes, for ASCII and nothing else.
 *
 * There is one locale in this system and it is "C". Saying so up front
 * is more useful than a table that could in principle be swapped,
 * because the consequence is real: isalpha() answers no for every byte
 * above 127, and a parser that relies on it to find letters in UTF-8
 * will find none. That is the correct answer for a byte-oriented
 * classifier — a byte of a multi-byte sequence is not a letter — and it
 * is the answer glibc gives in the C locale too.
 *
 * Written as functions rather than as the traditional table lookup. The
 * table is faster by a load, and it is also the source of the oldest
 * bug in this header's history: the table is indexed by the argument,
 * the argument is an int that may be EOF or a negative char, and the
 * lookup then reads before the start of the array. A comparison cannot
 * do that.
 */

int isalnum(int c);
int isalpha(int c);
int isascii(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);


#ifdef __cplusplus
}
#endif

#endif /* _CTYPE_H */
