#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include "ttf.h"

/* ===== STRING HELPERS ===== */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

static void uint_to_str(uint32_t val, char *out) {
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = (char)('0' + val % 10); val /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

/* ===== CMOS RTC CLOCK ===== */
static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    uint8_t v = inb(0x71);
    return v;
}

static inline uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static void clock_string(char *out) {
    uint8_t statusB = cmos_read(0x0B);
    int is_binary = statusB & 0x04;

    uint8_t hrs = cmos_read(0x04);
    uint8_t min = cmos_read(0x02);
    uint8_t sec = cmos_read(0x00);

    if (!is_binary) {
        hrs = bcd_to_bin(hrs);
        min = bcd_to_bin(min);
        sec = bcd_to_bin(sec);
    }

    out[0] = (char)('0' + hrs / 10);
    out[1] = (char)('0' + hrs % 10);
    out[2] = ':';
    out[3] = (char)('0' + min / 10);
    out[4] = (char)('0' + min % 10);
    out[5] = ':';
    out[6] = (char)('0' + sec / 10);
    out[7] = (char)('0' + sec % 10);
    out[8] = '\0';
}

/* ===== GEOMETRY HELPERS ===== */

#define MENUBAR_H 28

/* Default dock geometry (overridable at runtime via dock_cfg) */
#define DOCK_BAR_Y_DEFAULT  718
#define DOCK_BAR_H_DEFAULT   40
#define DOCK_BAR_W_DEFAULT  562
#define DOCK_ICON_SZ_DEFAULT 28
#define DOCK_APP_COUNT    7

/* Dock edge placement */
#define DOCK_EDGE_BOTTOM  0
#define DOCK_EDGE_LEFT    1
#define DOCK_EDGE_RIGHT   2

typedef struct {
    int32_t bar_y;
    int32_t bar_h;
    int32_t bar_w;
    int32_t icon_sz;
    int     edge;
} dock_config_t;

static dock_config_t dock_cfg = {
    .bar_y   = DOCK_BAR_Y_DEFAULT,
    .bar_h   = DOCK_BAR_H_DEFAULT,
    .bar_w   = DOCK_BAR_W_DEFAULT,
    .icon_sz = DOCK_ICON_SZ_DEFAULT,
    .edge    = DOCK_EDGE_BOTTOM,
};

/* Backward-compat macros — read from live config struct */
#define DOCK_BAR_Y    (dock_cfg.bar_y)
#define DOCK_BAR_H    (dock_cfg.bar_h)
#define DOCK_BAR_W    (dock_cfg.bar_w)
#define DOCK_ICON_SZ  (dock_cfg.icon_sz)

static void dock_recalc(uint32_t scr_w, uint32_t scr_h) {
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        dock_cfg.bar_y = (int32_t)scr_h - dock_cfg.bar_h - 2;
    } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
        dock_cfg.bar_y = ((int32_t)scr_h - dock_cfg.bar_w) / 2;
    } else {
        dock_cfg.bar_y = ((int32_t)scr_h - dock_cfg.bar_w) / 2;
    }
    (void)scr_w;
}

static void fill_tri(uint32_t *buf, uint32_t bw, uint32_t bh,
                     int x0, int y0, int x1, int y1, int x2, int y2,
                     uint32_t color) {
    int tmp;
    if (y0 > y1) { tmp=x0;x0=x1;x1=tmp; tmp=y0;y0=y1;y1=tmp; }
    if (y0 > y2) { tmp=x0;x0=x2;x2=tmp; tmp=y0;y0=y2;y2=tmp; }
    if (y1 > y2) { tmp=x1;x1=x2;x2=tmp; tmp=y1;y1=y2;y2=tmp; }
    if (y2 == y0) return;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= (int)bh) continue;
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1)
            xb = (y1 == y0) ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else
            xb = (y2 == y1) ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { tmp = xa; xa = xb; xb = tmp; }
        if (xa < 0) xa = 0;
        if (xb >= (int)bw) xb = (int)bw - 1;
        for (int x = xa; x <= xb; x++)
            buf[y * (int)bw + x] = color;
    }
}

static void thick_line(uint32_t *buf, uint32_t bw, uint32_t bh,
                       int x0, int y0, int x1, int y1,
                       int thick, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int half = thick / 2;
    for (;;) {
        for (int ry = -half; ry <= half; ry++)
            for (int rx = -half; rx <= half; rx++) {
                int px = x0 + rx, py = y0 + ry;
                if (px >= 0 && px < (int)bw && py >= 0 && py < (int)bh)
                    buf[py * (int)bw + px] = color;
            }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ===== DRAGON WALLPAPER ===== */

static void draw_dragon_logo(uint32_t *buf, uint32_t w, uint32_t h) {
    int cx = (int)w / 2;
    int cy = ((int)h + MENUBAR_H) / 2;
    uint32_t B = 0x000000u;
    uint32_t G = 0xD4AF37u;

    /* --- Wing (drawn first, behind body) --- */
    fill_tri(buf, w, h, cx+80,cy-50,  cx-300,cy-350, cx-20,cy-35,   B);
    fill_tri(buf, w, h, cx-20,cy-35,  cx-300,cy-350, cx-360,cy-15,  B);
    fill_tri(buf, w, h, cx-300,cy-350, cx-360,cy-15, cx-400,cy-120, B);

    /* Wing bone lines (gold) */
    thick_line(buf, w, h, cx+60,cy-45,  cx-280,cy-330, 2, G);
    thick_line(buf, w, h, cx+60,cy-45,  cx-380,cy-100, 2, G);
    thick_line(buf, w, h, cx-340,cy-10, cx-380,cy-100, 2, G);

    /* --- Tail --- */
    fill_tri(buf, w, h, cx-180,cy+20,  cx-160,cy+120, cx-290,cy+80,  B);
    fill_tri(buf, w, h, cx-180,cy+20,  cx-290,cy+80,  cx-420,cy+50,  B);
    fill_tri(buf, w, h, cx-290,cy+80,  cx-420,cy+50,  cx-480,cy-50,  B);

    /* Tail tip accent (gold) */
    fill_tri(buf, w, h, cx-460,cy-40,  cx-485,cy-25,  cx-465,cy+5,   G);

    /* --- Body / torso --- */
    fill_tri(buf, w, h, cx+80,cy-50,   cx+130,cy+80,  cx-20,cy-35,   B);
    fill_tri(buf, w, h, cx+130,cy+80,  cx-20,cy-35,   cx+80,cy+100,  B);
    fill_tri(buf, w, h, cx-20,cy-35,   cx+80,cy+100,  cx-180,cy+20,  B);
    fill_tri(buf, w, h, cx+80,cy+100,  cx-180,cy+20,  cx-120,cy+140, B);
    fill_tri(buf, w, h, cx-180,cy+20,  cx-120,cy+140, cx-160,cy+120, B);

    /* --- Neck --- */
    fill_tri(buf, w, h, cx+190,cy-95,  cx+340,cy+25,  cx+80,cy-50,   B);
    fill_tri(buf, w, h, cx+340,cy+25,  cx+80,cy-50,   cx+130,cy+80,  B);

    /* --- Head --- */
    fill_tri(buf, w, h, cx+260,cy-210, cx+400,cy-30,  cx+190,cy-95,  B);
    fill_tri(buf, w, h, cx+400,cy-30,  cx+190,cy-95,  cx+340,cy+25,  B);
    fill_tri(buf, w, h, cx+400,cy-30,  cx+340,cy+25,  cx+390,cy+60,  B);

    /* --- Horns --- */
    fill_tri(buf, w, h, cx+255,cy-205, cx+225,cy-320, cx+200,cy-180, B);
    fill_tri(buf, w, h, cx+185,cy-175, cx+150,cy-290, cx+150,cy-150, B);

    /* Horn tips (gold) */
    fill_tri(buf, w, h, cx+225,cy-320, cx+240,cy-270, cx+210,cy-265, G);
    fill_tri(buf, w, h, cx+150,cy-290, cx+165,cy-245, cx+135,cy-240, G);

    /* --- Eye (gold diamond) --- */
    fill_tri(buf, w, h, cx+340,cy-80,  cx+352,cy-60,  cx+340,cy-40,  G);
    fill_tri(buf, w, h, cx+340,cy-80,  cx+328,cy-60,  cx+340,cy-40,  G);

    /* --- Spine ridges (gold triangles along back) --- */
    fill_tri(buf, w, h, cx+55,cy-53,   cx+40,cy-78,   cx+25,cy-53,   G);
    fill_tri(buf, w, h, cx+15,cy-42,   cx+0,cy-65,    cx-15,cy-42,   G);
    fill_tri(buf, w, h, cx-25,cy-38,   cx-40,cy-58,   cx-55,cy-38,   G);
    fill_tri(buf, w, h, cx-70,cy-25,   cx-85,cy-45,   cx-100,cy-25,  G);
    fill_tri(buf, w, h, cx-115,cy-12,  cx-130,cy-32,  cx-145,cy-12,  G);

    /* --- Belly scales (gold) --- */
    fill_tri(buf, w, h, cx+100,cy+95,  cx+80,cy+112,  cx+100,cy+112, G);
    fill_tri(buf, w, h, cx+65,cy+110,  cx+45,cy+125,  cx+65,cy+125,  G);
    fill_tri(buf, w, h, cx+30,cy+122,  cx+10,cy+136,  cx+30,cy+136,  G);
    fill_tri(buf, w, h, cx-5,cy+132,   cx-25,cy+145,  cx-5,cy+145,   G);

    /* --- Legs (thick black lines) --- */
    thick_line(buf, w, h, cx+90,cy+95,   cx+140,cy+220, 5, B);
    thick_line(buf, w, h, cx+140,cy+220, cx+110,cy+300, 4, B);
    thick_line(buf, w, h, cx-100,cy+130, cx-80,cy+230,  5, B);
    thick_line(buf, w, h, cx-80,cy+230,  cx-120,cy+300, 4, B);

    /* --- Claws (gold) --- */
    /* Front foot */
    fill_tri(buf, w, h, cx+110,cy+298, cx+95,cy+318,  cx+108,cy+318, G);
    fill_tri(buf, w, h, cx+110,cy+298, cx+112,cy+320, cx+122,cy+315, G);
    fill_tri(buf, w, h, cx+110,cy+298, cx+128,cy+312, cx+132,cy+302, G);
    /* Hind foot */
    fill_tri(buf, w, h, cx-120,cy+298, cx-135,cy+318, cx-122,cy+318, G);
    fill_tri(buf, w, h, cx-120,cy+298, cx-118,cy+320, cx-108,cy+315, G);
    fill_tri(buf, w, h, cx-120,cy+298, cx-102,cy+312, cx-98,cy+302,  G);
}

/* ===== TARFS INTEGRATION (inline for single-TU build) ===== */

#define TAR_BLOCK_SIZE 512

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

static uint8_t *tarfs_base = 0;
static uint64_t tarfs_size = 0;

static uint64_t octal_parse(const char *s, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        val = val * 8 + (uint64_t)(s[i] - '0');
    return val;
}

static void tarfs_init(void *base, uint64_t size) {
    tarfs_base = (uint8_t *)base;
    tarfs_size = size;
}

static const void *fs_read_file(const char *filename, uint64_t *out_size) {
    if (!tarfs_base || !filename) {
        if (out_size) *out_size = 0;
        return 0;
    }

    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;

    while (ptr + TAR_BLOCK_SIZE <= end) {
        tar_header_t *hdr = (tar_header_t *)ptr;
        if (hdr->name[0] == '\0')
            break;

        uint64_t file_size = octal_parse(hdr->size, 12);

        const char *entry_name = hdr->name;
        if (entry_name[0] == '.' && entry_name[1] == '/')
            entry_name += 2;

        const char *query = filename;
        if (query[0] == '/')
            query++;

        if (str_eq(entry_name, query) || str_eq(hdr->name, filename)) {
            if (out_size)
                *out_size = file_size;
            return (const void *)(ptr + TAR_BLOCK_SIZE);
        }

        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }

    if (out_size) *out_size = 0;
    return 0;
}

/* ===== FORWARD DECLARATIONS (explorer) ===== */
static int explorer_open = 0;
static uint32_t explorer_scr_w = 0;
static uint32_t explorer_scr_h = 0;
static void explorer_open_window(uint32_t scr_w, uint32_t scr_h);

/* ===== SETTINGS APPLICATION STATE ===== */

static uint32_t desktop_bg_color = 0xFFFFFFu;
static uint64_t system_total_memory_mb = 0;

#define SETTINGS_WIN_W   420
#define SETTINGS_WIN_H   340
#define SETTINGS_TITLE_H  20
#define SETTINGS_BORDER    1

#define SETTINGS_BG_COUNT 5
static const uint32_t settings_bg_colors[SETTINGS_BG_COUNT] = {
    0xFFFFFFu, 0xFFF5E1u, 0x1E1E2Eu, 0xE8DFD0u, 0xC8D0D8u,
};
static const char *settings_bg_names[SETTINGS_BG_COUNT] = {
    "White", "Gold Tint", "Midnight", "Parchment", "Slate",
};
static int settings_bg_index = 0;

static int     settings_open = 0;
static int32_t set_win_x = -1;
static int32_t set_win_y = -1;
static int32_t set_win_w = SETTINGS_WIN_W;
static int32_t set_win_h = SETTINGS_WIN_H;
static int32_t set_win_disp_x_fp = -1;
static int32_t set_win_disp_y_fp = -1;
static int     set_win_dragging = 0;
static int32_t set_win_drag_ox = 0;
static int32_t set_win_drag_oy = 0;
static uint8_t set_win_prev_lmb = 0;
static uint8_t set_close_prev_lmb = 0;
static uint8_t set_btn_prev_lmb = 0;

static void settings_open_window(uint32_t scr_w, uint32_t scr_h);

/* ===== WEB BROWSER APPLICATION STATE ===== */

#define BROWSER_WIN_W   680
#define BROWSER_WIN_H   480
#define BROWSER_TITLE_H  24
#define BROWSER_ADDR_H   26
#define BROWSER_BORDER     1

static int     browser_open = 0;
static int32_t brw_win_x = -1;
static int32_t brw_win_y = -1;
static int32_t brw_win_w = BROWSER_WIN_W;
static int32_t brw_win_h = BROWSER_WIN_H;
static int32_t brw_win_disp_x_fp = -1;
static int32_t brw_win_disp_y_fp = -1;
static int     brw_win_dragging = 0;
static int32_t brw_win_drag_ox = 0;
static int32_t brw_win_drag_oy = 0;
static uint8_t brw_win_prev_lmb = 0;
static uint8_t brw_close_prev_lmb = 0;

static char    brw_address[128] = "socrates://home";
static int     brw_addr_len = 15;
static uint8_t brw_page_buf[4096];
static int     brw_page_len = 0;

static void browser_open_window(uint32_t scr_w, uint32_t scr_h);

/* ===== DROPDOWN MENU STATE ===== */

static int dropdown_open = 0;      /* 1 = Tools dropdown is visible */
static int is_terminal_open = 0;   /* 0 = hidden on boot */
static uint8_t menu_prev_lmb = 0;

/* "Tools" menu item hit box (pixel coords in menubar)
 * Menu string: "File   Edit   View   Tools   About"
 * Font size 16 with 1/8 inter-char padding, advance ~10.8 px/char, drawn at x=14.
 * "Tools" starts at char 21 → x ≈ 14+21*11 = 245, spans 5 chars = 55px.
 * Use generous bounds for reliable clicking. */
#define TOOLS_MENU_X  238
#define TOOLS_MENU_W   64
#define DROPDOWN_W    140
#define DROPDOWN_H     28
#define DROPDOWN_Y    MENUBAR_H

/* ===== DOCK APP REGISTRY ===== */

typedef struct {
    const char *name;
    const char *cmd;
    char        icon_letter;
} dock_app_entry_t;

static const dock_app_entry_t dock_apps[DOCK_APP_COUNT] = {
    { "hello",     "run hello",     'H' },
    { "goldsmith", "run goldsmith", 'G' },
    { "monolith",  "run monolith",  'M' },
    { "matrix",    "run matrix",    'X' },
    { "browser",   "run browser",   'B' },
    { "explorer",  "__explorer__",  'E' },
    { "settings",  "__settings__",  '*' },
};

static void dock_icon_rect(uint32_t scr_w, int idx,
                           int32_t *ox, int32_t *oy,
                           int32_t *ow, int32_t *oh) {
    int32_t bw = dock_cfg.bar_w;
    int32_t bh = dock_cfg.bar_h;
    int32_t by = dock_cfg.bar_y;
    int32_t isz = dock_cfg.icon_sz;

    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        int32_t dock_x = ((int32_t)scr_w - bw) / 2;
        int32_t inner_x = dock_x + 1;
        int32_t inner_w = bw - 2;
        int32_t cell_w  = inner_w / DOCK_APP_COUNT;
        *ox = inner_x + idx * cell_w + (cell_w - isz) / 2;
        *oy = by + (bh - isz) / 2;
    } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
        int32_t dock_y = by;
        int32_t inner_y = dock_y + 1;
        int32_t inner_h = bw - 2;
        int32_t cell_h  = inner_h / DOCK_APP_COUNT;
        *ox = 2 + (bh - isz) / 2;
        *oy = inner_y + idx * cell_h + (cell_h - isz) / 2;
    } else {
        int32_t dock_y = by;
        int32_t inner_y = dock_y + 1;
        int32_t inner_h = bw - 2;
        int32_t cell_h  = inner_h / DOCK_APP_COUNT;
        *ox = (int32_t)scr_w - bh - 2 + (bh - isz) / 2;
        *oy = inner_y + idx * cell_h + (cell_h - isz) / 2;
    }
    *ow = isz;
    *oh = isz;
}

