#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include "ttf.h"
#include "gfx.h"
#include "fat32.h"

/*
 * Socrates BSD 9 desktop.
 *
 * Layout of this file:
 *   1. shared globals + dock config + window-kind registry
 *   2. tarfs (ustar ramdisk, single-TU inline)
 *   3. forward declarations
 *   4. syscall gateway + ELF64 loader ("hello" canvas app)
 *   5. app modules  (term.h / browser.h / apps.h)
 *   6. window manager (z-order, focus, drag, close)
 *   7. wallpaper cache
 *   8. menubar + menus
 *   9. dock
 *  10. desktop_render / desktop_key_input glue
 */

/* ===== 1. GLOBALS ===== */

#define MENUBAR_H   30
#define WIN_TITLE_H 26
#define WIN_BORDER  1

static uint32_t desktop_tick = 0;
static uint32_t scr_w_cache = 1024;
static uint32_t scr_h_cache = 768;
static uint64_t system_total_memory_mb = 0;

/* --- dock configuration --- */

#define DOCK_EDGE_BOTTOM 0
#define DOCK_EDGE_LEFT   1
#define DOCK_EDGE_RIGHT  2

#define DOCK_APP_COUNT 8

typedef struct {
    int32_t bar_y;      /* bottom edge: top y of bar; sides: top of column */
    int32_t bar_h;      /* thickness */
    int32_t bar_w;      /* length along the edge */
    int32_t icon_sz;
    int     edge;
} dock_config_t;

static dock_config_t dock_cfg = {
    .bar_y = 0, .bar_h = 44, .bar_w = 420, .icon_sz = 32,
    .edge = DOCK_EDGE_BOTTOM,
};

static void dock_recalc(uint32_t scr_w, uint32_t scr_h) {
    (void)scr_w;
    dock_cfg.bar_h = dock_cfg.icon_sz + 12;
    dock_cfg.bar_w = DOCK_APP_COUNT * (dock_cfg.icon_sz + 16) + 14;
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM)
        dock_cfg.bar_y = (int32_t)scr_h - dock_cfg.bar_h - 4;
    else
        dock_cfg.bar_y = ((int32_t)scr_h - dock_cfg.bar_w) / 2;
}

/* --- window kinds --- */

enum {
    WK_TERM = 0,
    WK_BROWSER,
    WK_FILES,
    WK_PAINT,
    WK_SYSMON,
    WK_MATRIX,
    WK_HELLO,
    WK_SETTINGS,
    WK_ABOUT,
    WK_COUNT
};

typedef struct {
    const char *title;
    int32_t w, h;
} wk_meta_t;

static const wk_meta_t wk_meta[WK_COUNT] = {
    { "Terminal",         740, 480 },
    { "Socrates Browser", 800, 560 },
    { "Files",            600, 430 },
    { "Goldsmith",        640, 470 },
    { "Monolith",         400, 480 },
    { "Matrix",           620, 420 },
    { "hello",            600, 430 },
    { "Settings",         470, 390 },
    { "About Socrates",   380, 270 },
};

/* ===== 2. TARFS ===== */

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

static const void *tar_read_file(const char *filename, uint64_t *out_size) {
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
            if (out_size) *out_size = file_size;
            return (const void *)(ptr + TAR_BLOCK_SIZE);
        }
        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }
    if (out_size) *out_size = 0;
    return 0;
}

/* ===== 2.5 UNIFIED FILESYSTEM LAYER =====
 *
 * FAT32 on the ATA disk is the real filesystem: writable, persistent,
 * host-mountable.  The tar ramdisk remains only as a read-only fallback
 * so ISO-only boots (no hard disk attached) still work.
 */

#define FS_FILEBUF_MAX (256 * 1024)
static uint8_t fs_filebuf[FS_FILEBUF_MAX];
static const char *fs_errstr = "";

static int fs_writable(void) {
    return fat_vol.mounted;
}

static const void *fs_read_file(const char *filename, uint64_t *out_size) {
    if (fat_vol.mounted) {
        fat_dirent_t e;
        char abs[256];
        if (filename[0] != '/') {
            abs[0] = '/';
            str_copy(abs + 1, filename, 255);
        } else {
            str_copy(abs, filename, 256);
        }
        if (!fat_lookup(abs, &e) || (e.attr & FAT_ATTR_DIR)) {
            if (out_size) *out_size = 0;
            return 0;
        }
        uint32_t got = 0;
        if (fat_read_file(&e, fs_filebuf, FS_FILEBUF_MAX, &got) != 0) {
            if (out_size) *out_size = 0;
            return 0;
        }
        if (out_size) *out_size = got;
        return fs_filebuf;
    }
    return tar_read_file(filename, out_size);
}

static int fs_write_file(const char *path, const void *data, uint32_t len) {
    if (!fat_vol.mounted) {
        fs_errstr = "read-only filesystem (no disk attached)";
        return -1;
    }
    if (fat_write_file(path, (const uint8_t *)data, len) != 0) {
        fs_errstr = fat_errstr;
        return -1;
    }
    return 0;
}

static int fs_delete(const char *path) {
    if (!fat_vol.mounted) {
        fs_errstr = "read-only filesystem (no disk attached)";
        return -1;
    }
    if (fat_delete(path) != 0) {
        fs_errstr = fat_errstr;
        return -1;
    }
    return 0;
}

static int fs_mkdir(const char *path) {
    if (!fat_vol.mounted) {
        fs_errstr = "read-only filesystem (no disk attached)";
        return -1;
    }
    if (fat_mkdir(path) != 0) {
        fs_errstr = fat_errstr;
        return -1;
    }
    return 0;
}

/* FAT32-only directory listing (callers fall back to tar walks) */
typedef void (*fs_list_cb)(const char *name, uint32_t size, int is_dir);

static int fs_list(const char *path, fs_list_cb cb) {
    if (!fat_vol.mounted) return -1;
    fat_dirent_t d;
    if (!fat_lookup(path, &d) || !(d.attr & FAT_ATTR_DIR)) {
        fs_errstr = "no such directory";
        return -1;
    }
    fat_iter_t it;
    fat_iter_init(&it, d.first_clus);
    fat_dirent_t e;
    while (fat_iter_next(&it, &e) == 1)
        cb(e.name, e.size, (e.attr & FAT_ATTR_DIR) ? 1 : 0);
    return 0;
}

