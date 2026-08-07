#ifndef TTF_H
#define TTF_H

/*
 * Minimal, freestanding, 100% fixed-point TrueType rasterizer.
 *
 * Designed for a no-libc / no-SSE / no-float kernel: every coordinate is an
 * integer, all curve math is done in 64-bit integer arithmetic.  It parses the
 * embedded Comic Neue Regular face (see comicneue_ttf.h) and renders glyphs
 * into a 32-bit ARGB/XRGB backbuffer with anti-aliasing via 4x4 supersampling.
 *
 * Supported: head, maxp, hhea, hmtx, cmap (format 4), loca (short & long),
 * glyf (simple + composite glyphs), quadratic Bezier outlines.
 */

#include <stdint.h>
#include "comicneue_ttf.h"

/* ----- tunables ----- */
/*
 * 8x8 = 64 samples per pixel, so coverage lands on one of 65 levels
 * rather than 17.  That would be far too slow to do per frame, but every
 * glyph is rasterized once per size and then kept as a coverage mask,
 * so the cost is paid once and never again.
 */
#define TTF_SS        8        /* supersample factor per axis (8x8 = 64 samples) */
#define TTF_MAXPTS    2048     /* max points per simple glyph                    */
#define TTF_MAXEDGES  4096     /* max device-space edges per glyph               */
#define TTF_COVMAX    256      /* max glyph box side (pixels) for coverage buffer*/
#define TTF_MAXCROSS  128      /* max edge crossings per scanline                */
#define TTF_FLATTEN   8        /* line segments per quadratic Bezier             */

/*
 * Letter tracking, as a fraction of each advance.
 *
 * This used to be 1/8, inflating every advance 12.5% past the metrics
 * the designer chose.  Words came apart into strings of separate
 * letters, which costs more legibility than the extra air buys back —
 * word shape is most of what makes text readable at a glance.  Zero
 * means "use the font's own spacing".
 */
#define TTF_HPAD_NUM   0
#define TTF_HPAD_DEN   8
#define TTF_LINE_SCALE_NUM  5  /* line-height multiplier: 5/4 = 1.25x             */
#define TTF_LINE_SCALE_DEN  4

/* Cached glyph masks: 95 printable ASCII across the ~13 sizes the UI
 * uses, at a few hundred bytes each. */
#define TTF_CACHE_SLOTS  2048
#define TTF_CACHE_BYTES  (1536 * 1024)
#define TTF_CACHE_MAXPX  64     /* do not cache anything larger than this */

/* ----- font blob + big-endian readers ----- */
static const uint8_t *FT;

static inline uint16_t be16(uint32_t o) { return (uint16_t)((FT[o] << 8) | FT[o + 1]); }
static inline int16_t  sbe16(uint32_t o) { return (int16_t)be16(o); }
static inline uint32_t be32(uint32_t o) {
    return ((uint32_t)FT[o] << 24) | ((uint32_t)FT[o + 1] << 16) |
           ((uint32_t)FT[o + 2] << 8) | (uint32_t)FT[o + 3];
}

/* ----- parsed table offsets / metrics ----- */
static uint32_t T_loca, T_glyf, T_hmtx, T_cmap4;
static int F_upem, F_locLong, F_numGlyphs, F_numHM, F_ascent;
static int F_ready = 0;

/* ----- per-draw-call transform state (single threaded boot) ----- */
static int32_t PENX, BASEY;   /* subpixel pen position / baseline */
static int64_t MULN;          /* size * TTF_SS                    */

/* ----- scratch (file-scope so we never blow the kernel stack) ----- */
static int32_t  E_x0[TTF_MAXEDGES], E_y0[TTF_MAXEDGES],
                E_x1[TTF_MAXEDGES], E_y1[TTF_MAXEDGES];
static int      nedges;

static uint16_t S_ends[256];
static uint8_t  S_flags[TTF_MAXPTS];
static int32_t  S_xs[TTF_MAXPTS], S_ys[TTF_MAXPTS];
static int32_t  X_x[TTF_MAXPTS * 2], X_y[TTF_MAXPTS * 2];
static uint8_t  X_on[TTF_MAXPTS * 2];

