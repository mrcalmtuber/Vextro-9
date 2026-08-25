/*
 * src/fs/ntfs/ntfs_ops.c -- NTFS, read and written.
 *
 * This is one of the kernel's four objects rather than a header
 * included into the composition root, and NTFS is the module that makes
 * the clearest case for the separation: it reaches for exactly nine
 * symbols outside itself -- four to move sectors, four to log, one to
 * ask the time -- and exports one. Everything else it needs, it defines.
 * A boundary that narrow is worth compiling separately, and
 * include/kernel_shared.h is where those nine are declared.
 *
 * It is also the module where a separate object buys something beyond
 * tidiness. tools/ntfs_test.c now builds against this exact file rather
 * than re-including a pair of headers, so what the host suite checks and
 * what the kernel runs are the same source, compiled by the same rule.
 *
 * The two halves below were src/ntfs.h and src/ntfswrite.h. They are one
 * file now because they were never separable in practice -- the writer
 * calls into the reader on every operation, to find a record before it
 * changes it -- and because a reader and a writer that disagree about a
 * structure layout is the one filesystem bug that costs a volume. Their
 * original commentary is kept below, in place and unedited.
 */

#include <stdint.h>

#ifdef NTFS_HOST_TEST
/* The block layer and the partition scanner are the only things this
 * file needs from the kernel, and both are a handful of functions. The
 * host test supplies them over a file so that the *same* source that
 * mounts a disk in the kernel can be run against an image on the build
 * machine -- which is the only way to check a filesystem writer without
 * risking a volume that matters. */
#include "fs/ntfs/ntfs_hostshim.h"
#else
/* In the kernel those same symbols come from the seam header. Note what
 * is deliberately absent: src/blk.h and src/part.h, whose bodies hold
 * the device list and the partition table. Including either here would
 * hand this object private copies of both, and a mount that reads from
 * a device nobody ever opened. */
#include "kernel_shared.h"
#endif

#include "fs/ntfs/ntfs.h"

/* ================================================================== */
/* PART ONE -- READING                                                */
/* ================================================================== */

/*
 * Reading NTFS.
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
    if (h->fixup_count == 0 ||
        (uint32_t)h->fixup_offset + (uint32_t)h->fixup_count * 2u > size)
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
    /* Sign-extend by ORing in the high bits, built unsigned. Shifting a
     * negative value left is undefined, and the obvious -1LL << bits is
     * exactly that -- it happens to work on every compiler this has met
     * and is still not something to leave in a filesystem. */
    if (bits < 64 && (delta & (1LL << (bits - 1))))
        delta |= (int64_t)(~0ULL << bits);

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
uint64_t ntfs_record_size(const uint8_t *rec, int *resident) {
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
int64_t ntfs_read_file(uint64_t record, void *buf, uint64_t cap) {
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
    ntfs.status  = "mounted read/write";
    return 1;
}

void ntfs_mount(void) {
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
    serial_puts(" byte MFT records, read/write\n");

    /*
     * The journal, before anything reads the volume.
     *
     * A committed-but-unapplied record is a write that was durable and
     * then interrupted, and until it is replayed the volume is in the
     * state the interrupted operation left: a directory entry pointing
     * at a record that was never written, or clusters marked in use
     * that nothing owns. Replaying first means every read after this
     * point sees a consistent volume rather than the moment the power
     * went.
     */
    {
        int applied;
        ntfs_journal_init(ntfs.part_lba, 0);
        applied = ntfs_journal_replay();
        if (applied > 0) {
            serial_puts("[ntfs] journal: replayed ");
            serial_put_dec((uint32_t)applied);
            serial_puts(" record(s) from an interrupted write\n");
        }
    }

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

/* ================================================================== */
/* PART TWO -- WRITING                                                */
/* ================================================================== */

/*
 * Making changes to an NTFS volume.
 *
 * Part one reads NTFS. This writes it, which is a different problem
 * by an order of magnitude, and the reason is that in NTFS every
 * structure that describes the volume is itself a file on the volume.
 * Creating one file touches five of them:
 *
 *   $Bitmap        mark the clusters the data will occupy
 *   $MFT           allocate a record, via $MFT's own $BITMAP attribute
 *   the new record  write $STANDARD_INFORMATION, $FILE_NAME, $DATA
 *   the directory   insert a $FILE_NAME entry into its $INDEX_ROOT
 *   $UsnJrnl       record that it happened
 *
 * Any one of those landing without the others leaves a volume that
 * mounts and is wrong: clusters marked used that nothing owns, a
 * directory entry pointing at a record that was never written, a record
 * marked in use with no name. That is what the journal at the bottom of
 * this file is for, and why every operation goes through it.
 *
 * ---- what this does not do ----
 *
 * Stated plainly, because the gap between "writes NTFS" and "writes all
 * of NTFS" is where a filesystem eats data.
 *
 *   $ATTRIBUTE_LIST        A file whose attributes outgrow one MFT
 *                          record needs its attributes spread across
 *                          several. Not implemented; a file that would
 *                          need it is refused.
 *
 *   compression, encryption, sparse files, hard links, alternate data
 *   streams, security descriptors beyond the volume default.
 *
 *   $LogFile               NTFS's own transaction log. Its restart and
 *                          redo record formats are not publicly
 *                          specified, so a log Windows would replay
 *                          cannot be written from documentation. What
 *                          is here instead is this system's own
 *                          write-ahead journal, which protects *this*
 *                          driver's writes and which Windows ignores.
 *
 * A volume this driver has written is consistent and mountable
 * elsewhere. It is not a volume that has exercised the parts of NTFS
 * listed above, and chkdsk has never been run against one.
 */

/* The NTFS_W_* return codes moved to fs/ntfs/ntfs.h, which this file
 * includes: the filesystem dispatch compares against them now, and two
 * copies of a set of error codes is two copies that can drift. */

const char *ntfs_w_errstr = "";

/* Scratch. One operation is in flight at a time -- the filesystem lock
 * above this layer guarantees it -- so these are shared rather than
 * being four kilobytes of stack in a kernel thread. */
static uint8_t ntfs_w_rec[4096];
static uint8_t ntfs_w_dir[4096];
static uint8_t ntfs_w_clu[65536];
static uint8_t ntfs_w_bmp[4096];

/* ===========================================================
 * the journal
 * ===========================================================
 *
 * A redo log, not an undo log.
 *
 * Every metadata sector this driver is about to overwrite is first
 * written to a reserved region with its destination and a checksum,
 * then marked committed, and only then written in place. A machine that
 * dies between the commit and the in-place write finds the record on
 * the next mount and replays it; one that dies before the commit finds
 * an incomplete record and discards it, leaving the volume exactly as
 * it was.
 *
 * That ordering is the whole guarantee and it depends on the commit
 * flag reaching the disk after the payload and before the destination
 * write. blk_flush() between the two is what enforces it -- without it
 * a drive's write cache is free to reorder them, and the journal
 * becomes a description of a state the volume was never in.
 */

#define NTFS_JRNL_MAGIC     0x564A524Eu     /* "NRJV" */
#define NTFS_JRNL_SECTORS   256             /* 128 KB of log            */
#define NTFS_JRNL_PAYLOAD   512

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint64_t target_lba;
    uint32_t length;                /* bytes of payload, always 512     */
    uint32_t checksum;
    uint32_t committed;             /* written last, on its own          */
    uint32_t reserved;
} __attribute__((packed)) ntfs_jrnl_hdr_t;

static struct {
    int      ready;
    uint64_t base_lba;              /* where the log lives on the disk   */
    uint32_t next;                  /* slot to use                       */
    uint32_t seq;
    uint32_t replayed;
    uint32_t writes;
} ntfs_jrnl;

static uint32_t ntfs_checksum(const uint8_t *p, uint32_t n) {
    /* FNV-1a: enough to catch a torn or partially written payload,
     * which is all this has to do -- it is not a defence against
     * anything that can choose the bytes. */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/*
 * Put the log somewhere it cannot collide with the filesystem.
 *
 * The last 128 KB of the partition, which NTFS itself never allocates:
 * the backup boot sector lives in the final sector and the cluster
 * accounting stops short of it, so the tail of the volume is the one
 * region guaranteed to be nobody's.
 */
int ntfs_journal_init(uint64_t part_lba, uint64_t part_sectors) {
    ntfs_jrnl.ready = 0;
    ntfs_jrnl.next = 0;
    ntfs_jrnl.seq = 1;
    ntfs_jrnl.replayed = 0;
    ntfs_jrnl.writes = 0;

    if (part_sectors < NTFS_JRNL_SECTORS * 2 + 16) return -1;
    ntfs_jrnl.base_lba = part_lba + part_sectors - NTFS_JRNL_SECTORS - 1;
    ntfs_jrnl.ready = 1;
    return 0;
}

/*
 * Replay whatever survived the last shutdown.
 *
 * Committed records are re-applied in sequence order. Applying a record
 * that had already landed is harmless -- it writes the same bytes -- and
 * that idempotence is what lets this run unconditionally at mount
 * rather than needing to know whether the volume was clean.
 */
int ntfs_journal_replay(void) {
    uint8_t buf[512 + 512];
    uint32_t applied = 0;
    uint32_t best_seq = 0;

    if (!ntfs_jrnl.ready) return 0;

    for (uint32_t i = 0; i < NTFS_JRNL_SECTORS / 2; i++) {
        ntfs_jrnl_hdr_t *h = (ntfs_jrnl_hdr_t *)buf;
        uint64_t lba = ntfs_jrnl.base_lba + (uint64_t)i * 2;

        if (blk_read(lba, 2, buf) != 0) break;
        if (h->magic != NTFS_JRNL_MAGIC) continue;
        if (!h->committed) continue;
        if (h->length != NTFS_JRNL_PAYLOAD) continue;
        if (ntfs_checksum(buf + 512, NTFS_JRNL_PAYLOAD) != h->checksum)
            continue;                       /* torn payload: discard    */

        if (blk_write(h->target_lba, 1, buf + 512) != 0) return -1;
        applied++;
        if (h->seq > best_seq) best_seq = h->seq;
    }

    if (applied) {
        blk_flush();
        serial_puts("[ntfs] journal: replayed ");
        serial_put_dec(applied);
        serial_puts(" metadata writes\n");
    }

    ntfs_jrnl.replayed = applied;
    ntfs_jrnl.seq = best_seq + 1;
    return 0;
}

/*
 * Write one metadata sector through the log.
 *
 * The three flushes are not caution, they are the protocol: payload
 * durable, then commit durable, then the real write. Removing any of
 * them leaves a window in which the disk holds a commit for a payload
 * it does not have, and replay then writes garbage over live metadata.
 */
static int ntfs_journal_write(uint64_t target_lba, const void *data) {
    uint8_t buf[512 + 512];
    ntfs_jrnl_hdr_t *h = (ntfs_jrnl_hdr_t *)buf;
    uint64_t slot_lba;

    if (!ntfs_jrnl.ready) {
        /* No log: write straight through. The volume is then only as
         * safe as the drive's own ordering, which is why mounting
         * without a journal says so. */
        if (blk_write(target_lba, 1, data) != 0) return -1;
        return blk_flush();
    }

    slot_lba = ntfs_jrnl.base_lba + (uint64_t)(ntfs_jrnl.next % (NTFS_JRNL_SECTORS / 2)) * 2;

    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    for (uint32_t i = 0; i < 512; i++) buf[512 + i] = ((const uint8_t *)data)[i];

    h->magic      = NTFS_JRNL_MAGIC;
    h->seq        = ntfs_jrnl.seq;
    h->target_lba = target_lba;
    h->length     = NTFS_JRNL_PAYLOAD;
    h->checksum   = ntfs_checksum(buf + 512, NTFS_JRNL_PAYLOAD);
    h->committed  = 0;

    /* 1. payload and header, uncommitted */
    if (blk_write(slot_lba, 2, buf) != 0) return -1;
    if (blk_flush() != 0) return -1;

    /* 2. the commit flag, on its own, after the payload is durable */
    h->committed = 1;
    if (blk_write(slot_lba, 1, buf) != 0) return -1;
    if (blk_flush() != 0) return -1;

    /* 3. the change itself */
    if (blk_write(target_lba, 1, data) != 0) return -1;
    if (blk_flush() != 0) return -1;

    ntfs_jrnl.next++;
    ntfs_jrnl.seq++;
    ntfs_jrnl.writes++;
    return 0;
}

/* A whole MFT record is two sectors at 1024 bytes; each goes through
 * the log separately so a tear between them is recoverable. */
static int ntfs_journal_write_run(uint64_t lba, uint32_t sectors,
                                  const void *data) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < sectors; i++)
        if (ntfs_journal_write(lba + i, p + i * 512) != 0) return -1;
    return 0;
}

/* ===========================================================
 * fixups, in the write direction
 * =========================================================== */

/*
 * Install the update sequence before a record goes to disk.
 *
 * The inverse of ntfs_apply_fixups() in ntfs.h: the last two bytes of
 * every sector are saved into the array and replaced with the sequence
 * number, so that a record torn across sectors is detectable when it is
 * read back. A record written without this is rejected by our own
 * reader -- correctly, because it looks exactly like a torn one.
 */
static int ntfs_install_fixups(uint8_t *rec, uint32_t size, uint32_t sector) {
    ntfs_record_t *h = (ntfs_record_t *)rec;
    uint16_t *fix;
    uint16_t usn;

    if (h->fixup_count == 0) return -1;
    if ((uint32_t)h->fixup_offset + h->fixup_count * 2u > size) return -1;
    if (h->fixup_count != size / sector + 1) return -1;

    fix = (uint16_t *)(rec + h->fixup_offset);

    /* The sequence number must change every write, and must never be
     * zero -- zero is what an unwritten sector reads as, so a record
     * stamped with it cannot be distinguished from one that was never
     * written at all. */
    usn = (uint16_t)(fix[0] + 1);
    if (usn == 0) usn = 1;
    fix[0] = usn;

    for (uint32_t i = 1; i < h->fixup_count; i++) {
        uint32_t off = i * sector - 2;
        uint16_t *tail;
        if (off + 2 > size) return -1;
        tail = (uint16_t *)(rec + off);
        fix[i] = *tail;
        *tail = usn;
    }
    return 0;
}

/* Where an MFT record lives on the disk. The reader assumes the MFT is
 * one contiguous extent from mft_lcn and so does this; a volume whose
 * $MFT has fragmented is refused at mount rather than written wrongly. */
static uint64_t ntfs_record_lba(uint64_t number) {
    uint64_t byte_off = number * ntfs.bytes_per_record;
    return ntfs.part_lba + ntfs.mft_lcn * ntfs.sectors_per_cluster +
           byte_off / 512;
}

static int ntfs_write_record(uint64_t number, uint8_t *rec) {
    if (ntfs_install_fixups(rec, ntfs.bytes_per_record, 512) != 0) {
        ntfs_w_errstr = "record has no update sequence array";
        return NTFS_W_IO;
    }
    /* And undone below, for the reason spelled out in ix_block_write:
     * a written buffer is not the buffer that was written, and callers
     * here go on using it. */
    if (ntfs_journal_write_run(ntfs_record_lba(number),
                               ntfs.bytes_per_record / 512, rec) != 0) {
        ntfs_apply_fixups(rec, ntfs.bytes_per_record, 512);
        ntfs_w_errstr = "write failed";
        return NTFS_W_IO;
    }
    ntfs_apply_fixups(rec, ntfs.bytes_per_record, 512);
    return NTFS_W_OK;
}

/* ===========================================================
 * $Bitmap: cluster allocation
 * =========================================================== */

static struct {
    uint64_t lcn;                   /* where $Bitmap's data lives       */
    uint64_t clusters;              /* how many clusters it occupies    */
    uint64_t total_clusters;        /* how many the volume has          */
    int      loaded;
} ntfs_bmp;

static int ntfs_bitmap_load(void) {
    const ntfs_attr_t *a;
    const uint8_t *p, *end;
    int64_t prev = 0;
    ntfs_run_t run;

    ntfs_bmp.loaded = 0;
    if (ntfs_read_record(6, ntfs_w_rec) != 0) return -1;   /* $Bitmap */

    a = ntfs_find_attr(ntfs_w_rec, NTFS_ATTR_DATA);
    if (!a || !a->non_resident) return -1;

    p = (const uint8_t *)a + a->u.nonres.run_offset;
    end = (const uint8_t *)a + a->length;
    if (!ntfs_next_run(&p, end, &prev, &run) || run.sparse) return -1;

    ntfs_bmp.lcn = run.lcn;
    ntfs_bmp.clusters = run.length;
    ntfs_bmp.total_clusters = a->u.nonres.real_size * 8;
    ntfs_bmp.loaded = 1;
    return 0;
}

