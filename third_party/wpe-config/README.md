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
| WebKit's OS detection, passed without patching it | `vextro-project-inject.cmake` |
| a sysroot the find modules can see the four ported libraries in | `make webkit-sysroot` |

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
leaks. `apps/cxxtest.cpp` runs 107 more in ring 3 on the machine, which
is the only place the allocator, the static constructors, the guard
variables under four racing threads, `std::thread` over `SYS_CLONE` and
a vtable surviving the loader can be checked at all.

**2. Exceptions and RTTI.** Still off, and now by a positive decision
rather than by absence: `-fno-exceptions -fno-rtti` is what WebKit is
normally built with, and `libcxx/` is written for it. `at()` on a vector
out of range prints which index and stops; plain `operator new` on
exhaustion prints and stops; `new (std::nothrow)` returns null. Those
are the three choices available without an unwinder and the reasoning is
in `libcxx/include/new`.

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

Four libraries are ported and checked on the machine; the rest of this
section is what a real `make webkit` says, run rather than predicted.

**5. ~~cmake, ninja~~ — installed.** cmake 4.4.3 and ninja 1.13.2. The
reasoning that kept them out ("installing them unblocks nothing while
blocker 1 stands") expired when blockers 1 to 3 were cleared, and this
ladder listed it as step 4.

**6. The C++ standard library.** `<tuple>`, `<map>`, `<set>`, ranges and
`<condition_variable>` are still not written. What the four ports here
actually needed was smaller than expected and is now in: `<inttypes.h>`
(which the C library simply did not have), `<cfloat>`, and the
`is_trivially_copy_assignable` family. HarfBuzz compiled against
`libcxx/` with those three additions and no patches.

**7. The dependency set — four done, and the count is worse than "a few
more".** `Source/cmake/OptionsWPE.cmake` opens with **twenty-two
unconditional `find_package(... REQUIRED)`** and has more behind
conditionals:

    HarfBuzz  ICU  JPEG  Epoxy  LibGcrypt  Libtasn1  Libxkbcommon
    LibXml2  PNG  SQLite3  Threads  Unifdef  WebP  WPE  ZLIB
    LibSoup  GLIB  Cairo  Fontconfig  Freetype  LibXslt  ...

Done: **SQLite**, **FreeType**, **HarfBuzz**, and **WPE** (libwpe, from
the earlier work). Note that WebKit asks for `HarfBuzz COMPONENTS ICU` —
HarfBuzz built *against* ICU — so even the one that is ported is not yet
in the shape the configure wants.

Not done, and each its own project: ICU, GLIB, LibSoup, Cairo,
Fontconfig, libxml2, zlib, libpng, libjpeg, WebP, libgcrypt, libtasn1,
libxkbcommon, Epoxy.

Two of those deserve naming. **ICU** is not a library that can be
cross-compiled in the ordinary way: its build first compiles *host*
tools which then generate a thirty-megabyte data archive, and that
bootstrap is the work rather than the compile. **Epoxy** is an OpenGL
function loader, and there is no GL reachable from ring 3 at all —
`vextro-wpe.cmake` already turns the GPU off, which means this
`find_package` has to be defeated rather than satisfied.

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

`make webkit` reaches `OptionsWPE.cmake:8` — the first dependency check
of twenty-two — and stops:

    -- Found hb-features.h
    -- Found the following HarfBuzz libraries:
    --  HarfBuzz (required): .../build/webkit-sysroot/lib/libharfbuzz.a
    -- The following HarfBuzz libraries were not found:
    --  ICU (required)
    CMake Error: Could NOT find HarfBuzz (missing:
      _HarfBuzz_REQUIRED_LIBS_FOUND) (found suitable version "8.5.0",
      minimum required is "1.4.2")

Which is now an accurate statement rather than a confusing one. Before
`make webkit-sysroot` existed, the same line read *"Required version
(1.4.2) is higher than found version ()"* — HarfBuzz was built, tested
and passing 32 assertions in ring 3, and the build system could not see
it, because it looks with pkg-config (absent here) and then in the
layout a Unix installation has (which `build/` is not).

`build/webkit-sysroot/` is that layout, staged from artifacts the main
Makefile already built and the machine already ran. It is not a
substitute for anything: `libfreetype.a` in it is the archive `fttest`
exercised, and the two redirected config headers are copied over
upstream's so the headers in it describe the archive beside them.

So the frontier is exactly ICU, and there are seventeen behind it.

## The exit code

Non-zero, and this is the honest reading of it.

Clearing the OS gate moved the wall from *before* the dependency list to
*inside* it. That is the whole of what an OS-name shim can do, and it is
what was asked for. It is not, and could not be, a browser: eighteen
unported REQUIRED libraries stand behind that error, one of them
(**Epoxy**) an OpenGL function loader for a machine whose graphics
driver deliberately implements the blitter and not the 3D pipeline.

There is also a second gate further along that no build-system work
reaches. `WTF_OS_UNIX` gets the files chosen; the code in them still
assumes a POSIX process model in places this system has no answer for
yet — signals, `fork`/`exec`, descriptors passed between processes,
`mmap` with a file behind it. A genuine `WTF_OS_VEXTRO` inside WebKit
is what settles those, and it is upstream porting work of a different
kind from anything below it on this ladder.

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
8. **ICU**, which is where configure stops today. Not an ordinary
   cross-compile: its build first compiles *host* tools that generate a
   thirty-megabyte data archive, and that bootstrap is the work.
9. The other seventeen, in dependency order. Two are structural rather
   than laborious — **Epoxy** loads OpenGL entry points for a machine
   with no GL, and **GLib**/**LibSoup** are a second runtime with their
   own main loop, type system and TLS stack.
10. The `libc/` gaps WebKit's own configure named: `sys/time.h`,
    `langinfo.h`, `localtime_r`, `timegm`, `strnstr`, `regexec`,
    `SIGTRAP`, `tm_gmtoff`, `tm_zone`
11. A real `WTF_OS_VEXTRO` inside WebKit, for the places `OS(UNIX)`
    assumes a process model this system does not have
12. WPE WebKit, with this configuration
13. The browser application, over `third_party/wpe-port/` and the skin
    in `assets/ui/`

Steps 1 to 3 were each comparable in size to the C library written to
reach them. Steps 6 and 7 are small by comparison and were worth doing
precisely because they are: they turned an error that said nothing about
this system into a dependency list that says exactly what is left.

What remains is breadth for most of it and depth for two items. Steps 8
to 10 are ports onto interfaces that exist and are tested. Steps 9's two
named exceptions and step 11 are not — a GL implementation and a WTF
platform port are each their own project, and neither is reachable by
configuring anything.
