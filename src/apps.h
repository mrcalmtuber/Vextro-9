#ifndef APPS_H
#define APPS_H

/*
 * Built-in desktop applications:
 *   Files     — ramdisk explorer (double-click to open)
 *   Settings  — wallpaper theme, dock position/size, system info
 *   Goldsmith — mouse paint app
 *   Monolith  — live system monitor
 *   Matrix    — falling glyph rain demo
 *   About     — the obligatory dialog
 */

/* ===================== FILES ===================== */

#define EXP_PATH_MAX    256
#define EXP_MAX_ENTRIES 64
#define EXP_ICON_W      74
#define EXP_ICON_H      74
#define EXP_GRID_PAD    10
#define EXP_PATHBAR_H   26
#define EXP_DBLCLICK    30      /* ~0.5 s at 60 Hz */

typedef struct {
    char name[100];
    uint64_t size;
    int is_dir;
} exp_entry_t;

static char        exp_path[EXP_PATH_MAX] = "/";
static exp_entry_t exp_entries[EXP_MAX_ENTRIES];
static int         exp_entry_count = 0;
static int         exp_selected = -1;
static uint32_t    exp_last_click_tick = 0;
static int         exp_last_click_idx = -1;

static void exp_list_cb(const char *name, uint32_t size, int is_dir) {
    if (name[0] == '.') return;              /* hide host dotfiles */
    if (exp_entry_count >= EXP_MAX_ENTRIES) return;
    exp_entry_t *e = &exp_entries[exp_entry_count++];
    str_copy(e->name, name, 100);
    e->size = size;
    e->is_dir = is_dir;
}

static void exp_scan(void) {
    recent_push(WK_FILES, exp_path, exp_path);
    exp_entry_count = 0;
    exp_selected = -1;

    if (fs_writable()) {
        fs_list(exp_path, exp_list_cb);
        return;
    }

    if (!tarfs_base) return;

    const char *dir = exp_path;
    int dlen = str_len(dir);
    int has_slash_prefix = (dlen > 0 && dir[0] == '/');
    const char *dir_norm = has_slash_prefix ? dir + 1 : dir;
    int dir_norm_len = str_len(dir_norm);
    int is_root = (dir_norm_len == 0);

    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;

    char seen_dirs[EXP_MAX_ENTRIES][100];
    int seen_dir_count = 0;

    while (ptr + TAR_BLOCK_SIZE <= end) {
        tar_header_t *hdr = (tar_header_t *)ptr;
        if (hdr->name[0] == '\0') break;
        uint64_t file_size = octal_parse(hdr->size, 12);

        const char *name = hdr->name;
        if (name[0] == '.' && name[1] == '/') name += 2;

        if (name[0] != '\0') {
            const char *rel = name;
            if (!is_root) {
                int match = 1;
                for (int i = 0; i < dir_norm_len; i++) {
                    if (name[i] != dir_norm[i]) { match = 0; break; }
                }
                if (!match || name[dir_norm_len] != '/') goto next;
                rel = name + dir_norm_len + 1;
            }
            if (rel[0] == '\0') goto next;

            int slash_pos = -1;
            for (int i = 0; rel[i]; i++)
                if (rel[i] == '/') { slash_pos = i; break; }

            if (slash_pos >= 0) {
                char dir_name[100];
                for (int i = 0; i < slash_pos && i < 99; i++)
                    dir_name[i] = rel[i];
                dir_name[slash_pos < 99 ? slash_pos : 99] = '\0';
                int already = 0;
                for (int d = 0; d < seen_dir_count; d++)
                    if (str_eq(seen_dirs[d], dir_name)) { already = 1; break; }
                if (!already && seen_dir_count < EXP_MAX_ENTRIES &&
                    exp_entry_count < EXP_MAX_ENTRIES) {
                    str_copy(seen_dirs[seen_dir_count], dir_name, 100);
                    seen_dir_count++;
                    exp_entry_t *e = &exp_entries[exp_entry_count++];
                    str_copy(e->name, dir_name, 100);
                    e->size = 0;
                    e->is_dir = 1;
                }
            } else {
                int trailing = 0;
                int rlen = str_len(rel);
                if (rlen > 0 && rel[rlen - 1] == '/') trailing = 1;
                if (!trailing && exp_entry_count < EXP_MAX_ENTRIES) {
                    exp_entry_t *e = &exp_entries[exp_entry_count++];
                    str_copy(e->name, rel, 100);
                    e->size = file_size;
                    e->is_dir = (hdr->typeflag == '5');
                }
            }
        }
next:;
        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }
}

static void exp_nav_up(void) {
    int len = str_len(exp_path);
    if (len <= 1) return;
    if (exp_path[len - 1] == '/') exp_path[--len] = '\0';
    int last = -1;
    for (int i = 0; i < len; i++)
        if (exp_path[i] == '/') last = i;
    if (last <= 0) { exp_path[0] = '/'; exp_path[1] = '\0'; }
    else exp_path[last] = '\0';
    exp_scan();
}

static void exp_nav_into(const char *dirname) {
    if (str_len(exp_path) > 1) str_append(exp_path, "/", EXP_PATH_MAX);
    str_append(exp_path, dirname, EXP_PATH_MAX);
    exp_scan();
}

static int  img_is_sci(const char *name);
static int  img_open_named(const char *name);

static void exp_open_file(const char *name) {
    /* pictures go to the viewer, everything else to the browser */
    if (img_is_sci(name)) {
        img_open_named(name);
        wm_open(WK_IMAGE);
        return;
    }

    /* full ramdisk path for vextro://file/ */
    char url[300];
    str_copy(url, "vextro://file/", sizeof(url));
    const char *p = exp_path;
    if (p[0] == '/') p++;
    if (p[0]) {
        str_append(url, p, sizeof(url));
        str_append(url, "/", sizeof(url));
    }
    str_append(url, name, sizeof(url));
    brw_navigate(url);
    wm_open(WK_BROWSER);
}

static void exp_icon_rect(int idx, int32_t gx, int32_t gy, int32_t gw,
                          int32_t *ox, int32_t *oy) {
    int cols = (gw - EXP_GRID_PAD) / (EXP_ICON_W + EXP_GRID_PAD);
    if (cols < 1) cols = 1;
    *ox = gx + EXP_GRID_PAD + (idx % cols) * (EXP_ICON_W + EXP_GRID_PAD);
    *oy = gy + EXP_GRID_PAD + (idx / cols) * (EXP_ICON_H + EXP_GRID_PAD);
}

static void exp_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                      uint32_t tick) {
    (void)chh;
    int click = (lmb && !prev_lmb);
    if (!click) return;

    /* Up button in path bar */
    int32_t up_x = cx + cw - 34;
    int32_t up_y = cy + 3;
    if (mx >= up_x && mx < up_x + 28 && my >= up_y && my < up_y + 20) {
        exp_nav_up();
        exp_last_click_idx = -1;
        return;
    }

    int32_t gy = cy + EXP_PATHBAR_H;
    int hit = -1;
    for (int i = 0; i < exp_entry_count; i++) {
        int32_t ix, iy;
        exp_icon_rect(i, cx, gy, cw, &ix, &iy);
        if (mx >= ix && mx < ix + EXP_ICON_W &&
            my >= iy && my < iy + EXP_ICON_H) {
            hit = i;
            break;
        }
    }

    exp_selected = hit;
    if (hit >= 0) {
        if (hit == exp_last_click_idx &&
            tick - exp_last_click_tick < EXP_DBLCLICK) {
            if (exp_entries[hit].is_dir) exp_nav_into(exp_entries[hit].name);
            else exp_open_file(exp_entries[hit].name);
            exp_last_click_idx = -1;
            return;
        }
        exp_last_click_idx = hit;
        exp_last_click_tick = tick;
    } else {
        exp_last_click_idx = -1;
    }
}

static void exp_draw_folder(uint32_t *buf, uint32_t w, uint32_t h,
                            int32_t x, int32_t y) {
    gfx_rect(buf, w, h, x, y, 18, 5, C_GOLD_DIM);
    gfx_rect(buf, w, h, x, y + 4, 40, 26, 0xE8CE7Bu);
    gfx_rect(buf, w, h, x, y + 4, 40, 3, C_GOLD);
    gfx_rect_outline(buf, w, h, x, y + 4, 40, 26, C_GOLD_DIM);
}

static void exp_draw_file(uint32_t *buf, uint32_t w, uint32_t h,
                          int32_t x, int32_t y) {
    gfx_rect(buf, w, h, x, y, 26, 32, 0xFFFFFFu);
    gfx_rect_outline(buf, w, h, x, y, 26, 32, 0xB8BCC8u);
    for (int i = 0; i < 6; i++)
        gfx_rect(buf, w, h, x + i, y + i, 26 - i, 1, 0xD8DAE2u);
    for (int line = 0; line < 4; line++)
        gfx_rect(buf, w, h, x + 5, y + 10 + line * 5, 16 - line * 2, 2,
                 0xC2C6D0u);
}

static void exp_draw(uint32_t *buf, uint32_t w, uint32_t h,
                     int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                     uint32_t tick, int focused) {
    (void)focused;

    /* pick up files created elsewhere (terminal, host) while open */
    if (tick % 120 == 0) {
        int keep = exp_selected;
        exp_scan();
        if (keep >= 0 && keep < exp_entry_count)
            exp_selected = keep;
    }

    /* path bar */
    gfx_rect(buf, w, h, cx, cy, cw, EXP_PATHBAR_H, C_BG_PANEL);
    gfx_rect(buf, w, h, cx, cy + EXP_PATHBAR_H - 1, cw, 1, 0x2A3040u);
    ttf_draw_string(buf, (int)w, (int)h, cx + 10, cy + 4, exp_path,
                    C_GOLD, 14);

    /* Up button */
    int32_t up_x = cx + cw - 34;
    gfx_rect(buf, w, h, up_x, cy + 3, 28, 20, 0x202535u);
    gfx_rect_outline(buf, w, h, up_x, cy + 3, 28, 20, 0x323A4Eu);
    gfx_tri(buf, w, h, up_x + 14, cy + 7, up_x + 8, cy + 15,
            up_x + 20, cy + 15, C_TEXT);

    /* content */
    int32_t gy = cy + EXP_PATHBAR_H;
    gfx_rect(buf, w, h, cx, gy, cw, chh - EXP_PATHBAR_H, C_WIN_BG);

    for (int i = 0; i < exp_entry_count; i++) {
        int32_t ix, iy;
        exp_icon_rect(i, cx, gy, cw, &ix, &iy);
        if (iy > cy + chh) break;

        if (i == exp_selected) {
            gfx_rect_blend(buf, w, h, ix - 2, iy - 2, EXP_ICON_W + 4,
                           EXP_ICON_H + 4, C_GOLD, 60);
            gfx_rect_outline(buf, w, h, ix - 2, iy - 2, EXP_ICON_W + 4,
                             EXP_ICON_H + 4, C_GOLD_DIM);
        }

        int32_t icon_x = ix + (EXP_ICON_W - 40) / 2;
        if (exp_entries[i].is_dir)
            exp_draw_folder(buf, w, h, icon_x, iy + 6);
        else
            exp_draw_file(buf, w, h, ix + (EXP_ICON_W - 26) / 2, iy + 4);

        /* label (truncated, centered) */
        char label[20];
        int nlen = str_len(exp_entries[i].name);
        int maxc = 12;
        if (nlen > maxc) {
            for (int c = 0; c < maxc - 2; c++) label[c] = exp_entries[i].name[c];
            label[maxc - 2] = '.';
            label[maxc - 1] = '.';
            label[maxc] = '\0';
        } else {
            str_copy(label, exp_entries[i].name, sizeof(label));
        }
        int lw = ttf_text_width(label, 12);
        ttf_draw_string(buf, (int)w, (int)h, ix + (EXP_ICON_W - lw) / 2,
                        iy + EXP_ICON_H - 18, label, C_INK, 12);
    }

    if (exp_entry_count == 0) {
        const char *msg = tarfs_base ? "(empty directory)" : "(no ramdisk)";
        int mw = ttf_text_width(msg, 13);
        ttf_draw_string(buf, (int)w, (int)h, cx + (cw - mw) / 2,
                        gy + (chh - EXP_PATHBAR_H) / 2 - 8, msg,
                        0x8A8F9Cu, 13);
    }
}

/* ===================== SETTINGS ===================== */

#define WALL_THEME_COUNT 5

static const char *wall_theme_names[WALL_THEME_COUNT] = {
    "Midnight", "Ember", "Slate", "Forest", "Royal",
};
static const uint32_t wall_theme_top[WALL_THEME_COUNT] = {
    0x0A0E1Au, 0x1A0F08u, 0x14161Cu, 0x081410u, 0x120A1Cu,
};
static const uint32_t wall_theme_bot[WALL_THEME_COUNT] = {
    0x1A2238u, 0x38220Eu, 0x2A2E38u, 0x143024u, 0x2A1638u,
};
static int wall_theme = 0;

/*
 * Wallpaper slideshow. The interval is in seconds and 0 means off; the
 * choices are spread widely rather than evenly because the useful range
 * is "while I watch" at one end and "while I work" at the other.
 */
#define WALL_SLIDE_COUNT 4
static const int32_t wall_slide_secs[WALL_SLIDE_COUNT]  = { 0, 30, 120, 600 };
static const char   *wall_slide_names[WALL_SLIDE_COUNT] = {
    "Off", "30s", "2m", "10m",
};
static int wall_slide = 0;                  /* index into the above */
static uint32_t wall_slide_last = 0;        /* desktop_tick of the last change */

static const char *dock_edge_names[3] = { "Bottom", "Left", "Right" };

/* ---- the Users pane's own state ----
 *
 * The password is kept in the clear only for as long as it takes to hash
 * it, and is wiped immediately after; what the field displays is a mask.
 */
static char set_newname[USER_NAME_MAX] = "";
static char set_newpw[40] = "";
static char set_newpw_mask[40] = "";
static int  set_field = 0;              /* 0 name, 1 password */
static int  set_user_sel = -1;
static char set_msg[64] = "";
static int  set_msg_err = 0;

static void settings_mouse(int32_t mx, int32_t my, uint8_t lmb,
                           uint8_t prev_lmb, int32_t cx, int32_t cy,
                           int32_t cw, int32_t chh) {
    (void)cw; (void)chh;
    int click = (lmb && !prev_lmb);
    if (!click) return;

    /* wallpaper swatches */
    for (int i = 0; i < WALL_THEME_COUNT; i++) {
        int32_t sx = cx + 24 + i * 56;
        int32_t sy = cy + 52;
        if (mx >= sx && mx < sx + 44 && my >= sy && my < sy + 34) {
            wall_theme = i;
            wallpaper_set_theme(i);
            return;
        }
    }

    /* slideshow interval, on the same row as the theme name */
    for (int i = 0; i < WALL_SLIDE_COUNT; i++) {
        int32_t bx = cx + 130 + i * 58;
        int32_t by = cy + 88;
        if (mx >= bx && mx < bx + 50 && my >= by && my < by + 22) {
            wall_slide = i;
            wall_slide_last = desktop_tick;
            return;
        }
    }

    /* dock edge buttons */
    for (int i = 0; i < 3; i++) {
        int32_t bx = cx + 24 + i * 92;
        int32_t by = cy + 150;
        if (mx >= bx && mx < bx + 84 && my >= by && my < by + 26) {
            dock_cfg.edge = i;
            return;
        }
    }

    /* dock size - / + */
    {
        int32_t by = cy + 196;
        if (mx >= cx + 24 && mx < cx + 60 && my >= by && my < by + 26) {
            if (dock_cfg.icon_sz > 24) {
                dock_cfg.icon_sz -= 4;
                dock_cfg.bar_h -= 4;
                dock_cfg.bar_w -= 32;
            }
        }
        if (mx >= cx + 68 && mx < cx + 104 && my >= by && my < by + 26) {
            if (dock_cfg.icon_sz < 48) {
                dock_cfg.icon_sz += 4;
                dock_cfg.bar_h += 4;
                dock_cfg.bar_w += 32;
            }
        }
    }

    /* ---- Users ---- */
    if (!user_is_admin(user_current)) return;

    for (int i = 0; i < user_count; i++) {
        int32_t ry = cy + 380 + i * 26;
        if (my < ry - 2 || my >= ry + 22) continue;
        set_user_sel = i;

        int32_t ax = cx + 140;
        if (mx >= ax && mx < ax + 62) {
            int on = (users[i].flags & USER_FLAG_ADMIN) ? 0 : 1;
            if (user_set_admin(users[i].name, on) < 0) {
                str_copy(set_msg, user_err, sizeof(set_msg));
                set_msg_err = 1;
            } else {
                str_copy(set_msg, on ? "granted administrator"
                                     : "removed administrator", sizeof(set_msg));
                set_msg_err = 0;
            }
            return;
        }

        int32_t dx = cx + 214;
        if (i != user_current && mx >= dx && mx < dx + 62) {
            char nm[USER_NAME_MAX];
            str_copy(nm, users[i].name, sizeof(nm));
            if (user_del(nm) < 0) {
                str_copy(set_msg, user_err, sizeof(set_msg));
                set_msg_err = 1;
            } else {
                str_copy(set_msg, "deleted ", sizeof(set_msg));
                str_append(set_msg, nm, sizeof(set_msg));
                set_msg_err = 0;
                set_user_sel = -1;
            }
            return;
        }
        return;
    }

    /* new-account row: two fields and an Add button */
    {
        int32_t ry = cy + 380 + user_count * 26 + 8;
        if (my < ry - 3 || my >= ry + 19) return;

        int32_t fx = cx + 62;
        for (int f = 0; f < 2; f++) {
            int32_t bx = fx + f * (96 + 8);
            if (mx >= bx && mx < bx + 96) { set_field = f; return; }
        }

        int32_t gx = fx + 2 * (96 + 8);

        /* "Set" changes the password of whichever account is selected,
         * which is how someone changes their own without the shell ever
         * seeing it. */
        int32_t sx = gx + 58;
        if (mx >= sx && mx < sx + 52) {
            if (set_user_sel < 0) {
                str_copy(set_msg, "pick an account first", sizeof(set_msg));
                set_msg_err = 1;
            } else if (user_set_password(set_user_sel, set_newpw) < 0) {
                str_copy(set_msg, user_err, sizeof(set_msg));
                set_msg_err = 1;
            } else {
                str_copy(set_msg, "password changed for ", sizeof(set_msg));
                str_append(set_msg, user_name_of(set_user_sel), sizeof(set_msg));
                set_msg_err = 0;
            }
            for (uint32_t i = 0; i < sizeof(set_newpw); i++) set_newpw[i] = '\0';
            set_newpw_mask[0] = '\0';
            return;
        }

        if (mx >= gx && mx < gx + 52) {
            if (user_add(set_newname, set_newpw, 0) < 0) {
                str_copy(set_msg, user_err, sizeof(set_msg));
                set_msg_err = 1;
            } else {
                str_copy(set_msg, "created /home/", sizeof(set_msg));
                str_append(set_msg, set_newname, sizeof(set_msg));
                set_msg_err = 0;
                set_newname[0] = '\0';
                set_field = 0;
            }
            /* the plaintext does not outlive the hashing */
            for (uint32_t i = 0; i < sizeof(set_newpw); i++) set_newpw[i] = '\0';
            set_newpw_mask[0] = '\0';
        }
    }
}