/* ===== 3. FORWARD DECLARATIONS ===== */

static void term_print(const char *s);
static void term_print_c(const char *s, int color);
static void wm_open(int kind);
static void wm_close(int kind);
static int  wm_is_open(int kind);
static void wallpaper_set_theme(int idx);
static int  desktop_open_app_by_name(const char *name);
static void brw_navigate(const char *url);
static int  execute_bin(const char *filepath);

/* ===== 4. SYSCALL GATEWAY + ELF64 LOADER ===== */

/*
 * Syscall ABI (see apps/socrates.h):
 *   RAX = number, RDI = arg0, RSI = arg1, RDX = arg2, via int 0x80
 *   1 = print string    2 = draw pixel on app canvas    3 = mouse state
 */

#define APP_CANVAS_W 598
#define APP_CANVAS_H 402

static uint32_t app_canvas[APP_CANVAS_W * APP_CANVAS_H];
static char     app_win_title[64] = "hello";
static int      silent_launch = 0;

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
        if (px >= 0 && px < APP_CANVAS_W && py >= 0 && py < APP_CANVAS_H)
            app_canvas[py * APP_CANVAS_W + px] = (uint32_t)a2;
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

static int execute_bin_internal(const char *filepath, int verbose) {
    uint64_t fsize = 0;
    const void *fdata = fs_read_file(filepath, &fsize);
    if (!fdata || fsize < sizeof(Elf64_Ehdr)) {
        if (verbose) {
            term_print_c("run: file not found: ", 2);
            term_print_c(filepath, 2);
            term_print("\n");
        }
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)fdata;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        if (verbose) term_print_c("run: invalid ELF magic\n", 2);
        return -1;
    }
    if (ehdr->e_ident[4] != 2) {
        if (verbose) term_print_c("run: not a 64-bit ELF\n", 2);
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
        if (verbose) term_print_c("run: no loadable segments\n", 2);
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
            if (verbose) term_print_c("run: segment too large\n", 2);
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
        term_print_c("loading ELF64: ", 3);
        term_print_c(filepath, 3);
        term_print("\n");
    }

    for (int i = 0; i < APP_CANVAS_W * APP_CANVAS_H; i++)
        app_canvas[i] = 0;

    /* window title = program name */
    {
        int ti = 0;
        const char *p = filepath;
        while (*p && ti < 60) app_win_title[ti++] = *p++;
        app_win_title[ti] = '\0';
    }

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

    wm_open(WK_HELLO);
    return 0;
}

static int execute_bin(const char *filepath) {
    return execute_bin_internal(filepath, 1);
}

/* ===== 5. APP MODULES ===== */

#include "term.h"
#include "browser.h"
#include "apps.h"

/* Canvas app (WK_HELLO) content drawer */
static void hello_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       uint32_t tick, int focused) {
    (void)tick; (void)focused;
    int32_t bw2 = cw < APP_CANVAS_W ? cw : APP_CANVAS_W;
    int32_t bh2 = chh < APP_CANVAS_H ? chh : APP_CANVAS_H;
    for (int32_t y = 0; y < bh2; y++) {
        int32_t dy = cy + y;
        if (dy < 0 || dy >= (int32_t)h) continue;
        for (int32_t x = 0; x < bw2; x++) {
            int32_t dx = cx + x;
            if (dx < 0 || dx >= (int32_t)w) continue;
            buf[(uint32_t)dy * w + (uint32_t)dx] =
                app_canvas[y * APP_CANVAS_W + x];
        }
    }
}

/* ===== 6. WINDOW MANAGER ===== */

typedef struct {
    int     open;
    int32_t x, y, w, h;
} win_t;

static win_t wins[WK_COUNT];
static int wm_stack[WK_COUNT];
static int wm_stack_n = 0;
static int wm_focus = -1;
static int wm_drag = -1;
static int32_t wm_drag_ox = 0, wm_drag_oy = 0;

/* spawn animation (dock icon -> window) */
static struct {
    int active;
    int tick;
    int kind;
    int32_t src_x, src_y;
} spawn_anim = {0, 0, -1, 0, 0};

#define SPAWN_ANIM_FRAMES 12

static int wm_is_open(int kind) {
    return wins[kind].open;
}

static void wm_stack_remove(int kind) {
    int j = 0;
    for (int i = 0; i < wm_stack_n; i++)
        if (wm_stack[i] != kind)
            wm_stack[j++] = wm_stack[i];
    wm_stack_n = j;
}

static void wm_raise(int kind) {
    wm_stack_remove(kind);
    wm_stack[wm_stack_n++] = kind;
    wm_focus = kind;
}

static void wm_open(int kind) {
    if (kind < 0 || kind >= WK_COUNT) return;
    if (wins[kind].open) {
        wm_raise(kind);
        return;
    }
    win_t *win = &wins[kind];
    win->open = 1;
    win->w = wk_meta[kind].w;
    win->h = wk_meta[kind].h;

    /* cascade around the center, per-kind offset */
    int32_t off = (kind % 3) * 28 - 28;
    int32_t off2 = (kind % 4) * 22 - 33;
    win->x = ((int32_t)scr_w_cache - win->w) / 2 + off;
    win->y = MENUBAR_H +
             ((int32_t)scr_h_cache - MENUBAR_H - dock_cfg.bar_h - win->h) / 2 +
             off2;
    if (win->x < 0) win->x = 0;
    if (win->y < MENUBAR_H) win->y = MENUBAR_H;

    /* first-open hooks */
    if (kind == WK_BROWSER && brw_line_count == 0)
        brw_navigate_no_hist("socrates://home");
    if (kind == WK_FILES)
        exp_scan();

    wm_raise(kind);
}

static void wm_close(int kind) {
    if (!wins[kind].open) return;
    wins[kind].open = 0;
    wm_stack_remove(kind);
    if (wm_drag == kind) wm_drag = -1;
    wm_focus = wm_stack_n > 0 ? wm_stack[wm_stack_n - 1] : -1;
}

static void wm_content_rect(int kind, int32_t *cx, int32_t *cy,
                            int32_t *cw, int32_t *chh) {
    win_t *win = &wins[kind];
    *cx = win->x + WIN_BORDER;
    *cy = win->y + WIN_BORDER + WIN_TITLE_H;
    *cw = win->w - 2 * WIN_BORDER;
    *chh = win->h - 2 * WIN_BORDER - WIN_TITLE_H;
}

