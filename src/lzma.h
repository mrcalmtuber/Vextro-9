#ifndef LZMA_H
#define LZMA_H

#include <stdint.h>

/*
 * LZMA / LZMA2 / xz decompressor.
 *
 * One codec serves two consumers in this kernel: the .sci image format
 * (a raw LZMA stream) and ZIM clusters (an xz stream wrapping LZMA2).
 *
 * The decoder writes straight into the caller's output buffer and uses
 * that buffer as its dictionary window, so there is no separate 32 MB
 * ring: a match at distance d is simply out[out_pos - d - 1].  The
 * consequence is that the caller must supply a buffer big enough for the
 * whole uncompressed stream, which is true for both consumers here.
 *
 * Freestanding: no allocation, no libc.  The decoder state is ~30 KB of
 * probability model, so it lives in one static instance rather than on
 * the kernel stack.
 *
 * Structure follows the reference LzmaDec: the same probability layout
 * and the same state machine, written out longhand instead of behind
 * macros.
 */

/* ---- model geometry (LZMA spec) ---- */

#define LZ_NUM_STATES        12
#define LZ_NUM_POS_BITS_MAX  4
#define LZ_NUM_LEN_TO_POS    4
#define LZ_NUM_FULL_DIST     128
#define LZ_END_POS_MODEL_IDX 14
#define LZ_NUM_ALIGN_BITS    4
#define LZ_MATCH_MIN_LEN     2

/* length coder sub-layout */
#define LZ_LEN_CHOICE   0
#define LZ_LEN_CHOICE2  1
#define LZ_LEN_LOW      2
#define LZ_LEN_MID      (LZ_LEN_LOW + (16 * 8))
#define LZ_LEN_HIGH     (LZ_LEN_MID + (16 * 8))
#define LZ_LEN_TOTAL    (LZ_LEN_HIGH + 256)

/* probability array layout */
#define LZ_IS_MATCH     0
#define LZ_IS_REP       (LZ_IS_MATCH   + (LZ_NUM_STATES << LZ_NUM_POS_BITS_MAX))
#define LZ_IS_REP_G0    (LZ_IS_REP     + LZ_NUM_STATES)
#define LZ_IS_REP_G1    (LZ_IS_REP_G0  + LZ_NUM_STATES)
#define LZ_IS_REP_G2    (LZ_IS_REP_G1  + LZ_NUM_STATES)
#define LZ_IS_REP0_LONG (LZ_IS_REP_G2  + LZ_NUM_STATES)
#define LZ_POS_SLOT     (LZ_IS_REP0_LONG + (LZ_NUM_STATES << LZ_NUM_POS_BITS_MAX))
#define LZ_SPEC_POS     (LZ_POS_SLOT   + (LZ_NUM_LEN_TO_POS << 6))
#define LZ_ALIGN        (LZ_SPEC_POS   + LZ_NUM_FULL_DIST - LZ_END_POS_MODEL_IDX)
#define LZ_LEN_CODER    (LZ_ALIGN      + (1 << LZ_NUM_ALIGN_BITS))
#define LZ_REP_LEN_CODER (LZ_LEN_CODER + LZ_LEN_TOTAL)
#define LZ_LITERAL      (LZ_REP_LEN_CODER + LZ_LEN_TOTAL)

/* lc + lp is capped at 4, which covers every encoder anyone ships */
#define LZ_LCLP_MAX     4
#define LZ_PROBS_MAX    (LZ_LITERAL + (0x300u << LZ_LCLP_MAX))

#define LZ_PROB_INIT    1024        /* (1 << 11) / 2 */
#define LZ_NUM_MOVE_BITS 5
#define LZ_TOP          (1u << 24)