/* ===== MENU BAR ===== */

static void desktop_draw_menubar(uint32_t *buf, uint32_t w, uint32_t h) {
    for (uint32_t row = 0; row < MENUBAR_H && row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            buf[row * w + col] = 0xD4AF37u;

    int fs = 16;
    int ty = (MENUBAR_H - fs) / 2;
    ttf_draw_string(buf, (int)w, (int)h, 14, ty,
                    "File   Edit   View   Tools   About",
                    0xFFFFFFu, fs);

    char clk[16];
    clock_string(clk);
    int clk_w = 8 * (fs * 6 / 10 + fs / 8);
    int clk_x = (int)w - clk_w - 16;
    ttf_draw_string(buf, (int)w, (int)h, clk_x, ty, clk, 0xFFFFFFu, fs);
}

static void desktop_draw_dropdown(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!dropdown_open) return;

    int dx = TOOLS_MENU_X;
    int dy = DROPDOWN_Y;

    /* Dropdown background */
    for (int row = dy; row < dy + DROPDOWN_H && row < (int)h; row++)
        for (int col = dx; col < dx + DROPDOWN_W && col < (int)w; col++) {
            int is_border = (row == dy || row == dy + DROPDOWN_H - 1 ||
                             col == dx || col == dx + DROPDOWN_W - 1);
            buf[row * (int)w + col] = is_border ? 0xD4AF37u : 0x1A1508u;
        }

    /* "Terminal" label — centered vertically in the taller dropdown */
    int fs = 15;
    int tx = dx + 10;
    int tty = dy + (DROPDOWN_H - fs) / 2;
    ttf_draw_string(buf, (int)w, (int)h, tx, tty, "Terminal", 0xD4AF37u, fs);
}

static void desktop_handle_menu_click(int32_t mx, int32_t my, uint8_t lmb) {
    int click = (lmb && !menu_prev_lmb);
    menu_prev_lmb = lmb;

    if (!click) return;

    /* Click on "Tools" text in menubar */
    if (my >= 0 && my < MENUBAR_H &&
        mx >= TOOLS_MENU_X && mx < TOOLS_MENU_X + TOOLS_MENU_W) {
        dropdown_open = !dropdown_open;
        return;
    }

    /* Click inside dropdown */
    if (dropdown_open) {
        int dx = TOOLS_MENU_X;
        int dy = DROPDOWN_Y;
        if (mx >= dx && mx < dx + DROPDOWN_W &&
            my >= dy && my < dy + DROPDOWN_H) {
            is_terminal_open = 1;
            dropdown_open = 0;
            return;
        }
    }

    /* Click anywhere else closes dropdown */
    if (dropdown_open)
        dropdown_open = 0;
}

/* ===== TERMINAL WINDOW ===== */

#define TERM_WIN_W   700
#define TERM_WIN_H   420
#define TERM_TITLE_H  24
#define TERM_BORDER    1
#define TERM_FONT_SZ  16
#define TERM_LINE_H   20
#define TERM_PAD_X     6
#define TERM_PAD_Y     4

#define TERM_COLS     80
#define TERM_ROWS     20
#define TERM_BUF_SZ   (TERM_COLS * TERM_ROWS)

static char  term_buf[TERM_BUF_SZ];
static int   term_cx = 0;
static int   term_cy = 0;
static int   term_inited = 0;
static int   term_ready = 0;

#define TERM_CMD_MAX 80
static char  term_cmd_buf[TERM_CMD_MAX];
static int   term_cmd_len = 0;

/* ===== CLOSE BUTTON GEOMETRY ===== */

#define CLOSE_BTN_SIZE  14
#define CLOSE_BTN_PAD    3

/* ===== UNIFIED SPAWN ANIMATION (for all Dock apps) ===== */

#define SPAWN_ANIM_FRAMES 15

typedef struct {
    int     active;
    int     tick;
    int32_t src_x, src_y;
    int32_t dst_x, dst_y;
    int32_t dst_w, dst_h;
} spawn_anim_t;

static spawn_anim_t spawn_anim = {0};

static void spawn_anim_start(int32_t icon_cx, int32_t icon_cy,
                              int32_t win_x, int32_t win_y,
                              int32_t win_w, int32_t win_h) {
    spawn_anim.active = 1;
    spawn_anim.tick   = 0;
    spawn_anim.src_x  = icon_cx;
    spawn_anim.src_y  = icon_cy;
    spawn_anim.dst_x  = win_x;
    spawn_anim.dst_y  = win_y;
    spawn_anim.dst_w  = win_w;
    spawn_anim.dst_h  = win_h;
}

static void spawn_anim_draw(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!spawn_anim.active) return;
    spawn_anim.tick++;
    int t = spawn_anim.tick;
    if (t > SPAWN_ANIM_FRAMES) t = SPAWN_ANIM_FRAMES;

    int32_t src_w = 4, src_h = 4;
    int32_t cur_x = spawn_anim.src_x + (spawn_anim.dst_x - spawn_anim.src_x) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_y = spawn_anim.src_y + (spawn_anim.dst_y - spawn_anim.src_y) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_w = src_w + (spawn_anim.dst_w - src_w) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_h = src_h + (spawn_anim.dst_h - src_h) * t / SPAWN_ANIM_FRAMES;

    uint32_t alpha = (uint32_t)t * 255u / SPAWN_ANIM_FRAMES;
    for (int32_t row = cur_y; row < cur_y + cur_h && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = cur_x; col < cur_x + cur_w && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == cur_y || row == cur_y + cur_h - 1 ||
                             col == cur_x || col == cur_x + cur_w - 1);
            int is_titlebar = (row > cur_y && row < cur_y + TERM_TITLE_H + 1 &&
                               col > cur_x && col < cur_x + cur_w - 1);
            if (is_border || is_titlebar) {
                uint32_t idx = (uint32_t)row * w + (uint32_t)col;
                uint32_t bg_px = buf[idx];
                uint32_t br = (bg_px >> 16) & 0xFF;
                uint32_t bg_g = (bg_px >> 8) & 0xFF;
                uint32_t bb = bg_px & 0xFF;
                uint32_t r = (0xD4u * alpha + br * (255u - alpha)) / 255u;
                uint32_t g = (0xAFu * alpha + bg_g * (255u - alpha)) / 255u;
                uint32_t b = (0x37u * alpha + bb * (255u - alpha)) / 255u;
                buf[idx] = (r << 16) | (g << 8) | b;
            }
        }
    }

    if (spawn_anim.tick >= SPAWN_ANIM_FRAMES)
        spawn_anim.active = 0;
}

/* ===== DYNAMIC WINDOW STATE (Step 4: drag-and-drop) ===== */

static int32_t window_x = -1;
static int32_t window_y = -1;
static int32_t window_width  = TERM_WIN_W;
static int32_t window_height = TERM_WIN_H;

static int     is_dragging = 0;
static int32_t drag_offset_x = 0;
static int32_t drag_offset_y = 0;
static uint8_t drag_prev_lmb = 0;
static uint8_t close_prev_lmb = 0;

/* Smooth display position (spring physics) — 8-bit fixed point */
static int32_t window_disp_x_fp = -1;
static int32_t window_disp_y_fp = -1;

/* Trail history for liquid glass decay effect */
#define TRAIL_LEN 4
static int32_t trail_x[TRAIL_LEN];
static int32_t trail_y[TRAIL_LEN];
static int     trail_count = 0;
static int     trail_active = 0;

/* Open animation state */
#define OPEN_ANIM_FRAMES 15
static int open_anim_tick = 0;
static int open_anim_active = 0;

static void window_init_pos(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    window_width  = TERM_WIN_W;
    window_height = TERM_WIN_H;
    window_x = ((int32_t)scr_w - window_width) / 2;
    window_y = ((DOCK_BAR_Y - MENUBAR_H - window_height) / 2) + MENUBAR_H;
    if (window_x < 0) window_x = 0;
    if (window_y < MENUBAR_H) window_y = MENUBAR_H;
    window_disp_x_fp = window_x << 8;
    window_disp_y_fp = window_y << 8;
    trail_count = 0;
    trail_active = 0;
}

