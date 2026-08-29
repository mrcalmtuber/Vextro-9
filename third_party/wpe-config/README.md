# wpe-config

How WPE WebKit is configured for Vextro, and — stated plainly, because
it is the more useful half — what still stands between this
configuration and an engine that runs.

    vextro-toolchain.cmake   the cross description, read before any
                             compiler test
    vextro-wpe.cmake         the feature set, loaded as an initial cache

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
twenty-eight headers — `<vector>`, `<string>`, `<memory>`, `<new>`,
`<algorithm>`, `<atomic>`, `<mutex>`, `<optional>`, `<functional>` and
the rest.

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

## And the blocker that comes before all of them

`make webkit` now gets past its prerequisite check, past CMake's compiler
test, and stops here:

    CMake Error at Source/cmake/WebKitCommon.cmake:152 (message):
      Unknown OS 'Generic'

The code is a closed list:

    if (UNIX)
        ... macOS / Linux / other Unix
    elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
    elseif (CMAKE_SYSTEM_NAME MATCHES "Fuchsia")
    else ()
        message(FATAL_ERROR "Unknown OS '${CMAKE_SYSTEM_NAME}'")

`Generic` is CMake's name for a target with no operating system under it,
which is what this machine is. WebKit's build system has no such
category, and this fires before a single `find_package` is reached — so
"only ICU is missing" would have been false in a way no amount of
library porting would fix.

Getting past it is not a flag. It means a Vextro port *in WebKit*: a
`WTF_OS_VEXTRO`, an `OptionsVextro.cmake`, a `PlatformVextro.cmake`, and
then every `#if OS(UNIX)` in WTF and WebCore that assumes a POSIX process
model — signals, `fork`/`exec`, file descriptors passed between
processes, `mmap` with a file behind it. That is upstream porting work of
a different kind from anything below it on this ladder, and it is the
honest frontier.

The two `make webkit` failures before it were real and are fixed: neither
the recipe nor `vextro-toolchain.cmake` quoted paths, and this repository
lives in a directory with a space in its name — so CMake's first compiler
test failed with `x86_64-elf-gcc: error: Custom: linker input file not
found`. Both had been written and never run, because cmake was not
installed to run them.

## The order

1. ~~`libstdc++` for `x86_64-elf`~~ — done, as `libcxx/`
2. ~~VFS system calls and file descriptors in ring 3~~ — done
3. ~~Socket system calls over the kernel's existing stack~~ — done
4. ~~Install cmake and ninja~~ — done
5. ~~SQLite, FreeType, HarfBuzz~~ — done, and checked in ring 3
6. **Teach WebKit that this operating system exists** — `WTF_OS_VEXTRO`
   and the platform files. Everything below is unreachable until this is
   done, because it fails before any `find_package` runs.
7. The remaining eighteen dependencies, in dependency order, with ICU
   first because most of the rest are easier
8. WPE WebKit, with this configuration
9. The browser application, over `third_party/wpe-port/` and the skin in
   `assets/ui/`

Steps 1 to 3 were each comparable in size to the C library written to
reach them, and that is what has just been done. What remains is
breadth rather than depth: every item above is a port onto interfaces
that exist and are tested, not an interface that has to be invented.
