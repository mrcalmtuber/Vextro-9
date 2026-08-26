#ifndef TERM_H
#define TERM_H

/*
 * Vextro Terminal — monospace grid renderer with scrollback, command
 * history, line editing and async network commands (ping / dns / fetch).
 *
 * Included from desktop.h; relies on forward declarations there for
 * wm_open(), execute_bin(), tarfs helpers and the netstack globals.
 */

#define TERM_COLS     80
#define TERM_SB       240      /* scrollback lines                    */
#define TERM_LINE_H   13       /* px per text row (8 px glyph + lead) */
#define TERM_PAD      8
#define TERM_HIST     16
#define TERM_INPUT_MAX 120

static char    term_lines[TERM_SB][TERM_COLS + 1];
static uint8_t term_line_color[TERM_SB];
static int     term_row = 0;         /* current write row      */
static int     term_cx = 0;          /* column in current row  */
static int     term_view = 0;        /* scrollback offset      */
static int     term_cur_color = 0;

static char    term_input[TERM_INPUT_MAX];
static int     term_input_len = 0;
static int     term_input_cur = 0;

static char    term_hist[TERM_HIST][TERM_INPUT_MAX];
static int     term_hist_count = 0;
static int     term_hist_pos = -1;   /* -1 = editing new line */

/* Async command engine */
#define TERM_ASYNC_NONE  0
#define TERM_ASYNC_PING  1
#define TERM_ASYNC_DNS   2
#define TERM_ASYNC_FETCH 3

static int      term_async = TERM_ASYNC_NONE;
static uint32_t term_async_t0 = 0;
static char     term_async_arg[128];
static int      term_ping_next_at = 0;

/* Working directory + output redirection */
static char term_cwd[200] = "/";

#define TERM_CAP_MAX 24576
static int  term_redirect_active = 0;
static char term_cap[TERM_CAP_MAX];
static int  term_cap_len = 0;
static uint8_t term_append_buf[FS_FILEBUF_MAX];

/* Resolve a possibly-relative path against the cwd, handling . and .. */
static void term_resolve(const char *in, char *out /* >= 256 */) {
    char joined[512];
    if (in[0] == '/') {
        str_copy(joined, in, sizeof(joined));
    } else {
        str_copy(joined, term_cwd, sizeof(joined));
        str_append(joined, "/", sizeof(joined));
        str_append(joined, in, sizeof(joined));
    }

    char comps[16][64];
    int ncomp = 0;
    int i = 0;
    while (joined[i]) {
        while (joined[i] == '/') i++;
        if (!joined[i]) break;
        char comp[64];
        int cl = 0;
        while (joined[i] && joined[i] != '/' && cl < 63)
            comp[cl++] = joined[i++];
        while (joined[i] && joined[i] != '/') i++;   /* overlong tail */
        comp[cl] = '\0';
        if (str_eq(comp, ".")) continue;
        if (str_eq(comp, "..")) {
            if (ncomp > 0) ncomp--;
            continue;
        }
        if (ncomp < 16)
            str_copy(comps[ncomp++], comp, 64);
    }

    int p = 0;
    if (ncomp == 0) {
        out[p++] = '/';
    } else {
        for (int c = 0; c < ncomp; c++) {
            out[p++] = '/';
            for (int j = 0; comps[c][j] && p < 254; j++)
                out[p++] = comps[c][j];
        }
    }
    out[p] = '\0';
}

/* Line colors */
static const uint32_t term_palette[6] = {
    C_TERM_FG,   /* 0 default   */
    C_GOLD,      /* 1 gold      */
    C_RED,       /* 2 error     */
    0x6A7284u,   /* 3 dim       */
    C_GREEN,     /* 4 ok        */
    0x9FB6D8u,   /* 5 info blue */
};

static void term_clear(void) {
    for (int i = 0; i < TERM_SB; i++) {
        term_lines[i][0] = '\0';
        term_line_color[i] = 0;
    }
    term_row = 0;
    term_cx = 0;
    term_view = 0;
}

static void term_newline(void) {
    term_cx = 0;
    if (term_row < TERM_SB - 1) {
        term_row++;
    } else {
        for (int i = 0; i < TERM_SB - 1; i++) {
            for (int c = 0; c <= TERM_COLS; c++)
                term_lines[i][c] = term_lines[i + 1][c];
            term_line_color[i] = term_line_color[i + 1];
        }
    }
    term_lines[term_row][0] = '\0';
    term_line_color[term_row] = (uint8_t)term_cur_color;
}

static void term_putc(char ch) {
#ifdef USER_SELFTEST
    /* Mirrored like term_print: without this the spaces and newlines a
     * command emits one character at a time never reach the log, and its
     * output arrives as one run-together line. */
    if (!term_redirect_active) serial_putc(ch);
#endif
    if (term_redirect_active) {
        if (term_cap_len < TERM_CAP_MAX)
            term_cap[term_cap_len++] = ch;
        return;
    }
    if (ch == '\n') { term_newline(); return; }
    if (ch == '\t') {
        int spaces = 4 - (term_cx % 4);
        for (int i = 0; i < spaces; i++) term_putc(' ');
        return;
    }
    if (ch < 0x20 || ch > 0x7E) return;
    if (term_cx >= TERM_COLS) term_newline();
    term_lines[term_row][term_cx++] = ch;
    term_lines[term_row][term_cx] = '\0';
    term_line_color[term_row] = (uint8_t)term_cur_color;
}

static void term_print_c(const char *s, int color) {
    term_cur_color = color;
    while (*s) term_putc(*s++);
    term_cur_color = 0;
}

static void term_print(const char *s) {
    while (*s) term_putc(*s++);
}

/* ===== COMMAND IMPLEMENTATIONS ===== */

static int term_ls_count = 0;

static void term_ls_entry(const char *name, uint32_t size, int is_dir) {
    term_ls_count++;
    term_print("  ");
    if (is_dir) {
        term_print_c(name, 1);
        term_print_c("/", 1);
    } else {
        term_print(name);
    }
    int pad = 26 - str_len(name) - (is_dir ? 1 : 0);
    for (int i = 0; i < pad; i++) term_putc(' ');
    if (is_dir) {
        term_print_c("<dir>\n", 3);
    } else {
        char szbuf[16];
        uint_to_str(size, szbuf);
        term_print_c(szbuf, 3);
        term_print_c(" bytes\n", 3);
    }
}

static void term_cmd_ls(const char *arg) {
    term_ls_count = 0;

    if (fs_writable()) {
        char abs[256];
        term_resolve(arg && arg[0] ? arg : ".", abs);
        if (fs_list(abs, term_ls_entry) != 0) {
            term_print_c("ls: ", 2);
            term_print_c(fs_errstr, 2);
            term_print_c(": ", 2);
            term_print_c(abs, 2);
            term_putc('\n');
            return;
        }
        if (term_ls_count == 0) term_print_c("(empty)\n", 3);
        return;
    }

    /* tar fallback: flat root listing */
    if (!tarfs_base) {
        term_print_c("no filesystem available\n", 2);
        return;
    }
    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;
    while (ptr + TAR_BLOCK_SIZE <= end) {
        tar_header_t *hdr = (tar_header_t *)ptr;
        if (hdr->name[0] == '\0') break;
        uint64_t file_size = octal_parse(hdr->size, 12);
        if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            const char *name = hdr->name;
            if (name[0] == '.' && name[1] == '/') name += 2;
            if (name[0] != '\0')
                term_ls_entry(name, (uint32_t)file_size, 0);
        }
        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }
    if (term_ls_count == 0) term_print_c("(empty)\n", 3);
}

static void term_cmd_cat(const char *fname) {
    char abs[256];
    term_resolve(fname, abs);
    uint64_t fsize = 0;
    const void *data = fs_read_file(abs, &fsize);
    if (!data) {
        term_print_c("cat: file not found: ", 2);
        term_print_c(abs, 2);
        term_putc('\n');
        return;
    }
    const char *text = (const char *)data;
    for (uint64_t i = 0; i < fsize; i++) {
        if (text[i] == '\0') break;
        term_putc(text[i]);
    }
    if (fsize > 0 && text[fsize - 1] != '\n') term_putc('\n');
}

static void term_print_hex32(uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++)
        b[2 + i] = hx[(v >> (28 - 4 * i)) & 0xF];
    b[10] = '\0';
    term_print(b);
}

static int term_parse_hex(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        if (n >= 8) return 0;
        v = (v << 4) | d;
    }
    if (n == 0) return 0;
    *out = v;
    return 1;
}

static void term_gpu_reg_row(const char *key, uint32_t val) {
    term_print("  ");
    term_print(key);
    int pad = 18 - str_len(key);
    for (int i = 0; i < pad; i++) term_putc(' ');
    term_print_hex32(val);
    term_putc('\n');
}

/*
 * `gpu bench` -- hand the blitter a real batch and time it.
 *
 * The boot self-test proves the command encodings against a scratch
 * surface. This proves the thing the encodings exist for: that a batch
 * of work submitted once is faster than the processor doing it, on the
 * visible framebuffer, on whatever machine is actually running.
 *
 * It is a command rather than something the compositor does on its own,
 * and that is deliberate. Routing every frame through the blitter is a
 * claim about hardware, and the machine this was written on has no
 * supported iGPU to test it against; wiring an unverified path into the
 * one operation that puts pixels on the panel would be trading a
 * measured 60 fps for an untested one. This is how someone with the
 * hardware finds out first.
 */
static void term_cmd_gpu_bench(void) {
    if (!igpu.active) {
        term_print_c("gpu: blitter not active - ", 2);
        term_print_c(igpu.status, 2);
        term_putc('\n');
        return;
    }
    if (!igpu.fb_blittable) {
        term_print_c("gpu: the framebuffer is not GGTT-reachable on this "
                     "machine, so there is nothing to blit to\n", 2);
        return;
    }

    const int N = 64;
    const int bw = 48, bh = 32;

    /* What the processor costs for the same work, on the same memory --
     * the framebuffer itself, reached through the direct map, which is
     * what igpu.fb_phys is recorded for. */
    volatile uint32_t *fb =
        (volatile uint32_t *)(uintptr_t)phys_to_virt(igpu.fb_phys);
    const uint32_t pitch_px = igpu.fb_pitch_bytes / 4;

    uint64_t t0 = cycle_now();
    for (int i = 0; i < N; i++) {
        int x = (i * 37) % ((int)igpu.fb_w - bw);
        int y = (i * 53) % ((int)igpu.fb_h - bh);
        for (int r = 0; r < bh; r++)
            for (int c = 0; c < bw; c++)
                fb[(uint32_t)(y + r) * pitch_px + (uint32_t)(x + c)] = 0x101018u;
    }
    uint64_t t1 = cycle_now();

    /* And what one batch costs. */
    igpu_batch_begin();
    for (int i = 0; i < N; i++) {
        int x = (i * 37) % ((int)igpu.fb_w - bw);
        int y = (i * 53) % ((int)igpu.fb_h - bh);
        igpu_batch_fill(x, y, bw, bh, 0x101018u);
    }
    int ops = igpu_batch_ops();
    uint64_t t2 = cycle_now();
    int rc = igpu_batch_submit();
    uint64_t t3 = cycle_now();

    char nb[16];
    term_print_c("gpu bench: ", 3);
    uint_to_str((uint32_t)N, nb); term_print(nb);
    term_print(" rectangles of ");
    uint_to_str((uint32_t)bw, nb); term_print(nb);
    term_print("x");
    uint_to_str((uint32_t)bh, nb); term_print(nb);
    term_putc('\n');

    term_print("  cpu            ");
    uint_to_str(cycles_to_us(t1 - t0), nb); term_print(nb);
    term_print(" us\n");

    term_print("  batch encode   ");
    uint_to_str(cycles_to_us(t2 - t1), nb); term_print(nb);
    term_print(" us (");
    uint_to_str((uint32_t)ops, nb); term_print(nb);
    term_print(" commands, one submission)\n");

    term_print("  gpu execute    ");
    uint_to_str(cycles_to_us(t3 - t2), nb); term_print(nb);
    term_print(" us\n");

    if (rc != 0)
        term_print_c("  the batch did not complete - see `gpu error`\n", 2);

    /*
     * And the copy, which is the operation worth having. Moving a large
     * region of the screen is what dragging a window and scrolling a
     * list both are, and it is pure memory traffic -- the case where a
     * blitter beats a processor by the widest margin.
     */
    {
        int mw = (int)igpu.fb_w / 2, mh = (int)igpu.fb_h / 3;
        if (mw > 8 && mh > 8) {
            igpu_batch_begin();
            igpu_batch_move(0, 0, mw, mh, mw, mh);
            uint64_t m0 = cycle_now();
            int mrc = igpu_batch_submit();
            uint64_t m1 = cycle_now();

            term_print("  gpu copy       ");
            uint_to_str(cycles_to_us(m1 - m0), nb); term_print(nb);
            term_print(" us for ");
            uint_to_str((uint32_t)(mw * mh / 1000), nb); term_print(nb);
            term_print("k pixels");
            if (mrc != 0) term_print_c("  (failed)", 2);
            term_putc('\n');
        }
    }

    /* Both halves wrote straight to the panel, behind the compositor's
     * back. The frame diff has no idea, so ask for one full repaint. */
    gfx_force_full_flip = 1;
}