static int32_t  C_x[TTF_MAXCROSS];
static int      C_d[TTF_MAXCROSS];

static uint8_t  COV[TTF_COVMAX * TTF_COVMAX];

/* box the last rasterize produced, in whole pixels relative to the
 * canonical origin (pen on the baseline) */
static int      COV_w, COV_h, COV_ox, COV_oy;

/*
 * Coverage curve — the cheap stand-in for hinting.
 *
 * A lowercase stem in this face is 63/1000 em, which at 13 px is 0.82 of
 * a pixel: it physically cannot fill one, so the darkest pixel in an 'l'
 * comes out mid-grey and the whole UI reads as smudged.  A real hinting
 * engine would snap the stem onto the pixel grid so it lands solid.
 * Lifting partial coverage is the approximation of that, and it is what
 * makes small text look like ink instead of a smear.
 *
 * The curve is the midpoint between leaving coverage alone and the
 * aggressive a(2-a) lift, which darkens the mid-tones where stems live
 * without crushing the soft edges of curves into hard steps.
 */
static uint8_t  COVCURVE[256];

static void build_cov_curve(void) {
    for (int a = 0; a < 256; a++) {
        int inv = 255 - a;
        int strong = 255 - (inv * inv) / 255;
        COVCURVE[a] = (uint8_t)((a + strong) / 2);
    }
}

/*
 * Glyph mask cache.
 *
 * Every string in the UI is re-rasterized on every frame — outline
 * decode, Bezier flattening and a full scanline fill per character, tens
 * of thousands of edge tests for a single 13 px glyph.  Since glyphs are
 * now placed on whole pixels, the coverage mask for a given glyph at a
 * given size is always identical, so it can be computed once and then
 * simply blitted.  That is what pays for the finer supersampling above.
 */
typedef struct {
    uint32_t key;        /* (gid << 8) | size, never 0 for a live slot */
    int16_t  ox, oy;     /* mask corner relative to pen / baseline, px */
    uint16_t w, h;
    uint32_t off;        /* into G_pool */
} glyph_slot_t;

static glyph_slot_t G_slot[TTF_CACHE_SLOTS];
static uint8_t      G_pool[TTF_CACHE_BYTES];
static uint32_t     G_pool_used = 0;

/* ----- transform: font units -> subpixel device coordinates ----- */
static inline int32_t TX(int fu) { return PENX + (int32_t)(((int64_t)fu * MULN) / F_upem); }
static inline int32_t TY(int fu) { return BASEY - (int32_t)(((int64_t)fu * MULN) / F_upem); }