typedef struct {
    const uint8_t *in;
    uint64_t       in_pos, in_size;
    uint8_t       *out;
    uint64_t       out_pos, out_size;

    uint32_t range, code;
    uint32_t rep0, rep1, rep2, rep3;
    uint32_t state;
    uint32_t lc, lp, pb;
    uint64_t processed;          /* bytes since the last dictionary reset */
    uint64_t dict_start;         /* out_pos at the last dictionary reset  */

    const char *err;
    uint16_t probs[LZ_PROBS_MAX];
} lzma_t;

/* The model is far too big for the kernel stack. */
static lzma_t lzma_state;

static int lz_fail(lzma_t *s, const char *msg) {
    if (!s->err) s->err = msg;
    return -1;
}

/* ---- range decoder ---- */

static uint8_t lz_next_byte(lzma_t *s) {
    if (s->in_pos >= s->in_size) {
        lz_fail(s, "compressed stream ended early");
        return 0;
    }
    return s->in[s->in_pos++];
}

static int lz_rc_init(lzma_t *s) {
    if (s->in_size - s->in_pos < 5)
        return lz_fail(s, "truncated range coder header");
    if (s->in[s->in_pos] != 0)
        return lz_fail(s, "range coder header byte is not zero");
    s->in_pos++;
    s->code = 0;
    s->range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        s->code = (s->code << 8) | lz_next_byte(s);
    return s->err ? -1 : 0;
}

static void lz_normalize(lzma_t *s) {
    if (s->range < LZ_TOP) {
        s->range <<= 8;
        s->code = (s->code << 8) | lz_next_byte(s);
    }
}

static uint32_t lz_bit(lzma_t *s, uint16_t *prob) {
    uint32_t bound = (s->range >> 11) * (uint32_t)(*prob);
    uint32_t bit;
    if (s->code < bound) {
        s->range = bound;
        *prob = (uint16_t)(*prob + ((2048 - *prob) >> LZ_NUM_MOVE_BITS));
        bit = 0;
    } else {
        s->range -= bound;
        s->code -= bound;
        *prob = (uint16_t)(*prob - (*prob >> LZ_NUM_MOVE_BITS));
        bit = 1;
    }
    lz_normalize(s);
    return bit;
}

static uint32_t lz_direct(lzma_t *s, uint32_t nbits) {
    uint32_t result = 0;
    while (nbits--) {
        s->range >>= 1;
        s->code -= s->range;
        uint32_t t = 0 - (s->code >> 31);   /* 0xFFFFFFFF if code went negative */
        s->code += s->range & t;
        lz_normalize(s);
        result = (result << 1) + (t + 1);
    }
    return result;
}

static uint32_t lz_bittree(lzma_t *s, uint16_t *probs, uint32_t nbits) {
    uint32_t m = 1;
    for (uint32_t i = 0; i < nbits; i++)
        m = (m << 1) + lz_bit(s, &probs[m]);
    return m - ((uint32_t)1 << nbits);
}

