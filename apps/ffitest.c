/*
 * ffitest — libffi in ring 3.
 *
 * The seventeenth port, and the second in a row that WebKit's configure
 * never asks for. libffi is **GObject's** dependency: GLib 2.74's
 * meson.build:2102 makes `libffi >= 3.0.0` required, and
 * `gobject/gclosure.c` is the only file in the whole of GLib that uses
 * it.
 *
 * ---- half a library, and the missing half is the interesting one ----
 *
 * libffi does two separable things, and this system can do one of them.
 *
 * **Calling** — `ffi_prep_cif` and `ffi_call` — builds a description of
 * a function's signature at run time, marshals arguments into the
 * registers and stack slots the ABI names, and calls code that was
 * already executable. Nothing about it needs a writable code page, and
 * it is what every section below exercises.
 *
 * **Closures** — `ffi_closure_alloc` and `ffi_prep_closure_loc` —
 * manufacture a *new* function pointer, which means writing machine
 * code into memory and jumping to it. This kernel refuses that by
 * name: `src/desktop.h:2449` rejects any mapping asking for
 * PROT_WRITE and PROT_EXEC together, and `libc/include/sys/mman.h`
 * states the policy — every page writable or executable, never both,
 * with no sequence of ring-3 calls that arrives at one which is both.
 *
 * So `src/closures.c` and `src/tramp.c` are not compiled, and
 * `ffi_closure_alloc` is **absent from the archive**. A program that
 * wants one fails at link, naming the symbol, rather than faulting
 * later inside a heap buffer it wrote a trampoline into. That is the
 * same choice `third_party/libepoxy-port/vxgl.c` makes about GL entry
 * points it cannot serve.
 *
 * There is one wrinkle worth knowing and it is upstream's, not this
 * port's: `ffitarget.h` defines `FFI_CLOSURES 1` on x86-64
 * unconditionally, because it describes what the *architecture*
 * supports rather than what the archive contains. A consumer that tests
 * `#if FFI_CLOSURES` will compile and then fail to link. Section 1
 * asserts that state rather than hiding it.
 *
 * ---- and what GLib actually needs ----
 *
 * Measured, not hoped. The libffi entry points named anywhere in GLib
 * are `ffi_prep_cif`, `ffi_call` and the `ffi_type_*` descriptors.
 * `g_cclosure_marshal_generic` builds a call *into* a C function out of
 * a GValue array; it never manufactures a function pointer. The half
 * this system can build is exactly the half GLib asks for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ffi.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/* ---- the functions called through ffi_call ---- */

static int add_ints(int a, int b, int c) { return a + b + c; }

static double mix(int i, double d, int j, double e) {
    return d * i + e * j;
}

/* Ten integers and ten doubles: x86-64 SysV passes the first six
 * integers in registers and the first eight floats in SSE registers,
 * so everything after that goes on the stack. This is the function that
 * fails if the stack half of the marshaller is wrong. */
static long many(long a, long b, long c, long d, long e, long f,
                 long g, long h, long i, long j,
                 double p, double q, double r, double s, double t,
                 double u, double v, double w, double x, double y) {
    return (a + b + c + d + e + f + g + h + i + j)
         + (long)(p + q + r + s + t + u + v + w + x + y);
}

/* Structures, and the SysV classification algorithm that decides where
 * each one travels. These four cover the cases that matter. */
struct two_doubles { double x, y; };          /* SSE, SSE   */
struct int_ptr     { int a; void *p; };       /* INTEGER x2 */
struct mixed       { int i; double d; };      /* INTEGER, SSE */
struct big         { char b[64]; };           /* MEMORY     */

static struct two_doubles scale2(struct two_doubles v, double k) {
    struct two_doubles out;
    out.x = v.x * k;
    out.y = v.y * k;
    return out;
}

static int use_int_ptr(struct int_ptr v) {
    return v.a + (int)*(const char *)v.p;
}

static double use_mixed(struct mixed m) { return m.i + m.d; }

