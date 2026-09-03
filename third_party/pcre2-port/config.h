/*
 * third_party/pcre2-port/config.h — PCRE2's build configuration,
 * decided here instead of by ./configure.
 *
 * ---- why this is hand-written and libpng's was not ----
 *
 * PCRE2 ships `src/config.h.generic` for builds without autotools, and
 * the obvious move would be to copy it the way this tree copies
 * `scripts/pnglibconf.h.prebuilt`. That is the wrong call here, and the
 * difference is worth stating because the two files look alike.
 *
 * libpng's prebuilt is a *configuration*: upstream's considered default,
 * every option answered the way a normal build would answer it. PCRE2's
 * generic file is a *floor* — every HAVE_ is `#undef`, every SUPPORT_ is
 * off, because it has to compile on a machine nobody has described.
 * Copying it would tell PCRE2 this C library has no <string.h>, no
 * <limits.h> and no Unicode support, all three of which are false, and
 * the last of which GLib would then be entitled to complain about:
 * GRegex compiles patterns with PCRE2_UTF and PCRE2_UCP.
 *
 * So this file answers what the sources actually ask, and the list is
 * short because PCRE2 asks very little. It is a library that allocates
 * through a caller-supplied context and touches the operating system
 * nowhere at all.
 *
 * ================================================================
 * what is on
 * ================================================================
 *
 *   SUPPORT_UNICODE   the reason the port exists in this shape. GLib's
 *                     GRegex passes PCRE2_UTF|PCRE2_UCP for every
 *                     pattern it compiles, and without this those
 *                     options are refused at compile time with
 *                     PCRE2_ERROR_UNICODE_NOT_SUPPORTED. It costs
 *                     `pcre2_ucd.c`, which is a table.
 *
 *   SUPPORT_PCRE2_8   the 8-bit library, which is the one GLib links.
 *                     16 and 32 are separate archives built from the
 *                     same sources with a different
 *                     PCRE2_CODE_UNIT_WIDTH, and nothing here asks for
 *                     them, so they are not built — the same reasoning
 *                     that leaves libwebpmux.a out of the WebP port.
 *
 *   PCRE2_STATIC      there are no shared libraries on this system, and
 *                     without this pcre2.h decorates every prototype
 *                     with a dllimport-flavoured visibility attribute.
 *
 * ================================================================
 * what is off, and the one that is a real gap
 * ================================================================
 *
 *   SUPPORT_JIT       PCRE2's just-in-time compiler writes x86-64
 *                     machine code into a page and jumps to it, and on
 *                     this system that is not a build option — it is
 *                     refused by the kernel.
 *
 *                     `src/desktop.h:2449` rejects any mmap or mprotect
 *                     asking for PROT_WRITE and PROT_EXEC together, by
 *                     name, and `libc/include/sys/mman.h` states the
 *                     policy: every page of every program is writable or
 *                     executable and never both, so there is no sequence
 *                     of calls from ring 3 that arrives at a page which
 *                     is both. That note already says what follows —
 *                     "this is what makes a just-in-time compiler
 *                     impossible here rather than merely discouraged" —
 *                     and names WebKit's JIT tiers as the first
 *                     casualty. PCRE2's is the second.
 *
 *                     Nothing needs it. GLib calls pcre2_jit_compile()
 *                     on every pattern compiled with G_REGEX_OPTIMIZE
 *                     and has an explicit
 *                     `case PCRE2_ERROR_JIT_BADOPTION:` at
 *                     gregex.c:936 that logs and falls back to the
 *                     interpreter. Section 2 of apps/pcre2test.c takes
 *                     that branch deliberately, so it is a tested path
 *                     rather than a surprise.
 *
 *   SUPPORT_VALGRIND, SUPPORT_DIFF_FUZZ
 *                     developer tooling for a machine with Valgrind.
 *
 *   EBCDIC and friends
 *                     this is an ASCII machine.
 *
 *   HAVE_BZLIB_H, HAVE_ZLIB_H
 *                     read only by pcre2test, which is not built. zlib
 *                     *is* ported and staged, so this one could be
 *                     answered truthfully; it is left out because the
 *                     thing that reads it is not compiled, and a define
 *                     describing a program nobody builds is a claim with
 *                     nothing behind it. Same reasoning as WEBP_HAVE_PNG
 *                     in the libwebp port.
 */

#ifndef VEXTRO_PCRE2_CONFIG_H
#define VEXTRO_PCRE2_CONFIG_H

#define PACKAGE           "pcre2"
#define PACKAGE_NAME      "PCRE2"
#define PACKAGE_TARNAME   "pcre2"
#define PACKAGE_VERSION   "10.48"
#define PACKAGE_STRING    "PCRE2 10.48"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL       ""
#define VERSION           "10.48"

