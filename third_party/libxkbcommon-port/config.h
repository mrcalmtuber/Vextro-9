/*
 * third_party/libxkbcommon-port/config.h — libxkbcommon's build
 * configuration, decided here instead of by meson.
 *
 * The sixth port to be configured by hand, after FreeType, libjpeg-turbo,
 * libepoxy, libgpg-error/libgcrypt and libtasn1, and the first whose
 * build system is **meson** rather than autotools. That changes nothing
 * about the arrangement and one thing about the reasoning: meson's
 * `cc.has_function()` probes compile and link a program, and a cross
 * build has nowhere to run one either — but meson at least *knows* it is
 * cross-compiling, whereas the answers below still have to be written by
 * someone who knows this system. So they are.
 *
 * Eleven HAVE_ macros, five default rule names, three paths and a
 * version string. That is the whole of what `src/` reads out of a
 * config.h — the rest of meson's configuration data belongs to the
 * `xkbcli` tools and the test harness, neither of which is built here.
 *
 * ================================================================
 * HAVE_MMAP is off, and that is the decision this file exists for
 * ================================================================
 *
 * libxkbcommon reads every keymap, rules file and Compose table through
 * one function, `map_file()` in src/utils.c — and upstream ships **two**
 * implementations of it, chosen by this macro:
 *
 *   with HAVE_MMAP     fstat the descriptor, then
 *                      mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0)
 *   without            fseek to the end, ftell, malloc, fread
 *
 * This system's mmap is **anonymous only**. `SYS_MMAP` takes no
 * descriptor and there is no file-backed mapping behind it — a gap named
 * as one of the three still open in third_party/wpe-config/README.md.
 * With HAVE_MMAP set, every include file libxkbcommon opened would fail
 * at the first map and the whole keymap compile would report nothing
 * more specific than "failed to compile keymap".
 *
 * So it is left undefined, and the fallback that runs is **upstream's
 * own**, compiled by every platform without mmap and exercised by their
 * CI. It is not a shim written here and it is not a degraded mode: for
 * files this size — the largest is the 948-line evdev rules file — the
 * difference between a mapping and a read is a memcpy nobody can
 * measure. What it costs is one buffer's worth of memory per file while
 * that file is being parsed, and the buffer is freed by `unmap_file()`
 * on the way out, which is upstream's other half of the pair.
 *
 * This is the note to read first if file-backed mmap is ever built: the
 * only thing that would change is this macro, and the honest way to
 * decide would be to turn it on and see whether the keymap still
 * compiles.
 *
 * ================================================================
 * where the keymap data comes from
 * ================================================================
 *
 * DFLT_XKB_CONFIG_ROOT is `/etc/xkb` — a path on this system's own NTFS
 * volume, beside `/etc/ca-bundle.crt`, and not the `/usr/share/X11/xkb`
 * that a Linux distribution would use. `make disk.img` stages
 * xkeyboard-config 2.41's five directories there: rules, keycodes,
 * types, compat and symbols.
 *
 * That data is not optional decoration. WebKit's only keymap call is
 *
 *     xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
 *     xkb_keymap_new_from_names(context, &names, ...)
 *
 * (WPEKeymapXKB.cpp:180), and RMLVO names are resolved by *reading
 * files*: rules/evdev maps the five names onto a keycodes file, a types
 * file, a compat file and a symbols expression, and each of those is
 * then found by name under this root. Without the data the call returns
 * NULL and a browser has no keyboard. A library that links and cannot
 * answer the one question its caller asks is not a port.
 *
 * The *extra* path is a second directory, searched before that one, and
 * it deliberately does not exist. Upstream's pair is
 * `$sysconfdir/xkb` ahead of `$datadir/X11/xkb`: site-local overrides
 * first, the packaged tree behind them. `/etc/xkb.local` keeps that
 * shape — drop a `symbols/us` in there and it wins over the staged one,
 * without editing a file that `make disk.img` will overwrite.
 *
 * Pointing both names at `/etc/xkb` also works and was what this file
 * did first. It is worse for one small reason: libxkbcommon lists its
 * include paths when a lookup fails, and that build printed
 *
 *     2 include paths searched:
 *         /etc/xkb
 *         /etc/xkb
 *
 * which reads like a bug in the search rather than one directory named
 * twice.
 *
 * ---- and why the environment cannot change either ----
 *
 * libxkbcommon lets XKB_CONFIG_ROOT, XKB_DEFAULT_LAYOUT and six other
 * variables override all of this. On this system every one of them is
 * unset and will stay that way: `environ` in libc/process.c is an empty
 * vector, so `getenv` returns NULL for everything, and `secure_getenv`
 * is `getenv` here (see below). Resolution therefore always falls
 * through to the defaults in this file. That is a property worth
 * knowing rather than a limitation to apologise for — it means the
 * keymap a program gets is the one this file names, every time.
 */

#ifndef VEXTRO_LIBXKBCOMMON_CONFIG_H
#define VEXTRO_LIBXKBCOMMON_CONFIG_H

/* ---- the file-reading decision. See the long note above. ---- */
/* #undef HAVE_MMAP */

/* ---- what this C library actually has ---- */

