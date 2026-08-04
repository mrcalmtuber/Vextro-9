#ifndef BOOTANIM_H
#define BOOTANIM_H

#include <stdint.h>
#include "sincos_lut.h"

/*
 * The boot animation, computed rather than played back.
 *
 * There used to be a video here: 121 frames of 320x240 RGB565, which is
 * 18.5 MB of raw pixels carried in the ISO, generated from a 6.8 MB .mp4
 * carried in the repository, by ffmpeg -- a tool nothing else in this
 * project needs and which therefore had to be either a build dependency
 * or a thing that could be missing. Five seconds of screen cost more than
 * the entire rest of the system.
 *
 * This computes those seconds instead. The repository carries no video,
 * the ISO carries no frames, the build needs no ffmpeg, and being a
 * simulation rather than a recording it fills whatever screen it is
 * handed at whatever resolution that screen happens to be.
 *
 * ---- what it draws ----
 *
 * A sheet of liquid glass slides across the mark, left to right, and the
 * mark is only ever seen through it. That is a deliberate choice and not
 * only an aesthetic one: the simulation runs at a fraction of the panel's
 * resolution and is scaled up, so there is no fine detail to lose behind
 * the refraction. What would be a limitation in a photograph is the
 * subject here.
 *
 * The glass is four travelling waves at different orientations, speeds
 * and wavelengths -- x, y, x+y and x-y. Four is the smallest number that
 * stops the interference pattern reading as a grid, and because each one
 * varies along a single axis, all four are one-dimensional tables rebuilt
 * once per frame and then only indexed per pixel. The inner loop does no
 * trigonometry at all.
 *
 * From those tables come the two things that make glass look like glass:
 *
 *   the slope of the surface, which bends what is behind it -- the mark
 *   is sampled at an offset proportional to the gradient, so it swims;
 *
 *   and the same slope against a light direction, which is the highlight
 *   that runs along a moving surface and is most of what the eye reads as
 *   "wet".
 *
 * The leading edge carries a brighter flare, so the sheet has a front
 * rather than simply fading in. After it has crossed, the amplitude
 * decays and the mark settles.
 *
 * ---- constraints ----
 *
 * Integer only, like everything else in this kernel: the 360-entry sine
 * table at 1024 scale is the only source of curves, and every divide is a
 * shift. This runs before fpu_init(), before the IDT exists and before
 * any driver has been probed, so it can depend on nothing but the
 * framebuffer it is given.
 *
 * Include after ttf.h: the wordmark is set in the system's own face.
 */

#define BA_MAXW   640
#define BA_MAXH   400
#define BA_FRAMES 120          /* 5 seconds at 24 fps */

#define BA_GOLD_R 0xD4         /* C_GOLD, spelled out: gfx.h may not be */
#define BA_GOLD_G 0xAF         /* included yet where this is used       */
#define BA_GOLD_B 0x37

/* The mark, drawn once. Stride is ba_w, not BA_MAXW. */
static uint32_t ba_bg[BA_MAXW * BA_MAXH];

static int ba_w = 320, ba_h = 200;    /* simulation size  */
static int ba_scale = 1;              /* integer upscale  */
static int ba_offx = 0, ba_offy = 0;  /* centring         */

/*
 * Per-frame wave tables. Height and slope for each of the four
 * orientations; the diagonals are indexed by x+y and by x-y+ba_h, so both
 * need ba_w + ba_h entries.
 */
static int16_t ba_hx[BA_MAXW], ba_gxt[BA_MAXW];
static int16_t ba_hy[BA_MAXH], ba_gyt[BA_MAXH];
/*
 * The diagonals are read at a warped index -- see ba_render -- so they are
 * built over a wider range than the screen needs and addressed with a
 * bias. Sizing the table for the warp instead of clamping every lookup
 * keeps two compares out of the inner loop.
 */
#define BA_BIAS 128
#define BA_DIAG (BA_MAXW + BA_MAXH + 2 * BA_BIAS)

static int16_t ba_hd[BA_DIAG], ba_gdt[BA_DIAG];
static int16_t ba_he[BA_DIAG], ba_get[BA_DIAG];

/* How much glass covers each column this frame, and the flare at its
 * leading edge. Both vary along x alone. */
static int16_t ba_env[BA_MAXW], ba_flare[BA_MAXW];

static inline int32_t ba_sin(int32_t d) {
    d %= 360; if (d < 0) d += 360;
    return int_sin[d];
}
static inline int32_t ba_cos(int32_t d) {
    d %= 360; if (d < 0) d += 360;
    return int_cos[d];
}

