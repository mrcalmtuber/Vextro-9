#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include "ttf.h"
#include "gfx.h"
#include "fat32.h"
#include "exfat.h"
#include "bsd_format.h"
#include "sci.h"

/*
 * Vextro 9 desktop.
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

/* ===== THE GRID =====
 *
 * Chrome heights and window sizes were whatever number looked about
 * right when each was written -- 30 here, 26 there, 470 somewhere else --
 * and nothing lined up with anything. A menubar 30 tall against a title
 * bar 26 puts every window's content on a different odd offset, and the
 * eye reads that as sloppiness long before it can say why.
 *
 * So one number governs all of it. UI_SNAP rounds to the nearest 8 at
 * compile time -- the operand is always a literal, so the AND folds away
 * and this costs nothing at runtime -- and everything laid out below is
 * declared through it rather than adjusted by hand to agree.
 *
 * Eight because the font is 13 px on 8 px metrics and the icon grid is
 * already a multiple of it, so snapping to 8 moves the fewest things.
 * Dimensions only: negative operands would need a different rounding and
 * there are none here.
 */
#define UI_GRID       8
#define UI_SNAP(v)    (((v) + (UI_GRID / 2)) & ~(UI_GRID - 1))
#define UI_SNAP_UP(v) (((v) + UI_GRID - 1) & ~(UI_GRID - 1))

#define MENUBAR_H   UI_SNAP(30)      /* 32 */
#define WIN_TITLE_H UI_SNAP(26)      /* 24 */
#define WIN_BORDER  1

static uint32_t desktop_tick = 0;
static uint32_t scr_w_cache = 1024;
static uint32_t scr_h_cache = 768;
static uint64_t system_total_memory_mb = 0;

/* --- dock configuration --- */

#define DOCK_EDGE_BOTTOM 0
#define DOCK_EDGE_LEFT   1
#define DOCK_EDGE_RIGHT  2

/* Built-in launchers, plus one slot per app installed from the store. */
#define DOCK_BASE_COUNT 11
#define DOCK_MAX_ITEMS  25

static int dock_item_count = DOCK_BASE_COUNT;

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
    dock_cfg.bar_w = dock_item_count * (dock_cfg.icon_sz + 16) + 14;
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
    WK_STORE,
    WK_IMAGE,
    WK_WIKI,
    WK_SETTINGS,
    WK_ABOUT,
    WK_COUNT
};

typedef struct {
    const char *title;
    int32_t w, h;
} wk_meta_t;

static const wk_meta_t wk_meta[WK_COUNT] = {
    { "Terminal",          UI_SNAP(740), UI_SNAP(480) },
    { "Vextro Browser",  UI_SNAP(800), UI_SNAP(560) },
    { "Files",             UI_SNAP(600), UI_SNAP(430) },
    { "Goldsmith",         UI_SNAP(640), UI_SNAP(470) },
    { "Monolith",          UI_SNAP(400), UI_SNAP(480) },
    { "Matrix",            UI_SNAP(620), UI_SNAP(420) },
    { "hello",             UI_SNAP(600), UI_SNAP(430) },
    { "Agora App Store",   UI_SNAP(720), UI_SNAP(520) },
    { "Photos",            UI_SNAP(760), UI_SNAP(560) },
    /* Wide enough to read prose in: articles are laid out in this window
     * now rather than handed to the browser, and 520 was a search box. */
    { "Wikipedia",         UI_SNAP(780), UI_SNAP(580) },
    /* Taller since the Users pane joined it. */
    { "Settings",          UI_SNAP(470), UI_SNAP(560) },
    { "About Vextro",    UI_SNAP(380), UI_SNAP(270) },
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
 * exFAT is the system volume: it carries 64-bit sizes, so a file is no
 * longer capped at FAT32's 4 GB.  FAT32 is still probed as a fallback so
 * an older disk image keeps working, and the ustar ramdisk remains the
 * read-only last resort for ISO-only boots with no disk attached.
 *
 * Everything above this line talks to fs_* and never to a driver, so
 * which filesystem is mounted is decided in exactly one place.
 */

#define FS_NONE   0
#define FS_EXFAT  1
#define FS_FAT32  2

static int fs_kind = FS_NONE;

/* Big enough for a compressed full-colour image; larger files are read
 * through fs_read_range() a window at a time. */
#define FS_FILEBUF_MAX (4 * 1024 * 1024)
static uint8_t fs_filebuf[FS_FILEBUF_MAX];
static const char *fs_errstr = "";

/*
 * Try every disk the block layer found, in its order, and stop at the
 * first one carrying a volume we can read.
 *
 * On the machine this was developed against there is one disk and the
 * loop runs once.  On a real one there may be an NVMe system drive, a
 * SATA disk and a USB stick, and only one of them has the OS on it —
 * scanning is the only way to know which, since a bus position says
 * nothing about what is stored there.
 */
static void fs_mount(void) {
    for (int i = 0; i < blk_count; i++) {
        if (blk_select(i) != 0) continue;
        exfat_mount();
        if (exf_vol.mounted) fs_kind = FS_EXFAT;
        else {
            fat32_mount();
            if (fat_vol.mounted) fs_kind = FS_FAT32;
        }
        if (fs_kind != FS_NONE) {
            serial_puts("[fs] mounted ");
            serial_puts(fs_kind == FS_EXFAT ? "exFAT" : "FAT32");
            serial_puts(" on ");
            serial_puts(blk_bus_name());
            serial_puts(" disk ");
            serial_put_dec((uint32_t)i);
            serial_putc('\n');
            return;
        }
    }
    if (blk_count > 0) blk_select(0);
    serial_puts("[fs] no volume found on any disk\n");
    fs_kind = FS_NONE;
}

static const char *fs_name(void) {
    if (fs_kind == FS_EXFAT) return "exFAT";
    if (fs_kind == FS_FAT32) return "FAT32";
    return tarfs_base ? "ramdisk (read-only)" : "none";
}

static int fs_writable(void) { return fs_kind != FS_NONE; }

static uint32_t fs_total_kb(void) {
    if (fs_kind == FS_EXFAT) return exf_total_kb();
    if (fs_kind == FS_FAT32) return fat_total_kb();
    return 0;
}

static uint32_t fs_free_kb(void) {
    if (fs_kind == FS_EXFAT) return exf_free_kb();
    if (fs_kind == FS_FAT32) return fat_free_kb();
    return 0;
}

/* Normalise to an absolute path in the caller's buffer. */
static void fs_abs(const char *in, char *out, int max) {
    if (in[0] == '/') { str_copy(out, in, max); return; }
    out[0] = '/';
    str_copy(out + 1, in, max - 1);
}

/* Does the path exist, and what is it?  Replaces every direct driver
 * lookup that used to be scattered through the apps. */
static int fs_stat(const char *path, uint64_t *size, int *is_dir) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));

    if (fs_kind == FS_EXFAT) {
        exf_dirent_t e;
        if (!exf_lookup(abs, &e)) return 0;
        if (size) *size = e.size;
        if (is_dir) *is_dir = (e.attr & EXF_ATTR_DIR) ? 1 : 0;
        return 1;
    }
    if (fs_kind == FS_FAT32) {
        fat_dirent_t e;
        if (!fat_lookup(abs, &e)) return 0;
        if (size) *size = e.size;
        if (is_dir) *is_dir = (e.attr & FAT_ATTR_DIR) ? 1 : 0;
        return 1;
    }
    uint64_t n = 0;
    if (tar_read_file(abs, &n) && n > 0) {
        if (size) *size = n;
        if (is_dir) *is_dir = 0;
        return 1;
    }
    return 0;
}