static void draw_close_button(uint32_t *buf, uint32_t bw, uint32_t bh,
                              int32_t wx, int32_t wy, int32_t ww) {
    int32_t btn_x = wx + ww - TERM_BORDER - CLOSE_BTN_PAD - CLOSE_BTN_SIZE;
    int32_t btn_y = wy + TERM_BORDER + (TERM_TITLE_H - CLOSE_BTN_SIZE) / 2;
    int32_t sz = CLOSE_BTN_SIZE;

    for (int i = 0; i < sz; i++) {
        int32_t px, py;

        /* Top-left to bottom-right diagonal */
        px = btn_x + i; py = btn_y + i;
        if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
            buf[(uint32_t)py * bw + (uint32_t)px] = 0xFFFFFFu;
        px = btn_x + i + 1; py = btn_y + i;
        if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
            buf[(uint32_t)py * bw + (uint32_t)px] = 0xFFFFFFu;

        /* Top-right to bottom-left diagonal */
        px = btn_x + sz - 1 - i; py = btn_y + i;
        if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
            buf[(uint32_t)py * bw + (uint32_t)px] = 0xFFFFFFu;
        px = btn_x + sz - 2 - i; py = btn_y + i;
        if (px >= 0 && px < (int32_t)bw && py >= 0 && py < (int32_t)bh)
            buf[(uint32_t)py * bw + (uint32_t)px] = 0xFFFFFFu;
    }
}

static int hit_close_button(int32_t mx, int32_t my,
                            int32_t wx, int32_t wy, int32_t ww) {
    int32_t btn_x = wx + ww - TERM_BORDER - CLOSE_BTN_PAD - CLOSE_BTN_SIZE;
    int32_t btn_y = wy + TERM_BORDER + (TERM_TITLE_H - CLOSE_BTN_SIZE) / 2;
    return (mx >= btn_x && mx < btn_x + CLOSE_BTN_SIZE &&
            my >= btn_y && my < btn_y + CLOSE_BTN_SIZE);
}

static void window_handle_drag(int32_t mx, int32_t my, uint8_t lmb,
                                uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    int click  = (lmb && !drag_prev_lmb);
    int release = (!lmb && drag_prev_lmb);
    drag_prev_lmb = lmb;

    /* Close button check — on click, test before drag */
    int close_click = (lmb && !close_prev_lmb);
    close_prev_lmb = lmb;
    if (close_click && hit_close_button(mx, my, window_x, window_y, window_width)) {
        is_terminal_open = 0;
        term_ready = 0;
        term_inited = 0;
        is_dragging = 0;
        trail_active = 0;
        trail_count = 0;
        window_x = -1;
        return;
    }

    if (click && !is_dragging) {
        if (mx >= window_x && mx < window_x + window_width &&
            my >= window_y && my < window_y + TERM_TITLE_H + TERM_BORDER) {
            is_dragging = 1;
            drag_offset_x = mx - window_x;
            drag_offset_y = my - window_y;
        }
    }
    if (release) {
        if (is_dragging) trail_active = 0;
        is_dragging = 0;
    }

    if (is_dragging) {
        int32_t new_x = mx - drag_offset_x;
        int32_t new_y = my - drag_offset_y;

        if (new_x < 0) new_x = 0;
        if (new_y < MENUBAR_H) new_y = MENUBAR_H;
        if (new_x + window_width > (int32_t)scr_w)
            new_x = (int32_t)scr_w - window_width;
        if (new_y + window_height > DOCK_BAR_Y)
            new_y = DOCK_BAR_Y - window_height;

        window_x = new_x;
        window_y = new_y;
        trail_active = 1;
    }

    /* Spring-dampened display position: lerp 3/4 toward target each frame */
    int32_t target_x_fp = window_x << 8;
    int32_t target_y_fp = window_y << 8;
    window_disp_x_fp += (target_x_fp - window_disp_x_fp) * 3 / 4;
    window_disp_y_fp += (target_y_fp - window_disp_y_fp) * 3 / 4;

    /* Snap when close enough */
    int32_t dx = target_x_fp - window_disp_x_fp;
    int32_t dy = target_y_fp - window_disp_y_fp;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < 128 && dy < 128) {
        window_disp_x_fp = target_x_fp;
        window_disp_y_fp = target_y_fp;
    }

    /* Shift trail history */
    if (trail_active && is_dragging) {
        for (int i = TRAIL_LEN - 1; i > 0; i--) {
            trail_x[i] = trail_x[i - 1];
            trail_y[i] = trail_y[i - 1];
        }
        trail_x[0] = window_disp_x_fp >> 8;
        trail_y[0] = window_disp_y_fp >> 8;
        if (trail_count < TRAIL_LEN) trail_count++;
    } else {
        trail_count = 0;
    }
}

static void term_init(void) {
    for (int i = 0; i < TERM_BUF_SZ; i++) term_buf[i] = '\0';
    term_cx = 0;
    term_cy = 0;
    term_inited = 1;
}

static void term_scroll(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++)
        for (int c = 0; c < TERM_COLS; c++)
            term_buf[r * TERM_COLS + c] = term_buf[(r + 1) * TERM_COLS + c];
    for (int c = 0; c < TERM_COLS; c++)
        term_buf[(TERM_ROWS - 1) * TERM_COLS + c] = '\0';
    term_cy = TERM_ROWS - 1;
}

static void term_putc(char ch) {
    if (!term_inited) term_init();

    if (ch == '\n') {
        term_cx = 0;
        term_cy++;
        if (term_cy >= TERM_ROWS) term_scroll();
        return;
    }
    if (ch == '\b') {
        if (term_cx > 0) {
            term_cx--;
            term_buf[term_cy * TERM_COLS + term_cx] = '\0';
        }
        return;
    }
    if (ch == '\t') {
        int spaces = 4 - (term_cx % 4);
        for (int i = 0; i < spaces && term_cx < TERM_COLS; i++)
            term_putc(' ');
        return;
    }

    if (term_cx >= TERM_COLS) {
        term_cx = 0;
        term_cy++;
        if (term_cy >= TERM_ROWS) term_scroll();
    }
    term_buf[term_cy * TERM_COLS + term_cx] = ch;
    term_cx++;
}

static void term_print(const char *s) {
    while (*s) term_putc(*s++);
}

static void term_prompt(void) {
    term_print("> ");
}

/* Tarfs ls callback: print each filename into terminal */
static void term_ls_callback(const char *name, uint64_t size) {
    (void)size;
    term_print("  ");
    term_print(name);
    term_print("\n");
}

static void tarfs_list_to_terminal(void) {
    if (!tarfs_base) {
        term_print("  (no ramdisk loaded)\n");
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
            if (name[0] == '.' && name[1] == '/')
                name += 2;
            if (name[0] != '\0')
                term_ls_callback(name, file_size);
        }

        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }
}

/* ===== SYSCALL GATEWAY (int 0x80) ===== */

/*
 * Syscall ABI (matches apps/socrates.h wrapper convention):
 *   RAX = syscall number
 *   RDI = arg0, RSI = arg1, RDX = arg2
 *
 * Syscall 1 (sys_print):      RDI = pointer to null-terminated string
 * Syscall 2 (sys_draw_pixel): RDI = x, RSI = y, RDX = color (0xRRGGBB)
 * Syscall 3 (sys_get_mouse):  RDI = pointer to int32_t[4] output buffer
 *                              [0]=x, [1]=y, [2]=buttons, [3]=reserved
 */

#define APP_CANVAS_W (TERM_WIN_W - 2 * TERM_BORDER)
#define APP_CANVAS_H (TERM_WIN_H - TERM_BORDER - TERM_TITLE_H - TERM_BORDER)

static uint32_t app_canvas[APP_CANVAS_W * APP_CANVAS_H];
static int app_canvas_active = 0;
static int silent_launch = 0;

__attribute__((noinline, used))
void syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    switch (num) {
    case 1: {
        const char *str = (const char *)(uintptr_t)a0;
        if (str && !silent_launch) term_print(str);
        break;
    }
    case 2: {
        int32_t px = (int32_t)a0;
        int32_t py = (int32_t)a1;
        uint32_t color = (uint32_t)a2;
        if (px >= 0 && px < APP_CANVAS_W &&
            py >= 0 && py < APP_CANVAS_H) {
            app_canvas[py * APP_CANVAS_W + px] = color;
        }
        break;
    }
    case 3: {
        int32_t *out = (int32_t *)(uintptr_t)a0;
        if (out) {
            out[0] = mouse_x;
            out[1] = mouse_y;
            out[2] = (int32_t)mouse_buttons;
            out[3] = 0;
        }
        break;
    }
    default:
        break;
    }
}

/* ===== ELF64 BINARY LOADER ===== */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define ELF_PT_LOAD 1

#define APP_MEM_SIZE (256 * 1024)
static uint8_t app_memory[APP_MEM_SIZE] __attribute__((aligned(4096)));
static uint8_t app_stack[8192] __attribute__((aligned(16)));

/* App window state — independent of terminal */
static int     app_window_open   = 0;
static int32_t app_win_x         = -1;
static int32_t app_win_y         = -1;
static int32_t app_win_w         = TERM_WIN_W;
static int32_t app_win_h         = TERM_WIN_H;
static char    app_win_title[64];
static int     app_win_dragging  = 0;
static int32_t app_win_drag_ox   = 0;
static int32_t app_win_drag_oy   = 0;
static uint8_t app_win_prev_lmb  = 0;
static int32_t app_win_disp_x_fp = -1;
static int32_t app_win_disp_y_fp = -1;

static void app_window_init_pos(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    app_win_w = TERM_WIN_W;
    app_win_h = TERM_WIN_H;
    app_win_x = ((int32_t)scr_w - app_win_w) / 2;
    app_win_y = ((DOCK_BAR_Y - MENUBAR_H - app_win_h) / 2) + MENUBAR_H;
    if (app_win_x < 0) app_win_x = 0;
    if (app_win_y < MENUBAR_H) app_win_y = MENUBAR_H;
    app_win_disp_x_fp = app_win_x << 8;
    app_win_disp_y_fp = app_win_y << 8;
}

static int execute_bin_internal(const char *filepath, int verbose) {
    uint64_t fsize = 0;
    const void *fdata = fs_read_file(filepath, &fsize);
    if (!fdata || fsize < sizeof(Elf64_Ehdr)) {
        if (verbose) {
            term_print("Error: file not found: ");
            term_print(filepath);
            term_print("\n");
        }
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)fdata;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        if (verbose) term_print("Error: invalid ELF magic\n");
        return -1;
    }

    if (ehdr->e_ident[4] != 2) {
        if (verbose) term_print("Error: not a 64-bit ELF\n");
        return -1;
    }

    const uint8_t *file_bytes = (const uint8_t *)fdata;

    uint64_t base_vaddr = ~(uint64_t)0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file_bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type == ELF_PT_LOAD && ph->p_vaddr < base_vaddr)
            base_vaddr = ph->p_vaddr;
    }

    if (base_vaddr == ~(uint64_t)0) {
        if (verbose) term_print("Error: no loadable segments\n");
        return -1;
    }

    for (uint32_t i = 0; i < APP_MEM_SIZE; i++)
        app_memory[i] = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file_bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD) continue;

        uint64_t offset = ph->p_vaddr - base_vaddr;
        if (offset + ph->p_memsz > APP_MEM_SIZE) {
            if (verbose) term_print("Error: segment too large\n");
            return -1;
        }

        const uint8_t *src = file_bytes + ph->p_offset;
        uint8_t *dst = app_memory + offset;
        for (uint64_t j = 0; j < ph->p_filesz; j++)
            dst[j] = src[j];
    }

    uint64_t entry_offset = ehdr->e_entry - base_vaddr;
    uint64_t entry_addr = (uint64_t)(uintptr_t)(app_memory + entry_offset);
    uint64_t stack_top  = (uint64_t)(uintptr_t)(app_stack + sizeof(app_stack));

    if (verbose) {
        term_print("Loading ELF64: ");
        term_print(filepath);
        term_print("\n");
    }

    for (int i = 0; i < APP_CANVAS_W * APP_CANVAS_H; i++)
        app_canvas[i] = 0;
    app_canvas_active = 1;

    uint64_t saved_rsp;
    __asm__ volatile(
        "mov %%rsp, %[save]\n\t"
        "mov %[stk], %%rsp\n\t"
        "call *%[entry]\n\t"
        "mov %[save], %%rsp\n\t"
        : [save] "=&r"(saved_rsp)
        : [stk] "r"(stack_top),
          [entry] "r"(entry_addr)
        : "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11",
          "rax", "memory", "cc"
    );

    return 0;
}

static int execute_bin(const char *filepath) {
    return execute_bin_internal(filepath, 1);
}