static void term_cmd_gpu_error(void) {
    if (!igpu_crash.valid) {
        term_print_c("no GPU errors recorded\n", 4);
        return;
    }
    term_print_c("last GPU hang (BCS blitter)\n", 2);
    term_print("  parser died in    ");
    term_print_c(igpu_crash.cmd_name, 2);
    term_putc('\n');
    term_gpu_reg_row("IPEHR (bad cmd)", igpu_crash.ipehr);
    term_gpu_reg_row("IPEIR", igpu_crash.ipeir);
    term_gpu_reg_row("EIR", igpu_crash.eir);
    if (igpu_crash.eir & IGPU_ERR_INSTRUCTION)
        term_print_c("    - invalid instruction error\n", 2);
    if (igpu_crash.eir & IGPU_ERR_PAGE_TABLE)
        term_print_c("    - page table error\n", 2);
    if (igpu_crash.eir & IGPU_ERR_MEM_REFRESH)
        term_print_c("    - memory refresh error\n", 2);
    if (igpu_crash.eir & IGPU_ERR_PRIV)
        term_print_c("    - privilege violation\n", 2);
    term_gpu_reg_row("ESR", igpu_crash.esr);
    term_gpu_reg_row("INSTDONE", igpu_crash.instdone);
    term_gpu_reg_row("ACTHD", igpu_crash.acthd_lo);
    term_gpu_reg_row("RING_HEAD", igpu_crash.ring_head);
    term_gpu_reg_row("RING_TAIL", igpu_crash.ring_tail);
    term_gpu_reg_row("RING_CTL", igpu_crash.ring_ctl);
    if (igpu_crash.fault_reg & 1) {
        term_print_c("  GGTT fault at page ", 2);
        term_print_hex32(igpu_crash.fault_reg & 0xFFFFF000);
        term_putc('\n');
        term_gpu_reg_row("RING_FAULT_REG", igpu_crash.fault_reg);
    }
    term_gpu_reg_row("HWS[0]", igpu_crash.hws[0]);

    char nb[16];
    term_print("  breadcrumb        saw ");
    uint_to_str(igpu_crash.seqno_seen, nb);
    term_print(nb);
    term_print(" wanted ");
    uint_to_str(igpu_crash.seqno_expected, nb);
    term_print(nb);
    term_putc('\n');

    term_print("  ring at ACTHD\n");
    for (int i = 0; i < 8; i += 4) {
        term_print("    ");
        for (int j = 0; j < 4; j++) {
            term_print_hex32(igpu_crash.ring_window[i + j]);
            term_putc(' ');
        }
        term_putc('\n');
    }

    uint_to_str((uint32_t)igpu_crash.hang_count, nb);
    term_print("  hang count        ");
    term_print(nb);
    term_print("   recovery: ");
    if (igpu.active)
        term_print_c(igpu_crash.reset_ok ? "engine reset OK\n"
                                         : "pending\n", 4);
    else
        term_print_c("failed - CPU renderer\n", 2);
}

static void term_cmd_df(void) {
    if (!fs_writable()) {
        term_print_c("no writable volume (tar ramdisk fallback active)\n", 3);
        return;
    }
    char nb[16];
    uint32_t total = fs_total_kb(), free_kb = fs_free_kb();
    term_print("  volume    ");
    term_print(fs_name());
    term_print(" on ");
    term_print(blk_bus_name());          /* NVMe, SATA or IDE — whichever
                                          * bus the volume was found on */
    term_print(" (");
    uint_to_str(fs_kind == FS_NTFS  ? (uint32_t)ntfs_total_clusters()
              : fs_kind == FS_EXFAT ? exf_vol.cluster_count
                                    : fat_vol.nclusters, nb);
    term_print(nb);
    term_print(" clusters)\n  device    ");
    uint_to_str((uint32_t)(blk_sectors() / 2048), nb);
    term_print(nb);
    term_print(" MB raw\n  total     ");
    uint_to_str(total / 1024, nb);
    term_print(nb);
    term_print(" MB\n  used      ");
    uint_to_str((total - free_kb) / 1024, nb);
    term_print(nb);
    term_print(" MB\n  free      ");
    uint_to_str(free_kb / 1024, nb);
    term_print_c(nb, 4);
    term_print_c(" MB\n", 4);
}

static void term_cmd_mouse(void) {
    char nb[16];

    term_print("  pointer   ");
    if (mouse_absolute)
        term_print_c("absolute (VMware backdoor) - tracks without a grab\n", 4);
    else
        term_print_c("relative (PS/2) - the host must capture the cursor\n", 3);

    term_print("  packets   ");
    uint_to_str((uint32_t)mouse_pkt_len, nb);
    term_print(nb);
    term_print(mouse_pkt_len == 4 ? " bytes, wheel present\n"
                                  : " bytes, no wheel\n");

    term_print("  position  ");
    uint_to_str((uint32_t)mouse_x, nb); term_print(nb);
    term_print(", ");
    uint_to_str((uint32_t)mouse_y, nb); term_print(nb);
    term_print("   buttons ");
    uint_to_str((uint32_t)mouse_buttons, nb); term_print(nb);
    term_print("\n  screen    ");
    uint_to_str((uint32_t)(mouse_max_x + 1), nb); term_print(nb);
    term_print(" x ");
    uint_to_str((uint32_t)(mouse_max_y + 1), nb); term_print(nb);
    term_putc('\n');
}

static void term_cmd_net(void) {
    char buf[24];
    if (!e1000_found) {
        /* There may still be a radio. This command reports the wired
         * adapter, so it says where the other one is rather than
         * claiming the machine has no networking at all. */
        if (wifi_present()) {
            term_print_c("no wired adapter; a wireless one is present\n", 3);
            term_print("  try `wifi` for its state\n");
        } else {
            term_print_c("no network adapter detected\n", 2);
        }
        return;
    }
    term_print("  adapter   Intel 82540EM (e1000)\n");

    static const char hex[] = "0123456789ABCDEF";
    char mac[20];
    int p = 0;
    for (int i = 0; i < 6; i++) {
        if (i) mac[p++] = ':';
        mac[p++] = hex[(e1000_mac[i] >> 4) & 0xF];
        mac[p++] = hex[e1000_mac[i] & 0xF];
    }
    mac[p] = '\0';
    term_print("  mac       "); term_print(mac); term_putc('\n');

    ip_to_str(net_our_ip, buf);
    term_print("  ip        "); term_print(buf); term_putc('\n');
    ip_to_str(net_mask, buf);
    term_print("  netmask   "); term_print(buf); term_putc('\n');
    ip_to_str(net_gw_ip, buf);
    term_print("  gateway   "); term_print(buf); term_putc('\n');
    ip_to_str(net_dns_ip, buf);
    term_print("  dns       "); term_print(buf); term_putc('\n');

    uint32_t status = e1000_read(E1000_STATUS);
    term_print("  link      ");
    if (status & E1000_STATUS_LU) term_print_c("up\n", 4);
    else                          term_print_c("down\n", 2);
}

/*
 * `wifi` — the radio, from the terminal.
 *
 * The whole wireless stack is reachable from here and nowhere else yet:
 * joining a network is a decision with a password in it, so it does not
 * happen automatically at boot the way DHCP does on a cable. Once a
 * network is joined, lwIP moves onto it without being told -- see
 * vx_use_wifi() in vxport_impl.h -- and the browser, the package store
 * and everything else above the socket layer carry on unaware.
 */
static void term_cmd_wifi(int argc, char **argv) {
    static const char hex[] = "0123456789ABCDEF";

    if (!wifi_present()) {
        term_print_c("no wireless adapter\n", 2);
        /* Worth saying which of the two reasons it is: a machine with
         * no radio and a radio that could not be started look the same
         * from here and want completely different responses. */
        if (wifi_dev.ops) {
            term_print("  ");
            term_print(wifi_dev.ops->name);
            term_print(" found but stopped at: ");
            term_print(wifi_state_name(wifi_dev.state));
            term_putc('\n');
            if (wifi_dev.state == WIFI_STATE_NO_FIRMWARE)
                term_print_c("  this part needs a firmware image; see "
                             "src/net/wifi.c\n", 3);
        }
        return;
    }

    if (argc >= 2 && str_eq(argv[1], "scan")) {
        int n = wifi_scan(20);
        if (n <= 0) { term_print_c("no networks found\n", 2); return; }

        term_print("  SSID                              CH  SIGNAL  SECURITY\n");
        for (int i = 0; i < n; i++) {
            const wifi_bss_t *b = &wifi_bss_list[i];
            char num[8];
            int pad;

            term_print("  ");
            term_print(b->ssid_len ? b->ssid : "(hidden)");
            pad = 32 - (b->ssid_len ? b->ssid_len : 8);
            for (int j = 0; j < pad; j++) term_putc(' ');

            uint_to_str(b->channel, num);
            term_print_c(num, 3);
            pad = 4 - str_len(num);
            for (int j = 0; j < pad; j++) term_putc(' ');

            /* RSSI is negative dBm; print it as a magnitude with the
             * sign written out, because uint_to_str cannot. */
            term_print("-");
            uint_to_str((uint32_t)(-b->rssi), num);
            term_print(num);
            term_print(" dBm  ");

            term_print_c(wifi_security_name(b->security),
                         b->security == WIFI_SEC_OPEN ? 2 : 4);
            term_putc('\n');
        }
        return;
    }

    if (argc >= 2 && str_eq(argv[1], "connect")) {
        if (argc < 3) {
            term_print_c("usage: wifi connect <ssid> [passphrase]\n", 2);
            return;
        }
        term_print("joining ");
        term_print(argv[2]);
        term_print(" ...\n");

        if (wifi_connect(argv[2], argc >= 4 ? argv[3] : "") == 0) {
            term_print_c("connected\n", 4);
            if (wpa_sm_complete(&wifi_sm))
                term_print("  link is encrypted with CCMP\n");
        } else {
            term_print_c("could not join: ", 2);
            term_print(wifi_state_name(wifi_dev.state));
            term_putc('\n');
        }
        return;
    }

    if (argc >= 2 && str_eq(argv[1], "disconnect")) {
        wifi_disconnect();
        term_print("disconnected\n");
        return;
    }

    /* No sub-command: report where things stand. */
    term_print("  adapter   ");
    term_print(wifi_dev.ops->name);
    term_putc('\n');

    {
        char mac[20];
        int p = 0;
        for (int i = 0; i < 6; i++) {
            if (i) mac[p++] = ':';
            mac[p++] = hex[(wifi_dev.mac[i] >> 4) & 0xF];
            mac[p++] = hex[wifi_dev.mac[i] & 0xF];
        }
        mac[p] = '\0';
        term_print("  mac       "); term_print(mac); term_putc('\n');
    }

    term_print("  state     ");
    term_print_c(wifi_state_name(wifi_dev.state),
                 wifi_connected() ? 4 : 3);
    term_putc('\n');

    if (wifi_connected()) {
        char num[8];
        term_print("  network   ");
        term_print(wifi_target.ssid);
        term_putc('\n');
        term_print("  channel   ");
        uint_to_str(wifi_dev.channel, num);
        term_print(num);
        term_putc('\n');
        term_print("  cipher    ");
        term_print(wpa_sm_complete(&wifi_sm) ? "CCMP (AES)" : "none");
        term_putc('\n');
        term_print("  group key ");
        term_print(wifi_sm.gtk_valid ? "installed" : "none");
        term_putc('\n');
    }

    term_print("\n  wifi scan | wifi connect <ssid> <pass> | wifi disconnect\n");
}

/*
 * `video` — the hardware decoder, from the terminal.
 *
 * With no argument it reports what the machine has. With a file it
 * decodes an Annex B H.264 elementary stream into this window's canvas
 * -- which, because the canvas is the same memory Ring 3 has mapped, is
 * the decoder writing straight into a user-visible framebuffer.
 */
static void term_cmd_video(int argc, char **argv) {
    char num[16];

    if (argc < 2) {
        term_print("  engine    ");
        term_print_c(vdec_status(), vdec_hw_available() ? 4 : 3);
        term_putc('\n');

        term_print("  gpu       ");
        term_print(igpu.active ? (igpu.name ? igpu.name : "Intel") : "none");
        term_putc('\n');

        if (igpu.active) {
            term_print("  VCS ring  ");
            term_print(igpu_vcs.ready ? "up" : "down");
            term_putc('\n');
            term_print("  VECS ring ");
            term_print(igpu_vecs.ready ? "up" : "down");
            term_putc('\n');
        }

        if (vdec.open) {
            term_print("  stream    ");
            uint_to_str(vdec.width, num);  term_print(num);
            term_print("x");
            uint_to_str(vdec.height, num); term_print(num);
            term_putc('\n');
            term_print("  decoded   ");
            uint_to_str(vdec.frames_decoded, num); term_print(num);
            term_print(" frames, ");
            uint_to_str(vdec.frames_failed, num); term_print(num);
            term_print(" failed\n");
            term_print("  colour    ");
            term_print(vdec.hw_path ? "VEBOX (hardware)"
                                     : "integer conversion");
            term_putc('\n');
        }

        if (!vdec_hw_available())
            term_print_c("\n  no Quick Sync path on this machine; see"
                         " src/media/decode.c\n", 3);
        term_print("\n  video <file.h264>   decode into this window\n");
        return;
    }

    if (!vdec_hw_available()) {
        term_print_c("no hardware video engine on this machine\n", 2);
        return;
    }

    {
        char abs[256];
        uint64_t fsize = 0;
        const void *data;
        int frames;

        term_resolve(argv[1], abs);
        data = fs_read_file(abs, &fsize);
        if (!data) {
            term_print_c("video: file not found: ", 2);
            term_print_c(abs, 2);
            term_putc('\n');
            return;
        }

        if (vdec_open() != 0) {
            term_print_c("video: decoder would not open\n", 2);
            return;
        }

        /* The canvas: the pixels this window shows and the pixels a
         * Ring 3 program sees through os_canvas(). */
        vdec_set_target(app_canvas, APP_CANVAS_W, APP_CANVAS_H,
                         APP_CANVAS_W);

        frames = vdec_feed((const uint8_t *)data, (uint32_t)fsize);
        if (frames < 0) {
            term_print_c("video: nothing decodable in that file\n", 2);
            return;
        }

        uint_to_str((uint32_t)frames, num);
        term_print("decoded ");
        term_print(num);
        term_print(" pictures\n");
    }
}

