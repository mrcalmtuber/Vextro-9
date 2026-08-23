#ifndef MEDIA_MFX_H
#define MEDIA_MFX_H

/*
 * src/media/mfx.h — the command language of the Gen9 video engine.
 *
 * The MFX block is a fixed-function decoder sitting behind the VCS
 * command streamer. It is programmed the way the rest of the GPU is:
 * a stream of packets, each a header dword naming a command and a
 * length, followed by that command's payload. A decode is a sequence of
 * state packets that describe the picture, followed by one object
 * packet per slice that points at the entropy-coded bits and says "go".
 *
 * The state is layered, and the layering is the whole design:
 *
 *   MFX_PIPE_MODE_SELECT      which codec, decode or encode, VLD mode
 *   MFX_SURFACE_STATE         the shape of a picture in memory
 *   MFX_PIPE_BUF_ADDR_STATE   where the destination and every
 *                             reference picture live
 *   MFX_IND_OBJ_BASE_ADDR     where the compressed bits live
 *   MFX_BSP_BUF_BASE_ADDR     the engine's own scratch rows
 *   MFX_AVC_IMG_STATE         everything from the SPS and PPS
 *   MFX_AVC_QM_STATE          quantisation matrices
 *   MFX_AVC_SLICE_STATE       everything from the slice header
 *   MFD_AVC_BSD_OBJECT        the slice data itself
 *
 * ---- where these encodings come from, and what that means ----
 *
 * The command opcodes, field positions and the ordering requirements
 * below are taken from Intel's public Programmer's Reference Manuals
 * for Gen9 and from the open-source libva i965 driver, which is the
 * reference implementation of exactly this. They are not guesses.
 *
 * They are also not *verified*, and that distinction matters. Nothing
 * in this file has executed on a video engine, because no machine this
 * system is developed on has one -- QEMU models no Intel GPU at all, so
 * igpu_init() finds nothing and the whole path below is unreachable
 * here. A single field in the wrong bit position produces a hang that
 * the engine reports as a generic parser error, and that is the class
 * of bug that only silicon can find. What is tested (see
 * tools/media_test.c) is everything above and below this layer: the
 * bitstream parsing that feeds it and the surface geometry and colour
 * conversion that consume its output.
 */

#include <stdint.h>
#include "h264.h"

/*
 * A media command header.
 *
 *   [31:29] type = 3
 *   [28:27] pipeline = 2 for MFX, 2 for VEBOX (the opcode separates them)
 *   [26:24] media command opcode -- 0 common, 1 AVC, 4 VEBOX
 *   [23:21] sub-opcode A
 *   [20:16] sub-opcode B
 *   [15:0]  length in dwords, minus two
 */
#define MFX_CMD(pipeline, op, sub_a, sub_b)                                \
    ((3u << 29) | ((uint32_t)(pipeline) << 27) | ((uint32_t)(op) << 24) |  \
     ((uint32_t)(sub_a) << 21) | ((uint32_t)(sub_b) << 16))

#define MFX_PIPE_MODE_SELECT        MFX_CMD(2, 0, 0, 0)
#define MFX_SURFACE_STATE           MFX_CMD(2, 0, 0, 1)
#define MFX_PIPE_BUF_ADDR_STATE     MFX_CMD(2, 0, 0, 2)
#define MFX_IND_OBJ_BASE_ADDR_STATE MFX_CMD(2, 0, 0, 3)
#define MFX_BSP_BUF_BASE_ADDR_STATE MFX_CMD(2, 0, 0, 4)
#define MFX_QM_STATE                MFX_CMD(2, 0, 0, 7)
#define MFX_FQM_STATE               MFX_CMD(2, 0, 0, 8)
#define MFX_WAIT                    MFX_CMD(1, 0, 0, 0)

#define MFX_AVC_IMG_STATE           MFX_CMD(2, 1, 0, 0)
#define MFX_AVC_QM_STATE            MFX_CMD(2, 1, 0, 1)
#define MFX_AVC_DIRECTMODE_STATE    MFX_CMD(2, 1, 0, 2)
#define MFX_AVC_SLICE_STATE         MFX_CMD(2, 1, 0, 3)
#define MFX_AVC_REF_IDX_STATE       MFX_CMD(2, 1, 0, 4)
#define MFX_AVC_WEIGHTOFFSET_STATE  MFX_CMD(2, 1, 0, 5)

