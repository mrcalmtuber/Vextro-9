#ifndef ZIM_H
#define ZIM_H

#include <stdint.h>
#include "zstd.h"
#include "lzma.h"   /* xz clusters, for archives built before the 2021 default change */

/*
 * ZIM archive reader — the format Kiwix uses to ship offline Wikipedia.
 *
 * The archive stays on disk and is read a window at a time through
 * fs_pread(); a full English dump is tens of gigabytes, so nothing is
 * ever loaded whole.  Only one decompressed cluster is held in memory,
 * and consecutive articles usually live in the same one.
 *
 * Layout, taken from the format's own reader rather than from memory,
 * because the header's on-disk order is not the order its fields are
 * declared in:
 *
 *   header (80 bytes)   magic, version, uuid, counts, and the file
 *                       offsets of everything below
 *   mime list           NUL-terminated strings, ending with an empty one
 *   path pointers       one uint64 per entry, ordered by (namespace, path)
 *   cluster pointers    one uint64 per cluster
 *   directory entries   variable length, at the offsets above
 *   clusters            compressed blobs, zstd or xz since the format
 *                       switched defaults in 2021
 *
 * Ordering of the path pointer list is what makes lookup cheap: a
 * binary search over 19 million entries is about two dozen reads.
 */

#define ZIM_MAGIC        0x044D495Au
#define ZIM_STR_MAX      192
#define ZIM_MIME_MAX     24

/* mime values that are not really mime types */
#define ZIM_MIME_REDIRECT   0xFFFF
#define ZIM_MIME_LINKTARGET 0xFFFE
#define ZIM_MIME_DELETED    0xFFFD

/* cluster compression, low nibble of the cluster's first byte */
#define ZIM_COMP_NONE1   0
#define ZIM_COMP_NONE    1
#define ZIM_COMP_ZLIB    2
#define ZIM_COMP_BZIP2   3
#define ZIM_COMP_XZ      4
#define ZIM_COMP_ZSTD    5

/* One cluster in, one cluster out.  Real archives keep clusters near
 * 2 MB decompressed; these have generous headroom over that. */
#define ZIM_CBUF_MAX     (6u << 20)
#define ZIM_DBUF_MAX     (12u << 20)

typedef struct {
    uint16_t mime;
    char     ns;
    uint32_t cluster, blob;
    uint32_t redirect;
    int      is_redirect;
    char     url[ZIM_STR_MAX];
    char     title[ZIM_STR_MAX];
} zim_dirent_t;

static struct {
    int      open;
    fs_file_t f;
    uint16_t major, minor;
    uint32_t article_count;
    uint32_t cluster_count;
    uint64_t path_ptr_pos;
    uint64_t title_idx_pos;
    uint64_t cluster_ptr_pos;
    uint64_t mime_list_pos;
    uint32_t main_page;
    uint64_t checksum_pos;
    int      truncated;          /* the file is shorter than it claims */
    char     mimes[ZIM_MIME_MAX][48];
    int      mime_count;
    uint32_t title_count;        /* entries in the title-ordered listing */
} zim;

/*
 * The title-ordered listing.
 *
 * Entries are stored sorted by *path*, and a path is a URL — underscores
 * for spaces, punctuation sorting before letters.  Browsing that order
 * shows `!`, `!!`, `!!!`, `"` before anything a reader wants, and typing
 * a title with spaces in it never matches.
 *
 * Modern archives ship the answer: an entry holding the article indices
 * in *title* order.  The header's own titlePtrPos is retired in version 6
 * (it reads as all-ones), so this listing is where it lives now.
 */
#define ZIM_TITLE_MAX 500000
static uint32_t zim_title_idx[ZIM_TITLE_MAX];

static int zim_load_title_listing(void);   /* defined below zim_content */

static const char *zim_err = "";

/* the one decompressed cluster we keep */
static uint8_t  zim_cbuf[ZIM_CBUF_MAX];
static uint8_t  zim_dbuf[ZIM_DBUF_MAX];
static uint32_t zim_cached = 0xFFFFFFFFu;
static uint64_t zim_dlen = 0;
static int      zim_cached_ext = 0;

/* ---- little-endian readers ---- */

static uint16_t zim_r16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t zim_r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t zim_r64(const uint8_t *p) {
    return (uint64_t)zim_r32(p) | ((uint64_t)zim_r32(p + 4) << 32);
}

static int zim_read(uint64_t off, void *buf, uint32_t len) {
    uint32_t got = 0;
    if (fs_pread(&zim.f, off, buf, len, &got) != 0) return -1;
    return got == len ? 0 : -1;
}

/* ---- open ---- */

