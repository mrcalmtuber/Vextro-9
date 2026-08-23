/*
 * tools/ttfhint_test.c — does the hinting actually hint?
 *
 * A bytecode interpreter is unusually easy to ship broken. Every failure
 * path in src/ttfhint.h falls back to the unhinted outline, which is the
 * right behaviour and also means a version that errors on every single
 * glyph compiles, runs, draws readable text, and is completely inert.
 * "It builds and the screen looks fine" cannot distinguish the two.
 *
 * So this measures instead of asserting:
 *
 *   1. the font program and the pre-program run, at every size the UI
 *      uses, without the interpreter reporting an error;
 *   2. every glyph's own program runs the same way;
 *   3. hinting *changes* the outline -- a no-op would pass (1) and (2);
 *   4. and the change is the one claimed: on-curve points land on whole
 *      pixel boundaries far more often than the unhinted outline puts
 *      them there.
 *
 * (4) is the whole point of grid-fitting stated as a number. An outline
 * scaled and not hinted lands on a pixel boundary only by chance, and
 * the rate is what chance predicts. If running the bytecode does not
 * move that rate, the bytecode is not doing anything.
 *
 * Runs on the host against the same headers the kernel compiles.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* The only thing src/ttf.h needs from the kernel. */
static void serial_puts(const char *s) { printf("      [ttf] %s", s); }

#include "../src/ttf.h"

static int checks = 0, fails = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n", what); }
}

static void okf(const char *what, int cond, const char *fmt, long a, long b) {
    checks++;
    printf(cond ? "  ok    " : "  FAIL  ");
    printf("%s  (", what);
    printf(fmt, a, b);
    printf(")\n");
    if (!cond) fails++;
}

/* The sizes this interface is actually set in, plus a couple either
 * side. Hinting matters most at the small end and this is the small
 * end. */
