#ifndef TTF_H
#define TTF_H

/*
 * Freestanding TrueType rasterizer.
 *
 * It parses the embedded Comic Neue Regular face (see comicneue_ttf.h)
 * and renders glyphs into a 32-bit ARGB/XRGB backbuffer.
 *
 * Supported: head, maxp, hhea, hmtx, cmap (format 4), loca (short & long),
 * glyf (simple + composite glyphs), quadratic Bezier outlines.
 *
 * ---- what changed when the FPU arrived ----
 *
 * This was entirely fixed point, because the kernel it lived in had no
 * floating point at all. It worked, and it left two visible marks that
 * integers were the reason for.
 *
 * The first was faceting. Every quadratic was flattened into exactly
 * eight line segments regardless of how large it was drawn, because
 * choosing a count from the curve's actual size needs a square root.
 * Eight is generous at 13 px and plainly not enough at 40: the shoulder
 * of an 'n' in a heading came out as a short run of straight lines with
 * corners between them. Now the count comes from the curve's own
 * deviation from its chord, measured in device pixels, so a glyph is
 * flattened as finely as its size requires and no finer.
 *
 * The second was quantisation. Horizontal coverage was counted in
 * subpixel columns — eight per pixel, so a vertical stem edge could only
 * land on one of eight positions, and a near-vertical diagonal stepped
 * between them. Coverage is now the exact overlap between the filled
 * span and the pixel, computed as a real number, so the horizontal
 * resolution is continuous. Vertically it is still eight samples per
 * pixel row; that is where the remaining stair-stepping lives, and it is
 * far below what the eye picks up at UI sizes.
 *
 * None of this costs a frame. Every glyph is rasterised once per size
 * into a coverage mask and then blitted, so this runs when a character
 * is seen for the first time and never again.
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

/*
 * Curve flattening.
 *
 * A quadratic is split until its deviation from a straight line is below
 * TTF_FLATTEN_TOL device pixels. A twentieth of a pixel is far below
 * what anti-aliasing can express, so what comes out is limited by the
 * sampling rather than by the approximation — which is the point: it
 * should be impossible to tell that these are line segments at all.
 *
 * The bound matters as much as the tolerance. A near-degenerate curve at
 * an enormous size could ask for hundreds of segments and exhaust the
 * edge list, and 64 is already past the point of visible difference.
 */
#define TTF_FLATTEN_TOL  0.05f
#define TTF_FLATTEN_MAX  64

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
/* The hinting tables. Absent in a font with no bytecode, which is a
 * perfectly ordinary thing for a font to be -- all three are checked
 * before anything tries to run them. */
static uint32_t T_cvt, T_fpgm, T_prep;
static uint32_t L_cvt, L_fpgm, L_prep;
static int F_upem, F_locLong, F_numGlyphs, F_numHM, F_ascent;
static int F_ready = 0;

/*
 * Grid-fitting, on or off.
 *
 * On by default because the font asks for it: this face's gasp table
 * requests grid-fitting and grayscale at every size. The switch exists
 * so the two can be compared without rebuilding -- and because a font
 * program is arbitrary code from a file, and being able to turn it off
 * without recompiling is worth one integer.
 */
static int F_hinting = 1;

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

/* Scanline crossings, as real numbers. They used to be integers in
 * eighths of a pixel, which is where the horizontal quantisation came
 * from: two edges a hundredth of a pixel apart produced the same
 * crossing and therefore the same pixel. */
static float    C_xf[TTF_MAXCROSS];
static int      C_d[TTF_MAXCROSS];

static uint8_t  COV[TTF_COVMAX * TTF_COVMAX];

/* Where the exact areas accumulate before they are quantised into COV.
 * A quarter of a megabyte, used by one glyph at a time and never
 * touched again once the mask is cached. */
