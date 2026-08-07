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
 * The dragon off the desktop wallpaper draws breath and sets fire to the
 * screen, and the screen burns away to nothing, and then you log in.
 *
 * It is the same dragon. wall_dragon() takes a centre and a scale now, so
 * the wallpaper draws it full size in the middle and this draws it half
 * size and left of centre, from one set of polygons. The alternative --
 * a second dragon that has to be kept in step with the first -- is how
 * two drawings of the same thing slowly stop being the same thing.
 *
 * Two coupled fields do the work, and they are deliberately different
 * mechanisms because they are different phenomena:
 *
 *   FIRE is advected. Each cell pulls heat from the cell to its left and
 *   the cells below, so heat streams away from the mouth and rises, and a
 *   little noise per cell keeps the flame from looking laminar. The mix
 *   between "left" and "below" shifts over the sequence: the jet leaves
 *   the mouth almost flat, and once the breath stops what is left of it
 *   turns upward and gutters out, which is what fire does.
 *
 *   THE BURN is a front. Fire is not what destroys the screen -- the
 *   thing fire leaves behind is -- so the burn is a separate field that
 *   ignites where the jet lands and then eats outward on its own, with a
 *   bright ember rim and cold char behind it. It spreads in a distance
 *   metric squashed along x, so it runs ahead of itself in the direction
 *   the breath went.
 *
 * Neither needs a square root. The burn front compares squared distance
 * against a squared radius, perturbed per cell by a tiled noise field,
 * which is what makes its edge ragged instead of a circle -- a perfect
 * circle expanding out of a dragon's mouth reads as a shockwave, not as
 * something catching light.
 *
 * ---- constraints ----
 *
 * Integer only, like everything else in this kernel: the 360-entry sine
 * table at 1024 scale is the only source of curves, and every divide by a
 * constant is a shift. This runs before fpu_init(), before the IDT exists
 * and before any driver has been probed, so it can depend on nothing but
 * the framebuffer it is given.
 *
 * Include after desktop.h -- it borrows the dragon -- and after ttf.h,
 * because the wordmark is set in the system's own face.
 */

#define BA_W      640          /* simulation grid */
#define BA_H      400
#define BA_FRAMES 120          /* 5 seconds at 24 fps */

#define BA_GOLD_R 0xD4         /* C_GOLD, spelled out: this has to work  */
#define BA_GOLD_G 0xAF         /* whatever else has or has not been      */
#define BA_GOLD_B 0x37         /* included by the time it is used        */

/* The scene before anything happens to it: dragon, wordmark, ground. */
static uint32_t ba_bg[BA_W * BA_H];

/* Fire, and what fire leaves. 0 in ba_burn means untouched; otherwise it
 * counts down from ignition, so one byte carries both "is it burnt" and
 * "how recently", which is what the ember rim is drawn from. */
static uint8_t ba_heat[BA_W * BA_H];
static uint8_t ba_burn[BA_W * BA_H];

/* Ragged edge for the burn front. Tiled rather than per-pixel: 16 KB
 * against 256 KB, and at 128 across the repeat never lands twice inside
 * one flame. It has to be stable frame to frame or the edge boils. */
#define BA_NOISE 128
static uint8_t ba_noise[BA_NOISE * BA_NOISE];

/* Heat to colour. Black, through the reds, into orange, yellow and white,
 * built once because a 256-entry table is cheaper than deciding this per
 * pixel per frame and far easier to tune. */
static uint32_t ba_pal[256];

static int ba_scale = 1;              /* integer upscale to the panel */
static int ba_offx = 0, ba_offy = 0;  /* centring                     */
static int ba_mx = 0, ba_my = 0;      /* the mouth: origin of all of it */

static uint32_t ba_rng = 0x1BADB002u;
static inline uint32_t ba_rand(void) {
    ba_rng ^= ba_rng << 13;
    ba_rng ^= ba_rng >> 17;
    ba_rng ^= ba_rng << 5;
    return ba_rng;
}

static inline int32_t ba_clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ===== the scene ===== */