static const int SIZES[] = { 9, 10, 11, 12, 13, 14, 16, 20, 24 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

/*
 * Stage and hint one glyph, and report how its on-curve y coordinates
 * sit relative to the pixel grid.
 *
 * Only y, and only because only y is hinted: this face is ttfautohint
 * output and carries no x instructions at all. Counting x would measure
 * the absence of a feature in the font rather than the presence of one
 * in the interpreter.
 */
static void measure(int gid, int size, int hinted,
                    int *on_grid, int *total, int *moved)
{
    F_hinting = hinted;
    H_size_ready = 0;               /* force prep for this configuration */
    H_ppem = -1;

    MULN = (int64_t)size * TTF_SS;
    PENX = 0; BASEY = 0;
    hint_prepare_size(size);

    nedges = 0;
    S_device = 0;
    decode_glyph(gid, 0, 0, 0);

    if (!hinted) {
        /* Unhinted: S_ys still holds font units, so scale them the same
         * way the interpreter would have and ask the same question. */
        int n = TH_npts;                       /* left from a previous run */
        (void)n;
        return;
    }

    for (int i = 0; i < TH_npts - 4; i++) {
        if (!(TH_tag[i] & TT_ON_CURVE)) continue;
        (*total)++;
        if ((TH_cur_y[i] & 63) == 0) (*on_grid)++;
        if (TH_cur_y[i] != TH_org_y[i]) (*moved)++;
    }
}

int main(void) {
    printf("TrueType hinting\n\n");

    ok("the face parses", ttf_init() == 1);
    ok("it carries a font program", T_fpgm != 0 && L_fpgm > 0);
    ok("  and a control value table", T_cvt != 0 && L_cvt > 0);
    ok("  and a pre-program", T_prep != 0 && L_prep > 0);
    ok("the font program ran, so hinting is on", F_hinting == 1);

    if (!F_hinting) {
        printf("\n  hinting is off; nothing below can be measured\n");
        printf("\n%d checks, %d failures\n", checks, fails);
        return 1;
    }

    /* ---- the pre-program, at every size ---- */
    {
        int bad = 0;
        for (int i = 0; i < NSIZES; i++) {
            H_size_ready = 0; H_ppem = -1;
            hint_prepare_size(SIZES[i]);
            if (!H_size_ready || !F_hinting) bad++;
        }
        okf("the pre-program runs at every size", bad == 0,
            "%ld sizes, %ld refused", (long)NSIZES, (long)bad);
    }

    /* ---- every glyph's own program ---- */
    {
        TT_n_hinted = TT_n_failed = 0;
        for (int i = 0; i < NSIZES; i++) {
            for (int c = 32; c < 127; c++) {
                int gid = glyph_index((uint32_t)c);
                if (!gid) continue;
                F_hinting = 1;
                H_size_ready = 0; H_ppem = -1;
                MULN = (int64_t)SIZES[i] * TTF_SS;
                PENX = 0; BASEY = 0;
                hint_prepare_size(SIZES[i]);
                nedges = 0; S_device = 0;
                decode_glyph(gid, 0, 0, 0);
            }
        }
        okf("every glyph program runs without an interpreter error",
            TT_n_failed == 0, "%ld hinted, %ld failed",
            (long)TT_n_hinted, (long)TT_n_failed);
    }

    /*
     * ---- the measurement ----
     *
     * For each size: how many on-curve y coordinates sit exactly on a
     * pixel boundary? Unhinted, the answer is whatever the design
     * happens to give -- a few percent. Hinted, the stem edges the
     * program cares about are placed there deliberately.
     */
    printf("\n  on-curve Y coordinates landing exactly on a pixel boundary\n");
    printf("    px     unhinted        hinted\n");

    long tot_un_grid = 0, tot_un_all = 0, tot_h_grid = 0, tot_h_all = 0;
    long any_moved = 0;

    for (int i = 0; i < NSIZES; i++) {
        int size = SIZES[i];
        long ug = 0, ua = 0, hg = 0, ha = 0, mv = 0;

        for (int c = 32; c < 127; c++) {
            int gid = glyph_index((uint32_t)c);
            if (!gid) continue;

            /* Unhinted: scale the design coordinates and ask the same
             * question of them. Nothing has moved the points, so this is
             * the base rate -- what chance alone produces. */
            F_hinting = 0;
            H_size_ready = 0; H_ppem = -1;
            MULN = (int64_t)size * TTF_SS;
            PENX = 0; BASEY = 0;
            nedges = 0; S_device = 0;
            decode_glyph(gid, 0, 0, 0);

            int32_t sc = (int32_t)(((int64_t)size * 64 * 65536) / F_upem);
            int npts = 0;
            for (int c2 = 0; c2 < 64; c2++) { (void)c2; break; }
            /* S_ends/S_xs/S_ys hold the last simple glyph decoded. */
            npts = 0;
            {
                uint32_t glen; uint32_t go = glyf_offset(gid, &glen);
                if (glen == 0) continue;
                int nc = sbe16(go);
                if (nc < 0) continue;              /* composite: skip */
                npts = S_ends[nc - 1] + 1;
            }
            for (int k = 0; k < npts; k++) {
                if (!(S_flags[k] & 1)) continue;
                int32_t y = (int32_t)(((int64_t)S_ys[k] * sc) >> 16);
                ua++;
                if ((y & 63) == 0) ug++;
            }

            /* Hinted. */
            int g = 0, t = 0, m = 0;
            measure(gid, size, 1, &g, &t, &m);
            hg += g; ha += t; mv += m;
        }

        tot_un_grid += ug; tot_un_all += ua;
        tot_h_grid  += hg; tot_h_all  += ha;
        any_moved   += mv;

        printf("    %2d   %5ld/%-5ld %4ld%%   %5ld/%-5ld %4ld%%\n",
               size, ug, ua, ua ? ug * 100 / ua : 0,
               hg, ha, ha ? hg * 100 / ha : 0);
    }

    long un_pc = tot_un_all ? tot_un_grid * 100 / tot_un_all : 0;
    long h_pc  = tot_h_all  ? tot_h_grid  * 100 / tot_h_all  : 0;

    printf("\n");
    okf("hinting moves points at all (a no-op would not)", any_moved > 0,
        "%ld coordinates moved, %ld inspected", any_moved, tot_h_all);

    okf("hinted outlines land on the pixel grid far more often",
        h_pc >= un_pc * 2 && h_pc > 15,
        "unhinted %ld%%, hinted %ld%%", un_pc, h_pc);

    /*
     * ---- and a picture, because a number can be right about the wrong
     * thing ----
     *
     * A capital E at 11 px. Its three horizontal bars are exactly what
     * grid-fitting is for: unhinted they land wherever the maths puts
     * them and smear across two rows each, hinted they sit inside one.
     */
    for (int pass = 0; pass < 2; pass++) {
        F_hinting = pass;
        H_size_ready = 0; H_ppem = -1;
        for (int s = 0; s < TTF_CACHE_SLOTS; s++) G_slot[s].key = 0;
        G_pool_used = 0;

        MULN = (int64_t)11 * TTF_SS;
        int gid = glyph_index('E');
        const glyph_slot_t *g = glyph_mask(gid, 11);
        const uint8_t *m = g ? G_pool + g->off : COV;
        int w = g ? g->w : COV_w, h = g ? g->h : COV_h;

        printf("\n  'E' at 11 px, %s:\n", pass ? "hinted" : "unhinted");
        for (int y = 0; y < h; y++) {
            printf("      ");
            for (int x = 0; x < w; x++) {
                int a = m[y * w + x];
                putchar(a > 200 ? '#' : a > 128 ? '+' : a > 48 ? '.' : ' ');
            }
            printf("\n");
        }
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