static float    COVA[TTF_COVMAX * TTF_COVMAX];

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
    /*
     * Coverage raised to the power 3/4 — a text gamma of 1.33, which is
     * the value this kind of correction conventionally uses.
     *
     * What it replaces was the midpoint of two integer curves, chosen by
     * eye because the arithmetic to do it properly was not available.
     * The two land within a few levels of each other across the whole
     * range, which is a compliment to the eye that picked the old one;
     * the difference is that this one says what it is.
     *
     * x^(3/4) is x^(1/2) * x^(1/4), so it is two square roots and a
     * multiply — no logarithm, no exponential, nothing that would need a
     * maths library the kernel does not have.
     */
    for (int a = 0; a < 256; a++) {
        float t  = (float)a / 255.0f;
        float r2 = __builtin_sqrtf(t);
        float v  = r2 * __builtin_sqrtf(r2);
        int   c  = (int)(v * 255.0f + 0.5f);
        COVCURVE[a] = (uint8_t)(c > 255 ? 255 : c);
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

/* The bytecode interpreter. After the byte readers and the table
 * offsets it needs, before the glyph parsing that drives it. */
#include "ttfhint.h"

/*
 * Hinting state that belongs to the parser rather than the interpreter.
 *
 * TT_prepped remembers which ppem the pre-program last ran at, because
 * it must run again whenever the size changes and must *not* run once
 * per glyph -- it is the expensive half.
 */
static int H_size_ready = 0;      /* prep has run for H_ppem            */
static int H_ppem = -1;
static int32_t H_scale16 = 0;     /* font units -> 26.6, as 16.16       */

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
            /* The hinting tables. 'cvt ' really does have a trailing
             * space -- tags are four bytes and the name is three. */
            case 0x63767420: T_cvt  = off; L_cvt  = be32(o + 12); break;
            case 0x6670676D: T_fpgm = off; L_fpgm = be32(o + 12); break;
            case 0x70726570: T_prep = off; L_prep = be32(o + 12); break;
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

    /*
     * The font program, once.
     *
     * It defines the functions every glyph and the pre-program call, so
     * it has to succeed before any of them can run. If it does not --
     * a truncated table, an opcode this interpreter does not implement --
     * hinting is switched off for the whole face rather than left half
     * initialised, and text renders exactly as it did before any of this
     * existed.
     */
    if (F_hinting && T_fpgm && L_fpgm) {
        if (!tt_run_fpgm(FT + T_fpgm, L_fpgm)) {
            F_hinting = 0;
            serial_puts("[ttf] the font program did not run; hinting off\n");
        }
    } else if (!T_fpgm) {
        F_hinting = 0;                 /* nothing to run: an unhinted face */
    }

    F_ready = 1;
    return 1;
}

/*
 * Make sure the pre-program has run for this pixel size.
 *
 * Once per size, not once per glyph: it is where the font works out what
 * its stems should measure at this ppem, and it writes those answers
 * into the control value table for every glyph that follows to read.
 */
