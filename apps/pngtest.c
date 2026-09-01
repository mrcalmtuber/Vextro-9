/*
 * pngtest — libpng in ring 3.
 *
 * The fourteenth port, and the one that finally makes setjmp load-bearing.
 * libpng does not return error codes: png_error() longjmps to a buffer the
 * caller set with setjmp(png_jmpbuf(png)), and every call into the library
 * is made underneath that. libc/setjmp.S has been in this tree for a long
 * time and nothing in ring 3 depended on it until now.
 *
 * ---- driven the way WebKit drives it ----
 *
 * Source/WebCore/platform/image-decoders/png/PNGImageDecoder.cpp does not
 * call png_read_image. It uses the *progressive* reader:
 *
 *     png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, failed, warned)
 *     png_set_progressive_read_fn(png, decoder, headerAvailable,
 *                                 rowAvailable, pngComplete)
 *     if (setjmp(JMPBUF(m_png))) ...
 *     png_process_data(m_png, m_info, bytes, count)
 *
 * — because an image arrives off a network a few kilobytes at a time and
 * the decoder has to make progress on what it has. Sections 2 and 6 use
 * that shape, with the data fed in pieces small enough to split the IHDR
 * and land inside the IDAT. Section 9 uses the other thing that file does:
 * png_set_crc_action(PNG_CRC_QUIET_USE, ...), which is how its APNG path
 * reads chunks it has reassembled itself and whose CRCs no longer match.
 *
 * ---- and against files libpng did not write ----
 *
 * Every image here comes from apps/png_ref.h: six built from the
 * specification by tools/mkpngref.py, and one re-encoded by macOS's
 * ImageIO. The hand-built ones exist so that the *filter type of each row*
 * could be chosen rather than left to a heuristic — pngref_rgba8 uses all
 * five, one per row, which is the only way to be sure the Average and
 * Paeth reconstructions are right. See the head of that header.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <png.h>
#include <zlib.h>

#include "png_ref.h"

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/* Room for the largest image here — 16x16 at four channels — plus the
 * row pointer table png_read_image wants. */
#define MAXW 16
#define MAXH 16
static unsigned char pixels[MAXH][MAXW * 8];
static png_bytep rowptrs[MAXH];

/* What libpng said went wrong, captured rather than printed: several
 * checks below are about the *message*, and a library that longjmped
 * without saying why would otherwise look the same as one that said
 * something useful. */
static char last_error[128];
static int  error_count, warning_count;

static void on_error(png_structp png, png_const_charp msg) {
    error_count++;
    snprintf(last_error, sizeof last_error, "%s", msg);
    /* png_error's contract: this function must not return. */
    longjmp(png_jmpbuf(png), 1);
}

static void on_warning(png_structp png, png_const_charp msg) {
    (void)png; (void)msg;
    warning_count++;
}

/* ---- a memory source for the non-progressive reader ---- */

typedef struct { const unsigned char *p; size_t len, off; } memsrc_t;

static void mem_read(png_structp png, png_bytep dst, size_t want) {
    memsrc_t *s = (memsrc_t *)png_get_io_ptr(png);
    if (s->off + want > s->len)
        png_error(png, "read past the end of the buffer");
    memcpy(dst, s->p + s->off, want);
    s->off += want;
}

/*
 * Decode a whole file into `pixels`, with the transformations named.
 * Returns 1 on success, 0 if libpng longjmped out — which is a normal
 * outcome for two of the files here and not a test failure by itself.
 */