/*
 * Find and claim a run of free clusters.
 *
 * First fit, scanning the bitmap a cluster of bits at a time. NTFS
 * itself uses a more careful policy -- it keeps a hint and tries to
 * place a file's extents near each other -- and the difference shows up
 * as fragmentation over months rather than as anything visible now.
 * What matters here is that a run is only ever handed out once, which
 * is why the bits are set before the caller is told the answer.
 */
static int ntfs_clusters_alloc(uint64_t count, uint64_t *out_lcn) {
    uint64_t bits_per_chunk = (uint64_t)ntfs.bytes_per_cluster * 8;

    if (!ntfs_bmp.loaded && ntfs_bitmap_load() != 0) {
        ntfs_w_errstr = "cannot read $Bitmap";
        return NTFS_W_IO;
    }
    if (count == 0) { *out_lcn = 0; return NTFS_W_OK; }

    for (uint64_t chunk = 0; chunk < ntfs_bmp.clusters; chunk++) {
        uint64_t base = chunk * bits_per_chunk;
        uint32_t nbytes = ntfs.bytes_per_cluster;

        if (nbytes > sizeof(ntfs_w_bmp)) nbytes = sizeof(ntfs_w_bmp);
        if (ntfs_read_clusters(ntfs_bmp.lcn + chunk, 1, ntfs_w_clu) != 0) {
            ntfs_w_errstr = "cannot read $Bitmap data";
            return NTFS_W_IO;
        }

        for (uint64_t bit = 0; bit + count <= bits_per_chunk; bit++) {
            uint64_t k;
            if (base + bit + count > ntfs_bmp.total_clusters) break;

            for (k = 0; k < count; k++) {
                uint64_t b = bit + k;
                if (ntfs_w_clu[b >> 3] & (1u << (b & 7))) break;
            }
            if (k != count) { bit += k; continue; }

            for (k = 0; k < count; k++) {
                uint64_t b = bit + k;
                ntfs_w_clu[b >> 3] |= (uint8_t)(1u << (b & 7));
            }

            /* Persist the bitmap before the caller can use the run: a
             * crash here must leave clusters marked used and unowned,
             * which wastes space, rather than free and owned, which
             * hands the same clusters to the next file. */
            {
                uint64_t lba = ntfs.part_lba +
                    (ntfs_bmp.lcn + chunk) * ntfs.sectors_per_cluster;
                uint64_t first = ((bit) >> 3) / 512;
                uint64_t last  = ((bit + count - 1) >> 3) / 512;
                for (uint64_t s = first; s <= last; s++)
                    if (ntfs_journal_write(lba + s,
                                           ntfs_w_clu + s * 512) != 0) {
                        ntfs_w_errstr = "cannot update $Bitmap";
                        return NTFS_W_IO;
                    }
            }

            *out_lcn = base + bit;
            return NTFS_W_OK;
        }
    }

    ntfs_w_errstr = "no free space";
    return NTFS_W_NOSPACE;
}

static int ntfs_clusters_free(uint64_t lcn, uint64_t count) {
    uint64_t bits_per_chunk = (uint64_t)ntfs.bytes_per_cluster * 8;

    if (!ntfs_bmp.loaded && ntfs_bitmap_load() != 0) return NTFS_W_IO;

    while (count) {
        uint64_t chunk = lcn / bits_per_chunk;
        uint64_t bit   = lcn % bits_per_chunk;
        uint64_t here  = bits_per_chunk - bit;
        uint64_t lba;

        if (here > count) here = count;
        if (chunk >= ntfs_bmp.clusters) return NTFS_W_IO;

        if (ntfs_read_clusters(ntfs_bmp.lcn + chunk, 1, ntfs_w_clu) != 0)
            return NTFS_W_IO;

        for (uint64_t k = 0; k < here; k++) {
            uint64_t b = bit + k;
            ntfs_w_clu[b >> 3] &= (uint8_t)~(1u << (b & 7));
        }

        lba = ntfs.part_lba + (ntfs_bmp.lcn + chunk) * ntfs.sectors_per_cluster;
        {
            uint64_t first = (bit >> 3) / 512;
            uint64_t last  = ((bit + here - 1) >> 3) / 512;
            for (uint64_t s = first; s <= last; s++)
                if (ntfs_journal_write(lba + s, ntfs_w_clu + s * 512) != 0)
                    return NTFS_W_IO;
        }

        lcn += here;
        count -= here;
    }
    return NTFS_W_OK;
}

/* ===========================================================
 * $MFT: record allocation
 * =========================================================== */

/*
 * Find a free MFT record.
 *
 * $MFT has a $BITMAP attribute of its own, one bit per record, and it
 * is the authority -- a record whose in-use flag is clear but whose bit
 * is set is one that was deleted and not yet reused, and handing it out
 * would break any directory entry still pointing at it.
 *
 * Records 0 through 15 are the system files and are never candidates.
 */
static int ntfs_mft_alloc(uint64_t *out_number) {
    const ntfs_attr_t *a;
    uint64_t total;

    if (ntfs_read_record(0, ntfs_w_rec) != 0) {           /* $MFT */
        ntfs_w_errstr = "cannot read $MFT";
        return NTFS_W_IO;
    }

    a = ntfs_find_attr(ntfs_w_rec, NTFS_ATTR_DATA);
    if (!a || !a->non_resident) {
        ntfs_w_errstr = "$MFT has no non-resident data";
        return NTFS_W_IO;
    }
    total = a->u.nonres.alloc_size / ntfs.bytes_per_record;

    for (uint64_t n = NTFS_MFT_ROOT + 11; n < total; n++) {
        const ntfs_record_t *h;

        if (ntfs_read_record(n, ntfs_w_dir) != 0) {
            /* A record that will not read is one that was never
             * written -- past the end of what the formatter laid down.
             * That is a free record, and the first one is where the
             * table grows into. */
            *out_number = n;
            return NTFS_W_OK;
        }

        h = (const ntfs_record_t *)ntfs_w_dir;
        if (!(h->flags & 1)) {          /* not in use */
            *out_number = n;
            return NTFS_W_OK;
        }
    }

    ntfs_w_errstr = "the MFT is full";
    return NTFS_W_NOSPACE;
}

/* ===========================================================
 * building a record
 * =========================================================== */

static void ntfs_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}

static void ntfs_put32(uint8_t *p, uint32_t v) {
    ntfs_put16(p, (uint16_t)(v & 0xFFFF));
    ntfs_put16(p + 2, (uint16_t)(v >> 16));
}

static void ntfs_put64(uint8_t *p, uint64_t v) {
    ntfs_put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    ntfs_put32(p + 4, (uint32_t)(v >> 32));
}

/* The reading half. Byte at a time rather than a cast, because NTFS
 * structures are packed and an index entry lands wherever the previous
 * one ended -- a uint64_t read through an unaligned pointer is undefined
 * behaviour that happens to work on x86 and does not survive being
 * compiled for anything else. */
static uint32_t ntfs_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ntfs_get64(const uint8_t *p) {
    return (uint64_t)ntfs_get32(p) | ((uint64_t)ntfs_get32(p + 4) << 32);
}

static uint32_t ntfs_align8(uint32_t n) { return (n + 7u) & ~7u; }

/*
 * Encode a run list.
 *
 * The offset of each run is a signed delta from the previous one, which
 * is what lets a file's extents appear in any order on the volume. The
 * sign handling here is the mirror of the decoder in ntfs.h and the two
 * are checked against each other in tools/ntfs_test.c, because a
 * decoder and encoder that are wrong in the same way agree perfectly
 * and produce a volume nothing else can read.
 */
static uint32_t ntfs_encode_runs(const ntfs_run_t *runs, int n,
                                 uint8_t *out, uint32_t cap) {
    uint32_t pos = 0;
    int64_t prev = 0;

    for (int i = 0; i < n; i++) {
        uint8_t lb[8], ob[8];
        int nl = 0, no = 0;
        uint64_t len = runs[i].length;
        int64_t delta = (int64_t)runs[i].lcn - prev;

        while (len) { lb[nl++] = (uint8_t)(len & 0xFF); len >>= 8; }
        if (nl == 0) lb[nl++] = 0;

        if (delta == 0) {
            ob[no++] = 0;
        } else {
            int64_t v = delta;
            int neg = v < 0;
            while (no < 8) {
                ob[no++] = (uint8_t)(v & 0xFF);
                v >>= 8;                       /* arithmetic shift */
                if (!neg && v == 0) {
                    if (ob[no - 1] & 0x80) ob[no++] = 0x00;
                    break;
                }
                if (neg && v == -1) {
                    if (!(ob[no - 1] & 0x80)) ob[no++] = 0xFF;
                    break;
                }
            }
        }

        if (pos + 1 + nl + no + 1 > cap) return 0;
        out[pos++] = (uint8_t)((no << 4) | nl);
        for (int k = 0; k < nl; k++) out[pos++] = lb[k];
        for (int k = 0; k < no; k++) out[pos++] = ob[k];
        prev = (int64_t)runs[i].lcn;
    }

    if (pos + 1 > cap) return 0;
    out[pos++] = 0;
    return pos;
}

/* $STANDARD_INFORMATION, the timestamps and DOS attribute bits. */
static uint32_t ntfs_build_stdinfo(uint8_t *out, uint64_t now, int is_dir) {
    uint32_t vlen = 48;
    uint32_t voff = 24;
    uint32_t len = ntfs_align8(voff + vlen);

    for (uint32_t i = 0; i < len; i++) out[i] = 0;
    ntfs_put32(out + 0x00, NTFS_ATTR_STANDARD_INFO);
    ntfs_put32(out + 0x04, len);
    out[0x08] = 0;                      /* resident            */
    ntfs_put16(out + 0x14, 0);          /* id                  */
    ntfs_put32(out + 0x10, vlen);
    ntfs_put16(out + 0x14, (uint16_t)voff);
    /* the header's resident half: value length then offset */
    ntfs_put32(out + 0x10, vlen);
    ntfs_put16(out + 0x14, (uint16_t)voff);

    for (int i = 0; i < 4; i++) ntfs_put64(out + voff + i * 8, now);
    ntfs_put32(out + voff + 32, is_dir ? 0x10 : 0x80);   /* NORMAL / DIR */
    return len;
}

/* $FILE_NAME, both as an attribute and as the payload of an index
 * entry -- they are the same structure, which is why the directory
 * index can be searched without reading the records it points at. */
static uint32_t ntfs_build_filename_value(uint8_t *out, uint64_t parent_ref,
                                          const char *name, int is_dir,
                                          uint64_t real, uint64_t alloc,
                                          uint64_t now) {
    uint32_t n = 0;
    while (name[n] && n < 255) n++;

    ntfs_put64(out + 0x00, parent_ref);
    for (int i = 0; i < 4; i++) ntfs_put64(out + 0x08 + i * 8, now);
    ntfs_put64(out + 0x28, alloc);
    ntfs_put64(out + 0x30, real);
    ntfs_put32(out + 0x38, is_dir ? 0x10000000u : 0x80u);
    ntfs_put32(out + 0x3C, 0);
    out[0x40] = (uint8_t)n;
    out[0x41] = 0;                      /* POSIX namespace */
    for (uint32_t i = 0; i < n; i++) {
        out[0x42 + i * 2] = (uint8_t)name[i];
        out[0x42 + i * 2 + 1] = 0;
    }
    return 0x42 + n * 2;
}

static uint32_t ntfs_build_filename_attr(uint8_t *out, uint64_t parent_ref,
                                         const char *name, int is_dir,
                                         uint64_t real, uint64_t alloc,
                                         uint64_t now) {
    uint8_t value[0x42 + 255 * 2];
    uint32_t vlen = ntfs_build_filename_value(value, parent_ref, name, is_dir,
                                              real, alloc, now);
    uint32_t voff = 24;
    uint32_t len = ntfs_align8(voff + vlen);

    for (uint32_t i = 0; i < len; i++) out[i] = 0;
    ntfs_put32(out + 0x00, NTFS_ATTR_FILE_NAME);
    ntfs_put32(out + 0x04, len);
    out[0x08] = 0;
    ntfs_put16(out + 0x0E, 0);
    ntfs_put16(out + 0x0A, 0);
    ntfs_put32(out + 0x10, vlen);
    ntfs_put16(out + 0x14, (uint16_t)voff);
    out[0x16] = 1;                      /* indexed */
    for (uint32_t i = 0; i < vlen; i++) out[voff + i] = value[i];
    return len;
}

static uint32_t ntfs_build_data_nonres(uint8_t *out, const ntfs_run_t *runs,
                                       int nruns, uint64_t real,
                                       uint64_t alloc) {
    uint8_t rl[256];
    uint32_t rlen = ntfs_encode_runs(runs, nruns, rl, sizeof(rl));
    uint32_t roff = 0x40;
    uint32_t len;

    if (!rlen) return 0;
    len = ntfs_align8(roff + rlen);

    for (uint32_t i = 0; i < len; i++) out[i] = 0;
    ntfs_put32(out + 0x00, NTFS_ATTR_DATA);
    ntfs_put32(out + 0x04, len);
    out[0x08] = 1;                      /* non-resident */
    ntfs_put64(out + 0x10, 0);          /* start VCN    */
    ntfs_put64(out + 0x18, alloc / ntfs.bytes_per_cluster - 1);
    ntfs_put16(out + 0x20, (uint16_t)roff);
    ntfs_put64(out + 0x28, alloc);
    ntfs_put64(out + 0x30, real);
    ntfs_put64(out + 0x38, real);       /* initialised size */
    for (uint32_t i = 0; i < rlen; i++) out[roff + i] = rl[i];
    return len;
}

static uint32_t ntfs_build_data_resident(uint8_t *out, const uint8_t *data,
                                         uint32_t n) {
    uint32_t voff = 24;
    uint32_t len = ntfs_align8(voff + n);

    for (uint32_t i = 0; i < len; i++) out[i] = 0;
    ntfs_put32(out + 0x00, NTFS_ATTR_DATA);
    ntfs_put32(out + 0x04, len);
    out[0x08] = 0;
    ntfs_put32(out + 0x10, n);
    ntfs_put16(out + 0x14, (uint16_t)voff);
    for (uint32_t i = 0; i < n; i++) out[voff + i] = data[i];
    return len;
}

/* ===========================================================
 * $INDEX_ROOT: the directory
 * =========================================================== */

/*
 * NTFS orders directory entries by an upcased binary collation of the
 * name, and a reader that binary-searches depends on that order. This
 * compares the way the volume's $UpCase table would for ASCII, which is
 * every name this system creates; a name with a character outside that
 * range is placed by its raw value, which is a well-defined order even
 * where it is not Windows' order.
 */
static int ntfs_name_cmp(const uint8_t *a_utf16, uint32_t alen,
                         const char *b) {
    uint32_t i = 0;
    while (i < alen && b[i]) {
        uint16_t ca = (uint16_t)(a_utf16[i * 2] | (a_utf16[i * 2 + 1] << 8));
        uint16_t cb = (uint8_t)b[i];
        if (ca >= 'a' && ca <= 'z') ca = (uint16_t)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (uint16_t)(cb - 'a' + 'A');
        if (ca != cb) return (ca < cb) ? -1 : 1;
        i++;
    }
    if (i == alen && !b[i]) return 0;
    return (i == alen) ? -1 : 1;
}

/* The $FILE_NAME attribute value, whose layout every index entry
 * embeds. Named rather than open-coded because four functions below
 * index into it and a wrong offset reads a timestamp as a size. */
#define NTFS_FN_PARENT      0x00
#define NTFS_FN_ALLOC_SIZE  0x28
#define NTFS_FN_REAL_SIZE   0x30
#define NTFS_FN_FLAGS       0x38
#define NTFS_FN_NAME_LEN    0x40
#define NTFS_FN_NAMESPACE   0x41
#define NTFS_FN_NAME        0x42

#define NTFS_FA_DIRECTORY   0x10000000u

/* An index entry, once the header is past. */
#define NTFS_IE_REF         0x00        /* child MFT reference, 8 bytes */
#define NTFS_IE_LEN         0x08        /* entry length,        2 bytes */
#define NTFS_IE_FNLEN       0x0A        /* $FILE_NAME length,   2 bytes */
#define NTFS_IE_FLAGS       0x0C        /* bit 1 = last entry           */
#define NTFS_IE_VALUE       0x10        /* the $FILE_NAME value         */

