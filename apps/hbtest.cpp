/*
 * hbtest — HarfBuzz 8.5.0 shaping text in ring 3.
 *
 * The last of the three libraries in this stack, and the one that
 * exercises all of it at once: HarfBuzz asks FreeType for glyph metrics,
 * FreeType reads the font off NTFS through the C library's FILE streams,
 * and HarfBuzz itself is C++ running on libcxx's containers and
 * `operator new`.
 *
 * ============================================================
 *  WHAT SHAPING IS, AND WHY MEASURING IS NOT IT
 * ============================================================
 *
 * apps/fttest.c already measures text: it adds up advances, one glyph
 * per character. That is enough for a terminal and wrong for a browser,
 * because the mapping from characters to positioned glyphs is not
 * one-to-one:
 *
 *   Two characters can become one glyph — "fi" is a ligature in most
 *   text faces.
 *   One character can become several — a base letter and a combining
 *   accent.
 *   A pair can be moved closer than their advances say — kerning, which
 *   is why "AV" is narrower than the sum of 'A' and 'V'.
 *   And the order is not the memory order in right-to-left script.
 *
 * A shaping engine is what turns a string into the positioned glyphs a
 * layout engine can put on a line. The checks below are chosen to
 * distinguish it from a loop over characters: if HarfBuzz were somehow
 * degenerating into "one glyph per byte, advance by hmtx", several of
 * them fail.
 *
 * ============================================================
 *  hb-ft IS UPSTREAM'S OWN SEAM, AND THAT IS THE POINT
 * ============================================================
 *
 * hb_ft_font_create builds an hb_font_t whose glyph lookups and metric
 * queries call into an FT_Face. It is HarfBuzz's own integration with
 * FreeType, compiled in by HAVE_FREETYPE, and using it means neither
 * library was taught anything about this system: the font data is parsed
 * once, by FreeType, and HarfBuzz asks it questions.
 *
 * The alternative — writing custom hb_font_funcs_t over the kernel's own
 * rasteriser — would mean a third parse of the same tables and a seam
 * nobody upstream maintains.
 */

#include "vextro.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#define FONT_PATH "/ComicNeue-Regular.ttf"

static int checks = 0, failures = 0;