static int wm_hit_close(int kind, int32_t mx, int32_t my) {
    win_t *win = &wins[kind];
    int32_t bx = win->x + win->w - 22;
    int32_t by = win->y + WIN_TITLE_H / 2 + WIN_BORDER;
    int32_t dx = mx - bx, dy = my - by;
    return dx * dx + dy * dy <= 81;   /* r=9 hit circle */
}

static int wm_hit_window(int kind, int32_t mx, int32_t my) {
    win_t *win = &wins[kind];
    return mx >= win->x && mx < win->x + win->w &&
           my >= win->y && my < win->y + win->h;
}

/* topmost open window containing the point, or -1 */
static int wm_top_at(int32_t mx, int32_t my) {
    for (int i = wm_stack_n - 1; i >= 0; i--)
        if (wm_hit_window(wm_stack[i], mx, my))
            return wm_stack[i];
    return -1;
}

static const char *wm_title_for(int kind) {
    if (kind == WK_BROWSER) return brw_title;
    if (kind == WK_HELLO)   return app_win_title;
    return wk_meta[kind].title;
}

static void wm_draw_frame(uint32_t *buf, uint32_t w, uint32_t h, int kind) {
    win_t *win = &wins[kind];
    int focused = (wm_focus == kind);

    /* soft shadow */
    gfx_rect_blend(buf, w, h, win->x + 4, win->y + win->h, win->w, 4,
                   0x000000u, 60);
    gfx_rect_blend(buf, w, h, win->x + win->w, win->y + 4, 4, win->h,
                   0x000000u, 60);

    /* border */
    gfx_rect_outline(buf, w, h, win->x, win->y, win->w, win->h,
                     focused ? C_GOLD : C_BORDER_UNF);

    /* titlebar */
    gfx_rect(buf, w, h, win->x + 1, win->y + 1, win->w - 2, WIN_TITLE_H,
             focused ? C_TITLE_FOC : C_TITLE_UNF);
    gfx_rect(buf, w, h, win->x + 1, win->y + WIN_TITLE_H, win->w - 2, 1,
             focused ? C_GOLD_DIM : 0x262B38u);

    /* title text */
    {
        const char *title = wm_title_for(kind);
        int tw = ttf_text_width(title, 13);
        int tx = win->x + (win->w - tw) / 2;
        if (tx < win->x + 30) tx = win->x + 30;
        ttf_draw_string(buf, (int)w, (int)h, tx, win->y + 5, title,
                        focused ? C_TEXT : C_TEXT_DIM, 13);
    }

    /* close button */
    {
        int32_t bx = win->x + win->w - 22;
        int32_t by = win->y + WIN_TITLE_H / 2 + WIN_BORDER;
        gfx_circle(buf, w, h, bx, by, 7, focused ? C_RED : 0x4A5060u);
        if (focused) {
            gfx_line(buf, w, h, bx - 3, by - 3, bx + 3, by + 3, 1, 0x5A1616u);
            gfx_line(buf, w, h, bx - 3, by + 3, bx + 3, by - 3, 1, 0x5A1616u);
        }
    }
}

static void wm_draw_content(uint32_t *buf, uint32_t w, uint32_t h, int kind) {
    int32_t cx, cy, cw, chh;
    wm_content_rect(kind, &cx, &cy, &cw, &chh);
    int focused = (wm_focus == kind);

    switch (kind) {
    case WK_TERM:
        term_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_BROWSER:
        brw_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_FILES:
        exp_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_PAINT:
        paint_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_SYSMON:
        sysmon_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_MATRIX:
        mtx_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_HELLO:
        hello_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_SETTINGS:
        settings_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_ABOUT:
        about_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    default:
        break;
    }
}

static void wm_update(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                      int click_consumed) {
    int click = (lmb && !prev_lmb) && !click_consumed;

    if (!lmb)
        wm_drag = -1;

    if (click) {
        int hit = wm_top_at(mx, my);
        if (hit >= 0) {
            wm_raise(hit);
            if (wm_hit_close(hit, mx, my)) {
                wm_close(hit);
            } else if (my < wins[hit].y + WIN_TITLE_H + WIN_BORDER) {
                wm_drag = hit;
                wm_drag_ox = mx - wins[hit].x;
                wm_drag_oy = my - wins[hit].y;
            }
        } else {
            wm_focus = -1;
        }
    }

    if (wm_drag >= 0 && lmb) {
        win_t *win = &wins[wm_drag];
        int32_t nx = mx - wm_drag_ox;
        int32_t ny = my - wm_drag_oy;
        /* keep a grabbable strip on screen */
        if (nx < -win->w + 80) nx = -win->w + 80;
        if (nx > (int32_t)scr_w_cache - 80) nx = (int32_t)scr_w_cache - 80;
        if (ny < MENUBAR_H) ny = MENUBAR_H;
        if (ny > (int32_t)scr_h_cache - 60) ny = (int32_t)scr_h_cache - 60;
        win->x = nx;
        win->y = ny;
    }

    /* route mouse to the focused window's content handler */
    if (wm_focus >= 0 && wins[wm_focus].open && wm_drag < 0) {
        int32_t cx, cy, cw, chh;
        wm_content_rect(wm_focus, &cx, &cy, &cw, &chh);
        uint8_t eff_lmb = click_consumed ? 0 : lmb;
        switch (wm_focus) {
        case WK_BROWSER:
            brw_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_FILES:
            exp_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh,
                      desktop_tick);
            break;
        case WK_SETTINGS:
            settings_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_PAINT:
            paint_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh,
                        wm_top_at(mx, my) == WK_PAINT);
            break;
        default:
            break;
        }
    }
}

static void wm_draw_all(uint32_t *buf, uint32_t w, uint32_t h) {
    for (int i = 0; i < wm_stack_n; i++) {
        int kind = wm_stack[i];
        if (spawn_anim.active && spawn_anim.kind == kind)
            continue;   /* revealed when the animation lands */
        wm_draw_frame(buf, w, h, kind);
        wm_draw_content(buf, w, h, kind);
    }
}

/* --- spawn animation --- */

