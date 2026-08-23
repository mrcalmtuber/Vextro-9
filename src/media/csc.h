#ifndef MEDIA_CSC_H
#define MEDIA_CSC_H

/*
 * src/media/csc.h — NV12 out of the decoder, BGRA into a window.
 *
 * The MFX block decodes to NV12 and nothing else: a plane of luma at
 * full resolution, then a plane of Cb and Cr interleaved at half
 * resolution in both directions. A window framebuffer in this system is
 * 32-bit BGRA. Something has to convert, and there are two ways:
 *
 *   VEBOX   the video enhancement block does it in fixed function,
 *           reading NV12 and writing BGRA with no CPU involvement.
 *           That is the path src/media/decode.c prefers.
 *
 *   here    integer arithmetic, one pass over the picture.
 *
 * The second exists because it has to. A machine with no Intel GPU --
 * which is every machine this system is developed and tested on -- has
 * no VEBOX, and a decoder that produced NV12 nobody could display would
 * be useless. It is also the only part of the output path that can be
 * checked against known values without silicon, which is why it lives
 * in its own header and tools/media_test.c exercises it.
 *
 * ---- integer, and why ----
 *
 * The kernel is built with SSE available, so floating point would work.
 * It is still wrong here: the coefficients below are the standard
 * fixed-point approximations that every other decoder uses, and
 * matching them bit for bit is what makes a frame from this path
 * comparable with a frame from anywhere else. A float pipeline would
 * differ in the last bit on some pixels and there would be no way to
 * tell that from a bug.
 */

#include <stdint.h>

/*
 * The two colour matrices.
 *
 * Standard definition content is BT.601 and high definition is BT.709,
 * and they genuinely differ -- using one where the other belongs tints
 * the picture, most visibly in reds and greens. H.264 signals which in
 * the VUI, which this decoder does not parse, so the choice is made on
 * picture height the way most players do: 720 lines and above is
 * BT.709. That is a heuristic and it is right for essentially all real
 * content, but it is a heuristic.
 *
 * Both are the "limited range" forms: luma runs 16-235 and chroma
 * 16-240 rather than the full 0-255, which is what broadcast video has
 * always done and what H.264 defaults to.
 */
typedef struct {
    int32_t y_scale;        /* applied to (Y - 16)   */
    int32_t r_v;
    int32_t g_u, g_v;
    int32_t b_u;
} csc_matrix_t;

static const csc_matrix_t csc_bt601 = { 298, 409, -100, -208, 516 };
static const csc_matrix_t csc_bt709 = { 298, 459,  -55, -136, 541 };

static const csc_matrix_t *csc_pick(uint32_t height) {
    return (height >= 720) ? &csc_bt709 : &csc_bt601;
}

static inline uint8_t csc_clamp(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/*
 * Convert one NV12 picture into BGRA.
 *
 * `y_plane` is `y_pitch` bytes per line; `uv_plane` is `uv_pitch` bytes
 * per line and holds Cb,Cr pairs for each 2x2 block of luma. `dst` is
 * `dst_pitch` *pixels* per line, which is what a framebuffer is
 * naturally described in.
 *
 * The output is clipped to the destination: a 1080p video in a 598x402
 * window writes the top-left 598x402 of the frame rather than running
 * off the end. Scaling is deliberately not done here -- a nearest
 * neighbour shrink would look far worse than the hardware scaler that
 * belongs in this path, and pretending otherwise would hide that the
 * scaler is missing.
 */
static void csc_nv12_to_bgra(const uint8_t *y_plane, uint32_t y_pitch,
                             const uint8_t *uv_plane, uint32_t uv_pitch,
                             uint32_t width, uint32_t height,
                             uint32_t *dst, uint32_t dst_pitch,
                             uint32_t dst_w, uint32_t dst_h) {
    const csc_matrix_t *m = csc_pick(height);
    uint32_t w = (width  < dst_w) ? width  : dst_w;
    uint32_t h = (height < dst_h) ? height : dst_h;

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *yr = y_plane + (uint64_t)y * y_pitch;
        const uint8_t *cr = uv_plane + (uint64_t)(y >> 1) * uv_pitch;
        uint32_t *dr = dst + (uint64_t)y * dst_pitch;

        for (uint32_t x = 0; x < w; x++) {
            int32_t Y = ((int32_t)yr[x] - 16) * m->y_scale;
            int32_t U = (int32_t)cr[(x >> 1) * 2 + 0] - 128;
            int32_t V = (int32_t)cr[(x >> 1) * 2 + 1] - 128;

            uint8_t r = csc_clamp((Y + m->r_v * V + 128) >> 8);
            uint8_t g = csc_clamp((Y + m->g_u * U + m->g_v * V + 128) >> 8);
            uint8_t b = csc_clamp((Y + m->b_u * U + 128) >> 8);

            dr[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/*
 * The luma-only path.
 *
 * Used when a stream is monochrome (chroma_format_idc 0), where there
 * is no chroma plane at all and reading one would be a read of whatever
 * follows the luma plane in memory.
 */
static void csc_y_to_bgra(const uint8_t *y_plane, uint32_t y_pitch,
                          uint32_t width, uint32_t height,
                          uint32_t *dst, uint32_t dst_pitch,
                          uint32_t dst_w, uint32_t dst_h) {
    uint32_t w = (width  < dst_w) ? width  : dst_w;
    uint32_t h = (height < dst_h) ? height : dst_h;

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *yr = y_plane + (uint64_t)y * y_pitch;
        uint32_t *dr = dst + (uint64_t)y * dst_pitch;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = csc_clamp((((int32_t)yr[x] - 16) * 298 + 128) >> 8);
            dr[x] = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
    }
}

#endif /* MEDIA_CSC_H */
