#ifndef TERM_H
#define TERM_H

/*
 * Socrates Terminal — monospace grid renderer with scrollback, command
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

static void term_cmd_df(void) {
    if (!fs_writable()) {
        term_print_c("no writable volume (tar ramdisk fallback active)\n", 3);
        return;
    }
    char nb[16];
    uint32_t total = fat_total_kb(), free_kb = fat_free_kb();
    term_print("  volume    FAT32 on ata0 (");
    uint_to_str(fat_vol.nclusters, nb);
    term_print(nb);
    term_print(" clusters)\n  total     ");
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
    term_print_c("Socrates BSD 9 shell commands\n", 1);
    term_print("  ls [dir]  cat <f>  cd <dir>  pwd     browse the disk\n");
    term_print("  echo <text> > f    write a file  (>> appends)\n");
    term_print("  rm <f>  mkdir <d>  cp <a> <b>  df    manage the disk\n");
    term_print("  run <program>     execute an ELF app\n");
    term_print("  date / uptime / mem / uname          system info\n");
    term_print("  net / arp / ping / dns / fetch       networking\n");
    term_print("  gpu [test]        Intel iGPU blitter status / demo\n");
    term_print("  open <app>        terminal browser files settings\n");
    term_print("                    paint sysmon matrix about\n");
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

/* Split into argv (destructive on buf) */
static int term_split(char *buf, char **argv, int max) {
    int argc = 0;
    char *p = buf;
    while (*p && argc < max) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    return argc;
}

static void term_exec(char *cmdline);

static void term_build_prompt(char *out, int max) {
    str_copy(out, "socrates:", max);
    str_append(out, term_cwd, max);
    str_append(out, "> ", max);
}

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

static void term_exec(char *cmdline) {
    char *argv[8];
    int argc = term_split(cmdline, argv, 8);
    if (argc == 0) return;
    const char *cmd = argv[0];

    if (str_eq(cmd, "help")) {
        term_cmd_help();
    } else if (str_eq(cmd, "clear")) {
        term_clear();
    } else if (str_eq(cmd, "about")) {
        term_print_c("Socrates BSD 9\n", 1);
        term_print("A bare-metal x86_64 operating system.\n");
        term_print("TrueType rasterizer, window manager, TCP/IP stack,\n");
        term_print("HTTP browser and PS/2 HAL - no libc, no floats.\n");
    } else if (str_eq(cmd, "uname")) {
        term_print("Socrates BSD 9.0 x86_64 bare-metal\n");
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
            fat_dirent_t d;
            if (fat_lookup(abs, &d) && (d.attr & FAT_ATTR_DIR)) {
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
        if (argc >= 2 && str_eq(argv[1], "test")) {
            if (!igpu.active) {
                term_print_c("gpu test: no active iGPU (CPU renderer)\n", 2);
            } else if (!igpu.fb_blittable) {
                term_print_c("gpu test: framebuffer not GGTT-reachable\n", 2);
            } else {
                /* blit a tile onto the live framebuffer, verify by CPU */
                int tx = 40, ty = 60, tw = 120, th = 80;
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
        term_print("fetching http://");
        term_print(host);
        term_print(path);
        term_print(" ...\n");
    } else if (str_eq(cmd, "open")) {
        if (argc < 2) { term_print_c("usage: open <app>\n", 2); return; }
        if (!desktop_open_app_by_name(argv[1])) {
            term_print_c("unknown app: ", 2);
            term_print_c(argv[1], 2);
            term_putc('\n');
        }
    } else if (desktop_open_app_by_name(cmd)) {
        /* bare app name works too: "browser", "files", ... */
    } else if (str_eq(cmd, "reboot")) {
        term_print_c("rebooting...\n", 1);
        outb(0x64, 0xFE);
    } else if (str_eq(cmd, "shutdown") || str_eq(cmd, "poweroff")) {
        term_print_c("powering off...\n", 1);
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0x604) : "memory");
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0xB004) : "memory");
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
    term_print_c("Socrates BSD 9.0 ", 1);
    term_print_c("(x86_64 bare metal)\n", 3);
    if (fs_writable()) {
        char nb[16];
        term_print_c("FAT32 volume mounted: ", 3);
        uint_to_str(fat_free_kb() / 1024, nb);
        term_print_c(nb, 3);
        term_print_c(" MB free of ", 3);
        uint_to_str(fat_total_kb() / 1024, nb);
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
