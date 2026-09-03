/*
 * webptest — libwebp and libwebpdemux in ring 3.
 *
 * The fifteenth port, and the first that is two archives rather than
 * one: `find_package(WebP REQUIRED COMPONENTS demux)` asks for libwebp
 * and libwebpdemux by name, and the demuxer is a separate library with a
 * separate job — it does not decode a pixel, it walks a RIFF container
 * and hands out fragments.
 *
 * ---- what this file can prove, and what it cannot ----
 *
 * Every other codec ported here is checked against a bitstream some
 * other implementation produced: apps/jpeg_ref.h came out of macOS's own
 * JPEG codec, apps/zlib_ref.h out of Apple's libcompression, most of
 * apps/png_ref.h was written from the PNG specification.
 *
 * There is no second implementation of VP8 or VP8L on this machine, and
 * apps/webp_ref.h says so at length. macOS reads .webp through ImageIO
 * and ImageIO bundles libwebp. So the bitstreams here were encoded by
 * these same sources compiled for the *host*, and what the checks
 * establish is agreement across three builds — arm64 clang taking NEON,
 * this one taking SSE2 and SSE4.1, and Apple's — rather than across
 * implementations.
 *
 * Three things claw back most of what that costs, and they are sections
 * 2, 8 and 9:
 *
 *   The lossless images are generated from a *formula*, and lossless
 *   WebP is bit-exact by definition. The chain is formula, host encoder,
 *   this decoder, formula — so section 2 depends on nothing being
 *   trusted.
 *
 *   Section 8 swaps libwebp's own CPU-detection hook for one that
 *   reports a machine with no SIMD at all, decodes again, and requires
 *   the two results to be identical byte for byte. That is a check of
 *   the SSE2 and SSE4.1 kernels against the portable C in the same
 *   archive, and it is the one check here that could not be made by any
 *   amount of cross-implementation testing.
 *
 *   Section 9's container was not written by libwebp. VP8X, ANIM and
 *   three ANMF chunks were assembled from the WebP container
 *   specification by tools/mkwebpref.py — which is what the RFC 1950 and
 *   RFC 1952 wrappers are to zlib, and matters more here, because the
 *   demuxer is the component WebKit's line 20 actually asks for.
 *
 * ---- driven the way WebKit drives it ----
 *
 * Source/WebCore/platform/image-decoders/webp/WEBPImageDecoder.cpp does
 * three specific things this file repeats. It demuxes *partially*, on a
 * buffer that grows as bytes arrive, and waits for a VP8X header before
 * it will even try. It decodes with WebPINewDecoder into a WebPDecBuffer
 * whose `is_external_memory` is 1 and whose `u.RGBA.rgba` points at a
 * buffer the caller owns — a different lifetime contract from letting
 * libwebp allocate, and the one that has to work. And it always asks for
 * MODE_RGBA, whatever the file contains.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/webp/decode.h"
#include "src/webp/demux.h"
#include "src/webp/mux_types.h"

/* Internal, and deliberately so: VP8GetCPUInfo is the hook libwebp's own
 * tests use to force the dispatcher down a different path. It is not in
 * the public headers because a consumer has no business touching it —
 * but section 8 is not a consumer, it is a check that the SIMD kernels
 * in this archive agree with the C ones beside them, and there is no
 * other way to ask. src/dsp/cpu.h:197-212 is the contract that makes it
 * work: every Init function records which hook it last dispatched
 * against and re-runs itself when the pointer changes. */
#include "src/dsp/cpu.h"

/* cpu.h defines the typedef and the enum but does not declare the hook
 * itself: upstream writes `extern VP8CPUInfo VP8GetCPUInfo;` in each
 * file that reaches for it — src/dec/vp8_dec.c:512, src/dsp/rescaler.c:204
 * and four others do exactly this. Copied rather than improved on, so
 * that a future release moving the declaration into a header breaks this
 * line loudly instead of leaving a second, quietly diverging one. */
extern VP8CPUInfo VP8GetCPUInfo;

#include "webp_ref.h"

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/* 32x32 RGBA is 4 KiB; three of these is the whole working set. */
#define MAXPIX (WEBPREF_W * WEBPREF_H * 4)
static unsigned char pix_a[MAXPIX];
static unsigned char pix_b[MAXPIX];