static void term_exec(void) {
    term_cmd_buf[term_cmd_len] = '\0';

    if (str_eq(term_cmd_buf, "help")) {
        term_print("Socrates BSD 9 OS Active Commands:\n");
        term_print("  help, clear, about, ls, cat <f>, run <p>, files\n");
    } else if (str_eq(term_cmd_buf, "clear")) {
        for (int i = 0; i < TERM_BUF_SZ; i++) term_buf[i] = '\0';
        term_cx = 0;
        term_cy = 0;
        app_canvas_active = 0;
    } else if (str_eq(term_cmd_buf, "about")) {
        term_print("Socrates BSD 9 Operating System\n");
        term_print("Custom TrueType Rasterizer Engine\n");
        term_print("Hardware Abstraction Layer Active\n");
    } else if (str_eq(term_cmd_buf, "ls")) {
        term_print("Ramdisk contents:\n");
        tarfs_list_to_terminal();
    } else if (str_starts_with(term_cmd_buf, "cat ")) {
        const char *fname = term_cmd_buf + 4;
        /* Skip leading whitespace */
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            term_print("Usage: cat <filename>\n");
        } else {
            uint64_t fsize = 0;
            const void *data = fs_read_file(fname, &fsize);
            if (!data) {
                term_print("File not found: ");
                term_print(fname);
                term_print("\n");
            } else {
                const char *text = (const char *)data;
                for (uint64_t i = 0; i < fsize; i++) {
                    if (text[i] == '\0') break;
                    term_putc(text[i]);
                }
                if (fsize > 0 && text[fsize - 1] != '\n')
                    term_putc('\n');
            }
        }
    } else if (str_eq(term_cmd_buf, "files")) {
        explorer_open_window(explorer_scr_w, explorer_scr_h);
    } else if (str_starts_with(term_cmd_buf, "run ")) {
        const char *prog = term_cmd_buf + 4;
        while (*prog == ' ') prog++;
        if (*prog == '\0') {
            term_print("Usage: run <program>\n");
        } else {
            execute_bin(prog);
        }
    } else if (term_cmd_len > 0) {
        term_print("Unknown command: ");
        term_print(term_cmd_buf);
        term_print("\n");
    }
}

static void term_input(char ch) {
    if (!term_ready) {
        term_init();
        term_print("Socrates Terminal v1.0\n");
        term_prompt();
        term_ready = 1;
    }

    if (ch == '\n') {
        term_putc('\n');
        if (term_cmd_len > 0)
            term_exec();
        term_cmd_len = 0;
        term_cmd_buf[0] = '\0';
        term_prompt();
    } else if (ch == '\b') {
        if (term_cmd_len > 0) {
            term_cmd_len--;
            term_cmd_buf[term_cmd_len] = '\0';
            term_putc('\b');
        }
    } else if (ch >= 0x20 && ch < 0x7F) {
        if (term_cmd_len < TERM_CMD_MAX - 1) {
            term_cmd_buf[term_cmd_len++] = ch;
            term_cmd_buf[term_cmd_len] = '\0';
            term_putc(ch);
        }
    }
}

/* ===== DOCK LAUNCH CONTROLLER ===== */

static void dock_launch_app_from_icon(const char *app_name, int icon_idx);

static uint8_t dock_prev_lmb = 0;

static void desktop_handle_dock_click(int32_t mx, int32_t my,
                                       uint8_t lmb, uint32_t scr_w) {
    int click = (lmb && !dock_prev_lmb);
    dock_prev_lmb = lmb;
    if (!click) return;

    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(scr_w, i, &ix, &iy, &iw, &ih);
        if (mx >= ix && mx < ix + iw && my >= iy && my < iy + ih) {
            dock_launch_app_from_icon(dock_apps[i].name, i);
            return;
        }
    }
}

/* Draw a glass trail ghost at a given position with alpha blend (0-255) */
static void draw_trail_ghost(uint32_t *buf, uint32_t w, uint32_t h,
                             int32_t gx, int32_t gy,
                             int32_t gw, int32_t gh, int alpha) {
    uint32_t border_color = 0xD4AF37u;
    for (int32_t row = gy; row < gy + gh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = gx; col < gx + gw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == gy || row == gy + gh - 1 ||
                             col == gx || col == gx + gw - 1);
            if (!is_border) continue;
            uint32_t idx = (uint32_t)row * w + (uint32_t)col;
            uint32_t bg = buf[idx];
            uint32_t br = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >> 8) & 0xFF;
            uint32_t bb = bg & 0xFF;
            uint32_t fr = (border_color >> 16) & 0xFF;
            uint32_t fg = (border_color >> 8) & 0xFF;
            uint32_t fb = border_color & 0xFF;
            uint32_t r = (fr * (uint32_t)alpha + br * (255u - (uint32_t)alpha)) / 255u;
            uint32_t g = (fg * (uint32_t)alpha + bg_g * (255u - (uint32_t)alpha)) / 255u;
            uint32_t b = (fb * (uint32_t)alpha + bb * (255u - (uint32_t)alpha)) / 255u;
            buf[idx] = (r << 16) | (g << 8) | b;
        }
    }
}

static void desktop_draw_terminal(uint32_t *buf, uint32_t w, uint32_t h,
                                  uint32_t tick) {
    if (!is_terminal_open) return;
    if (!term_ready) return;

    /* Initialize window position on first draw */
    if (window_x < 0) window_init_pos(w, h);

    /* Open animation: scale from menubar origin to full size over 15 frames */
    if (open_anim_active) {
        open_anim_tick++;
        int t = open_anim_tick;
        if (t > OPEN_ANIM_FRAMES) t = OPEN_ANIM_FRAMES;

        /* Source: center of Tools menu in menubar */
        int32_t src_x = TOOLS_MENU_X + TOOLS_MENU_W / 2;
        int32_t src_y = MENUBAR_H / 2;
        int32_t src_w = 4;
        int32_t src_h = 4;

        /* Lerp from source to target */
        int32_t cur_x = src_x + (window_x - src_x) * t / OPEN_ANIM_FRAMES;
        int32_t cur_y = src_y + (window_y - src_y) * t / OPEN_ANIM_FRAMES;
        int32_t cur_w = src_w + (window_width  - src_w) * t / OPEN_ANIM_FRAMES;
        int32_t cur_h = src_h + (window_height - src_h) * t / OPEN_ANIM_FRAMES;

        /* Draw the growing outline */
        for (int32_t row = cur_y; row < cur_y + cur_h && row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = cur_x; col < cur_x + cur_w && col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_border = (row == cur_y || row == cur_y + cur_h - 1 ||
                                 col == cur_x || col == cur_x + cur_w - 1);
                int is_titlebar = (row > cur_y && row < cur_y + TERM_TITLE_H + 1 &&
                                   col > cur_x && col < cur_x + cur_w - 1);
                if (is_border || is_titlebar) {
                    uint32_t idx = (uint32_t)row * w + (uint32_t)col;
                    uint32_t bg_px = buf[idx];
                    uint32_t alpha = (uint32_t)t * 255u / OPEN_ANIM_FRAMES;
                    uint32_t br = (bg_px >> 16) & 0xFF;
                    uint32_t bg_g = (bg_px >> 8) & 0xFF;
                    uint32_t bb = bg_px & 0xFF;
                    uint32_t r = (0xD4u * alpha + br * (255u - alpha)) / 255u;
                    uint32_t g = (0xAFu * alpha + bg_g * (255u - alpha)) / 255u;
                    uint32_t b = (0x37u * alpha + bb * (255u - alpha)) / 255u;
                    buf[idx] = (r << 16) | (g << 8) | b;
                }
            }
        }

        if (open_anim_tick >= OPEN_ANIM_FRAMES)
            open_anim_active = 0;
        return;
    }

    /* Display coordinates from spring physics */
    int32_t wx = window_disp_x_fp >> 8;
    int32_t wy = window_disp_y_fp >> 8;
    int32_t ww = window_width;
    int32_t wh = window_height;

    /* Draw liquid glass trails behind the window */
    if (trail_active && trail_count > 0) {
        for (int i = trail_count - 1; i >= 0; i--) {
            int alpha = 50 - i * (50 / TRAIL_LEN);
            if (alpha < 8) alpha = 8;
            draw_trail_ghost(buf, w, h, trail_x[i], trail_y[i], ww, wh, alpha);
        }
    }

    /* Gold outer border (1px) */
    for (int32_t row = wy; row < wy + wh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx; col < wx + ww && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == wy || row == wy + wh - 1 ||
                             col == wx || col == wx + ww - 1);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title bar: 20px solid gold below the top border */
    int32_t tb_y = wy + TERM_BORDER;
    for (int32_t row = tb_y; row < tb_y + TERM_TITLE_H && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + TERM_BORDER; col < wx + ww - TERM_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title text in black */
    int title_fs = 14;
    const char *title = "Socrates Terminal v1.0";
    int tlen = 0;
    const char *tp = title;
    while (*tp++) tlen++;
    int title_tw = tlen * (title_fs * 6 / 10 + title_fs / 8);
    int title_tx = wx + (ww - title_tw) / 2;
    int title_ty = tb_y + (TERM_TITLE_H - title_fs) / 2;
    ttf_draw_string(buf, (int)w, (int)h, title_tx, title_ty,
                    title, 0x000000u, title_fs);

    /* Close "X" button */
    draw_close_button(buf, w, h, wx, wy, ww);

    /* Interior canvas: true black */
    int32_t ix = wx + TERM_BORDER;
    int32_t iy = tb_y + TERM_TITLE_H;
    int32_t iw = ww - 2 * TERM_BORDER;
    int32_t ih = wh - TERM_BORDER - TERM_TITLE_H - TERM_BORDER;
    for (int32_t row = iy; row < iy + ih && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = ix; col < ix + iw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0x000000u;
        }
    }

    /* Render terminal text lines */
    char linebuf[TERM_COLS + 1];
    for (int r = 0; r < TERM_ROWS; r++) {
        int len = 0;
        for (int c = 0; c < TERM_COLS; c++) {
            char ch = term_buf[r * TERM_COLS + c];
            if (ch == '\0') break;
            linebuf[len++] = ch;
        }
        if (len == 0) continue;
        linebuf[len] = '\0';
        int lx = ix + TERM_PAD_X;
        int ly = iy + TERM_PAD_Y + r * TERM_LINE_H;
        if (ly + TERM_FONT_SZ > iy + ih) break;
        ttf_draw_string(buf, (int)w, (int)h, lx, ly,
                        linebuf, 0xD4AF37u, TERM_FONT_SZ);
    }

    /* Composite app canvas overlay (only when app was launched from terminal) */
    if (app_canvas_active && !app_window_open) {
        for (int32_t cy = 0; cy < APP_CANVAS_H && (iy + cy) < (int32_t)h; cy++) {
            if (iy + cy < 0) continue;
            for (int32_t cx = 0; cx < APP_CANVAS_W && (ix + cx) < (int32_t)w; cx++) {
                if (ix + cx < 0) continue;
                uint32_t pix = app_canvas[cy * APP_CANVAS_W + cx];
                if (pix != 0x000000u)
                    buf[(uint32_t)(iy + cy) * w + (uint32_t)(ix + cx)] = pix;
            }
        }
    }

    /* Blinking underscore cursor */
    int caret_on = ((tick / 30) & 1) == 0;
    if (caret_on) {
        int cur_x = ix + TERM_PAD_X + term_cx * (TERM_FONT_SZ * 6 / 10);
        int cur_y = iy + TERM_PAD_Y + term_cy * TERM_LINE_H + TERM_FONT_SZ;
        int cur_w = TERM_FONT_SZ * 6 / 10;
        for (int rr = 0; rr < 2; rr++)
            for (int cc = 0; cc < cur_w; cc++) {
                int px = cur_x + cc;
                int py = cur_y + rr;
                if (px >= ix && px < ix + iw && py >= iy && py < iy + ih &&
                    px < (int)w && py < (int)h)
                    buf[(uint32_t)py * w + (uint32_t)px] = 0xD4AF37u;
            }
    }
}

/* ===== GEAR ICON FOR SETTINGS ===== */

static void draw_gear_icon(uint32_t *buf, uint32_t bw, uint32_t bh,
                           int32_t ox, int32_t oy) {
    uint32_t G = 0xD4AF37u;
    int32_t cx = ox + DOCK_ICON_SZ / 2;
    int32_t cy = oy + DOCK_ICON_SZ / 2;

    for (int32_t dy = -12; dy <= 12; dy++) {
        for (int32_t dx = -12; dx <= 12; dx++) {
            int32_t px = cx + dx;
            int32_t py = cy + dy;
            if (px < 0 || px >= (int32_t)bw || py < 0 || py >= (int32_t)bh)
                continue;

            int32_t d2 = dx * dx + dy * dy;
            int32_t adx = dx < 0 ? -dx : dx;
            int32_t ady = dy < 0 ? -dy : dy;

            if (d2 <= 9) continue;

            if (d2 <= 49) {
                buf[(uint32_t)py * bw + (uint32_t)px] = G;
                continue;
            }

            if (d2 <= 121) {
                int is_tooth = 0;
                if (adx <= 2) is_tooth = 1;
                if (ady <= 2) is_tooth = 1;
                int diff = adx - ady;
                if (diff < 0) diff = -diff;
                if (diff <= 2 && adx > 1 && ady > 1) is_tooth = 1;
                if (is_tooth)
                    buf[(uint32_t)py * bw + (uint32_t)px] = G;
            }
        }
    }
}

/* ===== DESKTOP DOCK ===== */