/* ---- the library that gets built ---- */
#define SUPPORT_PCRE2_8   1
#define SUPPORT_UNICODE   1
#define PCRE2_STATIC      1

/*
 * The annotation put in front of every exported symbol, and it has to
 * be *defined and empty* rather than left out.
 *
 * `pcre2_internal.h:158` reads
 *
 *     #define PCRE2_EXP_DECL  extern PCRE2_EXPORT
 *
 * unconditionally on every non-Windows target, so an undefined
 * PCRE2_EXPORT is not a missing optimisation — it is a syntax error in
 * the first declaration the compiler meets, which is what the first
 * attempt at this port produced. Upstream sets it in config.h from
 * `m4/pcre2_visibility.m4:79`: the annotation is
 * `__attribute__((visibility("default")))` when the build hides symbols
 * by default, and *empty* when it does not. Nothing here is hidden —
 * there are no shared objects on this system — so empty is the true
 * answer as well as the working one.
 */
#define PCRE2_EXPORT

/* ---- headers and builtins this toolchain has ----
 *
 * GCC 16 has both of these and PCRE2 uses them where it can:
 * __builtin_mul_overflow replaces a hand-rolled overflow check in the
 * compiler's size arithmetic, and __attribute__((uninitialized))
 * suppresses the automatic-variable initialisation that -ftrivial-auto-
 * var-init would otherwise apply to a large match frame. */
#define HAVE_BUILTIN_MUL_OVERFLOW    1
#define HAVE_ATTRIBUTE_UNINITIALIZED 1
#define HAVE_BUILTIN_UNREACHABLE     1
#define HAVE_BUILTIN_ASSUME          1

#define HAVE_ASSERT_H     1
#define HAVE_LIMITS_H     1
#define HAVE_STDINT_H     1
#define HAVE_INTTYPES_H   1
#define HAVE_STDIO_H      1
#define HAVE_STDLIB_H     1
#define HAVE_STRING_H     1
#define HAVE_STRINGS_H    1
#define HAVE_SYS_TYPES_H  1
#define HAVE_SYS_STAT_H   1
#define HAVE_UNISTD_H     1
#define HAVE_WCHAR_H      1
#define STDC_HEADERS      1

/*
 * ---- the tunables, at upstream's defaults ----
 *
 * Every one of these is a limit rather than a feature, and they are
 * copied from src/config.h.generic unchanged. They are written out
 * rather than left to pcre2_internal.h's fallbacks so that a future
 * change here is a visible edit rather than a silent drift from
 * upstream.
 *
 * HEAP_LIMIT and the two MATCH_LIMITs are the ones with a security
 * character: they are what stops a pathological pattern from running
 * for ever or eating the heap, and GLib does not override them.
 */
#define LINK_SIZE          2
#define HEAP_LIMIT         20000000
#define MATCH_LIMIT        10000000
#define MATCH_LIMIT_DEPTH  MATCH_LIMIT
#define MAX_NAME_COUNT     10000
#define MAX_NAME_SIZE      128
#define MAX_VARLOOKBEHIND  255
#define PARENS_NEST_LIMIT  250
#define NEWLINE_DEFAULT    2      /* LF, which is what \n is here */

/*
 * ---- not defined ----
 *
 *   SUPPORT_JIT              see the long note above; this is a
 *                            question about the kernel, not about PCRE2
 *   SUPPORT_PCRE2_16/32      separate archives nothing asks for
 *   SUPPORT_VALGRIND         no Valgrind here
 *   SUPPORT_DIFF_FUZZ        pcre2test's fuzzing support
 *   EBCDIC, EBCDIC_NL25, EBCDIC_IGNORING_COMPILER
 *                            an ASCII machine
 *   BSR_ANYCRLF              upstream's default is that \R matches any
 *                            Unicode newline; GLib relies on that
 *   NEVER_BACKSLASH_C        \C is disabled by GRegex through an option
 *                            rather than at build time
 *   HAVE_DIRENT_H, HAVE_SYS_WAIT_H, HAVE_REALPATH, HAVE_SECURE_GETENV,
 *   HAVE_SETRLIMIT, HAVE_MKOSTEMP, HAVE_MEMFD_CREATE, HAVE_PTHREAD,
 *   HAVE_ZLIB_H, HAVE_BZLIB_H, HAVE_READLINE_*, HAVE_EDITLINE_*
 *                            all read by pcre2test, pcre2grep or
 *                            pcre2posix, none of which is built. PCRE2's
 *                            library half does not call the operating
 *                            system at all -- it allocates through a
 *                            caller-supplied context and returns.
 *   HAVE_VISIBILITY          PCRE2_STATIC covers the same ground and
 *                            there are no shared objects to hide from
 *   HAVE_WINDOWS_H           Windows
 */

#endif /* VEXTRO_PCRE2_CONFIG_H */
