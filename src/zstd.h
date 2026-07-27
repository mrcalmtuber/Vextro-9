#ifndef ZSTD_H
#define ZSTD_H

#include <stdint.h>

/*
 * Zstandard decompressor.
 *
 * ZIM archives have defaulted to zstd since 2021, so an offline
 * Wikipedia cluster cannot be opened without one.  This is an original
 * implementation written from RFC 8878; the normalized distributions and
 * baseline tables below are the interoperability constants the RFC
 * publishes for exactly that purpose.
 *
 * Three entropy layers stack up:
 *
 *   FSE (tANS) decodes the sequence symbols and, sometimes, the Huffman
 *   weights.  Its bitstreams run backwards — the encoder writes forward,
 *   so the decoder starts at the last byte, whose highest set bit marks
 *   where the real data ends.
 *
 *   Huffman decodes the literals, in either one stream or four
 *   interleaved ones with a jump table.
 *
 *   Sequences then weave literals and matches together, with three
 *   recency-ordered repeat offsets whose update rules have an awkward
 *   special case when a sequence copies no literals at all.
 *
 * Freestanding: no allocation.  Output goes straight into the caller's
 * buffer, which doubles as the match window, exactly as the LZMA decoder
 * in lzma.h does.
 */

#define ZSTD_MAGIC        0xFD2FB528u
#define ZSTD_MAGIC_SKIP   0x184D2A50u   /* .. 0x184D2A5F: skippable */
#define ZSTD_BLOCK_MAX    (128 * 1024)

/* per-context FSE accuracy ceilings (RFC 8878 section 3.1.1.3.2.1) */
#define ZSTD_LL_LOG_MAX   9
#define ZSTD_ML_LOG_MAX   9
#define ZSTD_OF_LOG_MAX   8
#define ZSTD_HUF_LOG_MAX  6
#define ZSTD_FSE_LOG_MAX  9

#define ZSTD_LL_MAX       35
#define ZSTD_ML_MAX       52
#define ZSTD_OF_MAX       31

#define ZSTD_HUF_LOG      11            /* literals Huffman table log cap */

/* ---- small helpers ---- */

static uint32_t zs_rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t zs_rd24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t zs_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t zs_rd64(const uint8_t *p) {
    return (uint64_t)zs_rd32(p) | ((uint64_t)zs_rd32(p + 4) << 32);
}

static int zs_highbit(uint32_t v) {           /* floor(log2(v)), v >= 1 */
    int n = 0;
    while (v >>= 1) n++;
    return n;
}

/* ---- backward bit reader ---- */

typedef struct {
    const uint8_t *start;
    const uint8_t *ptr;
    uint64_t bits;
    int      consumed;      /* bits taken from the top of `bits` */
    int      size;
} zbr_t;

static int zbr_init(zbr_t *b, const uint8_t *src, uint32_t size) {
    if (size == 0) return -1;
    uint8_t last = src[size - 1];
    if (last == 0) return -1;              /* the end mark cannot be zero */
    int pad = 8 - zs_highbit(last);
    b->start = src;
    b->size = (int)size;
    if (size >= 8) {
        b->ptr = src + size - 8;
        b->bits = zs_rd64(b->ptr);
        b->consumed = pad;
    } else {
        b->ptr = src;
        b->bits = 0;
        for (uint32_t i = 0; i < size; i++)
            b->bits |= (uint64_t)src[i] << (8 * i);
        b->consumed = pad + (int)(8 - size) * 8;
    }
    return 0;
}

static uint32_t zbr_read(zbr_t *b, int n) {
    if (n <= 0) return 0;
    uint32_t v = (uint32_t)((b->bits << (b->consumed & 63)) >> 1 >> (63 - n));
    b->consumed += n;
    return v;
}

/* 0 unfinished, 1 at the first word, 2 exactly consumed, 3 overrun */
static int zbr_reload(zbr_t *b) {
    if (b->consumed > 64) return 3;
    if (b->size >= 8 && b->ptr >= b->start + 8) {
        b->ptr -= b->consumed >> 3;
        b->consumed &= 7;
        b->bits = zs_rd64(b->ptr);
        return 0;
    }
    if (b->ptr == b->start)
        return b->consumed < 64 ? 1 : 2;

    {
        int nb = b->consumed >> 3;
        if (b->ptr - nb < b->start) nb = (int)(b->ptr - b->start);
        b->ptr -= nb;
        b->consumed -= nb * 8;
        b->bits = zs_rd64(b->ptr);
        return 0;
    }
}

