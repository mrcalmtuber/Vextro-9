# libcxx

A freestanding C++ runtime and standard library subset for `x86_64-elf`,
built against `libc/`.

    include/     the headers, forty of them
    src/         the parts the compiler calls by name

## Why this exists rather than a port

`x86_64-elf-g++` has a C++ front end and had no C++ *runtime*:
`-print-file-name=libstdc++.a` echoed its input back, and
`#include <vector>` failed. That was the first item on the ladder in
`third_party/wpe-config/README.md` and the reason nothing C++ could be
compiled for this machine at all.

The two ways to get one are to build GNU's `libstdc++`, which needs the
matching GCC source tree and a sysroot configure, or LLVM's `libc++`,
which needs cmake — deliberately not installed here. Both would then
bring locales, iostreams, a threading model and a filesystem library
this system does not have, and each of those is a set of undefined
symbols to be worked around rather than a feature.

So it is written, as `libc/` was, and for the same reason: what a port
would spend its time removing is most of what a port would bring.

## What the compiler calls without being asked

`src/` is the part that is not optional. Every symbol in it is emitted
by the compiler into object files, and a runtime missing one fails to
link at the end of a long build with a mangled name and no explanation.

    operator new / delete       all twelve forms — sized delete is emitted
                                by default since C++14 and the aligned
                                forms for any over-aligned type
    __cxa_atexit / __cxa_finalize
                                static destructors, in reverse order
    __cxa_guard_acquire / release / abort
                                thread-safe function-local statics, over
                                the kernel's futex
    __cxa_pure_virtual          a pure virtual reached from a constructor
    __cxa_throw_bad_array_new_length
                                emitted for `new T[n]` even with
                                exceptions off
    __dso_handle                the token __cxa_atexit is passed
    std::terminate              where all of the above end up

`-fno-threadsafe-statics` would have removed the guard calls and is
deliberately not used: it removes the calls and leaves the race.

## Where the memory comes from

`operator new` calls `malloc`, which since descriptors arrived has an
mmap-backed path for anything of a quarter megabyte or more. So a large
`new` is a mapping of its own and `delete` gives the frames back rather
than parking them on a free list forever.

Going to `mmap` directly from `operator new` would be the other way to
satisfy "the memory operators are mmap-backed", and it would be worse:
two allocators over one address space, neither able to reuse what the
other freed, and a `new char[16]` costing a page.

## What happens when something fails

There is no unwinder. `throw` does not compile, `catch` is removed, and
the three ways out of a failure are to return a wrong value, to throw
anyway, or to stop.

    plain operator new, out of memory     prints and aborts
    new (std::nothrow), out of memory     returns null
    vector::at, string::at out of range   prints the index and aborts
    optional::value on an empty one       prints and aborts
    an empty std::function called         prints and aborts

Returning null from plain `new` is the dangerous option and the reason
it is not taken: code compiled against a throwing `new` does not check
the result — that is the whole contract — so null is not an error path,
it is a null dereference inside the constructor, reported as a fault in
the object rather than as running out of memory.

## The headers

    cstddef cstdint cstring cstdlib cstdio cmath cctype cassert
    cerrno climits cstdarg
    new initializer_list type_traits utility limits iterator
    algorithm memory functional ratio
    exception stdexcept
    array vector string string_view optional variant complex
    unordered_map (and unordered_set)
    atomic mutex condition_variable thread chrono
    typeinfo cxxabi.h cinttypes cfloat

`<initializer_list>` is the one whose contents the compiler already
knows: GCC constructs one itself for every braced list, by writing a
pointer and a length into an object of that exact layout. Getting it
wrong does not fail to compile — it produces a list whose length is a
pointer.

## Run-time type information

`-fno-rtti` was a decision here and is still the default for everything
compiled in this repository. It stopped being a decision that could be
kept for *ported* code when ICU arrived: ICU 74 uses `dynamic_cast` in
117 places and `typeid` in about forty, and unlike older versions it has
no fallback — `utypeinfo.h` includes `<typeinfo>` unconditionally. There
was no configuration of ICU without RTTI; there was only a choice
between RTTI and no ICU.

So `include/typeinfo`, `include/cxxabi.h` and `src/typeinfo.cpp` are the
part of `libsupc++` that RTTI needs. What that means concretely:

**The layouts are the ABI's, not ours.** The compiler emits a type
descriptor object into every `-frtti` translation unit that mentions a
type in a `typeid` or a `dynamic_cast`, and `cxxabi.h` describes how to
read one. A field in the wrong place there does not fail to compile — it
reads a base-class pointer out of the middle of a string.

