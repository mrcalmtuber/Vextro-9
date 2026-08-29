#ifndef VXUI_H
#define VXUI_H

/*
 * src/vxui.h — the runtime half of the Tailwind pipeline.
 *
 * tools/tailwind.py resolves the utility classes in the shell files
 * under assets/ui/
 * against the design tokens and emits build/ui/vxui_gen.h: a flat table
 * of nodes whose *styles* are constants. This file does the other half —
 * the part that cannot be done at build time — and then paints.
 *
 * ============================================================
 *  WHY THE LAYOUT IS NOT PRECOMPUTED
 * ============================================================
 *
 * It was the obvious design and it is wrong. brw_draw is handed a width
 * and a height on every frame, and the browser window is resizable: a
 * coordinate baked into a generated header would be correct at one
 * window size and wrong at every other. So what the build resolves is
 * everything that does not depend on the window — colours, spacing, font
 * sizes, which sides have a border — and the flex solve happens here,
 * against the rectangle the compositor actually hands over.
 *
 * ============================================================
 *  THE LAYOUT MODEL, AND ITS EDGES
 * ============================================================
 *
 * Two directions and no wrapping:
 *
 *   A `flex` container lays its children out in a row. Children with a
 *   fixed width take it; children with `flex-N` share what is left in
 *   proportion to N; `justify-between` spreads any remaining slack into
 *   the gaps; `items-center` centres each child on the cross axis.
 *
 *   Anything else stacks its children down the column, each as wide as
 *   the content box unless it says otherwise.
 *
 * That is a real subset of flexbox and not a general one. There is no
 * wrapping, no `flex-basis`, no `flex-shrink`, no baseline alignment,
 * and no absolute positioning. Adding them would be adding a layout
 * engine, which is what WebKit is for; what is here is the model the
 * shell uses, and a class outside it is refused by the build tool rather
 * than silently ignored here.
 *
 * ============================================================
 *  THE FONTS
 * ============================================================
 *
 * Two, and the mapping is stated because there is a real deviation in
 * it. `font-mono` reaches the 8x8 bitmap face the terminal draws with
 * (src/gfx.h, mono_char) — genuinely fixed-width, which is the property
 * a monospace class is asked for. Everything else is the TrueType face
 * in src/ttf.h.
 *
 * The deviation: the bitmap face has one size, so `font-mono text-sm`
 * picks the integer scale nearest the requested pixel size rather than
 * that size exactly. Fourteen-pixel text asks for scale 2, which is
 * sixteen pixels tall. The alternative is a proportional face for a
 * class that means "not proportional", which would be worse.
 *
 * `font-bold` has no bold face to reach for. It is drawn by stamping the
 * glyph twice, one pixel apart — which is what a rasteriser without a
 * bold weight has always done, and is visibly heavier without being a
 * second font.
 */

#include <stdint.h>

/* ---- what the generator emits ----
 *
 * The field names here are the ones tools/tailwind.py writes as
 * designated initialisers. The two must agree exactly; a rename on one
 * side is a compile error on the other, which is the intended safety
 * net.
 */
typedef struct {
    int16_t  first_child;      /* index, or -1                        */
    int16_t  child_count;
    int16_t  next_sibling;     /* index, or -1                        */
    uint16_t flags;            /* VXUI_F_*                            */
    int16_t  flex;             /* flex-N; 0 means "not flexible"      */
    int16_t  w, h;             /* fixed size in pixels, or -1 = auto  */
    int16_t  pad[4];           /* top, right, bottom, left            */
    int16_t  mar[4];
    int16_t  gap;
    uint8_t  has_bg;
    uint32_t bg;
    uint8_t  bg_alpha;         /* 255 unless a /N opacity was given   */
    uint8_t  has_fg;
    uint32_t fg;
    uint8_t  border_sides;     /* VXUI_SIDE_* bitmask                 */
    uint32_t border;
    uint8_t  radius;
    uint8_t  font_size;
    int8_t   track;            /* letter spacing, pixels              */
    uint32_t grad[3];          /* from, via, to                       */
    const char *text;
} vxui_node_t;

#define VXUI_SIDE_T 1
#define VXUI_SIDE_R 2
#define VXUI_SIDE_B 4
#define VXUI_SIDE_L 8