/*
 * `rdp` — the remote desktop service.
 *
 * Off by default and started by hand, deliberately: this server has no
 * encryption and no authentication, so starting it is a decision about
 * the network the machine is on rather than something that should
 * happen at boot.
 */
static void term_cmd_rdp(int argc, char **argv) {
    char num[16];

    if (argc >= 2 && str_eq(argv[1], "start")) {
        if (rdp_running()) {
            term_print("already running\n");
            return;
        }
        term_print_c("warning: RDP here is plaintext and unauthenticated.\n", 2);
        term_print_c("  keystrokes and the screen cross the network in the "
                     "clear,\n", 2);
        term_print_c("  and nothing proves this machine is who it says.\n", 2);
        if (rdp_start() != 0) {
            term_print_c("could not start: ", 2);
            term_print(rdp_status());
            term_putc('\n');
            return;
        }
        term_print_c("\nlistening on port 3389\n", 4);
        return;
    }

    if (argc >= 2 && str_eq(argv[1], "stop")) {
        rdp_stop();
        term_print("stopped\n");
        return;
    }

    term_print("  service   ");
    term_print_c(rdp_status(), rdp_running() ? 4 : 3);
    term_putc('\n');
    term_print("  port      3389\n");
    term_print("  security  ");
    term_print_c(rdp_is_encrypted() ? "encrypted" : "none (plaintext)", 2);
    term_putc('\n');

    if (rdp_running()) {
        term_print("  state     ");
        term_print(rdp_state_name(rdp.state));
        term_putc('\n');

        if (rdp_connected()) {
            term_print("  client    ");
            for (int i = 0; i < 4; i++) {
                uint_to_str(rdp.peer[i], num);
                term_print(num);
                if (i != 3) term_print(".");
            }
            term_putc('\n');
            term_print("  desktop   ");
            uint_to_str(rdp.frame_w, num); term_print(num);
            term_print("x");
            uint_to_str(rdp.frame_h, num); term_print(num);
            term_print(" at 16bpp\n");
            term_print("  sent      ");
            uint_to_str(rdp.tiles_sent, num); term_print(num);
            term_print(" tiles, ");
            uint_to_str(rdp.bytes_sent / 1024, num); term_print(num);
            term_print(" KB\n");
            term_print("  input     ");
            uint_to_str(rdp.events_in, num); term_print(num);
            term_print(" events\n");
        }
    }

    term_print("\n  rdp start | rdp stop\n");
}

static void term_cmd_arp(void) {
    int n = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) continue;
        uint8_t ip[4];
        ip_from_u32(arp_cache[i].ip, ip);
        char buf[24];
        ip_to_str(ip, buf);
        term_print("  ");
        term_print(buf);
        int pad = 18 - str_len(buf);
        for (int j = 0; j < pad; j++) term_putc(' ');
        char mac[20];
        int p = 0;
        for (int j = 0; j < 6; j++) {
            if (j) mac[p++] = ':';
            mac[p++] = hex[(arp_cache[i].mac[j] >> 4) & 0xF];
            mac[p++] = hex[arp_cache[i].mac[j] & 0xF];
        }
        mac[p] = '\0';
        term_print_c(mac, 3);
        term_putc('\n');
        n++;
    }
    if (n == 0) term_print_c("(arp cache empty)\n", 3);
}

static void term_cmd_uptime(void) {
    uint32_t secs = desktop_tick / 60;
    char buf[16];
    term_print("  up ");
    uint_to_str(secs / 3600, buf); term_print(buf); term_print("h ");
    uint_to_str((secs / 60) % 60, buf); term_print(buf); term_print("m ");
    uint_to_str(secs % 60, buf); term_print(buf); term_print("s\n");
}

static void term_cmd_date(void) {
    int hh, mm, ss, d, mo, yr;
    rtc_read(&hh, &mm, &ss, &d, &mo, &yr);
    char buf[16];
    term_print("  ");
    if (mo >= 1 && mo <= 12) term_print(month_names[mo - 1]);
    term_putc(' ');
    uint_to_str((uint32_t)d, buf); term_print(buf);
    term_print(", ");
    uint_to_str((uint32_t)yr, buf); term_print(buf);
    term_print("  ");
    char clk[10];
    clock_string(clk);
    term_print(clk);
    term_print(" UTC\n");
}

static void term_cmd_help(void) {
    term_print_c("Vextro 9 shell commands\n", 1);
    term_print("  ls [dir]  cat <f>  cd <dir>  pwd     browse the disk\n");
    term_print("  echo <text> > f    write a file  (>> appends)\n");
    term_print("  rm <f>  mkdir <d>  cp <a> <b>  df    manage the disk\n");
    term_print("  run <program>     execute an ELF app\n");
    term_print("  date / uptime / mem / uname / mouse   system info\n");
    term_print("  net / arp / ping / dns / fetch       networking\n");
    term_print("  wifi [scan|connect|disconnect]      wireless\n");
    term_print("  video [file.h264]                   hardware decode\n");
    term_print("  rdp [start|stop]                    remote desktop\n");
    term_print("  img <file.sci>    decode and show a compressed image\n");
    term_print("  peek <f> <off> [n]  read a window from a huge file\n");
    term_print("  zim open <f> | info | main | ls | find/get <path>\n");
    term_print("  llm load <f> | weights | tok <t> | eval <tok> | probe | gen <t>\n");
    term_print("  beep [hz]                        play a tone through AC97\n");
    term_print("  policy | allow | scan            security policy and scanning\n");
    term_print("  vault seal|open <..>             encrypted containers\n");
    term_print("  backup | restore <f> <pass>      this account's home directory\n");
    term_print("  store [list|install <id>|remove <id>|run <id>|refresh]\n");
    term_print("                    the Ingot app store\n");
    term_print("  gpu [test|error|decode <hex>]  iGPU status / hang report\n");
    term_print("  open <app>        terminal browser files settings\n");
    term_print("                    paint sysmon matrix about\n");
    term_print("  whoami / users / passwd / logout\n");
    term_print("  env [name]                       this session's profile\n");
    term_print("  swap                             pagefile and paging counters\n");
    term_print("  tickets [purge]                  cached Kerberos credentials\n");
    term_print("  useradd <name> <pw> [admin] / userdel <name>   (admin)\n");
    term_print("  history / clear / reboot / shutdown\n");
    term_print("  Any command's output can be redirected:  ls > list.txt\n");
    term_print("  PgUp/PgDn scroll, Up/Down history, Esc cancels a task\n");
}

static void term_prompt_begin(void) {
    /* nothing stored — the input line is drawn live under the output */
    term_hist_pos = -1;
    term_input_len = 0;
    term_input_cur = 0;
    term_input[0] = '\0';
}

/*
 * Split into argv (destructive on buf).
 *
 * Quotes are honoured, single and double alike. Without them a separator
 * that *is* a space cannot be written down -- `cut -d ' '` split into
 * `-d` and nothing, and `grep "two words"` searched for `"two`. The
 * quotes are removed as the argument is copied down over itself, which
 * needs no second buffer.
 */
static int term_split(char *buf, char **argv, int max) {
    int argc = 0;
    char *p = buf;
    while (*p && argc < max) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;

        char *w = p;                 /* where the unquoted text is built */
        argv[argc++] = w;
        while (*p && *p != ' ') {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) *w++ = *p++;
                if (*p) p++;         /* step over the closing quote */
            } else {
                *w++ = *p++;
            }
        }
        int at_end = (*p == '\0');
        *w = '\0';
        if (!at_end) p++;
    }
    return argc;
}

static int llm_read_thunk(void *ctx, uint64_t off, void *buf,
                          uint32_t len, uint32_t *got) {
    return fs_pread((fs_file_t *)ctx, off, buf, len, got);
}

/*
 * ===== Background model loading =====
 *
 * The chat panel used to tell people to go and type two commands in the
 * terminal before it would answer anything, and the second of those
 * commands read ~370 MB in a single call, so the whole machine sat
 * frozen while it ran.  Instead: notice the model on the volume at boot
 * and stream it in from the frame loop, a megabyte at a time, so the
 * desktop stays live and nothing needs typing.
 */
#define AI_IDLE     0
#define AI_PARSE    1      /* read the GGUF header, metadata and tensor table */
#define AI_WEIGHTS  2      /* stream the payload into the arena               */
#define AI_READY    3
#define AI_FAILED   4

/*
 * Two models can answer here, and which one is a choice.
 *
 *   /explain.gguf   135M, fine-tuned in this repository on passages from
 *                   the archive, to answer from the passage and to
 *                   refuse when the passage does not answer
 *   /qwen2.gguf     0.5B, general purpose, knows a great deal that is
 *                   not in the archive -- which is the problem
 *
 * The smaller one is preferred when it is present: it was trained for
 * exactly this task, it is four times faster, and being ignorant of
 * everything outside the passage is a feature here rather than a
 * limitation. `llm use <name>` switches at run time.
 */
#define AI_MODEL_PATH "/qwen2.gguf"
#define AI_MODEL_ALT  "/explain.gguf"

static int         ai_state = AI_IDLE;
static const char *ai_err   = "";
static fs_file_t   ai_file;
static char        ai_model[40] = AI_MODEL_ALT;

/*
 * Which slot is being filled, and what goes in each.
 *
 * Slot 0 answers; slot 1 is the second opinion. Both stay resident, so a
 * question can be put to each without either being reloaded -- see
 * LLM_SLOTS in llm.h for why that is an index rather than a reload.
 */
static int         ai_loading_slot = 0;
static char        ai_slot_path[LLM_SLOTS][40];

/* Load progress reporting: when it started, and the last tenth
 * announced. Declared here because a model switch resets them. */
static uint64_t    ai_t0 = 0;
static int         ai_last_decile = -1;

/* The arena the weights live in, remembered so a switch can reset it. */
static void       *ai_arena_base = 0;
static uint64_t    ai_arena_size = 0;

/* Begin, if the model is actually there.  Missing is not a failure — a
 * machine without one simply has no chat. */
static void ai_autoload_start(void) {
    if (ai_state != AI_IDLE) return;
    /* Declined, or not yet asked. Loading hundreds of megabytes on the
     * strength of an answer nobody has given would be the wrong
     * default. */
    if (ai_enabled != 1) return;

    /* The task-trained model if the volume carries it, the general one
     * otherwise. A machine with neither simply has no chat. */
    if (fs_stat(AI_MODEL_ALT, 0, 0)) str_copy(ai_model, AI_MODEL_ALT, sizeof(ai_model));
    else                             str_copy(ai_model, AI_MODEL_PATH, sizeof(ai_model));

    ai_loading_slot = 0;
    llm_slot_select(0);
    str_copy(ai_slot_path[0], ai_model, sizeof(ai_slot_path[0]));
    ai_slot_path[1][0] = '\0';

    if (fs_open(ai_model, &ai_file) != 0) {
        /* Not an error — a machine with no model simply has no chat. But
         * silence here is why "the chat panel does nothing" took so long
         * to trace on the ARM tree, where this was never even called. */
        serial_puts("[ai] no model at ");
        serial_puts(ai_model);
        serial_putc('\n');
        return;
    }
    serial_puts("[ai] loading ");
    serial_puts(ai_model);
    serial_putc('\n');
    ai_state = AI_PARSE;
}

/*
 * Load a different model, without rebooting.
 *
 * The arena is a bump allocator that never frees, so a second model
 * cannot simply be loaded on top of the first -- it is reset instead,
 * which is safe precisely because nothing outside llm.c holds a pointer
 * into it and every tensor is rebound by the load that follows.
 */
static int ai_switch(const char *path) {
    if (!ai_arena_base) return -1;
    if (fs_open(path, &ai_file) != 0) return -1;
    llm_arena_init(ai_arena_base, ai_arena_size);
    str_copy(ai_model, path, sizeof(ai_model));
    ai_last_decile = -1;
    ai_state = AI_PARSE;
    serial_puts("[ai] switching to ");
    serial_puts(path);
    serial_putc('\n');
    return 0;
}

/*
 * Loading says so, on the serial line.
 *
 * "[ai] loading" was the only thing this ever printed. Success was
 * silent, failure was silent, and progress was silent -- so a load that
 * had finished, one that had died on a read error, and one that was
 * simply slow all looked identical from outside, and the only symptom
 * anyone could report was that the chat panel would not answer. Every
 * outcome is announced now, and the tenths are announced as they pass so
 * that "slow" and "stuck" can be told apart.
 */

/* Defined below, next to ai_busy; needed here to report the tenths. */
static int ai_progress(void);