/* Typing into the Users pane's fields. */
static void settings_key(char ch) {
    if (!user_is_admin(user_current)) return;

    char *buf = set_field == 0 ? set_newname : set_newpw;
    int   max = set_field == 0 ? USER_NAME_MAX : (int)sizeof(set_newpw);

    int len = 0;
    while (buf[len]) len++;

    if (ch == '\t') {
        set_field = !set_field;
        return;
    }
    if (ch == '\b') {
        if (len > 0) buf[--len] = '\0';
    } else if (ch == '\n') {
        set_field = set_field == 0 ? 1 : 0;
        return;
    } else if (ch >= 0x20 && ch < 0x7F && len < max - 1) {
        buf[len++] = ch;
        buf[len] = '\0';
    } else {
        return;
    }

    /* keep the mask in step with the password's length */
    if (set_field == 1) {
        int n = 0;
        while (set_newpw[n]) { set_newpw_mask[n] = '*'; n++; }
        set_newpw_mask[n] = '\0';
    }
}

static void settings_draw(uint32_t *buf, uint32_t w, uint32_t h,
                          int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                          uint32_t tick, int focused) {
    (void)tick; (void)focused;
    gfx_rect(buf, w, h, cx, cy, cw, chh, C_WIN_BG);

    ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 14, "Wallpaper",
                    0x1A1E28u, 16);
    gfx_rect(buf, w, h, cx + 24, cy + 38, cw - 48, 1, 0xD5D8E0u);

    for (int i = 0; i < WALL_THEME_COUNT; i++) {
        int32_t sx = cx + 24 + i * 56;
        int32_t sy = cy + 52;
        gfx_vgrad(buf, w, h, sx, sy, 44, 34,
                  wall_theme_top[i], wall_theme_bot[i]);
        gfx_rect_outline(buf, w, h, sx - 1, sy - 1, 46, 36,
                         i == wall_theme ? C_GOLD : 0xB8BCC8u);
        if (i == wall_theme)
            gfx_rect_outline(buf, w, h, sx - 2, sy - 2, 48, 38, C_GOLD);
    }
    ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 92,
                    wall_theme_names[wall_theme], 0x60666Fu, 12);

    /* Slideshow: how often the wallpaper moves to the next theme. */
    for (int i = 0; i < WALL_SLIDE_COUNT; i++) {
        int32_t bx = cx + 130 + i * 58;
        int32_t by = cy + 88;
        const int on = (i == wall_slide);
        gfx_rect(buf, w, h, bx, by, 50, 22, on ? 0xE8E2CCu : 0xF0F1F4u);
        gfx_rect_outline(buf, w, h, bx, by, 50, 22,
                         on ? C_GOLD : 0xC8CCD4u);
        const int tw2 = ttf_text_width(wall_slide_names[i], 11);
        ttf_draw_string(buf, (int)w, (int)h, bx + (50 - tw2) / 2, by + 4,
                        wall_slide_names[i], on ? 0x6A5410u : 0x60666Fu, 11);
    }

    ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 116, "Dock",
                    0x1A1E28u, 16);
    gfx_rect(buf, w, h, cx + 24, cy + 140, cw - 48, 1, 0xD5D8E0u);

    for (int i = 0; i < 3; i++) {
        int32_t bx = cx + 24 + i * 92;
        int32_t by = cy + 150;
        int on = dock_cfg.edge == i;
        gfx_rect(buf, w, h, bx, by, 84, 26, on ? 0x2A2410u : 0xE4E6EBu);
        gfx_rect_outline(buf, w, h, bx, by, 84, 26,
                         on ? C_GOLD : 0xB8BCC8u);
        int tw = ttf_text_width(dock_edge_names[i], 13);
        ttf_draw_string(buf, (int)w, (int)h, bx + (84 - tw) / 2, by + 4,
                        dock_edge_names[i], on ? C_GOLD : 0x40454Fu, 13);
    }

    {
        int32_t by = cy + 196;
        gfx_rect(buf, w, h, cx + 24, by, 36, 26, 0xE4E6EBu);
        gfx_rect_outline(buf, w, h, cx + 24, by, 36, 26, 0xB8BCC8u);
        ttf_draw_string(buf, (int)w, (int)h, cx + 38, by + 3, "-",
                        0x40454Fu, 15);
        gfx_rect(buf, w, h, cx + 68, by, 36, 26, 0xE4E6EBu);
        gfx_rect_outline(buf, w, h, cx + 68, by, 36, 26, 0xB8BCC8u);
        ttf_draw_string(buf, (int)w, (int)h, cx + 80, by + 3, "+",
                        0x40454Fu, 15);

        char sz[32];
        str_copy(sz, "Icon size: ", sizeof(sz));
        char nb[8];
        uint_to_str((uint32_t)dock_cfg.icon_sz, nb);
        str_append(sz, nb, sizeof(sz));
        str_append(sz, " px", sizeof(sz));
        ttf_draw_string(buf, (int)w, (int)h, cx + 116, by + 5, sz,
                        0x60666Fu, 13);
    }

    ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 240, "System",
                    0x1A1E28u, 16);
    gfx_rect(buf, w, h, cx + 24, cy + 264, cw - 48, 1, 0xD5D8E0u);
    {
        char line[64], nb[16];

        str_copy(line, "Vextro 9.0  x86_64", sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 274, line,
                        0x40454Fu, 13);

        str_copy(line, "Display: ", sizeof(line));
        uint_to_str(w, nb); str_append(line, nb, sizeof(line));
        str_append(line, " x ", sizeof(line));
        uint_to_str(h, nb); str_append(line, nb, sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 294, line,
                        0x40454Fu, 13);

        str_copy(line, "Memory: ", sizeof(line));
        uint_to_str((uint32_t)system_total_memory_mb, nb);
        str_append(line, nb, sizeof(line));
        str_append(line, " MB", sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 314, line,
                        0x40454Fu, 13);
    }

    /* ---- Users ----
     *
     * Only administrators see this. Not a security boundary -- a `.vx`
     * application runs with kernel privileges and could do as it likes --
     * but the interface should not offer what it will refuse.
     */
    if (!user_is_admin(user_current)) return;

    ttf_draw_string(buf, (int)w, (int)h, cx + 24, cy + 346, "Users",
                    0x1A1E28u, 16);
    gfx_rect(buf, w, h, cx + 24, cy + 370, cw - 48, 1, 0xD5D8E0u);

    for (int i = 0; i < user_count; i++) {
        int32_t ry = cy + 380 + i * 26;
        int me = (i == user_current);
        int adm = (users[i].flags & USER_FLAG_ADMIN) != 0;

        if (i == set_user_sel)
            gfx_rect(buf, w, h, cx + 20, ry - 2, cw - 40, 24, 0xF0EAD8u);

        ttf_draw_string(buf, (int)w, (int)h, cx + 28, ry, users[i].name,
                        me ? C_GOLD_DIM : 0x40454Fu, 13);

        /* admin toggle */
        int32_t ax = cx + 140;
        gfx_rect(buf, w, h, ax, ry - 1, 62, 20, adm ? 0x2A2410u : 0xE4E6EBu);
        gfx_rect_outline(buf, w, h, ax, ry - 1, 62, 20,
                         adm ? C_GOLD : 0xB8BCC8u);
        ttf_draw_string(buf, (int)w, (int)h, ax + 10, ry + 1, "admin",
                        adm ? C_GOLD : 0x80858Fu, 12);

        /* delete -- never for the account in use */
        if (!me) {
            int32_t dx = cx + 214;
            gfx_rect(buf, w, h, dx, ry - 1, 62, 20, 0xE4E6EBu);
            gfx_rect_outline(buf, w, h, dx, ry - 1, 62, 20, 0xC08080u);
            ttf_draw_string(buf, (int)w, (int)h, dx + 10, ry + 1, "delete",
                            0xA04040u, 12);
        }
    }

    /* new-account row */
    {
        int32_t ry = cy + 380 + user_count * 26 + 8;
        ttf_draw_string(buf, (int)w, (int)h, cx + 24, ry, "New:",
                        0x60666Fu, 12);

        int32_t fx = cx + 62;
        for (int f = 0; f < 2; f++) {
            int32_t bw2 = 96;
            int32_t bx = fx + f * (bw2 + 8);
            gfx_rect(buf, w, h, bx, ry - 3, bw2, 22, 0xFFFFFFu);
            gfx_rect_outline(buf, w, h, bx, ry - 3, bw2, 22,
                             set_field == f ? C_GOLD : 0xB8BCC8u);
            const char *txt = f == 0 ? set_newname : set_newpw_mask;
            const char *ph  = f == 0 ? "username" : "password";
            if (txt[0])
                ttf_draw_string(buf, (int)w, (int)h, bx + 6, ry, txt,
                                C_INK, 12);
            else
                ttf_draw_string(buf, (int)w, (int)h, bx + 6, ry, ph,
                                0xA0A4AEu, 12);
        }

        int32_t gx = fx + 2 * (96 + 8);
        gfx_rect(buf, w, h, gx, ry - 3, 52, 22, 0x2A2410u);
        gfx_rect_outline(buf, w, h, gx, ry - 3, 52, 22, C_GOLD);
        ttf_draw_string(buf, (int)w, (int)h, gx + 12, ry, "Add",
                        C_GOLD, 12);

        /* Change the selected account's password to what is typed. */
        int32_t sx = gx + 58;
        gfx_rect(buf, w, h, sx, ry - 3, 52, 22, 0xE4E6EBu);
        gfx_rect_outline(buf, w, h, sx, ry - 3, 52, 22, 0xB8BCC8u);
        ttf_draw_string(buf, (int)w, (int)h, sx + 12, ry, "Set",
                        0x40454Fu, 12);

        if (set_msg[0])
            ttf_draw_string(buf, (int)w, (int)h, cx + 24, ry + 26, set_msg,
                            set_msg_err ? 0xB0322Eu : 0x50755Fu, 12);
    }
}

/* ===================== GOLDSMITH (paint) ===================== */

#define PAINT_MAX_W 640
#define PAINT_MAX_H 420
#define PAINT_TOOLBAR_H 34

static uint32_t paint_canvas[PAINT_MAX_W * PAINT_MAX_H];
static int      paint_inited = 0;
static uint32_t paint_color = 0xD4AF37u;
static int      paint_brush = 4;
static int32_t  paint_last_x = -1;
static int32_t  paint_last_y = -1;

static const uint32_t paint_colors[8] = {
    0xD4AF37u, 0x1A1E28u, 0xE05252u, 0x4FC87Au,
    0x5090E0u, 0xE08030u, 0x9060D0u, 0xFFFFFFu,
};

static void paint_clear(void) {
    for (int i = 0; i < PAINT_MAX_W * PAINT_MAX_H; i++)
        paint_canvas[i] = 0xFFFFFFu;
}

static void paint_dot(int32_t x, int32_t y) {
    int half = paint_brush / 2;
    for (int dy = -half; dy <= half; dy++)
        for (int dx = -half; dx <= half; dx++) {
            if (dx * dx + dy * dy > half * half + 1) continue;
            int32_t px = x + dx, py = y + dy;
            if (px >= 0 && px < PAINT_MAX_W && py >= 0 && py < PAINT_MAX_H)
                paint_canvas[py * PAINT_MAX_W + px] = paint_color;
        }
}

static void paint_stroke(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        paint_dot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void paint_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                        int over_top) {
    if (!paint_inited) { paint_clear(); paint_inited = 1; }
    int click = (lmb && !prev_lmb);

    /* toolbar: swatches */
    if (click) {
        for (int i = 0; i < 8; i++) {
            int32_t sx = cx + 8 + i * 30;
            int32_t sy = cy + 5;
            if (mx >= sx && mx < sx + 24 && my >= sy && my < sy + 24) {
                paint_color = paint_colors[i];
                return;
            }
        }
        /* brush sizes */
        for (int i = 0; i < 3; i++) {
            int32_t bx = cx + 8 + 8 * 30 + 10 + i * 30;
            int32_t by = cy + 5;
            if (mx >= bx && mx < bx + 24 && my >= by && my < by + 24) {
                paint_brush = 2 + i * 4;   /* 2 / 6 / 10 */
                return;
            }
        }
        /* clear button */
        int32_t clx = cx + cw - 62;
        if (mx >= clx && mx < clx + 54 && my >= cy + 5 && my < cy + 29) {
            paint_clear();
            return;
        }
    }

    /* canvas strokes */
    int32_t can_y = cy + PAINT_TOOLBAR_H;
    int32_t can_h = chh - PAINT_TOOLBAR_H;
    if (can_h > PAINT_MAX_H) can_h = PAINT_MAX_H;
    int32_t can_w = cw < PAINT_MAX_W ? cw : PAINT_MAX_W;

    if (lmb && over_top &&
        mx >= cx && mx < cx + can_w && my >= can_y && my < can_y + can_h) {
        int32_t px = mx - cx;
        int32_t py = my - can_y;
        if (paint_last_x >= 0)
            paint_stroke(paint_last_x, paint_last_y, px, py);
        else
            paint_dot(px, py);
        paint_last_x = px;
        paint_last_y = py;
    } else {
        paint_last_x = -1;
        paint_last_y = -1;
    }
}

static void paint_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       uint32_t tick, int focused) {
    (void)tick; (void)focused;
    if (!paint_inited) { paint_clear(); paint_inited = 1; }

    /* toolbar */
    gfx_rect(buf, w, h, cx, cy, cw, PAINT_TOOLBAR_H, C_BG_PANEL);
    for (int i = 0; i < 8; i++) {
        int32_t sx = cx + 8 + i * 30;
        gfx_rect(buf, w, h, sx, cy + 5, 24, 24, paint_colors[i]);
        gfx_rect_outline(buf, w, h, sx, cy + 5, 24, 24,
                         paint_colors[i] == paint_color ? C_GOLD : 0x3A4050u);
        if (paint_colors[i] == paint_color)
            gfx_rect_outline(buf, w, h, sx - 1, cy + 4, 26, 26, C_GOLD);
    }
    for (int i = 0; i < 3; i++) {
        int32_t bx = cx + 8 + 8 * 30 + 10 + i * 30;
        int sz = 2 + i * 4;
        int on = paint_brush == sz;
        gfx_rect(buf, w, h, bx, cy + 5, 24, 24, on ? 0x2A2410u : 0x202535u);
        gfx_rect_outline(buf, w, h, bx, cy + 5, 24, 24,
                         on ? C_GOLD : 0x3A4050u);
        gfx_circle(buf, w, h, bx + 12, cy + 17, sz / 2 + 1,
                   on ? C_GOLD : C_TEXT_DIM);
    }
    {
        int32_t clx = cx + cw - 62;
        gfx_rect(buf, w, h, clx, cy + 5, 54, 24, 0x202535u);
        gfx_rect_outline(buf, w, h, clx, cy + 5, 54, 24, 0x3A4050u);
        ttf_draw_string(buf, (int)w, (int)h, clx + 10, cy + 8, "Clear",
                        C_TEXT_DIM, 12);
    }

    /* canvas blit */
    int32_t can_y = cy + PAINT_TOOLBAR_H;
    int32_t can_h = chh - PAINT_TOOLBAR_H;
    if (can_h > PAINT_MAX_H) can_h = PAINT_MAX_H;
    int32_t can_w = cw < PAINT_MAX_W ? cw : PAINT_MAX_W;
    for (int32_t y = 0; y < can_h; y++) {
        int32_t dy = can_y + y;
        if (dy < 0 || dy >= (int32_t)h) continue;
        for (int32_t x = 0; x < can_w; x++) {
            int32_t dx = cx + x;
            if (dx < 0 || dx >= (int32_t)w) continue;
            buf[(uint32_t)dy * w + (uint32_t)dx] =
                paint_canvas[y * PAINT_MAX_W + x];
        }
    }
}

/* ===================== MONOLITH (system monitor) ===================== */

static void sysmon_row(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t x, int32_t y, const char *key,
                       const char *val, uint32_t val_col) {
    ttf_draw_string(buf, (int)w, (int)h, x, y, key, C_TEXT_DIM, 13);
    ttf_draw_string(buf, (int)w, (int)h, x + 130, y, val, val_col, 13);
}

static const char *tcp_state_names[6] = {
    "closed", "syn-sent", "established", "fin-wait-1", "fin-wait-2", "last-ack"
};

