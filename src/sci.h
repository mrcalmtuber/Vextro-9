#ifndef SCI_H
#define SCI_H

#include <stdint.h>
#include "lzma.h"

/*
 * .sci — Socrates Compressed Image.
 *
 * Full-colour images small enough to keep on a 64 MB disk.  The scheme
 * is the one PNG uses, because on photographic and synthetic images it
 * is hard to beat for the complexity:
 *
 *   1. Each row is prefixed with a filter byte and predicted from its
 *      left and upper neighbours (None / Sub / Up / Average / Paeth).
 *      The encoder picks whichever filter minimises the row's absolute
 *      sum, which turns smooth gradients into runs of near-zero bytes.
 *   2. The filtered plane is compressed as one LZMA stream — the same
 *      decoder the ZIM reader uses for xz clusters.
 *
 * On decode the plane is reconstructed in place and then converted to
 * the 0x00RRGGBB the framebuffer wants.
 *
 * Layout (little-endian, read byte-wise so alignment never matters):
 *
 *   0   magic 'S' 'C' 'I' 0x01
 *   4   uint16 width
 *   6   uint16 height
 *   8   uint8  channels     3 = RGB, 4 = RGBA (alpha is composited out)
 *   9   uint8  filter mode  1 = PNG adaptive per row
 *   10  uint8  codec        1 = raw LZMA
 *   11  uint8  reserved
 *   12  uint32 raw_size     height * (1 + width * channels)
 *   16  uint32 comp_size
 *   20  LZMA stream
 *
 * tools/mkimg.py writes this; it decodes PNG with nothing but Python's
 * own zlib, so the build has no image-library dependency.
 */

#define SCI_HDR_SIZE   20
#define SCI_MAGIC0     'S'
#define SCI_MAGIC1     'C'
#define SCI_MAGIC2     'I'
#define SCI_MAGIC3     0x01

#define SCI_FILTER_PNG 1
#define SCI_CODEC_LZMA 1

/* Bounds the static buffers below; also the largest image we accept. */
#define SCI_MAX_W      1280
#define SCI_MAX_H      800
#define SCI_MAX_RAW    ((uint32_t)SCI_MAX_H * (1 + SCI_MAX_W * 4))

typedef struct {
    uint32_t width, height;
    uint32_t channels;
    uint32_t filter, codec;
    uint32_t raw_size, comp_size;
} sci_info_t;

/* Reconstructed pixels, and the filtered plane the codec writes into. */
static uint32_t sci_pixels[SCI_MAX_W * SCI_MAX_H];
static uint8_t  sci_plane[SCI_MAX_RAW];

static uint32_t sci_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t sci_rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Returns NULL when the header describes an image we can load. */
static const char *sci_parse(const uint8_t *d, uint64_t len, sci_info_t *out) {
    if (len < SCI_HDR_SIZE) return "file is smaller than the header";
    if (d[0] != SCI_MAGIC0 || d[1] != SCI_MAGIC1 ||
        d[2] != SCI_MAGIC2 || d[3] != SCI_MAGIC3)
        return "not a .sci image";

    out->width    = sci_rd16(d + 4);
    out->height   = sci_rd16(d + 6);
    out->channels = d[8];
    out->filter   = d[9];
    out->codec    = d[10];
    out->raw_size = sci_rd32(d + 12);
    out->comp_size = sci_rd32(d + 16);

    if (out->width == 0 || out->height == 0) return "zero-sized image";
    if (out->width > SCI_MAX_W || out->height > SCI_MAX_H)
        return "image is larger than this viewer supports";
    if (out->channels != 3 && out->channels != 4)
        return "only RGB and RGBA are supported";
    if (out->filter != SCI_FILTER_PNG) return "unknown filter mode";
    if (out->codec != SCI_CODEC_LZMA) return "unknown codec";

    uint32_t stride = 1 + out->width * out->channels;
    if (out->raw_size != stride * out->height)
        return "raw size does not match the dimensions";
    if (out->raw_size > SCI_MAX_RAW) return "image plane is too large";
    if (out->comp_size > len - SCI_HDR_SIZE)
        return "compressed stream runs past the end of the file";
    return 0;
}

static int sci_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/*
 * Undo the row filters in place, then pack to 0x00RRGGBB.
 * Every row's filter byte is consumed as we go, so the plane is walked
 * exactly once.
 */
static const char *sci_reconstruct(const sci_info_t *info) {
    uint32_t bpp = info->channels;
    uint32_t rowlen = info->width * bpp;
    uint32_t stride = 1 + rowlen;

    for (uint32_t y = 0; y < info->height; y++) {
        uint8_t *row = sci_plane + (uint64_t)y * stride;
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = y > 0 ? (sci_plane + (uint64_t)(y - 1) * stride + 1)
                                    : 0;

        switch (filter) {
        case 0:
            break;
        case 1:
            for (uint32_t i = bpp; i < rowlen; i++)
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            break;
        case 2:
            if (prev)
                for (uint32_t i = 0; i < rowlen; i++)
                    cur[i] = (uint8_t)(cur[i] + prev[i]);
            break;
        case 3:
            for (uint32_t i = 0; i < rowlen; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (uint32_t i = 0; i < rowlen; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                cur[i] = (uint8_t)(cur[i] + sci_paeth(a, b, c));
            }
            break;
        default:
            return "unknown row filter";
        }

        uint32_t *dst = sci_pixels + (uint64_t)y * info->width;
        for (uint32_t x = 0; x < info->width; x++) {
            const uint8_t *px = cur + x * bpp;
            uint32_t r = px[0], g = px[1], b = px[2];
            if (bpp == 4) {
                /* No alpha in the framebuffer: composite onto white so
                 * transparent PNGs do not come out as black holes. */
                uint32_t a = px[3];
                r = (r * a + 255 * (255 - a)) / 255;
                g = (g * a + 255 * (255 - a)) / 255;
                b = (b * a + 255 * (255 - a)) / 255;
            }
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
    return 0;
}

/*
 * Decode a whole .sci file into sci_pixels.  Returns NULL on success or
 * a reason string, which the viewer shows verbatim.
 */
static const char *sci_decode(const uint8_t *data, uint64_t len,
                              sci_info_t *info) {
    const char *bad = sci_parse(data, len, info);
    if (bad) return bad;

    uint64_t produced = 0;
    const char *err = "decompression failed";
    if (lzma_alone_decode(data + SCI_HDR_SIZE, info->comp_size,
                          sci_plane, SCI_MAX_RAW, &produced, &err) != 0)
        return err;
    if (produced != info->raw_size)
        return "decompressed size does not match the header";

    return sci_reconstruct(info);
}

#endif /* SCI_H */
