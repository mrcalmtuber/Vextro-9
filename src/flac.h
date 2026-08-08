#ifndef VEXTRO_FLAC_H
#define VEXTRO_FLAC_H

/*
 * src/flac.h — a FLAC decoder.
 *
 * FLAC is the right compressed format for this machine and not a
 * compromise. It is lossless, so a decode can be checked against the
 * original samples exactly rather than "sounds about right"; and it is
 * defined entirely over integers -- Rice coding, fixed and LPC
 * predictors, an arithmetic shift -- so it runs under -mno-sse
 * -mno-80387 with nothing borrowed from the one translation unit that
 * is allowed floating point.
 *
 * What is implemented: the whole subset a real encoder emits.
 *
 *   STREAMINFO, and every other metadata block skipped by length
 *   CONSTANT, VERBATIM, FIXED (orders 0-4) and LPC (orders 1-32)
 *   Rice partitions, both 4-bit and 5-bit parameters, plus the escape
 *     that stores a partition as raw fixed-width samples
 *   independent, left/side, right/side and mid/side channels
 *   8, 16 and 24 bits per sample
 *   CRC-8 over each frame header and CRC-16 over each whole frame
 *
 * The CRCs are checked, not skipped. This decoder is pointed at files
 * off a volume that anything could have written, and a bit reader that
 * has lost its place will happily produce megabytes of plausible
 * nonsense; the frame CRC is what turns that into a clean error. Every
 * read is bounds-checked against the buffer as well -- the reader
 * refuses past the end rather than wrapping or trusting a length.
 *
 * Output matches the convention media.h already uses: interleaved
 * 16-bit stereo, mono widened to both channels.
 */

/* A block can legally hold 65535 samples per channel, which would be
 * half a megabyte of scratch for a case no encoder produces. 16384
 * covers every real file -- flac itself defaults to 4096 -- and a
 * stream that exceeds it is refused rather than truncated. */
#define FLAC_MAX_BLOCK   16384
#define FLAC_MAX_ORDER   32
#define FLAC_MAX_CH      2

static int32_t flac_ch[FLAC_MAX_CH][FLAC_MAX_BLOCK];

/* ===== bit reader ===== */

typedef struct {
    const uint8_t *d;
    uint64_t       nbits;      /* total, not bytes */
    uint64_t       pos;
    int            bad;
} flac_br_t;

static void flac_br_init(flac_br_t *b, const uint8_t *d, uint64_t nbytes) {
    b->d = d;
    b->nbits = nbytes * 8u;
    b->pos = 0;
    b->bad = 0;
}

/* Up to 32 bits, MSB first. Refuses past the end instead of wrapping. */
static uint32_t flac_bits(flac_br_t *b, int n) {
    uint32_t v = 0;
    if (n <= 0) return 0;
    if (b->bad || b->pos + (uint64_t)n > b->nbits) { b->bad = 1; return 0; }
    while (n > 0) {
        const uint64_t byte = b->pos >> 3;
        const int off = (int)(b->pos & 7u);
        const int avail = 8 - off;
        const int take = n < avail ? n : avail;
        uint32_t chunk = ((uint32_t)b->d[byte] << off) & 0xFFu;
        chunk >>= (8 - take);
        v = (v << take) | chunk;
        b->pos += (uint64_t)take;
        n -= take;
    }
    return v;
}

/* Two's-complement value of width n, sign extended. */
static int32_t flac_sbits(flac_br_t *b, int n) {
    if (n <= 0) return 0;
    uint32_t u = flac_bits(b, n);
    if (n < 32 && (u & (1u << (n - 1))))
        u |= ~((1u << n) - 1u);
    return (int32_t)u;
}

/* Rice quotient: the number of zeroes before the next 1. Bounded, so a
 * corrupt stream cannot spin here. */
static uint32_t flac_unary(flac_br_t *b) {
    uint32_t q = 0;
    while (!b->bad) {
        if (flac_bits(b, 1)) break;
        if (++q > (1u << 20)) { b->bad = 1; break; }
    }
    return q;
}