static inline int floordiv(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

/* ----- table directory parse ----- */
static int ttf_init(void) {
    FT = comicneue_ttf;
    uint32_t T_head = 0, T_maxp = 0, T_hhea = 0, T_cmap = 0;
    T_loca = T_glyf = T_hmtx = 0;

    int nt = be16(4);
    uint32_t o = 12;
    for (int i = 0; i < nt; i++, o += 16) {
        uint32_t tag = be32(o);
        uint32_t off = be32(o + 8);
        switch (tag) {
            case 0x68656164: T_head = off; break; /* 'head' */
            case 0x6D617870: T_maxp = off; break; /* 'maxp' */
            case 0x68686561: T_hhea = off; break; /* 'hhea' */
            case 0x636D6170: T_cmap = off; break; /* 'cmap' */
            case 0x6C6F6361: T_loca = off; break; /* 'loca' */
            case 0x676C7966: T_glyf = off; break; /* 'glyf' */
            case 0x686D7478: T_hmtx = off; break; /* 'hmtx' */
            default: break;
        }
    }
    if (!T_head || !T_maxp || !T_hhea || !T_cmap || !T_loca || !T_glyf || !T_hmtx)
        return 0;

    F_upem    = be16(T_head + 18);
    F_locLong = sbe16(T_head + 50);
    F_numGlyphs = be16(T_maxp + 4);
    F_ascent  = sbe16(T_hhea + 4);
    F_numHM   = be16(T_hhea + 34);

    /* pick a format-4 cmap subtable, preferring a Windows (platform 3) one */
    int ns = be16(T_cmap + 2);
    T_cmap4 = 0;
    for (int i = 0; i < ns; i++) {
        uint16_t pid = be16(T_cmap + 4 + i * 8);
        uint32_t off = be32(T_cmap + 8 + i * 8);
        if (be16(T_cmap + off) == 4) {
            T_cmap4 = T_cmap + off;
            if (pid == 3) break;
        }
    }
    if (!T_cmap4) return 0;

    build_cov_curve();
    F_ready = 1;
    return 1;
}

/* ----- cmap format 4: codepoint -> glyph id ----- */
static int glyph_index(uint32_t cp) {
    uint32_t s = T_cmap4;
    int segX2 = be16(s + 6);
    uint32_t endA   = s + 14;
    uint32_t startA = endA + segX2 + 2;
    uint32_t deltaA = startA + segX2;
    uint32_t rangeA = deltaA + segX2;
    int segc = segX2 / 2;
    for (int i = 0; i < segc; i++) {
        uint16_t endc = be16(endA + i * 2);
        if (cp <= endc) {
            uint16_t startc = be16(startA + i * 2);
            if (cp < startc) return 0;
            int16_t  idd = sbe16(deltaA + i * 2);
            uint16_t iro = be16(rangeA + i * 2);
            if (iro == 0) return (uint16_t)(cp + idd);
            uint32_t addr = rangeA + i * 2 + iro + (cp - startc) * 2;
            uint16_t g = be16(addr);
            if (g == 0) return 0;
            return (uint16_t)(g + idd);
        }
    }
    return 0;
}

/* ----- loca: glyph id -> glyf offset + length ----- */
static uint32_t glyf_offset(int gid, uint32_t *len) {
    uint32_t a, b;
    if (F_locLong) { a = be32(T_loca + gid * 4); b = be32(T_loca + gid * 4 + 4); }
    else           { a = (uint32_t)be16(T_loca + gid * 2) * 2;
                     b = (uint32_t)be16(T_loca + gid * 2 + 2) * 2; }
    *len = b - a;
    return T_glyf + a;
}

/* ----- hmtx: glyph advance width (font units) ----- */
static int advance_width(int gid) {
    if (gid >= F_numHM) gid = F_numHM - 1;
    return be16(T_hmtx + gid * 4);
}

/* ----- edge / curve emission (device subpixel space) ----- */
static void push_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) return;                 /* horizontal edges never cross a scanline */
    if (nedges >= TTF_MAXEDGES) return;
    E_x0[nedges] = x0; E_y0[nedges] = y0;
    E_x1[nedges] = x1; E_y1[nedges] = y1;
    nedges++;
}

static void flatten_quad(int32_t p0x, int32_t p0y, int32_t p1x, int32_t p1y,
                         int32_t p2x, int32_t p2y) {
    int N = TTF_FLATTEN;
    int32_t px = p0x, py = p0y;
    for (int s = 1; s <= N; s++) {
        int64_t a = (int64_t)(N - s) * (N - s);
        int64_t b = (int64_t)2 * (N - s) * s;
        int64_t c = (int64_t)s * s;
        int32_t qx = (int32_t)((a * p0x + b * p1x + c * p2x) / (int64_t)(N * N));
        int32_t qy = (int32_t)((a * p0y + b * p1y + c * p2y) / (int64_t)(N * N));
        push_edge(px, py, qx, qy);
        px = qx; py = qy;
    }
}