static void hint_prepare_size(int ppem) {
    if (!F_hinting) return;
    if (H_size_ready && H_ppem == ppem) return;

    H_ppem = ppem;
    /* Font units to 26.6 pixels, carried as 16.16 so the interpreter can
     * rescale control values without a second division. */
    H_scale16 = (int32_t)(((int64_t)ppem * 64 * 65536) / F_upem);

    if (!tt_run_prep(FT + T_cvt, L_cvt,
                     T_prep ? FT + T_prep : 0, T_prep ? L_prep : 0,
                     ppem, H_scale16)) {
        F_hinting = 0;
        serial_puts("[ttf] the pre-program did not run; hinting off\n");
        return;
    }
    H_size_ready = 1;
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

/*
 * Flatten a quadratic into as many segments as it actually needs.
 *
 * The old answer was eight, always, because picking a number from the
 * curve's size needs a square root and there was no floating point to
 * take one with. Eight is more than enough for body text and visibly
 * too few for a heading: the shoulder of an 'n' at 40 px came out as a
 * short run of straight lines with corners between them.
 *
 * A quadratic's greatest distance from the chord joining its endpoints
 * is |P0 - 2P1 + P2| / 4, and subdividing into N pieces reduces that by
 * N squared — so the count needed for a given tolerance is the square
 * root of the ratio. Two square roots and a divide, once per curve, at
 * glyph-cache fill time.
 */
static void flatten_quad(int32_t p0x, int32_t p0y, int32_t p1x, int32_t p1y,
                         int32_t p2x, int32_t p2y) {
    /* Deviation, in whole device pixels: the edge list is in units of
     * 1/TTF_SS of a pixel. */
    float dx = (float)(p0x - 2 * p1x + p2x) / (4.0f * (float)TTF_SS);
    float dy = (float)(p0y - 2 * p1y + p2y) / (4.0f * (float)TTF_SS);
    float dev = __builtin_sqrtf(dx * dx + dy * dy);

    int N = 1;
    if (dev > TTF_FLATTEN_TOL)
        N = (int)(__builtin_sqrtf(dev / TTF_FLATTEN_TOL) + 0.999f);
    if (N < 2) N = 2;
    if (N > TTF_FLATTEN_MAX) N = TTF_FLATTEN_MAX;

    int32_t px = p0x, py = p0y;
    const float inv = 1.0f / (float)N;
    for (int s = 1; s <= N; s++) {
        float t = (float)s * inv, u = 1.0f - t;
        float a = u * u, b = 2.0f * u * t, c = t * t;
        int32_t qx = (int32_t)(a * (float)p0x + b * (float)p1x + c * (float)p2x);
        int32_t qy = (int32_t)(a * (float)p0y + b * (float)p1y + c * (float)p2y);
        push_edge(px, py, qx, qy);
        px = qx; py = qy;
    }
}

/*
 * Whether S_xs/S_ys hold font units or finished device coordinates.
 *
 * Unhinted, they are font units and TX/TY scale them on the way out.
 * Hinted, the interpreter has already produced positions in pixels and
 * the composite offset has already been folded in, so the only thing
 * left to do is emit them -- scaling a second time would undo the
 * grid-fitting the whole exercise exists for.
 */
static int S_device = 0;

static inline int32_t PXo(int32_t v, int o) { return S_device ? v : TX(v + o); }
static inline int32_t PYo(int32_t v, int o) { return S_device ? v : TY(v + o); }

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

    int32_t Ax = PXo(X_x[r], ox), Ay = PYo(X_y[r], oy);
    int i = 1;
    while (i <= em) {
        int idx = (r + i) % em;
        if (X_on[idx]) {
            int32_t Bx = PXo(X_x[idx], ox), By = PYo(X_y[idx], oy);
            push_edge(Ax, Ay, Bx, By);
            Ax = Bx; Ay = By; i++;
        } else {
            int nidx = (r + i + 1) % em;
            int32_t Cx = PXo(X_x[idx],  ox), Cy = PYo(X_y[idx],  oy);
            int32_t Ex = PXo(X_x[nidx], ox), Ey = PYo(X_y[nidx], oy);
            flatten_quad(Ax, Ay, Cx, Cy, Ex, Ey);
            Ax = Ex; Ay = Ey; i += 2;
        }
    }
}

/* ----- hinting: font units -> grid-fitted device coordinates -----
 *
 * The interpreter works in 26.6 -- sixty-fourths of a pixel -- and the
 * rasteriser wants eighths, so the conversion is a divide by eight.
 * That looks lossy and is exactly not, for the coordinates that matter:
 * a point the program has snapped to a pixel boundary is a whole number
 * of 64ths, so it is a whole number of 8ths, and it lands precisely on
 * the boundary the rasteriser samples against. Rounding only affects
 * points the program deliberately left off the grid.
 */
static inline int32_t h26_to_dev(f26 v) {
    return (int32_t)(((int64_t)v * TTF_SS + 32) >> 6);
}

/* hmtx carries an advance and a left side bearing per glyph. The
 * bearing is needed for the phantom points, which the specification
 * appends to every glyph so that a program can read and move the
 * metrics along with the outline. */
static int left_bearing(int gid) {
    if (gid >= F_numHM) {
        /* Past the last full entry hmtx switches to bearings only. */
        uint32_t base = T_hmtx + (uint32_t)F_numHM * 4;
        return sbe16(base + (uint32_t)(gid - F_numHM) * 2);
    }
    return sbe16(T_hmtx + (uint32_t)gid * 4 + 2);
}

/*
 * Stage a glyph's points into the interpreter, run it, and hand back
 * device coordinates in S_xs/S_ys.
 *
 * Returns 1 if the outline is now hinted. Zero means use what was
 * already in S_xs/S_ys, unchanged and in font units -- every failure
 * path below leaves them exactly as they arrived.
 */
