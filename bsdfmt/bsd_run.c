/*
 * bsd_run — load and execute a .bsd image on a POSIX host.
 *
 * The interesting part is the memory protection dance.  You cannot just
 * malloc() a buffer, copy machine code into it and jump: on any modern
 * x86_64 CPU the heap is mapped with the NX bit set, so the first
 * instruction fetch faults and the process dies with SIGSEGV.  The
 * kernel has to be told that those pages are executable, and it will
 * only do that at page granularity.
 *
 * So:
 *
 *   1. mmap() one anonymous, page-aligned region spanning the whole
 *      image, RW- to begin with.  Anonymous pages come back zeroed,
 *      which is exactly what .bss wants.
 *   2. Copy the text and data images in at their relative offsets.
 *   3. mprotect() the text pages R-X and the data pages RW-.  Neither
 *      is ever W and X at the same time, so W^X holds throughout.
 *   4. Cast the entry address to a function pointer and call it.
 *
 * Entry ABI (SysV AMD64):  long entry(long arg)
 *   arg comes in RDI, the result comes back in RAX.  The image is
 *   freestanding — it must not assume a libc, a TLS block or a heap.
 *
 * Build for the host you intend to run on: the payload is x86_64
 * machine code, so an x86_64 kernel and CPU are required to execute it.
 * Use -n to inspect and map an image without running it, which works
 * anywhere.
 */
/* MAP_ANONYMOUS is a BSD extension that strict POSIX hides, and the two
 * libcs spell the unlock differently. */
#if defined(__linux__)
#  define _GNU_SOURCE 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#  define _DARWIN_C_SOURCE 1
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsd_format.h"

#ifndef MAP_ANONYMOUS
#  ifdef MAP_ANON
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

/* The 0x64 in the magic means x86_64, and the payload really is raw
 * machine code for that ISA.  Jumping to it anywhere else earns a
 * SIGILL, so say what went wrong instead of letting the CPU say it. */
#if defined(__x86_64__) || defined(_M_X64)
#  define BSD_HOST_IS_X86_64 1
#else
#  define BSD_HOST_IS_X86_64 0
#endif

typedef long (*bsd_entry_fn)(long);

