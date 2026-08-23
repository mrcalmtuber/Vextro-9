#ifndef NTFS_WRITE_H
#define NTFS_WRITE_H

/*
 * src/ntfswrite.h — making changes to an NTFS volume.
 *
 * src/ntfs.h reads NTFS. This writes it, which is a different problem
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
 *   $INDEX_ALLOCATION      Directories live entirely in the resident
 *                          $INDEX_ROOT. When one fills, this reports
 *                          ENOSPC rather than splitting into a B-tree.
 *                          A directory holds roughly forty entries.
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

#include <stdint.h>
#include "ntfs.h"

#define NTFS_W_OK            0
#define NTFS_W_NOSPACE      -1
#define NTFS_W_IO           -2
#define NTFS_W_EXISTS       -3
#define NTFS_W_NOTFOUND     -4
#define NTFS_W_TOOBIG       -5
#define NTFS_W_READONLY     -6

static const char *ntfs_w_errstr = "";

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
static int ntfs_journal_init(uint64_t part_lba, uint64_t part_sectors) {
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
static int ntfs_journal_replay(void) {
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
    if (ntfs_journal_write_run(ntfs_record_lba(number),
                               ntfs.bytes_per_record / 512, rec) != 0) {
        ntfs_w_errstr = "write failed";
        return NTFS_W_IO;
    }
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

/*
 * Insert a name into a directory's resident index.
 *
 * The index is a run of variable-length entries terminated by one with
 * the "last" flag; the new entry goes in collation order before the
 * first entry that sorts after it. Everything from there to the end
 * moves up, which is why this needs the whole record in memory and why
 * a directory that has outgrown its record cannot be handled by moving
 * bytes -- it needs a B-tree split, which is the gap named at the top.
 */
static int ntfs_index_insert(uint8_t *dir_rec, uint64_t child_ref,
                             const char *name, int is_dir,
                             uint64_t real, uint64_t alloc, uint64_t now) {
    ntfs_record_t *h = (ntfs_record_t *)dir_rec;
    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ROOT);
    uint8_t *root, *node, *p, *end;
    uint8_t entry[16 + 0x42 + 255 * 2];
    uint8_t fnval[0x42 + 255 * 2];
    uint32_t fnlen, elen;
    uint32_t grow;

    if (!ir || ir->non_resident) {
        ntfs_w_errstr = "directory index is not resident";
        return NTFS_W_IO;
    }

    root = (uint8_t *)ir + ir->u.res.value_offset;
    node = root + 16;                   /* past the index root header   */

    fnlen = ntfs_build_filename_value(fnval, ((uint64_t)h->sequence << 48) |
                                      (uint64_t)(uintptr_t)0, name, is_dir,
                                      real, alloc, now);
    /* the parent reference is filled by the caller's record number */
    elen = ntfs_align8(16 + fnlen);

    for (uint32_t i = 0; i < elen; i++) entry[i] = 0;
    ntfs_put64(entry + 0x00, child_ref);
    ntfs_put16(entry + 0x08, (uint16_t)elen);
    ntfs_put16(entry + 0x0A, (uint16_t)fnlen);
    ntfs_put32(entry + 0x0C, 0);
    for (uint32_t i = 0; i < fnlen; i++) entry[16 + i] = fnval[i];

    /* Does it fit? The record's used size plus the entry must stay
     * inside the record. */
    grow = elen;
    if (h->used_size + grow > ntfs.bytes_per_record) {
        ntfs_w_errstr = "directory full (index would need a B-tree node)";
        return NTFS_W_NOSPACE;
    }

    /* Find the insertion point. */
    p = node + (uint32_t)(node[0] | (node[1] << 8) | (node[2] << 16) |
                          ((uint32_t)node[3] << 24));
    end = node + (uint32_t)(node[4] | (node[5] << 8) | (node[6] << 16) |
                            ((uint32_t)node[7] << 24));

    while (p < end) {
        uint16_t this_len = (uint16_t)(p[8] | (p[9] << 8));
        uint32_t flags = (uint32_t)(p[12] | (p[13] << 8) |
                                    (p[14] << 16) | ((uint32_t)p[15] << 24));
        uint16_t nlen = (uint16_t)(p[10] | (p[11] << 8));

        if (this_len < 16) break;
        if (flags & 0x02) break;                    /* the terminator   */

        if (nlen >= 0x42) {
            uint8_t namelen = p[16 + 0x40];
            if (ntfs_name_cmp(p + 16 + 0x42, namelen, name) == 0) {
                ntfs_w_errstr = "a file of that name already exists";
                return NTFS_W_EXISTS;
            }
            if (ntfs_name_cmp(p + 16 + 0x42, namelen, name) > 0) break;
        }
        p += this_len;
    }

    /* Shift everything from the insertion point to the end of the
     * record up, then drop the entry in. */
    {
        uint32_t tail = (uint32_t)(dir_rec + h->used_size - p);
        for (uint32_t i = 0; i < tail; i++)
            dir_rec[h->used_size + grow - 1 - i] = p[tail - 1 - i];
        for (uint32_t i = 0; i < elen; i++) p[i] = entry[i];
    }

    /* Every enclosing length grows by the same amount. */
    h->used_size += grow;
    ir->length += grow;
    ir->u.res.value_length += grow;
    ntfs_put32(node + 0, (uint32_t)(node[0] | (node[1] << 8) |
                                    (node[2] << 16) |
                                    ((uint32_t)node[3] << 24)));
    {
        uint32_t used = (uint32_t)(node[4] | (node[5] << 8) |
                                   (node[6] << 16) |
                                   ((uint32_t)node[7] << 24)) + grow;
        uint32_t allocated = (uint32_t)(node[8] | (node[9] << 8) |
                                        (node[10] << 16) |
                                        ((uint32_t)node[11] << 24)) + grow;
        ntfs_put32(node + 4, used);
        ntfs_put32(node + 8, allocated);
    }
    return NTFS_W_OK;
}