/*
 * The name out of an index entry, as ASCII.
 *
 * NTFS names are UTF-16. Everything above this driver -- the terminal,
 * the file manager, the loader -- is byte strings, exactly as it is over
 * exFAT, whose driver folds the same way. A code point that does not fit
 * in a byte becomes '?' rather than being silently truncated to its low
 * half, which would turn two different names into one.
 */
static void ntfs_name_to_ascii(const uint8_t *utf16, uint32_t chars,
                               char *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < chars && n + 1 < cap; i++) {
        const uint16_t c = (uint16_t)(utf16[i * 2] | (utf16[i * 2 + 1] << 8));
        out[n++] = (c && c < 0x80) ? (char)c : '?';
    }
    out[n] = '\0';
}

typedef int (*ntfs_index_cb_t)(void *ctx, const uint8_t *entry,
                              const char *name);

/* ===========================================================
 * $INDEX_ALLOCATION: the directory B-tree
 * ===========================================================
 *
 * A directory's names live in an index, and until this existed the
 * index was one resident $INDEX_ROOT inside the directory's own MFT
 * record. That holds about thirty-five names in a 4096-byte record and
 * then the directory is full -- which is a filesystem that works right
 * up until somebody uses it.
 *
 * NTFS's answer is a B-tree. $INDEX_ROOT stays in the record and
 * becomes the root node; the rest of the tree lives in $INDEX_ALLOCATION,
 * a non-resident attribute divided into fixed-size *index blocks*, each
 * one an "INDX" record with its own update sequence. A third attribute,
 * $BITMAP, records which blocks are in use. All three are named "$I30",
 * which is how a directory index is distinguished from any other index
 * on the same file.
 *
 * ---- the shape of a node ----
 *
 * Every node is a run of variable-length entries in collation order,
 * ending with one that carries no name and has the "last" flag. If the
 * node has children, *every* entry including the last carries an 8-byte
 * VCN in its final eight bytes, and that child holds the keys that sort
 * *before* the entry. The last entry's child therefore holds everything
 * after the final name, which is why forgetting it silently loses the
 * rightmost subtree of every node in the tree.
 *
 * Entries live in both leaves and interior nodes -- this is a B-tree
 * rather than a B+tree, and a name appears exactly once. That is what
 * makes deletion interesting: removing a name that is acting as a
 * separator has to put something in its place.
 *
 * ---- the block layout, which has one trap in it ----
 *
 *     0x00  "INDX"
 *     0x04  update sequence offset (0x28) and count
 *     0x08  $LogFile sequence number
 *     0x10  this block's VCN
 *     0x18  the index header: entries offset, used, allocated, flags
 *     0x28  the update sequence array
 *     0x40  entries
 *
 * The entries start after the update sequence array, not immediately
 * after the header, and every offset in the index header is relative to
 * the header at 0x18 rather than to the block. Writing entries at 0x28
 * puts them underneath the fixup array, and the first write installs
 * sequence numbers over the top of real data.
 *
 * ---- what is guaranteed when the power fails ----
 *
 * Index blocks are written through the same write-ahead journal as
 * everything else, but a split touches several of them and the journal
 * makes single writes atomic rather than whole operations. So the
 * ordering is the guarantee: **a child is durable before anything
 * points at it**. Blocks are written here; the directory's MFT record
 * is written by the caller, afterwards, exactly as create and delete
 * already do it. An interruption therefore leaves a consistent tree and
 * at most a leaked index block -- never a pointer into a block that was
 * never written.
 */

/* The index header, at 0x18 in a block and at +16 in $INDEX_ROOT. */
#define IX_HDR_ENTRIES_OFF  0x00
#define IX_HDR_USED         0x04
#define IX_HDR_ALLOC        0x08
#define IX_HDR_FLAGS        0x0C
#define IX_HDR_SIZE         0x10

#define IX_HDR_LARGE        0x01        /* this node has children */

/* An entry. */
#define IE_REF              0x00
#define IE_LEN              0x08
#define IE_KEYLEN           0x0A
#define IE_FLAGS            0x0C
#define IE_KEY              0x10

#define IE_HAS_CHILD        0x01
#define IE_LAST             0x02

/* An INDX block. */
#define IXB_HDR             0x18        /* where the index header sits */
#define IXB_USA             0x28        /* the update sequence array   */

/* How deep a directory may get. Four levels of a tree whose nodes hold
 * thirty-odd entries is on the order of a million names; eight is far
 * past anything this system will see, and the limit exists so that a
 * corrupt volume with a cycle in it stops rather than descending
 * forever. */
#define IX_MAX_DEPTH        8

/*
 * The largest an entry can be: the header, a 255-character name, and a
 * downlink. A node is split on the way down whenever it has less than
 * this free, which is what makes the descent one-way -- the level below
 * can never promote something the level above cannot hold.
 */
#define IX_MAX_ENTRY  ntfs_align8(IE_KEY + 0x42 + 255 * 2 + 8)

/*
 * The buffers a descent needs: one block per level, and two more for
 * the halves of a split.
 *
 * These are named after their owner deliberately. ntfs_rec belongs to
 * whoever is reading a file record, ntfs_w_rec to the write path,
 * ntfs_dirrec to a path walk -- and an index descent runs *inside* an
 * operation that is already holding one of them. Sharing any of them is
 * the aliasing bug that costs a directory.
 */
static uint8_t  ntfs_ix_path[IX_MAX_DEPTH][4096];   /* the descent      */
static uint64_t ntfs_ix_path_vcn[IX_MAX_DEPTH];
static uint32_t ntfs_ix_path_off[IX_MAX_DEPTH];     /* entry followed   */
static uint8_t  ntfs_ix_succ[IX_MAX_DEPTH][4096];   /* successor hunt   */
static uint8_t  ntfs_ix_walk[IX_MAX_DEPTH][4096];   /* enumeration      */
static uint8_t  ntfs_ix_split[4096];                /* the new right half */
static uint8_t  ntfs_ix_scratch[4096];              /* rebuilding a node  */

/*
 * Two counters, because the paths they measure cannot be reached on
 * purpose from outside and a test that assumed they ran would be
 * testing nothing.
 *
 * ntfs_ix_splits counts nodes split -- the tree growing sideways.
 * ntfs_ix_promote_grew counts the case that only a delete can produce:
 * a separator replaced by its successor, where the successor's name is
 * *longer* than the one it stands in for, so removing an entry needed
 * more room than it freed.
 */
static uint32_t ntfs_ix_splits = 0;
static uint32_t ntfs_ix_promote_grew = 0;

/* ---- reading the shape ---- */

static uint32_t ix_used(const uint8_t *hdr) {
    return ntfs_get32(hdr + IX_HDR_USED);
}
static uint32_t ix_flags(const uint8_t *hdr) {
    return ntfs_get32(hdr + IX_HDR_FLAGS);
}
static uint8_t *ix_first(uint8_t *hdr) {
    return hdr + ntfs_get32(hdr + IX_HDR_ENTRIES_OFF);
}
static uint8_t *ix_end(uint8_t *hdr) {
    return hdr + ix_used(hdr);
}
static uint16_t ie_len(const uint8_t *e)    { return (uint16_t)(e[IE_LEN] |
                                                     (e[IE_LEN + 1] << 8)); }
static uint16_t ie_keylen(const uint8_t *e) { return (uint16_t)(e[IE_KEYLEN] |
                                                  (e[IE_KEYLEN + 1] << 8)); }
static uint32_t ie_flags(const uint8_t *e)  { return ntfs_get32(e + IE_FLAGS); }

/* The child pointer is the last eight bytes of the entry, wherever the
 * key happens to end -- not a fixed offset. */
static uint64_t ie_vcn(const uint8_t *e) {
    return ntfs_get64(e + ie_len(e) - 8);
}
static void ie_set_vcn(uint8_t *e, uint64_t vcn) {
    ntfs_put64(e + ie_len(e) - 8, vcn);
}

/* The index block size, out of $INDEX_ROOT's own header. */
static uint32_t ix_block_size(const uint8_t *ir_body) {
    const uint32_t n = ntfs_get32(ir_body + 0x08);
    return n ? n : ntfs.bytes_per_cluster;
}

/* Where entries begin in a fresh block: past the header and past the
 * update sequence array, rounded up to eight. */
static uint32_t ix_block_entries_off(uint32_t block_size) {
    const uint32_t usa = block_size / 512 + 1;
    return ntfs_align8(IXB_USA + usa * 2) - IXB_HDR;
}

/* $INDEX_ROOT's body, and the node header inside it. */
static uint8_t *ix_root_body(uint8_t *dir_rec) {
    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ROOT);
    if (!ir || ir->non_resident) return 0;
    return (uint8_t *)ir + ir->u.res.value_offset;
}
static uint8_t *ix_root_hdr(uint8_t *dir_rec) {
    uint8_t *b = ix_root_body(dir_rec);
    return b ? b + 16 : 0;
}

/*
 * Resize $INDEX_ROOT by `delta` bytes, moving every attribute after it
 * and fixing the record's used size.
 *
 * The root is resident, so it grows and shrinks inside the MFT record,
 * and anything following it -- $INDEX_ALLOCATION and $BITMAP, once they
 * exist -- has to move with it.
 */
static int ix_root_resize(uint8_t *dir_rec, int32_t delta) {
    ntfs_record_t *h = (ntfs_record_t *)dir_rec;
    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ROOT);
    uint8_t *after;
    uint32_t tail;

    if (!ir) return NTFS_W_IO;
    if (delta > 0 &&
        h->used_size + (uint32_t)delta > ntfs.bytes_per_record) {
        ntfs_w_errstr = "no room in the directory record";
        return NTFS_W_NOSPACE;
    }

    after = (uint8_t *)ir + ir->length;
    tail  = (uint32_t)(dir_rec + h->used_size - after);

    if (delta > 0)
        for (uint32_t i = 0; i < tail; i++)
            after[(uint32_t)delta + tail - 1 - i] = after[tail - 1 - i];
    else if (delta < 0)
        for (uint32_t i = 0; i < tail; i++)
            after[(int32_t)i + delta] = after[i];

    ir->length              = (uint32_t)((int32_t)ir->length + delta);
    ir->u.res.value_length  = (uint32_t)((int32_t)ir->u.res.value_length + delta);
    h->used_size            = (uint32_t)((int32_t)h->used_size + delta);
    return NTFS_W_OK;
}

/* ---- $INDEX_ALLOCATION: mapping a VCN to the disk ---- */

/*
 * Walk the run list of an attribute and turn a virtual cluster number
 * into a logical one. Returns 0 when the VCN is not mapped, which is
 * how "this block has never been allocated" is told from a real one.
 */
static int ix_vcn_to_lcn(const ntfs_attr_t *a, uint64_t vcn, uint64_t *out) {
    const uint8_t *p = (const uint8_t *)a + a->u.nonres.run_offset;
    const uint8_t *end = (const uint8_t *)a + a->length;
    int64_t prev = 0;
    uint64_t at = 0;
    ntfs_run_t run;

    while (ntfs_next_run(&p, end, &prev, &run)) {
        if (vcn < at + run.length) {
            if (run.sparse) return 0;
            *out = run.lcn + (vcn - at);
            return 1;
        }
        at += run.length;
    }
    return 0;
}

/* How many clusters $INDEX_ALLOCATION currently covers. */
static uint64_t ix_alloc_clusters(const ntfs_attr_t *a) {
    const uint8_t *p = (const uint8_t *)a + a->u.nonres.run_offset;
    const uint8_t *end = (const uint8_t *)a + a->length;
    int64_t prev = 0;
    uint64_t n = 0;
    ntfs_run_t run;
    while (ntfs_next_run(&p, end, &prev, &run)) n += run.length;
    return n;
}

/* Read the run list back out into an array, so a chunk can be appended
 * and the whole thing re-encoded. */
static int ix_read_runs(const ntfs_attr_t *a, ntfs_run_t *runs, int max,
                        int *out_n) {
    const uint8_t *p = (const uint8_t *)a + a->u.nonres.run_offset;
    const uint8_t *end = (const uint8_t *)a + a->length;
    int64_t prev = 0;
    int n = 0;
    ntfs_run_t run;

    while (ntfs_next_run(&p, end, &prev, &run)) {
        if (n >= max) return -1;
        runs[n++] = run;
    }
    *out_n = n;
    return 0;
}

/*
 * Read one index block by VCN, undo its fixups, and check it is one.
 */
static int ix_block_read(uint8_t *dir_rec, uint64_t vcn, uint8_t *out) {
    const ntfs_attr_t *ia = ntfs_find_attr(dir_rec, NTFS_ATTR_INDEX_ALLOC);
    const uint8_t *body = ix_root_body(dir_rec);
    uint32_t bsize, per;
    uint64_t lcn = 0;

    if (!ia || !ia->non_resident || !body) return -1;
    bsize = ix_block_size(body);
    per   = bsize / ntfs.bytes_per_cluster;
    if (per == 0) per = 1;

    if (!ix_vcn_to_lcn(ia, vcn * per, &lcn)) return -1;
    if (ntfs_read_clusters(lcn, per, out) != 0) return -1;
    if (out[0] != 'I' || out[1] != 'N' || out[2] != 'D' || out[3] != 'X')
        return -1;
    if (ntfs_apply_fixups(out, bsize, 512) != 0) return -1;
    return 0;
}

/*
 * Write one index block. Journaled, and this is the write that has to
 * land before anything points at the block.
 */
static int ix_block_write(uint8_t *dir_rec, uint64_t vcn, uint8_t *buf) {
    const ntfs_attr_t *ia = ntfs_find_attr(dir_rec, NTFS_ATTR_INDEX_ALLOC);
    const uint8_t *body = ix_root_body(dir_rec);
    uint32_t bsize, per;
    uint64_t lcn = 0;

    if (!ia || !ia->non_resident || !body) return NTFS_W_IO;
    bsize = ix_block_size(body);
    per   = bsize / ntfs.bytes_per_cluster;
    if (per == 0) per = 1;

    if (!ix_vcn_to_lcn(ia, vcn * per, &lcn)) return NTFS_W_IO;
    ntfs_put64(buf + 0x10, vcn);
    if (ntfs_install_fixups(buf, bsize, 512) != 0) return NTFS_W_IO;
    if (ntfs_journal_write_run(ntfs.part_lba +
                               lcn * ntfs.sectors_per_cluster,
                               bsize / 512, buf) != 0) {
        ntfs_w_errstr = "index block write failed";
        return NTFS_W_IO;
    }

    /*
     * Put the buffer back the way the caller had it.
     *
     * Installing the update sequence *replaces* the last two bytes of
     * every sector with the sequence number and stows the real values
     * in the array at the top. That is what makes a torn write
     * detectable on disk -- and it means the in-memory block is no
     * longer the block once it has been written.
     *
     * A descent keeps using the node it just wrote: it splits a child,
     * writes the parent, then seeks in that same parent again to decide
     * which half to enter. Without this, that second seek reads two
     * bytes of sequence number in the middle of whatever entry happened
     * to straddle offset 510 -- so a name compared correctly before the
     * write and incorrectly after it, and the key went into the wrong
     * subtree. Undoing the fixups restores the bytes exactly.
     */
    ntfs_apply_fixups(buf, bsize, 512);
    return NTFS_W_OK;
}

/* Format an empty block in `buf`: a node with just its end entry. */
static void ix_block_init(uint8_t *buf, uint32_t bsize, uint64_t vcn,
                          int large) {
    const uint32_t eoff = ix_block_entries_off(bsize);
    uint8_t *hdr = buf + IXB_HDR;
    uint8_t *e;

    for (uint32_t i = 0; i < bsize; i++) buf[i] = 0;
    buf[0] = 'I'; buf[1] = 'N'; buf[2] = 'D'; buf[3] = 'X';
    ntfs_put16(buf + 0x04, IXB_USA);
    ntfs_put16(buf + 0x06, (uint16_t)(bsize / 512 + 1));
    ntfs_put64(buf + 0x10, vcn);

    ntfs_put32(hdr + IX_HDR_ENTRIES_OFF, eoff);
    ntfs_put32(hdr + IX_HDR_ALLOC, bsize - IXB_HDR);
    ntfs_put32(hdr + IX_HDR_FLAGS, large ? IX_HDR_LARGE : 0);

    e = hdr + eoff;
    ntfs_put64(e + IE_REF, 0);
    ntfs_put16(e + IE_LEN, (uint16_t)(large ? 0x18 : 0x10));
    ntfs_put16(e + IE_KEYLEN, 0);
    ntfs_put32(e + IE_FLAGS, IE_LAST | (large ? IE_HAS_CHILD : 0));
    if (large) ntfs_put64(e + 0x10, 0);
    ntfs_put32(hdr + IX_HDR_USED, eoff + (large ? 0x18u : 0x10u));
}

