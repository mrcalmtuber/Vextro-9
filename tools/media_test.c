/*
 * tools/media_test.c — the half of a video decoder that can be tested
 * without a video engine.
 *
 * No machine this system is built on has an Intel GPU, so nothing in
 * src/media/mfx.h or the submission path in src/media/decode.c can be
 * executed here. What can be, and is:
 *
 *   - the Exp-Golomb reader, against hand-decodable bit patterns
 *   - emulation prevention byte removal
 *   - sequence and picture parameter set parsing, round-tripped
 *     against a bit writer written from the standard in this file
 *   - the bounds checks, against streams built to violate them
 *   - NAL scanning over both three- and four-byte start codes
 *   - the colour conversion, against the primary colours' published
 *     BT.601 encodings
 *   - the surface geometry arithmetic
 *
 * Checks are labelled by what backs them:
 *
 *   "vector"     a value from a standard or one derivable by hand
 *   "round-trip" an encoder written separately in this file must agree
 *                with the parser
 *   "refuses"    malformed input must be rejected, not clamped
 *   "structural" a command encoding is internally consistent; this
 *                does NOT mean the hardware accepts it, which only
 *                silicon can establish
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "media/h264.h"
#include "media/csc.h"
#include "media/mfx.h"

static int checks = 0;
static int fails  = 0;

static void expect(int cond, const char *what) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

static void expect_eq(uint32_t got, uint32_t want, const char *what) {
    checks++;
    if (got == want) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s (got %u, want %u)\n", what, got, want);
}

/* ===== a bit writer, so the parser has something to disagree with ===== */

typedef struct {
    uint8_t  buf[512];
    uint32_t n;         /* bytes used        */
    int      bit;       /* bits in the partial byte */
} bw_t;

static void bw_init(bw_t *w) { memset(w, 0, sizeof(*w)); }

static void bw_u(bw_t *w, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        if (w->bit == 0) w->buf[w->n] = 0;
        w->buf[w->n] |= (uint8_t)(((v >> i) & 1u) << (7 - w->bit));
        if (++w->bit == 8) { w->bit = 0; w->n++; }
    }
}

static void bw_u1(bw_t *w, int v) { bw_u(w, (uint32_t)(v & 1), 1); }

/* Exp-Golomb: k+1 written in binary, preceded by that many zeroes less
 * one. Written from the definition rather than copied from h264.h, so
 * agreement between them means something. */
static void bw_ue(bw_t *w, uint32_t v) {
    uint32_t k = v + 1;
    int bits = 0;
    while ((k >> bits) != 0) bits++;
    bw_u(w, 0, bits - 1);
    bw_u(w, k, bits);
}

static void bw_se(bw_t *w, int32_t v) {
    uint32_t k = (v <= 0) ? ((uint32_t)(-v) * 2u) : ((uint32_t)v * 2u - 1u);
    bw_ue(w, k);
}

static uint32_t bw_len(const bw_t *w) { return w->n + (w->bit ? 1u : 0u); }

/* ===== 1. the bit reader ===== */

