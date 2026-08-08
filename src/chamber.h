#ifndef VEXTRO_CHAMBER_H
#define VEXTRO_CHAMBER_H

#include "hyper.h"

/*
 * src/chamber.h — the window onto the hypervisor.
 *
 * A hypervisor is invisible when it works, which makes it the hardest
 * kind of thing to claim honestly. So this shows the parts that would
 * be forged if they were forged: what the processor reported about its
 * own virtualisation support, the physical addresses of the control
 * block and the nested page tables, a running tally of exits by cause,
 * and the last few exits with their codes and guest instruction
 * pointers. The guest's own screen is drawn from the page the nested
 * page table maps at 0xB8000 -- those characters got there because
 * guest code executed a store instruction, on the real processor.
 */

#define CHAMBER_ROW 15

static int chamber_ready = 0;

static void chamber_open(void) {
    if (!chamber_ready) {
        chamber_ready = 1;
        if (hv.supported && !hv.enabled) hv_reset();
    }
}

static void chamber_draw(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                         int32_t mx, int32_t my) {
    (void)mx; (void)my;
    chamber_open();
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x11141Cu);

    char line[128], nb[24];
    int32_t y = cy + 10;

    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, "Chamber", C_GOLD, 16);
    {
        const char *sub = "AMD-V, nested paging, one guest";
        ttf_draw_string(buf, (int)w, (int)h, cx + 14 + 78, y + 3, sub,
                        C_TEXT_DIM, 11);
    }
    y += 24;

    /* ---- capability ---- */
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, hv.status,
                    hv.supported ? C_TEXT : 0xC08050u, 12);
    y += 18;

    /*
     * The line above already carries the specific reason, measured on
     * this machine, so what follows is only the general shape of it.
     * Kept identical across both trees: hyper.h is what differs, and
     * this window reports whatever it found.
     */
    if (!hv.supported) {
        const char *why =
            "A guest needs hardware virtualisation the host can reach.";
        const char *why2 =
            "On x86 that is AMD-V, which QEMU exposes with -cpu max.";
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, why, C_TEXT_DIM, 11);
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, y + 14, why2,
                        C_TEXT_DIM, 11);
        return;
    }

    str_copy(line, "revision ", sizeof(line));
    uint_to_str(hv.revision, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, ",  ", sizeof(line));
    uint_to_str(hv.asid_max, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " ASIDs,  nested paging ", sizeof(line));
    str_append(line, hv.npt ? "yes" : "no", sizeof(line));
    str_append(line, ",  NRIP ", sizeof(line));
    str_append(line, hv.nrip ? "yes" : "no", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT_DIM, 11);
    y += 16;

    str_copy(line, "VMCB ", sizeof(line));
    hv_hex(hv.vmcb_phys, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, "   nCR3 ", sizeof(line));
    hv_hex(hv.npt_phys, nb);
    str_append(line, nb, sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT_DIM, 11);
    y += 22;

    /* ---- the guest's screen ---- */
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, "guest console",
                    C_TEXT_DIM, 10);
    y += 14;
    {
        const int32_t bh = 3 * CHAMBER_ROW + 10;
        gfx_rect(buf, w, h, cx + 14, y, cw - 28, bh, 0x05070Bu);
        gfx_rect_outline(buf, w, h, cx + 14, y, cw - 28, bh, 0x2A3040u);
        for (int r = 0; r < 3; r++) {
            char row[96];
            hv_screen_row(r, row, sizeof(row));
            if (row[0])
                ttf_draw_string(buf, (int)w, (int)h, cx + 22,
                                y + 5 + r * CHAMBER_ROW, row, 0x66DD88u, 12);
        }
        y += bh + 12;
    }

    /* ---- what the guest computed ---- */
    str_copy(line, "hypercall ", sizeof(line));
    hv_hex(hv.hypercall_arg, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " -> ", sizeof(line));
    hv_hex(hv_scratch32(0), nb);
    str_append(line, nb, sizeof(line));
    str_append(line, "   (stored by the guest)", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT, 11);
    y += 15;

    /* The CPUID answer, read back out of guest memory as four ASCII
     * words -- proof the guest received what the host injected. */
    {
        char sig[20];
        int n = 0;
        /* EBX, ECX then EDX -- the registers a CPUID vendor string is
         * carried in. EAX held the highest supported leaf, not text. */
        for (int r = 2; r <= 4; r++) {
            const uint32_t v = hv_scratch32((uint32_t)r * 4);
            for (int k = 0; k < 4; k++) {
                const char ch = (char)((v >> (k * 8)) & 0xFF);
                if (ch >= 32 && ch < 127 && n < 19) sig[n++] = ch;
            }
        }
        sig[n] = '\0';
        str_copy(line, "cpuid 40000000 -> ", sizeof(line));
        str_append(line, sig, sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT, 11);
        y += 15;
    }

    str_copy(line, "port ", sizeof(line));
    hv_hex(hv.io_port, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " <- ", sizeof(line));
    hv_hex(hv.io_value, nb);
    str_append(line, nb, sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT, 11);
    y += 20;

    /* ---- counters ---- */
    str_copy(line, "VMRUN ", sizeof(line));
    uint_to_str(hv.vmruns, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, "   exits: cpuid ", sizeof(line));
    uint_to_str(hv.n_cpuid, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, ", io ", sizeof(line));
    uint_to_str(hv.n_io, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, ", call ", sizeof(line));
    uint_to_str(hv.n_hypercall, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, ", hlt ", sizeof(line));
    uint_to_str(hv.n_hlt, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, ", intr ", sizeof(line));
    uint_to_str(hv.n_intr, nb);
    str_append(line, nb, sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_TEXT_DIM, 11);
    y += 20;

    /* ---- the exit log ---- */
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, "exits", C_TEXT_DIM, 10);
    y += 14;
    {
        const int rows = (cy + chh - y - 30) / CHAMBER_ROW;
        int shown = rows < hv.log_n ? rows : hv.log_n;
        for (int i = 0; i < shown; i++) {
            const int idx = (hv.log_head - shown + i + HV_LOG_MAX * 2) % HV_LOG_MAX;
            const hv_exit_t *e = &hv.log[idx];
            str_copy(line, hv_exit_name(e->code), sizeof(line));
            while (str_len(line) < 9) str_append(line, " ", sizeof(line));
            str_append(line, "rip ", sizeof(line));
            hv_hex(e->rip, nb);
            str_append(line, nb, sizeof(line));
            str_append(line, "   info1 ", sizeof(line));
            hv_hex(e->info1, nb);
            str_append(line, nb, sizeof(line));
            ttf_draw_string(buf, (int)w, (int)h, cx + 22, y, line,
                            0x8A93A8u, 10);
            y += CHAMBER_ROW;
        }
    }

    /* ---- controls ---- */
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, cy + chh - 20,
                    "r  run     s  single step     n  reset",
                    C_TEXT_DIM, 10);
}

static void chamber_key(char ch) {
    chamber_open();
    if (!hv.supported) return;
    if (ch == 'r' || ch == 'R')      hv_run(64);
    else if (ch == 's' || ch == 'S') hv_step();
    else if (ch == 'n' || ch == 'N') hv_reset();
}

static void chamber_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb) {
    (void)mx; (void)my;
    if (lmb && !prev_lmb) {
        chamber_open();
        if (hv.supported && !hv.finished) hv_run(64);
    }
}

#endif /* VEXTRO_CHAMBER_H */
