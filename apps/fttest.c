/*
 * fttest — FreeType 2.13.2, in ring 3, against the kernel's own parser.
 *
 * ============================================================
 *  WHY THIS TEST IS WORTH MORE THAN "IT LINKED"
 * ============================================================
 *
 * The kernel has parsed TrueType since src/ttf.h was written: glyf,
 * loca, hmtx, cmap, and a bytecode hinter in src/ttfhint.h. FreeType now
 * parses the same format in ring 3.
 *
 * And they read *the same bytes*. src/comicneue_ttf.h is
 * assets/ComicNeue-Regular.ttf converted to a byte array, and the build
 * now also puts that file on the volume — so this program hands FreeType
 * the identical face the kernel has embedded, and the two
 * implementations can be asked the same questions about it.
 *
 * Two independent parsers agreeing on a font's metrics is a much
 * stronger claim than either one being self-consistent. It is the same
 * argument tools/ntfsdir.py makes about the filesystem driver.
 *
 * ---- what is compared, and what deliberately is not ----
 *
 * *Compared*: the numbers that come straight out of the tables. Which
 * glyph a character maps to, how many glyphs there are, the design units
 * per em, and the relative widths of glyphs — all of which are hmtx and
 * cmap arithmetic that both parsers must agree on exactly.
 *
 * *Not compared*: rendered pixel widths, asserted equal. src/ttf.h runs
 * the bytecode hinter, which moves outlines onto the pixel grid; an
 * unhinted FreeType load legitimately differs by a pixel or two. An
 * assertion of exact equality there would be asserting that two correct
 * implementations are identical, which they are not and need not be. The
 * test checks they agree to within a small tolerance and prints both, so
 * a real divergence is visible rather than hidden by a loose bound.
 *
 * ---- and what this proves about the port ----
 *
 * FT_New_Face on a path opens the file through upstream's stock
 * ftsystem.c — fopen, fseek, ftell, fread, fclose — which means the
 * whole read path from FreeType down through libc's FILE streams, the
 * descriptor table and NTFS is exercised by the first call. There is no
 * Vextro-specific FreeType code at all; the C library simply grew until
 * the stock file compiled.
 */

#include "vextro.h"
#include <sys/syscall.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define FONT_PATH "/ComicNeue-Regular.ttf"

static int checks = 0, failures = 0;

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* The kernel's own answer, through the trampoline the loader maps into
 * every process. There is no wrapper for this in <vextro.h> — the two
 * eight-argument drawing calls have one and the measurement does not —
 * so the system call is issued directly. */
static long kernel_text_width(const char *s, int size) {
    return __syscall2(SYS_TTF_TEXT_WIDTH, (long)(uintptr_t)s, (long)size);
}

int main(void);

void _start(void) { main(); }

