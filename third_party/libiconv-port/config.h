/*
 * third_party/libiconv-port/config.h — GNU libiconv's build
 * configuration, decided here instead of by ./configure.
 *
 * The eighteenth port, and the second one in a row that exists because
 * of GLib rather than because of WebKit. It is also the first that
 * closes a gap this tree has been recording for three sessions:
 * `third_party/libxml2-port/config.h` says "there is no iconv in this C
 * library" as a plain absence, and GLib 2.74's meson.build:2060 makes
 * `dependency('iconv')` required outright. Both were pointing at the
 * same missing thing.
 *
 * ================================================================
 * a library that is three files and 274 headers
 * ================================================================
 *
 * The whole of libiconv's converter set is `#include`d into one
 * translation unit: `lib/iconv.c` pulls in `converters.h`, which pulls
 * in every encoding in turn, and the alias, flag and transliteration
 * tables come from `aliases.h`, `flags.h` and `translit.h` — all four
 * of which the release tarball ships pre-generated. So there is nothing
 * to run here and nothing to curate: three objects, and upstream's own
 * `lib/Makefile.in:59` names them.
 *
 * ================================================================
 * the platform questions, and there are only two that matter
 * ================================================================
 *
 *   HAVE_LANGINFO_CODESET is **not** defined, and that is the only
 *   answer that changes behaviour. `libcharset/lib/localcharset.c` asks
 *   the operating system what the current locale's character encoding
 *   is, and its first choice is `nl_langinfo(CODESET)`. There is no
 *   <langinfo.h> in this C library — it is one of the five gaps
 *   WebKit's own configure named and this tree has been carrying on its
 *   list ever since. Without it, localcharset takes upstream's fallback
 *   path: LC_ALL, then LC_CTYPE, then LANG out of the environment, then
 *   `setlocale(LC_CTYPE, NULL)`.
 *
 *   That path works here and answers "ASCII", because a ring-3 process
 *   on this system starts with an empty environment (`libc/process.c`
 *   gives `environ` a single NULL) and `setlocale` reports the C locale.
 *   ASCII is the right answer for a machine with no locale support, and
 *   it is only consulted by `iconv_open("", ...)` — the form that asks
 *   for "whatever the user's encoding is". Every consumer here names its
 *   encodings explicitly: libxml2 asks for the document's, and GLib's
 *   g_convert takes both names from its caller.
 *
 *   ICONV_CONST is empty. It exists for systems whose *native* iconv
 *   declares `iconv(iconv_t, const char**, ...)`; this is GNU
 *   libiconv's own header declaring GNU libiconv's own function, so the
 *   argument is a plain `char**` and anything else would be a
 *   prototype that does not match the definition.
 *
 * ================================================================
 * what is not built
 * ================================================================
 *
 *   The `iconv` program in src/. It is a command-line tool that reads
 *   files and writes to stdout, and nothing on this machine invokes it.
 *
 *   `--enable-extra-encodings`, which adds EUC-JISX0213, Shift_JISX0213
 *   and the other four. Upstream's default is off. Turning it on is one
 *   token in the Makefile if a consumer ever names one of them — and
 *   apps/iconvtest.c checks that a request for an encoding this build
 *   does not carry is *refused* rather than silently mapped to
 *   something near it, because a converter that quietly substituted a
 *   different encoding would corrupt text rather than fail.
 */

#ifndef VEXTRO_LIBICONV_CONFIG_H
#define VEXTRO_LIBICONV_CONFIG_H

#define PACKAGE           "libiconv"
#define PACKAGE_NAME      "GNU libiconv"
#define PACKAGE_TARNAME   "libiconv"
#define PACKAGE_VERSION   "1.18"
#define PACKAGE_STRING    "GNU libiconv 1.18"
#define PACKAGE_BUGREPORT "bug-gnu-libiconv@gnu.org"
#define PACKAGE_URL       "https://www.gnu.org/software/libiconv/"
#define VERSION           "1.18"

/*
 * The second argument of iconv(). Empty, because this header declares
 * this library's own function; see the note above.
 */
#define ICONV_CONST

/* ---- headers this C library has ---- */
#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_STRINGS_H   1
#define HAVE_STDINT_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_LIMITS_H    1
#define HAVE_WCHAR_H     1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H  1
#define HAVE_UNISTD_H    1
#define HAVE_LOCALE_H    1
#define STDC_HEADERS     1

/* mbstate_t is in <wchar.h> here, which is what iconv.h's
 * iconv_allocation_t and the mbstate-carrying converters need. */
#define HAVE_MBSTATE_T   1
#define HAVE_WCHAR_T     1

/* setlocale and getenv, which is the fallback path localcharset takes
 * in the absence of nl_langinfo. Both are real here. */
#define HAVE_SETLOCALE   1
#define HAVE_GETENV      1

/* x86-64 is little-endian. Left undefined rather than defined to 0:
 * libiconv tests it with #ifdef in several converters, so
 * `#define WORDS_BIGENDIAN 0` would select every big-endian path. */
/* #undef WORDS_BIGENDIAN */

/*
 * ---- not defined, and each for its own reason ----
 *
 *   HAVE_LANGINFO_CODESET
 *       there is no <langinfo.h> in this C library. See the long note
 *       above: this is the one answer that changes what the library
 *       does, and the fallback it selects is the right behaviour for a
 *       machine with no locales rather than a degradation.
 *
 *   ENABLE_EXTRA, USE_EXTRA
 *       --enable-extra-encodings, off by default upstream.
 *
 *   USE_DOS, USE_AIX, USE_OSF1, USE_ZOS
 *       the four platform-specific converter sets. This is none of
 *       them.
 *
 *   HAVE_WORKING_O_NOFOLLOW, HAVE_DECL_GETC_UNLOCKED, and the rest of
 *   the gnulib probe set
 *       read by srclib/ and src/iconv.c, neither of which is compiled.
 *       The library half of libiconv touches the operating system in
 *       exactly one place — localcharset's getenv — and nowhere else.
 *
 *   ENABLE_RELOCATABLE, INSTALLPREFIX, INSTALLDIR
 *       libiconv's relocation support, for a library that has to find
 *       its own installation directory at run time. There is no
 *       installation directory here; the archive is linked in.
 *
 *   HAVE_VISIBILITY
 *       nothing is hidden in a static archive on a system with no
 *       shared objects.
 */

#endif /* VEXTRO_LIBICONV_CONFIG_H */
