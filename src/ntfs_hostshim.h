#ifndef NTFS_HOST_SHIM_H
#define NTFS_HOST_SHIM_H

/*
 * src/ntfs_hostshim.h — the kernel, reduced to what a filesystem needs.
 *
 * src/ntfs.h and src/ntfswrite.h reach for four things outside
 * themselves: read sectors, write sectors, flush, and print. On the
 * build machine those are a file and stdout, which is what this
 * supplies -- so tools/ntfs_test.c exercises the identical source that
 * runs in the kernel, against an image made by tools/mkntfs.py.
 *
 * That matters more for a filesystem than for anything else here. A
 * reader can be checked by reading; a *writer* can only be checked by
 * writing, and the only volume available in the kernel is the one the
 * system is booted from. Testing on the host is what makes it possible
 * to be wrong about NTFS without losing anything.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static FILE *ntfs_host_img = 0;
static uint64_t ntfs_host_reads = 0;
static uint64_t ntfs_host_writes = 0;
static uint64_t ntfs_host_flushes = 0;

static int ntfs_host_open(const char *path) {
    ntfs_host_img = fopen(path, "r+b");
    return ntfs_host_img ? 0 : -1;
}

static void ntfs_host_close(void) {
    if (ntfs_host_img) fclose(ntfs_host_img);
    ntfs_host_img = 0;
}

static int blk_read(uint64_t lba, uint32_t count, void *buf) {
    if (!ntfs_host_img) return -1;
    if (fseeko(ntfs_host_img, (off_t)(lba * 512), SEEK_SET) != 0) return -1;
    if (fread(buf, 512, count, ntfs_host_img) != count) return -1;
    ntfs_host_reads += count;
    return 0;
}

static int blk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (!ntfs_host_img) return -1;
    if (fseeko(ntfs_host_img, (off_t)(lba * 512), SEEK_SET) != 0) return -1;
    if (fwrite(buf, 512, count, ntfs_host_img) != count) return -1;
    ntfs_host_writes += count;
    return 0;
}

static int blk_flush(void) {
    if (ntfs_host_img) fflush(ntfs_host_img);
    ntfs_host_flushes++;
    return 0;
}

/* The kernel's log, on stdout. Quiet by default so a passing test is
 * readable; the driver's own messages appear when something fails. */
static int ntfs_host_verbose = 0;

static void serial_puts(const char *s) {
    if (ntfs_host_verbose) fputs(s, stdout);
}

static void serial_putc(char c) {
    if (ntfs_host_verbose) fputc(c, stdout);
}

static void serial_put_dec(uint32_t v) {
    if (ntfs_host_verbose) printf("%u", v);
}

static void serial_put_hex32(uint32_t v) {
    if (ntfs_host_verbose) printf("%08x", v);
}

/* ntfswrite.h timestamps its records from the scheduler's tick count. */
static uint64_t sched_ticks = 0;

/* The partition scanner. The test mounts a bare volume with no
 * partition table, which is what mkntfs.py produces and what the
 * kernel handles through the same "whole disk is the volume" path. */
#define PART_MAX 4
typedef struct {
    uint64_t start;
    uint64_t sectors;
    int      used;
} part_entry_t;

static part_entry_t part_table[PART_MAX];
static int part_count = 0;

/* The kernel asks whether there is a disk at all before probing. On
 * the host there always is: the image file. */
static int blk_present(void) { return ntfs_host_img != 0; }

#endif /* NTFS_HOST_SHIM_H */