#define MFD_AVC_PICID_STATE         MFX_CMD(2, 1, 1, 5)
#define MFD_AVC_DPB_STATE           MFX_CMD(2, 1, 1, 6)
#define MFD_AVC_SLICEADDR           MFX_CMD(2, 1, 1, 7)
#define MFD_AVC_BSD_OBJECT          MFX_CMD(2, 1, 1, 8)

/* VEBOX: the "Clear Video" block. Colour conversion, denoise, deinterlace. */
#define VEB_STATE                   MFX_CMD(2, 4, 0, 0)
#define VEB_SURFACE_STATE           MFX_CMD(2, 4, 0, 1)
#define VEB_DI_IECP                 MFX_CMD(2, 4, 0, 2)

/* codec select in MFX_PIPE_MODE_SELECT */
#define MFX_CODEC_AVC               2
#define MFX_DECODE                  0
#define MFX_ENCODE                  1
#define MFX_VLD_MODE                0   /* full hardware bitstream decode */

/* surface formats */
#define MFX_SURFACE_NV12            4   /* the only decode output format */

/* ===== a small command writer ===== */

/*
 * Commands are built into a caller-owned array rather than emitted
 * straight into the ring, because a decode has to be assembled and
 * bounds-checked before any of it is visible to the engine: a stream
 * that is half written when it runs out of room is a hang, not a
 * dropped frame.
 */
typedef struct {
    uint32_t *dw;
    uint32_t  max;
    uint32_t  n;
    int       overflow;
} mfx_cs_t;

static void mfx_cs_init(mfx_cs_t *cs, uint32_t *buf, uint32_t max) {
    cs->dw = buf;
    cs->max = max;
    cs->n = 0;
    cs->overflow = 0;
}

static void mfx_out(mfx_cs_t *cs, uint32_t v) {
    if (cs->n >= cs->max) { cs->overflow = 1; return; }
    cs->dw[cs->n++] = v;
}

/* A graphics address in a command is a dword whose low twelve bits are
 * flags; the address itself is page aligned. Only the low 32 bits are
 * used here because the media GGTT window is inside the first 4 GB of
 * GPU address space by construction. */
static void mfx_out_addr(mfx_cs_t *cs, uint32_t gpu_addr) {
    mfx_out(cs, gpu_addr);
    mfx_out(cs, 0);                 /* address upper dword + attributes */
}

/* ===== the state packets ===== */

static void mfx_pipe_mode_select(mfx_cs_t *cs, int codec, int decode,
                                 int short_format) {
    mfx_out(cs, MFX_PIPE_MODE_SELECT | (5 - 2));
    mfx_out(cs,
            ((uint32_t)(decode ? MFX_DECODE : MFX_ENCODE) << 0) |
            ((uint32_t)codec << 1) |
            /* long format: the driver has parsed the slice headers and
             * supplies them, rather than asking the engine to do it.
             * Short format needs firmware; long format does not, which
             * is the whole reason this driver can work without one. */
            ((uint32_t)(short_format ? 1 : 0) << 4) |
            (1u << 5) |             /* stream-out disabled              */
            (1u << 7));             /* deblocker enabled in the pipe    */
    mfx_out(cs, 0);                 /* pre-deblocking / post filters    */
    mfx_out(cs, 0);                 /* picture status / error handling  */
    mfx_out(cs, 0);
}

/*
 * The shape of a picture in memory.
 *
 * NV12: a full-resolution plane of luma followed by a half-resolution
 * plane of interleaved Cb/Cr. The chroma plane's offset is given in
 * lines rather than bytes, which is why it is the surface height and
 * not the surface size.
 */
static void mfx_surface_state(mfx_cs_t *cs, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t chroma_line_offset) {
    mfx_out(cs, MFX_SURFACE_STATE | (6 - 2));
    mfx_out(cs, 0);                 /* surface id: 0 = decode target    */
    mfx_out(cs, ((height - 1) << 18) | ((width - 1) << 4));
    mfx_out(cs,
            ((uint32_t)MFX_SURFACE_NV12 << 28) |
            (0u << 27) |            /* interleave chroma                */
            ((pitch - 1) << 3) |
            (0u << 2) |             /* tile walk                        */
            (0u << 0));             /* linear; tiling needs a fence     */
    mfx_out(cs, (chroma_line_offset << 16) | 0);
    mfx_out(cs, (chroma_line_offset << 16) | 0);
}