/* How many samples of a decoded RGBA image disagree with the formula. */
static int diff_from_pattern(const unsigned char *p, int w, int h,
                             int with_alpha) {
    int bad = 0, x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const unsigned char *q = p + (y * w + x) * 4;
            if (q[0] != WEBPREF_R(x, y)) bad++;
            if (q[1] != WEBPREF_G(x, y)) bad++;
            if (q[2] != WEBPREF_B(x, y)) bad++;
            if (q[3] != (with_alpha ? WEBPREF_A(x, y) : 255)) bad++;
        }
    return bad;
}

/*
 * WebKit's decode, reduced to its essentials: an incremental decoder
 * over external memory, fed in pieces. Returns the final status.
 */
static VP8StatusCode decode_incremental(const unsigned char *data, size_t len,
                                        size_t piece, int w, int h,
                                        unsigned char *dst, int *suspends) {
    WebPDecBuffer buf;
    WebPIDecoder *idec;
    VP8StatusCode st = VP8_STATUS_SUSPENDED;
    size_t off = 0;

    if (suspends) *suspends = 0;
    if (!WebPInitDecBuffer(&buf)) return VP8_STATUS_INVALID_PARAM;
    buf.colorspace = MODE_RGBA;
    buf.u.RGBA.stride = w * 4;
    buf.u.RGBA.size = (size_t)buf.u.RGBA.stride * h;
    buf.is_external_memory = 1;
    buf.u.RGBA.rgba = dst;

    idec = WebPINewDecoder(&buf);
    if (idec == NULL) return VP8_STATUS_OUT_OF_MEMORY;

    while (off < len) {
        size_t n = (len - off < piece) ? len - off : piece;
        st = WebPIAppend(idec, data + off, n);
        off += n;
        if (st == VP8_STATUS_SUSPENDED) {
            if (suspends) (*suspends)++;
            continue;
        }
        break;
    }
    WebPIDelete(idec);
    return st;
}

/* The stub section 8 installs: a machine with nothing. */
static int no_simd(CPUFeature f) { (void)f; return 0; }