static void die(const char *msg) {
    fprintf(stderr, "bsd_run: %s\n", msg);
    exit(1);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "bsd_run: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void usage(FILE *out) {
    fputs("usage: bsd_run [-n] [-a ARG] [-q] IMAGE.bsd\n"
          "  -n   map and protect the image but do not call it\n"
          "  -a   integer argument passed to the entry point in RDI\n"
          "  -q   print only the return value\n", out);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long arg = 0;
    int no_exec = 0, quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) no_exec = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-a")) {
            if (++i >= argc) die("-a needs a value");
            arg = strtol(argv[i], NULL, 0);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            usage(stderr);
            die("unknown option");
        } else {
            path = argv[i];
        }
    }
    if (!path) { usage(stderr); die("no image given"); }

    /* ---- read the image ---- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) die_errno(path);

    struct stat st;
    if (fstat(fd, &st) != 0) die_errno("fstat");
    if (st.st_size <= 0) die("empty file");
    uint64_t file_size = (uint64_t)st.st_size;

    unsigned char *file = malloc((size_t)file_size);
    if (!file) die("out of memory");
    {
        size_t got = 0;
        while (got < file_size) {
            ssize_t n = read(fd, file + got, (size_t)file_size - got);
            if (n < 0) die_errno("read");
            if (n == 0) die("short read");
            got += (size_t)n;
        }
    }
    close(fd);

    /* ---- validate before trusting a single field ---- */
    bsd_header_t h;
    memcpy(&h, file, sizeof(h));

    const char *bad = bsd_validate(&h, file_size);
    if (bad) {
        fprintf(stderr, "bsd_run: %s: %s\n", path, bad);
        return 1;
    }

    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) die("cannot determine the page size");
    uint64_t host_pg = (uint64_t)pgsz;

    uint64_t base_vaddr = h.text_vaddr;
    uint64_t span = bsd_round_up(bsd_image_span(&h), host_pg);

    int has_data = (h.data_size || h.bss_size);
    uint64_t text_at   = h.text_vaddr - base_vaddr;        /* always 0 */
    uint64_t text_prot = bsd_round_up(h.text_size, host_pg);
    uint64_t data_at   = has_data ? h.data_vaddr - base_vaddr : 0;

    /*
     * Protection has host-page granularity.  The image is laid out for
     * 4 KB pages, which is what every x86_64 system uses; on a host with
     * coarser pages (16 KB on Apple silicon, 64 KB on some ppc64/arm64
     * kernels) the two segments can land in the same host page, and then
     * there is no way to give the text PROT_EXEC without handing it to
     * the data as well.  Rounding the text length up and calling
     * mprotect() anyway would "succeed" while quietly making the data
     * segment executable and read-only, which is both a W^X violation
     * and a fault waiting to happen on the first store.
     */
    int wx_possible = !has_data ||
                      (data_at % host_pg == 0 && text_prot <= data_at);

    if (!wx_possible && !no_exec) {
        fprintf(stderr,
                "bsd_run: %s: host pages are %llu bytes but the segments "
                "are %llu bytes apart,\n"
                "         so text and data share a page and W^X cannot be "
                "honoured. Run this\n"
                "         image on a host with %u-byte pages (any x86_64 "
                "system), or use -n to inspect it.\n",
                path, (unsigned long long)host_pg,
                (unsigned long long)data_at, BSD_PAGE_SIZE);
        free(file);
        return 1;
    }

    /* ---- 1. one RW anonymous mapping for the whole image ---- */
    void *base = mmap(NULL, (size_t)span, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) die_errno("mmap");

    unsigned char *mem = (unsigned char *)base;

    /* ---- 2. copy the segments in; .bss is already zero ---- */
    memcpy(mem + text_at, file + h.text_off, (size_t)h.text_size);
    if (h.data_size)
        memcpy(mem + data_at, file + h.data_off, (size_t)h.data_size);

    /* ---- 3. W^X: text becomes R-X, data stays RW- ---- */
    if (wx_possible) {
        if (mprotect(mem + text_at, (size_t)text_prot,
                     PROT_READ | PROT_EXEC) != 0)
            die_errno("mprotect(text, PROT_READ|PROT_EXEC)");

        if (has_data) {
            uint64_t data_prot =
                bsd_round_up(h.data_size + h.bss_size, host_pg);
            if (mprotect(mem + data_at, (size_t)data_prot,
                         PROT_READ | PROT_WRITE) != 0)
                die_errno("mprotect(data, PROT_READ|PROT_WRITE)");
        }
    }

    bsd_entry_fn entry;
    /* POSIX guarantees this cast; a union keeps -Wpedantic quiet about
     * ISO C's stance on object-to-function pointer conversions. */
    union { void *obj; bsd_entry_fn fn; } cast;
    cast.obj = mem + (h.entry - base_vaddr);
    entry = cast.fn;

    if (!quiet) {
        printf("bsd_run: %s\n", path);
        printf("  mapped %llu bytes at %p (host page %llu)\n",
               (unsigned long long)span, base,
               (unsigned long long)host_pg);
        printf("  text   %llu bytes at %p  %s\n",
               (unsigned long long)h.text_size, (void *)(mem + text_at),
               wx_possible ? "R-X" : "RW- (unprotected)");
        if (has_data)
            printf("  data   %llu bytes (+%llu bss) at %p  %s\n",
                   (unsigned long long)h.data_size,
                   (unsigned long long)h.bss_size, (void *)(mem + data_at),
                   wx_possible ? "RW-" : "RW- (unprotected)");
        printf("  entry  %p\n", (void *)entry);
        if (!wx_possible)
            printf("  warning: host pages are %llu bytes, so the segments "
                   "share one; protections not applied (inspection only)\n",
                   (unsigned long long)host_pg);
    }

    if (no_exec) {
        if (!quiet) printf("  -n given, not calling the entry point\n");
        munmap(base, (size_t)span);
        free(file);
        return 0;
    }

    if (!BSD_HOST_IS_X86_64) {
        fprintf(stderr,
                "bsd_run: %s holds x86_64 machine code, but this bsd_run was "
                "built for another\n"
                "         architecture. Run it on an x86_64 host, or use -n "
                "to map and inspect it here.\n", path);
        munmap(base, (size_t)span);
        free(file);
        return 1;
    }

    long rc = entry(arg);

    if (quiet) printf("%ld\n", rc);
    else       printf("  entry(%ld) returned %ld\n", arg, rc);

    munmap(base, (size_t)span);
    free(file);
    return 0;
}