**The vtables have to exist under their ABI names.** Those descriptors
point at `_ZTVN10__cxxabiv120__si_class_type_infoE` and its nine
siblings; each destructor in `src/typeinfo.cpp` is defined out of line
so that the compiler emits one there and exactly once.

**The search is written plainly.** `libsupc++` implements
`[expr.dynamic.cast]` as a single incremental pass that threads a
partial result through every recursion. `src/typeinfo.cpp` does the two
clauses of the rule literally instead — collect every subobject of the
target type, then ask of each whether the source sits publicly inside it
— which is slower on a diamond nobody has, and can be checked one
function at a time.

**And it is checked against a reference.** `apps/rtti_cases.h` is
compiled twice, once on the host against the host's own C++ runtime and
once in ring 3 against this one, with every expectation written as an
address the compiler works out statically. Both runs answer all 43 the
same way, including the case that catches the most plausible wrong
implementation: a base class present twice, where the target is reached
not through the source subobject but through the complete object.

`-fno-exceptions` is unaffected and stays. A `dynamic_cast` to a
*reference* is the one form that must throw on failure, so it is still
unavailable; `__cxa_bad_cast` prints and stops.

## What is not here

Stated rather than discovered, because the difference between "not
written yet" and "cannot work here" matters:

**Not written yet**, and ordinary additions to a runtime that now links:
`<map>`, `<set>`, `<deque>`, `<list>`, `<tuple>`, `<span>`, `<bit>`,
`<numeric>`, `<condition_variable>`, `<future>`, ranges.

**Cannot work here as the standard describes them**:

  - `<iostream>`. Wants locales and a stream hierarchy over a console
    that is one window; `<cstdio>` reaches the same terminal.
  - `<filesystem>`. Wants symbolic links, permissions and a working
    directory, none of which this volume or this kernel has. `<dirent.h>`
    and `<sys/stat.h>` describe what is actually there.
  - `<regex>`. Enormous, and nothing here needs it.
  - Anything throwing. `<stdexcept>` defines the types because ported
    code names them; constructing one works and throwing it does not
    compile.

## The four that clear the WebKit ladder

`third_party/wpe-config/README.md` named these as what the engine needs
beyond the first cut. Each is real rather than a shape:

**`<chrono>`** over `SYS_CLOCK`, with `<ratio>` underneath it — which
nobody asks for and duration arithmetic cannot be written without. Three
properties of the clock are stated in the header rather than left to be
assumed: the unit is nanoseconds, the resolution is a millisecond (the
scheduler's tick), and the epoch is boot. `steady_clock` and
`system_clock` are the same count and say so.

**`<thread>`** over `libc/pthread.c`, over `SYS_CLONE`. Destroying a
joinable thread calls `std::terminate`, as the standard requires — not a
silent detach and not a silent join, because the first leaves a thread
running into a dead scope and the second turns a missing `.join()` into
a deadlock under load. `hardware_concurrency()` answers **1**, and means
it: every ring-3 thread here runs on processor zero because the kernel
keeps one stack pointer for the next entry from user mode.

**`<unordered_map>`** with chained buckets rather than open addressing,
and the reason is a guarantee: the standard requires references to
elements to survive a rehash, and with open addressing the elements *are*
the array. Power-of-two buckets, so the index is a mask.

**`<variant>`** with a function-pointer table per operation, generated by
`index_sequence` and indexed by the live alternative — so visiting a
variant of ten costs what visiting one of two costs.
`valueless_by_exception()` is always false, and that is exact rather than
simplified: the state exists to describe a constructor that threw, and
nothing on this target can throw. Multi-variant `visit` is absent.

## Checked

    tools/cxx_test.cpp    218 checks on the host, against these headers
    apps/cxxtest.cpp      150 checks in ring 3, on the machine
    apps/rtti_cases.h      43 of those 150, compiled on both and
                              expected to agree

The split is not redundancy. The host test carries the volume a boot
test cannot afford — twenty thousand elements sorted in five adversarial
orders, every string length from 0 to 199, tracked objects counted for
leaks — and a failure there is a failure of the library, findable in a
second rather than a boot.

What it cannot check is everything that is not computation: that
`operator new` reaches the ring-3 allocator, that `crt0` walks
`.init_array` and `exit()` walks the destructors back, that a
function-local static is constructed exactly once with four threads
racing for it, and that a vtable survives a loader which maps an image
page by page under W^X. That is the machine test, and it is the reason
both exist.