static void ai_poll(void) {
    const char *err = "?";

    switch (ai_state) {
    case AI_PARSE:
        /* One shot: the tensor table has to be whole before anything
         * can be bound, and it is a small fraction of the file. */
        ai_t0 = cycle_now();
        if (llm_load(llm_read_thunk, &ai_file, ai_file.size, &err) != 0 ||
            llm_load_begin(&err) != 0) {
            ai_err = err;
            ai_state = AI_FAILED;
            serial_puts("[ai] load failed while parsing: ");
            serial_puts(err);
            serial_putc('\n');
            return;
        }
        ai_state = AI_WEIGHTS;
        return;

    case AI_WEIGHTS: {
        /*
         * A slice of the frame, not a number of chunks.
         *
         * This used to read four chunks per frame regardless of how long a
         * chunk took, and on an emulated machine one chunk is already more
         * than a frame — so the desktop ran at 1 fps for the entire load
         * and the pointer was unusable for the first minute after login,
         * which is precisely when someone is looking at it.
         *
         * Six milliseconds of a 16.6 ms frame leaves room for the
         * composite and the flip, so the load is invisible on a fast
         * machine and merely slow on a slow one, instead of taking the
         * whole desktop down with it.
         */
        uint64_t start = cycle_now();
        do {
            int r = llm_load_step(&err);
            if (r < 0) {
                ai_err = err;
                ai_state = AI_FAILED;
                serial_puts("[ai] load failed: ");
                serial_puts(err);
                serial_putc('\n');
                return;
            }
            if (r == 1) {
                serial_puts("[ai] slot ");
                serial_put_dec((uint32_t)ai_loading_slot);
                serial_puts(" ready in ");
                serial_put_dec(cycles_to_ms(cycle_now() - ai_t0));
                serial_puts(" ms: ");
                serial_puts(ai_slot_path[ai_loading_slot]);
                serial_putc('\n');

                /*
                 * The second model, if the volume has it. Loading it
                 * does not disturb the first: the arena is a bump
                 * allocator shared by both, so this continues where the
                 * last one stopped rather than overwriting it.
                 */
                const char *other = str_eq(ai_slot_path[0], AI_MODEL_ALT)
                                  ? AI_MODEL_PATH : AI_MODEL_ALT;
                if (ai_loading_slot == 0 && fs_stat(other, 0, 0) &&
                    fs_open(other, &ai_file) == 0) {
                    ai_loading_slot = 1;
                    llm_slot_select(1);
                    str_copy(ai_slot_path[1], other, sizeof(ai_slot_path[1]));
                    ai_last_decile = -1;
                    ai_t0 = cycle_now();
                    ai_state = AI_PARSE;
                    serial_puts("[ai] loading ");
                    serial_puts(other);
                    serial_puts(" as the second opinion\n");
                    return;
                }

                llm_slot_select(0);      /* answers come from slot 0 */
                ai_state = AI_READY;
                return;
            }
        } while (!budget_expired_ms(start, 6));

        {
            const int d = ai_progress() / 10;
            if (d != ai_last_decile) {
                ai_last_decile = d;
                serial_puts("[ai] ");
                serial_put_dec((uint32_t)ai_progress());
                serial_puts("%\n");
            }
        }
        return;
    }

    default:
        return;
    }
}

static int ai_busy(void) {
    return ai_state == AI_PARSE || ai_state == AI_WEIGHTS;
}

/* 0..100 across both phases; the parse counts as the first slice */
static int ai_progress(void) {
    if (ai_state == AI_READY) return 100;
    if (ai_state == AI_PARSE) return 0;
    if (ai_state != AI_WEIGHTS) return 0;
    int p = llm_load_progress();
    return p > 99 ? 99 : p;
}

static void term_exec(char *cmdline);

/* The chat engine lives in apps.h, which is included after this file;
 * the `ask` command reaches it through this. The question buffer's size
 * is stated here rather than there because `ask` has to build one before
 * apps.h exists; apps.h declares wiki_input with the same macro so the
 * two cannot drift. */
#define WIKI_INPUT_MAX 160
static int  wiki_ask(const char *question);

/* Whether a question also goes to the second model. Defined here rather
 * than in apps.h because `llm check` reaches it, and term.h is included
 * first. */
static int  wiki_xcheck = 1;

/*
 * The prompt now names who is at the keyboard, because more than one
 * person can be. Falls back to the bare machine name when no one is
 * logged in, which is what the harness builds see.
 */
static void term_build_prompt(char *out, int max) {
    if (user_current >= 0) {
        str_copy(out, user_name_of(user_current), max);
        str_append(out, "@vextro:", max);
    } else {
        str_copy(out, "vextro:", max);
    }
    str_append(out, term_cwd, max);
    str_append(out, "> ", max);
}


/* ===== security and archives =====
 *
 * Driven from the shell rather than only from a settings pane, because
 * these are the commands an administrator reaches for, and because a
 * passphrase typed as an argument is at least visible -- there is no
 * hidden prompt here pretending to be more private than it is.
 */
static void sec_require_admin(int *ok) {
    *ok = (user_current < 0) || user_is_admin(user_current);
    if (!*ok) term_print_c("that needs an administrator account\n", 2);
}

static void cmd_policy(int argc, char **argv) {
    if (argc < 2) {
        term_print_c("Policy\n", 1);
        term_print("  prompts    ");
        term_print(uac_level_names[uac_level]);
        term_print("\n  allow list ");
        term_print(allowlist_on ? "on" : "off");
        term_print("\n  scanner    ");
        term_print(scanner_on ? "on" : "off");

        /*
         * What the prompts have actually decided.
         *
         * The sentence that used to close this panel said that none of
         * it isolated a running program and that all of it only decided
         * whether one started. That was exactly true for as long as
         * there were no system calls a program could use to change the
         * machine. There are three now -- writing a file, writing the
         * registry, writing a block -- and every one of them goes
         * through the gateway, so the honest summary is different and
         * the numbers below are the evidence for it.
         */
        {
            char nb[12];
            term_print("\n\nElevation requests: ");
            uint_to_str(uac_grants, nb);   term_print(nb);
            term_print(" allowed, ");
            uint_to_str(uac_denials, nb);  term_print(nb);
            term_print(" refused\n");
        }
        term_print("\nEvery program starts with a restricted token, whoever\n"
                   "launched it. The allow list and the scanner decide\n"
                   "whether one starts; the prompts decide what it may do\n"
                   "to this machine afterwards. See 'help policy'.\n");
        return;
    }
    int ok; sec_require_admin(&ok); if (!ok) return;

    if (str_eq(argv[1], "prompts") && argc >= 3) {
        int v = argv[2][0] - '0';
        if (v < 0 || v >= UAC_LEVELS) {
            term_print_c("levels are 0..3\n", 2);
            for (int i = 0; i < UAC_LEVELS; i++) {
                char nb[4]; uint_to_str((uint32_t)i, nb);
                term_print("  "); term_print(nb); term_print("  ");
                term_print(uac_level_names[i]); term_print("\n");
            }
            return;
        }
        uac_level = v;
    } else if (str_eq(argv[1], "allowlist") && argc >= 3) {
        allowlist_on = str_eq(argv[2], "on");
    } else if (str_eq(argv[1], "scanner") && argc >= 3) {
        scanner_on = str_eq(argv[2], "on");
    } else {
        term_print_c("usage: policy [prompts <0-3>|allowlist on|off|"
                     "scanner on|off]\n", 2);
        return;
    }
    policy_save();
    term_print_c("saved\n", 3);
}

static void cmd_allow(int argc, char **argv) {
    if (argc < 2 || str_eq(argv[1], "list")) {
        if (allow_count == 0) { term_print("the allow list is empty\n"); return; }
        for (int i = 0; i < allow_count; i++) {
            term_print("  "); term_print(allow_names[i]); term_print("\n");
        }
        return;
    }
    int ok; sec_require_admin(&ok); if (!ok) return;
    if (str_eq(argv[1], "add") && argc >= 3) {
        if (!allow_add(argv[2])) { term_print_c("the list is full\n", 2); return; }
    } else if (str_eq(argv[1], "remove") && argc >= 3) {
        if (!allow_remove(argv[2])) { term_print_c("not on the list\n", 2); return; }
    } else {
        term_print_c("usage: allow [list|add <name>|remove <name>]\n", 2);
        return;
    }
    policy_save();
    term_print_c("saved\n", 3);
}

static void cmd_scan(int argc, char **argv) {
    const char *root = argc >= 2 ? argv[1] : "/apps";
    const int n = vw_collect(root);
    if (n == 0) { term_print("nothing to scan under "); term_print(root);
                  term_print("\n"); return; }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        uint64_t sz = 0;
        const void *d = fs_read_file(vw_files[i], &sz);
        if (!d) continue;
        const int v = scan_buffer((const uint8_t *)d, (uint32_t)sz);
        if (v != SCAN_CLEAN) {
            bad++;
            term_print_c("  THREAT  ", 2);
            term_print(vw_files[i]);
            term_print("  ");
            term_print_c(scan_detail, 2);
            term_print("\n");
        }
    }
    char nb[12];
    uint_to_str((uint32_t)n, nb);
    term_print("\n  scanned "); term_print(nb);
    uint_to_str((uint32_t)bad, nb);
    term_print(" files, "); term_print(nb); term_print(" flagged\n");
    if (bad) {
        char note[NOTIFY_TEXT];
        str_copy(note, "Scan flagged ", sizeof(note));
        str_append(note, nb, sizeof(note));
        str_append(note, " file(s)", sizeof(note));
        notify_push(NOTE_WARN, note);
    } else {
        notify_push(NOTE_GOOD, "Scan found nothing");
    }
}

static void cmd_vault(int argc, char **argv) {
    if (argc < 4) {
        term_print_c("usage: vault seal <dir> <file.vault> <passphrase>\n"
                     "       vault open <file.vault> <dir> <passphrase>\n", 2);
        return;
    }
    uint32_t files = 0;
    const char *err;
    char a[256], b[256];
    term_resolve(argv[2], a);
    term_resolve(argv[3], b);

    if (str_eq(argv[1], "seal")) {
        if (argc < 5) { term_print_c("a passphrase is required\n", 2); return; }
        err = vault_seal(a, b, argv[4], &files);
    } else if (str_eq(argv[1], "open")) {
        if (argc < 5) { term_print_c("a passphrase is required\n", 2); return; }
        err = vault_unseal(a, b, argv[4], &files);
    } else {
        term_print_c("vault: seal or open\n", 2);
        return;
    }
    if (err) { term_print_c("vault: ", 2); term_print_c(err, 2);
               term_print("\n"); return; }
    char nb[12];
    uint_to_str(files, nb);
    term_print_c("  ", 3); term_print(nb);
    term_print(str_eq(argv[1], "seal") ? " file(s) sealed into "
                                       : " file(s) restored to ");
    term_print(b); term_print("\n");
}

static void cmd_backup(int argc, char **argv) {
    /*
     * Refused rather than defaulted. With no account signed in this used
     * to fall back to "/", which is not a home directory -- it is the
     * whole volume, encyclopedia and model included, and the only reason
     * it failed instead of trying was that the buffer ran out.
     */
    if (user_current < 0) {
        term_print_c("backup: no account is signed in, so there is no home "
                     "directory to archive\n", 2);
        return;
    }
    char home[128];
    user_home(user_current, home, sizeof(home));

    const char *dest = argc >= 2 ? argv[1] : "/home.vault";
    const char *pass = argc >= 3 ? argv[2] : 0;
    if (!pass) {
        term_print_c("usage: backup [file.vault] <passphrase>\n"
                     "  archives this account's home directory, encrypted\n", 2);
        return;
    }
    char abs[256];
    term_resolve(dest, abs);
    uint32_t files = 0;
    const char *err = vault_seal(home, abs, pass, &files);
    if (err) { term_print_c("backup: ", 2); term_print_c(err, 2);
               term_print("\n"); return; }
    char nb[12]; uint_to_str(files, nb);
    term_print_c("  backed up ", 3); term_print(nb);
    term_print(" file(s) to "); term_print(abs); term_print("\n");
    char note[NOTIFY_TEXT];
    str_copy(note, "Backed up ", sizeof(note));
    str_append(note, nb, sizeof(note));
    str_append(note, " file(s)", sizeof(note));
    notify_push(NOTE_GOOD, note);
}

static void cmd_restore(int argc, char **argv) {
    if (argc < 3) {
        term_print_c("usage: restore <file.vault> <passphrase>\n", 2);
        return;
    }
    if (user_current < 0) {
        term_print_c("restore: no account is signed in\n", 2);
        return;
    }
    char home[128];
    user_home(user_current, home, sizeof(home));
    char abs[256];
    term_resolve(argv[1], abs);
    uint32_t files = 0;
    const char *err = vault_unseal(abs, home, argv[2], &files);
    if (err) { term_print_c("restore: ", 2); term_print_c(err, 2);
               term_print("\n"); return; }
    char nb[12]; uint_to_str(files, nb);
    term_print_c("  restored ", 3); term_print(nb);
    term_print(" file(s) to "); term_print(home); term_print("\n");
}


/* ===== audio =====
 *
 * A tone is generated rather than loaded so that "does the sound path
 * work" can be answered without a file on the volume: the integer sine
 * table drives it, since there is no FPU here.
 */
#define TONE_RATE     48000u
#define TONE_SAMPLES  (TONE_RATE / 2u * 2u)   /* half a second, stereo */
static int16_t tone_buf[TONE_SAMPLES];

static void cmd_beep(int argc, char **argv) {
#ifndef VEXTRO_HAVE_AUDIO
    /* This port has no sound device at all, so there is nothing to
     * configure and nothing to fix -- say which, rather than failing as
     * though something were wrong. */
    (void)argc; (void)argv;
    term_print_c("this port has no audio device\n", 2);
}
#else
    if (!ac97_found) {
        term_print_c("no AC97 device on this machine\n", 2);
        return;
    }
    uint32_t hz = 440;
    if (argc >= 2) {
        hz = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            hz = hz * 10 + (uint32_t)(*p - '0');
        if (hz < 20 || hz > 12000) { term_print_c("pick 20..12000 Hz\n", 2); return; }
    }

    /*
     * A square-ish wave built from the integer sine table. The phase is
     * carried in whole degrees scaled by 1024 so it never drifts: a
     * per-sample increment of hz*360/rate would round away and the pitch
     * would come out wrong at anything but round frequencies.
     */
    uint32_t phase = 0;
    const uint32_t inc = (hz * 360u * 1024u) / TONE_RATE;
    for (uint32_t i = 0; i < TONE_SAMPLES; i += 2) {
        const int32_t deg = (int32_t)((phase >> 10) % 360u);
        int32_t v = int_sin[deg] * 8;            /* 1024 -> ~8192, quiet */
        /* fade the last eighth so it does not end on a click */
        const uint32_t tail = TONE_SAMPLES / 8;
        if (i > TONE_SAMPLES - tail)
            v = v * (int32_t)(TONE_SAMPLES - i) / (int32_t)tail;
        tone_buf[i]     = (int16_t)v;            /* left  */
        tone_buf[i + 1] = (int16_t)v;            /* right */
        phase += inc;
    }

    if (ac97_play(tone_buf, TONE_SAMPLES, TONE_RATE) != 0) {
        term_print_c("the device refused the buffer\n", 2);
        return;
    }
    char nb[12];
    uint_to_str(hz, nb);
    term_print("  playing "); term_print(nb); term_print(" Hz for 0.5 s\n");
}
#endif