#define DOCK_RADIUS 12

static int dock_inside_rrect(int32_t px, int32_t py,
                             int32_t rx, int32_t ry,
                             int32_t rw, int32_t rh, int32_t rad) {
    if (px < rx || px >= rx + rw || py < ry || py >= ry + rh)
        return 0;
    int32_t dx = 0, dy = 0;
    if (px < rx + rad && py < ry + rad) {
        dx = rx + rad - px; dy = ry + rad - py;
    } else if (px >= rx + rw - rad && py < ry + rad) {
        dx = px - (rx + rw - rad - 1); dy = ry + rad - py;
    } else if (px < rx + rad && py >= ry + rh - rad) {
        dx = rx + rad - px; dy = py - (ry + rh - rad - 1);
    } else if (px >= rx + rw - rad && py >= ry + rh - rad) {
        dx = px - (rx + rw - rad - 1); dy = py - (ry + rh - rad - 1);
    }
    return (dx * dx + dy * dy) <= (rad * rad);
}

static int dock_on_border_rrect(int32_t px, int32_t py,
                                int32_t rx, int32_t ry,
                                int32_t rw, int32_t rh, int32_t rad) {
    if (!dock_inside_rrect(px, py, rx, ry, rw, rh, rad))
        return 0;
    if (!dock_inside_rrect(px, py, rx + 1, ry + 1, rw - 2, rh - 2, rad - 1))
        return 1;
    return 0;
}

static void desktop_draw_dock(uint32_t *buf, uint32_t w, uint32_t h) {
    int32_t bw = dock_cfg.bar_w;
    int32_t bh = dock_cfg.bar_h;
    int32_t by = dock_cfg.bar_y;
    int32_t isz = dock_cfg.icon_sz;

    int32_t dock_rx, dock_ry, dock_rw, dock_rh;
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        dock_rx = ((int32_t)w - bw) / 2;
        dock_ry = by;
        dock_rw = bw;
        dock_rh = bh;
    } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
        dock_rx = 2;
        dock_ry = by;
        dock_rw = bh;
        dock_rh = bw;
    } else {
        dock_rx = (int32_t)w - bh - 2;
        dock_ry = by;
        dock_rw = bh;
        dock_rh = bw;
    }

    for (int32_t row = dock_ry; row < dock_ry + dock_rh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = dock_rx; col < dock_rx + dock_rw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            if (!dock_inside_rrect(col, row, dock_rx, dock_ry,
                                   dock_rw, dock_rh, DOCK_RADIUS))
                continue;
            int is_border = dock_on_border_rrect(col, row, dock_rx, dock_ry,
                                                 dock_rw, dock_rh, DOCK_RADIUS);
            buf[row * (int32_t)w + col] = is_border ? 0xD4AF37u : 0x000000u;
        }
    }

    /* Draw each app icon box with its identifier letter */
    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, i, &ix, &iy, &iw, &ih);

        /* Metallic Gold outlined icon box */
        for (int32_t row = iy; row < iy + ih && row < (int32_t)h; row++)
            for (int32_t col = ix; col < ix + iw && col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_border = (row == iy || row == iy + ih - 1 ||
                                 col == ix || col == ix + iw - 1);
                if (is_border)
                    buf[row * (int32_t)w + col] = 0xD4AF37u;
            }

        /* Icon: gear shape for settings, letter for everything else */
        if (dock_apps[i].icon_letter == '*') {
            draw_gear_icon(buf, w, h, ix, iy);
        } else {
            int fs = isz > 24 ? 16 : 12;
            char letter[2] = { dock_apps[i].icon_letter, '\0' };
            int lw = fs * 6 / 10;
            int lx = ix + (iw - lw) / 2;
            int ly = iy + (ih - fs) / 2;
            ttf_draw_string(buf, (int)w, (int)h, lx, ly,
                            letter, 0xD4AF37u, fs);
        }
    }
}

/* ===== APP WINDOW (standalone, terminal-independent) ===== */

static uint8_t app_close_prev_lmb = 0;

static void app_window_handle_drag(int32_t mx, int32_t my, uint8_t lmb,
                                    uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    if (!app_window_open) return;
    int click   = (lmb && !app_win_prev_lmb);
    int release = (!lmb && app_win_prev_lmb);
    app_win_prev_lmb = lmb;

    /* Close button check */
    int app_close_click = (lmb && !app_close_prev_lmb);
    app_close_prev_lmb = lmb;
    if (app_close_click && hit_close_button(mx, my, app_win_x, app_win_y, app_win_w)) {
        app_window_open = 0;
        app_canvas_active = 0;
        app_win_dragging = 0;
        app_win_x = -1;
        return;
    }

    if (click && !app_win_dragging) {
        if (mx >= app_win_x && mx < app_win_x + app_win_w &&
            my >= app_win_y && my < app_win_y + TERM_TITLE_H + TERM_BORDER) {
            app_win_dragging = 1;
            app_win_drag_ox = mx - app_win_x;
            app_win_drag_oy = my - app_win_y;
        }
    }
    if (release) app_win_dragging = 0;

    if (app_win_dragging) {
        int32_t nx = mx - app_win_drag_ox;
        int32_t ny = my - app_win_drag_oy;
        if (nx < 0) nx = 0;
        if (ny < MENUBAR_H) ny = MENUBAR_H;
        if (nx + app_win_w > (int32_t)scr_w) nx = (int32_t)scr_w - app_win_w;
        if (ny + app_win_h > DOCK_BAR_Y) ny = DOCK_BAR_Y - app_win_h;
        app_win_x = nx;
        app_win_y = ny;
    }

    int32_t tx = app_win_x << 8;
    int32_t ty = app_win_y << 8;
    app_win_disp_x_fp += (tx - app_win_disp_x_fp) * 3 / 4;
    app_win_disp_y_fp += (ty - app_win_disp_y_fp) * 3 / 4;
    int32_t ddx = tx - app_win_disp_x_fp; if (ddx < 0) ddx = -ddx;
    int32_t ddy = ty - app_win_disp_y_fp; if (ddy < 0) ddy = -ddy;
    if (ddx < 128 && ddy < 128) {
        app_win_disp_x_fp = tx;
        app_win_disp_y_fp = ty;
    }
}

static void desktop_draw_app_window(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!app_window_open || !app_canvas_active) return;
    if (spawn_anim.active) return;
    if (app_win_x < 0) app_window_init_pos(w, h);

    int32_t wx = app_win_disp_x_fp >> 8;
    int32_t wy = app_win_disp_y_fp >> 8;
    int32_t ww = app_win_w;
    int32_t wh = app_win_h;

    /* Gold border */
    for (int32_t row = wy; row < wy + wh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx; col < wx + ww && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == wy || row == wy + wh - 1 ||
                             col == wx || col == wx + ww - 1);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title bar */
    int32_t tb_y = wy + TERM_BORDER;
    for (int32_t row = tb_y; row < tb_y + TERM_TITLE_H && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + TERM_BORDER; col < wx + ww - TERM_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title text */
    {
    int title_fs = 14;
    int tlen2 = 0;
    const char *tp2 = app_win_title;
    while (*tp2++) tlen2++;
    int title_tw = tlen2 * (title_fs * 6 / 10 + title_fs / 8);
    int title_tx = wx + (ww - title_tw) / 2;
    int title_ty = tb_y + (TERM_TITLE_H - title_fs) / 2;
    ttf_draw_string(buf, (int)w, (int)h, title_tx, title_ty,
                    app_win_title, 0x000000u, title_fs);
    }

    /* Close "X" button */
    draw_close_button(buf, w, h, wx, wy, ww);

    /* Black interior canvas */
    int32_t ix = wx + TERM_BORDER;
    int32_t iy = tb_y + TERM_TITLE_H;
    int32_t iw = ww - 2 * TERM_BORDER;
    int32_t ih = wh - TERM_BORDER - TERM_TITLE_H - TERM_BORDER;
    for (int32_t row = iy; row < iy + ih && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = ix; col < ix + iw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0x000000u;
        }
    }

    /* Composite app canvas pixels */
    for (int32_t cy = 0; cy < APP_CANVAS_H && (iy + cy) < (int32_t)h; cy++) {
        if (iy + cy < 0) continue;
        for (int32_t cx = 0; cx < APP_CANVAS_W && (ix + cx) < (int32_t)w; cx++) {
            if (ix + cx < 0) continue;
            uint32_t pix = app_canvas[cy * APP_CANVAS_W + cx];
            if (pix != 0x000000u)
                buf[(uint32_t)(iy + cy) * w + (uint32_t)(ix + cx)] = pix;
        }
    }
}

/* ===== FILE EXPLORER APPLICATION ===== */

#define EXPLORER_WIN_W  560
#define EXPLORER_WIN_H  440
#define EXPLORER_TITLE_H TERM_TITLE_H
#define EXPLORER_PATH_H  22
#define EXPLORER_BORDER   1

#define EXPLORER_ICON_W   64
#define EXPLORER_ICON_H   72
#define EXPLORER_GRID_PAD 12
#define EXPLORER_MAX_ENTRIES 64
#define EXPLORER_PATH_MAX   256

typedef struct {
    char name[100];
    uint64_t size;
    int is_dir;
} explorer_entry_t;

static int32_t  exp_win_x = -1;
static int32_t  exp_win_y = -1;
static int32_t  exp_win_w = EXPLORER_WIN_W;
static int32_t  exp_win_h = EXPLORER_WIN_H;
static int32_t  exp_win_disp_x_fp = -1;
static int32_t  exp_win_disp_y_fp = -1;
static int      exp_win_dragging = 0;
static int32_t  exp_win_drag_ox = 0;
static int32_t  exp_win_drag_oy = 0;
static uint8_t  exp_win_prev_lmb = 0;
static uint8_t  exp_close_prev_lmb = 0;

static char     exp_current_path[EXPLORER_PATH_MAX];
static explorer_entry_t exp_entries[EXPLORER_MAX_ENTRIES];
static int      exp_entry_count = 0;
static int      exp_path_inited = 0;

static uint8_t  exp_dblclick_prev_lmb = 0;
static uint32_t exp_last_click_tick = 0;
static int32_t  exp_last_click_idx = -1;
#define EXPLORER_DBLCLICK_TICKS 20

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, int max) {
    int len = str_len(dst);
    int i = 0;
    while (src[i] && len < max - 1) { dst[len++] = src[i++]; }
    dst[len] = '\0';
}