static inline int32_t ba_clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ===== the mark ===== */

static void ba_draw_bg(void) {
    const int w = ba_w, h = ba_h;
    const int cx = w / 2;
    const int cy = h * 40 / 100;

    /* A cold vertical gradient with a warm glow behind the mark. The
     * falloff is quadratic rather than a real radius: a soft glow is
     * exactly the case where nobody can tell, and it saves a square root
     * per pixel at a point in boot where there is no FPU switched on. */
    const int32_t glow_k = (w * w + h * h) / 900 + 1;

    for (int y = 0; y < h; y++) {
        const int32_t t = (y * 255) / (h - 1);
        const int32_t br = 4 + ((t * 6) >> 8);
        const int32_t bg_ = 5 + ((t * 8) >> 8);
        const int32_t bb = 9 + ((t * 13) >> 8);

        for (int x = 0; x < w; x++) {
            const int dx = x - cx, dy = y - cy;
            int32_t glow = 230 - (dx * dx + dy * dy) / glow_k;
            if (glow < 0) glow = 0;

            int32_t r = br + ((glow * BA_GOLD_R) >> 11);
            int32_t g = bg_ + ((glow * BA_GOLD_G) >> 11);
            int32_t b = bb + ((glow * BA_GOLD_B) >> 11);

            ba_bg[y * w + x] = ((uint32_t)ba_clampi(r, 0, 255) << 16) |
                               ((uint32_t)ba_clampi(g, 0, 255) << 8) |
                                (uint32_t)ba_clampi(b, 0, 255);
        }
    }

    /* The diamond the menubar wears, at |dx| + |dy| <= r. Hollow, because
     * a ring catches the refraction along two edges instead of one and
     * reads far better through moving glass than a solid does. */
    const int dr = h / 9;
    const int thick = dr / 4 + 1;
    for (int y = cy - dr; y <= cy + dr; y++) {
        if (y < 0 || y >= h) continue;
        for (int x = cx - dr; x <= cx + dr; x++) {
            if (x < 0 || x >= w) continue;
            int ax = x - cx; if (ax < 0) ax = -ax;
            int ay = y - cy; if (ay < 0) ay = -ay;
            const int d = ax + ay;
            if (d > dr || d < dr - thick) continue;
            ba_bg[y * w + x] = ((uint32_t)BA_GOLD_R << 16) |
                               ((uint32_t)BA_GOLD_G << 8) | BA_GOLD_B;
        }
    }

    /* The wordmark, in the face the system draws itself in. */
    {
        const char *s = "SOCRATES BSD 9";
        const int size = (h >= 300) ? 30 : 15;
        const int tw = ttf_text_width(s, size);
        ttf_draw_string(ba_bg, w, h, (w - tw) / 2, cy + dr + h / 14,
                        s, ((uint32_t)BA_GOLD_R << 16) |
                           ((uint32_t)BA_GOLD_G << 8) | BA_GOLD_B, size);
    }
}

/*
 * Choose a simulation size for this panel and draw the mark into it.
 *
 * Two sizes rather than one: 320x200 is 16:10 and scales by a whole
 * number onto most panels, but on anything 1280x800 or larger there is
 * room for four times the detail at the same cost per output pixel, and
 * the mark is worth it. Both are exact integer upscales, so nothing is
 * ever resampled.
 */
static void ba_init(int dst_w, int dst_h) {
    ba_w = 320; ba_h = 200;
    if (dst_w >= 1280 && dst_h >= 800) { ba_w = 640; ba_h = 400; }

    ba_scale = dst_w / ba_w;
    const int sy = dst_h / ba_h;
    if (sy < ba_scale) ba_scale = sy;
    if (ba_scale < 1) {
        /* A panel smaller than the simulation: fall back to the small
         * one, and accept clipping rather than not drawing at all. */
        ba_w = 320; ba_h = 200; ba_scale = 1;
    }

    ba_offx = (dst_w - ba_w * ba_scale) / 2;
    ba_offy = (dst_h - ba_h * ba_scale) / 2;
    if (ba_offx < 0) ba_offx = 0;
    if (ba_offy < 0) ba_offy = 0;

    ba_draw_bg();
}

/* ===== one frame ===== */