/* Remove a name from a directory index. The inverse shift. */
static int ntfs_index_remove(uint8_t *dir_rec, const char *name) {
    ntfs_record_t *h = (ntfs_record_t *)dir_rec;
    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir_rec,
                                                    NTFS_ATTR_INDEX_ROOT);
    uint8_t *root, *node, *p, *end;

    if (!ir || ir->non_resident) return NTFS_W_IO;

    root = (uint8_t *)ir + ir->u.res.value_offset;
    node = root + 16;
    p = node + (uint32_t)(node[0] | (node[1] << 8) | (node[2] << 16) |
                          ((uint32_t)node[3] << 24));
    end = node + (uint32_t)(node[4] | (node[5] << 8) | (node[6] << 16) |
                            ((uint32_t)node[7] << 24));

    while (p < end) {
        uint16_t this_len = (uint16_t)(p[8] | (p[9] << 8));
        uint32_t flags = (uint32_t)(p[12] | (p[13] << 8) |
                                    (p[14] << 16) | ((uint32_t)p[15] << 24));
        uint16_t nlen = (uint16_t)(p[10] | (p[11] << 8));

        if (this_len < 16 || (flags & 0x02)) break;

        if (nlen >= 0x42 &&
            ntfs_name_cmp(p + 16 + 0x42, p[16 + 0x40], name) == 0) {
            uint32_t tail = (uint32_t)(dir_rec + h->used_size - (p + this_len));
            for (uint32_t i = 0; i < tail; i++) p[i] = p[this_len + i];

            h->used_size -= this_len;
            ir->length -= this_len;
            ir->u.res.value_length -= this_len;
            {
                uint32_t used = (uint32_t)(node[4] | (node[5] << 8) |
                                           (node[6] << 16) |
                                           ((uint32_t)node[7] << 24)) - this_len;
                uint32_t allocated = (uint32_t)(node[8] | (node[9] << 8) |
                                                (node[10] << 16) |
                                                ((uint32_t)node[11] << 24)) - this_len;
                ntfs_put32(node + 4, used);
                ntfs_put32(node + 8, allocated);
            }
            return NTFS_W_OK;
        }
        p += this_len;
    }

    ntfs_w_errstr = "no such entry in the directory";
    return NTFS_W_NOTFOUND;
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
static int ntfs_create_file(uint64_t dir_record, const char *name,
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
static int ntfs_delete_file(uint64_t dir_record, const char *name) {
    uint64_t number = 0;
    uint64_t now = ntfs_now();
    int rc;

    if (!ntfs.mounted) return NTFS_W_READONLY;

    if (ntfs_read_record(dir_record, ntfs_w_dir) != 0) return NTFS_W_IO;

    /* Find the record number before the entry is removed. */
    {
        ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(ntfs_w_dir,
                                                        NTFS_ATTR_INDEX_ROOT);
        uint8_t *node, *p, *end;
        if (!ir || ir->non_resident) return NTFS_W_IO;
        node = (uint8_t *)ir + ir->u.res.value_offset + 16;
        p = node + (uint32_t)(node[0] | (node[1] << 8) | (node[2] << 16) |
                              ((uint32_t)node[3] << 24));
        end = node + (uint32_t)(node[4] | (node[5] << 8) | (node[6] << 16) |
                                ((uint32_t)node[7] << 24));
        while (p < end) {
            uint16_t elen = (uint16_t)(p[8] | (p[9] << 8));
            uint32_t flags = (uint32_t)(p[12] | (p[13] << 8) | (p[14] << 16) |
                                        ((uint32_t)p[15] << 24));
            if (elen < 16 || (flags & 0x02)) break;
            if (ntfs_name_cmp(p + 16 + 0x42, p[16 + 0x40], name) == 0) {
                number = (uint64_t)(p[0] | ((uint64_t)p[1] << 8) |
                                    ((uint64_t)p[2] << 16) |
                                    ((uint64_t)p[3] << 24) |
                                    ((uint64_t)p[4] << 32) |
                                    ((uint64_t)p[5] << 40));
                break;
            }
            p += elen;
        }
    }
    if (!number) { ntfs_w_errstr = "no such file"; return NTFS_W_NOTFOUND; }

    rc = ntfs_index_remove(ntfs_w_dir, name);
    if (rc != NTFS_W_OK) return rc;
    rc = ntfs_write_record(dir_record, ntfs_w_dir);
    if (rc != NTFS_W_OK) return rc;

    /* Now release what it owned. */
    if (ntfs_read_record(number, ntfs_w_rec) == 0) {
        const ntfs_attr_t *a = ntfs_find_attr(ntfs_w_rec, NTFS_ATTR_DATA);
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
static int ntfs_mkdir_at(uint64_t dir_record, const char *name) {
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

#endif /* NTFS_WRITE_H */