/* ---- FSE ---- */

typedef struct {
    uint16_t baseline;
    uint8_t  symbol;
    uint8_t  nbBits;
} zfse_t;

typedef struct {
    uint32_t       state;
    const zfse_t  *dt;
} zfse_state_t;

/* 32 bits from an absolute bit offset, zero-padded past the end. */
static uint32_t zs_bits_at(const uint8_t *in, uint32_t size, uint32_t bitpos) {
    uint32_t byte = bitpos >> 3;
    int off = (int)(bitpos & 7);
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t idx = byte + (uint32_t)i;
        v |= (uint64_t)(idx < size ? in[idx] : 0) << (8 * i);
    }
    return (uint32_t)(v >> off);
}

/*
 * Read a normalized distribution.  Field widths shrink as probability
 * is handed out, and small values use one bit fewer, so the reader has
 * to track the running remainder to know how wide the next field is.
 */
static int zfse_read_ncount(int16_t *norm, int *maxSVp, int *tableLogp,
                            const uint8_t *in, uint32_t size, uint32_t *used,
                            int log_max) {
    if (size < 1) return -1;
    uint32_t bitpos = 0;
    uint32_t bs = zs_bits_at(in, size, 0);

    int tableLog = (int)(bs & 0xF) + 5;
    if (tableLog > log_max) return -1;
    bitpos = 4;

    int remaining = (1 << tableLog) + 1;
    int threshold = 1 << tableLog;
    int nbBits = tableLog + 1;
    int charnum = 0;
    int previous0 = 0;
    int maxSV = *maxSVp;

    while (remaining > 1 && charnum <= maxSV) {
        if (previous0) {
            /* a run of zero-probability symbols, 2 bits at a time */
            int n0 = charnum;
            for (;;) {
                bs = zs_bits_at(in, size, bitpos);
                if ((bs & 3) != 3) break;
                n0 += 3;
                bitpos += 2;
            }
            bs = zs_bits_at(in, size, bitpos);
            n0 += (int)(bs & 3);
            bitpos += 2;
            if (n0 > maxSV + 1) return -1;
            while (charnum < n0) norm[charnum++] = 0;
            previous0 = 0;
            if (charnum > maxSV) break;
        }

        bs = zs_bits_at(in, size, bitpos);
        {
            int max = (2 * threshold - 1) - remaining;
            int count;
            if ((int)(bs & (uint32_t)(threshold - 1)) < max) {
                count = (int)(bs & (uint32_t)(threshold - 1));
                bitpos += (uint32_t)(nbBits - 1);
            } else {
                count = (int)(bs & (uint32_t)(2 * threshold - 1));
                if (count >= threshold) count -= max;
                bitpos += (uint32_t)nbBits;
            }
            count--;                       /* -1 encodes "less than one" */
            remaining -= count < 0 ? -count : count;
            if (charnum > maxSV) return -1;
            norm[charnum++] = (int16_t)count;
            previous0 = (count == 0);
            while (remaining < threshold) {
                nbBits--;
                threshold >>= 1;
            }
        }
        if (bitpos > size * 8 + 32) return -1;
    }
    if (remaining != 1) return -1;
    if (charnum < 1) return -1;

    *maxSVp = charnum - 1;
    *tableLogp = tableLog;
    *used = (bitpos + 7) >> 3;
    if (*used > size) return -1;
    return 0;
}

/*
 * Build the decoding table.  Low-probability symbols claim single cells
 * from the top; everything else is spread with the specification's
 * stride so that consecutive states rarely share a cache line.  Then
 * each symbol's cells are numbered in natural order, and the lower ones
 * are the ones that need an extra bit.
 */