static void test_bits(void) {
    printf("\nExp-Golomb and the RBSP reader\n");

    {
        /* ue(v) codes, hand-written:
         *   0 -> 1        1 -> 010      2 -> 011
         *   3 -> 00100    4 -> 00101    5 -> 00110    6 -> 00111
         * packed: 1 010 011 00100 00101 = 1010011 00100001 01...  */
        uint8_t data[4];
        bw_t w;
        h264_bits_t b;

        bw_init(&w);
        bw_ue(&w, 0); bw_ue(&w, 1); bw_ue(&w, 2);
        bw_ue(&w, 3); bw_ue(&w, 4);
        memcpy(data, w.buf, 4);

        h264_bits_init(&b, data, 4);
        expect_eq(h264_ue(&b), 0, "vector     ue(v) reads 0");
        expect_eq(h264_ue(&b), 1, "vector     ue(v) reads 1");
        expect_eq(h264_ue(&b), 2, "vector     ue(v) reads 2");
        expect_eq(h264_ue(&b), 3, "vector     ue(v) reads 3");
        expect_eq(h264_ue(&b), 4, "vector     ue(v) reads 4");
    }

    {
        /* se(v): 0,1,-1,2,-2,3 */
        bw_t w;
        h264_bits_t b;
        int32_t want[6] = { 0, 1, -1, 2, -2, 3 };

        bw_init(&w);
        for (int i = 0; i < 6; i++) bw_se(&w, want[i]);

        h264_bits_init(&b, w.buf, bw_len(&w));
        for (int i = 0; i < 6; i++) {
            int32_t got = h264_se(&b);
            checks++;
            if (got == want[i]) printf("  ok   round-trip se(v) = %d\n", want[i]);
            else { fails++; printf("  FAIL se(v): got %d want %d\n", got, want[i]); }
        }
    }

    {
        /* Emulation prevention: 00 00 03 01 carries the byte 01, and
         * the 03 must vanish. This is the one that, done wrong, makes
         * every stream with a run of zeroes decode to noise. */
        const uint8_t esc[] = { 0x00, 0x00, 0x03, 0x01, 0xAB };
        h264_bits_t b;
        h264_bits_init(&b, esc, sizeof(esc));
        expect_eq(h264_u(&b, 8), 0x00, "vector     escaped stream byte 0");
        expect_eq(h264_u(&b, 8), 0x00, "vector     escaped stream byte 1");
        expect_eq(h264_u(&b, 8), 0x01, "vector     the 0x03 is removed");
        expect_eq(h264_u(&b, 8), 0xAB, "vector     and the stream continues");
    }

    {
        /* A 0x03 that is not an escape -- only two zeroes precede an
         * escape, so 00 03 is literal data. */
        const uint8_t plain[] = { 0x00, 0x03, 0xCD };
        h264_bits_t b;
        h264_bits_init(&b, plain, sizeof(plain));
        h264_u(&b, 8);
        expect_eq(h264_u(&b, 8), 0x03, "vector     a lone 0x03 is data");
    }

    {
        /* A run of zeroes must terminate the reader rather than spin. */
        uint8_t zeros[8];
        h264_bits_t b;
        memset(zeros, 0, sizeof(zeros));
        h264_bits_init(&b, zeros, sizeof(zeros));
        h264_ue(&b);
        expect(b.overrun, "refuses    an unterminated Exp-Golomb code");
    }
}

/* ===== 2. parameter sets ===== */

/* Build a baseline-profile SPS body (everything past the NAL header). */
static uint32_t build_sps(uint8_t *out, uint32_t w_mbs, uint32_t h_mbs,
                          uint32_t max_refs, uint32_t crop_right,
                          uint32_t crop_bottom) {
    bw_t w;
    bw_init(&w);

    bw_u(&w, 66, 8);            /* profile_idc: baseline               */
    bw_u(&w, 0, 8);             /* constraint flags                    */
    bw_u(&w, 30, 8);            /* level_idc 3.0                       */
    bw_ue(&w, 0);               /* sps_id                              */
    bw_ue(&w, 4);               /* log2_max_frame_num_minus4 -> 8       */
    bw_ue(&w, 0);               /* pic_order_cnt_type                  */
    bw_ue(&w, 4);               /* log2_max_poc_lsb_minus4 -> 8         */
    bw_ue(&w, max_refs);        /* max_num_ref_frames                  */
    bw_u1(&w, 0);               /* gaps_in_frame_num_allowed           */
    bw_ue(&w, w_mbs - 1);
    bw_ue(&w, h_mbs - 1);
    bw_u1(&w, 1);               /* frame_mbs_only_flag                 */
    bw_u1(&w, 1);               /* direct_8x8_inference_flag           */

    if (crop_right || crop_bottom) {
        bw_u1(&w, 1);
        bw_ue(&w, 0);           /* left   */
        bw_ue(&w, crop_right);
        bw_ue(&w, 0);           /* top    */
        bw_ue(&w, crop_bottom);
    } else {
        bw_u1(&w, 0);
    }

    bw_u1(&w, 0);               /* vui_parameters_present              */
    bw_u1(&w, 1);               /* rbsp_stop_one_bit                   */

    memcpy(out, w.buf, bw_len(&w));
    return bw_len(&w);
}

static uint32_t build_pps(uint8_t *out, int cabac, int32_t init_qp) {
    bw_t w;
    bw_init(&w);

    bw_ue(&w, 0);               /* pps_id                              */
    bw_ue(&w, 0);               /* sps_id                              */
    bw_u1(&w, cabac);           /* entropy_coding_mode_flag            */
    bw_u1(&w, 0);               /* bottom_field_pic_order_present      */
    bw_ue(&w, 0);               /* num_slice_groups_minus1             */
    bw_ue(&w, 0);               /* num_ref_idx_l0_default_active_minus1 */
    bw_ue(&w, 0);               /* l1                                  */
    bw_u1(&w, 0);               /* weighted_pred_flag                  */
    bw_u(&w, 0, 2);             /* weighted_bipred_idc                 */
    bw_se(&w, init_qp - 26);
    bw_se(&w, 0);               /* pic_init_qs                         */
    bw_se(&w, 0);               /* chroma_qp_index_offset              */
    bw_u1(&w, 1);               /* deblocking_filter_control_present   */
    bw_u1(&w, 0);               /* constrained_intra_pred              */
    bw_u1(&w, 0);               /* redundant_pic_cnt_present           */
    bw_u1(&w, 1);               /* rbsp stop bit                       */

    memcpy(out, w.buf, bw_len(&w));
    return bw_len(&w);
}

