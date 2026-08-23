#ifndef IGPU_H
#define IGPU_H

#include <stdint.h>
#include "pci.h"

/*
 * Intel integrated GPU driver — Gen9/Gen9.5 (Skylake through Comet Lake).
 *
 * Scope: blitter (BCS) acceleration only.  We deliberately do NOT touch
 * the display hardware (pipes/planes/PLLs) — the firmware-set GOP mode
 * keeps scanning out, which is what keeps the OS working on every other
 * GPU vendor too.  This driver adds:
 *
 *   1. BAR0 (GTTMMADR) MMIO mapping — registers + GGTT
 *   2. GGTT setup for a private window at the top of GPU address space
 *   3. BCS legacy ring buffer (Gen9 still supports non-execlist mode)
 *   4. XY_COLOR_BLT execution with a CPU-verified self-test
 *
 * Register offsets, PTE encoding and command encodings are adapted from
 * established open-source references rather than invented here:
 *   - Linux i915: intel_engine_regs.h, intel_uncore.c (Gen9 forcewake
 *     table), intel_gpu_commands.h, gen8 GGTT PTE encode
 *   - SerenityOS IntelNativeGraphicsAdapter: probe-then-bail structure,
 *     "reuse the firmware framebuffer" philosophy
 *
 * Every wait is bounded; any failure tears the driver down and leaves
 * the system on the (fully functional) CPU rendering path.
 */

/* ===== SUPPORTED DEVICES (Gen9 / Gen9.5 graphics IP) ===== */

typedef struct { uint16_t id; const char *name; } igpu_id_t;

static const igpu_id_t igpu_known[] = {
    /* Skylake */
    {0x1902,"HD 510 (Skylake)"},   {0x1906,"HD 510 (Skylake)"},
    {0x190B,"HD 510 (Skylake)"},   {0x1912,"HD 530 (Skylake)"},
    {0x1916,"HD 520 (Skylake)"},   {0x191B,"HD 530 (Skylake)"},
    {0x191D,"HD P530 (Skylake)"},  {0x191E,"HD 515 (Skylake)"},
    {0x1921,"HD 520 (Skylake)"},   {0x1923,"HD 535 (Skylake)"},
    {0x1926,"Iris 540 (Skylake)"}, {0x1927,"Iris 550 (Skylake)"},
    {0x192B,"Iris 555 (Skylake)"}, {0x1932,"Iris Pro 580 (Skylake)"},
    /* Apollo Lake / Broxton */
    {0x5A84,"HD 505 (Apollo Lake)"}, {0x5A85,"HD 500 (Apollo Lake)"},
    /* Kaby Lake */
    {0x5902,"HD 610 (Kaby Lake)"}, {0x5906,"HD 610 (Kaby Lake)"},
    {0x5912,"HD 630 (Kaby Lake)"}, {0x5916,"HD 620 (Kaby Lake)"},
    {0x591B,"HD 630 (Kaby Lake)"}, {0x591D,"HD P630 (Kaby Lake)"},
    {0x591E,"HD 615 (Kaby Lake)"}, {0x5921,"HD 620 (Kaby Lake)"},
    {0x5926,"Iris 640 (Kaby Lake)"}, {0x5927,"Iris 650 (Kaby Lake)"},
    /* Coffee Lake / Whiskey Lake */
    {0x3E90,"UHD 610 (Coffee Lake)"}, {0x3E91,"UHD 630 (Coffee Lake)"},
    {0x3E92,"UHD 630 (Coffee Lake)"}, {0x3E93,"UHD 610 (Coffee Lake)"},
    {0x3E98,"UHD 630 (Coffee Lake)"}, {0x3E9B,"UHD 630 (Coffee Lake)"},
    {0x3EA0,"UHD 620 (Whiskey Lake)"}, {0x3EA5,"Iris 655 (Coffee Lake)"},
    /* Comet Lake */
    {0x9B21,"UHD (Comet Lake)"},  {0x9B41,"UHD (Comet Lake)"},
    {0x9BA4,"UHD 610 (Comet Lake)"}, {0x9BC4,"UHD (Comet Lake)"},
    {0x9BC5,"UHD 630 (Comet Lake)"}, {0x9BC8,"UHD 630 (Comet Lake)"},
    {0x9BCA,"UHD 620 (Comet Lake)"},
    /* Gemini Lake */
    {0x3184,"UHD 605 (Gemini Lake)"}, {0x3185,"UHD 600 (Gemini Lake)"},
    {0, 0}
};

/* ===== REGISTERS (offsets per Linux i915) ===== */

#define IGPU_BCS_BASE          0x22000      /* blitter command streamer */
#define IGPU_RING_TAIL         0x30
#define IGPU_RING_HEAD         0x34
#define IGPU_RING_START        0x38
#define IGPU_RING_CTL          0x3C
#define IGPU_ENG_MI_MODE       0x9C         /* engine base-relative */
#define IGPU_ENG_GFX_MODE      0x29C

#define IGPU_MI_MODE_STOP_RING (1u << 8)
#define IGPU_GFX_MODE_EXECLIST (1u << 15)   /* GFX_RUN_LIST_ENABLE */
#define IGPU_RING_CTL_VALID    (1u << 0)

#define IGPU_BLT_HWS_PGA       0x04280      /* BCS hardware status page */

/* Gen9 per-domain forcewake (i915 intel_uncore.c fw table) */
#define IGPU_FORCEWAKE_BLT     0xA188
#define IGPU_FORCEWAKE_BLT_ACK 0x130044
#define IGPU_FW_KERNEL_BIT     1u

#define IGPU_GFX_FLSH_CNTL     0x101008     /* GGTT write flush, gen6+ */

/* ---- error / hang diagnostics (offsets per Linux i915) ----
 * EIR/EMR/ESR live in the render block; the per-engine instruction
 * parser error registers are engine-base relative and latch the exact
 * command header that broke the pipeline. */
#define IGPU_EIR               0x20B0      /* Error Identity            */
#define IGPU_EMR               0x20B4      /* Error Mask                */
#define IGPU_ESR               0x20B8      /* Error Status              */
#define IGPU_ENG_IPEIR         0x64        /* parser error: address     */
#define IGPU_ENG_IPEHR         0x68        /* parser error: cmd header  */
#define IGPU_ENG_INSTDONE      0x6C        /* unit busy/done bits       */
#define IGPU_ENG_ACTHD         0x74        /* active head (fetch addr)  */
#define IGPU_ENG_ACTHD_UDW     0x5C        /* active head, upper dword  */
#define IGPU_RING_FAULT_REG    0x4094      /* GEN8_RING_FAULT_REG       */
#define IGPU_FAULT_TLB_DATA0   0x4B10
#define IGPU_FAULT_TLB_DATA1   0x4B14

/* EIR bits (i915 i915_reg.h, I915_ERROR_*) */
#define IGPU_ERR_INSTRUCTION   (1u << 0)
#define IGPU_ERR_MEM_REFRESH   (1u << 1)
#define IGPU_ERR_PRIV          (1u << 3)
#define IGPU_ERR_PAGE_TABLE    (1u << 4)