static void spawn_anim_start(int kind, int32_t icon_cx, int32_t icon_cy) {
    spawn_anim.active = 1;
    spawn_anim.tick = 0;
    spawn_anim.kind = kind;
    spawn_anim.src_x = icon_cx;
    spawn_anim.src_y = icon_cy;
}

static void spawn_anim_draw(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!spawn_anim.active) return;
    int kind = spawn_anim.kind;
    if (kind < 0 || !wins[kind].open) {
        spawn_anim.active = 0;
        return;
    }
    spawn_anim.tick++;
    int t = spawn_anim.tick;
    if (t >= SPAWN_ANIM_FRAMES) {
        spawn_anim.active = 0;
        return;
    }
    win_t *win = &wins[kind];
    int32_t cur_x = spawn_anim.src_x + (win->x - spawn_anim.src_x) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_y = spawn_anim.src_y + (win->y - spawn_anim.src_y) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_w = 6 + (win->w - 6) * t / SPAWN_ANIM_FRAMES;
    int32_t cur_h = 6 + (win->h - 6) * t / SPAWN_ANIM_FRAMES;

    uint32_t alpha = (uint32_t)t * 220u / SPAWN_ANIM_FRAMES;
    gfx_rect_blend(buf, w, h, cur_x, cur_y, cur_w, 2, C_GOLD, alpha);
    gfx_rect_blend(buf, w, h, cur_x, cur_y + cur_h - 2, cur_w, 2, C_GOLD, alpha);
    gfx_rect_blend(buf, w, h, cur_x, cur_y, 2, cur_h, C_GOLD, alpha);
    gfx_rect_blend(buf, w, h, cur_x + cur_w - 2, cur_y, 2, cur_h, C_GOLD, alpha);
    gfx_rect_blend(buf, w, h, cur_x, cur_y, cur_w, WIN_TITLE_H * t / SPAWN_ANIM_FRAMES,
                   C_TITLE_FOC, alpha);
}

/* ===== 7. WALLPAPER (cached, regenerated on theme/size change) ===== */

#define WALL_MAX_W 1920
#define WALL_MAX_H 1080

static uint32_t wallpaper[WALL_MAX_W * WALL_MAX_H];
static int      wall_cur_theme = 0;
static uint32_t wall_gen_w = 0;
static uint32_t wall_gen_h = 0;

static void wall_dragon(uint32_t *buf, uint32_t w, uint32_t h,
                        uint32_t body, uint32_t accent, uint32_t bright) {
    int cx = (int)w / 2;
    int cy = ((int)h + MENUBAR_H) / 2;

    /* wing */
    gfx_tri(buf, w, h, cx+80,cy-50,  cx-300,cy-350, cx-20,cy-35,   body);
    gfx_tri(buf, w, h, cx-20,cy-35,  cx-300,cy-350, cx-360,cy-15,  body);
    gfx_tri(buf, w, h, cx-300,cy-350, cx-360,cy-15, cx-400,cy-120, body);
    gfx_line(buf, w, h, cx+60,cy-45,  cx-280,cy-330, 2, accent);
    gfx_line(buf, w, h, cx+60,cy-45,  cx-380,cy-100, 2, accent);
    gfx_line(buf, w, h, cx-340,cy-10, cx-380,cy-100, 2, accent);

    /* tail */
    gfx_tri(buf, w, h, cx-180,cy+20,  cx-160,cy+120, cx-290,cy+80,  body);
    gfx_tri(buf, w, h, cx-180,cy+20,  cx-290,cy+80,  cx-420,cy+50,  body);
    gfx_tri(buf, w, h, cx-290,cy+80,  cx-420,cy+50,  cx-480,cy-50,  body);
    gfx_tri(buf, w, h, cx-460,cy-40,  cx-485,cy-25,  cx-465,cy+5,   accent);

    /* body */
    gfx_tri(buf, w, h, cx+80,cy-50,   cx+130,cy+80,  cx-20,cy-35,   body);
    gfx_tri(buf, w, h, cx+130,cy+80,  cx-20,cy-35,   cx+80,cy+100,  body);
    gfx_tri(buf, w, h, cx-20,cy-35,   cx+80,cy+100,  cx-180,cy+20,  body);
    gfx_tri(buf, w, h, cx+80,cy+100,  cx-180,cy+20,  cx-120,cy+140, body);
    gfx_tri(buf, w, h, cx-180,cy+20,  cx-120,cy+140, cx-160,cy+120, body);

    /* neck + head */
    gfx_tri(buf, w, h, cx+190,cy-95,  cx+340,cy+25,  cx+80,cy-50,   body);
    gfx_tri(buf, w, h, cx+340,cy+25,  cx+80,cy-50,   cx+130,cy+80,  body);
    gfx_tri(buf, w, h, cx+260,cy-210, cx+400,cy-30,  cx+190,cy-95,  body);
    gfx_tri(buf, w, h, cx+400,cy-30,  cx+190,cy-95,  cx+340,cy+25,  body);
    gfx_tri(buf, w, h, cx+400,cy-30,  cx+340,cy+25,  cx+390,cy+60,  body);

    /* horns */
    gfx_tri(buf, w, h, cx+255,cy-205, cx+225,cy-320, cx+200,cy-180, body);
    gfx_tri(buf, w, h, cx+185,cy-175, cx+150,cy-290, cx+150,cy-150, body);
    gfx_tri(buf, w, h, cx+225,cy-320, cx+240,cy-270, cx+210,cy-265, accent);
    gfx_tri(buf, w, h, cx+150,cy-290, cx+165,cy-245, cx+135,cy-240, accent);

    /* eye */
    gfx_tri(buf, w, h, cx+340,cy-80,  cx+352,cy-60,  cx+340,cy-40,  bright);
    gfx_tri(buf, w, h, cx+340,cy-80,  cx+328,cy-60,  cx+340,cy-40,  bright);

    /* spine ridges */
    gfx_tri(buf, w, h, cx+55,cy-53,   cx+40,cy-78,   cx+25,cy-53,   accent);
    gfx_tri(buf, w, h, cx+15,cy-42,   cx+0,cy-65,    cx-15,cy-42,   accent);
    gfx_tri(buf, w, h, cx-25,cy-38,   cx-40,cy-58,   cx-55,cy-38,   accent);
    gfx_tri(buf, w, h, cx-70,cy-25,   cx-85,cy-45,   cx-100,cy-25,  accent);
    gfx_tri(buf, w, h, cx-115,cy-12,  cx-130,cy-32,  cx-145,cy-12,  accent);

    /* belly scales */
    gfx_tri(buf, w, h, cx+100,cy+95,  cx+80,cy+112,  cx+100,cy+112, accent);
    gfx_tri(buf, w, h, cx+65,cy+110,  cx+45,cy+125,  cx+65,cy+125,  accent);
    gfx_tri(buf, w, h, cx+30,cy+122,  cx+10,cy+136,  cx+30,cy+136,  accent);
    gfx_tri(buf, w, h, cx-5,cy+132,   cx-25,cy+145,  cx-5,cy+145,   accent);

    /* legs + claws */
    gfx_line(buf, w, h, cx+90,cy+95,   cx+140,cy+220, 5, body);
    gfx_line(buf, w, h, cx+140,cy+220, cx+110,cy+300, 4, body);
    gfx_line(buf, w, h, cx-100,cy+130, cx-80,cy+230,  5, body);
    gfx_line(buf, w, h, cx-80,cy+230,  cx-120,cy+300, 4, body);
    gfx_tri(buf, w, h, cx+110,cy+298, cx+95,cy+318,  cx+108,cy+318, accent);
    gfx_tri(buf, w, h, cx+110,cy+298, cx+112,cy+320, cx+122,cy+315, accent);
    gfx_tri(buf, w, h, cx+110,cy+298, cx+128,cy+312, cx+132,cy+302, accent);
    gfx_tri(buf, w, h, cx-120,cy+298, cx-135,cy+318, cx-122,cy+318, accent);
    gfx_tri(buf, w, h, cx-120,cy+298, cx-118,cy+320, cx-108,cy+315, accent);
    gfx_tri(buf, w, h, cx-120,cy+298, cx-102,cy+312, cx-98,cy+302,  accent);
}

