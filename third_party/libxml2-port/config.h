/*
 * third_party/libxml2-port/config.h — libxml2's build configuration,
 * decided here instead of by ./configure.
 *
 * The seventh port configured by hand, and the one with the *smallest*
 * config.h relative to its size. libxml2 is 47 source files and a
 * quarter of a million lines, and almost none of what selects its
 * behaviour lives here: the feature set is in
 * `include/libxml/xmlversion.h`, which the Makefile generates from
 * upstream's `.in` by substituting 37 tokens. This file answers only
 * what the C files ask the *platform*.
 *
 * That split is worth knowing before changing anything. Turning XPath
 * off is an edit to the Makefile's WITH_XPATH; discovering that this
 * system has no iconv is an edit here.
 *
 * ================================================================
 * what is switched off, and which of those are absences
 * ================================================================
 *
 * Three of them are things this system does not have:
 *
 *   ZLIB, LZMA   `xzlib.c` decompresses gzipped and xz'd documents.
 *                Neither library is ported yet — zlib is two entries
 *                further down WebKit's own dependency list — so both
 *                are off and xzlib.c compiles to an empty translation
 *                unit. It stays in the object list because it is in
 *                upstream's unconditional LIBXML2_SRCS; a build that
 *                dropped it would differ from upstream's for no reason.
 *
 *   MODULES      `xmlmodule.c` loads shared objects with dlopen. There
 *                is no dynamic linker here — `libc/dlfcn.c` is a name
 *                *table*, which is exactly enough for libepoxy and
 *                exactly not enough for this.
 *
 *   ICONV        there is no iconv in this C library.
 *
 * ---- and ICU, which is present and deliberately not wired ----
 *
 * This is the one to read twice, because the obvious reading of it is
 * wrong. ICU 74.2 *is* ported here, it is staged in the same sysroot,
 * and `apps/icutest.cpp` runs 54 checks against it on every boot. So
 * LIBXML2_WITH_ICU could be turned on, and it is off on purpose.
 *
 * What it would buy is character-set conversion for encodings outside
 * the built-in set. What the built-in set already covers is UTF-8,
 * UTF-16 in both byte orders, ASCII — and, because WITH_ISO8859X is on,
 * the whole ISO-8859-1..16 family. XML's own default is UTF-8 and
 * WebKit does not hand libxml2 anything else: `XMLDocumentParser`
 * sniffs and converts the document *before* the parser sees it, so the
 * encoding layer being asked for anything exotic would mean WebKit had
 * already failed to do its job.
 *
 * Against that, wiring it on adds a link edge from libxml2 to libicuuc
 * that nothing in WebKit exercises, and one more archive that has to be
 * on the command line in the right order for a test binary to link. If
 * a real encoding gap ever shows up, this is a one-line change with the
 * library already sitting there.
 *
 * ---- FTP and HTTP, which are off for a third reason again ----
 *
 * Not absence: this system has sockets, DNS and TLS, and
 * `apps/fdtest.c` proves it. They are off because **WebKit prevents
 * libxml2 from fetching anything anyway**. XMLDocumentParserLibxml2.cpp
 * installs its own loader with `xmlSetExternalEntityLoader` precisely
 * so that an external entity in a document cannot become a network
 * request — that is the XXE defence, and it is the whole point.
 * `nanoftp.c` and `nanohttp.c` would be code that can only be reached
 * by defeating it. Upstream has since deleted both.
 *
 * ================================================================
 * the platform answers
 * ================================================================
 */

#ifndef VEXTRO_LIBXML2_CONFIG_H
#define VEXTRO_LIBXML2_CONFIG_H

#define VERSION "2.12.6"

/* ---- headers this C library has ---- */
#define HAVE_UNISTD_H       1
#define HAVE_FCNTL_H        1
#define HAVE_SYS_STAT_H     1
#define HAVE_SYS_TIME_H     1
#define HAVE_SYS_SELECT_H   1
#define HAVE_SYS_SOCKET_H   1
#define HAVE_NETINET_IN_H   1
#define HAVE_ARPA_INET_H    1
#define HAVE_NETDB_H        1
#define HAVE_POLL_H         1

/* ---- functions, checked against `nm build/libvextro.a` ---- */
#define HAVE_STAT           1
#define HAVE_ISASCII        1
#define HAVE_GETTIMEOFDAY   1
#define HAVE_PTHREAD_H      1

/*
 * va_copy, and the reason VA_LIST_IS_ARRAY does not appear below.
 *
 * xmlreader.c and xmlwriter.c pick a VA_COPY four ways: va_copy,
 * __va_copy, plain assignment, or a memcpy of sizeof(va_list). Only the
 * last two consult VA_LIST_IS_ARRAY, and only when neither of the first
 * two is available. This is C23 on GCC 16, so va_copy exists, the
 * question is never asked, and answering it would be answering a
 * question about a branch that is not compiled.
 *
 * Worth spelling out because the wrong answer there is invisible: on
 * x86-64 System V `va_list` *is* an array type, so the plain-assignment
 * branch would alias two va_lists onto one register-save area and
 * corrupt the second walk of an argument list — in error formatting,
 * which is the path a test only reaches when something else has already
 * gone wrong.
 */
#define HAVE_VA_COPY        1

/*
 * ---- not defined, and each for its own reason ----
 *
 *   HAVE_DLFCN_H, HAVE_DLOPEN, HAVE_DL_H, HAVE_SHLLOAD
 *       xmlmodule.c's four ways to load a shared object. There is no
 *       dynamic linker; see the note above.
 *
 *   HAVE_LIBREADLINE, HAVE_LIBHISTORY
 *       for xmllint's interactive shell, which is not built.
 *
 *   HAVE_FTIME, HAVE_SYS_TIMEB_H
 *       the fallback clock for platforms without gettimeofday, which
 *       this one has.
 *
 *   HAVE_MMAP, HAVE_SYS_MMAN_H
 *       read by xmllint, not by the library. Left off anyway, and
 *       consistently with libxkbcommon: this system's mmap is
 *       anonymous-only.
 *
 *   SUPPORT_IP6
 *       nanohttp's IPv6 path, in code that is not compiled.
 *
 *   XML_THREAD_LOCAL
 *       globals.c uses it, if defined, to put the parser's global state
 *       in thread-local storage; undefined, it takes the pthread_key
 *       path instead. The pthread path is chosen deliberately. TLS
 *       works here — every application is built -ftls-model=initial-exec
 *       and apps/threadtest.c checks it — but libxml2 initialises its
 *       globals from `xmlInitParser`, which a program may call before
 *       it has made a thread, and pthread_key_create /
 *       pthread_getspecific are the pair this C library has had longest
 *       and tests hardest.
 *
 *   HAVE_ATTRIBUTE_DESTRUCTOR, ATTRIBUTE_DESTRUCTOR
 *       threads.c would use these to run `xmlCleanupParser()` from a
 *       `__attribute__((destructor))` at exit. This is a *choice*, not
 *       a limitation — libc/exit.c really does walk `.fini_array` in
 *       reverse, so the destructor would run. It is off because all it
 *       does is free libxml2's globals a moment before the process's
 *       address space is destroyed, and because upstream's own guard on
 *       that block includes `!defined(LIBXML_STATIC)`: they do not
 *       expect it in an archive, which is what this is.
 *
 *   VA_LIST_IS_ARRAY
 *       unreachable; see the note by HAVE_VA_COPY.
 *
 *   XML_SOCKLEN_T
 *       nanohttp only.
 */

#endif /* VEXTRO_LIBXML2_CONFIG_H */