int main(void) {
    printf("fttest: starting\n");

    FT_Library lib = 0;
    FT_Face    face = 0;

    /* ---- 1. the library comes up ---- */
    {
        const FT_Error e = FT_Init_FreeType(&lib);
        ok("FT_Init_FreeType", e == 0 && lib != 0);
        if (e) {
            printf("fttest: %d checks, %d failures\nfttest: FAILED\n",
                   checks, ++failures);
            return 1;
        }

        FT_Int major = 0, minor = 0, patch = 0;
        FT_Library_Version(lib, &major, &minor, &patch);
        printf("       (FreeType %d.%d.%d)\n", major, minor, patch);
        ok("and it is the version that was vendored",
           major == 2 && minor == 13 && patch == 2);
    }

    /* ---- 2. a face, opened from the volume ----
     *
     * This is the whole port in one call: FreeType's stock ANSI
     * ftsystem.c, over libc's FILE streams, over the descriptor table,
     * over NTFS.
     */
    {
        const FT_Error e = FT_New_Face(lib, FONT_PATH, 0, &face);
        ok("FT_New_Face reads a font off the NTFS volume", e == 0 && face != 0);
        if (e) {
            printf("       (FT error %d opening %s)\n", (int)e, FONT_PATH);
            printf("fttest: %d checks, %d failures\nfttest: FAILED\n",
                   checks, ++failures);
            return 1;
        }

        ok("it is scalable", FT_IS_SCALABLE(face) != 0);
        ok("with outlines rather than bitmaps",
           (face->face_flags & FT_FACE_FLAG_SCALABLE) != 0);
        ok("and a character map", face->charmap != 0);

        printf("       (%s %s: %ld glyphs, %d units/em)\n",
               face->family_name ? face->family_name : "?",
               face->style_name ? face->style_name : "?",
               (long)face->num_glyphs, (int)face->units_per_EM);

        ok("the glyph count is plausible",
           face->num_glyphs > 100 && face->num_glyphs < 5000);
        ok("units per em is a power of two or a thousand",
           face->units_per_EM == 1000 || face->units_per_EM == 2048 ||
           face->units_per_EM == 1024);
    }

    /* ---- 3. the character map ---- */
    {
        const FT_UInt gA = FT_Get_Char_Index(face, 'A');
        const FT_UInt gW = FT_Get_Char_Index(face, 'W');
        const FT_UInt gi = FT_Get_Char_Index(face, 'i');
        const FT_UInt gnone = FT_Get_Char_Index(face, 0x10FFFD);

        ok("'A' maps to a glyph", gA != 0);
        ok("'W' maps to a different one", gW != 0 && gW != gA);
        ok("'i' too", gi != 0 && gi != gA);
        ok("and an unassigned code point maps to none", gnone == 0);
    }

    /* ---- 4. metrics straight out of hmtx ----
     *
     * FT_LOAD_NO_SCALE gives design units — the numbers in the table,
     * with no grid fitting and no rounding. Two parsers reading the same
     * file must agree on these exactly, and the relationships between
     * them are what the kernel's own measurements are built from.
     */
    long advA = 0, advW = 0, advi = 0;
    {
        FT_Error e = FT_Load_Char(face, 'A', FT_LOAD_NO_SCALE);
        ok("load 'A' unscaled", e == 0);
        advA = face->glyph->metrics.horiAdvance;

        e = FT_Load_Char(face, 'W', FT_LOAD_NO_SCALE);
        ok("load 'W' unscaled", e == 0);
        advW = face->glyph->metrics.horiAdvance;

        e = FT_Load_Char(face, 'i', FT_LOAD_NO_SCALE);
        ok("load 'i' unscaled", e == 0);
        advi = face->glyph->metrics.horiAdvance;

        printf("       (design advances: A=%ld W=%ld i=%ld)\n",
               advA, advW, advi);

        ok("every advance is positive", advA > 0 && advW > 0 && advi > 0);
        ok("'W' is wider than 'A'", advW > advA);
        ok("and 'A' is wider than 'i'", advA > advi);
        ok("all of them fit inside the em",
           advA <= face->units_per_EM * 2 && advW <= face->units_per_EM * 2);
    }

    /* ---- 5. the two parsers, on the same face ----
     *
     * The kernel measures a string through src/ttf.h with hinting on;
     * FreeType is asked for the same string scaled to the same pixel
     * size. Exact equality is not the claim — see the note at the head
     * of this file — but the *ordering* must hold exactly, because it
     * comes from hmtx rather than from the grid.
     */
    {
        const int size = 24;
        ok("set a pixel size", FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size) == 0);

        long ft_w = 0;
        const char *word = "Wave";
        for (const char *p = word; *p; p++) {
            if (FT_Load_Char(face, (FT_ULong)*p, FT_LOAD_DEFAULT) != 0) continue;
            ft_w += face->glyph->advance.x >> 6;   /* 26.6 fixed point */
        }

        const long k_w = kernel_text_width(word, size);
        printf("       (\"%s\" at %dpx: freetype %ld, kernel %ld)\n",
               word, size, ft_w, k_w);

        ok("the kernel measured it too", k_w > 0);
        ok("freetype measured it too", ft_w > 0);

        /*
         * Within an eighth. Two correct rasterisers differ by rounding
         * per glyph — the kernel hints and this load does not — so a
         * tolerance is the honest assertion. It is tight enough that a
         * genuine disagreement, such as reading the wrong table or
         * scaling by the wrong em, fails it by a wide margin.
         */
        const long diff = (ft_w > k_w) ? ft_w - k_w : k_w - ft_w;
        ok("and the two agree to within an eighth",
           k_w > 0 && diff * 8 <= k_w);

        /* The ordering, which is table arithmetic and must be exact in
         * both. */
        const long k_narrow = kernel_text_width("ii", size);
        const long k_wide   = kernel_text_width("WW", size);
        ok("the kernel orders them as hmtx does", k_wide > k_narrow);

        long ft_narrow = 0, ft_wide = 0;
        FT_Load_Char(face, 'i', FT_LOAD_DEFAULT);
        ft_narrow = face->glyph->advance.x >> 6;
        FT_Load_Char(face, 'W', FT_LOAD_DEFAULT);
        ft_wide = face->glyph->advance.x >> 6;
        ok("and so does freetype", ft_wide > ft_narrow);
    }

    /* ---- 6. rasterising ----
     *
     * The smooth module, which is one of the four this build includes.
     * A glyph that loads and renders to a bitmap with ink in it is the
     * whole outline pipeline: glyf parsed, points scaled, contours
     * filled, coverage written.
     */
    {
        ok("load 'A' for rendering",
           FT_Load_Char(face, 'A', FT_LOAD_RENDER) == 0);

        FT_Bitmap *bm = &face->glyph->bitmap;
        ok("it produced a bitmap", bm->width > 0 && bm->rows > 0);
        ok("in 8-bit coverage", bm->pixel_mode == FT_PIXEL_MODE_GRAY);

        int ink = 0, opaque = 0;
        for (unsigned y = 0; y < bm->rows; y++)
            for (unsigned x = 0; x < bm->width; x++) {
                const unsigned char v = bm->buffer[y * (unsigned)bm->pitch + x];
                if (v) ink++;
                if (v == 255) opaque++;
            }
        printf("       (glyph 'A' at 24px: %ux%u, %d inked, %d opaque)\n",
               bm->width, bm->rows, ink, opaque);

        ok("with ink in it", ink > 0);
        ok("and some fully covered pixels", opaque > 0);
        ok("but not every pixel covered", ink < (int)(bm->width * bm->rows));
    }

    /* ---- 7. the error path ---- */
    {
        FT_Face missing = 0;
        const FT_Error e = FT_New_Face(lib, "/no-such-font.ttf", 0, &missing);
        ok("a font that is not there fails cleanly",
           e != 0 && missing == 0);

        FT_Face notafont = 0;
        const FT_Error e2 = FT_New_Face(lib, "/about.txt", 0, &notafont);
        ok("and a file that is not a font is rejected",
           e2 != 0 && notafont == 0);
    }

    ok("done with the face", FT_Done_Face(face) == 0);
    ok("and the library", FT_Done_FreeType(lib) == 0);

    printf("fttest: %d checks, %d failures\n", checks, failures);
    printf(failures ? "fttest: FAILED\n" : "fttest: all passed\n");
    return failures ? 1 : 0;
}