/* engine reset (i915 GEN6_GDRST / GEN6_GRDOM_BLT) */
#define IGPU_GDRST             0x941C
#define IGPU_GRDOM_BLT         (1u << 3)

/* ===== COMMANDS (encodings per i915 intel_gpu_commands.h, gen8+) ===== */

#define MI_NOOP_CMD            0x00000000u
#define MI_FLUSH_DW_CMD        ((0x26u << 23) | 2)              /* 4 dw */
#define MI_STORE_DWORD_IMM_CMD ((0x20u << 23) | (1u << 22) | 2) /* GGTT, 4 dw */
#define XY_COLOR_BLT_CMD       ((0x2u << 29) | (0x50u << 22) | \
                                (1u << 21) | (1u << 20) | 5)    /* 7 dw */
#define BLT_DEPTH_32           (3u << 24)
#define BLT_ROP_PATCOPY        (0xF0u << 16)

/*
 * Surface-to-surface copy. Ten dwords on gen8+, because both addresses
 * are 64-bit: command, BR13, destination rectangle, destination address,
 * source origin, source pitch, source address.
 *
 * This is what makes the blitter useful for something other than
 * clearing. A fill can only paint a colour; a copy can move an image
 * that the processor has already composed, which is the expensive half
 * of every frame this system draws.
 */
#define XY_SRC_COPY_BLT_CMD    ((0x2u << 29) | (0x53u << 22) | \
                                (1u << 21) | (1u << 20) | 8)    /* 10 dw */
#define BLT_ROP_SRCCOPY        (0xCCu << 16)

/* ===== DRIVER STATE ===== */

#define IGPU_RING_PAGES 4
#define IGPU_RING_BYTES (IGPU_RING_PAGES * 4096)
#define IGPU_TEST_W     64
#define IGPU_TEST_H     64

static struct {
    int active;                 /* self-test passed, blitter usable */
    const char *name;
    const char *status;         /* human-readable stage/failure     */
    uint16_t device_id;
    volatile uint8_t *mmio;     /* BAR0 registers                   */
    volatile uint8_t *ggtt;     /* BAR0 upper half: GGTT PTEs       */
    uint64_t mmio_size;
    uint64_t aperture_base;     /* BAR2 (GMADR) phys                */
    uint64_t aperture_size;
    uint32_t ggtt_slots;
    uint32_t base_slot;         /* start of our private GGTT window */
    uint32_t ring_gpu;          /* GPU virtual addresses            */
    uint32_t hws_gpu;
    uint32_t status_gpu;
    uint32_t target_gpu;
    uint32_t ring_tail;         /* byte offset of next command      */
    uint32_t seqno;
    /* visible framebuffer as a blit target (when GGTT-reachable)   */
    int      fb_blittable;
    uint32_t fb_gpu_addr;
    uint64_t fb_phys;           /* for CPU-side verification        */
    uint32_t fb_pitch_bytes;
    uint32_t fb_w, fb_h;
} igpu = { .status = "not probed" };

static uint32_t igpu_ring_mem[IGPU_RING_BYTES / 4] __attribute__((aligned(4096)));
static uint8_t  igpu_hws_page[4096] __attribute__((aligned(4096)));
static uint8_t  igpu_status_page[4096] __attribute__((aligned(4096)));
static uint32_t igpu_target[IGPU_TEST_W * IGPU_TEST_H] __attribute__((aligned(4096)));

/* ===== CRASH RECORD (filled when a submission hangs) ===== */

static struct {
    int      valid;
    int      hang_count;
    int      reset_ok;
    uint32_t seqno_expected, seqno_seen;
    uint32_t eir, esr, instdone;
    uint32_t ipeir, ipehr;              /* the offending command header */
    uint32_t acthd_lo, acthd_hi;
    uint32_t ring_head, ring_tail, ring_ctl, ring_start;
    uint32_t fault_reg, fault_tlb0, fault_tlb1;
    uint32_t hws[8];                    /* HWS page, first dwords       */
    uint32_t ring_window[8];            /* ring stream ending at ACTHD  */
    char     cmd_name[48];              /* decoded IPEHR                */
} igpu_crash;

static void igpu_log(const char *msg) {
    serial_puts("[igpu] ");
    serial_puts(msg);
    serial_putc('\n');
}

static void igpu_log_reg(const char *name, uint32_t val) {
    serial_puts("[igpu]   ");
    serial_puts(name);
    serial_puts(" = 0x");
    serial_put_hex32(val);
    serial_putc('\n');
}

/* ===== COMMAND HEADER DECODER =====
 * Classifies a ring dword the way i915's error-state dump does, so a
 * hang report can say "XY_COLOR_BLT" instead of a bare hex number. */

static void igpu_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void igpu_decode_cmd(uint32_t dw, char *out, int max) {
    uint32_t type = dw >> 29;

    if (dw == 0) { igpu_strcpy(out, "MI_NOOP", max); return; }

    if (type == 0) {                       /* MI command, opcode 28:23 */
        uint32_t op = (dw >> 23) & 0x3F;
        const char *n = 0;
        switch (op) {
        case 0x00: n = "MI_NOOP"; break;
        case 0x02: n = "MI_USER_INTERRUPT"; break;
        case 0x04: n = "MI_FLUSH"; break;
        case 0x07: n = "MI_REPORT_HEAD"; break;
        case 0x0A: n = "MI_BATCH_BUFFER_END"; break;
        case 0x1C: n = "MI_SEMAPHORE_WAIT"; break;
        case 0x20: n = "MI_STORE_DWORD_IMM"; break;
        case 0x22: n = "MI_LOAD_REGISTER_IMM"; break;
        case 0x24: n = "MI_STORE_REGISTER_MEM"; break;
        case 0x26: n = "MI_FLUSH_DW"; break;
        case 0x31: n = "MI_BATCH_BUFFER_START"; break;
        default: break;
        }
        if (n) igpu_strcpy(out, n, max);
        else   igpu_strcpy(out, "MI (unknown opcode)", max);
        return;
    }

    if (type == 2) {                       /* 2D command, opcode 28:22 */
        uint32_t op = (dw >> 22) & 0x7F;
        const char *n = 0;
        switch (op) {
        case 0x40: n = "COLOR_BLT"; break;
        case 0x43: n = "SRC_COPY_BLT"; break;
        case 0x50: n = "XY_COLOR_BLT"; break;
        case 0x51: n = "XY_PAT_BLT"; break;
        case 0x53: n = "XY_SRC_COPY_BLT"; break;
        case 0x55: n = "XY_FULL_BLT"; break;
        default: break;
        }
        if (n) igpu_strcpy(out, n, max);
        else   igpu_strcpy(out, "2D blitter (unknown opcode)", max);
        return;
    }

    if (type == 3) {
        igpu_strcpy(out, "3D/media pipeline command", max);
        return;
    }
    igpu_strcpy(out, "unrecognized command", max);
}

static inline uint32_t igpu_read(uint32_t reg) {
    return *(volatile uint32_t *)(igpu.mmio + reg);
}

static inline void igpu_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(igpu.mmio + reg) = val;
}

/* masked register write: high 16 bits select which low bits to change */
static inline void igpu_write_masked(uint32_t reg, uint32_t mask, uint32_t val) {
    igpu_write(reg, (mask << 16) | val);
}

