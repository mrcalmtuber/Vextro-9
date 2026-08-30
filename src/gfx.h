#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "font.h"
#include "kernel_shared.h"   /* str_*, uint_to_str — moved out of here */

/*
 * Shared UI theme + software drawing primitives for the Vextro desktop.
 * All colors are 0xRRGGBB in a 32-bit XRGB backbuffer.
 */

/* ===== THEME PALETTE ===== */

#define C_GOLD       0xD4AF37u   /* brand accent            */
#define C_GOLD_DIM   0x8A742Au   /* muted accent            */
#define C_BG_PANEL   0x14161Eu   /* menubar / dock plates   */
#define C_TITLE_FOC  0x232838u   /* focused titlebar        */
#define C_TITLE_UNF  0x181B26u   /* unfocused titlebar      */
#define C_BORDER_UNF 0x3A4050u   /* unfocused window border */
#define C_WIN_BG     0xF2F2F5u   /* light app content bg    */
#define C_TEXT       0xE8E8F0u   /* light text on dark      */
#define C_TEXT_DIM   0x9098A8u   /* secondary text          */
#define C_INK        0x20242Cu   /* dark text on light      */
#define C_TERM_BG    0x0B0D13u   /* terminal canvas         */
#define C_TERM_FG    0xD5DAE5u   /* terminal default fg     */
#define C_RED        0xE05252u
#define C_GREEN      0x4FC87Au
#define C_BLUE       0x5090E0u
#define C_LINK       0x8A6D1Fu   /* link text on light bg   */

/* ===== DROP SHADOW =====
 *
 * Windows sat on the wallpaper with a one-pixel border and nothing else,
 * so a focused window and the thing behind it were the same distance
 * away. This is what puts them at different distances.
 *
 * ---- the falloff ----
 *
 * It used to be a shift: ring d at alpha >> d, which gives 104, 52, 26,
 * 13, 6, 3, 1 and is gone to nothing three pixels out. That was not a
 * judgement about how shadows look; it was the only smooth-ish curve
 * available to a kernel with no floating point.
 *
 * A real penumbra is a blurred step edge, which is an error function,
 * which to the eye is a smoothstep. That is what the table below holds,
 * computed once on first use: 1 - (3t^2 - 2t^3) across the radius, so
 * the shadow stays dark through its first third and then fades over the
 * rest instead of collapsing immediately.
 *
 * ---- and the corners ----
 *
 * Drawing rings gives a shadow with square corners, which nothing in the
 * world has: light wrapping past a corner is attenuated in two
 * directions at once. The four corner blocks are therefore drawn per
 * pixel with the distance taken radially, so the shadow is genuinely
 * rounded. That is four times the radius squared of per-pixel work --
 * a few hundred pixels -- against a frame of a million.
 *
 * The straight edges stay as strips, for the reason they always were:
 * filling the whole rectangle would blend width x height pixels of which
 * the window covers all but a thin band. The offset is larger downward
 * than sideways, because a light that is above casts further below.
 */
#define GFX_SHADOW_R  10     /* how far the penumbra reaches   */
#define GFX_SHADOW_A  120    /* alpha at the shadow's own edge */
#define GFX_SHADOW_DX 3      /* offset out                     */
#define GFX_SHADOW_DY 8      /* and, further, down             */
#define GFX_SHADOW_MAXR 24

static void gfx_rect_blend(uint32_t *buf, uint32_t bw, uint32_t bh,
                           int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color, uint32_t alpha);
static inline uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t alpha);

/*
 * gfx_fall[r][d] is the *profile* at distance d for a shadow of radius
 * r: the falloff normalised to a peak of 255, filled in the first time
 * that radius is asked for and scaled by the caller's own peak at use.
 *
 * Storing it already scaled would be one multiply cheaper and wrong. The
 * animation code asks for the same radius at a peak that changes every
 * frame as a window fades, and a table cached on radius alone would hand
 * back whatever alpha the first caller happened to want -- a ghost that
 * fades away while its shadow stays exactly as dark as it started.
 */
static uint8_t gfx_fall[GFX_SHADOW_MAXR + 1][GFX_SHADOW_MAXR + 2];
static uint8_t gfx_fall_ready[GFX_SHADOW_MAXR + 1];