static int zfse_build(zfse_t *dt, const int16_t *norm, int maxSV,
                      int tableLog) {
    if (tableLog > ZSTD_FSE_LOG_MAX) return -1;
    uint32_t tableSize = 1u << tableLog;
    uint32_t highThreshold = tableSize - 1;
    static uint8_t symbolTable[1u << ZSTD_FSE_LOG_MAX];
    static uint16_t symbolNext[256];

    for (int s = 0; s <= maxSV; s++) {
        if (norm[s] == -1) {
            symbolTable[highThreshold--] = (uint8_t)s;
            symbolNext[s] = 1;
        } else {
            symbolNext[s] = (uint16_t)norm[s];
        }
    }

    uint32_t step = (tableSize >> 1) + (tableSize >> 3) + 3;
    uint32_t mask = tableSize - 1;
    uint32_t pos = 0;
    for (int s = 0; s <= maxSV; s++) {
        for (int16_t i = 0; i < norm[s]; i++) {
            symbolTable[pos] = (uint8_t)s;
            do {
                pos = (pos + step) & mask;
            } while (pos > highThreshold);
        }
    }
    if (pos != 0) return -1;            /* the walk must close the cycle */

    for (uint32_t u = 0; u < tableSize; u++) {
        uint8_t sym = symbolTable[u];
        uint32_t next = symbolNext[sym]++;
        int nb = tableLog - zs_highbit(next);
        dt[u].symbol = sym;
        dt[u].nbBits = (uint8_t)nb;
        dt[u].baseline = (uint16_t)((next << nb) - tableSize);
    }
    return 0;
}

static void zfse_build_rle(zfse_t *dt, uint8_t symbol) {
    dt[0].symbol = symbol;
    dt[0].nbBits = 0;
    dt[0].baseline = 0;
}

static void zfse_init(zfse_state_t *st, const zfse_t *dt, zbr_t *b,
                      int tableLog) {
    st->dt = dt;
    st->state = zbr_read(b, tableLog);
}

static uint8_t zfse_symbol(const zfse_state_t *st) {
    return st->dt[st->state].symbol;
}

static void zfse_update(zfse_state_t *st, zbr_t *b) {
    const zfse_t *e = &st->dt[st->state];
    st->state = (uint32_t)e->baseline + zbr_read(b, e->nbBits);
}

/* ---- Huffman ---- */

typedef struct {
    uint8_t symbol;
    uint8_t nbBits;
} zhuf_e_t;

static zhuf_e_t zhuf_table[1u << ZSTD_HUF_LOG];
static int      zhuf_log;

static int zhuf_build_from_weights(const uint8_t *weights, int nsym) {
    uint32_t total = 0;
    for (int i = 0; i < nsym; i++) {
        if (weights[i] > ZSTD_HUF_LOG) return -1;
        if (weights[i]) total += 1u << (weights[i] - 1);
    }
    if (total == 0) return -1;

    int maxBits = zs_highbit(total) + 1;
    if (maxBits > ZSTD_HUF_LOG) return -1;
    uint32_t leftover = (1u << maxBits) - total;
    /* the final symbol's weight is whatever exactly fills the table */
    if (leftover == 0 || (leftover & (leftover - 1)) != 0) return -1;

    static uint8_t w[256];
    for (int i = 0; i < nsym; i++) w[i] = weights[i];
    uint8_t derived = (uint8_t)(zs_highbit(leftover) + 1);
    if (derived > ZSTD_HUF_LOG) return -1;
    w[nsym] = derived;
    int total_syms = nsym + 1;
    if (total_syms > 256) return -1;

    uint32_t rankCount[ZSTD_HUF_LOG + 2];
    for (int i = 0; i <= ZSTD_HUF_LOG + 1; i++) rankCount[i] = 0;
    for (int i = 0; i < total_syms; i++) rankCount[w[i]]++;

    /* Symbols sort by weight, keeping natural order inside a weight; a
     * symbol of weight k occupies 1<<(k-1) table cells. */
    uint32_t rankStart[ZSTD_HUF_LOG + 2];
    uint32_t pos = 0;
    for (int k = 1; k <= maxBits; k++) {
        rankStart[k] = pos;
        pos += rankCount[k] << (k - 1);
    }
    if (pos != (1u << maxBits)) return -1;

    for (int s = 0; s < total_syms; s++) {
        int k = w[s];
        if (k == 0) continue;
        int nb = maxBits + 1 - k;
        uint32_t n = 1u << (k - 1);
        for (uint32_t i = 0; i < n; i++) {
            zhuf_table[rankStart[k] + i].symbol = (uint8_t)s;
            zhuf_table[rankStart[k] + i].nbBits = (uint8_t)nb;
        }
        rankStart[k] += n;
    }
    zhuf_log = maxBits;
    return 0;
}

