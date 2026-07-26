#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "ata.h"
#include "gfx.h"

/*
 * FAT32 driver — read/write, 8.3 creation, LFN-aware reading.
 *
 * Design: the whole FAT lives in RAM (up to 2 MB → volumes up to ~256 MB
 * with 512-byte clusters) with per-sector dirty tracking; directory and
 * data sectors go straight to disk.  Every mutating operation ends with
 * a FAT flush + ATA cache flush, so state is always consistent on disk.
 */

#define FAT_MAX_FAT_BYTES (2u * 1024u * 1024u)
#define FAT_EOC           0x0FFFFFF8u
#define FAT_ATTR_RO       0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_LFN      0x0F

static struct {
    int      mounted;
    uint32_t part_lba;       /* absolute LBA of the volume (VBR)   */
    uint32_t spc;            /* sectors per cluster                */
    uint32_t resvd;
    uint32_t nfats;
    uint32_t fatsz;          /* sectors per FAT                    */
    uint32_t root_clus;
    uint32_t data_lba;       /* absolute LBA of cluster 2          */
    uint32_t nclusters;
    uint32_t fsinfo_lba;
} fat_vol;

static uint8_t  fat_table[FAT_MAX_FAT_BYTES];
static uint8_t  fat_dirty_bm[FAT_MAX_FAT_BYTES / 512 / 8];
static int      fat_any_dirty = 0;
static uint32_t fat_free_hint = 3;

static uint8_t  fat_secbuf[512];      /* scratch sector             */

static const char *fat_errstr = "";

/* ===== FAT ACCESS ===== */

static uint32_t fat_get(uint32_t c) {
    uint32_t v;
    const uint8_t *p = fat_table + c * 4;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v & 0x0FFFFFFF;
}

static void fat_set(uint32_t c, uint32_t val) {
    uint8_t *p = fat_table + c * 4;
    uint32_t old_hi = (uint32_t)p[3] & 0xF0;
    p[0] = (uint8_t)val;
    p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(((val >> 24) & 0x0F) | old_hi);
    uint32_t sec = (c * 4) / 512;
    fat_dirty_bm[sec >> 3] |= (uint8_t)(1 << (sec & 7));
    fat_any_dirty = 1;
}

static uint32_t fat_cluster_lba(uint32_t c) {
    return fat_vol.data_lba + (c - 2) * fat_vol.spc;
}

static uint32_t fat_count_free(void) {
    uint32_t free_n = 0;
    for (uint32_t c = 2; c < fat_vol.nclusters + 2; c++)
        if (fat_get(c) == 0) free_n++;
    return free_n;
}

static int fat_flush(void) {
    if (!fat_any_dirty) return 0;
    for (uint32_t sec = 0; sec < fat_vol.fatsz; sec++) {
        if (!(fat_dirty_bm[sec >> 3] & (1 << (sec & 7))))
            continue;
        for (uint32_t n = 0; n < fat_vol.nfats; n++) {
            uint32_t lba = fat_vol.part_lba + fat_vol.resvd +
                           n * fat_vol.fatsz + sec;
            if (ata_write(lba, 1, fat_table + sec * 512) != 0)
                return -1;
        }
        fat_dirty_bm[sec >> 3] &= (uint8_t)~(1 << (sec & 7));
    }
    fat_any_dirty = 0;

    /* keep FSInfo honest for host mounts */
    if (fat_vol.fsinfo_lba) {
        if (ata_read(fat_vol.fsinfo_lba, 1, fat_secbuf) == 0) {
            uint32_t free_n = fat_count_free();
            fat_secbuf[488] = (uint8_t)free_n;
            fat_secbuf[489] = (uint8_t)(free_n >> 8);
            fat_secbuf[490] = (uint8_t)(free_n >> 16);
            fat_secbuf[491] = (uint8_t)(free_n >> 24);
            fat_secbuf[492] = (uint8_t)fat_free_hint;
            fat_secbuf[493] = (uint8_t)(fat_free_hint >> 8);
            fat_secbuf[494] = (uint8_t)(fat_free_hint >> 16);
            fat_secbuf[495] = (uint8_t)(fat_free_hint >> 24);
            ata_write(fat_vol.fsinfo_lba, 1, fat_secbuf);
        }
    }
    ata_flush();
    return 0;
}