static void ba_build_palette(void) {
    for (int i = 0; i < 256; i++) {
        int32_t r, g, b;
        if (i < 32)        { r = i * 3;                  g = 0;               b = 0; }
        else if (i < 96)   { r = 96 + (i - 32) * 159 / 64;  g = (i - 32) * 40 / 64;  b = 0; }
        else if (i < 168)  { r = 255; g = 40 + (i - 96) * 150 / 72;  b = (i - 96) * 20 / 72; }
        else if (i < 216)  { r = 255; g = 190 + (i - 168) * 65 / 48; b = 20 + (i - 168) * 60 / 48; }
        else               { r = 255; g = 255; b = 80 + (i - 216) * 175 / 40; }
        ba_pal[i] = ((uint32_t)ba_clampi(r, 0, 255) << 16) |
                    ((uint32_t)ba_clampi(g, 0, 255) << 8) |
                     (uint32_t)ba_clampi(b, 0, 255);
    }
}

static void ba_draw_scene(void) {
    /* A cold ground, darkest at the top, so the fire has something to be
     * warmer than. */
    for (int y = 0; y < BA_H; y++) {
        const int32_t t = (y * 255) / (BA_H - 1);
        const uint32_t c = ((uint32_t)(3 + ((t * 7) >> 8)) << 16) |
                           ((uint32_t)(4 + ((t * 9) >> 8)) << 8) |
                            (uint32_t)(7 + ((t * 14) >> 8));
        for (int x = 0; x < BA_W; x++) ba_bg[y * BA_W + x] = c;
    }

    /*
     * The dragon, half size and left of centre. Left because the head is
     * on the right of the drawing and everything that happens next comes
     * out of its mouth, so it needs room in front of it rather than
     * behind.
     */
    const int cx = BA_W * 42 / 100;
    const int cy = BA_H * 52 / 100;
    wall_dragon(ba_bg, BA_W, BA_H, cx, cy, 1, 2,
                0x2C3752u, 0x4A5C82u,
                ((uint32_t)BA_GOLD_R << 16) | ((uint32_t)BA_GOLD_G << 8) | BA_GOLD_B);

    /* Mouth: the head's leading point in wall_dragon is (+400,-30) before
     * scaling, and the jaw runs back from there. */
    ba_mx = cx + 200;
    ba_my = cy - 12;

    /* The wordmark, in the face the system draws itself in, low enough
     * that the burn reaches it late. */
    {
        const char *s = "SOCRATES BSD 9";
        const int size = 26;
        const int tw = ttf_text_width(s, size);
        ttf_draw_string(ba_bg, BA_W, BA_H, (BA_W - tw) / 2, BA_H - 78, s,
                        ((uint32_t)BA_GOLD_R << 16) |
                        ((uint32_t)BA_GOLD_G << 8) | BA_GOLD_B, size);
    }
}

static void ba_init(int dst_w, int dst_h) {
    ba_scale = dst_w / BA_W;
    const int sy = dst_h / BA_H;
    if (sy < ba_scale) ba_scale = sy;
    if (ba_scale < 1) ba_scale = 1;    /* smaller panel: clip, do not skip */

    ba_offx = (dst_w - BA_W * ba_scale) / 2;
    ba_offy = (dst_h - BA_H * ba_scale) / 2;
    if (ba_offx < 0) ba_offx = 0;
    if (ba_offy < 0) ba_offy = 0;

    for (int i = 0; i < BA_W * BA_H; i++) { ba_heat[i] = 0; ba_burn[i] = 0; }
    for (int i = 0; i < BA_NOISE * BA_NOISE; i++) ba_noise[i] = (uint8_t)ba_rand();

    ba_build_palette();
    ba_draw_scene();
}

/* ===== the fire ===== */

/*
 * One step of advection.
 *
 * x descends and y ascends, and every read is from column x-1 or from row
 * y+1 -- both of which this pass has not reached yet. So the whole grid
 * is updated from the previous frame's values without a second buffer,
 * which is 256 KB and a copy per frame saved for the price of iterating
 * in the one order that makes it safe.
 */
static void ba_fire_step(int frame) {
    /* Early the jet is flat and fast; once the breath stops what is left
     * of it stands up and rises. */
    int32_t rise = frame < 40 ? 1 : (frame < 76 ? 3 : 6);
    int32_t cool = frame < 76 ? 2 : 6;

    for (int x = BA_W - 1; x >= 1; x--) {
        for (int y = 0; y < BA_H; y++) {
            const int i = y * BA_W + x;

            int32_t acc = (int32_t)ba_heat[i - 1] * (8 - rise);
            if (y > 0)          acc += (int32_t)ba_heat[i - 1 - BA_W];
            if (y < BA_H - 1)   acc += (int32_t)ba_heat[i - 1 + BA_W];
            if (y < BA_H - 1)   acc += (int32_t)ba_heat[i + BA_W] * rise;

            int32_t v = acc / 10 - cool;

            /* Flicker, scaled by how hot the cell already is: cold air
             * does not shimmer, and applying it flat makes a grey haze
             * over the whole screen instead of a flame. */
            if (v > 0) v -= (int32_t)((ba_rand() >> 24) * (uint32_t)v) >> 11;

            ba_heat[i] = (uint8_t)ba_clampi(v, 0, 255);
        }
    }
}