static void gfx_shadow_table(int r) {
    if (gfx_fall_ready[r]) return;
    for (int d = 0; d <= r + 1; d++) {
        float t = (float)d / (float)(r + 1);
        if (t > 1.0f) t = 1.0f;
        float s = 1.0f - (3.0f * t * t - 2.0f * t * t * t);
        int   a = (int)(255.0f * s + 0.5f);
        gfx_fall[r][d] = (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
    }
    gfx_fall_ready[r] = 1;
}

/*
 * A soft shadow under an arbitrary rectangle.
 *
 * Windows, menus, the dock, jump lists, the notification panel: anything
 * that floats over something else. A menu drawn without one reads as
 * painted onto the desktop rather than held above it, and until now
 * every one of them was.
 */
static void gfx_shadow_rect(uint32_t *buf, uint32_t bw, uint32_t bh,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            int radius, int peak, int32_t dx, int32_t dy) {
    if (w <= 0 || h <= 0) return;
    if (radius < 1) radius = 1;
    if (radius > GFX_SHADOW_MAXR) radius = GFX_SHADOW_MAXR;
    if (peak < 0) peak = 0;
    if (peak > 255) peak = 255;
    gfx_shadow_table(radius);
    const uint8_t *fall = gfx_fall[radius];

    const int32_t sx = x + dx, sy = y + dy;
    const int32_t rx = sx + w, ry = sy + h;      /* one past the edges */

    /* Straight edges: one strip per distance, four sides. */
    for (int d = 1; d <= radius; d++) {
        uint32_t a = (uint32_t)fall[d] * (uint32_t)peak / 255u;
        if (!a) continue;
        gfx_rect_blend(buf, bw, bh, sx, sy - d,      w, 1, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, sx, ry + d - 1,  w, 1, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, sx - d,     sy,  1, h, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, rx + d - 1, sy,  1, h, 0x000000u, a);
    }

    /*
     * Corners, per pixel and radially. (cx, cy) is the corner the
     * distance is measured from and (ox, oy) the direction the block
     * extends in, so one loop covers all four.
     */
    const int32_t cx[4] = { sx,     rx - 1, sx,     rx - 1 };
    const int32_t cy[4] = { sy,     sy,     ry - 1, ry - 1 };
    const int32_t ox[4] = { -1,      1,     -1,      1 };
    const int32_t oy[4] = { -1,     -1,      1,      1 };

    for (int c = 0; c < 4; c++) {
        for (int j = 1; j <= radius; j++) {
            int32_t py = cy[c] + oy[c] * j;
            if (py < 0 || py >= (int32_t)bh) continue;
            uint32_t *row = buf + (uint32_t)py * bw;
            for (int i = 1; i <= radius; i++) {
                int d = (int)(__builtin_sqrtf((float)(i * i + j * j)) + 0.5f);
                if (d > radius) continue;
                uint32_t a = (uint32_t)fall[d] * (uint32_t)peak / 255u;
                if (!a) continue;
                int32_t px = cx[c] + ox[c] * i;
                if (px < 0 || px >= (int32_t)bw) continue;
                row[px] = gfx_mix(0x000000u, row[px], a);
            }
        }
    }
}

/* What a window gets. */
static void gfx_shadow(uint32_t *buf, uint32_t bw, uint32_t bh,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_shadow_rect(buf, bw, bh, x, y, w, h,
                    GFX_SHADOW_R, GFX_SHADOW_A, GFX_SHADOW_DX, GFX_SHADOW_DY);
}

/* Tighter and closer, for the things that hover just above the surface
 * rather than well over it: menus, jump lists, the dock, popups. */
static void gfx_shadow_popup(uint32_t *buf, uint32_t bw, uint32_t bh,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_shadow_rect(buf, bw, bh, x, y, w, h, 7, 100, 0, 3);
}

/* ===== MOTION =====
 *
 * A critically damped spring, integrated one frame at a time.
 *
 * There was no motion here at all beyond a linear ramp over twelve
 * frames, because a linear ramp is what integers can express. It reads
 * as mechanical for a reason that is easy to state: real things do not
 * start and stop instantly, and a constant velocity means infinite
 * acceleration at both ends.
 *
 * This is not an easing curve looked up from a table — it is the
 * differential equation, stepped. Position accelerates toward the target
 * in proportion to the distance remaining and is damped in proportion to
 * its own speed, at exactly the damping that reaches the target as fast
 * as possible without going past it. The result is fast at first, slow
 * at the end, and never overshoots.
 *
 * k is the stiffness. 220 settles in roughly a third of a second at
 * 60 Hz, which is about as long as an interface can take before it
 * feels slow and about as short as it can take before it feels abrupt.
 */
typedef struct { float p, v; } spring_t;

#define SPRING_K   220.0f
#define SPRING_DT  (1.0f / 60.0f)

static inline void spring_step(spring_t *s, float target) {
    const float c = 2.0f * __builtin_sqrtf(SPRING_K);   /* critical */
    float a = -SPRING_K * (s->p - target) - c * s->v;
    s->v += a * SPRING_DT;
    s->p += s->v * SPRING_DT;
}

static inline int spring_settled(const spring_t *s, float target) {
    float d = s->p - target;  if (d < 0.0f) d = -d;
    float v = s->v;           if (v < 0.0f) v = -v;
    return d < 0.004f && v < 0.05f;
}

static inline float gfx_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/* ===== BASIC PRIMITIVES ===== */

static void gfx_rect(uint32_t *buf, uint32_t bw, uint32_t bh,
                     int32_t x, int32_t y, int32_t w, int32_t h,
                     uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w; if (x1 > (int32_t)bw) x1 = (int32_t)bw;
    int32_t y1 = y + h; if (y1 > (int32_t)bh) y1 = (int32_t)bh;
    for (int32_t r = y0; r < y1; r++)
        for (int32_t c = x0; c < x1; c++)
            buf[(uint32_t)r * bw + (uint32_t)c] = color;
}

static void gfx_rect_outline(uint32_t *buf, uint32_t bw, uint32_t bh,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color) {
    gfx_rect(buf, bw, bh, x, y, w, 1, color);
    gfx_rect(buf, bw, bh, x, y + h - 1, w, 1, color);
    gfx_rect(buf, bw, bh, x, y, 1, h, color);
    gfx_rect(buf, bw, bh, x + w - 1, y, 1, h, color);
}

/*
 * Interpolate two XRGB pixels.
 *
 * The motivation is narrower than it looks, and worth stating correctly.
 * The obvious spelling divides each channel by 255, and a divide reads
 * like the thing to remove -- but at -O2 the compiler already turns a
 * division by a constant into a reciprocal multiply and a shift, so
 * there was never a division instruction here to delete. Benchmarked in
 * isolation on a modern out-of-order core the two spellings are the same
 * speed to within measurement noise.
 *
 * What this actually saves is work per channel. Red and blue sit in
 * 0x00FF00FF with a byte of space between them, which absorbs the carry,
 * so one multiply interpolates both and green goes alongside in its own
 * lane: two multiplies and two fixups instead of three of each, plus no
 * unpacking and repacking of individual bytes. Fewer instructions, not
 * cheaper ones.
 *
 * Measured where it is actually spent: the desktop composite went from
 * 8,840k to 8,774k cycles, about 0.8%. Small, because blending glyph
 * coverage is a small share of a frame that also copies a wallpaper --
 * and quoted rather than rounded up, because a 0.8% win described as a
 * fast path is how a codebase accumulates folklore.
 *
 * The rounding is a real improvement though: (x + 128 + (x >> 8)) >> 8
 * rounds where the truncating divide floored. Checked exhaustively
 * against the exact value over all 16,777,216 channel triples -- never
 * off by more than one, and closer on average than what it replaced.
 */
static inline uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t alpha /*0..255*/) {
    const uint32_t ia = 255u - alpha;
    uint32_t rb = (a & 0x00FF00FFu) * alpha + (b & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g  = (a & 0x0000FF00u) * alpha + (b & 0x0000FF00u) * ia + 0x00008000u;
    g  = ((g  + ((g  >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
    return rb | g;
}

/* Blend a translucent rectangle over the existing pixels */
static void gfx_rect_blend(uint32_t *buf, uint32_t bw, uint32_t bh,
                           int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color, uint32_t alpha) {
    if (w <= 0 || h <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w; if (x1 > (int32_t)bw) x1 = (int32_t)bw;
    int32_t y1 = y + h; if (y1 > (int32_t)bh) y1 = (int32_t)bh;
    for (int32_t r = y0; r < y1; r++)
        for (int32_t c = x0; c < x1; c++) {
            uint32_t idx = (uint32_t)r * bw + (uint32_t)c;
            buf[idx] = gfx_mix(color, buf[idx], alpha);
        }
}

/*
 * Blend one buffer over another inside a rectangle, both the same stride.
 *
 * gfx_rect_blend fades towards a single colour, which is no use for Peek:
 * fading a window out has to reveal the wallpaper *behind that window*,
 * which is a different pixel at every position. Same arithmetic, source
 * read per pixel instead of held in a register.
 *
 * alpha is how much of src shows through, so 0 leaves buf untouched and
 * the caller can ramp without special-casing either end.
 */
static void gfx_blend_region(uint32_t *buf, const uint32_t *src,
                             uint32_t bw, uint32_t bh,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t alpha) {
    if (w <= 0 || h <= 0 || alpha == 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w; if (x1 > (int32_t)bw) x1 = (int32_t)bw;
    int32_t y1 = y + h; if (y1 > (int32_t)bh) y1 = (int32_t)bh;
    for (int32_t r = y0; r < y1; r++) {
        const uint32_t row = (uint32_t)r * bw;
        for (int32_t c = x0; c < x1; c++) {
            const uint32_t i = row + (uint32_t)c;
            buf[i] = gfx_mix(src[i], buf[i], alpha);
        }
    }
}

/*
 * Nearest-neighbour downscale of a screen rectangle into a small buffer.
 *
 * This is how the taskbar gets its previews. It samples the frame that is
 * already composited rather than re-rendering the window offscreen: at the
 * moment a window has just been drawn in the z-order walk, its pixels in
 * the back buffer are exactly that window and nothing on top of it yet, so
 * the cheapest correct capture is a read.
 *
 * The step is a 16.16 fixed-point accumulator -- one add per output pixel,
 * no division in the loop, and no FPU anywhere near it.
 */
static void gfx_downscale(uint32_t *dst, int32_t dw, int32_t dh,
                          const uint32_t *src, uint32_t sw, uint32_t sh,
                          int32_t sx, int32_t sy, int32_t srw, int32_t srh) {
    if (dw <= 0 || dh <= 0 || srw <= 0 || srh <= 0) return;
    const uint32_t stepx = ((uint32_t)srw << 16) / (uint32_t)dw;
    const uint32_t stepy = ((uint32_t)srh << 16) / (uint32_t)dh;
    uint32_t accy = 0;
    for (int32_t r = 0; r < dh; r++, accy += stepy) {
        int32_t yy = sy + (int32_t)(accy >> 16);
        if (yy < 0) yy = 0;
        if (yy >= (int32_t)sh) yy = (int32_t)sh - 1;
        const uint32_t row = (uint32_t)yy * sw;
        uint32_t accx = 0;
        for (int32_t c = 0; c < dw; c++, accx += stepx) {
            int32_t xx = sx + (int32_t)(accx >> 16);
            if (xx < 0) xx = 0;
            if (xx >= (int32_t)sw) xx = (int32_t)sw - 1;
            dst[(uint32_t)r * (uint32_t)dw + (uint32_t)c] = src[row + (uint32_t)xx];
        }
    }
}

/* Vertical gradient fill */
static void gfx_vgrad(uint32_t *buf, uint32_t bw, uint32_t bh,
                      int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t top, uint32_t bottom) {
    if (h <= 0) return;
    for (int32_t r = 0; r < h; r++) {
        int32_t yy = y + r;
        if (yy < 0 || yy >= (int32_t)bh) continue;
        uint32_t t = (uint32_t)(r * 255 / (h > 1 ? h - 1 : 1));
        uint32_t col = gfx_mix(bottom, top, t);
        int32_t x0 = x < 0 ? 0 : x;
        int32_t x1 = x + w; if (x1 > (int32_t)bw) x1 = (int32_t)bw;
        for (int32_t c = x0; c < x1; c++)
            buf[(uint32_t)yy * bw + (uint32_t)c] = col;
    }
}

/* Filled circle (for dock icons / buttons) */
static void gfx_circle(uint32_t *buf, uint32_t bw, uint32_t bh,
                       int32_t cx, int32_t cy, int32_t rad, uint32_t color) {
    for (int32_t dy = -rad; dy <= rad; dy++)
        for (int32_t dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy > rad * rad) continue;
            int32_t px = cx + dx, py = cy + dy;
            if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
                buf[(uint32_t)py * bw + (uint32_t)px] = color;
        }
}

static void gfx_circle_outline(uint32_t *buf, uint32_t bw, uint32_t bh,
                               int32_t cx, int32_t cy, int32_t rad,
                               uint32_t color) {
    for (int32_t dy = -rad; dy <= rad; dy++)
        for (int32_t dx = -rad; dx <= rad; dx++) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > rad * rad || d2 < (rad - 1) * (rad - 1)) continue;
            int32_t px = cx + dx, py = cy + dy;
            if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
                buf[(uint32_t)py * bw + (uint32_t)px] = color;
        }
}

static void gfx_tri(uint32_t *buf, uint32_t bw, uint32_t bh,
                    int x0, int y0, int x1, int y1, int x2, int y2,
                    uint32_t color) {
    int tmp;
    if (y0 > y1) { tmp=x0;x0=x1;x1=tmp; tmp=y0;y0=y1;y1=tmp; }
    if (y0 > y2) { tmp=x0;x0=x2;x2=tmp; tmp=y0;y0=y2;y2=tmp; }
    if (y1 > y2) { tmp=x1;x1=x2;x2=tmp; tmp=y1;y1=y2;y2=tmp; }
    if (y2 == y0) return;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= (int)bh) continue;
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1)
            xb = (y1 == y0) ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else
            xb = (y2 == y1) ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { tmp = xa; xa = xb; xb = tmp; }
        if (xa < 0) xa = 0;
        if (xb >= (int)bw) xb = (int)bw - 1;
        for (int x = xa; x <= xb; x++)
            buf[y * (int)bw + x] = color;
    }
}

static void gfx_line(uint32_t *buf, uint32_t bw, uint32_t bh,
                     int x0, int y0, int x1, int y1,
                     int thick, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int half = thick / 2;
    for (;;) {
        for (int ry = -half; ry <= half; ry++)
            for (int rx = -half; rx <= half; rx++) {
                int px = x0 + rx, py = y0 + ry;
                if (px >= 0 && px < (int)bw && py >= 0 && py < (int)bh)
                    buf[py * (int)bw + px] = color;
            }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ===== MONOSPACE BITMAP TEXT (8x8 core font, integer scale) =====
 * Pixel-perfect grid rendering for the terminal — every glyph occupies
 * exactly MONO_ADV(scale) horizontal pixels, so columns always line up. */

#define MONO_ADV(s)  (8 * (s))

static void mono_char(uint32_t *buf, uint32_t bw, uint32_t bh,
                      int32_t x, int32_t y, char ch, uint32_t color, int s) {
    if (ch < 0x20 || ch > 0x7E) return;
    const uint8_t *glyph = font8x8[ch - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            for (int ss = 0; ss < s; ss++)
                for (int tt = 0; tt < s; tt++) {
                    int32_t px = x + col * s + tt;
                    int32_t py = y + row * s + ss;
                    if (px >= 0 && px < (int32_t)bw &&
                        py >= 0 && py < (int32_t)bh)
                        buf[(uint32_t)py * bw + (uint32_t)px] = color;
                }
        }
    }
}

static void mono_text(uint32_t *buf, uint32_t bw, uint32_t bh,
                      int32_t x, int32_t y, const char *s,
                      uint32_t color, int scale) {
    for (; *s; s++) {
        mono_char(buf, bw, bh, x, y, *s, color, scale);
        x += MONO_ADV(scale);
    }
}

/* ===== SMALL STRING HELPERS (freestanding) ===== */

/* str_len, str_eq, str_starts_with, str_copy, str_append and
 * uint_to_str moved to include/kernel_shared.h, as `static inline`.
 * They are pure functions and three translation units need them; they
 * live at the seam rather than being copied into each. */


/*
 * Real time, counted by the PIT at ~60 Hz.
 *
 * Anything that wants "twice a second" has to key off this rather than a
 * frame counter: a frame is not a fixed amount of time, and during a
 * heavy background load the desktop drops to a few frames a second — at
 * which point a 30-frame interval is ten seconds, and the clock visibly
 * stops.
 */
static volatile uint32_t sys_ticks = 0;

static char chr_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char chr_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Bytewise compare, unsigned — the order archives are sorted in. */
static int str_cmp_bytes(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}





/* ===== CMOS RTC ===== */

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static inline uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static void rtc_read(int *hh, int *mm, int *ss, int *day, int *mon, int *yr) {
    uint8_t statusB = cmos_read(0x0B);
    int bin = statusB & 0x04;

    uint8_t h = cmos_read(0x04), m = cmos_read(0x02), s = cmos_read(0x00);
    uint8_t d = cmos_read(0x07), mo = cmos_read(0x08), y = cmos_read(0x09);

    if (!bin) {
        h = bcd_to_bin(h); m = bcd_to_bin(m); s = bcd_to_bin(s);
        d = bcd_to_bin(d); mo = bcd_to_bin(mo); y = bcd_to_bin(y);
    }
    if (hh) *hh = h;
    if (mm) *mm = m;
    if (ss) *ss = s;
    if (day) *day = d;
    if (mon) *mon = mo;
    if (yr)  *yr = 2000 + y;
}

/*
 * The same reading as a count of seconds since 1970-01-01.
 *
 * ---- why this is here and not in ring 3 ----
 *
 * rtc_read talks to the CMOS through ports 0x70 and 0x71, which ring 3
 * cannot reach and should not be given. Until this function existed the
 * consequence reached all the way up: libc's time() answered from the
 * monotonic tick, so a program that formatted "now" printed a date a few
 * seconds after the start of 1970. That was documented rather than
 * fixed, because nothing needed a calendar. ICU does — a date formatter
 * whose "now" is wrong is a date formatter that cannot be checked
 * against anything.
 *
 * ---- what it is not ----
 *
 * Not UTC, necessarily. The CMOS clock holds whatever the firmware put
 * in it, which on a machine configured for local time is local time,
 * and there is nowhere in the hardware that records which. Treating it
 * as UTC is the only choice available and it is the one every simple
 * system makes; the error, where there is one, is a whole number of
 * hours and is the same every boot.
 *
 * ---- and the conversion ----
 *
 * Days from a civil date by Howard Hinnant's method: shift the year so
 * that it begins in March, which puts the leap day at the end where it
 * cannot disturb the month-length arithmetic, and the whole thing
 * becomes four multiplications with no table and no loop. Correct for
 * every date in the proleptic Gregorian calendar, which is a much larger
 * range than a CMOS clock can express.
 */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t rtc_unix_seconds(void) {
    int hh = 0, mm = 0, ss = 0, d = 1, mo = 1, yr = 1970;
    rtc_read(&hh, &mm, &ss, &d, &mo, &yr);

    /* A clock that has never been set, or one read while the chip was
     * mid-update, can produce a date outside anything sensible. Reporting
     * zero is better than reporting a confident wrong century: a caller
     * that sees zero knows the clock is unset, which is exactly what
     * "the epoch" has always meant. */
    if (yr < 1970 || yr > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60)
        return 0;

    return days_from_civil(yr, (unsigned)mo, (unsigned)d) * 86400
           + (int64_t)hh * 3600 + (int64_t)mm * 60 + ss;
}

static const char *month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void clock_string(char *out) {
    int hh, mm, ss;
    rtc_read(&hh, &mm, &ss, 0, 0, 0);
    out[0] = (char)('0' + hh / 10);
    out[1] = (char)('0' + hh % 10);
    out[2] = ':';
    out[3] = (char)('0' + mm / 10);
    out[4] = (char)('0' + mm % 10);
    out[5] = ':';
    out[6] = (char)('0' + ss / 10);
    out[7] = (char)('0' + ss % 10);
    out[8] = '\0';
}

static void date_string(char *out /* >= 16 */) {
    int d, mo, yr;
    rtc_read(0, 0, 0, &d, &mo, &yr);
    if (mo < 1) mo = 1;
    if (mo > 12) mo = 12;
    const char *mn = month_names[mo - 1];
    int p = 0;
    out[p++] = mn[0]; out[p++] = mn[1]; out[p++] = mn[2];
    out[p++] = ' ';
    if (d >= 10) out[p++] = (char)('0' + d / 10);
    out[p++] = (char)('0' + d % 10);
    out[p++] = ' ';
    char yb[8];
    uint_to_str((uint32_t)yr, yb);
    for (int i = 0; yb[i]; i++) out[p++] = yb[i];
    out[p] = '\0';
}

/*
 * Set by anything that writes to the panel behind the compositor's back
 * — the iGPU blit test is the only one today.  The flip skips rows that
 * match the previously presented frame, and a direct write leaves it
 * believing a row is still on screen when something else has overwritten
 * it, so such a writer has to say so.
 */
static int gfx_force_full_flip = 0;

/* ===== TINY PSEUDO-RNG (for matrix rain etc.) ===== */

static uint32_t gfx_rng_state = 0x53525431u;

static uint32_t gfx_rand(void) {
    gfx_rng_state = gfx_rng_state * 1664525u + 1013904223u;
    return gfx_rng_state >> 8;
}

#endif /* GFX_H */