/* ===== GGTT ===== */

/* gen8+ GGTT PTE: phys | present | RW (i915 gen8_ggtt_pte_encode) */
static void igpu_ggtt_set(uint32_t slot, uint64_t phys) {
    *(volatile uint64_t *)(igpu.ggtt + (uint64_t)slot * 8) =
        phys | 0x3ULL;
}

static void igpu_ggtt_flush(void) {
    (void)*(volatile uint64_t *)(igpu.ggtt);        /* posting read */
    igpu_write(IGPU_GFX_FLSH_CNTL, 1);              /* GFX_FLSH_CNTL_EN */
}

/* ===== FORCEWAKE (keep the blitter domain awake around MMIO) ===== */

static int igpu_forcewake_get(void) {
    igpu_write(IGPU_FORCEWAKE_BLT,
               (IGPU_FW_KERNEL_BIT << 16) | IGPU_FW_KERNEL_BIT);
    for (int i = 0; i < 1000000; i++) {
        if (igpu_read(IGPU_FORCEWAKE_BLT_ACK) & IGPU_FW_KERNEL_BIT)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void igpu_forcewake_put(void) {
    igpu_write(IGPU_FORCEWAKE_BLT, IGPU_FW_KERNEL_BIT << 16);
}

/* ===== RING SETUP (used at init and after an engine reset) ===== */

static int igpu_ring_setup(void) {
    /* legacy (non-execlist) submission mode */
    igpu_write_masked(IGPU_BCS_BASE + IGPU_ENG_GFX_MODE,
                      IGPU_GFX_MODE_EXECLIST, 0);
    igpu_write_masked(IGPU_BCS_BASE + IGPU_ENG_MI_MODE,
                      IGPU_MI_MODE_STOP_RING, IGPU_MI_MODE_STOP_RING);

    igpu_write(IGPU_BLT_HWS_PGA, igpu.hws_gpu);

    igpu_write(IGPU_BCS_BASE + IGPU_RING_CTL, 0);
    igpu_write(IGPU_BCS_BASE + IGPU_RING_HEAD, 0);
    igpu_write(IGPU_BCS_BASE + IGPU_RING_TAIL, 0);
    igpu_write(IGPU_BCS_BASE + IGPU_RING_START, igpu.ring_gpu);
    igpu_write(IGPU_BCS_BASE + IGPU_RING_CTL,
               ((IGPU_RING_PAGES - 1) << 12) | IGPU_RING_CTL_VALID);

    igpu_write_masked(IGPU_BCS_BASE + IGPU_ENG_MI_MODE,
                      IGPU_MI_MODE_STOP_RING, 0);

    if (!(igpu_read(IGPU_BCS_BASE + IGPU_RING_CTL) & IGPU_RING_CTL_VALID))
        return -1;
    igpu.ring_tail = 0;
    return 0;
}

/* ===== HANG CAPTURE + ENGINE RESET =====
 * Modeled on i915's error-state capture: latch every register that
 * describes what the command streamer was doing when it died, decode
 * the offending packet, then try a blitter-domain GDRST. */

static void igpu_capture_error(void) {
    igpu_crash.valid = 1;
    igpu_crash.hang_count++;
    igpu_crash.seqno_expected = igpu.seqno;
    igpu_crash.seqno_seen =
        ((volatile uint32_t *)(igpu_status_page + 0x40))[0];

    igpu_crash.eir      = igpu_read(IGPU_EIR);
    igpu_crash.esr      = igpu_read(IGPU_ESR);
    igpu_crash.ipeir    = igpu_read(IGPU_BCS_BASE + IGPU_ENG_IPEIR);
    igpu_crash.ipehr    = igpu_read(IGPU_BCS_BASE + IGPU_ENG_IPEHR);
    igpu_crash.instdone = igpu_read(IGPU_BCS_BASE + IGPU_ENG_INSTDONE);
    igpu_crash.acthd_lo = igpu_read(IGPU_BCS_BASE + IGPU_ENG_ACTHD);
    igpu_crash.acthd_hi = igpu_read(IGPU_BCS_BASE + IGPU_ENG_ACTHD_UDW);
    igpu_crash.ring_head  = igpu_read(IGPU_BCS_BASE + IGPU_RING_HEAD);
    igpu_crash.ring_tail  = igpu_read(IGPU_BCS_BASE + IGPU_RING_TAIL);
    igpu_crash.ring_ctl   = igpu_read(IGPU_BCS_BASE + IGPU_RING_CTL);
    igpu_crash.ring_start = igpu_read(IGPU_BCS_BASE + IGPU_RING_START);
    igpu_crash.fault_reg  = igpu_read(IGPU_RING_FAULT_REG);
    igpu_crash.fault_tlb0 = igpu_read(IGPU_FAULT_TLB_DATA0);
    igpu_crash.fault_tlb1 = igpu_read(IGPU_FAULT_TLB_DATA1);

    /* HWS page snapshot (head reports / engine scratch) */
    for (int i = 0; i < 8; i++)
        igpu_crash.hws[i] = ((volatile uint32_t *)igpu_hws_page)[i];

    /* the ring stream leading up to the parser's active head */
    uint32_t head_off = (igpu_crash.acthd_lo - igpu.ring_gpu) &
                        (IGPU_RING_BYTES - 1);
    for (int i = 0; i < 8; i++) {
        uint32_t off = (head_off + IGPU_RING_BYTES - (7 - i) * 4 - 4) &
                       (IGPU_RING_BYTES - 1);
        igpu_crash.ring_window[i] = igpu_ring_mem[off / 4];
    }

    igpu_decode_cmd(igpu_crash.ipehr, igpu_crash.cmd_name,
                    sizeof(igpu_crash.cmd_name));

    /* EIR is write-1-to-clear — ack what we just captured */
    if (igpu_crash.eir)
        igpu_write(IGPU_EIR, igpu_crash.eir);

    serial_puts("[igpu] *** GPU HANG on BCS ***\n");
    serial_puts("[igpu]   parser died in: ");
    serial_puts(igpu_crash.cmd_name);
    serial_putc('\n');
    igpu_log_reg("IPEHR (bad cmd) ", igpu_crash.ipehr);
    igpu_log_reg("IPEIR           ", igpu_crash.ipeir);
    igpu_log_reg("EIR             ", igpu_crash.eir);
    if (igpu_crash.eir & IGPU_ERR_INSTRUCTION)
        igpu_log("  EIR: invalid instruction error");
    if (igpu_crash.eir & IGPU_ERR_PAGE_TABLE)
        igpu_log("  EIR: page table error");
    if (igpu_crash.eir & IGPU_ERR_MEM_REFRESH)
        igpu_log("  EIR: memory refresh error");
    if (igpu_crash.eir & IGPU_ERR_PRIV)
        igpu_log("  EIR: privilege violation");
    igpu_log_reg("ACTHD           ", igpu_crash.acthd_lo);
    igpu_log_reg("RING_HEAD       ", igpu_crash.ring_head);
    igpu_log_reg("RING_TAIL       ", igpu_crash.ring_tail);
    igpu_log_reg("INSTDONE        ", igpu_crash.instdone);
    if (igpu_crash.fault_reg & 1) {
        igpu_log("  GGTT fault (RING_FAULT_REG valid):");
        igpu_log_reg("fault vaddr page", igpu_crash.fault_reg & 0xFFFFF000);
        igpu_log_reg("fault raw       ", igpu_crash.fault_reg);
    }
    igpu_log_reg("HWS[0]          ", igpu_crash.hws[0]);
    igpu_log_reg("breadcrumb seen ", igpu_crash.seqno_seen);
    igpu_log_reg("breadcrumb want ", igpu_crash.seqno_expected);
    serial_puts("[igpu]   ring at ACTHD:");
    for (int i = 0; i < 8; i++) {
        serial_puts(" ");
        serial_put_hex32(igpu_crash.ring_window[i]);
    }
    serial_putc('\n');
}

static int igpu_engine_reset(void) {
    igpu_write(IGPU_GDRST, IGPU_GRDOM_BLT);
    int cleared = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!(igpu_read(IGPU_GDRST) & IGPU_GRDOM_BLT)) {
            cleared = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!cleared) {
        igpu_log("engine reset: GDRST never cleared");
        return -1;
    }
    if (igpu_ring_setup() != 0) {
        igpu_log("engine reset: ring did not come back");
        return -1;
    }
    igpu_log("BCS engine reset + ring re-init OK");
    return 0;
}

/* ===== RING SUBMISSION ===== */

static void igpu_ring_emit(const uint32_t *dw, int n) {
    for (int i = 0; i < n; i++) {
        igpu_ring_mem[igpu.ring_tail / 4] = dw[i];
        igpu.ring_tail = (igpu.ring_tail + 4) % IGPU_RING_BYTES;
    }
}

/* Submit a command batch followed by flush + seqno breadcrumb, then
 * wait for the breadcrumb to land in the status page. */
static int igpu_exec(const uint32_t *cmds, int ndw) {
    /* wrap early if the batch + breadcrumb wouldn't fit contiguously */
    uint32_t need = (uint32_t)(ndw + 12) * 4;
    if (igpu.ring_tail + need >= IGPU_RING_BYTES) {
        while (igpu.ring_tail != 0) {
            igpu_ring_mem[igpu.ring_tail / 4] = MI_NOOP_CMD;
            igpu.ring_tail = (igpu.ring_tail + 4) % IGPU_RING_BYTES;
        }
    }

    igpu_ring_emit(cmds, ndw);

    igpu.seqno++;
    uint32_t tail_cmds[9] = {
        MI_FLUSH_DW_CMD, 0, 0, 0,
        MI_STORE_DWORD_IMM_CMD,
        igpu.status_gpu + 0x40, 0,
        igpu.seqno,
        MI_NOOP_CMD,                       /* keep tail qword-aligned */
    };
    int tail_n = ((ndw + 8) & 1) ? 9 : 8;
    igpu_ring_emit(tail_cmds, tail_n);

    __asm__ volatile("sfence" ::: "memory");
    igpu_write(IGPU_BCS_BASE + IGPU_RING_TAIL, igpu.ring_tail);

    volatile uint32_t *brk = (volatile uint32_t *)(igpu_status_page + 0x40);
    for (int i = 0; i < 20000000; i++) {
        if (*brk == igpu.seqno)
            return 0;
        __asm__ volatile("pause");
    }

    /* ---- the breadcrumb never landed: this is a GPU hang ---- */
    igpu_capture_error();

    if (igpu_crash.hang_count >= 3) {
        igpu.active = 0;
        igpu.status = "disabled after repeated hangs - CPU renderer";
        igpu_log("too many hangs - blitter disabled, CPU renderer active");
        return -1;
    }
    igpu_crash.reset_ok = (igpu_engine_reset() == 0);
    if (igpu_crash.reset_ok) {
        igpu.status = "active - recovered from GPU hang (see 'gpu error')";
    } else {
        igpu.active = 0;
        igpu.status = "hung and reset failed - CPU renderer";
    }
    return -1;
}

/*
 * ===== COMMAND BUFFERS =====
 *
 * Every operation used to be its own submission: write the ring, ring
 * the doorbell, then spin on a breadcrumb until the engine had finished.
 * That round trip costs far more than the blit for anything small, and
 * it is paid once per rectangle — so a compositor that wanted to hand
 * the blitter fifty rectangles would wait fifty times, and be slower
 * than doing the work on the processor.
 *
 * A command buffer is filled with as many operations as the caller has
 * and submitted once. The engine executes them back to back and the
 * breadcrumb lands after the last one, so fifty rectangles cost one
 * wait. That is the whole of the idea and it is where nearly all of the
 * available speed is.
 *
 * Overflow is recorded rather than wrapped: silently dropping the tail
 * of a batch would produce a frame that is subtly wrong and never say
 * so, which is much worse than a batch that refuses and falls back to
 * the processor.
 */
#define IGPU_CB_DWORDS 512

typedef struct {
    uint32_t dw[IGPU_CB_DWORDS];
    int      n;          /* dwords used   */
    int      ops;        /* operations in */
    int      overflow;
} igpu_cb_t;

static void igpu_cb_reset(igpu_cb_t *cb) {
    cb->n = 0;
    cb->ops = 0;
    cb->overflow = 0;
}

static uint32_t *igpu_cb_room(igpu_cb_t *cb, int need) {
    if (cb->overflow || cb->n + need > IGPU_CB_DWORDS) {
        cb->overflow = 1;
        return 0;
    }
    uint32_t *p = cb->dw + cb->n;
    cb->n += need;
    cb->ops++;
    return p;
}

/* Fill a rectangle in a GGTT-addressed 32bpp surface. */
static int igpu_cb_fill(igpu_cb_t *cb, uint32_t dst_gpu, uint32_t pitch_bytes,
                        int x1, int y1, int x2, int y2, uint32_t color) {
    uint32_t *p = igpu_cb_room(cb, 7);
    if (!p) return -1;
    p[0] = XY_COLOR_BLT_CMD;
    p[1] = BLT_DEPTH_32 | BLT_ROP_PATCOPY | (pitch_bytes & 0xFFFF);
    p[2] = ((uint32_t)y1 << 16) | (uint32_t)x1;
    p[3] = ((uint32_t)y2 << 16) | (uint32_t)x2;
    p[4] = dst_gpu;
    p[5] = 0;
    p[6] = color;
    return 0;
}

/* Copy a rectangle from one GGTT-addressed 32bpp surface to another.
 * The rectangle is given as a destination position and a size, with the
 * source origin separate, which is how the hardware wants it. */
static int igpu_cb_copy(igpu_cb_t *cb,
                        uint32_t dst_gpu, uint32_t dst_pitch, int dx, int dy,
                        uint32_t src_gpu, uint32_t src_pitch, int sx, int sy,
                        int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    uint32_t *p = igpu_cb_room(cb, 10);
    if (!p) return -1;
    p[0] = XY_SRC_COPY_BLT_CMD;
    p[1] = BLT_DEPTH_32 | BLT_ROP_SRCCOPY | (dst_pitch & 0xFFFF);
    p[2] = ((uint32_t)dy << 16) | (uint32_t)dx;
    p[3] = ((uint32_t)(dy + h) << 16) | (uint32_t)(dx + w);
    p[4] = dst_gpu;
    p[5] = 0;
    p[6] = ((uint32_t)sy << 16) | (uint32_t)sx;
    p[7] = src_pitch & 0xFFFF;
    p[8] = src_gpu;
    p[9] = 0;
    return 0;
}

static int igpu_cb_submit(igpu_cb_t *cb) {
    if (cb->overflow) {
        igpu_log("command buffer overflowed - batch refused");
        return -1;
    }
    if (cb->n == 0) return 0;
    return igpu_exec(cb->dw, cb->n);
}

/* One-shot fill, for the callers that only ever have one. */
static int igpu_blt_fill(uint32_t dst_gpu, uint32_t pitch_bytes,
                         int x1, int y1, int x2, int y2, uint32_t color) {
    static igpu_cb_t cb;
    igpu_cb_reset(&cb);
    if (igpu_cb_fill(&cb, dst_gpu, pitch_bytes, x1, y1, x2, y2, color) != 0)
        return -1;
    return igpu_cb_submit(&cb);
}

/* ===== PUBLIC: batched work on the visible framebuffer ===== */

static igpu_cb_t igpu_frame_cb;

static void igpu_batch_begin(void) { igpu_cb_reset(&igpu_frame_cb); }

static int igpu_batch_fill(int x, int y, int w, int h, uint32_t color) {
    if (!igpu.active || !igpu.fb_blittable) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if ((uint32_t)(x + w) > igpu.fb_w) w = (int)igpu.fb_w - x;
    if ((uint32_t)(y + h) > igpu.fb_h) h = (int)igpu.fb_h - y;
    if (w <= 0 || h <= 0) return -1;
    return igpu_cb_fill(&igpu_frame_cb, igpu.fb_gpu_addr, igpu.fb_pitch_bytes,
                        x, y, x + w, y + h, color);
}

/* Move a region of the screen. Scrolling a list and dragging a window
 * are both this, and both are pure memory traffic the processor should
 * not be doing. */
static int igpu_batch_move(int sx, int sy, int dx, int dy, int w, int h) {
    if (!igpu.active || !igpu.fb_blittable) return -1;
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0) return -1;
    if ((uint32_t)(sx + w) > igpu.fb_w || (uint32_t)(dx + w) > igpu.fb_w)
        return -1;
    if ((uint32_t)(sy + h) > igpu.fb_h || (uint32_t)(dy + h) > igpu.fb_h)
        return -1;
    return igpu_cb_copy(&igpu_frame_cb,
                        igpu.fb_gpu_addr, igpu.fb_pitch_bytes, dx, dy,
                        igpu.fb_gpu_addr, igpu.fb_pitch_bytes, sx, sy, w, h);
}