static void flac_align(flac_br_t *b) { b->pos = (b->pos + 7u) & ~(uint64_t)7u; }

/* ===== CRC ===== */

/* CRC-8 with polynomial x^8 + x^2 + x + 1, over the frame header. */
static uint8_t flac_crc8(const uint8_t *d, uint64_t n) {
    uint8_t c = 0;
    for (uint64_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (uint8_t)((c & 0x80u) ? (((uint32_t)c << 1) ^ 0x07u)
                                     : ((uint32_t)c << 1));
    }
    return c;
}

/* CRC-16 with polynomial x^16 + x^15 + x^2 + 1, over the whole frame. */
static uint16_t flac_crc16(const uint8_t *d, uint64_t n) {
    uint16_t c = 0;
    for (uint64_t i = 0; i < n; i++) {
        c ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000u) ? (((uint32_t)c << 1) ^ 0x8005u)
                                        : ((uint32_t)c << 1));
    }
    return c;
}

/* ===== stream state ===== */

typedef struct {
    uint32_t rate;
    uint32_t nch;
    uint32_t bps;
    uint32_t min_block, max_block;
    uint64_t total;
} flac_info_t;

/* Sample rates and block sizes the frame header can name by index. */
static const uint32_t flac_rate_tab[16] = {
    0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
    32000, 44100, 48000, 96000, 0, 0, 0, 0
};
static const uint16_t flac_block_tab[16] = {
    0, 192, 576, 1152, 2304, 4608, 0, 0,
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};
static const uint8_t flac_bps_tab[8] = { 0, 8, 12, 0, 16, 20, 24, 0 };

/* ===== residual ===== */

/*
 * A residual is 2^order partitions of Rice-coded values.
 *
 * The partitioning is defined over the *whole* block, not over the
 * residual: partition k holds block>>porder samples, except partition 0,
 * which is short by the predictor order because those samples went out
 * as warm-up instead of being coded. Sizing the partitions over the
 * residual length instead is a mistake that hides completely at
 * partition order 0 -- which is what a short or simple frame uses -- and
 * only appears once an encoder picks a real partition order.
 *
 * `out` receives block - pred_order values.
 */
static int flac_residual(flac_br_t *b, int32_t *out, uint32_t block,
                         uint32_t pred_order) {
    const uint32_t method = flac_bits(b, 2);
    if (method > 1) return 0;                    /* reserved */
    const int pbits = method == 0 ? 4 : 5;
    const uint32_t escape = method == 0 ? 15u : 31u;

    const uint32_t porder = flac_bits(b, 4);
    const uint32_t parts = 1u << porder;
    if (block % parts) return 0;                 /* not evenly divisible */
    const uint32_t psize = block >> porder;
    if (psize < pred_order) return 0;
    const uint32_t total = block - pred_order;

    uint32_t i = 0;
    for (uint32_t p = 0; p < parts; p++) {
        const uint32_t count = (p == 0) ? psize - pred_order : psize;
        const uint32_t param = flac_bits(b, pbits);

        if (param == escape) {
            /* An unpredictable partition is stored raw, at a width the
             * stream states. Width 0 means the whole partition is zero. */
            const int width = (int)flac_bits(b, 5);
            for (uint32_t k = 0; k < count; k++) {
                if (i >= total) return 0;
                out[i++] = width ? flac_sbits(b, width) : 0;
            }
        } else {
            for (uint32_t k = 0; k < count; k++) {
                if (i >= total) return 0;
                const uint32_t q = flac_unary(b);
                const uint32_t r = param ? flac_bits(b, (int)param) : 0;
                const uint32_t u = (q << param) | r;
                /* zig-zag: the sign is the low bit */
                out[i++] = (int32_t)((u >> 1) ^ (~(u & 1u) + 1u));
            }
        }
        if (b->bad) return 0;
    }
    return i == total;
}

/* ===== subframes ===== */

static const int32_t flac_fixed_coef[5][4] = {
    { 0, 0, 0, 0 },
    { 1, 0, 0, 0 },
    { 2, -1, 0, 0 },
    { 3, -3, 1, 0 },
    { 4, -6, 4, -1 },
};