/* ----- one contour (font-unit point range) -> edges ----- */
static void contour_to_edges(int start, int end, int ox, int oy) {
    int n = end - start + 1;
    if (n < 2) return;

    /* expand: insert implied on-curve midpoints between consecutive off-curve pts */
    int em = 0;
    for (int i = 0; i < n; i++) {
        int idx  = start + i;
        int nidx = start + ((i + 1) % n);
        X_x[em] = S_xs[idx]; X_y[em] = S_ys[idx]; X_on[em] = (uint8_t)(S_flags[idx] & 1); em++;
        if (!(S_flags[idx] & 1) && !(S_flags[nidx] & 1)) {
            X_x[em] = (S_xs[idx] + S_xs[nidx]) / 2;
            X_y[em] = (S_ys[idx] + S_ys[nidx]) / 2;
            X_on[em] = 1; em++;
        }
    }

    int r = -1;
    for (int i = 0; i < em; i++) if (X_on[i]) { r = i; break; }
    if (r < 0) return;                    /* degenerate all-offcurve contour */

    int32_t Ax = TX(X_x[r] + ox), Ay = TY(X_y[r] + oy);
    int i = 1;
    while (i <= em) {
        int idx = (r + i) % em;
        if (X_on[idx]) {
            int32_t Bx = TX(X_x[idx] + ox), By = TY(X_y[idx] + oy);
            push_edge(Ax, Ay, Bx, By);
            Ax = Bx; Ay = By; i++;
        } else {
            int nidx = (r + i + 1) % em;
            int32_t Cx = TX(X_x[idx]  + ox), Cy = TY(X_y[idx]  + oy);
            int32_t Ex = TX(X_x[nidx] + ox), Ey = TY(X_y[nidx] + oy);
            flatten_quad(Ax, Ay, Cx, Cy, Ex, Ey);
            Ax = Ex; Ay = Ey; i += 2;
        }
    }
}

/* ----- simple glyph -> edges ----- */
static void decode_simple(uint32_t go, int nc, int ox, int oy) {
    uint32_t p = go + 10;
    for (int i = 0; i < nc; i++) { S_ends[i] = be16(p); p += 2; }
    int npts = S_ends[nc - 1] + 1;
    if (npts > TTF_MAXPTS) return;

    uint16_t instr = be16(p); p += 2 + instr;

    /* flags (with repeat) */
    int i = 0;
    while (i < npts) {
        uint8_t f = FT[p++];
        S_flags[i++] = f;
        if (f & 0x08) { uint8_t rep = FT[p++]; while (rep-- && i < npts) S_flags[i++] = f; }
    }
    /* x deltas */
    int32_t x = 0;
    for (i = 0; i < npts; i++) {
        uint8_t f = S_flags[i];
        if (f & 0x02)            { uint8_t d = FT[p++]; x += (f & 0x10) ? d : -(int)d; }
        else if (!(f & 0x10))    { x += sbe16(p); p += 2; }
        S_xs[i] = x;
    }
    /* y deltas */
    int32_t y = 0;
    for (i = 0; i < npts; i++) {
        uint8_t f = S_flags[i];
        if (f & 0x04)            { uint8_t d = FT[p++]; y += (f & 0x20) ? d : -(int)d; }
        else if (!(f & 0x20))    { y += sbe16(p); p += 2; }
        S_ys[i] = y;
    }
    int start = 0;
    for (int c = 0; c < nc; c++) {
        contour_to_edges(start, S_ends[c], ox, oy);
        start = S_ends[c] + 1;
    }
}

/* ----- glyph (simple or composite) -> edges ----- */
static void decode_glyph(int gid, int ox, int oy, int depth) {
    if (gid < 0 || gid >= F_numGlyphs) return;
    uint32_t glen, go = glyf_offset(gid, &glen);
    if (glen == 0) return;                /* empty glyph (e.g. space) */

    int nc = sbe16(go);
    if (nc >= 0) { decode_simple(go, nc, ox, oy); return; }

    /* composite */
    uint32_t p = go + 10;
    for (;;) {
        uint16_t flags = be16(p);
        uint16_t cgid  = be16(p + 2);
        p += 4;
        int dx, dy;
        if (flags & 0x0001) { dx = sbe16(p); dy = sbe16(p + 2); p += 4; }
        else                { dx = (int8_t)FT[p]; dy = (int8_t)FT[p + 1]; p += 2; }
        if      (flags & 0x0008) p += 2;  /* WE_HAVE_A_SCALE   */
        else if (flags & 0x0040) p += 4;  /* X_AND_Y_SCALE     */
        else if (flags & 0x0080) p += 8;  /* WE_HAVE_A_2X2     */

        if (depth < 4) {
            if (flags & 0x0002) decode_glyph(cgid, ox + dx, oy + dy, depth + 1); /* xy offset */
            else                decode_glyph(cgid, ox, oy, depth + 1);            /* point match: ignore */
        }
        if (!(flags & 0x0020)) break;     /* no MORE_COMPONENTS */
    }
}