static void term_run_command(void) {
    /* Echo prompt + command into the scrollback */
    char prompt[64];
    term_build_prompt(prompt, sizeof(prompt));
    term_print_c(prompt, 1);
    term_cur_color = 0;
    term_print(term_input);
    term_putc('\n');

    if (term_input_len > 0) {
        /* Save to history (skip duplicates of last entry) */
        if (term_hist_count == 0 ||
            !str_eq(term_hist[(term_hist_count - 1) % TERM_HIST], term_input)) {
            str_copy(term_hist[term_hist_count % TERM_HIST], term_input,
                     TERM_INPUT_MAX);
            term_hist_count++;
        }
        char buf[TERM_INPUT_MAX];
        str_copy(buf, term_input, TERM_INPUT_MAX);

        /* --- output redirection: cmd > file / cmd >> file --- */
        int gt = -1;
        for (int i = 0; buf[i]; i++)
            if (buf[i] == '>') { gt = i; break; }

        if (gt > 0) {
            int append = (buf[gt + 1] == '>');
            char target[128];
            const char *t = buf + gt + (append ? 2 : 1);
            while (*t == ' ') t++;
            str_copy(target, t, sizeof(target));
            int tl = str_len(target);
            while (tl > 0 && target[tl - 1] == ' ')
                target[--tl] = '\0';
            buf[gt] = '\0';

            if (tl == 0) {
                term_print_c("syntax: <command> > <file>\n", 2);
            } else if (term_async != TERM_ASYNC_NONE) {
                term_print_c("cannot redirect async commands\n", 2);
            } else {
                term_cap_len = 0;
                term_redirect_active = 1;
                term_exec(buf);
                term_redirect_active = 0;

                if (term_async != TERM_ASYNC_NONE) {
                    /* an async command slipped through — cancel it */
                    term_async = TERM_ASYNC_NONE;
                    ping_active = 0;
                    term_print_c("cannot redirect async commands\n", 2);
                } else {
                    char abs[256];
                    term_resolve(target, abs);
                    const uint8_t *out_data = (const uint8_t *)term_cap;
                    uint32_t out_len = (uint32_t)term_cap_len;

                    if (append) {
                        uint64_t old_len = 0;
                        const void *old = fs_read_file(abs, &old_len);
                        if (old) {
                            uint32_t n = 0;
                            const uint8_t *op = (const uint8_t *)old;
                            while (n < old_len && n < FS_FILEBUF_MAX)
                                { term_append_buf[n] = op[n]; n++; }
                            uint32_t m = 0;
                            while (m < out_len && n + m < FS_FILEBUF_MAX)
                                { term_append_buf[n + m] = out_data[m]; m++; }
                            out_data = term_append_buf;
                            out_len = n + m;
                        }
                    }
                    if (fs_write_file(abs, out_data, out_len) != 0) {
                        term_print_c("write failed: ", 2);
                        term_print_c(fs_errstr, 2);
                        term_putc('\n');
                    } else {
                        char nb[16];
                        uint_to_str(out_len, nb);
                        term_print_c("wrote ", 3);
                        term_print_c(nb, 3);
                        term_print_c(" bytes to ", 3);
                        term_print_c(abs, 3);
                        term_putc('\n');
                    }
                }
            }
        } else {
            term_exec(buf);
        }
    }
    if (term_async == TERM_ASYNC_NONE)
        term_prompt_begin();
    else {
        term_input_len = 0;
        term_input_cur = 0;
        term_input[0] = '\0';
    }
}

/* The Unix toolset. Needs term_resolve and the term_print helpers
 * above, so it is included here rather than at the top. */
#include "coreutils.h"