static uint32_t fat_alloc_cluster(void) {
    for (uint32_t i = 0; i < fat_vol.nclusters; i++) {
        uint32_t c = fat_free_hint + i;
        while (c >= fat_vol.nclusters + 2)
            c -= fat_vol.nclusters;
        if (c < 2) c = 2;
        if (fat_get(c) == 0) {
            fat_set(c, 0x0FFFFFFF);
            fat_free_hint = c + 1;
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t first) {
    uint32_t c = first;
    uint32_t guard = 0;
    while (c >= 2 && c < FAT_EOC && guard++ < fat_vol.nclusters + 2) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        c = next;
    }
}

/* ===== TIMESTAMPS ===== */

static void fat_now(uint16_t *date, uint16_t *tim) {
    int hh, mm, ss, d, mo, yr;
    rtc_read(&hh, &mm, &ss, &d, &mo, &yr);
    if (yr < 1980) yr = 1980;
    *date = (uint16_t)(((yr - 1980) << 9) | (mo << 5) | d);
    *tim  = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2));
}

/* ===== DIRECTORY ITERATION (LFN-aware) ===== */

#define FAT_NAME_MAX 64
#define FAT_REC_MAX  21     /* 20 LFN parts + 1 short entry */

typedef struct {
    char     name[FAT_NAME_MAX];
    uint8_t  attr;
    uint32_t size;
    uint32_t first_clus;
    /* location of the short entry (for size/cluster updates) */
    uint32_t ent_lba;
    int      ent_idx;
} fat_dirent_t;

typedef struct {
    uint32_t clus;
    uint32_t sec_in_clus;
    int      ent_in_sec;
    uint8_t  sec[512];
    uint32_t sec_lba;
    int      sec_loaded;
    /* LFN accumulation */
    char     lfn[FAT_NAME_MAX];
    int      lfn_valid;
    /* record locations (LFN entries + short entry) for deletion */
    uint32_t rec_lba[FAT_REC_MAX];
    int      rec_idx[FAT_REC_MAX];
    int      rec_n;
} fat_iter_t;

static void fat_iter_init(fat_iter_t *it, uint32_t clus) {
    it->clus = clus;
    it->sec_in_clus = 0;
    it->ent_in_sec = 0;
    it->sec_loaded = 0;
    it->lfn_valid = 0;
    it->rec_n = 0;
}