/*
 * Checked against `nm build/libvextro.a` rather than against
 * libc/include/*.h, because a declaration is not a definition — the
 * lesson third_party/wpe-config/ records about CMake's
 * check_function_exists applies just as well to reading one's own
 * headers. All four are `T` in the archive.
 */
#define HAVE_ASPRINTF          1
#define HAVE_VASPRINTF         1
#define HAVE_STRNDUP           1
#define HAVE_UNISTD_H          1

/* GCC, so the branch hints in src/utils.h are real. */
#define HAVE___BUILTIN_EXPECT  1

/*
 * Not here, and each absence lands on a fallback upstream maintains:
 *
 *   HAVE_SECURE_GETENV, HAVE___SECURE_GETENV
 *       src/utils.h:274-280 falls through to `#define secure_getenv
 *       getenv`. That is the right answer and not merely an accepted
 *       one: secure_getenv exists to make a setuid program ignore its
 *       inherited environment, and this system has no setuid bit, no
 *       inherited environment, and an `environ` that is empty.
 *
 *   HAVE_EACCESS, HAVE_EUIDACCESS
 *       src/utils.h:260-266 compiles the permission probe down to
 *       nothing and `check_eaccess()` returns true. Both functions ask
 *       whether the *effective* user could open a path; with neither
 *       defined libxkbcommon simply opens the file and handles the
 *       failure, which is the same answer arrived at one syscall later
 *       and without a race.
 *
 *   HAVE_MKOSTEMP, HAVE_POSIX_FALLOCATE
 *       read by the `xkbcli` tools and the test harness, neither of
 *       which is built here. Nothing under src/ mentions them.
 */
/* #undef HAVE_SECURE_GETENV */
/* #undef HAVE___SECURE_GETENV */
/* #undef HAVE_EACCESS */
/* #undef HAVE_EUIDACCESS */
/* #undef HAVE_MKOSTEMP */
/* #undef HAVE_POSIX_FALLOCATE */

/* ---- where the keymap data lives. See the long note above. ---- */
#define DFLT_XKB_CONFIG_ROOT        "/etc/xkb"
#define DFLT_XKB_CONFIG_EXTRA_PATH  "/etc/xkb.local"

/*
 * ---- and the one directory that is named and not shipped ----
 *
 * XLOCALEDIR is where src/compose/paths.c looks for X11's Compose
 * tables — the files that turn a sequence like <Multi_key> <a> <e> into
 * "æ". They belong to **libX11**, not to xkeyboard-config, and nothing
 * on this system uses them: WebKit's WPE port calls no `xkb_compose_*`
 * function, and the compose module is compiled only because it is part
 * of upstream's one library.
 *
 * So the path is named honestly rather than left to point at a Linux
 * location that does not exist here. `/etc/xkb/locale` is where those
 * files would go if libX11's locale tree were ever staged; until then
 * `xkb_compose_table_new_from_locale()` finds nothing and returns NULL,
 * which is its documented answer for a missing table and is the same
 * answer it gives on a Linux box with the data uninstalled. A named
 * empty directory beats a wrong absolute path.
 *
 * PATH_MAX is deliberately *not* defined here. meson supplies it only
 * for systems whose headers lack it; libc/include/limits.h has it, and
 * two defines of the same name from different files is how they end up
 * disagreeing.
 */
#define XLOCALEDIR                  "/etc/xkb/locale"

/* Reported by nothing under src/ — it is the version the xkbcli tools
 * print — but set so that the header and the archive agree if a tool is
 * ever built. */
#define LIBXKBCOMMON_VERSION        "1.7.0"

/*
 * ---- and what a context asks for when nobody says ----
 *
 * These five are the RMLVO defaults, used by
 * `xkb_keymap_new_from_names()` for any field the caller leaves NULL.
 * They are set to the same rules, model and layout WPEKeymapXKB.cpp
 * names explicitly, so a program that supplies nothing gets the keymap
 * WebKit would have asked for. `evdev` is the rule set written for Linux
 * input-layer keycodes, which is the numbering this system's PS/2 and
 * USB paths already produce; `pc105` is the physical model; `us` the
 * symbols.
 *
 * Note the names: **DEFAULT_XKB_RULES**, not XKB_DEFAULT_RULES. The
 * XKB_DEFAULT_* spelling also exists and is something else entirely —
 * the *environment variable* context-priv.c consults before falling back
 * to these — so defining that set instead compiles cleanly right up to
 * the point where context-priv.c:120 asks for the one that is missing.
 *
 * VARIANT and OPTIONS are the unquoted **NULL**, not "". That is what
 * meson emits when its default-variant option is empty
 * (meson.build:82-89 chooses between set_quoted and a bare NULL), and
 * the difference reaches rules.c: NULL means "this field was not
 * specified", while "" is a name to match against, and the two select
 * different rows of the rules table.
 */
#define DEFAULT_XKB_RULES     "evdev"
#define DEFAULT_XKB_MODEL     "pc105"
#define DEFAULT_XKB_LAYOUT    "us"
#define DEFAULT_XKB_VARIANT   NULL
#define DEFAULT_XKB_OPTIONS   NULL

#endif /* VEXTRO_LIBXKBCOMMON_CONFIG_H */