static void term_exec(char *cmdline) {
    /* Room for flags as well as operands: `grep -i -n pat file` is four,
     * and the coreutils below take more. */
    char *argv[16];
    int argc = term_split(cmdline, argv, 16);
    if (argc == 0) return;
    const char *cmd = argv[0];

    if (str_eq(cmd, "help")) {
        term_cmd_help();
    } else if (str_eq(cmd, "clear")) {
        term_clear();
    } else if (str_eq(cmd, "about")) {
        term_print_c("Vextro 9\n", 1);
        term_print("A bare-metal x86_64 operating system.\n");
        term_print("TrueType rasterizer, window manager, TCP/IP stack,\n");
        term_print("HTTP browser and PS/2 HAL - no libc, no floats.\n");
    } else if (str_eq(cmd, "uname")) {
        term_print("Vextro 9.0 x86_64 bare-metal\n");
    } else if (str_eq(cmd, "ls") || str_eq(cmd, "dir")) {
        term_cmd_ls(argc >= 2 ? argv[1] : 0);
    } else if (str_eq(cmd, "cat")) {
        if (argc < 2) term_print_c("usage: cat <file>\n", 2);
        else term_cmd_cat(argv[1]);
    } else if (str_eq(cmd, "cd")) {
        char abs[256];
        term_resolve(argc >= 2 ? argv[1] : "/", abs);
        if (!fs_writable()) {
            term_print_c("cd: no mounted volume\n", 2);
        } else {
            int is_dir = 0;
            if (fs_stat(abs, 0, &is_dir) && is_dir) {
                str_copy(term_cwd, abs, sizeof(term_cwd));
            } else {
                term_print_c("cd: no such directory: ", 2);
                term_print_c(abs, 2);
                term_putc('\n');
            }
        }
    } else if (str_eq(cmd, "pwd")) {
        term_print(term_cwd);
        term_putc('\n');
    } else if (str_eq(cmd, "rm") || str_eq(cmd, "rmdir")) {
        if (argc < 2) { term_print_c("usage: rm <file|empty dir>\n", 2); return; }
        char abs[256];
        term_resolve(argv[1], abs);
        if (fs_delete(abs) != 0) {
            term_print_c("rm: ", 2);
            term_print_c(fs_errstr, 2);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "mkdir")) {
        if (argc < 2) { term_print_c("usage: mkdir <dir>\n", 2); return; }
        char abs[256];
        term_resolve(argv[1], abs);
        if (fs_mkdir(abs) != 0) {
            term_print_c("mkdir: ", 2);
            term_print_c(fs_errstr, 2);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "cp")) {
        if (argc < 3) { term_print_c("usage: cp <src> <dst>\n", 2); return; }
        char src[256], dst[256];
        term_resolve(argv[1], src);
        term_resolve(argv[2], dst);
        uint64_t len = 0;
        const void *data = fs_read_file(src, &len);
        if (!data) {
            term_print_c("cp: not found: ", 2);
            term_print_c(src, 2);
            term_putc('\n');
        } else if (fs_write_file(dst, data, (uint32_t)len) != 0) {
            term_print_c("cp: ", 2);
            term_print_c(fs_errstr, 2);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "df") || str_eq(cmd, "disk")) {
        term_cmd_df();
    } else if (str_eq(cmd, "gpu")) {
        if (argc >= 2 && str_eq(argv[1], "error")) {
            term_cmd_gpu_error();
        } else if (argc >= 2 && str_eq(argv[1], "bench")) {
            term_cmd_gpu_bench();
        } else if (argc >= 2 && str_eq(argv[1], "decode")) {
            uint32_t dw;
            if (argc < 3 || !term_parse_hex(argv[2], &dw)) {
                term_print_c("usage: gpu decode <hex dword>\n", 2);
            } else {
                char name[48];
                igpu_decode_cmd(dw, name, sizeof(name));
                term_print("  ");
                term_print_hex32(dw);
                term_print("  ->  ");
                term_print_c(name, 1);
                char nb[8];
                term_print_c("  (len field ", 3);
                uint_to_str(dw & 0xFF, nb);
                term_print_c(nb, 3);
                term_print_c(")\n", 3);
            }
        } else if (argc >= 2 && str_eq(argv[1], "test")) {
            if (!igpu.active) {
                term_print_c("gpu test: no active iGPU (CPU renderer)\n", 2);
            } else if (!igpu.fb_blittable) {
                term_print_c("gpu test: framebuffer not GGTT-reachable\n", 2);
            } else {
                /* blit a tile onto the live framebuffer, verify by CPU */
                int tx = 40, ty = 60, tw = 120, th = 80;
                gfx_force_full_flip = 1;   /* the GPU writes the panel directly */
                if (igpu_screen_fill(tx, ty, tw, th, 0x00D4AF37) != 0) {
                    term_print_c("gpu test: blit submission failed\n", 2);
                } else {
                    volatile uint32_t *px = (volatile uint32_t *)
                        phys_to_virt(igpu.fb_phys +
                                     (uint64_t)ty * igpu.fb_pitch_bytes +
                                     (uint64_t)tx * 4);
                    int ok = 1;
                    for (int i = 0; i < tw; i++)
                        if ((px[i] & 0xFFFFFF) != 0xD4AF37) { ok = 0; break; }
                    if (ok)
                        term_print_c("XY_COLOR_BLT hit the live framebuffer"
                                     " - verified by CPU readback\n", 4);
                    else
                        term_print_c("blit submitted but pixels mismatch\n", 2);
                }
            }
        } else {
            term_print("  device    ");
            if (igpu.name) {
                term_print(igpu.name);
                term_putc('\n');
            } else {
                term_print_c("none detected\n", 3);
            }
            term_print("  status    ");
            term_print_c(igpu.status, igpu.active ? 4 : 3);
            term_putc('\n');
            if (igpu.active) {
                char nb[16];
                term_print("  ggtt      ");
                uint_to_str(igpu.ggtt_slots / 256, nb);   /* slots→MB */
                term_print(nb);
                term_print(" MB of GPU address space\n");
                term_print("  screen    ");
                term_print(igpu.fb_blittable ?
                           "blitter can write the framebuffer\n" :
                           "offscreen surfaces only\n");

                /*
                 * The compositor's own use of the engine, which is a
                 * different question from whether the engine works.
                 * A batch is refused rather than truncated when it
                 * overflows or the power well will not wake, and a
                 * refusal is silent to the eye -- the frame simply comes
                 * out on the processor instead -- so it is counted and
                 * reported here rather than left to be inferred from a
                 * frame rate.
                 */
                term_print("  compose   ");
                if (igpu_comp_active()) {
                    uint_to_str(igpu_comp.frames, nb);
                    term_print(nb);
                    term_print(" batches submitted, ");
                    uint_to_str(igpu_comp.refused, nb);
                    term_print(nb);
                    term_print(" refused\n");
                } else {
                    term_print("back buffer not mapped; CPU compositing\n");
                }
                term_print_c("  try 'gpu test' to blit to the screen\n", 3);
            } else {
                term_print("  renderer  CPU (portable framebuffer path)\n");
            }
            if (igpu_crash.valid) {
                term_print_c("  last hang ", 2);
                term_print_c(igpu_crash.cmd_name, 2);
                term_print_c("  ('gpu error' for the full report)\n", 3);
            }
        }
    } else if (str_eq(cmd, "run")) {
        if (argc < 2) term_print_c("usage: run <program>\n", 2);
        else {
            char abs[256];
            term_resolve(argv[1], abs);
            execute_bin(abs);
        }
    } else if (str_eq(cmd, "echo")) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) term_putc(' ');
            term_print(argv[i]);
        }
        term_putc('\n');
    } else if (str_eq(cmd, "date")) {
        term_cmd_date();
    } else if (str_eq(cmd, "uptime")) {
        term_cmd_uptime();
    } else if (str_eq(cmd, "mem")) {
        char buf[16];
        uint_to_str((uint32_t)system_total_memory_mb, buf);
        term_print("  total system memory: ");
        term_print(buf);
        term_print(" MB\n");
    } else if (str_eq(cmd, "mouse") || str_eq(cmd, "pointer")) {
        term_cmd_mouse();
    } else if (str_eq(cmd, "net") || str_eq(cmd, "ifconfig")) {
        term_cmd_net();
    } else if (str_eq(cmd, "wifi") || str_eq(cmd, "wlan")) {
        term_cmd_wifi(argc, argv);
    } else if (str_eq(cmd, "video")) {
        term_cmd_video(argc, argv);
    } else if (str_eq(cmd, "rdp")) {
        term_cmd_rdp(argc, argv);
    } else if (str_eq(cmd, "arp")) {
        term_cmd_arp();
    } else if (str_eq(cmd, "history")) {
        int start = term_hist_count > TERM_HIST ? term_hist_count - TERM_HIST : 0;
        for (int i = start; i < term_hist_count; i++) {
            char nb[8];
            uint_to_str((uint32_t)(i + 1), nb);
            term_print("  ");
            term_print_c(nb, 3);
            term_print("  ");
            term_print(term_hist[i % TERM_HIST]);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "ping")) {
        if (argc < 2) { term_print_c("usage: ping <host>\n", 2); return; }
        if (!e1000_found) { term_print_c("no network adapter\n", 2); return; }
        str_copy(term_async_arg, argv[1], sizeof(term_async_arg));
        term_async = TERM_ASYNC_PING;
        term_async_t0 = net_ticks;
        ping_active = 1;
        ping_seq = 0;
        ping_replies = 0;
        ping_sent_count = 0;
        ping_got_reply = 0;
        term_ping_next_at = 0;   /* resolve first */
        dns_resolve_start(term_async_arg);
        term_print("PING ");
        term_print(term_async_arg);
        term_print(" ...\n");
    } else if (str_eq(cmd, "dns") || str_eq(cmd, "nslookup")) {
        if (argc < 2) { term_print_c("usage: dns <host>\n", 2); return; }
        if (!e1000_found) { term_print_c("no network adapter\n", 2); return; }
        str_copy(term_async_arg, argv[1], sizeof(term_async_arg));
        term_async = TERM_ASYNC_DNS;
        term_async_t0 = net_ticks;
        dns_resolve_start(term_async_arg);
        term_print("resolving ");
        term_print(term_async_arg);
        term_print(" ...\n");
    } else if (str_eq(cmd, "fetch") || str_eq(cmd, "curl")) {
        if (argc < 2) { term_print_c("usage: fetch <url>\n", 2); return; }
        if (!e1000_found) { term_print_c("no network adapter\n", 2); return; }
        char host[128], path[256];
        uint16_t port;
        if (!http_parse_url(argv[1], host, sizeof(host), &port,
                            path, sizeof(path))) {
            term_print_c("fetch: only http:// urls are supported\n", 2);
            return;
        }
        term_async = TERM_ASYNC_FETCH;
        term_async_t0 = net_ticks;
        http_get(host, port, path);
        http_owner = HTTP_OWNER_TERM;
        term_print("fetching http://");
        term_print(host);
        term_print(path);
        term_print(" ...\n");
    } else if (str_eq(cmd, "peek")) {
        /* Read a window out of a file without loading the whole thing —
         * the only way to look inside an archive larger than any buffer,
         * which is what exFAT's 64-bit sizes now allow. */
        if (argc < 3) {
            term_print_c("usage: peek <file> <offset> [bytes]\n", 2);
            return;
        }
        char abs[256];
        term_resolve(argv[1], abs);
        uint64_t off = 0;
        for (const char *q = argv[2]; *q >= '0' && *q <= '9'; q++)
            off = off * 10 + (uint64_t)(*q - '0');
        uint32_t want = 256;
        if (argc >= 4) {
            want = 0;
            for (const char *q = argv[3]; *q >= '0' && *q <= '9'; q++)
                want = want * 10 + (uint32_t)(*q - '0');
        }
        if (want > 1024) want = 1024;

        static uint8_t peek_buf[1024];
        uint32_t got = 0;
        if (fs_read_range(abs, off, peek_buf, want, &got) != 0) {
            term_print_c("peek: ", 2);
            term_print_c(fs_errstr, 2);
            term_putc('\n');
            return;
        }
        if (got == 0) { term_print_c("(offset is past the end)\n", 3); return; }
        char nb[16];
        term_print_c("  ", 3);
        uint_to_str(got, nb); term_print_c(nb, 3);
        term_print_c(" bytes at offset ", 3);
        uint_to_str((uint32_t)off, nb); term_print_c(nb, 3);
        term_putc('\n');
        for (uint32_t i = 0; i < got; i++) {
            char c = (char)peek_buf[i];
            term_putc((c >= 0x20 && c < 0x7F) || c == '\n' ? c : '.');
        }
        if (term_cx > 0) term_putc('\n');
    } else if (str_eq(cmd, "llm")) {
        static fs_file_t llm_file;
        if (argc >= 2 && str_eq(argv[1], "fpu")) {
            uint32_t v = 0;
            int ok = llm_fpu_selftest(&v);
            term_print("  sum 1/n^2 to 20000 = ");
            /* the integer-only side never touches a float, so the value
             * arrives pre-scaled and is split by division */
            uint32_t whole = v / 10000, frac = v % 10000;
            char nb[16];
            uint_to_str(whole, nb); term_print(nb);
            term_putc('.');
            if (frac < 1000) term_putc('0');
            if (frac < 100) term_putc('0');
            if (frac < 10) term_putc('0');
            uint_to_str(frac, nb); term_print(nb);
            term_print_c(ok == 0 ? "   FPU OK (expected 1.6449)\n"
                                 : "   WRONG - SSE is not working\n",
                         ok == 0 ? 4 : 2);
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "bench")) {
            uint64_t d = 0, o = 0, b = 0;
            llm_bench(&d, &o, &b);
            if (!b) { term_print_c("load a model first\n", 2); return; }
            char nb[16];
            term_print("  one ffn_up matmul (Q5_0, 51% of the weights)\n");
            term_print("  dequantise only   ");
            uint_to_str(cycles_to_us(d), nb); term_print(nb); term_print(" us\n");
            term_print("  arithmetic only   ");
            uint_to_str(cycles_to_us(o), nb); term_print(nb); term_print(" us\n");
            term_print("  as the model runs ");
            uint_to_str(cycles_to_us(b), nb); term_print(nb); term_print(" us\n");
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "check")) {
            if (argc >= 3) wiki_xcheck = !str_eq(argv[2], "off");
            term_print("  cross-check ");
            term_print_c(wiki_xcheck ? "on" : "off", wiki_xcheck ? 4 : 3);
            term_print("\n  slot 0 ");
            term_print(ai_slot_path[0][0] ? ai_slot_path[0] : "(empty)");
            term_print(llm_slot_loaded(0) ? "  resident\n" : "  loading\n");
            term_print("  slot 1 ");
            term_print(ai_slot_path[1][0] ? ai_slot_path[1] : "(none)");
            term_print(llm_slot_loaded(1) ? "  resident\n" : "  not loaded\n");
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "use")) {
            if (argc < 3) {
                term_print("  in use: ");
                term_print_c(ai_model, 1);
                term_print("\n  usage: llm use explain|qwen2|<path>\n");
                return;
            }
            const char *path = argv[2];
            if (str_eq(path, "explain")) path = AI_MODEL_ALT;
            else if (str_eq(path, "qwen2")) path = AI_MODEL_PATH;
            if (ai_switch(path) != 0) {
                term_print_c("cannot open ", 2);
                term_print_c(path, 2);
                term_putc('\n');
            } else {
                term_print_c("loading ", 3);
                term_print_c(path, 3);
                term_print_c(" - ask again once it is resident\n", 3);
            }
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "weights")) {
            const char *werr = "?";
            term_print_c("loading weights (this reads the whole model)...\n", 3);
            if (llm_load_weights(&werr) != 0) {
                term_print_c("llm: ", 2); term_print_c(werr, 2); term_putc('\n');
                return;
            }
            term_print_c("weights resident\n", 4);
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "eval")) {
            if (argc < 3) { term_print_c("usage: llm eval <token> [pos]\n", 2); return; }
            /*
             * Without this the command runs the forward pass against
             * whatever is in the weight buffer -- zeros, before the load
             * finishes -- and prints a confident argmax computed from
             * nothing. Silently wrong is worse than refusing.
             */
            if (!llm_weights_loaded()) {
                term_print_c("no weights resident; run 'llm load' or wait "
                             "for the model to finish loading\n", 2);
                return;
            }
            int32_t tk = 0;
            for (const char *q = argv[2]; *q >= '0' && *q <= '9'; q++) tk = tk * 10 + (*q - '0');
            int pos = 0;
            if (argc >= 4) { pos = 0;
                for (const char *q = argv[3]; *q >= '0' && *q <= '9'; q++) pos = pos * 10 + (*q - '0'); }
            if (llm_eval(tk, pos) != 0) { term_print_c("eval failed\n", 2); return; }
            int best = llm_argmax();
            char piece[64];
            llm_decode(best, piece, sizeof(piece));
            char nb[16];
            term_print("  argmax ");
            uint_to_str((uint32_t)best, nb); term_print_c(nb, 1);
            term_print(" [");
            for (int k = 0; piece[k]; k++) term_putc(piece[k] == ' ' ? '_' : piece[k]);
            term_print("]\n");
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "probe")) {
            if (argc < 3) { term_print_c("usage: llm probe <x|xb|q|k|logits> [n]\n", 2); return; }
            int n = 6;
            if (argc >= 4) { n = 0;
                for (const char *q = argv[3]; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
                if (n < 1) n = 1;
                if (n > 12) n = 12;
            }
            static int32_t vals[12];
            if (llm_probe(argv[2], 0, n, vals) < 0) { term_print_c("no such probe\n", 2); return; }
            char nb[16];
            for (int i = 0; i < n; i++) {
                int32_t v = vals[i];
                term_print("   ");
                if (v < 0) { term_putc('-'); v = -v; }
                uint_to_str((uint32_t)(v / 1000000), nb); term_print(nb);
                term_putc('.');
                uint32_t fr = (uint32_t)(v % 1000000);
                for (uint32_t d = 100000; d >= 1; d /= 10) {
                    term_putc((char)('0' + (fr / d) % 10));
                    if (d == 1) break;
                }
                term_putc('\n');
            }
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "deq")) {
            if (argc < 3) { term_print_c("usage: llm deq <tensor> [n]\n", 2); return; }
            int ti = llm_tensor_find(argv[2]);
            if (ti < 0) { term_print_c("no such tensor\n", 2); return; }
            int n = 6;
            if (argc >= 4) {
                n = 0;
                for (const char *q = argv[3]; *q >= '0' && *q <= '9'; q++)
                    n = n * 10 + (*q - '0');
                if (n < 1) n = 1;
                if (n > 12) n = 12;
            }
            static int32_t vals[12];
            if (llm_tensor_peek(ti, 0, n, vals) < 0) {
                term_print_c("dequantise failed\n", 2);
                return;
            }
            char nb[16];
            term_print("  ");
            term_print_c(llm_tensor_name(ti), 1);
            term_print("  ");
            term_print(llm_quant_name(llm_tensor_type(ti)));
            term_print("  ");
            uint_to_str((uint32_t)llm_tensor_elems(ti), nb);
            term_print(nb);
            term_print(" elems\n");
            for (int i = 0; i < n; i++) {
                int32_t v = vals[i];
                term_print("   ");
                if (v < 0) { term_putc('-'); v = -v; }
                uint_to_str((uint32_t)(v / 1000000), nb); term_print(nb);
                term_putc('.');
                uint32_t f = (uint32_t)(v % 1000000);
                for (uint32_t d = 100000; d >= 1; d /= 10) {
                    term_putc((char)('0' + (f / d) % 10));
                    if (d == 1) break;
                }
                term_putc('\n');
            }
            return;
        }
        if (argc >= 2 && str_eq(argv[1], "tok")) {
            if (!llm_tok_ready()) {
                term_print_c("no tokenizer loaded (llm load <file.gguf>)\n", 2);
                return;
            }
            /* rejoin the argv the splitter took apart */
            char text[240];
            text[0] = '\0';
            for (int i = 2; i < argc; i++) {
                if (i > 2) str_append(text, " ", sizeof(text));
                str_append(text, argv[i], sizeof(text));
            }
            if (text[0] == '\0') { term_print_c("usage: llm tok <text>\n", 2); return; }

            static int32_t ids[256];
            int n = llm_encode(text, ids, 256);
            if (n < 0) { term_print_c("encode failed\n", 2); return; }

            char nb[16];
            term_print("  ");
            uint_to_str((uint32_t)n, nb);
            term_print_c(nb, 1);
            term_print(" tokens\n");
            for (int i = 0; i < n; i++) {
                char piece[64];
                llm_decode(ids[i], piece, sizeof(piece));
                term_print("   ");
                uint_to_str((uint32_t)ids[i], nb);
                term_print_c(nb, 3);
                term_print(" ");
                term_putc('[');
                for (int k = 0; piece[k]; k++)
                    term_putc(piece[k] == ' ' ? '_' : piece[k]);
                term_print("]\n");
            }
            /* decode everything back and compare with the input */
            char round[240];
            int ro = 0;
            for (int i = 0; i < n; i++) {
                char piece[64];
                llm_decode(ids[i], piece, sizeof(piece));
                for (int k = 0; piece[k] && ro < (int)sizeof(round) - 1; k++)
                    round[ro++] = piece[k];
            }
            round[ro] = '\0';
            term_print_c(str_eq(round, text) ? "  round trip OK\n"
                                             : "  ROUND TRIP MISMATCH\n",
                         str_eq(round, text) ? 4 : 2);
            return;
        }
        /* The background loader owns the arena while it runs; letting a
         * manual load re-parse underneath it would leave the streaming
         * step filling a buffer nothing points at any more. */
        if (ai_busy() && argc >= 2 &&
            (str_eq(argv[1], "load") || str_eq(argv[1], "weights"))) {
            char nb[8];
            uint_to_str((uint32_t)ai_progress(), nb);
            term_print_c("the model is already loading in the background - ", 3);
            term_print_c(nb, 3);
            term_print_c("%\n", 3);
            return;
        }

        if (argc >= 2 && str_eq(argv[1], "load")) {
            if (argc < 3) { term_print_c("usage: llm load <model.gguf>\n", 2); return; }
            char abs[256];
            term_resolve(argv[2], abs);
            if (fs_open(abs, &llm_file) != 0) {
                term_print_c("llm: cannot open ", 2);
                term_print_c(abs, 2);
                term_putc('\n');
                return;
            }
            const char *lerr = "?";
            term_print_c("parsing GGUF...\n", 3);
            if (llm_load(llm_read_thunk, &llm_file, llm_file.size, &lerr) != 0) {
                term_print_c("llm: ", 2);
                term_print_c(lerr, 2);
                term_putc('\n');
                return;
            }
        }
        const llm_info_t *mi = llm_get_info();
        if (!mi->loaded) {
            term_print_c("no model loaded (llm load <file.gguf>)\n", 2);
            return;
        }
        char nb[24];
        term_print("  arch       "); term_print_c(mi->arch, 1);
        term_print("  ("); term_print(mi->name); term_print(")\n");
        term_print("  layers     "); uint_to_str(mi->n_layer, nb); term_print(nb);
        term_print("   embd "); uint_to_str(mi->n_embd, nb); term_print(nb);
        term_print("   ff "); uint_to_str(mi->n_ff, nb); term_print(nb); term_putc('\n');
        term_print("  heads      "); uint_to_str(mi->n_head, nb); term_print(nb);
        term_print(" q / "); uint_to_str(mi->n_head_kv, nb); term_print(nb);
        term_print(" kv   vocab "); uint_to_str(mi->n_vocab, nb); term_print(nb);
        term_putc('\n');
        term_print("  tensors    "); uint_to_str((uint32_t)mi->n_tensors, nb);
        term_print(nb); term_print("   weights ");
        uint_to_str((uint32_t)(mi->weight_bytes / (1024 * 1024)), nb);
        term_print_c(nb, 1); term_print(" MB of ");
        uint_to_str((uint32_t)(mi->file_size / (1024 * 1024)), nb);
        term_print(nb); term_print(" MB file\n");
        term_print("  quant      ");
        for (uint32_t t = 0; t < 32; t++) {
            if (!mi->quant_counts[t]) continue;
            term_print(llm_quant_name(t));
            term_putc('*');
            uint_to_str(mi->quant_counts[t], nb);
            term_print(nb);
            term_putc(' ');
        }
        term_putc('\n');
        if (llm_tok_ready()) {
            term_print("  tokenizer  ");
            uint_to_str(llm_tok_count(), nb); term_print_c(nb, 1);
            term_print(" tokens, ");
            uint_to_str(llm_merge_count(), nb); term_print(nb);
            term_print(" merges\n");
        }
        term_print("  arena      ");
        uint_to_str((uint32_t)(llm_arena_total() / (1024 * 1024)), nb);
        term_print_c(nb, 4);
        term_print(" MB free for weights\n");
    } else if (str_eq(cmd, "ask")) {
        /*
         * The chat panel, from the shell.
         *
         * Every other subsystem here is driveable both ways — `zim`,
         * `store`, `img`, `fetch` all have shell equivalents of what the
         * windows do — and the one that was reachable only by clicking a
         * bubble was also the one that took longest to get right, because
         * it could not be scripted or tested without a pointer.
         *
         * The answer lands in the Wikipedia window's transcript as usual;
         * this only submits it and returns, because generation is paced
         * across frames and blocking the shell on it would freeze the
         * desktop for exactly as long as the answer takes.
         */
        if (argc < 2) { term_print_c("usage: ask <question>\n", 2); return; }
        /*
         * Rebuilt from argv, not read off cmdline.
         *
         * term_split tokenises in place: by the time this runs, cmdline
         * is "ask\0what\0berries\0are\0poisonous", so walking past the
         * first word landed on the NUL that ended it and every question
         * arrived empty. wiki_ask refuses an empty question, so `ask`
         * answered "could not start" for every input it was ever given
         * -- the command could not work at all.
         */
        char q[WIKI_INPUT_MAX];
        q[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) str_append(q, " ", sizeof(q));
            str_append(q, argv[i], sizeof(q));
        }
        if (wiki_ask(q))
            term_print_c("thinking - the answer appears in Wikipedia\n", 3);
        else
            term_print_c("could not start - see the Wikipedia window\n", 2);
    } else if (str_eq(cmd, "zim")) {
        if (argc < 2) {
            term_print_c("usage: zim open <file> | info | main | find <path>"
                         " | get <path> | ls [prefix]\n", 2);
            return;
        }
        if (str_eq(argv[1], "open")) {
            if (argc < 3) { term_print_c("usage: zim open <file>\n", 2); return; }
            char abs[256];
            term_resolve(argv[2], abs);
            if (zim_open(abs) != 0) {
                term_print_c("zim: ", 2);
                term_print_c(zim_err, 2);
                term_putc('\n');
                return;
            }
            term_print_c("opened ", 4);
            term_print_c(abs, 4);
            term_putc('\n');
        }
        if (!zim.open) { term_print_c("no archive open (zim open <file>)\n", 2); return; }

        char nb[24];
        if (str_eq(argv[1], "open") || str_eq(argv[1], "info")) {
            term_print("  version    ");
            uint_to_str(zim.major, nb); term_print(nb);
            term_print(".");
            uint_to_str(zim.minor, nb); term_print(nb);
            term_print("\n  entries    ");
            uint_to_str(zim.article_count, nb); term_print_c(nb, 1);
            term_print("\n  clusters   ");
            uint_to_str(zim.cluster_count, nb); term_print(nb);
            term_print("\n  size       ");
            uint_to_str((uint32_t)(zim.f.size / (1024 * 1024)), nb);
            term_print(nb); term_print(" MB\n  mime types ");
            uint_to_str((uint32_t)zim.mime_count, nb); term_print(nb);
            term_putc('\n');
            if (zim.truncated) {
                term_print_c("  WARNING: the file is shorter than its header says"
                             " - incomplete download\n", 2);
            }
            return;
        }

        if (str_eq(argv[1], "main")) {
            const uint8_t *d; uint32_t n; zim_dirent_t e;
            if (zim_content(zim.main_page, &d, &n, &e) != 0) {
                term_print_c("zim: ", 2); term_print_c(zim_err, 2); term_putc('\n');
                return;
            }
            term_print_c("main page: ", 1);
            term_print_c(e.title, 1);
            term_print("  (");
            uint_to_str(n, nb); term_print(nb);
            term_print(" bytes, ");
            term_print(zim_mime_name(e.mime));
            term_print(")\n");
            return;
        }

        if (str_eq(argv[1], "find") || str_eq(argv[1], "get")) {
            if (argc < 3) { term_print_c("usage: zim find|get <path>\n", 2); return; }
            uint32_t idx;
            if (!zim_find('C', argv[2], &idx)) {
                term_print_c("not found: ", 2);
                term_print_c(argv[2], 2);
                term_print_c("   (paths are case sensitive, try 'zim ls'"
                             " to browse)\n", 3);
                return;
            }
            const uint8_t *d; uint32_t n; zim_dirent_t e;
            if (zim_content(idx, &d, &n, &e) != 0) {
                term_print_c("zim: ", 2); term_print_c(zim_err, 2); term_putc('\n');
                return;
            }
            term_print_c(e.title, 1);
            term_print("  [");
            term_print(zim_mime_name(e.mime));
            term_print(", ");
            uint_to_str(n, nb); term_print(nb);
            term_print(" bytes, cluster ");
            uint_to_str(e.cluster, nb); term_print(nb);
            term_print("]\n");
            if (str_eq(argv[1], "get")) {
                uint32_t lim = n < 600 ? n : 600;
                for (uint32_t i = 0; i < lim; i++) {
                    char c = (char)d[i];
                    term_putc((c >= 0x20 && c < 0x7F) || c == '\n' ? c : '.');
                }
                if (term_cx > 0) term_putc('\n');
            }
            return;
        }

        if (str_eq(argv[1], "ls")) {
            const char *pfx = argc >= 3 ? argv[2] : "";
            uint32_t i = zim_lower_bound('C', pfx);
            zim_dirent_t e;
            int shown = 0;
            while (i < zim.article_count && shown < 20) {
                if (zim_dirent(i, &e) != 0) break;
                if (e.ns != 'C') break;
                term_print("  ");
                term_print_c(e.title, e.is_redirect ? 3 : 0);
                if (e.is_redirect) term_print_c("  ->", 3);
                term_putc('\n');
                i++; shown++;
            }
            if (shown == 0) term_print_c("  (nothing at that prefix)\n", 3);
            return;
        }

        term_print_c("unknown zim subcommand\n", 2);
    } else if (str_eq(cmd, "img") || str_eq(cmd, "view")) {
        if (argc < 2) { term_print_c("usage: img <file.sci>\n", 2); return; }
        char abs[256];
        term_resolve(argv[1], abs);
        if (img_open_path(abs) != 0) {
            term_print_c("img: ", 2);
            term_print_c(img_status(), 2);
            term_putc('\n');
            return;
        }
        term_print("  ");
        term_print_c(img_status(), 4);
        term_putc('\n');
        wm_open(WK_IMAGE);
    } else if (str_eq(cmd, "beep"))    { cmd_beep(argc, argv);
    } else if (str_eq(cmd, "policy"))  { cmd_policy(argc, argv);
    } else if (str_eq(cmd, "allow"))   { cmd_allow(argc, argv);
    } else if (str_eq(cmd, "scan"))    { cmd_scan(argc, argv);
    } else if (str_eq(cmd, "vault"))   { cmd_vault(argc, argv);
    } else if (str_eq(cmd, "backup"))  { cmd_backup(argc, argv);
    } else if (str_eq(cmd, "restore")) { cmd_restore(argc, argv);
    } else if (str_eq(cmd, "store") || str_eq(cmd, "ingot")) {
        store_cmd(argc, argv);
    } else if (str_eq(cmd, "open")) {
        if (argc < 2) { term_print_c("usage: open <app>\n", 2); return; }
        if (!desktop_open_app_by_name(argv[1])) {
            term_print_c("unknown app: ", 2);
            term_print_c(argv[1], 2);
            term_putc('\n');
        }
    } else if (desktop_open_app_by_name(cmd)) {
        /* bare app name works too: "browser", "files", ... */
    } else if (str_eq(cmd, "ai")) {
        if (argc >= 2 && str_eq(argv[1], "on")) {
            ai_choice_save(1);
            ai_autoload_start();
            term_print_c("AI features enabled\n", 4);
        } else if (argc >= 2 && str_eq(argv[1], "off")) {
            ai_choice_save(0);
            term_print_c("AI features disabled for this account\n", 3);
        } else {
            term_print("AI features are ");
            term_print_c(ai_enabled == 1 ? "on" : "off", ai_enabled == 1 ? 4 : 3);
            term_print("   (ai on | ai off)\n");
        }
    } else if (str_eq(cmd, "tickets")) {
        /*
         * klist, in effect. A credential cache that cannot be listed is
         * a credential cache nobody can reason about -- "am I still
         * authenticated" and "what is on my disk" are the two questions
         * it raises and neither was answerable before this.
         */
        if (argc >= 2 && str_eq(argv[1], "purge")) {
            if (krb_cc_purge() != 0)
                term_print_c("tickets: nothing cached to remove\n", 2);
            else
                term_print_c("cached credentials destroyed\n", 3);
            return;
        }
        if (argc >= 2) {
            term_print_c("usage: tickets [purge]\n", 2);
            return;
        }
        if (!krb_cc_bound) {
            term_print_c("no Kerberos session (no account signed in)\n", 2);
            return;
        }
        term_print_c("Kerberos credentials\n", 1);
        term_print("  cache      ");
        { char p[288]; krb_cc_path(krb_cc_user, p, sizeof p); term_print(p); }
        term_print("\n");
        if (!krb_have_tgt() && !krb_have_service()) {
            term_print("  none held\n");
            return;
        }
        if (krb_have_tgt()) {
            term_print("  TGT        ");
            term_print(krb.tgt.realm);
            term_print("  until ");
            term_print(krb.tgt.endtime);
            term_print(krb_cc_expired(krb.tgt.endtime) ? "  EXPIRED\n" : "\n");
        }
        if (krb_have_service()) {
            const krb_cred_t *s = krb_service_cred();
            term_print("  service    ");
            for (int i = 0; i < s->nsname; i++) {
                if (i) term_print("/");
                term_print(s->sname[i]);
            }
            term_print("  until ");
            term_print(s->endtime);
            term_print(krb_cc_expired(s->endtime) ? "  EXPIRED\n" : "\n");
        }
        term_print("\nThe cache is encrypted under a key derived from your\n"
                   "login password. Logging out wipes the key, not the file.\n");
    } else if (str_eq(cmd, "swap")) {
        /*
         * A pager that is thrashing and one that has never been needed
         * look identical from outside -- the machine is just slow, or
         * it is not -- and the difference is the whole diagnosis. So the
         * counters are readable rather than only reachable from serial.
         */
        if (!swap_ready) {
            term_print_c("swap is off: ", 2);
            term_print_c(swap_errstr[0] ? swap_errstr : "not started", 2);
            term_putc('\n');
            return;
        }
        char nb[16];
        term_print_c("Pagefile\n", 1);
        term_print("  file       " SWAP_PATH "\n  size       ");
        uint_to_str(SWAP_MB, nb);          term_print(nb);
        term_print(" MB\n  slots      ");
        uint_to_str(swap_slots_used, nb);  term_print(nb);
        term_print(" of ");
        uint_to_str(swap_slot_count, nb);  term_print(nb);
        term_print(" in use\n  paged out  ");
        uint_to_str((uint32_t)swap_outs, nb);  term_print(nb);
        term_print("\n  paged in   ");
        uint_to_str((uint32_t)swap_ins, nb);   term_print(nb);
        term_print("\n  refused    ");
        uint_to_str((uint32_t)swap_fails, nb); term_print(nb);
        term_print("\n  frames scanned by the clock  ");
        uint_to_str((uint32_t)swap_scans, nb); term_print(nb);
        term_print("\n\nOnly user pages are swappable. Kernel memory, page\n"
                   "tables and shared frames never leave RAM.\n");
    } else if (str_eq(cmd, "env")) {
        /*
         * Here so that "the environment is bound to the profile" is a
         * claim someone can check rather than one they have to take on
         * faith. Same table a Windows process is handed at startup.
         */
        if (argc >= 2) {
            const char *v = env_get(argv[1]);
            if (!v) { term_print_c("not set\n", 2); return; }
            term_print(v);
            term_putc('\n');
            return;
        }
        if (env_count == 0) {
            term_print_c("the environment is empty -- no account is "
                         "signed in\n", 3);
            return;
        }
        for (int i = 0; i < env_count; i++) {
            term_print_c(env_vars[i].name, 1);
            term_print("=");
            term_print(env_vars[i].value);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "whoami")) {
        if (user_current < 0) { term_print("nobody\n"); return; }
        term_print(user_name_of(user_current));
        if (user_is_admin(user_current)) term_print_c("  (administrator)", 3);
        term_putc('\n');
    } else if (str_eq(cmd, "users")) {
        for (int i = 0; i < user_count; i++) {
            term_print_c(i == user_current ? "* " : "  ", 1);
            term_print(users[i].name);
            if (users[i].flags & USER_FLAG_ADMIN)
                term_print_c("   admin", 3);
            term_putc('\n');
        }
        if (user_count == 0) term_print_c("no accounts\n", 2);
    } else if (str_eq(cmd, "passwd")) {
        /*
         * Typing a password into a shell that echoes it and keeps a
         * history is not something to encourage, so this only points at
         * the place where it can be done properly.
         */
        term_print_c("Change your password in Settings > Users: pick your\n", 3);
        term_print_c("account, type a new password, press Set.\n", 3);
    } else if (str_eq(cmd, "useradd")) {
        if (!user_is_admin(user_current)) {
            term_print_c("useradd: administrators only\n", 2);
            return;
        }
        if (argc < 3) {
            term_print_c("usage: useradd <name> <password> [admin]\n", 2);
            return;
        }
        int adm = (argc > 3 && str_eq(argv[3], "admin"));
        if (user_add(argv[1], argv[2], adm) < 0) {
            term_print_c("useradd: ", 2);
            term_print_c(user_err, 2);
            term_putc('\n');
        } else {
            /* The path is asked for rather than spelled, because it is
             * not /home/<name> any more and there is exactly one place
             * that knows what it is instead. */
            char home[PROFILE_PATH_MAX];
            profile_home(argv[1], home, sizeof(home));
            term_print("created ");
            term_print(argv[1]);
            term_print(" with ");
            term_print(home);
            term_putc('\n');
        }
    } else if (str_eq(cmd, "userdel")) {
        if (!user_is_admin(user_current)) {
            term_print_c("userdel: administrators only\n", 2);
            return;
        }
        if (argc < 2) { term_print_c("usage: userdel <name>\n", 2); return; }
        if (user_del(argv[1]) < 0) {
            term_print_c("userdel: ", 2);
            term_print_c(user_err, 2);
            term_putc('\n');
        } else {
            term_print("deleted ");
            term_print(argv[1]);
            term_print_c("   (their profile is left in place)\n", 3);
        }
    } else if (str_eq(cmd, "logout")) {
        term_print_c("logging out...\n", 1);
        want_logout = 1;
    } else if (str_eq(cmd, "reboot")) {
        term_print_c("rebooting...\n", 1);
        outb(0x64, 0xFE);
    } else if (str_eq(cmd, "shutdown") || str_eq(cmd, "poweroff")) {
        term_print_c("powering off...\n", 1);
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0x604) : "memory");
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0xB004) : "memory");
    } else if (cu_dispatch(cmd, argc, argv)) {
        /* handled by coreutils.h */
    } else {
        term_print_c("unknown command: ", 2);
        term_print_c(cmd, 2);
        term_print_c("   (try 'help')\n", 3);
    }
}