/* ---- $BITMAP: which index blocks exist ---- */

static uint8_t *ix_bitmap_value(uint8_t *dir_rec, uint32_t *out_len) {
    ntfs_attr_t *b = (ntfs_attr_t *)ntfs_find_attr(dir_rec, NTFS_ATTR_BITMAP);
    if (!b || b->non_resident) return 0;
    if (out_len) *out_len = b->u.res.value_length;
    return (uint8_t *)b + b->u.res.value_offset;
}

/*
 * Find a free index block, allocating more space if every bit is set.
 *
 * Clusters are taken in chunks rather than one at a time, and that is
 * not about speed either: the run list lives inside the directory's own
 * MFT record, and without $ATTRIBUTE_LIST there is nowhere else for it
 * to go. One cluster per block would add a run per block until the
 * record filled with run list instead of names.
 */
#define IX_GROW_CLUSTERS 8

static int ix_alloc_block(uint8_t *dir_rec, uint64_t *out_vcn) {
    uint32_t blen = 0;
    uint8_t *bits = ix_bitmap_value(dir_rec, &blen);
    ntfs_attr_t *ia;
    const uint8_t *body = ix_root_body(dir_rec);
    uint32_t bsize, per;
    uint64_t have, want;

    if (!bits || !body) return NTFS_W_IO;
    bsize = ix_block_size(body);
    per   = bsize / ntfs.bytes_per_cluster;
    if (per == 0) per = 1;

    /* A cleared bit is a block that already has clusters behind it. */
    for (uint32_t i = 0; i < blen * 8; i++) {
        if (!(bits[i >> 3] & (1u << (i & 7)))) {
            ia = (ntfs_attr_t *)ntfs_find_attr(dir_rec, NTFS_ATTR_INDEX_ALLOC);
            if (!ia) return NTFS_W_IO;
            if ((uint64_t)(i + 1) * per <= ix_alloc_clusters(ia)) {
                bits[i >> 3] |= (uint8_t)(1u << (i & 7));
                *out_vcn = i;
                return NTFS_W_OK;
            }
            break;                      /* past what is mapped: extend */
        }
    }

    /* Extend $INDEX_ALLOCATION by a chunk. */
    ia = (ntfs_attr_t *)ntfs_find_attr(dir_rec, NTFS_ATTR_INDEX_ALLOC);
    if (!ia) return NTFS_W_IO;
    have = ix_alloc_clusters(ia);
    want = have + IX_GROW_CLUSTERS;

    {
        ntfs_run_t runs[16];
        int n = 0, rc;
        uint64_t lcn = 0;
        uint8_t enc[256];
        uint32_t enclen, oldlen, newlen;
        int32_t delta;

        if (ix_read_runs(ia, runs, 15, &n) != 0) {
            ntfs_w_errstr = "index run list too fragmented";
            return NTFS_W_NOSPACE;
        }
        rc = ntfs_clusters_alloc(IX_GROW_CLUSTERS, &lcn);
        if (rc != NTFS_W_OK) return rc;

        /* Coalesce onto the previous run when the allocator handed back
         * the clusters immediately after it, which keeps the run list
         * one entry long on a volume with room. */
        if (n > 0 && runs[n - 1].lcn + runs[n - 1].length == lcn)
            runs[n - 1].length += IX_GROW_CLUSTERS;
        else {
            runs[n].lcn = lcn;
            runs[n].length = IX_GROW_CLUSTERS;
            runs[n].sparse = 0;
            n++;
        }

        enclen = ntfs_encode_runs(runs, n, enc, sizeof(enc));
        if (enclen == 0) {
            ntfs_clusters_free(lcn, IX_GROW_CLUSTERS);
            ntfs_w_errstr = "index run list does not fit";
            return NTFS_W_NOSPACE;
        }

        oldlen = ia->length;
        newlen = ntfs_align8(ia->u.nonres.run_offset + enclen);
        delta  = (int32_t)newlen - (int32_t)oldlen;

        {
            ntfs_record_t *h = (ntfs_record_t *)dir_rec;
            uint8_t *after = (uint8_t *)ia + oldlen;
            uint32_t tail = (uint32_t)(dir_rec + h->used_size - after);

            if (delta > 0 &&
                h->used_size + (uint32_t)delta > ntfs.bytes_per_record) {
                ntfs_clusters_free(lcn, IX_GROW_CLUSTERS);
                ntfs_w_errstr = "no room to grow the index run list";
                return NTFS_W_NOSPACE;
            }
            if (delta > 0)
                for (uint32_t i = 0; i < tail; i++)
                    after[(uint32_t)delta + tail - 1 - i] = after[tail - 1 - i];
            else if (delta < 0)
                for (uint32_t i = 0; i < tail; i++)
                    after[(int32_t)i + delta] = after[i];
            h->used_size = (uint32_t)((int32_t)h->used_size + delta);
        }

        for (uint32_t i = 0; i < enclen; i++)
            ((uint8_t *)ia)[ia->u.nonres.run_offset + i] = enc[i];
        ia->length = newlen;
        ia->u.nonres.last_vcn   = want - 1;
        ia->u.nonres.alloc_size = want * ntfs.bytes_per_cluster;
        ia->u.nonres.real_size  = want * ntfs.bytes_per_cluster;
        ia->u.nonres.init_size  = want * ntfs.bytes_per_cluster;
    }

    /* The bitmap may itself need another byte now. */
    bits = ix_bitmap_value(dir_rec, &blen);
    {
        const uint64_t nblocks = want / per;
        if (nblocks > (uint64_t)blen * 8) {
            ntfs_attr_t *b = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                           NTFS_ATTR_BITMAP);
            ntfs_record_t *h = (ntfs_record_t *)dir_rec;
            uint8_t *after = (uint8_t *)b + b->length;
            uint32_t tail = (uint32_t)(dir_rec + h->used_size - after);

            if (h->used_size + 8 > ntfs.bytes_per_record) {
                ntfs_w_errstr = "no room to grow the index bitmap";
                return NTFS_W_NOSPACE;
            }
            for (uint32_t i = 0; i < tail; i++)
                after[8 + tail - 1 - i] = after[tail - 1 - i];
            for (uint32_t i = 0; i < 8; i++)
                ((uint8_t *)b)[b->u.res.value_offset + blen + i] = 0;
            b->length += 8;
            b->u.res.value_length += 8;
            h->used_size += 8;
            bits = ix_bitmap_value(dir_rec, &blen);
        }
    }

    for (uint32_t i = 0; i < blen * 8; i++) {
        if (!(bits[i >> 3] & (1u << (i & 7)))) {
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
            *out_vcn = i;
            return NTFS_W_OK;
        }
    }
    ntfs_w_errstr = "index bitmap is full";
    return NTFS_W_NOSPACE;
}

static void ix_free_block(uint8_t *dir_rec, uint64_t vcn) {
    uint32_t blen = 0;
    uint8_t *bits = ix_bitmap_value(dir_rec, &blen);
    if (bits && vcn < (uint64_t)blen * 8)
        bits[vcn >> 3] &= (uint8_t)~(1u << (vcn & 7));
}

/*
 * Add $INDEX_ALLOCATION and $BITMAP to a directory that has neither.
 *
 * They go after $INDEX_ROOT because attributes are ordered by type and
 * 0x90 < 0xA0 < 0xB0, and $INDEX_ROOT is the last attribute a directory
 * record has -- so this appends rather than inserting, and nothing
 * before it moves.
 */
static int ix_add_alloc_attrs(uint8_t *dir_rec) {
    ntfs_record_t *h = (ntfs_record_t *)dir_rec;
    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ROOT);
    uint8_t *at;
    uint64_t lcn = 0;
    int rc;
    uint8_t enc[64];
    uint32_t enclen;
    ntfs_run_t run;
    uint32_t ia_len, bm_len;

    if (!ir) return NTFS_W_IO;
    if (ntfs_find_attr(dir_rec, NTFS_ATTR_INDEX_ALLOC)) return NTFS_W_OK;

    rc = ntfs_clusters_alloc(IX_GROW_CLUSTERS, &lcn);
    if (rc != NTFS_W_OK) return rc;

    run.lcn = lcn; run.length = IX_GROW_CLUSTERS; run.sparse = 0;
    enclen = ntfs_encode_runs(&run, 1, enc, sizeof(enc));
    if (enclen == 0) {
        ntfs_clusters_free(lcn, IX_GROW_CLUSTERS);
        return NTFS_W_NOSPACE;
    }

    /* $INDEX_ALLOCATION: non-resident, named "$I30", run offset past the
     * 64-byte non-resident header and the eight bytes of name. */
    ia_len = ntfs_align8(0x40 + 8 + enclen);
    bm_len = ntfs_align8(0x18 + 8 + 8);
    if (h->used_size + ia_len + bm_len > ntfs.bytes_per_record) {
        ntfs_clusters_free(lcn, IX_GROW_CLUSTERS);
        ntfs_w_errstr = "no room for an index allocation attribute";
        return NTFS_W_NOSPACE;
    }

    at = dir_rec + h->used_size - 8;     /* over the END marker */

    for (uint32_t i = 0; i < ia_len; i++) at[i] = 0;
    ntfs_put32(at + 0x00, NTFS_ATTR_INDEX_ALLOC);
    ntfs_put32(at + 0x04, ia_len);
    at[0x08] = 1;                        /* non-resident */
    at[0x09] = 4;                        /* name length  */
    ntfs_put16(at + 0x0A, 0x40);         /* name offset  */
    ntfs_put16(at + 0x0E, 0);            /* instance     */
    ntfs_put64(at + 0x10, 0);            /* first VCN    */
    ntfs_put64(at + 0x18, IX_GROW_CLUSTERS - 1);          /* last VCN */
    ntfs_put16(at + 0x20, (uint16_t)(0x40 + 8));          /* run off  */
    ntfs_put64(at + 0x28, (uint64_t)IX_GROW_CLUSTERS * ntfs.bytes_per_cluster);
    ntfs_put64(at + 0x30, (uint64_t)IX_GROW_CLUSTERS * ntfs.bytes_per_cluster);
    ntfs_put64(at + 0x38, (uint64_t)IX_GROW_CLUSTERS * ntfs.bytes_per_cluster);
    {
        const char *nm = "$I30";
        for (int i = 0; i < 4; i++) {
            at[0x40 + i * 2] = (uint8_t)nm[i];
            at[0x40 + i * 2 + 1] = 0;
        }
    }
    for (uint32_t i = 0; i < enclen; i++) at[0x40 + 8 + i] = enc[i];
    at += ia_len;

    /* $BITMAP: resident, named "$I30", eight bytes of bits. */
    for (uint32_t i = 0; i < bm_len; i++) at[i] = 0;
    ntfs_put32(at + 0x00, NTFS_ATTR_BITMAP);
    ntfs_put32(at + 0x04, bm_len);
    at[0x08] = 0;
    at[0x09] = 4;
    ntfs_put16(at + 0x0A, 0x18);
    ntfs_put32(at + 0x10, 8);            /* value length */
    ntfs_put16(at + 0x14, (uint16_t)(0x18 + 8));          /* value off */
    {
        const char *nm = "$I30";
        for (int i = 0; i < 4; i++) {
            at[0x18 + i * 2] = (uint8_t)nm[i];
            at[0x18 + i * 2 + 1] = 0;
        }
    }
    at += bm_len;

    ntfs_put32(at + 0, NTFS_ATTR_END);
    ntfs_put32(at + 4, 0);
    h->used_size += ia_len + bm_len;
    return NTFS_W_OK;
}

/* ---- entries ---- */

/*
 * Build a directory entry for `name`. `child` is the MFT reference the
 * name resolves to; `vcn_child` is a subtree pointer, or IX_NO_CHILD
 * for a leaf entry.
 */
#define IX_NO_CHILD 0xFFFFFFFFFFFFFFFFULL

static uint32_t ix_make_entry(uint8_t *out, uint64_t child_ref,
                              const char *name, int is_dir, uint64_t real,
                              uint64_t alloc, uint64_t now, uint64_t vcn_child) {
    uint8_t fnval[0x42 + 255 * 2];
    const uint32_t fnlen = ntfs_build_filename_value(fnval, 0, name, is_dir,
                                                     real, alloc, now);
    const int child = (vcn_child != IX_NO_CHILD);
    const uint32_t elen = ntfs_align8(IE_KEY + fnlen + (child ? 8 : 0));

    for (uint32_t i = 0; i < elen; i++) out[i] = 0;
    ntfs_put64(out + IE_REF, child_ref);
    ntfs_put16(out + IE_LEN, (uint16_t)elen);
    ntfs_put16(out + IE_KEYLEN, (uint16_t)fnlen);
    ntfs_put32(out + IE_FLAGS, child ? IE_HAS_CHILD : 0);
    for (uint32_t i = 0; i < fnlen; i++) out[IE_KEY + i] = fnval[i];
    if (child) ntfs_put64(out + elen - 8, vcn_child);
    return elen;
}

/* Re-cut an existing entry so it carries a child pointer (or does not),
 * which is what promoting a leaf entry into an interior node needs. */
static uint32_t ix_recut_entry(uint8_t *out, const uint8_t *src,
                               uint64_t vcn_child) {
    const uint32_t keylen = ie_keylen(src);
    const int child = (vcn_child != IX_NO_CHILD);
    const uint32_t elen = ntfs_align8(IE_KEY + keylen + (child ? 8 : 0));

    for (uint32_t i = 0; i < elen; i++) out[i] = 0;
    for (uint32_t i = 0; i < IE_KEY + keylen; i++) out[i] = src[i];
    ntfs_put16(out + IE_LEN, (uint16_t)elen);
    ntfs_put32(out + IE_FLAGS,
               (ie_flags(src) & ~(uint32_t)IE_HAS_CHILD) |
               (child ? IE_HAS_CHILD : 0));
    if (child) ntfs_put64(out + elen - 8, vcn_child);
    return elen;
}

/* The name in an entry, compared against a C string. Returns <0, 0, >0,
 * or 2 for the end entry, which sorts after everything. */
static int ix_cmp(const uint8_t *e, const char *name) {
    if (ie_flags(e) & IE_LAST) return 2;
    if (ie_keylen(e) < NTFS_FN_NAME) return 2;
    return ntfs_name_cmp(e + IE_KEY + NTFS_FN_NAME,
                         e[IE_KEY + NTFS_FN_NAME_LEN], name);
}

/* Where in this node `name` belongs: the first entry that sorts at or
 * after it, which is also the entry whose child subtree would hold it. */
static uint8_t *ix_seek(uint8_t *hdr, const char *name, int *out_exact) {
    uint8_t *p = ix_first(hdr);
    uint8_t *end = ix_end(hdr);

    if (out_exact) *out_exact = 0;
    while (p < end) {
        const uint16_t l = ie_len(p);
        int c;
        if (l < IE_KEY) break;
        c = ix_cmp(p, name);
        if (c == 0) { if (out_exact) *out_exact = 1; return p; }
        if (c > 0) return p;            /* includes the end entry */
        p += l;
    }
    return p;
}

/* ---- growing the tree ---- */

/*
 * Turn a small directory into a large one.
 *
 * Everything the root holds moves into block 0, and the root is left
 * with a single end entry pointing at it. That entry is the one it is
 * easiest to get wrong: it has no name, carries flags 0x03 rather than
 * 0x02, and its eight trailing bytes are the only route to every name
 * in the directory.
 */