static int flac_subframe(flac_br_t *b, int32_t *out, uint32_t block,
                         uint32_t bps) {
    if (flac_bits(b, 1)) return 0;               /* mandatory zero */
    const uint32_t type = flac_bits(b, 6);
    uint32_t wasted = 0;
    if (flac_bits(b, 1)) wasted = flac_unary(b) + 1u;
    if (b->bad || wasted >= bps) return 0;
    bps -= wasted;
    if (bps == 0 || bps > 32) return 0;

    if (type == 0) {                             /* CONSTANT */
        const int32_t v = flac_sbits(b, (int)bps);
        for (uint32_t i = 0; i < block; i++) out[i] = v;

    } else if (type == 1) {                      /* VERBATIM */
        for (uint32_t i = 0; i < block; i++) out[i] = flac_sbits(b, (int)bps);

    } else if (type >= 8 && type <= 12) {        /* FIXED, order 0..4 */
        const uint32_t order = type - 8u;
        if (order > block) return 0;
        for (uint32_t i = 0; i < order; i++) out[i] = flac_sbits(b, (int)bps);
        if (!flac_residual(b, out + order, block, order)) return 0;
        const int32_t *c = flac_fixed_coef[order];
        for (uint32_t i = order; i < block; i++) {
            int64_t sum = 0;
            for (uint32_t j = 0; j < order; j++)
                sum += (int64_t)c[j] * out[i - 1 - j];
            out[i] += (int32_t)sum;
        }

    } else if (type >= 32) {                     /* LPC, order 1..32 */
        const uint32_t order = (type & 31u) + 1u;
        if (order > FLAC_MAX_ORDER || order > block) return 0;
        for (uint32_t i = 0; i < order; i++) out[i] = flac_sbits(b, (int)bps);

        const int prec = (int)flac_bits(b, 4) + 1;
        if (prec == 16) return 0;                /* the escape, unused */
        const int32_t shift = flac_sbits(b, 5);
        if (shift < 0) return 0;                 /* a negative shift is illegal */

        int32_t coef[FLAC_MAX_ORDER];
        for (uint32_t i = 0; i < order; i++) coef[i] = flac_sbits(b, prec);
        if (b->bad) return 0;

        if (!flac_residual(b, out + order, block, order)) return 0;

        /*
         * 32-bit coefficients against 25-bit samples overflow 32 bits
         * long before order 32 does, so the accumulator is 64-bit. This
         * is the one place a narrow type would produce audible, subtle
         * corruption rather than an error.
         */
        for (uint32_t i = order; i < block; i++) {
            int64_t sum = 0;
            for (uint32_t j = 0; j < order; j++)
                sum += (int64_t)coef[j] * out[i - 1 - j];
            out[i] += (int32_t)(sum >> shift);
        }

    } else {
        return 0;                                /* reserved */
    }

    if (wasted)
        for (uint32_t i = 0; i < block; i++) out[i] <<= wasted;
    return !b->bad;
}

/* ===== frames ===== */

/*
 * Decode one frame into flac_ch[][] and report how many samples and
 * which channels. Returns 0 on success, or a message.
 */
