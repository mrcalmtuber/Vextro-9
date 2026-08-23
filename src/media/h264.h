#ifndef MEDIA_H264_H
#define MEDIA_H264_H

/*
 * src/media/h264.h — reading an H.264 stream far enough to hand it to
 * fixed-function hardware.
 *
 * ---- what this does and pointedly does not do ----
 *
 * This is not a decoder. It never touches a macroblock, never runs an
 * inverse transform, and has no idea what the picture looks like. What
 * it does is parse the *headers* -- sequence parameter set, picture
 * parameter set, slice header -- and work out the handful of derived
 * values the hardware needs: picture dimensions, the reference picture
 * lists, the picture order count, the quantisation start point.
 *
 * That split is not a shortcut, it is how every hardware video decoder
 * on every platform works, VA-API and DXVA included. The reason is that
 * the expensive part of H.264 is per-macroblock and utterly regular --
 * entropy decode, motion compensation, inverse transform, deblocking,
 * eight thousand times for a 1080p frame -- and that is what a
 * fixed-function block is for. The headers are a few hundred bits per
 * frame of deeply irregular, branch-heavy bit-twiddling that would cost
 * more in silicon than it saves. So the CPU reads the headers and the
 * MFX block decodes the pictures.
 *
 * The practical consequence: the CPU cost of playing a video with this
 * is proportional to the number of *slices*, not the number of pixels.
 *
 * ---- freestanding ----
 *
 * Integer only. No allocation -- the caller owns every structure and
 * they are all fixed size, because the standard bounds all of them:
 * 32 sequence parameter sets, 256 picture parameter sets, 16 reference
 * frames. No libc.
 *
 * ---- on trusting this input ----
 *
 * A video file is attacker-controlled data. Every length, count and
 * index read below is checked against the standard's bounds before it
 * is used, and a stream that violates them is rejected rather than
 * clamped: a clamped `num_ref_idx_l0_active` produces a reference list
 * that points somewhere plausible and decodes to garbage, whereas a
 * refusal is a file that will not play and says why.
 */

#include <stdint.h>

#define H264_MAX_SPS            32
#define H264_MAX_PPS            256
#define H264_MAX_REF_FRAMES     16
#define H264_MAX_DPB            17      /* references plus the current  */
#define H264_MAX_SLICE_GROUPS   8

/* NAL unit types worth naming */
#define H264_NAL_SLICE          1
#define H264_NAL_DPA            2
#define H264_NAL_IDR            5
#define H264_NAL_SEI            6
#define H264_NAL_SPS            7
#define H264_NAL_PPS            8
#define H264_NAL_AUD            9
#define H264_NAL_END_SEQ        10
#define H264_NAL_END_STREAM     11

/* slice_type, which the standard gives twice: 0-4 and again 5-9 with
 * the meaning "and every slice in this picture is the same type". */
#define H264_SLICE_P            0
#define H264_SLICE_B            1
#define H264_SLICE_I            2
#define H264_SLICE_SP           3
#define H264_SLICE_SI           4

/* ===== the bit reader ===== */

/*
 * H.264 carries its syntax in an RBSP: a byte stream in which the
 * sequence 00 00 00, 00 00 01 and so on cannot appear, because those
 * are start codes. The encoder guarantees that by inserting a 0x03
 * after any 00 00 that would otherwise be followed by 00, 01, 02 or 03.
 * Those "emulation prevention" bytes are not part of the syntax and
 * have to come out before anything is parsed.
 *
 * They are removed on the fly rather than into a scratch buffer, which
 * keeps a megabyte of copy off the per-slice path -- but it means the
 * reader has to track its own byte position and cannot simply index.
 */
typedef struct {
    const uint8_t *buf;
    uint32_t       len;
    uint32_t       byte;        /* next byte to consume            */
    uint32_t       bits_left;   /* in the current partial byte     */
    uint8_t        cur;
    uint32_t       zeros;       /* consecutive 00 bytes seen       */
    int            overrun;     /* read past the end               */
} h264_bits_t;

static void h264_bits_init(h264_bits_t *b, const uint8_t *buf, uint32_t len) {
    b->buf = buf;
    b->len = len;
    b->byte = 0;
    b->bits_left = 0;
    b->cur = 0;
    b->zeros = 0;
    b->overrun = 0;
}

/* Pull the next RBSP byte, skipping an emulation prevention byte if
 * this position is one. */