static void explorer_scan_directory(void) {
    exp_entry_count = 0;
    if (!tarfs_base) return;

    const char *dir = exp_current_path;
    int dlen = str_len(dir);
    int has_slash_prefix = (dlen > 0 && dir[0] == '/');
    const char *dir_norm = has_slash_prefix ? dir + 1 : dir;
    int dir_norm_len = str_len(dir_norm);
    int is_root = (dir_norm_len == 0);

    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;

    char seen_dirs[EXPLORER_MAX_ENTRIES][100];
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
            for (int i = 0; rel[i]; i++) {
                if (rel[i] == '/') { slash_pos = i; break; }
            }

            if (slash_pos >= 0) {
                char dir_name[100];
                for (int i = 0; i < slash_pos && i < 99; i++) dir_name[i] = rel[i];
                dir_name[slash_pos < 99 ? slash_pos : 99] = '\0';

                int already = 0;
                for (int d = 0; d < seen_dir_count; d++) {
                    if (str_eq(seen_dirs[d], dir_name)) { already = 1; break; }
                }
                if (!already && seen_dir_count < EXPLORER_MAX_ENTRIES &&
                    exp_entry_count < EXPLORER_MAX_ENTRIES) {
                    str_copy(seen_dirs[seen_dir_count], dir_name, 100);
                    seen_dir_count++;
                    explorer_entry_t *e = &exp_entries[exp_entry_count++];
                    str_copy(e->name, dir_name, 100);
                    e->size = 0;
                    e->is_dir = 1;
                }
            } else {
                int trailing_slash = 0;
                int rlen = str_len(rel);
                if (rlen > 0 && rel[rlen - 1] == '/') trailing_slash = 1;

                if (!trailing_slash && exp_entry_count < EXPLORER_MAX_ENTRIES) {
                    explorer_entry_t *e = &exp_entries[exp_entry_count++];
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

static void explorer_open_window(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    explorer_open = 1;
    exp_win_w = EXPLORER_WIN_W;
    exp_win_h = EXPLORER_WIN_H;
    exp_win_x = ((int32_t)scr_w - exp_win_w) / 2 + 40;
    exp_win_y = MENUBAR_H + 30;
    if (exp_win_x < 0) exp_win_x = 0;
    if (exp_win_y < MENUBAR_H) exp_win_y = MENUBAR_H;
    exp_win_disp_x_fp = exp_win_x << 8;
    exp_win_disp_y_fp = exp_win_y << 8;
    exp_win_dragging = 0;

    if (!exp_path_inited) {
        str_copy(exp_current_path, "/", EXPLORER_PATH_MAX);
        exp_path_inited = 1;
    }
    explorer_scan_directory();
}

static void explorer_navigate_up(void) {
    int len = str_len(exp_current_path);
    if (len <= 1) return;

    if (exp_current_path[len - 1] == '/')
        exp_current_path[--len] = '\0';

    int last_slash = -1;
    for (int i = 0; i < len; i++)
        if (exp_current_path[i] == '/') last_slash = i;

    if (last_slash == 0)
        exp_current_path[1] = '\0';
    else if (last_slash > 0)
        exp_current_path[last_slash] = '\0';

    explorer_scan_directory();
}

static void explorer_navigate_into(const char *dirname) {
    int len = str_len(exp_current_path);
    if (len > 1)
        str_append(exp_current_path, "/", EXPLORER_PATH_MAX);
    str_append(exp_current_path, dirname, EXPLORER_PATH_MAX);
    explorer_scan_directory();
}

static void explorer_icon_rect(int idx, int32_t content_x, int32_t content_y,
                                int32_t content_w,
                                int32_t *ox, int32_t *oy) {
    int cols = (content_w - EXPLORER_GRID_PAD) /
               (EXPLORER_ICON_W + EXPLORER_GRID_PAD);
    if (cols < 1) cols = 1;
    int col = idx % cols;
    int row = idx / cols;
    *ox = content_x + EXPLORER_GRID_PAD + col * (EXPLORER_ICON_W + EXPLORER_GRID_PAD);
    *oy = content_y + EXPLORER_GRID_PAD + row * (EXPLORER_ICON_H + EXPLORER_GRID_PAD);
}

static void draw_folder_icon(uint32_t *buf, uint32_t bw, uint32_t bh,
                             int32_t cx, int32_t cy) {
    uint32_t G = 0xD4AF37u;
    /* Folder tab */
    for (int32_t r = cy; r < cy + 4 && r < (int32_t)bh; r++) {
        if (r < 0) continue;
        for (int32_t c = cx; c < cx + 16 && c < (int32_t)bw; c++) {
            if (c < 0) continue;
            buf[(uint32_t)r * bw + (uint32_t)c] = G;
        }
    }
    /* Folder body */
    for (int32_t r = cy + 4; r < cy + 28 && r < (int32_t)bh; r++) {
        if (r < 0) continue;
        for (int32_t c = cx; c < cx + 36 && c < (int32_t)bw; c++) {
            if (c < 0) continue;
            int is_edge = (r == cy + 4 || r == cy + 27 ||
                           c == cx || c == cx + 35);
            buf[(uint32_t)r * bw + (uint32_t)c] = is_edge ? G : 0x3D2E00u;
        }
    }
}

static void draw_file_icon(uint32_t *buf, uint32_t bw, uint32_t bh,
                           int32_t cx, int32_t cy) {
    uint32_t W = 0xFFFFFFu;
    uint32_t G = 0xD4AF37u;
    for (int32_t r = cy; r < cy + 30 && r < (int32_t)bh; r++) {
        if (r < 0) continue;
        for (int32_t c = cx; c < cx + 24 && c < (int32_t)bw; c++) {
            if (c < 0) continue;
            int is_edge = (r == cy || r == cy + 29 ||
                           c == cx || c == cx + 23);
            buf[(uint32_t)r * bw + (uint32_t)c] = is_edge ? G : W;
        }
    }
    /* Corner fold */
    for (int32_t r = cy; r < cy + 6 && r < (int32_t)bh; r++) {
        if (r < 0) continue;
        int fold_end = cx + 24 - (r - cy);
        for (int32_t c = fold_end; c < cx + 24 && c < (int32_t)bw; c++) {
            if (c < 0) continue;
            buf[(uint32_t)r * bw + (uint32_t)c] = G;
        }
    }
    /* Text lines */
    for (int line = 0; line < 3; line++) {
        int32_t ly = cy + 10 + line * 5;
        int32_t lw = 16 - line * 3;
        for (int32_t c = cx + 4; c < cx + 4 + lw && c < (int32_t)bw; c++) {
            if (c < 0 || ly < 0 || ly >= (int32_t)bh) continue;
            buf[(uint32_t)ly * bw + (uint32_t)c] = 0xAAAAAA;
        }
    }
}

static void explorer_handle_drag(int32_t mx, int32_t my, uint8_t lmb,
                                  uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    if (!explorer_open) return;

    int click   = (lmb && !exp_win_prev_lmb);
    int release = (!lmb && exp_win_prev_lmb);
    exp_win_prev_lmb = lmb;

    int close_click = (lmb && !exp_close_prev_lmb);
    exp_close_prev_lmb = lmb;
    if (close_click && hit_close_button(mx, my, exp_win_x, exp_win_y, exp_win_w)) {
        explorer_open = 0;
        exp_win_dragging = 0;
        exp_win_x = -1;
        return;
    }

    if (click && !exp_win_dragging) {
        if (mx >= exp_win_x && mx < exp_win_x + exp_win_w &&
            my >= exp_win_y && my < exp_win_y + EXPLORER_TITLE_H + EXPLORER_BORDER) {
            exp_win_dragging = 1;
            exp_win_drag_ox = mx - exp_win_x;
            exp_win_drag_oy = my - exp_win_y;
        }
    }
    if (release) exp_win_dragging = 0;

    if (exp_win_dragging) {
        int32_t nx = mx - exp_win_drag_ox;
        int32_t ny = my - exp_win_drag_oy;
        if (nx < 0) nx = 0;
        if (ny < MENUBAR_H) ny = MENUBAR_H;
        if (nx + exp_win_w > (int32_t)scr_w) nx = (int32_t)scr_w - exp_win_w;
        if (ny + exp_win_h > DOCK_BAR_Y) ny = DOCK_BAR_Y - exp_win_h;
        exp_win_x = nx;
        exp_win_y = ny;
    }

    int32_t tx = exp_win_x << 8;
    int32_t ty = exp_win_y << 8;
    exp_win_disp_x_fp += (tx - exp_win_disp_x_fp) * 3 / 4;
    exp_win_disp_y_fp += (ty - exp_win_disp_y_fp) * 3 / 4;
    int32_t ddx = tx - exp_win_disp_x_fp; if (ddx < 0) ddx = -ddx;
    int32_t ddy = ty - exp_win_disp_y_fp; if (ddy < 0) ddy = -ddy;
    if (ddx < 128 && ddy < 128) {
        exp_win_disp_x_fp = tx;
        exp_win_disp_y_fp = ty;
    }
}

static void explorer_handle_click(int32_t mx, int32_t my, uint8_t lmb,
                                   uint32_t tick) {
    if (!explorer_open) return;

    int click = (lmb && !exp_dblclick_prev_lmb);
    exp_dblclick_prev_lmb = lmb;
    if (!click) return;

    int32_t wx = exp_win_disp_x_fp >> 8;
    int32_t wy = exp_win_disp_y_fp >> 8;
    int32_t content_x = wx + EXPLORER_BORDER;
    int32_t content_y = wy + EXPLORER_BORDER + EXPLORER_TITLE_H + EXPLORER_PATH_H;
    int32_t content_w = exp_win_w - 2 * EXPLORER_BORDER;

    /* Check "Up" button in path bar */
    int32_t up_x = wx + exp_win_w - EXPLORER_BORDER - 30;
    int32_t up_y = wy + EXPLORER_BORDER + EXPLORER_TITLE_H + 2;
    if (mx >= up_x && mx < up_x + 26 && my >= up_y && my < up_y + 18) {
        explorer_navigate_up();
        exp_last_click_idx = -1;
        return;
    }

    int hit_idx = -1;
    for (int i = 0; i < exp_entry_count; i++) {
        int32_t ix, iy;
        explorer_icon_rect(i, content_x, content_y, content_w, &ix, &iy);
        if (mx >= ix && mx < ix + EXPLORER_ICON_W &&
            my >= iy && my < iy + EXPLORER_ICON_H) {
            hit_idx = i;
            break;
        }
    }

    if (hit_idx >= 0 && exp_entries[hit_idx].is_dir) {
        if (hit_idx == exp_last_click_idx &&
            (tick - exp_last_click_tick) < EXPLORER_DBLCLICK_TICKS) {
            explorer_navigate_into(exp_entries[hit_idx].name);
            exp_last_click_idx = -1;
            return;
        }
        exp_last_click_idx = hit_idx;
        exp_last_click_tick = tick;
    } else {
        exp_last_click_idx = -1;
    }
}

static void desktop_draw_explorer(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!explorer_open) return;
    if (spawn_anim.active) return;
    if (exp_win_x < 0) return;

    int32_t wx = exp_win_disp_x_fp >> 8;
    int32_t wy = exp_win_disp_y_fp >> 8;
    int32_t ww = exp_win_w;
    int32_t wh = exp_win_h;

    /* Gold border */
    for (int32_t row = wy; row < wy + wh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx; col < wx + ww && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == wy || row == wy + wh - 1 ||
                             col == wx || col == wx + ww - 1);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title bar */
    int32_t tb_y = wy + EXPLORER_BORDER;
    for (int32_t row = tb_y; row < tb_y + EXPLORER_TITLE_H && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + EXPLORER_BORDER;
             col < wx + ww - EXPLORER_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title text */
    {
        int title_fs = 14;
        const char *title = "File Explorer";
        int tlen = 13;
        int title_tw = tlen * (title_fs * 6 / 10 + title_fs / 8);
        int title_tx = wx + (ww - title_tw) / 2;
        int title_ty = tb_y + (EXPLORER_TITLE_H - title_fs) / 2;
        ttf_draw_string(buf, (int)w, (int)h, title_tx, title_ty,
                        title, 0x000000u, title_fs);
    }

    draw_close_button(buf, w, h, wx, wy, ww);

    /* Path bar */
    int32_t pb_y = tb_y + EXPLORER_TITLE_H;
    for (int32_t row = pb_y; row < pb_y + EXPLORER_PATH_H && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + EXPLORER_BORDER;
             col < wx + ww - EXPLORER_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0x1A1508u;
        }
    }

    /* Path text */
    {
        int pfs = 13;
        int px = wx + EXPLORER_BORDER + 6;
        int py = pb_y + (EXPLORER_PATH_H - pfs) / 2;
        ttf_draw_string(buf, (int)w, (int)h, px, py,
                        exp_current_path, 0xD4AF37u, pfs);
    }

    /* Up button */
    {
        int32_t up_x = wx + ww - EXPLORER_BORDER - 30;
        int32_t up_y = pb_y + 2;
        for (int32_t r = up_y; r < up_y + 18 && r < (int32_t)h; r++) {
            if (r < 0) continue;
            for (int32_t c = up_x; c < up_x + 26 && c < (int32_t)w; c++) {
                if (c < 0) continue;
                int is_edge = (r == up_y || r == up_y + 17 ||
                               c == up_x || c == up_x + 25);
                buf[(uint32_t)r * w + (uint32_t)c] = is_edge ? 0xD4AF37u : 0x1A1508u;
            }
        }
        int ufs = 12;
        int ux = up_x + 4;
        int uy = up_y + 3;
        ttf_draw_string(buf, (int)w, (int)h, ux, uy, "Up", 0xD4AF37u, ufs);
    }

    /* Content area: black background */
    int32_t cx = wx + EXPLORER_BORDER;
    int32_t cy = pb_y + EXPLORER_PATH_H;
    int32_t cw = ww - 2 * EXPLORER_BORDER;
    int32_t ch = wh - EXPLORER_BORDER - EXPLORER_TITLE_H -
                 EXPLORER_PATH_H - EXPLORER_BORDER;
    for (int32_t row = cy; row < cy + ch && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = cx; col < cx + cw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0x000000u;
        }
    }

    /* Draw file/folder icons in grid */
    for (int i = 0; i < exp_entry_count; i++) {
        int32_t ix, iy;
        explorer_icon_rect(i, cx, cy, cw, &ix, &iy);

        if (iy + EXPLORER_ICON_H < cy || iy > cy + ch) continue;

        int32_t icon_cx = ix + (EXPLORER_ICON_W - 36) / 2;
        int32_t icon_cy = iy + 4;

        if (exp_entries[i].is_dir)
            draw_folder_icon(buf, w, h, icon_cx, icon_cy);
        else
            draw_file_icon(buf, w, h, icon_cx + 6, icon_cy);

        /* Filename label beneath icon */
        int nfs = 12;
        int nlen = str_len(exp_entries[i].name);
        int max_chars = EXPLORER_ICON_W / (nfs * 6 / 10 + nfs / 8);
        if (nlen > max_chars) nlen = max_chars;
        char label[32];
        for (int c = 0; c < nlen && c < 31; c++) label[c] = exp_entries[i].name[c];
        label[nlen < 31 ? nlen : 31] = '\0';
        int lw = nlen * (nfs * 6 / 10 + nfs / 8);
        int lx = ix + (EXPLORER_ICON_W - lw) / 2;
        int ly = iy + EXPLORER_ICON_H - 16;
        ttf_draw_string(buf, (int)w, (int)h, lx, ly,
                        label, 0xFFFFFFu, nfs);
    }

    /* Empty directory message */
    if (exp_entry_count == 0) {
        int efs = 12;
        const char *empty = "(empty)";
        int elen = 7;
        int etw = elen * (efs * 6 / 10);
        int ex = cx + (cw - etw) / 2;
        int ey = cy + ch / 2 - efs / 2;
        ttf_draw_string(buf, (int)w, (int)h, ex, ey,
                        empty, 0x666666u, efs);
    }
}

/* ===== SETTINGS APPLICATION ===== */

static void settings_open_window(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    settings_open = 1;
    set_win_w = SETTINGS_WIN_W;
    set_win_h = SETTINGS_WIN_H;
    set_win_x = ((int32_t)scr_w - set_win_w) / 2 - 60;
    set_win_y = MENUBAR_H + 50;
    if (set_win_x < 0) set_win_x = 0;
    if (set_win_y < MENUBAR_H) set_win_y = MENUBAR_H;
    set_win_disp_x_fp = set_win_x << 8;
    set_win_disp_y_fp = set_win_y << 8;
    set_win_dragging = 0;
}

static void settings_handle_drag(int32_t mx, int32_t my, uint8_t lmb,
                                  uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    if (!settings_open) return;

    int click   = (lmb && !set_win_prev_lmb);
    int release = (!lmb && set_win_prev_lmb);
    set_win_prev_lmb = lmb;

    int close_click = (lmb && !set_close_prev_lmb);
    set_close_prev_lmb = lmb;
    if (close_click &&
        hit_close_button(mx, my, set_win_x, set_win_y, set_win_w)) {
        settings_open = 0;
        set_win_dragging = 0;
        set_win_x = -1;
        return;
    }

    if (click && !set_win_dragging) {
        if (mx >= set_win_x && mx < set_win_x + set_win_w &&
            my >= set_win_y && my < set_win_y + SETTINGS_TITLE_H +
            SETTINGS_BORDER) {
            set_win_dragging = 1;
            set_win_drag_ox = mx - set_win_x;
            set_win_drag_oy = my - set_win_y;
        }
    }
    if (release) set_win_dragging = 0;

    if (set_win_dragging) {
        int32_t nx = mx - set_win_drag_ox;
        int32_t ny = my - set_win_drag_oy;
        if (nx < 0) nx = 0;
        if (ny < MENUBAR_H) ny = MENUBAR_H;
        if (nx + set_win_w > (int32_t)scr_w)
            nx = (int32_t)scr_w - set_win_w;
        if (ny + set_win_h > DOCK_BAR_Y)
            ny = DOCK_BAR_Y - set_win_h;
        set_win_x = nx;
        set_win_y = ny;
    }

    int32_t tx = set_win_x << 8;
    int32_t ty = set_win_y << 8;
    set_win_disp_x_fp += (tx - set_win_disp_x_fp) * 3 / 4;
    set_win_disp_y_fp += (ty - set_win_disp_y_fp) * 3 / 4;
    int32_t ddx = tx - set_win_disp_x_fp;
    if (ddx < 0) ddx = -ddx;
    int32_t ddy = ty - set_win_disp_y_fp;
    if (ddy < 0) ddy = -ddy;
    if (ddx < 128 && ddy < 128) {
        set_win_disp_x_fp = tx;
        set_win_disp_y_fp = ty;
    }
}

static const char *dock_edge_names[] = { "Bottom", "Left", "Right" };

static void settings_handle_click(int32_t mx, int32_t my, uint8_t lmb) {
    if (!settings_open) return;

    int click = (lmb && !set_btn_prev_lmb);
    set_btn_prev_lmb = lmb;
    if (!click) return;

    int32_t wx = set_win_disp_x_fp >> 8;
    int32_t wy = set_win_disp_y_fp >> 8;
    int32_t btn_x = wx + SETTINGS_BORDER + 20;
    int32_t btn_y = wy + SETTINGS_BORDER + SETTINGS_TITLE_H + 140;

    if (mx >= btn_x && mx < btn_x + 200 &&
        my >= btn_y && my < btn_y + 30) {
        settings_bg_index = (settings_bg_index + 1) % SETTINGS_BG_COUNT;
        desktop_bg_color = settings_bg_colors[settings_bg_index];
    }

    /* Dock position button */
    int32_t dock_btn_y = wy + SETTINGS_BORDER + SETTINGS_TITLE_H + 220;
    if (mx >= btn_x && mx < btn_x + 200 &&
        my >= dock_btn_y && my < dock_btn_y + 30) {
        dock_cfg.edge = (dock_cfg.edge + 1) % 3;
    }

    /* Dock scale buttons: [-] and [+] for icon size */
    int32_t scale_y = wy + SETTINGS_BORDER + SETTINGS_TITLE_H + 260;
    if (mx >= btn_x && mx < btn_x + 40 &&
        my >= scale_y && my < scale_y + 28) {
        if (dock_cfg.icon_sz > 20) {
            dock_cfg.icon_sz -= 4;
            dock_cfg.bar_h -= 4;
            dock_cfg.bar_w -= 28;
        }
    }
    if (mx >= btn_x + 50 && mx < btn_x + 90 &&
        my >= scale_y && my < scale_y + 28) {
        if (dock_cfg.icon_sz < 48) {
            dock_cfg.icon_sz += 4;
            dock_cfg.bar_h += 4;
            dock_cfg.bar_w += 28;
        }
    }
}

static void desktop_draw_settings(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!settings_open) return;
    if (spawn_anim.active) return;
    if (set_win_x < 0) return;

    int32_t wx = set_win_disp_x_fp >> 8;
    int32_t wy = set_win_disp_y_fp >> 8;
    int32_t ww = set_win_w;
    int32_t wh = set_win_h;

    /* Gold border */
    for (int32_t row = wy; row < wy + wh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx; col < wx + ww && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == wy || row == wy + wh - 1 ||
                             col == wx || col == wx + ww - 1);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title bar */
    int32_t tb_y = wy + SETTINGS_BORDER;
    for (int32_t row = tb_y; row < tb_y + SETTINGS_TITLE_H &&
         row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + SETTINGS_BORDER;
             col < wx + ww - SETTINGS_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    {
        int title_fs = 14;
        const char *title = "Settings";
        int tlen = 8;
        int title_tw = tlen * (title_fs * 6 / 10 + title_fs / 8);
        int title_tx = wx + (ww - title_tw) / 2;
        int title_ty = tb_y + (SETTINGS_TITLE_H - title_fs) / 2;
        ttf_draw_string(buf, (int)w, (int)h, title_tx, title_ty,
                        title, 0x000000u, title_fs);
    }

    draw_close_button(buf, w, h, wx, wy, ww);

    /* Interior: True White canvas */
    int32_t ix = wx + SETTINGS_BORDER;
    int32_t iy = tb_y + SETTINGS_TITLE_H;
    int32_t iw = ww - 2 * SETTINGS_BORDER;
    int32_t ih = wh - 2 * SETTINGS_BORDER - SETTINGS_TITLE_H;
    for (int32_t row = iy; row < iy + ih && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = ix; col < ix + iw && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xFFFFFFu;
        }
    }

    /* "System Information" header */
    ttf_draw_string(buf, (int)w, (int)h, ix + 20, iy + 15,
                    "System Information", 0xD4AF37u, 16);

    /* Gold separator line */
    {
        int32_t sep_y = iy + 35;
        if (sep_y >= 0 && sep_y < (int32_t)h) {
            for (int32_t c = ix + 20; c < ix + iw - 20 &&
                 c < (int32_t)w; c++) {
                if (c < 0) continue;
                buf[(uint32_t)sep_y * w + (uint32_t)c] = 0xD4AF37u;
            }
        }
    }

    /* Resolution display */
    {
        char res_str[64];
        char num[12];
        str_copy(res_str, "Resolution: ", 64);
        uint_to_str(w, num);
        str_append(res_str, num, 64);
        str_append(res_str, " x ", 64);
        uint_to_str(h, num);
        str_append(res_str, num, 64);
        ttf_draw_string(buf, (int)w, (int)h, ix + 20, iy + 50,
                        res_str, 0x000000u, 14);
    }

    /* Memory display */
    {
        char mem_str[64];
        char num[12];
        str_copy(mem_str, "System Memory: ", 64);
        uint_to_str((uint32_t)system_total_memory_mb, num);
        str_append(mem_str, num, 64);
        str_append(mem_str, " MB", 64);
        ttf_draw_string(buf, (int)w, (int)h, ix + 20, iy + 75,
                        mem_str, 0x000000u, 14);
    }

    /* Background Color section header */
    ttf_draw_string(buf, (int)w, (int)h, ix + 20, iy + 110,
                    "Background Color", 0xD4AF37u, 14);

    /* Toggle button */
    {
        int32_t btn_x = ix + 20;
        int32_t btn_y = iy + 140;
        int32_t btn_w = 200;
        int32_t btn_h = 30;
        for (int32_t row = btn_y; row < btn_y + btn_h &&
             row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = btn_x; col < btn_x + btn_w &&
                 col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == btn_y || row == btn_y + btn_h - 1 ||
                               col == btn_x || col == btn_x + btn_w - 1);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : 0x1A1508u;
            }
        }
        ttf_draw_string(buf, (int)w, (int)h, btn_x + 12, btn_y + 8,
                        "Change Background", 0xD4AF37u, 13);
    }

    /* Color swatch showing current background */
    {
        int32_t sw_x = ix + 240;
        int32_t sw_y = iy + 143;
        int32_t sw_sz = 24;
        for (int32_t row = sw_y; row < sw_y + sw_sz &&
             row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = sw_x; col < sw_x + sw_sz &&
                 col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == sw_y || row == sw_y + sw_sz - 1 ||
                               col == sw_x || col == sw_x + sw_sz - 1);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : desktop_bg_color;
            }
        }
    }

    /* Color name label */
    ttf_draw_string(buf, (int)w, (int)h, ix + 270, iy + 150,
                    settings_bg_names[settings_bg_index], 0x000000u, 13);

    /* Dock Configuration section header */
    ttf_draw_string(buf, (int)w, (int)h, ix + 20, iy + 190,
                    "Dock Configuration", 0xD4AF37u, 14);

    /* Dock Edge toggle button */
    {
        int32_t btn_x = ix + 20;
        int32_t btn_y = iy + 220;
        int32_t btn_w = 200;
        int32_t btn_h = 30;
        for (int32_t row = btn_y; row < btn_y + btn_h &&
             row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = btn_x; col < btn_x + btn_w &&
                 col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == btn_y || row == btn_y + btn_h - 1 ||
                               col == btn_x || col == btn_x + btn_w - 1);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : 0x1A1508u;
            }
        }
        char edge_label[64];
        str_copy(edge_label, "Position: ", 64);
        str_append(edge_label, dock_edge_names[dock_cfg.edge], 64);
        ttf_draw_string(buf, (int)w, (int)h, btn_x + 12, btn_y + 8,
                        edge_label, 0xD4AF37u, 13);
    }

    /* Dock scale buttons */
    {
        int32_t btn_x = ix + 20;
        int32_t btn_y = iy + 260;
        /* [-] button */
        for (int32_t row = btn_y; row < btn_y + 28 && row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = btn_x; col < btn_x + 40 && col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == btn_y || row == btn_y + 27 ||
                               col == btn_x || col == btn_x + 39);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : 0x1A1508u;
            }
        }
        ttf_draw_string(buf, (int)w, (int)h, btn_x + 14, btn_y + 6,
                        "-", 0xD4AF37u, 14);

        /* [+] button */
        int32_t btn2_x = btn_x + 50;
        for (int32_t row = btn_y; row < btn_y + 28 && row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = btn2_x; col < btn2_x + 40 && col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == btn_y || row == btn_y + 27 ||
                               col == btn2_x || col == btn2_x + 39);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : 0x1A1508u;
            }
        }
        ttf_draw_string(buf, (int)w, (int)h, btn2_x + 12, btn_y + 6,
                        "+", 0xD4AF37u, 14);

        /* Scale label */
        char sz_label[32];
        str_copy(sz_label, "Icon Size: ", 32);
        char num[8];
        uint_to_str((uint32_t)dock_cfg.icon_sz, num);
        str_append(sz_label, num, 32);
        ttf_draw_string(buf, (int)w, (int)h, btn2_x + 50, btn_y + 6,
                        sz_label, 0x000000u, 13);
    }
}

