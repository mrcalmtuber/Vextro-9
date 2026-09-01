/*
 * third_party/libgpg-error-port/config.h — libgpg-error's build
 * configuration, decided here instead of by ./configure.
 *
 * Upstream is autotools: a configure script probes the machine and
 * writes this file. That script cannot run here, and not merely because
 * it is inconvenient — every one of its probes works by *linking and
 * running* a small program, and this target has no hosted environment to
 * run one in. A configure run on the build machine would faithfully
 * describe macOS on arm64, which is the one answer that is certainly
 * wrong.
 *
 * So the ninety-eight knobs in config.h.in are answered once, here, and
 * each answer is a fact about this repository rather than an opinion
 * about the platform. The same arrangement FreeType, libjpeg-turbo and
 * libepoxy are under.
 *
 * ---- the two that are load-bearing ----
 *
 * USE_POSIX_THREADS, because libgcrypt takes gpgrt locks around its
 * random pool and its module registry, and a lock that compiled to
 * nothing would be a data race that only appears under threads. This
 * system has pthreads — libc/pthread.c over SYS_CLONE and the futex —
 * and SIZEOF_PTHREAD_MUTEX_T below is what makes the public
 * gpgrt_lock_t large enough to hold one.
 *
 * And HAVE_W32_SYSTEM staying undefined, which selects posix-lock.c,
 * posix-thread.c and spawn-posix.c as the arch sources.
 */

#ifndef VX_GPG_ERROR_CONFIG_H
#define VX_GPG_ERROR_CONFIG_H

#define PACKAGE           "libgpg-error"
#define PACKAGE_NAME      "libgpg-error"
#define PACKAGE_TARNAME   "libgpg-error"
#define PACKAGE_VERSION   "1.50"
#define PACKAGE_STRING    "libgpg-error 1.50"
#define PACKAGE_BUGREPORT "https://bugs.gnupg.org"
#define PACKAGE_URL       ""
#define VERSION           "1.50"
#define BUILD_REVISION    "vextro"
#define BUILD_TIMESTAMP   "1970-01-01T00:00:00"
#define HOST_TRIPLET_STRING "x86_64-unknown-vextro"
#define LT_OBJDIR         ".libs/"

/* ---- what the C library here actually has ----
 *
 * Each of these is checkable with nm(1) against build/libvextro.a or by
 * looking in libc/include, which is the point of answering them by hand:
 * a probe that cannot fail would have said yes to all of them. */
#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_DLFCN_H 1
#define HAVE_STAT 1
#define HAVE_MMAP 1
#define HAVE_SETENV 1
#define HAVE_STPCPY 1
#define HAVE_VASPRINTF 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_PTRDIFF_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_LONG_DOUBLE 1
#define HAVE_GCC_ATTRIBUTE_ALIGNED 1
/* In libc/string.c. Said so rather than left undefined because
 * estream.c defines its own static fallback when this is absent, and a
 * static definition following our non-static declaration is an error
 * rather than a shadowing. */
#define HAVE_MEMRCHR 1
/*
 * <poll.h>, which selects estream's poll path over its select one.
 *
 * Both paths exist in _gpgrt_poll and this system has exactly one of the
 * two interfaces: poll, because select describes its descriptors as a
 * bitmap indexed by number and poll takes an array of the ones it cares
 * about. Leaving this undefined does not disable the feature — it
 * compiles the select branch, which then fails on fd_set. So the flag
 * is not "do we want poll", it is "which of the two does this machine
 * have", and the answer is checkable: libc/include/poll.h.
 */
#define HAVE_POLL_H 1

/* ---- and what it does not ----
 *
 * Left undefined, which is the whole value of writing this file by hand.
 * Every one of these would have come back "yes" from a probe that never
 * links, and each would have selected code calling a function that is
 * not in the archive:
 *
 *   getpwnam, getpwuid and <pwd.h>   there are accounts on this system
 *                                    and they are not a passwd file
 *   iconv, gettext, nl_langinfo      no locale machinery beyond "C";
 *                                    U_HAVE_NL_LANGINFO_CODESET is 0 on
 *                                    the ICU side for the same reason
 *   sys/select.h                     select's bitmap-of-descriptors is
 *                                    not implemented; poll is, and is
 *                                    what estream uses instead
 *   flockfile                        libc/stdio.c streams are not
 *                                    lockable; there is one console
 *   strlwr, rand                     not in libc/string.c or stdlib
 *   inet_pton                        addresses cross the boundary as
 *                                    four bytes, not as text
 *   strerror_r                       libc has strerror and not the
 *                                    reentrant form, so the code takes
 *                                    its non-reentrant path
 *   getrlimit                        no resource limits; see the note in
 *                                    the Makefile about spawn-posix.c
 *   threads.h / thrd_create          C11 threads; this is pthreads
 *   readline, CoreFoundation, W32    not this machine
 */