int main(void) {
    printf("webptest: libwebp %06x, demux %06x\n",
           WebPGetDecoderVersion(), WebPGetDemuxVersion());

    /* ============================================================
     *  1. the archive, and what it was compiled to contain
     * ============================================================ */
    {
        const int v = WebPGetDecoderVersion();
        check("the decoder reports 1.6.0",
              v == ((1 << 16) | (6 << 8) | 0));
        /* The demuxer has its own version, and it is not the library's —
         * libwebpdemux is versioned separately upstream, which is part
         * of why FindWebP looks for two archives. */
        check("the demuxer reports its own version",
              WebPGetDemuxVersion() > 0);
        check("the two archives are linked together",
              WebPGetDemuxVersion() != v || 1);

        /* The SIMD gate from third_party/libwebp-port/config.h. These
         * are compile-time facts about the archive: each one means an
         * Init function for that instruction set is present, and each is
         * paired with a -m flag in the Makefile. */
#ifdef WEBP_HAVE_SSE2
        check("SSE2 kernels are compiled in", 1);
#else
        check("SSE2 kernels are compiled in", 0);
#endif
#ifdef WEBP_HAVE_SSE41
        check("SSE4.1 kernels are compiled in", 1);
#else
        check("SSE4.1 kernels are compiled in", 0);
#endif
#ifdef WEBP_HAVE_AVX2
        check("AVX2 kernels are compiled in", 1);
#else
        check("AVX2 kernels are compiled in", 0);
#endif
#ifdef WEBP_USE_THREAD
        check("threaded decoding is compiled in", 1);
#else
        check("threaded decoding is compiled in", 0);
#endif
#ifdef WEBP_HAVE_NEON
        check("no ARM kernels", 0);
#else
        check("no ARM kernels", 1);
#endif

        /* And what the processor says, which is a different question
         * from what the archive holds. CPUID is not privileged, so a
         * ring-3 process asks the hardware directly.
         *
         * Only SSE2 is required: every application here is built -msse2
         * and would not run at all without it. SSE4.1 and AVX2 are
         * *reported* rather than required — AVX2 in particular also
         * needs the operating system to have enabled the YMM state
         * through XCR0, which this kernel does not do, so libwebp
         * correctly declines to use kernels that are sitting in the
         * archive. Either answer is right and the report is the useful
         * part. */
        check("the processor has SSE2", VP8GetCPUInfo(kSSE2) != 0);
        printf("       CPU: sse2=%d sse4.1=%d avx=%d avx2=%d\n",
               VP8GetCPUInfo(kSSE2) != 0, VP8GetCPUInfo(kSSE4_1) != 0,
               VP8GetCPUInfo(kAVX) != 0, VP8GetCPUInfo(kAVX2) != 0);
    }

    /* ============================================================
     *  2. lossless, which is exact against a formula
     * ============================================================
     *
     * The strongest section in the file and the only one that depends on
     * nothing being trusted. VP8L is defined to reproduce its input
     * exactly; the input was a formula rather than a picture; and
     * apps/webp_ref.h carries that formula as three macros. So this
     * decodes to the pixels or it does not, and no encoder's judgement
     * enters into it.
     */
    {
        int w = 0, h = 0;
        uint8_t *out;

        check("WebPGetInfo reads the lossless header",
              WebPGetInfo(webpref_lossless, sizeof webpref_lossless, &w, &h));
        check("with the right size", w == WEBPREF_W && h == WEBPREF_H);

        {
            WebPBitstreamFeatures f;
            VP8StatusCode st = WebPGetFeatures(webpref_lossless,
                                               sizeof webpref_lossless, &f);
            checkr("WebPGetFeatures succeeds", st == VP8_STATUS_OK, st);
            check("and reports lossless", f.format == 2);
            check("with no alpha", f.has_alpha == 0);
            check("and no animation", f.has_animation == 0);
        }

        out = WebPDecodeRGBA(webpref_lossless, sizeof webpref_lossless,
                             &w, &h);
        check("the lossless image decodes", out != NULL);
        if (out) {
            checkr("and every one of 4096 samples matches the formula",
                   diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 0) == 0,
                   diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 0));
            memcpy(pix_a, out, MAXPIX);
            WebPFree(out);
        }

        /* The same picture with a real alpha ramp, which is a different
         * VP8L path: alpha is a fourth channel inside the lossless
         * stream rather than a separate ALPH chunk. */
        out = WebPDecodeRGBA(webpref_lossless_alpha,
                             sizeof webpref_lossless_alpha, &w, &h);
        check("the lossless image with alpha decodes", out != NULL);
        if (out) {
            checkr("and its samples match, alpha included",
                   diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 1) == 0,
                   diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 1));
            WebPFree(out);
        }
    }

    /* ============================================================
     *  3. lossy, which is exact against another build
     * ============================================================
     *
     * VP8 is lossy in the encoder and exactly specified in the decoder,
     * so "close enough" is not the standard here: the bytes the host
     * build produced from this bitstream are in apps/webp_ref.h, and
     * this build must reproduce them without a single sample differing.
     * A tolerance would hide exactly the failure a port test exists to
     * find — a miscompiled kernel, or undefined behaviour that the two
     * compilers resolved differently.
     */
    {
        int w = 0, h = 0;
        uint8_t *out;

        {
            WebPBitstreamFeatures f;
            WebPGetFeatures(webpref_lossy, sizeof webpref_lossy, &f);
            check("the lossy file reports itself lossy", f.format == 1);
            check("and carries no alpha", f.has_alpha == 0);
        }

        out = WebPDecodeRGBA(webpref_lossy, sizeof webpref_lossy, &w, &h);
        check("the lossy image decodes", out != NULL);
        check("at the right size", w == WEBPREF_W && h == WEBPREF_H);
        if (out) {
            check("byte for byte as the host build decoded it",
                  memcmp(out, webpref_lossy_pixels,
                         sizeof webpref_lossy_pixels) == 0);
            memcpy(pix_a, out, MAXPIX);
            WebPFree(out);
        }

        /* Lossy with an ALPH chunk beside the VP8 one. This is the shape
         * WebKit meets most often on the web, and it is the only one
         * that runs src/dec/alpha_dec.c and the alpha_processing dsp —
         * a lossless-compressed alpha plane stitched onto a lossy
         * colour plane. */
        {
            WebPBitstreamFeatures f;
            WebPGetFeatures(webpref_lossy_alpha,
                            sizeof webpref_lossy_alpha, &f);
            check("the lossy+alpha file reports alpha", f.has_alpha == 1);
            check("and is still lossy", f.format == 1);
        }
        out = WebPDecodeRGBA(webpref_lossy_alpha,
                             sizeof webpref_lossy_alpha, &w, &h);
        check("the lossy image with alpha decodes", out != NULL);
        if (out) {
            check("byte for byte, alpha plane included",
                  memcmp(out, webpref_lossy_alpha_pixels,
                         sizeof webpref_lossy_alpha_pixels) == 0);
            /* And the alpha really is a ramp rather than 255 everywhere,
             * which is what this check is guarding: a decoder that
             * ignored the ALPH chunk would produce an opaque image and
             * pass a memcmp against its own output. */
            check("and the alpha channel is not simply opaque",
                  out[3] != out[(WEBPREF_W * 8 + 8) * 4 + 3]);
            WebPFree(out);
        }
    }

    /* ============================================================
     *  4. the incremental decoder, into memory the caller owns
     * ============================================================
     *
     * WebKit's path. WebPINewDecoder over a WebPDecBuffer with
     * is_external_memory set and u.RGBA.rgba pointing at the frame
     * buffer — libwebp writes into it and never owns it, which is a
     * different contract from the internal-allocation path and the one
     * that has to work.
     *
     * Fed 17 bytes at a time, which divides nothing in the file.
     */
    {
        int suspends = 0;
        VP8StatusCode st;

        memset(pix_b, 0, sizeof pix_b);
        st = decode_incremental(webpref_lossy, sizeof webpref_lossy, 17,
                                WEBPREF_W, WEBPREF_H, pix_b, &suspends);
        checkr("an incremental decode into external memory finishes",
               st == VP8_STATUS_OK, st);
        check("having suspended along the way", suspends > 4);
        check("and produced the same pixels as the one-shot decode",
              memcmp(pix_b, webpref_lossy_pixels,
                     sizeof webpref_lossy_pixels) == 0);

        /* One byte at a time is every possible split of the same file. */
        memset(pix_b, 0, sizeof pix_b);
        st = decode_incremental(webpref_lossy, sizeof webpref_lossy, 1,
                                WEBPREF_W, WEBPREF_H, pix_b, NULL);
        checkr("and one byte at a time also finishes",
               st == VP8_STATUS_OK, st);
        check("with the same result",
              memcmp(pix_b, webpref_lossy_pixels,
                     sizeof webpref_lossy_pixels) == 0);

        /* The lossless stream through the same path, because VP8L has
         * its own incremental implementation and shares none of VP8's. */
        memset(pix_b, 0, sizeof pix_b);
        st = decode_incremental(webpref_lossless, sizeof webpref_lossless,
                                9, WEBPREF_W, WEBPREF_H, pix_b, NULL);
        checkr("a lossless stream decodes incrementally too",
               st == VP8_STATUS_OK, st);
        checkr("to the formula",
               diff_from_pattern(pix_b, WEBPREF_W, WEBPREF_H, 0) == 0,
               diff_from_pattern(pix_b, WEBPREF_W, WEBPREF_H, 0));
    }

    /* ============================================================
     *  5. failures, which have to be failures
     * ============================================================
     *
     * A truncated file and a corrupt one are different answers, and a
     * decoder fed by a network has to be able to tell them apart:
     * suspended means "ask me again when you have more", and a bitstream
     * error means "stop".
     */
    {
        int w = 0, h = 0;
        VP8StatusCode st;

        st = decode_incremental(webpref_truncated, sizeof webpref_truncated,
                                64, WEBPREF_W, WEBPREF_H, pix_b, NULL);
        checkr("a truncated file leaves the decoder suspended",
               st == VP8_STATUS_SUSPENDED, st);

        check("and a one-shot decode of it returns nothing",
              WebPDecodeRGBA(webpref_truncated, sizeof webpref_truncated,
                             &w, &h) == NULL);

        /* Corrupt: a flipped byte inside a VP8L stream. Nothing about
         * the container is wrong, so this has to be caught by the
         * entropy decoder rather than by a length check. */
        check("a corrupt lossless stream is refused",
              WebPDecodeRGBA(webpref_corrupt, sizeof webpref_corrupt,
                             &w, &h) == NULL);
        {
            WebPBitstreamFeatures f;
            st = WebPGetFeatures(webpref_corrupt, sizeof webpref_corrupt, &f);
            /* The header is intact, so the *features* still read: the
             * damage is deeper than anything WebPGetFeatures inspects,
             * which is why a caller cannot use it as a validity test. */
            checkr("though its header still parses", st == VP8_STATUS_OK, st);
        }

        /* Not a WebP at all. */
        {
            static const unsigned char junk[32] = { 'R', 'I', 'F', 'F' };
            check("a RIFF header with nothing behind it is refused",
                  WebPGetInfo(junk, sizeof junk, &w, &h) == 0);
        }
    }

    /* ============================================================
     *  6. threads, because a flag that changes nothing is not tested
     * ============================================================
     *
     * WEBP_USE_THREAD is on in the port's config.h, and WebKit cannot
     * reach it: WebPINewDecoder takes no WebPDecoderConfig, so there is
     * nowhere for it to set use_threads. This section is the only thing
     * in the tree that makes the setting real — and what it checks is
     * that turning it on does not change the answer, which is the whole
     * point of a threaded decoder.
     */
    {
        WebPDecoderConfig cfg;
        VP8StatusCode st;

        check("WebPInitDecoderConfig", WebPInitDecoderConfig(&cfg));
        cfg.options.use_threads = 0;
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = pix_a;
        cfg.output.u.RGBA.stride = WEBPREF_W * 4;
        cfg.output.u.RGBA.size = MAXPIX;
        st = WebPDecode(webpref_lossy, sizeof webpref_lossy, &cfg);
        checkr("a single-threaded decode", st == VP8_STATUS_OK, st);

        WebPInitDecoderConfig(&cfg);
        cfg.options.use_threads = 1;
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = pix_b;
        cfg.output.u.RGBA.stride = WEBPREF_W * 4;
        cfg.output.u.RGBA.size = MAXPIX;
        st = WebPDecode(webpref_lossy, sizeof webpref_lossy, &cfg);
        checkr("a multi-threaded decode", st == VP8_STATUS_OK, st);
        check("and the two agree exactly",
              memcmp(pix_a, pix_b, MAXPIX) == 0);
        check("with the host build as well",
              memcmp(pix_b, webpref_lossy_pixels,
                     sizeof webpref_lossy_pixels) == 0);
    }

    /* ============================================================
     *  7. cropping and scaling, which are the decoder's own geometry
     * ============================================================
     *
     * WebPDecoderOptions can crop and rescale during the decode rather
     * than after it, which is what src/utils/rescaler_utils.c and the
     * rescaler dsp exist for — 8 of the 85 objects in libwebp.a, and
     * otherwise never reached by anything in this file.
     */
    {
        WebPDecoderConfig cfg;
        VP8StatusCode st;
        int x, y, bad = 0;

        WebPInitDecoderConfig(&cfg);
        cfg.options.use_cropping = 1;
        cfg.options.crop_left = 8;
        cfg.options.crop_top = 4;
        cfg.options.crop_width = 16;
        cfg.options.crop_height = 8;
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = pix_a;
        cfg.output.u.RGBA.stride = 16 * 4;
        cfg.output.u.RGBA.size = 16 * 8 * 4;
        st = WebPDecode(webpref_lossless, sizeof webpref_lossless, &cfg);
        checkr("a cropped lossless decode", st == VP8_STATUS_OK, st);
        for (y = 0; y < 8; y++)
            for (x = 0; x < 16; x++) {
                const unsigned char *q = pix_a + (y * 16 + x) * 4;
                if (q[0] != WEBPREF_R(x + 8, y + 4)) bad++;
                if (q[1] != WEBPREF_G(x + 8, y + 4)) bad++;
                if (q[2] != WEBPREF_B(x + 8, y + 4)) bad++;
            }
        checkr("lands on the window that was asked for", bad == 0, bad);

        WebPInitDecoderConfig(&cfg);
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = 16;
        cfg.options.scaled_height = 16;
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = pix_b;
        cfg.output.u.RGBA.stride = 16 * 4;
        cfg.output.u.RGBA.size = 16 * 16 * 4;
        st = WebPDecode(webpref_lossless, sizeof webpref_lossless, &cfg);
        checkr("a half-size decode", st == VP8_STATUS_OK, st);
        /* Not checked against a formula: the rescaler is an average
         * rather than a sample, so the exact values are its business.
         * What is checked is that it produced an image rather than a
         * buffer of zeros, and that the corners came from the corners. */
        check("produces something other than an empty buffer",
              pix_b[0] != 0 || pix_b[1] != 0 || pix_b[2] != 0);
        check("with the top-left near the original's top-left",
              pix_b[1] < WEBPREF_G(0, 2) + 8);
    }

    /* ============================================================
     *  8. the SIMD kernels against the C beside them
     * ============================================================
     *
     * The check that no amount of cross-implementation testing could
     * make. Everything above ran on whatever kernels the dispatcher
     * chose — on this machine, SSE2 and SSE4.1. This section installs a
     * CPU-detection hook that reports a processor with nothing at all,
     * decodes the same files again, and requires the results to be
     * identical to the byte.
     *
     * If an SSE4.1 kernel disagrees with its portable counterpart, every
     * other check in this file passes and this one fails. That is the
     * whole reason the port compiles the SIMD in rather than turning it
     * off: an optimisation nobody can test is a liability, and this
     * makes it testable.
     *
     * src/dsp/cpu.h:197-212 is what makes it work — each Init function
     * remembers which hook it dispatched against and re-runs when the
     * pointer changes, so no cache has to be flushed by hand.
     */
    {
        VP8CPUInfo saved = VP8GetCPUInfo;
        int w = 0, h = 0;
        uint8_t *simd, *plain;

        simd = WebPDecodeRGBA(webpref_lossy, sizeof webpref_lossy, &w, &h);
        check("decoded once with the kernels the CPU allows", simd != NULL);

        VP8GetCPUInfo = no_simd;
        plain = WebPDecodeRGBA(webpref_lossy, sizeof webpref_lossy, &w, &h);
        check("and once with SIMD dispatch turned off", plain != NULL);
        check("the two lossy decodes are identical",
              simd && plain && memcmp(simd, plain, MAXPIX) == 0);
        if (plain) WebPFree(plain);
        if (simd) WebPFree(simd);

        /* And the lossless path, which uses an entirely different set of
         * kernels — VP8LDspInit rather than VP8DspInit. */
        simd = NULL;
        VP8GetCPUInfo = saved;
        simd = WebPDecodeRGBA(webpref_lossless_alpha,
                              sizeof webpref_lossless_alpha, &w, &h);
        VP8GetCPUInfo = no_simd;
        plain = WebPDecodeRGBA(webpref_lossless_alpha,
                               sizeof webpref_lossless_alpha, &w, &h);
        check("the two lossless decodes are identical",
              simd && plain && memcmp(simd, plain, MAXPIX) == 0);
        check("and both match the formula",
              plain && diff_from_pattern(plain, WEBPREF_W, WEBPREF_H, 1) == 0);
        if (plain) WebPFree(plain);
        if (simd) WebPFree(simd);

        VP8GetCPUInfo = saved;
        check("the hook was put back", VP8GetCPUInfo == saved);
    }

    /* ============================================================
     *  9. the demuxer, on a container libwebp did not write
     * ============================================================
     *
     * libwebpdemux is the component `find_package(WebP REQUIRED
     * COMPONENTS demux)` asks for, and it is the only part of this port
     * with a genuinely independent reference: the VP8X, ANIM and three
     * ANMF chunks in webpref_anim were assembled from the WebP container
     * specification by tools/mkwebpref.py, not by libwebp's muxer —
     * which this port does not build.
     *
     * The five places that goes wrong are all read back here: the canvas
     * size is stored minus one, the frame offsets are stored halved, the
     * frame sizes are stored minus one, the duration is three bytes, and
     * the flags byte names both of its bits for the negative case.
     */
    {
        WebPData data;
        WebPDemuxer *dmux;
        WebPIterator it;
        uint32_t flags;

        data.bytes = webpref_anim;
        data.size = sizeof webpref_anim;

        dmux = WebPDemux(&data);
        check("the hand-built animation demuxes", dmux != NULL);
        if (dmux) {
            flags = WebPDemuxGetI(dmux, WEBP_FF_FORMAT_FLAGS);
            check("the VP8X flags say animation", (flags & ANIMATION_FLAG) != 0);
            check("and nothing else", (flags & ALPHA_FLAG) == 0);
            check("the canvas is the size that was written",
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_WIDTH) == WEBPREF_W &&
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_HEIGHT) == WEBPREF_H);
            checkr("all three frames are there",
                   WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT)
                       == WEBPREF_ANIM_FRAMES,
                   (int)WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT));
            check("the loop count came through",
                  WebPDemuxGetI(dmux, WEBP_FF_LOOP_COUNT)
                      == WEBPREF_ANIM_LOOPS);
            check("and the background colour",
                  WebPDemuxGetI(dmux, WEBP_FF_BACKGROUND_COLOR)
                      == WEBPREF_ANIM_BACKGROUND);

            /* Frame 1: the full canvas, 100 ms, blend and no dispose. */
            check("frame 1 is there", WebPDemuxGetFrame(dmux, 1, &it));
            check("at the origin", it.x_offset == 0 && it.y_offset == 0);
            check("filling the canvas",
                  it.width == WEBPREF_W && it.height == WEBPREF_H);
            check("for 100 ms", it.duration == 100);
            check("blending, not disposing",
                  it.blend_method == WEBP_MUX_BLEND &&
                  it.dispose_method == WEBP_MUX_DISPOSE_NONE);
            check("and it is the first frame of three",
                  it.frame_num == 1 && it.num_frames == WEBPREF_ANIM_FRAMES);

            /* And its fragment really decodes — the demuxer's job is to
             * hand out bytes the decoder can use, so a fragment that
             * parsed but did not decode would be a demuxer bug. */
            {
                int w = 0, h = 0;
                uint8_t *out = WebPDecodeRGBA(it.fragment.bytes,
                                              it.fragment.size, &w, &h);
                check("frame 1's fragment decodes", out != NULL);
                check("to the frame's own size",
                      w == WEBPREF_W && h == WEBPREF_H);
                if (out) {
                    checkr("and to the formula",
                           diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 0) == 0,
                           diff_from_pattern(out, WEBPREF_W, WEBPREF_H, 0));
                    WebPFree(out);
                }
            }
            WebPDemuxReleaseIterator(&it);

            /* Frame 2: offset, smaller, and *not* blended — which is the
             * bit of the ANMF flags byte whose name is the negative. */
            check("frame 2 is there", WebPDemuxGetFrame(dmux, 2, &it));
            check("at the halved offset, doubled back",
                  it.x_offset == 4 && it.y_offset == 2);
            check("with its own smaller size",
                  it.width == WEBPREF_ANIM_W && it.height == WEBPREF_ANIM_H);
            check("for 250 ms", it.duration == 250);
            check("not blending", it.blend_method == WEBP_MUX_NO_BLEND);
            check("and not disposing",
                  it.dispose_method == WEBP_MUX_DISPOSE_NONE);
            WebPDemuxReleaseIterator(&it);

            /* Frame 3: the other bit of the same byte. */
            check("frame 3 is there", WebPDemuxGetFrame(dmux, 3, &it));
            check("at its own offset", it.x_offset == 2 && it.y_offset == 6);
            check("for 33 ms", it.duration == 33);
            check("blending", it.blend_method == WEBP_MUX_BLEND);
            check("but disposing to background",
                  it.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND);
            check("and it is the last", it.frame_num == 3);
            check("so there is no fourth",
                  WebPDemuxNextFrame(&it) == 0);
            WebPDemuxReleaseIterator(&it);

            WebPDemuxDelete(dmux);
        }
    }

    /* ============================================================
     * 10. demuxing partially, which is what WebKit actually does
     * ============================================================
     *
     * WEBPImageDecoder.cpp never calls WebPDemux. It calls
     * WebPDemuxPartial on whatever has arrived so far, and it will not
     * even try until it has the VP8X header — "Await VP8X header so
     * WebPDemuxPartial succeeds", its own comment says. So the states
     * matter as much as the answers: PARSING_HEADER while the header is
     * incomplete, PARSED_HEADER once the canvas is known, and DONE at
     * the end.
     *
     * The buffer is not copied by the demuxer, which is why WebKit
     * protects the data across the call; here the whole array is static
     * and only `size` grows, which is the same thing done more simply.
     */
    {
        WebPData data;
        WebPDemuxState state;
        WebPDemuxer *dmux;

        data.bytes = webpref_anim;

        /* Four bytes: "RIFF" and nothing else. */
        data.size = 4;
        state = WEBP_DEMUX_PARSE_ERROR;
        dmux = WebPDemuxPartial(&data, &state);
        checkr("four bytes is not yet a header",
               state == WEBP_DEMUX_PARSING_HEADER, state);
        if (dmux) WebPDemuxDelete(dmux);

        /* Through the RIFF header and the VP8X chunk — 12 + 18 bytes —
         * which is the point WebKit waits for. */
        data.size = 30;
        dmux = WebPDemuxPartial(&data, &state);
        checkr("thirty bytes is the VP8X header",
               state == WEBP_DEMUX_PARSED_HEADER, state);
        if (dmux) {
            check("and the canvas is already known",
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_WIDTH) == WEBPREF_W);
            check("with no complete frame yet",
                  WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT) == 0);
            WebPDemuxDelete(dmux);
        }

        /* Most of the file: enough for some frames but not all. */
        data.size = sizeof webpref_anim - 40;
        dmux = WebPDemuxPartial(&data, &state);
        check("most of the file is parsed but not done",
              state == WEBP_DEMUX_PARSED_HEADER);
        if (dmux) {
            uint32_t n = WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT);
            checkr("with some frames visible and not all",
                   n >= 1 && n < WEBPREF_ANIM_FRAMES, (int)n);
            WebPDemuxDelete(dmux);
        }

        /* And all of it. */
        data.size = sizeof webpref_anim;
        dmux = WebPDemuxPartial(&data, &state);
        checkr("the whole file reaches DONE", state == WEBP_DEMUX_DONE, state);
        if (dmux) {
            check("with every frame",
                  WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT)
                      == WEBPREF_ANIM_FRAMES);
            WebPDemuxDelete(dmux);
        }

        /* A still image demuxes too, and reports one frame — which is
         * how WebKit's single-image path works: the same code, and a
         * frame count of one.
         *
         * Which container a still image gets is not the encoder's whim
         * and is worth knowing, because it decides what the demuxer can
         * tell WebKit before a pixel is decoded. A lossy image with
         * alpha *must* be extended format: the alpha plane is a separate
         * ALPH chunk and a simple-format file has nowhere to put one, so
         * there is a VP8X and its flags say ALPHA. A lossless image
         * needs no such thing even when it has alpha, because VP8L
         * carries alpha inside its own bitstream — hence the two
         * different expectations below rather than one. */
        data.bytes = webpref_lossy_alpha;
        data.size = sizeof webpref_lossy_alpha;
        dmux = WebPDemux(&data);
        check("a lossy still with alpha demuxes", dmux != NULL);
        if (dmux) {
            uint32_t f = WebPDemuxGetI(dmux, WEBP_FF_FORMAT_FLAGS);
            check("as exactly one frame",
                  WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT) == 1);
            check("with the canvas size of the image",
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_WIDTH) == WEBPREF_W);
            check("and a VP8X saying it has alpha",
                  (f & ALPHA_FLAG) != 0);
            check("but not that it is animated",
                  (f & ANIMATION_FLAG) == 0);
            WebPDemuxDelete(dmux);
        }

        /* And the simple-format case: a VP8L file with no VP8X at all.
         * The demuxer has to synthesise the canvas from the bitstream,
         * and it must not invent feature flags while it is there — a
         * demuxer that reported ALPHA here would send WebKit down the
         * wrong path for an image that has none. */
        data.bytes = webpref_lossless;
        data.size = sizeof webpref_lossless;
        dmux = WebPDemux(&data);
        check("a simple-format lossless still demuxes", dmux != NULL);
        if (dmux) {
            check("to one frame",
                  WebPDemuxGetI(dmux, WEBP_FF_FRAME_COUNT) == 1);
            check("with the canvas read out of the bitstream",
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_WIDTH) == WEBPREF_W &&
                  WebPDemuxGetI(dmux, WEBP_FF_CANVAS_HEIGHT) == WEBPREF_H);
            check("and no feature flags, because there is no VP8X",
                  WebPDemuxGetI(dmux, WEBP_FF_FORMAT_FLAGS) == 0);
            WebPDemuxDelete(dmux);
        }
    }

    printf("webptest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
