#ifndef NTFS_H
#define NTFS_H

/*
 * src/ntfs.h — reading NTFS.
 *
 * NTFS is not a table of directory entries with a file allocation table
 * behind it. It is a database, and the database is a file: $MFT, whose
 * every record describes one file — including, in record 0, $MFT
 * itself. Everything else follows from that one idea, and it is what
 * makes the format worth implementing rather than merely supporting.
 *
 * A record is a header and a list of *attributes*, each typed. The name
 * is an attribute. The data is an attribute. The security descriptor is
 * an attribute. A small file's data sits inside its own record —
 * "resident" — so a 200-byte file costs one 1 KB record and no clusters
 * at all. A large file's data is a list of runs, each a length and a
 * *difference* from the previous run's start, which is how a
 * multi-gigabyte file's layout fits in a few dozen bytes.
 *
 * Two details bite anyone reading this format for the first time, and
 * both are handled below:
 *
 *   Fixups. Every sector of a record ends with two bytes that are a
 *   copy of a per-record sequence number, and the real values live in an
 *   array at the top of the record. It is a torn-write detector: if the
 *   last two bytes of any sector do not match, the record was written
 *   half-way. They have to be swapped back before the record can be
 *   parsed, and a parser that forgets sees corruption every 512 bytes.
 *
 *   Signed run offsets. A data run's start is a signed difference from
 *   the previous one, in a field whose width is given in the same byte
 *   as the length's. Reading it unsigned works until a file is laid out
 *   backwards on the volume, and then it points somewhere absurd.
 *
 * This reads. It does not write. Writing NTFS safely means maintaining
 * $LogFile, $Bitmap, $Secure and the B-tree indices in step, and a
 * half-correct writer is far worse than none — so the mount is
 * read-only and says so.
 */

#include <stdint.h>
#include "blk.h"
#include "part.h"

#define NTFS_ATTR_STANDARD_INFO 0x10
#define NTFS_ATTR_ATTRIBUTE_LIST 0x20
#define NTFS_ATTR_FILE_NAME     0x30
#define NTFS_ATTR_SECURITY_DESC 0x50
#define NTFS_ATTR_DATA          0x80
#define NTFS_ATTR_INDEX_ROOT    0x90
#define NTFS_ATTR_INDEX_ALLOC   0xA0
#define NTFS_ATTR_BITMAP        0xB0
#define NTFS_ATTR_END           0xFFFFFFFF

#define NTFS_MFT_ROOT 5            /* record 5 is always the root directory */

typedef struct {
    uint8_t  jump[3];
    char     oem[8];               /* "NTFS    " */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  unused0[5];
    uint8_t  media;
    uint8_t  unused1[2];
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t unused2[2];
    uint64_t total_sectors;
    uint64_t mft_cluster;
    uint64_t mftmirr_cluster;
    int8_t   clusters_per_record;  /* negative: 2^-n bytes */
    uint8_t  unused3[3];
    int8_t   clusters_per_index;
    uint8_t  unused4[3];
    uint64_t serial;
} __attribute__((packed)) ntfs_boot_t;

typedef struct {
    char     magic[4];             /* "FILE" */
    uint16_t fixup_offset;
    uint16_t fixup_count;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t link_count;
    uint16_t attr_offset;
    uint16_t flags;                /* 1 in use, 2 directory */
    uint32_t used_size;
    uint32_t alloc_size;
    uint64_t base_record;
    uint16_t next_attr_id;
} __attribute__((packed)) ntfs_record_t;

typedef struct {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t id;
    union {
        struct {
            uint32_t value_length;
            uint16_t value_offset;
            uint8_t  indexed;
            uint8_t  padding;
        } __attribute__((packed)) res;
        struct {
            uint64_t start_vcn;
            uint64_t last_vcn;
            uint16_t run_offset;
            uint16_t compression;
            uint32_t padding;
            uint64_t alloc_size;
            uint64_t real_size;
            uint64_t init_size;
        } __attribute__((packed)) nonres;
    } u;
} __attribute__((packed)) ntfs_attr_t;

static struct {
    int      mounted;
    uint64_t part_lba;             /* where the volume starts on the disk */
    uint32_t bytes_per_cluster;
    uint32_t bytes_per_record;
    uint32_t sectors_per_cluster;
    uint64_t mft_lcn;
    uint64_t serial;
    uint64_t records_seen;
    const char *status;
} ntfs = { .status = "not probed" };

