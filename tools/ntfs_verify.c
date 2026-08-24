/*
 * tools/ntfs_verify.c — the volume the build produced, read by the
 * driver that will boot from it.
 *
 * tools/mkntfs.py writes an NTFS volume. src/fs/ntfs/ntfs_ops.c reads
 * one. They were written from the same specification but they are not
 * the same code, in the same language, by the same route -- and the
 * only thing that makes a formatter trustworthy is a *different*
 * implementation agreeing about what it produced.
 *
 * So this mounts the real 8 GB disk.img with the kernel's driver, on
 * the host, and checks every seeded file byte for byte against the
 * source it came from. It is the check that has to pass before the
 * machine boots from the thing, because the failure mode on the other
 * side is a system that mounts, lists its files, and hands back the
 * wrong bytes for one of them.
 *
 * Three properties, in order of how badly they fail:
 *
 *   - every file resolves by path, and its size matches the source
 *   - its bytes match, read in windows through ntfs_read_range, which
 *     is the path a 937 MB archive actually uses
 *   - pagefile.sys is exactly one extent, which is what src/swap.h
 *     requires and cannot check for itself until it is far too late
 *
 * Read-only: it opens the image, reads, and never writes. A verifier
 * that mutated the volume it was verifying would be checking something
 * other than what ships.
 */

#define NTFS_HOST_TEST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fs/ntfs/ntfs_ops.c"

static int checks = 0, fails = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

/* Compare a file on the volume against a file on the host, in windows,
 * through the ranged read -- the same call the ZIM reader makes. */
static int compare(const char *vpath, const char *hostpath) {
    static uint8_t a[256 * 1024], b[256 * 1024];
    uint64_t rec = 0, vsize = 0, off = 0;
    int is_dir = 1;
    FILE *h;
    long hsize;

    if (ntfs_lookup(vpath, &rec, 0, &is_dir, &vsize) != 0 || is_dir) {
        printf("  FAIL %s: does not resolve on the volume\n", vpath);
        return 0;
    }
    h = fopen(hostpath, "rb");
    if (!h) { printf("  FAIL %s: host source missing\n", hostpath); return 0; }
    fseek(h, 0, SEEK_END); hsize = ftell(h); rewind(h);

    if ((uint64_t)hsize != vsize) {
        printf("  FAIL %s: size %llu on the volume, %ld on the host\n",
               vpath, (unsigned long long)vsize, hsize);
        fclose(h);
        return 0;
    }

    while (off < vsize) {
        const int64_t got = ntfs_read_range(rec, off, a, sizeof(a));
        size_t want;
        if (got <= 0) {
            printf("  FAIL %s: ranged read returned %lld at offset %llu\n",
                   vpath, (long long)got, (unsigned long long)off);
            fclose(h);
            return 0;
        }
        want = fread(b, 1, (size_t)got, h);
        if (want != (size_t)got || memcmp(a, b, want) != 0) {
            printf("  FAIL %s: bytes differ near offset %llu\n",
                   vpath, (unsigned long long)off);
            fclose(h);
            return 0;
        }
        off += (uint64_t)got;
    }
    fclose(h);
    printf("  ok   %-28s %llu bytes match\n", vpath,
           (unsigned long long)vsize);
    checks++;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ntfs_verify <image> [vpath=hostpath ...]\n");
        return 2;
    }
    if (ntfs_host_open(argv[1]) != 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    printf("Vextro NTFS: the built volume, read by the kernel's driver\n");
    printf("==========================================================\n\n");

    ok("the volume mounts", ntfs_try(0) == 1);
    if (!ntfs.mounted) { printf("\ncannot continue\n"); return 1; }

    printf("       %u byte clusters, %u byte MFT records\n",
           ntfs.bytes_per_cluster, ntfs.bytes_per_record);

    /* Space has to be sane before anything else is believable: a bitmap
     * read backwards reports a nearly empty volume as nearly full. */
    {
        uint64_t total = 0, freec = 0;
        ok("the free-space bitmap reads", ntfs_space(&total, &freec) == 0);
        ok("  total clusters match an 8 GB volume", total == 2097152);
        ok("  and some of it is used", total - freec > 0);
        ok("  and some of it is free", freec > 0);
        printf("       %llu of %llu clusters used (%llu MB)\n",
               (unsigned long long)(total - freec),
               (unsigned long long)total,
               (unsigned long long)((total - freec) * 4096 / (1024 * 1024)));
    }

    /* The pagefile: the one file whose *shape* matters as much as its
     * contents, because the pager cannot use a fragmented one. */
    {
        uint64_t rec = 0, lba = 0, bytes = 0, size = 0;
        printf("\nthe pagefile\n");
        ok("pagefile.sys resolves",
           ntfs_lookup("/pagefile.sys", &rec, 0, 0, &size) == 0);
        ok("  and is 256 MB", size == 256ULL * 1024 * 1024);
        ok("  and is exactly one extent",
           ntfs_single_extent(rec, size, &lba, &bytes) == 0);
        printf("       one run at LBA %llu, %llu MB\n",
               (unsigned long long)lba,
               (unsigned long long)(bytes / (1024 * 1024)));
    }

    printf("\nseeded files, byte for byte\n");
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = '\0';
        if (!compare(argv[i], eq + 1)) fails++;
    }

    ntfs_host_close();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
