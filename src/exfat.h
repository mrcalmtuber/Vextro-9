#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>
#include "ata.h"

/*
 * exFAT driver — read and write.
 *
 * FAT32 caps a file at 4 GB, which is the whole reason the system volume
 * moved over: an encyclopedia archive is far larger than that.  exFAT
 * carries 64-bit sizes everywhere, so the ceiling is gone.
 *
 * Three things make it a different animal from FAT32:
 *
 *   - Free space lives in an allocation bitmap, not in the FAT.  One bit
 *     per cluster, so allocating is a bit scan rather than a chain walk.
 *
 *   - A file may be *contiguous*, flagged NoFatChain, in which case the
 *     FAT holds nothing for it and its clusters are simply consecutive.
 *     Anything this driver writes builds a real chain; anything it reads
 *     may be either, and both paths are implemented.
 *
 *   - A directory entry is a *set*: one 0x85 file entry, one 0xC0 stream
 *     entry, and enough 0xC1 entries to hold the name in UTF-16, all
 *     covered by a 16-bit checksum.  There is no 8.3 fallback, so names
 *     are finally free-form.
 *
 * Names are compared case-insensitively over ASCII.  The on-disk up-case
 * table is read and its checksum noted, but comparison uses the ASCII
 * fold directly: every name this OS creates is ASCII, and folding a
 * non-ASCII name with the table would still not make it displayable in
 * an 8x8 bitmap font.
 */

#define EXF_SECTOR       512
#define EXF_NAME_MAX     128
#define EXF_ENTRY_SIZE   32

/* entry types */
#define EXF_ET_BITMAP    0x81
#define EXF_ET_UPCASE    0x82
#define EXF_ET_LABEL     0x83
#define EXF_ET_FILE      0x85
#define EXF_ET_STREAM    0xC0
#define EXF_ET_NAME      0xC1
#define EXF_ET_INUSE     0x80

#define EXF_ATTR_RO      0x01
#define EXF_ATTR_HIDDEN  0x02
#define EXF_ATTR_DIR     0x10

#define EXF_FLAG_ALLOC    0x01
#define EXF_FLAG_NOCHAIN  0x02

#define EXF_EOC          0xFFFFFFF7u   /* anything >= this ends a chain */

static struct {
    int      mounted;
    uint64_t total_sectors;
    uint32_t fat_offset;        /* sectors */
    uint32_t fat_length;        /* sectors */
    uint32_t heap_offset;       /* sectors */
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t spc;               /* sectors per cluster */
    uint32_t cluster_bytes;
    uint32_t bitmap_cluster;
    uint64_t bitmap_bytes;
    uint32_t upcase_cluster;
    uint64_t upcase_bytes;
    uint64_t part_lba;          /* where the volume starts on the device */
} exf_vol;

static const char *exf_errstr = "";
static uint8_t exf_secbuf[EXF_SECTOR];
static uint8_t exf_dirbuf[EXF_SECTOR];

typedef struct {
    char     name[EXF_NAME_MAX];
    uint64_t size;
    uint32_t first_clus;
    uint8_t  attr;
    uint8_t  contiguous;
    /* where the entry set lives, so it can be updated or deleted */
    uint64_t set_lba;
    int      set_idx;
    int      set_count;         /* total entries in the set */
} exf_dirent_t;

/* ---- raw device access, offset by the partition start ---- */

static int exf_read_sec(uint64_t lba, uint32_t count, void *buf) {
    return ata_read((uint32_t)(exf_vol.part_lba + lba), count, buf);
}

static int exf_write_sec(uint64_t lba, uint32_t count, const void *buf) {
    return ata_write((uint32_t)(exf_vol.part_lba + lba), count, buf);
}