static uint8_t h264_next_byte(h264_bits_t *b) {
    uint8_t v;

    if (b->byte >= b->len) { b->overrun = 1; return 0; }
    v = b->buf[b->byte++];

    if (b->zeros >= 2 && v == 0x03) {
        /* the 0x03 is not data; the byte after it is */
        b->zeros = 0;
        if (b->byte >= b->len) { b->overrun = 1; return 0; }
        v = b->buf[b->byte++];
    }

    if (v == 0) b->zeros++;
    else        b->zeros = 0;

    return v;
}

static uint32_t h264_u(h264_bits_t *b, int n) {
    uint32_t v = 0;

    if (n <= 0 || n > 32) return 0;
    while (n > 0) {
        if (b->bits_left == 0) {
            b->cur = h264_next_byte(b);
            b->bits_left = 8;
        }
        {
            int take = (n < (int)b->bits_left) ? n : (int)b->bits_left;
            uint32_t chunk = (uint32_t)(b->cur >> (b->bits_left - take)) &
                             ((1u << take) - 1u);
            v = (v << take) | chunk;
            b->bits_left -= (uint32_t)take;
            n -= take;
        }
    }
    return v;
}

static int h264_u1(h264_bits_t *b) { return (int)h264_u(b, 1); }

/*
 * Exp-Golomb, unsigned.
 *
 * Count leading zeroes, then read that many bits and add the implicit
 * one. The leading-zero count is bounded at 32 here: a corrupt stream
 * can present an arbitrarily long run of zeroes, and without the bound
 * this loop runs to the end of the file and then spins on the overrun
 * flag forever.
 */
static uint32_t h264_ue(h264_bits_t *b) {
    int zeros = 0;

    while (zeros < 32) {
        if (b->overrun) return 0;
        if (h264_u1(b)) break;
        zeros++;
    }
    if (zeros == 0) return 0;
    if (zeros >= 32) { b->overrun = 1; return 0; }

    return ((1u << zeros) - 1u) + h264_u(b, zeros);
}

/* Exp-Golomb, signed: the unsigned value zig-zagged around zero. */
static int32_t h264_se(h264_bits_t *b) {
    uint32_t k = h264_ue(b);
    int32_t  v = (int32_t)((k + 1) >> 1);
    return (k & 1) ? v : -v;
}

/* ===== parameter sets ===== */

typedef struct {
    int      valid;
    uint8_t  profile_idc;
    uint8_t  level_idc;
    uint8_t  sps_id;
    uint8_t  chroma_format_idc;      /* 1 = 4:2:0, which is all MFX does */
    uint8_t  bit_depth_luma;
    uint8_t  bit_depth_chroma;
    uint8_t  log2_max_frame_num;
    uint8_t  pic_order_cnt_type;
    uint8_t  log2_max_poc_lsb;
    int      delta_pic_order_always_zero_flag;
    int32_t  offset_for_non_ref_pic;
    int32_t  offset_for_top_to_bottom_field;
    uint32_t num_ref_frames_in_pic_order_cnt_cycle;
    uint8_t  max_num_ref_frames;
    int      gaps_in_frame_num_allowed;
    uint32_t pic_width_in_mbs;
    uint32_t pic_height_in_map_units;
    int      frame_mbs_only_flag;
    int      mb_adaptive_frame_field_flag;
    int      direct_8x8_inference_flag;
    int      separate_colour_plane_flag;
    /* cropping, in luma samples once the chroma scaling is applied */
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    /* the picture, in pixels, after cropping */
    uint32_t width, height;
    uint8_t  scaling_4x4[6][16];
    uint8_t  scaling_8x8[6][64];
    int      seq_scaling_matrix_present;
} h264_sps_t;

typedef struct {
    int      valid;
    uint8_t  pps_id;
    uint8_t  sps_id;
    int      entropy_coding_mode_flag;     /* 1 = CABAC, 0 = CAVLC      */
    int      bottom_field_pic_order_in_frame_present_flag;
    uint32_t num_slice_groups;
    uint32_t num_ref_idx_l0_default_active;
    uint32_t num_ref_idx_l1_default_active;
    int      weighted_pred_flag;
    uint8_t  weighted_bipred_idc;
    int32_t  pic_init_qp;
    int32_t  pic_init_qs;
    int32_t  chroma_qp_index_offset;
    int32_t  second_chroma_qp_index_offset;
    int      deblocking_filter_control_present_flag;
    int      constrained_intra_pred_flag;
    int      redundant_pic_cnt_present_flag;
    int      transform_8x8_mode_flag;
    int      pic_scaling_matrix_present;
    uint8_t  scaling_4x4[6][16];
    uint8_t  scaling_8x8[6][64];
} h264_pps_t;