static int hint_glyph_points(int gid, int npts, int nc,
                             uint32_t ins_off, uint32_t ins_len,
                             int ox, int oy) {
    if (!F_hinting || !H_size_ready) return 0;
    if (ins_len == 0) return 0;
    if (npts + 4 > TT_MAX_PTS || nc > TT_MAX_CONT) return 0;

    /*
     * Scale into the interpreter's two parallel copies. `org` is where
     * the designer put the point and never changes; `cur` is what the
     * program moves. Interpolation reads both -- that is how an
     * untouched point knows where it sat relative to the ones that did
     * move -- so keeping them separate is not an optimisation.
     *
     * A composite's component offset is folded in here, before hinting
     * rather than after, so the component is hinted where it will
     * actually sit rather than at the origin.
     */
    for (int i = 0; i < npts; i++) {
        f26 x = (f26)(((int64_t)(S_xs[i] + ox) * H_scale16) >> 16);
        f26 y = (f26)(((int64_t)(S_ys[i] + oy) * H_scale16) >> 16);
        TH_org_x[i] = TH_cur_x[i] = x;
        TH_org_y[i] = TH_cur_y[i] = y;
        TH_tag[i] = (uint8_t)(S_flags[i] & 1);      /* on-curve; untouched */
    }

    /* The four phantom points: the two horizontal metrics and the two
     * vertical ones. This face never moves them -- it hints in y and
     * leaves the advance alone -- but a program may read them, and an
     * out-of-range read is what would otherwise abort the glyph. */
    {
        uint32_t glen_unused;
        int xmin = sbe16(glyf_offset(gid, &glen_unused) + 2);
        int lsb  = left_bearing(gid);
        int adv  = advance_width(gid);
        int p    = npts;
        int32_t pp1 = xmin - lsb + ox;

        TH_org_x[p] = TH_cur_x[p] = (f26)(((int64_t)pp1 * H_scale16) >> 16);
        TH_org_y[p] = TH_cur_y[p] = 0; TH_tag[p] = 0; p++;
        TH_org_x[p] = TH_cur_x[p] = (f26)(((int64_t)(pp1 + adv) * H_scale16) >> 16);
        TH_org_y[p] = TH_cur_y[p] = 0; TH_tag[p] = 0; p++;
        TH_org_x[p] = TH_cur_x[p] = 0;
        TH_org_y[p] = TH_cur_y[p] = (f26)(((int64_t)F_ascent * H_scale16) >> 16);
        TH_tag[p] = 0; p++;
        TH_org_x[p] = TH_cur_x[p] = 0;
        TH_org_y[p] = TH_cur_y[p] = 0; TH_tag[p] = 0;
    }

    for (int c = 0; c < nc; c++) TH_ends[c] = S_ends[c];
    TH_npts  = npts + 4;
    TH_ncont = nc;

    if (!tt_run_glyph(FT + ins_off, ins_len)) return 0;

    /* Back out as device coordinates. The y axis flips here, exactly as
     * TY() does: the font's y grows upward and the framebuffer's grows
     * down, and a glyph hinted the right way up and blitted upside down
     * is a spectacular way to discover that. */
    for (int i = 0; i < npts; i++) {
        S_xs[i] = PENX  + h26_to_dev(TH_cur_x[i]);
        S_ys[i] = BASEY - h26_to_dev(TH_cur_y[i]);
    }
    return 1;
}

/* ----- simple glyph -> edges ----- */
static void decode_simple(int gid, uint32_t go, int nc, int ox, int oy) {
    uint32_t p = go + 10;
    for (int i = 0; i < nc; i++) { S_ends[i] = be16(p); p += 2; }
    int npts = S_ends[nc - 1] + 1;
    if (npts > TTF_MAXPTS) return;

    /*
     * The instructions. This used to read the length and step over the
     * bytes -- `p += 2 + instr` and nothing else -- which is where all
     * of the font's grid-fitting went.
     */
    uint16_t instr = be16(p);
    uint32_t ins_off = p + 2;
    p += 2 + instr;

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
    /*
     * Grid-fit, then emit.
     *
     * S_device tells contour_to_edges which space the points are in, and
     * is restored afterwards so that a composite whose first component
     * hinted and whose second did not still transforms each correctly.
     */
    int was_device = S_device;
    S_device = hint_glyph_points(gid, npts, nc, ins_off, instr, ox, oy);

    int start = 0;
    for (int c = 0; c < nc; c++) {
        contour_to_edges(start, S_ends[c], ox, oy);
        start = S_ends[c] + 1;
    }
    S_device = was_device;
}