/* Read the tree description; returns bytes consumed or -1. */
static int zhuf_read_tree(const uint8_t *in, uint32_t size) {
    if (size < 1) return -1;
    uint8_t hb = in[0];
    static uint8_t weights[256];

    if (hb >= 128) {
        int nsym = hb - 127;
        uint32_t nbytes = ((uint32_t)nsym + 1) / 2;
        if (1 + nbytes > size) return -1;
        for (int i = 0; i < nsym; i++)
            weights[i] = (i & 1) ? (in[1 + i / 2] & 0xF)
                                 : (uint8_t)(in[1 + i / 2] >> 4);
        if (zhuf_build_from_weights(weights, nsym) != 0) return -1;
        return (int)(1 + nbytes);
    }

    /* FSE-compressed weights: one bitstream, two interleaved states */
    uint32_t csize = hb;
    if (1 + csize > size || csize < 1) return -1;

    static int16_t norm[256];
    static zfse_t dt[1u << ZSTD_HUF_LOG_MAX];
    int maxSV = 255, tableLog = 0;
    uint32_t used = 0;
    if (zfse_read_ncount(norm, &maxSV, &tableLog, in + 1, csize, &used,
                         ZSTD_HUF_LOG_MAX) != 0)
        return -1;
    if (zfse_build(dt, norm, maxSV, tableLog) != 0) return -1;

    zbr_t b;
    if (zbr_init(&b, in + 1 + used, csize - used) != 0) return -1;

    zfse_state_t s1, s2;
    zfse_init(&s1, dt, &b, tableLog);
    zfse_init(&s2, dt, &b, tableLog);

    /* The two states take turns.  Decoding stops when advancing a state
     * would need bits the stream no longer has; the other state's symbol
     * is then emitted and the series is complete. */
    int n = 0;
    for (;;) {
        if (n + 2 > 255) return -1;
        weights[n++] = zfse_symbol(&s1);
        zfse_update(&s1, &b);
        if (zbr_reload(&b) == 3) {          /* only an overrun ends it */
            weights[n++] = zfse_symbol(&s2);
            break;
        }
        weights[n++] = zfse_symbol(&s2);
        zfse_update(&s2, &b);
        if (zbr_reload(&b) == 3) {
            weights[n++] = zfse_symbol(&s1);
            break;
        }
    }

    if (zhuf_build_from_weights(weights, n) != 0) return -1;
    return (int)(1 + csize);
}

static int zhuf_decode_stream(uint8_t *out, uint32_t count,
                              const uint8_t *in, uint32_t size) {
    zbr_t b;
    if (count == 0) return 0;
    if (zbr_init(&b, in, size) != 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        /* The container holds 64 bits; a code is at most 11.  Refilling
         * every symbol keeps `consumed` inside the container, which the
         * peek mask assumes. */
        zbr_reload(&b);
        if (b.consumed > 64) return -1;
        uint32_t idx = zbr_read(&b, zhuf_log);
        b.consumed -= zhuf_log;                 /* peek, not consume */
        const zhuf_e_t *e = &zhuf_table[idx];
        b.consumed += e->nbBits;
        out[i] = e->symbol;
    }
    return 0;
}

/* ---- default distributions (RFC 8878 section 3.1.1.3.2.2) ---- */

static const int16_t zstd_ll_default[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1
};
static const int16_t zstd_ml_default[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1
};
static const int16_t zstd_of_default[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};

/* baselines and extra bits (RFC 8878 tables 16 and 17) */
static const uint32_t zstd_ll_base[36] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024,
    2048, 4096, 8192, 16384, 32768, 65536
};
static const uint8_t zstd_ll_bits[36] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16
};
static const uint32_t zstd_ml_base[53] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515,
    1027, 2051, 4099, 8195, 16387, 32771, 65539
};
static const uint8_t zstd_ml_bits[53] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16
};

/* ---- decoder state carried across blocks ---- */

static zfse_t zstd_ll_dt[1u << ZSTD_LL_LOG_MAX];
static zfse_t zstd_ml_dt[1u << ZSTD_ML_LOG_MAX];
static zfse_t zstd_of_dt[1u << ZSTD_OF_LOG_MAX];
static int    zstd_ll_log, zstd_ml_log, zstd_of_log;
static int    zstd_have_ll, zstd_have_ml, zstd_have_of;

static uint8_t  zstd_literals[ZSTD_BLOCK_MAX];
static uint32_t zstd_lit_size;
static uint32_t zstd_rep[3];

static const char *zstd_err;

static int zstd_fail(const char *msg) {
    if (!zstd_err) zstd_err = msg;
    return -1;
}