static void wallpaper_regen(uint32_t w, uint32_t h) {
    if (w > WALL_MAX_W) w = WALL_MAX_W;
    if (h > WALL_MAX_H) h = WALL_MAX_H;

    uint32_t top = wall_theme_top[wall_cur_theme];
    uint32_t bot = wall_theme_bot[wall_cur_theme];
    gfx_vgrad(wallpaper, w, h, 0, 0, (int32_t)w, (int32_t)h, top, bot);

    /* faint horizon glow band */
    for (int32_t y = (int32_t)h * 2 / 5; y < (int32_t)h * 3 / 5; y++) {
        int32_t band = (int32_t)h / 5;
        int32_t d = y - (int32_t)h / 2;
        if (d < 0) d = -d;
        uint32_t a = (uint32_t)(18 - 18 * d * 2 / band);
        if (a > 0)
            gfx_rect_blend(wallpaper, w, h, 0, y, (int32_t)w, 1, C_GOLD, a);
    }

    uint32_t body   = gfx_mix(0x000000u, bot, 150);
    uint32_t accent = C_GOLD_DIM;
    wall_dragon(wallpaper, w, h, body, accent, C_GOLD);

    /* signature bottom-left */
    ttf_draw_string(wallpaper, (int)w, (int)h, 24, (int)h - 46,
                    "SOCRATES BSD 9", gfx_mix(C_GOLD, bot, 140), 15);
    gfx_rect(wallpaper, w, h, 24, (int32_t)h - 24, 120, 1,
             gfx_mix(C_GOLD, bot, 90));

    wall_gen_w = w;
    wall_gen_h = h;
}

static void wallpaper_set_theme(int idx) {
    if (idx < 0 || idx >= WALL_THEME_COUNT) return;
    wall_cur_theme = idx;
    wallpaper_regen(scr_w_cache, scr_h_cache);
}

/* ===== 8. MENUBAR ===== */

typedef struct {
    const char *label;
    int action;      /* >=0: open window kind; 100 reboot; 101 shutdown; -1 sep */
} menu_item_t;

static const menu_item_t menu_system[] = {
    { "About Socrates", WK_ABOUT },
    { "Settings",       WK_SETTINGS },
    { "-",              -1 },
    { "Restart",        100 },
    { "Shut Down",      101 },
};

static const menu_item_t menu_apps[] = {
    { "Terminal",  WK_TERM },
    { "Browser",   WK_BROWSER },
    { "Files",     WK_FILES },
    { "-",         -1 },
    { "Goldsmith", WK_PAINT },
    { "Monolith",  WK_SYSMON },
    { "Matrix",    WK_MATRIX },
    { "hello.elf", WK_HELLO },
};

#define MENU_COUNT 2
static const char *menu_labels[MENU_COUNT] = { "Socrates", "Apps" };
static const menu_item_t *menu_items[MENU_COUNT] = { menu_system, menu_apps };
static const int menu_item_count[MENU_COUNT] = { 5, 8 };

static int menu_open_idx = -1;

#define MENU_ITEM_H 26
#define MENU_DD_W   170

/* label x range in the bar */
static void menu_label_rect(int idx, int32_t *x0, int32_t *x1) {
    int32_t x = 40;   /* after the logo mark */
    for (int i = 0; i < MENU_COUNT; i++) {
        int lw = ttf_text_width(menu_labels[i], 14) + 24;
        if (i == idx) {
            *x0 = x;
            *x1 = x + lw;
            return;
        }
        x += lw;
    }
    *x0 = 0; *x1 = 0;
}

static void menu_action(int action) {
    if (action >= 0 && action < WK_COUNT) {
        if (action == WK_HELLO) {
            silent_launch = 1;
            execute_bin_internal("hello", 0);
            silent_launch = 0;
        }
        wm_open(action);
    } else if (action == 100) {
        outb(0x64, 0xFE);
    } else if (action == 101) {
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0x604) : "memory");
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000),
                         "Nd"((uint16_t)0xB004) : "memory");
    }
}