/*
 * The breath.
 *
 * Advection alone will not carry a jet across a screen: the flicker is
 * multiplicative, so whatever survives one cell survives the next a
 * little less, and the flame dies about forty cells out however hard it
 * is thrown. So the jet is *drawn* -- a cone from the mouth, widening and
 * cooling along its length, its axis wobbling on the sine table -- and
 * advection is left to do what it is good at, which is smearing the edges
 * and lifting the whole thing as it goes.
 *
 * Injected rather than assigned, so the fire the last frame left is not
 * cut away by the cone this frame draws.
 */
static void ba_breathe(int32_t strength, int32_t reach, int32_t phase) {
    if (strength <= 0 || reach <= 0) return;

    for (int32_t t = 0; t < reach; t++) {
        const int x = ba_mx + (int)t;
        if (x < 0) continue;
        if (x >= BA_W) break;

        /* Broader the further it has travelled, and cooler -- but on a
         * curve rather than a ramp. A linear falloff spends most of the
         * jet's length lukewarm and the flame reads as a thin ribbon;
         * holding the temperature and then dropping it late is what makes
         * it a body of fire with a tip. */
        const int32_t spread = 4 + t / 3;
        const int32_t fall   = (t * t) / (reach + 1);
        const int32_t temp   = strength - (fall * strength) / (reach + 1);
        if (temp <= 0) break;

        /* The axis is not a straight line. Amplitude grows with distance,
         * so it leaves the mouth aimed and loses its aim downrange. */
        const int32_t wob = (int_sin[(int)((t * 3 + phase) % 360)] * (t / 4)) >> 10;

        for (int32_t dy = -spread; dy <= spread; dy++) {
            const int y = ba_my + (int)(dy + wob);
            if (y < 0 || y >= BA_H) continue;

            int32_t v = temp - (temp * dy * dy) / (spread * spread + 1);
            v -= (int32_t)(ba_rand() >> 26);
            if (v <= 0) continue;

            const int i = y * BA_W + x;
            if (v > (int32_t)ba_heat[i]) ba_heat[i] = (uint8_t)ba_clampi(v, 0, 255);
        }
    }
}

/* ===== the burn ===== */

/*
 * The front, as a squared radius the whole grid is tested against.
 *
 * Squashing dx by half means the front runs about 1.4x further along the
 * axis the breath went, which is the difference between the screen
 * catching fire from the flame and the screen having a circle drawn on
 * it. R grows linearly so the front moves at a constant speed; R2 is what
 * the test actually uses, because comparing squares costs two multiplies
 * and taking a root costs a loop.
 */
static void ba_burn_step(int frame) {
    if (frame < 34) return;

    const int32_t R  = (frame - 34) * 9;
    const int32_t R2 = R * R;

    for (int y = 0; y < BA_H; y++) {
        const int32_t dy = y - ba_my;
        const int32_t dy2 = dy * dy;
        const uint8_t *nrow = &ba_noise[(y & (BA_NOISE - 1)) * BA_NOISE];

        for (int x = 0; x < BA_W; x++) {
            const int i = y * BA_W + x;
            if (ba_burn[i]) {
                /* Ageing, and the rate is the whole character of it. At
                 * one step a frame the ember outlives the animation and
                 * the burnt screen just glows orange; at fourteen it is
                 * cold char a few frames behind the front, which is
                 * roughly what burning paper does. Never to zero, because
                 * zero means "never burnt" and this pixel is burnt. */
                ba_burn[i] = (uint8_t)(ba_burn[i] > 23 ? ba_burn[i] - 22 : 1);
                continue;
            }

            const int32_t dx = x - ba_mx;
            /*
             * Strongly squashed downrange and not at all behind, so the
             * front travels about two and a half times further the way
             * the breath went. Without that bias it is a circle, and a
             * circle means the screen behind the dragon catches light
             * before the flame in front of it has touched anything --
             * which is the fire and the burn plainly disagreeing about
             * what just happened.
             */
            const int32_t sx = dx > 0 ? (dx * dx) / 6 : (dx * dx);
            const int32_t d2 = sx + dy2;

            /* The ragged edge, as a fraction of the radius rather than a
             * fixed distance -- perturbing d2 by a constant makes the
             * front a torn mess while it is small and a smooth circle
             * once it is large. */
            const int32_t jag = (int32_t)nrow[x & (BA_NOISE - 1)] * (R2 >> 11);

            if (d2 + jag < R2 || ba_heat[i] > 120) ba_burn[i] = 255;
        }
    }
}

