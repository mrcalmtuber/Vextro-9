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

    /* full ramdisk path for socrates://file/ */
    char url[300];
    str_copy(url, "socrates://file/", sizeof(url));
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

static const char *dock_edge_names[3] = { "Bottom", "Left", "Right" };

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

        str_copy(line, "Socrates BSD 9.0  x86_64", sizeof(line));
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
static int        wiki_status_err = 0;
static int        wiki_tried_open = 0;

/* Archives people actually leave lying around, tried in order. */
static const char *wiki_candidates[4] = {
    "/wiki.zim", "/wikipedia.zim", "/zim/wiki.zim", "/wiki/wiki.zim"
};

static void wiki_search(void);

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
            return;
        }
    }
    str_copy(wiki_status, "No archive found.  Put a .zim on the disk as "
             "/wiki.zim, or run 'zim open <file>'.", sizeof(wiki_status));
    wiki_status_err = 1;
}

static void wiki_search(void) {
    wiki_hit_count = 0;
    wiki_sel = 0;
    if (!zim.open) return;

    uint32_t i = zim_lower_bound('C', wiki_query);
    zim_dirent_t e;
    while (i < zim.article_count && wiki_hit_count < WIKI_RESULTS) {
        if (zim_dirent(i, &e) != 0) break;
        if (e.ns != 'C') break;
        /* the walk stops as soon as the prefix stops matching */
        int k = 0;
        while (wiki_query[k] && e.url[k] == wiki_query[k]) k++;
        if (wiki_query[k] != '\0') break;

        wiki_hit_t *h = &wiki_hits[wiki_hit_count++];
        h->index = i;
        str_copy(h->title, e.title, sizeof(h->title));
        h->redirect = e.is_redirect;
        i++;
    }
}

static void wiki_open_hit(int idx) {
    if (idx < 0 || idx >= wiki_hit_count) return;
    zim_dirent_t e;
    if (zim_dirent(wiki_hits[idx].index, &e) != 0) return;

    char url[BRW_ADDR_MAX];
    str_copy(url, "zim://", sizeof(url));
    str_append(url, e.url, sizeof(url));
    brw_navigate(url);
    wm_open(WK_BROWSER);
}

static void wiki_key(char ch) {
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
    if (!click) return;
    if (mx < cx || mx >= cx + cw || my < cy || my >= cy + chh) return;

    int32_t list_y = cy + 74;
    int idx = (my - list_y) / WIKI_ROW_H;
    if (idx >= 0 && idx < wiki_hit_count) {
        wiki_sel = idx;
        wiki_open_hit(idx);
    }
}

static void wiki_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                      uint32_t tick, int focused) {
    wiki_autoopen();
    gfx_rect(buf, w, h, cx, cy, cw, chh, C_WIN_BG);

    /* header */
    gfx_vgrad(buf, w, h, cx, cy, cw, 42, 0x1B2030u, 0x11141Cu);
    gfx_rect(buf, w, h, cx, cy + 41, cw, 1, C_GOLD_DIM);
    ttf_draw_string(buf, (int)w, (int)h, cx + 16, cy + 10, "Wikipedia",
                    C_GOLD, 18);
    {
        const char *sub = zim.open ? "offline archive" : "no archive";
        int tw = ttf_text_width(sub, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + cw - tw - 16, cy + 16, sub,
                        C_TEXT_DIM, 12);
    }

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
        const char *t = "Socrates BSD 9";
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