/* returns 1 if the click was consumed by the menubar/menus */
static int menu_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb) {
    int click = (lmb && !prev_lmb);
    if (!click) return 0;

    /* click on a bar label */
    if (my >= 0 && my < MENUBAR_H) {
        for (int i = 0; i < MENU_COUNT; i++) {
            int32_t x0, x1;
            menu_label_rect(i, &x0, &x1);
            if (mx >= x0 && mx < x1) {
                menu_open_idx = (menu_open_idx == i) ? -1 : i;
                return 1;
            }
        }
        menu_open_idx = -1;
        return 1;    /* clicks on the bar never fall through */
    }

    /* click inside an open dropdown */
    if (menu_open_idx >= 0) {
        int32_t x0, x1;
        menu_label_rect(menu_open_idx, &x0, &x1);
        int n = menu_item_count[menu_open_idx];
        int32_t dy = MENUBAR_H;
        if (mx >= x0 && mx < x0 + MENU_DD_W &&
            my >= dy && my < dy + n * MENU_ITEM_H) {
            int idx = (my - dy) / MENU_ITEM_H;
            if (idx >= 0 && idx < n && menu_items[menu_open_idx][idx].action >= 0) {
                menu_action(menu_items[menu_open_idx][idx].action);
            }
            menu_open_idx = -1;
            return 1;
        }
        menu_open_idx = -1;
        /* fall through: the click still hits whatever was underneath? no —
         * closing a menu consumes the click, like every other desktop */
        return 1;
    }
    return 0;
}

static void menubar_draw(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t mx, int32_t my) {
    gfx_rect(buf, w, h, 0, 0, (int32_t)w, MENUBAR_H, C_BG_PANEL);
    gfx_rect(buf, w, h, 0, MENUBAR_H - 1, (int32_t)w, 1, 0x2A3040u);

    /* logo mark: gold diamond */
    {
        int32_t lx = 20, ly = MENUBAR_H / 2;
        gfx_tri(buf, w, h, lx, ly - 7, lx - 6, ly, lx, ly + 7, C_GOLD);
        gfx_tri(buf, w, h, lx, ly - 7, lx + 6, ly, lx, ly + 7, C_GOLD);
    }

    /* menu labels */
    for (int i = 0; i < MENU_COUNT; i++) {
        int32_t x0, x1;
        menu_label_rect(i, &x0, &x1);
        int hot = (menu_open_idx == i) ||
                  (my >= 0 && my < MENUBAR_H && mx >= x0 && mx < x1);
        if (hot)
            gfx_rect(buf, w, h, x0, 2, x1 - x0, MENUBAR_H - 5, 0x252B3Cu);
        ttf_draw_string(buf, (int)w, (int)h, x0 + 12, 6, menu_labels[i],
                        i == 0 ? C_GOLD : C_TEXT, 14);
    }

    /* right side: net indicator + date + clock */
    {
        char clk[10];
        clock_string(clk);
        int cw2 = ttf_text_width(clk, 14);
        int32_t x = (int32_t)w - cw2 - 16;
        ttf_draw_string(buf, (int)w, (int)h, x, 6, clk, C_TEXT, 14);

        char dt[16];
        date_string(dt);
        int dw = ttf_text_width(dt, 13);
        x -= dw + 18;
        ttf_draw_string(buf, (int)w, (int)h, x, 7, dt, C_TEXT_DIM, 13);

        /* net dot */
        x -= 22;
        int up = 0;
        if (e1000_found) {
            uint32_t status = e1000_read(E1000_STATUS);
            up = (status & E1000_STATUS_LU) ? 1 : 0;
        }
        gfx_circle(buf, w, h, x, MENUBAR_H / 2, 4, up ? C_GREEN : 0x555C6Eu);
    }
}

static void menu_dropdown_draw(uint32_t *buf, uint32_t w, uint32_t h,
                               int32_t mx, int32_t my) {
    if (menu_open_idx < 0) return;

    int32_t x0, x1;
    menu_label_rect(menu_open_idx, &x0, &x1);
    int n = menu_item_count[menu_open_idx];
    int32_t dy = MENUBAR_H;
    int32_t dh = n * MENU_ITEM_H;

    gfx_rect_blend(buf, w, h, x0 + 3, dy + 3, MENU_DD_W, dh, 0x000000u, 70);
    gfx_rect(buf, w, h, x0, dy, MENU_DD_W, dh, 0x1A1E2Au);
    gfx_rect_outline(buf, w, h, x0, dy, MENU_DD_W, dh, C_GOLD_DIM);

    for (int i = 0; i < n; i++) {
        const menu_item_t *it = &menu_items[menu_open_idx][i];
        int32_t iy = dy + i * MENU_ITEM_H;
        if (it->action < 0) {
            gfx_rect(buf, w, h, x0 + 10, iy + MENU_ITEM_H / 2, MENU_DD_W - 20,
                     1, 0x2E3444u);
            continue;
        }
        int hot = (mx >= x0 && mx < x0 + MENU_DD_W &&
                   my >= iy && my < iy + MENU_ITEM_H);
        if (hot)
            gfx_rect(buf, w, h, x0 + 1, iy, MENU_DD_W - 2, MENU_ITEM_H,
                     0x2A2410u);
        ttf_draw_string(buf, (int)w, (int)h, x0 + 14, iy + 5, it->label,
                        hot ? C_GOLD : C_TEXT, 13);
        /* open-window marker */
        if (it->action < WK_COUNT && wm_is_open(it->action))
            gfx_circle(buf, w, h, x0 + MENU_DD_W - 12, iy + MENU_ITEM_H / 2,
                       2, C_GOLD);
    }
}

/* ===== 9. DOCK ===== */

static const int dock_kinds[DOCK_APP_COUNT] = {
    WK_TERM, WK_BROWSER, WK_FILES, WK_PAINT,
    WK_SYSMON, WK_MATRIX, WK_HELLO, WK_SETTINGS,
};

static void dock_bar_rect(uint32_t scr_w, int32_t *rx, int32_t *ry,
                          int32_t *rw, int32_t *rh) {
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        *rx = ((int32_t)scr_w - dock_cfg.bar_w) / 2;
        *ry = dock_cfg.bar_y;
        *rw = dock_cfg.bar_w;
        *rh = dock_cfg.bar_h;
    } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
        *rx = 4;
        *ry = dock_cfg.bar_y;
        *rw = dock_cfg.bar_h;
        *rh = dock_cfg.bar_w;
    } else {
        *rx = (int32_t)scr_w - dock_cfg.bar_h - 4;
        *ry = dock_cfg.bar_y;
        *rw = dock_cfg.bar_h;
        *rh = dock_cfg.bar_w;
    }
}