/* 4x4 ordered dither, applied at the panel's resolution rather than the
 * simulation's. The gradient behind the mark spans a handful of levels
 * over hundreds of rows, which bands visibly on a flat panel; scattering
 * the rounding error at full resolution costs one table lookup per output
 * pixel and removes it.
 *
 * Kept to a single level. The gradient steps by one at a time, so one
 * level is all that is needed to break it -- and the screen is mostly
 * near-black, where a wider spread stops being a smoothing and becomes a
 * visible weave. */
static const int8_t ba_bayer[16] = {
    -1,  1, -1,  1,
     1, -1,  0, -1,
    -1,  1, -1,  0,
     0, -1,  1, -1,
};

static void ba_tables(int frame) {
    const int32_t t = frame;

    /* Amplitude: full while the sheet is crossing, then decaying to a
     * residual shimmer rather than to nothing -- glass that has stopped
     * moving entirely stops reading as liquid. */
    int32_t amp = 256;
    if (t > 62) {
        amp = 256 - ((t - 62) * 210) / 46;
        if (amp < 46) amp = 46;
    }

    /*
     * Two scales, and the coupling between them is the whole trick.
     *
     * Four plain sine waves interfere into a lattice -- regular, obviously
     * synthetic, and nothing like a liquid. So the first pair are slow and
     * broad and are not really waves at all: they are a flow field, and
     * their height is used in ba_render to displace where the second,
     * finer pair is sampled. Bending a periodic function through another
     * periodic function is what breaks the repeat and gives the surface
     * something to flow along.
     */
    for (int x = 0; x < ba_w; x++) {
        const int32_t p = x * 2 + t * 7;
        ba_hx[x]  = (int16_t)((ba_sin(p) * 44 * amp) >> 16);
        ba_gxt[x] = (int16_t)((ba_cos(p) * 7 * amp) >> 16);
    }
    for (int y = 0; y < ba_h; y++) {
        const int32_t p = y * 3 - t * 5;
        ba_hy[y]  = (int16_t)((ba_sin(p) * 38 * amp) >> 16);
        ba_gyt[y] = (int16_t)((ba_cos(p) * 7 * amp) >> 16);
    }
    const int n = ba_w + ba_h + 2 * BA_BIAS;
    for (int i = 0; i < n; i++) {
        const int32_t j = i - BA_BIAS;
        const int32_t p1 = j * 9 + t * 11;
        ba_hd[i]  = (int16_t)((ba_sin(p1) * 10 * amp) >> 16);
        ba_gdt[i] = (int16_t)((ba_cos(p1) * 15 * amp) >> 16);
        const int32_t p2 = j * 13 - t * 8;
        ba_he[i]  = (int16_t)((ba_sin(p2) * 8 * amp) >> 16);
        ba_get[i] = (int16_t)((ba_cos(p2) * 13 * amp) >> 16);
    }

    /*
     * The sheet's leading edge, sweeping past the right-hand side by the
     * time it has finished. The ramp behind it is what makes it a sheet
     * arriving rather than the whole screen switching on.
     */
    const int32_t ramp = ba_w / 6 + 1;
    const int32_t edge = (t * (ba_w + 2 * ramp)) / 60 - ramp;

    for (int x = 0; x < ba_w; x++) {
        int32_t e = ((edge - x) * 256) / ramp;
        ba_env[x] = (int16_t)ba_clampi(e, 0, 256);

        /* A flare on the edge itself: brightest where the ramp is
         * steepest, which is where a real sheet would catch the light. */
        int32_t d = edge - x; if (d < 0) d = -d;
        int32_t f = 256 - (d * 256) / (ramp / 2 + 1);
        ba_flare[x] = (int16_t)ba_clampi(f, 0, 256);
    }
}

/*
 * Render one frame into a 32-bit XRGB buffer.
 *
 * The caller owns presentation: the x86 tree writes straight to the
 * framebuffer and paces on the PIT, the aarch64 tree composes into its
 * back buffer and paces on the architected timer. Everything above that
 * line is the same on both, which is why it lives in one file.
 */