static void sysmon_draw(uint32_t *buf, uint32_t w, uint32_t h,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                        uint32_t tick, int focused) {
    (void)focused;
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x10131Cu);

    ttf_draw_string(buf, (int)w, (int)h, cx + 16, cy + 10, "MONOLITH",
                    C_GOLD, 17);
    ttf_draw_string(buf, (int)w, (int)h, cx + 110, cy + 14,
                    "system monitor", C_TEXT_DIM, 12);
    gfx_rect(buf, w, h, cx + 16, cy + 36, cw - 32, 1, 0x2A3040u);

    char nb[24];
    int32_t y = cy + 48;

    /* uptime */
    {
        uint32_t secs = tick / 60;
        char up[32];
        uint_to_str(secs / 3600, nb); str_copy(up, nb, sizeof(up));
        str_append(up, "h ", sizeof(up));
        uint_to_str((secs / 60) % 60, nb); str_append(up, nb, sizeof(up));
        str_append(up, "m ", sizeof(up));
        uint_to_str(secs % 60, nb); str_append(up, nb, sizeof(up));
        str_append(up, "s", sizeof(up));
        sysmon_row(buf, w, h, cx + 16, y, "uptime", up, C_TEXT);
    }
    y += 22;
    {
        char res[24];
        uint_to_str(w, nb); str_copy(res, nb, sizeof(res));
        str_append(res, " x ", sizeof(res));
        uint_to_str(h, nb); str_append(res, nb, sizeof(res));
        sysmon_row(buf, w, h, cx + 16, y, "display", res, C_TEXT);
    }
    y += 22;
    {
        char mem[24];
        uint_to_str((uint32_t)system_total_memory_mb, nb);
        str_copy(mem, nb, sizeof(mem));
        str_append(mem, " MB", sizeof(mem));
        sysmon_row(buf, w, h, cx + 16, y, "memory", mem, C_TEXT);
    }
    y += 22;
    {
        char mp[32];
        uint_to_str((uint32_t)(mouse_x < 0 ? 0 : mouse_x), nb);
        str_copy(mp, nb, sizeof(mp));
        str_append(mp, ", ", sizeof(mp));
        uint_to_str((uint32_t)(mouse_y < 0 ? 0 : mouse_y), nb);
        str_append(mp, nb, sizeof(mp));
        sysmon_row(buf, w, h, cx + 16, y, "pointer", mp, C_TEXT);
    }
    y += 22;
    {
        uint_to_str(tick, nb);
        sysmon_row(buf, w, h, cx + 16, y, "frames", nb, C_TEXT);
    }

    y += 30;
    ttf_draw_string(buf, (int)w, (int)h, cx + 16, y, "NETWORK", C_GOLD, 13);
    gfx_rect(buf, w, h, cx + 16, y + 20, cw - 32, 1, 0x2A3040u);
    y += 28;

    if (e1000_found) {
        uint32_t status = e1000_read(E1000_STATUS);
        sysmon_row(buf, w, h, cx + 16, y, "link",
                   (status & E1000_STATUS_LU) ? "up" : "down",
                   (status & E1000_STATUS_LU) ? C_GREEN : C_RED);
        y += 22;
        char ipb[24];
        ip_to_str(net_our_ip, ipb);
        sysmon_row(buf, w, h, cx + 16, y, "address", ipb, C_TEXT);
        y += 22;
        sysmon_row(buf, w, h, cx + 16, y, "tcp",
                   tcp_state_names[tcp_state], C_TEXT);
        y += 22;
        int arps = 0;
        for (int i = 0; i < ARP_CACHE_SIZE; i++)
            if (arp_cache[i].valid) arps++;
        uint_to_str((uint32_t)arps, nb);
        sysmon_row(buf, w, h, cx + 16, y, "arp entries", nb, C_TEXT);
        y += 22;
    } else {
        sysmon_row(buf, w, h, cx + 16, y, "adapter", "not found", C_RED);
        y += 22;
    }

    y += 8;
    ttf_draw_string(buf, (int)w, (int)h, cx + 16, y, "STORAGE", C_GOLD, 13);
    gfx_rect(buf, w, h, cx + 16, y + 20, cw - 32, 1, 0x2A3040u);
    y += 28;

    if (fs_writable()) {
        char v[48];
        str_copy(v, fs_name(), sizeof(v));
        str_append(v, ", ", sizeof(v));
        uint_to_str(fs_free_kb() / 1024, nb);
        str_append(v, nb, sizeof(v));
        str_append(v, " / ", sizeof(v));
        uint_to_str(fs_total_kb() / 1024, nb);
        str_append(v, nb, sizeof(v));
        str_append(v, " MB free", sizeof(v));
        sysmon_row(buf, w, h, cx + 16, y, "disk", v, C_GREEN);
    } else {
        sysmon_row(buf, w, h, cx + 16, y, "disk", fs_name(), C_RED);
    }
    y += 22;
    sysmon_row(buf, w, h, cx + 16, y, "gpu",
               igpu.active ? igpu.name : "CPU renderer",
               igpu.active ? C_GREEN : C_TEXT);

    /* activity pulse bar */
    {
        int32_t bx = cx + 16;
        int32_t by = cy + chh - 26;
        int32_t bw2 = cw - 32;
        gfx_rect(buf, w, h, bx, by, bw2, 10, 0x1A1F2Cu);
        int fill = (int)((tick % 120) * (uint32_t)bw2 / 120);
        gfx_rect(buf, w, h, bx, by, fill, 10, C_GOLD_DIM);
        gfx_rect_outline(buf, w, h, bx, by, bw2, 10, 0x2A3040u);
    }
}

/* ===================== MATRIX RAIN ===================== */

#define MTX_MAX_COLS 100

static int     mtx_inited = 0;
static int32_t mtx_y[MTX_MAX_COLS];       /* head position (rows, fixed .4) */
static int32_t mtx_speed[MTX_MAX_COLS];   /* rows per frame in .4 fp        */
static int32_t mtx_len[MTX_MAX_COLS];

static void mtx_init(void) {
    for (int i = 0; i < MTX_MAX_COLS; i++) {
        mtx_y[i] = -(int32_t)(gfx_rand() % 400);
        mtx_speed[i] = 3 + (int32_t)(gfx_rand() % 14);
        mtx_len[i] = 6 + (int32_t)(gfx_rand() % 14);
    }
    mtx_inited = 1;
}

static void mtx_draw(uint32_t *buf, uint32_t w, uint32_t h,
                     int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                     uint32_t tick, int focused) {
    (void)focused;
    if (!mtx_inited) mtx_init();

    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x05070Au);

    int cols = cw / 9;
    if (cols > MTX_MAX_COLS) cols = MTX_MAX_COLS;
    int rows = chh / 11;

    for (int c = 0; c < cols; c++) {
        mtx_y[c] += mtx_speed[c];
        int head = mtx_y[c] >> 4;

        if (head - mtx_len[c] > rows) {
            mtx_y[c] = -(int32_t)(gfx_rand() % 60) << 4;
            mtx_speed[c] = 3 + (int32_t)(gfx_rand() % 14);
            mtx_len[c] = 6 + (int32_t)(gfx_rand() % 14);
            head = mtx_y[c] >> 4;
        }

        for (int t = 0; t < mtx_len[c]; t++) {
            int row = head - t;
            if (row < 0 || row >= rows) continue;
            /* deterministic pseudo-random glyph that mutates over time */
            uint32_t g = (uint32_t)(c * 31 + row * 17 + (tick / 6) * (t == 0 ? 13 : 1));
            char ch = (char)(0x21 + (g % 0x5D));
            uint32_t col;
            if (t == 0) col = 0xE8F0D8u;                       /* bright head */
            else if (t < 3) col = C_GOLD;
            else {
                uint32_t fade = 200 - (uint32_t)t * 180 / (uint32_t)mtx_len[c];
                col = gfx_mix(0x30A050u, 0x05070Au, fade);
            }
            mono_char(buf, w, h, cx + c * 9, cy + row * 11, ch, col, 1);
        }
    }
}

/* ===================== PHOTOS (.sci viewer) =====================
 *
 * A gallery down the left, the decoded image on the right.  Decoding is
 * the expensive part — LZMA over the whole plane — so it happens once
 * when a picture is selected, never per frame.
 */

#define IMG_MAX_FILES 32
#define IMG_SIDEBAR_W 150
#define IMG_INFO_H    26

typedef struct {
    char name[64];
    uint32_t size;
} img_entry_t;

static img_entry_t img_files[IMG_MAX_FILES];
static int         img_file_count = 0;
static int         img_sel = -1;
static int         img_scanned = 0;

static sci_info_t  img_info;
static int         img_loaded = 0;
static uint32_t    img_file_size = 0;
static char        img_name[64] = "";
static char        img_msg[96] = "";
static int         img_msg_err = 0;
static int         img_actual = 0;    /* 1 = 1:1 pixels, 0 = fit to window */

static const char *img_dirs[2] = { "/pics", "/" };

static int img_is_sci(const char *name) {
    int n = str_len(name);
    return n > 4 && name[n - 4] == '.' && name[n - 3] == 's' &&
           name[n - 2] == 'c' && name[n - 1] == 'i';
}

static void img_scan_cb(const char *name, uint32_t size, int is_dir) {
    if (is_dir || !img_is_sci(name)) return;
    if (img_file_count >= IMG_MAX_FILES) return;
    for (int i = 0; i < img_file_count; i++)
        if (str_eq(img_files[i].name, name)) return;
    img_entry_t *e = &img_files[img_file_count++];
    str_copy(e->name, name, sizeof(e->name));
    e->size = size;
}

static char img_scan_dir[32];

static void img_scan(void) {
    img_file_count = 0;
    img_scanned = 1;
    for (int d = 0; d < 2; d++) {
        str_copy(img_scan_dir, img_dirs[d], sizeof(img_scan_dir));
        fs_list(img_scan_dir, img_scan_cb);
    }
}

/* Build the full path for entry idx by trying each gallery directory. */
static int img_path_for(const char *name, char *out, int max) {
    for (int d = 0; d < 2; d++) {
        str_copy(out, img_dirs[d], max);
        if (!str_eq(img_dirs[d], "/")) str_append(out, "/", max);
        str_append(out, name, max);
        int is_dir = 0;
        if (fs_writable() && fs_stat(out, 0, &is_dir) && !is_dir)
            return 1;
    }
    str_copy(out, "/", max);
    str_append(out, name, max);
    return 0;
}

static int img_open_path(const char *path) {
    uint64_t len = 0;
    const void *data = fs_read_file(path, &len);
    if (!data || len == 0) {
        img_loaded = 0;
        str_copy(img_msg, "cannot read ", sizeof(img_msg));
        str_append(img_msg, path, sizeof(img_msg));
        img_msg_err = 1;
        return -1;
    }

    recent_push(WK_IMAGE, path, path);

    const char *bad = sci_decode((const uint8_t *)data, len, &img_info);
    if (bad) {
        img_loaded = 0;
        str_copy(img_msg, bad, sizeof(img_msg));
        img_msg_err = 1;
        return -1;
    }

    img_loaded = 1;
    img_file_size = (uint32_t)len;
    img_msg_err = 0;

    /* remember the leaf name for the title bar */
    const char *leaf = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') leaf = p + 1;
    str_copy(img_name, leaf, sizeof(img_name));

    char nb[16];
    str_copy(img_msg, "", sizeof(img_msg));
    uint_to_str(img_info.width, nb);  str_append(img_msg, nb, sizeof(img_msg));
    str_append(img_msg, " x ", sizeof(img_msg));
    uint_to_str(img_info.height, nb); str_append(img_msg, nb, sizeof(img_msg));
    str_append(img_msg, "   ", sizeof(img_msg));
    uint_to_str(img_info.raw_size / 1024, nb);
    str_append(img_msg, nb, sizeof(img_msg));
    str_append(img_msg, " KB raw -> ", sizeof(img_msg));
    uint_to_str(img_file_size / 1024, nb);
    str_append(img_msg, nb, sizeof(img_msg));
    str_append(img_msg, " KB on disk  (", sizeof(img_msg));
    uint_to_str(img_file_size * 100 / (img_info.raw_size ? img_info.raw_size : 1),
                nb);
    str_append(img_msg, nb, sizeof(img_msg));
    str_append(img_msg, "%)", sizeof(img_msg));
    return 0;
}

static int img_open_named(const char *name) {
    char path[160];
    img_path_for(name, path, sizeof(path));
    if (!img_scanned) img_scan();
    for (int i = 0; i < img_file_count; i++)
        if (str_eq(img_files[i].name, name)) { img_sel = i; break; }
    return img_open_path(path);
}

static const char *img_status(void) { return img_msg; }

static void img_select(int idx) {
    if (idx < 0 || idx >= img_file_count) return;
    img_sel = idx;
    char path[160];
    img_path_for(img_files[idx].name, path, sizeof(path));
    img_open_path(path);
}

static void img_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    int click = (lmb && !prev_lmb);
    if (!click) return;
    if (mx < cx || mx >= cx + cw || my < cy || my >= cy + chh) return;

    if (mx < cx + IMG_SIDEBAR_W) {
        int idx = (my - cy - 26) / 20;
        if (idx >= 0 && idx < img_file_count) img_select(idx);
        return;
    }
    /* clicking the picture toggles fit / actual size */
    if (img_loaded && my < cy + chh - IMG_INFO_H) img_actual = !img_actual;
}

static void img_key(char ch) {
    if (ch == KEY_DOWN && img_sel + 1 < img_file_count) img_select(img_sel + 1);
    else if (ch == KEY_UP && img_sel > 0) img_select(img_sel - 1);
    else if (ch == ' ') img_actual = !img_actual;
    else if (ch == 'r' || ch == 'R') { img_scan(); }
    else if (ch == 27) wm_close(WK_IMAGE);
}

static void img_draw(uint32_t *buf, uint32_t w, uint32_t h,
                     int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                     uint32_t tick, int focused) {
    (void)focused;
    if (!img_scanned) img_scan();
    if (tick % 240 == 0) {
        int keep = img_sel;
        img_scan();
        if (keep < img_file_count) img_sel = keep;
    }

    /* ---- sidebar ---- */
    gfx_rect(buf, w, h, cx, cy, IMG_SIDEBAR_W, chh, 0x1A1E2Au);
    gfx_rect(buf, w, h, cx + IMG_SIDEBAR_W - 1, cy, 1, chh, 0x2E3444u);
    ttf_draw_string(buf, (int)w, (int)h, cx + 12, cy + 6, "Pictures",
                    C_GOLD, 13);

    for (int i = 0; i < img_file_count; i++) {
        int32_t ry = cy + 26 + i * 20;
        if (ry + 18 > cy + chh) break;
        if (i == img_sel)
            gfx_rect(buf, w, h, cx + 2, ry - 2, IMG_SIDEBAR_W - 5, 20,
                     0x2A2410u);
        char label[22];
        store_fit(label, sizeof(label), img_files[i].name,
                  IMG_SIDEBAR_W - 24, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + 12, ry, label,
                        i == img_sel ? C_GOLD : C_TEXT_DIM, 12);
    }
    if (img_file_count == 0)
        ttf_draw_string(buf, (int)w, (int)h, cx + 12, cy + 30,
                        "no .sci files", 0x6A7284u, 12);

    /* ---- picture area ---- */
    int32_t vx = cx + IMG_SIDEBAR_W;
    int32_t vw = cw - IMG_SIDEBAR_W;
    int32_t vh = chh - IMG_INFO_H;
    gfx_rect(buf, w, h, vx, cy, vw, vh, 0x0E1016u);

    if (img_loaded && vw > 8 && vh > 8) {
        uint32_t iw = img_info.width, ih = img_info.height;
        int32_t dw, dh;

        if (img_actual) {
            dw = (int32_t)iw;
            dh = (int32_t)ih;
        } else {
            /* fit, preserving aspect, never upscaling past 1:1 */
            int32_t sw = (int32_t)((uint64_t)vw * 1024 / iw);
            int32_t sh = (int32_t)((uint64_t)vh * 1024 / ih);
            int32_t s = sw < sh ? sw : sh;
            if (s > 1024) s = 1024;
            dw = (int32_t)((uint64_t)iw * s / 1024);
            dh = (int32_t)((uint64_t)ih * s / 1024);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
        }

        int32_t ox = vx + (vw - dw) / 2;
        int32_t oy = cy + (vh - dh) / 2;

        for (int32_t y = 0; y < dh; y++) {
            int32_t dy = oy + y;
            if (dy < cy || dy >= cy + vh) continue;
            uint32_t sy = (uint32_t)((uint64_t)y * ih / (uint64_t)dh);
            if (sy >= ih) sy = ih - 1;
            const uint32_t *srow = sci_pixels + (uint64_t)sy * iw;
            for (int32_t x = 0; x < dw; x++) {
                int32_t dx = ox + x;
                if (dx < vx || dx >= vx + vw) continue;
                uint32_t sx = (uint32_t)((uint64_t)x * iw / (uint64_t)dw);
                if (sx >= iw) sx = iw - 1;
                buf[(uint32_t)dy * w + (uint32_t)dx] = srow[sx];
            }
        }
    } else if (!img_loaded) {
        const char *msg = img_msg_err ? img_msg
                                      : "Select a picture from the list";
        int tw = ttf_text_width(msg, 13);
        ttf_draw_string(buf, (int)w, (int)h, vx + (vw - tw) / 2,
                        cy + vh / 2 - 8, msg,
                        img_msg_err ? C_RED : 0x6A7284u, 13);
    }

    /* ---- info bar ---- */
    gfx_rect(buf, w, h, vx, cy + vh, vw, IMG_INFO_H, C_BG_PANEL);
    gfx_rect(buf, w, h, vx, cy + vh, vw, 1, 0x2E3444u);
    if (img_loaded) {
        ttf_draw_string(buf, (int)w, (int)h, vx + 10, cy + vh + 5, img_name,
                        C_GOLD, 12);
        int nw = ttf_text_width(img_name, 12);
        char fit[96];
        store_fit(fit, sizeof(fit), img_msg, vw - nw - 90, 12);
        ttf_draw_string(buf, (int)w, (int)h, vx + 20 + nw, cy + vh + 5, fit,
                        C_TEXT_DIM, 12);
        const char *z = img_actual ? "1:1" : "fit";
        ttf_draw_string(buf, (int)w, (int)h, vx + vw - 30, cy + vh + 5, z,
                        C_TEXT_DIM, 12);
    } else if (img_msg_err) {
        ttf_draw_string(buf, (int)w, (int)h, vx + 10, cy + vh + 5, img_msg,
                        C_RED, 12);
    }
}

/* ===================== WIKIPEDIA (ZIM browser) =====================
 *
 * Type a title, get the entries that start with it.  The archive's path
 * list is sorted, so a prefix search is one binary search plus a walk —
 * no index is built and nothing is held in memory.
 */

#define WIKI_RESULTS   16
#define WIKI_QUERY_MAX 64
#define WIKI_ROW_H     22

typedef struct {
    uint32_t index;
    char     title[72];
    int      redirect;
} wiki_hit_t;

static char       wiki_query[WIKI_QUERY_MAX] = "";
static int        wiki_qlen = 0;
static wiki_hit_t wiki_hits[WIKI_RESULTS];
static int        wiki_hit_count = 0;
static int        wiki_sel = 0;
static char       wiki_status[112] = "";
static int        wiki_mode;            /* 0 search, 1 chat */
static void       wiki_chat_key(char ch);
static void       wiki_feed(int k);
static int        wiki_status_err = 0;
static int        wiki_tried_open = 0;

/* Archives people actually leave lying around, tried in order. */
static const char *wiki_candidates[4] = {
    "/wiki.zim", "/wikipedia.zim", "/zim/wiki.zim", "/wiki/wiki.zim"
};

static void wiki_search(void);

/* ===== reading an article, in this window =====
 *
 * Opening a result used to hand a zim:// URL to the browser and raise the
 * browser's window, which is why reading an article looked like being
 * thrown into a different application — and why Back, being the browser's
 * history, led back to whatever page the browser had been showing rather
 * than to the search results.
 *
 * The article is now laid out by wikidoc.h and drawn in this window, over
 * the result list, with its own history. The browser still understands
 * zim:// and is untouched.
 */

#define WIKI_HIST_MAX 12
#define WIKI_PATH_MAX 160

typedef struct {
    char path[WIKI_PATH_MAX];
    int  scroll;
} wiki_hist_t;

static int         wiki_view = 0;             /* 0 results, 1 article */
static char        wiki_art_path[WIKI_PATH_MAX] = "";
static char        wiki_art_title[WD_TITLE_MAX] = "";
static int         wiki_scroll = 0;
static int         wiki_hover = -1;
static wiki_hist_t wiki_hist[WIKI_HIST_MAX];
static int         wiki_hist_n = 0;
static int         wiki_art_wrap = 0;
static int         wiki_last_cw = 0;
static int         wiki_view_h = 0;           /* for the wheel handler */
static int         wiki_sb_drag = 0;

/* Text column, allowing for the margins and the scrollbar. */
static int wiki_wrap_for(int cw) {
    int px = cw - 28 - 14;
    return px > 120 ? px : 120;
}

/*
 * Turn a link found inside an article into a ZIM entry path.
 *
 * Article markup addresses siblings relatively — "Moon", "./Moon" and
 * "../A/Moon" all mean the entry Moon — and percent-encodes them. Same
 * rules the browser applies in brw_resolve_href; brw_pct_decode is reused
 * rather than reimplemented.
 */