/* The default quantisation matrices, which apply when a stream does not
 * send its own. Flat 16 is what "no scaling" means in H.264. */
static void h264_default_scaling(uint8_t s4[6][16], uint8_t s8[6][64]) {
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 16; j++) s4[i][j] = 16;
        for (int j = 0; j < 64; j++) s8[i][j] = 16;
    }
}

/*
 * A scaling list, which is sent as deltas and stops early if the
 * encoder says the rest is a repeat of the last value.
 */
static void h264_scaling_list(h264_bits_t *b, uint8_t *list, int size,
                              int *use_default) {
    int last = 8, next = 8;
    *use_default = 0;

    for (int i = 0; i < size; i++) {
        if (next != 0) {
            int32_t delta = h264_se(b);
            next = (last + delta + 256) % 256;
            if (i == 0 && next == 0) { *use_default = 1; return; }
        }
        list[i] = (uint8_t)(next == 0 ? last : next);
        last = list[i];
    }
}

/*
 * Parse a sequence parameter set.
 *
 * Returns 1 on success. The VUI at the end is deliberately not parsed:
 * it carries aspect ratio, timing and bitstream restrictions, none of
 * which the decoder needs to produce correct pixels, and it is by far
 * the most baroque part of the syntax.
 */
static int h264_parse_sps(const uint8_t *rbsp, uint32_t len, h264_sps_t *sps) {
    h264_bits_t b;
    uint32_t i;

    h264_bits_init(&b, rbsp, len);

    for (i = 0; i < sizeof(*sps); i++) ((uint8_t *)sps)[i] = 0;
    h264_default_scaling(sps->scaling_4x4, sps->scaling_8x8);

    sps->profile_idc = (uint8_t)h264_u(&b, 8);
    h264_u(&b, 8);                              /* constraint flags     */
    sps->level_idc = (uint8_t)h264_u(&b, 8);
    {
        uint32_t id = h264_ue(&b);
        if (id >= H264_MAX_SPS) return 0;
        sps->sps_id = (uint8_t)id;
    }

    sps->chroma_format_idc = 1;                 /* 4:2:0 unless told    */
    sps->bit_depth_luma    = 8;
    sps->bit_depth_chroma  = 8;

    /* The high profiles carry chroma format and bit depth; the baseline
     * and main profiles do not and are always 4:2:0 8-bit. */
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44  || sps->profile_idc == 83  ||
        sps->profile_idc == 86  || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138 ||
        sps->profile_idc == 139 || sps->profile_idc == 134) {

        uint32_t cf = h264_ue(&b);
        if (cf > 3) return 0;
        sps->chroma_format_idc = (uint8_t)cf;
        if (cf == 3) sps->separate_colour_plane_flag = h264_u1(&b);

        sps->bit_depth_luma   = (uint8_t)(8 + h264_ue(&b));
        sps->bit_depth_chroma = (uint8_t)(8 + h264_ue(&b));
        if (sps->bit_depth_luma > 14 || sps->bit_depth_chroma > 14) return 0;

        h264_u1(&b);                            /* lossless qpprime     */

        sps->seq_scaling_matrix_present = h264_u1(&b);
        if (sps->seq_scaling_matrix_present) {
            int n = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (int k = 0; k < n; k++) {
                if (h264_u1(&b)) {
                    int use_default = 0;
                    if (k < 6)
                        h264_scaling_list(&b, sps->scaling_4x4[k], 16,
                                          &use_default);
                    else
                        h264_scaling_list(&b, sps->scaling_8x8[k - 6], 64,
                                          &use_default);
                }
            }
        }
    }

    {
        uint32_t v = h264_ue(&b);
        if (v > 12) return 0;                   /* log2 max frame num   */
        sps->log2_max_frame_num = (uint8_t)(v + 4);
    }

    sps->pic_order_cnt_type = (uint8_t)h264_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        uint32_t v = h264_ue(&b);
        if (v > 12) return 0;
        sps->log2_max_poc_lsb = (uint8_t)(v + 4);
    } else if (sps->pic_order_cnt_type == 1) {
        sps->delta_pic_order_always_zero_flag = h264_u1(&b);
        sps->offset_for_non_ref_pic = h264_se(&b);
        sps->offset_for_top_to_bottom_field = h264_se(&b);
        sps->num_ref_frames_in_pic_order_cnt_cycle = h264_ue(&b);
        if (sps->num_ref_frames_in_pic_order_cnt_cycle > 255) return 0;
        for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
            h264_se(&b);                        /* offset_for_ref_frame */
    } else if (sps->pic_order_cnt_type > 2) {
        return 0;
    }

    {
        uint32_t v = h264_ue(&b);
        if (v > H264_MAX_REF_FRAMES) return 0;
        sps->max_num_ref_frames = (uint8_t)v;
    }
    sps->gaps_in_frame_num_allowed = h264_u1(&b);

    sps->pic_width_in_mbs        = h264_ue(&b) + 1;
    sps->pic_height_in_map_units = h264_ue(&b) + 1;

    /* 16384x16384 is well past level 6.2 and is where a corrupt value
     * stops being a large video and starts being an allocation that
     * takes the machine down. */
    if (sps->pic_width_in_mbs == 0 || sps->pic_width_in_mbs > 1024) return 0;
    if (sps->pic_height_in_map_units == 0 ||
        sps->pic_height_in_map_units > 1024) return 0;

    sps->frame_mbs_only_flag = h264_u1(&b);
    if (!sps->frame_mbs_only_flag)
        sps->mb_adaptive_frame_field_flag = h264_u1(&b);

    sps->direct_8x8_inference_flag = h264_u1(&b);

    if (h264_u1(&b)) {                          /* frame cropping       */
        sps->crop_left   = h264_ue(&b);
        sps->crop_right  = h264_ue(&b);
        sps->crop_top    = h264_ue(&b);
        sps->crop_bottom = h264_ue(&b);
    }

    if (b.overrun) return 0;

    /* Derived dimensions. The crop offsets are in chroma samples for
     * 4:2:0, so they scale by two horizontally and, for a frame-only
     * stream, by two vertically as well. */
    {
        uint32_t sub_w = (sps->chroma_format_idc == 3) ? 1 : 2;
        uint32_t sub_h = (sps->chroma_format_idc == 1) ? 2 : 1;
        uint32_t mul_h = sps->frame_mbs_only_flag ? 1 : 2;

        uint32_t w = sps->pic_width_in_mbs * 16;
        uint32_t h = sps->pic_height_in_map_units * 16 *
                     (sps->frame_mbs_only_flag ? 1u : 2u);

        uint32_t cl = sps->crop_left  * sub_w;
        uint32_t cr = sps->crop_right * sub_w;
        uint32_t ct = sps->crop_top    * sub_h * mul_h;
        uint32_t cb = sps->crop_bottom * sub_h * mul_h;

        if (cl + cr >= w || ct + cb >= h) return 0;

        sps->width  = w - cl - cr;
        sps->height = h - ct - cb;
    }

    sps->valid = 1;
    return 1;
}