static uint8_t  ntfs_rec[4096];
static uint8_t  ntfs_clu[65536];

/*
 * Undo the fixups.
 *
 * The last two bytes of every sector in the record were replaced with a
 * copy of the update sequence number when it was written; the values
 * they displaced are in the array. If any sector's copy does not match,
 * that sector was never written and the record is torn — which is the
 * whole reason the mechanism exists, so it is reported rather than
 * patched over.
 */
static int ntfs_apply_fixups(uint8_t *rec, uint32_t size, uint32_t sector) {
    ntfs_record_t *h = (ntfs_record_t *)rec;
    if (h->fixup_count == 0 || h->fixup_offset + h->fixup_count * 2 > size)
        return -1;

    const uint16_t *fix = (const uint16_t *)(rec + h->fixup_offset);
    uint16_t usn = fix[0];

    for (uint32_t i = 1; i < h->fixup_count; i++) {
        uint32_t off = i * sector - 2;
        if (off + 2 > size) return -1;
        uint16_t *tail = (uint16_t *)(rec + off);
        if (*tail != usn) return -1;         /* torn write */
        *tail = fix[i];
    }
    return 0;
}

static int ntfs_read_clusters(uint64_t lcn, uint32_t count, void *buf) {
    uint64_t lba = ntfs.part_lba + lcn * ntfs.sectors_per_cluster;
    return blk_read(lba, count * ntfs.sectors_per_cluster, buf);
}

/*
 * Walk a data run list.
 *
 * Each run begins with a byte packing two nibbles: how many bytes the
 * length occupies, and how many the offset does. The offset is a signed
 * difference from the previous run's start, so a file whose second
 * extent is *before* its first has a negative one — read it unsigned and
 * the resulting cluster number is astronomical.
 *
 * A run with a zero-width offset is a hole: a sparse file's unwritten
 * region, which reads as zeroes and occupies nothing.
 */
typedef struct {
    uint64_t lcn;
    uint64_t length;
    int      sparse;
} ntfs_run_t;

static int ntfs_next_run(const uint8_t **p, const uint8_t *end,
                         int64_t *prev_lcn, ntfs_run_t *out) {
    if (*p >= end || **p == 0) return 0;
    uint8_t hdr = *(*p)++;
    int len_size = hdr & 0x0F;
    int off_size = (hdr >> 4) & 0x0F;
    if (len_size == 0 || *p + len_size + off_size > end) return 0;

    uint64_t length = 0;
    for (int i = 0; i < len_size; i++)
        length |= (uint64_t)(*(*p)++) << (i * 8);

    if (off_size == 0) {
        out->sparse = 1;
        out->lcn = 0;
        out->length = length;
        return 1;
    }

    int64_t delta = 0;
    for (int i = 0; i < off_size; i++)
        delta |= (int64_t)(uint64_t)(*(*p)++) << (i * 8);
    /* Sign-extend from the width the run actually used. */
    int bits = off_size * 8;
    if (bits < 64 && (delta & (1LL << (bits - 1))))
        delta |= -1LL << bits;

    *prev_lcn += delta;
    out->sparse = 0;
    out->lcn = (uint64_t)*prev_lcn;
    out->length = length;
    return 1;
}

/* Read one MFT record by number, fixups undone. */
static int ntfs_read_record(uint64_t number, uint8_t *out) {
    uint64_t byte_off = number * ntfs.bytes_per_record;
    uint64_t lcn = ntfs.mft_lcn + byte_off / ntfs.bytes_per_cluster;
    uint32_t within = (uint32_t)(byte_off % ntfs.bytes_per_cluster);

    if (ntfs_read_clusters(lcn, 1, ntfs_clu) != 0) return -1;
    for (uint32_t i = 0; i < ntfs.bytes_per_record; i++)
        out[i] = ntfs_clu[within + i];

    const ntfs_record_t *h = (const ntfs_record_t *)out;
    if (h->magic[0] != 'F' || h->magic[1] != 'I' ||
        h->magic[2] != 'L' || h->magic[3] != 'E') return -1;
    if (ntfs_apply_fixups(out, ntfs.bytes_per_record, 512) != 0) return -1;
    ntfs.records_seen++;
    return 0;
}