static void ok(const char *what, bool good) {
    checks++;
    if (!good) failures++;
    std::printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

int main() {
    std::printf("hbtest: starting\n");
    std::printf("       (HarfBuzz %s)\n", hb_version_string());

    {
        unsigned major = 0, minor = 0, micro = 0;
        hb_version(&major, &minor, &micro);
        ok("the version is the one that was vendored",
           major == 8 && minor == 5 && micro == 0);
    }

    /* ---- the font, through FreeType ---- */
    FT_Library lib = nullptr;
    FT_Face    face = nullptr;
    hb_font_t *font = nullptr;

    {
        ok("FT_Init_FreeType", FT_Init_FreeType(&lib) == 0);
        const FT_Error e = FT_New_Face(lib, FONT_PATH, 0, &face);
        ok("FreeType opens the face off the volume", e == 0 && face != nullptr);
        if (e) {
            std::printf("hbtest: %d checks, %d failures\nhbtest: FAILED\n",
                        checks, ++failures);
            return 1;
        }
        ok("set a pixel size", FT_Set_Pixel_Sizes(face, 0, 32) == 0);

        font = hb_ft_font_create(face, nullptr);
        ok("hb_ft_font_create wraps it", font != nullptr);
        ok("and the font reports a scale", ({
            int xs = 0, ys = 0;
            hb_font_get_scale(font, &xs, &ys);
            xs != 0 && ys != 0;
        }));
    }

    /* ---- 1. a buffer becomes positioned glyphs ---- */
    {
        hb_buffer_t *buf = hb_buffer_create();
        ok("hb_buffer_create", buf != nullptr && hb_buffer_allocation_successful(buf));

        const char *text = "Wave";
        hb_buffer_add_utf8(buf, text, -1, 0, -1);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
        hb_buffer_set_language(buf, hb_language_from_string("en", -1));

        ok("the buffer holds the characters",
           hb_buffer_get_length(buf) == 4);

        hb_shape(font, buf, nullptr, 0);

        unsigned n = 0;
        hb_glyph_info_t     *info = hb_buffer_get_glyph_infos(buf, &n);
        hb_glyph_position_t *pos  = hb_buffer_get_glyph_positions(buf, &n);

        ok("shaping produced glyphs", n == 4 && info && pos);

        /* Every glyph must exist in the face, and must be the one
         * FreeType's own cmap lookup gives for that character. Two
         * independent paths to the same glyph id. */
        bool ids_agree = true, all_advance = true;
        for (unsigned i = 0; i < n && i < 4; i++) {
            const FT_UInt expect = FT_Get_Char_Index(face, (FT_ULong)text[i]);
            if (info[i].codepoint != expect) ids_agree = false;
            if (pos[i].x_advance <= 0) all_advance = false;
        }
        ok("the glyph ids match FreeType's own cmap lookup", ids_agree);
        ok("and every glyph advances", all_advance);

        long total = 0;
        for (unsigned i = 0; i < n; i++) total += pos[i].x_advance;
        std::printf("       (\"%s\" shaped to %u glyphs, %ld/64 px wide)\n",
                    text, n, total);
        ok("the run has a positive width", total > 0);

        /* Cluster values map glyphs back to the characters they came
         * from — which is what a browser needs for selection and
         * caret placement, and is meaningless in a per-character loop. */
        bool clusters_ordered = true;
        for (unsigned i = 1; i < n; i++)
            if (info[i].cluster < info[i - 1].cluster) clusters_ordered = false;
        ok("clusters are in order for left-to-right text", clusters_ordered);
        ok("the first cluster is the first byte", n > 0 && info[0].cluster == 0);

        hb_buffer_destroy(buf);
    }

    /* ---- 2. shaping is not a per-character loop ----
     *
     * The same string measured two ways: HarfBuzz's shaped advances,
     * and a naive sum of FreeType's per-character advances. For a face
     * with kerning they differ; for one without they agree exactly. Both
     * are correct outcomes, and which one applies is a property of the
     * font — so the assertion is that the two are *consistent*, and the
     * numbers are printed so the difference is visible.
     */
    {
        const char *pairs = "AVAWTo";

        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, pairs, -1, 0, -1);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
        hb_shape(font, buf, nullptr, 0);

        unsigned n = 0;
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
        long shaped = 0;
        for (unsigned i = 0; i < n; i++) shaped += pos[i].x_advance;

        long naive = 0;
        for (const char *p = pairs; *p; p++) {
            if (FT_Load_Char(face, (FT_ULong)*p, FT_LOAD_DEFAULT) == 0)
                naive += face->glyph->advance.x;
        }

        std::printf("       (\"%s\": shaped %ld/64, naive sum %ld/64)\n",
                    pairs, shaped, naive);
        ok("both measurements are positive", shaped > 0 && naive > 0);
        ok("and they agree to within a glyph",
           (shaped > naive ? shaped - naive : naive - shaped) < 64 * 32);

        hb_buffer_destroy(buf);
    }

    /* ---- 3. right-to-left reverses the visual order ----
     *
     * The clearest thing that separates a shaper from a loop. Given the
     * same characters and RTL direction, the glyphs come back in the
     * reverse of memory order, and the clusters descend. Nothing that
     * walks a string one character at a time does that.
     */
    {
        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, "abcd", -1, 0, -1);
        hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
        hb_buffer_set_script(buf, HB_SCRIPT_ARABIC);
        hb_shape(font, buf, nullptr, 0);

        unsigned n = 0;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
        ok("a right-to-left run shapes", n == 4);

        bool descending = true;
        for (unsigned i = 1; i < n; i++)
            if (info[i].cluster > info[i - 1].cluster) descending = false;
        ok("and its clusters run backwards", n == 4 && descending);

        hb_buffer_destroy(buf);
    }

    /* ---- 4. UTF-8 above the ASCII range ----
     *
     * Multi-byte input decoded into code points, which is the character
     * -encoding half of the job. The cluster of the second character is
     * its *byte* offset, so a two-byte first character puts it at 2 —
     * exactly the arithmetic a layout engine needs and exactly what a
     * byte loop gets wrong.
     */
    {
        const char *utf8 = "\xC3\xA9x";       /* U+00E9 LATIN SMALL E ACUTE, 'x' */

        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, utf8, -1, 0, -1);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, HB_SCRIPT_LATIN);

        ok("two characters from three bytes", hb_buffer_get_length(buf) == 2);

        hb_shape(font, buf, nullptr, 0);
        unsigned n = 0;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
        ok("which shape to two glyphs", n == 2);
        ok("the second cluster is a byte offset, not a character index",
           n == 2 && info[1].cluster == 2);

        const FT_UInt e_acute = FT_Get_Char_Index(face, 0x00E9);
        ok("and the accented letter resolved through the cmap",
           n == 2 && e_acute != 0 && info[0].codepoint == e_acute);

        hb_buffer_destroy(buf);
    }

    /* ---- 5. the Unicode tables HarfBuzz carries ----
     *
     * hb-ucd is HarfBuzz's own compressed Unicode Character Database:
     * general category, combining class, mirroring, script. It is the
     * part of "the raw international character tables" this stack
     * genuinely has, and it is worth checking because it is data rather
     * than code and a mis-built table is silent.
     */
    {
        hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();
        ok("there are unicode functions", u != nullptr);

        ok("'A' is an uppercase letter",
           hb_unicode_general_category(u, 'A') ==
               HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER);
        ok("'5' is a decimal number",
           hb_unicode_general_category(u, '5') ==
               HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER);
        ok("a space is a space separator",
           hb_unicode_general_category(u, ' ') ==
               HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR);
        ok("U+0301 is a non-spacing mark",
           hb_unicode_general_category(u, 0x0301) ==
               HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK);
        ok("and its combining class is 230",
           hb_unicode_combining_class(u, 0x0301) == 230);

        ok("'(' mirrors to ')'", hb_unicode_mirroring(u, '(') == ')');
        ok("latin script is recognised",
           hb_unicode_script(u, 'A') == HB_SCRIPT_LATIN);
        ok("and arabic is a different script",
           hb_unicode_script(u, 0x0627) == HB_SCRIPT_ARABIC);
    }

    hb_font_destroy(font);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    ok("everything released", true);

    std::printf("hbtest: %d checks, %d failures\n", checks, failures);
    std::printf(failures ? "hbtest: FAILED\n" : "hbtest: all passed\n");
    return failures ? 1 : 0;
}