/* ===== DOCK LAUNCH IMPLEMENTATION (after all window state is declared) ===== */

static void dock_launch_app_from_icon(const char *app_name, int icon_idx) {
    int32_t icx, icy, icw, ich;
    dock_icon_rect(explorer_scr_w, icon_idx, &icx, &icy, &icw, &ich);
    int32_t icon_cx = icx + icw / 2;
    int32_t icon_cy = icy + ich / 2;

    if (str_eq(app_name, "explorer")) {
        explorer_open_window(explorer_scr_w, explorer_scr_h);
        spawn_anim_start(icon_cx, icon_cy, exp_win_x, exp_win_y,
                          exp_win_w, exp_win_h);
        return;
    }
    if (str_eq(app_name, "settings")) {
        settings_open_window(explorer_scr_w, explorer_scr_h);
        spawn_anim_start(icon_cx, icon_cy, set_win_x, set_win_y,
                          set_win_w, set_win_h);
        return;
    }
    if (str_eq(app_name, "browser")) {
        browser_open_window(explorer_scr_w, explorer_scr_h);
        spawn_anim_start(icon_cx, icon_cy, brw_win_x, brw_win_y,
                          brw_win_w, brw_win_h);
        return;
    }

    /* Generic ELF app — copy name into window title */
    int ti = 0;
    const char *p = app_name;
    while (*p && ti < 60)
        app_win_title[ti++] = *p++;
    app_win_title[ti] = '\0';

    for (int i = 0; i < APP_CANVAS_W * APP_CANVAS_H; i++)
        app_canvas[i] = 0;

    silent_launch = 1;
    execute_bin_internal(app_name, 0);
    silent_launch = 0;

    app_window_open = 1;
    app_canvas_active = 1;
    app_window_init_pos(explorer_scr_w, explorer_scr_h);
    spawn_anim_start(icon_cx, icon_cy, app_win_x, app_win_y,
                      app_win_w, app_win_h);
}

