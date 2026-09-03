/*
 * third_party/libffi-port/fficonfig.h — libffi's build configuration,
 * decided here instead of by ./configure.
 *
 * Like PCRE2 beside it, libffi is not on WebKit's list. It is on
 * **GObject's**: GLib 2.74's meson.build:2102 makes `libffi >= 3.0.0` a
 * required dependency, and `gobject/gclosure.c` is the one file in all
 * of GLib that uses it.
 *
 * ================================================================
 * half of libffi is built here, and the missing half is the point
 * ================================================================
 *
 * libffi does two separable things.
 *
 *   **Calling**, which is `ffi_prep_cif` and `ffi_call`: build a
 *   description of a function's signature at run time, marshal
 *   arguments into registers and onto the stack according to the ABI,
 *   and call it. This is pure computation over memory that is already
 *   executable — the function being called is ordinary compiled code.
 *
 *   **Closures**, which are `ffi_closure_alloc` and
 *   `ffi_prep_closure_loc`: synthesise a *new* function pointer at run
 *   time, so that C code holding a plain function pointer ends up in
 *   your dispatcher. Doing that means writing machine code into memory
 *   and then jumping to it.
 *
 * **The second is impossible on this system, and not by omission.**
 * `src/desktop.h:2449` refuses any mmap or mprotect that asks for
 * PROT_WRITE and PROT_EXEC together, by name, and
 * `libc/include/sys/mman.h` states the policy in full: every page of
 * every program is writable or executable and never both, and there is
 * no sequence of calls from ring 3 that arrives at a page which is
 * both. That note already names the first casualty — WebKit's JIT tiers
 * — and PCRE2's JIT was the second. libffi's closures are the third.
 *
 * libffi offers exactly three ways to get executable memory
 * (`src/closures.c:126, 161, 392`) and this kernel refuses all three:
 *
 *   the default          `ffi_closure_alloc` is malloc, and the
 *                        trampoline is written into the heap, which is
 *                        mapped NX.
 *   FFI_MMAP_EXEC_WRIT   maps one region twice, once writable and once
 *                        executable. Needs two virtual addresses over
 *                        one physical page, which an anonymous-only
 *                        mmap cannot express.
 *   FFI_EXEC_TRAMPOLINE_TABLE
 *                        static trampolines in a read-only executable
 *                        page with per-closure data beside them. This
 *                        one *would* fit W^X — it never writes to
 *                        executable memory — but upstream implements it
 *                        for aarch64 and arm on Darwin only. There is no
 *                        x86-64 implementation to enable.
 *
 * ---- so closures.c and tramp.c are not compiled ----
 *
 * That is deliberate, and it is not a stub. Compiling `closures.c` in
 * its default form would produce an `ffi_closure_alloc` that returns
 * heap memory, an `ffi_prep_closure_loc` that writes a trampoline into
 * it, and a fault at the moment something called through the result —
 * a page fault at an address in the middle of a buffer, with nothing to
 * connect it to this decision. Leaving the two objects out means
 * `ffi_closure_alloc` is absent from the archive and anything that wants
 * it fails at *link* time, naming the symbol. That is the same choice
 * `third_party/libepoxy-port/vxgl.c` makes about GL entry points it
 * cannot serve, and for the same reason.
 *
 * ---- and GLib does not need them ----
 *
 * Measured rather than hoped: `gobject/gclosure.c` is the only file in
 * GLib that mentions libffi at all, and the entry points it uses are
 * `ffi_prep_cif`, `ffi_call` and the `ffi_type_*` descriptors. There is
 * no `ffi_prep_closure_loc` and no `ffi_closure_alloc` anywhere in
 * GLib. `g_cclosure_marshal_generic` builds a call *into* a C function
 * from a GValue array; it never has to manufacture a function pointer.
 *
 * So the half of libffi this system can build is exactly the half GLib
 * asks for. Whether some later consumer needs the other half is a
 * separate question with a real answer — an x86-64
 * FFI_EXEC_TRAMPOLINE_TABLE, which is upstream work — and it is written
 * down here rather than discovered.
 *
 * ================================================================
 * the platform answers
 * ================================================================
 */

#ifndef VEXTRO_FFICONFIG_H
#define VEXTRO_FFICONFIG_H

#define PACKAGE           "libffi"
#define PACKAGE_NAME      "libffi"
#define PACKAGE_TARNAME   "libffi"
#define PACKAGE_VERSION   "3.5.2"
#define PACKAGE_STRING    "libffi 3.5.2"
#define PACKAGE_BUGREPORT "http://github.com/libffi/libffi/issues"
#define PACKAGE_URL       ""
#define VERSION           "3.5.2"