static const char *flac_frame(flac_br_t *b, const flac_info_t *si,
                              uint32_t *out_block) {
    const uint64_t frame_start_byte = b->pos >> 3;

    if (flac_bits(b, 14) != 0x3FFEu) return "lost frame sync";
    if (flac_bits(b, 1)) return "reserved bit set in a frame header";
    flac_bits(b, 1);                             /* blocking strategy */

    const uint32_t bs_code = flac_bits(b, 4);
    const uint32_t sr_code = flac_bits(b, 4);
    const uint32_t ch_code = flac_bits(b, 4);
    const uint32_t bd_code = flac_bits(b, 3);
    if (flac_bits(b, 1)) return "reserved bit set in a frame header";

    /* The coded frame or sample number: a UTF-8-shaped variable length
     * integer. Only its length matters here. */
    const uint32_t first = flac_bits(b, 8);
    int extra = 0;
    if      ((first & 0x80u) == 0x00u) extra = 0;
    else if ((first & 0xE0u) == 0xC0u) extra = 1;
    else if ((first & 0xF0u) == 0xE0u) extra = 2;
    else if ((first & 0xF8u) == 0xF0u) extra = 3;
    else if ((first & 0xFCu) == 0xF8u) extra = 4;
    else if ((first & 0xFEu) == 0xFCu) extra = 5;
    else if (first == 0xFEu)           extra = 6;
    else return "bad frame number";
    for (int i = 0; i < extra; i++)
        if ((flac_bits(b, 8) & 0xC0u) != 0x80u) return "bad frame number";

    uint32_t block = flac_block_tab[bs_code];
    if (bs_code == 6)      block = flac_bits(b, 8) + 1u;
    else if (bs_code == 7) block = flac_bits(b, 16) + 1u;
    if (block == 0) return "reserved block size";
    if (block > FLAC_MAX_BLOCK) return "block larger than this decoder allows";

    if (sr_code == 12)      flac_bits(b, 8);
    else if (sr_code == 13) flac_bits(b, 16);
    else if (sr_code == 14) flac_bits(b, 16);
    else if (sr_code == 15) return "invalid sample rate code";

    uint32_t bps = flac_bps_tab[bd_code];
    if (bps == 0) bps = si->bps;
    if (bps == 0 || bps > 32) return "unsupported bit depth";

    /* The header CRC covers every byte from the sync code to here. */
    flac_align(b);
    const uint64_t hdr_end = b->pos >> 3;
    const uint8_t want8 = (uint8_t)flac_bits(b, 8);
    if (flac_crc8(b->d + frame_start_byte,
                  hdr_end - frame_start_byte) != want8)
        return "frame header failed its CRC";

    /*
     * Channel assignment. 0-7 are independent channels; 8, 9 and 10
     * store one channel plus a difference, and the difference needs one
     * extra bit of headroom because it spans twice the range.
     */
    uint32_t nch;
    if (ch_code < 8)       nch = ch_code + 1u;
    else if (ch_code < 11) nch = 2u;
    else return "reserved channel assignment";
    if (nch != si->nch) return "channel count changed mid-stream";
    if (nch > FLAC_MAX_CH) return "more than two channels";

    for (uint32_t c = 0; c < nch; c++) {
        uint32_t cbps = bps;
        if ((ch_code == 8 && c == 1) ||          /* left/side  */
            (ch_code == 9 && c == 0) ||          /* right/side */
            (ch_code == 10 && c == 1))           /* mid/side   */
            cbps++;
        if (!flac_subframe(b, flac_ch[c], block, cbps))
            return "corrupt subframe";
    }

    flac_align(b);
    const uint64_t body_end = b->pos >> 3;
    const uint16_t want16 = (uint16_t)flac_bits(b, 16);
    if (b->bad) return "frame ran past the end of the file";
    if (flac_crc16(b->d + frame_start_byte,
                   body_end - frame_start_byte) != want16)
        return "frame failed its CRC";

    /* Undo the stereo decorrelation, in place. */
    if (ch_code == 8) {                          /* left, side */
        for (uint32_t i = 0; i < block; i++)
            flac_ch[1][i] = flac_ch[0][i] - flac_ch[1][i];
    } else if (ch_code == 9) {                   /* side, right */
        for (uint32_t i = 0; i < block; i++)
            flac_ch[0][i] += flac_ch[1][i];
    } else if (ch_code == 10) {                  /* mid, side */
        for (uint32_t i = 0; i < block; i++) {
            int32_t side = flac_ch[1][i];
            /* the low bit of the sum lives in the side channel */
            int32_t mid = (flac_ch[0][i] << 1) | (side & 1);
            flac_ch[0][i] = (mid + side) >> 1;
            flac_ch[1][i] = (mid - side) >> 1;
        }
    }

    *out_block = block;
    return 0;
}

/* ===== the whole file ===== */

/*
 * Decode a FLAC file into interleaved 16-bit stereo.
 *
 * Returns 0 on success, or a message suitable for showing a person.
 * Stops cleanly when the output buffer fills: a long track plays as much
 * as there is room for rather than failing.
 */