static int decode(const unsigned char *data, size_t len,
                  int expand, int strip16,
                  png_uint_32 *w, png_uint_32 *h,
                  int *depth, int *colour, int *channels,
                  png_infop *keep_info, png_structp *keep_png) {
    png_structp png;
    png_infop info;
    memsrc_t src = { data, len, 0 };
    png_uint_32 iw, ih;
    int idepth, icolour, interlace;
    png_uint_32 y;

    error_count = 0; warning_count = 0; last_error[0] = 0;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                 on_error, on_warning);
    if (!png) return 0;
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }

    png_set_read_fn(png, &src, mem_read);
    png_read_info(png, info);
    png_get_IHDR(png, info, &iw, &ih, &idepth, &icolour, &interlace,
                 NULL, NULL);

    if (expand) {
        /* What WebKit does: palettes, low-bit greys and tRNS all become
         * 8-bit channels, so the rest of the decoder has one case. */
        png_set_expand(png);
    }
    if (strip16 && idepth == 16)
        png_set_strip_16(png);
    png_read_update_info(png, info);

    if (w) *w = iw;
    if (h) *h = ih;
    if (depth) *depth = idepth;
    if (colour) *colour = icolour;
    if (channels) *channels = png_get_channels(png, info);

    if (ih > MAXH || png_get_rowbytes(png, info) > sizeof pixels[0]) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }
    for (y = 0; y < ih; y++) rowptrs[y] = pixels[y];
    png_read_image(png, rowptrs);
    png_read_end(png, info);

    if (keep_info && keep_png) {
        /* The caller wants to interrogate the end-of-file state (text
         * chunks); it takes ownership of both structures. */
        *keep_png = png; *keep_info = info;
    } else {
        png_destroy_read_struct(&png, &info, NULL);
    }
    return 1;
}

/* ---- the progressive reader, in WebKit's shape ---- */

typedef struct {
    png_uint_32 width, height;
    int          channels;
    int          header_seen, rows_seen, complete;
} prog_t;

static void prog_info(png_structp png, png_infop info) {
    prog_t *st = (prog_t *)png_get_progressive_ptr(png);
    png_uint_32 w, h, y;
    int depth, colour, interlace;

    png_get_IHDR(png, info, &w, &h, &depth, &colour, &interlace, NULL, NULL);
    png_set_expand(png);
    if (depth == 16) png_set_strip_16(png);
    /* Required for an interlaced image read progressively: it is what
     * turns seven passes into seven calls per row rather than one. */
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    st->width = w; st->height = h;
    st->channels = png_get_channels(png, info);
    st->header_seen = 1;
    for (y = 0; y < h && y < MAXH; y++) rowptrs[y] = pixels[y];
    memset(pixels, 0, sizeof pixels);
}

static void prog_row(png_structp png, png_bytep row, png_uint_32 rownum,
                     int pass) {
    prog_t *st = (prog_t *)png_get_progressive_ptr(png);
    (void)pass;
    if (rownum >= MAXH) return;
    /* png_progressive_combine_row rather than a memcpy: for an
     * interlaced image `row` holds only this pass's pixels, spaced out,
     * and combine_row is what merges them into the row already there. */
    png_progressive_combine_row(png, rowptrs[rownum], row);
    st->rows_seen++;
}

static void prog_end(png_structp png, png_infop info) {
    prog_t *st = (prog_t *)png_get_progressive_ptr(png);
    (void)info;
    st->complete = 1;
}

/*
 * Feed a file to the progressive reader in `piece` bytes at a time.
 * Returns 1 if it ran to the end, 0 if libpng longjmped.
 */
static int decode_progressive(const unsigned char *data, size_t len,
                              size_t piece, prog_t *st, int quiet_crc) {
    png_structp png;
    png_infop info;
    size_t off = 0;

    memset(st, 0, sizeof *st);
    error_count = 0; warning_count = 0; last_error[0] = 0;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                 on_error, on_warning);
    if (!png) return 0;
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }
    if (quiet_crc)
        png_set_crc_action(png, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
    png_set_progressive_read_fn(png, st, prog_info, prog_row, prog_end);

    while (off < len) {
        size_t n = len - off < piece ? len - off : piece;
        png_process_data(png, info, (png_bytep)(data + off), n);
        off += n;
    }
    png_destroy_read_struct(&png, &info, NULL);
    return 1;
}

/* ---- a memory sink for the writer ---- */

static unsigned char written[8192];
static size_t written_len;

