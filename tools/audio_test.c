/*
 * IMA ADPCM and G.711 check, run on the host.
 *
 * Decodes a WAVE file through src/adpcm.h -- the same code the kernel
 * uses -- and writes raw interleaved 16-bit stereo. tools/audio_test.py
 * compares that against macOS `afconvert`, an independent decoder for
 * both formats, so agreement is evidence rather than self-consistency.
 *
 * The chunk walk here is a small duplicate of the one in media.h. That
 * is deliberate: adpcm.h holds the codecs and nothing else, so it can be
 * tested without dragging in the filesystem, the window manager and the
 * rest of the kernel.
 *
 *   cc -O2 -o build/audio_test tools/audio_test.c
 *   ./build/audio_test in.wav out.raw
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "../src/adpcm.h"

#define MAX_SAMPLES (1u << 24)

static int16_t pcm[MAX_SAMPLES];

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static int tag(const uint8_t *p, const char *t) {
    return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] &&
           p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3];
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.wav> <out.raw>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    if (n < 44 || !tag(d, "RIFF") || !tag(d + 8, "WAVE")) {
        fprintf(stderr, "not a WAVE file\n");
        return 1;
    }

    uint64_t off = 12;
    uint16_t fmt = 0, ch = 2, bits = 16, align = 0;
    uint32_t rate = 0, got = 0;
    int have_fmt = 0;

    while (off + 8 <= (uint64_t)n) {
        const uint8_t *hdr = d + off;
        const uint32_t sz = rd32(hdr + 4);
        const uint64_t body = off + 8;
        if (sz > (uint64_t)n - body) break;

        if (tag(hdr, "fmt ") && sz >= 16) {
            fmt = rd16(d + body);
            ch = rd16(d + body + 2);
            rate = rd32(d + body + 4);
            align = rd16(d + body + 12);
            bits = rd16(d + body + 14);
            have_fmt = 1;
        } else if (tag(hdr, "data") && have_fmt) {
            if (fmt == 0x11)
                got = adpcm_decode_ima(d + body, sz, align, ch,
                                       pcm, MAX_SAMPLES);
            else if (fmt == 6 || fmt == 7)
                got = g711_decode(d + body, sz, ch, fmt == 6,
                                  pcm, MAX_SAMPLES);
            else {
                fprintf(stderr, "codec 0x%x is not one this tests\n", fmt);
                return 1;
            }
            break;
        }
        off = body + sz + (sz & 1);
    }
    (void)bits;

    if (!got) { fprintf(stderr, "decoded nothing\n"); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }
    fwrite(pcm, 2, got, o);
    fclose(o);
    printf("%u %u\n", got, rate);
    return 0;
}