static uint16_t exf_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t exf_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t exf_rd64(const uint8_t *p) {
    return (uint64_t)exf_rd32(p) | ((uint64_t)exf_rd32(p + 4) << 32);
}
static void exf_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void exf_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void exf_wr64(uint8_t *p, uint64_t v) {
    exf_wr32(p, (uint32_t)v);
    exf_wr32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t exf_cluster_lba(uint32_t clus) {
    return (uint64_t)exf_vol.heap_offset +
           (uint64_t)(clus - 2) * exf_vol.spc;
}

/* ---- FAT ---- */

static uint32_t exf_fat_get(uint32_t clus) {
    if (clus < 2 || clus >= exf_vol.cluster_count + 2) return EXF_EOC;
    uint64_t byte = (uint64_t)clus * 4;
    uint64_t lba = exf_vol.fat_offset + byte / EXF_SECTOR;
    if (exf_read_sec(lba, 1, exf_secbuf) != 0) return EXF_EOC;
    return exf_rd32(exf_secbuf + (byte % EXF_SECTOR));
}

static int exf_fat_set(uint32_t clus, uint32_t val) {
    if (clus < 2 || clus >= exf_vol.cluster_count + 2) return -1;
    uint64_t byte = (uint64_t)clus * 4;
    uint64_t lba = exf_vol.fat_offset + byte / EXF_SECTOR;
    if (exf_read_sec(lba, 1, exf_secbuf) != 0) return -1;
    exf_wr32(exf_secbuf + (byte % EXF_SECTOR), val);
    return exf_write_sec(lba, 1, exf_secbuf);
}

/* Cluster that follows `clus` in a file's allocation. */
static uint32_t exf_next_cluster(uint32_t clus, int contiguous) {
    if (contiguous) {
        uint32_t n = clus + 1;
        return n < exf_vol.cluster_count + 2 ? n : EXF_EOC;
    }
    return exf_fat_get(clus);
}

/* ---- allocation bitmap ---- */

static int exf_bitmap_test(uint32_t clus, int *out) {
    uint64_t idx = clus - 2;
    uint64_t byte = idx / 8;
    if (byte >= exf_vol.bitmap_bytes) return -1;
    uint64_t lba = exf_cluster_lba(exf_vol.bitmap_cluster) + byte / EXF_SECTOR;
    if (exf_read_sec(lba, 1, exf_secbuf) != 0) return -1;
    *out = (exf_secbuf[byte % EXF_SECTOR] >> (idx & 7)) & 1;
    return 0;
}

static int exf_bitmap_set(uint32_t clus, int value) {
    uint64_t idx = clus - 2;
    uint64_t byte = idx / 8;
    if (byte >= exf_vol.bitmap_bytes) return -1;
    uint64_t lba = exf_cluster_lba(exf_vol.bitmap_cluster) + byte / EXF_SECTOR;
    if (exf_read_sec(lba, 1, exf_secbuf) != 0) return -1;
    uint8_t mask = (uint8_t)(1u << (idx & 7));
    if (value) exf_secbuf[byte % EXF_SECTOR] |= mask;
    else       exf_secbuf[byte % EXF_SECTOR] &= (uint8_t)~mask;
    return exf_write_sec(lba, 1, exf_secbuf);
}

static uint32_t exf_alloc_hint = 2;

static uint32_t exf_alloc_cluster(void) {
    /* One pass from the hint, then one from the start. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? exf_alloc_hint : 2;
        for (uint32_t c = start; c < exf_vol.cluster_count + 2; c++) {
            int used = 1;
            if (exf_bitmap_test(c, &used) != 0) break;
            if (!used) {
                if (exf_bitmap_set(c, 1) != 0) return 0;
                exf_alloc_hint = c + 1;
                return c;
            }
        }
    }
    exf_errstr = "disk full";
    return 0;
}

static void exf_free_chain(uint32_t first, int contiguous, uint64_t size) {
    if (first < 2) return;
    if (contiguous) {
        uint64_t n = (size + exf_vol.cluster_bytes - 1) / exf_vol.cluster_bytes;
        for (uint64_t i = 0; i < n; i++)
            exf_bitmap_set(first + (uint32_t)i, 0);
        return;
    }
    uint32_t c = first;
    uint32_t guard = 0;
    while (c >= 2 && c < EXF_EOC && guard++ < exf_vol.cluster_count + 2) {
        uint32_t next = exf_fat_get(c);
        exf_bitmap_set(c, 0);
        exf_fat_set(c, 0);
        c = next;
    }
}

/* ---- name handling ---- */

static char exf_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int exf_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (exf_upper(*a) != exf_upper(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static uint16_t exf_name_hash(const char *name) {
    uint16_t h = 0;
    for (const char *p = name; *p; p++) {
        uint16_t u = (uint16_t)(unsigned char)exf_upper(*p);
        uint8_t lo = (uint8_t)u, hi = (uint8_t)(u >> 8);
        h = (uint16_t)(((h << 15) | (h >> 1)) + lo);
        h = (uint16_t)(((h << 15) | (h >> 1)) + hi);
    }
    return h;
}

static uint16_t exf_set_checksum(const uint8_t *entries, int count) {
    uint16_t sum = 0;
    int n = count * EXF_ENTRY_SIZE;
    for (int i = 0; i < n; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + entries[i]);
    }
    return sum;
}

/* ---- directory iteration ---- */

typedef struct {
    uint32_t clus;
    int      contiguous;
    uint32_t sec_in_clus;
    int      idx;               /* entry index inside the sector */
    uint64_t lba;
    int      loaded;
} exf_iter_t;

static void exf_iter_init(exf_iter_t *it, uint32_t clus, int contiguous) {
    it->clus = clus;
    it->contiguous = contiguous;
    it->sec_in_clus = 0;
    it->idx = 0;
    it->loaded = 0;
}

/* Load the sector the iterator points at.  Returns 0 ok, -1 end/error. */
static int exf_iter_load(exf_iter_t *it) {
    if (it->clus < 2 || it->clus >= EXF_EOC) return -1;
    it->lba = exf_cluster_lba(it->clus) + it->sec_in_clus;
    if (exf_read_sec(it->lba, 1, exf_dirbuf) != 0) return -1;
    it->loaded = 1;
    return 0;
}

static int exf_iter_advance(exf_iter_t *it) {
    it->idx++;
    if (it->idx < EXF_SECTOR / EXF_ENTRY_SIZE) return 0;
    it->idx = 0;
    it->sec_in_clus++;
    if (it->sec_in_clus >= exf_vol.spc) {
        it->sec_in_clus = 0;
        it->clus = exf_next_cluster(it->clus, it->contiguous);
        if (it->clus < 2 || it->clus >= EXF_EOC) return -1;
    }
    it->loaded = 0;
    return 0;
}

/*
 * Pull the next complete entry set.  Returns 1 on a name, 0 at the end
 * of the directory, -1 on an I/O error.
 */
static int exf_iter_next(exf_iter_t *it, exf_dirent_t *out) {
    for (;;) {
        if (!it->loaded && exf_iter_load(it) != 0) return 0;
        const uint8_t *e = exf_dirbuf + it->idx * EXF_ENTRY_SIZE;
        uint8_t type = e[0];

        if (type == 0x00) return 0;                 /* end of directory */

        if (type != EXF_ET_FILE) {                  /* not a file set */
            if (exf_iter_advance(it) != 0) return 0;
            continue;
        }

        /* --- a file entry set starts here --- */
        int secondary = e[1];
        uint16_t attrs = exf_rd16(e + 4);
        uint64_t set_lba = it->lba;
        int set_idx = it->idx;

        out->attr = (uint8_t)attrs;
        out->set_lba = set_lba;
        out->set_idx = set_idx;
        out->set_count = secondary + 1;
        out->name[0] = '\0';
        out->size = 0;
        out->first_clus = 0;
        out->contiguous = 0;

        int name_len = 0, name_pos = 0;
        int got_stream = 0;
        int remaining = secondary;

        while (remaining-- > 0) {
            if (exf_iter_advance(it) != 0) return 0;
            if (!it->loaded && exf_iter_load(it) != 0) return 0;
            const uint8_t *s = exf_dirbuf + it->idx * EXF_ENTRY_SIZE;

            if (s[0] == EXF_ET_STREAM) {
                out->contiguous = (s[1] & EXF_FLAG_NOCHAIN) ? 1 : 0;
                name_len = s[3];
                out->size = exf_rd64(s + 24);
                out->first_clus = exf_rd32(s + 20);
                got_stream = 1;
            } else if (s[0] == EXF_ET_NAME) {
                for (int k = 0; k < 15 && name_pos < EXF_NAME_MAX - 1; k++) {
                    uint16_t u = exf_rd16(s + 2 + k * 2);
                    if (name_pos >= name_len) break;
                    out->name[name_pos++] =
                        (u >= 0x20 && u < 0x7F) ? (char)u : '?';
                }
            }
        }
        out->name[name_pos] = '\0';

        if (exf_iter_advance(it) != 0) {
            /* the set ended exactly at the end of the directory */
            return got_stream && out->name[0] ? 1 : 0;
        }
        if (got_stream && out->name[0]) return 1;
    }
}

/* ---- lookup ---- */

static int exf_find_in_dir(uint32_t dir_clus, int dir_contig, const char *name,
                           exf_dirent_t *out) {
    exf_iter_t it;
    exf_iter_init(&it, dir_clus, dir_contig);
    exf_dirent_t e;
    uint16_t want = exf_name_hash(name);
    while (exf_iter_next(&it, &e) == 1) {
        /* the hash is only a filter; the name still decides */
        if (exf_name_hash(e.name) != want) continue;
        if (exf_name_eq(e.name, name)) {
            if (out) *out = e;
            return 1;
        }
    }
    return 0;
}

/* Resolve an absolute path.  The root has no entry set of its own, so it
 * is described by hand. */
static int exf_lookup(const char *path, exf_dirent_t *out) {
    if (!exf_vol.mounted) { exf_errstr = "no volume"; return 0; }

    exf_dirent_t cur;
    for (int i = 0; i < EXF_NAME_MAX; i++) cur.name[i] = 0;
    cur.attr = EXF_ATTR_DIR;
    cur.first_clus = exf_vol.root_cluster;
    cur.contiguous = 0;
    cur.size = 0;
    cur.set_lba = 0;
    cur.set_idx = 0;
    cur.set_count = 0;

    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        char comp[EXF_NAME_MAX];
        int n = 0;
        while (*p && *p != '/' && n < EXF_NAME_MAX - 1) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/') p++;
        if (n == 0) continue;

        if (!(cur.attr & EXF_ATTR_DIR)) { exf_errstr = "not a directory"; return 0; }
        exf_dirent_t next;
        if (!exf_find_in_dir(cur.first_clus, cur.contiguous, comp, &next)) {
            exf_errstr = "not found";
            return 0;
        }
        cur = next;
    }
    if (out) *out = cur;
    return 1;
}

/* ---- reading ---- */

/*
 * Range read: the whole point of moving to exFAT is that files can be
 * far larger than any buffer, so callers read windows rather than whole
 * files.
 */
static int exf_read_range(const exf_dirent_t *f, uint64_t offset,
                          uint8_t *buf, uint32_t len, uint32_t *got) {
    *got = 0;
    if (!exf_vol.mounted) { exf_errstr = "no volume"; return -1; }
    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = (uint32_t)(f->size - offset);

    uint32_t clus = f->first_clus;
    uint64_t skip_clusters = offset / exf_vol.cluster_bytes;
    uint32_t in_clus = (uint32_t)(offset % exf_vol.cluster_bytes);

    if (f->contiguous) {
        clus += (uint32_t)skip_clusters;
    } else {
        for (uint64_t i = 0; i < skip_clusters; i++) {
            clus = exf_fat_get(clus);
            if (clus < 2 || clus >= EXF_EOC) { exf_errstr = "short chain"; return -1; }
        }
    }

    while (len > 0) {
        if (clus < 2 || clus >= EXF_EOC) break;
        uint32_t sec = in_clus / EXF_SECTOR;
        uint32_t off = in_clus % EXF_SECTOR;

        while (sec < exf_vol.spc && len > 0) {
            uint64_t lba = exf_cluster_lba(clus) + sec;

            /* whole sectors straight into the caller's buffer */
            if (off == 0 && len >= EXF_SECTOR) {
                uint32_t runs = (exf_vol.spc - sec);
                uint32_t want = len / EXF_SECTOR;
                if (want > runs) want = runs;
                if (exf_read_sec(lba, want, buf) != 0) {
                    exf_errstr = "read error";
                    return -1;
                }
                buf += want * EXF_SECTOR;
                len -= want * EXF_SECTOR;
                *got += want * EXF_SECTOR;
                sec += want;
                continue;
            }

            if (exf_read_sec(lba, 1, exf_secbuf) != 0) {
                exf_errstr = "read error";
                return -1;
            }
            uint32_t n = EXF_SECTOR - off;
            if (n > len) n = len;
            for (uint32_t i = 0; i < n; i++) buf[i] = exf_secbuf[off + i];
            buf += n;
            len -= n;
            *got += n;
            off = 0;
            sec++;
        }

        in_clus = 0;
        clus = exf_next_cluster(clus, f->contiguous);
    }
    return 0;
}

static int exf_read_file(const exf_dirent_t *f, uint8_t *buf, uint32_t max,
                         uint32_t *got) {
    uint32_t want = f->size > max ? max : (uint32_t)f->size;
    return exf_read_range(f, 0, buf, want, got);
}

/* ---- listing ---- */

typedef void (*exf_list_cb)(const char *name, uint32_t size, int is_dir);

static int exf_list(const char *path, exf_list_cb cb) {
    exf_dirent_t d;
    if (!exf_lookup(path, &d) || !(d.attr & EXF_ATTR_DIR)) {
        exf_errstr = "no such directory";
        return -1;
    }
    exf_iter_t it;
    exf_iter_init(&it, d.first_clus, d.contiguous);
    exf_dirent_t e;
    while (exf_iter_next(&it, &e) == 1)
        cb(e.name, (uint32_t)e.size, (e.attr & EXF_ATTR_DIR) ? 1 : 0);
    return 0;
}

/* ---- directory mutation ---- */

/* Split "/a/b/c" into the parent directory and the leaf name. */
static int exf_split(const char *path, exf_dirent_t *parent, char *leaf) {
    int len = 0;
    while (path[len]) len++;
    int cut = -1;
    for (int i = 0; i < len; i++)
        if (path[i] == '/') cut = i;
    if (cut < 0) { exf_errstr = "bad path"; return 0; }

    char dir[256];
    int n = 0;
    if (cut == 0) { dir[n++] = '/'; }
    else for (int i = 0; i < cut && n < 255; i++) dir[n++] = path[i];
    dir[n] = '\0';

    int j = 0;
    for (int i = cut + 1; path[i] && j < EXF_NAME_MAX - 1; i++) leaf[j++] = path[i];
    leaf[j] = '\0';
    if (j == 0) { exf_errstr = "bad path"; return 0; }

    if (!exf_lookup(dir, parent) || !(parent->attr & EXF_ATTR_DIR)) {
        exf_errstr = "no such directory";
        return 0;
    }
    return 1;
}

/* How many 32-byte entries a name needs: file + stream + name chunks. */
static int exf_entries_for(const char *name) {
    int n = 0;
    while (name[n]) n++;
    return 2 + (n + 14) / 15;
}

/*
 * Find `need` consecutive free entry slots in a directory, growing it by
 * a cluster if there is no room.  Returns 0 and fills lba/idx.
 */
static int exf_find_slots(exf_dirent_t *dir, int need, uint64_t *out_lba,
                          int *out_idx) {
    uint32_t clus = dir->first_clus;
    uint32_t prev = clus;
    uint32_t guard = 0;
    const int per_sec = EXF_SECTOR / EXF_ENTRY_SIZE;

    while (clus >= 2 && clus < EXF_EOC && guard++ < exf_vol.cluster_count + 2) {
        for (uint32_t s = 0; s < exf_vol.spc; s++) {
            uint64_t lba = exf_cluster_lba(clus) + s;
            if (exf_read_sec(lba, 1, exf_dirbuf) != 0) {
                exf_errstr = "dir read error";
                return -1;
            }

            /*
             * A set must live inside one sector, so runs never straddle.
             * The subtle part is the 0x00 terminator: every reader,
             * this one and the host's alike, stops at it, so a set must
             * never be written past one.  If the tail of this sector is
             * too short, the gap is stamped with deleted markers to keep
             * the directory walkable, and the search moves on.
             */
            int run = 0, run_idx_local = -1;
            for (int i = 0; i < per_sec; i++) {
                uint8_t t = exf_dirbuf[i * EXF_ENTRY_SIZE];

                if (t == 0x00) {
                    /* everything from here to the end of the sector is
                     * virgin, so the only question is whether it fits */
                    if (per_sec - i >= need) {
                        *out_lba = lba;
                        *out_idx = i;
                        return 0;
                    }
                    for (int k = i; k < per_sec; k++)
                        exf_dirbuf[k * EXF_ENTRY_SIZE] = 0x05;  /* deleted */
                    if (exf_write_sec(lba, 1, exf_dirbuf) != 0) {
                        exf_errstr = "dir write error";
                        return -1;
                    }
                    run = 0;
                    break;
                }

                if (!(t & EXF_ET_INUSE)) {
                    if (run == 0) run_idx_local = i;
                    if (++run == need) {
                        *out_lba = lba;
                        *out_idx = run_idx_local;
                        return 0;
                    }
                } else {
                    run = 0;
                }
            }
        }
        prev = clus;
        clus = exf_next_cluster(clus, dir->contiguous);
    }

    /* grow the directory by one cluster */
    uint32_t nc = exf_alloc_cluster();
    if (!nc) return -1;
    if (dir->contiguous) {
        /* a contiguous directory that needs a non-adjacent cluster has to
         * become a chained one; rebuild the chain over its clusters */
        uint64_t used = dir->size ? dir->size : exf_vol.cluster_bytes;
        uint32_t n = (uint32_t)((used + exf_vol.cluster_bytes - 1)
                                / exf_vol.cluster_bytes);
        for (uint32_t i = 0; i + 1 < n; i++)
            exf_fat_set(dir->first_clus + i, dir->first_clus + i + 1);
        prev = dir->first_clus + n - 1;
        dir->contiguous = 0;
    }
    exf_fat_set(prev, nc);
    exf_fat_set(nc, 0xFFFFFFFF);

    for (int i = 0; i < EXF_SECTOR; i++) exf_dirbuf[i] = 0;
    for (uint32_t s = 0; s < exf_vol.spc; s++)
        if (exf_write_sec(exf_cluster_lba(nc) + s, 1, exf_dirbuf) != 0) {
            exf_errstr = "dir grow error";
            return -1;
        }
    *out_lba = exf_cluster_lba(nc);
    *out_idx = 0;
    return 0;
}

static void exf_now(uint32_t *stamp) {
    int hh, mm, ss, d, mo, yr;
    rtc_read(&hh, &mm, &ss, &d, &mo, &yr);
    if (yr < 1980) yr = 1980;
    uint32_t date = (uint32_t)(((yr - 1980) << 9) | (mo << 5) | d);
    uint32_t tim = (uint32_t)((hh << 11) | (mm << 5) | (ss / 2));
    *stamp = (date << 16) | tim;
}

/* Write a complete entry set at (lba, idx). */
static int exf_write_set(uint64_t lba, int idx, const char *name,
                         uint16_t attrs, uint32_t first_clus, uint64_t size,
                         int contiguous) {
    int nlen = 0;
    while (name[nlen]) nlen++;
    int nchunks = (nlen + 14) / 15;
    int total = 2 + nchunks;

    uint8_t set[EXF_ENTRY_SIZE * (2 + (EXF_NAME_MAX + 14) / 15)];
    for (int i = 0; i < total * EXF_ENTRY_SIZE; i++) set[i] = 0;

    uint32_t stamp;
    exf_now(&stamp);

    uint8_t *fe = set;
    fe[0] = EXF_ET_FILE;
    fe[1] = (uint8_t)(total - 1);
    exf_wr16(fe + 4, attrs);
    exf_wr32(fe + 8, stamp);
    exf_wr32(fe + 12, stamp);
    exf_wr32(fe + 16, stamp);

    uint8_t *se = set + EXF_ENTRY_SIZE;
    se[0] = EXF_ET_STREAM;
    se[1] = (uint8_t)(EXF_FLAG_ALLOC | (contiguous ? EXF_FLAG_NOCHAIN : 0));
    se[3] = (uint8_t)nlen;
    exf_wr16(se + 4, exf_name_hash(name));
    exf_wr64(se + 8, size);            /* ValidDataLength */
    exf_wr32(se + 20, first_clus);
    exf_wr64(se + 24, size);           /* DataLength */

    for (int c = 0; c < nchunks; c++) {
        uint8_t *ne = set + EXF_ENTRY_SIZE * (2 + c);
        ne[0] = EXF_ET_NAME;
        for (int k = 0; k < 15; k++) {
            int pos = c * 15 + k;
            uint16_t u = pos < nlen ? (uint16_t)(unsigned char)name[pos] : 0;
            exf_wr16(ne + 2 + k * 2, u);
        }
    }

    exf_wr16(set + 2, exf_set_checksum(set, total));

    /* the slot finder guarantees the whole set fits in one sector */
    if (exf_read_sec(lba, 1, exf_dirbuf) != 0) return -1;
    for (int i = 0; i < total * EXF_ENTRY_SIZE; i++)
        exf_dirbuf[idx * EXF_ENTRY_SIZE + i] = set[i];
    return exf_write_sec(lba, 1, exf_dirbuf);
}

/* Mark every entry of a set not-in-use. */
static int exf_clear_set(const exf_dirent_t *e) {
    if (exf_read_sec(e->set_lba, 1, exf_dirbuf) != 0) return -1;
    for (int i = 0; i < e->set_count; i++) {
        int slot = e->set_idx + i;
        if (slot >= EXF_SECTOR / EXF_ENTRY_SIZE) break;
        exf_dirbuf[slot * EXF_ENTRY_SIZE] &= (uint8_t)~EXF_ET_INUSE;
    }
    return exf_write_sec(e->set_lba, 1, exf_dirbuf);
}

/* ---- public write operations ---- */

static int exf_write_file(const char *path, const uint8_t *data, uint32_t len) {
    exf_errstr = "";
    if (!exf_vol.mounted) { exf_errstr = "no writable volume"; return -1; }

    exf_dirent_t parent;
    char leaf[EXF_NAME_MAX];
    if (!exf_split(path, &parent, leaf)) return -1;

    exf_dirent_t existing;
    int have = exf_find_in_dir(parent.first_clus, parent.contiguous, leaf,
                               &existing);
    if (have && (existing.attr & EXF_ATTR_DIR)) {
        exf_errstr = "is a directory";
        return -1;
    }
    if (have)
        exf_free_chain(existing.first_clus, existing.contiguous, existing.size);

    /* allocate a chain and stream the data into it */
    uint32_t need = (len + exf_vol.cluster_bytes - 1) / exf_vol.cluster_bytes;
    uint32_t first = 0, prev = 0;
    uint32_t written = 0;

    for (uint32_t i = 0; i < need; i++) {
        uint32_t c = exf_alloc_cluster();
        if (!c) {
            if (first) exf_free_chain(first, 0, (uint64_t)written);
            return -1;
        }
        if (prev) exf_fat_set(prev, c);
        else first = c;
        exf_fat_set(c, 0xFFFFFFFF);
        prev = c;

        uint64_t lba = exf_cluster_lba(c);
        for (uint32_t s = 0; s < exf_vol.spc; s++) {
            uint32_t left = len - written;
            if (left >= EXF_SECTOR) {
                if (exf_write_sec(lba + s, 1, data + written) != 0) {
                    exf_errstr = "write error";
                    return -1;
                }
                written += EXF_SECTOR;
            } else {
                for (int b = 0; b < EXF_SECTOR; b++)
                    exf_secbuf[b] = (uint32_t)b < left ? data[written + b] : 0;
                if (exf_write_sec(lba + s, 1, exf_secbuf) != 0) {
                    exf_errstr = "write error";
                    return -1;
                }
                written += left;
            }
            if (written >= len) {
                /* zero the tail of the cluster */
                for (int b = 0; b < EXF_SECTOR; b++) exf_secbuf[b] = 0;
                for (uint32_t t = s + 1; t < exf_vol.spc; t++)
                    exf_write_sec(lba + t, 1, exf_secbuf);
                break;
            }
        }
    }

    if (have) {
        /* reuse the entry set: only the stream entry changes */
        if (exf_read_sec(existing.set_lba, 1, exf_dirbuf) != 0) return -1;
        uint8_t *se = exf_dirbuf + (existing.set_idx + 1) * EXF_ENTRY_SIZE;
        se[1] = (uint8_t)EXF_FLAG_ALLOC;          /* chained now */
        exf_wr64(se + 8, len);
        exf_wr32(se + 20, first);
        exf_wr64(se + 24, len);
        /* recompute the set checksum over the whole set */
        uint8_t set[EXF_ENTRY_SIZE * (2 + (EXF_NAME_MAX + 14) / 15)];
        int total = existing.set_count;
        for (int i = 0; i < total * EXF_ENTRY_SIZE; i++)
            set[i] = exf_dirbuf[existing.set_idx * EXF_ENTRY_SIZE + i];
        uint16_t sum = exf_set_checksum(set, total);
        exf_wr16(exf_dirbuf + existing.set_idx * EXF_ENTRY_SIZE + 2, sum);
        return exf_write_sec(existing.set_lba, 1, exf_dirbuf);
    }

    int need_slots = exf_entries_for(leaf);
    uint64_t lba;
    int idx;
    if (exf_find_slots(&parent, need_slots, &lba, &idx) != 0) return -1;
    return exf_write_set(lba, idx, leaf, 0x20, first, len, 0);
}

static int exf_mkdir(const char *path) {
    exf_errstr = "";
    if (!exf_vol.mounted) { exf_errstr = "no writable volume"; return -1; }

    exf_dirent_t parent;
    char leaf[EXF_NAME_MAX];
    if (!exf_split(path, &parent, leaf)) return -1;
    if (exf_find_in_dir(parent.first_clus, parent.contiguous, leaf, 0)) {
        exf_errstr = "already exists";
        return -1;
    }

    uint32_t c = exf_alloc_cluster();
    if (!c) return -1;
    exf_fat_set(c, 0xFFFFFFFF);

    for (int i = 0; i < EXF_SECTOR; i++) exf_dirbuf[i] = 0;
    for (uint32_t s = 0; s < exf_vol.spc; s++)
        if (exf_write_sec(exf_cluster_lba(c) + s, 1, exf_dirbuf) != 0) {
            exf_errstr = "io error";
            return -1;
        }

    int need_slots = exf_entries_for(leaf);
    uint64_t lba;
    int idx;
    if (exf_find_slots(&parent, need_slots, &lba, &idx) != 0) return -1;
    return exf_write_set(lba, idx, leaf, EXF_ATTR_DIR, c,
                         exf_vol.cluster_bytes, 0);
}

static int exf_dir_is_empty(const exf_dirent_t *d) {
    exf_iter_t it;
    exf_iter_init(&it, d->first_clus, d->contiguous);
    exf_dirent_t e;
    return exf_iter_next(&it, &e) != 1;
}

static int exf_delete(const char *path) {
    exf_errstr = "";
    if (!exf_vol.mounted) { exf_errstr = "no writable volume"; return -1; }

    exf_dirent_t parent;
    char leaf[EXF_NAME_MAX];
    if (!exf_split(path, &parent, leaf)) return -1;

    exf_dirent_t e;
    if (!exf_find_in_dir(parent.first_clus, parent.contiguous, leaf, &e)) {
        exf_errstr = "not found";
        return -1;
    }
    if ((e.attr & EXF_ATTR_DIR) && !exf_dir_is_empty(&e)) {
        exf_errstr = "directory not empty";
        return -1;
    }
    exf_free_chain(e.first_clus, e.contiguous, e.size);
    return exf_clear_set(&e);
}

/* ---- mount ---- */

static uint64_t exf_free_clusters(void) {
    uint64_t free_n = 0;
    uint64_t bytes = (exf_vol.cluster_count + 7) / 8;
    if (bytes > exf_vol.bitmap_bytes) bytes = exf_vol.bitmap_bytes;

    uint64_t sectors = (bytes + EXF_SECTOR - 1) / EXF_SECTOR;
    uint32_t counted = 0;
    for (uint64_t s = 0; s < sectors; s++) {
        if (exf_read_sec(exf_cluster_lba(exf_vol.bitmap_cluster) + s, 1,
                         exf_secbuf) != 0)
            break;
        for (int b = 0; b < EXF_SECTOR; b++) {
            for (int bit = 0; bit < 8; bit++) {
                if (counted >= exf_vol.cluster_count) break;
                if (!((exf_secbuf[b] >> bit) & 1)) free_n++;
                counted++;
            }
        }
    }
    return free_n;
}

static uint32_t exf_total_kb(void) {
    return (uint32_t)((uint64_t)exf_vol.cluster_count *
                      exf_vol.cluster_bytes / 1024);
}

static uint32_t exf_free_kb(void) {
    return (uint32_t)(exf_free_clusters() * exf_vol.cluster_bytes / 1024);
}

/* Try to mount an exFAT volume whose VBR is at `lba`. */
static int exfat_try(uint64_t lba) {
    uint8_t vbr[EXF_SECTOR];
    exf_vol.part_lba = lba;
    exf_vol.mounted = 0;
    if (ata_read((uint32_t)lba, 1, vbr) != 0) return 0;

    const char *sig = "EXFAT   ";
    for (int i = 0; i < 8; i++)
        if (vbr[3 + i] != (uint8_t)sig[i]) return 0;

    uint8_t bps_shift = vbr[0x6C];
    uint8_t spc_shift = vbr[0x6D];
    if (bps_shift != 9) return 0;          /* 512-byte sectors only */
    if (spc_shift > 25) return 0;

    exf_vol.total_sectors = exf_rd64(vbr + 0x48);
    exf_vol.fat_offset    = exf_rd32(vbr + 0x50);
    exf_vol.fat_length    = exf_rd32(vbr + 0x54);
    exf_vol.heap_offset   = exf_rd32(vbr + 0x58);
    exf_vol.cluster_count = exf_rd32(vbr + 0x5C);
    exf_vol.root_cluster  = exf_rd32(vbr + 0x60);
    exf_vol.spc           = 1u << spc_shift;
    exf_vol.cluster_bytes = exf_vol.spc * EXF_SECTOR;

    if (exf_vol.cluster_count == 0 || exf_vol.root_cluster < 2) return 0;
    if (exf_vol.root_cluster >= exf_vol.cluster_count + 2) return 0;

    exf_vol.mounted = 1;
    exf_vol.bitmap_cluster = 0;
    exf_vol.upcase_cluster = 0;

    /* the bitmap and up-case table are announced by root entries */
    exf_iter_t it;
    exf_iter_init(&it, exf_vol.root_cluster, 0);
    for (int guard = 0; guard < 256; guard++) {
        if (!it.loaded && exf_iter_load(&it) != 0) break;
        const uint8_t *e = exf_dirbuf + it.idx * EXF_ENTRY_SIZE;
        if (e[0] == 0x00) break;
        if (e[0] == EXF_ET_BITMAP) {
            exf_vol.bitmap_cluster = exf_rd32(e + 20);
            exf_vol.bitmap_bytes = exf_rd64(e + 24);
        } else if (e[0] == EXF_ET_UPCASE) {
            exf_vol.upcase_cluster = exf_rd32(e + 20);
            exf_vol.upcase_bytes = exf_rd64(e + 24);
        }
        if (exf_iter_advance(&it) != 0) break;
    }

    if (exf_vol.bitmap_cluster < 2) {
        exf_vol.mounted = 0;
        return 0;
    }
    exf_alloc_hint = 2;
    return 1;
}

/*
 * Mount: try the whole device as a superfloppy, then each MBR partition.
 * A partitioned disk is the normal shape for a big exFAT volume, and it
 * is also how a FAT32 boot partition can sit alongside the system one.
 */
static void exfat_mount(void) {
    exf_vol.mounted = 0;
    if (!ata_present) return;

    if (exfat_try(0)) return;

    uint8_t mbr[EXF_SECTOR];
    if (ata_read(0, 1, mbr) != 0) return;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return;

    for (int i = 0; i < 4; i++) {
        const uint8_t *p = mbr + 446 + i * 16;
        uint8_t type = p[4];
        uint32_t start = exf_rd32(p + 8);
        if (type == 0 || start == 0) continue;
        if (exfat_try(start)) return;
    }
}

#endif /* EXFAT_H */