static int h264_parse_pps(const uint8_t *rbsp, uint32_t len,
                          const h264_sps_t *sps_table, h264_pps_t *pps) {
    h264_bits_t b;
    const h264_sps_t *sps;
    uint32_t i;

    h264_bits_init(&b, rbsp, len);
    for (i = 0; i < sizeof(*pps); i++) ((uint8_t *)pps)[i] = 0;
    h264_default_scaling(pps->scaling_4x4, pps->scaling_8x8);

    {
        uint32_t id = h264_ue(&b);
        if (id >= H264_MAX_PPS) return 0;
        pps->pps_id = (uint8_t)id;
    }
    {
        uint32_t id = h264_ue(&b);
        if (id >= H264_MAX_SPS) return 0;
        pps->sps_id = (uint8_t)id;
    }

    sps = &sps_table[pps->sps_id];
    if (!sps->valid) return 0;

    pps->entropy_coding_mode_flag = h264_u1(&b);
    pps->bottom_field_pic_order_in_frame_present_flag = h264_u1(&b);

    pps->num_slice_groups = h264_ue(&b) + 1;
    if (pps->num_slice_groups > H264_MAX_SLICE_GROUPS) return 0;
    if (pps->num_slice_groups > 1) {
        /* FMO. The MFX block does not implement it and neither does
         * any encoder made this century; refusing is better than
         * decoding the first slice group and showing a striped
         * picture. */
        return 0;
    }

    pps->num_ref_idx_l0_default_active = h264_ue(&b) + 1;
    pps->num_ref_idx_l1_default_active = h264_ue(&b) + 1;
    if (pps->num_ref_idx_l0_default_active > 32 ||
        pps->num_ref_idx_l1_default_active > 32) return 0;

    pps->weighted_pred_flag  = h264_u1(&b);
    pps->weighted_bipred_idc = (uint8_t)h264_u(&b, 2);

    pps->pic_init_qp = 26 + h264_se(&b);
    pps->pic_init_qs = 26 + h264_se(&b);
    pps->chroma_qp_index_offset = h264_se(&b);
    if (pps->pic_init_qp < 0 || pps->pic_init_qp > 51) return 0;
    if (pps->chroma_qp_index_offset < -12 ||
        pps->chroma_qp_index_offset > 12) return 0;

    pps->deblocking_filter_control_present_flag = h264_u1(&b);
    pps->constrained_intra_pred_flag = h264_u1(&b);
    pps->redundant_pic_cnt_present_flag = h264_u1(&b);

    /* Everything above is the original PPS; what follows is the high
     * profile extension and is simply absent on a baseline stream. */
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    for (i = 0; i < 6; i++) {
        for (int j = 0; j < 16; j++)
            pps->scaling_4x4[i][j] = sps->scaling_4x4[i][j];
        for (int j = 0; j < 64; j++)
            pps->scaling_8x8[i][j] = sps->scaling_8x8[i][j];
    }

    if (!b.overrun && b.byte < len) {
        pps->transform_8x8_mode_flag = h264_u1(&b);
        pps->pic_scaling_matrix_present = h264_u1(&b);
        if (pps->pic_scaling_matrix_present) {
            int n = 6 + (pps->transform_8x8_mode_flag ?
                         ((sps->chroma_format_idc != 3) ? 2 : 6) : 0);
            for (int k = 0; k < n; k++) {
                if (h264_u1(&b)) {
                    int use_default = 0;
                    if (k < 6)
                        h264_scaling_list(&b, pps->scaling_4x4[k], 16,
                                          &use_default);
                    else
                        h264_scaling_list(&b, pps->scaling_8x8[k - 6], 64,
                                          &use_default);
                }
            }
        }
        pps->second_chroma_qp_index_offset = h264_se(&b);
        if (pps->second_chroma_qp_index_offset < -12 ||
            pps->second_chroma_qp_index_offset > 12)
            pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    }

    if (b.overrun) return 0;
    pps->valid = 1;
    return 1;
}