/* ---- literals section ---- */

static int zstd_decode_literals(const uint8_t *in, uint32_t size,
                                uint32_t *consumed) {
    if (size < 1) return zstd_fail("truncated literals header");
    uint32_t type = in[0] & 3;
    uint32_t fmt = (in[0] >> 2) & 3;
    uint32_t regen = 0, comp = 0, hdr = 0;

    if (type == 0 || type == 1) {                 /* raw / RLE */
        if (fmt == 1) {
            if (size < 2) return zstd_fail("truncated literals header");
            regen = (in[0] >> 4) | ((uint32_t)in[1] << 4);
            hdr = 2;
        } else if (fmt == 3) {
            if (size < 3) return zstd_fail("truncated literals header");
            regen = (in[0] >> 4) | ((uint32_t)in[1] << 4) |
                    ((uint32_t)in[2] << 12);
            hdr = 3;
        } else {
            regen = in[0] >> 3;
            hdr = 1;
        }
        if (regen > ZSTD_BLOCK_MAX) return zstd_fail("literals too large");

        if (type == 0) {
            if (hdr + regen > size) return zstd_fail("truncated raw literals");
            for (uint32_t i = 0; i < regen; i++) zstd_literals[i] = in[hdr + i];
            *consumed = hdr + regen;
        } else {
            if (hdr + 1 > size) return zstd_fail("truncated RLE literals");
            for (uint32_t i = 0; i < regen; i++) zstd_literals[i] = in[hdr];
            *consumed = hdr + 1;
        }
        zstd_lit_size = regen;
        return 0;
    }

    /* compressed (2) or treeless (3) */
    int streams = 4;
    if (fmt == 0) {
        if (size < 3) return zstd_fail("truncated literals header");
        streams = 1;
        uint32_t v = zs_rd24(in) >> 4;
        regen = v & 0x3FF;
        comp = (v >> 10) & 0x3FF;
        hdr = 3;
    } else if (fmt == 1) {
        if (size < 3) return zstd_fail("truncated literals header");
        uint32_t v = zs_rd24(in) >> 4;
        regen = v & 0x3FF;
        comp = (v >> 10) & 0x3FF;
        hdr = 3;
    } else if (fmt == 2) {
        if (size < 4) return zstd_fail("truncated literals header");
        uint32_t v = zs_rd32(in) >> 4;
        regen = v & 0x3FFF;
        comp = (v >> 14) & 0x3FFF;
        hdr = 4;
    } else {
        if (size < 5) return zstd_fail("truncated literals header");
        uint64_t v = ((uint64_t)zs_rd32(in) | ((uint64_t)in[4] << 32)) >> 4;
        regen = (uint32_t)(v & 0x3FFFF);
        comp = (uint32_t)((v >> 18) & 0x3FFFF);
        hdr = 5;
    }
    if (regen > ZSTD_BLOCK_MAX) return zstd_fail("literals too large");
    if (hdr + comp > size) return zstd_fail("truncated literals payload");

    const uint8_t *p = in + hdr;
    uint32_t left = comp;

    if (type == 2) {                              /* a new tree is present */
        int used = zhuf_read_tree(p, left);
        if (used < 0) return zstd_fail("bad Huffman tree description");
        p += used;
        left -= (uint32_t)used;
    } else if (zhuf_log == 0) {
        return zstd_fail("treeless literals with no previous tree");
    }

    if (streams == 1) {
        if (zhuf_decode_stream(zstd_literals, regen, p, left) != 0)
            return zstd_fail("literal stream decode failed");
    } else {
        if (left < 6) return zstd_fail("truncated literals jump table");
        uint32_t s1 = zs_rd16(p), s2 = zs_rd16(p + 2), s3 = zs_rd16(p + 4);
        p += 6;
        left -= 6;
        if (s1 + s2 + s3 > left) return zstd_fail("bad literals jump table");
        uint32_t s4 = left - s1 - s2 - s3;

        uint32_t seg = (regen + 3) / 4;
        uint32_t last = regen - 3 * seg;
        if (3 * seg > regen) return zstd_fail("bad literals segmentation");

        if (zhuf_decode_stream(zstd_literals, seg, p, s1) != 0 ||
            zhuf_decode_stream(zstd_literals + seg, seg, p + s1, s2) != 0 ||
            zhuf_decode_stream(zstd_literals + 2 * seg, seg,
                               p + s1 + s2, s3) != 0 ||
            zhuf_decode_stream(zstd_literals + 3 * seg, last,
                               p + s1 + s2 + s3, s4) != 0)
            return zstd_fail("literal stream decode failed");
    }

    zstd_lit_size = regen;
    *consumed = hdr + comp;
    return 0;
}