static void wiki_resolve_link(const char *href, char *out, int max) {
    out[0] = '\0';
    if (!href || !href[0]) return;
    if (str_starts_with(href, "http://") || str_starts_with(href, "https://") ||
        str_starts_with(href, "//") || str_starts_with(href, "vextro://"))
        return;                                  /* not an archive entry */

    const char *q = href;
    if (str_starts_with(q, "zim://")) q += 6;
    while (str_starts_with(q, "./")) q += 2;
    while (str_starts_with(q, "../")) q += 3;
    if (q[0] && q[1] == '/' &&
        (q[0] == 'A' || q[0] == 'C' || q[0] == 'I' || q[0] == 'M'))
        q += 2;

    brw_pct_decode(q, out, max);
    for (int i = 0; out[i]; i++) if (out[i] == '#') { out[i] = '\0'; break; }
}

/*
 * Load an entry and lay it out. `push` records where we were, so Back can
 * return to it.
 */
static int wiki_load(const char *path, int push) {
    if (!zim.open || !path || !path[0]) return -1;

    uint32_t idx;
    if (!zim_find('C', path, &idx)) {
        str_copy(wiki_status, "No entry named ", sizeof(wiki_status));
        str_append(wiki_status, path, sizeof(wiki_status));
        wiki_status_err = 1;
        return -1;
    }

    const uint8_t *data;
    uint32_t len;
    zim_dirent_t e;
    if (zim_content(idx, &data, &len, &e) != 0) {
        str_copy(wiki_status, zim_err, sizeof(wiki_status));
        wiki_status_err = 1;
        return -1;
    }

    if (push && wiki_view == 1 && wiki_art_path[0] &&
        wiki_hist_n < WIKI_HIST_MAX) {
        str_copy(wiki_hist[wiki_hist_n].path, wiki_art_path, WIKI_PATH_MAX);
        wiki_hist[wiki_hist_n].scroll = wiki_scroll;
        wiki_hist_n++;
    } else if (push && wiki_view == 1 && wiki_art_path[0]) {
        /* full: drop the oldest so the most recent depth is kept */
        for (int i = 1; i < WIKI_HIST_MAX; i++) wiki_hist[i - 1] = wiki_hist[i];
        str_copy(wiki_hist[WIKI_HIST_MAX - 1].path, wiki_art_path, WIKI_PATH_MAX);
        wiki_hist[WIKI_HIST_MAX - 1].scroll = wiki_scroll;
    }

    int wrap = wiki_wrap_for(wiki_last_cw > 0 ? wiki_last_cw
                                              : wk_meta[WK_WIKI].w);
    wdoc_parse(data, (int)len, wrap);
    wiki_art_wrap = wrap;

    str_copy(wiki_art_path, path, WIKI_PATH_MAX);
    if (e.title[0]) str_copy(wiki_art_title, e.title, WD_TITLE_MAX);
    else if (wd_title[0]) str_copy(wiki_art_title, wd_title, WD_TITLE_MAX);
    else str_copy(wiki_art_title, path, WD_TITLE_MAX);

    /* Recorded by title, since that is what an article is called, but
     * reopened by its ZIM path, which is what the archive is keyed on. */
    recent_push(WK_WIKI, wiki_art_title, path);

    wiki_view = 1;
    wiki_scroll = 0;
    wiki_hover = -1;
    wiki_status_err = 0;
    str_copy(wiki_status, wiki_art_path, sizeof(wiki_status));
    return 0;
}

/* Back: to the previous article, or out to the results if this was the
 * first one. Never to another application. */
static void wiki_back(void) {
    if (wiki_hist_n > 0) {
        wiki_hist_n--;
        char path[WIKI_PATH_MAX];
        int  sc = wiki_hist[wiki_hist_n].scroll;
        str_copy(path, wiki_hist[wiki_hist_n].path, sizeof(path));
        if (wiki_load(path, 0) == 0) wiki_scroll = sc;   /* restore the place */
        return;
    }
    wiki_view = 0;
    wiki_scroll = 0;
    wiki_hover = -1;
}

static void wiki_scroll_by(int dy) {
    int maxs = wd_total_h - wiki_view_h;
    if (maxs < 0) maxs = 0;
    wiki_scroll += dy;
    if (wiki_scroll > maxs) wiki_scroll = maxs;
    if (wiki_scroll < 0) wiki_scroll = 0;
}

/*
 * Open whatever the archive nominates as its front door.
 *
 * Without this an empty query lists from the very first entry, which in
 * path order is punctuation — `!`, `!!`, `"` — and reads as garbage.  Every
 * Kiwix archive carries a main page; this is what Kiwix itself opens.
 *
 * Prefer the W/mainPage entry: the header's main_page field is an index
 * whose meaning changed between archive versions, so it is the fallback
 * rather than the first choice.
 */
static int wiki_want_main = 0;   /* set during draw, acted on from the poll */

static void wiki_open_main_page(void) {
    if (!zim.open) return;

    uint32_t idx;
    zim_dirent_t e;
    if (zim_find('W', "mainPage", &idx) &&
        zim_resolve(idx, &e) == 0 && e.ns == 'C') {
        /* resolved past the redirect, so this is the real article */
    } else if (zim.main_page < zim.article_count &&
               zim_dirent(zim.main_page, &e) == 0 && e.ns == 'C') {
        /* older layout: the header index is a path index */
    } else {
        return;                       /* no front door; the list stands */
    }

    wiki_hist_n = 0;
    wiki_load(e.url, 0);
}

/*
 * Runs from the frame loop's poll phase, not from drawing.  The archive is
 * opened lazily on the first draw of the window, and laying out the front
 * page from inside the draw would have it appear a frame late; doing it
 * from the poll keeps the two in step.
 */
static void wiki_poll(void) {
    if (!wiki_want_main) return;
    wiki_want_main = 0;
    wiki_open_main_page();
}

static void wiki_autoopen(void) {
    if (wiki_tried_open) return;
    wiki_tried_open = 1;
    if (zim.open) return;

    for (int i = 0; i < 4; i++) {
        if (!fs_stat(wiki_candidates[i], 0, 0)) continue;
        if (zim_open(wiki_candidates[i]) == 0) {
            char nb[16];
            str_copy(wiki_status, "", sizeof(wiki_status));
            uint_to_str(zim.article_count, nb);
            str_append(wiki_status, nb, sizeof(wiki_status));
            str_append(wiki_status, " entries from ", sizeof(wiki_status));
            str_append(wiki_status, wiki_candidates[i], sizeof(wiki_status));
            if (zim.truncated)
                str_append(wiki_status, "  (WARNING: incomplete download)",
                           sizeof(wiki_status));
            wiki_status_err = 0;
            wiki_search();
            wiki_want_main = 1;       /* raised next poll, not mid-draw */
            return;
        }
    }
    str_copy(wiki_status, "No archive found.  Put a .zim on the disk as "
             "/wiki.zim, or run 'zim open <file>'.", sizeof(wiki_status));
    wiki_status_err = 1;
}

/* Is this entry something a reader would want listed? */
static int wiki_listable(const zim_dirent_t *e) {
    if (e->ns != 'C') return 0;
    if (e->is_redirect) return 0;      /* an alias, not an article */
    /* namespace C also holds stylesheets, scripts and images; opening one
     * would dump raw bytes into the browser */
    const char *m = zim_mime_name(e->mime);
    for (int i = 0; m[i] && m[i + 1] && m[i + 2] && m[i + 3]; i++)
        if (m[i] == 'h' && m[i + 1] == 't' && m[i + 2] == 'm' && m[i + 3] == 'l')
            return 1;
    return 0;
}

/*
 * Walk the title-ordered listing from `start`, collecting listable
 * entries whose title still has `q` as a prefix.  Case-insensitive on the
 * comparison, but the index itself is ordered case-sensitively, which is
 * why the caller probes more than one starting point.
 */
static void wiki_collect(uint32_t start, const char *q) {
    zim_dirent_t e;
    for (uint32_t r = start;
         r < zim.title_count && wiki_hit_count < WIKI_RESULTS; r++) {
        uint32_t idx = zim_title_at(r);
        if (zim_dirent(idx, &e) != 0) break;

        int k = 0;
        while (q[k] && e.title[k] &&
               chr_lower(q[k]) == chr_lower(e.title[k])) k++;
        if (q[k] != '\0') break;       /* past the prefix run */

        if (!wiki_listable(&e)) continue;

        for (int d = 0; d < wiki_hit_count; d++)      /* no duplicates */
            if (wiki_hits[d].index == idx) goto next;

        {
            wiki_hit_t *h = &wiki_hits[wiki_hit_count++];
            h->index = idx;
            str_copy(h->title, e.title, sizeof(h->title));
            h->redirect = 0;
        }
    next: ;
    }
}

/* Lower bound over the title listing, comparing titles bytewise. */
static uint32_t wiki_title_lower_bound(const char *q) {
    uint32_t lo = 0, hi = zim.title_count;
    zim_dirent_t e;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (zim_dirent(zim_title_at(mid), &e) != 0) break;
        if (str_cmp_bytes(e.title, q) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void wiki_search(void) {
    wiki_hit_count = 0;
    wiki_sel = 0;
    if (!zim.open) return;

    /* Without the listing (older archives) fall back to path order. */
    if (zim.title_count == 0) {
        uint32_t i = zim_lower_bound('C', wiki_query);
        zim_dirent_t e;
        while (i < zim.article_count && wiki_hit_count < WIKI_RESULTS) {
            if (zim_dirent(i, &e) != 0) break;
            if (e.ns != 'C') break;
            int k = 0;
            while (wiki_query[k] && e.url[k] == wiki_query[k]) k++;
            if (wiki_query[k] != '\0') break;
            if (wiki_listable(&e)) {
                wiki_hit_t *h = &wiki_hits[wiki_hit_count++];
                h->index = i;
                str_copy(h->title, e.title, sizeof(h->title));
                h->redirect = 0;
            }
            i++;
        }
        return;
    }

    /*
     * The index is sorted bytewise, so uppercase sorts before lowercase
     * and a single probe cannot be case-insensitive: typed "new york"
     * lands nowhere near "New York".  Rather than pretend otherwise,
     * probe the spellings people actually type — as given, with the
     * first letter capitalised, and title-cased — and merge the results.
     * wiki_collect already rejects duplicates.
     */
    char v[3][WIKI_QUERY_MAX];
    int nv = 1;
    str_copy(v[0], wiki_query, WIKI_QUERY_MAX);

    str_copy(v[nv], wiki_query, WIKI_QUERY_MAX);
    v[nv][0] = chr_upper(v[nv][0]);
    if (!str_eq(v[nv], v[0])) nv++;

    str_copy(v[nv], wiki_query, WIKI_QUERY_MAX);
    for (int i = 0; v[nv][i]; i++)
        if (i == 0 || v[nv][i - 1] == ' ')
            v[nv][i] = chr_upper(v[nv][i]);
    if (!str_eq(v[nv], v[0]) && (nv < 2 || !str_eq(v[nv], v[1]))) nv++;

    for (int i = 0; i < nv && wiki_hit_count < WIKI_RESULTS; i++)
        wiki_collect(wiki_title_lower_bound(v[i]), v[i]);
}

static void wiki_open_hit(int idx) {
    if (idx < 0 || idx >= wiki_hit_count) return;
    zim_dirent_t e;
    if (zim_dirent(wiki_hits[idx].index, &e) != 0) return;
    wiki_hist_n = 0;                 /* a new search starts a new trail */
    wiki_load(e.url, 0);
}

static void wiki_key(char ch) {
    if (wiki_mode == 1) { wiki_chat_key(ch); return; }

    /* Reading an article: the keyboard drives the page, and Backspace and
     * Escape step back through it rather than out of the window. */
    if (wiki_view == 1) {
        switch (ch) {
        case 27:  case '\b': wiki_back(); return;
        case KEY_DOWN:  wiki_scroll_by(48);  return;
        case KEY_UP:    wiki_scroll_by(-48); return;
        case KEY_PGDN:  wiki_scroll_by(wiki_view_h - 40); return;
        case KEY_PGUP:  wiki_scroll_by(-(wiki_view_h - 40)); return;
        case KEY_HOME:  wiki_scroll = 0; return;
        case KEY_END:   wiki_scroll_by(wd_total_h); return;
        default: return;
        }
    }

    if (ch == 27) { wm_close(WK_WIKI); return; }
    if (ch == '\n') { wiki_open_hit(wiki_sel); return; }
    if (ch == KEY_DOWN) {
        if (wiki_sel + 1 < wiki_hit_count) wiki_sel++;
        return;
    }
    if (ch == KEY_UP) {
        if (wiki_sel > 0) wiki_sel--;
        return;
    }
    if (ch == '\b') {
        if (wiki_qlen > 0) {
            wiki_query[--wiki_qlen] = '\0';
            wiki_search();
        }
        return;
    }
    if (ch >= 0x20 && ch < 0x7F && wiki_qlen < WIKI_QUERY_MAX - 1) {
        wiki_query[wiki_qlen++] = ch;
        wiki_query[wiki_qlen] = '\0';
        wiki_search();
    }
}

static void wiki_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    int click = (lmb && !prev_lmb);

    /* The scrollbar keeps tracking outside the window while held, the way
     * a dragged scrollbar should. */
    if (wiki_view == 1 && wiki_sb_drag) {
        if (!lmb) { wiki_sb_drag = 0; return; }
        int32_t vy = cy + 74, vh = chh - 74 - 22;
        int maxs = wd_total_h - vh;
        if (maxs > 0 && vh > 0) {
            int rel = my - vy;
            if (rel < 0) rel = 0;
            if (rel > vh) rel = vh;
            wiki_scroll = rel * maxs / (vh > 0 ? vh : 1);
        }
        return;
    }

    if (mx < cx || mx >= cx + cw || my < cy || my >= cy + chh) return;

    /* the bubble in the header toggles between searching and asking */
    if (click && ai_enabled == 1 &&
        mx >= cx + cw - 42 && mx < cx + cw - 8 && my >= cy + 8 && my < cy + 36) {
        wiki_mode = !wiki_mode;
        return;
    }
    if (wiki_mode) return;

    if (wiki_view == 1) {
        int32_t vy = cy + 74, vh = chh - 74 - 22;
        int32_t x0 = cx + 14;

        /* hover, so links light up under the pointer */
        wiki_hover = wdoc_hit(x0, vy, cw - 28, vh, wiki_scroll, mx, my);

        if (!click) return;

        /* Back chevron */
        if (mx >= cx + 10 && mx < cx + 74 && my >= cy + 48 && my < cy + 70) {
            wiki_back();
            return;
        }

        int32_t sb_x = cx + cw - 12;
        if (mx >= sb_x - 2 && wd_total_h > vh) {
            wiki_sb_drag = 1;
            return;
        }

        if (wiki_hover >= 0) {
            char path[WIKI_PATH_MAX];
            wiki_resolve_link(wdoc_href_of(wiki_hover), path, sizeof(path));
            if (path[0]) wiki_load(path, 1);
        }
        return;
    }

    if (!click) return;

    int32_t list_y = cy + 74;
    int idx = (my - list_y) / WIKI_ROW_H;
    if (idx >= 0 && idx < wiki_hit_count) {
        wiki_sel = idx;
        wiki_open_hit(idx);
    }
}

/* ===== the chat side of the Wikipedia app =====
 *
 * Retrieval is lexical rather than vector-based: the archive's path list
 * is already sorted, so the question's most distinctive word finds an
 * article by prefix in a couple of dozen reads.  That article's opening
 * text becomes the context the model is asked to answer from.
 *
 * Generation is driven a step at a time from the draw loop.  A forward
 * pass takes about a minute under emulation, and calling it straight
 * through would freeze the desktop, so each frame advances it a little
 * and redraws.
 */

/* Room for more than one passage: retrieval gathers up to three
 * articles now, and one lead paragraph each is what makes the difference
 * between a question the context answers and one it merely touches. */
#define WIKI_CTX_CHARS 1100
#define WIKI_PASSAGES  3
#define WIKI_ANS_MAX   512
#define WIKI_LOG_MAX   1600

static char  wiki_input[WIKI_INPUT_MAX];
/* The model's answer before verification, for the log. */
static char  wiki_draft[WIKI_ANS_MAX];
static int   wiki_input_len;
static char  wiki_log[WIKI_LOG_MAX];
static char  wiki_context[WIKI_CTX_CHARS + 8];
static char  wiki_source[96];
static char  wiki_answer[WIKI_ANS_MAX];
static int   wiki_answer_len;

#define WIKI_MAX_PROMPT_TOKS 512
static int32_t wiki_toks[WIKI_MAX_PROMPT_TOKS];
static int   wiki_ntok, wiki_tokidx;
static int   wiki_pos, wiki_gen_n;
static int   wiki_busy;            /* 0 idle, 1 prefill, 2 generating */
static uint64_t wiki_t0 = 0;       /* when the question was asked        */
static int   wiki_im_end;
/* The first few generated token ids, kept so that an answer which comes
 * out empty can say what it actually produced. An empty string on the
 * serial line is indistinguishable from a crash. */
#define WIKI_GEN_TRACE 8
static int   wiki_gen_ids[WIKI_GEN_TRACE];

/*
 * The last few tokens this answer produced, held back from the next pick
 * so decoding cannot settle into emitting one token forever. Only
 * *generated* tokens go in -- penalising the prompt would hold back the
 * very words the retrieved article is about.
 */
#define WIKI_REPEAT_WIN 24
static int32_t wiki_recent[WIKI_REPEAT_WIN];
static int     wiki_recent_n;

static void wiki_log_add(const char *s) {
    int n = str_len(wiki_log);
    int i = 0;
    while (s[i] && n < WIKI_LOG_MAX - 2) wiki_log[n++] = s[i++];
    wiki_log[n] = '\0';
}

/* strip tags and entities out of an article, keeping the readable text */
/* Is the tag starting at src[i] named `name`? Case-insensitive, and the
 * name must end at a delimiter so <s> does not read as <style>. */
static int wiki_tag_is(const uint8_t *src, uint32_t len, uint32_t i,
                       const char *name) {
    uint32_t k = 0;
    while (name[k]) {
        if (i + 1 + k >= len) return 0;
        char a = (char)src[i + 1 + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != name[k]) return 0;
        k++;
    }
    if (i + 1 + k >= len) return 0;
    const char nxt = (char)src[i + 1 + k];
    return nxt == '>' || nxt == ' ' || nxt == '\t' || nxt == '\n' ||
           nxt == '\r' || nxt == '/';
}

/*
 * HTML to plain text, for feeding an article to the model.
 *
 * The style and script elements have to be skipped *whole*, contents and
 * all. The previous version tried to, with a flag set by any tag whose
 * name began with 's' -- span, strong, sup, section -- and cleared again
 * by the very next character, so it skipped nothing. What reached the
 * model as the article on the Moon was:
 *
 *     .mw-parser-output .hatnote{font-style:italic} ...
 *
 * -- a stylesheet. Every article whose HTML opens with a style block was
 * grounded on CSS, and the model, given no facts at all, either invented
 * some or said it did not know. This is the kind of bug that looks like a
 * bad model.
 */
static void wiki_html_text(const uint8_t *src, uint32_t len, char *out, int max) {
    int o = 0, in_tag = 0, space = 1;
    for (uint32_t i = 0; i < len && o < max - 1; i++) {
        char c = (char)src[i];

        if (c == '<') {
            /* a comment runs to "-->", not to the first '>' */
            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' &&
                src[i+3] == '-') {
                uint32_t j = i + 4;
                while (j + 2 < len &&
                       !(src[j] == '-' && src[j+1] == '-' && src[j+2] == '>'))
                    j++;
                i = (j + 2 < len) ? j + 2 : len;
                continue;
            }
            /* style and script: skip the element and everything in it */
            const char *elem = 0;
            if (wiki_tag_is(src, len, i, "style"))  elem = "style";
            else if (wiki_tag_is(src, len, i, "script")) elem = "script";
            if (elem) {
                uint32_t j = i + 1;
                while (j + 1 < len) {
                    if (src[j] == '<' && src[j+1] == '/' &&
                        wiki_tag_is(src, len, j + 1, elem)) break;
                    j++;
                }
                while (j < len && src[j] != '>') j++;
                i = j;
                continue;
            }
            in_tag = 1;
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag) continue;

        if (c == '&') {
            /* An entity becomes a space rather than nothing: dropping
             * &nbsp; outright welds the words on either side together. */
            while (i < len && src[i] != ';' && src[i] != ' ') i++;
            c = ' ';
        }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = 1;
        } else {
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) continue;
            space = 0;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static int wiki_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/*
 * Splitting a question into words is the whole of retrieval's front end,
 * and the first version of it hung.
 *
 * It skipped spaces and ended a word at a space or a question mark — so
 * on reaching a '?', the word loop stopped immediately and the space loop
 * did not advance either, because '?' is not a space. Neither branch
 * consumed the character and the outer loop ran forever.
 *
 * A question mark is how questions end, so every genuine use of the chat
 * panel hit it. The symptom was a desktop that froze the instant return
 * was pressed and never came back — which reads as an enormously slow
 * language model rather than as four lines of string handling, and was
 * diagnosed as exactly that for a while.
 *
 * Splitting on "is this a word character" instead of enumerating
 * separators makes progress structural: one of the two loops always
 * consumes the character under the cursor, whatever it is.
 */

/* Does `title` begin with `key`, ignoring case? */
static char wiki_fold(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c == '_') c = ' ';          /* a ZIM path spells a space '_' */
    return c;
}