/*
 * alloca, which libffi's x86-64 backend leans on hard: ffi_call builds
 * the outgoing register file and stack frame with it (src/x86/ffi64.c
 * lines 582, 591, 700, 831), so it has to be the *real* stack-moving
 * one rather than a heap allocation.
 *
 * This C library has no <alloca.h>, which is what autoconf's
 * AC_FUNC_ALLOCA normally probes for. The block below is that macro's
 * own fallback, and on GCC it is the better answer anyway:
 * __builtin_alloca compiles to an adjustment of the stack pointer with
 * no call at all.
 */
#ifdef __GNUC__
# define alloca __builtin_alloca
#endif
#define HAVE_ALLOCA 1

/* ---- type sizes, which decide argument classification ---- */
#define SIZEOF_SIZE_T        8
#define SIZEOF_DOUBLE        8
#define SIZEOF_LONG_DOUBLE   16
#define HAVE_LONG_DOUBLE     1
#define HAVE_LONG_DOUBLE_VARIANT 0

/* x86-64 stacks grow down. Read by src/closures.c and the raw API. */
#define STACK_DIRECTION -1

/*
 * What the assembler will accept. Both are true of the binutils in this
 * cross toolchain, and both matter to src/x86/unix64.S: without the CFI
 * pseudo-ops the unwinder has nothing to walk, and without PC-relative
 * addressing the jump table at the end of ffi_call_unix64 has to be
 * written a longer way.
 */
#define HAVE_AS_CFI_PSEUDO_OP 1
#define HAVE_AS_X86_PCREL     1

/* The section flags for .eh_frame. "a" is allocatable and not writable,
 * which is what a read-only unwind table wants and what this system's
 * W^X mapping expects to see. */
#define EH_FRAME_FLAGS "a"

#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1

/* ---- headers this C library has ---- */
#define HAVE_STDIO_H   1
#define HAVE_STDLIB_H  1
#define HAVE_STRING_H  1
#define HAVE_STRINGS_H 1
#define HAVE_STDINT_H  1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H  1
#define HAVE_MEMCPY    1
#define STDC_HEADERS   1

/*
 * ---- not defined, and the first three are the whole story above ----
 *
 *   FFI_MMAP_EXEC_WRIT       needs one physical page at two virtual
 *                            addresses; mmap here is anonymous-only
 *   FFI_EXEC_TRAMPOLINE_TABLE
 *                            would fit W^X and has no x86-64
 *                            implementation upstream
 *   FFI_EXEC_STATIC_TRAMP    memfd_create and a double mmap, Linux-only
 *
 *   HAVE_MMAP, HAVE_MMAP_ANON, HAVE_MMAP_DEV_ZERO, HAVE_MMAP_FILE
 *                            read only by closures.c, which is not
 *                            compiled. This system *does* have an
 *                            anonymous mmap; the defines are left out
 *                            because the file that reads them is not in
 *                            the archive, and a define describing code
 *                            nobody builds is a claim with nothing
 *                            behind it.
 *
 *   HAVE_DLFCN_H             there is no dynamic linker here
 *   HAVE_MEMFD_CREATE        Linux
 *   FFI_DEBUG                assertions and tracing; off, like every
 *                            other port here
 *   SYMBOL_UNDERSCORE        ELF, so no leading underscore
 *   HAVE_RO_EH_FRAME         a configure probe for whether .eh_frame
 *                            ends up read-only; read only by closures.c
 */

/*
 * ================================================================
 * upstream's AH_BOTTOM, copied verbatim
 * ================================================================
 *
 * configure.ac:327-346 appends this block to every fficonfig.h autoconf
 * generates, so a hand-written one has to carry it: FFI_HIDDEN is used
 * on the *declaration* of half a dozen internal functions
 * (include/ffi_common.h:144-152) and on their definitions, and it is
 * defined nowhere else in the tree. Leaving it out is not a missing
 * optimisation — it is `ffi_status FFI_HIDDEN ffi_prep_cif_core(...)`
 * parsing as two identifiers, which is what the first attempt at this
 * port produced.
 *
 * It is reproduced rather than simplified, LIBFFI_ASM branch and all,
 * because the .S files include this header too and need the assembler
 * spelling of the same idea.
 */
#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
#ifdef LIBFFI_ASM
#ifdef __APPLE__
#define FFI_HIDDEN(name) .private_extern name
#else
#define FFI_HIDDEN(name) .hidden name
#endif
#else
#define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
#endif
#else
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif
#endif

#endif /* VEXTRO_FFICONFIG_H */