/* ===== ASYNC POLL — advance ping/dns/fetch, called every frame ===== */

static void term_async_finish(void) {
    term_async = TERM_ASYNC_NONE;
    ping_active = 0;
    term_prompt_begin();
}

static void term_async_poll(void) {
    if (term_async == TERM_ASYNC_NONE) return;

    if (term_async == TERM_ASYNC_DNS) {
        if (dns_state == DNS_STATE_DONE) {
            char buf[24];
            ip_to_str(dns_result, buf);
            term_print(term_async_arg);
            term_print(" -> ");
            term_print_c(buf, 4);
            term_putc('\n');
            term_async_finish();
        } else if (dns_state == DNS_STATE_FAIL) {
            term_print_c("could not resolve host\n", 2);
            term_async_finish();
        }
        return;
    }

    if (term_async == TERM_ASYNC_PING) {
        /* Phase 1: wait for DNS */
        if (term_ping_next_at == 0) {
            if (dns_state == DNS_STATE_DONE) {
                for (int i = 0; i < 4; i++) ping_target[i] = dns_result[i];
                term_ping_next_at = (int)net_ticks;   /* fire immediately */
            } else if (dns_state == DNS_STATE_FAIL) {
                term_print_c("ping: unknown host\n", 2);
                term_async_finish();
                return;
            } else {
                return;
            }
        }

        /* Reply arrived for the outstanding packet? */
        if (ping_got_reply) {
            ping_got_reply = 0;
            char buf[24], nb[12];
            ip_to_str(ping_target, buf);
            term_print("reply from ");
            term_print(buf);
            term_print(": icmp_seq=");
            uint_to_str(ping_seq, nb);
            term_print(nb);
            term_print(" time=");
            uint_to_str(ping_last_rtt * 17, nb);
            term_print(nb);
            term_print_c(" ms\n", 4);
        }

        /* Timeout on outstanding packet */
        if (ping_sent_count > ping_replies &&
            (int)net_ticks - term_ping_next_at > 90 &&
            ping_sent_count > 0) {
            /* declared lost when the next one fires */
        }

        if ((int)net_ticks >= term_ping_next_at) {
            if (ping_sent_count >= 4) {
                /* done — summary */
                char nb[12];
                term_print("--- ");
                uint_to_str((uint32_t)ping_sent_count, nb);
                term_print(nb);
                term_print(" sent, ");
                uint_to_str((uint32_t)ping_replies, nb);
                term_print(nb);
                term_print(" received ---\n");
                term_async_finish();
                return;
            }
            ping_seq++;
            ping_sent_count++;
            ping_sent_tick = net_ticks;
            icmp_send_echo(ping_target, ping_seq);
            term_ping_next_at = (int)net_ticks + 45;   /* ~0.75 s apart */
        }
        return;
    }

    if (term_async == TERM_ASYNC_FETCH) {
        if (http_state == HTTP_DONE) {
            char nb[12];
            term_print_c("-- HTTP ", 1);
            uint_to_str((uint32_t)http_status_code, nb);
            term_print_c(nb, 1);
            term_print_c(" --\n", 1);
            /* dump up to ~4 KB of body */
            int limit = http_body_len < 4096 ? http_body_len : 4096;
            for (int i = 0; i < limit; i++) {
                char c = (char)http_body[i];
                if (c == '\r') continue;
                term_putc(c);
            }
            if (term_cx > 0) term_putc('\n');
            if (http_body_len > limit)
                term_print_c("...(truncated - use the browser)\n", 3);
            term_async_finish();
        } else if (http_state == HTTP_ERROR) {
            term_print_c("fetch failed: ", 2);
            term_print_c(http_err, 2);
            term_putc('\n');
            term_async_finish();
        }
        return;
    }
}