static int fat_ci_eq_ch(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

static int fat_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (!fat_ci_eq_ch(*a, *b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* decode an 8.3 field honoring the NT lowercase flags */
static void fat_decode_short(const uint8_t *ent, char *out) {
    int nt = ent[12];
    int p = 0;
    for (int i = 0; i < 8 && ent[i] != ' '; i++) {
        char c = (char)ent[i];
        if ((nt & 0x08) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (i == 0 && (uint8_t)c == 0x05) c = (char)0xE5;
        out[p++] = c;
    }
    if (ent[8] != ' ') {
        out[p++] = '.';
        for (int i = 8; i < 11 && ent[i] != ' '; i++) {
            char c = (char)ent[i];
            if ((nt & 0x10) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[p++] = c;
        }
    }
    out[p] = '\0';
}

/* returns 1 = entry produced, 0 = end of directory, -1 = io error */
static int fat_iter_next(fat_iter_t *it, fat_dirent_t *out) {
    /* a record (LFN parts + short entry) never spans two calls: LFN parts
     * are consumed inside this loop, so each call starts a fresh record */
    it->rec_n = 0;
    for (;;) {
        if (it->clus < 2 || it->clus >= FAT_EOC)
            return 0;

        if (!it->sec_loaded) {
            it->sec_lba = fat_cluster_lba(it->clus) + it->sec_in_clus;
            if (ata_read(it->sec_lba, 1, it->sec) != 0)
                return -1;
            it->sec_loaded = 1;
        }

        while (it->ent_in_sec < 16) {
            uint8_t *ent = it->sec + it->ent_in_sec * 32;
            int idx = it->ent_in_sec;
            it->ent_in_sec++;

            uint8_t b0 = ent[0];
            if (b0 == 0x00)
                return 0;                     /* end of directory */
            if (b0 == 0xE5) {                 /* deleted */
                it->lfn_valid = 0;
                it->rec_n = 0;
                continue;
            }

            if ((ent[11] & 0x3F) == FAT_ATTR_LFN) {
                int seq = b0 & 0x1F;
                if (b0 & 0x40) {              /* first (highest) part */
                    for (int i = 0; i < FAT_NAME_MAX; i++) it->lfn[i] = 0;
                    it->rec_n = 0;
                }
                if (it->rec_n < FAT_REC_MAX) {
                    it->rec_lba[it->rec_n] = it->sec_lba;
                    it->rec_idx[it->rec_n] = idx;
                    it->rec_n++;
                }
                if (seq >= 1 && seq <= 20) {
                    static const int off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    int base = (seq - 1) * 13;
                    for (int i = 0; i < 13 && base + i < FAT_NAME_MAX - 1; i++) {
                        uint16_t u = (uint16_t)(ent[off[i]] |
                                                (ent[off[i] + 1] << 8));
                        if (u == 0x0000 || u == 0xFFFF) break;
                        it->lfn[base + i] =
                            (u >= 0x20 && u < 0x7F) ? (char)u : '_';
                    }
                    it->lfn_valid = 1;
                }
                continue;
            }

            if (ent[11] & 0x08) {             /* volume label */
                it->lfn_valid = 0;
                it->rec_n = 0;
                continue;
            }

            /* short entry — a real file or directory */
            if (it->rec_n < FAT_REC_MAX) {
                it->rec_lba[it->rec_n] = it->sec_lba;
                it->rec_idx[it->rec_n] = idx;
                it->rec_n++;
            }

            if (it->lfn_valid && it->lfn[0]) {
                for (int i = 0; i < FAT_NAME_MAX; i++)
                    out->name[i] = it->lfn[i];
                out->name[FAT_NAME_MAX - 1] = '\0';
            } else {
                fat_decode_short(ent, out->name);
            }
            it->lfn_valid = 0;

            out->attr = ent[11];
            out->size = (uint32_t)ent[28] | ((uint32_t)ent[29] << 8) |
                        ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
            out->first_clus = ((uint32_t)ent[26] | ((uint32_t)ent[27] << 8)) |
                              (((uint32_t)ent[20] |
                                ((uint32_t)ent[21] << 8)) << 16);
            out->ent_lba = it->sec_lba;
            out->ent_idx = idx;

            /* skip . and .. */
            if (out->name[0] == '.' &&
                (out->name[1] == '\0' ||
                 (out->name[1] == '.' && out->name[2] == '\0'))) {
                it->rec_n = 0;
                continue;
            }
            return 1;
        }

        /* next sector / cluster */
        it->sec_loaded = 0;
        it->ent_in_sec = 0;
        it->sec_in_clus++;
        if (it->sec_in_clus >= fat_vol.spc) {
            it->sec_in_clus = 0;
            it->clus = fat_get(it->clus);
        }
    }
}

/* ===== PATH RESOLUTION ===== */

/* look up `name` inside directory cluster; optionally capture the iterator
 * record state (for deletion). returns 1 found / 0 not found / -1 error */
static int fat_find_in_dir(uint32_t dir_clus, const char *name,
                           fat_dirent_t *out, fat_iter_t *it_out) {
    fat_iter_t it;
    fat_iter_init(&it, dir_clus);
    fat_dirent_t e;
    int r;
    while ((r = fat_iter_next(&it, &e)) == 1) {
        if (fat_name_eq(e.name, name)) {
            if (out) *out = e;
            if (it_out) *it_out = it;
            return 1;
        }
    }
    return r < 0 ? -1 : 0;
}

/* resolve an absolute path ("/a/b/c"); "/" yields a pseudo root entry */
static int fat_lookup(const char *path, fat_dirent_t *out) {
    if (!fat_vol.mounted) return 0;

    fat_dirent_t cur;
    cur.name[0] = '/';
    cur.name[1] = '\0';
    cur.attr = FAT_ATTR_DIR;
    cur.size = 0;
    cur.first_clus = fat_vol.root_clus;
    cur.ent_lba = 0;
    cur.ent_idx = -1;

    const char *p = path;
    while (*p == '/') p++;

    char comp[FAT_NAME_MAX];
    while (*p) {
        int cl = 0;
        while (*p && *p != '/' && cl < FAT_NAME_MAX - 1)
            comp[cl++] = *p++;
        comp[cl] = '\0';
        while (*p == '/') p++;

        if (!(cur.attr & FAT_ATTR_DIR)) return 0;
        fat_dirent_t next;
        if (fat_find_in_dir(cur.first_clus, comp, &next, 0) != 1)
            return 0;
        cur = next;
    }
    if (out) *out = cur;
    return 1;
}

/* split "/a/b/c" into parent dir cluster + final name.
 * returns 1 ok / 0 parent missing or not a dir */
static int fat_split(const char *path, uint32_t *parent_clus,
                     char *name_out /* FAT_NAME_MAX */) {
    int len = str_len(path);
    int last = -1;
    for (int i = 0; i < len; i++)
        if (path[i] == '/') last = i;

    char parent[256];
    if (last <= 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        int n = last < 255 ? last : 255;
        for (int i = 0; i < n; i++) parent[i] = path[i];
        parent[n] = '\0';
    }
    const char *nm = (last >= 0) ? path + last + 1 : path;
    if (!*nm) return 0;
    str_copy(name_out, nm, FAT_NAME_MAX);

    fat_dirent_t d;
    if (!fat_lookup(parent, &d)) return 0;
    if (!(d.attr & FAT_ATTR_DIR)) return 0;
    *parent_clus = d.first_clus;
    return 1;
}

/* ===== FILE READ ===== */

static int fat_read_file(const fat_dirent_t *f, uint8_t *buf,
                         uint32_t maxlen, uint32_t *outlen) {
    uint32_t remaining = f->size < maxlen ? f->size : maxlen;
    uint32_t got = 0;
    uint32_t c = f->first_clus;
    uint32_t guard = 0;
    uint32_t clus_bytes = fat_vol.spc * 512;

    while (remaining > 0 && c >= 2 && c < FAT_EOC &&
           guard++ < fat_vol.nclusters + 2) {
        uint32_t lba = fat_cluster_lba(c);
        for (uint32_t s = 0; s < fat_vol.spc && remaining > 0; s++) {
            if (ata_read(lba + s, 1, fat_secbuf) != 0) return -1;
            uint32_t n = remaining < 512 ? remaining : 512;
            for (uint32_t i = 0; i < n; i++)
                buf[got + i] = fat_secbuf[i];
            got += n;
            remaining -= n;
        }
        c = fat_get(c);
        (void)clus_bytes;
    }
    if (outlen) *outlen = got;
    return 0;
}

/* ===== 8.3 NAME ENCODING ===== */

/* returns 0 if the name doesn't fit 8.3; fills field[11] + nt flags */
static int fat_encode_short(const char *name, uint8_t *field, uint8_t *nt) {
    int len = str_len(name);
    int dot = -1;
    for (int i = len - 1; i >= 0; i--)
        if (name[i] == '.') { dot = i; break; }

    int blen = dot >= 0 ? dot : len;
    int elen = dot >= 0 ? len - dot - 1 : 0;
    if (blen < 1 || blen > 8 || elen > 3) return 0;

    int base_lower = 1, base_upper = 1, ext_lower = 1, ext_upper = 1;
    for (int i = 0; i < 11; i++) field[i] = ' ';

    for (int i = 0; i < blen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { base_upper = 0; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') base_lower = 0;
        else if (!((c >= '0' && c <= '9') || c == '_' || c == '-' ||
                   c == '~' || c == '!' || c == '#' || c == '$' ||
                   c == '%' || c == '&' || c == '@' || c == '^'))
            return 0;
        field[i] = (uint8_t)c;
    }
    for (int i = 0; i < elen; i++) {
        char c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') { ext_upper = 0; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') ext_lower = 0;
        else if (!((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~'))
            return 0;
        field[8 + i] = (uint8_t)c;
    }

    if (!base_lower && !base_upper) return 0;   /* mixed case needs LFN */
    if (elen && !ext_lower && !ext_upper) return 0;

    *nt = 0;
    if (blen && base_lower) *nt |= 0x08;
    if (elen && ext_lower)  *nt |= 0x10;
    return 1;
}

/* ===== DIRECTORY MUTATION ===== */

/* find a free 32-byte slot in the directory, extending it if needed.
 * returns 0 ok (lba/idx set) / -1 error */
static int fat_find_slot(uint32_t dir_clus, uint32_t *out_lba, int *out_idx) {
    uint32_t c = dir_clus;
    uint32_t prev = c;
    uint32_t guard = 0;

    while (c >= 2 && c < FAT_EOC && guard++ < fat_vol.nclusters + 2) {
        for (uint32_t s = 0; s < fat_vol.spc; s++) {
            uint32_t lba = fat_cluster_lba(c) + s;
            if (ata_read(lba, 1, fat_secbuf) != 0) {
                fat_errstr = "dir read error";
                return -1;
            }
            for (int e = 0; e < 16; e++) {
                uint8_t b0 = fat_secbuf[e * 32];
                if (b0 == 0x00 || b0 == 0xE5) {
                    *out_lba = lba;
                    *out_idx = e;
                    return 0;
                }
            }
        }
        prev = c;
        c = fat_get(c);
    }

    /* directory is full — grow it by one zeroed cluster */
    uint32_t nc = fat_alloc_cluster();
    if (!nc) { fat_errstr = "disk full"; return -1; }
    fat_set(prev, nc);
    for (int i = 0; i < 512; i++) fat_secbuf[i] = 0;
    for (uint32_t s = 0; s < fat_vol.spc; s++)
        if (ata_write(fat_cluster_lba(nc) + s, 1, fat_secbuf) != 0) {
            fat_errstr = "dir grow error";
            return -1;
        }
    *out_lba = fat_cluster_lba(nc);
    *out_idx = 0;
    return 0;
}

/* write a fresh 8.3 dir entry at (lba, idx) */
static int fat_write_entry(uint32_t lba, int idx, const uint8_t *field,
                           uint8_t nt, uint8_t attr, uint32_t first,
                           uint32_t size) {
    if (ata_read(lba, 1, fat_secbuf) != 0) return -1;
    uint8_t *ent = fat_secbuf + idx * 32;
    for (int i = 0; i < 32; i++) ent[i] = 0;
    for (int i = 0; i < 11; i++) ent[i] = field[i];
    ent[11] = attr;
    ent[12] = nt;
    uint16_t date, tim;
    fat_now(&date, &tim);
    ent[14] = (uint8_t)tim;  ent[15] = (uint8_t)(tim >> 8);
    ent[16] = (uint8_t)date; ent[17] = (uint8_t)(date >> 8);
    ent[18] = (uint8_t)date; ent[19] = (uint8_t)(date >> 8);
    ent[20] = (uint8_t)(first >> 16); ent[21] = (uint8_t)(first >> 24);
    ent[22] = (uint8_t)tim;  ent[23] = (uint8_t)(tim >> 8);
    ent[24] = (uint8_t)date; ent[25] = (uint8_t)(date >> 8);
    ent[26] = (uint8_t)first; ent[27] = (uint8_t)(first >> 8);
    ent[28] = (uint8_t)size;        ent[29] = (uint8_t)(size >> 8);
    ent[30] = (uint8_t)(size >> 16); ent[31] = (uint8_t)(size >> 24);
    return ata_write(lba, 1, fat_secbuf);
}

/* patch first-cluster + size + mtime of an existing short entry */
static int fat_update_entry(uint32_t lba, int idx, uint32_t first,
                            uint32_t size) {
    if (ata_read(lba, 1, fat_secbuf) != 0) return -1;
    uint8_t *ent = fat_secbuf + idx * 32;
    uint16_t date, tim;
    fat_now(&date, &tim);
    ent[20] = (uint8_t)(first >> 16); ent[21] = (uint8_t)(first >> 24);
    ent[22] = (uint8_t)tim;  ent[23] = (uint8_t)(tim >> 8);
    ent[24] = (uint8_t)date; ent[25] = (uint8_t)(date >> 8);
    ent[26] = (uint8_t)first; ent[27] = (uint8_t)(first >> 8);
    ent[28] = (uint8_t)size;        ent[29] = (uint8_t)(size >> 8);
    ent[30] = (uint8_t)(size >> 16); ent[31] = (uint8_t)(size >> 24);
    return ata_write(lba, 1, fat_secbuf);
}

/* ===== PUBLIC OPERATIONS ===== */

static int fat_write_file(const char *path, const uint8_t *data,
                          uint32_t len) {
    fat_errstr = "";
    if (!fat_vol.mounted) { fat_errstr = "no writable volume"; return -1; }

    uint32_t parent;
    char name[FAT_NAME_MAX];
    if (!fat_split(path, &parent, name)) {
        fat_errstr = "no such directory";
        return -1;
    }

    fat_dirent_t existing;
    int have = fat_find_in_dir(parent, name, &existing, 0);
    if (have < 0) { fat_errstr = "io error"; return -1; }
    if (have == 1 && (existing.attr & FAT_ATTR_DIR)) {
        fat_errstr = "is a directory";
        return -1;
    }

    uint8_t field[11], nt;
    if (have == 0 && !fat_encode_short(name, field, &nt)) {
        fat_errstr = "name must fit 8.3 (e.g. NOTES.TXT)";
        return -1;
    }

    /* free the old chain before allocating the new one */
    if (have == 1 && existing.first_clus >= 2)
        fat_free_chain(existing.first_clus);

    uint32_t clus_bytes = fat_vol.spc * 512;
    uint32_t need = (len + clus_bytes - 1) / clus_bytes;
    uint32_t first = 0, prev = 0;
    uint32_t written = 0;

    for (uint32_t i = 0; i < need; i++) {
        uint32_t c = fat_alloc_cluster();
        if (!c) {
            if (first) fat_free_chain(first);
            fat_flush();
            fat_errstr = "disk full";
            return -1;
        }
        if (prev) fat_set(prev, c);
        else first = c;
        prev = c;

        uint32_t lba = fat_cluster_lba(c);
        for (uint32_t s = 0; s < fat_vol.spc; s++) {
            uint32_t left = len - written;
            if (left >= 512) {
                if (ata_write(lba + s, 1, data + written) != 0) {
                    fat_errstr = "io error";
                    return -1;
                }
                written += 512;
            } else {
                for (int b = 0; b < 512; b++)
                    fat_secbuf[b] = (uint32_t)b < left ? data[written + b] : 0;
                if (ata_write(lba + s, 1, fat_secbuf) != 0) {
                    fat_errstr = "io error";
                    return -1;
                }
                written += left;
            }
        }
    }

    int rc;
    if (have == 1) {
        rc = fat_update_entry(existing.ent_lba, existing.ent_idx, first, len);
    } else {
        uint32_t slot_lba;
        int slot_idx;
        if (fat_find_slot(parent, &slot_lba, &slot_idx) != 0) return -1;
        rc = fat_write_entry(slot_lba, slot_idx, field, nt, 0x20, first, len);
    }
    if (rc != 0) { fat_errstr = "io error"; return -1; }
    fat_flush();
    return 0;
}

static int fat_mkdir(const char *path) {
    fat_errstr = "";
    if (!fat_vol.mounted) { fat_errstr = "no writable volume"; return -1; }

    uint32_t parent;
    char name[FAT_NAME_MAX];
    if (!fat_split(path, &parent, name)) {
        fat_errstr = "no such directory";
        return -1;
    }
    if (fat_find_in_dir(parent, name, 0, 0) == 1) {
        fat_errstr = "already exists";
        return -1;
    }
    uint8_t field[11], nt;
    if (!fat_encode_short(name, field, &nt)) {
        fat_errstr = "name must fit 8.3";
        return -1;
    }

    uint32_t clus = fat_alloc_cluster();
    if (!clus) { fat_errstr = "disk full"; return -1; }

    /* zero the cluster, then write . and .. */
    for (int i = 0; i < 512; i++) fat_secbuf[i] = 0;
    for (uint32_t s = 1; s < fat_vol.spc; s++)
        if (ata_write(fat_cluster_lba(clus) + s, 1, fat_secbuf) != 0) {
            fat_errstr = "dir zero error";
            return -1;
        }

    uint16_t date, tim;
    fat_now(&date, &tim);
    for (int e = 0; e < 2; e++) {
        uint8_t *ent = fat_secbuf + e * 32;
        for (int i = 0; i < 11; i++) ent[i] = ' ';
        ent[0] = '.';
        if (e == 1) ent[1] = '.';
        ent[11] = FAT_ATTR_DIR;
        ent[22] = (uint8_t)tim;  ent[23] = (uint8_t)(tim >> 8);
        ent[24] = (uint8_t)date; ent[25] = (uint8_t)(date >> 8);
        uint32_t fc = e == 0 ? clus :
                      (parent == fat_vol.root_clus ? 0 : parent);
        ent[20] = (uint8_t)(fc >> 16); ent[21] = (uint8_t)(fc >> 24);
        ent[26] = (uint8_t)fc;         ent[27] = (uint8_t)(fc >> 8);
    }
    if (ata_write(fat_cluster_lba(clus), 1, fat_secbuf) != 0) {
        fat_errstr = "io error";
        return -1;
    }

    uint32_t slot_lba;
    int slot_idx;
    if (fat_find_slot(parent, &slot_lba, &slot_idx) != 0) return -1;
    if (fat_write_entry(slot_lba, slot_idx, field, nt,
                        FAT_ATTR_DIR, clus, 0) != 0) {
        fat_errstr = "io error";
        return -1;
    }
    fat_flush();
    return 0;
}

static int fat_dir_is_empty(uint32_t clus) {
    fat_iter_t it;
    fat_iter_init(&it, clus);
    fat_dirent_t e;
    return fat_iter_next(&it, &e) != 1;
}

static int fat_delete(const char *path) {
    fat_errstr = "";
    if (!fat_vol.mounted) { fat_errstr = "no writable volume"; return -1; }

    uint32_t parent;
    char name[FAT_NAME_MAX];
    if (!fat_split(path, &parent, name)) {
        fat_errstr = "no such directory";
        return -1;
    }

    fat_dirent_t e;
    fat_iter_t it;
    int have = fat_find_in_dir(parent, name, &e, &it);
    if (have != 1) { fat_errstr = "not found"; return -1; }

    if ((e.attr & FAT_ATTR_DIR) && !fat_dir_is_empty(e.first_clus)) {
        fat_errstr = "directory not empty";
        return -1;
    }

    /* mark the whole record (LFN parts + short entry) deleted */
    for (int i = 0; i < it.rec_n; i++) {
        if (ata_read(it.rec_lba[i], 1, fat_secbuf) != 0) {
            fat_errstr = "io error";
            return -1;
        }
        fat_secbuf[it.rec_idx[i] * 32] = 0xE5;
        if (ata_write(it.rec_lba[i], 1, fat_secbuf) != 0) {
            fat_errstr = "io error";
            return -1;
        }
    }

    if (e.first_clus >= 2)
        fat_free_chain(e.first_clus);
    fat_flush();
    return 0;
}

/* ===== MOUNT ===== */

static int fat32_try_vbr(uint32_t lba) {
    if (ata_read(lba, 1, fat_secbuf) != 0) return 0;
    if (fat_secbuf[510] != 0x55 || fat_secbuf[511] != 0xAA) return 0;
    if (fat_secbuf[0] != 0xEB && fat_secbuf[0] != 0xE9) return 0;

    uint16_t bps = (uint16_t)(fat_secbuf[11] | (fat_secbuf[12] << 8));
    uint8_t  spc = fat_secbuf[13];
    uint16_t resvd = (uint16_t)(fat_secbuf[14] | (fat_secbuf[15] << 8));
    uint8_t  nfats = fat_secbuf[16];
    uint16_t fatsz16 = (uint16_t)(fat_secbuf[22] | (fat_secbuf[23] << 8));
    uint32_t totsec = (uint32_t)fat_secbuf[32] |
                      ((uint32_t)fat_secbuf[33] << 8) |
                      ((uint32_t)fat_secbuf[34] << 16) |
                      ((uint32_t)fat_secbuf[35] << 24);
    uint32_t fatsz32 = (uint32_t)fat_secbuf[36] |
                       ((uint32_t)fat_secbuf[37] << 8) |
                       ((uint32_t)fat_secbuf[38] << 16) |
                       ((uint32_t)fat_secbuf[39] << 24);
    uint32_t root_clus = (uint32_t)fat_secbuf[44] |
                         ((uint32_t)fat_secbuf[45] << 8) |
                         ((uint32_t)fat_secbuf[46] << 16) |
                         ((uint32_t)fat_secbuf[47] << 24);
    uint16_t fsinfo = (uint16_t)(fat_secbuf[48] | (fat_secbuf[49] << 8));

    if (bps != 512) return 0;
    if (spc == 0 || (spc & (spc - 1)) != 0) return 0;
    if (fatsz16 != 0 || fatsz32 == 0) return 0;     /* not FAT32 */
    if (nfats == 0 || nfats > 2) return 0;
    if (root_clus < 2) return 0;
    if (fatsz32 * 512 > FAT_MAX_FAT_BYTES) {
        serial_puts("[fat32] FAT too large for in-RAM cache\n");
        return 0;
    }

    fat_vol.part_lba = lba;
    fat_vol.spc = spc;
    fat_vol.resvd = resvd;
    fat_vol.nfats = nfats;
    fat_vol.fatsz = fatsz32;
    fat_vol.root_clus = root_clus;
    fat_vol.data_lba = lba + resvd + nfats * fatsz32;
    fat_vol.nclusters = (totsec - resvd - nfats * fatsz32) / spc;
    uint32_t map_max = fatsz32 * 512 / 4 - 2;
    if (fat_vol.nclusters > map_max) fat_vol.nclusters = map_max;
    fat_vol.fsinfo_lba = fsinfo ? lba + fsinfo : 0;

    if (ata_read(lba + resvd, fatsz32, fat_table) != 0)
        return 0;
    for (uint32_t i = 0; i < sizeof(fat_dirty_bm); i++)
        fat_dirty_bm[i] = 0;
    fat_any_dirty = 0;
    fat_free_hint = 3;
    fat_vol.mounted = 1;
    return 1;
}

static void fat32_mount(void) {
    fat_vol.mounted = 0;
    if (!ata_present) return;

    if (fat32_try_vbr(0)) {
        serial_puts("[fat32] mounted superfloppy volume\n");
        return;
    }

    /* try MBR partitions */
    if (ata_read(0, 1, fat_secbuf) != 0) return;
    if (fat_secbuf[510] != 0x55 || fat_secbuf[511] != 0xAA) return;
    uint32_t starts[4];
    uint8_t types[4];
    for (int p = 0; p < 4; p++) {
        const uint8_t *pe = fat_secbuf + 446 + p * 16;
        types[p] = pe[4];
        starts[p] = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) |
                    ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
    }
    for (int p = 0; p < 4; p++) {
        if ((types[p] == 0x0B || types[p] == 0x0C) && starts[p]) {
            if (fat32_try_vbr(starts[p])) {
                serial_puts("[fat32] mounted MBR partition\n");
                return;
            }
        }
    }
    serial_puts("[fat32] no FAT32 volume found\n");
}

/* ===== INFO ===== */

static uint32_t fat_total_kb(void) {
    return fat_vol.mounted ?
        fat_vol.nclusters * fat_vol.spc / 2 : 0;
}

static uint32_t fat_free_kb(void) {
    return fat_vol.mounted ?
        fat_count_free() * fat_vol.spc / 2 : 0;
}

#endif /* FAT32_H */