static int zim_open(const char *path) {
    zim.open = 0;
    zim_cached = 0xFFFFFFFFu;
    zim_err = "";

    if (fs_open(path, &zim.f) != 0) { zim_err = "file not found"; return -1; }

    uint8_t h[80];
    if (zim_read(0, h, sizeof(h)) != 0) { zim_err = "cannot read header"; return -1; }

    if (zim_r32(h) != ZIM_MAGIC) { zim_err = "not a ZIM archive"; return -1; }

    zim.major = zim_r16(h + 4);
    zim.minor = zim_r16(h + 6);
    /* h+8 .. h+23 is the uuid */
    zim.article_count   = zim_r32(h + 24);
    zim.cluster_count   = zim_r32(h + 28);
    zim.path_ptr_pos    = zim_r64(h + 32);
    zim.title_idx_pos   = zim_r64(h + 40);
    zim.cluster_ptr_pos = zim_r64(h + 48);
    zim.mime_list_pos   = zim_r64(h + 56);
    zim.main_page       = zim_r32(h + 64);
    /* h+68 is layoutPage, unused */
    zim.checksum_pos    = zim_r64(h + 72);

    if (zim.major != 5 && zim.major != 6) {
        zim_err = "unsupported ZIM major version";
        return -1;
    }
    if (zim.article_count == 0 || zim.cluster_count == 0) {
        zim_err = "archive has no entries";
        return -1;
    }

    /* The checksum is 16 bytes at the very end, so this is an exact test
     * for a half-finished download — the common failure by far. */
    zim.truncated = (zim.checksum_pos + 16 != zim.f.size);

    /* mime list */
    zim.mime_count = 0;
    {
        static uint8_t mbuf[2048];
        uint32_t got = 0;
        if (fs_pread(&zim.f, zim.mime_list_pos, mbuf, sizeof(mbuf), &got) == 0) {
            uint32_t i = 0;
            while (i < got && zim.mime_count < ZIM_MIME_MAX) {
                if (mbuf[i] == 0) break;            /* empty string ends it */
                int j = 0;
                while (i < got && mbuf[i] && j < 47)
                    zim.mimes[zim.mime_count][j++] = (char)mbuf[i++];
                zim.mimes[zim.mime_count][j] = '\0';
                while (i < got && mbuf[i]) i++;     /* skip an overlong tail */
                i++;
                zim.mime_count++;
            }
        }
    }

    zim.open = 1;
    zim_load_title_listing();     /* optional: absent on older archives */
    return 0;
}

/* ---- directory entries ---- */

static int zim_dirent_at_offset(uint64_t off, zim_dirent_t *out) {
    uint8_t buf[512];
    uint32_t got = 0;
    if (fs_pread(&zim.f, off, buf, sizeof(buf), &got) != 0 || got < 16) {
        zim_err = "cannot read directory entry";
        return -1;
    }

    out->mime = zim_r16(buf);
    uint8_t param_len = buf[2];
    out->ns = (char)buf[3];
    /* buf+4 is the revision, always zero in practice */

    uint32_t p;
    out->is_redirect = (out->mime == ZIM_MIME_REDIRECT);
    if (out->is_redirect) {
        out->redirect = zim_r32(buf + 8);
        out->cluster = out->blob = 0;
        p = 12;
    } else {
        out->cluster = zim_r32(buf + 8);
        out->blob = zim_r32(buf + 12);
        out->redirect = 0;
        p = 16;
    }

    int i = 0;
    while (p < got && buf[p] && i < ZIM_STR_MAX - 1) out->url[i++] = (char)buf[p++];
    out->url[i] = '\0';
    if (p < got) p++;

    i = 0;
    while (p < got && buf[p] && i < ZIM_STR_MAX - 1) out->title[i++] = (char)buf[p++];
    out->title[i] = '\0';

    /* an empty title means the path doubles as the title */
    if (out->title[0] == '\0')
        str_copy(out->title, out->url, ZIM_STR_MAX);

    (void)param_len;
    return 0;
}

static int zim_dirent(uint32_t index, zim_dirent_t *out) {
    if (index >= zim.article_count) { zim_err = "entry index out of range"; return -1; }
    uint8_t p[8];
    if (zim_read(zim.path_ptr_pos + (uint64_t)index * 8, p, 8) != 0) {
        zim_err = "cannot read the path pointer list";
        return -1;
    }
    return zim_dirent_at_offset(zim_r64(p), out);
}

/* Follow redirects to the entry that actually holds content. */
static int zim_resolve(uint32_t index, zim_dirent_t *out) {
    for (int hops = 0; hops < 8; hops++) {
        if (zim_dirent(index, out) != 0) return -1;
        if (!out->is_redirect) return 0;
        index = out->redirect;
    }
    zim_err = "redirect loop";
    return -1;
}

/* ---- lookup ----
 * The path list is ordered by namespace then path, so a plain binary
 * search finds an entry in about log2(19,000,000) reads. */