/* ---- sequences ---- */

static int zstd_load_fse(int mode, zfse_t *dt, int *log, int *have,
                         const int16_t *dflt, int dflt_max, int dflt_log,
                         int log_max, int sym_max,
                         const uint8_t *in, uint32_t size, uint32_t *used) {
    *used = 0;
    if (mode == 0) {                              /* predefined */
        if (zfse_build(dt, dflt, dflt_max, dflt_log) != 0)
            return zstd_fail("bad predefined table");
        *log = dflt_log;
        *have = 1;
        return 0;
    }
    if (mode == 1) {                              /* RLE */
        if (size < 1) return zstd_fail("truncated RLE table");
        if (in[0] > sym_max) return zstd_fail("RLE symbol out of range");
        zfse_build_rle(dt, in[0]);
        *log = 0;
        *have = 1;
        *used = 1;
        return 0;
    }
    if (mode == 2) {                              /* FSE compressed */
        static int16_t norm[256];
        int maxSV = sym_max, tableLog = 0;
        if (zfse_read_ncount(norm, &maxSV, &tableLog, in, size, used,
                             log_max) != 0)
            return zstd_fail("bad FSE table description");
        if (zfse_build(dt, norm, maxSV, tableLog) != 0)
            return zstd_fail("bad FSE table");
        *log = tableLog;
        *have = 1;
        return 0;
    }
    /* repeat */
    if (!*have) return zstd_fail("repeat mode with no previous table");
    return 0;
}

/*
 * Execute one block's sequences into `out`, which is also the window the
 * matches are copied from.
 */
