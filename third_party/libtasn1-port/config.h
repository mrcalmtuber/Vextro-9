/*
 * third_party/libtasn1-port/config.h — libtasn1's build configuration,
 * decided here instead of by ./configure.
 *
 * The same arrangement FreeType, libjpeg-turbo, libepoxy, libgpg-error
 * and libgcrypt are under, and for the reason spelled out beside each of
 * them: configure's probes work by compiling, linking and *running* a
 * program, this target has nowhere to run one, and a configure pass on
 * the build machine would faithfully describe macOS on arm64.
 *
 * ---- this one is small, and that is a fact about libtasn1 ----
 *
 * The other ports needed a long file because they have a long list of
 * things to decide: which assembly, which allocator, which entropy
 * source. libtasn1 has almost none. It is a DER encoder and decoder that
 * calls malloc, free, memcpy and snprintf, and the whole of what its
 * sources read out of config.h is six macros:
 *
 *     VERSION                     what asn1_check_version answers
 *     SIZEOF_UNSIGNED_LONG_INT    used 21 times, in the tag arithmetic
 *     SIZEOF_UNSIGNED_INT         used twice, the same way
 *     HAVE_SYS_TYPES_H            whether int.h includes it
 *     HAVE_CONFIG_H               set on the command line, not here
 *     _GL_INLINE et al            the gnulib block copied in below
 *
 * Everything else in upstream's 1260-line config.h.in is for the parts
 * of the distribution this build does not compile: the three
 * command-line tools in src/ (asn1Parser, asn1Coding, asn1Decoding) and
 * the test suite, which between them pull in getopt, read-file, fstat
 * and a dozen more gnulib modules. lib/ pulls in three, and only three:
 * c-ctype, strverscmp and the intprops/minmax header-only pair. The
 * absent macros are absent because nothing asks for them, not because
 * they were skipped.
 *
 * ---- the gnulib inline block is copied verbatim, on purpose ----
 *
 * gl/c-ctype.h opens with
 *
 *     #ifndef _GL_INLINE_HEADER_BEGIN
 *      #error "Please include config.h first."
 *     #endif
 *
 * which is gnulib telling you that these five macros are part of the
 * contract between its headers and the config.h that configure writes.
 * They are not a policy anyone gets to choose: they decide whether
 * c-ctype's functions are `inline`, `extern inline` or `static`, and the
 * matching translation unit — c_ctype.c, which is in the object list
 * below for exactly this reason — is what emits the out-of-line copies
 * under the first of those three. Reduced to a hand-picked pair of
 * #defines, an -O0 build or a taken address would find them missing.
 *
 * So the block is reproduced below exactly as it appears at
 * config.h.in:471-530, with no edits. On this compiler
 * __GNUC_STDC_INLINE__ is defined, so it selects C99 semantics:
 * _GL_INLINE is `inline` and _GL_EXTERN_INLINE is `extern inline`.
 *
 * ---- and the one function this C library does not have ----
 *
 * strverscmp, which version.c calls and which gnulib supplies in
 * gl/strverscmp.c. It is declared at the bottom of this file rather than
 * in libc/include/string.h, and the distinction is deliberate: the
 * declaration and the definition should live on the same side of the
 * boundary. Putting the prototype in the C library's header while the
 * only definition sits inside libtasn1.a would mean any *other* program
 * could compile a call to it and fail at link — and "the headers
 * describe the archive beside them" is a rule this tree enforces
 * everywhere else (see the note about jconfigint.h in the sysroot
 * staging rule). Upstream reaches the same arrangement by a longer
 * road: gnulib substitutes its own <string.h> over the system's and
 * declares it there.
 */

#ifndef VEXTRO_LIBTASN1_CONFIG_H
#define VEXTRO_LIBTASN1_CONFIG_H

/* ---- who we are ---- */

#define PACKAGE          "libtasn1"
#define PACKAGE_NAME     "GNU Libtasn1"
#define PACKAGE_TARNAME  "libtasn1"
#define PACKAGE_VERSION  "4.19.0"
#define PACKAGE_STRING   "GNU Libtasn1 4.19.0"
#define PACKAGE_BUGREPORT "help-libtasn1@gnu.org"
#define PACKAGE_URL      "https://www.gnu.org/software/libtasn1/"

/*
 * VERSION is the one asn1_check_version compares against, through
 * ASN1_VERSION in the public header rather than through this macro — but
 * errors.c reads VERSION directly for its libtasn1_strerror table's
 * banner, so the two must agree. They come from the same tarball.
 */
#define VERSION          "4.19.0"

/* ---- the machine ---- */