static uint32_t lz_bittree_rev(lzma_t *s, uint16_t *probs, uint32_t nbits) {
    uint32_t m = 1, sym = 0;
    for (uint32_t i = 0; i < nbits; i++) {
        uint32_t b = lz_bit(s, &probs[m]);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

static uint32_t lz_len_decode(lzma_t *s, uint16_t *p, uint32_t pos_state) {
    if (lz_bit(s, &p[LZ_LEN_CHOICE]) == 0)
        return lz_bittree(s, &p[LZ_LEN_LOW + (pos_state << 3)], 3);
    if (lz_bit(s, &p[LZ_LEN_CHOICE2]) == 0)
        return 8 + lz_bittree(s, &p[LZ_LEN_MID + (pos_state << 3)], 3);
    return 16 + lz_bittree(s, &p[LZ_LEN_HIGH], 8);
}

/* ---- state reset ---- */

static void lz_reset_probs(lzma_t *s) {
    uint32_t n = LZ_LITERAL + (0x300u << (s->lc + s->lp));
    if (n > LZ_PROBS_MAX) n = LZ_PROBS_MAX;
    for (uint32_t i = 0; i < n; i++)
        s->probs[i] = LZ_PROB_INIT;
    s->state = 0;
    s->rep0 = s->rep1 = s->rep2 = s->rep3 = 0;
}

static int lz_set_props(lzma_t *s, uint8_t d) {
    if (d >= 9 * 5 * 5) return lz_fail(s, "invalid LZMA properties byte");
    s->lc = d % 9;  d /= 9;
    s->lp = d % 5;
    s->pb = d / 5;
    if (s->lc + s->lp > LZ_LCLP_MAX)
        return lz_fail(s, "lc+lp is larger than this decoder supports");
    if (s->pb > LZ_NUM_POS_BITS_MAX)
        return lz_fail(s, "invalid pb");
    return 0;
}

/* ---- the main loop ---- */

/* Decode until out_pos reaches limit.  Returns 0 on success. */
static int lz_decode_to(lzma_t *s, uint64_t limit) {
    uint32_t pb_mask = ((uint32_t)1 << s->pb) - 1;
    uint32_t lp_mask = ((uint32_t)1 << s->lp) - 1;

    while (s->out_pos < limit) {
        if (s->err) return -1;

        uint32_t pos_state = (uint32_t)s->processed & pb_mask;
        uint32_t state = s->state;

        if (lz_bit(s, &s->probs[LZ_IS_MATCH + (state << LZ_NUM_POS_BITS_MAX)
                                + pos_state]) == 0) {
            /* ---- literal ---- */
            uint8_t prev = s->out_pos > s->dict_start
                         ? s->out[s->out_pos - 1] : 0;
            uint32_t lit_state = (((uint32_t)s->processed & lp_mask) << s->lc)
                               + ((uint32_t)prev >> (8 - s->lc));
            uint16_t *probs = &s->probs[LZ_LITERAL + 0x300u * lit_state];
            uint32_t symbol = 1;

            if (state < 7) {
                do {
                    symbol = (symbol << 1) + lz_bit(s, &probs[symbol]);
                } while (symbol < 0x100);
            } else {
                /* matched literal: predict against the byte at rep0 */
                if (s->out_pos < s->dict_start + (uint64_t)s->rep0 + 1)
                    return lz_fail(s, "match distance before the dictionary");
                uint32_t match_byte = s->out[s->out_pos - s->rep0 - 1];
                do {
                    uint32_t match_bit = (match_byte >> 7) & 1;
                    match_byte <<= 1;
                    uint32_t bit = lz_bit(s, &probs[((1 + match_bit) << 8)
                                                    + symbol]);
                    symbol = (symbol << 1) + bit;
                    if (match_bit != bit) {
                        while (symbol < 0x100)
                            symbol = (symbol << 1) + lz_bit(s, &probs[symbol]);
                        break;
                    }
                } while (symbol < 0x100);
            }

            if (s->out_pos >= s->out_size)
                return lz_fail(s, "output buffer overflow");
            s->out[s->out_pos++] = (uint8_t)symbol;
            s->processed++;

            s->state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            continue;
        }

        /* ---- match ---- */
        uint32_t len;

        if (lz_bit(s, &s->probs[LZ_IS_REP + state])) {
            /* repeated distance */
            if (s->out_pos == s->dict_start)
                return lz_fail(s, "rep match with an empty dictionary");

            if (lz_bit(s, &s->probs[LZ_IS_REP_G0 + state]) == 0) {
                if (lz_bit(s, &s->probs[LZ_IS_REP0_LONG
                                        + (state << LZ_NUM_POS_BITS_MAX)
                                        + pos_state]) == 0) {
                    /* one byte at rep0 */
                    if (s->out_pos < s->dict_start + (uint64_t)s->rep0 + 1)
                        return lz_fail(s, "match distance before the dictionary");
                    if (s->out_pos >= s->out_size)
                        return lz_fail(s, "output buffer overflow");
                    s->out[s->out_pos] = s->out[s->out_pos - s->rep0 - 1];
                    s->out_pos++;
                    s->processed++;
                    s->state = state < 7 ? 9 : 11;
                    continue;
                }
            } else {
                uint32_t dist;
                if (lz_bit(s, &s->probs[LZ_IS_REP_G1 + state]) == 0) {
                    dist = s->rep1;
                } else {
                    if (lz_bit(s, &s->probs[LZ_IS_REP_G2 + state]) == 0) {
                        dist = s->rep2;
                    } else {
                        dist = s->rep3;
                        s->rep3 = s->rep2;
                    }
                    s->rep2 = s->rep1;
                }
                s->rep1 = s->rep0;
                s->rep0 = dist;
            }
            len = lz_len_decode(s, &s->probs[LZ_REP_LEN_CODER], pos_state);
            s->state = state < 7 ? 8 : 11;
        } else {
            /* new distance */
            s->rep3 = s->rep2;
            s->rep2 = s->rep1;
            s->rep1 = s->rep0;

            len = lz_len_decode(s, &s->probs[LZ_LEN_CODER], pos_state);
            s->state = state < 7 ? 7 : 10;

            uint32_t len_to_pos = len < LZ_NUM_LEN_TO_POS - 1
                                ? len : LZ_NUM_LEN_TO_POS - 1;
            uint32_t pos_slot = lz_bittree(s, &s->probs[LZ_POS_SLOT
                                                        + (len_to_pos << 6)], 6);
            if (pos_slot < 4) {
                s->rep0 = pos_slot;
            } else {
                uint32_t direct_bits = (pos_slot >> 1) - 1;
                uint32_t dist = (2 | (pos_slot & 1)) << direct_bits;

                if (pos_slot < LZ_END_POS_MODEL_IDX) {
                    /* base is offset so the tree indices land in SpecPos */
                    uint16_t *base = &s->probs[LZ_SPEC_POS + dist - pos_slot - 1];
                    uint32_t mask = 1, i = 1;
                    do {
                        if (lz_bit(s, &base[i])) {
                            i = (i << 1) + 1;
                            dist |= mask;
                        } else {
                            i = i << 1;
                        }
                        mask <<= 1;
                    } while (--direct_bits);
                } else {
                    dist += lz_direct(s, direct_bits - LZ_NUM_ALIGN_BITS)
                            << LZ_NUM_ALIGN_BITS;
                    dist += lz_bittree_rev(s, &s->probs[LZ_ALIGN],
                                           LZ_NUM_ALIGN_BITS);
                    if (dist == 0xFFFFFFFFu) {
                        /* end-of-stream marker */
                        return 0;
                    }
                }
                s->rep0 = dist;
            }
        }

        len += LZ_MATCH_MIN_LEN;

        if (s->out_pos < s->dict_start + (uint64_t)s->rep0 + 1)
            return lz_fail(s, "match distance before the dictionary");
        if (s->out_pos + len > s->out_size)
            return lz_fail(s, "output buffer overflow");
        if (s->out_pos + len > limit)
            len = (uint32_t)(limit - s->out_pos);

        uint64_t src = s->out_pos - s->rep0 - 1;
        for (uint32_t i = 0; i < len; i++)
            s->out[s->out_pos + i] = s->out[src + i];   /* may overlap: byte-wise */
        s->out_pos += len;
        s->processed += len;
    }
    return s->err ? -1 : 0;
}

/* ===== raw LZMA ("alone" / .lzma) =====
 * 1 props byte, 4 byte dictionary size, 8 byte uncompressed size, stream. */

static int lzma_alone_decode(const uint8_t *in, uint64_t in_size,
                             uint8_t *out, uint64_t out_size,
                             uint64_t *out_len, const char **err) {
    lzma_t *s = &lzma_state;
    s->err = 0;

    if (in_size < 13) { *err = "LZMA stream is too short"; return -1; }

    if (lz_set_props(s, in[0]) != 0) { *err = s->err; return -1; }

    uint64_t declared = 0;
    for (int i = 0; i < 8; i++)
        declared |= (uint64_t)in[5 + i] << (8 * i);

    uint64_t want = out_size;
    if (declared != 0xFFFFFFFFFFFFFFFFull) {
        if (declared > out_size) { *err = "image is larger than the buffer"; return -1; }
        want = declared;
    }

    s->in = in;
    s->in_pos = 13;
    s->in_size = in_size;
    s->out = out;
    s->out_pos = 0;
    s->out_size = out_size;
    s->processed = 0;
    s->dict_start = 0;

    lz_reset_probs(s);
    if (lz_rc_init(s) != 0) { *err = s->err; return -1; }
    if (lz_decode_to(s, want) != 0) { *err = s->err; return -1; }

    *out_len = s->out_pos;
    return 0;
}

/* ===== LZMA2 ===== */

/* in_used, when non-NULL, receives the number of input bytes consumed —
 * the xz layer needs it to find the next block. */
static int lzma2_decode(const uint8_t *in, uint64_t in_size,
                        uint8_t *out, uint64_t out_size,
                        uint64_t *out_len, uint64_t *in_used,
                        const char **err) {
    lzma_t *s = &lzma_state;
    s->err = 0;
    s->in = in;
    s->in_size = in_size;
    s->out = out;
    s->out_size = out_size;
    s->out_pos = 0;
    s->processed = 0;
    s->dict_start = 0;
    s->lc = 3; s->lp = 0; s->pb = 2;
    lz_reset_probs(s);

    uint64_t pos = 0;
    int have_props = 0;

    for (;;) {
        if (pos >= in_size) { *err = "LZMA2 stream ended without a terminator"; return -1; }
        uint8_t control = in[pos++];

        if (control == 0) break;                 /* end of stream */

        if (control < 3) {
            /* uncompressed chunk: 1 = dict reset, 2 = no reset */
            if (pos + 2 > in_size) { *err = "truncated LZMA2 chunk header"; return -1; }
            uint32_t size = ((uint32_t)in[pos] << 8 | in[pos + 1]) + 1;
            pos += 2;
            if (pos + size > in_size) { *err = "truncated LZMA2 chunk"; return -1; }
            if (s->out_pos + size > out_size) { *err = "output buffer overflow"; return -1; }
            for (uint32_t i = 0; i < size; i++)
                out[s->out_pos + i] = in[pos + i];
            pos += size;
            s->out_pos += size;
            if (control == 1) {
                s->dict_start = s->out_pos - size;
                s->processed = 0;
            }
            s->processed += size;
            /* an uncompressed chunk always resets the LZMA state */
            lz_reset_probs(s);
            continue;
        }

        if (control < 0x80) { *err = "invalid LZMA2 control byte"; return -1; }

        if (pos + 4 > in_size) { *err = "truncated LZMA2 chunk header"; return -1; }
        uint32_t unpack = ((uint32_t)(control & 0x1F) << 16)
                        | ((uint32_t)in[pos] << 8) | in[pos + 1];
        unpack += 1;
        uint32_t pack = ((uint32_t)in[pos + 2] << 8) | in[pos + 3];
        pack += 1;
        pos += 4;

        uint32_t reset = ((uint32_t)control >> 5) & 0x3;
        if (reset >= 2) {
            if (pos >= in_size) { *err = "truncated LZMA2 properties"; return -1; }
            if (lz_set_props(s, in[pos++]) != 0) { *err = s->err; return -1; }
            have_props = 1;
        }
        if (!have_props) { *err = "LZMA2 chunk before any properties"; return -1; }
        if (reset == 3) {
            s->dict_start = s->out_pos;
            s->processed = 0;
        }
        if (reset >= 1) lz_reset_probs(s);

        if (pos + pack > in_size) { *err = "truncated LZMA2 chunk"; return -1; }
        if (s->out_pos + unpack > out_size) { *err = "output buffer overflow"; return -1; }

        s->in = in;
        s->in_pos = pos;
        s->in_size = pos + pack;
        if (lz_rc_init(s) != 0) { *err = s->err; return -1; }
        if (lz_decode_to(s, s->out_pos + unpack) != 0) { *err = s->err; return -1; }

        pos += pack;
    }

    *out_len = s->out_pos;
    if (in_used) *in_used = pos;
    return 0;
}

/* ===== xz container =====
 * Only what a ZIM cluster needs: a stream of blocks whose single filter
 * is LZMA2.  Integrity checks are parsed for their length and skipped —
 * the ZIM index already tells us how big every blob should be. */

static uint64_t xz_varint(const uint8_t *in, uint64_t size, uint64_t *pos,
                          int *ok) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 9; i++) {
        if (*pos >= size) { *ok = 0; return 0; }
        uint8_t b = in[(*pos)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *ok = 1; return v; }
        shift += 7;
    }
    *ok = 0;
    return 0;
}