static int zstd_decode_sequences(const uint8_t *in, uint32_t size,
                                 uint8_t *out, uint64_t *outpos,
                                 uint64_t outmax, uint64_t block_start) {
    uint32_t pos = 0;
    if (size < 1) return zstd_fail("truncated sequences section");

    uint32_t nbSeq = in[pos++];
    if (nbSeq == 0) {
        /* no sequences: the literals are the whole block */
        if (*outpos + zstd_lit_size > outmax)
            return zstd_fail("output buffer overflow");
        for (uint32_t i = 0; i < zstd_lit_size; i++)
            out[(*outpos)++] = zstd_literals[i];
        return 0;
    }
    if (nbSeq >= 128) {
        if (nbSeq < 255) {
            if (pos >= size) return zstd_fail("truncated sequence count");
            nbSeq = ((nbSeq - 128) << 8) + in[pos++];
        } else {
            if (pos + 2 > size) return zstd_fail("truncated sequence count");
            nbSeq = zs_rd16(in + pos) + 0x7F00;
            pos += 2;
        }
    }

    if (pos >= size) return zstd_fail("truncated compression modes");
    uint8_t modes = in[pos++];
    int ll_mode = (modes >> 6) & 3;
    int of_mode = (modes >> 4) & 3;
    int ml_mode = (modes >> 2) & 3;

    uint32_t used = 0;
    if (zstd_load_fse(ll_mode, zstd_ll_dt, &zstd_ll_log, &zstd_have_ll,
                      zstd_ll_default, 35, 6, ZSTD_LL_LOG_MAX, ZSTD_LL_MAX,
                      in + pos, size - pos, &used) != 0) return -1;
    pos += used;
    if (zstd_load_fse(of_mode, zstd_of_dt, &zstd_of_log, &zstd_have_of,
                      zstd_of_default, 28, 5, ZSTD_OF_LOG_MAX, ZSTD_OF_MAX,
                      in + pos, size - pos, &used) != 0) return -1;
    pos += used;
    if (zstd_load_fse(ml_mode, zstd_ml_dt, &zstd_ml_log, &zstd_have_ml,
                      zstd_ml_default, 52, 6, ZSTD_ML_LOG_MAX, ZSTD_ML_MAX,
                      in + pos, size - pos, &used) != 0) return -1;
    pos += used;

    if (pos > size) return zstd_fail("truncated sequences section");

    zbr_t b;
    if (zbr_init(&b, in + pos, size - pos) != 0)
        return zstd_fail("bad sequence bitstream");

    /* initial states are read literals-length, offset, match-length */
    zfse_state_t sll, sof, sml;
    zfse_init(&sll, zstd_ll_dt, &b, zstd_ll_log);
    zfse_init(&sof, zstd_of_dt, &b, zstd_of_log);
    zfse_init(&sml, zstd_ml_dt, &b, zstd_ml_log);

    uint32_t lit_pos = 0;

    for (uint32_t n = 0; n < nbSeq; n++) {
        uint32_t ll_code = zfse_symbol(&sll);
        uint32_t ml_code = zfse_symbol(&sml);
        uint32_t of_code = zfse_symbol(&sof);

        if (ll_code > ZSTD_LL_MAX || ml_code > ZSTD_ML_MAX ||
            of_code > ZSTD_OF_MAX)
            return zstd_fail("sequence symbol out of range");

        /* Extra bits are read offset, match length, literals length.  One
         * sequence can want 31 + 16 + 16 bits plus three state updates,
         * which is more than the container holds, so it is refilled
         * between every read. */
        uint32_t offset_value = (1u << of_code) + zbr_read(&b, (int)of_code);
        zbr_reload(&b);
        uint32_t ml = zstd_ml_base[ml_code] +
                      zbr_read(&b, zstd_ml_bits[ml_code]);
        zbr_reload(&b);
        uint32_t ll = zstd_ll_base[ll_code] +
                      zbr_read(&b, zstd_ll_bits[ll_code]);
        zbr_reload(&b);

        /* resolve the offset, including the repeat codes */
        uint32_t offset;
        if (offset_value > 3) {
            offset = offset_value - 3;
            zstd_rep[2] = zstd_rep[1];
            zstd_rep[1] = zstd_rep[0];
            zstd_rep[0] = offset;
        } else {
            /* with no literals, the repeat slots shift by one */
            uint32_t idx = offset_value + (ll == 0 ? 1 : 0);
            if (idx == 4) {
                offset = zstd_rep[0] - 1;
                if (offset == 0) return zstd_fail("invalid repeat offset");
                zstd_rep[2] = zstd_rep[1];
                zstd_rep[1] = zstd_rep[0];
                zstd_rep[0] = offset;
            } else {
                offset = zstd_rep[idx - 1];
                if (idx == 2) {
                    zstd_rep[1] = zstd_rep[0];
                    zstd_rep[0] = offset;
                } else if (idx == 3) {
                    zstd_rep[2] = zstd_rep[1];
                    zstd_rep[1] = zstd_rep[0];
                    zstd_rep[0] = offset;
                }
            }
        }
        if (offset == 0) return zstd_fail("zero offset");

        /* copy literals */
        if (lit_pos + ll > zstd_lit_size)
            return zstd_fail("sequence wants more literals than exist");
        if (*outpos + ll + ml > outmax)
            return zstd_fail("output buffer overflow");
        for (uint32_t i = 0; i < ll; i++)
            out[(*outpos)++] = zstd_literals[lit_pos++];

        /* copy the match from earlier output */
        if ((uint64_t)offset > *outpos)
            return zstd_fail("match offset before the start of output");
        uint64_t src = *outpos - offset;
        for (uint32_t i = 0; i < ml; i++)
            out[*outpos + i] = out[src + i];       /* may overlap */
        *outpos += ml;

        if (n + 1 < nbSeq) {
            zfse_update(&sll, &b);
            zfse_update(&sml, &b);
            zbr_reload(&b);
            zfse_update(&sof, &b);
            zbr_reload(&b);
        }
    }

    /* whatever literals remain are appended verbatim */
    uint32_t tail = zstd_lit_size - lit_pos;
    if (*outpos + tail > outmax) return zstd_fail("output buffer overflow");
    for (uint32_t i = 0; i < tail; i++)
        out[(*outpos)++] = zstd_literals[lit_pos++];

    (void)block_start;
    return 0;
}

/* ---- frame ---- */

/*
 * Decode one zstd frame.  Returns 0 on success with *out_len set, or -1
 * with *err pointing at a reason.
 */