static void dock_icon_rect(uint32_t scr_w, int idx,
                           int32_t *ox, int32_t *oy,
                           int32_t *ow, int32_t *oh) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(scr_w, &rx, &ry, &rw, &rh);
    int32_t isz = dock_cfg.icon_sz;
    int32_t cell = isz + 16;

    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        *ox = rx + 7 + idx * cell + (cell - isz) / 2;
        *oy = ry + (rh - isz) / 2;
    } else {
        *ox = rx + (rw - isz) / 2;
        *oy = ry + 7 + idx * cell + (cell - isz) / 2;
    }
    *ow = isz;
    *oh = isz;
}

/* pictogram icons — everything scales off sz */
static void dock_draw_glyph(uint32_t *buf, uint32_t w, uint32_t h,
                            int kind, int32_t x, int32_t y, int32_t sz) {
    int32_t cx = x + sz / 2;
    int32_t cy = y + sz / 2;
    int32_t q = sz / 4;

    switch (kind) {
    case WK_TERM:
        mono_text(buf, w, h, x + q / 2 + 2, cy - 4, ">_", C_GOLD, 1);
        break;
    case WK_BROWSER:
        gfx_circle_outline(buf, w, h, cx, cy, q + 3, C_TEXT);
        gfx_rect(buf, w, h, cx - q - 3, cy, 2 * (q + 3) + 1, 1, C_TEXT);
        gfx_circle_outline(buf, w, h, cx, cy, q + 3, C_TEXT);
        /* vertical meridian: thin ellipse approximated by lines */
        gfx_line(buf, w, h, cx, cy - q - 3, cx - q / 2 - 1, cy, 1, C_TEXT);
        gfx_line(buf, w, h, cx - q / 2 - 1, cy, cx, cy + q + 3, 1, C_TEXT);
        gfx_line(buf, w, h, cx, cy - q - 3, cx + q / 2 + 1, cy, 1, C_TEXT);
        gfx_line(buf, w, h, cx + q / 2 + 1, cy, cx, cy + q + 3, 1, C_TEXT);
        break;
    case WK_FILES:
        gfx_rect(buf, w, h, cx - q - 2, cy - q, q + 2, 3, C_GOLD_DIM);
        gfx_rect(buf, w, h, cx - q - 2, cy - q + 2, 2 * q + 5, q + q - 1,
                 0xE8CE7Bu);
        gfx_rect_outline(buf, w, h, cx - q - 2, cy - q + 2, 2 * q + 5,
                         q + q - 1, C_GOLD_DIM);
        break;
    case WK_PAINT:
        gfx_line(buf, w, h, cx + q, cy - q, cx - q + 2, cy + q - 2, 3, C_TEXT);
        gfx_tri(buf, w, h, cx - q, cy + q - 4, cx - q + 4, cy + q,
                cx - q - 2, cy + q + 2, C_GOLD);
        break;
    case WK_SYSMON:
        gfx_rect(buf, w, h, cx - q - 2, cy + q - q / 2, 4, q / 2 + 2, C_TEXT);
        gfx_rect(buf, w, h, cx - 2, cy - 2, 4, q + 4, C_GOLD);
        gfx_rect(buf, w, h, cx + q - 2, cy - q, 4, 2 * q + 2, C_TEXT);
        break;
    case WK_MATRIX:
        for (int i = -1; i <= 1; i++) {
            int32_t colx = cx + i * (q - 1) - 1;
            int32_t top = cy - q + ((i + 1) * 3);
            for (int d = 0; d < 3; d++)
                gfx_rect(buf, w, h, colx, top + d * 5, 2, 3,
                         d == 0 ? C_GREEN : 0x2E7048u);
        }
        break;
    case WK_HELLO:
        gfx_circle_outline(buf, w, h, cx, cy, q + 3, C_GOLD);
        gfx_rect(buf, w, h, cx - q / 2 - 1, cy - 2, 2, 3, C_GOLD);
        gfx_rect(buf, w, h, cx + q / 2 - 1, cy - 2, 2, 3, C_GOLD);
        gfx_rect(buf, w, h, cx - q / 2, cy + q / 2, q + 1, 2, C_GOLD);
        break;
    case WK_SETTINGS: {
        int32_t r_out = q + 3;
        for (int32_t dy = -r_out - 2; dy <= r_out + 2; dy++)
            for (int32_t dx = -r_out - 2; dx <= r_out + 2; dx++) {
                int32_t d2 = dx * dx + dy * dy;
                int32_t px = cx + dx, py = cy + dy;
                if (px < 0 || px >= (int32_t)w || py < 0 || py >= (int32_t)h)
                    continue;
                int32_t adx = dx < 0 ? -dx : dx;
                int32_t ady = dy < 0 ? -dy : dy;
                if (d2 <= (q / 2) * (q / 2)) continue;    /* hub hole */
                if (d2 <= (q + 1) * (q + 1)) {
                    buf[(uint32_t)py * w + (uint32_t)px] = C_TEXT;
                    continue;
                }
                if (d2 <= (r_out + 2) * (r_out + 2)) {
                    int tooth = (adx <= 1) || (ady <= 1);
                    int diff = adx - ady;
                    if (diff < 0) diff = -diff;
                    if (diff <= 1 && adx > 1) tooth = 1;
                    if (tooth)
                        buf[(uint32_t)py * w + (uint32_t)px] = C_TEXT;
                }
            }
        break;
    }
    default:
        break;
    }
}

static void dock_launch(int idx) {
    int kind = dock_kinds[idx];
    int32_t ix, iy, iw, ih;
    dock_icon_rect(scr_w_cache, idx, &ix, &iy, &iw, &ih);

    if (kind == WK_HELLO && !wins[WK_HELLO].open) {
        silent_launch = 1;
        execute_bin_internal("hello", 0);
        silent_launch = 0;
    }
    int was_open = wins[kind].open;
    wm_open(kind);
    if (!was_open)
        spawn_anim_start(kind, ix + iw / 2, iy + ih / 2);
}