/*
 * x86-64 System V. These two are not decoration: parser_aux.c and
 * decoding.c use SIZEOF_UNSIGNED_LONG_INT twenty-one times to decide how
 * many bytes of a DER tag can be accumulated into an `unsigned long`
 * before it overflows, and a wrong answer here is not a compile error —
 * it is a decoder that rejects valid tags, or accepts ones it should
 * not.
 */
#define SIZEOF_UNSIGNED_INT        4
#define SIZEOF_UNSIGNED_LONG_INT   8

/* Little-endian, so WORDS_BIGENDIAN stays undefined. */
/* #undef WORDS_BIGENDIAN */

/* ---- headers this C library has ---- */

#define STDC_HEADERS      1
#define HAVE_STDIO_H      1
#define HAVE_STDLIB_H     1
#define HAVE_STRING_H     1
#define HAVE_STRINGS_H    1
#define HAVE_INTTYPES_H   1
#define HAVE_STDINT_H     1
#define HAVE_LIMITS_H     1
#define HAVE_UNISTD_H     1
#define HAVE_SYS_TYPES_H  1
#define HAVE_SYS_STAT_H   1
#define HAVE_WCHAR_H      1
#define HAVE___INLINE     1

/*
 * Not here, and each absence is load-bearing somewhere:
 *
 *   HAVE_FEATURES_H       gl/libc-config.h asks, and takes its
 *                         non-glibc path when the answer is no. That
 *                         path is the one that works: the glibc path
 *                         reaches for __libc_lock_* and _dl_*.
 *   HAVE_MINMAX_IN_LIMITS_H, HAVE_MINMAX_IN_SYS_PARAM_H
 *                         neither of our headers defines MIN/MAX, so
 *                         gl/minmax.h defines them itself, which is
 *                         what decoding.c wants.
 *   HAVE_STRVERSCMP       the whole reason gl/strverscmp.c is compiled.
 *   HAVE_VISIBILITY       deliberately off; see the note by ASN1_API
 *                         below.
 *   HAVE_ALLOCA_H         there is no alloca.h here. ASN1.c's bison
 *                         skeleton checks __GNUC__ first and settles on
 *                         __builtin_alloca before it ever looks, so the
 *                         parser stack is still alloca'd; nothing falls
 *                         back to malloc.
 */
/* #undef HAVE_FEATURES_H */
/* #undef HAVE_MINMAX_IN_LIMITS_H */
/* #undef HAVE_MINMAX_IN_SYS_PARAM_H */
/* #undef HAVE_STRVERSCMP */
/* #undef HAVE_VISIBILITY */
/* #undef HAVE_ALLOCA_H */

/*
 * ---- what ASN1_API comes out as ----
 *
 * includes/libtasn1.h picks between four spellings. With HAVE_VISIBILITY
 * off and _MSC_VER absent it lands on the empty one, which is the right
 * answer for a static archive: __attribute__((visibility("default"))) is
 * meaningful in a shared object and inert in a .a, and upstream's
 * libtasn1.map version script — which is the other half of that
 * mechanism — is not used here either.
 *
 * -DASN1_BUILDING is still passed on the command line, matching
 * upstream's AM_CPPFLAGS, because it is what the header keys on to know
 * it is being read by the library rather than by a consumer.
 */

/*
 * ---- gnulib's inline machinery ----
 *
 * Copied verbatim from upstream's config.h.in, lines 471-530. Do not
 * edit; see the long note at the top of this file for why it is whole.
 */

#if (((defined __APPLE__ && defined __MACH__) \
      || defined __DragonFly__ || defined __FreeBSD__) \
     && (defined HAVE___HEADER_INLINE \
         ? (defined __cplusplus && defined __GNUC_STDC_INLINE__ \
            && ! defined __clang__) \
         : ((! defined _DONT_USE_CTYPE_INLINE_ \
             && (defined __GNUC__ || defined __cplusplus)) \
            || (defined _FORTIFY_SOURCE && 0 < _FORTIFY_SOURCE \
                && defined __GNUC__ && ! defined __cplusplus))))
# define _GL_EXTERN_INLINE_STDHEADER_BUG
#endif
#if ((__GNUC__ \
      ? defined __GNUC_STDC_INLINE__ && __GNUC_STDC_INLINE__ \
      : (199901L <= __STDC_VERSION__ \
         && !defined __HP_cc \
         && !defined __PGI \
         && !(defined __SUNPRO_C && __STDC__))) \
     && !defined _GL_EXTERN_INLINE_STDHEADER_BUG)
# define _GL_INLINE inline
# define _GL_EXTERN_INLINE extern inline
# define _GL_EXTERN_INLINE_IN_USE
#elif (2 < __GNUC__ + (7 <= __GNUC_MINOR__) && !defined __STRICT_ANSI__ \
       && !defined _GL_EXTERN_INLINE_STDHEADER_BUG)