/*
 * Where everything the decoder reads and writes lives.
 *
 * The reference picture slots are positional: slot n here is what
 * reference index n in MFX_AVC_REF_IDX_STATE names. An unused slot must
 * still be written -- with the destination address, not zero, because a
 * null address that a corrupt reference index happens to select is a
 * page fault on the engine rather than a wrong picture.
 */
static void mfx_pipe_buf_addr_state(mfx_cs_t *cs,
                                    uint32_t pre_deblock, uint32_t post_deblock,
                                    uint32_t intra_rowstore,
                                    uint32_t deblock_rowstore,
                                    const uint32_t *refs, int nrefs,
                                    uint32_t mv_write, const uint32_t *mv_read,
                                    int n_mv_read) {
    int i;

    mfx_out(cs, MFX_PIPE_BUF_ADDR_STATE | (61 - 2));

    mfx_out_addr(cs, pre_deblock);
    mfx_out_addr(cs, post_deblock);
    mfx_out_addr(cs, 0);            /* pre-deblock stream-out           */
    mfx_out_addr(cs, 0);            /* post-deblock stream-out          */
    mfx_out_addr(cs, intra_rowstore);
    mfx_out_addr(cs, deblock_rowstore);

    for (i = 0; i < 16; i++)
        mfx_out(cs, (i < nrefs && refs[i]) ? refs[i] : post_deblock);
    mfx_out(cs, 0);                 /* reference address upper bits     */

    mfx_out_addr(cs, mv_write);
    for (i = 0; i < 16; i++)
        mfx_out(cs, (i < n_mv_read && mv_read[i]) ? mv_read[i] : mv_write);
    mfx_out(cs, 0);

    /* the remaining address slots this generation does not use for
     * AVC decode: scaled reference, SLICE size stream-out, and the
     * two auxiliary surfaces */
    for (i = 0; i < 6; i++) mfx_out(cs, 0);
}

/* Where the compressed bitstream is. */
static void mfx_ind_obj_base_addr(mfx_cs_t *cs, uint32_t bitstream,
                                  uint32_t bitstream_limit) {
    mfx_out(cs, MFX_IND_OBJ_BASE_ADDR_STATE | (26 - 2));
    mfx_out_addr(cs, bitstream);            /* BSD indirect object      */
    mfx_out_addr(cs, bitstream + bitstream_limit);
    for (int i = 0; i < 21; i++) mfx_out(cs, 0);
}

/* The engine's private row stores: one line of context per macroblock
 * row for the bitstream decoder and the motion vector predictor. */
static void mfx_bsp_buf_base_addr(mfx_cs_t *cs, uint32_t bsd_rowstore,
                                  uint32_t mpr_rowstore) {
    mfx_out(cs, MFX_BSP_BUF_BASE_ADDR_STATE | (10 - 2));
    mfx_out_addr(cs, bsd_rowstore);
    mfx_out_addr(cs, mpr_rowstore);
    for (int i = 0; i < 5; i++) mfx_out(cs, 0);
}

/*
 * Everything the sequence and picture parameter sets said.
 *
 * This is the packet that carries the SPS and PPS across, and almost
 * every field in it is a direct copy of something h264.h parsed. The
 * exceptions are the two frame-size fields, which the standard states
 * in macroblocks minus one and the hardware wants the same way.
 */