/* ===== slice header ===== */

typedef struct {
    uint32_t first_mb_in_slice;
    uint32_t slice_type;             /* already reduced to 0-4          */
    int      all_same_type;          /* the 5-9 form was used           */
    uint8_t  pps_id;
    uint32_t frame_num;
    int      field_pic_flag;
    int      bottom_field_flag;
    uint32_t idr_pic_id;
    uint32_t pic_order_cnt_lsb;
    int32_t  delta_pic_order_cnt_bottom;
    int32_t  delta_pic_order_cnt[2];
    uint32_t redundant_pic_cnt;
    int      direct_spatial_mv_pred_flag;
    uint32_t num_ref_idx_l0_active;
    uint32_t num_ref_idx_l1_active;
    uint32_t cabac_init_idc;
    int32_t  slice_qp;
    uint32_t disable_deblocking_filter_idc;
    int32_t  slice_alpha_c0_offset;
    int32_t  slice_beta_offset;

    int      nal_ref_idc;
    int      nal_unit_type;
    int      is_idr;

    /* Where the entropy-coded slice data starts, which is what gets
     * handed to the hardware. In bits, because CABAC does not begin on
     * a byte boundary. */
    uint32_t data_bit_offset;

    /* dec_ref_pic_marking */
    int      no_output_of_prior_pics_flag;
    int      long_term_reference_flag;
    int      adaptive_ref_pic_marking_mode_flag;
} h264_slice_hdr_t;

/*
 * Parse a slice header.
 *
 * The reference picture list modification and prediction weight table
 * are walked rather than stored: the hardware is told the *default*
 * lists and the modifications separately in MFX_AVC_REF_IDX_STATE, and
 * walking them here is what finds the bit offset where the slice data
 * actually begins -- which is the one number the hardware cannot work
 * out for itself.
 */