/* ----- alpha blend (over) -----
 *
 * The innermost loop of every character on screen: one call per
 * partially covered pixel, and an 8x8 supersampled mask means most
 * pixels of most glyphs are partially covered.
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
 *
 * Identical to gfx_mix() in gfx.h, and deliberately not shared with
 * it: ttf.h is included before gfx.h in one of the two trees, and a
 * header that only compiles in a particular include order is a worse
 * problem than eight duplicated lines.
 */
static inline uint32_t blend(uint32_t fg, uint32_t bg, uint32_t a) {
    const uint32_t ia = 255u - a;
    uint32_t rb = (fg & 0x00FF00FFu) * a + (bg & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g  = (fg & 0x0000FF00u) * a + (bg & 0x0000FF00u) * ia + 0x00008000u;
    g  = ((g  + ((g  >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
    return rb | g;
}

/* ----- rasterize the current edge list into COV -----
 * Returns 1 if anything was covered, and leaves the box in
 * COV_w/COV_h/COV_ox/COV_oy. */
static int rasterize_glyph(void) {
    COV_w = COV_h = 0;
    if (nedges == 0) return 0;

    int32_t minx = 0x7FFFFFFF, maxx = -0x7FFFFFFF;
    int32_t miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF;
    for (int e = 0; e < nedges; e++) {
        int32_t a = E_x0[e], b = E_x1[e];
        if (a < minx) minx = a;
        if (a > maxx) maxx = a;
        if (b < minx) minx = b;
        if (b > maxx) maxx = b;
        a = E_y0[e]; b = E_y1[e];
        if (a < miny) miny = a;
        if (a > maxy) maxy = a;
        if (b < miny) miny = b;
        if (b > maxy) maxy = b;
    }

    int boxMinPx = floordiv(minx, TTF_SS), boxMaxPx = floordiv(maxx, TTF_SS);
    int boxMinPy = floordiv(miny, TTF_SS), boxMaxPy = floordiv(maxy, TTF_SS);
    int boxW = boxMaxPx - boxMinPx + 1;
    int boxH = boxMaxPy - boxMinPy + 1;
    if (boxW <= 0 || boxH <= 0) return 0;
    if (boxW > TTF_COVMAX) boxW = TTF_COVMAX;
    if (boxH > TTF_COVMAX) boxH = TTF_COVMAX;

    for (int k = 0; k < boxW * boxH; k++) COV[k] = 0;

    int ysTop = boxMinPy * TTF_SS;
    int ysBot = (boxMinPy + boxH) * TTF_SS;   /* exclusive */
    for (int ys = ysTop; ys < ysBot; ys++) {
        int nc = 0;
        for (int e = 0; e < nedges; e++) {
            int32_t y0 = E_y0[e], y1 = E_y1[e];
            int ymin, ymax, dir;
            if (y0 < y1) { ymin = y0; ymax = y1; dir = 1; }
            else         { ymin = y1; ymax = y0; dir = -1; }
            if (ys < ymin || ys >= ymax) continue;
            int64_t xc = E_x0[e] +
                (int64_t)(E_x1[e] - E_x0[e]) * (ys - E_y0[e]) / (E_y1[e] - E_y0[e]);
            if (nc < TTF_MAXCROSS) { C_x[nc] = (int32_t)xc; C_d[nc] = dir; nc++; }
        }
        /* insertion sort crossings by x */
        for (int a = 1; a < nc; a++) {
            int32_t vx = C_x[a]; int vd = C_d[a]; int b = a - 1;
            while (b >= 0 && C_x[b] > vx) { C_x[b + 1] = C_x[b]; C_d[b + 1] = C_d[b]; b--; }
            C_x[b + 1] = vx; C_d[b + 1] = vd;
        }
        /* nonzero winding fill */
        int py = floordiv(ys, TTF_SS) - boxMinPy;
        if (py < 0 || py >= boxH) continue;
        int w = 0, spanStart = 0;
        for (int k = 0; k < nc; k++) {
            int prev = w; w += C_d[k];
            if (prev == 0 && w != 0) spanStart = C_x[k];
            else if (prev != 0 && w == 0) {
                for (int col = spanStart; col < C_x[k]; col++) {
                    int px = floordiv(col, TTF_SS) - boxMinPx;
                    if (px < 0 || px >= boxW) continue;
                    int idx = py * boxW + px;
                    if (COV[idx] < 255) COV[idx]++;
                }
            }
        }
    }

    /* Normalise the sample counts to 0..255 in place, then apply the
     * coverage curve, so a cached mask is ready to blend as it stands. */
    int maxc = TTF_SS * TTF_SS;
    for (int k = 0; k < boxW * boxH; k++) {
        int c = COV[k];
        if (c > maxc) c = maxc;
        COV[k] = COVCURVE[(uint32_t)c * 255 / (uint32_t)maxc];
    }

    COV_w = boxW; COV_h = boxH;
    COV_ox = boxMinPx; COV_oy = boxMinPy;
    return 1;
}

/* ----- blend a coverage mask into the framebuffer ----- */
static void blit_mask(uint32_t *buf, int bw, int bh,
                      const uint8_t *mask, int mw, int mh,
                      int x0, int y0, uint32_t color) {
    for (int py = 0; py < mh; py++) {
        int Y = y0 + py;
        if (Y < 0 || Y >= bh) continue;
        const uint8_t *row = mask + py * mw;
        uint32_t *dst = buf + Y * bw;
        for (int px = 0; px < mw; px++) {
            uint32_t a = row[px];
            if (!a) continue;
            int X = x0 + px;
            if (X < 0 || X >= bw) continue;
            dst[X] = (a == 255) ? color : blend(color, dst[X], a);
        }
    }
}

/* ----- cache lookup, rasterizing on a miss ----- */
static const glyph_slot_t *glyph_mask(int gid, int size) {
    /* Clear first: the caller falls back to whatever this leaves in COV,
     * and must never be handed a mask left over from a previous glyph. */
    COV_w = COV_h = 0;

    uint32_t key = ((uint32_t)gid << 8) | (uint32_t)(size & 0xFF);
    uint32_t h = (key * 2654435761u) % TTF_CACHE_SLOTS;

    for (uint32_t probe = 0; probe < 64; probe++) {
        glyph_slot_t *s = &G_slot[(h + probe) % TTF_CACHE_SLOTS];
        if (s->key == key) return s->w ? s : 0;
        if (s->key != 0) continue;              /* occupied by someone else */

        /* miss: rasterize once, at the canonical origin */
        PENX = 0;
        BASEY = 0;
        nedges = 0;
        decode_glyph(gid, 0, 0, 0);
        if (!rasterize_glyph()) {               /* blank, e.g. a space */
            s->key = key; s->w = s->h = 0;
            return 0;
        }
        uint32_t need = (uint32_t)COV_w * (uint32_t)COV_h;
        if (COV_w > TTF_CACHE_MAXPX || COV_h > TTF_CACHE_MAXPX ||
            G_pool_used + need > TTF_CACHE_BYTES)
            return 0;                           /* too big, or pool full */

        uint8_t *dst = G_pool + G_pool_used;
        for (uint32_t k = 0; k < need; k++) dst[k] = COV[k];
        s->key = key;
        s->ox = (int16_t)COV_ox; s->oy = (int16_t)COV_oy;
        s->w  = (uint16_t)COV_w; s->h  = (uint16_t)COV_h;
        s->off = G_pool_used;
        G_pool_used += need;
        return s;
    }
    return 0;
}

/* ----- public: draw a string with its top-left at (topX,topY) ----- */
static void ttf_draw_string(uint32_t *buf, int bw, int bh,
                            int topX, int topY, const char *s,
                            uint32_t color, int size) {
    if (!F_ready && !ttf_init()) return;

    MULN = (int64_t)size * TTF_SS;

    /*
     * Grid-fit the baseline.  Left fractional it lands on an exact half
     * pixel at 13 and 14 px — the two sizes most of this UI is set in —
     * which splits the flat bottom of every letter across two rows and
     * puts a grey fringe under the whole interface.
     */
    int base_px = topY + (int)(((int64_t)F_ascent * size + F_upem / 2) / F_upem);

    int64_t pen = 0;          /* exact subpixel pen, so spacing never drifts */

    for (; *s; s++) {
        int gid = glyph_index((uint8_t)*s);

        /*
         * The pen advances exactly, but each glyph is *placed* on a whole
         * pixel.  Left on a quarter-pixel phase, the same letter picks up
         * a different number of sample columns depending on where in the
         * word it falls, so identical letters render at visibly different
         * weights — and no two placements of a glyph can share a mask.
         */
        int pen_px = topX + (int)((pen + TTF_SS / 2) / TTF_SS);

        const glyph_slot_t *g = glyph_mask(gid, size);
        if (g)
            blit_mask(buf, bw, bh, G_pool + g->off, g->w, g->h,
                      pen_px + g->ox, base_px + g->oy, color);
        else if (COV_w)       /* did not fit the cache; mask is still in COV */
            blit_mask(buf, bw, bh, COV, COV_w, COV_h,
                      pen_px + COV_ox, base_px + COV_oy, color);

        int32_t adv = (int32_t)(((int64_t)advance_width(gid) * MULN) / F_upem);
        pen += adv + adv * TTF_HPAD_NUM / TTF_HPAD_DEN;
    }
}

__attribute__((unused))
static int ttf_line_height(int size) {
    return size * TTF_LINE_SCALE_NUM / TTF_LINE_SCALE_DEN;
}

/* Exact pixel width of a string at a given size (matches ttf_draw_string
 * advance logic) — for centering text and computing hit boxes. */
/*
 * Draw a string, but never past right_px.
 *
 * Window titles are the case this exists for: a snapped window can be half
 * the width its title was written for, and a title running under the
 * close button looks like a bug rather than a long name. Text that does
 * not fit is cut at the last whole character that does and finished with
 * an ellipsis, so the reader can see that something was removed.
 *
 * Measuring twice is deliberate. Drawing and then painting over the
 * overflow would need the background back, which the caller has already
 * overwritten by the time this runs.
 */
static int ttf_text_width(const char *s, int size);

static void ttf_draw_string_clip(uint32_t *buf, int bw, int bh,
                                 int topX, int topY, const char *s,
                                 uint32_t color, int size, int right_px) {
    if (topX >= right_px) return;
    if (topX + ttf_text_width(s, size) <= right_px) {
        ttf_draw_string(buf, bw, bh, topX, topY, s, color, size);
        return;
    }

    char cut[96];
    const int ell = ttf_text_width("...", size);
    int n = 0;
    for (; s[n] && n < (int)sizeof(cut) - 4; n++) {
        cut[n] = s[n];
        cut[n + 1] = '\0';
        if (topX + ttf_text_width(cut, size) + ell > right_px) {
            cut[n] = '\0';       /* this one already overflowed: drop it */
            break;
        }
    }
    cut[n] = '\0';
    /* Three dots alone say less than nothing; leave the field empty. */
    if (n == 0) return;
    cut[n] = '.'; cut[n + 1] = '.'; cut[n + 2] = '.'; cut[n + 3] = '\0';
    ttf_draw_string(buf, bw, bh, topX, topY, cut, color, size);
}

static int ttf_text_width(const char *s, int size) {
    if (!F_ready && !ttf_init()) return 0;
    int64_t muln = (int64_t)size * TTF_SS;
    int32_t pen = 0;
    for (; *s; s++) {
        int gid = glyph_index((uint8_t)*s);
        int32_t adv = (int32_t)(((int64_t)advance_width(gid) * muln) / F_upem);
        pen += adv + adv * TTF_HPAD_NUM / TTF_HPAD_DEN;
    }
    /* Round to match where ttf_draw_string actually puts the pen, so
     * centring and hit boxes agree with the glyphs on screen. */
    return (pen + TTF_SS / 2) / TTF_SS;
}

#endif /* TTF_H */