static void ba_render(uint32_t *dst, int stride, int frame) {
    ba_tables(frame);

    /* The last stretch dims to black, so the login screen does not
     * arrive as a cut. */
    int32_t fade = 256;
    if (frame >= BA_FRAMES - 18)
        fade = ((BA_FRAMES - 1 - frame) * 256) / 18;

    const int w = ba_w, h = ba_h, sc = ba_scale;

    for (int y = 0; y < h; y++) {
        const int16_t gy_row = ba_gyt[y];

        for (int x = 0; x < w; x++) {
            const int32_t e = ba_env[x];

            int32_t r, g, b;

            if (e == 0) {
                /* Ahead of the sheet: bare mark, well below the glass. */
                const uint32_t c = ba_bg[y * w + x];
                r = (int32_t)((c >> 16) & 0xFF) >> 2;
                g = (int32_t)((c >> 8) & 0xFF) >> 2;
                b = (int32_t)(c & 0xFF) >> 2;
            } else {
                /* Where the flow field carries this pixel. Bounded by
                 * construction, which is why the tables above are wider
                 * than the screen and no clamp is needed here. */
                int32_t warp = (ba_hx[x] + ba_hy[y]) >> 3;
                if (warp >  BA_BIAS - 8) warp =  BA_BIAS - 8;
                if (warp < -(BA_BIAS - 8)) warp = -(BA_BIAS - 8);

                const int d1 = x + y + warp + BA_BIAS;
                const int d2 = x - y + h - warp + BA_BIAS;

                const int32_t gx = ba_gxt[x] + ba_gdt[d1] + ba_get[d2];
                const int32_t gy = gy_row + ba_gdt[d1] - ba_get[d2];

                /* Refraction: sample the mark where the surface bends it
                 * to, scaled by how much glass is over this column. */
                int sxp = x + ((gx * e) >> 12);
                int syp = y + ((gy * e) >> 12);
                sxp = ba_clampi(sxp, 0, w - 1);
                syp = ba_clampi(syp, 0, h - 1);

                const uint32_t c = ba_bg[syp * w + sxp];
                r = (int32_t)((c >> 16) & 0xFF);
                g = (int32_t)((c >> 8) & 0xFF);
                b = (int32_t)(c & 0xFF);

                /* Thickness: the glass is not colourless, it carries a
                 * trace of the gold it is lying on. */
                const int32_t hsum = ba_hx[x] + ba_hy[y] + ba_hd[d1] + ba_he[d2];
                const int32_t tint = (hsum * e) >> 12;
                r += (tint * BA_GOLD_R) >> 8;
                g += (tint * BA_GOLD_G) >> 8;
                b += (tint * BA_GOLD_B) >> 8;

                /* Specular: the slope against a light up and to the left.
                 * Squared, so the highlight is a band rather than a wash;
                 * this is most of what the eye reads as a wet surface. */
                int32_t sp = gx - gy;
                if (sp > 0) {
                    sp = (sp * sp) >> 8;
                    sp = (sp * e) >> 8;
                    if (sp > 255) sp = 255;
                    r += sp;
                    g += (sp * 246) >> 8;
                    b += (sp * 214) >> 8;
                }
                /* and a cool rim down the far side, which is what
                 * gives a wave a back as well as a front. */
                else {
                    int32_t rim = ((-sp) * e) >> 13;
                    if (rim > 30) rim = 30;
                    r += rim >> 1;
                    g += (rim * 3) >> 2;
                    b += rim;
                }

                /* The leading edge itself. */
                const int32_t fl = ba_flare[x];
                if (fl) {
                    r += (fl * 90) >> 8;
                    g += (fl * 80) >> 8;
                    b += (fl * 52) >> 8;
                }
            }

            r = (r * fade) >> 8;
            g = (g * fade) >> 8;
            b = (b * fade) >> 8;

            /*
             * Dither once, then fill.
             *
             * This used to perturb and re-pack every output pixel, which
             * put a table lookup and three clamps inside the upscale and
             * cost more than the simulation it was upscaling -- the whole
             * animation ran at 15 fps under emulation instead of 24.
             * Dithering the simulated pixel instead scatters the same
             * rounding error over a slightly coarser grid, which the eye
             * cannot tell apart on a gradient this soft, and leaves the
             * inner loop as stores.
             */
            const int32_t d = ba_bayer[((y & 3) << 2) | (x & 3)];
            const uint32_t px = ((uint32_t)ba_clampi(r + d, 0, 255) << 16) |
                                ((uint32_t)ba_clampi(g + d, 0, 255) << 8) |
                                 (uint32_t)ba_clampi(b + d, 0, 255);

            uint32_t *row = dst + (size_t)(ba_offy + y * sc) * (size_t)stride
                                + ba_offx + x * sc;
            for (int dy = 0; dy < sc; dy++, row += stride)
                for (int dx = 0; dx < sc; dx++) row[dx] = px;
        }
    }
}

#endif /* BOOTANIM_H */
