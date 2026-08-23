#ifndef MEDIA_DECODE_C
#define MEDIA_DECODE_C

/*
 * src/media/decode.c — compressed frames in, window pixels out.
 *
 * ---- the path ----
 *
 *   H.264 bytes
 *     -> h264.h            NAL scan, SPS/PPS/slice headers  (CPU)
 *     -> mfx.h             MFX command stream               (CPU)
 *     -> VCS ring          fixed-function decode            (GPU)
 *     -> NV12 surface
 *     -> VEBOX or csc.h    colour conversion                (GPU or CPU)
 *     -> window canvas     which Ring 3 already has mapped
 *
 * The last step is the interesting one. A window's pixels in this
 * system are `app_canvas`, a kernel array that vmm_map_shared() maps
 * into every process's address space -- so a program that calls
 * os_canvas() is holding a pointer to those exact pages. Mapping the
 * same physical pages into the GPU's GGTT means the decoder's output
 * surface *is* the window: the engine writes, and the bytes it writes
 * are already at the address the Ring 3 program is reading. There is no
 * copy between the decoder and the application, and no syscall in the
 * path at all.
 *
 * That is what "straight into user-space framebuffers" has to mean to
 * be worth anything. Decoding into a kernel buffer and then memcpying
 * it to the window would move 960 KB per frame -- at 30 fps that is
 * 28 MB/s of pure memory traffic to accomplish nothing.
 *
 * ---- what is CPU work and what is not ----
 *
 * Worth being exact, because the brief asks for CPU-bound decoding to
 * be bypassed entirely and one part of it cannot be.
 *
 * The per-macroblock work -- CABAC or CAVLC entropy decode, inverse
 * transform, intra prediction, motion compensation, deblocking -- is
 * all fixed function. For 1080p that is 8160 macroblocks a frame and it
 * is the entire cost of decoding; none of it is executed by the
 * processor here.
 *
 * The headers are parsed on the CPU: a few hundred bits per slice.
 * That is not an omission, it is how every hardware decoder works,
 * VA-API and DXVA included -- the syntax is irregular and branch-heavy
 * and costs more in silicon than it saves. The processor's share of
 * decoding a frame is therefore proportional to the number of slices,
 * not to the number of pixels, which is the property that matters.
 *
 * ---- what has and has not run ----
 *
 * No machine this system is built or tested on has an Intel GPU. QEMU
 * models none, so igpu_init() reports "no Intel display controller" and
 * everything from the VCS ring downward is unreachable. The MFX command
 * encodings in mfx.h come from Intel's public documentation and the
 * open-source reference driver; they have not touched silicon and a
 * misplaced field would show up as an engine hang that only hardware
 * can reveal.
 *
 * What is verified, on the host, every build: the bitstream parser, the
 * surface geometry, and the colour conversion. See tools/media_test.c.
 */

#include <stdint.h>
#include "../igpu.h"
#include "../pmm.h"
#include "h264.h"
#include "mfx.h"
#include "csc.h"

static void vdec_log(const char *msg) {
    serial_puts("[media] ");
    serial_puts(msg);
    serial_putc('\n');
}

/* ===== surfaces ===== */

/*
 * A decoded picture.
 *
 * NV12 with the chroma plane immediately after the luma one, which is
 * what MFX_SURFACE_STATE describes with a single base address and a
 * line offset. The allocation is physically contiguous because the GGTT
 * maps 4 KB pages and a surface that was scattered would need its pages
 * mapped individually -- which works, but pmm_alloc_contig() already
 * exists and a decode surface is allocated once per stream.
 */
typedef struct {
    uint8_t *cpu;           /* kernel virtual                          */
    uint64_t phys;
    uint32_t gpu;           /* GGTT address the engine uses            */
    uint32_t pages;
    uint32_t pitch;         /* bytes per luma line                     */
    uint32_t height;        /* luma lines, macroblock aligned          */
    uint32_t uv_offset;     /* bytes from the base to the chroma plane */
    int      in_use;

    /* what picture this holds, for reference list construction */
    int32_t  poc;
    uint32_t frame_num;
    int      is_reference;
    int      is_long_term;
} vdec_surface_t;