/* returns 1 if the click was consumed by the dock */
static int dock_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb) {
    int click = (lmb && !prev_lmb);
    if (!click) return 0;

    int32_t rx, ry, rw, rh;
    dock_bar_rect(scr_w_cache, &rx, &ry, &rw, &rh);
    if (mx < rx || mx >= rx + rw || my < ry || my >= ry + rh)
        return 0;

    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(scr_w_cache, i, &ix, &iy, &iw, &ih);
        if (mx >= ix - 4 && mx < ix + iw + 4 &&
            my >= iy - 4 && my < iy + ih + 4) {
            dock_launch(i);
            return 1;
        }
    }
    return 1;   /* clicks on the bar background are still consumed */
}

static void dock_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t mx, int32_t my) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(w, &rx, &ry, &rw, &rh);

    /* translucent plate */
    gfx_rect_blend(buf, w, h, rx, ry, rw, rh, C_BG_PANEL, 215);
    gfx_rect_outline(buf, w, h, rx, ry, rw, rh, 0x2E3444u);
    gfx_rect(buf, w, h, rx, ry, rw, 1, 0x3A4254u);

    int hover = -1;
    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, i, &ix, &iy, &iw, &ih);
        int kind = dock_kinds[i];
        int hot = (mx >= ix - 4 && mx < ix + iw + 4 &&
                   my >= iy - 4 && my < iy + ih + 4);
        if (hot) hover = i;

        /* icon tile */
        gfx_rect(buf, w, h, ix, iy, iw, ih, hot ? 0x262C3Eu : 0x1C2130u);
        gfx_rect_outline(buf, w, h, ix, iy, iw, ih,
                         hot ? C_GOLD_DIM : 0x2A3040u);
        dock_draw_glyph(buf, w, h, kind, ix, iy, iw);

        /* running indicator */
        if (wm_is_open(kind)) {
            if (dock_cfg.edge == DOCK_EDGE_BOTTOM)
                gfx_circle(buf, w, h, ix + iw / 2, ry + rh - 4, 2, C_GOLD);
            else if (dock_cfg.edge == DOCK_EDGE_LEFT)
                gfx_circle(buf, w, h, rx + rw - 4, iy + ih / 2, 2, C_GOLD);
            else
                gfx_circle(buf, w, h, rx + 4, iy + ih / 2, 2, C_GOLD);
        }
    }

    /* tooltip */
    if (hover >= 0) {
        int kind = dock_kinds[hover];
        const char *name = wk_meta[kind].title;
        int tw = ttf_text_width(name, 12);
        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, hover, &ix, &iy, &iw, &ih);

        int32_t tx, ty;
        if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
            tx = ix + iw / 2 - tw / 2 - 8;
            ty = ry - 26;
        } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
            tx = rx + rw + 8;
            ty = iy + ih / 2 - 11;
        } else {
            tx = rx - tw - 24;
            ty = iy + ih / 2 - 11;
        }
        gfx_rect(buf, w, h, tx, ty, tw + 16, 22, 0x1A1E2Au);
        gfx_rect_outline(buf, w, h, tx, ty, tw + 16, 22, C_GOLD_DIM);
        ttf_draw_string(buf, (int)w, (int)h, tx + 8, ty + 3, name, C_TEXT, 12);
    }
}

/* ===== 10. RENDER GLUE ===== */

static uint8_t desk_prev_lmb = 0;

static int desktop_open_app_by_name(const char *name) {
    int kind = -1;
    if (str_eq(name, "terminal") || str_eq(name, "term")) kind = WK_TERM;
    else if (str_eq(name, "browser") || str_eq(name, "web")) kind = WK_BROWSER;
    else if (str_eq(name, "files") || str_eq(name, "explorer")) kind = WK_FILES;
    else if (str_eq(name, "settings")) kind = WK_SETTINGS;
    else if (str_eq(name, "paint") || str_eq(name, "goldsmith")) kind = WK_PAINT;
    else if (str_eq(name, "sysmon") || str_eq(name, "monolith")) kind = WK_SYSMON;
    else if (str_eq(name, "matrix")) kind = WK_MATRIX;
    else if (str_eq(name, "about")) kind = WK_ABOUT;
    else if (str_eq(name, "hello")) {
        silent_launch = 1;
        execute_bin_internal("hello", 0);
        silent_launch = 0;
        wm_open(WK_HELLO);
        return 1;
    }
    if (kind < 0) return 0;
    wm_open(kind);
    return 1;
}

/* Route one keyboard character to the focused window */
static void desktop_key_input(char ch) {
    if (menu_open_idx >= 0) {
        if (ch == 27) menu_open_idx = -1;
        return;
    }
    if (wm_focus == WK_TERM) {
        if (ch == KEY_PGUP || ch == KEY_PGDN) {
            int32_t cx, cy, cw, chh;
            wm_content_rect(WK_TERM, &cx, &cy, &cw, &chh);
            int rows = (chh - 2 * TERM_PAD) / TERM_LINE_H - 1;
            term_scroll_key(ch, rows > 4 ? rows - 2 : 4);
        } else {
            term_key(ch);
        }
        return;
    }
    if (wm_focus == WK_BROWSER) {
        brw_key(ch);
        return;
    }
    if (wm_focus == WK_ABOUT && ch == 27) {
        wm_close(WK_ABOUT);
        return;
    }
    if (wm_focus == WK_MATRIX && ch == 27) {
        wm_close(WK_MATRIX);
        return;
    }
}

static void desktop_render(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t mx, int32_t my, uint8_t buttons) {
    desktop_tick++;
    scr_w_cache = w;
    scr_h_cache = h;

    dock_recalc(w, h);

    if (wall_gen_w != w || wall_gen_h != h)
        wallpaper_regen(w, h);

    /* async engines */
    term_async_poll();
    brw_poll();

    /* ---- input ---- */
    uint8_t lmb = buttons & 1;
    int consumed = menu_mouse(mx, my, lmb, desk_prev_lmb);
    if (!consumed)
        consumed = dock_mouse(mx, my, lmb, desk_prev_lmb);
    wm_update(mx, my, lmb, desk_prev_lmb, consumed);
    desk_prev_lmb = lmb;

    /* ---- draw ---- */
    for (uint32_t i = 0; i < w * h; i++)
        buf[i] = wallpaper[i];

    wm_draw_all(buf, w, h);
    spawn_anim_draw(buf, w, h);
    menubar_draw(buf, w, h, mx, my);
    menu_dropdown_draw(buf, w, h, mx, my);
    dock_draw(buf, w, h, mx, my);
}

#endif /* DESKTOP_H */
