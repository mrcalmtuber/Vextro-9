#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "font.h"

/*
 * Shared UI theme + software drawing primitives for the Socrates desktop.
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
 * The falloff is a shift and nothing else: ring d is drawn at
 * GFX_SHADOW_A >> d, so the alpha halves every pixel outward -- 52, 26,
 * 13, 6, 3, 1 -- which is the shape a real penumbra has anyway and costs
 * one shift per ring rather than a multiply per pixel.
 *
 * Rings, not a filled rectangle. A filled shadow blends width x height
 * pixels of which the window then covers all but a thin L, so nearly all
 * of that work is thrown away; the rings cover exactly the band that
 * shows. The offset is larger downward than sideways, because a light
 * that is above casts further below.
 */
#define GFX_SHADOW_R  6      /* how far the penumbra reaches   */
#define GFX_SHADOW_A  104    /* alpha at the shadow's own edge */
#define GFX_SHADOW_DX 4      /* offset out                     */
#define GFX_SHADOW_DY 6      /* and, further, down             */

static void gfx_rect_blend(uint32_t *buf, uint32_t bw, uint32_t bh,
                           int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color, uint32_t alpha);

static void gfx_shadow(uint32_t *buf, uint32_t bw, uint32_t bh,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;

    const int32_t sx = x + GFX_SHADOW_DX;
    const int32_t sy = y + GFX_SHADOW_DY;

    for (int32_t d = GFX_SHADOW_R; d >= 0; d--) {
        const uint32_t a = (uint32_t)GFX_SHADOW_A >> d;
        if (!a) continue;

        const int32_t rx = sx - d, ry = sy - d;
        const int32_t rw = w + 2 * d, rh = h + 2 * d;

        /* The ring only, one pixel thick on each side. */
        gfx_rect_blend(buf, bw, bh, rx, ry, rw, 1, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, rx, ry + rh - 1, rw, 1, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, rx, ry + 1, 1, rh - 2, 0x000000u, a);
        gfx_rect_blend(buf, bw, bh, rx + rw - 1, ry + 1, 1, rh - 2, 0x000000u, a);
    }
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

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

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

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, int max) {
    int len = str_len(dst);
    int i = 0;
    while (src[i] && len < max - 1) { dst[len++] = src[i++]; }
    dst[len] = '\0';
}

static void uint_to_str(uint32_t val, char *out) {
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = (char)('0' + val % 10); val /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
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