/* ===== WEB BROWSER APPLICATION ===== */

static void browser_open_window(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    browser_open = 1;
    brw_win_w = BROWSER_WIN_W;
    brw_win_h = BROWSER_WIN_H;
    brw_win_x = ((int32_t)scr_w - brw_win_w) / 2 + 20;
    brw_win_y = MENUBAR_H + 20;
    if (brw_win_x < 0) brw_win_x = 0;
    if (brw_win_y < MENUBAR_H) brw_win_y = MENUBAR_H;
    brw_win_disp_x_fp = brw_win_x << 8;
    brw_win_disp_y_fp = brw_win_y << 8;
    brw_win_dragging = 0;
    brw_page_len = 0;
    str_copy(brw_address, "socrates://home", 128);
    brw_addr_len = 15;
}

static void browser_addr_input(char ch) {
    if (ch == '\b') {
        if (brw_addr_len > 0) {
            brw_addr_len--;
            brw_address[brw_addr_len] = '\0';
        }
    } else if (ch >= 0x20 && ch < 0x7F) {
        if (brw_addr_len < 126) {
            brw_address[brw_addr_len++] = ch;
            brw_address[brw_addr_len] = '\0';
        }
    }
}

static void browser_handle_drag(int32_t mx, int32_t my, uint8_t lmb,
                                 uint32_t scr_w, uint32_t scr_h) {
    (void)scr_h;
    if (!browser_open) return;

    int click   = (lmb && !brw_win_prev_lmb);
    int release = (!lmb && brw_win_prev_lmb);
    brw_win_prev_lmb = lmb;

    int close_click = (lmb && !brw_close_prev_lmb);
    brw_close_prev_lmb = lmb;
    if (close_click &&
        hit_close_button(mx, my, brw_win_x, brw_win_y, brw_win_w)) {
        browser_open = 0;
        brw_win_dragging = 0;
        brw_win_x = -1;
        return;
    }

    if (click && !brw_win_dragging) {
        if (mx >= brw_win_x && mx < brw_win_x + brw_win_w &&
            my >= brw_win_y && my < brw_win_y + BROWSER_TITLE_H +
            BROWSER_BORDER) {
            brw_win_dragging = 1;
            brw_win_drag_ox = mx - brw_win_x;
            brw_win_drag_oy = my - brw_win_y;
        }
    }
    if (release) brw_win_dragging = 0;

    if (brw_win_dragging) {
        int32_t nx = mx - brw_win_drag_ox;
        int32_t ny = my - brw_win_drag_oy;
        if (nx < 0) nx = 0;
        if (ny < MENUBAR_H) ny = MENUBAR_H;
        if (nx + brw_win_w > (int32_t)scr_w)
            nx = (int32_t)scr_w - brw_win_w;
        if (ny + brw_win_h > DOCK_BAR_Y)
            ny = DOCK_BAR_Y - brw_win_h;
        brw_win_x = nx;
        brw_win_y = ny;
    }

    int32_t tx = brw_win_x << 8;
    int32_t ty = brw_win_y << 8;
    brw_win_disp_x_fp += (tx - brw_win_disp_x_fp) * 3 / 4;
    brw_win_disp_y_fp += (ty - brw_win_disp_y_fp) * 3 / 4;
    int32_t ddx = tx - brw_win_disp_x_fp;
    if (ddx < 0) ddx = -ddx;
    int32_t ddy = ty - brw_win_disp_y_fp;
    if (ddy < 0) ddy = -ddy;
    if (ddx < 128 && ddy < 128) {
        brw_win_disp_x_fp = tx;
        brw_win_disp_y_fp = ty;
    }
}

static void desktop_draw_browser(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!browser_open) return;
    if (spawn_anim.active) return;
    if (brw_win_x < 0) return;

    int32_t wx = brw_win_disp_x_fp >> 8;
    int32_t wy = brw_win_disp_y_fp >> 8;
    int32_t ww = brw_win_w;
    int32_t wh = brw_win_h;

    /* Gold border */
    for (int32_t row = wy; row < wy + wh && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx; col < wx + ww && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_border = (row == wy || row == wy + wh - 1 ||
                             col == wx || col == wx + ww - 1);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title bar */
    int32_t tb_y = wy + BROWSER_BORDER;
    for (int32_t row = tb_y; row < tb_y + BROWSER_TITLE_H &&
         row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + BROWSER_BORDER;
             col < wx + ww - BROWSER_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
        }
    }

    /* Title text */
    {
        int title_fs = 14;
        const char *title = "Socrates Browser";
        int tlen = 16;
        int title_tw = tlen * (title_fs * 6 / 10 + title_fs / 8);
        int title_tx = wx + (ww - title_tw) / 2;
        int title_ty = tb_y + (BROWSER_TITLE_H - title_fs) / 2;
        ttf_draw_string(buf, (int)w, (int)h, title_tx, title_ty,
                        title, 0x000000u, title_fs);
    }

    draw_close_button(buf, w, h, wx, wy, ww);

    /* Address bar: dark background with gold border */
    int32_t ab_y = tb_y + BROWSER_TITLE_H;
    for (int32_t row = ab_y; row < ab_y + BROWSER_ADDR_H &&
         row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = wx + BROWSER_BORDER;
             col < wx + ww - BROWSER_BORDER && col < (int32_t)w; col++) {
            if (col < 0) continue;
            int is_edge = (row == ab_y || row == ab_y + BROWSER_ADDR_H - 1);
            buf[(uint32_t)row * w + (uint32_t)col] =
                is_edge ? 0xD4AF37u : 0x1A1508u;
        }
    }

    /* Address bar text field: inset rounded rect */
    {
        int32_t field_x = wx + BROWSER_BORDER + 8;
        int32_t field_y = ab_y + 3;
        int32_t field_w = ww - 2 * BROWSER_BORDER - 16;
        int32_t field_h = BROWSER_ADDR_H - 6;
        for (int32_t row = field_y; row < field_y + field_h &&
             row < (int32_t)h; row++) {
            if (row < 0) continue;
            for (int32_t col = field_x; col < field_x + field_w &&
                 col < (int32_t)w; col++) {
                if (col < 0) continue;
                int is_edge = (row == field_y || row == field_y + field_h - 1 ||
                               col == field_x || col == field_x + field_w - 1);
                buf[(uint32_t)row * w + (uint32_t)col] =
                    is_edge ? 0xD4AF37u : 0x0D0A04u;
            }
        }

        /* URL text */
        int afs = 13;
        int ax = field_x + 6;
        int ay = field_y + (field_h - afs) / 2;
        ttf_draw_string(buf, (int)w, (int)h, ax, ay,
                        brw_address, 0xD4AF37u, afs);
    }

    /* Viewport canvas: True White */
    int32_t vp_x = wx + BROWSER_BORDER;
    int32_t vp_y = ab_y + BROWSER_ADDR_H;
    int32_t vp_w = ww - 2 * BROWSER_BORDER;
    int32_t vp_h = wh - BROWSER_BORDER - BROWSER_TITLE_H -
                   BROWSER_ADDR_H - BROWSER_BORDER;
    for (int32_t row = vp_y; row < vp_y + vp_h && row < (int32_t)h; row++) {
        if (row < 0) continue;
        for (int32_t col = vp_x; col < vp_x + vp_w && col < (int32_t)w; col++) {
            if (col < 0) continue;
            buf[(uint32_t)row * w + (uint32_t)col] = 0xFFFFFFu;
        }
    }

    /* Render page content or default home page */
    if (brw_page_len > 0) {
        int pfs = 14;
        int py = vp_y + 10;
        int px = vp_x + 12;
        int line_h = ttf_line_height(pfs);
        int ci = 0;
        char line[128];
        int li = 0;
        while (ci < brw_page_len && ci < 4096) {
            if (brw_page_buf[ci] == '\n' || li >= 120) {
                line[li] = '\0';
                if (py + pfs < vp_y + vp_h)
                    ttf_draw_string(buf, (int)w, (int)h, px, py,
                                    line, 0x000000u, pfs);
                py += line_h;
                li = 0;
                ci++;
            } else {
                if (brw_page_buf[ci] >= 0x20 && brw_page_buf[ci] < 0x7F)
                    line[li++] = (char)brw_page_buf[ci];
                ci++;
            }
        }
        if (li > 0) {
            line[li] = '\0';
            if (py + pfs < vp_y + vp_h)
                ttf_draw_string(buf, (int)w, (int)h, px, py,
                                line, 0x000000u, pfs);
        }
    } else {
        /* Default home page: centered title and status */
        int hfs = 22;
        const char *htitle = "Socrates BDS 9 Browser";
        int htlen = 22;
        int htw = htlen * (hfs * 6 / 10 + hfs / 8);
        int htx = vp_x + (vp_w - htw) / 2;
        int hty = vp_y + vp_h / 2 - hfs - 10;
        ttf_draw_string(buf, (int)w, (int)h, htx, hty,
                        htitle, 0xD4AF37u, hfs);

        int sfs = 15;
        const char *status = "System Status: Online (Local Sandbox Mode)";
        int slen = 43;
        int stw = slen * (sfs * 6 / 10 + sfs / 8);
        int stx = vp_x + (vp_w - stw) / 2;
        int sty = hty + hfs + 14;
        ttf_draw_string(buf, (int)w, (int)h, stx, sty,
                        status, 0xD4AF37u, sfs);
    }
}

/* ===== MAIN DESKTOP RENDER ===== */
static uint32_t desktop_tick = 0;

static int prev_terminal_open = 0;

static void desktop_render(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t mx, int32_t my, uint8_t buttons) {
    desktop_tick++;

    /* Cache screen dimensions for deferred launches */
    explorer_scr_w = w;
    explorer_scr_h = h;

    /* Recalculate dock geometry for current screen size */
    dock_recalc(w, h);

    /* Handle menu clicks and dock clicks */
    desktop_handle_menu_click(mx, my, buttons & 1);
    desktop_handle_dock_click(mx, my, buttons & 1, w);

    /* Detect terminal just opened — trigger animation */
    if (is_terminal_open && !prev_terminal_open) {
        window_init_pos(w, h);
        open_anim_active = 1;
        open_anim_tick = 0;
    }
    prev_terminal_open = is_terminal_open;

    /* Only open terminal on first keyboard input if it's open */
    if (is_terminal_open && !term_ready) {
        term_init();
        term_print("Socrates Terminal v1.0\n");
        term_prompt();
        term_ready = 1;
    }

    /* Handle window drag (only when not animating) */
    if (is_terminal_open && !open_anim_active)
        window_handle_drag(mx, my, buttons & 1, w, h);

    /* Handle app window drag */
    if (app_window_open)
        app_window_handle_drag(mx, my, buttons & 1, w, h);

    /* Handle explorer window */
    if (explorer_open) {
        explorer_handle_drag(mx, my, buttons & 1, w, h);
        explorer_handle_click(mx, my, buttons & 1, desktop_tick);
    }

    /* Handle settings window */
    if (settings_open) {
        settings_handle_drag(mx, my, buttons & 1, w, h);
        settings_handle_click(mx, my, buttons & 1);
    }

    /* Handle browser window */
    if (browser_open) {
        browser_handle_drag(mx, my, buttons & 1, w, h);
    }

    /* Clear to desktop background color */
    for (uint32_t i = 0; i < w * h; i++) buf[i] = desktop_bg_color;

    draw_dragon_logo(buf, w, h);
    desktop_draw_menubar(buf, w, h);
    desktop_draw_dropdown(buf, w, h);
    desktop_draw_terminal(buf, w, h, desktop_tick);
    desktop_draw_app_window(buf, w, h);
    desktop_draw_explorer(buf, w, h);
    desktop_draw_settings(buf, w, h);
    desktop_draw_browser(buf, w, h);
    spawn_anim_draw(buf, w, h);
    desktop_draw_dock(buf, w, h);
}

#endif /* DESKTOP_H */