static struct big fill_big(char c) {
    struct big b;
    memset(b.b, c, sizeof b.b);
    return b;
}

static int sum_big(struct big b) {
    int i, n = 0;
    for (i = 0; i < (int)sizeof b.b; i++) n += b.b[i];
    return n;
}

int main(void) {
    printf("ffitest: libffi %s\n", ffi_get_version());

    /* ============================================================
     *  1. what the archive is, and what it is not
     * ============================================================ */
    {
        check("the version string is 3.5.2",
              strcmp(ffi_get_version(), "3.5.2") == 0);
        check("and the number agrees",
              ffi_get_version_number() == 30502);
        check("the header was generated for X86_64",
              FFI_DEFAULT_ABI == FFI_UNIX64);

        /* The wrinkle described at the head of this file: ffitarget.h
         * says the architecture supports closures, which is true, while
         * this archive does not contain the allocator, which is also
         * true. Asserting it keeps the state visible rather than
         * surprising. */
#if FFI_CLOSURES
        check("ffitarget.h claims closures, describing the architecture", 1);
#else
        check("ffitarget.h claims closures, describing the architecture", 0);
#endif
        printf("       closures are compiled out: ffi_closure_alloc is not "
               "in libffi.a\n");

        /* No trampoline table on x86-64 — the one strategy that would
         * have fitted this kernel's W^X rule, and upstream implements it
         * for aarch64 and arm on Darwin only. */
#if FFI_EXEC_TRAMPOLINE_TABLE
        check("no static trampoline table on x86-64", 0);
#else
        check("no static trampoline table on x86-64", 1);
#endif
    }

    /* ============================================================
     *  2. integers, which is the simplest call there is
     * ============================================================ */
    {
        ffi_cif cif;
        ffi_type *args[3];
        void *values[3];
        int a = 3, b = 4, c = 5;
        ffi_arg rc;
        ffi_status st;

        args[0] = args[1] = args[2] = &ffi_type_sint;
        values[0] = &a; values[1] = &b; values[2] = &c;

        st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_sint, args);
        checkr("ffi_prep_cif for three ints", st == FFI_OK, st);
        check("the cif reports three arguments", cif.nargs == 3);
        check("and the ABI it was prepared for", cif.abi == FFI_DEFAULT_ABI);

        ffi_call(&cif, FFI_FN(add_ints), &rc, values);
        checkr("ffi_call returns their sum", (int)rc == 12, (int)rc);

        /* The return slot for a type narrower than a word is an
         * ffi_arg, not an int — libffi widens on the way out, and a
         * caller that passed an `int*` would be writing four bytes into
         * a place the callee treats as eight. */
        check("the return value was widened to ffi_arg",
              sizeof(ffi_arg) == 8);
    }

    /* ============================================================
     *  3. integers and floats together, which is the classification
     * ============================================================
     *
     * On x86-64 SysV, integers travel in rdi/rsi/rdx/rcx/r8/r9 and
     * floating point in xmm0..7 — two independent sequences. So
     * `mix(int, double, int, double)` puts its arguments in rdi, xmm0,
     * rsi, xmm1, and a marshaller that walked one register file would
     * get the right answer for all-int and all-double signatures and
     * the wrong one here.
     */
    {
        ffi_cif cif;
        ffi_type *args[4];
        void *values[4];
        int i = 3, j = 5;
        double d = 2.5, e = 1.5, result = 0;

        args[0] = &ffi_type_sint;   values[0] = &i;
        args[1] = &ffi_type_double; values[1] = &d;
        args[2] = &ffi_type_sint;   values[2] = &j;
        args[3] = &ffi_type_double; values[3] = &e;

        check("a mixed signature prepares",
              ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_double,
                           args) == FFI_OK);
        ffi_call(&cif, FFI_FN(mix), &result, values);
        check("and 2.5*3 + 1.5*5 is 15", result == 15.0);
    }

    /* ============================================================
     *  4. twenty arguments, so that the stack half runs
     * ============================================================
     *
     * Six integer registers and eight SSE registers, then the stack.
     * Ten of each means four integers and two doubles are pushed, and
     * the alignment of that area is the ABI detail most easily got
     * wrong.
     */
    {
        ffi_cif cif;
        ffi_type *args[20];
        void *values[20];
        long ints[10];
        double dbls[10];
        ffi_arg rc = 0;
        int k;

        for (k = 0; k < 10; k++) {
            ints[k] = k + 1;                 /* 1..10, sum 55 */
            dbls[k] = (double)(k + 1) * 10;  /* 10..100, sum 550 */
            args[k] = &ffi_type_slong;      values[k] = &ints[k];
            args[k + 10] = &ffi_type_double; values[k + 10] = &dbls[k];
        }

        check("a twenty-argument signature prepares",
              ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 20, &ffi_type_slong,
                           args) == FFI_OK);
        ffi_call(&cif, FFI_FN(many), &rc, values);
        checkr("and every one of them arrives", (long)rc == 605, (int)rc);
    }

    /* ============================================================
     *  5. structures by value, which is where the ABI gets hard
     * ============================================================
     *
     * The SysV classification algorithm sorts each eightbyte of a
     * structure into INTEGER, SSE or MEMORY and passes it accordingly,
     * and libffi reimplements that algorithm in src/x86/ffi64.c. These
     * four structures are the cases it has to get right, and each fails
     * differently: the wrong class puts a double in rdi, or splits a
     * structure that should have gone in memory, or forgets the hidden
     * return pointer.
     */
    {
        ffi_cif cif;
        ffi_type *args[2];
        void *values[2];

        /* Two doubles: SSE + SSE, so it travels in xmm0 and xmm1 both
         * ways, including the return. */
        {
            ffi_type td, *elems[3];
            struct two_doubles in = { 1.5, 2.5 }, out = { 0, 0 };
            double k = 4.0;

            elems[0] = elems[1] = &ffi_type_double;
            elems[2] = NULL;
            td.size = td.alignment = 0;
            td.type = FFI_TYPE_STRUCT;
            td.elements = elems;

            args[0] = &td;              values[0] = &in;
            args[1] = &ffi_type_double; values[1] = &k;

            check("a two-double structure prepares",
                  ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &td, args)
                      == FFI_OK);
            check("and libffi computed its size", td.size == 16);
            ffi_call(&cif, FFI_FN(scale2), &out, values);
            check("both members come back scaled",
                  out.x == 6.0 && out.y == 10.0);
        }

        /* An int and a pointer: one eightbyte each, both INTEGER. */
        {
            ffi_type td, *elems[3];
            struct int_ptr in;
            static const char ch = 7;
            ffi_arg rc = 0;

            in.a = 35;
            in.p = (void *)&ch;

            elems[0] = &ffi_type_sint;
            elems[1] = &ffi_type_pointer;
            elems[2] = NULL;
            td.size = td.alignment = 0;
            td.type = FFI_TYPE_STRUCT;
            td.elements = elems;

            args[0] = &td; values[0] = &in;
            check("an int-and-pointer structure prepares",
                  ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint,
                               args) == FFI_OK);
            check("padded to sixteen bytes", td.size == 16);
            ffi_call(&cif, FFI_FN(use_int_ptr), &rc, values);
            checkr("and both members arrive", (int)rc == 42, (int)rc);
        }

        /* An int and a double: INTEGER then SSE, which is the mixed
         * case — the first eightbyte in rdi, the second in xmm0. */
        {
            ffi_type td, *elems[3];
            struct mixed in;
            double out = 0;

            in.i = 10; in.d = 0.25;
            elems[0] = &ffi_type_sint;
            elems[1] = &ffi_type_double;
            elems[2] = NULL;
            td.size = td.alignment = 0;
            td.type = FFI_TYPE_STRUCT;
            td.elements = elems;

            args[0] = &td; values[0] = &in;
            check("a mixed-class structure prepares",
                  ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_double,
                               args) == FFI_OK);
            ffi_call(&cif, FFI_FN(use_mixed), &out, values);
            check("and both halves arrive in their own register file",
                  out == 10.25);
        }

        /* Sixty-four bytes: too big for registers, so it is MEMORY —
         * passed on the stack, and *returned* through a hidden pointer
         * in rdi that shifts every other argument along one. */
        {
            ffi_type td, *elems[65];
            struct big out;
            char c = 3;
            int k;
            ffi_arg rc = 0;

            for (k = 0; k < 64; k++) elems[k] = &ffi_type_schar;
            elems[64] = NULL;
            td.size = td.alignment = 0;
            td.type = FFI_TYPE_STRUCT;
            td.elements = elems;

            args[0] = &ffi_type_schar; values[0] = &c;
            check("a 64-byte structure prepares as a return type",
                  ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &td, args)
                      == FFI_OK);
            check("with the size it really is", td.size == 64);
            memset(&out, 0, sizeof out);
            ffi_call(&cif, FFI_FN(fill_big), &out, values);
            check("and comes back through the hidden pointer",
                  out.b[0] == 3 && out.b[63] == 3);

            /* And the same structure going the other way. */
            args[0] = &td; values[0] = &out;
            check("it prepares as an argument too",
                  ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint,
                               args) == FFI_OK);
            ffi_call(&cif, FFI_FN(sum_big), &rc, values);
            checkr("and all 64 bytes arrive", (int)rc == 3 * 64, (int)rc);
        }
    }

    /* ============================================================
     *  6. a variadic function, called through ffi_prep_cif_var
     * ============================================================
     *
     * The variadic ABI is not the same as the fixed one — on x86-64 al
     * must hold the number of SSE registers used — so libffi has a
     * separate preparation call for it, and the callee here is a real
     * variadic function out of this C library rather than a stand-in.
     */
    {
        ffi_cif cif;
        ffi_type *args[5];
        void *values[5];
        char buf[64];
        char *bufp = buf;
        size_t cap = sizeof buf;
        const char *fmt = "%s=%d";
        const char *name = "answer";
        int n = 42;
        ffi_arg rc = 0;

        args[0] = &ffi_type_pointer; values[0] = &bufp;
        args[1] = &ffi_type_uint64;  values[1] = &cap;
        args[2] = &ffi_type_pointer; values[2] = &fmt;
        args[3] = &ffi_type_pointer; values[3] = &name;
        args[4] = &ffi_type_sint;    values[4] = &n;

        check("a variadic signature prepares",
              ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 3, 5,
                               &ffi_type_sint, args) == FFI_OK);
        memset(buf, 0, sizeof buf);
        ffi_call(&cif, FFI_FN(snprintf), &rc, values);
        checkr("snprintf, called through libffi, returns 9",
               (int)rc == 9, (int)rc);
        check("and formatted its arguments",
              strcmp(buf, "answer=42") == 0);
    }

    /* ============================================================
     *  7. what a wrong cif is answered with
     * ============================================================ */
    {
        ffi_cif cif;
        ffi_type *args[1];
        ffi_type bad;
        ffi_status st;

        args[0] = &ffi_type_sint;
        st = ffi_prep_cif(&cif, (ffi_abi)999, 1, &ffi_type_sint, args);
        checkr("an ABI that does not exist is refused",
               st == FFI_BAD_ABI, st);

        /* A structure type whose element list is empty is not a
         * structure, and libffi says so rather than computing a size of
         * zero and marshalling nothing. */
        {
            ffi_type *elems[1];
            elems[0] = NULL;
            bad.size = bad.alignment = 0;
            bad.type = FFI_TYPE_STRUCT;
            bad.elements = elems;
            args[0] = &bad;
            st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint,
                              args);
            checkr("and an empty structure type is refused",
                   st == FFI_BAD_TYPEDEF, st);
        }
    }

    printf("ffitest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