static void mem_write(png_structp png, png_bytep src, size_t n) {
    if (written_len + n > sizeof written)
        png_error(png, "wrote more than the sink can hold");
    memcpy(written + written_len, src, n);
    written_len += n;
}

static void mem_flush(png_structp png) { (void)png; }

int main(void) {
    printf("pngtest: libpng %s, zlib %s\n",
           png_get_libpng_ver(NULL), zlibVersion());

    /* ============================================================
     *  1. the configuration this port compiled
     * ============================================================
     *
     * libpng's configuration is one generated header, pnglibconf.h, and
     * this port copies upstream's scripts/pnglibconf.h.prebuilt verbatim
     * — no edits, because every option in it is supportable here. These
     * checks are that claim written down: the two that would have been
     * in question are setjmp and floating point, and both are asserted
     * rather than described.
     */
    {
        check("the header's version matches the archive's",
              strcmp(PNG_LIBPNG_VER_STRING, png_get_libpng_ver(NULL)) == 0);
        check("which is 1.6.58",
              strcmp(png_get_libpng_ver(NULL), "1.6.58") == 0);
        check("and the numeric version agrees",
              png_access_version_number() == PNG_LIBPNG_VER);
        /* png_get_header_ver reports the version of the *header* the
         * archive was built against, which is how libpng catches a
         * header and a library from different releases. */
        check("the archive was built against this header",
              strcmp(png_get_header_ver(NULL), PNG_LIBPNG_VER_STRING) == 0);

#ifdef PNG_SETJMP_SUPPORTED
        check("error handling is setjmp, which this system has", 1);
#else
        check("error handling is setjmp, which this system has", 0);
#endif
#ifdef PNG_FLOATING_POINT_SUPPORTED
        check("floating point is compiled in", 1);
#else
        check("floating point is compiled in", 0);
#endif
#ifdef PNG_READ_INTERLACING_SUPPORTED
        check("interlaced images can be read", 1);
#else
        check("interlaced images can be read", 0);
#endif
#ifdef PNG_PROGRESSIVE_READ_SUPPORTED
        check("the progressive reader is compiled in, which WebKit needs", 1);
#else
        check("the progressive reader is compiled in, which WebKit needs", 0);
#endif
#ifdef PNG_WRITE_SUPPORTED
        check("and the writer", 1);
#else
        check("and the writer", 0);
#endif
#ifdef PNG_tEXt_SUPPORTED
        check("text chunks are read", 1);
#else
        check("text chunks are read", 0);
#endif
        /* And the one that is off, which is upstream's default rather
         * than a decision here: libpng's x86 intrinsics are explicit
         * opt-in, unlike ARM's, so -msse2 in the build flags does not
         * quietly pull in code selected by a CPUID this port never
         * runs. Fourteen sources are the whole library. */
#ifdef PNG_INTEL_SSE_IMPLEMENTATION
        check("no x86 SIMD was compiled in", 0);
#else
        check("no x86 SIMD was compiled in", 1);
#endif
        check("libpng and zlib agree on zlib's version",
              strcmp(ZLIB_VERSION, zlibVersion()) == 0);
    }

    /* ============================================================
     *  2. an image decoded the way WebKit decodes one
     * ============================================================
     *
     * The progressive reader, fed 7 bytes at a time. That number is
     * chosen to be awkward: the signature is 8 bytes and the IHDR chunk
     * is 25, so no chunk boundary in the file lands on a multiple of 7
     * and libpng has to hold partial headers across calls.
     */
    {
        prog_t st;
        int ok = decode_progressive(pngref_rgb8, sizeof pngref_rgb8,
                                    7, &st, 0);
        check("a PNG fed 7 bytes at a time decodes", ok);
        check("the header callback ran", st.header_seen);
        check("the end callback ran", st.complete);
        check("with the right size", st.width == 16 && st.height == 16);
        check("and three channels", st.channels == 3);
        check("sixteen rows arrived", st.rows_seen == 16);

        {
            int bad = 0;
            unsigned x, y;
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    if (pixels[y][x * 3 + 0] != PNGREF_R(x, y)) bad++;
                    if (pixels[y][x * 3 + 1] != PNGREF_G(x, y)) bad++;
                    if (pixels[y][x * 3 + 2] != PNGREF_B(x, y)) bad++;
                }
            checkr("and every one of 768 samples is right", bad == 0, bad);
        }
        check("nothing was reported as an error", error_count == 0);

        /* One byte at a time is the same file through the same code with
         * every possible split, which is the cheapest way to be sure no
         * header is parsed out of a buffer that only half exists. */
        ok = decode_progressive(pngref_rgb8, sizeof pngref_rgb8, 1, &st, 0);
        check("and one byte at a time decodes as well", ok && st.complete);
        check("to the same pixels",
              pixels[0][0] == PNGREF_R(0, 0) &&
              pixels[15][15 * 3 + 2] == PNGREF_B(15, 15));
    }

    /* ============================================================
     *  3. all five row filters
     * ============================================================
     *
     * pngref_rgba8 uses filter None, Sub, Up, Average and Paeth, one per
     * row, cycling. A real encoder picks filters to compress well and
     * might never emit Average at all, so this image is the only thing
     * here that exercises the reconstruction code completely — and a
     * decoder with Sub and Up transposed passes every other check in
     * this file and fails this one.
     */
    {
        png_uint_32 w, h;
        int depth, colour, channels, bad = 0;
        unsigned x, y;

        check("the five-filter image decodes",
              decode(pngref_rgba8, sizeof pngref_rgba8, 1, 0,
                     &w, &h, &depth, &colour, &channels, NULL, NULL));
        check("it is 8x10", w == 8 && h == 10);
        check("colour type 6", colour == PNG_COLOR_TYPE_RGB_ALPHA);
        check("with four channels", channels == 4);

        for (y = 0; y < 10; y++)
            for (x = 0; x < 8; x++) {
                if (pixels[y][x * 4 + 0] != PNGREF_R(x, y)) bad++;
                if (pixels[y][x * 4 + 1] != PNGREF_G(x, y)) bad++;
                if (pixels[y][x * 4 + 2] != PNGREF_B(x, y)) bad++;
                if (pixels[y][x * 4 + 3] !=
                    (unsigned char)((x + y) * 8)) bad++;
            }
        checkr("and every sample survives all five filters", bad == 0, bad);
    }

    /* ============================================================
     *  4. sixteen bits a sample, which is the one big-endian thing
     * ============================================================
     *
     * PNG stores 16-bit samples most significant byte first, and this
     * machine is little-endian, so libpng's byte order is the one place
     * a decoder can be wrong in a way that still produces a picture.
     */
    {
        png_uint_32 w, h;
        int depth, colour, channels, bad = 0;
        unsigned x, y;

        check("the 16-bit grey image decodes",
              decode(pngref_gray16, sizeof pngref_gray16, 0, 0,
                     &w, &h, &depth, &colour, &channels, NULL, NULL));
        check("it is 4x4 at 16 bits", w == 4 && h == 4 && depth == 16);
        check("grey, one channel",
              colour == PNG_COLOR_TYPE_GRAY && channels == 1);

        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                unsigned expect = (x * 4096 + y * 257) & 0xFFFF;
                unsigned got = ((unsigned)pixels[y][x * 2] << 8) |
                               pixels[y][x * 2 + 1];
                if (got != expect) bad++;
            }
        checkr("and the samples are big-endian as PNG says", bad == 0, bad);

        /* png_set_strip_16 is what a consumer that wants eight bits
         * asks for, and it must keep the *high* byte. */
        check("the same image decodes again with strip_16",
              decode(pngref_gray16, sizeof pngref_gray16, 0, 1,
                     &w, &h, &depth, &colour, &channels, NULL, NULL));
        check("and 0x3000 became 0x30", pixels[0][3] == 0x30);
    }

    /* ============================================================
     *  5. a palette, and transparency that is not a channel
     * ============================================================
     *
     * Colour type 3 stores indices rather than intensities, so a decoder
     * that treats the samples as grey produces a plausible picture of
     * the wrong thing. png_set_expand — which is the first thing
     * WebKit's headerAvailable does — turns the palette and the tRNS
     * array into real RGBA.
     */
    {
        png_uint_32 w, h;
        int depth, colour, channels;
        static const unsigned char pal[4][3] = {
            { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 }
        };
        static const unsigned char alpha[4] = { 0xFF, 0x80, 0x40, 0x00 };
        int bad = 0;
        unsigned x, y;

        check("the palette image decodes",
              decode(pngref_pal8, sizeof pngref_pal8, 1, 0,
                     &w, &h, &depth, &colour, &channels, NULL, NULL));
        check("it is 8x8", w == 8 && h == 8);
        check("declared as a palette", colour == PNG_COLOR_TYPE_PALETTE);
        check("and expanded to four channels", channels == 4);

        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                unsigned idx = (x + y) % 4;
                if (pixels[y][x * 4 + 0] != pal[idx][0]) bad++;
                if (pixels[y][x * 4 + 1] != pal[idx][1]) bad++;
                if (pixels[y][x * 4 + 2] != pal[idx][2]) bad++;
                if (pixels[y][x * 4 + 3] != alpha[idx]) bad++;
            }
        checkr("every pixel is its palette entry, with its tRNS alpha",
               bad == 0, bad);
    }

    /* ============================================================
     *  6. Adam7, seven passes into one image
     * ============================================================
     *
     * The interlaced file holds exactly the pixels pngref_rgb8 does, in
     * a completely different order — so the check is that both roads
     * arrive at the same place. Read progressively, because that is
     * where interlacing is hardest: the row callback is handed one
     * pass's worth of a row at a time and png_progressive_combine_row
     * has to merge it into what is already there.
     */
    {
        prog_t st;
        int bad = 0;
        unsigned x, y;

        check("the interlaced image decodes progressively",
              decode_progressive(pngref_interlaced,
                                 sizeof pngref_interlaced, 13, &st, 0));
        check("with the right size", st.width == 16 && st.height == 16);
        check("and the end callback ran", st.complete);
        /* 112 row callbacks for a 16-row image, which is seven passes
         * times sixteen — every row on every pass, including the passes
         * that contribute nothing to it.
         *
         * That is a consequence of png_set_interlace_handling above, and
         * it is worth pinning down rather than glossing. Without that
         * call the callback would fire only for the rows a pass actually
         * carries, which for this image is 30 times, and each call would
         * hand over a short row of packed pixels the caller has to place
         * itself. With it, libpng expands every pass to the full width
         * and png_progressive_combine_row merges it. WebKit makes the
         * same call for the same reason, and 30 versus 112 is how you
         * can tell which of the two contracts you are under. */
        checkr("the row callback ran once per pass per row",
               st.rows_seen == 7 * 16, st.rows_seen);

        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                if (pixels[y][x * 3 + 0] != PNGREF_R(x, y)) bad++;
                if (pixels[y][x * 3 + 1] != PNGREF_G(x, y)) bad++;
                if (pixels[y][x * 3 + 2] != PNGREF_B(x, y)) bad++;
            }
        checkr("and the result is the same image as the flat one",
               bad == 0, bad);
    }

    /* ============================================================
     *  7. a file from somebody else's encoder entirely
     * ============================================================
     *
     * pngref_sips is the same 16x16 image re-encoded by macOS's ImageIO:
     * a mature encoder with its own filter heuristic, and it carries an
     * sRGB chunk and an eXIf chunk that nothing here would have thought
     * to write. Every other image in this file was built from the
     * specification by a script; this one was not.
     */
    {
        png_uint_32 w, h;
        int depth, colour, channels, bad = 0;
        unsigned x, y;

        check("the ImageIO file decodes",
              decode(pngref_sips, sizeof pngref_sips, 1, 1,
                     &w, &h, &depth, &colour, &channels, NULL, NULL));
        check("it is 16x16", w == 16 && h == 16);
        check("with the colour type the generator recorded",
              colour == PNGREF_SIPS_COLOUR_TYPE);
        check("and that many channels", channels == PNGREF_SIPS_CHANNELS);

        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                unsigned char *p = &pixels[y][x * PNGREF_SIPS_CHANNELS];
                if (p[0] != PNGREF_R(x, y)) bad++;
                if (p[1] != PNGREF_G(x, y)) bad++;
                if (p[2] != PNGREF_B(x, y)) bad++;
            }
        checkr("and holds the pixels a third decoder found in it",
               bad == 0, bad);
        check("with no warnings", warning_count == 0);
    }

    /* ============================================================
     *  8. the text a file carries
     * ============================================================ */
    {
        png_structp png = NULL;
        png_infop info = NULL;
        png_uint_32 w, h;
        int depth, colour, channels;

        check("the annotated image decodes",
              decode(pngref_rgb8, sizeof pngref_rgb8, 0, 0,
                     &w, &h, &depth, &colour, &channels, &info, &png));
        if (png && info) {
            png_textp text = NULL;
            int n = 0;
            png_get_text(png, info, &text, &n);
            checkr("both tEXt chunks were kept", n == 2, n);
            if (n == 2) {
                check("the first names the writer",
                      strcmp(text[0].key, "Software") == 0 &&
                      strcmp(text[0].text,
                             "vextro tools/mkpngref.py") == 0);
                check("and the second says what it is",
                      strcmp(text[1].key, "Comment") == 0 &&
                      strcmp(text[1].text, "not written by libpng") == 0);
            }
            png_destroy_read_struct(&png, &info, NULL);
        }
    }

    /* ============================================================
     *  9. damage, and the longjmp out of it
     * ============================================================
     *
     * This is the section the whole port hangs on. libpng reports
     * errors by calling png_error, which calls the caller's handler,
     * which must not return — it longjmps. Nothing else in ring 3 has
     * ever needed setjmp to work, so if libc/setjmp.S were wrong about
     * which registers to save this is where it would show, and it would
     * show as a wild jump rather than a wrong answer.
     */
    {
        png_uint_32 w, h;
        int depth, colour, channels;

        /* A broken IDAT CRC. libpng treats a CRC failure in a critical
         * chunk as fatal by default. */
        check("a file with a broken IDAT CRC does not decode",
              decode(pngref_badcrc, sizeof pngref_badcrc, 0, 0,
                     &w, &h, &depth, &colour, &channels, NULL, NULL) == 0);
        checkr("the error handler ran exactly once", error_count == 1,
               error_count);
        check("and libpng said what was wrong",
              strstr(last_error, "CRC") != NULL);

        /* A file that stops in the middle. The reader asks for bytes
         * that are not there, and mem_read raises the error itself —
         * which is a longjmp from *inside the caller's own callback*,
         * two frames deeper than the one above. */
        check("a truncated file does not decode",
              decode(pngref_truncated, sizeof pngref_truncated, 0, 0,
                     &w, &h, &depth, &colour, &channels, NULL, NULL) == 0);
        checkr("with one error", error_count == 1, error_count);

        /* And the same damaged file read the way WebKit reads a
         * reassembled APNG frame: png_set_crc_action(PNG_CRC_QUIET_USE)
         * says "use the data anyway and do not even warn", which turns
         * the fatal error above into a successful decode. A flag that
         * is passed and never changes an answer is not a tested flag. */
        {
            prog_t st;
            int bad = 0;
            unsigned x, y;
            check("with PNG_CRC_QUIET_USE the same file decodes",
                  decode_progressive(pngref_badcrc, sizeof pngref_badcrc,
                                     64, &st, 1) && st.complete);
            check("to the full image", st.width == 16 && st.height == 16);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++)
                    if (pixels[y][x * 3] != PNGREF_R(x, y)) bad++;
            checkr("with the pixels intact, because only the CRC was hurt",
                   bad == 0, bad);
            check("and quiet means quiet", warning_count == 0);
        }
    }

    /* ============================================================
     * 10. writing, and reading back what was written
     * ============================================================
     *
     * The weakest section here by construction — it proves the two
     * halves of one library agree, which is exactly the thing sections 2
     * to 7 exist to avoid relying on. It is worth having anyway: the
     * encoder is half the archive, WebKit's canvas.toDataURL path uses
     * it, and a deflate misconfigured through libpng would show up here
     * and nowhere else.
     *
     * The bytes are also inspected by hand before libpng is allowed to
     * read them back, so at least the container is checked against the
     * specification rather than against the writer's own reader.
     */
    {
        png_structp png;
        png_infop info;
        unsigned x, y;

        written_len = 0;
        error_count = 0; warning_count = 0;

        png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                      on_error, on_warning);
        check("png_create_write_struct", png != NULL);
        info = png ? png_create_info_struct(png) : NULL;
        check("png_create_info_struct", info != NULL);

        if (png && info) {
            if (setjmp(png_jmpbuf(png))) {
                check("writing did not longjmp", 0);
            } else {
                for (y = 0; y < 16; y++) {
                    for (x = 0; x < 16; x++) {
                        pixels[y][x * 3 + 0] = PNGREF_R(x, y);
                        pixels[y][x * 3 + 1] = PNGREF_G(x, y);
                        pixels[y][x * 3 + 2] = PNGREF_B(x, y);
                    }
                    rowptrs[y] = pixels[y];
                }
                png_set_write_fn(png, NULL, mem_write, mem_flush);
                png_set_IHDR(png, info, 16, 16, 8, PNG_COLOR_TYPE_RGB,
                             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
                             PNG_FILTER_TYPE_BASE);
                png_write_info(png, info);
                png_write_image(png, rowptrs);
                png_write_end(png, info);
                check("a 16x16 RGB image was written", written_len > 0);
            }
            png_destroy_write_struct(&png, &info);
        }

        /* By hand, against the specification: the eight-byte signature,
         * then a chunk whose length is 13 and whose type is IHDR, then
         * the width and height most significant byte first. */
        check("it starts with the PNG signature",
              written_len > 8 &&
              memcmp(written, "\211PNG\r\n\032\n", 8) == 0);
        check("followed by a 13-byte IHDR",
              written_len > 33 &&
              written[8] == 0 && written[9] == 0 &&
              written[10] == 0 && written[11] == 13 &&
              memcmp(written + 12, "IHDR", 4) == 0);
        check("declaring 16 by 16",
              written[16] == 0 && written[17] == 0 &&
              written[18] == 0 && written[19] == 16 &&
              written[20] == 0 && written[21] == 0 &&
              written[22] == 0 && written[23] == 16);
        check("8 bits, colour type 2, no interlace",
              written[24] == 8 && written[25] == 2 && written[28] == 0);
        check("and ends with IEND",
              written_len > 12 &&
              memcmp(written + written_len - 8, "IEND", 4) == 0);

        /* And now through the reader. */
        {
            png_uint_32 w, h;
            int depth, colour, channels, bad = 0;
            memset(pixels, 0, sizeof pixels);
            check("what was written decodes",
                  decode(written, written_len, 0, 0,
                         &w, &h, &depth, &colour, &channels, NULL, NULL));
            check("as 16x16 RGB",
                  w == 16 && h == 16 && depth == 8 && channels == 3);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    if (pixels[y][x * 3 + 0] != PNGREF_R(x, y)) bad++;
                    if (pixels[y][x * 3 + 1] != PNGREF_G(x, y)) bad++;
                    if (pixels[y][x * 3 + 2] != PNGREF_B(x, y)) bad++;
                }
            checkr("with the pixels that went in", bad == 0, bad);
        }
    }

    printf("pngtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
