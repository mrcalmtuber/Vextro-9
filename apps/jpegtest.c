/*
 * jpegtest — libjpeg-turbo in ring 3.
 *
 * The library is upstream and correct; what could be wrong is the *port*
 * of it, and the port is the two hand-written configuration headers, a
 * freestanding C library underneath, and a cross compiler with no hosted
 * environment. So the checks below are chosen for what each would catch
 * if one of those three were subtly wrong rather than for covering the
 * API:
 *
 *   Decoding somebody else's bitstream. apps/jpeg_ref.h was encoded by
 *   macOS's `sips`, which shares no code with this library, and the
 *   picture it was made from is a formula the test recomputes. A round
 *   trip through our own encoder and back would prove the two halves of
 *   one library agree with each other — which they would even if both
 *   were wrong about the DCT.
 *
 *   Luma and chroma asserted separately. Averaging them would hide a
 *   decoder that had lost the chroma planes entirely, since two thirds
 *   of a correct answer looks like a good average.
 *
 *   The in-memory source manager, because that is the one a browser
 *   uses: WebKit hands the decoder a buffer it already has rather than a
 *   FILE*, and MEM_SRCDST_SUPPORTED in our jconfig.h is what makes that
 *   compile. If it were off, this file would not link.
 *
 *   An encode, so that the half of the library nothing else exercises
 *   runs at all — and its output is fed back through the decoder, which
 *   is a weaker claim than the first check and is labelled as such.
 *
 *   The error handler, which is the one piece of libjpeg every port gets
 *   wrong: it longjmps out of the library, and setjmp/longjmp on this
 *   system are libc/posix.c's own.
 */

#include "vextro.h"
#include "jpeg_ref.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <jerror.h>

/* LIBJPEG_TURBO_VERSION comes out of jconfig.h unquoted, the way
 * upstream's own configure_file leaves it. */
#define VX_STR2(x) #x
#define VX_STR(x)  VX_STR2(x)

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void check_le(const char *what, long got, long limit) {
    checks++;
    if (got > limit) {
        failures++;
        printf("  FAIL  %s: %ld, wanted at most %ld\n", what, got, limit);
    }
}

/*
 * ---- the error path, which is the part a port breaks ----
 *
 * libjpeg reports a fatal error by calling error_exit and expecting it
 * never to return; the documented way to survive one is to longjmp out
 * of it. That makes setjmp and longjmp part of this library's contract
 * rather than an optional convenience, and they are libc/posix.c's own
 * on this system — hand-written, saving exactly the callee-saved
 * registers the ABI says must survive a call.
 *
 * So the last check in this file deliberately hands the decoder
 * something that is not a JPEG, purely to make this function run. A port
 * where longjmp lost a register would come back to a corrupted frame
 * rather than to the `if` below.
 */
struct vx_err {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    int fired;
};

static void vx_error_exit(j_common_ptr cinfo) {
    struct vx_err *e = (struct vx_err *)cinfo->err;
    e->fired = 1;
    longjmp(e->setjmp_buffer, 1);
}

/* Quiet: a deliberately malformed stream would otherwise print a
 * warning per corrupt marker and bury the report. */
static void vx_output_message(j_common_ptr cinfo) { (void)cinfo; }

int main(void) {
    printf("jpegtest: libjpeg-turbo %s in ring 3\n",
           VX_STR(LIBJPEG_TURBO_VERSION));

    /* ============================================================
     *  1. decode a bitstream this system did not produce
     * ============================================================ */
    static unsigned char pixels[JPEG_REF_W * JPEG_REF_H * 3];
    {
        struct jpeg_decompress_struct cinfo;
        struct vx_err err;

        cinfo.err = jpeg_std_error(&err.pub);
        err.pub.error_exit = vx_error_exit;
        err.pub.output_message = vx_output_message;
        err.fired = 0;

        if (setjmp(err.setjmp_buffer)) {
            check("the reference image decodes without an error", 0);
            jpeg_destroy_decompress(&cinfo);
            goto after_decode;
        }

        jpeg_create_decompress(&cinfo);
        /* The in-memory source manager — what a browser uses, and what
         * MEM_SRCDST_SUPPORTED in our jconfig.h turns on. */
        jpeg_mem_src(&cinfo, jpeg_ref, (unsigned long)JPEG_REF_LEN);

        check("the header reads", jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK);
        check("the width is what sips wrote", (int)cinfo.image_width == JPEG_REF_W);
        check("the height is what sips wrote", (int)cinfo.image_height == JPEG_REF_H);
        check("three components", cinfo.num_components == 3);

        cinfo.out_color_space = JCS_RGB;
        check("decompression starts", jpeg_start_decompress(&cinfo) == TRUE);
        check("and gives three output components", cinfo.output_components == 3);

        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW row = &pixels[cinfo.output_scanline * JPEG_REF_W * 3];
            if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
        }
        check("every scanline came back",
              (int)cinfo.output_scanline == JPEG_REF_H);
        check("decompression finishes", jpeg_finish_decompress(&cinfo) == TRUE);
        jpeg_destroy_decompress(&cinfo);
    }