static const void *fs_read_file(const char *filename, uint64_t *out_size) {
    char abs[256];
    fs_abs(filename, abs, sizeof(abs));

    if (fs_kind == FS_EXFAT) {
        exf_dirent_t e;
        if (!exf_lookup(abs, &e) || (e.attr & EXF_ATTR_DIR)) {
            if (out_size) *out_size = 0;
            return 0;
        }
        uint32_t got = 0;
        if (exf_read_file(&e, fs_filebuf, FS_FILEBUF_MAX, &got) != 0) {
            if (out_size) *out_size = 0;
            return 0;
        }
        if (out_size) *out_size = got;
        return fs_filebuf;
    }
    if (fs_kind == FS_FAT32) {
        fat_dirent_t e;
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

/*
 * Read a window out of a file.  This is what makes a multi-gigabyte
 * archive usable: nothing has to fit in a buffer, only the slice being
 * looked at.  exFAT only — the fallbacks cannot hold such a file anyway.
 */
static int fs_read_range(const char *path, uint64_t offset, void *buf,
                         uint32_t len, uint32_t *got) {
    *got = 0;
    char abs[256];
    fs_abs(path, abs, sizeof(abs));

    if (fs_kind == FS_EXFAT) {
        exf_dirent_t e;
        if (!exf_lookup(abs, &e) || (e.attr & EXF_ATTR_DIR)) {
            fs_errstr = "not found";
            return -1;
        }
        if (exf_read_range(&e, offset, (uint8_t *)buf, len, got) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }

    uint64_t size = 0;
    const void *d = fs_read_file(abs, &size);
    if (!d) { fs_errstr = "not found"; return -1; }
    if (offset >= size) return 0;
    uint32_t n = (uint32_t)(size - offset);
    if (n > len) n = len;
    const uint8_t *src = (const uint8_t *)d + offset;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = src[i];
    *got = n;
    return 0;
}

/*
 * An open-file handle.  Reading an archive means thousands of small
 * reads, and resolving the path through the directory tree every time
 * would dominate the cost, so the located entry is kept.
 */
typedef struct {
    int      kind;
    int      valid;
    uint64_t size;
    exf_dirent_t exf;
    char     path[160];
} fs_file_t;

static int fs_open(const char *path, fs_file_t *f) {
    f->valid = 0;
    f->kind = fs_kind;
    str_copy(f->path, path, sizeof(f->path));

    if (fs_kind == FS_EXFAT) {
        char abs[256];
        fs_abs(path, abs, sizeof(abs));
        if (!exf_lookup(abs, &f->exf) || (f->exf.attr & EXF_ATTR_DIR)) {
            fs_errstr = "not found";
            return -1;
        }
        f->size = f->exf.size;
        f->valid = 1;
        return 0;
    }

    uint64_t sz = 0;
    int is_dir = 0;
    if (!fs_stat(path, &sz, &is_dir) || is_dir) {
        fs_errstr = "not found";
        return -1;
    }
    f->size = sz;
    f->valid = 1;
    return 0;
}

static int fs_pread(fs_file_t *f, uint64_t off, void *buf, uint32_t len,
                    uint32_t *got) {
    *got = 0;
    if (!f->valid) { fs_errstr = "file not open"; return -1; }
    if (f->kind == FS_EXFAT) {
        if (exf_read_range(&f->exf, off, (uint8_t *)buf, len, got) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    return fs_read_range(f->path, off, buf, len, got);
}

static int fs_write_file(const char *path, const void *data, uint32_t len) {
    if (fs_kind == FS_EXFAT) {
        if (exf_write_file(path, (const uint8_t *)data, len) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_write_file(path, (const uint8_t *)data, len) != 0) {
            fs_errstr = fat_errstr;
            return -1;
        }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

static int fs_delete(const char *path) {
    if (fs_kind == FS_EXFAT) {
        if (exf_delete(path) != 0) { fs_errstr = exf_errstr; return -1; }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_delete(path) != 0) { fs_errstr = fat_errstr; return -1; }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

static int fs_mkdir(const char *path) {
    if (fs_kind == FS_EXFAT) {
        if (exf_mkdir(path) != 0) { fs_errstr = exf_errstr; return -1; }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_mkdir(path) != 0) { fs_errstr = fat_errstr; return -1; }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

typedef void (*fs_list_cb)(const char *name, uint32_t size, int is_dir);

static int fs_list(const char *path, fs_list_cb cb) {
    if (fs_kind == FS_EXFAT) {
        if (exf_list(path, (exf_list_cb)cb) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
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
    return -1;
}

#include "zim.h"

/* Accounts. After the filesystem layer above, which they are stored
 * through, and after login.h (included by kernel.c first) for xorshift32
 * and idt.h for cycle_now, which together seed the salt. */
#include "sha256.h"
#include "users.h"
/*
 * Whether this account wants the language model at all.
 *
 * Not everyone does: the weights are 380 MB, loading them costs real time
 * on every boot, and a machine used as a desktop has no need of them. So
 * the choice is asked once, on the first login of each account, and kept.
 *
 *   -1  not asked yet -- the dialog is up
 *    0  declined: nothing is loaded, and the Wikipedia window shows no
 *       chat tab, because offering something that has been switched off
 *       is worse than not offering it
 *    1  accepted
 *
 * Stored per account in /home/<name>/settings.cfg rather than globally,
 * since two people using the same machine can reasonably disagree.
 */
static int ai_enabled = -1;
static void ai_choice_save(int on);   /* defined with the session code */

/* Set by the menu or the `logout` command; the render loop acts on it,
 * because tearing the session down from inside a draw would pull the
 * window list out from under the code walking it. */
static int want_logout = 0;

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
static void store_cmd(int argc, char **argv);
static void store_fit(char *dst, int dst_max, const char *src,
                      int budget, int font);
static int  img_open_path(const char *path);
static const char *img_status(void);

/* ===== 4. SYSCALL GATEWAY + ELF64 LOADER ===== */

/*
 * Syscall ABI (see apps/vextro.h):
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
#ifdef APP_SELFTEST
        /* The terminal is a framebuffer window; a headless harness cannot
         * read it. Mirror to serial so what the app says is checkable. */
        if (str) { serial_puts("[app] "); serial_puts(str); }
#endif
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

/*
 * Two loaders share one app arena.  Both lay the image out at
 * app_memory + (vaddr - base_vaddr), so images have to be position
 * independent — neither loader processes relocations.
 *
 * Note that the kernel runs apps in ring 0 out of a static buffer, so it
 * cannot enforce W^X the way the host-side loader in bsdfmt/bsd_run.c
 * does; the .bsd page alignment is still honoured, and is what would let
 * a future paging-aware loader mark the text arena NX-clear and the data
 * arena NX-set without touching the format.
 */

/* ===== imported symbols =====
 *
 * What the kernel lends to applications. See bsdfmt/bsd_format.h for why
 * the table lives in the image rather than in the header.
 *
 * Unlike the aarch64 tree, this needs no deferred setter: ttf.h and gfx.h
 * are both included before this file, so the addresses are available here
 * and the table can simply be a constant.
 */
static const bsd_export_t kernel_exports[] = {
    { "ttf_draw_string", (uint64_t)(uintptr_t)ttf_draw_string },
    { "ttf_text_width",  (uint64_t)(uintptr_t)ttf_text_width  },
    { "gfx_rect",        (uint64_t)(uintptr_t)gfx_rect        },
    { 0, 0 }
};

static int bsd_name_eq(const char *a, const char *b) {
    for (int i = 0; i < BSD_IMPORT_NAMELEN; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;                       /* full-width name, no terminator */
}

/*
 * Find the import table in a loaded image and fill it in.
 *
 * `data` points at the image as loaded, `len` bounds it. The tag is
 * searched for on eight-byte boundaries because the structure is
 * eight-aligned by construction; scanning bytewise would find a tag that
 * happened to straddle two unrelated values.
 *
 * The whole span is scanned rather than just the data segment. An image
 * whose table is const — which is the natural way to write one, since the
 * loader is the only thing that writes to it — has it folded into .rodata
 * and linked with the text, and such an image can report data_size == 0.
 * Searching only the data segment finds nothing in exactly the case the
 * feature is for.
 *
 * Returns the number of names resolved, or -1 if the table is malformed.
 * An image with no table at all is not an error — that is every image that
 * existed before this — so it returns zero.
 */
static int bsd_resolve_imports(uint8_t *data, uint64_t len) {
    if (!data || len < sizeof(bsd_import_hdr_t)) return 0;

    uint64_t limit = len - sizeof(bsd_import_hdr_t);
    for (uint64_t off = 0; off <= limit; off += 8) {
        bsd_import_hdr_t *hdr = (bsd_import_hdr_t *)(data + off);
        if (hdr->magic != BSD_IMPORT_MAGIC) continue;

        if (hdr->count == 0 || hdr->count > BSD_IMPORT_MAX) return -1;

        /* The entries must lie wholly inside the image. A count that
         * overruns is the one way this can be turned into a write past
         * the image, so it is checked before anything is written. */
        uint64_t need = sizeof(bsd_import_hdr_t) +
                        (uint64_t)hdr->count * sizeof(bsd_import_t);
        if (need > len - off) return -1;

        bsd_import_t *e = (bsd_import_t *)(data + off + sizeof(bsd_import_hdr_t));
        int found = 0;
        for (uint32_t i = 0; i < hdr->count; i++) {
            e[i].addr = 0;
            for (int k = 0; kernel_exports[k].name; k++) {
                if (bsd_name_eq(e[i].name, kernel_exports[k].name)) {
                    e[i].addr = kernel_exports[k].addr;
                    found++;
                    break;
                }
            }
        }
        return found;
    }
    return 0;                       /* no table: an ordinary image */
}

/* Load an ELF64 image (used by `hello` and `run <elf>`). */
static int load_elf_image(const uint8_t *file, uint64_t fsize, int verbose,
                          uint64_t *out_entry) {
    if (fsize < sizeof(Elf64_Ehdr)) {
        if (verbose) term_print_c("run: file too small for an ELF64\n", 2);
        return -1;
    }
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    if (ehdr->e_ident[4] != 2) {
        if (verbose) term_print_c("run: not a 64-bit ELF\n", 2);
        return -1;
    }

    uint64_t base_vaddr = ~(uint64_t)0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type == ELF_PT_LOAD && ph->p_vaddr < base_vaddr)
            base_vaddr = ph->p_vaddr;
    }
    if (base_vaddr == ~(uint64_t)0) {
        if (verbose) term_print_c("run: no loadable segments\n", 2);
        return -1;
    }

    for (uint32_t i = 0; i < APP_MEM_SIZE; i++)
        app_memory[i] = 0;

    uint64_t span = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD) continue;
        uint64_t offset = ph->p_vaddr - base_vaddr;
        if (offset + ph->p_memsz > APP_MEM_SIZE) {
            if (verbose) term_print_c("run: segment too large\n", 2);
            return -1;
        }
        const uint8_t *src = file + ph->p_offset;
        uint8_t *dst = app_memory + offset;
        for (uint64_t j = 0; j < ph->p_filesz; j++)
            dst[j] = src[j];
        if (offset + ph->p_memsz > span) span = offset + ph->p_memsz;
    }

    /* Imports are a property of the image, not of the container it arrived
     * in, and `hello` ships here as a plain ELF — so this belongs on both
     * load paths, not just the .bsd one. */
    int nimp = bsd_resolve_imports(app_memory, span);
    if (nimp < 0) {
        if (verbose) term_print_c("run: malformed import table\n", 2);
        return -1;
    }
    if (nimp > 0) {
        serial_puts("[vextro] elf: resolved ");
        serial_put_dec((uint32_t)nimp);
        serial_puts(" imported symbols\n");
    }

    if (verbose) term_print_c("loading ELF64: ", 3);
    *out_entry = (uint64_t)(uintptr_t)(app_memory +
                                       (ehdr->e_entry - base_vaddr));
    return 0;
}

/* Load a .bsd image — the format every app store package uses. */
static int load_bsd_image(const uint8_t *file, uint64_t fsize, int verbose,
                          uint64_t *out_entry) {
    /* Copy the header out byte-wise: the filesystem cache hands back a
     * plain byte buffer and we do not want to assume its alignment. */
    bsd_header_t h;
    uint8_t *hp = (uint8_t *)&h;
    if (fsize < sizeof(h)) {
        if (verbose) term_print_c("run: file too small for a .bsd header\n", 2);
        return -1;
    }
    for (uint64_t i = 0; i < sizeof(h); i++) hp[i] = file[i];

    const char *bad = bsd_validate(&h, fsize);
    if (bad) {
        if (verbose) {
            term_print_c("run: ", 2);
            term_print_c(bad, 2);
            term_print("\n");
        }
        return -1;
    }
    if (bsd_image_span(&h) > APP_MEM_SIZE) {
        if (verbose) term_print_c("run: image too large for the app arena\n", 2);
        return -1;
    }

    for (uint32_t i = 0; i < APP_MEM_SIZE; i++)
        app_memory[i] = 0;

    uint64_t base = h.text_vaddr;
    uint8_t *dst = app_memory + (h.text_vaddr - base);
    for (uint64_t i = 0; i < h.text_size; i++)
        dst[i] = file[h.text_off + i];

    if (h.data_size) {
        dst = app_memory + (h.data_vaddr - base);
        for (uint64_t i = 0; i < h.data_size; i++)
            dst[i] = file[h.data_off + i];
    }
    /* .bss needs no work: the arena was just zeroed. */

    /* Lend the image whatever kernel functions it asked for, before it can
     * run and call through them. */
    int nimp = bsd_resolve_imports(app_memory, bsd_image_span(&h));
    if (nimp < 0) {
        if (verbose) term_print_c("run: malformed import table\n", 2);
        return -1;
    }
    if (nimp > 0) {
        serial_puts("[vextro] .bsd: resolved ");
        serial_put_dec((uint32_t)nimp);
        serial_puts(" imported symbols\n");
    }

    if (verbose) term_print_c("loading .bsd: ", 3);
    *out_entry = (uint64_t)(uintptr_t)(app_memory + (h.entry - base));
    return 0;
}

static int execute_bin_internal(const char *filepath, int verbose) {
    uint64_t fsize = 0;
    const void *fdata = fs_read_file(filepath, &fsize);
    if (!fdata || fsize < 8) {
        if (verbose) {
            term_print_c("run: file not found: ", 2);
            term_print_c(filepath, 2);
            term_print("\n");
        }
        return -1;
    }

    const uint8_t *file = (const uint8_t *)fdata;
    uint64_t entry_addr = 0;
    int rc;

    if (file[0] == (uint8_t)BSD_MAGIC0 && file[1] == (uint8_t)BSD_MAGIC1 &&
        file[2] == (uint8_t)BSD_MAGIC2 && file[3] == (uint8_t)BSD_MAGIC3) {
        rc = load_bsd_image(file, fsize, verbose, &entry_addr);
    } else if (file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' &&
               file[3] == 'F') {
        rc = load_elf_image(file, fsize, verbose, &entry_addr);
    } else {
        if (verbose)
            term_print_c("run: not a .bsd or ELF64 executable\n", 2);
        return -1;
    }
    if (rc != 0) return -1;

    if (verbose) {
        term_print_c(filepath, 3);
        term_print("\n");
    }

    uint64_t stack_top = (uint64_t)(uintptr_t)(app_stack + sizeof(app_stack));

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
/* After browser.h: reuses brw_fold_cp for Unicode folding. Before apps.h,
 * which is where the Wikipedia window uses it. */
#include "wikidoc.h"
#include "apps.h"
#include "store.h"

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
        brw_navigate_no_hist("vextro://home");
    if (kind == WK_FILES)
        exp_scan();
    if (kind == WK_STORE)
        store_restat();

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
    case WK_STORE:
        store_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_IMAGE:
        img_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
        break;
    case WK_WIKI:
        wiki_draw(buf, w, h, cx, cy, cw, chh, desktop_tick, focused);
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
        case WK_STORE:
            store_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_IMAGE:
            img_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_WIKI:
            wiki_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
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

/*
 * Every window, bottom of the stack upward.
 *
 * Shadow, frame, content -- in that order and per window, not shadows for
 * all of them and then frames for all of them. Drawn in one pass over the
 * stack, a window's shadow lands on whatever is already beneath it and is
 * then covered by whatever is drawn after, which is what makes the stack
 * read as a stack. Hoisting the shadows into a pass of their own would
 * put the topmost window's shadow underneath every other window, which is
 * exactly the depth cue inverted.
 */
static void wm_draw_all(uint32_t *buf, uint32_t w, uint32_t h) {
    for (int i = 0; i < wm_stack_n; i++) {
        int kind = wm_stack[i];
        if (spawn_anim.active && spawn_anim.kind == kind)
            continue;   /* revealed when the animation lands */
        const win_t *win = &wins[kind];
        gfx_shadow(buf, w, h, win->x, win->y, win->w, win->h);
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

/* Tracks the back buffer's bound — see BUF_MAX_W in kernel.c */
#ifndef WALL_MAX_W
#define WALL_MAX_W 1920
#endif
#ifndef WALL_MAX_H
#define WALL_MAX_H 1080
#endif

static uint32_t wallpaper[WALL_MAX_W * WALL_MAX_H];
static int      wall_cur_theme = 0;
static uint32_t wall_gen_w = 0;
static uint32_t wall_gen_h = 0;

/*
 * The dragon.
 *
 * Placed and scaled by the caller rather than centred on the buffer,
 * because there are two of them now: the wallpaper draws it full size in
 * the middle of the screen, and the boot animation draws it half size and
 * off to the left, so its head has somewhere to breathe. Same polygons
 * either way -- the thing on the login screen's other side is the same
 * drawing, not a second one that has to be kept in step.
 *
 * num/den scale every coordinate about (cx,cy); pass 1,1 for full size.
 */
static void wall_dragon(uint32_t *buf, uint32_t w, uint32_t h,
                        int cx, int cy, int num, int den,
                        uint32_t body, uint32_t accent, uint32_t bright) {
#define DX(v) (cx + ((v) * num) / den)
#define DY(v) (cy + ((v) * num) / den)
#define DW(v) (((v) * num) / den < 1 ? 1 : ((v) * num) / den)

    /* wing */
    gfx_tri(buf, w, h, DX(+80),DY(-50),  DX(-300),DY(-350), DX(-20),DY(-35),   body);
    gfx_tri(buf, w, h, DX(-20),DY(-35),  DX(-300),DY(-350), DX(-360),DY(-15),  body);
    gfx_tri(buf, w, h, DX(-300),DY(-350), DX(-360),DY(-15), DX(-400),DY(-120), body);
    gfx_line(buf, w, h, DX(+60),DY(-45),  DX(-280),DY(-330), DW(2), accent);
    gfx_line(buf, w, h, DX(+60),DY(-45),  DX(-380),DY(-100), DW(2), accent);
    gfx_line(buf, w, h, DX(-340),DY(-10), DX(-380),DY(-100), DW(2), accent);

    /* tail */
    gfx_tri(buf, w, h, DX(-180),DY(+20),  DX(-160),DY(+120), DX(-290),DY(+80),  body);
    gfx_tri(buf, w, h, DX(-180),DY(+20),  DX(-290),DY(+80),  DX(-420),DY(+50),  body);
    gfx_tri(buf, w, h, DX(-290),DY(+80),  DX(-420),DY(+50),  DX(-480),DY(-50),  body);
    gfx_tri(buf, w, h, DX(-460),DY(-40),  DX(-485),DY(-25),  DX(-465),DY(+5),   accent);

    /* body */
    gfx_tri(buf, w, h, DX(+80),DY(-50),   DX(+130),DY(+80),  DX(-20),DY(-35),   body);
    gfx_tri(buf, w, h, DX(+130),DY(+80),  DX(-20),DY(-35),   DX(+80),DY(+100),  body);
    gfx_tri(buf, w, h, DX(-20),DY(-35),   DX(+80),DY(+100),  DX(-180),DY(+20),  body);
    gfx_tri(buf, w, h, DX(+80),DY(+100),  DX(-180),DY(+20),  DX(-120),DY(+140), body);
    gfx_tri(buf, w, h, DX(-180),DY(+20),  DX(-120),DY(+140), DX(-160),DY(+120), body);

    /* neck + head */
    gfx_tri(buf, w, h, DX(+190),DY(-95),  DX(+340),DY(+25),  DX(+80),DY(-50),   body);
    gfx_tri(buf, w, h, DX(+340),DY(+25),  DX(+80),DY(-50),   DX(+130),DY(+80),  body);
    gfx_tri(buf, w, h, DX(+260),DY(-210), DX(+400),DY(-30),  DX(+190),DY(-95),  body);
    gfx_tri(buf, w, h, DX(+400),DY(-30),  DX(+190),DY(-95),  DX(+340),DY(+25),  body);
    gfx_tri(buf, w, h, DX(+400),DY(-30),  DX(+340),DY(+25),  DX(+390),DY(+60),  body);

    /* horns */
    gfx_tri(buf, w, h, DX(+255),DY(-205), DX(+225),DY(-320), DX(+200),DY(-180), body);
    gfx_tri(buf, w, h, DX(+185),DY(-175), DX(+150),DY(-290), DX(+150),DY(-150), body);
    gfx_tri(buf, w, h, DX(+225),DY(-320), DX(+240),DY(-270), DX(+210),DY(-265), accent);
    gfx_tri(buf, w, h, DX(+150),DY(-290), DX(+165),DY(-245), DX(+135),DY(-240), accent);

    /* eye */
    gfx_tri(buf, w, h, DX(+340),DY(-80),  DX(+352),DY(-60),  DX(+340),DY(-40),  bright);
    gfx_tri(buf, w, h, DX(+340),DY(-80),  DX(+328),DY(-60),  DX(+340),DY(-40),  bright);

    /* spine ridges */
    gfx_tri(buf, w, h, DX(+55),DY(-53),   DX(+40),DY(-78),   DX(+25),DY(-53),   accent);
    gfx_tri(buf, w, h, DX(+15),DY(-42),   DX(+0),DY(-65),    DX(-15),DY(-42),   accent);
    gfx_tri(buf, w, h, DX(-25),DY(-38),   DX(-40),DY(-58),   DX(-55),DY(-38),   accent);
    gfx_tri(buf, w, h, DX(-70),DY(-25),   DX(-85),DY(-45),   DX(-100),DY(-25),  accent);
    gfx_tri(buf, w, h, DX(-115),DY(-12),  DX(-130),DY(-32),  DX(-145),DY(-12),  accent);

    /* belly scales */
    gfx_tri(buf, w, h, DX(+100),DY(+95),  DX(+80),DY(+112),  DX(+100),DY(+112), accent);
    gfx_tri(buf, w, h, DX(+65),DY(+110),  DX(+45),DY(+125),  DX(+65),DY(+125),  accent);
    gfx_tri(buf, w, h, DX(+30),DY(+122),  DX(+10),DY(+136),  DX(+30),DY(+136),  accent);
    gfx_tri(buf, w, h, DX(-5),DY(+132),   DX(-25),DY(+145),  DX(-5),DY(+145),   accent);

    /* legs + claws */
    gfx_line(buf, w, h, DX(+90),DY(+95),   DX(+140),DY(+220), DW(5), body);
    gfx_line(buf, w, h, DX(+140),DY(+220), DX(+110),DY(+300), DW(4), body);
    gfx_line(buf, w, h, DX(-100),DY(+130), DX(-80),DY(+230),   DW(5), body);
    gfx_line(buf, w, h, DX(-80),DY(+230),  DX(-120),DY(+300), DW(4), body);
    gfx_tri(buf, w, h, DX(+110),DY(+298), DX(+95),DY(+318),  DX(+108),DY(+318), accent);
    gfx_tri(buf, w, h, DX(+110),DY(+298), DX(+112),DY(+320), DX(+122),DY(+315), accent);
    gfx_tri(buf, w, h, DX(+110),DY(+298), DX(+128),DY(+312), DX(+132),DY(+302), accent);
    gfx_tri(buf, w, h, DX(-120),DY(+298), DX(-135),DY(+318), DX(-122),DY(+318), accent);
    gfx_tri(buf, w, h, DX(-120),DY(+298), DX(-118),DY(+320), DX(-108),DY(+315), accent);
    gfx_tri(buf, w, h, DX(-120),DY(+298), DX(-102),DY(+312), DX(-98),DY(+302),  accent);
#undef DX
#undef DY
#undef DW
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
    wall_dragon(wallpaper, w, h, (int)w / 2, ((int)h + MENUBAR_H) / 2,
                1, 1, body, accent, C_GOLD);

    /* Signature bottom-left, with the rule under it measured rather than
     * guessed — it used to be 120px because that happened to be how wide the
     * old name set, and a shorter one would have left the rule hanging. */
    {
        const char *sig = "VEXTRO 9";
        const int32_t sw = ttf_text_width(sig, 15);
        ttf_draw_string(wallpaper, (int)w, (int)h, 24, (int)h - 46,
                        sig, gfx_mix(C_GOLD, bot, 140), 15);
        gfx_rect(wallpaper, w, h, 24, (int32_t)h - 24, sw, 1,
                 gfx_mix(C_GOLD, bot, 90));
    }

    wall_gen_w = w;
    wall_gen_h = h;
}

static void wallpaper_set_theme(int idx) {
    if (idx < 0 || idx >= WALL_THEME_COUNT) return;
    wall_cur_theme = idx;
    wallpaper_regen(scr_w_cache, scr_h_cache);
}

/* ===== 8. MENUBAR ===== */

/* action codes above the window kinds */
#define MENU_ACT_REBOOT   100
#define MENU_ACT_SHUTDOWN 101
#define MENU_ACT_LOGOUT   102

#define MENU_ACT_APP_BASE 200      /* + index into store_inst[] */

typedef struct {
    const char *label;
    int action;      /* >=0 window kind; 100/101 power; 200+ app; -1 sep */
} menu_item_t;

static const menu_item_t menu_system[] = {
    { "About Vextro", WK_ABOUT },
    { "Settings",       WK_SETTINGS },
    { "-",              -1 },
    { "Log Out",        MENU_ACT_LOGOUT },
    { "Restart",        MENU_ACT_REBOOT },
    { "Shut Down",      MENU_ACT_SHUTDOWN },
};

/* The Apps menu is rebuilt each frame so installed packages appear in it. */
#define MENU_APPS_MAX (10 + STORE_MAX_INST)

static menu_item_t menu_apps[MENU_APPS_MAX];
static int         menu_apps_n = 0;

#define MENU_COUNT 2
static const char *menu_labels[MENU_COUNT] = { "Vextro", "Apps" };
static const menu_item_t *menu_items[MENU_COUNT] = { menu_system, menu_apps };
static int menu_item_count[MENU_COUNT] = { 5, 0 };

static void menu_rebuild(void) {
    int n = 0;
    menu_apps[n].label = "Terminal";     menu_apps[n++].action = WK_TERM;
    menu_apps[n].label = "Browser";      menu_apps[n++].action = WK_BROWSER;
    menu_apps[n].label = "Files";        menu_apps[n++].action = WK_FILES;
    menu_apps[n].label = "App Store";    menu_apps[n++].action = WK_STORE;
    menu_apps[n].label = "Photos";       menu_apps[n++].action = WK_IMAGE;
    menu_apps[n].label = "Wikipedia";    menu_apps[n++].action = WK_WIKI;
    menu_apps[n].label = "-";            menu_apps[n++].action = -1;
    menu_apps[n].label = "Goldsmith";    menu_apps[n++].action = WK_PAINT;
    menu_apps[n].label = "Monolith";     menu_apps[n++].action = WK_SYSMON;
    menu_apps[n].label = "Matrix";       menu_apps[n++].action = WK_MATRIX;
    menu_apps[n].label = "hello.elf";    menu_apps[n++].action = WK_HELLO;

    if (store_inst_count > 0) {
        menu_apps[n].label = "-";        menu_apps[n++].action = -1;
        for (int i = 0; i < store_inst_count && n < MENU_APPS_MAX; i++) {
            menu_apps[n].label = store_inst[i].name;
            menu_apps[n].action = MENU_ACT_APP_BASE + i;
            n++;
        }
    }
    menu_apps_n = n;
    menu_item_count[1] = n;
}

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
    if (action >= MENU_ACT_APP_BASE) {
        store_launch_inst(action - MENU_ACT_APP_BASE);
    } else if (action >= 0 && action < WK_COUNT) {
        if (action == WK_HELLO) {
            silent_launch = 1;
            execute_bin_internal("hello", 0);
            silent_launch = 0;
        }
        wm_open(action);
    } else if (action == MENU_ACT_LOGOUT) {
        want_logout = 1;
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
        /*
         * The clock and date come off the CMOS, which is slow enough
         * that reading it on every frame is a waste — almost every read
         * returns what the last one did.  Sample twice a second instead.
         *
         * Keyed to PIT ticks, not frames: frames are not a unit of time,
         * and under a heavy background load the desktop drops to a few a
         * second, which would leave the clock visibly stopped.
         */
        static char clk[10] = "";
        static char dt[16]  = "";
        static uint32_t clock_stamp = 0;
        if (clk[0] == '\0' || sys_ticks - clock_stamp >= 30) {
            clock_stamp = sys_ticks;
            clock_string(clk);
            date_string(dt);
        }

        int cw2 = ttf_text_width(clk, 14);
        int32_t x = (int32_t)w - cw2 - 16;
        ttf_draw_string(buf, (int)w, (int)h, x, 6, clk, C_TEXT, 14);

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

/* A dock slot is either a built-in window kind or an installed app
 * (which runs into the shared canvas window, WK_HELLO). */
typedef struct {
    int kind;
    int inst;      /* index into store_inst[], or -1 for a built-in */
} dock_item_t;

static const int dock_base_kinds[DOCK_BASE_COUNT] = {
    WK_TERM, WK_BROWSER, WK_FILES, WK_STORE, WK_IMAGE, WK_WIKI, WK_PAINT,
    WK_SYSMON, WK_MATRIX, WK_HELLO, WK_SETTINGS,
};

static dock_item_t dock_items[DOCK_MAX_ITEMS];

static void dock_rebuild(void) {
    int n = 0;
    for (int i = 0; i < DOCK_BASE_COUNT; i++) {
        dock_items[n].kind = dock_base_kinds[i];
        dock_items[n].inst = -1;
        n++;
    }
    for (int i = 0; i < store_inst_count && n < DOCK_MAX_ITEMS; i++) {
        dock_items[n].kind = WK_HELLO;
        dock_items[n].inst = i;
        n++;
    }
    dock_item_count = n;
}

static const char *dock_item_name(int idx) {
    if (dock_items[idx].inst >= 0)
        return store_inst[dock_items[idx].inst].name;
    if (dock_items[idx].kind == WK_HELLO) return "hello";
    return wk_meta[dock_items[idx].kind].title;
}

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
    case WK_IMAGE:
        /* a framed photo: horizon, sun, and a hill */
        gfx_rect_outline(buf, w, h, cx - q - 3, cy - q - 1, 2 * q + 6,
                         2 * q + 2, C_TEXT);
        gfx_circle(buf, w, h, cx + q - 1, cy - q / 2, 2, C_GOLD);
        gfx_tri(buf, w, h, cx - q - 2, cy + q, cx - 1, cy - q / 2 - 1,
                cx + q / 2, cy + q, C_GOLD_DIM);
        gfx_tri(buf, w, h, cx - 2, cy + q, cx + q / 2 + 2, cy - 1,
                cx + q + 2, cy + q, C_GOLD);
        break;
    case WK_WIKI:
        /* an open book: two leaves meeting at the spine */
        gfx_tri(buf, w, h, cx, cy - q, cx - q - 3, cy - q + 2,
                cx - q - 3, cy + q, C_TEXT);
        gfx_tri(buf, w, h, cx, cy - q, cx - q - 3, cy + q, cx, cy + q - 1,
                C_TEXT);
        gfx_tri(buf, w, h, cx, cy - q, cx + q + 3, cy - q + 2,
                cx + q + 3, cy + q, C_GOLD_DIM);
        gfx_tri(buf, w, h, cx, cy - q, cx + q + 3, cy + q, cx, cy + q - 1,
                C_GOLD_DIM);
        gfx_rect(buf, w, h, cx - 1, cy - q, 2, 2 * q, C_GOLD);
        break;
    case WK_STORE:
        /* a shopping bag with a download arrow in it */
        gfx_rect_outline(buf, w, h, cx - q - 2, cy - q + 2, 2 * q + 4,
                         2 * q + 1, C_TEXT);
        gfx_line(buf, w, h, cx - q / 2 - 1, cy - q + 2,
                 cx - q / 2 - 1, cy - q - 3, 1, C_TEXT);
        gfx_line(buf, w, h, cx + q / 2 + 1, cy - q + 2,
                 cx + q / 2 + 1, cy - q - 3, 1, C_TEXT);
        gfx_line(buf, w, h, cx - q / 2 - 1, cy - q - 3,
                 cx + q / 2 + 1, cy - q - 3, 1, C_TEXT);
        gfx_rect(buf, w, h, cx - 1, cy - q + 5, 2, q + q / 2 - 6, C_GOLD);
        gfx_line(buf, w, h, cx - 4, cy + q / 2 - 3, cx, cy + q / 2 + 1,
                 1, C_GOLD);
        gfx_line(buf, w, h, cx + 4, cy + q / 2 - 3, cx, cy + q / 2 + 1,
                 1, C_GOLD);
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

/* Draw the pictogram for a dock slot — installed apps borrow the store's
 * category glyphs so the dock icon matches the storefront card. */
static void dock_draw_item_glyph(uint32_t *buf, uint32_t w, uint32_t h,
                                 int idx, int32_t x, int32_t y, int32_t sz) {
    if (dock_items[idx].inst >= 0) {
        store_icon_glyph(buf, w, h, x, y, sz,
                         store_inst[dock_items[idx].inst].icon);
        return;
    }
    dock_draw_glyph(buf, w, h, dock_items[idx].kind, x, y, sz);
}

static void dock_launch(int idx) {
    int32_t ix, iy, iw, ih;
    dock_icon_rect(scr_w_cache, idx, &ix, &iy, &iw, &ih);

    if (dock_items[idx].inst >= 0) {
        int was_open = wins[WK_HELLO].open;
        store_launch_inst(dock_items[idx].inst);
        if (!was_open && wins[WK_HELLO].open)
            spawn_anim_start(WK_HELLO, ix + iw / 2, iy + ih / 2);
        return;
    }

    int kind = dock_items[idx].kind;
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

    for (int i = 0; i < dock_item_count; i++) {
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
    for (int i = 0; i < dock_item_count; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, i, &ix, &iy, &iw, &ih);
        int kind = dock_items[i].kind;
        int hot = (mx >= ix - 4 && mx < ix + iw + 4 &&
                   my >= iy - 4 && my < iy + ih + 4);
        if (hot) hover = i;

        /* icon tile */
        gfx_rect(buf, w, h, ix, iy, iw, ih, hot ? 0x262C3Eu : 0x1C2130u);
        gfx_rect_outline(buf, w, h, ix, iy, iw, ih,
                         hot ? C_GOLD_DIM : 0x2A3040u);
        dock_draw_item_glyph(buf, w, h, i, ix, iy, iw);

        /* separator before the installed-app section */
        if (i == DOCK_BASE_COUNT && dock_item_count > DOCK_BASE_COUNT) {
            if (dock_cfg.edge == DOCK_EDGE_BOTTOM)
                gfx_rect(buf, w, h, ix - 9, iy + 2, 1, ih - 4, 0x353C50u);
            else
                gfx_rect(buf, w, h, ix + 2, iy - 9, iw - 4, 1, 0x353C50u);
        }

        /* Running indicator.  Canvas apps all share WK_HELLO, so the one
         * that is actually loaded is identified by the window title. */
        int running;
        if (kind == WK_HELLO)
            running = wins[WK_HELLO].open &&
                      str_eq(app_win_title, dock_item_name(i));
        else
            running = wm_is_open(kind);

        if (running) {
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
        const char *name = dock_item_name(hover);
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
    else if (str_eq(name, "store") || str_eq(name, "agora") ||
             str_eq(name, "apps")) kind = WK_STORE;
    else if (str_eq(name, "photos") || str_eq(name, "image") ||
             str_eq(name, "images")) kind = WK_IMAGE;
    else if (str_eq(name, "wikipedia") || str_eq(name, "wiki") ||
             str_eq(name, "encyclopedia")) kind = WK_WIKI;
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
    if (wm_focus == WK_STORE) {
        store_key(ch);
        return;
    }
    if (wm_focus == WK_IMAGE) {
        img_key(ch);
        return;
    }
    if (wm_focus == WK_WIKI) {
        wiki_key(ch);
        return;
    }
    if (wm_focus == WK_SETTINGS) {
        if (ch == 27) { wm_close(WK_SETTINGS); return; }
        settings_key(ch);
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

/*
 * Route wheel notches to the focused window, positive towards the top of
 * the document.  Each window already knows how to scroll itself for the
 * keyboard, so this is mostly a matter of choosing a step: a notch is
 * three lines of terminal, or a comfortable fraction of a page elsewhere.
 */
static void desktop_wheel_input(int32_t notches) {
    if (notches == 0 || menu_open_idx >= 0) return;

    int32_t mag = notches < 0 ? -notches : notches;
    if (mag > 8) mag = 8;                       /* a flick should not hurl */
    int32_t step = notches > 0 ? mag : -mag;

    switch (wm_focus) {
    case WK_TERM:
        term_scroll_key(step > 0 ? KEY_PGUP : KEY_PGDN, (int)(mag * 3));
        break;
    case WK_BROWSER:
        brw_scroll_by((int)(-step * 48), brw_view_h_cache);
        break;
    case WK_STORE:
        store_scroll_by((int)(-step * 48));
        break;
    case WK_WIKI:
        /* Scrolls the article when one is open, and otherwise walks the
         * result list. Not while the chat panel has the window. */
        if (wiki_mode == 0) {
            if (wiki_view == 1)
                wiki_scroll_by((int)(-step * 48));
            else
                for (int32_t i = 0; i < mag; i++)
                    wiki_key(step > 0 ? KEY_UP : KEY_DOWN);
        }
        break;
    case WK_IMAGE:
        for (int32_t i = 0; i < mag; i++)
            img_key(step > 0 ? KEY_UP : KEY_DOWN);
        break;
    default:
        break;
    }
}

/* ===== the model opt-in =====
 *
 * Shown once per account, on the first login, over the desktop. It takes
 * the whole screen's input while it is up: a modal that could be clicked
 * behind is not a choice, it is an obstacle.
 */
#define AID_W 460
#define AID_H 190

static int ai_dialog_hit(int32_t mx, int32_t my, uint32_t w, uint32_t h,
                         int which) {
    int32_t x0 = ((int32_t)w - AID_W) / 2;
    int32_t y0 = ((int32_t)h - AID_H) / 2;
    int32_t by = y0 + AID_H - 52;
    int32_t bx = which == 0 ? x0 + AID_W - 230 : x0 + AID_W - 116;
    return mx >= bx && mx < bx + 100 && my >= by && my < by + 34;
}

static void ai_dialog_draw(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t mx, int32_t my) {
    /* dim what is behind, so the dialog reads as the only live thing */
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t p = buf[i];
        buf[i] = ((p >> 1) & 0x7F7F7Fu);
    }

    int32_t x0 = ((int32_t)w - AID_W) / 2;
    int32_t y0 = ((int32_t)h - AID_H) / 2;

    gfx_rect(buf, w, h, x0, y0, AID_W, AID_H, 0x14161Eu);
    gfx_rect_outline(buf, w, h, x0, y0, AID_W, AID_H, C_GOLD);
    gfx_rect(buf, w, h, x0, y0, AID_W, 2, C_GOLD);

    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 22,
                    "Enable AI features?", C_GOLD, 18);
    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 58,
                    "This machine can run a language model on the CPU to",
                    C_TEXT_DIM, 13);
    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 78,
                    "answer questions from the offline encyclopedia.",
                    C_TEXT_DIM, 13);
    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 102,
                    "It loads 380 MB at every boot. You can leave it off.",
                    0x707888u, 12);

    for (int i = 0; i < 2; i++) {
        int32_t by = y0 + AID_H - 52;
        int32_t bx = i == 0 ? x0 + AID_W - 230 : x0 + AID_W - 116;
        int hot = ai_dialog_hit(mx, my, w, h, i);
        int yes = (i == 1);
        gfx_rect(buf, w, h, bx, by, 100, 34, yes ? 0x2A2410u : 0x1B1E26u);
        gfx_rect_outline(buf, w, h, bx, by, 100, 34,
                         hot ? C_GOLD : (yes ? C_GOLD_DIM : 0x3A4050u));
        const char *lbl = yes ? "Enable" : "No thanks";
        int tw = ttf_text_width(lbl, 14);
        ttf_draw_string(buf, (int)w, (int)h, bx + (100 - tw) / 2, by + 9,
                        lbl, yes ? C_GOLD : C_TEXT_DIM, 14);
    }
}

static void desktop_render(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t mx, int32_t my, uint8_t buttons) {
    desktop_tick++;
    scr_w_cache = w;
    scr_h_cache = h;

    dock_rebuild();
    menu_rebuild();
    dock_recalc(w, h);

    if (wall_gen_w != w || wall_gen_h != h)
        wallpaper_regen(w, h);

    /* async engines */
    term_async_poll();
    brw_poll();
    store_poll();
    ai_poll();
    wiki_poll();

    /* ---- the opt-in, while it is unanswered ---- */
    if (ai_enabled < 0) {
        uint8_t lmb0 = buttons & 1;
        int click = lmb0 && !desk_prev_lmb;
        desk_prev_lmb = lmb0;

        for (uint32_t i = 0; i < w * h; i++) buf[i] = wallpaper[i];
        menubar_draw(buf, w, h, mx, my);
        dock_draw(buf, w, h, mx, my);
        ai_dialog_draw(buf, w, h, mx, my);

        if (click) {
            if (ai_dialog_hit(mx, my, w, h, 1)) {
                ai_choice_save(1);
                ai_autoload_start();
                serial_puts("[ai] enabled by the user\n");
            } else if (ai_dialog_hit(mx, my, w, h, 0)) {
                ai_choice_save(0);
                serial_puts("[ai] declined; model not loaded\n");
            }
        }
        return;                      /* nothing else runs while it is up */
    }

    /* ---- input ---- */
    uint8_t lmb = buttons & 1;
    int consumed = menu_mouse(mx, my, lmb, desk_prev_lmb);
    if (!consumed)
        consumed = dock_mouse(mx, my, lmb, desk_prev_lmb);
    wm_update(mx, my, lmb, desk_prev_lmb, consumed);
    desk_prev_lmb = lmb;

    /*
     * ---- draw ----
     *
     * One pass, back to front, into the back buffer -- never into the
     * panel. Order is the whole of it:
     *
     *   the wallpaper clears the frame, which is why there is no separate
     *     clear: every pixel is written, so clearing first would be a
     *     second full-screen write for nothing;
     *   then the window stack, each window casting its shadow onto what
     *     is already under it before its own frame and content go down;
     *   then the chrome, which is always on top of every window;
     *   and the pointer, which is composited inside the flip rather than
     *     here -- see vga_flip -- because drawing it into this buffer is
     *     what made it shimmer.
     *
     * The frame reaches the panel from vga_flip, which diffs against the
     * previous one and writes only the rows that changed, through the
     * Gen9 blitter where there is one and a plain copy where there is
     * not.
     */
    for (uint32_t i = 0; i < w * h; i++)
        buf[i] = wallpaper[i];

    wm_draw_all(buf, w, h);
    spawn_anim_draw(buf, w, h);
    menubar_draw(buf, w, h, mx, my);
    menu_dropdown_draw(buf, w, h, mx, my);
    dock_draw(buf, w, h, mx, my);
}


/* ===== 12. SESSIONS =====
 *
 * App state is global and outlives a window being closed, which is what
 * makes reopening the terminal feel like returning to it rather than
 * starting over. Across a *logout* that same property is a leak: the next
 * person to log in would inherit the last one's shell history, browser
 * address, open documents and drawing.
 *
 * Everything cleared here is in-memory only. Files on the volume are not
 * touched -- a home directory is the point, not a scratch space.
 */
static void session_end(void) {
    /* every window shut, nothing focused */
    for (int k = 0; k < WK_COUNT; k++) wins[k].open = 0;
    wm_stack_n = 0;
    wm_focus = -1;
    wm_drag = -1;
    menu_open_idx = -1;

    /* terminal: history, scrollback and working directory */
    term_hist_count = 0;
    term_hist_pos = -1;
    term_input_len = 0;
    term_input[0] = '\0';
    term_clear();
    str_copy(term_cwd, "/", sizeof(term_cwd));

    /* browser: history and current page */
    brw_hist_n = 0;
    brw_line_count = 0;
    brw_scroll = 0;
    str_copy(brw_addr, "vextro://home", BRW_ADDR_MAX);
    brw_title[0] = '\0';

    /* encyclopedia: reading position, trail and search */
    wiki_view = 0;
    wiki_hist_n = 0;
    wiki_scroll = 0;
    wiki_art_path[0] = '\0';
    wiki_art_title[0] = '\0';
    wiki_qlen = 0;
    wiki_query[0] = '\0';
    wiki_hit_count = 0;
    wiki_sel = 0;
    wiki_mode = 0;

    /* files, photos, and the canvas */
    str_copy(exp_path, "/", sizeof(exp_path));
    exp_selected = -1;
    img_loaded = 0;
    img_name[0] = '\0';
    for (uint32_t i = 0; i < PAINT_MAX_W * PAINT_MAX_H; i++)
        paint_canvas[i] = 0xFFFFFFu;
}

/*
 * Start a session as `name`: their home directory becomes the working
 * directory for the shell and the file browser.
 */
static void session_begin(const char *name) {
    session_end();

    char home[80];
    str_copy(home, "/home/", sizeof(home));
    str_append(home, name, sizeof(home));

    /* Only if it is really there -- a volume that could not be written
     * when the account was made should land in / rather than somewhere
     * that does not exist. */
    int is_dir = 0;
    if (fs_stat(home, 0, &is_dir) && is_dir) {   /* returns 1 when found */
        str_copy(term_cwd, home, sizeof(term_cwd));
        str_copy(exp_path, home, sizeof(exp_path));
    }

    /* Their answer about the model, if they have given one. */
    ai_enabled = -1;
    {
        char cfg[96];
        str_copy(cfg, home, sizeof(cfg));
        str_append(cfg, "/settings.cfg", sizeof(cfg));
        uint64_t n = 0;
        const void *d = fs_read_file(cfg, &n);
        if (d && n >= 4) {
            const char *p = (const char *)d;
            for (uint64_t i = 0; i + 3 < n; i++)
                if (p[i]=='a' && p[i+1]=='i' && p[i+2]=='=') {
                    ai_enabled = (p[i+3] == '1') ? 1 : 0;
                    break;
                }
        }
    }
}

/* Record the answer so it is only asked once. */
static void ai_choice_save(int on) {
    ai_enabled = on;
    if (user_current < 0) return;
    char cfg[96];
    user_home(user_current, cfg, sizeof(cfg));
    str_append(cfg, "/settings.cfg", sizeof(cfg));
    fs_write_file(cfg, on ? "ai=1\n" : "ai=0\n", 5);
}

#endif /* DESKTOP_H */