static int zim_cmp(char ns_a, const char *a, char ns_b, const char *b) {
    if (ns_a != ns_b) return ns_a < ns_b ? -1 : 1;
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* Index of the first entry >= (ns, url). */
static uint32_t zim_lower_bound(char ns, const char *url) {
    uint32_t lo = 0, hi = zim.article_count;
    zim_dirent_t e;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (zim_dirent(mid, &e) != 0) break;
        if (zim_cmp(e.ns, e.url, ns, url) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int zim_find(char ns, const char *url, uint32_t *index) {
    uint32_t i = zim_lower_bound(ns, url);
    if (i >= zim.article_count) return 0;
    zim_dirent_t e;
    if (zim_dirent(i, &e) != 0) return 0;
    if (zim_cmp(e.ns, e.url, ns, url) != 0) return 0;
    *index = i;
    return 1;
}

/* ---- clusters ---- */

static int zim_cluster_range(uint32_t n, uint64_t *off, uint64_t *end) {
    if (n >= zim.cluster_count) { zim_err = "cluster out of range"; return -1; }
    uint8_t p[8];
    if (zim_read(zim.cluster_ptr_pos + (uint64_t)n * 8, p, 8) != 0) {
        zim_err = "cannot read the cluster pointer list";
        return -1;
    }
    *off = zim_r64(p);
    if (n + 1 < zim.cluster_count) {
        if (zim_read(zim.cluster_ptr_pos + (uint64_t)(n + 1) * 8, p, 8) != 0) {
            zim_err = "cannot read the cluster pointer list";
            return -1;
        }
        *end = zim_r64(p);
    } else {
        *end = zim.checksum_pos;                  /* last cluster runs to the end */
    }
    if (*end <= *off) { zim_err = "bad cluster extent"; return -1; }
    return 0;
}

static int zim_load_cluster(uint32_t n) {
    if (zim_cached == n) return 0;

    uint64_t off, end;
    if (zim_cluster_range(n, &off, &end) != 0) return -1;

    uint64_t clen = end - off;
    if (clen > ZIM_CBUF_MAX) { zim_err = "cluster larger than the read buffer"; return -1; }

    uint32_t got = 0;
    if (fs_pread(&zim.f, off, zim_cbuf, (uint32_t)clen, &got) != 0 || got < 1) {
        zim_err = "cannot read cluster";
        return -1;
    }

    uint8_t info = zim_cbuf[0];
    int comp = info & 0x0F;
    zim_cached_ext = (info & 0x10) ? 1 : 0;

    const uint8_t *payload = zim_cbuf + 1;
    uint32_t plen = got - 1;
    const char *cerr = "decompression failed";

    if (comp == ZIM_COMP_NONE || comp == ZIM_COMP_NONE1) {
        if (plen > ZIM_DBUF_MAX) { zim_err = "cluster too large"; return -1; }
        for (uint32_t i = 0; i < plen; i++) zim_dbuf[i] = payload[i];
        zim_dlen = plen;
    } else if (comp == ZIM_COMP_ZSTD) {
        if (zstd_decode(payload, plen, zim_dbuf, ZIM_DBUF_MAX,
                        &zim_dlen, &cerr) != 0) {
            zim_err = cerr;
            return -1;
        }
    } else if (comp == ZIM_COMP_XZ) {
        if (xz_decode(payload, plen, zim_dbuf, ZIM_DBUF_MAX,
                      &zim_dlen, &cerr) != 0) {
            zim_err = cerr;
            return -1;
        }
    } else if (comp == ZIM_COMP_ZLIB) {
        zim_err = "cluster uses zlib, which this reader does not support";
        return -1;
    } else if (comp == ZIM_COMP_BZIP2) {
        zim_err = "cluster uses bzip2, which this reader does not support";
        return -1;
    } else {
        zim_err = "unknown cluster compression";
        return -1;
    }

    zim_cached = n;
    return 0;
}

/*
 * A cluster starts with an offset table; the first offset is also the
 * table's own size, which is how the blob count is discovered.
 */
static int zim_blob(uint32_t cluster, uint32_t blob,
                    const uint8_t **data, uint32_t *len) {
    if (zim_load_cluster(cluster) != 0) return -1;

    uint32_t esize = zim_cached_ext ? 8 : 4;
    if (zim_dlen < esize) { zim_err = "cluster is empty"; return -1; }

    uint64_t first = zim_cached_ext ? zim_r64(zim_dbuf) : zim_r32(zim_dbuf);
    if (first < esize || first > zim_dlen) { zim_err = "bad blob table"; return -1; }
    uint32_t count = (uint32_t)(first / esize) - 1;
    if (blob >= count) { zim_err = "blob index out of range"; return -1; }

    uint64_t a = zim_cached_ext ? zim_r64(zim_dbuf + (uint64_t)blob * 8)
                                : zim_r32(zim_dbuf + (uint64_t)blob * 4);
    uint64_t b = zim_cached_ext ? zim_r64(zim_dbuf + (uint64_t)(blob + 1) * 8)
                                : zim_r32(zim_dbuf + (uint64_t)(blob + 1) * 4);
    if (b < a || b > zim_dlen) { zim_err = "bad blob extent"; return -1; }

    *data = zim_dbuf + a;
    *len = (uint32_t)(b - a);
    return 0;
}

/* Fetch an entry's content, following redirects. */
static int zim_content(uint32_t index, const uint8_t **data, uint32_t *len,
                       zim_dirent_t *out) {
    zim_dirent_t e;
    if (zim_resolve(index, &e) != 0) return -1;
    if (e.mime >= ZIM_MIME_DELETED) { zim_err = "entry has no content"; return -1; }
    if (zim_blob(e.cluster, e.blob, data, len) != 0) return -1;
    if (out) *out = e;
    return 0;
}

static const char *zim_mime_name(uint16_t m) {
    if (m == ZIM_MIME_REDIRECT) return "redirect";
    if (m < (uint16_t)zim.mime_count) return zim.mimes[m];
    return "?";
}

/*
 * Read the title-ordered listing into memory.
 *
 * This deliberately does not go through zim_content().  The listing lives
 * in an *uncompressed* cluster, and in a Simple English dump that cluster
 * is 24 MB — four times the cluster read buffer, so the normal path
 * refuses it.  But an uncompressed cluster is just bytes sitting in the
 * file, so the blob can be read straight out at its own offset: no
 * decompression, no staging, one read of exactly the wanted range.
 *
 * Absence is not an error.  Older archives have no such entry, and the
 * caller falls back to path order.
 */
static int zim_load_title_listing(void) {
    zim.title_count = 0;

    uint32_t e;
    /* note zim_find is 1-on-success, unlike its neighbours */
    if (!zim_find('X', "listing/titleOrdered/v1", &e)) return -1;

    zim_dirent_t d;
    if (zim_dirent(e, &d) != 0 || d.is_redirect) return -1;

    uint64_t coff, cend;
    if (zim_cluster_range(d.cluster, &coff, &cend) != 0) return -1;

    uint8_t info;
    if (zim_read(coff, &info, 1) != 0) return -1;
    int comp = info & 0x0F;
    int ext  = (info & 0x10) ? 1 : 0;

    const uint8_t *blob = 0;
    uint32_t blen = 0;

    if (comp == ZIM_COMP_NONE || comp == ZIM_COMP_NONE1) {
        /* offsets are relative to the byte after the info byte */
        uint64_t base = coff + 1;
        uint32_t w = ext ? 8 : 4;
        uint8_t ob[16];
        if (zim_read(base + (uint64_t)d.blob * w, ob, 2 * w) != 0) return -1;
        uint64_t o0 = ext ? zim_r64(ob)     : zim_r32(ob);
        uint64_t o1 = ext ? zim_r64(ob + w) : zim_r32(ob + w);
        if (o1 <= o0) return -1;

        uint64_t n = (o1 - o0) / 4;
        if (n > ZIM_TITLE_MAX) n = ZIM_TITLE_MAX;

        /* straight into the index array, then byte-swapped in place */
        uint8_t *dst = (uint8_t *)zim_title_idx;
        if (zim_read(base + o0, dst, (uint32_t)(n * 4)) != 0) return -1;
        for (uint64_t i = 0; i < n; i++)
            zim_title_idx[i] = zim_r32(dst + i * 4);
        zim.title_count = (uint32_t)n;
        return 0;
    }

    /* compressed listing: only workable if the cluster fits the buffers */
    if (zim_content(e, &blob, &blen, 0) != 0) return -1;
    uint32_t n = blen / 4;
    if (n > ZIM_TITLE_MAX) n = ZIM_TITLE_MAX;
    for (uint32_t i = 0; i < n; i++)
        zim_title_idx[i] = zim_r32(blob + i * 4);
    zim.title_count = n;
    return 0;
}

/* Path-list index of the article at `rank` in title order. */
static uint32_t zim_title_at(uint32_t rank) {
    return rank < zim.title_count ? zim_title_idx[rank] : 0;
}

/*
 * Note on sorting: the listing is ordered bytewise on the title *after*
 * the empty-title fallback in zim_dirent_at_offset — an empty title field
 * means "same as the path", not "no title".  That fallback is what makes
 * the order exact; without it a search for "New York" lands on "Noynoy
 * Aquino".  e.title is therefore always safe to compare directly.
 */

#endif /* ZIM_H */