static const ntfs_attr_t *ntfs_find_attr(const uint8_t *rec, uint32_t type) {
    const ntfs_record_t *h = (const ntfs_record_t *)rec;
    uint32_t off = h->attr_offset;
    while (off + 8 <= ntfs.bytes_per_record) {
        const ntfs_attr_t *a = (const ntfs_attr_t *)(rec + off);
        if (a->type == NTFS_ATTR_END) break;
        if (a->length < 16 || off + a->length > ntfs.bytes_per_record) break;
        if (a->type == type) return a;
        off += a->length;
    }
    return 0;
}

/*
 * The name, out of the $FILE_NAME attribute.
 *
 * NTFS stores names in UTF-16 and a file may have several — a long one
 * and a generated 8.3 one, distinguished by a namespace byte. The DOS
 * namespace entry is skipped when a longer one exists, because it is a
 * derived name rather than the file's own.
 */
static int ntfs_record_name(const uint8_t *rec, char *out, int cap) {
    const ntfs_record_t *h = (const ntfs_record_t *)rec;
    uint32_t off = h->attr_offset;
    int best = -1, best_len = 0;
    const uint8_t *best_name = 0;

    while (off + 8 <= ntfs.bytes_per_record) {
        const ntfs_attr_t *a = (const ntfs_attr_t *)(rec + off);
        if (a->type == NTFS_ATTR_END) break;
        if (a->length < 16 || off + a->length > ntfs.bytes_per_record) break;

        if (a->type == NTFS_ATTR_FILE_NAME && !a->non_resident) {
            const uint8_t *v = rec + off + a->u.res.value_offset;
            int nlen = v[64];
            int space = v[65];       /* 0 POSIX, 1 Win32, 2 DOS, 3 both */
            if (space != 2 && nlen > best_len) {
                best_len = nlen;
                best_name = v + 66;
                best = space;
            }
        }
        off += a->length;
    }
    if (!best_name) return -1;
    (void)best;

    int n = 0;
    for (int i = 0; i < best_len && n < cap - 1; i++) {
        uint16_t c = (uint16_t)(best_name[i * 2] | (best_name[i * 2 + 1] << 8));
        out[n++] = (c && c < 0x80) ? (char)c : '?';
    }
    out[n] = '\0';
    return n;
}

/* How large is this file, and is it resident? */
static uint64_t ntfs_record_size(const uint8_t *rec, int *resident) {
    const ntfs_attr_t *d = ntfs_find_attr(rec, NTFS_ATTR_DATA);
    if (!d) { if (resident) *resident = 0; return 0; }
    if (!d->non_resident) {
        if (resident) *resident = 1;
        return d->u.res.value_length;
    }
    if (resident) *resident = 0;
    return d->u.nonres.real_size;
}

/*
 * Read a file's contents by MFT record number.
 *
 * A resident file is copied straight out of its own record. A
 * non-resident one is assembled by walking its runs; a sparse run
 * contributes zeroes without touching the disk, which is the point of
 * being sparse.
 */
static int64_t ntfs_read_file(uint64_t record, void *buf, uint64_t cap) {
    if (!ntfs.mounted) return -1;
    if (ntfs_read_record(record, ntfs_rec) != 0) return -1;

    const ntfs_attr_t *d = ntfs_find_attr(ntfs_rec, NTFS_ATTR_DATA);
    if (!d) return -1;
    uint8_t *out = (uint8_t *)buf;

    if (!d->non_resident) {
        uint32_t n = d->u.res.value_length;
        if (n > cap) n = (uint32_t)cap;
        const uint8_t *src = (const uint8_t *)d + d->u.res.value_offset;
        for (uint32_t i = 0; i < n; i++) out[i] = src[i];
        return n;
    }

    uint64_t want = d->u.nonres.real_size;
    if (want > cap) want = cap;

    const uint8_t *p = (const uint8_t *)d + d->u.nonres.run_offset;
    const uint8_t *end = (const uint8_t *)d + d->length;
    int64_t prev = 0;
    uint64_t done = 0;
    ntfs_run_t run;

    while (done < want && ntfs_next_run(&p, end, &prev, &run)) {
        uint64_t bytes = run.length * ntfs.bytes_per_cluster;
        if (bytes > want - done) bytes = want - done;

        if (run.sparse) {
            for (uint64_t i = 0; i < bytes; i++) out[done + i] = 0;
            done += bytes;
            continue;
        }
        uint64_t left = bytes;
        uint64_t lcn = run.lcn;
        while (left) {
            uint32_t chunk = sizeof(ntfs_clu);
            if ((uint64_t)chunk > left) chunk = (uint32_t)left;
            uint32_t clusters = (chunk + ntfs.bytes_per_cluster - 1) /
                                ntfs.bytes_per_cluster;
            if (ntfs_read_clusters(lcn, clusters, ntfs_clu) != 0) return -1;
            for (uint32_t i = 0; i < chunk; i++) out[done + i] = ntfs_clu[i];
            done += chunk;
            left -= chunk;
            lcn  += clusters;
        }
    }
    return (int64_t)done;
}