after_decode:

    /* ============================================================
     *  2. is it the picture it was made from?
     * ============================================================
     *
     * Luma and chroma separately. Red and green ramp smoothly and
     * survive quality 90 almost exactly; blue is an eight-pixel
     * checkerboard, which is precisely the frequency 4:2:0 subsampling
     * blurs, so it is allowed a wider band. Averaging the three would
     * let a decoder that had lost the chroma planes pass on the strength
     * of the two that were right.
     */
    {
        long err_r = 0, err_g = 0, err_b = 0;
        int worst_ramp = 0;
        for (int y = 0; y < JPEG_REF_H; y++) {
            for (int x = 0; x < JPEG_REF_W; x++) {
                unsigned char want[3];
                jpeg_expected(x, y, want);
                const unsigned char *got = &pixels[(y * JPEG_REF_W + x) * 3];
                int dr = (int)got[0] - (int)want[0];
                int dg = (int)got[1] - (int)want[1];
                int db = (int)got[2] - (int)want[2];
                if (dr < 0) dr = -dr;
                if (dg < 0) dg = -dg;
                if (db < 0) db = -db;
                err_r += dr; err_g += dg; err_b += db;
                if (dr > worst_ramp) worst_ramp = dr;
                if (dg > worst_ramp) worst_ramp = dg;
            }
        }
        const long n = JPEG_REF_W * JPEG_REF_H;
        check_le("the red ramp is within a few levels", err_r / n, 6);
        check_le("the green ramp is within a few levels", err_g / n, 6);
        check_le("the blue checkerboard survives subsampling", err_b / n, 40);
        /* A single wildly wrong pixel in a smooth ramp is a block the
         * IDCT got wrong, and a mean would absorb it. */
        check_le("no ramp pixel is wildly wrong", worst_ramp, 40);

        /* And a corner, named, so a failure says where rather than how
         * much. The top-left is the darkest end of both ramps. */
        unsigned char want[3];
        jpeg_expected(0, 0, want);
        const int d0 = (int)pixels[0] - (int)want[0];
        check("the top-left pixel is the top-left pixel",
              d0 > -12 && d0 < 12);
    }

    /* ============================================================
     *  3. the encoder, and back through the decoder
     * ============================================================
     *
     * A weaker claim than the first section and labelled as one: this
     * checks that the two halves of the library agree with each other,
     * which they would even if both shared a wrong idea of the DCT.
     * It is here because nothing else runs the encoder at all, and an
     * encoder that faulted in ring 3 — on a stack, through our malloc —
     * would otherwise not be discovered.
     */
    {
        unsigned char *out = NULL;
        unsigned long outlen = 0;
        struct jpeg_compress_struct cinfo;
        struct vx_err err;

        cinfo.err = jpeg_std_error(&err.pub);
        err.pub.error_exit = vx_error_exit;
        err.pub.output_message = vx_output_message;
        err.fired = 0;

        if (setjmp(err.setjmp_buffer)) {
            check("compression runs without an error", 0);
            jpeg_destroy_compress(&cinfo);
            goto after_encode;
        }

        jpeg_create_compress(&cinfo);
        jpeg_mem_dest(&cinfo, &out, &outlen);
        cinfo.image_width      = JPEG_REF_W;
        cinfo.image_height     = JPEG_REF_H;
        cinfo.input_components = 3;
        cinfo.in_color_space   = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, 92, TRUE);
        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row = &pixels[cinfo.next_scanline * JPEG_REF_W * 3];
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);

        check("the encoder produced a stream", out != NULL && outlen > 128);
        /* Every JPEG begins FFD8 and ends FFD9. Cheap, and it catches a
         * destination manager that dropped the tail — which is exactly
         * what an in-memory manager gets wrong when realloc misbehaves. */
        check("it starts with SOI",
              out && outlen >= 2 && out[0] == 0xFF && out[1] == 0xD8);
        check("it ends with EOI",
              out && outlen >= 2 && out[outlen - 2] == 0xFF &&
              out[outlen - 1] == 0xD9);
        /* Sixty-four squared of gradient at quality 92 is a few
         * kilobytes. A stream of a hundred bytes would mean an encoder
         * that wrote headers and no scan; one of twelve kilobytes would
         * mean no compression happened at all. */
        check("and it is a plausible size", outlen > 700 && outlen < 12288);

        if (out) {
            struct jpeg_decompress_struct dc;
            struct vx_err derr;
            static unsigned char again[JPEG_REF_W * JPEG_REF_H * 3];

            dc.err = jpeg_std_error(&derr.pub);
            derr.pub.error_exit = vx_error_exit;
            derr.pub.output_message = vx_output_message;
            derr.fired = 0;

            if (setjmp(derr.setjmp_buffer)) {
                check("our own stream decodes again", 0);
                jpeg_destroy_decompress(&dc);
                free(out);
                goto after_encode;
            }
            jpeg_create_decompress(&dc);
            jpeg_mem_src(&dc, out, outlen);
            jpeg_read_header(&dc, TRUE);
            dc.out_color_space = JCS_RGB;
            jpeg_start_decompress(&dc);
            while (dc.output_scanline < dc.output_height) {
                JSAMPROW row = &again[dc.output_scanline * JPEG_REF_W * 3];
                if (jpeg_read_scanlines(&dc, &row, 1) != 1) break;
            }
            jpeg_finish_decompress(&dc);
            jpeg_destroy_decompress(&dc);

            long e = 0;
            for (long i = 0; i < JPEG_REF_W * JPEG_REF_H * 3; i++) {
                int d = (int)again[i] - (int)pixels[i];
                e += d < 0 ? -d : d;
            }
            check("our own round trip is close to where it started",
                  e / (JPEG_REF_W * JPEG_REF_H * 3) <= 8);
            free(out);
        }
    }