static int wiki_title_starts(const char *title, const char *key) {
    int i = 0;
    for (; key[i]; i++) {
        char a = wiki_fold(title[i]), b = wiki_fold(key[i]);
        if (!a || a != b) return 0;
    }
    /* a prefix match must end at a word boundary, so "Neutron" does not
     * claim to be "Neutronium" */
    char nxt = title[i];
    return nxt == '\0' || nxt == ' ' || nxt == '_' || nxt == '(';
}

/*
 * Candidate scoring, before anything is decompressed.
 *
 * The old retrieval took the first key that matched anything, which is
 * how "what berries are poisonous" ended up reading the article on
 * *Poison*: "poisonous" is the longest word in the question, it matches,
 * and the search stopped there. "Berry" was never considered, and the
 * model was asked about berries while holding a page about toxicology.
 *
 * So candidates are collected and *ranked* instead, on the title alone --
 * a directory entry is about twenty reads, while decompressing a cluster
 * is far more, so ranking on titles and fetching only the winners is what
 * makes considering several affordable.
 */

/*
 * Words that ask a question rather than name its subject. Skipped when
 * choosing what to look up, so the noun wins: "what" and "moon" are both
 * four letters, and whichever a tie-break happened to pick decided
 * whether the answer was about the Moon or about nothing.
 */
static int wiki_is_stopword(const char *w) {
    static const char *const stop[] = {
        "a", "an", "the", "is", "are", "was", "were", "be", "been",
        "what", "which", "who", "whom", "whose", "where", "when", "why",
        "how", "do", "does", "did", "can", "could", "would", "should",
        "of", "in", "on", "at", "to", "for", "from", "by", "with",
        "and", "or", "but", "it", "its", "this", "that", "these", "those",
        "made", "make", "about", "tell", "me", "you", "there", "many",
        "much", "some", "any", "have", "has", "had", "not", "no", 0
    };
    for (int i = 0; stop[i]; i++) {
        int k = 0;
        while (stop[i][k] && w[k] &&
               (w[k] | 32) == stop[i][k]) k++;
        if (!stop[i][k] && !w[k]) return 1;
    }
    return 0;
}

#define WIKI_MAX_CAND 12

typedef struct {
    uint32_t idx;
    int      score;
    char     title[96];
} wiki_cand_t;

static wiki_cand_t wiki_cands[WIKI_MAX_CAND];
static int         wiki_ncand;

/* Whole-word, case-insensitive: does `hay` contain `needle` as a word? */
static int wiki_word_in(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    for (int i = 0; hay[i]; i++) {
        if (i && wiki_is_word(hay[i - 1])) continue;   /* not a boundary */
        int k = 0;
        while (needle[k] && hay[i + k] &&
               wiki_fold(hay[i + k]) == wiki_fold(needle[k])) k++;
        if (!needle[k] && !wiki_is_word(hay[i + k])) return 1;
    }
    return 0;
}

/*
 * Singular form of an English plural, well enough for article titles.
 * Wikipedia titles the article about berries *Berry*, so without this the
 * one word in the question that names the subject matches nothing.
 */
static void wiki_singular(const char *w, char *out, int max) {
    int n = 0;
    while (w[n] && n < max - 1) { out[n] = w[n]; n++; }
    out[n] = '\0';
    if (n > 3 && out[n - 3] == 'i' && out[n - 2] == 'e' && out[n - 1] == 's') {
        out[n - 3] = 'y'; out[n - 2] = '\0';          /* berries -> berry */
    } else if (n > 3 && out[n - 2] == 'e' && out[n - 1] == 's' &&
               (out[n - 3] == 'h' || out[n - 3] == 's' || out[n - 3] == 'x')) {
        out[n - 2] = '\0';                            /* bushes -> bush   */
    } else if (n > 3 && out[n - 1] == 's' && out[n - 2] != 's') {
        out[n - 1] = '\0';                            /* plants -> plant  */
    }
}

/* Does the question ask what something *is*? Set while parsing it,
 * and read by the passage scoring below. */
static int wiki_definitional;

/*
 * Does this text contain the question's word, allowing for endings?
 *
 * Whole-word matching is too strict across an inflection: the question
 * says "poisonous" and the article says "poison". Comparing a stem --
 * the word less its last few letters, but never fewer than four -- makes
 * those the same term without making unrelated short words collide.
 */
static int wiki_stem_in(const char *hay, const char *word) {
    int n = 0;
    while (word[n]) n++;
    int stem = n - 3;
    if (stem < 4) stem = 4;
    if (stem > n) stem = n;

    for (int i = 0; hay[i]; i++) {
        if (i && wiki_is_word(hay[i - 1])) continue;
        int k = 0;
        while (k < stem && hay[i + k] &&
               wiki_fold(hay[i + k]) == wiki_fold(word[k])) k++;
        if (k == stem) return 1;
    }
    return 0;
}

/*
 * Drop the title where the markup repeats it before the prose starts.
 *
 * A ZIM article carries its name in a header and again in the first
 * heading, so the stripped text opens "Volcano Volcano A volcano is a
 * mountain..." -- which reads as a stutter in a quotation and wastes
 * characters of a budgeted context on nothing.
 */
static void wiki_strip_title_runs(char *text, const char *title) {
    int tl = 0;
    while (title[tl]) tl++;
    if (tl < 2) return;

    int at = 0;
    for (;;) {
        while (text[at] == ' ') at++;
        int k = 0;
        while (k < tl && text[at + k] &&
               wiki_fold(text[at + k]) == wiki_fold(title[k])) k++;
        if (k < tl || wiki_is_word(text[at + k])) break;
        at += k;
    }
    while (text[at] == ' ') at++;
    if (at == 0 || !text[at]) return;

    int w = 0;
    while (text[at + w]) { text[w] = text[at + w]; w++; }
    text[w] = '\0';
}

/*
 * Pick the passage *inside* an article that answers the question.
 *
 * Retrieval used to hand the model each article's opening paragraph,
 * which is the right default and the wrong answer surprisingly often.
 * "What berries are poisonous" retrieves *Berry*, whose lead explains
 * what a berry is and names raspberry and blueberry -- so the model was
 * given a passage that mentions berries, does not mention poison, and
 * cannot answer the question. The sentence that does is further down.
 *
 * So the article is read further in, and the window of it with the most
 * of the question's content words is what gets sent. Ties go to the
 * earlier window, which keeps the lead as the default when nothing
 * matches better.
 */
#define WIKI_ART_CHARS 3600

static char wiki_article[WIKI_ART_CHARS + 8];

static void wiki_pick_passage(const char *text, char words[][32], int nwords,
                              int room, char *out, int outmax) {
    int len = 0;
    while (text[len]) len++;
    if (room > outmax - 1) room = outmax - 1;

    if (len <= room) {
        int i = 0;
        for (; i < len; i++) out[i] = text[i];
        out[i] = '\0';
        return;
    }

    int best = 0, best_score = -1;
    for (int st = 0; st < len; st += 48) {
        int end = st + room;
        if (end > len) { st = len - room; end = len; }

        int score = 0;
        for (int w = 0; w < nwords; w++) {
            if (wiki_is_stopword(words[w])) continue;
            /* whole-word, inside this window only */
            for (int i = st; i + 0 < end; i++) {
                if (i > st && wiki_is_word(text[i - 1])) continue;
                int k = 0;
                while (words[w][k] && i + k < end &&
                       wiki_fold(text[i + k]) == wiki_fold(words[w][k])) k++;
                if (!words[w][k] && !wiki_is_word(text[i + k])) {
                    score += 10;
                    /*
                     * A definition beats a mention.
                     *
                     * An encyclopedia article opens with a hatnote and an
                     * infobox -- "Alternative names Luna Adjectives lunar,
                     * selenic Orbit..." -- in which the subject appears
                     * many times and is never defined. The sentence that
                     * defines it says "The Moon is ...", so a window where
                     * the subject is followed shortly by a copula is worth
                     * more than one where it merely occurs.
                     */
                    for (int j = i + k; j < end && j < i + k + 3; j++) {
                        if (text[j] != ' ') continue;
                        const char *cop[] = { "is ", "are ", "was ", "were ", 0 };
                        for (int cc = 0; cop[cc]; cc++) {
                            int m = 0;
                            while (cop[cc][m] && j + 1 + m < end &&
                                   wiki_fold(text[j + 1 + m]) == cop[cc][m]) m++;
                            if (!cop[cc][m]) { score += 6; break; }
                        }
                        break;
                    }
                    break;
                }
            }
        }
        /*
         * The opening is where a subject is defined, so it starts well
         * ahead -- enough to outweigh a copula found further down.
         *
         * Simple English writes "Gravity, or gravitation, is a natural
         * phenomenon", so the lead does not match "<subject> is" at all,
         * while a paragraph about gravitons further down says "gravity
         * is caused by" and does. The article was quoted from the
         * gravitons, which define nothing. Position has to count for
         * more than sentence shape; matching the question's words still
         * counts for more than either.
         */
        /*
         * "What is X" asks for a definition, and an encyclopedia puts
         * the definition first. Other questions are about some
         * particular fact, which can be anywhere, so the preference for
         * the opening is only strong when the question asked for one.
         */
        if (st < 300)      score += wiki_definitional ? 14 : 5;
        else if (st < 900) score += wiki_definitional ? 4 : 2;

        /*
         * Prefer prose, wherever it is.
         *
         * Position alone is not enough: the Moon article opens with a
         * hatnote and an infobox -- "DesignationsAlternative
         * namesLunaAdjectiveslunar" -- which is where the subject is
         * named most often and defined never. Sentences have full stops
         * and are mostly lower case; a table of fields has neither. So
         * the window is judged on whether it reads like writing, and
         * that outweighs being first.
         */
        {
            int stops = 0, caps = 0, letters = 0;
            for (int k = st; k < end; k++) {
                if (text[k] == '.' && (k + 1 >= end || text[k + 1] == ' '))
                    stops++;
                if (text[k] >= 'A' && text[k] <= 'Z') caps++;
                if (wiki_is_word(text[k])) letters++;
            }
            if (stops >= 2) score += 8;
            if (letters > 0 && caps * 100 > letters * 18) score -= 12;
        }

        if (score > best_score) { best_score = score; best = st; }
        if (end == len) break;
    }

    /* Start on a word boundary, so the passage does not open mid-word. */
    while (best > 0 && wiki_is_word(text[best])) best--;
    while (best > 0 && text[best] == ' ') best--;
    if (best > 0) best++;

    int i = 0;
    while (i < room && text[best + i]) { out[i] = text[best + i]; i++; }
    out[i] = '\0';
}

/*
 * Look one key up and, if the archive has it, record it as a candidate.
 *
 * Scoring, in order of what actually decides a good answer:
 *   an exact title match is worth most -- the question named the article
 *   every *other* content word from the question that appears in the
 *     title is worth a lot, because it means the article is about the
 *     intersection rather than one half of it
 *   a longer key beats a shorter one, being more specific
 *   a disambiguated title ("Poison (band)") is penalised
 */
static void wiki_consider(const char *key, char words[][32], int nwords,
                          int bonus) {
    if (!key[0] || wiki_ncand >= WIKI_MAX_CAND) return;

    uint32_t idx = zim_lower_bound('C', key);
    zim_dirent_t e;
    if (idx >= zim.article_count || zim_dirent(idx, &e) != 0) return;
    if (e.ns != 'C') return;
    /*
     * lower_bound lands on the next entry alphabetically when the key is
     * absent, so the result has to be checked. Without this, asking about
     * something the archive does not cover retrieves whatever happened to
     * sort next and feeds it to the model as fact.
     */
    if (!wiki_title_starts(e.title, key)) return;

    for (int i = 0; i < wiki_ncand; i++)
        if (wiki_cands[i].idx == idx) return;         /* already have it */

    int score = bonus;
    int klen = 0; while (key[klen]) klen++;
    score += klen;

    int tlen = 0; while (e.title[tlen] && tlen < 95) tlen++;
    if (tlen == klen) score += 24;                    /* exact title */

    for (int w = 0; w < nwords; w++) {
        if (wiki_is_stopword(words[w])) continue;
        if (wiki_word_in(key, words[w])) continue;    /* already counted */
        if (wiki_word_in(e.title, words[w])) score += 18;
    }
    for (int i = 0; e.title[i]; i++)
        if (e.title[i] == '(') { score -= 12; break; }

    wiki_cand_t *c = &wiki_cands[wiki_ncand++];
    c->idx = idx;
    c->score = score;
    str_copy(c->title, e.title, sizeof(c->title));
}

/*
 * Find articles for the question and load their opening text.
 *
 * Pairs of adjacent content words are considered before single words,
 * because a phrase names an article exactly where either half of it does
 * not: "What is a neutron star?" has "neutron" as its longest single
 * word, and a single-word search retrieves an article about subatomic
 * particles.
 *
 * Up to three articles are then read, best first, and concatenated. One
 * passage answers a question only when the question happens to be about
 * exactly one article; most are not.
 *
 * Returns the number of passages placed in wiki_context.
 */
/* The question's words, kept so a survey can reuse them without
 * parsing the sentence a second time. */
static char wiki_words[8][32];
static int  wiki_nwords;


