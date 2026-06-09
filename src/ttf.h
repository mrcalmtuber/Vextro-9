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
#define TTF_SS        4        /* supersample factor per axis (4x4 = 16 samples) */
#define TTF_MAXPTS    2048     /* max points per simple glyph                    */
#define TTF_MAXEDGES  4096     /* max device-space edges per glyph               */
#define TTF_COVMAX    256      /* max glyph box side (pixels) for coverage buffer*/
#define TTF_MAXCROSS  128      /* max edge crossings per scanline                */
#define TTF_FLATTEN   8        /* line segments per quadratic Bezier             */

/* ----- high-density rendering: character padding and line-height ----- */
#define TTF_HPAD_NUM   1       /* horizontal inter-char padding: HPAD_NUM/HPAD_DEN of advance */
#define TTF_HPAD_DEN   8
#define TTF_LINE_SCALE_NUM  5  /* line-height multiplier: 5/4 = 1.25x             */
#define TTF_LINE_SCALE_DEN  4

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

/* ----- alpha blend (over) ----- */
static inline uint32_t blend(uint32_t fg, uint32_t bg, uint32_t a) {
    uint32_t fr = (fg >> 16) & 0xFF, fgn = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    uint32_t br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t r = (fr * a + br * (255 - a)) / 255;
    uint32_t g = (fgn * a + bgn * (255 - a)) / 255;
    uint32_t b = (fb * a + bb * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ----- rasterize current edge list into buf ----- */
static void rasterize_glyph(uint32_t *buf, int bw, int bh, uint32_t color) {
    if (nedges == 0) return;

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
    if (boxW <= 0 || boxH <= 0) return;
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

    /* blend coverage (0..SS*SS) into the framebuffer with smooth AA */
    int maxc = TTF_SS * TTF_SS;
    for (int py = 0; py < boxH; py++) {
        for (int px = 0; px < boxW; px++) {
            int c = COV[py * boxW + px];
            if (!c) continue;
            if (c > maxc) c = maxc;
            uint32_t a = (uint32_t)c * 255 / (uint32_t)maxc;
            int X = boxMinPx + px, Y = boxMinPy + py;
            if (X < 0 || Y < 0 || X >= bw || Y >= bh) continue;
            buf[Y * bw + X] = blend(color, buf[Y * bw + X], a);
        }
    }
}

/* ----- public: draw a string with its top-left at (topX,topY) ----- */
static void ttf_draw_string(uint32_t *buf, int bw, int bh,
                            int topX, int topY, const char *s,
                            uint32_t color, int size) {
    if (!F_ready && !ttf_init()) return;

    MULN  = (int64_t)size * TTF_SS;
    PENX  = topX * TTF_SS;
    BASEY = topY * TTF_SS + (int32_t)(((int64_t)F_ascent * MULN) / F_upem);

    for (; *s; s++) {
        int gid = glyph_index((uint8_t)*s);
        nedges = 0;
        decode_glyph(gid, 0, 0, 0);
        rasterize_glyph(buf, bw, bh, color);
        int32_t adv = (int32_t)(((int64_t)advance_width(gid) * MULN) / F_upem);
        PENX += adv + adv * TTF_HPAD_NUM / TTF_HPAD_DEN;
    }
}

static int ttf_line_height(int size) {
    return size * TTF_LINE_SCALE_NUM / TTF_LINE_SCALE_DEN;
}

#endif /* TTF_H */