after_encode:

    /* ============================================================
     *  4. the error path
     * ============================================================
     *
     * Deliberately not a JPEG. The only reason this check exists is to
     * make error_exit run, which makes longjmp run, which is the piece
     * of this port most likely to be quietly wrong — libc/posix.c's
     * setjmp saves the callee-saved registers by hand, and a missing one
     * shows up as a return into a corrupted frame rather than as a
     * compile error.
     */
    {
        static const unsigned char garbage[64] = {
            0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x02, 0x77, 0x77
        };
        struct jpeg_decompress_struct cinfo;
        struct vx_err err;
        volatile int survived = 0;

        cinfo.err = jpeg_std_error(&err.pub);
        err.pub.error_exit = vx_error_exit;
        err.pub.output_message = vx_output_message;
        err.fired = 0;

        if (setjmp(err.setjmp_buffer) == 0) {
            jpeg_create_decompress(&cinfo);
            jpeg_mem_src(&cinfo, garbage, sizeof(garbage));
            jpeg_read_header(&cinfo, TRUE);
            jpeg_start_decompress(&cinfo);
            while (cinfo.output_scanline < cinfo.output_height) {
                unsigned char row[8];
                JSAMPROW r = row;
                if (jpeg_read_scanlines(&cinfo, &r, 1) != 1) break;
            }
        } else {
            survived = 1;
        }
        jpeg_destroy_decompress(&cinfo);

        check("a malformed stream raises an error rather than running on",
              err.fired == 1);
        check("and longjmp came back to the right frame", survived == 1);
    }

    printf("jpegtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/* The loader enters a program here and expects it to return; main() is
 * this file's own shape because it reads better with one. */
void _start(void) { main(); }