/* ===== KEY INPUT ===== */

static void term_key(char ch) {
    term_view = 0;   /* typing snaps back to the bottom */

    if (term_async != TERM_ASYNC_NONE) {
        if (ch == 27) {   /* ESC cancels */
            term_print_c("^C\n", 3);
            if (term_async == TERM_ASYNC_FETCH) tcp_abort();
            term_async_finish();
        }
        return;
    }

    if (ch == '\n') {
        term_run_command();
        return;
    }
    if (ch == '\b') {
        if (term_input_cur > 0) {
            for (int i = term_input_cur - 1; i < term_input_len; i++)
                term_input[i] = term_input[i + 1];
            term_input_len--;
            term_input_cur--;
        }
        return;
    }
    if (ch == KEY_DEL) {
        if (term_input_cur < term_input_len) {
            for (int i = term_input_cur; i < term_input_len; i++)
                term_input[i] = term_input[i + 1];
            term_input_len--;
        }
        return;
    }
    if (ch == KEY_LEFT)  { if (term_input_cur > 0) term_input_cur--; return; }
    if (ch == KEY_RIGHT) { if (term_input_cur < term_input_len) term_input_cur++; return; }
    if (ch == KEY_HOME)  { term_input_cur = 0; return; }
    if (ch == KEY_END)   { term_input_cur = term_input_len; return; }

    if (ch == KEY_UP || ch == KEY_DOWN) {
        int total = term_hist_count < TERM_HIST ? term_hist_count : TERM_HIST;
        if (total == 0) return;
        if (ch == KEY_UP) {
            if (term_hist_pos < 0) term_hist_pos = 0;
            else if (term_hist_pos < total - 1) term_hist_pos++;
        } else {
            if (term_hist_pos < 0) return;
            term_hist_pos--;
        }
        if (term_hist_pos < 0) {
            term_input[0] = '\0';
            term_input_len = 0;
            term_input_cur = 0;
        } else {
            int idx = (term_hist_count - 1 - term_hist_pos) % TERM_HIST;
            str_copy(term_input, term_hist[idx], TERM_INPUT_MAX);
            term_input_len = str_len(term_input);
            term_input_cur = term_input_len;
        }
        return;
    }

    if (ch >= 0x20 && ch < 0x7F) {
        if (term_input_len < TERM_INPUT_MAX - 1) {
            for (int i = term_input_len; i > term_input_cur; i--)
                term_input[i] = term_input[i - 1];
            term_input[term_input_cur++] = ch;
            term_input_len++;
            term_input[term_input_len] = '\0';
        }
    }
}

/* Scroll keys are handled even without focus-follows behavior */
static void term_scroll_key(char ch, int page_rows) {
    if (ch == KEY_PGUP) {
        term_view += page_rows;
        int max_view = term_row - 4;
        if (max_view < 0) max_view = 0;
        if (term_view > max_view) term_view = max_view;
    } else if (ch == KEY_PGDN) {
        term_view -= page_rows;
        if (term_view < 0) term_view = 0;
    }
}

/* ===== FIRST-BOOT BANNER ===== */

static int term_banner_done = 0;

static void term_banner(void) {
    if (term_banner_done) return;
    term_banner_done = 1;
    term_print_c("Vextro 9.0 ", 1);
    term_print_c("(x86_64 bare metal)\n", 3);
    if (fs_writable()) {
        char nb[16];
        term_print_c(fs_name(), 3);
        term_print_c(" volume mounted: ", 3);
        uint_to_str(fs_free_kb() / 1024, nb);
        term_print_c(nb, 3);
        term_print_c(" MB free of ", 3);
        uint_to_str(fs_total_kb() / 1024, nb);
        term_print_c(nb, 3);
        term_print_c(" MB (writable, persistent)\n", 3);
    } else {
        term_print_c("no disk found - read-only ramdisk fallback\n", 2);
    }
    term_print_c("Type 'help' for commands.\n\n", 3);
}

/* ===== DRAW ===== */

static void term_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                      uint32_t tick, int focused) {
    term_banner();

    gfx_rect(buf, w, h, cx, cy, cw, chh, C_TERM_BG);

    int rows = (chh - 2 * TERM_PAD) / TERM_LINE_H;
    if (rows < 2) return;
    int out_rows = rows - 1;                 /* last row = input line */

    /* Visible output range with scrollback */
    int bottom = term_row - term_view;       /* last visible line idx */
    int first = bottom - out_rows + 1;
    if (first < 0) first = 0;

    int y = cy + TERM_PAD;
    for (int r = first; r <= bottom && r <= term_row; r++) {
        if (term_lines[r][0] != '\0') {
            uint32_t col = term_palette[term_line_color[r] < 6 ?
                                        term_line_color[r] : 0];
            mono_text(buf, w, h, cx + TERM_PAD, y, term_lines[r], col, 1);
        }
        y += TERM_LINE_H;
    }

    /* Input line (hidden while an async command runs or when scrolled) */
    if (term_async == TERM_ASYNC_NONE && term_view == 0) {
        int32_t ix = cx + TERM_PAD;
        char prompt[64];
        term_build_prompt(prompt, sizeof(prompt));
        mono_text(buf, w, h, ix, y, prompt, C_GOLD, 1);
        ix += MONO_ADV(1) * str_len(prompt);
        mono_text(buf, w, h, ix, y, term_input, C_TERM_FG, 1);

        /* Blinking block cursor */
        if (focused && ((tick / 30) & 1) == 0) {
            int32_t cur_px = ix + term_input_cur * MONO_ADV(1);
            gfx_rect(buf, w, h, cur_px, y - 1, MONO_ADV(1), 10, C_GOLD);
            if (term_input_cur < term_input_len)
                mono_char(buf, w, h, cur_px, y,
                          term_input[term_input_cur], C_TERM_BG, 1);
        }
    } else if (term_view > 0) {
        /* Scrollback indicator */
        char nb[16];
        uint_to_str((uint32_t)term_view, nb);
        char msg[32];
        str_copy(msg, "-- scrollback +", sizeof(msg));
        str_append(msg, nb, sizeof(msg));
        str_append(msg, " --", sizeof(msg));
        mono_text(buf, w, h, cx + TERM_PAD, y, msg, C_GOLD_DIM, 1);
    } else {
        mono_text(buf, w, h, cx + TERM_PAD, y, "(working... Esc cancels)",
                  0x6A7284u, 1);
    }
}

#endif /* TERM_H */