static const char *flac_decode(const uint8_t *d, uint64_t n,
                               int16_t *out, uint32_t out_max,
                               uint32_t *out_samples, uint32_t *out_rate) {
    *out_samples = 0;
    if (n < 42) return "too small to be a FLAC file";
    if (!(d[0] == 'f' && d[1] == 'L' && d[2] == 'a' && d[3] == 'C'))
        return "not a FLAC file";

    /* --- metadata --- */
    /* Zeroed rather than left to the have_streaminfo check alone: the
     * compiler cannot see that the flag guards every field, and a
     * warning that has to be reasoned away is a warning that hides the
     * next one. */
    flac_info_t si = { 0, 0, 0, 0, 0, 0 };
    uint64_t off = 4;
    int have_streaminfo = 0;
    for (;;) {
        if (off + 4 > n) return "metadata runs past the end of the file";
        const uint8_t hdr = d[off];
        const uint32_t len = ((uint32_t)d[off + 1] << 16) |
                             ((uint32_t)d[off + 2] << 8) | d[off + 3];
        const uint64_t body = off + 4;
        if (len > n - body) return "a metadata block runs past the end";

        if ((hdr & 0x7Fu) == 0) {                /* STREAMINFO */
            if (len < 34) return "short STREAMINFO";
            const uint8_t *p = d + body;
            si.min_block = ((uint32_t)p[0] << 8) | p[1];
            si.max_block = ((uint32_t)p[2] << 8) | p[3];
            si.rate = ((uint32_t)p[10] << 12) | ((uint32_t)p[11] << 4) |
                      (uint32_t)(p[12] >> 4);
            si.nch = (uint32_t)((p[12] >> 1) & 7u) + 1u;
            si.bps = (uint32_t)(((p[12] & 1u) << 4) | (p[13] >> 4)) + 1u;
            si.total = ((uint64_t)(p[13] & 0x0Fu) << 32) |
                       ((uint64_t)p[14] << 24) | ((uint64_t)p[15] << 16) |
                       ((uint64_t)p[16] << 8) | (uint64_t)p[17];
            have_streaminfo = 1;
        }
        off = body + len;
        if (hdr & 0x80u) break;                  /* last metadata block */
    }
    if (!have_streaminfo) return "no STREAMINFO block";

    if (si.nch < 1 || si.nch > FLAC_MAX_CH) return "only mono or stereo";
    if (si.rate < 4000 || si.rate > 96000) return "unusual sample rate";
    if (si.bps != 8 && si.bps != 16 && si.bps != 24)
        return "only 8, 16 and 24 bits per sample";
    if (si.max_block > FLAC_MAX_BLOCK)
        return "block size larger than this decoder allows";

    /* --- frames --- */
    flac_br_t br;
    flac_br_init(&br, d, n);
    br.pos = off * 8u;

    /* How to get from the stream's depth to the 16 bits the codec wants */
    const int shift_down = (int)si.bps - 16;

    uint32_t written = 0;
    int frames = 0;
    while ((br.pos >> 3) + 16 <= n) {
        uint32_t block = 0;
        const char *bad = flac_frame(&br, &si, &block);
        if (bad) {
            /* A clean end of stream is not an error; a broken first
             * frame is. Anything decoded so far is still playable. */
            if (frames > 0) break;
            return bad;
        }
        frames++;

        for (uint32_t i = 0; i < block; i++) {
            if (written + 2 > out_max) { block = i; break; }
            int32_t l = flac_ch[0][i];
            int32_t r = si.nch == 2 ? flac_ch[1][i] : l;
            if (shift_down > 0)      { l >>= shift_down; r >>= shift_down; }
            else if (shift_down < 0) { l <<= -shift_down; r <<= -shift_down; }
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            out[written++] = (int16_t)l;
            out[written++] = (int16_t)r;
        }
        if (written + 2 > out_max) break;
    }

    if (frames == 0) return "no audio frames";
    *out_samples = written;
    *out_rate = si.rate;
    return 0;
}

#endif /* VEXTRO_FLAC_H */