/* ----- glyph (simple or composite) -> edges ----- */
static void decode_glyph(int gid, int ox, int oy, int depth) {
    if (gid < 0 || gid >= F_numGlyphs) return;
    uint32_t glen, go = glyf_offset(gid, &glen);
    if (glen == 0) return;                /* empty glyph (e.g. space) */

    int nc = sbe16(go);
    if (nc >= 0) { decode_simple(gid, go, nc, ox, oy); return; }

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

    for (int k = 0; k < boxW * boxH; k++) COVA[k] = 0.0f;

    /*
     * One pass per subpixel scanline, sampled at the row's centre.
     *
     * The crossings are real numbers now, not integers rounded to the
     * nearest eighth of a pixel, and what a span contributes to a pixel
     * is the exact length of their overlap. Horizontal coverage is
     * therefore continuous: a stem edge one hundredth of a pixel further
     * right produces a pixel one hundredth darker, where before it
     * produced an identical pixel until it crossed the next eighth.
     *
     * That is the whole of the difference, and it is most visible on
     * near-vertical diagonals — the leg of a 'k', the stem of a 'y' —
     * which used to step between eight discrete shades and now do not.
     */
    const float inv_ss  = 1.0f / (float)TTF_SS;
    const float scanW   = inv_ss;          /* each scanline's share of a pixel */

    int ysTop = boxMinPy * TTF_SS;
    int ysBot = (boxMinPy + boxH) * TTF_SS;   /* exclusive */
    for (int ys = ysTop; ys < ysBot; ys++) {
        float yc = (float)ys + 0.5f;
        int nc = 0;
        for (int e = 0; e < nedges; e++) {
            float y0 = (float)E_y0[e], y1 = (float)E_y1[e];
            float ymin, ymax; int dir;
            if (y0 < y1) { ymin = y0; ymax = y1; dir = 1; }
            else         { ymin = y1; ymax = y0; dir = -1; }
            if (yc < ymin || yc >= ymax) continue;
            float xc = (float)E_x0[e] +
                ((float)E_x1[e] - (float)E_x0[e]) * (yc - y0) / (y1 - y0);
            if (nc < TTF_MAXCROSS) { C_xf[nc] = xc; C_d[nc] = dir; nc++; }
        }
        /* insertion sort crossings by x */
        for (int a = 1; a < nc; a++) {
            float vx = C_xf[a]; int vd = C_d[a]; int b = a - 1;
            while (b >= 0 && C_xf[b] > vx) {
                C_xf[b + 1] = C_xf[b]; C_d[b + 1] = C_d[b]; b--;
            }
            C_xf[b + 1] = vx; C_d[b + 1] = vd;
        }

        /* nonzero winding fill */
        int py = floordiv(ys, TTF_SS) - boxMinPy;
        if (py < 0 || py >= boxH) continue;
        float *row = COVA + (size_t)py * boxW;

        int w = 0;
        float spanStart = 0.0f;
        for (int k = 0; k < nc; k++) {
            int prev = w; w += C_d[k];
            if (prev == 0 && w != 0) { spanStart = C_xf[k]; continue; }
            if (prev == 0 || w != 0) continue;

            /* Span, converted from subpixel units into pixels relative
             * to the mask's own corner. */
            float a = spanStart * inv_ss - (float)boxMinPx;
            float b = C_xf[k]   * inv_ss - (float)boxMinPx;
            if (b <= 0.0f || a >= (float)boxW) continue;
            if (a < 0.0f) a = 0.0f;
            if (b > (float)boxW) b = (float)boxW;
            if (b <= a) continue;

            int p0 = (int)a;
            int p1 = (int)b;
            if (p1 >= boxW) p1 = boxW - 1;

            if (p0 == p1) {
                row[p0] += (b - a) * scanW;
            } else {
                row[p0] += ((float)(p0 + 1) - a) * scanW;
                for (int px = p0 + 1; px < p1; px++) row[px] += scanW;
                row[p1] += (b - (float)p1) * scanW;
            }
        }
    }

    /* To eight bits, through the gamma curve, so a cached mask is ready
     * to blend exactly as it stands. */
    for (int k = 0; k < boxW * boxH; k++) {
        float c = COVA[k];
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        COV[k] = COVCURVE[(int)(c * 255.0f + 0.5f)];
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

        /*
         * A miss: rasterise once, at the canonical origin.
         *
         * The pre-program runs here rather than per glyph. It is where
         * the font decides what a stem measures at this ppem, the answer
         * is the same for every glyph at that size, and it is the
         * expensive half of hinting.
         *
         * The origin matters more than it did. Hinting positions points
         * against the pixel grid, so it is only meaningful if the glyph
         * is drawn where it was hinted -- which is why the pen is a
         * whole pixel in ttf_draw_string and why the mask cache is keyed
         * by size.
         */
        hint_prepare_size(size);

        PENX = 0;
        BASEY = 0;
        nedges = 0;
        S_device = 0;
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
