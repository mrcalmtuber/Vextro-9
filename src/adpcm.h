#ifndef VEXTRO_ADPCM_H
#define VEXTRO_ADPCM_H

/*
 * src/adpcm.h — the lossy codecs that live inside a WAVE container.
 *
 * Three formats, all of them integer, all of them decodable in a few
 * dozen lines. They are here because they are what a WAV file is
 * actually likely to hold when it is not raw PCM:
 *
 *   0x11  IMA ADPCM   4 bits per sample, ~4:1, block-adaptive
 *   0x06  A-law       8 bits per sample, the ITU-T G.711 European law
 *   0x07  mu-law      8 bits per sample, the G.711 North American law
 *
 * IMA ADPCM is a differential coder with a step size that walks up and
 * down a fixed table: each nibble says how far to move and, through the
 * index table, how much to trust the next one. The two tables below are
 * the published ones -- they are the format, not a tuning choice.
 *
 * G.711 is a logarithmic companding of 13 or 14 bits into 8, which is
 * pure bit manipulation. It is what a telephone sounds like, and it
 * decodes exactly.
 */

static const int16_t adpcm_step[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t adpcm_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int32_t pred;
    int32_t idx;
} adpcm_state_t;

/* One 4-bit code into one 16-bit sample, advancing the state. */
static int16_t adpcm_nibble(adpcm_state_t *s, uint8_t nib) {
    const int32_t step = adpcm_step[s->idx];

    /* step * (nib + 0.5) / 4, done in shifts */
    int32_t diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;

    s->pred += diff;
    if (s->pred > 32767) s->pred = 32767;
    else if (s->pred < -32768) s->pred = -32768;

    s->idx += adpcm_index[nib & 15u];
    if (s->idx < 0) s->idx = 0;
    else if (s->idx > 88) s->idx = 88;

    return (int16_t)s->pred;
}

/*
 * Decode an IMA ADPCM data chunk into interleaved stereo.
 *
 * The chunk is a sequence of blocks of `align` bytes. Each block opens
 * with a four-byte preamble per channel -- the starting predictor and
 * step index -- which is what lets playback begin at any block instead
 * of only at the start. After the preambles the nibbles arrive in
 * four-byte groups, one group per channel at a time, low nibble first.
 *
 * Returns samples written, or 0 if the layout does not hold together.
 */
static uint32_t adpcm_decode_ima(const uint8_t *d, uint32_t n, uint32_t align,
                                 int channels, int16_t *out, uint32_t out_max) {
    if (channels < 1 || channels > 2) return 0;
    const uint32_t preamble = 4u * (uint32_t)channels;
    if (align < preamble + 4u) return 0;

    uint32_t written = 0;
    for (uint32_t base = 0; base + align <= n; base += align) {
        const uint8_t *blk = d + base;
        adpcm_state_t st[2];

        for (int c = 0; c < channels; c++) {
            const uint8_t *p = blk + 4 * c;
            st[c].pred = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            st[c].idx = p[2];
            if (st[c].idx > 88) return written;   /* corrupt block header */
        }

        /* the preamble carries the first sample of each channel */
        if (written + 2 > out_max) return written;
        out[written++] = (int16_t)st[0].pred;
        out[written++] = (int16_t)st[channels == 2 ? 1 : 0].pred;

        for (uint32_t off = preamble; off + 4u * (uint32_t)channels <= align;
             off += 4u * (uint32_t)channels) {
            for (int k = 0; k < 8; k++) {
                int16_t s[2];
                for (int c = 0; c < channels; c++) {
                    const uint8_t byte = blk[off + 4 * c + (k >> 1)];
                    const uint8_t nib = (k & 1) ? (uint8_t)(byte >> 4)
                                                : (uint8_t)(byte & 0x0Fu);
                    s[c] = adpcm_nibble(&st[c], nib);
                }
                if (written + 2 > out_max) return written;
                out[written++] = s[0];
                out[written++] = channels == 2 ? s[1] : s[0];
            }
        }
    }
    return written;
}

/* ===== G.711 ===== */

/* mu-law: sign, 3-bit exponent, 4-bit mantissa, all stored inverted. */
static int16_t g711_ulaw(uint8_t u) {
    u = (uint8_t)~u;
    const int sign = u & 0x80u;
    const int exp = (u >> 4) & 7;
    const int man = u & 0x0Fu;
    int32_t v = ((man << 1) + 33) << exp;
    v -= 33;
    v <<= 2;
    if (v > 32767) v = 32767;
    return (int16_t)(sign ? -v : v);
}

/* A-law: the same idea with every other bit inverted and a different
 * treatment of the smallest exponent. */
static int16_t g711_alaw(uint8_t a) {
    a ^= 0x55u;
    const int sign = a & 0x80u;
    const int exp = (a >> 4) & 7;
    const int man = a & 0x0Fu;
    int32_t v = exp ? ((man << 1) + 33) << exp
                    : (man << 1) + 1;
    v <<= exp ? 2 : 3;
    if (v > 32767) v = 32767;
    return (int16_t)(sign ? -v : v);
}

/* A whole G.711 data chunk into interleaved stereo. */
static uint32_t g711_decode(const uint8_t *d, uint32_t n, int channels,
                            int alaw, int16_t *out, uint32_t out_max) {
    if (channels < 1 || channels > 2) return 0;
    uint32_t written = 0;
    for (uint32_t i = 0; i + (uint32_t)channels <= n;
         i += (uint32_t)channels) {
        const int16_t l = alaw ? g711_alaw(d[i]) : g711_ulaw(d[i]);
        const int16_t r = channels == 2
            ? (alaw ? g711_alaw(d[i + 1]) : g711_ulaw(d[i + 1])) : l;
        if (written + 2 > out_max) break;
        out[written++] = l;
        out[written++] = r;
    }
    return written;
}

#endif /* VEXTRO_ADPCM_H */