#define VXUI_F_FLEX_ROW   (1u << 0)
#define VXUI_F_FLEX_COL   (1u << 1)
#define VXUI_F_ITEMS_CTR  (1u << 2)
#define VXUI_F_JUST_BETW  (1u << 3)
#define VXUI_F_JUST_CTR   (1u << 4)
#define VXUI_F_JUST_END   (1u << 5)
#define VXUI_F_FONT_MONO  (1u << 6)
#define VXUI_F_FONT_BOLD  (1u << 7)
#define VXUI_F_WIDTH_FULL (1u << 8)
#define VXUI_F_GRAD_TEXT  (1u << 9)
#define VXUI_F_SHADOW_IN  (1u << 10)
#define VXUI_F_TEXT_TRANS (1u << 11)

typedef struct { int16_t x, y, w, h; } vxui_rect_t;

#include "ui/vxui_gen.h"

/* Where the solver put each node this frame. Indexed the same as
 * vxui_nodes, so a caller with a VXUI_ID_* constant can ask where its
 * rectangle ended up — which is how the address field knows where to
 * draw its text and where a click lands. */
static vxui_rect_t vxui_rects[VXUI_NODE_COUNT];

/* ============================================================
 *  measuring
 * ============================================================ */

/* The advance of the mono face nearest a requested pixel size. The face
 * is 8 pixels and scales by whole numbers only. */
static int vxui_mono_scale(int px) {
    int s = (px + 4) / 8;
    return s < 1 ? 1 : s;
}

static int vxui_text_w(const vxui_node_t *n, const char *s) {
    if (!s || !*s) return 0;
    if (n->flags & VXUI_F_FONT_MONO) {
        int len = 0;
        while (s[len]) len++;
        return len * MONO_ADV(vxui_mono_scale(n->font_size ? n->font_size : 14));
    }
    return ttf_text_width(s, n->font_size ? n->font_size : 14);
}

static int vxui_text_h(const vxui_node_t *n) {
    const int px = n->font_size ? n->font_size : 14;
    if (n->flags & VXUI_F_FONT_MONO) return 8 * vxui_mono_scale(px);
    return px;
}

/*
 * How much room a node wants along the main axis of its parent, before
 * any flexing. A fixed width wins; otherwise its text, plus padding.
 */
static int vxui_natural_w(int idx, const char *text_override) {
    const vxui_node_t *n = &vxui_nodes[idx];
    if (n->w >= 0) return n->w;
    const char *t = text_override ? text_override : n->text;
    int inner = vxui_text_w(n, t);

    /* A container with no text of its own is as wide as its children. */
    if (n->first_child >= 0) {
        int sum = 0, count = 0;
        for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
            sum += vxui_natural_w(c, 0) +
                   vxui_nodes[c].mar[3] + vxui_nodes[c].mar[1];
            count++;
        }
        if (count > 1) sum += n->gap * (count - 1);
        if (sum > inner) inner = sum;
    }
    return inner + n->pad[3] + n->pad[1];
}

static int vxui_natural_h(int idx) {
    const vxui_node_t *n = &vxui_nodes[idx];
    if (n->h >= 0) return n->h;
    int inner = n->text ? vxui_text_h(n) : 0;

    if (n->first_child >= 0) {
        if (n->flags & VXUI_F_FLEX_ROW) {
            for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
                const int ch = vxui_natural_h(c) +
                               vxui_nodes[c].mar[0] + vxui_nodes[c].mar[2];
                if (ch > inner) inner = ch;
            }
        } else {
            int sum = 0, count = 0;
            for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
                sum += vxui_natural_h(c) +
                       vxui_nodes[c].mar[0] + vxui_nodes[c].mar[2];
                count++;
            }
            if (count > 1) sum += n->gap * (count - 1);
            inner = sum;
        }
    }
    return inner + n->pad[0] + n->pad[2];
}

/* ============================================================
 *  the solve
 * ============================================================ */

static void vxui_solve(int idx, int x, int y, int w, int h);

