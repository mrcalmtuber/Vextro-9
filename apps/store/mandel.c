/*
 * mandel — escape-time Mandelbrot renderer for Vextro 9.
 *
 * Everything is 16.16 fixed point: the kernel is built without SSE and
 * without an FPU, so userland apps have no floats either.  Squares are
 * taken through int64_t because a 16.16 value squared needs 64 bits
 * before the shift back down.
 */
#include "../vextro.h"

#define FP        16
#define ONE       (1 << FP)
#define MAX_ITER  96

/* View: centre (-0.6, 0), 3.19 units wide across the canvas. */
#define CENTRE_X  (-39322)                     /* -0.6 in 16.16   */
#define STEP      350                          /* units per pixel */

static uint32_t mix(uint32_t a, uint32_t b, int t /* 0..255 */) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = (ar * (255 - t) + br * t) / 255;
    uint32_t g = (ag * (255 - t) + bg * t) / 255;
    uint32_t l = (ab * (255 - t) + bb * t) / 255;
    return (r << 16) | (g << 8) | l;
}

/* Deep blue -> gold -> warm white, black inside the set. */
static uint32_t shade(int iter) {
    if (iter >= MAX_ITER) return 0x05070Cu;
    int t = iter * 255 / MAX_ITER;
    if (t < 128) return mix(0x081226u, 0xD4AF37u, t * 2);
    return mix(0xD4AF37u, 0xFFF3D0u, (t - 128) * 2);
}

void _start(void) {
    os_print("mandel: escape-time fractal, 16.16 fixed point\n");

    int cx = OS_CANVAS_W / 2;
    int cy = OS_CANVAS_H / 2;

    for (int py = 0; py < OS_CANVAS_H; py++) {
        int y0 = (py - cy) * STEP;
        for (int px = 0; px < OS_CANVAS_W; px++) {
            int x0 = CENTRE_X + (px - cx) * STEP;

            int zx = 0, zy = 0;
            int iter = 0;
            for (; iter < MAX_ITER; iter++) {
                int64_t zx2 = ((int64_t)zx * zx) >> FP;
                int64_t zy2 = ((int64_t)zy * zy) >> FP;
                if (zx2 + zy2 > (int64_t)4 * ONE) break;
                int ny = (int)((((int64_t)zx * zy) >> (FP - 1)) + y0);
                zx = (int)(zx2 - zy2 + x0);
                zy = ny;
            }
            os_draw_pixel(px, py, shade(iter));
        }
    }

    os_print("mandel: done - 96 iterations per pixel, no FPU used\n");
}