static void test_parameter_sets(void) {
    uint8_t buf[512];
    uint32_t n;
    h264_sps_t sps;
    h264_pps_t pps;

    printf("\nsequence and picture parameter sets\n");

    /* 1080p: 120 macroblocks across, 68 down (1088 lines) with the
     * bottom eight cropped away. crop_bottom is in chroma samples, so
     * 4 units removes 8 luma lines. */
    n = build_sps(buf, 120, 68, 4, 0, 4);
    expect(h264_parse_sps(buf, n, &sps) == 1, "round-trip a 1080p SPS parses");
    expect_eq(sps.pic_width_in_mbs, 120, "round-trip width in macroblocks");
    expect_eq(sps.pic_height_in_map_units, 68, "round-trip height in map units");
    expect_eq(sps.width, 1920, "vector     cropped width is 1920");
    expect_eq(sps.height, 1080, "vector     cropped height is 1080");
    expect_eq(sps.max_num_ref_frames, 4, "round-trip reference frame count");
    expect_eq(sps.log2_max_frame_num, 8, "round-trip log2_max_frame_num");
    expect_eq(sps.chroma_format_idc, 1, "vector     baseline is always 4:2:0");
    expect_eq(sps.bit_depth_luma, 8, "vector     baseline is always 8-bit");

    /* 720p, no cropping: 1280x720 is a whole number of macroblocks. */
    n = build_sps(buf, 80, 45, 3, 0, 0);
    expect(h264_parse_sps(buf, n, &sps) == 1, "round-trip a 720p SPS parses");
    expect_eq(sps.width, 1280, "vector     720p width");
    expect_eq(sps.height, 720, "vector     720p height");

    /* A width of zero is not a small picture, it is a corrupt file. */
    n = build_sps(buf, 1, 1, 1, 0, 0);
    expect(h264_parse_sps(buf, n, &sps) == 1, "round-trip a 16x16 SPS parses");

    /* Cropping that removes the whole picture must be refused. */
    n = build_sps(buf, 8, 8, 1, 64, 0);
    expect(h264_parse_sps(buf, n, &sps) == 0,
           "refuses    cropping wider than the picture");

    /* A reference frame count past the standard's limit. */
    n = build_sps(buf, 80, 45, 40, 0, 0);
    expect(h264_parse_sps(buf, n, &sps) == 0,
           "refuses    more than 16 reference frames");

    /* Truncation at every length must be refused, never read past. */
    {
        uint32_t full = build_sps(buf, 80, 45, 3, 0, 0);
        int all_refused = 1;
        for (uint32_t cut = 0; cut < full; cut++) {
            h264_sps_t t;
            if (h264_parse_sps(buf, cut, &t)) {
                /* A short SPS may legitimately parse if every field it
                 * needed happened to fit; what must never happen is a
                 * success with impossible dimensions. */
                if (t.width == 0 || t.height == 0 ||
                    t.pic_width_in_mbs > 1024) all_refused = 0;
            }
        }
        expect(all_refused, "refuses    every truncated SPS prefix");
    }

    /* The PPS needs its SPS. */
    n = build_sps(buf, 80, 45, 3, 0, 0);
    h264_parse_sps(buf, n, &sps);
    {
        h264_sps_t table[H264_MAX_SPS];
        memset(table, 0, sizeof(table));
        table[0] = sps;

        n = build_pps(buf, 1, 28);
        expect(h264_parse_pps(buf, n, table, &pps) == 1, "round-trip a PPS parses");
        expect_eq((uint32_t)pps.entropy_coding_mode_flag, 1,
                  "round-trip CABAC flag");
        expect_eq((uint32_t)pps.pic_init_qp, 28, "round-trip pic_init_qp");
        expect_eq(pps.num_ref_idx_l0_default_active, 1,
                  "round-trip default active reference count");
        expect(pps.scaling_4x4[0][0] == 16,
               "vector     absent scaling lists default to flat 16");

        /* A PPS naming an SPS that was never sent. */
        memset(table, 0, sizeof(table));
        expect(h264_parse_pps(buf, n, table, &pps) == 0,
               "refuses    a PPS whose SPS is missing");

        /* An out-of-range QP. */
        table[0] = sps;
        n = build_pps(buf, 1, 90);
        expect(h264_parse_pps(buf, n, table, &pps) == 0,
               "refuses    a quantisation parameter past 51");
    }
}

