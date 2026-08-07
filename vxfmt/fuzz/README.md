# Fuzzing `vx_validate()`

`vx_validate()` is the only thing between a `.vx` payload downloaded over
plain HTTP by the store and a loader that runs it with full kernel
privileges in a shared address space. This fuzzes exactly that function
and nothing around it.

## Build

```sh
make            # both binaries
make check-seeds
```

`harness` is instrumented (`afl-clang-fast`, `AFL_USE_ASAN=1`, `-g -O1`)
and is what `afl-fuzz` drives. `harness-repl` is ASan-only with no
instrumentation, for replaying a crash and reading the report without the
forkserver in the way.

## Run

```sh
afl-fuzz -i in -o out -m none -- ./harness @@
```

`-m none` because ASan reserves an enormous amount of address space and
AFL's default memory limit kills the target before it starts. `@@` is the
input path; the harness takes a file, not stdin.

## What the harness deliberately does not do

`vx_run.c` does a great deal after validating: `mmap`, `memcpy`,
`mprotect`, and then it **calls the image's entry point**. A fuzzer that
went through `vx_run`'s `main()` would be executing attacker-controlled
bytes rather than testing the check meant to stop them. The harness calls
`vx_validate()` directly and returns.

## Two things found while reading, not fixed here

**1. `vx_run.c` over-reads on files shorter than the header.** It does

```c
unsigned char *file = malloc((size_t)file_size);
...
vx_header_t h;
memcpy(&h, file, sizeof(h));          /* 80 bytes */
const char *bad = vx_validate(&h, file_size);
```

The buffer is exactly `file_size` bytes and the copy is unconditionally
80, *before* anything is validated. Any file of 1..79 bytes is a heap
over-read of up to 79 bytes. ASan flags it immediately.

The kernel's loader does not have this bug — `load_vx_image()` in
`src/desktop.h` tests `fsize < sizeof(h)` and returns before copying.
The two loaders disagree, and the POSIX one is the wrong one.

This is why the harness copies `min(file_size, 80)` into a zero-filled
header: so that `vx_validate`'s own short-file branch can be fuzzed
without the harness manufacturing the very over-read it is looking for.

**2. The import table is not covered by `vx_validate()` at all.**
`vx_resolve_imports()` in `src/desktop.h` scans the *loaded image* for
`VX_IMPORT_MAGIC`, reads a `count` out of it and walks that many
32-byte entries. `vx_validate()` never sees that structure — it validates
the 80-byte header and stops. The bound checks on `count` live in the
resolver, so they are outside everything this harness exercises.

That is a second fuzz target, not a second seed corpus: it needs a
harness that calls `vx_resolve_imports()` on a buffer, and that function
currently lives in the kernel header rather than in `vxfmt/`.