static int igpu_batch_submit(void) {
    if (!igpu.active) return -1;
    if (igpu_frame_cb.n == 0) return 0;
    if (igpu_forcewake_get() != 0) return -1;
    int rc = igpu_cb_submit(&igpu_frame_cb);
    igpu_forcewake_put();
    igpu_cb_reset(&igpu_frame_cb);
    return rc;
}

static int igpu_batch_ops(void) { return igpu_frame_cb.ops; }

/* ===== PUBLIC: fill on the visible framebuffer (real hardware) ===== */

static int igpu_screen_fill(int x, int y, int w, int h, uint32_t color) {
    if (!igpu.active || !igpu.fb_blittable) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if ((uint32_t)(x + w) > igpu.fb_w) w = (int)igpu.fb_w - x;
    if ((uint32_t)(y + h) > igpu.fb_h) h = (int)igpu.fb_h - y;
    if (igpu_forcewake_get() != 0) return -1;
    int rc = igpu_blt_fill(igpu.fb_gpu_addr, igpu.fb_pitch_bytes,
                           x, y, x + w, y + h, color);
    igpu_forcewake_put();
    return rc;
}

/* ===========================================================
 * MEDIA COMMAND STREAMERS (VCS / VECS)
 * ===========================================================
 *
 * The blitter above is one of four command streamers on a Gen9 part.
 * The other three that matter here are:
 *
 *   VCS   0x12000   video decode/encode -- the MFX fixed-function
 *                   block, which is what "Quick Sync" names
 *   VCS2  0x1C000   a second video engine on some GT3/GT4 parts
 *   VECS  0x1A000   the video enhancement box: colour space
 *                   conversion, denoise, deinterlace -- "Clear Video"
 *
 * They are programmed exactly like the BCS: a ring buffer in memory,
 * a GGTT mapping for it, head/tail/start/ctl registers at the engine's
 * base, and a breadcrumb written by the engine when it has drained the
 * ring. What differs is the forcewake domain -- the media wells are
 * gated separately from the blitter, and MMIO to a sleeping well reads
 * back zeroes and drops writes without faulting, which looks exactly
 * like a dead engine.
 *
 * ---- why this is a separate structure and BCS was left alone ----
 *
 * The blitter path above is verified at boot by a self-test that reads
 * back every pixel of a GPU-filled tile. Retrofitting it onto this
 * abstraction to save a hundred lines would put the one graphics path
 * that is known to work on this machine through an untested rewrite,
 * to no benefit. The new engines get the general form; BCS keeps the
 * one that has been proven.
 */