static int wiki_retrieve(const char *question) {
    wiki_context[0] = '\0';
    wiki_source[0] = '\0';
    wiki_ncand = 0;
    if (!zim.open) return 0;

    char (*words)[32] = wiki_words;
    int  nwords = 0, i = 0;
    while (question[i] && nwords < 8) {
        while (question[i] && !wiki_is_word(question[i])) i++;
        if (!question[i]) break;
        int st = i;
        while (question[i] && wiki_is_word(question[i])) i++;
        int ln = i - st;
        if (ln > 31) ln = 31;
        for (int k = 0; k < ln; k++) words[nwords][k] = question[st + k];
        words[nwords][ln] = '\0';
        nwords++;
    }
    wiki_nwords = nwords;
    wiki_definitional = 0;
    if (nwords >= 2) {
        const char *w0 = words[0], *w1 = words[1];
        const int what = (w0[0] | 32) == 'w' &&
                         (str_eq(w0, "what") || str_eq(w0, "What") ||
                          str_eq(w0, "who")  || str_eq(w0, "Who"));
        const int isare = str_eq(w1, "is") || str_eq(w1, "are") ||
                          str_eq(w1, "was") || str_eq(w1, "were");
        wiki_definitional = what && isare;
    }
    if (nwords == 0) return 0;

    char key[72], sing[32];

    /*
     * Adjacent content-word pairs. A pair of two question-words names
     * nothing: without the content test, "what is the moon made of" forms
     * "What_is", which is a real prefix -- it retrieves "What Is a
     * Woman?" and the model is asked about the moon while reading about
     * something else entirely.
     *
     * Joined with an underscore, because the sorted list this searches is
     * of *paths*, and a ZIM path spells a space as '_'.
     */
    for (int w = 0; w + 1 < nwords; w++) {
        if (wiki_is_stopword(words[w]) && wiki_is_stopword(words[w + 1]))
            continue;
        str_copy(key, words[w], sizeof(key));
        str_append(key, "_", sizeof(key));
        str_append(key, words[w + 1], sizeof(key));
        if (key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 32);
        wiki_consider(key, words, nwords, 30);     /* a phrase is worth more */
    }

    /* Then every content word, and its singular, which is what an
     * encyclopedia actually titles its articles with. */
    for (int w = 0; w < nwords; w++) {
        if (wiki_is_stopword(words[w])) continue;
        str_copy(key, words[w], sizeof(key));
        if (key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 32);
        wiki_consider(key, words, nwords, 0);

        wiki_singular(words[w], sing, sizeof(sing));
        if (!str_eq(sing, words[w])) {
            str_copy(key, sing, sizeof(key));
            if (key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 32);
            wiki_consider(key, words, nwords, 6);   /* prefer the singular */
        }
    }

    /*
     * No fallback to the question words themselves.
     *
     * There used to be one, for questions made entirely of common words.
     * But that is precisely the case where finding "something" is worse
     * than finding nothing: "what is a qwertzuiop" has no subject in the
     * archive, and the fallback matched *Ask* -- a redirect to
     * "Question" -- so the model was handed an article about questions
     * and answered "Types of question" with every appearance of
     * confidence. If no content word names an article, the honest answer
     * is that the archive does not cover it.
     */
    if (wiki_ncand == 0) return 0;

    /* ---- read the best few ---- */
    char taken_titles[WIKI_PASSAGES][96];
    int used = 0, taken = 0, best_cover = 0;

    int content_words = 0;
    for (int w = 0; w < nwords; w++)
        if (!wiki_is_stopword(words[w])) content_words++;
    for (int pass = 0; pass < WIKI_PASSAGES; pass++) {
        int best = -1;
        for (int c = 0; c < wiki_ncand; c++)
            if (wiki_cands[c].score > -1000 &&
                (best < 0 || wiki_cands[c].score > wiki_cands[best].score))
                best = c;
        if (best < 0) break;
        wiki_cands[best].score = -1000;               /* consumed */

        const uint8_t *d;
        uint32_t n;
        zim_dirent_t got;
        if (zim_content(wiki_cands[best].idx, &d, &n, &got) != 0) continue;

        /*
         * Duplicates can only be spotted here, after resolution. The
         * candidates were deduplicated by directory index, but "Berries"
         * and "Berry" are two different entries that redirect to one
         * article -- so the plural and the singular both scored, both
         * won, and the same passage went into the context twice, taking
         * a slot from something that might have answered the question.
         */
        int dup = 0;
        for (int t = 0; t < taken; t++)
            if (str_eq(taken_titles[t], got.title)) { dup = 1; break; }
        if (dup) { pass--; continue; }

        /*
         * Share the budget between the passages that actually exist,
         * not the three there might have been.
         *
         * Dividing by WIKI_PASSAGES regardless gave a lone passage a
         * third of the context and threw the rest away -- so the model
         * was answering from 366 characters when 1100 were available,
         * and the verifier then rejected perfectly good answers for
         * using wording that had been trimmed off.
         */
        int left = 0;
        for (int c = 0; c < wiki_ncand; c++)
            if (wiki_cands[c].score > -1000) left++;
        left++;                              /* this one, already claimed */
        if (left > WIKI_PASSAGES - pass) left = WIKI_PASSAGES - pass;
        if (left < 1) left = 1;
        int room = (WIKI_CTX_CHARS - used) / left;

        /*
         * Capped, and the cap is the point.
         *
         * Handing a lone passage the whole budget was tried and made
         * things worse: 1100 characters of the Moon article is mostly
         * hatnote and infobox, the prompt hit the 512-token ceiling, and
         * the model answered from the furniture. A passage chosen for
         * matching the question beats a longer one that happens to
         * contain it.
         */
        if (room > 500) room = 500;
        if (room < 80) break;

        /* Read well past the lead, then choose the part that matches. */
        wiki_html_text(d, n < 60000 ? n : 60000, wiki_article, WIKI_ART_CHARS);
        wiki_strip_title_runs(wiki_article, got.title);
        char one[WIKI_CTX_CHARS + 8];
        wiki_pick_passage(wiki_article, words, nwords, room - 4,
                          one, (int)sizeof(one));

        if (used) { str_append(wiki_context, "\n", sizeof(wiki_context)); }
        str_append(wiki_context, "[", sizeof(wiki_context));
        str_append(wiki_context, got.title, sizeof(wiki_context));
        str_append(wiki_context, "] ", sizeof(wiki_context));
        str_append(wiki_context, one, sizeof(wiki_context));

        used = 0;
        while (wiki_context[used]) used++;

        if (taken == 0) str_copy(wiki_source, got.title, sizeof(wiki_source));
        else {
            str_append(wiki_source, " + ", sizeof(wiki_source));
            str_append(wiki_source, got.title, sizeof(wiki_source));
        }
        str_copy(taken_titles[taken], got.title, sizeof(taken_titles[0]));

        /*
         * How much of the question this one passage covers, on its own.
         *
         * Per passage, not across all of them, and that distinction is
         * the whole point. "What berries are poisonous" retrieves *Berry*
         * and *Poison*: between them every word of the question is
         * present, and the model duly welded the two together and
         * reported that raspberries and strawberries are poisonous. No
         * single passage said anything of the kind. A fact the archive
         * does not state is not made true by being spread across two
         * articles.
         */
        int cover = 0;
        for (int w = 0; w < nwords; w++) {
            if (wiki_is_stopword(words[w])) continue;
            char sg[32];
            wiki_singular(words[w], sg, sizeof(sg));
            if (wiki_stem_in(one, words[w]) || wiki_stem_in(one, sg)) cover++;
        }
        if (cover > best_cover) best_cover = cover;

        taken++;
    }

    /*
     * Every content word has to appear in one passage, or this is not an
     * answer the archive contains. Refusing here is what stops a small
     * model from bridging two articles into a confident falsehood, and a
     * refusal is worth more than a fluent sentence that is wrong.
     */
    if (taken && content_words > 1 && best_cover < content_words) {
        serial_puts("[wiki] no single passage covers the question (");
        serial_put_dec((uint32_t)best_cover);
        serial_puts(" of ");
        serial_put_dec((uint32_t)content_words);
        serial_puts(" terms)\n");
        wiki_context[0] = '\0';
        return 0;
    }
    return taken;
}

/* =====================================================================
 * Surveying a category
 *
 * Some questions are not about one article. "What berries are poisonous"
 * is a question about the *set* of berries, and no single page answers
 * it: the archive knows the answer, but it is spread across Deadly
 * nightshade, Pokeweed and their neighbours, one fact per article.
 *
 * Retrieval that returns passages cannot answer that, and a model handed
 * two unrelated passages will bridge them into a falsehood -- which is
 * exactly how raspberries were once reported as poisonous. So a question
 * of this shape is answered by *surveying*: take the article that names
 * the category, follow its links, read each one, and keep the entries
 * that actually say the thing being asked about.
 *
 * The whole archive cannot be scanned -- 281,219 entries, two reads
 * each, and the titles are sorted so that "Blueberry" does not sit
 * anywhere near "Berry". The category article's own links are the
 * curated list of its members, which is both far smaller and better
 * chosen than anything a substring search would produce.
 *
 * Paced across frames like every other long job here, and reported, so
 * a survey in progress never looks like a hang.
 * ===================================================================== */

#define WIKI_SCAN_MAX   96      /* candidate links to consider   */
#define WIKI_SCAN_HITS  10      /* entries quoted in the answer  */

static char     wiki_scan_path[WIKI_SCAN_MAX][WIKI_PATH_MAX];
static int      wiki_scan_n, wiki_scan_at, wiki_scan_hits;
static char     wiki_scan_subj[32], wiki_scan_qual[32];
static char     wiki_scan_names[WIKI_SCAN_HITS][64];
static char     wiki_scan_quote[WIKI_SCAN_HITS][200];

