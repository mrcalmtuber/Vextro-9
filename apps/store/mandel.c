/*
 * mandel — escape-time Mandelbrot renderer for Vextro 9.
 *
 * This used to be entirely 16.16 fixed point, and the comment at the top
 * said why: the kernel was built without SSE and without an FPU, so
 * userland had no floats either. Both halves of that are now false. A
 * program runs in ring 3 with its own floating-point state, saved and
 * restored on every context switch, so it can use the unit as freely as
 * anything else on the machine.
 *
 * What that buys is not speed — the fixed-point version was fast — it is
 * range and smoothness. Sixteen fractional bits run out at about a
 * thousandth of the width of the set, and an iteration count is an
 * integer, so the bands between colours were steps. Doubles have fifty-two
 * bits, and the normalised iteration count below is continuous: the
 * colour varies smoothly across a band instead of jumping at its edge.
 *
 * It also writes to the window's pixels directly rather than asking the
 * kernel for each one. The canvas is mapped into this address space, so
 * a full repaint is 240,000 stores instead of 240,000 syscalls.
 */
#include "../vextro.h"
#include <stdint.h>

#define MAX_ITER   320
#define BAILOUT    (1 << 16)        /* large, so the smoothing is accurate */

/* View: centre (-0.6, 0), 3.2 units across the canvas. */
#define CENTRE_X   (-0.6)
#define CENTRE_Y   ( 0.0)
#define VIEW_W     ( 3.2)

/*
 * log2, from the exponent field and a cubic on the mantissa.
 *
 * There is no maths library here and there does not need to be: an IEEE
 * float already carries its own base-2 logarithm's integer part in the
 * exponent, so all that is left is the fraction, and a cubic fitted on
 * [1,2) gets that to about a hundredth. Colour is the only consumer, and
 * a hundredth of a shade is invisible.
 */
static float log2f_fast(float x) {
    union { float f; uint32_t i; } v;
    v.f = x;
    int e = (int)((v.i >> 23) & 0xFFu) - 127;
    v.i = (v.i & 0x007FFFFFu) | 0x3F800000u;    /* mantissa into [1,2) */
    float m = v.f;
    float p = ((0.1520749f * m - 0.9130591f) * m + 2.4757048f) * m
              - 1.7145509f;
    return (float)e + p;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float ar = (float)((a >> 16) & 0xFF), ag = (float)((a >> 8) & 0xFF);
    float ab = (float)(a & 0xFF);
    float br = (float)((b >> 16) & 0xFF), bg = (float)((b >> 8) & 0xFF);
    float bb = (float)(b & 0xFF);
    uint32_t r = (uint32_t)(ar + (br - ar) * t);
    uint32_t g = (uint32_t)(ag + (bg - ag) * t);
    uint32_t l = (uint32_t)(ab + (bb - ab) * t);
    return (r << 16) | (g << 8) | l;
}

/* Deep blue -> gold -> warm white, black inside the set. */
static uint32_t shade(float t) {
    if (t < 0.0f) return 0x05070Cu;
    float u = t * 2.0f;
    if (u < 1.0f) return mix(0x081226u, 0xD4AF37u, u);
    return mix(0xD4AF37u, 0xFFF3D0u, u - 1.0f);
}

void _start(void) {
    os_print("mandel: escape-time fractal in double precision\n");

    int cw = OS_CANVAS_W, ch = OS_CANVAS_H;
    uint32_t *canvas = os_canvas(&cw, &ch);
    if (canvas)
        os_print("mandel: drawing straight into the mapped canvas\n");

    const double step = VIEW_W / (double)cw;
    const double x0   = CENTRE_X - step * (double)cw * 0.5;
    const double y0   = CENTRE_Y - step * (double)ch * 0.5;

    for (int py = 0; py < ch; py++) {
        const double ci = y0 + step * (double)py;
        for (int px = 0; px < cw; px++) {
            const double cr = x0 + step * (double)px;

            double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
            int n = 0;
            while (n < MAX_ITER && zr2 + zi2 <= (double)BAILOUT) {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                n++;
            }

            float t;
            if (n >= MAX_ITER) {
                t = -1.0f;                     /* inside the set */
            } else {
                /*
                 * Normalised iteration count. The escaped magnitude says
                 * how far past the bailout this point went, and taking
                 * the logarithm of its logarithm turns that into the
                 * fraction of an iteration it overshot by -- which is
                 * what makes the bands continuous rather than stepped.
                 */
                float mag = (float)(zr2 + zi2);
                float nu  = log2f_fast(log2f_fast(mag) * 0.5f);
                t = ((float)n + 1.0f - nu) / (float)MAX_ITER;
                t = t * 3.2f;                  /* stretch the useful range */
                if (t > 1.0f) t = 1.0f;
            }

            uint32_t c = shade(t);
            if (canvas) canvas[py * cw + px] = c;
            else        os_draw_pixel(px, py, c);
        }

        /* The scheduler preempts this anyway, a thousand times a second.
         * Standing aside at the end of each row simply means the window
         * fills in visibly rather than in one jump at the end. */
        if ((py & 15) == 0) os_yield();
    }

    os_print("mandel: done - 320 iterations, smooth colouring\n");
}