/*
 * Try to mount an NTFS volume.
 *
 * Every partition the scanner found is offered, plus the whole device,
 * because a volume written by a tool that skipped the partition table is
 * still a volume.
 */
static int ntfs_try(uint64_t part_lba) {
    static uint8_t boot[512];
    if (blk_read(part_lba, 1, boot) != 0) return 0;
    const ntfs_boot_t *b = (const ntfs_boot_t *)boot;

    const char sig[8] = { 'N','T','F','S',' ',' ',' ',' ' };
    for (int i = 0; i < 8; i++) if (b->oem[i] != sig[i]) return 0;
    if (b->bytes_per_sector != 512 && b->bytes_per_sector != 4096) return 0;
    if (b->sectors_per_cluster == 0) return 0;

    ntfs.part_lba           = part_lba;
    ntfs.sectors_per_cluster = b->sectors_per_cluster;
    ntfs.bytes_per_cluster  = (uint32_t)b->bytes_per_sector *
                              b->sectors_per_cluster;
    ntfs.mft_lcn            = b->mft_cluster;
    ntfs.serial             = b->serial;

    /* A negative value is a power of two in bytes rather than a count of
     * clusters, which is how a 1 KB record fits on a volume with 4 KB
     * clusters. */
    if (b->clusters_per_record < 0)
        ntfs.bytes_per_record = 1u << (uint8_t)(-b->clusters_per_record);
    else
        ntfs.bytes_per_record = (uint32_t)b->clusters_per_record *
                                ntfs.bytes_per_cluster;

    if (ntfs.bytes_per_record < 512 ||
        ntfs.bytes_per_record > sizeof(ntfs_rec)) return 0;
    if (ntfs.bytes_per_cluster > sizeof(ntfs_clu)) return 0;

    /* Record 0 is $MFT itself. If it does not parse, this is not a
     * volume we can read whatever the boot sector claims. */
    if (ntfs_read_record(0, ntfs_rec) != 0) return 0;

    ntfs.mounted = 1;
    ntfs.status  = "mounted read-only";
    return 1;
}

static void ntfs_mount(void) {
    if (!blk_present()) { ntfs.status = "no disk"; return; }

    if (ntfs_try(0)) goto done;
    for (int i = 0; i < part_count; i++)
        if (ntfs_try(part_table[i].start)) goto done;

    ntfs.status = "no NTFS volume found";
    return;

done:
    serial_puts("[ntfs] volume at LBA ");
    serial_put_dec((uint32_t)ntfs.part_lba);
    serial_puts(": ");
    serial_put_dec(ntfs.bytes_per_cluster);
    serial_puts(" byte clusters, ");
    serial_put_dec(ntfs.bytes_per_record);
    serial_puts(" byte MFT records, read-only\n");

    /* Name the root, which is the cheapest end-to-end proof that record
     * reading, fixups and attribute walking all work. */
    if (ntfs_read_record(NTFS_MFT_ROOT, ntfs_rec) == 0) {
        char name[64];
        if (ntfs_record_name(ntfs_rec, name, sizeof(name)) > 0) {
            serial_puts("[ntfs] root directory record reads as \"");
            serial_puts(name);
            serial_puts("\"\n");
        }
    }
}

#endif /* NTFS_H */