static void vxui_solve_row(const vxui_node_t *n, int cx, int cy, int cw, int ch) {
    int fixed = 0, total_flex = 0, count = 0;
    for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
        const vxui_node_t *k = &vxui_nodes[c];
        count++;
        if (k->flex > 0) total_flex += k->flex;
        else fixed += vxui_natural_w(c, 0);
        fixed += k->mar[3] + k->mar[1];
    }
    if (count > 1) fixed += n->gap * (count - 1);

    int slack = cw - fixed;
    if (slack < 0) slack = 0;

    /* justify-* only has anything to distribute when nothing is
     * flexible: a flex child has already eaten the slack. */
    int lead = 0, between = 0;
    if (total_flex == 0 && count > 0) {
        if (n->flags & VXUI_F_JUST_BETW) {
            between = count > 1 ? slack / (count - 1) : 0;
        } else if (n->flags & VXUI_F_JUST_CTR) {
            lead = slack / 2;
        } else if (n->flags & VXUI_F_JUST_END) {
            lead = slack;
        }
    }

    int pen = cx + lead;
    int given = 0, seen = 0;
    for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
        const vxui_node_t *k = &vxui_nodes[c];
        seen++;

        int kw;
        if (k->flex > 0) {
            /* The last flexible child takes whatever integer division
             * left behind, so the row fills its container exactly rather
             * than ending a pixel or two short. */
            kw = (total_flex ? slack * k->flex / total_flex : 0);
            given += kw;
            if (seen == count) kw += slack - given;
        } else {
            kw = vxui_natural_w(c, 0);
        }

        int kh = (k->h >= 0) ? k->h : vxui_natural_h(c);
        if (kh > ch - k->mar[0] - k->mar[2]) kh = ch - k->mar[0] - k->mar[2];

        int ky = cy + k->mar[0];
        if (n->flags & VXUI_F_ITEMS_CTR) ky = cy + (ch - kh) / 2;

        vxui_solve(c, pen + k->mar[3], ky, kw, kh);
        pen += k->mar[3] + kw + k->mar[1] + n->gap + between;
    }
}

static void vxui_solve_col(const vxui_node_t *n, int cx, int cy, int cw, int ch) {
    (void)ch;
    int pen = cy;
    for (int c = n->first_child; c >= 0; c = vxui_nodes[c].next_sibling) {
        const vxui_node_t *k = &vxui_nodes[c];

        int kw = cw - k->mar[3] - k->mar[1];
        if (k->w >= 0 && !(k->flags & VXUI_F_WIDTH_FULL)) kw = k->w;

        const int kh = (k->h >= 0) ? k->h : vxui_natural_h(c);

        pen += k->mar[0];
        vxui_solve(c, cx + k->mar[3], pen, kw, kh);
        pen += kh + k->mar[2] + n->gap;
    }
}

/* Place `idx` at this rectangle and lay its subtree out inside it. */
static void vxui_solve(int idx, int x, int y, int w, int h) {
    if (idx < 0 || idx >= VXUI_NODE_COUNT) return;
    const vxui_node_t *n = &vxui_nodes[idx];

    vxui_rects[idx].x = (int16_t)x;
    vxui_rects[idx].y = (int16_t)y;
    vxui_rects[idx].w = (int16_t)w;
    vxui_rects[idx].h = (int16_t)h;

    if (n->first_child < 0) return;

    const int cx = x + n->pad[3];
    const int cy = y + n->pad[0];
    int cw = w - n->pad[3] - n->pad[1];
    int chh = h - n->pad[0] - n->pad[2];
    if (cw < 0) cw = 0;
    if (chh < 0) chh = 0;

    if (n->flags & VXUI_F_FLEX_ROW) vxui_solve_row(n, cx, cy, cw, chh);
    else                            vxui_solve_col(n, cx, cy, cw, chh);
}

/* ============================================================
 *  painting
 * ============================================================ */

/*
 * A filled rectangle with rounded corners.
 *
 * Per-row insets rather than a corner mask: for each row inside the
 * corner band, how far in the edge sits is the horizontal offset of a
 * circle of the given radius. Two multiplications and a comparison per
 * row, no square root, and no table.
 */
static void vxui_fill_round(uint32_t *buf, uint32_t bw, uint32_t bh,
                            int x, int y, int w, int h, int r,
                            uint32_t col, uint8_t alpha) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int row = 0; row < h; row++) {
        int inset = 0;
        if (r > 0) {
            const int dy = (row < r) ? (r - 1 - row)
                         : (row >= h - r) ? (row - (h - r))
                         : -1;
            if (dy >= 0) {
                inset = r;
                for (int k = 0; k <= r; k++) {
                    if (k * k + dy * dy <= r * r) { inset = r - k; break; }
                }
            }
        }
        const int rx = x + inset;
        const int rw = w - 2 * inset;
        if (rw <= 0) continue;
        if (alpha >= 255) gfx_rect(buf, bw, bh, rx, y + row, rw, 1, col);
        else gfx_rect_blend(buf, bw, bh, rx, y + row, rw, 1, col, alpha);
    }
}

/* One pixel of border on whichever sides were asked for. The corners of
 * a rounded box are drawn by the same per-row logic as the fill, one
 * pixel wide, so a rounded border follows its own fill exactly. */