#define VDEC_MAX_SURFACES  (H264_MAX_DPB + 1)

/* The decoder, of which there is one. A second concurrent stream would
 * need a second DPB and a second set of row stores, and nothing in this
 * system has asked for one. */
static struct {
    int             ready;          /* hardware is up                   */
    int             open;           /* a stream is being decoded        */

    h264_sps_t      sps[H264_MAX_SPS];
    h264_pps_t      pps[H264_MAX_PPS];
    h264_poc_state_t poc;

    uint32_t        width, height;      /* cropped, what gets displayed */
    uint32_t        mb_width, mb_height;
    uint32_t        surf_pitch, surf_height;

    vdec_surface_t surf[VDEC_MAX_SURFACES];
    int             nsurf;
    int             current;            /* index of the picture in flight */

    /* the engine's private scratch */
    uint8_t        *rowstore;
    uint32_t        rowstore_gpu;
    uint32_t        rowstore_pages;
    uint32_t        intra_gpu, deblock_gpu, bsd_gpu, mpr_gpu, mv_gpu;

    /* the compressed bits, where the engine reads them from */
    uint8_t        *bitstream;
    uint32_t        bitstream_gpu;
    uint32_t        bitstream_cap;

    /* where converted pixels go */
    uint32_t       *target;
    uint32_t        target_w, target_h, target_pitch;
    uint32_t        target_gpu;

    uint32_t        frames_decoded;
    uint32_t        frames_failed;
    int             hw_path;            /* 0 = CPU colour conversion    */
    const char     *status;
} vdec;

/* ===== allocation ===== */

static int vdec_surface_alloc(vdec_surface_t *s, uint32_t pitch,
                               uint32_t height) {
    uint32_t luma  = pitch * height;
    uint32_t total = luma + luma / 2;           /* NV12 is 1.5x luma    */
    uint32_t pages = (total + 4095) / 4096;
    uint64_t phys;

    phys = pmm_alloc_contig(pages);
    if (!phys) return -1;

    s->phys      = phys;
    s->cpu       = (uint8_t *)phys_to_virt(phys);
    s->pages     = pages;
    s->pitch     = pitch;
    s->height    = height;
    s->uv_offset = luma;
    s->in_use    = 0;
    s->is_reference = 0;

    s->gpu = igpu_media_map(phys, pages);
    if (!s->gpu) {
        pmm_free_contig(phys, pages);
        s->cpu = 0;
        return -1;
    }

    /* Mid-grey with neutral chroma, so a surface that is displayed
     * before anything has decoded into it is obviously blank rather
     * than whatever the last tenant of that memory left. */
    for (uint32_t i = 0; i < luma; i++) s->cpu[i] = 16;
    for (uint32_t i = luma; i < total; i++) s->cpu[i] = 128;

    return 0;
}

/*
 * The engine's row stores.
 *
 * MFX keeps one line of context per macroblock column for several of
 * its stages -- the bitstream decoder, the motion vector predictor, the
 * intra predictor and the deblocker each want their own. The sizes are
 * per-macroblock-column and generous here; undersizing them corrupts
 * the bottom of every picture in a way that looks like a broken stream.
 */