static int zstd_decode(const uint8_t *in, uint64_t in_size,
                       uint8_t *out, uint64_t out_size,
                       uint64_t *out_len, const char **err) {
    zstd_err = 0;
    *out_len = 0;

    if (in_size < 4) { *err = "zstd stream is too short"; return -1; }
    uint32_t magic = zs_rd32(in);

    if ((magic & 0xFFFFFFF0u) == ZSTD_MAGIC_SKIP) {
        *err = "skippable frame, no content";
        return -1;
    }
    if (magic != ZSTD_MAGIC) { *err = "not a zstd frame"; return -1; }

    uint64_t pos = 4;
    if (pos >= in_size) { *err = "truncated frame header"; return -1; }

    uint8_t fhd = in[pos++];
    int fcs_flag = (fhd >> 6) & 3;
    int single_segment = (fhd >> 5) & 1;
    int checksum = (fhd >> 2) & 1;
    int dict_flag = fhd & 3;
    if ((fhd >> 3) & 1) { *err = "reserved frame header bit is set"; return -1; }

    if (!single_segment) {
        if (pos >= in_size) { *err = "truncated window descriptor"; return -1; }
        pos++;                                    /* window size: the output
                                                   * buffer is the window */
    }
    static const int dict_bytes[4] = { 0, 1, 2, 4 };
    pos += (uint64_t)dict_bytes[dict_flag];
    if (dict_flag) { *err = "dictionaries are not supported"; return -1; }

    int fcs_bytes = fcs_flag == 0 ? (single_segment ? 1 : 0)
                  : fcs_flag == 1 ? 2 : fcs_flag == 2 ? 4 : 8;
    uint64_t declared = 0;
    if (fcs_bytes) {
        if (pos + (uint64_t)fcs_bytes > in_size) {
            *err = "truncated frame content size";
            return -1;
        }
        if (fcs_bytes == 1) declared = in[pos];
        else if (fcs_bytes == 2) declared = zs_rd16(in + pos) + 256;
        else if (fcs_bytes == 4) declared = zs_rd32(in + pos);
        else declared = zs_rd64(in + pos);
        pos += (uint64_t)fcs_bytes;
    }
    if (declared && declared > out_size) {
        *err = "frame is larger than the output buffer";
        return -1;
    }

    /* every frame starts with a fresh entropy and offset history */
    zstd_rep[0] = 1; zstd_rep[1] = 4; zstd_rep[2] = 8;
    zstd_have_ll = zstd_have_ml = zstd_have_of = 0;
    zhuf_log = 0;

    uint64_t outpos = 0;
    for (;;) {
        if (pos + 3 > in_size) { *err = "truncated block header"; return -1; }
        uint32_t bh = zs_rd24(in + pos);
        pos += 3;
        int last = bh & 1;
        int btype = (int)((bh >> 1) & 3);
        uint32_t bsize = bh >> 3;

        if (btype == 3) { *err = "reserved block type"; return -1; }
        /* An RLE block's size field is the *decoded* length; its content
         * is a single byte, so it must not be bounds-checked against the
         * input the way raw and compressed blocks are. */
        uint32_t in_need = (btype == 1) ? 1u : bsize;
        if (pos + in_need > in_size) { *err = "truncated block"; return -1; }

        uint64_t block_start = outpos;

        if (btype == 0) {                          /* raw */
            if (outpos + bsize > out_size) { *err = "output buffer overflow"; return -1; }
            for (uint32_t i = 0; i < bsize; i++) out[outpos++] = in[pos + i];
        } else if (btype == 1) {                   /* RLE */
            if (bsize == 0) { *err = "bad RLE block"; return -1; }
            if (outpos + bsize > out_size) { *err = "output buffer overflow"; return -1; }
            for (uint32_t i = 0; i < bsize; i++) out[outpos++] = in[pos];
            pos += 1;
            if (last) break;
            continue;
        } else {                                   /* compressed */
            uint32_t lit_used = 0;
            if (zstd_decode_literals(in + pos, bsize, &lit_used) != 0) {
                *err = zstd_err ? zstd_err : "literals failed";
                return -1;
            }
            if (lit_used > bsize) { *err = "literals overran the block"; return -1; }
            if (zstd_decode_sequences(in + pos + lit_used, bsize - lit_used,
                                      out, &outpos, out_size,
                                      block_start) != 0) {
                *err = zstd_err ? zstd_err : "sequences failed";
                return -1;
            }
        }

        pos += bsize;
        if (last) break;
    }

    if (checksum) pos += 4;                        /* XXH64 low bits: skipped */
    if (declared && outpos != declared) {
        *err = "decoded size does not match the frame header";
        return -1;
    }

    *out_len = outpos;
    return 0;
}

#endif /* ZSTD_H */