#define IGPU_VCS_BASE           0x12000
#define IGPU_VCS2_BASE          0x1C000
#define IGPU_VECS_BASE          0x1A000

/* Gen9 forcewake domains (i915 intel_uncore.c) */
#define IGPU_FORCEWAKE_MEDIA    0xA270
#define IGPU_FW_ACK_MEDIA       0x0D88

#define IGPU_ENG_RING_PAGES     4
#define IGPU_ENG_RING_BYTES     (IGPU_ENG_RING_PAGES * 4096)

typedef struct {
    const char *name;
    uint32_t    base;            /* command streamer MMIO base        */
    uint32_t    fw_reg, fw_ack;  /* its forcewake domain              */

    uint32_t   *ring;            /* CPU view of the ring pages        */
    uint32_t    ring_gpu;        /* and the GPU address of the same   */
    uint32_t    tail;            /* byte offset of the next dword     */

    uint8_t    *hws;             /* hardware status page              */
    uint32_t    hws_gpu;
    volatile uint32_t *status;   /* breadcrumb page                   */
    uint32_t    status_gpu;

    uint32_t    seqno;
    int         ready;
    uint32_t    hangs;
} igpu_engine_t;

static int igpu_engine_fw_get(const igpu_engine_t *e) {
    igpu_write(e->fw_reg, (IGPU_FW_KERNEL_BIT << 16) | IGPU_FW_KERNEL_BIT);
    for (int i = 0; i < 1000000; i++) {
        if (igpu_read(e->fw_ack) & IGPU_FW_KERNEL_BIT) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void igpu_engine_fw_put(const igpu_engine_t *e) {
    igpu_write(e->fw_reg, IGPU_FW_KERNEL_BIT << 16);
}

/*
 * Bring an engine's ring up in legacy (non-execlist) mode.
 *
 * Gen9 still honours the legacy tail-pointer doorbell on every engine,
 * which is what makes this tractable without a GuC: execlist submission
 * would need a context image, a logical ring context descriptor and a
 * hardware status page layout for each engine, and none of that buys
 * anything for a single-client decoder.
 */
static int igpu_engine_ring_setup(igpu_engine_t *e) {
    igpu_write_masked(e->base + IGPU_ENG_GFX_MODE, IGPU_GFX_MODE_EXECLIST, 0);
    igpu_write_masked(e->base + IGPU_ENG_MI_MODE,
                      IGPU_MI_MODE_STOP_RING, IGPU_MI_MODE_STOP_RING);

    igpu_write(e->base + IGPU_RING_CTL, 0);
    igpu_write(e->base + IGPU_RING_HEAD, 0);
    igpu_write(e->base + IGPU_RING_TAIL, 0);
    igpu_write(e->base + IGPU_RING_START, e->ring_gpu);
    igpu_write(e->base + IGPU_RING_CTL,
               ((IGPU_ENG_RING_PAGES - 1) << 12) | IGPU_RING_CTL_VALID);

    igpu_write_masked(e->base + IGPU_ENG_MI_MODE, IGPU_MI_MODE_STOP_RING, 0);

    if (!(igpu_read(e->base + IGPU_RING_CTL) & IGPU_RING_CTL_VALID))
        return -1;

    e->tail  = 0;
    e->seqno = 0;
    return 0;
}

static void igpu_engine_emit(igpu_engine_t *e, const uint32_t *dw, int n) {
    for (int i = 0; i < n; i++) {
        e->ring[e->tail / 4] = dw[i];
        e->tail = (e->tail + 4) % IGPU_ENG_RING_BYTES;
    }
}

/*
 * Submit a command stream and wait for it to retire.
 *
 * The breadcrumb is an MI_STORE_DWORD_IMM into the engine's own status
 * page, exactly as on the blitter. A decode is not fast -- a 1080p
 * frame is a few milliseconds of fixed-function work -- so the timeout
 * here is generous, but it is still bounded: a media engine that has
 * faulted stops retiring and never says so, and an unbounded spin in a
 * kernel thread takes the desktop with it.
 */
static int igpu_engine_exec(igpu_engine_t *e, const uint32_t *cmds, int ndw) {
    uint32_t need = (uint32_t)(ndw + 12) * 4;

    if (!e->ready) return -1;

    if (e->tail + need >= IGPU_ENG_RING_BYTES) {
        while (e->tail != 0) {
            e->ring[e->tail / 4] = MI_NOOP_CMD;
            e->tail = (e->tail + 4) % IGPU_ENG_RING_BYTES;
        }
    }

    igpu_engine_emit(e, cmds, ndw);

    e->seqno++;
    {
        uint32_t tail_cmds[9] = {
            MI_FLUSH_DW_CMD, 0, 0, 0,
            MI_STORE_DWORD_IMM_CMD,
            e->status_gpu + 0x40, 0,
            e->seqno,
            MI_NOOP_CMD,
        };
        int tail_n = ((ndw + 8) & 1) ? 9 : 8;
        igpu_engine_emit(e, tail_cmds, tail_n);
    }

    __asm__ volatile("sfence" ::: "memory");
    igpu_write(e->base + IGPU_RING_TAIL, e->tail);

    for (int i = 0; i < 40000000; i++) {
        if (e->status[0x40 / 4] == e->seqno) return 0;
        __asm__ volatile("pause");
    }

    e->hangs++;
    serial_puts("[igpu] *** hang on ");
    serial_puts(e->name);
    serial_puts(" ***\n");
    igpu_log_reg("  IPEHR ", igpu_read(e->base + IGPU_ENG_IPEHR));
    igpu_log_reg("  ACTHD ", igpu_read(e->base + IGPU_ENG_ACTHD));
    igpu_log_reg("  HEAD  ", igpu_read(e->base + IGPU_RING_HEAD));
    igpu_log_reg("  TAIL  ", igpu_read(e->base + IGPU_RING_TAIL));

    if (e->hangs >= 3) {
        e->ready = 0;
        igpu_log("  engine disabled after repeated hangs");
    }
    return -1;
}

/* ===========================================================
 * A GGTT window for media surfaces
 * ===========================================================
 *
 * The blitter's window is 64 pages, which is ample for a ring, a status
 * page and a 64x64 test tile. A decoder is a different shape: one 1080p
 * NV12 surface is 3 MB, and H.264 allows sixteen reference frames plus
 * the one being decoded, so a decoded picture buffer is tens of
 * megabytes and every byte of it has to be reachable by the engine.
 *
 * So the media window is its own region, allocated *below* the
 * blitter's rather than by growing it -- the blitter's base slot is
 * baked into a self-test that passes, and moving it to make room would
 * risk the working path to benefit the new one.
 *
 * Allocation is a bump pointer with no free. A decoder allocates its
 * surfaces once when the stream's dimensions become known and holds
 * them until it is torn down; a general-purpose GPU virtual address
 * allocator would be more code than the thing it serves.
 */

#define IGPU_MEDIA_SLOTS    16384        /* 64 MB of GPU address space */

static struct {
    uint32_t base_slot;
    uint32_t next_slot;
    uint32_t limit_slot;
    int      ready;
} igpu_media_gtt;

/*
 * Map a physically contiguous run into the media window and return the
 * GPU address it now answers to.
 *
 * Returns 0 on exhaustion rather than wrapping, because a surface that
 * silently aliased another would decode into the wrong picture and
 * produce corruption that looks like a codec bug.
 */
static uint32_t igpu_media_map(uint64_t phys, uint32_t pages) {
    uint32_t slot, gpu;

    if (!igpu_media_gtt.ready) return 0;
    if (igpu_media_gtt.next_slot + pages > igpu_media_gtt.limit_slot) return 0;

    slot = igpu_media_gtt.next_slot;
    for (uint32_t i = 0; i < pages; i++)
        igpu_ggtt_set(slot + i, phys + (uint64_t)i * 4096);

    igpu_media_gtt.next_slot += pages;
    gpu = slot * 4096;
    igpu_ggtt_flush();
    return gpu;
}

/* Map a run of pages that are contiguous in kernel virtual space but
 * not necessarily in physical space -- which is what a large static
 * kernel array is, and what the window canvas is. */
static uint32_t igpu_media_map_virt(void *va, uint32_t bytes) {
    uint32_t pages = (bytes + 4095) / 4096;
    uint32_t slot, gpu;

    if (!igpu_media_gtt.ready) return 0;
    if (igpu_media_gtt.next_slot + pages > igpu_media_gtt.limit_slot) return 0;

    slot = igpu_media_gtt.next_slot;
    for (uint32_t i = 0; i < pages; i++)
        igpu_ggtt_set(slot + i,
                      kern_virt_to_phys((uint8_t *)va + (uint64_t)i * 4096));

    igpu_media_gtt.next_slot += pages;
    gpu = slot * 4096;
    igpu_ggtt_flush();
    return gpu;
}

/* ===== the two media engines ===== */

static uint32_t igpu_vcs_ring[IGPU_ENG_RING_BYTES / 4]
                    __attribute__((aligned(4096)));
static uint8_t  igpu_vcs_hws[4096]    __attribute__((aligned(4096)));
static uint8_t  igpu_vcs_status[4096] __attribute__((aligned(4096)));

static uint32_t igpu_vecs_ring[IGPU_ENG_RING_BYTES / 4]
                    __attribute__((aligned(4096)));
static uint8_t  igpu_vecs_hws[4096]    __attribute__((aligned(4096)));
static uint8_t  igpu_vecs_status[4096] __attribute__((aligned(4096)));

static igpu_engine_t igpu_vcs = {
    "VCS (video decode)", IGPU_VCS_BASE,
    IGPU_FORCEWAKE_MEDIA, IGPU_FW_ACK_MEDIA,
    igpu_vcs_ring, 0, 0,
    igpu_vcs_hws, 0, (volatile uint32_t *)igpu_vcs_status, 0,
    0, 0, 0
};

static igpu_engine_t igpu_vecs = {
    "VECS (video enhance)", IGPU_VECS_BASE,
    IGPU_FORCEWAKE_MEDIA, IGPU_FW_ACK_MEDIA,
    igpu_vecs_ring, 0, 0,
    igpu_vecs_hws, 0, (volatile uint32_t *)igpu_vecs_status, 0,
    0, 0, 0
};

/*
 * Bring the media engines up.
 *
 * Called after the blitter is known good, because everything here
 * depends on the GGTT and the MMIO mapping that step established. A
 * failure is not fatal to anything: the blitter keeps working, the
 * desktop keeps compositing, and the decoder reports that it has no
 * hardware rather than producing a picture some other way and calling
 * it accelerated.
 */
static int igpu_media_init(void) {
    if (!igpu.active || !igpu.mmio) return -1;

    /* The media window sits directly below the blitter's. The bound is
     * checked before the subtraction, not after: base_slot is unsigned,
     * so a GGTT smaller than the window would wrap to an enormous slot
     * number and the first mapping would write PTEs past the end of the
     * aperture. */
    if (igpu.base_slot < IGPU_MEDIA_SLOTS) {
        igpu_media_gtt.ready = 0;
        igpu_log("GGTT too small for a media window");
        return -1;
    }

    igpu_media_gtt.base_slot  = igpu.base_slot - IGPU_MEDIA_SLOTS;
    igpu_media_gtt.next_slot  = igpu_media_gtt.base_slot;
    igpu_media_gtt.limit_slot = igpu.base_slot;
    igpu_media_gtt.ready      = 1;

    if (igpu_engine_fw_get(&igpu_vcs) != 0) {
        igpu_log("media forcewake ack timeout - no video engines");
        igpu_media_gtt.ready = 0;
        return -1;
    }

    /* Ring, status and hardware status pages for each engine. */
    igpu_vcs.ring_gpu = igpu_media_map_virt(igpu_vcs_ring,
                                            IGPU_ENG_RING_BYTES);
    igpu_vcs.hws_gpu    = igpu_media_map_virt(igpu_vcs_hws, 4096);
    igpu_vcs.status_gpu = igpu_media_map_virt(igpu_vcs_status, 4096);

    igpu_vecs.ring_gpu = igpu_media_map_virt(igpu_vecs_ring,
                                             IGPU_ENG_RING_BYTES);
    igpu_vecs.hws_gpu    = igpu_media_map_virt(igpu_vecs_hws, 4096);
    igpu_vecs.status_gpu = igpu_media_map_virt(igpu_vecs_status, 4096);

    if (!igpu_vcs.ring_gpu || !igpu_vecs.ring_gpu) {
        igpu_engine_fw_put(&igpu_vcs);
        igpu_media_gtt.ready = 0;
        igpu_log("could not map media rings into the GGTT");
        return -1;
    }

    for (int i = 0; i < 1024; i++) {
        ((volatile uint32_t *)igpu_vcs_status)[i]  = 0;
        ((volatile uint32_t *)igpu_vecs_status)[i] = 0;
    }
    __asm__ volatile("mfence" ::: "memory");

    if (igpu_engine_ring_setup(&igpu_vcs) == 0) {
        igpu_vcs.ready = 1;
        igpu_log("VCS ring enabled (MFX video decode)");
    } else {
        igpu_log("VCS ring did not report valid");
    }

    if (igpu_engine_ring_setup(&igpu_vecs) == 0) {
        igpu_vecs.ready = 1;
        igpu_log("VECS ring enabled (colour space conversion)");
    } else {
        igpu_log("VECS ring did not report valid");
    }

    igpu_engine_fw_put(&igpu_vcs);
    return (igpu_vcs.ready || igpu_vecs.ready) ? 0 : -1;
}

/* ===== INITIALIZATION ===== */

static void igpu_fail(const char *why) {
    igpu.status = why;
    igpu.active = 0;
    igpu_log(why);
    if (igpu.mmio) {
        /* stop the ring and drop forcewake — leave hw as we found it */
        igpu_write(IGPU_BCS_BASE + IGPU_RING_CTL, 0);
        igpu_forcewake_put();
    }
}

static void igpu_init(uint64_t fb_phys, uint32_t fb_w, uint32_t fb_h,
                      uint32_t fb_pitch_px) {
    igpu.active = 0;
    igpu.status = "no Intel iGPU found";

    /* ---- probe: Intel display-class device with a known Gen9 ID ---- */
    pci_dev_t dev;
    if (!pci_find_class(0xFF0000, 0x030000, 0x8086, &dev)) {
        igpu_log("no Intel display controller on PCI bus");
        return;
    }
    igpu.device_id = dev.device;
    igpu.name = 0;
    for (int i = 0; igpu_known[i].id; i++)
        if (igpu_known[i].id == dev.device) {
            igpu.name = igpu_known[i].name;
            break;
        }
    if (!igpu.name) {
        igpu.status = "Intel iGPU present but not Gen9 - CPU rendering";
        serial_puts("[igpu] unsupported Intel device 8086:");
        serial_put_hex32(dev.device);
        serial_putc('\n');
        return;
    }
    serial_puts("[igpu] found ");
    serial_puts(igpu.name);
    serial_puts(" (8086:");
    serial_put_hex32(dev.device);
    serial_puts(")\n");

    pci_enable(&dev, PCI_CMD_MEM | PCI_CMD_MASTER);

    /* ---- BAR0: GTTMMADR (registers + GGTT in the upper half) ---- */
    uint64_t bar0, bar0_size;
    if (pci_bar(&dev, 0, &bar0, &bar0_size) != 0 || bar0_size < 0x200000) {
        igpu_fail("BAR0 missing or too small");
        return;
    }
    igpu.mmio = mmio_map(bar0, bar0_size);
    if (!igpu.mmio) {
        igpu_fail("BAR0 MMIO mapping failed");
        return;
    }
    igpu.mmio_size = bar0_size;
    igpu.ggtt = igpu.mmio + bar0_size / 2;
    igpu.ggtt_slots = (uint32_t)((bar0_size / 2) / 8);
    igpu_log("BAR0 mapped (registers + GGTT)");

    /* BAR2: GMADR aperture — lets us address the firmware framebuffer */
    if (pci_bar(&dev, 2, &igpu.aperture_base, &igpu.aperture_size) != 0) {
        igpu.aperture_base = 0;
        igpu.aperture_size = 0;
    }

    /* ---- forcewake the blitter power well ---- */
    if (igpu_forcewake_get() != 0) {
        igpu_fail("forcewake ack timeout");
        return;
    }
    igpu_log("blitter forcewake acquired");

    /* ---- private GGTT window at the very top of GPU address space
     * (far away from the firmware's stolen-memory scanout mappings) ---- */
    igpu.base_slot = igpu.ggtt_slots - 64;
    igpu.ring_gpu   = (igpu.base_slot + 0) * 4096;
    igpu.hws_gpu    = (igpu.base_slot + 8) * 4096;
    igpu.status_gpu = (igpu.base_slot + 9) * 4096;
    igpu.target_gpu = (igpu.base_slot + 16) * 4096;

    for (int p = 0; p < IGPU_RING_PAGES; p++)
        igpu_ggtt_set(igpu.base_slot + (uint32_t)p,
                      kern_virt_to_phys((uint8_t *)igpu_ring_mem + p * 4096));
    igpu_ggtt_set(igpu.base_slot + 8, kern_virt_to_phys(igpu_hws_page));
    igpu_ggtt_set(igpu.base_slot + 9, kern_virt_to_phys(igpu_status_page));
    for (int p = 0; p < 4; p++)
        igpu_ggtt_set(igpu.base_slot + 16 + (uint32_t)p,
                      kern_virt_to_phys((uint8_t *)igpu_target + p * 4096));
    igpu_ggtt_flush();
    igpu_log("GGTT window programmed");

    /* ---- BCS ring: legacy (non-execlist) submission mode ---- */
    if (igpu_ring_setup() != 0) {
        igpu_fail("ring did not report valid");
        return;
    }
    igpu.seqno = 0;
    igpu_log("BCS ring buffer enabled (legacy submission)");

    /* ---- self-test: fill a 64x64 tile and verify every pixel ---- */
    for (int i = 0; i < IGPU_TEST_W * IGPU_TEST_H; i++)
        igpu_target[i] = 0x11111111;
    for (int i = 0; i < 4096 / 4; i++)
        ((volatile uint32_t *)igpu_status_page)[i] = 0;
    __asm__ volatile("mfence" ::: "memory");

    if (igpu_blt_fill(igpu.target_gpu, IGPU_TEST_W * 4,
                      0, 0, IGPU_TEST_W, IGPU_TEST_H, 0x00C0FFEE) != 0) {
        igpu_fail("self-test: breadcrumb timeout");
        return;
    }

    /* make CPU reads see the GPU's writes regardless of PPAT config */
    for (int off = 0; off < IGPU_TEST_W * IGPU_TEST_H * 4; off += 64)
        __asm__ volatile("clflush (%0)" ::
                         "r"((uint8_t *)igpu_target + off) : "memory");
    __asm__ volatile("mfence" ::: "memory");

    for (int i = 0; i < IGPU_TEST_W * IGPU_TEST_H; i++) {
        if (igpu_target[i] != 0x00C0FFEE) {
            igpu_fail("self-test: pixel mismatch");
            return;
        }
    }
    igpu_log("self-test passed: XY_COLOR_BLT verified by CPU readback");

    /*
     * ---- second self-test: a batch, and a copy ----
     *
     * The point of the first test is that the engine runs at all. The
     * point of this one is that it runs several commands from a single
     * submission and that XY_SRC_COPY_BLT is encoded correctly -- which
     * is a claim about ten dwords of bit layout that no amount of
     * reading the specification settles as well as the hardware does.
     *
     * Four stripes are filled into the top half of the scratch surface
     * in one batch, then the top half is copied over the bottom. Every
     * pixel is checked, including that the stripes are in the right
     * places: a copy that transposed its source and destination
     * rectangles would still fill the surface, just wrongly.
     */
    {
        static const uint32_t stripe[4] = {
            0x00112233u, 0x00445566u, 0x00778899u, 0x00AABBCCu
        };
        for (int i = 0; i < IGPU_TEST_W * IGPU_TEST_H; i++)
            igpu_target[i] = 0x11111111;
        __asm__ volatile("mfence" ::: "memory");

        igpu_cb_t cb;
        igpu_cb_reset(&cb);
        for (int i = 0; i < 4; i++)
            igpu_cb_fill(&cb, igpu.target_gpu, IGPU_TEST_W * 4,
                         0, i * 8, IGPU_TEST_W, i * 8 + 8, stripe[i]);
        igpu_cb_copy(&cb,
                     igpu.target_gpu, IGPU_TEST_W * 4, 0, 32,
                     igpu.target_gpu, IGPU_TEST_W * 4, 0, 0,
                     IGPU_TEST_W, 32);

        if (igpu_cb_submit(&cb) != 0) {
            igpu_fail("batch self-test: breadcrumb timeout");
            return;
        }

        for (int off = 0; off < IGPU_TEST_W * IGPU_TEST_H * 4; off += 64)
            __asm__ volatile("clflush (%0)" ::
                             "r"((uint8_t *)igpu_target + off) : "memory");
        __asm__ volatile("mfence" ::: "memory");

        int bad = 0;
        for (int y = 0; y < IGPU_TEST_H && !bad; y++) {
            int band = (y % 32) / 8;
            uint32_t want = (y < 32 || y >= 64) ? stripe[band]
                          : (band < 4 ? stripe[band] : 0x11111111u);
            if (y >= 32) want = stripe[(y - 32) / 8];
            for (int x = 0; x < IGPU_TEST_W; x++)
                if (igpu_target[y * IGPU_TEST_W + x] != want) { bad = 1; break; }
        }
        if (bad) {
            igpu_fail("batch self-test: pixel mismatch");
            return;
        }
        igpu_log("self-test passed: 5-command batch and XY_SRC_COPY_BLT "
                 "verified by CPU readback");
    }

    /* ---- can the blitter reach the visible framebuffer? ----
     * On Intel-scanout machines the GOP framebuffer lives in the GMADR
     * aperture, i.e. it already has a GGTT mapping at (phys - GMADR). */
    igpu.fb_blittable = 0;
    if (igpu.aperture_size && fb_phys >= igpu.aperture_base &&
        fb_phys + (uint64_t)fb_pitch_px * fb_h * 4 <=
            igpu.aperture_base + igpu.aperture_size) {
        igpu.fb_gpu_addr = (uint32_t)(fb_phys - igpu.aperture_base);
        igpu.fb_phys = fb_phys;
        igpu.fb_pitch_bytes = fb_pitch_px * 4;
        igpu.fb_w = fb_w;
        igpu.fb_h = fb_h;
        igpu.fb_blittable = 1;
        igpu_log("framebuffer is in the GMADR aperture - screen blits OK");
    } else {
        igpu_log("framebuffer not GGTT-reachable - offscreen blits only");
    }

    igpu_forcewake_put();
    igpu.active = 1;
    igpu.status = "active - blitter self-test passed";

    /* The video engines, now that the GGTT and the MMIO window they
     * both depend on are known good. Failure here costs nothing that
     * was working a moment ago. */
    igpu_media_init();
}

#endif /* IGPU_H */