static int vdec_rowstore_alloc(uint32_t mb_width) {
    uint32_t per = ((mb_width + 15) & ~15u) * 64;       /* 64 B / MB col */
    uint32_t total = per * 5;
    uint32_t pages = (total + 4095) / 4096;
    uint64_t phys = pmm_alloc_contig(pages);

    if (!phys) return -1;

    vdec.rowstore = (uint8_t *)phys_to_virt(phys);
    vdec.rowstore_pages = pages;
    vdec.rowstore_gpu = igpu_media_map(phys, pages);
    if (!vdec.rowstore_gpu) {
        pmm_free_contig(phys, pages);
        vdec.rowstore = 0;
        return -1;
    }

    for (uint32_t i = 0; i < total; i++) vdec.rowstore[i] = 0;

    vdec.intra_gpu   = vdec.rowstore_gpu + per * 0;
    vdec.deblock_gpu = vdec.rowstore_gpu + per * 1;
    vdec.bsd_gpu     = vdec.rowstore_gpu + per * 2;
    vdec.mpr_gpu     = vdec.rowstore_gpu + per * 3;
    vdec.mv_gpu      = vdec.rowstore_gpu + per * 4;
    return 0;
}

/* ===== the decoded picture buffer ===== */

static vdec_surface_t *vdec_get_free_surface(void) {
    for (int i = 0; i < vdec.nsurf; i++)
        if (!vdec.surf[i].in_use) return &vdec.surf[i];

    /*
     * Every surface is held. That means the stream declared fewer
     * reference frames than it actually keeps alive, which a corrupt
     * or hostile file can do deliberately. Evicting the oldest
     * non-reference picture is what a real DPB does; if every one is a
     * reference the stream is malformed and decoding stops rather than
     * overwriting a picture that is still being predicted from.
     */
    {
        vdec_surface_t *oldest = 0;
        for (int i = 0; i < vdec.nsurf; i++) {
            vdec_surface_t *s = &vdec.surf[i];
            if (s->is_reference) continue;
            if (!oldest || s->poc < oldest->poc) oldest = s;
        }
        if (oldest) { oldest->in_use = 0; return oldest; }
    }
    return 0;
}

/* Drop every reference, which is what an IDR picture means. */
static void vdec_dpb_flush(void) {
    for (int i = 0; i < vdec.nsurf; i++) {
        vdec.surf[i].in_use = 0;
        vdec.surf[i].is_reference = 0;
        vdec.surf[i].is_long_term = 0;
    }
}

/*
 * Build the reference picture list for a slice.
 *
 * The default order the standard specifies: for a P slice, short-term
 * references sorted by descending frame number; for a B slice, list 0
 * is references before the current picture in display order followed by
 * those after, and list 1 is the reverse. Slice header modifications
 * are not applied -- h264.h walks them to find the slice data offset
 * but does not record them, so a stream that reorders its lists will
 * predict from the wrong picture.
 *
 * That is a real limitation and it is stated rather than hidden: the
 * common case, which is every stream a normal encoder produces without
 * being asked for reordering, uses the default lists.
 */