/* Percent-decode in place, enough for the paths a ZIM link carries. */
static void wiki_unescape(char *s) {
    int r = 0, w = 0;
    while (s[r]) {
        if (s[r] == '%' && s[r + 1] && s[r + 2]) {
            int hi = -1, lo = -1;
            const char a = s[r + 1], b = s[r + 2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if ((a | 32) >= 'a' && (a | 32) <= 'f') hi = (a | 32) - 'a' + 10;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if ((b | 32) >= 'a' && (b | 32) <= 'f') lo = (b | 32) - 'a' + 10;
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

/*
 * Every internal link in an article, as archive paths.
 *
 * Skips anything with a scheme (an outside link), anchors, and the
 * namespaces that are not articles. What remains is the last path
 * component, which is what the 'C' namespace is keyed by.
 */
static void wiki_collect_links(const uint8_t *html, uint32_t len) {
    wiki_scan_n = 0;
    for (uint32_t i = 0; i + 6 < len && wiki_scan_n < WIKI_SCAN_MAX; i++) {
        if (!(html[i] == 'h' && html[i+1] == 'r' && html[i+2] == 'e' &&
              html[i+3] == 'f' && html[i+4] == '='))
            continue;
        uint32_t j = i + 5;
        const char quote = (char)html[j];
        if (quote != '"' && quote != '\'') continue;
        j++;

        char path[WIKI_PATH_MAX];
        int n = 0;
        while (j < len && (char)html[j] != quote && n < WIKI_PATH_MAX - 1) {
            const char c = (char)html[j];
            if (c == '#' || c == '?') break;      /* fragment, query */
            path[n++] = c;
            j++;
        }
        path[n] = '\0';
        i = j;
        if (n == 0) continue;

        /* an outside link: scheme, or protocol-relative */
        int has_scheme = 0;
        for (int k = 0; path[k] && path[k] != '/'; k++)
            if (path[k] == ':') { has_scheme = 1; break; }
        if (has_scheme || (path[0] == '/' && path[1] == '/')) continue;

        /* keep the last component: "../A/Blueberry" -> "Blueberry" */
        int last = 0;
        for (int k = 0; path[k]; k++) if (path[k] == '/') last = k + 1;
        const char *tail = path + last;
        if (!tail[0] || tail[0] == '.') continue;

        char clean[WIKI_PATH_MAX];
        str_copy(clean, tail, sizeof(clean));
        wiki_unescape(clean);
        if (!clean[0]) continue;

        int dup = 0;
        for (int k = 0; k < wiki_scan_n; k++)
            if (str_eq(wiki_scan_path[k], clean)) { dup = 1; break; }
        if (dup) continue;

        str_copy(wiki_scan_path[wiki_scan_n++], clean, WIKI_PATH_MAX);
    }
}

/*
 * The sentence around the first mention of `word`, for quoting.
 * A fact is worth more with the sentence that states it attached, and
 * far more than a bare title.
 */
static void wiki_quote_around(const char *text, const char *word,
                              char *out, int max) {
    out[0] = '\0';
    int n = 0;
    while (text[n]) n++;

    int at = -1, wl = 0;
    while (word[wl]) wl++;
    int stem = wl - 3;
    if (stem < 4) stem = 4;
    if (stem > wl) stem = wl;

    for (int i = 0; i < n && at < 0; i++) {
        if (i && wiki_is_word(text[i - 1])) continue;
        int k = 0;
        while (k < stem && text[i + k] &&
               wiki_fold(text[i + k]) == wiki_fold(word[k])) k++;
        if (k == stem) at = i;
    }
    if (at < 0) return;

    int st = at;
    while (st > 0 && !(text[st - 1] == '.' && text[st] == ' ')) st--;
    while (text[st] == ' ') st++;
    int en = at;
    while (text[en] && !(text[en] == '.' &&
                         (text[en + 1] == ' ' || !text[en + 1]))) en++;
    if (text[en] == '.') en++;

    /*
     * Keep the match inside the quote.
     *
     * A navigation strip has no full stops in it, so the search for a
     * sentence boundary runs to the top of the article and the quote is
     * then truncated long before reaching the word it was supposed to be
     * showing -- the Stone fruit entry quoted a list of links and never
     * displayed "poisonous" at all. Centring on the match means the
     * evidence is always visible, whatever the surrounding text is.
     */
    if (at - st > (max - 1) / 2) {
        st = at - (max - 1) / 2;
        while (st > 0 && wiki_is_word(text[st])) st--;
        while (text[st] == ' ') st++;
    }
    if (en - st > max - 1) {
        en = st + max - 1;
        /* end on a word boundary rather than mid-word */
        while (en > at && wiki_is_word(text[en - 1]) && wiki_is_word(text[en]))
            en--;
    }

    int w = 0;
    for (int i = st; i < en && w < max - 1; i++) out[w++] = text[i];
    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

/*
 * Is this a sentence, or a strip of links flattened into text?
 *
 * An article's navigation furniture -- "see also" rows, disambiguation
 * lists, category strips -- survives the HTML stripper as a long run of
 * Title Case words with no verbs and no full stops. One such run
 * contained the word "poison" in "poison dates in Raiders of the Lost
 * Ark", which matched perfectly and told the reader nothing.
 *
 * Prose is mostly lower case. A run where nearly every word is
 * capitalised is a list, whatever it happens to contain.
 */
static int wiki_looks_like_prose(const char *t) {
    int words = 0, capped = 0, i = 0;
    while (t[i]) {
        while (t[i] && !wiki_is_word(t[i])) i++;
        if (!t[i]) break;
        if (t[i] >= 'A' && t[i] <= 'Z') capped++;
        words++;
        while (t[i] && wiki_is_word(t[i])) i++;
    }
    if (words < 6) return 0;
    if (capped * 10 >= words * 4) return 0;   /* 40%+ capitalised: a list */

    /*
     * And it has to contain a sentence boundary. Capitalisation alone
     * let one strip through -- a run naming Practical Magic, Harry
     * Nilsson and Raiders of the Lost Ark came to 36% and passed, while
     * being no kind of sentence. A full stop is what separates a
     * quotation from a row of link captions.
     */
    for (int k = 0; t[k]; k++)
        if (t[k] == '.') return 1;
    return 0;
}

/* One candidate per call, so the caller can spend a frame's slice. */
static int wiki_scan_one(void) {
    if (wiki_scan_at >= wiki_scan_n) return 1;
    const char *path = wiki_scan_path[wiki_scan_at++];

    uint32_t idx;
    if (!zim_find('C', path, &idx)) return 0;

    const uint8_t *d;
    uint32_t n;
    zim_dirent_t got;
    if (zim_content(idx, &d, &n, &got) != 0) return 0;

    wiki_html_text(d, n < 60000 ? n : 60000, wiki_article, WIKI_ART_CHARS);

    /* It has to be a member of the category *and* say the thing asked
     * about; either alone is how an unrelated article gets quoted. */
    if (!wiki_stem_in(wiki_article, wiki_scan_subj) &&
        !wiki_stem_in(got.title, wiki_scan_subj)) return 0;
    if (!wiki_stem_in(wiki_article, wiki_scan_qual)) return 0;

    for (int k = 0; k < wiki_scan_hits; k++)
        if (str_eq(wiki_scan_names[k], got.title)) return 0;

    char quote[200];
    wiki_quote_around(wiki_article, wiki_scan_qual, quote, sizeof(quote));
    if (!quote[0]) return 0;
    if (!wiki_looks_like_prose(quote)) return 0;

    int used = 0;
    while (wiki_context[used]) used++;
    if (used + (int)str_len(quote) + 80 > WIKI_CTX_CHARS) {
        wiki_scan_at = wiki_scan_n;            /* full; stop early */
        return 1;
    }

    if (used) str_append(wiki_context, "\n", sizeof(wiki_context));
    str_append(wiki_context, "[", sizeof(wiki_context));
    str_append(wiki_context, got.title, sizeof(wiki_context));
    str_append(wiki_context, "] ", sizeof(wiki_context));
    str_append(wiki_context, quote, sizeof(wiki_context));

    if (wiki_scan_hits < WIKI_SCAN_HITS) {
        str_copy(wiki_scan_names[wiki_scan_hits], got.title,
                 sizeof(wiki_scan_names[0]));
        str_copy(wiki_scan_quote[wiki_scan_hits], quote,
                 sizeof(wiki_scan_quote[0]));
        wiki_scan_hits++;
    }
    if (wiki_scan_hits >= WIKI_SCAN_HITS) return 1;
    return 0;
}

/*
 * Set a survey going, if the question looks like one.
 *
 * It has to name a category the archive has an article for, and a
 * property to test its members against. Returns 1 if a survey started.
 */
static int wiki_scan_begin(char words[][32], int nwords) {
    wiki_scan_n = wiki_scan_at = wiki_scan_hits = 0;
    wiki_scan_subj[0] = wiki_scan_qual[0] = '\0';

    /* The subject is the content word with an article of its own; the
     * property is the other one. */
    char subj[32] = "", qual[32] = "";
    uint32_t subj_idx = 0;
    int have_subj = 0;

    for (int w = 0; w < nwords; w++) {
        if (wiki_is_stopword(words[w])) continue;
        char sg[32], key[72];
        wiki_singular(words[w], sg, sizeof(sg));
        str_copy(key, sg, sizeof(key));
        if (key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 32);

        uint32_t idx = zim_lower_bound('C', key);
        zim_dirent_t e;
        if (!have_subj && idx < zim.article_count &&
            zim_dirent(idx, &e) == 0 && e.ns == 'C' &&
            wiki_title_starts(e.title, key)) {
            str_copy(subj, sg, sizeof(subj));
            subj_idx = idx;
            have_subj = 1;
        } else if (!qual[0]) {
            str_copy(qual, words[w], sizeof(qual));
        }
    }
    if (!have_subj || !qual[0]) return 0;

    const uint8_t *d;
    uint32_t n;
    zim_dirent_t got;
    if (zim_content(subj_idx, &d, &n, &got) != 0) return 0;

    wiki_collect_links(d, n);
    if (wiki_scan_n == 0) return 0;

    str_copy(wiki_scan_subj, subj, sizeof(wiki_scan_subj));
    str_copy(wiki_scan_qual, qual, sizeof(wiki_scan_qual));
    str_copy(wiki_source, got.title, sizeof(wiki_source));
    wiki_context[0] = '\0';

    serial_puts("[wiki] surveying ");
    serial_put_dec((uint32_t)wiki_scan_n);
    serial_puts(" entries linked from ");
    serial_puts(got.title);
    serial_puts(" for \"");
    serial_puts(qual);
    serial_puts("\"\n");
    return 1;
}

/* =====================================================================
 * Verifying what the model wrote
 *
 * The model is the least trustworthy part of this pipeline and the only
 * part that can invent. Everything upstream -- the archive, the
 * retrieval, the passage selection -- deals in text that exists; the
 * model is where text that does not exist can appear. So its draft is
 * checked against the evidence before anyone sees it, sentence by
 * sentence, and what cannot be traced is dropped.
 *
 * This is a *deterministic* check, not a second opinion from a second
 * model. A small model asked to judge a small model produces another
 * guess, and two guesses do not make a verification. "Every content
 * word in this sentence appears in the source" is a claim that can be
 * decided, shown, and disagreed with.
 *
 * It catches the three failures seen in practice:
 *
 *   invention   a claim built from words the source never used
 *   collage     "Blackberries Rubus BlackBerry", which is not a sentence
 *   echo        "Poison", the title handed back as an answer
 *
 * What it cannot catch is a false *relation* between words that are all
 * present -- "raspberries are poisonous" drawn from a page listing
 * raspberries and a page about poison. That one is prevented upstream
 * instead, by requiring a single passage to carry the whole question.
 * ===================================================================== */

/* A word worth checking: long enough to carry meaning, not a stopword. */
static int wiki_checkable(const char *w) {
    int n = 0;
    while (w[n]) n++;
    if (n < 4) return 0;                 /* function words and numbers */
    return !wiki_is_stopword(w);
}

/*
 * Keep the sentences the evidence supports; drop the rest.
 *
 * Returns the number of sentences kept, and reports how many were
 * dropped so the answer can say so out loud.
 */
static int wiki_verify_answer(char *ans, int *dropped) {
    char kept[WIKI_ANS_MAX];
    kept[0] = '\0';
    *dropped = 0;

    int i = 0, nkept = 0;
    while (ans[i]) {
        while (ans[i] == ' ' || ans[i] == '\n') i++;
        if (!ans[i]) break;

        const int st = i;
        while (ans[i] && !(ans[i] == '.' &&
                           (ans[i + 1] == ' ' || ans[i + 1] == '\0')))
            i++;
        if (ans[i] == '.') i++;
        const int en = i;
        if (en <= st) break;

        /* every checkable word in this sentence, against the evidence */
        int bad = 0, words = 0;
        int j = st;
        while (j < en) {
            while (j < en && !wiki_is_word(ans[j])) j++;
            if (j >= en) break;
            char w[32];
            int n = 0;
            while (j < en && wiki_is_word(ans[j]) && n < 31) w[n++] = ans[j++];
            w[n] = '\0';
            if (!wiki_checkable(w)) continue;
            words++;
            char sg[32];
            wiki_singular(w, sg, sizeof(sg));
            if (!wiki_stem_in(wiki_context, w) &&
                !wiki_stem_in(wiki_context, sg)) bad++;
        }

        /*
         * One stray word is tolerated; a quarter of them is not.
         *
         * Strict zero rejected a faithful answer -- "a magma chamber
         * under the Earth's surface" -- because the HTML stripper had
         * lost "ground," from that very sentence, so "surface" appeared
         * unsupported when the entry does support it. Meanwhile the
         * answer this check exists to stop, "the force that keeps
         * objects in orbit around a planet", has five unsupported words
         * out of eight -- well past a third -- and is refused either
         * way. A third is the line: paraphrase adds a word or two,
         * invention adds most of them.
         */
        if (words == 0 || bad > 3 || bad * 3 > words) { (*dropped)++; continue; }

        if (kept[0]) str_append(kept, " ", sizeof(kept));
        for (int k = st; k < en && str_len(kept) < WIKI_ANS_MAX - 2; k++) {
            const int l = str_len(kept);
            kept[l] = ans[k];
            kept[l + 1] = '\0';
        }
        nkept++;
    }

    if (nkept == 0) { ans[0] = '\0'; return 0; }

    /* A surviving fragment still has to read as an answer: four words,
     * and not a run of capitalised link text. */
    int words = 0, capped = 0, k = 0;
    while (kept[k]) {
        while (kept[k] && !wiki_is_word(kept[k])) k++;
        if (!kept[k]) break;
        if (kept[k] >= 'A' && kept[k] <= 'Z') capped++;
        words++;
        while (kept[k] && wiki_is_word(kept[k])) k++;
    }
    if (words < 4 || capped * 10 >= words * 6) { ans[0] = '\0'; return 0; }

    str_copy(ans, kept, WIKI_ANS_MAX);
    return nkept;
}

/*
 * The sentence in the evidence that best answers the question, for when
 * the model's draft does not survive. Falling back to the source is
 * always available, because the source is what the answer was supposed
 * to be made of.
 */
static void wiki_evidence_line(char *out, int max) {
    out[0] = '\0';
    int best_score = -1, best_st = 0, best_en = 0;
    int i = 0;
    while (wiki_context[i]) {
        while (wiki_context[i] == ' ' || wiki_context[i] == '\n') i++;
        if (!wiki_context[i]) break;
        const int st = i;
        while (wiki_context[i] &&
               !(wiki_context[i] == '.' && (wiki_context[i + 1] == ' ' ||
                                            wiki_context[i + 1] == '\0')) &&
               wiki_context[i] != '\n')
            i++;
        if (wiki_context[i] == '.') i++;
        const int en = i;

        char sentence[400];
        int n = 0;
        for (int k = st; k < en && n < (int)sizeof(sentence) - 1; k++)
            sentence[n++] = wiki_context[k];
        sentence[n] = '\0';

        int score = 0;
        for (int w = 0; w < wiki_nwords; w++) {
            if (wiki_is_stopword(wiki_words[w])) continue;
            char sg[32];
            wiki_singular(wiki_words[w], sg, sizeof(sg));
            if (wiki_stem_in(sentence, wiki_words[w]) ||
                wiki_stem_in(sentence, sg)) score++;
        }
        if (score > best_score) { best_score = score; best_st = st; best_en = en; }
    }
    if (best_score <= 0) return;

    /* Skip the "[Title] " label the passage was stored under: the
     * caller already names the source, and repeating it inside the
     * quotation makes it look like part of the sentence. */
    if (wiki_context[best_st] == '[') {
        int k = best_st;
        while (wiki_context[k] && wiki_context[k] != ']') k++;
        if (wiki_context[k] == ']') {
            k++;
            while (wiki_context[k] == ' ') k++;
            if (k < best_en) best_st = k;
        }
    }

    int n = 0;
    for (int k = best_st; k < best_en && n < max - 1; k++)
        out[n++] = wiki_context[k];
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static void wiki_begin_generation(void);

static void wiki_submit(void) {
    if (wiki_busy) return;
    if (wiki_input_len == 0) return;
    if (!llm_weights_loaded()) {
        /* The transcript already says why, but the transcript is a
         * window somebody has to go and look at. Say it on the serial
         * line too, so a refused question is diagnosable from a log. */
        serial_puts("[wiki] refused: weights not resident (ai_state ");
        serial_put_dec((uint32_t)ai_state);
        serial_puts(")\n");
        if (ai_busy()) {
            char nb[8];
            uint_to_str((uint32_t)ai_progress(), nb);
            wiki_log_add("\nStill loading the model - ");
            wiki_log_add(nb);
            wiki_log_add("%.  Ask again in a moment.\n");
        } else if (ai_state == AI_FAILED) {
            wiki_log_add("\nThe model could not be loaded: ");
            wiki_log_add(ai_err);
            wiki_log_add("\n");
        } else {
            wiki_log_add("\nNo model on this volume.  Copy a Qwen2 GGUF to "
                         AI_MODEL_PATH " and reboot.\n");
        }
        return;
    }

    /*
     * Make sure the archive is open before retrieving from it.
     *
     * This used to happen only in wiki_draw(), so the encyclopedia was
     * opened as a side effect of the window being painted. A question
     * asked before that — from the shell, or from a harness that opens
     * the window and asks in the same frame — retrieved nothing, and the
     * model answered from its own weights with no grounding at all. It
     * still produced a fluent paragraph, which is the worst possible
     * failure: a confident wrong answer that looks exactly like a
     * correct one.
     */
    wiki_autoopen();

    /* On the serial line as well as in the transcript. A question that
     * takes a while to answer is indistinguishable from one that was
     * never asked, unless something says it arrived. */
    serial_puts("[wiki] ask: ");
    serial_puts(wiki_input);
    serial_putc('\n');
    wiki_t0 = cycle_now();

    wiki_log_add("\nYou: ");
    wiki_log_add(wiki_input);
    wiki_log_add("\n");

    /*
     * Retrieve first, and refuse if nothing was found.
     *
     * This is a retrieval-augmented answer or it is nothing. A 0.5B model
     * asked a question with no context will still produce a confident,
     * fluent paragraph out of its own weights -- which is the worst
     * possible failure, because it is indistinguishable from a correct
     * answer until you check it. Saying "not in the archive" is worth
     * more than a plausible sentence.
     */
    const int passages = wiki_retrieve(wiki_input);
    if (passages == 0 || !wiki_context[0]) {
        /*
         * No one article answers it -- so try surveying a category
         * instead. This is the path that answers "what berries are
         * poisonous": there is no page by that name, but there is a page
         * that lists berries, and the answer is in which of them say so.
         */
        if (wiki_scan_begin(wiki_words, wiki_nwords)) {
            wiki_busy = 4;
            wiki_log_add("[surveying entries linked from ");
            wiki_log_add(wiki_source);
            wiki_log_add("]\n");
            return;
        }
        serial_puts("[wiki] no article matched; refusing to answer\n");
        wiki_log_add("AI: The archive does not have an article that answers "
                     "that, so I have nothing to answer from. I only answer "
                     "from Wikipedia, never from memory.\n");
        return;
    }
    wiki_begin_generation();
}

/*
 * Build the prompt from whatever is in wiki_context and start the model.
 *
 * Split out of wiki_submit because there are two ways to arrive here now
 * -- a passage retrieved directly, or a survey that assembled one out of
 * many entries -- and both need exactly the same prompt and prefill.
 */
static void wiki_begin_generation(void) {
    wiki_log_add("[context: ");
    wiki_log_add(wiki_source);
    wiki_log_add("]\n");

    /*
     * Qwen2's chat format, with the retrieved passage as the grounding.
     *
     * The context is trimmed to what will still leave room for the
     * question and the assistant turn, rather than appended and allowed to
     * fill the buffer. str_append stops at the bound, so a long article
     * used to silently push the question — the entire point of the
     * exercise — off the end, and the model was asked to continue an
     * article rather than answer anything.
     */
    /* Sized for the system instruction, three passages and the
     * question. The token budget below is the real limit: 512 prompt
     * tokens is about two thousand characters, so this buffer is a
     * little larger than that and the trim keeps it honest. */
    static char prompt[2048];
    /*
     * The instruction does the other half of the grounding.
     *
     * "Answer the question using the context" leaves answering from
     * memory open, and a model that half-remembers something will take
     * that opening every time -- which is how "what berries are
     * poisonous" came back as a sentence about poison that named no
     * berry. Naming the failure case explicitly, and giving it exact
     * words to say, is what makes a small model decline instead of
     * inventing.
     */
    static const char sys_part[] =
        "<|im_start|>system\nYou answer only from the Context below."
        " Do not use any other knowledge. Quote the relevant fact from"
        " the Context. If the Context does not contain the answer, reply"
        " exactly: Not in the archive."
        " Be brief.<|im_end|>\n<|im_start|>user\n";
    static const char tail_part[] = "<|im_end|>\n<|im_start|>assistant\n";

    prompt[0] = '\0';
    str_append(prompt, sys_part, sizeof(prompt));

    if (wiki_context[0]) {
        int used = (int)sizeof(sys_part) - 1 + 9 /* "Context: " */ + 1
                 + 10 /* "Question: " */ + wiki_input_len
                 + (int)sizeof(tail_part) - 1;
        int room = (int)sizeof(prompt) - 1 - used;
        if (room > 64) {
            char save = 0;
            int clen = 0;
            while (wiki_context[clen] && clen < room) clen++;
            save = wiki_context[clen];
            wiki_context[clen] = '\0';
            str_append(prompt, "Context: ", sizeof(prompt));
            str_append(prompt, wiki_context, sizeof(prompt));
            str_append(prompt, "\n", sizeof(prompt));
            wiki_context[clen] = save;
        }
    }
    str_append(prompt, "Question: ", sizeof(prompt));
    str_append(prompt, wiki_input, sizeof(prompt));
    str_append(prompt, tail_part, sizeof(prompt));

    wiki_ntok = llm_encode(prompt, wiki_toks, WIKI_MAX_PROMPT_TOKS);
    if (wiki_ntok <= 0) {
        /* Said out loud, on both channels. Every failure below this point
         * used to return in silence, which is why an unanswered question
         * looked identical to a hung machine. */
        serial_puts("[wiki] tokenizer refused the prompt\n");
        wiki_log_add("(could not tokenize)\n");
        return;
    }
    serial_puts("[wiki] prompt ");
    serial_put_dec((uint32_t)wiki_ntok);
    serial_puts(" tokens, context from ");
    serial_puts(wiki_source[0] ? wiki_source : "(nothing found)");
    serial_putc('\n');
    /*
     * The opening of what the model was actually given. Judging a
     * grounded answer means seeing the grounding: without this, a wrong
     * answer could be the retrieval's fault or the model's and there is
     * no way to tell them apart from a log.
     */
    serial_puts("[wiki] context: ");
    for (int i = 0; i < 220 && wiki_context[i]; i++)
        serial_putc(wiki_context[i] == '\n' ? ' ' : wiki_context[i]);
    serial_putc('\n');

    wiki_im_end = llm_token_id("<|im_end|>");
    wiki_tokidx = 0;
    wiki_pos = 0;
    wiki_gen_n = 0;
    wiki_answer[0] = '\0';
    wiki_answer_len = 0;
    wiki_recent_n = 0;
    wiki_busy = 1;
    if (wiki_ntok > 1) {
        /* everything but the last token, in batches */
        if (llm_prefill_begin(wiki_toks, wiki_ntok - 1, 0) != 0) {
            serial_puts("[wiki] could not start the prefill\n");
            wiki_busy = 0;
            return;
        }
    } else {
        wiki_busy = 3;
        wiki_feed(0);
    }

    wiki_input[0] = '\0';
    wiki_input_len = 0;
}

/*
 * Feed prompt token `k`, asking for logits only where they can be read.
 *
 * Only the last prompt token's logits choose anything — everything before
 * it exists to fill the key/value cache. Computing the head for all of
 * them was about three fifths of the cost of reading an article, spent
 * entirely on numbers nothing looks at.
 */
static void wiki_feed(int k) {
    if (k + 1 < wiki_ntok) llm_eval_begin_prefill(wiki_toks[k], wiki_pos);
    else                   llm_eval_begin(wiki_toks[k], wiki_pos);
}

/*
 * Submit a question from outside the chat panel — the shell's `ask`.
 *
 * Every other subsystem here is driveable both ways: `zim`, `store`,
 * `img` and `fetch` all have shell equivalents of what their windows do.
 * The chat was reachable only by clicking a bubble, which also made it
 * the one feature that could not be scripted — and therefore the one
 * whose bugs survived longest, because reproducing them needed a hand on
 * a mouse. Returns 1 if generation started.
 */
static int wiki_ask(const char *question) {
    if (!question || !question[0]) return 0;
    str_copy(wiki_input, question, sizeof(wiki_input));
    wiki_input_len = 0;
    while (wiki_input[wiki_input_len]) wiki_input_len++;
    wiki_mode = 1;                     /* show the chat, not the search */
    /* The shell says "the answer appears in Wikipedia", so put it where
     * that sentence promises. */
    wm_open(WK_WIKI);
    wiki_submit();
    return wiki_busy != 0;
}

/* advance generation; called once per frame */
static void wiki_gen_poll(void) {
    if (!wiki_busy) return;

    /*
     * A survey in progress: read one linked entry per turn, within the
     * frame's slice, until the list is exhausted or enough of it has
     * answered. Only then is the model given anything.
     */
    if (wiki_busy == 4) {
        uint64_t scan_start = cycle_now();
        do {
            if (wiki_scan_one()) {
                serial_puts("[wiki] survey read ");
                serial_put_dec((uint32_t)wiki_scan_at);
                serial_puts(" entries, ");
                serial_put_dec((uint32_t)wiki_scan_hits);
                serial_puts(" matched\n");
                if (wiki_scan_hits > 0) {
                    /*
                     * The survey's result is quoted archive text, and it
                     * is reported as such rather than paraphrased.
                     *
                     * Handing these findings to the model destroyed them:
                     * given three correct quotations it replied
                     * "Blackberries Rubus BlackBerry". A 0.5B model adds
                     * nothing to a list of facts already in the right
                     * order, and can only subtract from it. So the answer
                     * is the evidence, with the article each line came
                     * from, and the reader can weigh a sentence like
                     * Tomato's -- "wrongly thought that they were
                     * poisonous" -- for themselves, which a summary would
                     * have flattened.
                     */
                    char nb[12];
                    wiki_busy = 0;
                    wiki_log_add("AI: I read ");
                    uint_to_str((uint32_t)wiki_scan_at, nb);
                    wiki_log_add(nb);
                    wiki_log_add(" entries linked from ");
                    wiki_log_add(wiki_source);
                    wiki_log_add(". ");
                    uint_to_str((uint32_t)wiki_scan_hits, nb);
                    wiki_log_add(nb);
                    wiki_log_add(wiki_scan_hits == 1 ? " mentions \""
                                                     : " mention \"");
                    wiki_log_add(wiki_scan_qual);
                    wiki_log_add("\":\n");
                    for (int k = 0; k < wiki_scan_hits; k++) {
                        wiki_log_add("  ");
                        wiki_log_add(wiki_scan_names[k]);
                        wiki_log_add(" - ");
                        wiki_log_add(wiki_scan_quote[k]);
                        wiki_log_add("\n");
                    }
                    wiki_log_add("Why: these are the entries' own words. "
                                 "Nothing here was written by the model.\n");
                } else {
                    wiki_busy = 0;
                    wiki_log_add("AI: I read ");
                    uint_to_str((uint32_t)wiki_scan_at, (char *)wiki_answer);
                    wiki_log_add(wiki_answer);
                    wiki_log_add(" entries from the archive and none of them "
                                 "says that, so I have nothing to answer "
                                 "from.\n");
                    wiki_answer[0] = '\0';
                }
                return;
            }
        } while (!budget_expired_ms(scan_start, 8));
        return;
    }

    /*
     * As much of the frame as can be spared, rather than two layers.
     *
     * A fixed two steps per frame was catastrophic in the other direction
     * from the model loader. One step is one transformer layer or one
     * slice of the logit head — about sixty steps per token — and the
     * prompt is a retrieved article, two or three hundred tokens of it.
     * At two steps per frame that is fifteen thousand frames, so a prompt
     * evaluation the hardware can do in three seconds took over two
     * minutes, with the desktop idling at 60 fps the whole time because it
     * had been told not to work any harder.
     *
     * Eight milliseconds is a larger slice than the loader gets, because
     * this only happens while somebody is deliberately waiting for an
     * answer and a slightly heavier frame is the right trade then.
     */
    uint64_t start = cycle_now();

    /*
     * Phase 1 is the prompt, read in batches.
     *
     * All of it but the final token goes through llm_prefill_step, which
     * dequantises each weight once per batch of eight instead of once per
     * token. Reading an article is where a question's time goes, and
     * dequantisation is where reading's time goes, so this is the only
     * change that moves the number much.
     *
     * The last token is fed singly, because it is the one whose logits
     * choose the first word of the answer.
     */
    if (wiki_busy == 1) {
        do {
            if (llm_prefill_step() != 1) continue;
            wiki_tokidx = wiki_ntok - 1;
            wiki_pos    = wiki_ntok - 1;
            wiki_busy   = 3;              /* prompt cached; feed the last */
            wiki_feed(wiki_tokidx);
            break;
        } while (!budget_expired_ms(start, 8));
        if (wiki_busy == 1) return;       /* more prompt to read */
    }

    do {
        if (llm_eval_step() != 1) continue;

        if (wiki_busy == 3) wiki_busy = 2;    /* last prompt token done */

        int next = llm_argmax_penalized(wiki_recent, wiki_recent_n);
        if (next == wiki_im_end || wiki_gen_n >= 48 || wiki_pos + 1 >= LLM_CTX_MAX) {
            /*
             * Check the draft before anyone reads it. What survives is
             * traceable to the entry; what does not is dropped, and if
             * nothing survives the entry answers in its own words.
             */
            {
                /* Keep the draft for the log: the serial line should show
                 * what the model actually wrote, not what survived the
                 * check, or a rejected answer reads as an empty one. */
                str_copy(wiki_draft, wiki_answer, sizeof(wiki_draft));

                int dropped = 0;
                const int kept = wiki_verify_answer(wiki_answer, &dropped);

                wiki_log_add("AI: ");
                if (kept > 0) {
                    wiki_log_add(wiki_answer);
                    wiki_log_add("\n");
                    wiki_log_add("Why: every claim above is stated in the "
                                 "entry for ");
                    wiki_log_add(wiki_source);
                    wiki_log_add(".");
                    if (dropped > 0) {
                        char nb[12];
                        uint_to_str((uint32_t)dropped, nb);
                        wiki_log_add(" I dropped ");
                        wiki_log_add(nb);
                        wiki_log_add(dropped == 1 ? " sentence the entry did"
                                                    " not support."
                                                  : " sentences the entry did"
                                                    " not support.");
                    }
                    wiki_log_add("\n");
                } else {
                    char line[400];
                    wiki_evidence_line(line, sizeof(line));
                    if (line[0]) {
                        wiki_log_add("I could not verify a written answer "
                                     "against the entry, so here is what it "
                                     "says:\n  ");
                        wiki_log_add(wiki_source);
                        wiki_log_add(" - ");
                        wiki_log_add(line);
                        wiki_log_add("\nWhy: the draft answer used wording "
                                     "the entry does not, so it was "
                                     "discarded and the entry quoted "
                                     "instead.\n");
                    } else {
                        wiki_log_add("The entry does not answer that, and I "
                                     "will not answer from memory.\n");
                    }
                }
                serial_puts("[wiki] verify: kept ");
                serial_put_dec((uint32_t)kept);
                serial_puts(", dropped ");
                serial_put_dec((uint32_t)dropped);
                serial_putc('\n');
            }
            wiki_busy = 0;

            serial_puts("[wiki] answered in ");
            serial_put_dec(cycles_to_ms(cycle_now() - wiki_t0));
            serial_puts(" ms, ");
            serial_put_dec((uint32_t)wiki_ntok);
            serial_puts(" prompt + ");
            serial_put_dec((uint32_t)wiki_gen_n);
            serial_puts(" generated tokens\n[wiki] draft: ");
            serial_puts(wiki_draft);
            serial_putc('\n');
            if (wiki_answer_len == 0 && wiki_gen_n > 0) {
                serial_puts("[wiki] answer decoded empty; first ids:");
                const int n = wiki_gen_n < WIKI_GEN_TRACE
                            ? wiki_gen_n : WIKI_GEN_TRACE;
                for (int i = 0; i < n; i++) {
                    serial_putc(' ');
                    serial_put_dec((uint32_t)wiki_gen_ids[i]);
                }
                serial_putc('\n');
            }
            return;
        }

        if (wiki_gen_n < WIKI_GEN_TRACE) wiki_gen_ids[wiki_gen_n] = next;

        if (wiki_recent_n < WIKI_REPEAT_WIN) {
            wiki_recent[wiki_recent_n++] = next;
        } else {
            for (int i = 0; i + 1 < WIKI_REPEAT_WIN; i++)
                wiki_recent[i] = wiki_recent[i + 1];
            wiki_recent[WIKI_REPEAT_WIN - 1] = next;
        }

        char piece[64];
        llm_decode(next, piece, sizeof(piece));
        for (int i = 0; piece[i] && wiki_answer_len < WIKI_ANS_MAX - 2; i++)
            wiki_answer[wiki_answer_len++] = piece[i];
        wiki_answer[wiki_answer_len] = '\0';

        wiki_gen_n++;
        wiki_pos++;
        llm_eval_begin(next, wiki_pos);
    } while (!budget_expired_ms(start, 8));
}

static void wiki_chat_key(char ch) {
    if (ch == 27) { wiki_mode = 0; return; }
    if (wiki_busy) return;
    if (ch == '\n') { wiki_submit(); return; }
    if (ch == '\b') {
        if (wiki_input_len > 0) wiki_input[--wiki_input_len] = '\0';
        return;
    }
    if (ch >= 0x20 && ch < 0x7F && wiki_input_len < (int)sizeof(wiki_input) - 1) {
        wiki_input[wiki_input_len++] = ch;
        wiki_input[wiki_input_len] = '\0';
    }
}

static void wiki_chat_draw(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                           uint32_t tick, int focused) {
    gfx_rect(buf, w, h, cx, cy + 42, cw, chh - 42, C_WIN_BG);

    /* transcript */
    int32_t y = cy + 52;
    const char *p = wiki_log;
    char line[96];
    int li = 0;
    while (*p && y < cy + chh - 62) {
        if (*p == '\n' || li >= 88) {
            line[li] = '\0';
            if (li) {
                int is_you = (line[0] == 'Y' && line[1] == 'o' && line[2] == 'u');
                int is_ctx = (line[0] == '[');
                ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line,
                                is_you ? 0x1A1E28u : (is_ctx ? 0x8A8F9Cu : C_LINK), 12);
            }
            y += 17;
            li = 0;
            if (*p == '\n') p++;
            continue;
        }
        line[li++] = *p++;
    }
    if (li && y < cy + chh - 62) {
        line[li] = '\0';
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, line, C_LINK, 12);
    }

    /* the answer as it arrives */
    if (wiki_busy) {
        int pct = llm_eval_progress();
        char st[64], nb[12];
        str_copy(st, wiki_busy == 4 ? "surveying the archive " :
                 wiki_busy == 1 ? "reading the question " : "thinking ",
                 sizeof(st));
        uint_to_str((uint32_t)(wiki_busy == 1
                    ? wiki_tokidx * 100 / (wiki_ntok ? wiki_ntok : 1) : pct), nb);
        str_append(st, nb, sizeof(st));
        str_append(st, "%", sizeof(st));
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, cy + chh - 56, st,
                        C_LINK, 12);
        int32_t bw = cw - 28;
        gfx_rect(buf, w, h, cx + 14, cy + chh - 38, bw, 4, 0xD5D8E0u);
        gfx_rect(buf, w, h, cx + 14, cy + chh - 38, bw * pct / 100, 4, C_GOLD);
        if (wiki_answer_len) {
            char fit[96];
            store_fit(fit, sizeof(fit), wiki_answer, cw - 28, 12);
            ttf_draw_string(buf, (int)w, (int)h, cx + 14, cy + chh - 74, fit,
                            0x2E7D4Fu, 12);
        }
    }

    /* input box */
    gfx_rect(buf, w, h, cx + 12, cy + chh - 30, cw - 24, 24, 0xFFFFFFu);
    gfx_rect_outline(buf, w, h, cx + 12, cy + chh - 30, cw - 24, 24,
                     wiki_busy ? 0xD0D3DAu : (focused ? C_GOLD : 0xB8BCC8u));
    if (wiki_input_len == 0) {
        ttf_draw_string(buf, (int)w, (int)h, cx + 20, cy + chh - 27,
                        wiki_busy ? "working..." : "Ask about an article...",
                        0xA0A4AEu, 12);
    } else {
        char fit[96];
        store_fit(fit, sizeof(fit), wiki_input, cw - 40, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + 20, cy + chh - 27, fit,
                        C_INK, 12);
    }
    if (focused && !wiki_busy && ((tick / 30) & 1) == 0) {
        int cwid = ttf_text_width(wiki_input, 12);
        gfx_rect(buf, w, h, cx + 21 + cwid, cy + chh - 26, 1, 17, C_INK);
    }
}

static void wiki_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                      uint32_t tick, int focused) {
    wiki_autoopen();
    /* wiki_gen_poll() is driven from the frame loop, not from here --
     * see desktop_render. An answer must not depend on being watched. */
    gfx_rect(buf, w, h, cx, cy, cw, chh, C_WIN_BG);

    /* header */
    gfx_vgrad(buf, w, h, cx, cy, cw, 42, 0x1B2030u, 0x11141Cu);
    gfx_rect(buf, w, h, cx, cy + 41, cw, 1, C_GOLD_DIM);
    ttf_draw_string(buf, (int)w, (int)h, cx + 16, cy + 10, "Wikipedia",
                    C_GOLD, 18);
    {
        /*
         * The subtitle is the one place someone waiting for an answer
         * looks, so every state that involves waiting has to name itself
         * here. Three of them did not.
         *
         * Reading the prompt is the long one: the retrieved article is a
         * few hundred tokens and every one is a full forward pass, so
         * there is a real wait between pressing return and the first word
         * appearing. Saying nothing during it makes a working machine look
         * like a hung one — which is exactly how it looked.
         *
         * And "ask" was shown whenever the loader was not *currently*
         * busy, including when there is no model on the volume at all. An
         * invitation to do something impossible is worse than silence.
         */
        static char ai_sub[32];
        const char *sub;
        char nb[8];
        if (wiki_mode && ai_busy()) {
            uint_to_str((uint32_t)ai_progress(), nb);
            str_copy(ai_sub, "loading the model ", sizeof(ai_sub));
            str_append(ai_sub, nb, sizeof(ai_sub));
            str_append(ai_sub, "%", sizeof(ai_sub));
            sub = ai_sub;
        } else if (wiki_mode && (wiki_busy == 1 || wiki_busy == 3)) {
            /* consuming the prompt: progress is real and worth showing */
            int pct = wiki_ntok > 0 ? wiki_tokidx * 100 / wiki_ntok : 0;
            uint_to_str((uint32_t)pct, nb);
            str_copy(ai_sub, "reading the article ", sizeof(ai_sub));
            str_append(ai_sub, nb, sizeof(ai_sub));
            str_append(ai_sub, "%", sizeof(ai_sub));
            sub = ai_sub;
        } else if (wiki_mode && wiki_busy) {
            sub = "answering";
        } else if (wiki_mode) {
            sub = llm_weights_loaded() ? "ask" : "no model on the volume";
        } else {
            sub = zim.open ? "offline archive" : "no archive";
        }
        if (ai_enabled != 1 && wiki_mode) wiki_mode = 0;   /* cannot be here */
        int tw = ttf_text_width(sub, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + cw - tw - 52, cy + 16, sub,
                        C_TEXT_DIM, 12);
    }

    /* The chat bubble, top right -- absent entirely when the account
     * turned the model off. An affordance for something switched off is
     * worse than no affordance. */
    if (ai_enabled == 1)
    {
        int32_t bx = cx + cw - 40, by = cy + 10;
        uint32_t fill = wiki_mode ? C_GOLD : 0x2A3040u;
        uint32_t ink  = wiki_mode ? 0x11141Cu : C_TEXT;
        gfx_rect(buf, w, h, bx, by, 30, 18, fill);
        gfx_rect(buf, w, h, bx + 1, by - 1, 28, 1, fill);
        gfx_rect(buf, w, h, bx + 1, by + 18, 28, 1, fill);
        /* tail */
        gfx_tri(buf, w, h, bx + 6, by + 18, bx + 6, by + 24, bx + 14, by + 18,
                fill);
        for (int d = 0; d < 3; d++)
            gfx_rect(buf, w, h, bx + 7 + d * 8, by + 8, 3, 3, ink);
    }

    if (wiki_mode) {
        wiki_chat_draw(buf, w, h, cx, cy, cw, chh, tick, focused);
        return;
    }

    /* ---- reading an article ---- */
    if (wiki_view == 1) {
        int32_t vy = cy + 74, vh = chh - 74 - 22;
        wiki_view_h = vh;

        /* Re-lay-out if the column changed. Layout is a function of the
         * width it was given, so this cannot drift the way the browser's
         * stale global does. */
        if (cw != wiki_last_cw) {
            wiki_last_cw = cw;
            int wrap = wiki_wrap_for(cw);
            if (wrap != wiki_art_wrap && wiki_art_path[0]) {
                uint32_t idx;
                const uint8_t *data;
                uint32_t len;
                zim_dirent_t e;
                if (zim_find('C', wiki_art_path, &idx) &&
                    zim_content(idx, &data, &len, &e) == 0) {
                    wdoc_parse(data, (int)len, wrap);
                    wiki_art_wrap = wrap;
                    int maxs = wd_total_h - vh;
                    if (maxs < 0) maxs = 0;
                    if (wiki_scroll > maxs) wiki_scroll = maxs;
                }
            }
        }

        /* Back chevron and the article's title */
        {
            int back_hot = 1;
            uint32_t bc = back_hot ? C_GOLD_DIM : 0xB8BCC8u;
            int32_t bx = cx + 18, by = cy + 59;
            for (int k = 0; k < 6; k++) {
                gfx_rect(buf, w, h, bx + k, by - k, 1, 1, bc);
                gfx_rect(buf, w, h, bx + k, by + k, 1, 1, bc);
            }
            ttf_draw_string(buf, (int)w, (int)h, cx + 30, cy + 50,
                            wiki_hist_n > 0 ? "Back" : "Results", bc, 12);

            char fit[WD_TITLE_MAX];
            store_fit(fit, sizeof(fit), wiki_art_title, cw - 110, 15);
            int tw = ttf_text_width(fit, 15);
            ttf_draw_string(buf, (int)w, (int)h,
                            cx + cw - tw - 16, cy + 50, fit, C_INK, 15);
            gfx_rect(buf, w, h, cx + 10, cy + 72, cw - 20, 1, 0xD5D8E0u);
        }

        wdoc_draw(buf, w, h, cx + 14, vy, cw - 28, vh, wiki_scroll,
                  wiki_hover);

        /* scrollbar */
        if (wd_total_h > vh && vh > 40) {
            int32_t sb_x = cx + cw - 12;
            gfx_rect(buf, w, h, sb_x, vy, 10, vh, 0xE2E3E8u);
            int knob_h = vh * vh / wd_total_h;
            if (knob_h < 24) knob_h = 24;
            int maxs = wd_total_h - vh;
            int knob_y = vy + (vh - knob_h) * wiki_scroll /
                         (maxs > 0 ? maxs : 1);
            gfx_rect(buf, w, h, sb_x + 1, knob_y, 8, knob_h, 0xA8ACB8u);
        }

        gfx_rect(buf, w, h, cx, cy + chh - 22, cw, 22, 0xE8E9EEu);
        gfx_rect(buf, w, h, cx, cy + chh - 22, cw, 1, 0xD5D8E0u);
        {
            const char *st = wiki_status;
            char link[WIKI_PATH_MAX];
            if (wiki_hover >= 0) {
                wiki_resolve_link(wdoc_href_of(wiki_hover), link, sizeof(link));
                if (link[0]) st = link;
            }
            char fit[112];
            store_fit(fit, sizeof(fit), st, cw - 24, 12);
            ttf_draw_string(buf, (int)w, (int)h, cx + 12, cy + chh - 19, fit,
                            wiki_status_err ? 0xB0322Eu : 0x50555Fu, 12);
        }
        return;
    }

    wiki_last_cw = cw;

    /* search box */
    gfx_rect(buf, w, h, cx + 14, cy + 50, cw - 28, 24, 0xFFFFFFu);
    gfx_rect_outline(buf, w, h, cx + 14, cy + 50, cw - 28, 24,
                     focused ? C_GOLD : 0xB8BCC8u);
    if (wiki_qlen == 0) {
        ttf_draw_string(buf, (int)w, (int)h, cx + 22, cy + 53,
                        "Type an article title...", 0xA0A4AEu, 13);
    } else {
        ttf_draw_string(buf, (int)w, (int)h, cx + 22, cy + 53, wiki_query,
                        C_INK, 13);
    }
    if (focused && ((tick / 30) & 1) == 0) {
        int cwid = ttf_text_width(wiki_query, 13);
        gfx_rect(buf, w, h, cx + 23 + cwid, cy + 54, 1, 17, C_INK);
    }

    /* results */
    int32_t ly = cy + 74;
    for (int i = 0; i < wiki_hit_count; i++) {
        int32_t ry = ly + i * WIKI_ROW_H;
        if (ry + WIKI_ROW_H > cy + chh - 22) break;
        if (i == wiki_sel) {
            gfx_rect(buf, w, h, cx + 8, ry, cw - 16, WIKI_ROW_H, 0x2A2410u);
            gfx_rect(buf, w, h, cx + 8, ry, 3, WIKI_ROW_H, C_GOLD);
        }
        char fit[72];
        store_fit(fit, sizeof(fit), wiki_hits[i].title, cw - 60, 13);
        ttf_draw_string(buf, (int)w, (int)h, cx + 20, ry + 3, fit,
                        i == wiki_sel ? C_GOLD
                                      : (wiki_hits[i].redirect ? 0x8A8F9Cu
                                                               : C_INK), 13);
        if (wiki_hits[i].redirect)
            ttf_draw_string(buf, (int)w, (int)h, cx + cw - 74, ry + 4,
                            "redirect", 0xA0A4AEu, 11);
    }

    if (zim.open && wiki_hit_count == 0) {
        const char *msg = wiki_qlen ? "No entry starts with that"
                                    : "Start typing to search";
        int tw = ttf_text_width(msg, 13);
        ttf_draw_string(buf, (int)w, (int)h, cx + (cw - tw) / 2, ly + 30, msg,
                        0x8A8F9Cu, 13);
    }

    /* status bar */
    gfx_rect(buf, w, h, cx, cy + chh - 22, cw, 22, 0xE8E9EEu);
    gfx_rect(buf, w, h, cx, cy + chh - 22, cw, 1, 0xD5D8E0u);
    {
        char fit[112];
        store_fit(fit, sizeof(fit), wiki_status, cw - 24, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + 12, cy + chh - 19, fit,
                        wiki_status_err ? 0xB0322Eu : 0x50555Fu, 12);
    }
}

/* ===================== ABOUT ===================== */

static void about_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       uint32_t tick, int focused) {
    (void)tick; (void)focused;
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x10131Cu);

    /* mini dragon mark */
    int32_t dx = cx + cw / 2;
    int32_t dy = cy + 52;
    gfx_tri(buf, w, h, dx - 34, dy + 12, dx, dy - 22, dx + 34, dy + 12, C_GOLD);
    gfx_tri(buf, w, h, dx - 20, dy + 12, dx, dy - 8, dx + 20, dy + 12, 0x10131Cu);

    {
        const char *t = "Vextro 9";
        int tw = ttf_text_width(t, 20);
        ttf_draw_string(buf, (int)w, (int)h, cx + (cw - tw) / 2, cy + 78,
                        t, C_TEXT, 20);
    }
    {
        const char *t = "Version 9.0 - bare metal x86_64";
        int tw = ttf_text_width(t, 13);
        ttf_draw_string(buf, (int)w, (int)h, cx + (cw - tw) / 2, cy + 110,
                        t, C_TEXT_DIM, 13);
    }
    gfx_rect(buf, w, h, cx + 40, cy + 138, cw - 80, 1, 0x2A3040u);
    {
        const char *lines[4] = {
            "TrueType rasterizer - no floats",
            "TCP/IP stack + HTTP browser",
            "Window manager, terminal, paint",
            "Comic Neue (OFL) - Limine boot",
        };
        for (int i = 0; i < 4; i++) {
            int tw = ttf_text_width(lines[i], 12);
            ttf_draw_string(buf, (int)w, (int)h, cx + (cw - tw) / 2,
                            cy + 152 + i * 20, lines[i], 0x9098A8u, 12);
        }
    }
}

#endif /* APPS_H */