static void vxui_stroke(uint32_t *buf, uint32_t bw, uint32_t bh,
                        int x, int y, int w, int h, int r,
                        uint8_t sides, uint32_t col) {
    if (w <= 0 || h <= 0 || !sides) return;

    if (r <= 0) {
        if (sides & VXUI_SIDE_T) gfx_rect(buf, bw, bh, x, y, w, 1, col);
        if (sides & VXUI_SIDE_B) gfx_rect(buf, bw, bh, x, y + h - 1, w, 1, col);
        if (sides & VXUI_SIDE_L) gfx_rect(buf, bw, bh, x, y, 1, h, col);
        if (sides & VXUI_SIDE_R) gfx_rect(buf, bw, bh, x + w - 1, y, 1, h, col);
        return;
    }

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int row = 0; row < h; row++) {
        int inset = 0;
        int in_corner = 0;
        const int dy = (row < r) ? (r - 1 - row)
                     : (row >= h - r) ? (row - (h - r)) : -1;
        if (dy >= 0) {
            in_corner = 1;
            inset = r;
            for (int k = 0; k <= r; k++) {
                if (k * k + dy * dy <= r * r) { inset = r - k; break; }
            }
        }
        const int rx = x + inset;
        const int rw = w - 2 * inset;
        if (rw <= 0) continue;

        if (in_corner) {
            /* In the corner band every row contributes its two end
             * pixels, which together trace the arc. */
            if (sides & (VXUI_SIDE_L | VXUI_SIDE_T | VXUI_SIDE_B))
                gfx_rect(buf, bw, bh, rx, y + row, 1, 1, col);
            if (sides & (VXUI_SIDE_R | VXUI_SIDE_T | VXUI_SIDE_B))
                gfx_rect(buf, bw, bh, rx + rw - 1, y + row, 1, 1, col);
        } else {
            if (sides & VXUI_SIDE_L) gfx_rect(buf, bw, bh, rx, y + row, 1, 1, col);
            if (sides & VXUI_SIDE_R)
                gfx_rect(buf, bw, bh, rx + rw - 1, y + row, 1, 1, col);
        }
        if (row == 0 && (sides & VXUI_SIDE_T))
            gfx_rect(buf, bw, bh, rx, y, rw, 1, col);
        if (row == h - 1 && (sides & VXUI_SIDE_B))
            gfx_rect(buf, bw, bh, rx, y + h - 1, rw, 1, col);
    }
}

/* Two blended lines inside the top and left edges. What an inset shadow
 * amounts to at this scale: enough to read as recessed, and cheaper than
 * a blur. */
static void vxui_shadow_inner(uint32_t *buf, uint32_t bw, uint32_t bh,
                              int x, int y, int w, int h) {
    if (w <= 2 || h <= 2) return;
    gfx_rect_blend(buf, bw, bh, x + 1, y + 1, w - 2, 1, 0x000000u, 90);
    gfx_rect_blend(buf, bw, bh, x + 1, y + 2, w - 2, 1, 0x000000u, 45);
    gfx_rect_blend(buf, bw, bh, x + 1, y + 1, 1, h - 2, 0x000000u, 70);
}

/* Two colours mixed, for the gradient below. */
static uint32_t vxui_lerp(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    const int t = num * 255 / den;
    return gfx_mix(b, a, (uint8_t)(255 - t));
}

/*
 * A gradient across three stops, at position num/den.
 *
 * from -> via over the first half, via -> to over the second, which is
 * what `from-X via-Y to-Z` means and where the extra stop earns its
 * keep: two stops between gold-light and gold-dark passes through a
 * muddy olive, and the third pins the middle to the signature gold.
 */
static uint32_t vxui_grad_at(const uint32_t *stops, int num, int den) {
    if (den <= 0) return stops[0];
    const int half = den / 2;
    if (num <= half) return vxui_lerp(stops[0], stops[1], num, half ? half : 1);
    return vxui_lerp(stops[1], stops[2], num - half, den - half);
}