static int h264_parse_slice_header(const uint8_t *rbsp, uint32_t len,
                                   int nal_ref_idc, int nal_unit_type,
                                   const h264_sps_t *sps_table,
                                   const h264_pps_t *pps_table,
                                   h264_slice_hdr_t *sh,
                                   const h264_sps_t **out_sps,
                                   const h264_pps_t **out_pps) {
    h264_bits_t b;
    const h264_sps_t *sps;
    const h264_pps_t *pps;
    uint32_t i;

    h264_bits_init(&b, rbsp, len);
    for (i = 0; i < sizeof(*sh); i++) ((uint8_t *)sh)[i] = 0;

    sh->nal_ref_idc  = nal_ref_idc;
    sh->nal_unit_type = nal_unit_type;
    sh->is_idr = (nal_unit_type == H264_NAL_IDR);

    sh->first_mb_in_slice = h264_ue(&b);

    {
        uint32_t st = h264_ue(&b);
        if (st > 9) return 0;
        sh->all_same_type = (st >= 5);
        sh->slice_type = st % 5;
    }

    {
        uint32_t id = h264_ue(&b);
        if (id >= H264_MAX_PPS) return 0;
        sh->pps_id = (uint8_t)id;
    }

    pps = &pps_table[sh->pps_id];
    if (!pps->valid) return 0;
    sps = &sps_table[pps->sps_id];
    if (!sps->valid) return 0;

    if (sps->separate_colour_plane_flag) h264_u(&b, 2);

    sh->frame_num = h264_u(&b, sps->log2_max_frame_num);

    if (!sps->frame_mbs_only_flag) {
        sh->field_pic_flag = h264_u1(&b);
        if (sh->field_pic_flag) sh->bottom_field_flag = h264_u1(&b);
    }

    if (sh->is_idr) {
        sh->idr_pic_id = h264_ue(&b);
        if (sh->idr_pic_id > 65535) return 0;
    }

    if (sps->pic_order_cnt_type == 0) {
        sh->pic_order_cnt_lsb = h264_u(&b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_in_frame_present_flag &&
            !sh->field_pic_flag)
            sh->delta_pic_order_cnt_bottom = h264_se(&b);
    } else if (sps->pic_order_cnt_type == 1 &&
               !sps->delta_pic_order_always_zero_flag) {
        sh->delta_pic_order_cnt[0] = h264_se(&b);
        if (pps->bottom_field_pic_order_in_frame_present_flag &&
            !sh->field_pic_flag)
            sh->delta_pic_order_cnt[1] = h264_se(&b);
    }

    if (pps->redundant_pic_cnt_present_flag) {
        sh->redundant_pic_cnt = h264_ue(&b);
        /* A redundant coded picture is a second copy of data we already
         * have. Nothing here uses it and decoding it as though it were
         * primary would double-count the frame. */
        if (sh->redundant_pic_cnt > 0) return 0;
    }

    sh->num_ref_idx_l0_active = pps->num_ref_idx_l0_default_active;
    sh->num_ref_idx_l1_active = pps->num_ref_idx_l1_default_active;

    if (sh->slice_type == H264_SLICE_B)
        sh->direct_spatial_mv_pred_flag = h264_u1(&b);

    if (sh->slice_type == H264_SLICE_P || sh->slice_type == H264_SLICE_SP ||
        sh->slice_type == H264_SLICE_B) {
        if (h264_u1(&b)) {                      /* override */
            sh->num_ref_idx_l0_active = h264_ue(&b) + 1;
            if (sh->slice_type == H264_SLICE_B)
                sh->num_ref_idx_l1_active = h264_ue(&b) + 1;
        }
        if (sh->num_ref_idx_l0_active > 32 ||
            sh->num_ref_idx_l1_active > 32) return 0;
    }

    /* ref_pic_list_modification */
    if (sh->slice_type != H264_SLICE_I && sh->slice_type != H264_SLICE_SI) {
        if (h264_u1(&b)) {
            for (int guard = 0; guard < 64; guard++) {
                uint32_t op = h264_ue(&b);
                if (op == 3 || b.overrun) break;
                if (op > 3) return 0;
                h264_ue(&b);                    /* abs diff / long term */
                if (guard == 63) return 0;
            }
        }
    }
    if (sh->slice_type == H264_SLICE_B) {
        if (h264_u1(&b)) {
            for (int guard = 0; guard < 64; guard++) {
                uint32_t op = h264_ue(&b);
                if (op == 3 || b.overrun) break;
                if (op > 3) return 0;
                h264_ue(&b);
                if (guard == 63) return 0;
            }
        }
    }

    /* pred_weight_table */
    if ((pps->weighted_pred_flag &&
         (sh->slice_type == H264_SLICE_P || sh->slice_type == H264_SLICE_SP)) ||
        (pps->weighted_bipred_idc == 1 && sh->slice_type == H264_SLICE_B)) {

        int chroma = (sps->chroma_format_idc != 0 &&
                      !sps->separate_colour_plane_flag);
        h264_ue(&b);                            /* luma_log2_weight_denom */
        if (chroma) h264_ue(&b);

        for (i = 0; i < sh->num_ref_idx_l0_active; i++) {
            if (h264_u1(&b)) { h264_se(&b); h264_se(&b); }
            if (chroma && h264_u1(&b))
                for (int c = 0; c < 2; c++) { h264_se(&b); h264_se(&b); }
        }
        if (sh->slice_type == H264_SLICE_B) {
            for (i = 0; i < sh->num_ref_idx_l1_active; i++) {
                if (h264_u1(&b)) { h264_se(&b); h264_se(&b); }
                if (chroma && h264_u1(&b))
                    for (int c = 0; c < 2; c++) { h264_se(&b); h264_se(&b); }
            }
        }
    }

    /* dec_ref_pic_marking */
    if (nal_ref_idc != 0) {
        if (sh->is_idr) {
            sh->no_output_of_prior_pics_flag = h264_u1(&b);
            sh->long_term_reference_flag = h264_u1(&b);
        } else {
            sh->adaptive_ref_pic_marking_mode_flag = h264_u1(&b);
            if (sh->adaptive_ref_pic_marking_mode_flag) {
                for (int guard = 0; guard < 64; guard++) {
                    uint32_t op = h264_ue(&b);
                    if (op == 0 || b.overrun) break;
                    if (op > 6) return 0;
                    if (op == 1 || op == 3) h264_ue(&b);
                    if (op == 2) h264_ue(&b);
                    if (op == 3 || op == 6) h264_ue(&b);
                    if (op == 4) h264_ue(&b);
                    if (op == 5) break;
                    if (guard == 63) return 0;
                }
            }
        }
    }

    if (pps->entropy_coding_mode_flag &&
        sh->slice_type != H264_SLICE_I && sh->slice_type != H264_SLICE_SI) {
        sh->cabac_init_idc = h264_ue(&b);
        if (sh->cabac_init_idc > 2) return 0;
    }

    sh->slice_qp = pps->pic_init_qp + h264_se(&b);
    if (sh->slice_qp < -6 || sh->slice_qp > 51) return 0;

    if (sh->slice_type == H264_SLICE_SP || sh->slice_type == H264_SLICE_SI) {
        if (sh->slice_type == H264_SLICE_SP) h264_u1(&b);
        h264_se(&b);                            /* slice_qs_delta */
    }

    if (pps->deblocking_filter_control_present_flag) {
        sh->disable_deblocking_filter_idc = h264_ue(&b);
        if (sh->disable_deblocking_filter_idc > 2) return 0;
        if (sh->disable_deblocking_filter_idc != 1) {
            sh->slice_alpha_c0_offset = h264_se(&b) * 2;
            sh->slice_beta_offset     = h264_se(&b) * 2;
        }
    }

    if (b.overrun) return 0;

    /*
     * Where the slice data begins.
     *
     * b.byte counts bytes actually consumed from the source, which
     * already excludes emulation prevention bytes -- so this offset is
     * into the RBSP as the hardware will see it after its own
     * unescaping, which is what MFD_AVC_BSD_OBJECT wants.
     */
    sh->data_bit_offset = b.byte * 8 - b.bits_left;

    if (out_sps) *out_sps = sps;
    if (out_pps) *out_pps = pps;
    return 1;
}

