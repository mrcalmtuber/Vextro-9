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
    uint_to_str(fs_kind == FS_EXFAT ? exf_vol.cluster_count
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
        term_print_c("no network adapter detected\n", 2);
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

#define AI_MODEL_PATH "/qwen2.gguf"

static int         ai_state = AI_IDLE;
static const char *ai_err   = "";
static fs_file_t   ai_file;

/* Begin, if the model is actually there.  Missing is not a failure — a
 * machine without one simply has no chat. */
static void ai_autoload_start(void) {
    if (ai_state != AI_IDLE) return;
    /* Declined, or not yet asked. Loading 380 MB on the strength of an
     * answer nobody has given would be the wrong default. */
    if (ai_enabled != 1) return;
    if (fs_open(AI_MODEL_PATH, &ai_file) != 0) {
        /* Not an error — a machine with no model simply has no chat. But
         * silence here is why "the chat panel does nothing" took so long
         * to trace on the ARM tree, where this was never even called. */
        serial_puts("[ai] no model at " AI_MODEL_PATH "\n");
        return;
    }
    serial_puts("[ai] loading " AI_MODEL_PATH "\n");
    ai_state = AI_PARSE;
}

static void ai_poll(void) {
    const char *err = "?";

    switch (ai_state) {
    case AI_PARSE:
        /* One shot: the tensor table has to be whole before anything
         * can be bound, and it is a small fraction of the file. */
        if (llm_load(llm_read_thunk, &ai_file, ai_file.size, &err) != 0 ||
            llm_load_begin(&err) != 0) {
            ai_err = err;
            ai_state = AI_FAILED;
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
            if (r < 0) { ai_err = err; ai_state = AI_FAILED; return; }
            if (r == 1) { ai_state = AI_READY; return; }
        } while (!budget_expired_ms(start, 6));
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
 * the `ask` command reaches it through this. */
static int  wiki_ask(const char *question);

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
        term_print("\n\nNone of this isolates a running program; it decides\n"
                   "whether one starts. See 'help policy'.\n");
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
        const char *q = cmdline;
        while (*q && *q != ' ') q++;
        while (*q == ' ') q++;
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
            term_print("created ");
            term_print(argv[1]);
            term_print(" with /home/");
            term_print(argv[1]);
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
            term_print_c("   (their /home is left in place)\n", 3);
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
