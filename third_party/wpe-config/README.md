# wpe-config

How WPE WebKit is configured for Vextro, and — stated plainly, because
it is the more useful half — what still stands between this
configuration and an engine that runs.

    vextro-toolchain.cmake        the cross description, read before any
                                  compiler test
    vextro-project-inject.cmake   the OS gate, run inside project()
    vextro-wpe.cmake              the feature set, loaded as an initial
                                  cache

Nothing under `third_party/wpewebkit-2.46.5/` is patched. Everything
here works through seams CMake provides for exactly this — a toolchain
file, an initial cache, `CMAKE_PROJECT_INCLUDE` and a
`CMAKE_FIND_ROOT_PATH` — so an upstream release can be replaced without
carrying a patch set forward.

## The JIT is off, and not as a preference

JavaScriptCore has four execution tiers. Three of them work by writing
machine code into memory and jumping to it. On this system that is not
slow or discouraged — it is impossible, and impossible by construction:

- The linker scripts put text and data in separate segments.
- The loader maps each page with the protection its segment asked for.
- **`SYS_MMAP` and `SYS_MPROTECT` refuse `PROT_WRITE|PROT_EXEC`.** The
  refusal is in the system call, so no sequence of calls from user space
  arrives at a page that is both.

That third point is what makes this a configuration decision rather than
a tuning one. A JIT does not fail at the `mprotect` whose return value it
checked; it fails at the jump, thousands of instructions later, with a
fault at an address inside a data buffer and nothing connecting it to the
cause. `apps/threadtest.c` asserts the refusal in both directions, so the
property this configuration depends on is checked on every boot rather
than assumed.

`ENABLE_C_LOOP=ON` is the positive form of the same decision, and the one
that actually matters: it selects the portable bytecode interpreter over
the assembly LLInt, which is itself generated code. The cost is real —
roughly an order of magnitude on arithmetic-heavy JavaScript — and it is
a configuration WebKit supports, being what it ships for architectures
with no assembler backend.

## What is done and what is not

**Done, and checked on the machine:**

| | |
|---|---|
| libwpe 1.16.2, vendored and cross-compiled | `third_party/libwpe/` |
| the Vextro backend: view, renderer host, EGL, EGL target | `third_party/wpe-port/` |
| the pixel path into the ring-3 window | `vxwpe_present()` |
| input translated from Vextro's state to WPE events | `vxwpe_pump()` |
| the static loader, with no `dlopen` | upstream's `loader-static.c` |
| the configuration above | this directory |
| a C library a POSIX port can build against | `libc/`, `make test` |
| threads, `mmap`, TLS in the kernel | `SYS_CLONE`, `SYS_MMAP`, `SYS_SET_FSBASE` |
| the browser skin, from Tailwind utility classes | `assets/ui/`, `src/vxui.h` |
| SQLite, FreeType, HarfBuzz, cross-compiled and green in ring 3 | `apps/sqltest.c`, `fttest.c`, `hbtest.cpp` |
| ICU 74.2, and the data archive read off the volume | `apps/icutest.cpp` |
| run-time type information, so a library may use `dynamic_cast` | `libcxx/src/typeinfo.cpp` |
| a calendar, so that a date formatter has a real "now" | `SYS_WALLCLOCK`, `libc/calendar.c` |
| libjpeg-turbo, libepoxy, libgcrypt, libtasn1, libxkbcommon and libxml2 in ring 3 | `apps/jpegtest.c`, `gltest.c`, `gcrypttest.c`, `tasn1test.c`, `xkbtest.c`, `xmltest.c` |
| the keymap data a browser needs, on the volume | `/etc/xkb`, 250 files from xkeyboard-config |
| WebKit's OS detection, passed without patching it | `vextro-project-inject.cmake` |
| a sysroot the find modules can see all twelve ported libraries in | `make webkit-sysroot` |

`apps/wpetest.c` runs all of that in ring 3 on every `APP_SELFTEST` boot.

## The three that were blocking, and are not now

This section used to list six blockers. The first three were the ones
that could not be worked around, and all three are done:

**1. A C++ runtime for the target.** Was: "`x86_64-elf-g++` exists and
has no `libstdc++`, no `libsupc++`, and no C++ standard headers."