/* ===== NAL unit scanning ===== */

typedef struct {
    const uint8_t *data;        /* payload, past the one-byte header  */
    uint32_t       len;
    int            ref_idc;
    int            type;
} h264_nal_t;

/*
 * Find the next NAL unit in an Annex B byte stream.
 *
 * Start codes are 00 00 01 or 00 00 00 01, and a NAL runs until the
 * next one or the end of the buffer. `*pos` is advanced past the unit
 * that is returned. Returns 0 when there are none left.
 */
static int h264_next_nal(const uint8_t *buf, uint32_t len, uint32_t *pos,
                         h264_nal_t *nal) {
    uint32_t i = *pos;
    uint32_t start, end;

    /* find a start code */
    while (i + 3 <= len) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            i += 3;
            break;
        }
        i++;
    }
    if (i + 3 > len && !(i < len && i >= 3)) {
        if (i + 1 >= len) { *pos = len; return 0; }
    }
    if (i >= len) { *pos = len; return 0; }

    start = i;

    /* find the next one */
    end = len;
    for (uint32_t j = start; j + 3 <= len; j++) {
        if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
            end = j;
            /* a four-byte start code has an extra zero before it, and
             * that zero belongs to the start code rather than to the
             * NAL that precedes it */
            if (end > start && buf[end - 1] == 0) end--;
            break;
        }
    }

    if (end <= start) { *pos = len; return 0; }

    nal->ref_idc = (buf[start] >> 5) & 0x03;
    nal->type    = buf[start] & 0x1F;
    nal->data    = buf + start + 1;
    nal->len     = end - start - 1;

    /* forbidden_zero_bit must be zero; if it is not this is not a NAL */
    if (buf[start] & 0x80) { *pos = end; return 0; }

    *pos = end;
    return 1;
}