static void mfx_avc_img_state(mfx_cs_t *cs, const h264_sps_t *sps,
                              const h264_pps_t *pps,
                              const h264_slice_hdr_t *sh,
                              uint32_t mbs_total) {
    uint32_t w_mbs = sps->pic_width_in_mbs;
    uint32_t h_mbs = sps->pic_height_in_map_units *
                     (sps->frame_mbs_only_flag ? 1u : 2u);

    mfx_out(cs, MFX_AVC_IMG_STATE | (16 - 2));

    mfx_out(cs, mbs_total);
    mfx_out(cs, ((h_mbs - 1) << 16) | (w_mbs - 1));

    mfx_out(cs,
            ((uint32_t)(sps->chroma_format_idc) << 10) |
            ((uint32_t)(pps->entropy_coding_mode_flag ? 1 : 0) << 12) |
            ((uint32_t)(pps->weighted_bipred_idc) << 14) |
            ((uint32_t)(pps->weighted_pred_flag ? 1 : 0) << 16) |
            ((uint32_t)(pps->transform_8x8_mode_flag ? 1 : 0) << 17) |
            ((uint32_t)(pps->constrained_intra_pred_flag ? 1 : 0) << 18) |
            ((uint32_t)(sps->direct_8x8_inference_flag ? 1 : 0) << 20) |
            ((uint32_t)(sh->field_pic_flag ? 1 : 0) << 21) |
            ((uint32_t)(sps->frame_mbs_only_flag ? 0u : 1u) << 22));

    mfx_out(cs,
            ((uint32_t)(sh->is_idr ? 1 : 0) << 0) |
            ((uint32_t)(sps->mb_adaptive_frame_field_flag ? 1 : 0) << 1) |
            ((uint32_t)(sh->bottom_field_flag ? 1 : 0) << 2) |
            ((uint32_t)(pps->chroma_qp_index_offset & 0x1F) << 8) |
            ((uint32_t)(pps->second_chroma_qp_index_offset & 0x1F) << 16));

    /* the remaining dwords are encode-only rate control and the
     * error-concealment controls, which decode leaves at their
     * defaults */
    for (int i = 0; i < 11; i++) mfx_out(cs, 0);
}

/*
 * The quantisation matrices.
 *
 * Sent as four separate packets -- intra luma, intra chroma, inter
 * luma, inter chroma -- because the hardware indexes them by type
 * rather than taking one table. A stream with no explicit matrices
 * still has to send these; the flat-16 default is what h264.h filled
 * in, and omitting the packet leaves whatever the previous decode set.
 */
static void mfx_avc_qm_state(mfx_cs_t *cs, int type, const uint8_t *list,
                             int count) {
    mfx_out(cs, MFX_AVC_QM_STATE | ((2 + count / 4) - 2));
    mfx_out(cs, (uint32_t)type);
    for (int i = 0; i < count; i += 4)
        mfx_out(cs, (uint32_t)list[i] |
                    ((uint32_t)list[i + 1] << 8) |
                    ((uint32_t)list[i + 2] << 16) |
                    ((uint32_t)list[i + 3] << 24));
}

/* Where the direct-mode motion vectors of the co-located picture are,
 * which a B slice needs and a P slice does not. */
static void mfx_avc_directmode_state(mfx_cs_t *cs, uint32_t mv_write,
                                     const uint32_t *mv_read, int n) {
    mfx_out(cs, MFX_AVC_DIRECTMODE_STATE | (69 - 2));
    for (int i = 0; i < 16; i++) {
        mfx_out(cs, (i < n && mv_read[i]) ? mv_read[i] : 0);
        mfx_out(cs, 0);
    }
    mfx_out_addr(cs, mv_write);
    for (int i = 0; i < 34; i++) mfx_out(cs, 0);
}

/*
 * Everything the slice header said, plus where in the bitstream this
 * slice's macroblocks start and stop.
 *
 * `first_mb` and `next_mb` are what let a picture be split across
 * several slices: each object decodes the range between them, and the
 * last slice of a picture is the one whose next_mb is the picture's
 * macroblock count.
 */
static void mfx_avc_slice_state(mfx_cs_t *cs, const h264_slice_hdr_t *sh,
                                const h264_pps_t *pps,
                                uint32_t first_mb_x, uint32_t first_mb_y,
                                uint32_t next_mb_x, uint32_t next_mb_y,
                                int is_last) {
    uint32_t slice_type = sh->slice_type;

    mfx_out(cs, MFX_AVC_SLICE_STATE | (11 - 2));

    mfx_out(cs, slice_type);

    mfx_out(cs, (sh->num_ref_idx_l0_active - 1) |
                ((sh->num_ref_idx_l1_active - 1) << 8) |
                ((uint32_t)(sh->cabac_init_idc) << 24));

    mfx_out(cs, ((uint32_t)(sh->slice_qp & 0x3F) << 0) |
                ((uint32_t)(sh->disable_deblocking_filter_idc) << 27) |
                ((uint32_t)(sh->slice_alpha_c0_offset & 0xF) << 8) |
                ((uint32_t)(sh->slice_beta_offset & 0xF) << 16) |
                ((uint32_t)(pps->deblocking_filter_control_present_flag
                                ? 1u : 0u) << 30));

    mfx_out(cs, (first_mb_y << 24) | (first_mb_x << 16));
    mfx_out(cs, (next_mb_y << 24) | (next_mb_x << 16));

    mfx_out(cs, ((uint32_t)(is_last ? 1 : 0) << 19) |
                ((uint32_t)(sh->direct_spatial_mv_pred_flag ? 1 : 0) << 18));

    for (int i = 0; i < 4; i++) mfx_out(cs, 0);
}

