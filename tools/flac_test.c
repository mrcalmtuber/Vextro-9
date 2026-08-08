/*
 * FLAC decoder check, run on the host against the reference encoder.
 *
 * FLAC is lossless, which makes the test unusually strong: decode a file
 * this decoder has never seen and the samples must match the original
 * PCM *exactly*. Not close -- exactly. tools/flac_test.py drives it:
 *   1. generate a WAV
 *   2. compress it with the reference `flac` encoder
 *   3. decode with this decoder and compare against the original
 *
 * A decoder tested only against its own encoder can hold a mistaken
 * belief about the format and still pass. This cannot.
 *
 *   cc -O2 -o build/flac_test tools/flac_test.c
 *   ./build/flac_test in.flac out.raw
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "../src/flac.h"

#define MAX_SAMPLES (1u << 24)          /* interleaved int16 */

static int16_t pcm[MAX_SAMPLES];

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.flac> <out.raw>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    uint32_t got = 0, rate = 0;
    const char *bad = flac_decode(buf, (uint64_t)n, pcm, MAX_SAMPLES,
                                  &got, &rate);
    if (bad) { fprintf(stderr, "decode failed: %s\n", bad); return 1; }

    fprintf(stderr, "decoded %u frames at %u Hz\n", got / 2, rate);
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }
    fwrite(pcm, 2, got, o);
    fclose(o);
    printf("%u %u\n", got, rate);
    return 0;
}