Now: `libcxx/`. `operator new` in all twelve of its forms over the
ring-3 allocator; `__cxa_atexit`, `__cxa_finalize`, the guard variables
that make a function-local static thread-safe (over the kernel's futex),
`__cxa_pure_virtual`, `__cxa_throw_bad_array_new_length`; and
thirty-five headers — `<vector>`, `<string>`, `<memory>`, `<new>`,
`<algorithm>`, `<atomic>`, `<mutex>`, `<optional>`, `<functional>`,
`<unordered_map>`, `<thread>`, `<chrono>`, `<variant>` and the rest.

Written rather than ported, and it is worth saying why since the
directive allowed either. GNU's `libstdc++` needs the matching GCC
source tree and a sysroot configure; LLVM's `libc++` needs cmake, which
is item 3 below. Both would then drag in locales, iostreams and a
threading model this system does not have.

**Checked**: `tools/cxx_test.cpp` runs 218 checks on the host against
these same headers — sorting in five adversarial orders, every string
length across the small-buffer boundary, tracked objects counted for
leaks. `apps/cxxtest.cpp` runs 150 more in ring 3 on the machine, which
is the only place the allocator, the static constructors, the guard
variables under four racing threads, `std::thread` over `SYS_CLONE` and
a vtable surviving the loader can be checked at all.

**2. Exceptions and RTTI — and these two have come apart.**

Exceptions are still off, and now by a positive decision rather than by
absence: `-fno-exceptions` is what WebKit is normally built with, and
`libcxx/` is written for it. `at()` on a vector out of range prints which
index and stops; plain `operator new` on exhaustion prints and stops;
`new (std::nothrow)` returns null. Those are the three choices available
without an unwinder and the reasoning is in `libcxx/include/new`.

RTTI is no longer off, because ICU made the question concrete. It uses
`dynamic_cast` in 117 places and `typeid` in about forty, with no
fallback path, so `-fno-rtti` was not a preference that could be kept —
it was a decision not to port ICU. `libcxx/include/typeinfo`,
`libcxx/include/cxxabi.h` and `libcxx/src/typeinfo.cpp` are what sits
underneath it: the descriptor hierarchy GCC emits into every `-frtti`
translation unit, and the search in `[expr.dynamic.cast]` over it.

The two flags stay independent, which is the useful part. `-frtti` costs
a pointer per polymorphic class and a table per type. Only ICU and one
test object are built with it; everything else here, WebKit included, is
still `-fno-exceptions -fno-rtti`.

**Checked**: `apps/rtti_cases.h` is compiled twice — once on the host
against the host's own C++ runtime, once in ring 3 against this one —
and the 43 expectations are addresses the compiler works out statically,
so neither run is checking an implementation against itself. They cover
single inheritance, multiple inheritance with an offset base, both
cross-cast directions, a virtual diamond, a repeated non-virtual base
reachable only through the second clause of the rule, a target present
twice that must come back null, and a private base that must not be
found.

**3. File descriptors in ring 3.** Was: "the largest remaining item."

Now: nineteen system calls, `src/vfs.h`, and the POSIX layer over them
in `libc/file.c`. `open`, `read`, `write`, `close`, `lseek`, `stat`,
`fstat`, `getdents`, `unlink`, `mkdir`, `fsync`, `ftruncate` — plus real
`FILE` streams, `fopen`/`fread`/`fgets`/`fseek`, the `scanf` family, and
`opendir`/`readdir` in `<dirent.h>`.

**4. Sockets in ring 3.** Was: "nothing exports it across the
system-call boundary."

Now: `socket`, `connect`, `send`, `recv`, `shutdown`, `setsockopt`,
`getaddrinfo` and `gethostbyname`, over the same `src/vxnet.h` seam the
kernel's own browser uses — lwIP for plain TCP, Mbed TLS for
`IPPROTO_TLS`. The certificate caveat is unchanged and is repeated in
every header that mentions TLS: there is no CA store on this volume, so
nothing establishes that the certificate belongs to the host that was
asked for.

**Checked**: `apps/fdprobe.c` (13 checks, raw system calls),
`apps/fdtest.c` (73, through the C library), and `vfs_selftest()` in
`src/vfs.h` (14, kernel-side — the write-back path, which a ring-3
program cannot reach with nobody signed in to permit it).

## What is left

Five libraries are ported and checked on the machine; the rest of this
section is what a real `make webkit` says, run rather than predicted.

**5. ~~cmake, ninja~~ — installed.** cmake 4.4.3 and ninja 1.13.2. The
reasoning that kept them out ("installing them unblocks nothing while
blocker 1 stands") expired when blockers 1 to 3 were cleared, and this
ladder listed it as step 4.

**6. The C++ standard library.** `<tuple>`, `<map>`, `<set>` and ranges
are still not written. What the five ports here actually needed was
smaller than expected each time, and is now in: `<inttypes.h>` and
`<cfloat>` for HarfBuzz, then
`<condition_variable>`, `<typeinfo>`, `<cxxabi.h>`, `<complex>`,
`<cinttypes>` and `is_standard_layout` for ICU. None of the five needed
a patch to its own source.

**7. The dependency set — eight found, and the count is still worse than
"a few more".** `Source/cmake/OptionsWPE.cmake` opens with **twenty-two
unconditional `find_package(... REQUIRED)`** and has more behind
conditionals:

    HarfBuzz  ICU  JPEG  Epoxy  LibGcrypt  Libtasn1  Libxkbcommon
    LibXml2  PNG  SQLite3  Threads  Unifdef  WebP  WPE  ZLIB
    LibSoup  GLIB  Cairo  Fontconfig  Freetype  LibXslt  ...

Ported, built, and staged into the sysroot: **SQLite**, **FreeType**,
**WPE**, **HarfBuzz**, **ICU 74.2**, **libjpeg-turbo 3.0.4**,
**libepoxy 1.5.10**, **libgpg-error 1.50 + libgcrypt 1.10.3**,
**libtasn1 4.19.0**, **libxkbcommon 1.7.0** and **libxml2 2.12.6**.

Eight of those have actually been *found by a configure run*, which is
the only claim worth making: HarfBuzz with the `harfbuzz-icu` archive
that satisfies `find_package(HarfBuzz COMPONENTS ICU)`, ICU with the
`data i18n uc` components, JPEG at version 62, Epoxy at 1.5.10,
LibGcrypt at 1.10.3, Libtasn1 at 4.19.0, Libxkbcommon at 1.7.0 and
LibXml2 at 2.12.6. The
other three sit further down `OptionsWPE.cmake` than configure has
reached — behind PNG on line 16 — so nothing has exercised
`FindSQLite3`, `FindWPE` or CMake's own `FindFreetype` against the
layout they are staged in. Staged and findable are not the same fact
until a run says so.

Not done, and each its own project: GLIB, LibSoup, Cairo, Fontconfig,
zlib, libpng, WebP, Unifdef.

### ICU was easier than this file used to claim

This section said ICU could not be cross-compiled in the ordinary way,
because its build first compiles host tools that generate a
thirty-megabyte data archive — and that the bootstrap, not the compile,
was the work.

That was wrong, and it is worth recording as wrong rather than quietly
replacing. **The source tarball ships the finished archive**, at
`data/in/icudt74l.dat`: thirty megabytes, already packaged, and the `l`
on the end means little-endian, which is this machine. The host
bootstrap exists to *filter* that archive down to a subset — which is
what a project shipping ICU on a phone wants and is not what was needed
here. There is no data build in this repository at all; the file is
copied onto the volume and ICU is told where to look with
`u_setDataDirectory("/")`, after which it opens it through its own
stdio path.

What ICU did cost was two things neither of which was the data:

**RTTI.** ICU 74 uses `dynamic_cast` in 117 places and `typeid` in about
forty, and unlike older versions it has no `U_HAVE_RTTI` fallback —
`utypeinfo.h` includes `<typeinfo>` unconditionally. So it is the one
library here compiled `-frtti`, and `libcxx/` grew what sits underneath
that: `std::type_info`, the `__cxxabiv1` descriptor hierarchy, and
`__dynamic_cast`. See `libcxx/src/typeinfo.cpp`.

**A calendar.** `time()` answered from the monotonic tick, so every date
ICU produced would have been in January 1970. `SYS_WALLCLOCK` carries
the CMOS reading up to ring 3 and `libc/calendar.c` takes it apart.

**Epoxy** still deserves its naming. It is an OpenGL function loader,
and there is no GL reachable from ring 3 at all — `vextro-wpe.cmake`
already turns the GPU off, which means this `find_package` has to be
defeated rather than satisfied.

## The OS gate, and how it was passed

This section used to be titled "the blocker that comes before all of
them". It described a `FATAL_ERROR` in WebKit's own operating-system
detection that fired before a single `find_package` ran:

    CMake Error at Source/cmake/WebKitCommon.cmake:152 (message):
      Unknown OS 'Generic'

The code is a closed list:

    if (UNIX)
        ... macOS / Linux / other Unix
    elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
    elseif (CMAKE_SYSTEM_NAME MATCHES "Fuchsia")
    else ()
        message(FATAL_ERROR "Unknown OS '${CMAKE_SYSTEM_NAME}'")

`Generic` is CMake's name for a target with no operating system under
it, and it is the one answer the list refuses. Configuring at all meant
landing on one of those three names.

**It is passed, and not by patching WebKit.** Nothing under
`third_party/wpewebkit-2.46.5/` is modified. The layer is two files in
this directory:

`vextro-project-inject.cmake` sets `UNIX`. It runs because
`vextro-toolchain.cmake` names it in `CMAKE_PROJECT_INCLUDE` — CMake's
own seam for configuring a project whose `CMakeLists.txt` you do not
own, run as the last step of every `project()` call.

The obvious thing — `set(UNIX 1)` in the toolchain file — does not work,
and the reason is worth keeping. `project()` recomputes `UNIX`, `WIN32`
and `APPLE` from `CMAKE_SYSTEM_NAME` *after* reading the toolchain file.
Measured with a four-line probe against this exact toolchain:

    -- BEFORE project(): UNIX='1'      inherited from the macOS host
    -- TOOLCHAIN: UNIX='1'             set there
    -- AFTER  project(): UNIX=''       recomputed for Generic

WebKit calls `project()` on line 10 and `include(WebKitCommon)` on line
16, so the assignment is undone six lines before it is read.
`CMAKE_PROJECT_INCLUDE` is the window between them.

**`UNIX`, and deliberately not `CMAKE_SYSTEM_NAME Linux`.** Both get
past line 152; they do not get past it to the same place. Naming the
system Linux sets `WTF_OS_LINUX` and selects the paths written for
glibc, `/proc`, raw `futex(2)`, `epoll`, `memfd` and a dynamic loader,
none of which exist here — every one a claim that would come back as a
compile error with no way to tell it from an unported dependency.
`UNIX` with the system name left as `Generic` sets `WTF_OS_UNIX`, the
generic path the BSDs use. That is the weaker claim and the true one: it
asserts a process model — descriptors, sockets, threads, `mmap`, a
copy-on-write `fork` — and `src/vfs.h`, `libc/socket.c`,
`libc/pthread.c` and `libc/mmap.c` are that model, green on the machine.
It also leaves CMake loading `Platform/Generic` rather than
`Platform/Linux`, which is the accurate description of a machine with no
runtime linker.

The compiler half is separate and easy to miss: `Source/WTF/wtf/
PlatformOS.h` derives `OS()` from predefined macros and never from these
CMake variables, and `x86_64-elf-gcc` predefines none of them. So the
toolchain also passes `-D__unix__=1` — the smallest true thing to say —
and `-D__vextro__=1`, which claims nothing and is there to be asked.
Both are scoped to this toolchain file; the C library, `libcxx/` and the
three library ports are built by the main Makefile without them.

**What configure does now.** It runs WebKit's whole feature-detection
pass against this C library, and the results are real answers rather
than a wall:

    found       errno.h  sys/mman.h  sys/types.h  stdint.h  stddef.h
                vasprintf  __int128_t  _Float16  builtin atomics
                -fno-exceptions  -fno-rtti  -fcoroutines
    not found   features.h  langinfo.h  sys/param.h  sys/time.h
                localtime_r  timegm  strnstr  regexec  SIGTRAP
                statx  malloc_trim  tm_gmtoff  tm_zone  st_birthtime
                std::filesystem

That right-hand column is a work list for `libc/` — small, specific, and
produced by WebKit itself rather than guessed at.

Two earlier `make webkit` failures were real and are fixed: neither the
recipe nor `vextro-toolchain.cmake` quoted paths, and this repository
lives in a directory with a space in its name — so CMake's first
compiler test failed with `x86_64-elf-gcc: error: Custom: linker input
file not found`. Both had been written and never run, because cmake was
not installed to run them.

## Where it stops today

`make webkit` reaches `OptionsWPE.cmake:16` — the *ninth* dependency
check of twenty-two — and stops:

    -- Found HarfBuzz: .../build/webkit-sysroot/include/harfbuzz
       (found suitable version "8.5.0", minimum required is "1.4.2")
    -- Found ICU: .../build/webkit-sysroot/include
       (found suitable version "74.2", minimum required is "61.2")
       found components: data i18n uc
    -- Found JPEG: .../build/webkit-sysroot/lib/libjpeg.a
       (found version "62")
    -- Found Epoxy: .../build/webkit-sysroot/lib/libepoxy.a
       (Required is at least version "1.5.4")
    -- Found LibGcrypt: .../build/webkit-sysroot/lib/libgcrypt.a
       (found suitable version "1.10.3", minimum required is "1.7.0")
    -- Found Libtasn1: .../build/webkit-sysroot/lib/libtasn1.a
    --   Found xkbcommon, version 1.7.0
    -- Found Libxkbcommon: TRUE (Required is at least version "0.4.0")
    -- Found LibXml2: .../build/webkit-sysroot/lib/libxml2.a
       (found suitable version "2.12.6", minimum required is "2.8.0")
    -- Could NOT find ZLIB (missing: ZLIB_LIBRARY ZLIB_INCLUDE_DIR)
    CMake Error: Could NOT find PNG
      (missing: PNG_LIBRARY PNG_PNG_INCLUDE_DIR)

Libtasn1 prints no version, and that is `FindLibtasn1.cmake` rather than
a gap: it hands `find_package_handle_standard_args` a single
`REQUIRED_VARS LIBTASN1_LIBRARIES` and no `VERSION_VAR`, and
`OptionsWPE.cmake:13` asks for no minimum. It is the only one of the
seven written with nothing to compare.

**A correction, because this section used to say otherwise.** It read
that Epoxy could not be found without a **pkg-config module** — that
`FindEpoxy.cmake` takes `Epoxy_VERSION` from pkg-config and nowhere
else, and that the sysroot therefore got as far as
`Required version (1.5.4) is higher than found version ()` about an
archive sitting in it. The first half is true and the conclusion is not.
`FindEpoxy.cmake:70-77` guards that `FATAL_ERROR` with
`if (Epoxy_VERSION)`, so an **empty** version takes the other branch and
prints a warning instead:

    CMake Warning at Source/cmake/FindEpoxy.cmake:77 (message):
      Cannot determine Epoxy version withouit pkg-config

— upstream's typo included — and configure carries on.
`find_package_handle_standard_args` does not reject a package for an
unset `VERSION_VAR`. The quoted error was **HarfBuzz's**, whose find
module computes a version out of the headers, and it was generalised to
Epoxy without being reproduced.

This is measurable rather than arguable: pkg-config is **not installed
on this machine**, `build/webkit/CMakeCache.txt` records
`PKG_CONFIG_EXECUTABLE-NOTFOUND`, every find module has been taking its
`find_path`/`find_library` fallback all along, and all six that have
been reached still succeed. The `.pc` files `make webkit-sysroot` writes
are inert today.

They are written and kept anyway. Installing pkg-config is one command,
and on a machine that has it the toolchain's `PKG_CONFIG_LIBDIR` points
at that directory *and nowhere else* — which is what would stop the
host's own libraries from answering a query about this system's.

Both of the first two lines were errors before this. HarfBuzz was
rejected for not being built against ICU — `find_package(HarfBuzz 1.4.2
REQUIRED COMPONENTS ICU)` looks for a `libharfbuzz-icu` beside it, and
there was none. And before that it was not found at all: the message
read *"Required version (1.4.2) is higher than found version ()"* about
a library that was built, tested, and passing 32 assertions in ring 3,
because the find modules look with pkg-config (absent here) and then in
the layout a Unix installation has, which `build/` is not.

`build/webkit-sysroot/` is that layout, staged by `make webkit-sysroot`
from artifacts the main Makefile already built and the machine already
ran. It is not a substitute for anything: `libicuuc.a` in it is the
archive `icutest` exercised, and FreeType's two redirected config
headers are copied over upstream's so the headers in it describe the
archive beside them.

So the frontier is now PNG, and there are nine behind it — but note the
ZLIB line above it. `FindPNG` looks for zlib first and does not find it
either, so the next entry is really **two** ports, and zlib is the one
that has to come first.

## The exit code

Non-zero, and this is the honest reading of it.

Clearing the OS gate moved the wall from *before* the dependency list to
*inside* it; ICU moved it two entries along, and JPEG and Epoxy two
more. None of that is a browser: thirteen unported REQUIRED libraries
stand behind the current error.

**Epoxy is the one worth being precise about**, because it is the entry
that changed character rather than merely being ticked off. It is an
OpenGL function *loader* — a dispatcher that resolves two thousand entry
points by name at run time — so it builds perfectly well on a machine
with no OpenGL, and `find_package(Epoxy 1.5.4)` is satisfied honestly by
an archive generated from Khronos's own registry. What decides the
outcome is what `dlsym` finds, and on this machine it finds nine: the
provider in `third_party/libepoxy-port/vxgl.c` binds glClear,
glClearColor, glViewport, glGetString, glGetIntegerv, glGetError,
glFlush, glFinish and glReadPixels to the framebuffer at
`/dev/dri/renderD128`, and everything with a pipeline behind it is
absent from the table. `apps/gltest.c` checks the whole chain — dispatch
stub, dlopen, dlsym, provider, render node, window — and its last section
forks a child that calls `glDrawArrays`, which prints

    glDrawArrays() not found: this system does not have that symbol

and aborts. That is libepoxy's own code path, unpatched, naming the call
it could not serve. A stub that returned quietly would have produced a
black window and no explanation. The rung that changes it is a software
GL over `src/g3d.h`'s rasteriser, which is a separate project.

The arithmetic is worth stating plainly, because "two down, sixteen to
go" reads more encouraging than it is. The two done here are the two
that had no substitute — nothing else supplies the Unicode tables, and
nothing else supplies shaping. Several of the sixteen are small (zlib,
libpng, libjpeg, libtasn1). Two are not: **GLib** with **LibSoup** is a
second runtime with its own main loop, type system and TLS stack, and
**Epoxy** wants a GL implementation this machine does not have and this
kernel deliberately does not provide.

There is also a second gate further along that no build-system work
reaches, and it has moved. `WTF_OS_UNIX` gets the files chosen; the code
in them assumes a POSIX process model, and this system now has one for
most of what they assume.

**What was built for it (30 Aug 2026):** `include/vls.h` and
`src/sched/vls_core.c`, the Vextro Linux Subset — sixty-seven Linux
system calls translated, POSIX signals with a real frame on the user
stack and an `rt_sigreturn` that restores from the `ucontext`, `execve`,
`wait4`, `dup`/`dup2`, `clone(CLONE_VM)`, process identity and
parentage on `addr_space_t`, and eight `/dev` nodes. 111 checks in ring
3, `apps/vlstest.c`. It does not move the `make webkit` line and was
never going to: that line is a missing *library*, and this is the rung
after the libraries.

**Pipes, `poll` and `socketpair` arrived with libgcrypt** (30 Aug 2026),
because its error library's `visibility.c` defines every public name in
one file and the locks came with the spawn module. `src/vfs.h` has an
`FD_PIPE` kind now — a ring with separately counted ends, shared rather
than duplicated across a fork — and `SYS_POLL` is the readiness
interface GLib's main loop is built on.

**What it still does not settle:** descriptor passing between processes
(no `SCM_RIGHTS`), and `mmap` with a file behind it. A genuine
`WTF_OS_VEXTRO` inside WebKit is still what settles the last of it, and
it is upstream porting work of a different kind from anything below it
on this ladder. What has changed is how much of it is left.

## The order

1. ~~`libstdc++` for `x86_64-elf`~~ — done, as `libcxx/`
2. ~~VFS system calls and file descriptors in ring 3~~ — done
3. ~~Socket system calls over the kernel's existing stack~~ — done
4. ~~Install cmake and ninja~~ — done
5. ~~SQLite, FreeType, HarfBuzz~~ — done, and checked in ring 3
6. ~~Get past WebKit's OS detection~~ — done, without patching WebKit;
   `vextro-project-inject.cmake` and `CMAKE_PROJECT_INCLUDE`
7. ~~Make the ported libraries findable~~ — done,
   `make webkit-sysroot`
8. ~~ICU~~ — done, 74.2, with `libharfbuzz-icu` beside it. Its data
   archive ships prebuilt; what it actually cost was RTTI and a
   calendar, both of which are now in.
9. ~~JPEG~~, ~~Epoxy~~, ~~LibGcrypt~~, ~~Libtasn1~~, ~~Libxkbcommon~~,
   ~~LibXml2~~ — done.
   libjpeg-turbo 3.0.4 (no SIMD, 8/12/16-bit, 20 checks in ring 3
   against a bitstream macOS encoded); libepoxy 1.5.10 (nine entry
   points bound to the framebuffer, the rest refused by name);
   libgcrypt 1.10.3 with libgpg-error 1.50 (33 checks against FIPS 197,
   RFC 4231, RFC 6070 and NIST, on the portable C paths); libtasn1
   4.19.0 (94 checks against DER the build machine's OpenSSL wrote,
   through the ASN.1 module WebKit itself compiles); libxkbcommon 1.7.0
   with 2.7 MB of xkeyboard-config staged at `/etc/xkb`, compiling the
   `evdev/pc105/us` keymap WebKit asks for by name (67 checks against
   Linux keycodes and X11 keysyms); libxml2 2.12.6 with zlib, iconv,
   ICU, modules, FTP and HTTP all compiled out (66 checks, driven the
   way `XMLDocumentParserLibxml2.cpp` drives it). **PNG** is where
   configure stops today, and there are nine behind it — but `FindPNG`
   wants zlib first, so that entry is two ports and zlib leads. The
   rest are small — WebP, Unifdef — and `GLib`/`LibSoup` is not.
10. The `libc/` gaps WebKit's own configure named: `langinfo.h`,
    `strnstr`, `regexec`, `statx`, `malloc_trim`. `sys/time.h`,
    `localtime_r`, `timegm`, `SIGTRAP`, `tm_gmtoff` and `tm_zone` were
    on this list and are now written — ICU needed them first.
11. ~~A process model for the places `OS(UNIX)` assumes one~~ — mostly
    done, as **VLS**: signals, `execve`, `wait4`, `dup`, `clone`, and
    `/dev`. ~~Pipes~~ are done too, built because libgcrypt would not
    link without them — `pipe`, `pipe2`, `poll` and `socketpair`. What
    is left is descriptor passing *between* processes and **file-backed
    `mmap`**; the second of those is now a named cost rather than an
    abstract gap, since it is why `libxkbcommon-port/config.h` leaves
    HAVE_MMAP undefined and reads every keymap file through upstream's
    fread fallback. A real `WTF_OS_VEXTRO` inside WebKit is what settles
    the first.
12. WPE WebKit, with this configuration
13. The browser application, over `third_party/wpe-port/` and the skin
    in `assets/ui/`

Steps 1 to 3 were each comparable in size to the C library written to
reach them. Steps 6 and 7 were small and worth doing precisely because
they are: they turned an error that said nothing about this system into
a dependency list that says exactly what is left. Step 8 was the largest
single port here — 445 translation units — and it is the one that made
two long-standing gaps in the system underneath it impossible to keep
ignoring.

What remains is breadth for most of it and depth for three items: a GL
implementation, a GLib-shaped second runtime, and a WTF platform port.
None of those is reachable by configuring anything. Step 11 was the one
that could be done out of order — it needs no library and no configure
run, only a kernel — and it was, which is why it is struck through above
while step 9 is not.