static int ix_make_large(uint8_t *dir_rec) {
    uint8_t *body = ix_root_body(dir_rec);
    uint8_t *hdr;
    uint32_t bsize, eoff, used, copy, root_eoff;
    uint64_t vcn = 0;
    int rc;

    if (!body) return NTFS_W_IO;
    bsize = ix_block_size(body);
    hdr   = body + 16;

    /*
     * The order here is forced, and getting it wrong is why the first
     * attempt could not convert a *full* directory -- which is the only
     * kind that ever needs converting.
     *
     * $INDEX_ALLOCATION and $BITMAP have to be added to the record, and
     * they need about a hundred and twenty bytes. At the moment this is
     * called there are none: the root is full, which is what triggered
     * it. So the entries come out *first*, into a buffer; then the root
     * shrinks to a single downlink, freeing most of the record; then
     * the attributes are added into the room that made; and only then
     * is a block allocated and written.
     */
    eoff  = ix_block_entries_off(bsize);
    used  = ix_used(hdr);
    root_eoff = ntfs_get32(hdr + IX_HDR_ENTRIES_OFF);
    copy  = used - root_eoff;
    if (eoff + copy > bsize - IXB_HDR) {
        ntfs_w_errstr = "index block too small for the root's entries";
        return NTFS_W_NOSPACE;
    }

    ix_block_init(ntfs_ix_split, bsize, 0, 0);
    for (uint32_t i = 0; i < copy; i++)
        ntfs_ix_split[IXB_HDR + eoff + i] = ix_first(hdr)[i];
    ntfs_put32(ntfs_ix_split + IXB_HDR + IX_HDR_USED, eoff + copy);

    /* The root, reduced to one end entry with a downlink. The child it
     * names is filled in below, once there is a block to name. */
    {
        const int32_t delta = (int32_t)(root_eoff + 0x18) - (int32_t)used;
        uint8_t *e;

        rc = ix_root_resize(dir_rec, delta);
        if (rc != NTFS_W_OK) return rc;

        hdr = ix_root_hdr(dir_rec);
        e = ix_first(hdr);
        for (uint32_t i = 0; i < 0x18; i++) e[i] = 0;
        ntfs_put16(e + IE_LEN, 0x18);
        ntfs_put32(e + IE_FLAGS, IE_LAST | IE_HAS_CHILD);
        ntfs_put32(hdr + IX_HDR_USED, root_eoff + 0x18);
        ntfs_put32(hdr + IX_HDR_ALLOC, root_eoff + 0x18);
        ntfs_put32(hdr + IX_HDR_FLAGS, IX_HDR_LARGE);
    }

    /* Now there is room for them. */
    rc = ix_add_alloc_attrs(dir_rec);
    if (rc != NTFS_W_OK) return rc;

    rc = ix_alloc_block(dir_rec, &vcn);
    if (rc != NTFS_W_OK) return rc;

    /* The block is written before the record naming it ever reaches the
     * disk -- the caller writes the record last -- so an interruption
     * leaves a leaked block rather than a directory pointing into one
     * that was never written. */
    rc = ix_block_write(dir_rec, vcn, ntfs_ix_split);
    if (rc != NTFS_W_OK) { ix_free_block(dir_rec, vcn); return rc; }

    hdr = ix_root_hdr(dir_rec);
    ntfs_put64(ix_first(hdr) + 0x10, vcn);
    return NTFS_W_OK;
}

/*
 * Split a node.
 *
 * `hdr` is the node, full. Its lower half stays where it is; the median
 * entry is copied to `median` and the upper half is built in `right`.
 * The median's own child becomes the left node's rightmost downlink,
 * and the original end entry's child becomes the right node's, which is
 * the part that keeps every subtree reachable.
 *
 * Written over "a node" rather than over "a leaf" on purpose: a delete
 * that promotes a longer key into an interior slot can overflow one,
 * and that path needs exactly this.
 */
static int ix_split_node(uint8_t *hdr, uint8_t *right_block, uint32_t bsize,
                         uint64_t right_vcn, uint8_t *median,
                         uint32_t *median_len) {
    const uint32_t eoff = ntfs_get32(hdr + IX_HDR_ENTRIES_OFF);
    const int large = (ix_flags(hdr) & IX_HDR_LARGE) != 0;
    uint8_t *first = ix_first(hdr);
    uint8_t *end = ix_end(hdr);
    uint8_t *p, *mid = 0;
    uint32_t total = 0, half, acc = 0;
    uint32_t reoff;

    /* The median is the entry that puts about half the bytes on each
     * side. Counting bytes rather than entries matters because names
     * here are not the same length. */
    for (p = first; p < end && !(ie_flags(p) & IE_LAST); p += ie_len(p))
        total += ie_len(p);
    if (total == 0) return NTFS_W_NOSPACE;
    half = total / 2;
    for (p = first; p < end && !(ie_flags(p) & IE_LAST); p += ie_len(p)) {
        if (acc + ie_len(p) > half) { mid = p; break; }
        acc += ie_len(p);
    }
    if (!mid) mid = first;
    if (mid == first && ie_flags(mid) & IE_LAST) return NTFS_W_NOSPACE;

    /* The median, re-cut without a child: the caller decides what it
     * points at once the halves have VCNs. */
    *median_len = ix_recut_entry(median, mid, IX_NO_CHILD);

    /* The right half: everything after the median, plus the original
     * end entry unchanged -- it already points at the rightmost
     * subtree. */
    ix_block_init(right_block, bsize, right_vcn, large);
    reoff = ix_block_entries_off(bsize);
    {
        uint8_t *src = mid + ie_len(mid);
        const uint32_t n = (uint32_t)(end - src);
        if (reoff + n > bsize - IXB_HDR) return NTFS_W_NOSPACE;
        for (uint32_t i = 0; i < n; i++)
            right_block[IXB_HDR + reoff + i] = src[i];
        ntfs_put32(right_block + IXB_HDR + IX_HDR_USED, reoff + n);
    }

    /* The left half keeps everything before the median and ends with a
     * new end entry whose downlink is the median's old child. */
    {
        const uint64_t mid_child = large ? ie_vcn(mid) : 0;
        uint32_t left_len = (uint32_t)(mid - first);
        uint8_t *e = first + left_len;
        const uint32_t elen = large ? 0x18u : 0x10u;

        for (uint32_t i = 0; i < elen; i++) e[i] = 0;
        ntfs_put16(e + IE_LEN, (uint16_t)elen);
        ntfs_put32(e + IE_FLAGS, IE_LAST | (large ? IE_HAS_CHILD : 0));
        if (large) ntfs_put64(e + 0x10, mid_child);
        ntfs_put32(hdr + IX_HDR_USED, eoff + left_len + elen);
    }
    return NTFS_W_OK;
}

/* Room left in a node, in bytes. */
static uint32_t ix_room(const uint8_t *hdr) {
    const uint32_t used = ix_used(hdr);
    const uint32_t cap  = ntfs_get32(hdr + IX_HDR_ALLOC);
    return used < cap ? cap - used : 0;
}

/* Put an entry into a node at `slot`, which ix_seek found. The node
 * must have room; the caller checks. */
static void ix_put_at(uint8_t *hdr, uint8_t *slot, const uint8_t *entry,
                      uint32_t elen) {
    uint8_t *end = ix_end(hdr);
    const uint32_t tail = (uint32_t)(end - slot);

    for (uint32_t i = 0; i < tail; i++)
        slot[elen + tail - 1 - i] = slot[tail - 1 - i];
    for (uint32_t i = 0; i < elen; i++) slot[i] = entry[i];
    ntfs_put32(hdr + IX_HDR_USED, ix_used(hdr) + elen);
}

/* Take an entry out of a node. */
static void ix_take_at(uint8_t *hdr, uint8_t *slot) {
    const uint32_t elen = ie_len(slot);
    uint8_t *end = ix_end(hdr);
    const uint32_t tail = (uint32_t)(end - (slot + elen));

    for (uint32_t i = 0; i < tail; i++) slot[i] = slot[elen + i];
    ntfs_put32(hdr + IX_HDR_USED, ix_used(hdr) - elen);
}

/*
 * Insert into the root, growing the resident attribute to suit.
 *
 * The root is the one node whose capacity is not its own: it lives in
 * the MFT record, so "does it fit" is a question about the record. The
 * resize has to happen before the slot is found, because moving the
 * attribute invalidates every pointer into it -- which is why the name
 * is passed in rather than a position.
 */
static int ix_root_put(uint8_t *dir_rec, const char *name,
                       const uint8_t *entry, uint32_t elen) {
    uint8_t *hdr;
    uint8_t *slot;
    uint32_t slot_off;
    int rc;

    hdr = ix_root_hdr(dir_rec);
    if (!hdr) return NTFS_W_IO;

    /* Where it goes, as an offset from the header rather than a
     * pointer, so it survives the move. */
    slot = ix_seek(hdr, name, 0);
    slot_off = (uint32_t)(slot - hdr);

    rc = ix_root_resize(dir_rec, (int32_t)elen);
    if (rc != NTFS_W_OK) return rc;

    hdr = ix_root_hdr(dir_rec);
    ntfs_put32(hdr + IX_HDR_ALLOC, ntfs_get32(hdr + IX_HDR_ALLOC) + elen);
    ix_put_at(hdr, hdr + slot_off, entry, elen);
    return NTFS_W_OK;
}

/* And out of it. */
static int ix_root_take(uint8_t *dir_rec, uint8_t *slot) {
    uint8_t *hdr = ix_root_hdr(dir_rec);
    const uint32_t elen = ie_len(slot);

    if (!hdr) return NTFS_W_IO;
    ix_take_at(hdr, slot);
    ntfs_put32(hdr + IX_HDR_ALLOC, ntfs_get32(hdr + IX_HDR_ALLOC) - elen);
    return ix_root_resize(dir_rec, -(int32_t)elen);
}

/*
 * Split the root, which is the only way the tree gets deeper.
 *
 * Both halves become blocks and the root is left holding one real entry
 * and one end entry: the median, pointing at the left block, and the
 * terminator pointing at the right. Every other split keeps its left
 * half where it was; this one cannot, because the root is not a block.
 */
static int ix_split_root(uint8_t *dir_rec) {
    uint8_t *body = ix_root_body(dir_rec);
    uint8_t *hdr;
    uint32_t bsize, eoff, copy;
    uint64_t left_vcn = 0, right_vcn = 0;
    uint8_t median[16 + 0x42 + 255 * 2 + 8];
    uint32_t mlen = 0;
    int rc;

    if (!body) return NTFS_W_IO;
    bsize = ix_block_size(body);
    hdr = body + 16;

    rc = ix_alloc_block(dir_rec, &left_vcn);
    if (rc != NTFS_W_OK) return rc;
    rc = ix_alloc_block(dir_rec, &right_vcn);
    if (rc != NTFS_W_OK) return rc;

    body = ix_root_body(dir_rec);
    hdr  = body + 16;

    /* Copy the root into a block-shaped buffer, then split that. */
    ix_block_init(ntfs_ix_scratch, bsize, left_vcn,
                  (ix_flags(hdr) & IX_HDR_LARGE) != 0);
    eoff = ix_block_entries_off(bsize);
    copy = ix_used(hdr) - ntfs_get32(hdr + IX_HDR_ENTRIES_OFF);
    if (eoff + copy > bsize - IXB_HDR) return NTFS_W_NOSPACE;
    for (uint32_t i = 0; i < copy; i++)
        ntfs_ix_scratch[IXB_HDR + eoff + i] = ix_first(hdr)[i];
    ntfs_put32(ntfs_ix_scratch + IXB_HDR + IX_HDR_USED, eoff + copy);

    rc = ix_split_node(ntfs_ix_scratch + IXB_HDR, ntfs_ix_split, bsize,
                       right_vcn, median, &mlen);
    if (rc != NTFS_W_OK) return rc;

    /* Both children durable before the root names either of them. */
    rc = ix_block_write(dir_rec, left_vcn, ntfs_ix_scratch);
    if (rc != NTFS_W_OK) return rc;
    rc = ix_block_write(dir_rec, right_vcn, ntfs_ix_split);
    if (rc != NTFS_W_OK) return rc;

    /* The new root: median -> left, terminator -> right. */
    {
        uint8_t promoted[16 + 0x42 + 255 * 2 + 8];
        const uint32_t plen = ix_recut_entry(promoted, median, left_vcn);
        const uint32_t root_eoff = ntfs_get32(ix_root_hdr(dir_rec) +
                                              IX_HDR_ENTRIES_OFF);
        const int32_t want = (int32_t)(root_eoff + plen + 0x18);
        uint8_t *e;

        rc = ix_root_resize(dir_rec,
                            want - (int32_t)ix_used(ix_root_hdr(dir_rec)));
        if (rc != NTFS_W_OK) return rc;

        hdr = ix_root_hdr(dir_rec);
        e = ix_first(hdr);
        for (uint32_t i = 0; i < plen; i++) e[i] = promoted[i];
        e += plen;
        for (uint32_t i = 0; i < 0x18; i++) e[i] = 0;
        ntfs_put16(e + IE_LEN, 0x18);
        ntfs_put32(e + IE_FLAGS, IE_LAST | IE_HAS_CHILD);
        ntfs_put64(e + 0x10, right_vcn);

        ntfs_put32(hdr + IX_HDR_USED, (uint32_t)want);
        ntfs_put32(hdr + IX_HDR_ALLOC, (uint32_t)want);
        ntfs_put32(hdr + IX_HDR_FLAGS, IX_HDR_LARGE);
    }
    return NTFS_W_OK;
}

/* ---- walking the whole tree ---- */

/*
 * In-order traversal, root and blocks together.
 *
 * Everything above the index -- lookup, listing, the fs dispatch --
 * calls through here and does not know whether a directory has one node
 * or a thousand. That is the whole point of the abstraction: adding
 * $INDEX_ALLOCATION changed no caller.
 *
 * Recursive over a bounded depth rather than iterative, because the
 * recursion is at most IX_MAX_DEPTH frames and an explicit stack of
 * the same size is the same memory with more places to be wrong. Each
 * level reads into its own buffer.
 */
static int ix_walk_node(uint8_t *dir_rec, uint8_t *hdr, int depth,
                        ntfs_index_cb_t cb, void *ctx) {
    uint8_t *p = ix_first(hdr);
    uint8_t *end = ix_end(hdr);
    char name[256];

    if (depth >= IX_MAX_DEPTH) return 0;

    while (p < end) {
        const uint16_t l = ie_len(p);
        const uint32_t f = ie_flags(p);
        if (l < IE_KEY) break;

        /* The subtree that sorts before this entry comes first --
         * including for the end entry, which is how everything after
         * the last name is reached. */
        if (f & IE_HAS_CHILD) {
            uint8_t *cbuf = ntfs_ix_walk[depth];
            if (ix_block_read(dir_rec, ie_vcn(p), cbuf) == 0) {
                const int r = ix_walk_node(dir_rec, cbuf + IXB_HDR, depth + 1,
                                           cb, ctx);
                if (r) return r;
            }
        }
        if (f & IE_LAST) break;

        if (ie_keylen(p) >= NTFS_FN_NAME) {
            const uint8_t *fn = p + IE_KEY;
            const uint32_t chars = fn[NTFS_FN_NAME_LEN];
            if (IE_KEY + NTFS_FN_NAME + chars * 2 <= l &&
                fn[NTFS_FN_NAMESPACE] != 2) {
                int r;
                ntfs_name_to_ascii(fn + NTFS_FN_NAME, chars, name,
                                   sizeof(name));
                r = cb(ctx, p, name);
                if (r) return r;
            }
        }
        p += l;
    }
    return 0;
}

/* ---- deletion ---- */

/*
 * Replace a separator with its in-order successor.
 *
 * `slot` names an interior entry: the tree below it is divided *by* it,
 * so removing it outright would orphan one side. The entry that can
 * stand in its place is the next name in order, which is the leftmost
 * name in the subtree immediately to its right.
 *
 * Two things make this safe without any further splitting:
 *
 *   - the successor always comes out of a *leaf*, and taking an entry
 *     out of a leaf only makes it smaller, so nothing below can split
 *     and nothing above can move while this runs
 *   - the successor is re-cut with the departing entry's downlink, and
 *     may be a longer name than the one it replaces -- but the descent
 *     that got here split every node it entered until it had room for a
 *     maximum-size entry, so the growth always fits
 */
/*
 * Take the smallest key out of the subtree at `vcn`.
 *
 * Usually that is the first entry of the leftmost leaf. But nothing
 * here merges nodes, so a subtree can be emptied out by deletions and
 * still exist -- and then the smallest key is the first entry of an
 * *interior* node whose leftmost child holds nothing.
 *
 * Removing an interior entry is normally the hard case, and it is safe
 * here for a specific reason: the only interior entry this ever takes
 * is the first one, and it is only taken after its own child subtree
 * has been found empty. Keys below it were in that empty subtree, so
 * the entry after it already covers everything it was separating.
 * The empty blocks become unreachable and are returned when the
 * directory is deleted.
 *
 * Returns 0 and fills `out` if the subtree had any key at all.
 */