static int xz_decode(const uint8_t *in, uint64_t in_size,
                     uint8_t *out, uint64_t out_size,
                     uint64_t *out_len, const char **err) {
    if (in_size < 12) { *err = "xz stream is too short"; return -1; }
    if (in[0] != 0xFD || in[1] != '7' || in[2] != 'z' || in[3] != 'X' ||
        in[4] != 'Z' || in[5] != 0) {
        *err = "not an xz stream";
        return -1;
    }

    /* stream flags: low nibble of byte 7 selects the check size */
    uint8_t check_id = in[7] & 0x0F;
    uint32_t check_size = 0;
    if (check_id != 0) {
        static const uint8_t sizes[16] = {
            0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64
        };
        check_size = sizes[check_id];
    }

    uint64_t pos = 12;              /* header (6) + flags (2) + CRC32 (4) */
    uint64_t total = 0;

    for (;;) {
        if (pos >= in_size) { *err = "xz stream ended without an index"; return -1; }
        uint8_t first = in[pos];
        if (first == 0) break;      /* index indicator: blocks are done */

        uint64_t header_size = ((uint64_t)first + 1) * 4;
        if (pos + header_size > in_size) { *err = "truncated xz block header"; return -1; }
        uint64_t hstart = pos;
        uint64_t p = pos + 1;

        uint8_t flags = in[p++];
        uint32_t nfilters = (flags & 0x03) + 1;
        if (nfilters != 1) { *err = "xz block uses a filter chain"; return -1; }

        int ok = 1;
        if (flags & 0x40) { xz_varint(in, in_size, &p, &ok); if (!ok) goto bad; }
        if (flags & 0x80) { xz_varint(in, in_size, &p, &ok); if (!ok) goto bad; }

        uint64_t filter_id = xz_varint(in, in_size, &p, &ok);
        if (!ok) goto bad;
        uint64_t props_size = xz_varint(in, in_size, &p, &ok);
        if (!ok) goto bad;
        if (filter_id != 0x21) { *err = "xz block filter is not LZMA2"; return -1; }
        if (props_size != 1 || p >= in_size) { *err = "bad LZMA2 filter properties"; return -1; }
        p++;                        /* dictionary size byte: our window is the output buffer */

        pos = hstart + header_size; /* skip padding + header CRC32 */

        /* The block's compressed size is optional in the header, so the
         * next block is located from what LZMA2 actually consumed. */
        uint64_t produced = 0, consumed = 0;
        if (lzma2_decode(in + pos, in_size - pos, out + total,
                         out_size - total, &produced, &consumed, err) != 0)
            return -1;
        total += produced;

        pos += consumed;
        pos = (pos + 3) & ~(uint64_t)3;          /* block padding */
        pos += check_size;
        if (pos > in_size) { *err = "xz block runs past the end of the stream"; return -1; }
    }

    *out_len = total;
    return 0;

bad:
    *err = "malformed xz block header";
    return -1;
}

#endif /* LZMA_H */