static void vxui_draw_text(uint32_t *buf, uint32_t bw, uint32_t bh,
                           const vxui_node_t *n, const char *s,
                           int x, int y) {
    if (!s || !*s) return;
    const int px = n->font_size ? n->font_size : 14;
    const uint32_t col = n->has_fg ? n->fg : 0xFFFFFFu;

    /*
     * The plain path: one call, no per-glyph work. Taken whenever the
     * node asks for neither a gradient nor letter spacing, which is
     * almost every node — and it matters, because the per-glyph path
     * below is a call per character.
     */
    if (!(n->flags & VXUI_F_GRAD_TEXT) && n->track == 0) {
        if (n->flags & VXUI_F_FONT_MONO) {
            const int sc = vxui_mono_scale(px);
            int cx = x;
            for (const char *p = s; *p; p++) {
                mono_char(buf, bw, bh, cx, y, *p, col, sc);
                cx += MONO_ADV(sc);
            }
        } else {
            ttf_draw_string(buf, (int)bw, (int)bh, x, y, s, col, px);
            if (n->flags & VXUI_F_FONT_BOLD)
                ttf_draw_string(buf, (int)bw, (int)bh, x + 1, y, s, col, px);
        }
        return;
    }

    /*
     * The gradient path: one glyph at a time, with the colour taken from
     * the glyph's own position across the run.
     *
     * This is what `text-transparent bg-clip-text bg-gradient-to-r`
     * means — the text is a mask over a gradient — and it is the only
     * way to get it without a compositing layer the rasteriser does not
     * have. The advance comes from ttf_text_width of the prefix, so the
     * spacing is the face's own rather than a guess.
     */
    const int total = vxui_text_w(n, s);
    char prefix[128];
    int i = 0;
    int cx = x;

    for (const char *p = s; *p && i < (int)sizeof(prefix) - 1; p++, i++) {
        const uint32_t c = (n->flags & VXUI_F_GRAD_TEXT)
                         ? vxui_grad_at(n->grad, cx - x, total ? total : 1)
                         : col;

        if (n->flags & VXUI_F_FONT_MONO) {
            const int sc = vxui_mono_scale(px);
            mono_char(buf, bw, bh, cx, y, *p, c, sc);
            cx += MONO_ADV(sc) + n->track;
        } else {
            const char one[2] = { *p, '\0' };
            ttf_draw_string(buf, (int)bw, (int)bh, cx, y, one, c, px);
            if (n->flags & VXUI_F_FONT_BOLD)
                ttf_draw_string(buf, (int)bw, (int)bh, cx + 1, y, one, c, px);
            cx += ttf_text_width(one, px) + n->track;
        }
    }
}

/*
 * Paint one node. The subtree is *not* painted — the caller walks, so
 * that it can substitute text (the address bar's contents, the
 * encyclopedia's figures) node by node.
 */
static void vxui_paint_node(uint32_t *buf, uint32_t bw, uint32_t bh,
                            int idx, const char *text_override) {
    if (idx < 0 || idx >= VXUI_NODE_COUNT) return;
    const vxui_node_t *n = &vxui_nodes[idx];
    const vxui_rect_t r = vxui_rects[idx];
    if (r.w <= 0 || r.h <= 0) return;

    if (n->has_bg)
        vxui_fill_round(buf, bw, bh, r.x, r.y, r.w, r.h, n->radius,
                        n->bg, n->bg_alpha);

    if (n->flags & VXUI_F_SHADOW_IN)
        vxui_shadow_inner(buf, bw, bh, r.x, r.y, r.w, r.h);

    if (n->border_sides)
        vxui_stroke(buf, bw, bh, r.x, r.y, r.w, r.h, n->radius,
                    n->border_sides, n->border);

    const char *t = text_override ? text_override : n->text;
    if (t && *t) {
        const int th = vxui_text_h(n);
        int tx = r.x + n->pad[3];
        int ty = r.y + n->pad[0];

        /* Centred on the cross axis when the node asked for it, which
         * for a single line of text is what items-center means. */
        if (n->flags & VXUI_F_ITEMS_CTR) ty = r.y + (r.h - th) / 2;
        if (n->flags & VXUI_F_JUST_CTR) {
            const int tw = vxui_text_w(n, t);
            tx = r.x + (r.w - tw) / 2;
        }
        vxui_draw_text(buf, bw, bh, n, t, tx, ty);
    }
}

/* Paint a whole subtree, in tree order so a child lands on top of its
 * parent's background. */
static void vxui_paint_tree(uint32_t *buf, uint32_t bw, uint32_t bh, int idx) {
    if (idx < 0) return;
    vxui_paint_node(buf, bw, bh, idx, 0);
    for (int c = vxui_nodes[idx].first_child; c >= 0;
         c = vxui_nodes[c].next_sibling)
        vxui_paint_tree(buf, bw, bh, c);
}

#endif /* VXUI_H */