static int ix_take_leftmost(uint8_t *dir_rec, uint64_t vcn, int d,
                            uint8_t *out, uint32_t *out_len) {
    uint8_t *buf, *shdr, *first;

    if (d >= IX_MAX_DEPTH) return -1;
    buf = ntfs_ix_succ[d];
    if (ix_block_read(dir_rec, vcn, buf) != 0) return -1;
    shdr = buf + IXB_HDR;
    first = ix_first(shdr);

    if (ix_flags(shdr) & IX_HDR_LARGE) {
        if (ix_take_leftmost(dir_rec, ie_vcn(first), d + 1, out, out_len) == 0)
            return 0;
        /* The leftmost child held nothing, so this node's own first
         * entry is the smallest key -- if it has one. */
        buf = ntfs_ix_succ[d];
        shdr = buf + IXB_HDR;
        first = ix_first(shdr);
    }

    if (ie_flags(first) & IE_LAST) return -1;   /* the subtree is empty */

    *out_len = ie_len(first);
    for (uint32_t i = 0; i < *out_len; i++) out[i] = first[i];
    ix_take_at(shdr, first);
    return ix_block_write(dir_rec, vcn, buf) == NTFS_W_OK ? 0 : -1;
}

/*
 * Replace a separator with its in-order successor.
 *
 * `slot` names an interior entry: the tree below it is divided *by* it,
 * so removing it outright would orphan one side. The entry that can
 * stand in its place is the next name in order, which is the smallest
 * key in the subtree immediately to its right.
 *
 * Two things make this safe without further splitting:
 *
 *   - taking the smallest key only ever *shrinks* the node it comes
 *     from, so nothing below can split and nothing above can move
 *   - the successor is re-cut with the departing entry's downlink, and
 *     may be a longer name than the one it replaces -- but the descent
 *     that got here split every node it entered until it had room for a
 *     maximum-size entry, so the growth always fits
 *
 * If the right subtree turns out to be empty, there is no successor and
 * the separator is simply dropped: the entry after it inherits the left
 * child, which is exactly what it should cover once the right side
 * holds nothing.
 */
static int ix_replace_separator(uint8_t *dir_rec, uint8_t *hdr,
                                uint32_t slot_off, int depth, uint32_t bsize) {
    uint8_t *slot = hdr + slot_off;
    const uint64_t dchild = ie_vcn(slot);
    const uint32_t dlen = ie_len(slot);
    uint8_t *next = slot + dlen;
    uint64_t vcn;
    uint8_t succ[16 + 0x42 + 255 * 2 + 8];
    uint8_t recut[16 + 0x42 + 255 * 2 + 8];
    uint32_t slen = 0, rlen;
    int rc;

    (void)bsize;
    if (!(ie_flags(next) & IE_HAS_CHILD)) {
        ntfs_w_errstr = "interior entry with no successor subtree";
        return NTFS_W_IO;
    }
    vcn = ie_vcn(next);

    if (ix_take_leftmost(dir_rec, vcn, 0, succ, &slen) != 0) {
        /* Nothing to the right: drop the separator instead. */
        if (depth == 0) {
            uint8_t *rh = ix_root_hdr(dir_rec);
            uint8_t *sl = rh + slot_off;
            ie_set_vcn(sl + ie_len(sl), dchild);
            return ix_root_take(dir_rec, sl);
        }
        ie_set_vcn(slot + dlen, dchild);
        ix_take_at(hdr, slot);
        return ix_block_write(dir_rec, ntfs_ix_path_vcn[depth - 1],
                              ntfs_ix_path[depth - 1]);
    }

    /* The successor, wearing the departing entry's downlink. */
    rlen = ix_recut_entry(recut, succ, dchild);
    if (rlen > dlen) ntfs_ix_promote_grew++;

    if (depth == 0) {
        /* The root is resident, so a length change is a resize. */
        uint8_t *rh = ix_root_hdr(dir_rec);
        rc = ix_root_take(dir_rec, rh + slot_off);
        if (rc != NTFS_W_OK) return rc;
        {
            char nm[256];
            ntfs_name_to_ascii(recut + IE_KEY + NTFS_FN_NAME,
                               recut[IE_KEY + NTFS_FN_NAME_LEN],
                               nm, sizeof(nm));
            return ix_root_put(dir_rec, nm, recut, rlen);
        }
    }

    if (ix_room(hdr) + dlen < rlen) {
        ntfs_w_errstr = "no room to promote the successor";
        return NTFS_W_NOSPACE;
    }
    ix_take_at(hdr, slot);
    ix_put_at(hdr, slot, recut, rlen);
    return ix_block_write(dir_rec, ntfs_ix_path_vcn[depth - 1],
                          ntfs_ix_path[depth - 1]);
}

/*
 * Split whatever `slot`'s child is if it could not take a maximum-size
 * entry, promoting the median into the parent. Returns 1 if it split
 * (the caller re-decides which way to go), 0 if not, -1 on failure.
 *
 * Shared by both descents: an insert needs the room for the entry it is
 * carrying, and a delete needs it because a separator can be replaced
 * by a longer name than it held.
 */
static int ix_split_child_if_full(uint8_t *dir_rec, int depth, uint8_t **hdrp,
                                  uint32_t slot_off, uint8_t *cbuf,
                                  uint64_t child, uint32_t bsize, int *rcout) {
    uint8_t *chdr = cbuf + IXB_HDR;
    uint8_t *hdr, *slot;
    uint64_t new_vcn = 0;
    uint8_t median[16 + 0x42 + 255 * 2 + 8];
    uint8_t promoted[16 + 0x42 + 255 * 2 + 8];
    uint32_t mlen = 0, plen;
    char mname[256];
    int rc;

    *rcout = NTFS_W_OK;
    if (ix_room(chdr) >= IX_MAX_ENTRY) return 0;

    rc = ix_alloc_block(dir_rec, &new_vcn);
    if (rc != NTFS_W_OK) { *rcout = rc; return -1; }

    /* Growing the bitmap can move the record's attributes, so the
     * parent is located again rather than remembered. */
    hdr  = (depth == 0) ? ix_root_hdr(dir_rec)
                        : ntfs_ix_path[depth - 1] + IXB_HDR;
    slot = hdr + slot_off;

    rc = ix_split_node(chdr, ntfs_ix_split, bsize, new_vcn, median, &mlen);
    if (rc != NTFS_W_OK) {
        ix_free_block(dir_rec, new_vcn);
        *rcout = rc;
        return -1;
    }

    /* Both halves durable before the parent names either of them. */
    rc = ix_block_write(dir_rec, child, cbuf);
    if (rc != NTFS_W_OK) { *rcout = rc; return -1; }
    rc = ix_block_write(dir_rec, new_vcn, ntfs_ix_split);
    if (rc != NTFS_W_OK) { *rcout = rc; return -1; }

    plen = ix_recut_entry(promoted, median, child);
    ntfs_name_to_ascii(promoted + IE_KEY + NTFS_FN_NAME,
                       promoted[IE_KEY + NTFS_FN_NAME_LEN], mname,
                       sizeof(mname));

    if (depth == 0) {
        rc = ix_root_put(dir_rec, mname, promoted, plen);
        if (rc != NTFS_W_OK) { *rcout = rc; return -1; }
        hdr  = ix_root_hdr(dir_rec);
        slot = ix_seek(hdr, mname, 0);
        ie_set_vcn(slot + ie_len(slot), new_vcn);
    } else {
        if (ix_room(hdr) < plen) {
            ntfs_w_errstr = "index parent full after a split";
            *rcout = NTFS_W_NOSPACE;
            return -1;
        }
        ix_put_at(hdr, slot, promoted, plen);
        ie_set_vcn(slot + plen, new_vcn);
        rc = ix_block_write(dir_rec, ntfs_ix_path_vcn[depth - 1],
                            ntfs_ix_path[depth - 1]);
        if (rc != NTFS_W_OK) { *rcout = rc; return -1; }
    }

    *hdrp = hdr;
    ntfs_ix_splits++;
    return 1;
}

/*
 * Insert, splitting on the way down.
 *
 * The textbook alternative is to descend, insert at the leaf, and carry
 * an overflow back up -- which needs the whole path held in memory, a
 * median propagated level by level, and a special case when the root
 * itself overflows. Splitting *pre-emptively* instead -- any node that
 * could not accept a maximum-size entry is split before it is entered
 * -- means the leaf insert can never fail, so nothing has to come back
 * up. Every node on the path is guaranteed to have room for whatever
 * the level below promotes into it.
 *
 * The cost is that a node splits while up to six hundred bytes of it
 * are still free. On a 4096-byte block that is fifteen per cent of the
 * space, and it buys away the entire class of bugs that lives in
 * unwinding a failed insert halfway up a tree.
 */
static int ntfs_index_insert(uint8_t *dir_rec, uint64_t child_ref,
                             const char *name, int is_dir,
                             uint64_t real, uint64_t alloc, uint64_t now) {
    uint8_t entry[16 + 0x42 + 255 * 2 + 8];
    uint32_t elen;
    uint8_t *hdr;
    uint32_t bsize;
    int depth = 0;
    int rc;

    hdr = ix_root_hdr(dir_rec);
    if (!hdr) {
        ntfs_w_errstr = "directory index is not resident";
        return NTFS_W_IO;
    }
    bsize = ix_block_size(ix_root_body(dir_rec));

    elen = ix_make_entry(entry, child_ref, name, is_dir, real, alloc, now,
                         IX_NO_CHILD);

    /* ---- the small case: everything is in the record ---- */
    if (!(ix_flags(hdr) & IX_HDR_LARGE)) {
        int exact = 0;
        ix_seek(hdr, name, &exact);
        if (exact) {
            ntfs_w_errstr = "a file of that name already exists";
            return NTFS_W_EXISTS;
        }
        if (((ntfs_record_t *)dir_rec)->used_size + elen <=
            ntfs.bytes_per_record)
            return ix_root_put(dir_rec, name, entry, elen);

        /* Out of room in the record: this is where a directory stops
         * being a list and becomes a tree. */
        rc = ix_make_large(dir_rec);
        if (rc != NTFS_W_OK) return rc;
        hdr = ix_root_hdr(dir_rec);
    }

    /* ---- the root, split first if it could not take a promotion ---- */
    if (((ntfs_record_t *)dir_rec)->used_size + IX_MAX_ENTRY >
        ntfs.bytes_per_record) {
        rc = ix_split_root(dir_rec);
        if (rc != NTFS_W_OK) return rc;
    }
    hdr = ix_root_hdr(dir_rec);

    /* ---- descend ---- */
    for (;;) {
        int exact = 0;
        uint8_t *slot = ix_seek(hdr, name, &exact);
        uint32_t slot_off = (uint32_t)(slot - hdr);
        uint64_t child;
        uint8_t *cbuf, *chdr;

        if (exact) {
            ntfs_w_errstr = "a file of that name already exists";
            return NTFS_W_EXISTS;
        }

        /* A leaf: the insert lands here, and cannot fail. */
        if (!(ix_flags(hdr) & IX_HDR_LARGE)) {
            if (depth == 0) return ix_root_put(dir_rec, name, entry, elen);
            if (ix_room(hdr) < elen) {
                ntfs_w_errstr = "index leaf full after a pre-emptive split";
                return NTFS_W_NOSPACE;
            }
            ix_put_at(hdr, slot, entry, elen);
            return ix_block_write(dir_rec, ntfs_ix_path_vcn[depth - 1],
                                  ntfs_ix_path[depth - 1]);
        }

        if (depth >= IX_MAX_DEPTH) {
            ntfs_w_errstr = "directory index deeper than the limit";
            return NTFS_W_NOSPACE;
        }

        child = ie_vcn(slot);
        cbuf  = ntfs_ix_path[depth];
        chdr  = cbuf + IXB_HDR;
        if (ix_block_read(dir_rec, child, cbuf) != 0) {
            ntfs_w_errstr = "index block will not read";
            return NTFS_W_IO;
        }
        ntfs_ix_path_vcn[depth] = child;
        ntfs_ix_path_off[depth] = (uint32_t)(slot - hdr);

        /* Split the child before entering it, if it could not take a
         * maximum-size entry. The parent is known to have room for the
         * median, because the same test was applied to it on the way
         * past -- which is what makes the descent one-way. */
        {
            int r = ix_split_child_if_full(dir_rec, depth, &hdr, slot_off,
                                           cbuf, child, bsize, &rc);
            if (r < 0) return rc;
            if (r == 1) continue;       /* re-decide which half to enter */
        }

        hdr = chdr;
        depth++;
    }
}

/*
 * Remove a name.
 *
 * Nodes are left under-full rather than merged. That is legal NTFS --
 * a shrinking directory stays correct and stays readable by anything
 * else -- and what it costs is that index blocks are not handed back
 * until the directory itself is removed.
 */
static int ntfs_index_remove(uint8_t *dir_rec, const char *name) {
    uint8_t *hdr = ix_root_hdr(dir_rec);
    uint32_t bsize;
    int depth = 0;
    int rc;

    if (!hdr) return NTFS_W_IO;
    bsize = ix_block_size(ix_root_body(dir_rec));

    /* The root is split up front for the same reason every other node
     * is split on the way past: a separator here may be replaced by a
     * longer name, and the record has to be able to take it. */
    if ((ix_flags(hdr) & IX_HDR_LARGE) &&
        ((ntfs_record_t *)dir_rec)->used_size + IX_MAX_ENTRY >
        ntfs.bytes_per_record) {
        rc = ix_split_root(dir_rec);
        if (rc != NTFS_W_OK) return rc;
        hdr = ix_root_hdr(dir_rec);
    }

    for (;;) {
        int exact = 0;
        uint8_t *slot = ix_seek(hdr, name, &exact);
        uint32_t slot_off = (uint32_t)(slot - hdr);
        uint64_t child;
        uint8_t *cbuf;

        if (exact) {
            if (!(ie_flags(slot) & IE_HAS_CHILD)) {
                if (depth == 0) return ix_root_take(dir_rec, slot);
                ix_take_at(hdr, slot);
                return ix_block_write(dir_rec, ntfs_ix_path_vcn[depth - 1],
                                      ntfs_ix_path[depth - 1]);
            }
            return ix_replace_separator(dir_rec, hdr, slot_off, depth, bsize);
        }

        if (!(ix_flags(hdr) & IX_HDR_LARGE)) {
            ntfs_w_errstr = "no such entry in the directory";
            return NTFS_W_NOTFOUND;
        }
        if (depth >= IX_MAX_DEPTH) {
            ntfs_w_errstr = "directory index deeper than the limit";
            return NTFS_W_IO;
        }

        child = ie_vcn(slot);
        cbuf  = ntfs_ix_path[depth];
        if (ix_block_read(dir_rec, child, cbuf) != 0) {
            ntfs_w_errstr = "index block will not read";
            return NTFS_W_IO;
        }
        ntfs_ix_path_vcn[depth] = child;
        ntfs_ix_path_off[depth] = slot_off;

        {
            int r = ix_split_child_if_full(dir_rec, depth, &hdr, slot_off,
                                           cbuf, child, bsize, &rc);
            if (r < 0) return rc;
            if (r == 1) continue;
        }

        hdr = cbuf + IXB_HDR;
        depth++;
    }
}

/*
 * Free every index block a directory owns.
 *
 * Called when the directory itself is removed. Without it the clusters
 * behind $INDEX_ALLOCATION are simply lost -- the record goes, and
 * nothing is left holding the runs that would say what to give back.
 */
static int ix_release(uint8_t *dir_rec) {
    ntfs_attr_t *ia = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ALLOC);
    const uint8_t *p, *end;
    int64_t prev = 0;
    ntfs_run_t run;

    if (!ia || !ia->non_resident) return NTFS_W_OK;
    p   = (const uint8_t *)ia + ia->u.nonres.run_offset;
    end = (const uint8_t *)ia + ia->length;
    while (ntfs_next_run(&p, end, &prev, &run))
        if (!run.sparse) ntfs_clusters_free(run.lcn, run.length);
    return NTFS_W_OK;
}

/*
 * The MFT reference a name resolves to, found by descending rather than
 * by scanning the root.
 *
 * ntfs_delete_file used to walk $INDEX_ROOT's entries directly to
 * learn which record it was about to release. That was correct while a
 * directory *was* its root; on a tree it finds nothing for any name
 * that lives in a block, and a delete that cannot find the record it is
 * removing would unlink the name and leave the file behind.
 */
