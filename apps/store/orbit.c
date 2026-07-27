/*
 * orbit — Newtonian gravity integrator for Socrates BSD 9.
 *
 * Five bodies fall around a central star under a = -GM r / |r|^3,
 * advanced with a semi-implicit (Euler-Cromer) step so the orbits stay
 * closed instead of spiralling out the way plain Euler would.
 *
 * All state is 16.16 fixed point pixels; the square root is a
 * bit-by-bit integer isqrt over int64_t.  No FPU is involved.
 */
#include "../socrates.h"

#define FP     16
#define ONE    (1 << FP)

/* GM chosen so a body at r = 150 px circles once every ~1200 steps. */
#define GM     6062080                  /* 92.5 px^3/step^2 in 16.16 */

#define BODIES 5
#define STEPS  4200

static int64_t isqrt64(int64_t val) {
    if (val <= 0) return 0;
    uint64_t v = (uint64_t)val;
    uint64_t rem = 0, root = 0;
    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | ((v >> 62) & 3);
        v <<= 2;
        if (root < rem) {
            rem -= root + 1;
            root += 2;
        }
    }
    return (int64_t)(root >> 1);
}

static void dot(int x, int y, uint32_t color) {
    if (x < 0 || x >= OS_CANVAS_W || y < 0 || y >= OS_CANVAS_H) return;
    os_draw_pixel(x, y, color);
}

static void disc(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                dot(cx + dx, cy + dy, color);
}

/* Body state, kept in parallel arrays so nothing lands in .data as a
 * pointer (the kernel's ELF loader performs no relocation). */
static int64_t px[BODIES], py[BODIES], vx[BODIES], vy[BODIES];
/* Launch radii, and the launch speed as a fraction of the circular
 * speed (in 256ths).  Anything other than 256 opens the orbit into an
 * ellipse; these are picked so the apoapsis still clears the canvas. */
static int     radius[BODIES] = { 48, 74, 104, 134, 164 };
static int     ecc[BODIES]    = { 256, 224, 256, 272, 240 };
static uint32_t bright[BODIES] = {
    0xFFE9A8u, 0x8FD0F0u, 0xE07A5Fu, 0x9AE6B4u, 0xC0A8F0u,
};
static uint32_t faint[BODIES] = {
    0x4A422Cu, 0x2A3E4Au, 0x452A24u, 0x25412Fu, 0x38304Au,
};

void _start(void) {
    os_print("orbit: 5-body gravity integrator, 16.16 fixed point\n");

    int cx = OS_CANVAS_W / 2;
    int cy = OS_CANVAS_H / 2;

    /* Background: a dark field with a faint vertical gradient. */
    for (int y = 0; y < OS_CANVAS_H; y++) {
        uint32_t shade = 0x05070Cu + (uint32_t)(y / 40) * 0x000102u;
        for (int x = 0; x < OS_CANVAS_W; x++)
            os_draw_pixel(x, y, shade);
    }

    /* Seed each body on a circular orbit, then perturb the speed so a
     * couple of them trace visible ellipses. */
    for (int i = 0; i < BODIES; i++) {
        int64_t r = (int64_t)radius[i] << FP;
        px[i] = r;
        py[i] = 0;
        int64_t vcirc = isqrt64(((int64_t)GM << 32) / r);
        vx[i] = 0;
        vy[i] = vcirc * ecc[i] / 256;
    }

    /* Integrate, laying down a trail dot per body per step. */
    for (int s = 0; s < STEPS; s++) {
        for (int i = 0; i < BODIES; i++) {
            int64_t r2 = ((px[i] * px[i]) >> FP) + ((py[i] * py[i]) >> FP);
            if (r2 < (int64_t)16 << FP) continue;      /* swallowed by the star */
            int64_t r = isqrt64(r2 << FP);
            int64_t acc = ((int64_t)GM << FP) / r2;    /* |a| in 16.16 */

            vx[i] -= acc * px[i] / r;
            vy[i] -= acc * py[i] / r;
            px[i] += vx[i];
            py[i] += vy[i];

            dot(cx + (int)(px[i] >> FP), cy + (int)(py[i] >> FP), faint[i]);
        }
    }

    /* Star, then each body at its final position. */
    disc(cx, cy, 9, 0x3A2E10u);
    disc(cx, cy, 6, 0xD4AF37u);
    disc(cx, cy, 3, 0xFFF6DCu);

    for (int i = 0; i < BODIES; i++)
        disc(cx + (int)(px[i] >> FP), cy + (int)(py[i] >> FP), 3, bright[i]);

    os_print("orbit: 4200 steps integrated\n");
}