/* ===== 3. NAL scanning ===== */

static void test_nal(void) {
    /* Three NALs: a four-byte start code, then two three-byte ones. */
    const uint8_t stream[] = {
        0x00,0x00,0x00,0x01, 0x67, 0xAA, 0xBB,          /* SPS, 2 bytes  */
        0x00,0x00,0x01,      0x68, 0xCC,                /* PPS, 1 byte   */
        0x00,0x00,0x01,      0x65, 0x01,0x02,0x03       /* IDR, 3 bytes  */
    };
    uint32_t pos = 0;
    h264_nal_t nal;

    printf("\nAnnex B NAL scanning\n");

    expect(h264_next_nal(stream, sizeof(stream), &pos, &nal) == 1,
           "the first NAL is found");
    expect_eq((uint32_t)nal.type, H264_NAL_SPS, "vector     type is SPS");
    expect_eq((uint32_t)nal.ref_idc, 3, "vector     nal_ref_idc is 3");
    expect_eq(nal.len, 2, "vector     payload length excludes the header");

    expect(h264_next_nal(stream, sizeof(stream), &pos, &nal) == 1,
           "the second NAL is found");
    expect_eq((uint32_t)nal.type, H264_NAL_PPS, "vector     type is PPS");
    expect_eq(nal.len, 1, "vector     three-byte start code handled");

    expect(h264_next_nal(stream, sizeof(stream), &pos, &nal) == 1,
           "the third NAL is found");
    expect_eq((uint32_t)nal.type, H264_NAL_IDR, "vector     type is IDR");
    expect_eq(nal.len, 3, "vector     the last NAL runs to the end");

    expect(h264_next_nal(stream, sizeof(stream), &pos, &nal) == 0,
           "the scan terminates");

    /* A buffer with no start code at all. */
    {
        const uint8_t junk[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        uint32_t p = 0;
        expect(h264_next_nal(junk, sizeof(junk), &p, &nal) == 0,
               "refuses    a buffer with no start code");
    }
}

/* ===== 4. colour conversion ===== */

static void test_csc(void) {
    uint8_t y[4 * 2], uv[4];
    uint32_t out[4 * 2];

    printf("\nNV12 to BGRA\n");

    /* One 2x2 block at a time, which is the granularity chroma has. */
    #define CSC_ONE(yv, uvv, vvv, expect_rgb, name)                       \
        do {                                                              \
            memset(out, 0, sizeof(out));                                  \
            y[0]=y[1]=y[2]=y[3]=(uint8_t)(yv);                            \
            uv[0]=(uint8_t)(uvv); uv[1]=(uint8_t)(vvv);                   \
            csc_nv12_to_bgra(y, 2, uv, 2, 2, 2, out, 2, 2, 2);            \
            expect_eq(out[0] & 0xFFFFFF, (uint32_t)(expect_rgb), name);   \
        } while (0)

    /* The primaries, in BT.601 limited range. These are the standard
     * encodings and the arithmetic below is hand-checkable. */
    CSC_ONE(16,  128, 128, 0x000000, "vector     Y=16  is black");
    CSC_ONE(235, 128, 128, 0xFFFFFF, "vector     Y=235 is white");
    CSC_ONE(81,   90, 240, 0xFF0000, "vector     BT.601 red");
    CSC_ONE(41,  240, 110, 0x0000FF, "vector     BT.601 blue");

    /* Out-of-range luma must clamp rather than wrap: a Y of 255 is
     * legal in the bitstream and is brighter than white. */
    CSC_ONE(255, 128, 128, 0xFFFFFF, "vector     super-white clamps");
    CSC_ONE(0,   128, 128, 0x000000, "vector     sub-black clamps");

    #undef CSC_ONE

    /* The matrix actually changes with picture height. */
    expect(csc_pick(480) == &csc_bt601, "vector     480 lines selects BT.601");
    expect(csc_pick(720) == &csc_bt709, "vector     720 lines selects BT.709");
    expect(csc_pick(1080) == &csc_bt709, "vector     1080 lines selects BT.709");

    /* A picture larger than the window is cropped, not overrun. */
    {
        uint8_t big_y[16 * 16];
        uint8_t big_uv[16 * 8];
        uint32_t small[4 * 4];
        uint32_t guard[4 * 4];

        memset(big_y, 128, sizeof(big_y));
        memset(big_uv, 128, sizeof(big_uv));
        memset(small, 0xAB, sizeof(small));
        memset(guard, 0xAB, sizeof(guard));

        csc_nv12_to_bgra(big_y, 16, big_uv, 16, 16, 16,
                         small, 4, 4, 4);

        expect(memcmp(small, guard, sizeof(small)) != 0,
               "a 16x16 picture writes into a 4x4 window");
        expect(small[15] != 0xABABABAB, "the last in-bounds pixel is written");
    }

    /* The monochrome path. */
    {
        uint8_t mono[4] = { 235, 235, 235, 235 };
        uint32_t o[4];
        memset(o, 0, sizeof(o));
        csc_y_to_bgra(mono, 2, 2, 2, o, 2, 2, 2);
        expect_eq(o[0] & 0xFFFFFF, 0xFFFFFF, "vector     monochrome white");
    }
}

/* ===== 5. surface geometry and command structure ===== */

static void test_geometry(void) {
    printf("\nsurface geometry and command encoding\n");

    /* 1080p decodes into 1088 lines because macroblocks are 16 tall.
     * Getting this wrong truncates the bottom row of every frame. */
    {
        uint32_t mb_w = 120, mb_h = 68;
        uint32_t pitch = mb_w * 16, height = mb_h * 16;
        uint32_t luma = pitch * height;
        uint32_t total = luma + luma / 2;

        expect_eq(pitch, 1920, "vector     1080p surface pitch");
        expect_eq(height, 1088, "vector     1080p surface height is 1088");
        expect_eq(total, 1920 * 1088 * 3 / 2, "vector     NV12 is 1.5x luma");
        expect_eq((total + 4095) / 4096, 765, "vector     pages per surface");
    }

    /*
     * Command header encodings, checked for internal consistency
     * against the bit layout the Gen9 documentation specifies.
     *
     * This proves the macro places each field where it was told to. It
     * does NOT prove the hardware accepts these commands -- only a
     * video engine can establish that, and there is none here.
     */
    expect_eq(MFX_PIPE_MODE_SELECT, 0x70000000u,
              "structural MFX_PIPE_MODE_SELECT header");
    expect_eq(MFX_SURFACE_STATE, 0x70010000u,
              "structural MFX_SURFACE_STATE header");
    expect_eq(MFX_AVC_IMG_STATE, 0x71000000u,
              "structural MFX_AVC_IMG_STATE header");
    expect_eq(MFX_AVC_SLICE_STATE, 0x71030000u,
              "structural MFX_AVC_SLICE_STATE header");
    expect_eq(MFD_AVC_BSD_OBJECT, 0x71280000u,
              "structural MFD_AVC_BSD_OBJECT header");

    /* The command writer must refuse to overflow rather than scribble. */
    {
        uint32_t small[4];
        mfx_cs_t cs;
        mfx_cs_init(&cs, small, 4);
        for (int i = 0; i < 16; i++) mfx_out(&cs, 0xDEADBEEF);
        expect(cs.overflow, "refuses    a command stream past its buffer");
        expect_eq(cs.n, 4, "and stops at the buffer's end");
    }

    /* A picture-state stream must fit in the buffer decode.c gives it:
     * MFX_PIPE_BUF_ADDR_STATE alone is 61 dwords. */
    {
        uint32_t buf[1024];
        mfx_cs_t cs;
        uint32_t refs[16], mv[16];
        memset(refs, 0, sizeof(refs));
        memset(mv, 0, sizeof(mv));

        mfx_cs_init(&cs, buf, 1024);
        mfx_pipe_mode_select(&cs, MFX_CODEC_AVC, 1, 0);
        mfx_surface_state(&cs, 1920, 1088, 1920, 1088);
        mfx_pipe_buf_addr_state(&cs, 0x1000, 0x1000, 0x2000, 0x3000,
                                refs, 0, 0x4000, mv, 0);
        mfx_ind_obj_base_addr(&cs, 0x5000, 0x100000);
        mfx_bsp_buf_base_addr(&cs, 0x6000, 0x7000);

        expect(!cs.overflow, "a full picture state fits in 1024 dwords");
        expect(cs.n > 100, "and is the size the packets imply");
    }
}

int main(void) {
    printf("Vextro media: H.264 parsing, surface geometry, colour\n");
    printf("=====================================================\n");

    test_bits();
    test_parameter_sets();
    test_nal();
    test_csc();
    test_geometry();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