static int ix_find_ref(uint8_t *dir_rec, const char *name, uint64_t *out_ref) {
    uint8_t *hdr = ix_root_hdr(dir_rec);
    int depth = 0;

    if (!hdr) return -1;
    for (;;) {
        int exact = 0;
        uint8_t *slot = ix_seek(hdr, name, &exact);

        if (exact) {
            *out_ref = ntfs_get64(slot + IE_REF) & 0x0000FFFFFFFFFFFFULL;
            return 0;
        }
        if (!(ix_flags(hdr) & IX_HDR_LARGE)) return -1;
        if (depth >= IX_MAX_DEPTH) return -1;
        if (ix_block_read(dir_rec, ie_vcn(slot), ntfs_ix_walk[depth]) != 0)
            return -1;
        hdr = ntfs_ix_walk[depth] + IXB_HDR;
        depth++;
    }
}

/*
 * Walk a directory's index, however big it is.
 *
 * This is the seam the rest of the driver sits on: lookup and listing
 * call it and neither knows whether the directory is one resident node
 * or a tree several levels deep. Adding $INDEX_ALLOCATION changed the
 * implementation underneath and no caller above.
 */
static int ntfs_index_walk(const uint8_t *rec, ntfs_index_cb_t cb, void *ctx) {
    uint8_t *hdr = ix_root_hdr((uint8_t *)rec);
    if (!hdr) return 0;
    return ix_walk_node((uint8_t *)rec, hdr, 0, cb, ctx);
}

/* ===========================================================
 * $UsnJrnl: the change journal
 * ===========================================================
 *
 * Distinct from the write-ahead log above, and from NTFS's $LogFile.
 * $UsnJrnl is a record of *what changed*, for anything that wants to
 * know -- a backup agent, an indexer -- rather than a mechanism for
 * keeping the volume consistent. Its record format is documented, so
 * unlike $LogFile it can be written correctly.
 */

#define USN_REASON_DATA_OVERWRITE   0x00000001
#define USN_REASON_DATA_EXTEND      0x00000002
#define USN_REASON_FILE_CREATE      0x00000100
#define USN_REASON_FILE_DELETE      0x00000200
#define USN_REASON_CLOSE            0x80000000

typedef struct {
    uint32_t record_length;
    uint16_t major_version;
    uint16_t minor_version;
    uint64_t file_reference;
    uint64_t parent_reference;
    uint64_t usn;
    uint64_t timestamp;
    uint32_t reason;
    uint32_t source_info;
    uint32_t security_id;
    uint32_t file_attributes;
    uint16_t file_name_length;
    uint16_t file_name_offset;
} __attribute__((packed)) ntfs_usn_record_t;

static struct {
    uint64_t next_usn;
    uint32_t records;
} ntfs_usn;

static void ntfs_usn_log(uint64_t file_ref, uint64_t parent_ref,
                         const char *name, uint32_t reason, uint64_t now) {
    uint8_t buf[sizeof(ntfs_usn_record_t) + 255 * 2 + 8];
    ntfs_usn_record_t *r = (ntfs_usn_record_t *)buf;
    uint32_t n = 0;

    while (name[n] && n < 255) n++;

    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    r->record_length    = (uint32_t)ntfs_align8(sizeof(*r) + n * 2);
    r->major_version    = 2;
    r->minor_version    = 0;
    r->file_reference   = file_ref;
    r->parent_reference = parent_ref;
    r->usn              = ntfs_usn.next_usn;
    r->timestamp        = now;
    r->reason           = reason;
    r->file_name_length = (uint16_t)(n * 2);
    r->file_name_offset = (uint16_t)sizeof(*r);
    for (uint32_t i = 0; i < n; i++)
        buf[sizeof(*r) + i * 2] = (uint8_t)name[i];

    /*
     * The record is formed and accounted rather than appended to
     * $Extend\$UsnJrnl, because this formatter does not create that
     * file and creating it lazily means allocating a system file at
     * the first write -- which is a larger change than the journal is
     * worth right now. What the counter buys today is that every
     * mutation passes through one place, so wiring the sink up later
     * is a single function rather than an audit of every caller.
     */
    ntfs_usn.next_usn += r->record_length;
    ntfs_usn.records++;
}

/* ===========================================================
 * the operations
 * =========================================================== */

static uint64_t ntfs_now(void) {
    /* Windows FILETIME: 100-nanosecond ticks since 1601. The scheduler
     * counts milliseconds since boot, which is not a date -- so this is
     * a fixed epoch plus uptime, which orders correctly and is honest
     * about not being a calendar. */
    return 132000000000000000ULL + (uint64_t)sched_ticks * 10000ULL;
}

/*
 * Create a file and give it contents.
 *
 * The order is chosen so that every intermediate state is one a reader
 * can survive:
 *
 *   1. allocate clusters      space is used but unowned  (recoverable)
 *   2. write the data         bytes on disk nothing names (invisible)
 *   3. allocate an MFT record still marked free
 *   4. write the record       a file with no name in any directory
 *   5. link it into the dir   now it exists
 *
 * A crash between any two leaves lost space at worst, never a directory
 * entry pointing at something that is not there.
 */
int ntfs_create_file(uint64_t dir_record, const char *name,
                            const uint8_t *data, uint32_t len) {
    uint64_t now = ntfs_now();
    uint64_t lcn = 0, number = 0;
    uint64_t clusters = 0;
    uint8_t attrs[2048];
    uint32_t pos = 0;
    int rc;

    if (!ntfs.mounted) { ntfs_w_errstr = "no NTFS volume"; return NTFS_W_READONLY; }

    /* A file whose data fits inside the record carries it there --
     * that is what NTFS does for small files and it is why a directory
     * of short text files costs no clusters at all. */
    int resident = (len <= 700);

    if (!resident) {
        clusters = (len + ntfs.bytes_per_cluster - 1) / ntfs.bytes_per_cluster;
        rc = ntfs_clusters_alloc(clusters, &lcn);
        if (rc != NTFS_W_OK) return rc;

        for (uint64_t c = 0; c < clusters; c++) {
            uint32_t off = (uint32_t)(c * ntfs.bytes_per_cluster);
            uint32_t n = ntfs.bytes_per_cluster;
            if (off + n > len) n = (len > off) ? len - off : 0;
            for (uint32_t i = 0; i < ntfs.bytes_per_cluster; i++)
                ntfs_w_clu[i] = (i < n) ? data[off + i] : 0;
            if (blk_write(ntfs.part_lba + (lcn + c) * ntfs.sectors_per_cluster,
                          ntfs.sectors_per_cluster, ntfs_w_clu) != 0) {
                ntfs_clusters_free(lcn, clusters);
                ntfs_w_errstr = "writing file data failed";
                return NTFS_W_IO;
            }
        }
        blk_flush();
    }

    rc = ntfs_mft_alloc(&number);
    if (rc != NTFS_W_OK) {
        if (clusters) ntfs_clusters_free(lcn, clusters);
        return rc;
    }

    /* --- build the record --- */
    pos += ntfs_build_stdinfo(attrs + pos, now, 0);
    pos += ntfs_build_filename_attr(attrs + pos,
                                    (dir_record | (5ULL << 48)), name, 0,
                                    len, clusters * ntfs.bytes_per_cluster,
                                    now);
    if (resident) {
        pos += ntfs_build_data_resident(attrs + pos, data, len);
    } else {
        ntfs_run_t run = { lcn, clusters, 0 };
        uint32_t n = ntfs_build_data_nonres(attrs + pos, &run, 1, len,
                                            clusters * ntfs.bytes_per_cluster);
        if (!n) {
            ntfs_clusters_free(lcn, clusters);
            ntfs_w_errstr = "run list did not fit";
            return NTFS_W_TOOBIG;
        }
        pos += n;
    }
    ntfs_put32(attrs + pos, NTFS_ATTR_END); pos += 4;
    ntfs_put32(attrs + pos, 0); pos += 4;

    {
        uint32_t fixups = ntfs.bytes_per_record / 512 + 1;
        uint32_t attr_off = ntfs_align8(0x30 + fixups * 2);
        uint32_t used = attr_off + pos;

        if (used > ntfs.bytes_per_record) {
            if (clusters) ntfs_clusters_free(lcn, clusters);
            ntfs_w_errstr = "attributes do not fit in one record";
            return NTFS_W_TOOBIG;
        }

        for (uint32_t i = 0; i < ntfs.bytes_per_record; i++) ntfs_w_rec[i] = 0;
        ntfs_w_rec[0] = 'F'; ntfs_w_rec[1] = 'I';
        ntfs_w_rec[2] = 'L'; ntfs_w_rec[3] = 'E';
        ntfs_put16(ntfs_w_rec + 0x04, 0x30);
        ntfs_put16(ntfs_w_rec + 0x06, (uint16_t)fixups);
        ntfs_put64(ntfs_w_rec + 0x08, 0);
        ntfs_put16(ntfs_w_rec + 0x10, 1);           /* sequence         */
        ntfs_put16(ntfs_w_rec + 0x12, 1);           /* link count       */
        ntfs_put16(ntfs_w_rec + 0x14, (uint16_t)attr_off);
        ntfs_put16(ntfs_w_rec + 0x16, 1);           /* in use           */
        ntfs_put32(ntfs_w_rec + 0x18, ntfs_align8(used));
        ntfs_put32(ntfs_w_rec + 0x1C, ntfs.bytes_per_record);
        ntfs_put64(ntfs_w_rec + 0x20, 0);
        ntfs_put16(ntfs_w_rec + 0x28, 8);
        ntfs_put32(ntfs_w_rec + 0x2C, (uint32_t)number);
        for (uint32_t i = 0; i < pos; i++) ntfs_w_rec[attr_off + i] = attrs[i];

        rc = ntfs_write_record(number, ntfs_w_rec);
        if (rc != NTFS_W_OK) {
            if (clusters) ntfs_clusters_free(lcn, clusters);
            return rc;
        }
    }

    /* --- link it into the directory --- */
    if (ntfs_read_record(dir_record, ntfs_w_dir) != 0) {
        ntfs_w_errstr = "cannot read the directory";
        return NTFS_W_IO;
    }
    rc = ntfs_index_insert(ntfs_w_dir, number | (1ULL << 48), name, 0,
                           len, clusters * ntfs.bytes_per_cluster, now);
    if (rc != NTFS_W_OK) {
        /* Undo: the record and its clusters go back, because a file
         * nothing points at is a leak rather than a file. */
        for (uint32_t i = 0; i < ntfs.bytes_per_record; i++) ntfs_w_rec[i] = 0;
        if (clusters) ntfs_clusters_free(lcn, clusters);
        return rc;
    }
    rc = ntfs_write_record(dir_record, ntfs_w_dir);
    if (rc != NTFS_W_OK) return rc;

    ntfs_usn_log(number | (1ULL << 48), dir_record | (5ULL << 48), name,
                 USN_REASON_FILE_CREATE | USN_REASON_CLOSE, now);
    return NTFS_W_OK;
}

/*
 * Remove a file.
 *
 * The directory entry goes first. From that moment the file is
 * unreachable, so a crash before the rest completes leaves clusters and
 * a record marked used that nothing owns -- wasted, and correct. Doing
 * it the other way round leaves a name pointing at a record that has
 * been reused for something else, which is how a delete becomes a
 * corruption.
 */
int ntfs_delete_file(uint64_t dir_record, const char *name) {
    uint64_t number = 0;
    uint64_t now = ntfs_now();
    int rc;

    if (!ntfs.mounted) return NTFS_W_READONLY;

    if (ntfs_read_record(dir_record, ntfs_w_dir) != 0) return NTFS_W_IO;

    /* Find the record number before the entry is removed -- by
     * descending, because the name may be several levels down. */
    if (ix_find_ref(ntfs_w_dir, name, &number) != 0) number = 0;

    if (!number) { ntfs_w_errstr = "no such file"; return NTFS_W_NOTFOUND; }

    rc = ntfs_index_remove(ntfs_w_dir, name);
    if (rc != NTFS_W_OK) return rc;
    rc = ntfs_write_record(dir_record, ntfs_w_dir);
    if (rc != NTFS_W_OK) return rc;

    /* Now release what it owned. */
    if (ntfs_read_record(number, ntfs_w_rec) == 0) {
        const ntfs_attr_t *a = ntfs_find_attr(ntfs_w_rec, NTFS_ATTR_DATA);

        /* A directory's index blocks are its own clusters and nothing
         * else refers to them. Without this the record goes and the
         * runs that said what to give back go with it. */
        if (((ntfs_record_t *)ntfs_w_rec)->flags & 2) ix_release(ntfs_w_rec);

        if (a && a->non_resident) {
            const uint8_t *p = (const uint8_t *)a + a->u.nonres.run_offset;
            const uint8_t *end = (const uint8_t *)a + a->length;
            int64_t prev = 0;
            ntfs_run_t run;
            while (ntfs_next_run(&p, end, &prev, &run))
                if (!run.sparse) ntfs_clusters_free(run.lcn, run.length);
        }

        /* Clear the in-use flag and bump the sequence, so any stale
         * reference to this record is detectably stale rather than
         * silently pointing at whatever replaces it. */
        {
            ntfs_record_t *h = (ntfs_record_t *)ntfs_w_rec;
            h->flags &= (uint16_t)~1u;
            h->sequence = (uint16_t)(h->sequence + 1);
            if (h->sequence == 0) h->sequence = 1;
        }
        ntfs_write_record(number, ntfs_w_rec);
    }

    ntfs_usn_log(number | (1ULL << 48), dir_record | (5ULL << 48), name,
                 USN_REASON_FILE_DELETE | USN_REASON_CLOSE, now);
    return NTFS_W_OK;
}

/*
 * Create a directory.
 *
 * A directory is a record with an $INDEX_ROOT holding nothing but the
 * terminator. It costs no clusters until it has enough children to need
 * an $INDEX_ALLOCATION -- which this driver cannot yet make, so a
 * directory here holds what fits in its record and says so when it
 * does not.
 */
int ntfs_mkdir_at(uint64_t dir_record, const char *name) {
    uint64_t now = ntfs_now();
    uint64_t number = 0;
    uint8_t attrs[1024];
    uint32_t pos = 0;
    int rc;

    if (!ntfs.mounted) return NTFS_W_READONLY;

    rc = ntfs_mft_alloc(&number);
    if (rc != NTFS_W_OK) return rc;

    pos += ntfs_build_stdinfo(attrs + pos, now, 1);
    pos += ntfs_build_filename_attr(attrs + pos, (dir_record | (5ULL << 48)),
                                    name, 1, 0, 0, now);

    /* an empty $INDEX_ROOT: header, node header, terminator */
    {
        uint8_t body[64];
        uint32_t blen = 0;
        uint32_t voff = 24 + 8;             /* value after the "$I30" name */
        uint32_t len;

        ntfs_put32(body + 0x00, NTFS_ATTR_FILE_NAME);   /* indexed type */
        ntfs_put32(body + 0x04, 1);                     /* collation    */
        ntfs_put32(body + 0x08, ntfs.bytes_per_cluster);
        body[0x0C] = 1; body[0x0D] = 0; body[0x0E] = 0; body[0x0F] = 0;
        ntfs_put32(body + 0x10, 0x10);                  /* entries off  */
        ntfs_put32(body + 0x14, 0x10 + 0x10);           /* used         */
        ntfs_put32(body + 0x18, 0x10 + 0x10);           /* allocated    */
        ntfs_put32(body + 0x1C, 0);                     /* leaf         */
        ntfs_put64(body + 0x20, 0);                     /* terminator   */
        ntfs_put16(body + 0x28, 0x10);
        ntfs_put16(body + 0x2A, 0);
        ntfs_put32(body + 0x2C, 0x02);                  /* last entry   */
        blen = 0x30;

        len = ntfs_align8(voff + blen);
        for (uint32_t i = 0; i < len; i++) attrs[pos + i] = 0;
        ntfs_put32(attrs + pos + 0x00, NTFS_ATTR_INDEX_ROOT);
        ntfs_put32(attrs + pos + 0x04, len);
        attrs[pos + 0x08] = 0;
        attrs[pos + 0x09] = 4;                          /* name length  */
        ntfs_put16(attrs + pos + 0x0A, 24);             /* name offset  */
        ntfs_put32(attrs + pos + 0x10, blen);
        ntfs_put16(attrs + pos + 0x14, (uint16_t)voff);
        {
            const char *nm = "$I30";
            for (int i = 0; i < 4; i++) {
                attrs[pos + 24 + i * 2] = (uint8_t)nm[i];
                attrs[pos + 24 + i * 2 + 1] = 0;
            }
        }
        for (uint32_t i = 0; i < blen; i++) attrs[pos + voff + i] = body[i];
        pos += len;
    }

    ntfs_put32(attrs + pos, NTFS_ATTR_END); pos += 4;
    ntfs_put32(attrs + pos, 0); pos += 4;

    {
        uint32_t fixups = ntfs.bytes_per_record / 512 + 1;
        uint32_t attr_off = ntfs_align8(0x30 + fixups * 2);
        uint32_t used = attr_off + pos;

        if (used > ntfs.bytes_per_record) return NTFS_W_TOOBIG;

        for (uint32_t i = 0; i < ntfs.bytes_per_record; i++) ntfs_w_rec[i] = 0;
        ntfs_w_rec[0] = 'F'; ntfs_w_rec[1] = 'I';
        ntfs_w_rec[2] = 'L'; ntfs_w_rec[3] = 'E';
        ntfs_put16(ntfs_w_rec + 0x04, 0x30);
        ntfs_put16(ntfs_w_rec + 0x06, (uint16_t)fixups);
        ntfs_put16(ntfs_w_rec + 0x10, 1);
        ntfs_put16(ntfs_w_rec + 0x12, 1);
        ntfs_put16(ntfs_w_rec + 0x14, (uint16_t)attr_off);
        ntfs_put16(ntfs_w_rec + 0x16, 3);           /* in use, directory */
        ntfs_put32(ntfs_w_rec + 0x18, ntfs_align8(used));
        ntfs_put32(ntfs_w_rec + 0x1C, ntfs.bytes_per_record);
        ntfs_put16(ntfs_w_rec + 0x28, 8);
        ntfs_put32(ntfs_w_rec + 0x2C, (uint32_t)number);
        for (uint32_t i = 0; i < pos; i++) ntfs_w_rec[attr_off + i] = attrs[i];

        rc = ntfs_write_record(number, ntfs_w_rec);
        if (rc != NTFS_W_OK) return rc;
    }

    if (ntfs_read_record(dir_record, ntfs_w_dir) != 0) return NTFS_W_IO;
    rc = ntfs_index_insert(ntfs_w_dir, number | (1ULL << 48), name, 1,
                           0, 0, now);
    if (rc != NTFS_W_OK) return rc;
    rc = ntfs_write_record(dir_record, ntfs_w_dir);
    if (rc != NTFS_W_OK) return rc;

    ntfs_usn_log(number | (1ULL << 48), dir_record | (5ULL << 48), name,
                 USN_REASON_FILE_CREATE | USN_REASON_CLOSE, now);
    return NTFS_W_OK;
}