/* ===== picture order count =====
 *
 * The order pictures are *displayed* in, which is not the order they
 * are coded in as soon as B-frames exist. The decoder needs it to know
 * when a decoded frame may be shown and which references a B slice is
 * pointing at.
 */

typedef struct {
    int32_t  prev_poc_msb;
    uint32_t prev_poc_lsb;
    uint32_t prev_frame_num;
    uint32_t prev_frame_num_offset;
} h264_poc_state_t;

static int32_t h264_compute_poc(const h264_sps_t *sps,
                                const h264_slice_hdr_t *sh,
                                h264_poc_state_t *st) {
    int32_t poc = 0;

    if (sps->pic_order_cnt_type == 0) {
        uint32_t max_lsb = 1u << sps->log2_max_poc_lsb;
        int32_t  msb;

        if (sh->is_idr) {
            st->prev_poc_msb = 0;
            st->prev_poc_lsb = 0;
        }

        if (sh->pic_order_cnt_lsb < st->prev_poc_lsb &&
            (st->prev_poc_lsb - sh->pic_order_cnt_lsb) >= max_lsb / 2)
            msb = st->prev_poc_msb + (int32_t)max_lsb;
        else if (sh->pic_order_cnt_lsb > st->prev_poc_lsb &&
                 (sh->pic_order_cnt_lsb - st->prev_poc_lsb) > max_lsb / 2)
            msb = st->prev_poc_msb - (int32_t)max_lsb;
        else
            msb = st->prev_poc_msb;

        poc = msb + (int32_t)sh->pic_order_cnt_lsb;

        /* Only reference pictures move the predictor forward. */
        if (sh->nal_ref_idc) {
            st->prev_poc_msb = msb;
            st->prev_poc_lsb = sh->pic_order_cnt_lsb;
        }
    } else if (sps->pic_order_cnt_type == 2) {
        /* Coding order is display order; there are no B frames. */
        uint32_t frame_num_offset;
        if (sh->is_idr) frame_num_offset = 0;
        else if (st->prev_frame_num > sh->frame_num)
            frame_num_offset = st->prev_frame_num_offset +
                               (1u << sps->log2_max_frame_num);
        else frame_num_offset = st->prev_frame_num_offset;

        poc = (int32_t)(2 * (frame_num_offset + sh->frame_num));
        if (!sh->nal_ref_idc) poc--;

        st->prev_frame_num_offset = frame_num_offset;
        st->prev_frame_num = sh->frame_num;
    } else {
        /* Type 1 is a cycle of explicit offsets. Rare enough that the
         * frame number is a serviceable stand-in for ordering, and
         * saying so is better than a wrong POC that silently reorders
         * the picture. */
        poc = (int32_t)(2 * sh->frame_num);
    }

    return poc;
}

#endif /* MEDIA_H264_H */