/*
 * The reference index lists.
 *
 * Each entry names a slot in MFX_PIPE_BUF_ADDR_STATE plus the field
 * parity. Slots beyond the slice's active count are filled with the
 * reserved value rather than left as whatever the last slice used --
 * the hardware reads all thirty-two regardless of the active count.
 */
static void mfx_avc_ref_idx_state(mfx_cs_t *cs, int list,
                                  const uint8_t *slots, int count) {
    mfx_out(cs, MFX_AVC_REF_IDX_STATE | (10 - 2));
    mfx_out(cs, (uint32_t)list);

    for (int i = 0; i < 8; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) {
            int idx = i * 4 + j;
            uint32_t e = (idx < count) ? (uint32_t)slots[idx] : 0x80u;
            v |= e << (j * 8);
        }
        mfx_out(cs, v);
    }
}

/*
 * The slice data, and the instruction to decode it.
 *
 * `offset` is where in the indirect object this slice's bytes begin;
 * `length` is how many there are. `bit_offset` is where inside the
 * first byte the entropy coding starts, which is what the slice header
 * parse produced and what CABAC needs to align its engine -- an
 * off-by-one here decodes noise rather than failing.
 */
static void mfd_avc_bsd_object(mfx_cs_t *cs, uint32_t offset, uint32_t length,
                               uint32_t bit_offset, int is_first_slice) {
    mfx_out(cs, MFD_AVC_BSD_OBJECT | (6 - 2));
    mfx_out(cs, length);
    mfx_out(cs, offset);
    mfx_out(cs, (1u << 31) |                    /* last slice in object */
                ((bit_offset & 0x7) << 16) |
                ((uint32_t)(is_first_slice ? 1 : 0) << 17));
    mfx_out(cs, 0);
    mfx_out(cs, 0);
}

/* ===== VEBOX: NV12 to packed RGB ===== */

/*
 * The video enhancement box does colour space conversion in fixed
 * function, which is what makes an end-to-end hardware path possible:
 * MFX writes NV12, VEBOX reads it and writes BGRA, and the CPU never
 * touches a pixel.
 *
 * VEBOX is state-heavy -- the real programming model puts the
 * conversion coefficients in a state heap that VEB_STATE points at
 * rather than in the command -- so what is emitted here is the command
 * layer, with the coefficient block built by the caller. As with the
 * MFX commands above, this has never run.
 */
static void veb_surface_state(mfx_cs_t *cs, uint32_t in_gpu, uint32_t out_gpu,
                              uint32_t width, uint32_t height,
                              uint32_t in_pitch, uint32_t out_pitch) {
    mfx_out(cs, VEB_SURFACE_STATE | (16 - 2));
    mfx_out(cs, 0);                             /* input surface        */
    mfx_out(cs, ((height - 1) << 18) | ((width - 1) << 4));
    mfx_out(cs, ((uint32_t)MFX_SURFACE_NV12 << 28) | ((in_pitch - 1) << 3));
    mfx_out(cs, 0);
    mfx_out(cs, in_gpu);
    mfx_out(cs, 0);
    mfx_out(cs, 1);                             /* output surface       */
    mfx_out(cs, ((height - 1) << 18) | ((width - 1) << 4));
    mfx_out(cs, (0xAu << 28) | ((out_pitch - 1) << 3));  /* B8G8R8A8    */
    mfx_out(cs, 0);
    mfx_out(cs, out_gpu);
    for (int i = 0; i < 4; i++) mfx_out(cs, 0);
}

static void veb_di_iecp(mfx_cs_t *cs, uint32_t in_gpu, uint32_t out_gpu) {
    mfx_out(cs, VEB_DI_IECP | (10 - 2));
    mfx_out(cs, in_gpu);
    mfx_out(cs, 0);
    for (int i = 0; i < 4; i++) mfx_out(cs, 0);
    mfx_out(cs, out_gpu);
    mfx_out(cs, 0);
    mfx_out(cs, 0);
}

#endif /* MEDIA_MFX_H */