# if defined __GNUC_GNU_INLINE__ && __GNUC_GNU_INLINE__
   /* __gnu_inline__ suppresses a GCC 4.2 diagnostic.  */
#  define _GL_INLINE extern inline __attribute__ ((__gnu_inline__))
# else
#  define _GL_INLINE extern inline
# endif
# define _GL_EXTERN_INLINE extern
# define _GL_EXTERN_INLINE_IN_USE
#else
# define _GL_INLINE _GL_UNUSED static
# define _GL_EXTERN_INLINE _GL_UNUSED static
#endif

/* In GCC 4.6 (inclusive) to 5.1 (exclusive),
   suppress bogus "no previous prototype for 'FOO'"
   and "no previous declaration for 'FOO'" diagnostics,
   when FOO is an inline function in the header; see
   <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=54113> and
   <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=63877>.  */
#if __GNUC__ == 4 && 6 <= __GNUC_MINOR__
# if defined __GNUC_STDC_INLINE__ && __GNUC_STDC_INLINE__
#  define _GL_INLINE_HEADER_CONST_PRAGMA
# else
#  define _GL_INLINE_HEADER_CONST_PRAGMA \
     _Pragma ("GCC diagnostic ignored \"-Wsuggest-attribute=const\"")
# endif
# define _GL_INLINE_HEADER_BEGIN \
    _Pragma ("GCC diagnostic push") \
    _Pragma ("GCC diagnostic ignored \"-Wmissing-prototypes\"") \
    _Pragma ("GCC diagnostic ignored \"-Wmissing-declarations\"") \
    _GL_INLINE_HEADER_CONST_PRAGMA
# define _GL_INLINE_HEADER_END \
    _Pragma ("GCC diagnostic pop")
#else
# define _GL_INLINE_HEADER_BEGIN
# define _GL_INLINE_HEADER_END
#endif

/*
 * _GL_UNUSED is referenced by the `#else` arm above — the one this
 * compiler does not take — and gnulib normally emits it from the same
 * m4 macro. Defined here so the block stays self-contained rather than
 * only happening to work.
 */
#ifndef _GL_UNUSED
# define _GL_UNUSED __attribute__ ((__unused__))
#endif

/*
 * ---- and the one attribute lib/ uses ----
 *
 * parser_aux.c declares _asn1_hash_name _GL_ATTRIBUTE_PURE, and that is
 * the only _GL_ATTRIBUTE_* macro anything under lib/ mentions — the
 * other thirty in upstream's config.h.in belong to modules this build
 * does not compile.
 *
 * _GL_HAS_ATTRIBUTE is upstream's, from config.h.in:830-836. Its `#else`
 * arm — a table of _GL_GNUC_PREREQ version tests, fifty lines, for
 * compilers with no __has_attribute — is not reproduced, and the
 * condition is left in place so that such a compiler stops here with
 * something readable instead of quietly building with no attributes at
 * all. Every GCC since 5 has __has_attribute; this one is 16.
 */
#if defined __has_attribute
# define _GL_HAS_ATTRIBUTE(attr) __has_attribute (__##attr##__)
#else
# error "no __has_attribute here; config.h.in:837 has upstream's fallback table"
#endif

/* _GL_ATTRIBUTE_PURE declares that It is OK for a compiler to omit duplicate
   calls to the function with the same arguments if observable state is not
   changed between calls.
   This attribute is safe for a function that does not affect
   observable state, and always returns exactly once.
   (This attribute is looser than _GL_ATTRIBUTE_CONST.)  */
/* Applies to: functions.  */
#if _GL_HAS_ATTRIBUTE (pure)
# define _GL_ATTRIBUTE_PURE __attribute__ ((__pure__))
#else
# define _GL_ATTRIBUTE_PURE
#endif

/*
 * gl/c-ctype.h and gl/libc-config.h check this to be sure they were not
 * included before config.h. Nothing reads its value.
 */
#define _GL_CONFIG_H_INCLUDED 1

/*
 * ---- strverscmp ----
 *
 * Declared here, defined in gl/strverscmp.c, which this build compiles
 * into libtasn1.a. version.c includes <config.h> before <string.h>, so
 * this prototype is in scope by the time it calls the function.
 *
 * The declaration cannot be left out: this compiler makes an implicit
 * declaration a hard error, and -w does not soften it.
 */
#ifdef __cplusplus
extern "C" {
#endif
int strverscmp (const char *__s1, const char *__s2);
#ifdef __cplusplus
}
#endif

#endif /* VEXTRO_LIBTASN1_CONFIG_H */