/* ===== one frame ===== */

/* 4x4 ordered dither at the panel's resolution. The ground behind the
 * dragon spans a handful of levels over hundreds of rows, which bands
 * visibly on a flat panel; scattering the rounding error removes it. One
 * level is enough -- the gradient steps by one at a time, and the screen
 * is mostly near-black, where a wider spread stops being a smoothing and
 * becomes a visible weave. */
static const int8_t ba_bayer[16] = {
    -1,  1, -1,  1,
     1, -1,  0, -1,
    -1,  1, -1,  0,
     0, -1,  1, -1,
};

/*
 * Render one frame into a 32-bit XRGB buffer.
 *
 * The caller owns presentation: both trees compose into their back buffer
 * and blit, but they pace themselves off different clocks. Everything
 * above that line is the same on both, which is why it lives in one file.
 */
static void ba_render(uint32_t *dst, int stride, int frame) {
    /* The breath: a beat of ignition at the mouth, then the jet, then it
     * stops and the fire has to live on what it was given. */
    int32_t jet = 0, reach = 0;
    if (frame >= 16 && frame < 30) {          /* the intake, at the mouth */
        jet = 70 + (frame - 16) * 13;
        reach = 6 + (frame - 16) * 3;
    } else if (frame >= 30 && frame < 70) {   /* the breath */
        jet = 255;
        reach = 48 + (frame - 30) * 9;
        if (reach > 300) reach = 300;
    } else if (frame >= 70 && frame < 80) {   /* and it stops */
        jet = 255 - (frame - 70) * 25;
        reach = 300 - (frame - 70) * 28;
    }

    ba_breathe(jet, reach, frame * 11);
    ba_fire_step(frame);
    ba_burn_step(frame);

    /* In at the start, out at the end. */
    int32_t fade = 256;
    if (frame < 12) fade = frame * 256 / 12;
    else if (frame >= BA_FRAMES - 20)
        fade = ((BA_FRAMES - 1 - frame) * 256) / 20;

    const int sc = ba_scale;

    for (int y = 0; y < BA_H; y++) {
        for (int x = 0; x < BA_W; x++) {
            const int i = y * BA_W + x;

            const uint32_t c = ba_bg[i];
            int32_t r = (int32_t)((c >> 16) & 0xFF);
            int32_t g = (int32_t)((c >> 8) & 0xFF);
            int32_t b = (int32_t)(c & 0xFF);

            /* What the fire left. 255 is this instant's ignition and 1 is
             * cold char, so the countdown is the ember. */
            const int32_t bn = ba_burn[i];
            if (bn) {
                /* Char first: whatever was here is mostly gone. */
                r >>= 3; g >>= 3; b >>= 3;

                /* Then the rim, which is only the leading edge -- the
                 * top of the count, not the whole of it. */
                if (bn > 170) {
                    const int32_t e = (bn - 170) * 3;
                    r += (e * 235) >> 8;
                    g += (e * 110) >> 8;
                    b += (e *  20) >> 8;
                } else if (bn > 60) {
                    const int32_t e = (bn - 60) * 2;
                    r += (e * 90) >> 8;
                    g += (e * 22) >> 8;
                }
            }

            /* The flame itself, over the top of all of it. */
            const int32_t hv = ba_heat[i];
            if (hv > 8) {
                const uint32_t f = ba_pal[hv];
                r += (int32_t)((f >> 16) & 0xFF);
                g += (int32_t)((f >> 8) & 0xFF);
                b += (int32_t)(f & 0xFF);
            }

            r = (ba_clampi(r, 0, 255) * fade) >> 8;
            g = (ba_clampi(g, 0, 255) * fade) >> 8;
            b = (ba_clampi(b, 0, 255) * fade) >> 8;

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
