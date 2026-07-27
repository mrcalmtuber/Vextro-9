/*
 * plasma — interference field renderer for Socrates BSD 9.
 *
 * There is no libm and no FPU here, so the sine table is generated at
 * startup by running a magic-circle oscillator:
 *
 *     x -= (d * y) >> 16;   y += (d * x) >> 16;
 *
 * With d = 2*pi/256 in 16.16 that is a rotation matrix accurate to a
 * few parts in ten thousand, and because it is symplectic the vector
 * stays on the unit circle instead of spiralling out over 256 steps.
 */
#include "../socrates.h"

#define FP    16
#define ONE   (1 << FP)
#define TWOPI 1608            /* 2*pi/256 in 16.16 */

static int sintab[256];       /* one period, amplitude 65536 */

static void build_sin(void) {
    int64_t x = ONE, y = 0;
    for (int i = 0; i < 256; i++) {
        sintab[i] = (int)y;
        x -= (TWOPI * y) >> FP;
        y += (TWOPI * x) >> FP;
    }
}

static int isqrt32(int v) {
    if (v <= 0) return 0;
    unsigned int rem = 0, root = 0, val = (unsigned int)v;
    for (int i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | ((val >> 30) & 3);
        val <<= 2;
        if (root < rem) {
            rem -= root + 1;
            root += 2;
        }
    }
    return (int)(root >> 1);
}

static uint32_t mix(uint32_t a, uint32_t b, int t /* 0..255 */) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((((ar * (255 - t) + br * t) / 255) << 16) |
            (((ag * (255 - t) + bg * t) / 255) << 8) |
            (((ab * (255 - t) + bb * t) / 255)));
}

/* Midnight -> violet -> gold -> warm white. */
static uint32_t palette(int t) {
    if (t < 85)  return mix(0x070A16u, 0x4A2A6Au, t * 3);
    if (t < 170) return mix(0x4A2A6Au, 0xD4AF37u, (t - 85) * 3);
    return mix(0xD4AF37u, 0xFFF6DCu, (t - 170) * 3);
}

void _start(void) {
    os_print("plasma: interference field, runtime-generated sine table\n");
    build_sin();

    int cx = OS_CANVAS_W / 2;
    int cy = OS_CANVAS_H / 2;

    for (int y = 0; y < OS_CANVAS_H; y++) {
        for (int x = 0; x < OS_CANVAS_W; x++) {
            int dx = x - cx, dy = y - cy;
            int dist = isqrt32(dx * dx + dy * dy);

            int v = sintab[(x * 3 / 2) & 255] +
                    sintab[(y * 2) & 255] +
                    sintab[((x + y) * 3 / 4) & 255] +
                    sintab[(dist * 2) & 255];

            int t = (v + 4 * ONE) >> 11;
            if (t < 0) t = 0;
            if (t > 255) t = 255;

            os_draw_pixel(x, y, palette(t));
        }
    }

    os_print("plasma: done\n");
}