static int vdec_build_ref_list(int slice_type, int32_t cur_poc,
                                uint8_t *slots, int max) {
    int n = 0;

    for (int i = 0; i < vdec.nsurf && n < max; i++) {
        vdec_surface_t *s = &vdec.surf[i];
        if (!s->in_use || !s->is_reference) continue;
        if (i == vdec.current) continue;
        slots[n++] = (uint8_t)i;
    }

    /* insertion sort: descending frame_num for P, ascending distance
     * from the current POC for B */
    for (int i = 1; i < n; i++) {
        uint8_t k = slots[i];
        int j = i - 1;
        while (j >= 0) {
            int swap;
            if (slice_type == H264_SLICE_B) {
                int32_t da = vdec.surf[slots[j]].poc - cur_poc;
                int32_t db = vdec.surf[k].poc - cur_poc;
                if (da < 0) da = -da;
                if (db < 0) db = -db;
                swap = da > db;
            } else {
                swap = vdec.surf[slots[j]].frame_num <
                       vdec.surf[k].frame_num;
            }
            if (!swap) break;
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = k;
    }
    return n;
}

/* ===== building and submitting a picture ===== */

#define VDEC_CS_DWORDS  1024
static uint32_t vdec_cs_buf[VDEC_CS_DWORDS];

/*
 * Emit the per-picture state.
 *
 * This runs once per picture rather than once per slice: the SPS, PPS,
 * surface and buffer addresses do not change between the slices of one
 * frame, and re-sending them would cost a pipeline flush each time.
 */
static void vdec_emit_picture_state(mfx_cs_t *cs, const h264_sps_t *sps,
                                     const h264_pps_t *pps,
                                     const h264_slice_hdr_t *sh,
                                     vdec_surface_t *dst,
                                     const uint8_t *refs, int nrefs) {
    uint32_t ref_gpu[16];
    uint32_t mv_read[16];
    int i;

    for (i = 0; i < 16; i++) {
        ref_gpu[i] = (i < nrefs) ? vdec.surf[refs[i]].gpu : 0;
        mv_read[i] = 0;
    }

    mfx_pipe_mode_select(cs, MFX_CODEC_AVC, 1, 0);

    mfx_surface_state(cs, vdec.width, vdec.surf_height,
                      vdec.surf_pitch, vdec.surf_height);

    mfx_pipe_buf_addr_state(cs, dst->gpu, dst->gpu,
                            vdec.intra_gpu, vdec.deblock_gpu,
                            ref_gpu, nrefs,
                            vdec.mv_gpu, mv_read, 0);

    mfx_ind_obj_base_addr(cs, vdec.bitstream_gpu, vdec.bitstream_cap);
    mfx_bsp_buf_base_addr(cs, vdec.bsd_gpu, vdec.mpr_gpu);

    mfx_avc_img_state(cs, sps, pps, sh, vdec.mb_width * vdec.mb_height);

    /* The four quantisation matrices, in the order the hardware
     * indexes them. */
    mfx_avc_qm_state(cs, 0, pps->scaling_4x4[0], 16);   /* intra Y      */
    mfx_avc_qm_state(cs, 1, pps->scaling_4x4[1], 16);   /* intra chroma */
    mfx_avc_qm_state(cs, 2, pps->scaling_4x4[3], 16);   /* inter Y      */
    mfx_avc_qm_state(cs, 3, pps->scaling_4x4[4], 16);   /* inter chroma */

    if (sh->slice_type == H264_SLICE_B)
        mfx_avc_directmode_state(cs, vdec.mv_gpu, mv_read, 0);
}

/*
 * Decode one slice.
 *
 * `data` points at the slice NAL's payload -- past the one-byte NAL
 * header -- and `len` is its length. The bytes are copied into the
 * indirect object buffer the engine reads from, because that buffer has
 * to be GGTT-mapped and the caller's is not.
 */
static int vdec_decode_slice(const uint8_t *data, uint32_t len,
                              int nal_ref_idc, int nal_type) {
    h264_slice_hdr_t sh;
    const h264_sps_t *sps = 0;
    const h264_pps_t *pps = 0;
    vdec_surface_t *dst;
    mfx_cs_t cs;
    uint8_t refs[32];
    int nrefs;
    int32_t poc;
    uint32_t first_mb, next_mb;

    if (!h264_parse_slice_header(data, len, nal_ref_idc, nal_type,
                                 vdec.sps, vdec.pps, &sh, &sps, &pps))
        return -1;

    if (len > vdec.bitstream_cap) return -1;

    /* An IDR resets everything: no picture before it may be referenced. */
    if (sh.is_idr) vdec_dpb_flush();

    poc = h264_compute_poc(sps, &sh, &vdec.poc);

    /* The first slice of a picture claims a surface; the rest decode
     * into the one already in flight. `first_mb_in_slice` being zero is
     * what marks the first slice, and is the only signal there is. */
    if (sh.first_mb_in_slice == 0) {
        dst = vdec_get_free_surface();
        if (!dst) return -1;

        dst->in_use       = 1;
        dst->poc          = poc;
        dst->frame_num    = sh.frame_num;
        dst->is_reference = (nal_ref_idc != 0);

        vdec.current = (int)(dst - vdec.surf);
    } else {
        if (vdec.current < 0) return -1;
        dst = &vdec.surf[vdec.current];
    }

    nrefs = vdec_build_ref_list((int)sh.slice_type, poc, refs,
                                 (int)sizeof(refs));

    /* Copy the slice into the buffer the engine reads. */
    for (uint32_t i = 0; i < len; i++) vdec.bitstream[i] = data[i];
    __asm__ volatile("sfence" ::: "memory");

    mfx_cs_init(&cs, vdec_cs_buf, VDEC_CS_DWORDS);

    if (sh.first_mb_in_slice == 0)
        vdec_emit_picture_state(&cs, sps, pps, &sh, dst, refs, nrefs);

    if (sh.slice_type != H264_SLICE_I && sh.slice_type != H264_SLICE_SI)
        mfx_avc_ref_idx_state(&cs, 0, refs, nrefs);
    if (sh.slice_type == H264_SLICE_B)
        mfx_avc_ref_idx_state(&cs, 1, refs, nrefs);

    first_mb = sh.first_mb_in_slice;
    next_mb  = vdec.mb_width * vdec.mb_height;

    mfx_avc_slice_state(&cs, &sh, pps,
                        first_mb % vdec.mb_width, first_mb / vdec.mb_width,
                        next_mb % vdec.mb_width, next_mb / vdec.mb_width,
                        1);

    mfd_avc_bsd_object(&cs, 0, len, sh.data_bit_offset & 7,
                       sh.first_mb_in_slice == 0);

    if (cs.overflow) {
        vdec_log("command stream overflowed - slice dropped");
        return -1;
    }

    return igpu_engine_exec(&igpu_vcs, cs.dw, (int)cs.n);
}

/* ===== stream setup ===== */

/*
 * A sequence parameter set has arrived and it changes the picture
 * geometry, so every surface has to be reallocated.
 *
 * Surfaces are macroblock-aligned rather than cropped: the hardware
 * decodes whole macroblocks and the crop is applied when the picture is
 * displayed, not when it is decoded. A 1080p stream is 1088 lines of
 * surface holding 1080 lines of picture.
 */
static int vdec_configure(const h264_sps_t *sps) {
    uint32_t pitch, height;
    int want;

    vdec.mb_width  = sps->pic_width_in_mbs;
    vdec.mb_height = sps->pic_height_in_map_units *
                      (sps->frame_mbs_only_flag ? 1u : 2u);
    vdec.width  = sps->width;
    vdec.height = sps->height;

    pitch  = vdec.mb_width * 16;
    height = vdec.mb_height * 16;

    if (vdec.surf_pitch == pitch && vdec.surf_height == height &&
        vdec.nsurf > 0)
        return 0;                       /* nothing changed */

    for (int i = 0; i < vdec.nsurf; i++) {
        if (vdec.surf[i].cpu)
            pmm_free_contig(vdec.surf[i].phys, vdec.surf[i].pages);
        vdec.surf[i].cpu = 0;
    }
    vdec.nsurf = 0;
    vdec.current = -1;

    vdec.surf_pitch  = pitch;
    vdec.surf_height = height;

    /* One more than the stream says it needs: the picture being decoded
     * is not a reference until it is finished. */
    want = sps->max_num_ref_frames + 2;
    if (want > VDEC_MAX_SURFACES) want = VDEC_MAX_SURFACES;

    for (int i = 0; i < want; i++) {
        if (vdec_surface_alloc(&vdec.surf[i], pitch, height) != 0) {
            /* Out of contiguous memory. Whatever was allocated still
             * forms a usable, smaller DPB as long as there are at
             * least two -- one reference and one being decoded. */
            break;
        }
        vdec.nsurf++;
    }

    if (vdec.nsurf < 2) {
        vdec_log("not enough contiguous memory for a picture buffer");
        return -1;
    }

    if (vdec_rowstore_alloc(vdec.mb_width) != 0) {
        vdec_log("could not allocate the engine's row stores");
        return -1;
    }

    /* serial_put_dec rather than klibc's uint_to_str: this file is
     * included alongside the drivers, well before gfx.h declares that
     * one, and pulling gfx.h forward to reach a number formatter would
     * reorder half the kernel's includes. */
    serial_puts("[media] stream is ");
    serial_put_dec(vdec.width);
    serial_putc('x');
    serial_put_dec(vdec.height);
    serial_puts(", ");
    serial_put_dec((uint32_t)vdec.nsurf);
    serial_puts(" surfaces\n");

    return 0;
}

/* ===== output ===== */

/*
 * Put the most recently decoded picture into the window.
 *
 * The hardware path hands both surfaces to VEBOX and returns without
 * the processor reading a pixel. The fallback converts on the CPU,
 * which is the only thing that can work on a machine with no video
 * engine -- and is still not a software *decoder*: it is one pass of
 * integer arithmetic over an already-decoded frame, not an H.264
 * implementation.
 */
static int vdec_present(void) {
    vdec_surface_t *s;

    if (vdec.current < 0 || !vdec.target) return -1;
    s = &vdec.surf[vdec.current];
    if (!s->cpu) return -1;

    if (vdec.hw_path && igpu_vecs.ready && vdec.target_gpu) {
        mfx_cs_t cs;
        mfx_cs_init(&cs, vdec_cs_buf, VDEC_CS_DWORDS);
        veb_surface_state(&cs, s->gpu, vdec.target_gpu,
                          vdec.width, vdec.height,
                          vdec.surf_pitch, vdec.target_pitch * 4);
        veb_di_iecp(&cs, s->gpu, vdec.target_gpu);
        if (!cs.overflow &&
            igpu_engine_exec(&igpu_vecs, cs.dw, (int)cs.n) == 0)
            return 0;
        /* VEBOX did not retire. Fall through rather than showing
         * nothing: a converted frame late beats a dropped one. */
        vdec.hw_path = 0;
        vdec_log("VEBOX conversion failed - using the integer path");
    }

    /* The engine wrote these pages; make sure this processor sees the
     * writes before reading them. */
    for (uint32_t off = 0; off < s->pitch * s->height; off += 64)
        __asm__ volatile("clflush (%0)" :: "r"(s->cpu + off) : "memory");
    __asm__ volatile("mfence" ::: "memory");

    csc_nv12_to_bgra(s->cpu, s->pitch,
                     s->cpu + s->uv_offset, s->pitch,
                     vdec.width, vdec.height,
                     vdec.target, vdec.target_pitch,
                     vdec.target_w, vdec.target_h);
    return 0;
}

/* ===== the public interface ===== */

/*
 * Point the decoder at a window.
 *
 * `fb` is the window's pixels -- for an application window that is the
 * shared canvas Ring 3 already has mapped, so mapping it into the GGTT
 * here is what lets the engine write directly into what the program
 * sees. A framebuffer that cannot be mapped is not an error: the
 * integer conversion path writes to it with ordinary stores.
 */
static void vdec_set_target(uint32_t *fb, uint32_t w, uint32_t h,
                             uint32_t pitch_px) {
    vdec.target       = fb;
    vdec.target_w     = w;
    vdec.target_h     = h;
    vdec.target_pitch = pitch_px;
    vdec.target_gpu   = 0;

    if (vdec.ready && fb)
        vdec.target_gpu = igpu_media_map_virt(fb, pitch_px * h * 4);

    vdec.hw_path = (vdec.target_gpu != 0) && igpu_vecs.ready;
}

/*
 * Feed the decoder a chunk of Annex B byte stream.
 *
 * Parameter sets are absorbed, slices are decoded, everything else is
 * skipped. Returns the number of pictures completed.
 */
static int vdec_feed(const uint8_t *buf, uint32_t len) {
    uint32_t pos = 0;
    h264_nal_t nal;
    int pictures = 0;

    if (!vdec.open) return -1;

    while (h264_next_nal(buf, len, &pos, &nal)) {
        switch (nal.type) {
        case H264_NAL_SPS: {
            h264_sps_t sps;
            if (!h264_parse_sps(nal.data, nal.len, &sps)) {
                vdec_log("malformed sequence parameter set");
                break;
            }
            vdec.sps[sps.sps_id] = sps;
            if (vdec_configure(&sps) != 0) return pictures;
            break;
        }

        case H264_NAL_PPS: {
            h264_pps_t pps;
            if (!h264_parse_pps(nal.data, nal.len, vdec.sps, &pps)) {
                vdec_log("malformed picture parameter set");
                break;
            }
            vdec.pps[pps.pps_id] = pps;
            break;
        }

        case H264_NAL_IDR:
        case H264_NAL_SLICE:
            if (!vdec.nsurf) break;        /* no SPS yet */
            if (vdec_decode_slice(nal.data, nal.len,
                                   nal.ref_idc, nal.type) == 0) {
                vdec.frames_decoded++;
                pictures++;
                vdec_present();
            } else {
                vdec.frames_failed++;
            }
            break;

        default:
            break;                          /* SEI, AUD, filler */
        }
    }

    return pictures;
}

/*
 * Open the decoder.
 *
 * Returns 0 if there is a hardware decode path. Everything the stream
 * needs beyond this is allocated when the first sequence parameter set
 * arrives, because until then the picture size is unknown.
 */
static int vdec_open(void) {
    uint64_t phys;
    uint32_t pages;

    if (vdec.open) return 0;

    for (uint32_t i = 0; i < sizeof(vdec); i++) ((uint8_t *)&vdec)[i] = 0;
    vdec.current = -1;
    vdec.status  = "no video engine";

    if (!igpu.active) {
        vdec_log("no Intel GPU on this machine - no hardware decode");
        return -1;
    }
    if (!igpu_vcs.ready) {
        vdec_log("the video engine did not come up - no hardware decode");
        return -1;
    }

    /* The indirect object buffer: where slice bits go for the engine to
     * read. One megabyte holds any single slice of any level. */
    vdec.bitstream_cap = 1024 * 1024;
    pages = vdec.bitstream_cap / 4096;
    phys  = pmm_alloc_contig(pages);
    if (!phys) {
        vdec_log("no contiguous memory for the bitstream buffer");
        return -1;
    }
    vdec.bitstream     = (uint8_t *)phys_to_virt(phys);
    vdec.bitstream_gpu = igpu_media_map(phys, pages);
    if (!vdec.bitstream_gpu) {
        pmm_free_contig(phys, pages);
        vdec_log("could not map the bitstream buffer into the GGTT");
        return -1;
    }

    vdec.ready  = 1;
    vdec.open   = 1;
    vdec.status = "hardware decode ready";
    vdec_log("MFX decoder ready");
    return 0;
}

static void vdec_close(void) {
    if (!vdec.open) return;

    for (int i = 0; i < vdec.nsurf; i++)
        if (vdec.surf[i].cpu)
            pmm_free_contig(vdec.surf[i].phys, vdec.surf[i].pages);

    vdec.open  = 0;
    vdec.ready = 0;
    vdec.nsurf = 0;
    vdec.current = -1;
}

static int vdec_hw_available(void) {
    return igpu.active && igpu_vcs.ready;
}

static const char *vdec_status(void) {
    if (!igpu.active)      return "no Intel GPU";
    if (!igpu_vcs.ready)   return "GPU present, video engine down";
    if (!vdec.open)       return "idle";
    return vdec.status;
}

#endif /* MEDIA_DECODE_C */