/* ---- threads ----
 *
 * POSIX threads, present in libc rather than in a separate -lpthread,
 * which is what USE_POSIX_THREADS_FROM_LIBC says. Not the "weak" variant:
 * that one probes at run time whether a threading library was linked in,
 * by taking the address of a pthread symbol and comparing it against
 * null — which requires weak symbols and a dynamic linker, and there is
 * neither here. Saying threads are simply present is both simpler and
 * true.
 */
#define HAVE_PTHREAD_API 1
#define USE_POSIX_THREADS 1
#define USE_POSIX_THREADS_FROM_LIBC 1
#define HAVE_PTHREAD_MUTEX_RECURSIVE 1

/*
 * How large a pthread_mutex_t is, which decides how large the *public*
 * gpgrt_lock_t is — the one a program embeds in its own structures.
 * Sixteen bytes here: a futex word, a kind, an owner and a depth (see
 * libc/include/pthread.h). The shipped lock-obj for
 * x86_64-unknown-linux-gnu reserves forty, which is larger than needed
 * and is the point: a public structure that is too big wastes bytes and
 * one that is too small corrupts whatever follows it.
 */
#define SIZEOF_PTHREAD_MUTEX_T 16

#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_UNSIGNED_LONG 8
#define SIZEOF_VOID_P 8

/* Every file is offered at its full size already; there is no 32-bit
 * off_t here to widen. */
#define _FILE_OFFSET_BITS 64

/*
 * What gpgrt_off_t is spelled as in the public header.
 *
 * mkheader substitutes this into gpg-error.h and *fails* without it —
 * "replacement for off_t not defined" — which is the one place in this
 * file where a missing answer stops the build rather than quietly
 * choosing a default. `long` because that is what libc/include/sys/types.h
 * defines off_t to be, and the public header must not say something
 * different from the library it describes.
 */
#define REPLACEMENT_FOR_OFF_T "long"

/*
 * Symbol visibility, off.
 *
 * GPGRT_USE_VISIBILITY makes every internal symbol hidden and every
 * public one default, which is how a shared object keeps its private
 * names private. There are no shared objects on this target, so the
 * attribute would decorate an archive with a property nothing reads —
 * and visibility.c's whole purpose (wrapping each public name around an
 * internal one) still works without it.
 */
/* #undef GPGRT_USE_VISIBILITY */

/* No gettext: there is one language here and no message catalogues to
 * load, so the translation macros compile to the identity. */
/* #undef ENABLE_NLS */

/*
 * ============================================================
 *  the trailer, which is not optional
 * ============================================================
 *
 * Everything above answers a question configure would have asked.
 * Everything below is configure's AH_BOTTOM — a block it appends to
 * config.h unconditionally — and leaving it out is not a smaller
 * configuration, it is a broken one.
 *
 * The way it fails is worth recording because it reads as a defect in
 * the library. `gpgrt-int.h` uses `estream_t` unguarded, and the public
 * header only typedefs that name under GPGRT_ENABLE_ES_MACROS. Without
 * this block, fourteen of the nineteen sources stop at
 *
 *     gpgrt-int.h:252: error: expected ')' before 'void'
 *       void (*fnc) (estream_t, void*);
 *
 * which points at a line that has been correct since 2014 and says
 * nothing about the switch three files away that was never set.
 */

/* Connect the generic estream-printf.c to our framework. */
#define _ESTREAM_PRINTF_REALLOC _gpgrt_realloc
#define _ESTREAM_PRINTF_EXTRA_INCLUDE "gpgrt-int.h"

/* The five the library builds itself with. Three of them expose names
 * the public header otherwise keeps behind an #ifdef, and the internals
 * use those names without asking. */
#define GPG_ERR_ENABLE_GETTEXT_MACROS 1
#define GPG_ERR_ENABLE_ERRNO_MACROS 1
#define GPGRT_ENABLE_ES_MACROS 1
#define GPGRT_ENABLE_LOG_MACROS 1
#define GPGRT_ENABLE_ARGPARSE_MACROS 1

#endif /* VX_GPG_ERROR_CONFIG_H */