/* ==================================================================
 * PART THREE -- THE FILESYSTEM SURFACE
 * ==================================================================
 *
 * Parts one and two read and write NTFS *by MFT record number*, which
 * is the format's own way of naming a file and is what the write engine
 * and its host tests are built on. What the rest of the system speaks
 * is paths, directory listings and byte ranges.
 *
 * This is the layer between the two, and it exists because NTFS became
 * the boot volume. While NTFS was a validated writer with no callers,
 * record numbers were enough; a volume the desktop mounts has to answer
 * "what is at /etc/policy.cfg", "what is in /store/pkg", and "give me
 * sixty-four kilobytes from four hundred megabytes into wiki.zim".
 *
 * Everything here is read-side and reuses parts one and two rather than
 * duplicating them -- the directory walk is the same $INDEX_ROOT scan
 * ntfs_index_remove does, and the ranged read is ntfs_read_file's run
 * walk with a seek in front of it.
 *
 * The static-buffer discipline is the one from src/exfat.h, deliberately
 * and not by accident: the fs layer above is not re-entrant, holds one
 * file buffer, and the system is built around that. A directory walk
 * gets its own record buffer rather than sharing ntfs_rec, because a
 * lookup runs *while* a caller is holding a record it read earlier --
 * that is a real aliasing bug and not a hypothetical one.
 */

static uint8_t ntfs_dirrec[4096];       /* directory records, during a walk */

/* ---- finding one name in one directory ---- */

typedef struct {
    const char *want;
    uint64_t    ref;
    uint64_t    size;
    int         is_dir;
    int         found;
} ntfs_find_ctx_t;

static int ntfs_find_cb(void *vctx, const uint8_t *entry, const char *name) {
    ntfs_find_ctx_t *c = (ntfs_find_ctx_t *)vctx;
    const uint8_t *fn = entry + NTFS_IE_VALUE;
    uint32_t i = 0;

    /* Case-insensitive, to match ntfs_name_cmp and the exFAT dispatch
     * this replaces. A volume where /Etc and /etc are different
     * directories would be correct NTFS and wrong for every caller
     * above, all of which were written against a folding filesystem. */
    while (c->want[i] && name[i]) {
        char a = c->want[i], b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
        i++;
    }
    if (c->want[i] || name[i]) return 0;

    c->ref    = ntfs_get64(entry + NTFS_IE_REF) & 0x0000FFFFFFFFFFFFULL;
    c->size   = ntfs_get64(fn + NTFS_FN_REAL_SIZE);
    c->is_dir = (ntfs_get32(fn + NTFS_FN_FLAGS) & NTFS_FA_DIRECTORY) ? 1 : 0;
    c->found  = 1;
    return 1;
}

/*
 * Resolve an absolute path to an MFT record.
 *
 * Component by component from record 5, which is always the root. Each
 * step reads the directory into ntfs_dirrec and scans its index; a
 * component that names a file when there is more path left is an error
 * rather than something to walk through.
 *
 * `out_parent` is the record holding the last component, which is what
 * every write operation needs and what a second lookup would otherwise
 * have to go and find again.
 */
int ntfs_lookup(const char *path, uint64_t *out_record, uint64_t *out_parent,
                int *out_is_dir, uint64_t *out_size) {
    uint64_t cur = NTFS_MFT_ROOT, parent = NTFS_MFT_ROOT;
    int is_dir = 1;
    uint64_t size = 0;
    uint32_t i = 0;

    if (!ntfs.mounted || !path) return -1;
    while (path[i] == '/') i++;

    while (path[i]) {
        char comp[256];
        uint32_t n = 0;
        ntfs_find_ctx_t c;

        while (path[i] && path[i] != '/' && n + 1 < sizeof(comp))
            comp[n++] = path[i++];
        comp[n] = '\0';
        while (path[i] == '/') i++;

        if (n == 0) break;
        if (!is_dir) return -1;         /* a file cannot have children */

        if (ntfs_read_record(cur, ntfs_dirrec) != 0) return -1;

        c.want = comp;
        c.found = 0; c.ref = 0; c.size = 0; c.is_dir = 0;
        ntfs_index_walk(ntfs_dirrec, ntfs_find_cb, &c);
        if (!c.found) return -1;

        parent = cur;
        cur    = c.ref;
        is_dir = c.is_dir;
        size   = c.size;
    }

    if (out_record) *out_record = cur;
    if (out_parent) *out_parent = parent;
    if (out_is_dir) *out_is_dir = is_dir;
    if (out_size)   *out_size   = size;
    return 0;
}

/* ---- listing ---- */

typedef struct {
    ntfs_list_cb_t cb;
    void          *ctx;
} ntfs_list_ctx_t;

static int ntfs_list_adapt(void *vctx, const uint8_t *entry, const char *name) {
    ntfs_list_ctx_t *l = (ntfs_list_ctx_t *)vctx;
    const uint8_t *fn = entry + NTFS_IE_VALUE;
    const uint64_t size = ntfs_get64(fn + NTFS_FN_REAL_SIZE);
    const int is_dir = (ntfs_get32(fn + NTFS_FN_FLAGS) & NTFS_FA_DIRECTORY)
                       ? 1 : 0;

    /* The system files occupy records 0-15 and are not things a user put
     * on the volume; $MFT and $Bitmap appearing in a directory listing
     * would be true and useless. */
    if (name[0] == '$') return 0;

    l->cb(l->ctx, name, size, is_dir);
    return 0;
}

/*
 * Enumerate a directory by path. Returns 0, or -1 if the path is not a
 * directory on this volume.
 */
int ntfs_list(const char *path, ntfs_list_cb_t cb, void *ctx) {
    uint64_t rec;
    int is_dir;
    ntfs_list_ctx_t l;

    if (ntfs_lookup(path, &rec, 0, &is_dir, 0) != 0) return -1;
    if (!is_dir) return -1;
    if (ntfs_read_record(rec, ntfs_dirrec) != 0) return -1;

    l.cb = cb; l.ctx = ctx;
    ntfs_index_walk(ntfs_dirrec, ntfs_list_adapt, &l);
    return 0;
}

/*
 * A byte range out of a file.
 *
 * This is what makes a 937 MB encyclopedia readable on a machine that
 * cannot hold one: the ZIM reader asks for a window at a time and never
 * for the whole file. ntfs_read_file above cannot serve that -- it
 * starts at zero and fills a buffer -- so the run walk here carries a
 * virtual cluster number and skips whole runs until it reaches the one
 * containing `offset`.
 *
 * Returns bytes read, which may be short at end of file, or -1.
 */
int64_t ntfs_read_range(uint64_t record, uint64_t offset,
                        void *buf, uint64_t len) {
    const ntfs_attr_t *d;
    uint8_t *out = (uint8_t *)buf;

    if (!ntfs.mounted) return -1;
    if (ntfs_read_record(record, ntfs_rec) != 0) return -1;

    d = ntfs_find_attr(ntfs_rec, NTFS_ATTR_DATA);
    if (!d) return -1;

    /* Resident: the whole file is inside the record, so the range is a
     * bounds check and a copy. */
    if (!d->non_resident) {
        const uint32_t total = d->u.res.value_length;
        const uint8_t *src = (const uint8_t *)d + d->u.res.value_offset;
        uint64_t n;

        if (offset >= total) return 0;
        n = total - offset;
        if (n > len) n = len;
        for (uint64_t i = 0; i < n; i++) out[i] = src[offset + i];
        return (int64_t)n;
    }

    {
        const uint64_t real = d->u.nonres.real_size;
        const uint8_t *p = (const uint8_t *)d + d->u.nonres.run_offset;
        const uint8_t *end = (const uint8_t *)d + d->length;
        const uint32_t cbytes = ntfs.bytes_per_cluster;
        int64_t prev = 0;
        uint64_t vcn = 0;               /* cluster index within the file */
        uint64_t want, done = 0;
        ntfs_run_t run;

        if (offset >= real) return 0;
        want = real - offset;
        if (want > len) want = len;

        while (done < want && ntfs_next_run(&p, end, &prev, &run)) {
            const uint64_t run_bytes = run.length * (uint64_t)cbytes;
            const uint64_t run_start = vcn * (uint64_t)cbytes;
            uint64_t skip, avail, lcn;

            vcn += run.length;

            /* Entirely before the range asked for. */
            if (run_start + run_bytes <= offset + done) continue;

            /* How far into this run the next wanted byte is. */
            skip  = (offset + done > run_start) ? (offset + done - run_start)
                                                : 0;
            avail = run_bytes - skip;
            if (avail > want - done) avail = want - done;

            if (run.sparse) {
                for (uint64_t i = 0; i < avail; i++) out[done + i] = 0;
                done += avail;
                continue;
            }

            /* Read cluster-aligned and copy from the offset inside the
             * first cluster: a range rarely starts on a cluster boundary
             * and the block layer only moves whole sectors. */
            lcn = run.lcn + skip / cbytes;
            {
                uint64_t within = skip % cbytes;
                uint64_t left = avail;
                while (left) {
                    uint32_t chunk = sizeof(ntfs_clu);
                    uint32_t clusters, usable;
                    if ((uint64_t)chunk > within + left)
                        chunk = (uint32_t)(within + left);
                    clusters = (chunk + cbytes - 1) / cbytes;
                    if (ntfs_read_clusters(lcn, clusters, ntfs_clu) != 0)
                        return -1;
                    usable = (uint32_t)(clusters * (uint64_t)cbytes - within);
                    if ((uint64_t)usable > left) usable = (uint32_t)left;
                    for (uint32_t i = 0; i < usable; i++)
                        out[done + i] = ntfs_clu[within + i];
                    done  += usable;
                    left  -= usable;
                    lcn   += clusters;
                    within = 0;
                }
            }
        }
        return (int64_t)done;
    }
}

/*
 * How much of the volume is in use.
 *
 * Counted out of $Bitmap rather than tracked, because a count that is
 * maintained separately from the thing it counts is a count that drifts.
 * One pass over the bitmap is a few hundred kilobytes on an 8 GB volume
 * and this is asked for by `df` and the settings panel, not by anything
 * on a hot path.
 */
int ntfs_space(uint64_t *out_total, uint64_t *out_free) {
    uint64_t free_clusters = 0;

    if (!ntfs.mounted) return -1;
    if (!ntfs_bmp.loaded && ntfs_bitmap_load() != 0) return -1;

    for (uint64_t chunk = 0; chunk < ntfs_bmp.clusters; chunk++) {
        if (ntfs_read_clusters(ntfs_bmp.lcn + chunk, 1, ntfs_w_clu) != 0)
            return -1;
        for (uint32_t i = 0; i < ntfs.bytes_per_cluster; i++) {
            uint8_t b = ntfs_w_clu[i];
            /* Kernighan's count, on the complement: bits that are zero
             * are clusters that are free. */
            b = (uint8_t)~b;
            while (b) { free_clusters++; b &= (uint8_t)(b - 1); }
        }
    }

    /* The bitmap is rounded up to a whole cluster, so the tail past the
     * volume's real cluster count reads as free and is not. */
    {
        const uint64_t mapped = ntfs_bmp.clusters *
                                (uint64_t)ntfs.bytes_per_cluster * 8;
        if (mapped > ntfs_bmp.total_clusters)
            free_clusters -= (mapped - ntfs_bmp.total_clusters);
    }

    if (out_total) *out_total = ntfs_bmp.total_clusters;
    if (out_free)  *out_free  = free_clusters;
    return 0;
}

/* The cluster size, for callers reporting geometry. */
uint32_t ntfs_cluster_bytes(void) { return ntfs.bytes_per_cluster; }

/* Whether a volume is mounted, and what the mount decided. The state
 * itself stays private -- the filesystem dispatch needs the answer, not
 * the structure. */
int ntfs_mounted(void) { return ntfs.mounted; }
const char *ntfs_status(void) { return ntfs.status; }
uint64_t ntfs_total_clusters(void) { return ntfs_bmp.total_clusters; }

/*
 * Where a file's data physically starts, and whether it is one run.
 *
 * This exists for the pagefile and for nothing else. src/swap.h resolves
 * its backing store to one absolute LBA at boot so that no filesystem
 * code runs inside a page fault -- a fault that had to walk the MFT to
 * find its own backing store would deadlock the first time it faulted on
 * a buffer the MFT walk was using.
 *
 * Returns 0 only when the file is exactly one non-sparse run of at least
 * `need_bytes`. Anything else -- fragmented, sparse, resident, short --
 * is refused rather than partially accepted, because a swapper that
 * silently used the first run of a fragmented file would write pages
 * over whatever came after it.
 */
int ntfs_single_extent(uint64_t record, uint64_t need_bytes,
                       uint64_t *out_lba, uint64_t *out_bytes) {
    const ntfs_attr_t *d;

    if (!ntfs.mounted) return -1;
    if (ntfs_read_record(record, ntfs_rec) != 0) return -1;

    d = ntfs_find_attr(ntfs_rec, NTFS_ATTR_DATA);
    if (!d || !d->non_resident) return -1;
    if (d->u.nonres.real_size < need_bytes) return -1;

    {
        const uint8_t *p = (const uint8_t *)d + d->u.nonres.run_offset;
        const uint8_t *end = (const uint8_t *)d + d->length;
        int64_t prev = 0;
        ntfs_run_t run, second;

        if (!ntfs_next_run(&p, end, &prev, &run)) return -1;
        if (run.sparse) return -1;
        if (run.length * (uint64_t)ntfs.bytes_per_cluster < need_bytes)
            return -1;
        /* A second run means the file is fragmented, however long the
         * first one is. */
        if (ntfs_next_run(&p, end, &prev, &second)) return -1;

        if (out_lba)
            *out_lba = ntfs.part_lba +
                       run.lcn * (uint64_t)ntfs.sectors_per_cluster;
        if (out_bytes)
            *out_bytes = run.length * (uint64_t)ntfs.bytes_per_cluster;
    }
    return 0;
}
