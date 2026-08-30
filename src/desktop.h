#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include "ttf.h"
#include "gfx.h"
#include "sysx.h"
#include "fat32.h"
#include "exfat.h"
#include "vx_format.h"
#include "pe.h"
#include "registry.h"
#include "sci.h"
/* The Linux subset's seam: the signal state hung off an address space,
 * the personality flag, and the router this file's system call switch
 * hands a Linux-numbered call to. src/sched/vls_core.c is the other side
 * and the two meet only here. */
#include "vls.h"

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
#define DOCK_BASE_COUNT 16
#define DOCK_MAX_ITEMS  25

/* The Show Desktop tab occupies the far end of the bar, past the last
 * launcher. It is the only thing that triggers Peek. */
#define DOCK_SHOWDESK_W 18

static int dock_item_count = DOCK_BASE_COUNT;

typedef struct {
    int32_t bar_y;      /* bottom edge: top y of bar; sides: top of column */
    int32_t bar_h;      /* thickness */
    int32_t bar_w;      /* length along the edge */
    int32_t icon_sz;
    int     edge;
} dock_config_t;

static dock_config_t dock_cfg = {
    /* 40, not the 32 this started at: the taskbar is a click target for
     * running windows now, not just a row of launchers, and Settings can
     * still take it back down to 24. */
    .bar_y = 0, .bar_h = 52, .bar_w = 420, .icon_sz = 40,
    .edge = DOCK_EDGE_BOTTOM,
};

/*
 * The icon size actually used, which is what Settings asked for shrunk
 * until the row fits the screen.
 *
 * Sixteen launchers at 40 px need 928 px of bar. The x86 default mode is
 * 1280 wide and never noticed; the ARM port's is 800, where the row ran
 * off both ends and the launchers at each end could not be clicked at
 * all. Fitting is computed rather than assumed, so it stays correct at
 * any mode and any number of installed apps.
 */
static int32_t dock_eff_isz = 40;

static void dock_recalc(uint32_t scr_w, uint32_t scr_h) {
    const int32_t along = (dock_cfg.edge == DOCK_EDGE_BOTTOM)
                          ? (int32_t)scr_w : (int32_t)scr_h;
    int32_t isz = dock_cfg.icon_sz;
    while (isz > 18 &&
           dock_item_count * (isz + 16) + 14 + DOCK_SHOWDESK_W > along - 16)
        isz -= 2;

    dock_eff_isz = isz;
    dock_cfg.bar_h = isz + 12;
    dock_cfg.bar_w = dock_item_count * (isz + 16) + 14 + DOCK_SHOWDESK_W;
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
    WK_CALC,
    WK_MEDIA,
    WK_SOLID,
    WK_CHIP8,
    WK_CHAMBER,
    WK_ABOUT,
    WK_COUNT
};

/*
 * Defined in shell.h, which cannot be included until after the apps --
 * the gadgets there read state the apps own. The apps, in turn, are what
 * record recent items, so the declaration has to come first and the
 * definition later.
 */
static void recent_push(int kind, const char *label, const char *path);

/* Reopening a remembered item is app-specific, so this is defined once
 * every app that handles a kind is in scope. The start menu and the jump
 * lists both reach it from above that point. */
static void desktop_open_recent(int kind, const char *path);

/*
 * The Action Center's ring is in shell.h too, but the subsystems that
 * report into it -- the store, the login loop, the network watch -- are
 * all in scope well before that. What an entry looks like is declared
 * here so they can file one; where the entries are kept is not their
 * business.
 */
#define NOTIFY_TEXT 72
enum { NOTE_INFO = 0, NOTE_GOOD, NOTE_WARN };
static void notify_push(int cat, const char *text);

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
    { "Ingot",             UI_SNAP(720), UI_SNAP(560) },
    { "Photos",            UI_SNAP(760), UI_SNAP(560) },
    /* Wide enough to read prose in: articles are laid out in this window
     * now rather than handed to the browser, and 520 was a search box. */
    { "Wikipedia",         UI_SNAP(780), UI_SNAP(580) },
    /* Taller since the Users pane joined it. */
    { "Settings",          UI_SNAP(470), UI_SNAP(560) },
    { "Calculator",        UI_SNAP(330), UI_SNAP(500) },
    { "Media Player",      UI_SNAP(560), UI_SNAP(420) },
    { "Solid",             UI_SNAP(520), UI_SNAP(440) },
    { "CHIP-8",            UI_SNAP(560), UI_SNAP(400) },
    { "Chamber",           UI_SNAP(600), UI_SNAP(540) },
    { "About Vextro",      UI_SNAP(380), UI_SNAP(270) },
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
#define FS_NTFS   3

static int fs_kind = FS_NONE;

/*
 * Splitting an absolute path into the directory holding the last
 * component and the component itself.
 *
 * Every NTFS write needs both: the driver's create, delete and mkdir
 * take a parent *record* and a name, because that is what an index
 * insert actually operates on. exFAT's writer re-walks the path
 * internally and hides this; doing the walk here instead means it
 * happens once and the record is the one the insert uses.
 *
 * Returns 0 on success. A path with no parent -- "/" -- is refused,
 * because there is nothing above the root to insert into.
 */
static int ntfs_split(const char *abs, uint64_t *out_dir, char *leaf,
                      int leaf_max) {
    int last = -1, n = 0;

    for (int i = 0; abs[i]; i++) if (abs[i] == '/') last = i;
    if (last < 0) return -1;

    while (abs[last + 1 + n] && n < leaf_max - 1) {
        leaf[n] = abs[last + 1 + n];
        n++;
    }
    leaf[n] = '\0';
    if (n == 0) return -1;              /* a trailing slash, or the root */

    {
        char dir[256];
        int i = 0;
        if (last == 0) {
            dir[0] = '/'; dir[1] = '\0';
        } else {
            while (i < last && i < (int)sizeof(dir) - 1) { dir[i] = abs[i]; i++; }
            dir[i] = '\0';
        }
        {
            int is_dir = 0;
            if (ntfs_lookup(dir, out_dir, 0, &is_dir, 0) != 0) return -1;
            if (!is_dir) return -1;
        }
    }
    return 0;
}

/* Big enough for a compressed full-colour image; larger files are read
 * through fs_read_range() a window at a time. */
#define FS_FILEBUF_MAX (4 * 1024 * 1024)
static uint8_t fs_filebuf[FS_FILEBUF_MAX];
static const char *fs_errstr = "";

/*
 * Two things from profile.h, which cannot be included until after
 * users.h -- it asks who is signed in -- and users.h cannot be included
 * until after this section, since it stores the account table through
 * it. Declared here and defined there, the way security.h:129 already
 * forward-declares xorshift32() out of login.h.
 *
 * They matter because this is the only place they can go. Every path
 * that reaches a disk on a person's behalf passes through the eight
 * functions below, so a check here is a check everywhere, and a check
 * anywhere else would be one of a hundred that all have to agree.
 *
 *   fs_native  accepts the Windows spelling of a path -- backslashes,
 *              and a drive letter -- and produces the one the drivers
 *              parse. Note carefully that fs_abs() was never enough:
 *              fs_write_file, fs_delete, fs_mkdir and fs_list handed the
 *              caller's string straight to a driver without normalising
 *              anything at all.
 *   prof_may   1 if the account signed in may touch this path.
 */
static void fs_native(const char *in, char *out, int max);
static int  prof_may(const char *path, int write);

/*
 * And one from swap.h, for the same reason and with a sharper
 * consequence. The pager resolves the pagefile to a run of raw sectors
 * once and writes to them directly for the rest of the boot, so deleting
 * the file -- or overwriting it, which frees its clusters first -- hands
 * those same clusters to the next file created while the pager is still
 * writing pages into them. That is filesystem corruption with no
 * symptom until something reads the other file back.
 */
static int swap_owns_path(const char *abs);

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
        /* NTFS first: it is what this system formats and boots from now.
         * exFAT and FAT32 stay behind it so that a volume made by an
         * older build, or a USB stick from somewhere else, still
         * mounts -- the boot volume changed, the ability to read the
         * others did not. */
        ntfs_mount();
        if (ntfs_mounted()) fs_kind = FS_NTFS;
        else {
            exfat_mount();
            if (exf_vol.mounted) fs_kind = FS_EXFAT;
            else {
                fat32_mount();
                if (fat_vol.mounted) fs_kind = FS_FAT32;
            }
        }
        if (fs_kind != FS_NONE) {
            serial_puts("[fs] mounted ");
            serial_puts(fs_kind == FS_NTFS  ? "NTFS"
                      : fs_kind == FS_EXFAT ? "exFAT" : "FAT32");
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
    if (fs_kind == FS_NTFS)  return "NTFS";
    if (fs_kind == FS_EXFAT) return "exFAT";
    if (fs_kind == FS_FAT32) return "FAT32";
    return tarfs_base ? "ramdisk (read-only)" : "none";
}

static int fs_writable(void) { return fs_kind != FS_NONE; }

/* Counted out of $Bitmap on NTFS, which is a pass over a few hundred
 * kilobytes. Cached, because `df` and the settings panel ask on every
 * refresh and the answer only changes when something is written. */
static uint32_t ntfs_total_kb_cache = 0, ntfs_free_kb_cache = 0;
static int      ntfs_space_valid = 0;

static void ntfs_space_refresh(void) {
    uint64_t total = 0, freec = 0;
    if (ntfs_space(&total, &freec) != 0) return;
    {
        const uint64_t per_kb = ntfs_cluster_bytes() / 1024;
        ntfs_total_kb_cache = (uint32_t)(total * per_kb);
        ntfs_free_kb_cache  = (uint32_t)(freec * per_kb);
    }
    ntfs_space_valid = 1;
}

static uint32_t fs_total_kb(void) {
    if (fs_kind == FS_NTFS) {
        if (!ntfs_space_valid) ntfs_space_refresh();
        return ntfs_total_kb_cache;
    }
    if (fs_kind == FS_EXFAT) return exf_total_kb();
    if (fs_kind == FS_FAT32) return fat_total_kb();
    return 0;
}

static uint32_t fs_free_kb(void) {
    if (fs_kind == FS_NTFS) {
        if (!ntfs_space_valid) ntfs_space_refresh();
        return ntfs_free_kb_cache;
    }
    if (fs_kind == FS_EXFAT) return exf_free_kb();
    if (fs_kind == FS_FAT32) return fat_free_kb();
    return 0;
}

/* Normalise to an absolute path in the caller's buffer. Separators and
 * drive letters are fs_native's problem now, so that `\Documents and
 * Settings\alice` and `/Documents and Settings/alice` are one path. */
static void fs_abs(const char *in, char *out, int max) {
    fs_native(in, out, max);
}

/* Does the path exist, and what is it?  Replaces every direct driver
 * lookup that used to be scattered through the apps.
 *
 * `ino` is the fourth output and the newest: a number that identifies
 * the file rather than the path to it. On NTFS that is the MFT record,
 * which the lookup already has in its hand — this is what lets stat(2)
 * in ring 3 fill in st_ino, and therefore what lets ported code decide
 * whether two paths are the same file. Zero on the filesystems that
 * have no such number to give, which is honest: a caller comparing two
 * zeroes learns nothing, where a caller comparing two invented numbers
 * would learn something false. */
static int fs_stat_ex(const char *path, uint64_t *size, int *is_dir,
                      uint64_t *ino) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 0)) return 0;
    if (ino) *ino = 0;

    if (fs_kind == FS_NTFS) {
        uint64_t sz = 0, rec = 0;
        int dir = 0;
        if (ntfs_lookup(abs, &rec, 0, &dir, &sz) != 0) return 0;
        if (size) *size = sz;
        if (is_dir) *is_dir = dir;
        if (ino) *ino = rec;
        return 1;
    }
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

static int fs_stat(const char *path, uint64_t *size, int *is_dir) {
    return fs_stat_ex(path, size, is_dir, 0);
}

const void *fs_read_file(const char *filename, uint64_t *out_size) {
    char abs[256];
    fs_abs(filename, abs, sizeof(abs));
    if (!prof_may(abs, 0)) { if (out_size) *out_size = 0; return 0; }

    if (fs_kind == FS_NTFS) {
        uint64_t rec = 0;
        int dir = 1;
        int64_t got;
        if (ntfs_lookup(abs, &rec, 0, &dir, 0) != 0 || dir) {
            if (out_size) *out_size = 0;
            return 0;
        }
        got = ntfs_read_file(rec, fs_filebuf, FS_FILEBUF_MAX);
        if (got < 0) {
            if (out_size) *out_size = 0;
            return 0;
        }
        if (out_size) *out_size = (uint64_t)got;
        return fs_filebuf;
    }
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
    if (!prof_may(abs, 0)) return -1;

    if (fs_kind == FS_NTFS) {
        uint64_t rec = 0;
        int dir = 1;
        int64_t n;
        if (ntfs_lookup(abs, &rec, 0, &dir, 0) != 0 || dir) {
            fs_errstr = "not found";
            return -1;
        }
        n = ntfs_read_range(rec, offset, buf, len);
        if (n < 0) { fs_errstr = "read failed"; return -1; }
        *got = (uint32_t)n;
        return 0;
    }
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
    uint64_t ntfs_record;     /* resolved once, for FS_NTFS */
    char     path[160];
} fs_file_t;

/*
 * The check has to be here rather than in fs_pread, and that is the
 * whole reason this comment exists: fs_pread reads through the
 * exf_dirent_t cached in the handle and never looks at a path again. A
 * handle opened without a check is a key to the file for as long as it
 * is held.
 */
static int fs_open(const char *path, fs_file_t *f) {
    f->valid = 0;
    f->kind = fs_kind;
    str_copy(f->path, path, sizeof(f->path));

    {
        char abs[256];
        fs_abs(path, abs, sizeof(abs));
        if (!prof_may(abs, 0)) return -1;
    }

    if (fs_kind == FS_NTFS) {
        char abs[256];
        int dir = 1;
        fs_abs(path, abs, sizeof(abs));
        if (ntfs_lookup(abs, &f->ntfs_record, 0, &dir, &f->size) != 0 || dir) {
            fs_errstr = "not found";
            return -1;
        }
        f->valid = 1;
        return 0;
    }
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
    if (f->kind == FS_NTFS) {
        /* Straight to the record the handle resolved at open time: an
         * archive read is thousands of small reads and re-walking the
         * path for each would dominate the cost. */
        const int64_t n = ntfs_read_range(f->ntfs_record, off, buf, len);
        if (n < 0) { fs_errstr = "read failed"; return -1; }
        *got = (uint32_t)n;
        return 0;
    }
    if (f->kind == FS_EXFAT) {
        if (exf_read_range(&f->exf, off, (uint8_t *)buf, len, got) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    return fs_read_range(f->path, off, buf, len, got);
}

int fs_write_file(const char *path, const void *data, uint32_t len) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 1)) return -1;
    if (swap_owns_path(abs)) {
        fs_errstr = "the pagefile is in use";
        return -1;
    }

    if (fs_kind == FS_NTFS) {
        uint64_t dir = 0;
        char leaf[256];
        int rc;

        if (ntfs_split(abs, &dir, leaf, sizeof(leaf)) != 0) {
            fs_errstr = "no such directory";
            return -1;
        }
        /* Replace rather than update in place. NTFS's writer creates a
         * record with its data attribute already sized, so changing a
         * file's length means rebuilding it -- and delete-then-create is
         * exactly what exFAT's writer does above, so the two agree about
         * what "write this file" means. Both halves go through the
         * journal, so a power loss between them replays to one state or
         * the other rather than to a directory entry with no record. */
        if (ntfs_lookup(abs, 0, 0, 0, 0) == 0)
            ntfs_delete_file(dir, leaf);

        rc = ntfs_create_file(dir, leaf, (const uint8_t *)data, len);
        if (rc != NTFS_W_OK) {
            fs_errstr = ntfs_w_errstr;
            return -1;
        }
        ntfs_space_valid = 0;
        return 0;
    }
    if (fs_kind == FS_EXFAT) {
        if (exf_write_file(abs, (const uint8_t *)data, len) != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_write_file(abs, (const uint8_t *)data, len) != 0) {
            fs_errstr = fat_errstr;
            return -1;
        }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

static int fs_delete(const char *path) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 1)) return -1;
    if (swap_owns_path(abs)) {
        fs_errstr = "the pagefile is in use";
        return -1;
    }

    if (fs_kind == FS_NTFS) {
        uint64_t dir = 0;
        char leaf[256];
        if (ntfs_split(abs, &dir, leaf, sizeof(leaf)) != 0) {
            fs_errstr = "no such directory";
            return -1;
        }
        if (ntfs_delete_file(dir, leaf) != NTFS_W_OK) {
            fs_errstr = ntfs_w_errstr;
            return -1;
        }
        ntfs_space_valid = 0;
        return 0;
    }
    if (fs_kind == FS_EXFAT) {
        if (exf_delete(abs) != 0) { fs_errstr = exf_errstr; return -1; }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_delete(abs) != 0) { fs_errstr = fat_errstr; return -1; }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

int fs_mkdir(const char *path) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 1)) return -1;

    if (fs_kind == FS_NTFS) {
        uint64_t dir = 0;
        char leaf[256];
        /* Already there is success, not an error: callers do
         * fs_mkdir("/etc") before writing into it and do not expect the
         * second boot to fail. exFAT's writer behaves the same way. */
        if (ntfs_lookup(abs, 0, 0, 0, 0) == 0) return 0;
        if (ntfs_split(abs, &dir, leaf, sizeof(leaf)) != 0) {
            fs_errstr = "no such directory";
            return -1;
        }
        if (ntfs_mkdir_at(dir, leaf) != NTFS_W_OK) {
            fs_errstr = ntfs_w_errstr;
            return -1;
        }
        ntfs_space_valid = 0;
        return 0;
    }
    if (fs_kind == FS_EXFAT) {
        if (exf_mkdir(abs) != 0) { fs_errstr = exf_errstr; return -1; }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        if (fat_mkdir(abs) != 0) { fs_errstr = fat_errstr; return -1; }
        return 0;
    }
    fs_errstr = "read-only filesystem (no disk attached)";
    return -1;
}

typedef void (*fs_list_cb)(const char *name, uint32_t size, int is_dir);

/*
 * The same enumeration, with somewhere to put the answer.
 *
 * Every caller in the interface was happy with a bare function pointer,
 * because each of them had exactly one listing in flight and could keep
 * the running total in a file-scope variable. src/vfs.h is not: a
 * program calling opendir() wants the entries in *its* buffer, and a
 * static one shared between callers is a listing that comes back wrong
 * the moment two things enumerate at once.
 *
 * So the context-carrying form is the real one and fs_list is a wrapper
 * over it. Nothing about the existing callers changes.
 */
typedef void (*fs_list_ctx_cb)(void *ctx, const char *name, uint32_t size,
                               int is_dir);

static void fs_ntfs_list_adapt(void *ctx, const char *name, uint64_t size,
                               int is_dir);

typedef struct {
    fs_list_ctx_cb cb;
    void          *ctx;
} fs_list_bridge_t;

static void fs_ntfs_list_adapt(void *ctx, const char *name, uint64_t size,
                               int is_dir) {
    fs_list_bridge_t *b = (fs_list_bridge_t *)ctx;
    b->cb(b->ctx, name, (uint32_t)size, is_dir);
}

/* exFAT and FAT32 hand the callback no context of their own, so the
 * bridge for those two has to be reached through a file-scope pointer.
 * That is safe for exactly the reason the general case is not: neither
 * driver blocks inside an enumeration, and a system call cannot be
 * preempted between the store below and the walk that reads it. */
static fs_list_bridge_t *fs_list_active = 0;

static void fs_plain_list_adapt(const char *name, uint32_t size, int is_dir) {
    if (fs_list_active) fs_list_active->cb(fs_list_active->ctx, name, size,
                                           is_dir);
}

static int fs_list_ctx(const char *path, fs_list_ctx_cb cb, void *ctx) {
    char abs[256];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 0)) return -1;

    fs_list_bridge_t bridge = { cb, ctx };

    if (fs_kind == FS_NTFS) {
        /* The driver hands back a 64-bit size and a context pointer;
         * this layer's callback takes a 32-bit one. The adapter is the
         * whole difference, and it lives here rather than in the driver
         * because the driver should not know what the desktop's
         * callback signature happens to be. */
        if (ntfs_list(abs, fs_ntfs_list_adapt, &bridge) != 0) {
            fs_errstr = "no such directory";
            return -1;
        }
        return 0;
    }
    if (fs_kind == FS_EXFAT) {
        fs_list_bridge_t *saved = fs_list_active;
        fs_list_active = &bridge;
        int rc = exf_list(abs, (exf_list_cb)fs_plain_list_adapt);
        fs_list_active = saved;
        if (rc != 0) {
            fs_errstr = exf_errstr;
            return -1;
        }
        return 0;
    }
    if (fs_kind == FS_FAT32) {
        fat_dirent_t d;
        if (!fat_lookup(abs, &d) || !(d.attr & FAT_ATTR_DIR)) {
            fs_errstr = "no such directory";
            return -1;
        }
        fat_iter_t it;
        fat_iter_init(&it, d.first_clus);
        fat_dirent_t e;
        while (fat_iter_next(&it, &e) == 1)
            cb(ctx, e.name, e.size, (e.attr & FAT_ATTR_DIR) ? 1 : 0);
        return 0;
    }
    return -1;
}

static void fs_list_plain_bridge(void *ctx, const char *name, uint32_t size,
                                 int is_dir) {
    ((fs_list_cb)ctx)(name, size, is_dir);
}

static int fs_list(const char *path, fs_list_cb cb) {
    return fs_list_ctx(path, fs_list_plain_bridge, (void *)cb);
}

#include "zim.h"

/* Accounts. After the filesystem layer above, which they are stored
 * through, and after login.h (included by kernel.c first) for xorshift32
 * and idt.h for cycle_now, which together seed the salt. */
#include "sha256.h"
#include "chacha20.h"
#include "users.h"
/* After users.h, because the isolation guard asks who is signed in, and
 * before everything that stores anything: this is where fs_native() and
 * prof_may() -- forward-declared above the filesystem layer -- are
 * actually defined. */
#include "profile.h"
/*
 * The pager. After the filesystem layer, because its area is a file it
 * allocates through exFAT; before anything that runs, because from here
 * on a user page may not be in memory at all. swap_init() is called from
 * kernel.c once the volume is mounted.
 */
#include "swap.h"
/* After users.h: the policies here are per-account. */
#include "security.h"
/*
 * Whether this account wants the language model at all.
 *
 * Not everyone does: the weights are hundreds of megabytes, loading
 * them costs real time
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

/* ===== 3b. THE REGISTRY, ON DISK =====
 *
 * The hive is written whole and to a temporary name first, then the old
 * one is replaced. That is the cheapest correct commit available on a
 * filesystem with no rename-is-atomic guarantee: the window in which
 * neither file is complete is one write of a small file, and the log
 * says which state to expect if the machine stops inside it.
 */
#define REG_HIVE_PATH "/etc/registry.hive"
#define REG_LOG_PATH  "/etc/registry.log"

static int reg_flush(void) {
    if (!reg_nodes || !reg_dirty) return 0;

    uint64_t bytes = sizeof(reg_header_t) +
                     (uint64_t)reg_count * sizeof(reg_node_t);
    uint8_t *buf = (uint8_t *)kmalloc(bytes);
    if (!buf) return -1;

    reg_header_t *h = (reg_header_t *)buf;
    h->magic      = REG_MAGIC;
    h->version    = REG_VERSION;
    h->node_count = reg_count;
    h->sequence   = ++reg_sequence;
    h->checksum   = reg_checksum(reg_nodes, reg_count);
    h->reserved[0] = h->reserved[1] = h->reserved[2] = 0;

    for (uint64_t i = 0; i < (uint64_t)reg_count * sizeof(reg_node_t); i++)
        buf[sizeof(reg_header_t) + i] = ((const uint8_t *)reg_nodes)[i];

    /* The intent, first. A hive found with a log whose sequence is
     * ahead of the hive's own was interrupted mid-write. */
    uint32_t log[2] = { REG_MAGIC, h->sequence };
    fs_write_file(REG_LOG_PATH, (const char *)log, sizeof(log));

    int rc = fs_write_file(REG_HIVE_PATH, (const char *)buf, (uint32_t)bytes);
    kfree(buf);
    if (rc >= 0) {
        reg_dirty = 0;
        /* Intent discharged. */
        uint32_t done[2] = { REG_MAGIC, 0 };
        fs_write_file(REG_LOG_PATH, (const char *)done, sizeof(done));
    }
    return rc >= 0 ? 0 : -1;
}

static void reg_load(void) {
    if (!reg_ready()) return;

    uint64_t sz = 0;
    const void *d = fs_read_file(REG_HIVE_PATH, &sz);
    if (!d || sz < sizeof(reg_header_t)) {
        serial_puts("[registry] no hive - starting a new one\n");
        reg_dirty = 1;
        return;
    }
    const reg_header_t *h = (const reg_header_t *)d;
    if (h->magic != REG_MAGIC || h->version != REG_VERSION ||
        h->node_count == 0 || h->node_count > REG_MAX_NODES) {
        serial_puts("[registry] hive header rejected - starting a new one\n");
        return;
    }
    uint64_t need = sizeof(reg_header_t) +
                    (uint64_t)h->node_count * sizeof(reg_node_t);
    if (sz < need) {
        serial_puts("[registry] hive is short - starting a new one\n");
        return;
    }

    const reg_node_t *src =
        (const reg_node_t *)((const uint8_t *)d + sizeof(reg_header_t));
    if (reg_checksum(src, h->node_count) != h->checksum) {
        serial_puts("[registry] hive checksum failed - starting a new one\n");
        return;
    }

    for (uint64_t i = 0; i < (uint64_t)h->node_count * sizeof(reg_node_t); i++)
        ((uint8_t *)reg_nodes)[i] = ((const uint8_t *)src)[i];
    reg_count = h->node_count;
    reg_sequence = h->sequence;
    reg_dirty = 0;

    /* Was the last write interrupted? The log says what sequence was
     * being written; a hive older than that never landed. */
    uint64_t lsz = 0;
    const void *ld = fs_read_file(REG_LOG_PATH, &lsz);
    if (ld && lsz >= 8) {
        const uint32_t *log = (const uint32_t *)ld;
        if (log[0] == REG_MAGIC && log[1] && log[1] > h->sequence) {
            serial_puts("[registry] a commit was interrupted - the hive is "
                        "the state before it\n");
        }
    }

    serial_puts("[registry] hive loaded: ");
    serial_put_dec(reg_count);
    serial_puts(" nodes, sequence ");
    serial_put_dec(reg_sequence);
    serial_puts("\n");
}

/* ===== 4. SYSCALL GATEWAY + ELF64 LOADER ===== */

/*
 * Syscall ABI (see apps/vextro.h):
 *   RAX = number, RDI = arg0, RSI = arg1, RDX = arg2, R10/R8/R9 = arg3-5
 *   reached by `int 0x80` or by SYSCALL; both arrive here.
 *
 * Everything below runs on a kernel stack with interrupts masked, with
 * the calling process's page tables still loaded — which is what makes a
 * user pointer dereferenceable at all, and exactly why every one of them
 * is checked first. `user_range_mapped` asks two questions that both
 * matter: is this address in the half of the space a program is allowed
 * to name, and is it actually mapped to that program with the access the
 * call is about to perform. A pointer that fails either is a refusal,
 * not a fault.
 */

#define APP_CANVAS_W 598
#define APP_CANVAS_H 402

/*
 * ===== 4a. OFFSCREEN WINDOW SURFACES =====
 *
 * Every application used to draw into `app_canvas` — one array, mapped
 * into every process at the same virtual address, and therefore *the
 * same physical pixels in all of them*. Two programs running at once
 * fought over the frame: whichever ran last in a slice owned the window,
 * and what the compositor put on screen was an interleaving of both.
 *
 * That is also an isolation hole with nothing to do with graphics. A
 * shared writable mapping between two ring-3 processes is a channel
 * between them, and it existed for no better reason than that there had
 * only ever been one application at a time to worry about.
 *
 * So a process now renders exclusively into a surface of its own, and
 * the compositor is what puts surfaces on the screen. The mapping is
 * still at `canvas_va` and SYS_CANVAS still answers the same three
 * values, so no binary already on somebody's disk notices the change —
 * what changed is which physical pages that address resolves to.
 *
 * ---- why the surfaces are static and counted ----
 *
 * They are page-aligned because a mapping is made of whole pages, and
 * they are a fixed pool rather than heap allocations for two reasons
 * that both matter. kmalloc's large path hands back a pointer 64 bytes
 * into a page, so a mapped canvas would land 64 bytes off the start of
 * the user's page and every pixel would be shifted; and a surface that
 * never moves can be given a GGTT mapping once at boot rather than one
 * per spawn, which the blitter needs and which the GPU's bump allocator
 * could not take back.
 *
 * Six of them, and the seventh concurrent application falls back to the
 * shared canvas with a line on the wire saying so. That is a real limit
 * and it is stated rather than hidden: five and a half megabytes of
 * pixels is already the third largest static allocation in this kernel,
 * behind the wallpaper and the back buffer.
 */
#define APP_SURF_MAX   6
#define APP_SURF_PX    (APP_CANVAS_W * APP_CANVAS_H)
#define APP_SURF_BYTES (((APP_SURF_PX * 4u) + 4095u) & ~4095u)
#define APP_SURF_WORDS (APP_SURF_BYTES / 4u)

/* The trailing words of each row are padding to the page boundary and
 * are never drawn; they exist so that surface n+1 starts page-aligned
 * like surface n, which a plain [MAX][PX] array would not. */
static uint32_t app_surf_mem[APP_SURF_MAX][APP_SURF_WORDS]
    __attribute__((aligned(4096)));

struct app_surface {
    uint32_t *px;
    uint32_t  refs;        /* address spaces mapping it; 0 means free   */
    uint32_t  gen;         /* bumped whenever the owner draws           */
    uint32_t  pid;         /* whoever created it, for the taskbar       */
    uint32_t  gpu_addr;    /* GGTT address, or 0 if unreachable         */
    char      name[SCHED_NAME_LEN];
};
typedef struct app_surface app_surface_t;

static app_surface_t app_surf[APP_SURF_MAX];

/* The surface the application window shows: the most recent one to have
 * been claimed and still live. Null means the window shows the shared
 * fallback canvas, which is what a single-application machine did before
 * any of this and what it still does if the pool is exhausted. */
static app_surface_t *app_surf_front = 0;

/* Page-aligned for the same reason the pool is. Still here because a
 * process that could not be given a surface has to draw somewhere, and
 * because term.h points the video decoder at it. */
static uint32_t app_canvas[APP_CANVAS_W * APP_CANVAS_H]
    __attribute__((aligned(4096)));
static char     app_win_title[64] = "hello";
static int      silent_launch = 0;

static void app_surf_init(void) {
    for (int i = 0; i < APP_SURF_MAX; i++) {
        app_surf[i].px   = app_surf_mem[i];
        app_surf[i].refs = 0;
        app_surf[i].gen  = 0;
        app_surf[i].pid  = 0;
        app_surf[i].name[0] = '\0';
    }
}

static void app_surf_clear(app_surface_t *s) {
    for (uint32_t i = 0; i < APP_SURF_PX; i++) s->px[i] = 0;
    s->gen++;
}

/* Claim one for a process that is starting. Null when the pool is full,
 * which the caller reports rather than treats as a failure to launch. */
static app_surface_t *app_surf_claim(const char *name) {
    for (int i = 0; i < APP_SURF_MAX; i++) {
        if (app_surf[i].refs) continue;
        app_surf[i].refs = 1;
        app_surf[i].pid  = 0;
        str_copy(app_surf[i].name, name, SCHED_NAME_LEN);
        app_surf_clear(&app_surf[i]);
        return &app_surf[i];
    }
    return 0;
}

/* A fork maps the parent's pixels rather than copying them: the two
 * halves of a forked program are one program as far as the window is
 * concerned, and giving the child a blank surface of its own would show
 * whichever of them drew last on a canvas the other had never seen. */
static void app_surf_ref(app_surface_t *s) {
    if (s) s->refs++;
}

static void app_surf_release(app_surface_t *s) {
    if (!s || !s->refs) return;
    if (--s->refs) return;
    s->pid     = 0;
    s->name[0] = '\0';
    /* The window has to show something. When the program that owned it
     * ends, the front falls to whatever else is still running rather
     * than to the empty fallback canvas -- closing one of two
     * applications should reveal the other, not a blank frame. */
    if (app_surf_front == s) {
        app_surf_front = 0;
        for (int i = 0; i < APP_SURF_MAX; i++)
            if (app_surf[i].refs) { app_surf_front = &app_surf[i]; break; }
    }
}

/*
 * What the last frame cost the engine.
 *
 * Declared up here with the surfaces rather than down in the compositor
 * that writes them, because the system monitor reads them and its
 * header is included before the compositor's section. Two counters and
 * an include order is a smaller price than a forward declaration nobody
 * would think to keep in step.
 */
static uint32_t aero_gpu_batches = 0;   /* submissions in the last frame */
static uint32_t aero_gpu_ops     = 0;   /* operations the engine took    */

/* How many surfaces have a live process behind them — what the taskbar
 * needs to know before it decides whether to draw previews at all. */
static int app_surf_live(void) {
    int n = 0;
    for (int i = 0; i < APP_SURF_MAX; i++) if (app_surf[i].refs) n++;
    return n;
}

/* Where the calling process's pixels are, whichever kind it turned out
 * to be. Every syscall that writes pixels goes through this rather than
 * naming a buffer, which is what makes the fallback invisible to the
 * service routines. */
static uint32_t *app_surf_current(void) {
    if (vmm_current && vmm_current->surface)
        return vmm_current->surface->px;
    return app_canvas;
}

/* Set while a user thread is running so a refusal can name it. */
static char     app_fault_note[96];

static void app_refuse(const char *what) {
    str_copy(app_fault_note, what, sizeof(app_fault_note));
    serial_puts("[syscall] refused: ");
    serial_puts(what);
    serial_puts("\n");
}

/* A string a program handed us, brought across the boundary before
 * anything reads it twice. Nothing below ever walks user memory
 * directly: a length checked and then re-read is a length that can
 * change, and here it cannot, because the copy is what gets used. */
static int sys_copy_string(uint64_t uptr, char *dst, int cap) {
    if (!vmm_current) {
        /* A kernel-mode caller — the boot self-test, or an app running
         * before address spaces were available. Its pointers are the
         * kernel's own and are trusted as such. */
        const char *s = (const char *)(uintptr_t)uptr;
        if (!s) return -1;
        int i = 0;
        for (; s[i] && i < cap - 1; i++) dst[i] = s[i];
        dst[i] = '\0';
        return i;
    }
    return user_strncpy_in(vmm_current, dst, uptr, cap);
}

/* The eight-word argument block the trampolines build for the two calls
 * that have more parameters than registers. */
static int sys_read_args(uint64_t uptr, uint64_t *out, int n) {
    if (!vmm_current) {
        const uint64_t *p = (const uint64_t *)(uintptr_t)uptr;
        if (!p) return -1;
        for (int i = 0; i < n; i++) out[i] = p[i];
        return 0;
    }
    if (!user_range_mapped(vmm_current, uptr, (uint64_t)n * 8, 0)) return -1;
    const uint64_t *p = (const uint64_t *)(uintptr_t)uptr;
    for (int i = 0; i < n; i++) out[i] = p[i];
    return 0;
}

/* Is `buf` a pixel buffer of bw x bh that the caller may write? */
static int sys_canvas_ok(uint64_t buf, int64_t bw, int64_t bh) {
    if (bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192) return 0;
    uint64_t bytes = (uint64_t)bw * (uint64_t)bh * 4u;
    if (!vmm_current) return buf != 0;
    return user_range_mapped(vmm_current, buf, bytes, 1);
}

static uint64_t sys_sbrk(int64_t delta);

/*
 * And exec, for the same reason and with a longer reach: it needs the
 * loader, the allow list, the scanner and the address-space builder,
 * every one of which is defined further down this file than the system
 * call switch that has to reach it.
 */
static int64_t sys_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp);

/*
 * ===== 4b. THE ELEVATION GATEWAY =====
 *
 * Every process in this system starts restricted, and that sentence is
 * doing more work than it looks like.
 *
 * users.h has always been careful to say what its administrator flag is
 * not: "a security boundary". It governed the interface, and it could
 * only govern the interface, because the interface was the only way a
 * person reached the disk — there were no file system calls, so a ring-3
 * program had nothing to be restricted *from*. The flag decided which
 * buttons a person saw. It decided nothing at all about a program.
 *
 * The three doors added below change that, and they are chosen rather
 * than arbitrary: writing a file rewrites NTFS metadata, writing the
 * registry rewrites the configuration hive, and writing a block goes
 * straight past both to the disk. Those are the three ways a program can
 * change this machine permanently, and now that a program can ask, the
 * question of whether it may has an answer that has to be made rather
 * than assumed.
 *
 * ---- why an administrator's program is still restricted ----
 *
 * The obvious design is that a program run by an administrator is an
 * administrative program. It is also the design that makes the account
 * flag worthless: everything that account launches — a downloaded
 * binary, a store package, something that arrived over the network —
 * inherits the whole machine without anybody being asked.
 *
 * So the flag is demoted to what it actually is: permission to *be
 * asked*. A restricted token is what a process gets, an administrator's
 * session is what makes elevation possible, and an answer at the
 * keyboard is what makes it happen. A non-administrator's program is
 * refused without a prompt, because there is no answer that could grant
 * it.
 *
 * The elevation lasts for the life of that process and is not inherited
 * across a fork; see the note in SYS_FORK about why a program that could
 * pass it on would only have to fork to launder it.
 *
 * ---- what the policy level does ----
 *
 * uac_level has existed in the policy file since the security module was
 * split out, with four settings and a comment saying it governed "how
 * often privileged actions should ask". Nothing read it, because nothing
 * had a privileged action to gate. This is what reads it.
 */
#define UAC_OP_LEN     28
#define UAC_DETAIL_LEN 96

/* What the privileged calls will accept in one go.
 *
 * A cap rather than a stream, and a kernel buffer rather than a walk
 * through user memory: the bytes have to be readable *after* the person
 * has answered the prompt, and the only way to guarantee that is to
 * have taken a copy before asking. Half a megabyte is more than any
 * configuration file and small enough to sit in the image. */
#define SYS_WRITE_MAX  (512u * 1024u)
#define FS_PATH_MAX    256

/* How long a frozen thread waits for an answer before the silence is
 * taken as a refusal. Long enough to read the prompt and think about it;
 * bounded, because a program that blocks forever on a dialog nobody is
 * looking at is indistinguishable from one that has hung. */
#define UAC_WAIT_TICKS 60000u          /* scheduler ticks: one minute */

static struct {
    volatile int  pending;             /* a question is on the screen  */
    volatile int  answered;
    volatile int  granted;
    uint32_t      pid;
    char          program[SCHED_NAME_LEN];
    char          op[UAC_OP_LEN];
    char          detail[UAC_DETAIL_LEN];
} uac_req;

static uint32_t uac_grants = 0;
static uint32_t uac_denials = 0;

/*
 * Put the question on the screen and wait for the answer.
 *
 * Extracted from uac_guard, which used to be the only thing that asked
 * anybody anything. There are two guards now — this one's caller, and
 * net_guard in src/vfs.h — and they differ in the policy in front of the
 * question and in where the answer is recorded, not in the question
 * itself. Sharing the asking is what keeps "only one prompt may be
 * outstanding" a property of the system rather than of one function.
 *
 * Deliberately does *not* touch the token. That is the whole point of
 * the split: elevating for one answer is uac_guard's decision about the
 * three privileged doors, and the network is not one of them. See the
 * note beside `net_ok` in include/kernel_shared.h.
 *
 * Only ever called from a system call on a ring-3 thread, and never
 * from the compositor, whose own thread is the one that has to stay
 * running to draw what it is asking about.
 */
static int uac_prompt(const char *op, const char *detail) {
    /*
     * One question at a time.
     *
     * A queue would be the general answer and the wrong one here: a
     * prompt is a claim about what the person is looking at, and two
     * stacked prompts about two different programs is exactly how
     * somebody ends up granting the second while reading the first.
     * The loser is refused and told why.
     */
    if (uac_req.pending) {
        serial_puts("[uac] refused (another request is being decided): ");
        serial_puts(cur_thread->name);
        serial_putc('\n');
        uac_denials++;
        return 0;
    }

    uac_req.answered = 0;
    uac_req.granted  = 0;
    uac_req.pid      = cur_thread->pid;
    str_copy(uac_req.program, cur_thread->name, SCHED_NAME_LEN);
    str_copy(uac_req.op, op, UAC_OP_LEN);
    str_copy(uac_req.detail, detail ? detail : "", UAC_DETAIL_LEN);
    uac_req.pending  = 1;

    serial_puts("[uac] ");
    serial_puts(uac_req.program);
    serial_puts(" wants to ");
    serial_puts(op);
    serial_puts(": ");
    serial_puts(uac_req.detail);
    serial_puts(" - asking\n");

    /*
     * Freeze the thread.
     *
     * The channel is the request's own address, which is the usual
     * convention here — nothing dereferences it, it is only compared.
     * Waking in quarter-second steps rather than waiting on the channel
     * alone: a wake that arrives between the state being set and the
     * yield is a wake that is lost, and re-testing the condition on a
     * timer costs four wakeups a second to make that impossible to care
     * about.
     *
     * This runs with interrupts masked, because every system call does.
     * That is exactly what makes the sleep safe rather than unsafe here
     * — sched_block_on_locked takes the flags as it finds them, and the
     * yield that follows is a software-raised vector, which an INT
     * delivers whether or not IF is set.
     */
    {
        const uint64_t deadline = sched_ticks + UAC_WAIT_TICKS;
        while (!uac_req.answered && sched_ticks < deadline)
            sched_block_on((void *)&uac_req, 250);
    }

    const int ok = uac_req.answered && uac_req.granted;
    uac_req.pending  = 0;
    uac_req.answered = 0;

    if (ok) uac_grants++;
    else    uac_denials++;
    return ok;
}

/*
 * May the calling process do one of the three privileged things?
 *
 * Returns 1 to allow and 0 to refuse. When the answer is not already
 * known it asks through uac_prompt above, which freezes the calling
 * thread until somebody answers.
 */
static int uac_guard(const char *op, const char *detail) {
    addr_space_t *as = vmm_current;

    /*
     * A kernel-mode caller: the shell, the package installer, a boot
     * self-test. There is no application to ask about and no ring-3
     * thread to freeze — the person is already at the keyboard driving
     * it, and the interface asked them whatever it needed to.
     */
    if (!as || !cur_thread || !sched_running) return 1;

    if (as->token == UAC_TOKEN_ELEVATED) return 1;

    /* The policy says not to ask. Recorded on the token rather than
     * merely returned, so a later call does not re-derive it. */
    if (uac_level == UAC_NEVER) {
        as->token = UAC_TOKEN_ELEVATED;
        return 1;
    }

    if (!as->sid_admin) {
        char note[NOTIFY_TEXT];
        str_copy(note, "Refused: ", sizeof(note));
        str_append(note, cur_thread->name, sizeof(note));
        str_append(note, " is not running as an administrator", sizeof(note));
        notify_push(NOTE_WARN, note);
        serial_puts("[uac] refused (not an administrator's session): ");
        serial_puts(cur_thread->name);
        serial_puts(" wanted to ");
        serial_puts(op);
        serial_putc('\n');
        uac_denials++;
        return 0;
    }

    /* Named before the prompt rather than after it. uac_prompt parks
     * this thread and reuses uac_req for whoever asks next, so the only
     * copy of the name that is certainly still about this request when
     * the answer comes back is one taken here. */
    char who[SCHED_NAME_LEN];
    str_copy(who, cur_thread->name, sizeof(who));

    const int ok = uac_prompt(op, detail);

    if (ok) {
        as->token = UAC_TOKEN_ELEVATED;
        serial_puts("[uac] granted; ");
        serial_puts(who);
        serial_puts(" is elevated for the rest of this run\n");
    } else {
        char note[NOTIFY_TEXT];
        str_copy(note, "Denied: ", sizeof(note));
        str_append(note, who, sizeof(note));
        notify_push(NOTE_WARN, note);
        serial_puts("[uac] denied\n");
    }
    return ok;
}

/*
 * The answer, from the compositor's thread.
 *
 * Writes the verdict and releases whoever is parked on the request. It
 * cannot be the same thread that asked -- the asker is asleep -- which
 * is the whole reason the prompt is drawn by the interface rather than
 * by the program.
 */
static void uac_answer(int granted) {
    if (!uac_req.pending) return;
    uac_req.granted  = granted;
    uac_req.answered = 1;
    sched_wake_chan((void *)&uac_req, 1);
}

/*
 * File descriptors, and the two things one can name.
 *
 * Here rather than higher up because it needs all of it: the filesystem
 * layer to read through, the accounts to ask permission of, and
 * uac_prompt immediately above to put a question about the network on
 * the screen. Immediately before the system call gateway, because that
 * is the only thing that uses it.
 */
#include "vfs.h"

/*
 * ===== 4c. THE TWO SOCKET OPERATIONS THAT PARK =====
 *
 * Out of the switch below because they are the only service routines in
 * this kernel that touch user memory on *both* sides of a wait, and the
 * shape of that is worth being able to read in one piece:
 *
 *     check the buffer, copy it in, park, copy it out, check again.
 *
 * The last step is not belt and braces. Every other thread on the
 * machine runs while this one is parked — see the note at the head of
 * src/vfs.h — and one of them may be a sibling of the caller with a
 * perfect right to munmap the very buffer this call was given. Writing
 * to it afterwards without asking again is a page fault taken in ring 0
 * at an address a program chose, which is the one class of bug this
 * boundary exists to make impossible.
 */
static int64_t sys_sock_send(addr_space_t *as, vfs_desc_t *d,
                             uint64_t uptr, uint64_t len) {
    if (!d->connected) return -VXE_NOTCONN;
    if (d->busy_tx) {
        app_refuse("send: another thread is already sending on that socket");
        return -VXE_BUSY;
    }
    if (len > VFS_SOCK_BOUNCE) len = VFS_SOCK_BOUNCE;
    if (!len) return 0;

    if (!user_range_mapped(as, uptr, len, 0)) {
        app_refuse("send: unreadable buffer");
        return -VXE_FAULT;
    }
    const int staged = vfs_sock_stage(&d->tx);
    if (staged != 0) return staged;

    const uint8_t *src = (const uint8_t *)(uintptr_t)uptr;
    for (uint64_t i = 0; i < len; i++) d->tx[i] = src[i];

    d->busy_tx = 1;
    d->busy++;
    const int n = d->tls ? vxsec_write(d->sock, d->tx, (int)len)
                         : vxnet_send(d->sock, d->tx, (int)len);
    d->busy--;
    d->busy_tx = 0;

    if (n < 0) return -VXE_IO;
    return n;
}

static int64_t sys_sock_recv(addr_space_t *as, vfs_desc_t *d,
                             uint64_t uptr, uint64_t len) {
    if (!d->connected) return -VXE_NOTCONN;
    if (d->busy_rx) {
        app_refuse("recv: another thread is already receiving on that socket");
        return -VXE_BUSY;
    }
    if (len > VFS_SOCK_BOUNCE) len = VFS_SOCK_BOUNCE;
    if (!len) return 0;

    if (!user_range_mapped(as, uptr, len, 1)) {
        app_refuse("recv: unwritable buffer");
        return -VXE_FAULT;
    }
    const int staged = vfs_sock_stage(&d->rx);
    if (staged != 0) return staged;

    d->busy_rx = 1;
    d->busy++;
    const int n = d->tls ? vxsec_read(d->sock, d->rx, (int)len)
                         : vxnet_recv(d->sock, d->rx, (int)len);
    d->busy--;
    d->busy_rx = 0;

    if (n < 0) return -VXE_IO;
    if (n == 0) return 0;               /* the peer closed: end of stream */

    if (!user_range_mapped(as, uptr, (uint64_t)n, 1)) {
        app_refuse("recv: the buffer stopped being writable while waiting");
        return -VXE_FAULT;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)uptr;
    for (int i = 0; i < n; i++) dst[i] = d->rx[i];
    return n;
}

/* An address and a port in a form a person reading a prompt can act on.
 * "Do you want this program to reach 93.184.216.34:443" is a question
 * with an answer; "do you want this program to use the network" is
 * not. */
static void sys_net_describe(char *out, int cap, const uint8_t ip[4],
                             uint16_t port) {
    char nb[16];
    out[0] = '\0';
    for (int i = 0; i < 4; i++) {
        uint_to_str(ip[i], nb);
        str_append(out, nb, cap);
        if (i != 3) str_append(out, ".", cap);
    }
    str_append(out, ":", cap);
    uint_to_str(port, nb);
    str_append(out, nb, cap);
}

/*
 * ---- why this is no longer the function the entry stub calls ----
 *
 * It was, and the split happened when a second numbering arrived. A
 * Linux program and a Vextro one both issue `syscall` and their numbers
 * collide — 1 is SYS_PRINT here and `write` there — so something above
 * this has to decide which of the two a given call is written in. That
 * something is syscall_service at the bottom of this file.
 *
 * The reason this one had to be renamed rather than have the routing put
 * at its top is exact and was worth an hour to see: the router
 * *forwards* a translated call back into this switch. Linux `write`
 * becomes native SYS_WRITE, which is 6, and 6 re-entering a router that
 * routes by personality would be read as Linux `close`. One door in, one
 * door below it, and nothing can go round twice.
 */
static uint64_t syscall_native(uint64_t num, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3, uint64_t a4,
                               uint64_t a5) {
    /* a3 is a real argument now -- send and recv carry their flags
     * there -- so it is no longer in this list. a4 and a5 still have no
     * caller and are named here so that the compiler does not have to
     * take a view about it. */
    (void)a4; (void)a5;

    switch (num) {
    case SYS_PRINT: {
        char buf[256];
        if (sys_copy_string(a0, buf, sizeof(buf)) < 0) {
            app_refuse("print: unreadable string");
            return (uint64_t)-1;
        }
        if (!silent_launch) term_print(buf);
#ifdef APP_SELFTEST
        /* The terminal is a framebuffer window; a headless harness cannot
         * read it. Mirror to serial so what the app says is checkable. */
        serial_puts("[app] "); serial_puts(buf);
#endif
        return 0;
    }

    case SYS_DRAW_PIXEL: {
        int32_t px = (int32_t)a0;
        int32_t py = (int32_t)a1;
        if (px >= 0 && px < APP_CANVAS_W && py >= 0 && py < APP_CANVAS_H)
            app_surf_current()[py * APP_CANVAS_W + px] = (uint32_t)a2;
        /* The compositor reads this to know whether a preview is worth
         * rescaling. One store per pixel-plotting call is nothing beside
         * the syscall that carried it. */
        if (vmm_current && vmm_current->surface) vmm_current->surface->gen++;
        return 0;
    }

    case SYS_GET_MOUSE: {
        if (vmm_current && !user_range_mapped(vmm_current, a0, 16, 1)) {
            app_refuse("mouse: unwritable buffer");
            return (uint64_t)-1;
        }
        int32_t *out = (int32_t *)(uintptr_t)a0;
        if (!out) return (uint64_t)-1;
        out[0] = mouse_x;
        out[1] = mouse_y;
        out[2] = (int32_t)mouse_buttons;
        out[3] = 0;
        return 0;
    }

    case SYS_EXIT:
        /* The parent is told here rather than at the reap. The reaper
         * runs on the compositor thread, which during a boot self-test
         * is the thread blocked waiting for this program to finish — so
         * a wait4 satisfied only by a reap would be satisfied only after
         * the waiter returned. See addr_space_t.exit_reported. */
        vls_report_exit(vmm_current, (int)a0, 0);
        sched_exit((int)a0);
        return 0;                       /* never reached */

    case SYS_SBRK:
        return sys_sbrk((int64_t)a0);

    case SYS_WRITE: {
        /*
         * ---- one call, three destinations ----
         *
         * This has taken a descriptor since it was written and has
         * always answered for 1 and 2. Now that descriptors exist it
         * looks the number up: a file, a socket, or the console. A
         * second call number for "write to a file" would have meant
         * every ported program calling the wrong one, since what a
         * program has in its hand is an integer it got from open().
         *
         * The failure value changed with it, from a bare -1 to the
         * negated error numbers the calls from 40 upwards use. Nothing
         * on disk notices: apps/vextro.h has never had a wrapper for
         * this call — a program that prints uses SYS_PRINT — and both
         * values are negative, which is what the one caller in libc
         * tests. It is worth noticing anyway that the old -1 lands on
         * EPERM under the new reading, which is not a wrong answer to
         * "you may not write to that".
         */
        addr_space_t *as = vmm_current;
        const int kind = vfs_kind_of(as, (int64_t)a0);

        if (kind == FD_FREE)  return (uint64_t)(int64_t)-VXE_BADF;
        if (kind == FD_DIR)   return (uint64_t)(int64_t)-VXE_BADF;

        if (kind == FD_FILE || kind == FD_SOCK || kind == FD_DEV) {
            vfs_desc_t *d = vfs_get(as, (int64_t)a0);
            if (!d) return (uint64_t)(int64_t)-VXE_BADF;

            if (kind == FD_SOCK)
                return (uint64_t)sys_sock_send(as, d, a1, a2);

            uint64_t len = a2 > VFS_IO_MAX ? VFS_IO_MAX : a2;
            if (!len) return 0;
            if (!user_range_mapped(as, a1, len, 0)) {
                app_refuse("write: unreadable buffer");
                return (uint64_t)(int64_t)-VXE_FAULT;
            }
            if (kind == FD_DEV)
                return (uint64_t)dev_write(d, (const void *)(uintptr_t)a1,
                                           (uint32_t)len);
            return (uint64_t)vfs_file_write(d, (const void *)(uintptr_t)a1,
                                            (uint32_t)len);
        }

        /* fd 1 and 2 both land in the terminal; there is one console. */
        if (a0 != 1 && a0 != 2) return (uint64_t)(int64_t)-VXE_BADF;
        uint64_t len = a2 > 255 ? 255 : a2;
        if (vmm_current && !user_range_mapped(vmm_current, a1, len, 0)) {
            app_refuse("write: unreadable buffer");
            return (uint64_t)-1;
        }
        char buf[256];
        const char *src = (const char *)(uintptr_t)a1;
        uint64_t i = 0;
        for (; i < len; i++) buf[i] = src[i];
        buf[i] = '\0';
        if (!silent_launch) term_print(buf);
#ifdef APP_SELFTEST
        serial_puts("[app] "); serial_puts(buf);
#endif
        return len;
    }

    case SYS_YIELD:
        sched_yield();
        return 0;

    case SYS_FORK: {
        /*
         * A second thread over a copy-on-write duplicate of this
         * process's address space.
         *
         * Nothing is copied. Every writable page in both copies is made
         * read-only and flagged, and the first write from either side
         * faults into the handler that gives that side a private page.
         * Two instances of a program therefore share every page neither
         * of them writes, which for a program's text is all of it.
         *
         * Returns 0 in the child and the child's pid in the parent, the
         * way it has been done since the seventh edition.
         */
        if (!vmm_current || !cur_thread) return (uint64_t)-1;
        addr_space_t *child = (addr_space_t *)kmalloc(sizeof(addr_space_t));
        if (!child) return (uint64_t)-1;
        for (uint64_t i = 0; i < sizeof(addr_space_t); i++)
            ((uint8_t *)child)[i] = 0;
        if (vmm_fork(child, vmm_current) != 0) {
            vmm_destroy(child);
            kfree(child);
            return (uint64_t)-1;
        }
        child->canvas_va = vmm_current->canvas_va;
        child->tramp_va  = vmm_current->tramp_va;
        /* vmm_fork copied the page tables, so the child already *maps*
         * the parent's surface; what it does not yet have is a claim on
         * it. Without the reference the parent's exit would put the
         * surface back in the pool while the child was still drawing
         * into it, and the next program to launch would inherit a
         * window somebody else was painting. */
        child->surface = vmm_current->surface;
        app_surf_ref(child->surface);

        /* The token is inherited, and the elevation is not. A child of
         * an elevated process starts restricted: the answer at the
         * keyboard was about one program, and a program that could pass
         * it on would only have to fork to launder it. */
        child->sid       = vmm_current->sid;
        child->sid_admin = vmm_current->sid_admin;
        child->token     = UAC_TOKEN_RESTRICTED;

        /* And the network answer is not inherited either, for exactly
         * the reason the elevation is not: it was an answer about one
         * program, and a program that could hand it on would only have
         * to fork to launder it. */
        child->net_ok    = NET_UNASKED;

        /*
         * The open files, duplicated.
         *
         * A failure here is a failure of the whole fork, not something
         * to carry on past: a child that starts with some of its
         * parent's descriptors and not others is a program whose next
         * read comes from the wrong place. vfs_clone_table leaves the
         * partial table behind on the child, which the teardown below
         * releases along with everything else.
         */
        if (vfs_clone_table(child, vmm_current) != 0) {
            app_surf_release(child->surface);
            child->surface = 0;
            if (child->files) { vfs_table_destroy(child->files);
                                child->files = 0; }
            vmm_destroy(child);
            kfree(child);
            app_refuse("fork: could not duplicate the open files");
            return (uint64_t)-1;
        }

        /*
         * ---- and the two things a Linux program expects to survive ----
         *
         * The signal handlers, because POSIX says a child inherits its
         * dispositions: a program that installs a SIGCHLD handler and
         * then forks a worker expects the worker to have the same idea
         * of what SIGPIPE means. Pending signals are *not* inherited and
         * vls_sig_clone explains why — a signal addressed to the parent
         * that was duplicated would be one signal delivered twice.
         *
         * And the personality, because a Linux binary that forks does
         * not get a chance to set it again: the child resumes at the
         * same instruction with the same numbering compiled into it. A
         * child that came back native would make its next call into
         * whatever this system's number happened to mean.
         */
        if (vls_sig_clone(child, vmm_current) != 0) {
            app_surf_release(child->surface);
            child->surface = 0;
            if (child->files) { vfs_table_destroy(child->files);
                                child->files = 0; }
            vmm_destroy(child);
            kfree(child);
            app_refuse("fork: could not duplicate the signal handlers");
            return (uint64_t)-1;
        }
        child->personality = vmm_current->personality;

        /* The child resumes at the same instruction with RAX zero. Its
         * frame is built from the parent's, which the syscall stub has
         * already saved. */
        thread_t *t = sched_fork_thread(cur_thread, child);
        if (!t) {
            app_surf_release(child->surface);
            child->surface = 0;
            vls_sig_free(child);
            vmm_destroy(child);
            kfree(child);
            return (uint64_t)-1;
        }

        /*
         * ---- parentage, which this system did not have until now ----
         *
         * The child's process identity is the tid of the thread that
         * carries it, exactly as at spawn; what is new is the second
         * line. Nothing anywhere recorded who forked whom, because until
         * wait4 and SIGCHLD existed there was nothing to be told and
         * nobody to tell. Recorded once here and never updated: a child
         * whose parent has ended keeps the number, and a wait4 that
         * finds no such process is exactly the answer POSIX gives.
         */
        child->pid  = t->pid;
        child->ppid = vmm_current->pid;
        return t->pid;
    }

    case SYS_TICKS:
        return sched_ticks;

    /*
     * How much memory there is, and how much of it is left.
     *
     * The number has been reserved since the syscall table was written
     * and the case was never added, so every call to it fell through to
     * the refusal at the bottom of this switch and answered -1. Nothing
     * noticed, because nothing in ring 3 asked until sysconf() in the C
     * library had a reason to.
     *
     * Two words in kilobytes: free first, then total. Kilobytes rather
     * than pages because a page size is a thing this system might one
     * day have two of, and a unit that is the same on both sides of the
     * boundary is one less thing for a program to be wrong about.
     */
    case SYS_MEMINFO: {
        if (vmm_current && !user_range_mapped(vmm_current, a0, 16, 1)) {
            app_refuse("meminfo: unwritable buffer");
            return (uint64_t)-1;
        }
        uint64_t *out = (uint64_t *)(uintptr_t)a0;
        if (!out) return (uint64_t)-1;
        out[0] = pmm_free_kb();
        out[1] = pmm_total_kb();
        return 0;
    }

    case SYS_CANVAS: {
        /* Where the window's pixels are, so a program can write them
         * itself instead of paying a syscall per pixel. */
        if (vmm_current && !user_range_mapped(vmm_current, a0, 24, 1)) {
            app_refuse("canvas: unwritable buffer");
            return (uint64_t)-1;
        }
        uint64_t *out = (uint64_t *)(uintptr_t)a0;
        if (!out) return (uint64_t)-1;
        /* The address is unchanged from what it always was; what is
         * behind it is now this process's own pages rather than a
         * canvas shared with everything else running. */
        out[0] = vmm_current ? vmm_current->canvas_va
                             : (uint64_t)(uintptr_t)app_canvas;
        out[1] = APP_CANVAS_W;
        out[2] = APP_CANVAS_H;
        /* A program that asks for this address writes through it
         * afterwards with no syscall to notice, so `gen` cannot be a
         * dirty flag for that path and is not treated as one — the
         * compositor rescales previews on a fixed cadence. What the
         * bump does say is that this surface has been claimed by a
         * program that intends to draw, which is what stops an empty
         * preview appearing for one that never will. */
        if (vmm_current && vmm_current->surface) vmm_current->surface->gen++;
        /* The address in RAX as well, so a caller that treats this as a
         * function returning a pointer -- which is the natural way to
         * write it -- gets one without also reading the buffer. */
        return out[0];
    }

    case SYS_RANDOM: {
        /*
         * Fill a user buffer with hardware entropy.
         *
         * a0 is the buffer, a1 its length. Returns the number of bytes
         * actually written, which the caller must check: a processor
         * whose generator is drained returns fewer, and a program that
         * assumes it got what it asked for is a program using a
         * half-initialised key.
         *
         * The buffer is filled in the kernel and copied out in bounded
         * chunks rather than written through the user pointer directly,
         * so a length that straddles the end of a mapping is refused
         * before any entropy is spent on it.
         */
        uint32_t want = (uint32_t)a1;
        uint32_t done = 0;
        uint8_t chunk[64];

        if (!a0 || want == 0) return 0;
        if (want > (1u << 20)) want = 1u << 20;

        if (vmm_current && !user_range_mapped(vmm_current, a0, want, 1)) {
            app_refuse("random: unwritable buffer");
            return (uint64_t)-1;
        }

        while (done < want) {
            uint32_t n = want - done;
            uint32_t got;
            if (n > sizeof(chunk)) n = sizeof(chunk);

            got = vx_random(chunk, n);
            if (got != n) {
                /* Whatever was produced before the failure is real and
                 * is reported; what was not produced is not invented. */
                for (uint32_t i = 0; i < got; i++)
                    ((uint8_t *)(uintptr_t)a0)[done + i] = chunk[i];
                done += got;
                break;
            }
            for (uint32_t i = 0; i < n; i++)
                ((uint8_t *)(uintptr_t)a0)[done + i] = chunk[i];
            done += n;
        }

        /* Do not leave key material in a kernel buffer that the next
         * caller of this syscall will reuse. */
        for (uint32_t i = 0; i < sizeof(chunk); i++) chunk[i] = 0;
        return done;
    }

    /*
     * ===== SYS_FUTEX =====
     *
     * The kernel half of a lock that is not usually a system call.
     *
     * A mutex in this system used to have nowhere to go. There was one
     * synchronisation primitive available to ring 3 — sched_yield, spun
     * on — which burns a whole scheduling slice per attempt and, at
     * PRIO_NORMAL against a compositor at PRIO_UI, can spin for a very
     * long time waiting for a thread that is ready to run and simply is
     * not being run.
     *
     * A futex splits the problem where the cost actually is. The
     * uncontended case — which is nearly every case — is a compare and
     * exchange on a word in the program's own memory, and the kernel
     * never hears about it at all. Only a collision comes here, and what
     * it asks for is not a lock but a *place to sleep*: park this thread
     * until somebody says that word changed.
     *
     * ---- what the channel is, and why it is not the pointer ----
     *
     * The scheduler's wait channels are addresses that nothing
     * dereferences and everything compares. The obvious channel is the
     * user pointer, and it is wrong: two processes sharing a page map it
     * at different virtual addresses, so a waiter and a waker would
     * agree about the memory and disagree about the channel, and the
     * wake would go nowhere. The physical address is the same number on
     * both sides of that, which is what makes a futex work across a fork
     * rather than only within one process.
     *
     * Resolving it also has a second effect worth having: a word that
     * had been paged out is read back in before anybody sleeps on it,
     * rather than after.
     *
     * It has one cost, and it is worth naming so that it is not
     * rediscovered later as a mystery. A frame is not forever: if the
     * page holding the word is evicted and faulted back in while a
     * thread is parked on it, the word's physical address changes, and a
     * wake issued afterwards names the new frame while the sleeper is
     * still waiting on the old one. The wake is missed. What catches it
     * is the bounded park below -- the waiter returns to its own retry
     * loop within FUTEX_PARK_MS and takes the lock it should already
     * have had -- so the failure mode is a fifth of a second of latency
     * rather than a hang. It cannot happen at all to a lock in a shared
     * mapping, which is flagged PTE_SHARED and never a candidate for
     * eviction; only a lock on the heap can see it.
     *
     * ---- the lost wakeup ----
     *
     * "If the value is still what I expect, sleep" is two statements,
     * and between them is a window in which the value changes and the
     * wake is delivered to a thread that is not yet asleep — which then
     * sleeps, having already been woken. sched_block_on_locked exists
     * for exactly this and is used exactly as its comment describes: the
     * flags are taken before the test and handed to the sleep, so
     * nothing can run in between.
     *
     * Interrupts are already masked here, as they are for the whole of
     * every system call, and application processors never run user
     * threads — so on this machine there is genuinely no other
     * processor that could be the waker. If that changes, this is one of
     * the places src/smp.h lists as needing a real lock.
     */
    case SYS_FUTEX: {
        const uint64_t uaddr = a0;
        const uint64_t op    = a1;
        const uint32_t val   = (uint32_t)a2;

        if (uaddr & 3u) {
            app_refuse("futex: address is not four-byte aligned");
            return (uint64_t)-1;
        }
        if (vmm_current && !user_range_mapped(vmm_current, uaddr, 4, 1)) {
            app_refuse("futex: word is not writable by the caller");
            return (uint64_t)-1;
        }

        /* The channel: the word's physical address, so that every
         * process mapping the page agrees about it. */
        void *chan;
        if (vmm_current) {
            uint64_t phys = vmm_resolve(vmm_current, uaddr);
            if (!phys) {
                app_refuse("futex: word is not resident");
                return (uint64_t)-1;
            }
            chan = (void *)(uintptr_t)phys;
        } else {
            chan = (void *)(uintptr_t)uaddr;
        }

        if (op == FUTEX_WAKE) {
            /* val is how many to release. One is the handover a mutex
             * wants; anything more is a broadcast, which is what a
             * condition variable wants. */
            sched_wake_chan(chan, val > 1);
            return 0;
        }
        if (op != FUTEX_WAIT) {
            app_refuse("futex: unknown operation");
            return (uint64_t)-1;
        }

        uint64_t flags = irq_save();
        const uint32_t seen = *(volatile uint32_t *)(uintptr_t)uaddr;
        if (seen != val) {
            /* It changed before we could sleep, which means the
             * condition the caller was waiting for has already
             * happened. Not an error -- the caller re-tests and
             * proceeds. */
            irq_restore(flags);
            return 1;
        }
        /*
         * A bounded park rather than an indefinite one, re-entered by
         * the caller's own loop. The bound is what stops a program that
         * waits on a word nobody will ever wake from becoming a thread
         * this kernel can never reap, and a spurious return is
         * something every futex caller has to handle anyway.
         */
        sched_block_on_locked(chan, FUTEX_PARK_MS, flags);
        return 0;
    }

    /*
     * ---- the three privileged doors ----
     *
     * The shape is the same in all three and the order matters: copy
     * the arguments across the boundary first, so that what is checked
     * is what is used; then ask the gateway, which may freeze this
     * thread and put a question on the screen; then do the work.
     *
     * Asking *after* the copy is not an accident. The prompt names the
     * file, the key or the sector the program asked for, and a name read
     * out of user memory a second time, after the person has answered
     * about the first, is the oldest trick there is.
     */
    case SYS_FS_WRITE: {
        /* a0 path, a1 buffer, a2 length */
        char path[FS_PATH_MAX];
        if (sys_copy_string(a0, path, sizeof(path)) < 0) {
            app_refuse("fs_write: unreadable path");
            return (uint64_t)-1;
        }
        uint32_t len = (uint32_t)a2;
        if (len > SYS_WRITE_MAX) {
            app_refuse("fs_write: buffer too large");
            return (uint64_t)-1;
        }
        if (vmm_current && !user_range_mapped(vmm_current, a1, len, 0)) {
            app_refuse("fs_write: unreadable buffer");
            return (uint64_t)-1;
        }

        /* The profile boundary first: it is not a question anybody gets
         * to answer, it is another account's private tree. */
        char abs[FS_PATH_MAX];
        fs_abs(path, abs, sizeof(abs));
        if (!prof_may(abs, 1)) {
            app_refuse("fs_write: outside this account's profile");
            return (uint64_t)-1;
        }
        if (!uac_guard("write a file", abs)) return (uint64_t)-1;

        /*
         * The copy happens *after* the question, and the order is not
         * interchangeable.
         *
         * uac_guard blocks -- that is its whole job -- and while this
         * thread is parked at the prompt every other thread on the
         * machine runs. Staging into this buffer beforehand meant a
         * second program entering the same call could overwrite it while
         * the first was frozen: the second is refused, because only one
         * question may be outstanding, but the damage is done at the top
         * of its case rather than at the end, and the first thread then
         * wakes up granted and writes the *second* program's bytes to
         * its own path.
         *
         * Copying afterwards is safe for a reason worth writing down.
         * Interrupts are masked for the whole of a system call, and the
         * only place that changes is inside uac_guard, which restores
         * them exactly as it found them and returns with them still
         * masked. So from here to the end of the case nothing else on
         * this processor runs, and application processors never run user
         * threads at all. The copy and the write are one indivisible
         * step.
         *
         * The path is not re-read here, which is the other half of the
         * same argument: it was copied to a stack local before the
         * prompt named it, so what was asked about and what is written
         * to cannot differ.
         */
        static uint8_t staged[SYS_WRITE_MAX];
        const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
        for (uint32_t i = 0; i < len; i++) staged[i] = src[i];

        return fs_write_file(abs, staged, len) == 0 ? len : (uint64_t)-1;
    }

    case SYS_REG_SET: {
        /* a0 key path, a1 value name, a2 string */
        char key[128], name[64], val[192];
        if (sys_copy_string(a0, key, sizeof(key)) < 0 ||
            sys_copy_string(a1, name, sizeof(name)) < 0 ||
            sys_copy_string(a2, val, sizeof(val)) < 0) {
            app_refuse("reg_set: unreadable arguments");
            return (uint64_t)-1;
        }
        char what[UAC_DETAIL_LEN];
        str_copy(what, key, sizeof(what));
        str_append(what, "\\", sizeof(what));
        str_append(what, name, sizeof(what));
        if (!uac_guard("change a registry value", what)) return (uint64_t)-1;
        return reg_set_string(key, name, val) == 0 ? 0 : (uint64_t)-1;
    }

    case SYS_BLK_WRITE: {
        /* a0 LBA, a1 buffer, a2 sector count. The rawest door in the
         * system: it does not know what a file is, so nothing above it
         * can protect anything below it. */
        uint32_t count = (uint32_t)a2;
        if (!count || count > SYS_WRITE_MAX / 512u) {
            app_refuse("blk_write: sector count out of range");
            return (uint64_t)-1;
        }
        uint32_t len = count * 512u;
        if (vmm_current && !user_range_mapped(vmm_current, a1, len, 0)) {
            app_refuse("blk_write: unreadable buffer");
            return (uint64_t)-1;
        }

        char what[UAC_DETAIL_LEN];
        char nb[16];
        str_copy(what, "sector ", sizeof(what));
        uint_to_str((uint32_t)a0, nb);
        str_append(what, nb, sizeof(what));
        str_append(what, ", ", sizeof(what));
        uint_to_str(count, nb);
        str_append(what, nb, sizeof(what));
        str_append(what, " of them", sizeof(what));
        if (!uac_guard("write directly to the disk", what))
            return (uint64_t)-1;

        /* After the question, for the reason set out in SYS_FS_WRITE
         * above: the guard is the only point in a system call where
         * another thread can run, so anything staged before it can be
         * overwritten by a program that is then refused. */
        static uint8_t staged[SYS_WRITE_MAX];
        const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
        for (uint32_t i = 0; i < len; i++) staged[i] = src[i];

        return blk_write(a0, count, staged) == 0 ? len : (uint64_t)-1;
    }

    case SYS_TTF_TEXT_WIDTH: {
        char s[192];
        if (sys_copy_string(a0, s, sizeof(s)) < 0) return 0;
        return (uint64_t)(int64_t)ttf_text_width(s, (int)a1);
    }

    case SYS_TTF_DRAW_STRING: {
        uint64_t v[8];
        if (sys_read_args(a0, v, 8) != 0) {
            app_refuse("ttf_draw_string: unreadable arguments");
            return (uint64_t)-1;
        }
        if (!sys_canvas_ok(v[0], (int64_t)(int32_t)v[1],
                                 (int64_t)(int32_t)v[2])) {
            app_refuse("ttf_draw_string: buffer outside the caller");
            return (uint64_t)-1;
        }
        char s[192];
        if (sys_copy_string(v[5], s, sizeof(s)) < 0) return (uint64_t)-1;
        ttf_draw_string((uint32_t *)(uintptr_t)v[0], (int)v[1], (int)v[2],
                        (int)v[3], (int)v[4], s,
                        (uint32_t)v[6], (int)v[7]);
        return 0;
    }

    case SYS_GFX_RECT: {
        uint64_t v[8];
        if (sys_read_args(a0, v, 8) != 0) {
            app_refuse("gfx_rect: unreadable arguments");
            return (uint64_t)-1;
        }
        if (!sys_canvas_ok(v[0], (int64_t)(uint32_t)v[1],
                                 (int64_t)(uint32_t)v[2])) {
            app_refuse("gfx_rect: buffer outside the caller");
            return (uint64_t)-1;
        }
        gfx_rect((uint32_t *)(uintptr_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                 (int32_t)v[3], (int32_t)v[4], (int32_t)v[5], (int32_t)v[6],
                 (uint32_t)v[7]);
        return 0;
    }

    /*
     * ===== THE MEMORY CALLS =====
     *
     * mmap, munmap and mprotect: address space reserved, released, and
     * re-permitted. The reservation machinery is in src/vmm.h and the
     * long note there explains why a promise is a record rather than a
     * mapping; what is here is the boundary — argument checking, the
     * address decision, and the one refusal that is a security property
     * rather than a validation.
     */
    case SYS_MMAP: {
        /* a0 hint, a1 length, a2 protection, a3 flags */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)-1;

        const uint64_t prot  = a2;
        const uint64_t flags = a3;

        if (!a1) { app_refuse("mmap: zero length"); return (uint64_t)-1; }

        /*
         * Anonymous or nothing.
         *
         * There is no file to map, because ring 3 has no way to open one
         * -- the only file system call a program has is SYS_FS_WRITE, and
         * it takes a path and writes it whole. A port that asks to map a
         * descriptor is asking for something this system cannot do, and
         * the honest answer is a refusal it can see rather than a page of
         * zeros where it expected a file.
         */
        if (!(flags & VX_MAP_ANONYMOUS)) {
            app_refuse("mmap: only anonymous mappings exist here");
            return (uint64_t)-1;
        }

        uint64_t len = PAGE_ALIGN_UP(a1);
        if (len < a1) { app_refuse("mmap: length overflows"); return (uint64_t)-1; }

        /*
         * Neither writable nor executable, ever, in one page.
         *
         * This is the same rule the loader has always applied to an image
         * and it has to be applied here too, or it is not a rule. A
         * just-in-time compiler's whole method is to ask for memory it
         * can write and then run, and if mmap will hand that out then
         * everything the two-segment linker script and the page-by-page
         * loader accomplish is one system call away from being undone.
         *
         * Refused rather than quietly downgraded. A JIT given
         * non-executable pages it believes are executable does not fail
         * at the mmap; it fails much later, jumping into a page it wrote
         * machine code into, with a fault that names an address in the
         * middle of a buffer and nothing to connect it to this decision.
         */
        if ((prot & VX_PROT_WRITE) && (prot & VX_PROT_EXEC)) {
            app_refuse("mmap: a page may be writable or executable, "
                       "not both");
            return (uint64_t)-1;
        }

        uint64_t pte = PTE_USER;
        if (prot & VX_PROT_WRITE) pte |= PTE_WRITE;
        if (!(prot & VX_PROT_EXEC)) pte |= PTE_NX;

        uint64_t base;
        if ((flags & VX_MAP_FIXED) && a0) {
            base = PAGE_ALIGN_DOWN(a0);
            if (!user_range_ok(base, len)) {
                app_refuse("mmap: fixed address outside user space");
                return (uint64_t)-1;
            }
            /* A fixed mapping replaces whatever was there, which is what
             * MAP_FIXED means everywhere else and what a program relies
             * on when it carves a smaller mapping out of a larger
             * reservation it already holds. */
            vmm_unmap_range(as, base, len);
            vmm_area_remove(as, base, len);
        } else {
            base = as->mmap_next;
            if (base + len < base || base + len > USER_MMAP_MAX) {
                app_refuse("mmap: the mapping region is exhausted");
                return (uint64_t)-1;
            }
            /* A page of separation between consecutive mappings, so that
             * a run off the end of one lands in nothing rather than in
             * the next -- the same argument kstack_alloc makes for
             * kernel stacks, for the same price. */
            as->mmap_next = base + len + PAGE_SIZE;
        }

        if (vmm_area_add(as, base, len, pte,
                         (flags & VX_MAP_FIXED) ? VMA_FIXED : 0) != 0) {
            app_refuse("mmap: too many separate mappings");
            return (uint64_t)-1;
        }
        return base;
    }

    case SYS_MUNMAP: {
        /* a0 address, a1 length */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)-1;
        if (!a1) return 0;

        uint64_t base = PAGE_ALIGN_DOWN(a0);
        uint64_t len  = PAGE_ALIGN_UP(a0 + a1) - base;
        if (!user_range_ok(base, len)) {
            app_refuse("munmap: range outside user space");
            return (uint64_t)-1;
        }
        /*
         * The image, the trampolines, the canvas and the stack are not a
         * program's to unmap. Nothing stops a program trying -- the
         * addresses are in its own half of the space -- and a program
         * that unmapped its own trampoline page would take every later
         * system call with it, for reasons that would appear as a fault
         * on an address the program never named.
         */
        if (base < USER_MMAP_BASE) {
            app_refuse("munmap: that range was not obtained from mmap");
            return (uint64_t)-1;
        }

        vmm_unmap_range(as, base, len);
        if (vmm_area_remove(as, base, len) != 0) {
            app_refuse("munmap: no room to split the reservation");
            return (uint64_t)-1;
        }

        /* Give the addresses back if they were the last handed out. Not
         * a general free list -- the region is forty-eight terabytes and
         * does not need one -- but the allocate-and-release-in-order
         * pattern is what a thread pool does with its stacks, and left
         * unhandled it would walk the region for as long as the program
         * ran. */
        if (base + len + PAGE_SIZE == as->mmap_next) as->mmap_next = base;
        return 0;
    }

    case SYS_MPROTECT: {
        /* a0 address, a1 length, a2 protection */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)-1;
        if (!a1) return 0;

        uint64_t base = PAGE_ALIGN_DOWN(a0);
        uint64_t len  = PAGE_ALIGN_UP(a0 + a1) - base;
        if (!user_range_ok(base, len)) {
            app_refuse("mprotect: range outside user space");
            return (uint64_t)-1;
        }
        /* The same refusal as mmap, and it has to be in both. Asking for
         * writable-and-executable in one step and asking for it in two
         * are the same request. */
        if ((a2 & VX_PROT_WRITE) && (a2 & VX_PROT_EXEC)) {
            app_refuse("mprotect: a page may be writable or executable, "
                       "not both");
            return (uint64_t)-1;
        }
        /*
         * And a program may not re-permit memory it did not get from
         * mmap. Without this line the image's own text is one call away
         * from being writable -- the range is in the caller's half of the
         * space, so every other check here passes -- and W^X would hold
         * only for programs that did not think to ask.
         */
        if (base < USER_MMAP_BASE) {
            app_refuse("mprotect: that range was not obtained from mmap");
            return (uint64_t)-1;
        }

        uint64_t pte = PTE_USER;
        if (a2 & VX_PROT_WRITE) pte |= PTE_WRITE;
        if (!(a2 & VX_PROT_EXEC)) pte |= PTE_NX;
        vmm_protect_range(as, base, len, pte);
        return 0;
    }

    /*
     * ===== THE THREAD CALLS =====
     */
    case SYS_CLONE: {
        /* a0 entry, a1 stack top, a2 argument, a3 thread pointer */
        addr_space_t *as = vmm_current;
        if (!as || !as->live || !cur_thread) return (uint64_t)-1;

        if (!user_range_ok(a0, 1) || !user_range_ok(a1, 1)) {
            app_refuse("clone: entry or stack outside user space");
            return (uint64_t)-1;
        }
        /*
         * The stack has to be there before the thread is, and this is the
         * one place that can check it. A new thread does not start by
         * calling anything -- it is IRETQ-ed into with RSP already
         * loaded -- so the first instruction it executes pushes, and a
         * stack that was merely *reserved* would fault on that push with
         * no frame yet built and nothing to attribute it to.
         *
         * user_range_mapped is what backs it: a reservation is faulted in
         * by the check itself. The word below the top, because the top is
         * one past the end of the allocation and is not itself part of
         * it.
         */
        if (!user_range_mapped(as, a1 - 16, 16, 1)) {
            app_refuse("clone: the new thread's stack is not writable");
            return (uint64_t)-1;
        }
        /* Sixteen-byte aligned, as the ABI requires of a function's stack
         * on entry. A thread whose stack is misaligned runs correctly
         * until the first MOVAPS against a local, which is a fault a long
         * way from the call that caused it. */
        if (a1 & 15u) {
            app_refuse("clone: the stack pointer is not sixteen-aligned");
            return (uint64_t)-1;
        }

        thread_t *t = sched_spawn_thread(as, a0, a1, a2, a3, cur_thread->name);
        if (!t) {
            app_refuse("clone: no room for another thread");
            return (uint64_t)-1;
        }
        return t->pid;
    }

    /*
     * End this thread and nothing else.
     *
     * The same code SYS_EXIT runs, under a name that says which of the
     * two meanings is wanted. Until a process could have two threads the
     * distinction did not exist and SYS_EXIT meant both; it keeps meaning
     * what it always did, which is this, and the whole-process form is
     * the new one below.
     */
    case SYS_THREAD_EXIT:
        /* Only if this is the last thread standing in the space, since
         * this call ends one thread and not the program. A worker
         * finishing is not a process ending and its parent must not be
         * told that it is. */
        if (vmm_current && vmm_current->refs <= 1)
            vls_report_exit(vmm_current, (int)a0, 0);
        sched_exit((int)a0);
        return 0;                       /* never reached */

    /*
     * End every thread of this process.
     *
     * What exit() means in C, and what main() returning means. A library
     * that starts worker threads and then calls exit expects the program
     * to stop, not to keep running on the workers with the thread that
     * called it gone.
     *
     * Marking the siblings is enough to end them and it is worth saying
     * why, because it looks too easy. A thread marked T_ZOMBIE is never
     * chosen by sched_pick again, so it executes no further user
     * instruction from this moment. What it does not do is release its
     * kernel stack -- the reaper does that, on the compositor thread, for
     * the same reason a thread cannot free the stack it is standing on.
     *
     * There is no thread of this process running anywhere else while this
     * executes: interrupts are masked for the whole of a system call, and
     * user threads are pinned to processor zero precisely so that this
     * kind of statement stays true. A sibling parked in the futex service
     * is asleep in the kernel rather than running, and is reaped from the
     * sleeping state exactly as one that had exited.
     */
    case SYS_EXIT_GROUP: {
        addr_space_t *as = vmm_current;
        if (as && cur_thread) {
            for (int i = 0; i < SCHED_MAX_THREADS; i++) {
                thread_t *t = threads[i];
                if (!t || t == cur_thread) continue;
                if (t->as != as || t->state == T_FREE) continue;
                t->exit_code = (int)a0;
                t->state     = T_ZOMBIE;
            }
        }
        vls_report_exit(as, (int)a0, 0);
        sched_exit((int)a0);
        return 0;                       /* never reached */
    }

    case SYS_GETTID:
        return cur_thread ? cur_thread->pid : 0;

    /*
     * ===== THE FIVE CALLS A SECOND PROCESS NEEDS =====
     *
     * Native numbers rather than Linux-only ones, because each is
     * something a program written for *this* system now wants and none
     * of them is an emulation of anything: a Vextro program that forks
     * has to be able to wait, and a shell written here has to be able to
     * replace itself. The Linux table in src/sched/vls_core.c forwards
     * to these rather than reimplementing them.
     *
     * The signal calls are deliberately *not* here. Those are Linux's
     * shape — a sigaction structure, a sixty-four bit mask, a frame laid
     * on the user stack — and giving them native numbers as well would
     * be a second door into one room. They are reachable through the
     * call bias, which is how the test suite for them is written.
     */
    case SYS_EXECVE:
        return (uint64_t)sys_execve(a0, a1, a2);

    case SYS_WAIT4:
        return (uint64_t)vls_wait4(a0, a1, a2, a3);

    /*
     * ---- duplicating a descriptor, and what cannot be duplicated ----
     *
     * A descriptor here *is* the open file description: the offset, the
     * write-back image and the socket handle all live in the same
     * structure the number indexes. Unix keeps them separate, so two of
     * its descriptors can share one position in a file; this system has
     * nowhere to put a shared description, which src/vfs.h already says
     * at the point a fork declines to duplicate a socket.
     *
     * That makes three cases rather than one:
     *
     *   A device node, a console stream, a directory or a file opened
     *   for reading has no mutable state that two holders could
     *   disagree about. Those duplicate exactly, and they are what
     *   `dup2(fd, 1)` is nearly always used on — redirecting output to
     *   /dev/null or to a log is the whole idiom.
     *
     *   A file opened for *writing* has a write-back image, and two
     *   images of one file both flushed at close means whichever closed
     *   last silently wins. Refused, by name.
     *
     *   A socket, for the reason vfs_clone_table gives at length: two
     *   holders of one connection do not each get the response, they get
     *   alternating halves of it.
     */
    case SYS_DUP:
    case SYS_DUP2: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        const int kind = vfs_kind_of(as, (int64_t)a0);
        if (kind == FD_FREE) return (uint64_t)(int64_t)-VXE_BADF;

        if (num == SYS_DUP2 && (int64_t)a0 == (int64_t)a1) {
            /* dup2 onto itself is defined to be a no-op that still
             * checks the descriptor is open, which the line above did. */
            return a1;
        }

        struct proc_files *f = vfs_files(as, 1);
        if (!f) return (uint64_t)(int64_t)-VXE_NOMEM;

        if (kind == FD_SOCK) {
            app_refuse("dup: a connection cannot be held twice on this "
                       "system");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }

        vfs_desc_t *s = &f->d[a0 < FD_MAX ? a0 : 0];
        if (kind == FD_FILE && s->writable) {
            app_refuse("dup: a file open for writing has one write-back "
                       "image and cannot be held twice");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }

        int nfd;
        if (num == SYS_DUP2) {
            if ((int64_t)a1 < 0 || a1 >= FD_MAX)
                return (uint64_t)(int64_t)-VXE_BADF;
            nfd = (int)a1;
            /* The target is closed first, and silently, which is what
             * dup2 promises: it is how a program points its own output
             * somewhere else without a window in which it has none. */
            if (f->d[nfd].kind != FD_FREE && f->d[nfd].kind != FD_CONSOLE)
                vfs_close_desc(&f->d[nfd]);
        } else {
            nfd = vfs_alloc(as);
            if (nfd < 0) return (uint64_t)(int64_t)nfd;
        }

        vfs_desc_t *o = &f->d[nfd];
        *o = *s;
        o->wbuf = 0; o->wcap = 0; o->ents = 0; o->rx = 0; o->tx = 0;
        o->busy = o->busy_rx = o->busy_tx = 0;
        /* A duplicate is not close-on-exec however the original was
         * marked, and that is POSIX rather than a simplification: the
         * whole point of dup2 before an exec is to hand the new image a
         * descriptor. */
        o->cloexec = 0;

        if (kind == FD_DIR && s->ents && s->nents) {
            const uint64_t bytes = (uint64_t)s->nents * sizeof(vx_dirent_t);
            o->ents = (vx_dirent_t *)kmalloc_pool(bytes, KPOOL_PAGED);
            if (!o->ents) { vfs_desc_reset(o); return (uint64_t)(int64_t)-VXE_NOMEM; }
            for (uint64_t k = 0; k < bytes; k++)
                ((uint8_t *)o->ents)[k] = ((const uint8_t *)s->ents)[k];
        }
        return (uint64_t)nfd;
    }

    /*
     * ---- which numbering this process speaks ----
     *
     * A one-way door, and the refusal to go back is the whole safety of
     * it: a program that could return to the native numbering
     * half-way through would have made some calls in one numbering and
     * some in the other, and 1 means SYS_PRINT in one and `write` in the
     * other. Returns the previous value, so a caller can tell that it
     * took.
     *
     * A Linux binary loaded by a future loader will have this set for it
     * before its first instruction. Until then it is set by a program
     * about itself, which is what makes the subset testable from a
     * program built in this tree.
     */
    case SYS_PERSONALITY: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;
        const uint64_t was = as->personality;
        if (a0 == VLS_PERSONALITY_LINUX) {
            if (!as->personality) {
                serial_puts("[VLS] pid ");
                serial_put_dec(as->pid);
                serial_puts(" (");
                serial_puts(cur_thread ? cur_thread->name : "?");
                serial_puts(") now speaks the linux abi\n");
            }
            as->personality = VLS_PERSONALITY_LINUX;
        } else if (a0 != VLS_PERSONALITY_NATIVE) {
            return (uint64_t)(int64_t)-VXE_INVAL;
        } else if (as->personality == VLS_PERSONALITY_LINUX) {
            app_refuse("personality: a process cannot go back to the "
                       "native numbering");
            return (uint64_t)(int64_t)-VXE_PERM;
        }
        return was;
    }

    /*
     * Where this thread's own variables are.
     *
     * The address is not checked for being *mapped*, only for being in
     * user space, and that is deliberate. A thread control block is
     * ordinary memory the program allocated; if it is wrong, the program
     * faults reading its own thread-local variable, in its own address
     * space, at an address it chose. That is a bug in the program and it
     * is reported as one. What the check prevents is the case that is not
     * the program's business at all: a base in the higher half, which
     * would let a `__thread` reference read kernel memory.
     */
    case SYS_SET_FSBASE:
        if (a0 && !user_range_ok(a0, 1)) {
            app_refuse("set_fsbase: base outside user space");
            return (uint64_t)-1;
        }
        sched_set_fsbase(a0);
        return 0;

    /*
     * Nanoseconds since boot.
     *
     * Derived from the tick count rather than from the timestamp counter,
     * which is the less precise of the two and the one that is actually
     * monotonic here: TSC frequency is not something this kernel
     * calibrates, and a value that jumped when the processor changed
     * speed would be worse than a coarse one. The tick is a millisecond,
     * so the bottom six digits are always zero and the unit is a promise
     * about the scale rather than about the resolution.
     */
    case SYS_CLOCK:
        return sched_ticks * 1000000ull;

    /*
     * The calendar, as seconds since 1970.
     *
     * The reading comes straight off the CMOS chip on every call rather
     * than being latched at boot and advanced by the tick. That costs
     * six port reads and buys the property that matters: a machine
     * suspended for an hour comes back with the right date, and a
     * kernel that had cached one would come back an hour behind and
     * stay there.
     */
    case SYS_WALLCLOCK:
        return (uint64_t)rtc_unix_seconds();

    /*
     * Sleep, in milliseconds.
     *
     * A program could build this out of SYS_TICKS and SYS_YIELD, and one
     * that did would spin: yielding in a loop keeps the thread ready to
     * run, so the scheduler keeps running it, and a hundred millisecond
     * wait costs a hundred milliseconds of processor rather than none.
     * Asking to be taken off the queue is the thing that cannot be done
     * from user space.
     */
    case SYS_NANOSLEEP: {
        uint64_t ms = a0;
        if (!ms) { sched_yield(); return 0; }
        if (ms > 60000ull) ms = 60000ull;
        sched_sleep_ms(ms);
        return 0;
    }

    /*
     * ===== THE DESCRIPTOR CALLS =====
     *
     * Everything from here down returns a negated error number rather
     * than a bare -1; src/syscall.h explains why at the point the
     * numbers are assigned, and the short version is that a port which
     * cannot tell ENOENT from EACCES from EISDIR cannot decide whether
     * to create the file, ask for a password, or give up.
     *
     * Two rules hold across all of them and are worth stating once
     * rather than eighteen times:
     *
     *   A kernel-mode caller is refused. `vmm_current` being null means
     *   the shell or a boot self-test is calling, and that code has
     *   fs_open and vxnet_socket directly — there is no process for a
     *   descriptor to belong to and no table to put one in.
     *
     *   A path is copied across the boundary before anything looks at
     *   it twice, and normalised once. What is checked is what is used,
     *   which is the property sys_copy_string exists for.
     */
    case SYS_OPEN: {
        /* a0 path, a1 flags, a2 mode.
         *
         * The mode is accepted and ignored. There are no permission
         * bits on this volume — what may touch a file is decided by the
         * profile the path is in and by uac_guard — so storing a
         * caller's 0644 would be recording a number nothing ever reads,
         * and refusing the argument would break every port that passes
         * one. */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;
        (void)a2;

        char path[FS_PATH_MAX];
        if (sys_copy_string(a0, path, sizeof(path)) < 0) {
            app_refuse("open: unreadable path");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        char abs[FS_PATH_MAX];
        fs_abs(path, abs, sizeof(abs));

        const uint32_t fl  = (uint32_t)a1;
        const uint32_t acc = fl & VX_O_ACCMODE;
        const int wants_write = (acc == VX_O_WRONLY || acc == VX_O_RDWR);

        /*
         * ---- a device node is not a file, and none of the three
         *      questions below is about it ----
         *
         * This has to be decided here rather than inside vfs_open,
         * because all three gates are applied before vfs_open is
         * reached and every one of them would give a wrong answer.
         *
         * The profile boundary is about another account's private tree,
         * and /dev is in nobody's profile. The pagefile guard is about a
         * run of raw sectors, and /dev has none. And the elevation
         * gateway is about "the three ways a program can change this
         * machine permanently" — rewriting NTFS metadata, rewriting the
         * registry, writing raw sectors — of which writing to /dev/null
         * is none. `open("/dev/null", O_RDWR)` is what a program does
         * before it has decided to do anything at all, and it put a
         * password prompt on the screen, which is precisely how somebody
         * is trained to click yes.
         *
         * The render node is the interesting case and it comes out the
         * same way: a write to it lands in this process's own window
         * surface, which the process already has mapped at canvas_va and
         * may write to freely without asking anyone.
         */
        const int is_dev = dev_claims(abs);

        /* The profile boundary first: it is not a question anybody gets
         * to answer, it is another account's private tree. */
        if (!is_dev && !prof_may(abs, wants_write)) {
            app_refuse("open: outside this account's profile");
            return (uint64_t)(int64_t)-VXE_ACCES;
        }
        if (!is_dev && wants_write && swap_owns_path(abs)) {
            app_refuse("open: the pagefile is in use");
            return (uint64_t)(int64_t)-VXE_BUSY;
        }

        /*
         * ---- the question, and where it is asked ----
         *
         * At open, and never at close. That placement is what makes the
         * write-back scheme in src/vfs.h safe rather than merely
         * convenient: uac_guard works by freezing a ring-3 thread and
         * putting a prompt on the screen, and by the time a descriptor's
         * image is being written the program may have ended — there
         * would be no thread to freeze and nobody the question is about.
         * Asking here means the grant is already in hand when the bytes
         * reach the disk, including when that happens on the janitor's
         * thread after the process is gone.
         */
        if (!is_dev && wants_write && !uac_guard("write a file", abs))
            return (uint64_t)(int64_t)-VXE_PERM;

        /*
         * The descriptor is taken *after* the prompt, not before.
         *
         * uac_guard is the one point in a system call where another
         * thread of this same process runs, and a number reserved
         * beforehand is a number that thread could be handed too. This
         * is the same hazard the long note on SYS_FS_WRITE works
         * through, in its other form.
         */
        const int fd = vfs_alloc(as);
        if (fd < 0) return (uint64_t)(int64_t)fd;

        vfs_desc_t *d = &as->files->d[fd];
        const int rc = vfs_open(d, abs, fl);
        if (rc != 0) {
            vfs_desc_reset(d);
            return (uint64_t)(int64_t)rc;
        }
        return (uint64_t)(int64_t)fd;
    }

    case SYS_READ: {
        /* a0 fd, a1 buffer, a2 length */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        const int kind = vfs_kind_of(as, (int64_t)a0);
        if (kind == FD_FREE) return (uint64_t)(int64_t)-VXE_BADF;

        if (kind == FD_CONSOLE) {
            /*
             * There is no console input in ring 3, and this says so
             * rather than answering zero.
             *
             * Zero is end-of-file, and a parser told it has reached the
             * end of a file it never opened will conclude the input was
             * empty and carry on — which is the exact failure the note
             * at the top of libc/include/stdio.h says fread and fgets
             * were left out to avoid. The keyboard belongs to the
             * terminal window, and giving a background program a claim
             * on it is a feature with a design, not a return value.
             */
            app_refuse("read: there is no console input in ring 3");
            return (uint64_t)(int64_t)-VXE_IO;
        }

        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (kind == FD_DIR) return (uint64_t)(int64_t)-VXE_ISDIR;
        if (kind == FD_SOCK) return (uint64_t)sys_sock_recv(as, d, a1, a2);

        uint64_t len = a2 > VFS_IO_MAX ? VFS_IO_MAX : a2;
        if (!len) return 0;
        if (!user_range_mapped(as, a1, len, 1)) {
            app_refuse("read: unwritable buffer");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        /* A device node reads through src/devfs.h. The range check above
         * is the same one the file path gets and is what makes it safe
         * for either to write straight through the caller's pointer. */
        if (kind == FD_DEV)
            return (uint64_t)dev_read(d, (void *)(uintptr_t)a1,
                                      (uint32_t)len);
        return (uint64_t)vfs_file_read(d, (void *)(uintptr_t)a1,
                                       (uint32_t)len);
    }

    case SYS_CLOSE: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        const int kind = vfs_kind_of(as, (int64_t)a0);
        if (kind == FD_FREE) return (uint64_t)(int64_t)-VXE_BADF;

        /* Closing a console stream is a real operation, not a no-op:
         * it frees the number, and vfs_alloc hands the lowest free one
         * back out — which is how `close(0); open(...)` has redirected
         * a program's own input since the seventh edition. The table is
         * created here if it did not exist, because that is the only
         * place the fact can be recorded. */
        struct proc_files *f = vfs_files(as, 1);
        if (!f) return (uint64_t)(int64_t)-VXE_NOMEM;

        vfs_desc_t *d = &f->d[a0];
        if (d->busy) {
            app_refuse("close: a call is still waiting in that descriptor");
            return (uint64_t)(int64_t)-VXE_BUSY;
        }
        /* Marked before the work, because closing a socket parks and a
         * sibling could reach this same descriptor while it does. It is
         * cleared by the reset inside vfs_close_desc. */
        d->busy = 1;
        vfs_close_desc(d);
        return 0;
    }

    case SYS_LSEEK: {
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) {
            return (uint64_t)(int64_t)(vfs_kind_of(as, (int64_t)a0)
                                       == FD_CONSOLE ? -VXE_SPIPE
                                                     : -VXE_BADF);
        }
        return (uint64_t)vfs_seek(d, (int64_t)a1, (int)a2);
    }

    case SYS_STAT: {
        /* a0 path, a1 a vx_stat_t to fill in */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        char path[FS_PATH_MAX];
        if (sys_copy_string(a0, path, sizeof(path)) < 0) {
            app_refuse("stat: unreadable path");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        if (!user_range_mapped(as, a1, sizeof(vx_stat_t), 1)) {
            app_refuse("stat: unwritable buffer");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }

        char abs[FS_PATH_MAX];
        fs_abs(path, abs, sizeof(abs));

        /*
         * The device nodes answer here as well as at open, and that is
         * not a duplication of effort — it is the more important of the
         * two. A program almost never opens /dev/null blind: it stats it
         * first, to find out whether this system has one, and decides
         * from the answer whether to redirect or to fall back. A stat
         * that said "no such file" for a node the open would have
         * succeeded on is a system whose devices exist and cannot be
         * found.
         */
        if (dev_claims(abs)) {
            vx_stat_t *dst = (vx_stat_t *)(uintptr_t)a1;
            const dev_node_t *n = dev_lookup(abs);
            if (n) dev_stat(n, dst); else dev_stat_dir(dst);
            return 0;
        }

        uint64_t size = 0, ino = 0;
        int is_dir = 0;
        /* A path in another account's profile answers "no such file",
         * not "permission denied", and that is deliberate: the second
         * answer tells a program that something it may not look at
         * exists, which is itself the thing being kept from it. */
        if (!fs_stat_ex(abs, &size, &is_dir, &ino))
            return (uint64_t)(int64_t)-VXE_NOENT;

        vfs_fill_stat((vx_stat_t *)(uintptr_t)a1, size, is_dir, ino);
        return 0;
    }

    case SYS_FSTAT: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;
        if (!user_range_mapped(as, a1, sizeof(vx_stat_t), 1)) {
            app_refuse("fstat: unwritable buffer");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }

        const int kind = vfs_kind_of(as, (int64_t)a0);
        if (kind == FD_FREE) return (uint64_t)(int64_t)-VXE_BADF;

        vx_stat_t *st = (vx_stat_t *)(uintptr_t)a1;
        if (kind == FD_CONSOLE) {
            vfs_fill_stat(st, 0, 0, 0);
            st->mode = VX_S_IFCHR;
            return 0;
        }

        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;

        if (kind == FD_SOCK) {
            vfs_fill_stat(st, 0, 0, 0);
            st->mode = VX_S_IFSOCK;
            return 0;
        }
        if (kind == FD_DEV) {
            const dev_node_t *n = dev_lookup(d->path);
            if (!n) return (uint64_t)(int64_t)-VXE_BADF;
            dev_stat(n, st);
            return 0;
        }
        if (kind == FD_DIR) {
            vfs_fill_stat(st, d->nents, 1, 0);
            return 0;
        }
        /* The size a program should see is the one it would read back,
         * which for a descriptor being written is the image and not what
         * is presently on the disk. */
        vfs_fill_stat(st, (d->writable && d->wbuf) ? d->wlen : d->file.size,
                      0, d->file.valid ? d->file.ntfs_record : 0);
        return 0;
    }

    case SYS_GETDENTS: {
        /* a0 fd, a1 buffer, a2 bytes. Returns the number of *entries*
         * written, not bytes: the records are fixed-length, so a count
         * is the useful form and a byte total would only have to be
         * divided again. */
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_DIR) return (uint64_t)(int64_t)-VXE_NOTDIR;

        const uint64_t cap = a2 / sizeof(vx_dirent_t);
        if (!cap) return (uint64_t)(int64_t)-VXE_INVAL;
        if (d->pos >= d->nents) return 0;

        uint64_t n = d->nents - d->pos;
        if (n > cap) n = cap;

        const uint64_t bytes = n * sizeof(vx_dirent_t);
        if (!user_range_mapped(as, a1, bytes, 1)) {
            app_refuse("getdents: unwritable buffer");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        const uint8_t *src = (const uint8_t *)&d->ents[d->pos];
        uint8_t *dst = (uint8_t *)(uintptr_t)a1;
        for (uint64_t i = 0; i < bytes; i++) dst[i] = src[i];
        d->pos += n;
        return n;
    }

    case SYS_UNLINK: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        char path[FS_PATH_MAX];
        if (sys_copy_string(a0, path, sizeof(path)) < 0) {
            app_refuse("unlink: unreadable path");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        char abs[FS_PATH_MAX];
        fs_abs(path, abs, sizeof(abs));

        if (!prof_may(abs, 1)) {
            app_refuse("unlink: outside this account's profile");
            return (uint64_t)(int64_t)-VXE_ACCES;
        }
        if (swap_owns_path(abs)) {
            app_refuse("unlink: the pagefile is in use");
            return (uint64_t)(int64_t)-VXE_BUSY;
        }
        uint64_t size = 0;
        int is_dir = 0;
        if (!fs_stat(abs, &size, &is_dir))
            return (uint64_t)(int64_t)-VXE_NOENT;

        /* Deleting is the fourth way a program makes a permanent change
         * and goes through the same door as the other three. */
        if (!uac_guard("delete a file", abs))
            return (uint64_t)(int64_t)-VXE_PERM;

        return fs_delete(abs) == 0 ? 0 : (uint64_t)(int64_t)-VXE_IO;
    }

    case SYS_MKDIR: {
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;
        (void)a1;                          /* mode; see SYS_OPEN */

        char path[FS_PATH_MAX];
        if (sys_copy_string(a0, path, sizeof(path)) < 0) {
            app_refuse("mkdir: unreadable path");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        char abs[FS_PATH_MAX];
        fs_abs(path, abs, sizeof(abs));

        if (!prof_may(abs, 1)) {
            app_refuse("mkdir: outside this account's profile");
            return (uint64_t)(int64_t)-VXE_ACCES;
        }
        {
            uint64_t size = 0;
            int is_dir = 0;
            if (fs_stat(abs, &size, &is_dir))
                return (uint64_t)(int64_t)-VXE_EXIST;
        }
        if (!uac_guard("create a directory", abs))
            return (uint64_t)(int64_t)-VXE_PERM;

        return fs_mkdir(abs) == 0 ? 0 : (uint64_t)(int64_t)-VXE_IO;
    }

    case SYS_FSYNC: {
        vfs_desc_t *d = vfs_get(vmm_current, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        return (uint64_t)(int64_t)vfs_flush(d);
    }

    case SYS_FTRUNCATE: {
        vfs_desc_t *d = vfs_get(vmm_current, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_FILE || !d->writable || !d->wbuf)
            return (uint64_t)(int64_t)-VXE_BADF;

        const uint64_t want = a1;
        if (want > (uint64_t)VFS_WBUF_MAX)
            return (uint64_t)(int64_t)-VXE_NOSPC;
        const int rc = vfs_wbuf_reserve(d, want);
        if (rc != 0) return (uint64_t)(int64_t)rc;

        for (uint64_t i = d->wlen; i < want; i++) d->wbuf[i] = 0;
        d->wlen  = (uint32_t)want;
        d->dirty = 1;
        return 0;
    }

    /*
     * ===== THE SOCKET CALLS =====
     *
     * Underneath is src/vxnet.h, which is the same seam the kernel's own
     * browser and package store reach the network through — lwIP for the
     * plain path, Mbed TLS for the encrypted one, and about a quarter of
     * a million lines of it that stops at that header. Nothing here
     * knows what a pbuf is.
     *
     * The TLS caveat is repeated wherever TLS is mentioned because it is
     * the kind of thing that gets discovered rather than read: there is
     * no certificate authority store on this volume, so the chain is
     * parsed and the handshake signature is checked against the key in
     * the leaf, and nothing establishes that the leaf belongs to the
     * host that was asked for. That stops somebody listening. It does
     * not stop somebody in the middle.
     */
    case SYS_SOCKET: {
        /* a0 family, a1 type, a2 protocol */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        if (a0 != VX_AF_INET) {
            app_refuse("socket: only IPv4 exists here");
            return (uint64_t)(int64_t)-VXE_AFNOSUPPORT;
        }
        if (a1 != VX_SOCK_STREAM) {
            /* Refused rather than accepted and made to fail later, for
             * the reason mmap refuses a non-anonymous mapping: lwIP is
             * configured here for TCP, and a datagram socket that
             * returned a descriptor and then could not carry anything
             * would be discovered several layers into a port. */
            app_refuse("socket: only stream sockets exist here");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        const int tls = (a2 == VX_IPPROTO_TLS);
        if (a2 != 0 && a2 != VX_IPPROTO_TCP && !tls) {
            app_refuse("socket: unknown protocol");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        if (tls && !vxsec_ready()) {
            app_refuse("socket: TLS is not available");
            return (uint64_t)(int64_t)-VXE_NETDOWN;
        }

        const int fd = vfs_alloc(as);
        if (fd < 0) return (uint64_t)(int64_t)fd;

        vfs_desc_t *d = &as->files->d[fd];
        vfs_desc_reset(d);
        d->kind     = FD_SOCK;
        d->tls      = tls ? 1 : 0;
        d->readable = 1;
        d->writable = 1;

        /*
         * A plain socket gets its lwIP descriptor now; a TLS one does
         * not get anything until connect, and that asymmetry is
         * vxsec_open's rather than a choice made here — it takes a host
         * and a port and does the resolution, the connection and the
         * handshake as one operation, because a TLS session is not a
         * socket you later encrypt.
         */
        if (!tls) {
            d->sock = vxnet_socket();
            if (d->sock < 0) {
                vfs_desc_reset(d);
                app_refuse("socket: the network is not up");
                return (uint64_t)(int64_t)-VXE_NETDOWN;
            }
        }
        return (uint64_t)(int64_t)fd;
    }

    case SYS_CONNECT: {
        /* a0 fd, a1 four bytes of address, a2 port in host order.
         *
         * Four bytes rather than a sockaddr_in, because that structure's
         * layout is a thing two systems can disagree about and its port
         * field is in network order — a byte-swap that would then have
         * to be got right on both sides of a privilege boundary. The
         * unpacking happens once, in libc/socket.c, where a mistake is a
         * program's own. */
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (d->connected) return (uint64_t)(int64_t)-VXE_ISCONN;
        if (d->busy) return (uint64_t)(int64_t)-VXE_BUSY;
        if (d->tls) {
            app_refuse("connect: a TLS session needs the host's name, "
                       "not its address");
            return (uint64_t)(int64_t)-VXE_INVAL;
        }
        if (!user_range_mapped(as, a1, 4, 0)) {
            app_refuse("connect: unreadable address");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        const uint16_t port = (uint16_t)a2;
        if (!port) return (uint64_t)(int64_t)-VXE_INVAL;

        uint8_t ip[4];
        {
            const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
            for (int i = 0; i < 4; i++) ip[i] = src[i];
        }

        if (!vfs_is_loopback(ip)) {
            char what[UAC_DETAIL_LEN];
            sys_net_describe(what, (int)sizeof(what), ip, port);
            if (!net_guard(what)) return (uint64_t)(int64_t)-VXE_PERM;

            /*
             * The descriptor is looked up again, and this is hazard (a)
             * from src/vfs.h one park earlier than the place it is
             * described.
             *
             * net_guard puts a question on the screen and freezes this
             * thread, and every other thread of this process runs while
             * it waits -- one of which may close this descriptor and open
             * something else, which vfs_alloc will hand the same number
             * to. `d` would then point at a live socket belonging to a
             * sibling, and this call would connect it. The busy count
             * cannot help: it is not raised until after the guard, which
             * is exactly the window.
             */
            d = vfs_get(as, (int64_t)a0);
            if (!d || d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_BADF;
            if (d->connected) return (uint64_t)(int64_t)-VXE_ISCONN;
            if (d->busy) return (uint64_t)(int64_t)-VXE_BUSY;
            if (d->tls) return (uint64_t)(int64_t)-VXE_INVAL;
        }

        d->busy++;
        const int rc = vxnet_connect(d->sock, ip, port);
        d->busy--;
        if (rc != 0) return (uint64_t)(int64_t)-VXE_CONNREFUSED;
        d->connected = 1;
        return 0;
    }

    case SYS_CONNECT_HOST: {
        /* a0 fd, a1 host name, a2 port. The only way to open a TLS
         * session, and the way a plain socket gets a name resolved
         * without the program having to carry a DNS client. */
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (d->connected) return (uint64_t)(int64_t)-VXE_ISCONN;
        if (d->busy) return (uint64_t)(int64_t)-VXE_BUSY;

        char host[128];
        if (sys_copy_string(a1, host, sizeof(host)) < 0) {
            app_refuse("connect: unreadable host name");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        const uint16_t port = (uint16_t)a2;
        if (!port) return (uint64_t)(int64_t)-VXE_INVAL;

        /* Recognised as loopback *before* the guard, because resolving a
         * name is itself a question asked of a server somewhere else —
         * see vfs_host_is_loopback. */
        uint8_t ip[4];
        const int local = vfs_host_is_loopback(host, ip);
        if (!local) {
            char what[UAC_DETAIL_LEN];
            str_copy(what, host, sizeof(what));
            str_append(what, ":", sizeof(what));
            {
                char nb[16];
                uint_to_str(port, nb);
                str_append(what, nb, sizeof(what));
            }
            if (!net_guard(what)) return (uint64_t)(int64_t)-VXE_PERM;

            /* Looked up again after the prompt, for the reason set out
             * in SYS_CONNECT above: the guard is a park, and a sibling
             * may have closed this number and been handed it back for
             * something else. */
            d = vfs_get(as, (int64_t)a0);
            if (!d || d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_BADF;
            if (d->connected) return (uint64_t)(int64_t)-VXE_ISCONN;
            if (d->busy) return (uint64_t)(int64_t)-VXE_BUSY;
        }

        d->busy++;
        int rc;
        if (d->tls) {
            /* Resolution, connection and handshake in one call; the
             * descriptor is a vxsec slot rather than an lwIP one from
             * here on, which `tls` is what records. */
            const int slot = vxsec_open(host, port);
            if (slot >= 0) { d->sock = slot; rc = 0; } else rc = -1;
        } else {
            if (!local && !vxnet_resolve(host, ip)) {
                d->busy--;
                return (uint64_t)(int64_t)-VXE_HOSTUNREACH;
            }
            rc = vxnet_connect(d->sock, ip, port);
        }
        d->busy--;

        if (rc != 0) return (uint64_t)(int64_t)-VXE_CONNREFUSED;
        d->connected = 1;
        return 0;
    }

    case SYS_SEND: {
        /* a0 fd, a1 buffer, a2 length, a3 flags */
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (a3) {
            /* MSG_OOB, MSG_DONTWAIT and the rest. Refused rather than
             * ignored: a caller that asked not to block and was blocked
             * anyway has been lied to. */
            app_refuse("send: no message flags are supported");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        return (uint64_t)sys_sock_send(as, d, a1, a2);
    }

    case SYS_RECV: {
        addr_space_t *as = vmm_current;
        vfs_desc_t *d = vfs_get(as, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (a3) {
            app_refuse("recv: no message flags are supported");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        return (uint64_t)sys_sock_recv(as, d, a1, a2);
    }

    case SYS_SOCKOPT: {
        /* a0 fd, a1 option, a2 value */
        vfs_desc_t *d = vfs_get(vmm_current, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (d->tls) {
            /* The lwIP descriptor a TLS session runs over belongs to
             * Mbed TLS and is not reachable from here. Saying so is
             * better than accepting a timeout that would never take
             * effect. */
            app_refuse("setsockopt: not available on a TLS session");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        if (d->sock < 0) return (uint64_t)(int64_t)-VXE_NOTSOCK;

        int rc;
        switch (a1) {
        case VX_OPT_RCVTIMEO: rc = vxnet_rcv_timeout(d->sock, (uint32_t)a2); break;
        case VX_OPT_SNDTIMEO: rc = vxnet_snd_timeout(d->sock, (uint32_t)a2); break;
        case VX_OPT_NODELAY:  rc = vxnet_nodelay(d->sock, a2 ? 1 : 0); break;
        default:
            app_refuse("setsockopt: unknown option");
            return (uint64_t)(int64_t)-VXE_INVAL;
        }
        return rc == 0 ? 0 : (uint64_t)(int64_t)-VXE_INVAL;
    }

    case SYS_SHUTDOWN: {
        vfs_desc_t *d = vfs_get(vmm_current, (int64_t)a0);
        if (!d) return (uint64_t)(int64_t)-VXE_BADF;
        if (d->kind != FD_SOCK) return (uint64_t)(int64_t)-VXE_NOTSOCK;
        if (!d->connected) return (uint64_t)(int64_t)-VXE_NOTCONN;
        if (d->tls) {
            /* A TLS session ends with a close_notify record, which is
             * not a half-close and cannot be one: the record layer has
             * no way to say "I have finished writing" that leaves the
             * read direction usable. */
            app_refuse("shutdown: a TLS session closes whole");
            return (uint64_t)(int64_t)-VXE_OPNOTSUPP;
        }
        if (a1 > VX_SHUT_RDWR) return (uint64_t)(int64_t)-VXE_INVAL;
        return vxnet_shutdown(d->sock, (int)a1) == 0
                   ? 0 : (uint64_t)(int64_t)-VXE_IO;
    }

    case SYS_RESOLVE: {
        /* a0 host name, a1 four bytes to fill in. Separate from connect
         * because a program that wants to print an address, or to
         * connect to the same host twice, should not have to make a
         * connection to find out what it is. */
        addr_space_t *as = vmm_current;
        if (!as || !as->live) return (uint64_t)(int64_t)-VXE_PERM;

        char host[128];
        if (sys_copy_string(a0, host, sizeof(host)) < 0) {
            app_refuse("resolve: unreadable host name");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }
        if (!user_range_mapped(as, a1, 4, 1)) {
            app_refuse("resolve: unwritable buffer");
            return (uint64_t)(int64_t)-VXE_FAULT;
        }

        uint8_t ip[4];
        if (!vfs_host_is_loopback(host, ip)) {
            if (!vfs_parse_ipv4(host, ip)) {
                /* A name, so this is a question for a server somewhere
                 * else, and asking it is reaching the network. */
                if (!net_guard(host)) return (uint64_t)(int64_t)-VXE_PERM;
                if (!vxnet_resolve(host, ip))
                    return (uint64_t)(int64_t)-VXE_HOSTUNREACH;
            }
        }

        uint8_t *out = (uint8_t *)(uintptr_t)a1;
        for (int i = 0; i < 4; i++) out[i] = ip[i];
        return 0;
    }

    default:
        return (uint64_t)-1;
    }
}

/*
 * The break.
 *
 * malloc() in the user-space C library is built on this and nothing
 * else: ask for more address space, get the old end back, and the pages
 * appear underneath. Shrinking moves the break but does not unmap —
 * returning pages a program has just decided it does not want, only to
 * fault them back in when it changes its mind, is a poor trade at this
 * scale, and the address space goes away wholesale when the process
 * does.
 */
static uint64_t sys_sbrk(int64_t delta) {
    addr_space_t *as = vmm_current;
    if (!as || !as->live) return (uint64_t)-1;

    uint64_t old = as->brk;
    if (delta == 0) return old;

    if (delta < 0) {
        uint64_t back = (uint64_t)(-delta);
        uint64_t want = back > old - USER_HEAP_BASE ? USER_HEAP_BASE
                                                    : old - back;
        as->brk = want;
        return old;
    }

    uint64_t want = old + (uint64_t)delta;
    if (want < old || want > USER_HEAP_MAX) return (uint64_t)-1;
    if (want > as->brk_top) {
        uint64_t top  = PAGE_ALIGN_UP(want);
        uint64_t need = top - as->brk_top;
        if (vmm_alloc_range(as, as->brk_top, need,
                            PTE_USER | PTE_WRITE | PTE_NX) != need)
            return (uint64_t)-1;
        as->brk_top = top;
    }
    as->brk = want;
    return old;
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

/*
 * The staging arena.
 *
 * An image is assembled here first and only then copied into the address
 * space that will run it. Doing it in two steps rather than writing
 * straight into the process's pages buys two things worth the copy: the
 * import table can be found and patched while the whole image is
 * contiguous and writable, and a malformed header is rejected before any
 * page of the new address space has been touched.
 */
#define APP_MEM_SIZE (4 * 1024 * 1024)
#define APP_MAX_PAGES (APP_MEM_SIZE / 4096)
static uint8_t *app_memory = 0;               /* kmalloc'd on first use */

/* What each staged page will be mapped as. Written by whichever loader
 * ran, read once when the pages are handed to the process. */
static uint8_t app_page_prot[APP_MAX_PAGES];

static int app_arena_ready(void) {
    if (app_memory) return 1;
    app_memory = (uint8_t *)kmalloc_paged(APP_MEM_SIZE);
    return app_memory != 0;
}

/*
 * Two loaders share that arena. Both lay the image out at
 * app_memory + (vaddr - base_vaddr), so images have to be position
 * independent — neither loader processes relocations — and both are then
 * mapped at the address they were linked for, which makes the
 * independence a belt rather than the only thing holding it up.
 *
 * W^X is real now. The .vx format has always put .data on its own page
 * precisely so that the two segments could carry different protections;
 * until there were page tables there was nothing to carry them with. A
 * text page is mapped read and execute, a data page read, write and
 * no-execute, and nothing an application maps is ever both writable and
 * executable.
 */

/* ===== imported symbols =====
 *
 * What the kernel lends to applications. See vxfmt/vx_format.h for why
 * the table lives in the image rather than in the header.
 *
 * The names are unchanged and every image that used them still works,
 * but what they resolve to is not a kernel address any more — a ring-3
 * program calling one of those would fault on the first instruction.
 * Each name now resolves to a stub on the trampoline page mapped into
 * the calling process, which turns the call into a syscall. Filled in
 * once at boot, after the trampoline's own address is known.
 */
static vx_export_t kernel_exports[] = {
    { "ttf_draw_string", 0 },
    { "ttf_text_width",  0 },
    { "gfx_rect",        0 },
    { 0, 0 }
};

/*
 * Where the stubs land *in this process*.
 *
 * The trampoline page is placed at a different address in every address
 * space now, so the addresses patched into an image's import table
 * cannot be computed once at boot. They are recomputed per launch,
 * against that process's own base, immediately before the staged image
 * is copied into its pages.
 */
/* The Windows-facing half of the same table. Same trampoline page, same
 * per-process base; different calling convention on the way in. */
static void pe_exports_for(uint64_t tramp_base) {
    const struct { const char *n; const uint8_t *s; } map[] = {
        { "VxPrint",      petramp_print },
        { "VxExit",       petramp_exit },
        { "VxDrawPixel",  petramp_pixel },
        { "VxGetMouse",   petramp_mouse },
        { "VxYield",      petramp_yield },
        { "VxCanvas",     petramp_canvas },
        { "VxTicks",      petramp_ticks },
        { "VxTextWidth",  petramp_textw },
        { "VxDrawString", petramp_drawstr },
        { "VxFillRect",   petramp_fillrect },
        { "VxAlloc",      petramp_sbrk },
    };
    for (int i = 0; pe_exports[i].name; i++) {
        pe_exports[i].stub = 0;
        for (int k = 0; k < (int)(sizeof(map) / sizeof(map[0])); k++)
            if (str_eq(pe_exports[i].name, map[k].n))
                pe_exports[i].stub =
                    tramp_base + (uint64_t)(map[k].s - utramp_start);
    }
}

static void kernel_exports_for(uint64_t tramp_base) {
    kernel_exports[0].addr = tramp_base +
        (uint64_t)(utramp_ttf_draw_string - utramp_start);
    kernel_exports[1].addr = tramp_base +
        (uint64_t)(utramp_ttf_text_width  - utramp_start);
    kernel_exports[2].addr = tramp_base +
        (uint64_t)(utramp_gfx_rect        - utramp_start);
}

static void kernel_exports_init(void) {
    kernel_exports_for(USER_TRAMP_VA);
    pe_exports_for(USER_TRAMP_VA);
}

static int vx_name_eq(const char *a, const char *b) {
    for (int i = 0; i < VX_IMPORT_NAMELEN; i++) {
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
static int vx_resolve_imports(uint8_t *data, uint64_t len) {
    if (!data || len < sizeof(vx_import_hdr_t)) return 0;

    uint64_t limit = len - sizeof(vx_import_hdr_t);
    for (uint64_t off = 0; off <= limit; off += 8) {
        vx_import_hdr_t *hdr = (vx_import_hdr_t *)(data + off);
        if (hdr->magic != VX_IMPORT_MAGIC) continue;

        if (hdr->count == 0 || hdr->count > VX_IMPORT_MAX) return -1;

        /* The entries must lie wholly inside the image. A count that
         * overruns is the one way this can be turned into a write past
         * the image, so it is checked before anything is written. */
        uint64_t need = sizeof(vx_import_hdr_t) +
                        (uint64_t)hdr->count * sizeof(vx_import_t);
        if (need > len - off) return -1;

        vx_import_t *e = (vx_import_t *)(data + off + sizeof(vx_import_hdr_t));
        int found = 0;
        for (uint32_t i = 0; i < hdr->count; i++) {
            e[i].addr = 0;
            for (int k = 0; kernel_exports[k].name; k++) {
                if (vx_name_eq(e[i].name, kernel_exports[k].name)) {
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

/* Record what a run of staged bytes may be used for. Called once per
 * segment; the union across overlapping segments is deliberate, because
 * a page shared by two segments has to satisfy both. */
static void app_mark_prot(uint64_t off, uint64_t len, uint8_t prot) {
    uint64_t p0 = off / 4096;
    uint64_t p1 = (off + len + 4095) / 4096;
    for (uint64_t p = p0; p < p1 && p < APP_MAX_PAGES; p++)
        app_page_prot[p] |= prot;
}

/* Load an ELF64 image (used by `hello` and `run <elf>`). */
static int load_elf_image(const uint8_t *file, uint64_t fsize, int verbose,
                          uint64_t *out_entry, uint64_t *out_base,
                          uint64_t *out_span) {
    if (fsize < sizeof(Elf64_Ehdr)) {
        if (verbose) term_print_c("run: file too small for an ELF64\n", 2);
        return -1;
    }
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    if (ehdr->e_ident[4] != 2) {
        if (verbose) term_print_c("run: not a 64-bit ELF\n", 2);
        return -1;
    }
    if (!app_arena_ready()) {
        if (verbose) term_print_c("run: no memory for the load arena\n", 2);
        return -1;
    }

    /*
     * The lowest address any segment actually occupies.
     *
     * p_memsz == 0 is skipped, and that is not a refinement — it is a
     * bug fix. A program with nothing in .data or .bss still gets a
     * second PT_LOAD from the linker, empty, with a virtual address of
     * zero, because there is nothing in it to give an address to. Count
     * that and the image appears to be linked at zero: the whole thing
     * shifts up by a page, and the loader's own bounds check then
     * rejects it for living outside user space. It went unnoticed
     * because every program that existed had a .data section.
     */
    uint64_t base_vaddr = ~(uint64_t)0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_vaddr < base_vaddr) base_vaddr = ph->p_vaddr;
    }
    if (base_vaddr == ~(uint64_t)0) {
        if (verbose) term_print_c("run: no loadable segments\n", 2);
        return -1;
    }

    for (uint32_t i = 0; i < APP_MEM_SIZE; i++)
        app_memory[i] = 0;
    for (uint32_t i = 0; i < APP_MAX_PAGES; i++)
        app_page_prot[i] = 0;

    uint64_t span = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (file + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0) continue;
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

        uint8_t prot = APROT_READ;
        if (ph->p_flags & 2) prot |= APROT_WRITE;   /* PF_W */
        if (ph->p_flags & 1) prot |= APROT_EXEC;    /* PF_X */
        app_mark_prot(offset, ph->p_memsz, prot);
    }

    /* Imports are a property of the image, not of the container it arrived
     * in, and `hello` ships here as a plain ELF — so this belongs on both
     * load paths, not just the .vx one. */
    int nimp = vx_resolve_imports(app_memory, span);
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
    *out_entry = ehdr->e_entry;
    *out_base  = base_vaddr;
    *out_span  = span;
    return 0;
}

/* Load a .vx image — the format every app store package uses. */
static int load_vx_image(const uint8_t *file, uint64_t fsize, int verbose,
                          uint64_t *out_entry, uint64_t *out_base,
                          uint64_t *out_span) {
    /* Copy the header out byte-wise: the filesystem cache hands back a
     * plain byte buffer and we do not want to assume its alignment. */
    vx_header_t h;
    uint8_t *hp = (uint8_t *)&h;
    if (fsize < sizeof(h)) {
        if (verbose) term_print_c("run: file too small for a .vx header\n", 2);
        return -1;
    }
    for (uint64_t i = 0; i < sizeof(h); i++) hp[i] = file[i];

    const char *bad = vx_validate(&h, fsize);
    if (bad) {
        if (verbose) {
            term_print_c("run: ", 2);
            term_print_c(bad, 2);
            term_print("\n");
        }
        return -1;
    }
    if (vx_image_span(&h) > APP_MEM_SIZE) {
        if (verbose) term_print_c("run: image too large for the app arena\n", 2);
        return -1;
    }
    if (!app_arena_ready()) {
        if (verbose) term_print_c("run: no memory for the load arena\n", 2);
        return -1;
    }

    for (uint32_t i = 0; i < APP_MEM_SIZE; i++)
        app_memory[i] = 0;
    for (uint32_t i = 0; i < APP_MAX_PAGES; i++)
        app_page_prot[i] = 0;

    uint64_t base = h.text_vaddr;
    uint8_t *dst = app_memory + (h.text_vaddr - base);
    for (uint64_t i = 0; i < h.text_size; i++)
        dst[i] = file[h.text_off + i];
    app_mark_prot(h.text_vaddr - base, h.text_size, APROT_READ | APROT_EXEC);

    if (h.data_size) {
        dst = app_memory + (h.data_vaddr - base);
        for (uint64_t i = 0; i < h.data_size; i++)
            dst[i] = file[h.data_off + i];
    }
    /* .bss needs no work: the arena was just zeroed. But it does need a
     * mapping, so the data segment's protection covers memsz and not
     * just the bytes that came from the file. */
    if (h.data_vaddr >= base)
        app_mark_prot(h.data_vaddr - base,
                      vx_image_span(&h) - (h.data_vaddr - base),
                      APROT_READ | APROT_WRITE);

    /* Lend the image whatever kernel functions it asked for, before it can
     * run and call through them. */
    int nimp = vx_resolve_imports(app_memory, vx_image_span(&h));
    if (nimp < 0) {
        if (verbose) term_print_c("run: malformed import table\n", 2);
        return -1;
    }
    if (nimp > 0) {
        serial_puts("[vextro] .vx: resolved ");
        serial_put_dec((uint32_t)nimp);
        serial_puts(" imported symbols\n");
    }

    if (verbose) term_print_c("loading .vx: ", 3);
    *out_entry = h.entry;
    *out_base  = base;
    *out_span  = vx_image_span(&h);
    return 0;
}

/* ===== from a staged image to a running process ===== */

/*
 * Where a process's movable pieces go this time.
 *
 * Chosen once and passed to both the loader and the spawner, because the
 * two need the same answer and the loader needs it *first*: a PE's
 * imports are written into the image at link time, against the address
 * the trampoline page will actually have. Picking it afterwards linked
 * one process's imports against another's page, which faults on an
 * instruction fetch at an address that looks almost right.
 */
typedef struct {
    uint64_t stack_top;
    uint64_t tramp_va;
    uint64_t canvas_va;
    uint64_t heap_base;
} app_layout_t;

static void app_layout_pick(app_layout_t *l) {
    l->stack_top = USER_STACK_TOP - aslr_offset(256);
    l->tramp_va  = USER_TRAMP_VA  + aslr_offset(1024);
    l->canvas_va = USER_CANVAS_VA + aslr_offset(1024);
    l->heap_base = USER_HEAP_BASE + aslr_offset(4096);
}


/* Copy into an address space that is not the current one, a page at a
 * time, through the direct map. Switching CR3 to do it would be simpler
 * to read and would mean the kernel briefly ran with a half-built set of
 * page tables loaded. */
static int as_write(addr_space_t *as, uint64_t va, const void *src,
                    uint64_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len) {
        uint64_t phys = vmm_resolve(as, va);
        if (!phys) return -1;
        uint64_t chunk = 4096 - (va & 0xFFF);
        if (chunk > len) chunk = len;
        uint8_t *dst = (uint8_t *)(uintptr_t)phys_to_virt(phys);
        for (uint64_t i = 0; i < chunk; i++) dst[i] = s[i];
        va  += chunk;
        s   += chunk;
        len -= chunk;
    }
    return 0;
}

static int as_poke64(addr_space_t *as, uint64_t va, uint64_t val) {
    return as_write(as, va, &val, 8);
}

/*
 * A NUL-terminated UTF-16LE string into a user address space. Returns
 * the length in bytes, not counting the terminator -- which is what a
 * UNICODE_STRING's Length field wants.
 *
 * Every character this is asked to write is ASCII by construction: the
 * profile paths come from user_name_ok(), which admits lowercase letters
 * and digits, and from our own spelling of the tree. So the high byte is
 * always zero and there is no encoder here to get wrong.
 */
static uint32_t as_write_utf16(addr_space_t *as, uint64_t va, const char *s) {
    uint32_t n = 0;
    for (; s[n]; n++) {
        uint16_t u = (uint16_t)(unsigned char)s[n];
        if (as_write(as, va + n * 2, &u, 2) != 0) return 0;
    }
    uint16_t z = 0;
    if (as_write(as, va + n * 2, &z, 2) != 0) return 0;
    return n * 2;
}

/*
 * Fill in a UNICODE_STRING at `at`, with its text at `buf`.
 *
 * MaximumLength counts the terminator and Length does not; getting that
 * backwards is the classic way to have a runtime read one character too
 * few off the end of a path.
 */
static int as_unistr(addr_space_t *as, uint64_t at, uint64_t buf,
                     const char *s) {
    uint32_t len = as_write_utf16(as, buf, s);
    uint16_t l = (uint16_t)len, m = (uint16_t)(len + 2);
    if (as_write(as, at + UNISTR_LENGTH,     &l, 2) != 0) return -1;
    if (as_write(as, at + UNISTR_MAX_LENGTH, &m, 2) != 0) return -1;
    return as_poke64(as, at + UNISTR_BUFFER, buf);
}

/*
 * Build the address space, map what a program is entitled to see, and
 * hand it to the scheduler.
 *
 * What it is entitled to see is a short list, and the shortness is the
 * point: its own image, its own stack, its own heap once it asks, one
 * page of trampolines, and the pixels of its own window. Everything else
 * in the machine — the kernel, the framebuffer, the disk cache, every
 * other process — is either in the half of the address space its
 * descriptors forbid or is not mapped at all.
 */
/*
 * Everything a process is entitled to see, mapped into a space that has
 * already been created.
 *
 * Split out of app_spawn because exec needs precisely this and nothing
 * else around it. The two differ at both ends and agree completely in
 * the middle: a spawn creates a thread afterwards and an exec re-points
 * one, a spawn claims a window and an exec keeps the one the process
 * already had. What neither may differ in is the *mapping* — the same
 * four regions, the same W^X, the same guard page — because a second
 * copy of that would be a second place for a protection bit to be
 * wrong, and the one that was wrong would be the one nobody read.
 *
 * `keep` is the surface to reuse, or null to claim a fresh one. It is
 * the whole of exec's difference here and it matters: releasing the
 * window and claiming it again would put it back in the pool for the
 * length of one call, which is long enough for another program's launch
 * to take it.
 *
 * Returns 0 having filled in the stack pointer and the page count, or -1
 * having left the space for the caller to destroy.
 */
static int app_map_image(addr_space_t *as, const char *name,
                         uint64_t base_va, uint64_t span,
                         const app_layout_t *lay, app_surface_t *keep,
                         uint64_t *out_sp, uint64_t *out_pages) {
    const uint64_t stack_top = lay->stack_top;
    const uint64_t tramp_va  = lay->tramp_va;
    const uint64_t canvas_va = lay->canvas_va;
    as->brk = as->brk_top = lay->heap_base;

    /*
     * The token, and it is restricted whoever launched this.
     *
     * The account's administrator flag is recorded beside it rather than
     * folded into it, because the two answer different questions: the
     * flag says this session *may be asked* to elevate, the token says
     * whether it has been. A program started by an administrator gets
     * exactly the same privileges as one started by anybody else until
     * somebody answers a prompt about it. See uac_guard.
     */
    as->sid       = (uint32_t)(user_current + 1);
    as->sid_admin = (uint8_t)(user_is_admin(user_current) ? 1 : 0);
    as->token     = UAC_TOKEN_RESTRICTED;

    /* Now that this process's trampoline address is known, resolve the
     * staged image's imports against it. Doing it here rather than in
     * the loader is what lets the page move. */
    kernel_exports_for(tramp_va);
    vx_resolve_imports(app_memory, span);

    /* The image, one page at a time, each with the protection its
     * segment asked for. A page that no segment claimed is not mapped
     * at all rather than mapped and empty. */
    uint64_t pages = (span + 4095) / 4096;
    for (uint64_t p = 0; p < pages; p++) {
        uint8_t prot = app_page_prot[p];
        if (!prot) prot = APROT_READ;              /* padding inside a span */
        uint64_t flags = PTE_USER;
        if (prot & APROT_WRITE) flags |= PTE_WRITE;
        if (!(prot & APROT_EXEC)) flags |= PTE_NX;

        uint64_t phys = pmm_alloc();
        if (!phys) return -1;
        if (vmm_map(as, base_va + p * 4096, phys, flags) != 0) {
            pmm_free(phys);
            return -1;
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)phys_to_virt(phys);
        for (int i = 0; i < 4096; i++) dst[i] = app_memory[p * 4096 + i];
    }

    /* Stack: writable, never executable. */
    if (vmm_alloc_range(as, stack_top - USER_STACK_SIZE,
                        USER_STACK_SIZE,
                        PTE_USER | PTE_WRITE | PTE_NX) != USER_STACK_SIZE)
        return -1;
    /* One unmapped page below it, so an overflow is a fault at a known
     * address rather than a write into the heap. */
    {
        uint64_t *g = vmm_walk(as, stack_top - USER_STACK_SIZE - PAGE_SIZE, 1);
        if (g) *g = PTE_GUARD;
    }

    /* The trampolines: readable and executable, and writable by nobody.
     * They are the same physical page in every process. */
    if (vmm_map_shared(as, tramp_va, utramp_start,
                       (uint64_t)(utramp_end - utramp_start),
                       PTE_USER) != 0)
        return -1;

    /* The window's pixels, so a program can draw without a syscall per
     * pixel. No-execute, because a canvas full of attacker-chosen bytes
     * that the program can also jump to is the whole of the exploit.
     *
     * This process's own surface, not the shared canvas: two programs
     * running at once each get their pages, and neither can read or
     * write the other's. */
    as->surface = keep ? keep : app_surf_claim(name);
    if (as->surface) {
        if (vmm_map_shared(as, canvas_va, as->surface->px, APP_SURF_BYTES,
                           PTE_USER | PTE_WRITE | PTE_NX) != 0)
            return -1;
    } else {
        serial_puts("[compositor] no free surface; ");
        serial_puts(name);
        serial_puts(" shares the fallback canvas\n");
        if (vmm_map_shared(as, canvas_va, app_canvas, sizeof(app_canvas),
                           PTE_USER | PTE_WRITE | PTE_NX) != 0)
            return -1;
    }
    as->canvas_va = canvas_va;
    as->tramp_va  = tramp_va;

    /*
     * The return address.
     *
     * Every app in this system is a `void _start(void)` that simply
     * returns when it is done, which used to work because the kernel had
     * called it. Nothing calls it now, so the value it returns *to* has
     * to be put there by hand: the address of a stub that asks to exit.
     * Placing it at STACK_TOP-8 also leaves the stack pointer where the
     * ABI says it is at function entry — eight past a sixteen-byte
     * boundary — which matters now that applications may use SSE.
     */
    const uint64_t sp = stack_top - 8;
    if (as_poke64(as, sp, tramp_va + (uint64_t)(utramp_exit - utramp_start))
        != 0) return -1;

    *out_sp    = sp;
    *out_pages = pages;
    return 0;
}

static thread_t *app_spawn(const char *name, uint64_t base_va, uint64_t span,
                           uint64_t entry_va, const app_layout_t *lay,
                           int verbose) {
    if (!vmm_ready) {
        if (verbose) term_print_c("run: no virtual memory manager\n", 2);
        return 0;
    }
    if (!sched_running) {
        /* A thread created here would be created and never picked. Say
         * so rather than open a window that stays empty forever. */
        if (verbose)
            term_print_c("run: no scheduler on this machine "
                         "(no APIC timer)\n", 2);
        serial_puts("[exec] refused: the scheduler is not running\n");
        return 0;
    }
    if (base_va < USER_MIN || base_va + span > USER_IMAGE_MAX) {
        if (verbose) term_print_c("run: image links outside user space\n", 2);
        serial_puts("[exec] image links outside user space\n");
        return 0;
    }

    addr_space_t *as = (addr_space_t *)kmalloc(sizeof(addr_space_t));
    if (!as) {
        serial_puts("[exec] no memory for an address space\n");
        return 0;
    }
    for (uint64_t i = 0; i < sizeof(addr_space_t); i++) ((uint8_t *)as)[i] = 0;
    if (vmm_create(as) != 0) {
        serial_puts("[exec] could not create an address space\n");
        kfree(as);
        return 0;
    }

    /*
     * Where everything movable goes, this time.
     *
     * Two processes running the same program used to have byte-identical
     * layouts, so an address learned once was correct in every run and
     * in every instance. Displacing the stack, the heap and the two
     * shared mappings costs nothing and means an overflow that overwrites
     * a return address has nowhere reliable to point it.
     *
     * The image itself does not move, and cannot until a loader here
     * processes relocations: an image has to land where it was linked.
     * That is the honest limit of this.
     */
    uint64_t sp = 0, pages = 0;
    if (app_map_image(as, name, base_va, span, lay, 0, &sp, &pages) != 0)
        goto fail;

    thread_t *t = sched_spawn_user(as, entry_va, sp, name, PRIO_NORMAL);
    if (!t) goto fail;

    /*
     * Who this process is.
     *
     * The pid of the thread that created the space, fixed for the life of
     * the space and unchanged by an exec — Linux's tgid, and this
     * system's only process identity. Nothing had one until kill and
     * wait4 existed, because until then nothing could ask about a
     * process other than itself. No parent, because the desktop launched
     * this rather than a program forking it; only SYS_FORK sets ppid.
     */
    as->pid  = t->pid;
    as->ppid = 0;

    if (as->surface) {
        as->surface->pid = t->pid;
        app_surf_front   = as->surface;
    }

    serial_puts("[exec] ");
    serial_puts(name);
    serial_puts(" -> pid ");
    serial_put_dec(t->pid);
    serial_puts(", ring 3, ");
    serial_put_dec((uint32_t)pages);
    serial_puts(" image pages, ");
    serial_puts(as->surface ? "own surface" : "shared canvas");
    serial_puts(", ");
    serial_puts(as->token == UAC_TOKEN_ELEVATED ? "elevated" : "restricted");
    serial_puts(" token\n");
    return t;

fail:
    app_surf_release(as->surface);
    as->surface = 0;
    vmm_destroy(as);
    kfree(as);
    if (verbose) term_print_c("run: could not build the address space\n", 2);
    serial_puts("[exec] could not map ");
    serial_puts(name);
    serial_puts(" into its address space\n");
    return 0;
}

/*
 * A PE image, into an address space.
 *
 * Almost the same as app_spawn and deliberately not shared with it: the
 * staging buffer is different, the base is chosen rather than dictated,
 * and the page protections come from section characteristics instead of
 * program headers. What is identical is everything that matters for
 * safety -- the same four mappings, the same W^X, the same stack with a
 * guard page and an exit address on it.
 */
/*
 * The trap handler's way in.
 *
 * It has the faulting address and nothing else; everything needed to
 * answer "was this inside a __try" lives in the staged image, which is
 * this file's. So the lookup happens here and the answer goes back as
 * an absolute address to resume at.
 *
 * One limitation, stated rather than discovered: the resumption keeps
 * the faulting frame. That is correct when the fault is inside the same
 * function as the `__try` -- which is what the scope table lookup
 * requires anyway, since it is that function's table -- and it is why
 * no stack unwinding happens here. A fault deeper in a call chain finds
 * no entry and the thread dies as before.
 */
static uint32_t win_seh_handled = 0;

static int win_seh_dispatch(uint64_t rip, int vector, uint64_t *resume) {
    uint32_t target = 0;
    uint32_t code = win_exception_code(vector);
    if (!win_dispatch_exception(pe_stage, rip, code, &target)) return 0;
    *resume = win_image.base + target;
    win_seh_handled++;
    serial_puts("[seh] ");
    serial_puts(win_exception_name(code));
    serial_puts(" caught by __except\n");
    return 1;
}

static void win_seh_install(void) { trap_seh_hook = win_seh_dispatch; }

static thread_t *pe_spawn(const char *name, pe_image_t *img,
                          const app_layout_t *lay, int verbose) {
    if (!vmm_ready || !sched_running) return 0;

    addr_space_t *as = (addr_space_t *)kmalloc(sizeof(addr_space_t));
    if (!as) return 0;
    for (uint64_t i = 0; i < sizeof(addr_space_t); i++) ((uint8_t *)as)[i] = 0;
    if (vmm_create(as) != 0) { kfree(as); return 0; }

    const uint64_t stack_top = lay->stack_top;
    const uint64_t tramp_va  = lay->tramp_va;
    const uint64_t canvas_va = lay->canvas_va;
    as->brk = as->brk_top = lay->heap_base;

    /*
     * The token, and it is restricted whoever launched this.
     *
     * The account's administrator flag is recorded beside it rather than
     * folded into it, because the two answer different questions: the
     * flag says this session *may be asked* to elevate, the token says
     * whether it has been. A program started by an administrator gets
     * exactly the same privileges as one started by anybody else until
     * somebody answers a prompt about it. See uac_guard.
     */
    as->sid       = (uint32_t)(user_current + 1);
    as->sid_admin = (uint8_t)(user_is_admin(user_current) ? 1 : 0);
    as->token     = UAC_TOKEN_RESTRICTED;
    uint64_t pages = (img->image_size + 4095) / 4096;
    for (uint64_t p = 0; p < pages; p++) {
        uint8_t prot = pe_page_prot[p];
        if (!prot) prot = APROT_READ;
        uint64_t flags = PTE_USER;
        if (prot & APROT_WRITE) flags |= PTE_WRITE;
        if (!(prot & APROT_EXEC)) flags |= PTE_NX;

        uint64_t phys = pmm_alloc();
        if (!phys) goto fail;
        if (vmm_map(as, img->base + p * 4096, phys, flags) != 0) {
            pmm_free(phys);
            goto fail;
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)phys_to_virt(phys);
        for (int i = 0; i < 4096; i++) dst[i] = pe_stage[p * 4096 + i];
    }

    if (vmm_alloc_range(as, stack_top - USER_STACK_SIZE, USER_STACK_SIZE,
                        PTE_USER | PTE_WRITE | PTE_NX) != USER_STACK_SIZE)
        goto fail;
    {
        uint64_t *g = vmm_walk(as, stack_top - USER_STACK_SIZE - PAGE_SIZE, 1);
        if (g) *g = PTE_GUARD;
    }
    if (vmm_map_shared(as, tramp_va, utramp_start,
                       (uint64_t)(utramp_end - utramp_start), PTE_USER) != 0)
        goto fail;
    /* Its own surface, on the same terms as an ELF or .vx image: a
     * Windows program is a process here like any other and gets the
     * same isolation. */
    as->surface = app_surf_claim(name);
    if (as->surface) {
        if (vmm_map_shared(as, canvas_va, as->surface->px, APP_SURF_BYTES,
                           PTE_USER | PTE_WRITE | PTE_NX) != 0)
            goto fail;
    } else {
        serial_puts("[compositor] no free surface; ");
        serial_puts(name);
        serial_puts(" shares the fallback canvas\n");
        if (vmm_map_shared(as, canvas_va, app_canvas, sizeof(app_canvas),
                           PTE_USER | PTE_WRITE | PTE_NX) != 0)
            goto fail;
    }
    as->canvas_va = canvas_va;
    as->tramp_va  = tramp_va;

    /*
     * The Microsoft convention wants thirty-two bytes of shadow space
     * below the return address before the first call, so the entry
     * point is given a stack that already has it. Getting this wrong
     * corrupts the caller's frame on the callee's first spill, which is
     * not a fault -- it is wrong data, later.
     */
    uint64_t sp = (stack_top - 64) & ~15ULL;
    sp -= 8;
    if (as_poke64(as, sp, tramp_va + (uint64_t)(petramp_exit - utramp_start))
        != 0) goto fail;

    /*
     * The Thread and Process Environment Blocks.
     *
     * Two pages a Windows program expects to find already populated
     * before its entry point runs. Nothing here calls them optional: a C
     * runtime reads its stack bounds out of GS:[0x08] and GS:[0x10] the
     * first time it needs to know whether a buffer fits, and a zeroed
     * pair of those means every such check concludes the stack is
     * nowhere and behaves accordingly.
     *
     * Writable because a program sets its own last-error value, and NX
     * because it is data.
     */
    if (vmm_alloc_range(as, WIN_TEB_VA, 3 * PAGE_SIZE,
                        PTE_USER | PTE_WRITE | PTE_NX) != 3 * PAGE_SIZE)
        goto fail;

    as_poke64(as, WIN_TEB_VA + TEB_EXCEPTION_LIST, 0);
    as_poke64(as, WIN_TEB_VA + TEB_STACK_BASE,  stack_top);
    as_poke64(as, WIN_TEB_VA + TEB_STACK_LIMIT, stack_top - USER_STACK_SIZE);
    as_poke64(as, WIN_TEB_VA + TEB_SELF,        WIN_TEB_VA);
    as_poke64(as, WIN_TEB_VA + TEB_PEB,         WIN_PEB_VA);
    as_poke64(as, WIN_TEB_VA + TEB_LAST_ERROR,  0);

    as_poke64(as, WIN_PEB_VA + PEB_IMAGE_BASE,     img->base);
    as_poke64(as, WIN_PEB_VA + PEB_LDR,            0);

    /*
     * The process parameters, which used to be a literal zero.
     *
     * This is the other half of binding a session to a profile. The
     * shell's working directory was always set at login; a Windows
     * program never saw it, because the field it reads its own current
     * directory out of did not exist. Now it starts in the profile of
     * whoever launched it, with an environment block that names that
     * profile, and it inherits both the same way it would on the system
     * this is imitating.
     *
     * The directory is the Windows spelling -- `C:\Documents and
     * Settings\<name>` -- because that is the form a program will
     * concatenate a filename onto. fs_native() is what makes the result
     * resolve when it comes back through fs_*.
     */
    {
        const char *cwd = env_get("USERPROFILE");
        if (!cwd) cwd = "C:\\";

        as_poke64(as, WIN_PARAMS_VA + RUPP_MAX_LENGTH, PAGE_SIZE);
        as_poke64(as, WIN_PARAMS_VA + RUPP_LENGTH,     RUPP_SIZE);

        as_unistr(as, WIN_PARAMS_VA + RUPP_CURDIR_PATH,
                  WIN_PARAMS_VA + WIN_PARAMS_CURDIR_OFF, cwd);
        as_poke64(as, WIN_PARAMS_VA + RUPP_CURDIR_HANDLE, 0);

        as_unistr(as, WIN_PARAMS_VA + RUPP_IMAGE_PATH,
                  WIN_PARAMS_VA + WIN_PARAMS_IMAGE_OFF, name);
        as_unistr(as, WIN_PARAMS_VA + RUPP_COMMAND_LINE,
                  WIN_PARAMS_VA + WIN_PARAMS_CMDLINE_OFF, name);
        as_unistr(as, WIN_PARAMS_VA + RUPP_DLL_PATH,
                  WIN_PARAMS_VA + WIN_PARAMS_CURDIR_OFF, cwd);

        /* Built into a kernel buffer first: as_write() walks page tables
         * per call, and the block is one run of bytes that is better
         * copied once than a character at a time. */
        static uint8_t envblk[WIN_PARAMS_ENV_CAP];
        uint32_t n = env_build_utf16(envblk, sizeof(envblk));
        if (n && as_write(as, WIN_PARAMS_VA + WIN_PARAMS_ENV_OFF,
                          envblk, n) == 0)
            as_poke64(as, WIN_PARAMS_VA + RUPP_ENVIRONMENT,
                      WIN_PARAMS_VA + WIN_PARAMS_ENV_OFF);
        else
            as_poke64(as, WIN_PARAMS_VA + RUPP_ENVIRONMENT, 0);

        as_poke64(as, WIN_PEB_VA + PEB_PROCESS_PARAMS, WIN_PARAMS_VA);
    }
    /* Version 5.1 build 2600: what this system is aiming at, and what a
     * program checking for "at least XP" wants to see. */
    as_poke64(as, WIN_PEB_VA + PEB_OS_MAJOR, 5);
    as_poke64(as, WIN_PEB_VA + PEB_OS_MINOR, 1);
    as_poke64(as, WIN_PEB_VA + PEB_OS_BUILD, 2600);

    thread_t *t = sched_spawn_user(as, img->entry, sp, name, PRIO_NORMAL);
    if (!t) goto fail;

    /* A Windows process has a process identity for the same reason an
     * ELF one does, and gets it the same way. It is what a forked child
     * of it would record as its parent. */
    as->pid  = t->pid;
    as->ppid = 0;

    if (as->surface) {
        as->surface->pid = t->pid;
        app_surf_front   = as->surface;
    }

    /* One write, for every Windows process there will ever be: the TEB
     * is at the same virtual address in each of them, and GS_BASE is a
     * linear address resolved through the current CR3. See the note in
     * sched.h about why this is not in the context switch. */
    sched_set_gs_base(WIN_TEB_VA);
    as_poke64(as, WIN_TEB_VA + TEB_PROCESS_ID, t->pid);
    as_poke64(as, WIN_TEB_VA + TEB_THREAD_ID,  t->pid);
    return t;

fail:
    app_surf_release(as->surface);
    as->surface = 0;
    vmm_destroy(as);
    kfree(as);
    if (verbose) term_print_c("run: could not map the PE image\n", 2);
    serial_puts("[exec] could not map the PE image\n");
    return 0;
}

/*
 * What to say when a program ends.
 *
 * It used to need saying because a program that had ended was a program
 * whose CALL had returned, and the machine was visibly frozen until it
 * did. Now that they run alongside the interface there is nothing to
 * notice, so the Action Center is told.
 */
static void app_reaped(thread_t *t) {
    /*
     * The surface goes back to the pool first, and unconditionally —
     * before the early return below, because a silent launch is still a
     * process that held one and a pool that leaked six slots would leave
     * every later program on the shared canvas with no way to tell why.
     *
     * sched_reap calls this hook before it destroys the address space,
     * which is the only window in which t->as is still readable. The
     * refcount is what makes a forked pair safe: the surface is only
     * returned when the second of them ends.
     */
    /*
     * ---- but only for the last thread in it ----
     *
     * The surface follows the address space, not the thread: several
     * threads of one program draw into one window, because they are one
     * program. Releasing on the first exit would put the window back in
     * the pool while the siblings were still painting it, and the next
     * program to launch would inherit a canvas somebody else was writing.
     *
     * `refs` is read here and decremented by sched_reap immediately
     * after, so one means "this is the last". Both run on the compositor
     * thread, in that order, with nothing in between.
     */
    if (t->as && t->as->surface && t->as->refs <= 1) {
        app_surf_release(t->as->surface);
        t->as->surface = 0;
    }

    /*
     * And the open files, on the same "last thread out" condition and
     * for the same reason: several threads of one program share one
     * table, because they are one program.
     *
     * Detached and handed on rather than released here. This runs on the
     * compositor's thread, inside the frame's critical section, and
     * closing a TLS session or writing a dirty file back would block it
     * — see the note on the janitor at the head of src/vfs.h. Reap stays
     * a pointer swap.
     */
    if (t->as && t->as->files && t->as->refs <= 1) {
        struct proc_files *f = t->as->files;
        t->as->files = 0;
        vfs_retire_table(f);
    }

    /*
     * ---- and telling this process's parent that it has ended ----
     *
     * Here rather than in sched_exit, and the window is narrow enough to
     * be worth naming: sched_reap calls this hook and destroys the
     * address space immediately afterwards, so this is the last moment
     * at which the dying process's identity, its parent's number and the
     * signal that killed it are all still readable. A moment later the
     * status the parent is waiting for would be memory that had been
     * freed.
     *
     * vls_child_exited records the status against the *parent's* signal
     * state, posts a SIGCHLD, and wakes anything parked in wait4. It
     * does nothing at all if the parent has already ended, which is what
     * an orphan is.
     */
    if (t->as && t->as->refs <= 1) {
        const int killed = t->as->sig ? (int)t->as->sig->killed_by : 0;
        vls_report_exit(t->as, killed ? killed : t->exit_code, killed);
        /* The handlers go with the process. Freed after the line above,
         * because that line reads them. */
        vls_sig_free(t->as);
    }

    /* A worker thread ending is not an event anybody wants told about;
     * the program it belongs to is still running and will announce
     * itself when it finishes. Only the last thread out reports. */
    if (t->as && t->as->refs > 1) return;

    if (silent_launch) return;
    char note[NOTIFY_TEXT];
    str_copy(note, t->name, sizeof(note));
    str_append(note, t->exit_code ? " stopped with an error" : " finished",
               sizeof(note));
    notify_push(t->exit_code ? NOTE_WARN : NOTE_GOOD, note);
}

/*
 * The name a policy decision is about: the last path component, without
 * its extension, which is what an administrator types and what the store
 * calls a package.
 */
static void policy_short_name(const char *path, char *out, int cap) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    const char *p = path + last + 1;
    int n = 0;
    while (p[n] && n < cap - 1 && p[n] != '.') { out[n] = p[n]; n++; }
    out[n] = '\0';
}

/*
 * Every program in the system starts here, which is the only reason
 * policy can be enforced at all: one door, checked once.
 *
 * Refusals are announced on both channels and filed with the Action
 * Center. A program that simply does not start, with no reason given,
 * is indistinguishable from a broken one.
 */
static int execute_bin_full(const char *filepath, int verbose,
                            int wait_for_exit) {
    uint64_t fsize = 0;
    const void *fdata = fs_read_file(filepath, &fsize);
    if (!fdata || fsize < 8) {
        if (verbose) {
            term_print_c("run: file not found: ", 2);
            term_print_c(filepath, 2);
            term_print("\n");
        }
        /* On serial too, and regardless of verbosity. A launch that fails
         * silently is indistinguishable from one that never happened,
         * which is exactly what a headless test cannot tell apart. */
        serial_puts("[exec] cannot read ");
        serial_puts(filepath);
        serial_puts("\n");
        return -1;
    }

    char shortname[ALLOW_NAME];
    policy_short_name(filepath, shortname, sizeof(shortname));

    if (!allow_permits(shortname)) {
        char note[NOTIFY_TEXT];
        str_copy(note, "Blocked by the allow list: ", sizeof(note));
        str_append(note, shortname, sizeof(note));
        notify_push(NOTE_WARN, note);
        serial_puts("[policy] blocked (not on the allow list): ");
        serial_puts(shortname);
        serial_putc('\n');
        if (verbose) {
            term_print_c("run: blocked - ", 2);
            term_print_c(shortname, 2);
            term_print_c(" is not on this account's allow list\n", 2);
        }
        return -1;
    }

    if (scanner_on) {
        const int verdict = scan_buffer((const uint8_t *)fdata,
                                        (uint32_t)fsize);
        if (verdict != SCAN_CLEAN) {
            char note[NOTIFY_TEXT];
            str_copy(note, verdict == SCAN_SIGNATURE
                         ? "Threat blocked: " : "Refused a malformed program: ",
                     sizeof(note));
            str_append(note, shortname, sizeof(note));
            notify_push(NOTE_WARN, note);
            serial_puts("[scan] refused ");
            serial_puts(shortname);
            serial_puts(": ");
            serial_puts(scan_detail);
            serial_putc('\n');
            if (verbose) {
                term_print_c("run: refused - ", 2);
                term_print_c(scan_detail, 2);
                term_print("\n");
            }
            return -1;
        }
    }

    const uint8_t *file = (const uint8_t *)fdata;
    uint64_t entry_va = 0, base_va = 0, span = 0;
    int rc;

    /*
     * A Windows executable takes a different road entirely: it is
     * relocated rather than fixed, its imports are written into a table
     * rather than patched into code, and its sections carry their own
     * protections. pe_load does all three into its own staging buffer,
     * so the rest of this function only has to hand over pages.
     */
    if (file[0] == 'M' && file[1] == 'Z') {
        app_layout_t lay;
        app_layout_pick(&lay);
        /* Before the image is linked, so its import table is written
         * against the page this process will actually have. */
        kernel_exports_for(lay.tramp_va);
        pe_exports_for(lay.tramp_va);

        uint64_t pe_base = 0x0000000000400000ULL + aslr_offset(2048);
        /*
         * Zeroed before the loader sees it, and not as a formality: every
         * early return in pe_load is a `return -1` taken before a single
         * field of this structure is written, and the four lines below
         * that print `img.sections`, `img.relocations`,
         * `img.imports_resolved` and `img.pdata_size` are reached only
         * when it returned zero. That is correct today and it is correct
         * because of a property of a function in another file. A report
         * built out of an uninitialised stack frame is the kind of wrong
         * that reads as a plausible number.
         */
        pe_image_t img;
        for (uint64_t i = 0; i < sizeof(img); i++) ((uint8_t *)&img)[i] = 0;
        win_image_forget();
        int prc = pe_load(file, fsize, pe_base, &img);
        if (prc != 0) {
            serial_puts("[exec] ");
            serial_puts(pe_error(prc));
            if (img.missing[0]) {
                serial_puts(": wanted ");
                serial_puts(img.missing);
            }
            serial_puts("\n");
            if (verbose) {
                term_print_c("run: ", 2);
                term_print_c(pe_error(prc), 2);
                if (img.missing[0]) {
                    term_print_c(" - wanted ", 2);
                    term_print_c(img.missing, 2);
                }
                term_print("\n");
            }
            return -1;
        }
        for (int i = 0; i < APP_CANVAS_W * APP_CANVAS_H; i++) app_canvas[i] = 0;
        {
            int ti = 0;
            const char *pp = filepath;
            while (*pp && ti < 60) app_win_title[ti++] = *pp++;
            app_win_title[ti] = '\0';
        }
        wm_open(WK_HELLO);

        serial_puts("[exec] PE64 ");
        serial_puts(shortname);
        serial_puts(": ");
        serial_put_dec((uint32_t)img.sections);
        serial_puts(" sections, ");
        serial_put_dec((uint32_t)img.relocations);
        serial_puts(" relocations, ");
        serial_put_dec((uint32_t)img.imports_resolved);
        serial_puts(" imports");
        if (img.pdata_size) {
            serial_puts(", ");
            serial_put_dec(img.pdata_size / 12);
            serial_puts(" unwind entries");
        }
        serial_puts("\n");

        /* Where its exception and resource tables are, so the trap
         * handler can find them without re-reading a header. */
        win_image_record(&img);

        {
            char sres[64];
            /* Resource string 1, if it has one. Read here rather than on
             * demand because .rsrc is in the staged image, and the stage
             * is reused by the next program to run. */
            if (win_load_string(pe_stage, 1, sres, sizeof sres) > 0) {
                serial_puts("[exec]   string 1: \"");
                serial_puts(sres);
                serial_puts("\"\n");
            }
        }

        thread_t *pt = pe_spawn(shortname, &img, &lay, verbose);
        if (!pt) return -1;
        if (wait_for_exit) sched_join(pt, 30000);
        return 0;
    }

    if (file[0] == (uint8_t)VX_MAGIC0 && file[1] == (uint8_t)VX_MAGIC1 &&
        file[2] == (uint8_t)VX_MAGIC2 && file[3] == (uint8_t)VX_MAGIC3) {
        rc = load_vx_image(file, fsize, verbose, &entry_va, &base_va, &span);
    } else if (file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' &&
               file[3] == 'F') {
        rc = load_elf_image(file, fsize, verbose, &entry_va, &base_va, &span);
    } else {
        if (verbose)
            term_print_c("run: not a .vx or ELF64 executable\n", 2);
        serial_puts("[exec] not an executable: ");
        serial_puts(filepath);
        serial_puts("\n");
        return -1;
    }
    if (rc != 0) {
        serial_puts("[exec] could not load ");
        serial_puts(filepath);
        serial_puts("\n");
        return -1;
    }

    if (verbose) {
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

    /* The window opens before the program does. It used to be the other
     * way round because there was nothing to look at until the program
     * had finished — it ran to completion inside this call, with the
     * whole machine stopped. Now it is a thread, and an empty window
     * filling in as the work happens is the honest picture of that. */
    wm_open(WK_HELLO);

    app_layout_t lay;
    app_layout_pick(&lay);
    thread_t *t = app_spawn(shortname, base_va, span, entry_va, &lay, verbose);
    if (!t) return -1;

    if (wait_for_exit) {
        int code = sched_join(t, 30000);
        if (code < 0 && verbose)
            term_print_c("run: the program did not finish in time\n", 2);
    }
    return 0;
}

/* Launch and return; the program runs as a thread from here on. */
/* ============================================================
 *  exec: the same process, a different program
 * ============================================================
 *
 * The one operation a process can perform on itself that changes
 * everything about it except which process it is, and the only reason
 * this system has never had it is that nothing needed it: a program was
 * started by the desktop and ran until it stopped. A browser is the
 * program that needs it — WebKit's UI process launches its web process
 * and its network process by forking and executing, and there was no
 * second half to that pair here.
 *
 * ---- what is kept, and why each ----
 *
 *   The process identity. That is the definition of exec: `pid` and
 *   `ppid` survive, so a parent that forked and is waiting still
 *   recognises what comes back.
 *
 *   The window. Released and re-claimed would put it back in the pool
 *   for the length of one call, which is long enough for another
 *   launch to take it — and the program would come back to find its
 *   window belonged to somebody else.
 *
 *   The descriptors, minus the ones marked close-on-exec. That flag has
 *   been recorded on every descriptor since open() existed here and has
 *   never been *read* by anything, because nothing could exec. This is
 *   what it was for.
 *
 *   The blocked signal mask, and not the handlers. vls_sig_reset_for_exec
 *   explains the asymmetry: a handler is an address in an image that no
 *   longer exists, an ignore is a decision about the process.
 *
 * ---- and the two rules that make a failure safe ----
 *
 * Everything is built before anything is torn down, and the old address
 * space is destroyed last. A failed exec must leave the process exactly
 * as it was and return -1, which is only possible if the space it was
 * running in is still there to go back to. Every early return below
 * happens before the first irreversible step.
 *
 * And a multi-threaded process is refused rather than having its
 * siblings killed. Linux kills them; doing that here would leave zombie
 * threads still pointing at an address space this call had already
 * destroyed, and the reaper decrementing a reference count on freed
 * memory is a fault with no stack left to blame. The pattern that
 * matters — fork, then exec in the child — is single-threaded in the
 * child by construction, so this costs nothing that is actually done.
 */
#define EXEC_ARG_MAX    32
#define EXEC_ARG_BYTES  2048

typedef struct {
    int      n;
    uint32_t off[EXEC_ARG_MAX];       /* into `blob` */
    uint32_t len;
    char     blob[EXEC_ARG_BYTES];
} exec_vec_t;

/*
 * A NULL-terminated array of user string pointers, taken whole into
 * kernel memory.
 *
 * Before anything is torn down, because after the address space is gone
 * so are the strings — and a program's own argv lives in the memory the
 * exec is about to unmap. This is the piece that is easy to leave until
 * the new stack is ready and impossible to do there.
 */
static int exec_take_vector(uint64_t uptr, exec_vec_t *v) {
    v->n = 0;
    v->len = 0;
    v->blob[0] = '\0';
    if (!uptr) return 0;

    for (int i = 0; i < EXEC_ARG_MAX; i++) {
        uint64_t sp = 0;
        if (!user_range_mapped(vmm_current, uptr + (uint64_t)i * 8, 8, 0))
            return -1;
        sp = *(const uint64_t *)(uintptr_t)(uptr + (uint64_t)i * 8);
        if (!sp) { v->n = i; return 0; }

        char tmp[256];
        const int got = sys_copy_string(sp, tmp, sizeof(tmp));
        if (got < 0) return -1;
        const uint32_t need = (uint32_t)got + 1;
        if (v->len + need > EXEC_ARG_BYTES) return -1;

        v->off[i] = v->len;
        for (uint32_t k = 0; k < need; k++) v->blob[v->len + k] = tmp[k];
        v->len += need;
        v->n = i + 1;
    }
    /* More than thirty-two of them. Refused rather than truncated: a
     * program handed half its arguments does the wrong thing quietly,
     * where one told the exec failed does not do it at all. */
    return -1;
}

/* Lay a vector out on the new stack, growing down, and answer where the
 * pointer array ended up. The strings are already there — placed by the
 * caller in one block — so this writes only the array of pointers into
 * it, NULL-terminated as the ABI requires. */
static int exec_place_vector(addr_space_t *as, const exec_vec_t *v,
                             uint64_t strings_va, uint64_t *p,
                             uint64_t *out_va) {
    *p -= (uint64_t)(v->n + 1) * 8;
    for (int i = 0; i < v->n; i++)
        if (as_poke64(as, *p + (uint64_t)i * 8,
                      strings_va + v->off[i]) != 0) return -1;
    if (as_poke64(as, *p + (uint64_t)v->n * 8, 0) != 0) return -1;
    *out_va = *p;
    return 0;
}

static int64_t sys_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    addr_space_t *old = vmm_current;
    if (!old || !old->live || !cur_thread || !cur_thread->user)
        return -VXE_PERM;
    /* The frame is what gets rewritten to land in the new image, and
     * only the SYSCALL door carries a return address this code can
     * reach — `int 0x80` leaves it in the processor's own frame, above
     * what syscall_frame_t covers. */
    if (!cur_thread->sframe || !cur_thread->sfast) return -VXE_PERM;

    if (old->refs > 1) {
        app_refuse("execve: this process has more than one thread; "
                   "exec from a single-threaded child instead");
        return -VXE_AGAIN;
    }

    char path[FS_PATH_MAX];
    if (sys_copy_string(upath, path, sizeof(path)) < 0) {
        app_refuse("execve: unreadable path");
        return -VXE_FAULT;
    }

    static exec_vec_t argv, envp;    /* static: 4 KB is too much stack */
    if (exec_take_vector(uargv, &argv) != 0) {
        app_refuse("execve: the argument vector is unreadable or too large");
        return -VXE_FAULT;
    }
    if (exec_take_vector(uenvp, &envp) != 0) {
        app_refuse("execve: the environment is unreadable or too large");
        return -VXE_FAULT;
    }

    char abs[FS_PATH_MAX];
    fs_abs(path, abs, sizeof(abs));
    if (!prof_may(abs, 0)) {
        app_refuse("execve: outside this account's profile");
        return -VXE_ACCES;
    }

    uint64_t fsize = 0;
    const void *fdata = fs_read_file(abs, &fsize);
    if (!fdata || fsize < 8) return -VXE_NOENT;

    /*
     * The same two gates a launch from the desktop passes, and they are
     * here rather than skipped because exec is otherwise a way around
     * both: a program that could exec anything could run what the allow
     * list forbids and what the scanner would have refused, by asking
     * for it from inside a process that was already permitted.
     */
    char shortname[ALLOW_NAME];
    policy_short_name(abs, shortname, sizeof(shortname));
    if (!allow_permits(shortname)) {
        serial_puts("[policy] execve blocked (not on the allow list): ");
        serial_puts(shortname);
        serial_putc('\n');
        return -VXE_ACCES;
    }
    if (scanner_on) {
        const int verdict = scan_buffer((const uint8_t *)fdata,
                                        (uint32_t)fsize);
        if (verdict != SCAN_CLEAN) {
            serial_puts("[scan] execve refused ");
            serial_puts(shortname);
            serial_puts(": ");
            serial_puts(scan_detail);
            serial_putc('\n');
            return -VXE_ACCES;
        }
    }

    const uint8_t *file = (const uint8_t *)fdata;
    uint64_t entry_va = 0, base_va = 0, span = 0;
    int rc;
    if (file[0] == (uint8_t)VX_MAGIC0 && file[1] == (uint8_t)VX_MAGIC1 &&
        file[2] == (uint8_t)VX_MAGIC2 && file[3] == (uint8_t)VX_MAGIC3) {
        rc = load_vx_image(file, fsize, 0, &entry_va, &base_va, &span);
    } else if (file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' &&
               file[3] == 'F') {
        rc = load_elf_image(file, fsize, 0, &entry_va, &base_va, &span);
    } else {
        /* ENOEXEC, which is the answer a shell turns into "try it as a
         * script" and this system turns into a refusal it can print. */
        return -8;
    }
    if (rc != 0) return -VXE_IO;
    if (base_va < USER_MIN || base_va + span > USER_IMAGE_MAX)
        return -VXE_NOMEM;

    /* ---- from here the new space is built; nothing is torn down yet ---- */

    addr_space_t *nas = (addr_space_t *)kmalloc(sizeof(addr_space_t));
    if (!nas) return -VXE_NOMEM;
    for (uint64_t i = 0; i < sizeof(addr_space_t); i++) ((uint8_t *)nas)[i] = 0;
    if (vmm_create(nas) != 0) { kfree(nas); return -VXE_NOMEM; }
    nas->refs = 1;

    app_layout_t lay;
    app_layout_pick(&lay);

    uint64_t sp = 0, pages = 0;
    if (app_map_image(nas, shortname, base_va, span, &lay,
                      old->surface, &sp, &pages) != 0) {
        /* The surface was the caller's and stays the caller's: it was
         * passed in rather than claimed, so it is not this path's to
         * release. Clearing the pointer first is what stops vmm_destroy
         * from being handed a space that still claims one. */
        nas->surface = 0;
        vmm_destroy(nas);
        kfree(nas);
        return -VXE_NOMEM;
    }

    /*
     * ---- the arguments, on the new stack ----
     *
     * Not the System V startup block, and the difference is deliberate.
     * That convention puts argc at the stack pointer and expects _start
     * to read it from there — but every program on this system is a
     * `void _start(void)` that *returns*, with the address it returns to
     * placed on the stack by the loader, and a block where that address
     * has to be would break all of them.
     *
     * So the strings and the two pointer arrays go on the stack, and
     * their addresses go in RDI, RSI and RDX: the ordinary C calling
     * convention. A program written as `_start(void)` ignores three
     * registers it was going to ignore anyway; one written as
     * `_start(int argc, char **argv, char **envp)` gets its arguments
     * without a word of assembly. Both work, which is the property that
     * matters when the images being executed are the ones already on the
     * volume.
     */
    uint64_t p = lay.stack_top;
    uint64_t strings_va = 0, argv_va = 0, envp_va = 0;

    const uint32_t sbytes = argv.len + envp.len;
    p -= sbytes ? sbytes : 8u;
    p &= ~15ull;
    strings_va = p;
    if (argv.len && as_write(nas, p, argv.blob, argv.len) != 0) goto exec_fail;
    if (envp.len && as_write(nas, p + argv.len, envp.blob, envp.len) != 0)
        goto exec_fail;
    const uint64_t envp_strings_va = strings_va + argv.len;

    p = strings_va;
    if (exec_place_vector(nas, &argv, strings_va, &p, &argv_va) != 0)
        goto exec_fail;
    if (exec_place_vector(nas, &envp, envp_strings_va, &p, &envp_va) != 0)
        goto exec_fail;

    /* Sixteen-aligned, then one word for the address _start returns to —
     * which leaves RSP eight past a boundary, exactly where the ABI says
     * it is when a function is entered. */
    p &= ~15ull;
    p -= 8;
    if (as_poke64(nas, p, lay.tramp_va +
                          (uint64_t)(utramp_exit - utramp_start)) != 0)
        goto exec_fail;
    sp = p;

    /* ---- the point of no return ---- */

    nas->pid       = old->pid;
    nas->ppid      = old->ppid;
    nas->net_ok    = NET_UNASKED;   /* a new program, so a new question */
    /*
     * Back to the native numbering, and this is a decision rather than
     * an oversight. The image that has just been loaded is one of this
     * system's own — a .vx or an ELF built by this tree — because that
     * is all the loader above can read. Its calls are written in native
     * numbers. Carrying a Linux personality across into it would make
     * its first `write` mean `close`. A day when this loader can read a
     * foreign ELF is the day this line reads the image and decides.
     */
    nas->personality = VLS_PERSONALITY_NATIVE;

    /* The descriptors move rather than being duplicated, and the ones
     * marked close-on-exec are closed on the way. That flag has been
     * recorded at every open since descriptors existed here and this is
     * the first code that has ever read it. */
    nas->files = old->files;
    old->files = 0;
    int closed = 0;
    if (nas->files) {
        for (int i = 0; i < FD_MAX; i++) {
            vfs_desc_t *d = &nas->files->d[i];
            if (d->kind == FD_FREE || d->kind == FD_CONSOLE) continue;
            if (!d->cloexec) continue;
            vfs_close_desc(d);
            closed++;
        }
    }

    /* The signal state moves too, and is reset for the new image. The
     * blocked mask is what survives; see vls_sig_reset_for_exec. */
    nas->sig = old->sig;
    old->sig = 0;
    vls_sig_reset_for_exec(nas);

    /* The window is the process's, and the process is the same one. */
    old->surface = 0;

    sched_adopt_space(cur_thread, nas);

    /*
     * The frame the syscall will return along, rewritten to land in the
     * new image. SYSRETQ takes RIP from RCX and RFLAGS from R11, which
     * is why those two carry the entry point and the flags rather than
     * fields named for them.
     *
     * Every other register is cleared. Not tidiness: they hold the old
     * program's pointers, and a new image that read one would be reading
     * an address in a space that no longer exists.
     */
    syscall_frame_t *f = cur_thread->sframe;
    f->rcx      = entry_va;
    f->r11      = 0x202;
    f->user_rsp = sp;
    f->rdi      = (uint64_t)argv.n;
    f->rsi      = argv_va;
    f->rdx      = envp_va;
    f->rbx = f->rbp = 0;
    f->r8 = f->r9 = f->r10 = f->r12 = f->r13 = f->r14 = f->r15 = 0;

    /* Not before now. The space the caller was running in is only safe
     * to destroy once nothing points at it, and CR3 held it until
     * sched_adopt_space three lines ago. */
    vmm_destroy(old);
    kfree(old);

    str_copy(cur_thread->name, shortname, SCHED_NAME_LEN);
    if (nas->surface)
        str_copy(nas->surface->name, shortname, SCHED_NAME_LEN);

    serial_puts("[exec] execve ");
    serial_puts(shortname);
    serial_puts(" in pid ");
    serial_put_dec(nas->pid);
    serial_puts(", ");
    serial_put_dec((uint32_t)pages);
    serial_puts(" image pages, ");
    serial_put_dec((uint32_t)argv.n);
    serial_puts(" args, ");
    serial_put_dec((uint32_t)closed);
    serial_puts(" descriptors closed on exec\n");
    return 0;

exec_fail:
    /* Nothing irreversible has happened: the caller is still running in
     * `old`, still holds its own descriptors, and still owns the
     * surface — which is why it is cleared here rather than released. */
    nas->surface = 0;
    vmm_destroy(nas);
    kfree(nas);
    return -VXE_NOMEM;
}

/* ============================================================
 *  the router's side of the seam
 * ============================================================
 *
 * Ten small functions and one assignment. src/sched/vls_core.c is a
 * separate object and reaches everything here through the table below,
 * for the reason the Makefile gives beside KERN_MODULES: a module that
 * included this file would compile, link, and be given a private copy of
 * the compositor's state. It is the same shape as sched_reap_hook, and
 * the dependency points the same way — from the composition root into
 * the module, never back.
 */
static uint64_t vlsh_native(uint64_t num, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    return syscall_native(num, a0, a1, a2, a3, a4, a5);
}

static int vlsh_range_ok(uint64_t va, uint64_t len) {
    return user_range_ok(va, len);
}

static int vlsh_range_mapped(uint64_t va, uint64_t len, int write) {
    if (!vmm_current) return 0;
    return user_range_mapped(vmm_current, va, len, write);
}

static int vlsh_copy_in(uint64_t uptr, void *dst, uint64_t len) {
    if (!vlsh_range_mapped(uptr, len, 0)) return -1;
    const uint8_t *s = (const uint8_t *)(uintptr_t)uptr;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

static int vlsh_copy_out(uint64_t uptr, const void *src, uint64_t len) {
    if (!vlsh_range_mapped(uptr, len, 1)) return -1;
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)(uintptr_t)uptr;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

static int vlsh_copy_string(uint64_t uptr, char *dst, int cap) {
    return sys_copy_string(uptr, dst, cap);
}

static int64_t vlsh_execve(uint64_t path, uint64_t argv, uint64_t envp) {
    return sys_execve(path, argv, envp);
}

/* Where the signal trampoline is in *this* process. One physical page in
 * every address space, at an address the layout displaces per process —
 * so the offset is fixed and the base is not. */
static uint64_t vlsh_sigreturn_va(void) {
    if (!vmm_current || !vmm_current->tramp_va) return 0;
    return vmm_current->tramp_va +
           (uint64_t)(utramp_sigreturn - utramp_start);
}

static void vlsh_refuse(const char *what) { app_refuse(what); }

static uint32_t vlsh_pid(void)  { return vmm_current ? vmm_current->pid  : 0; }
static uint32_t vlsh_ppid(void) { return vmm_current ? vmm_current->ppid : 0; }
/*
 * Never zero, and that is the whole of this function.
 *
 * `sid` is the account identifier and is user_current + 1, so a machine
 * with nobody signed in — a boot self-test, or a machine sitting at its
 * login screen — has user_current at -1 and a sid of zero. Handing that
 * back as a uid says "root" to every ported library that asks, and a
 * great deal of ported code reads uid zero as permission to skip a
 * check, write to a system directory, or decline to drop privilege.
 * None of that is true here: every process holds
 * UAC_TOKEN_RESTRICTED whoever started it.
 *
 * So a process with no account behind it is nobody, which is the number
 * Unix has used for exactly this for forty years, and is a truthful
 * answer rather than a safe-looking one.
 */
#define VLS_UID_NOBODY 65534u

static uint32_t vlsh_uid(void) {
    if (!vmm_current || !vmm_current->sid) return VLS_UID_NOBODY;
    return vmm_current->sid;
}

static void vls_install(void) {
    vls_host.native        = vlsh_native;
    vls_host.range_ok      = vlsh_range_ok;
    vls_host.range_mapped  = vlsh_range_mapped;
    vls_host.copy_in       = vlsh_copy_in;
    vls_host.copy_out      = vlsh_copy_out;
    vls_host.copy_string   = vlsh_copy_string;
    vls_host.execve        = vlsh_execve;
    vls_host.sigreturn_va  = vlsh_sigreturn_va;
    vls_host.refuse        = vlsh_refuse;
    vls_host.pid           = vlsh_pid;
    vls_host.ppid          = vlsh_ppid;
    vls_host.uid           = vlsh_uid;
    vls_init();
}

/*
 * ---- a signal, on the way out of a system call ----
 *
 * One of exactly two points where a caught signal can be delivered, and
 * the reason there are two rather than one is that they are the only two
 * places a thread returns to ring 3 with a complete register set in a
 * frame that can be rewritten. The other is the fault path in
 * src/trap.h.
 *
 * The timer interrupt is deliberately not a third. src/sched/scheduler.c
 * says sched_on_tick is hand-tuned and compiled general-regs-only,
 * moving extended state through registers the compiler has been told not
 * to touch; a signal check in it would be new instructions in the most
 * delicate function in this kernel, and the last thing added to it
 * produced a #GP two instructions later.
 *
 * What that costs, stated rather than left to be discovered: a thread
 * asleep in the kernel sees a caught signal when it next wakes rather
 * than at the moment it is sent, and no system call returns EINTR. A
 * signal that *kills* does not wait — vls_signal_post ends the target
 * immediately — so the case this delays is the one where the program
 * asked to be told and is not in a hurry.
 */
static uint64_t syscall_deliver_signals(uint64_t ret) {
    addr_space_t *as = vmm_current;
    if (!as || !as->sig || !as->sig->pending) return ret;
    if (!cur_thread || !cur_thread->user) return ret;

    /*
     * The thread's own frame, not the global.
     *
     * This runs after wait4 may have parked for fifty milliseconds, and
     * during that park the child being waited for entered the kernel and
     * wrote syscall_cur_frame. Reading the global here delivered the
     * signal into the child's saved registers. See the note in
     * syscall_dispatch_frame.
     */
    syscall_frame_t *f = cur_thread->sframe;
    /* The legacy gate keeps the return address in the processor's own
     * interrupt frame, above what syscall_frame_t covers, so there is no
     * RIP here to redirect. Every program that can install a handler
     * reaches this kernel through SYSCALL. */
    if (!f || !cur_thread->sfast) return ret;

    vls_regs_t r;
    for (uint64_t i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    /* The first fifteen words of the two structures are the same
     * registers in the same order, which include/vls.h arranges on
     * purpose so that this is a copy rather than fifteen assignments
     * that could be mis-ordered. */
    for (int i = 0; i < 15; i++)
        ((uint64_t *)&r)[i] = ((const uint64_t *)f)[i];
    r.rax    = ret;                 /* what the call was about to answer */
    r.rip    = f->rcx;              /* SYSCALL left the return address   */
    r.rsp    = f->user_rsp;
    r.rflags = f->r11;

    const int what = vls_signal_dispatch(as, &r);
    if (what == VLS_DELIVER_HANDLER) {
        for (int i = 0; i < 15; i++)
            ((uint64_t *)f)[i] = ((const uint64_t *)&r)[i];
        f->rcx      = r.rip;
        f->user_rsp = r.rsp;
        f->r11      = r.rflags;
        return r.rax;
    }
    if (what == VLS_DELIVER_FATAL) {
        const int sig = as->sig ? (int)as->sig->killed_by : 0;
        serial_puts("[VLS] pid ");
        serial_put_dec(as->pid);
        serial_puts(" ended by signal ");
        serial_put_dec((uint32_t)sig);
        serial_puts("\n");
        vls_report_exit(as, sig, 1);
        /* Every thread of the process, the same way SYS_EXIT_GROUP does
         * it and with the same argument for why marking is enough. */
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            thread_t *t = threads[i];
            if (!t || t == cur_thread) continue;
            if (t->as != as || t->state == T_FREE) continue;
            t->exit_code = 128 + sig;
            t->state     = T_ZOMBIE;
        }
        sched_exit(128 + sig);
    }
    return ret;
}

/*
 * ---- the one door, and which numbering is behind it ----
 *
 * Everything a ring-3 program asks for arrives here, from either entry
 * stub, and leaves by exactly one of three paths. include/vls.h explains
 * why there are two ways to be a Linux call and why both are needed; the
 * short of it is that the bias works without state and the personality
 * works without cooperation, and the signal trampoline on the shared
 * page needs the first while an actual Linux binary needs the second.
 */
static uint64_t syscall_service(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
    uint64_t ret;
    if (num >= VLS_CALL_BIAS) {
        ret = vls_syscall(num - VLS_CALL_BIAS, a0, a1, a2, a3, a4, a5);
    } else if (vmm_current &&
               vmm_current->personality == VLS_PERSONALITY_LINUX) {
        ret = vls_syscall(num, a0, a1, a2, a3, a4, a5);
    } else {
        ret = syscall_native(num, a0, a1, a2, a3, a4, a5);
    }
    return syscall_deliver_signals(ret);
}

static int execute_bin_internal(const char *filepath, int verbose) {
    return execute_bin_full(filepath, verbose, 0);
}

/* Launch and wait. Only for callers that have nothing to draw while they
 * wait — the boot self-test — because it stops the compositor thread. */
static int execute_bin_blocking(const char *filepath, int verbose) {
    return execute_bin_full(filepath, verbose, 1);
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

/*
 * The packages figure the browser's start page shows.
 *
 * Declared in src/browser.h, which is included before store.h and so
 * cannot see the install list. Defined here, immediately after the file
 * that owns it — the same arrangement prof_may and fs_native use for the
 * filesystem layer's forward declarations.
 */
static int brw_installed_count(void) { return store_inst_count; }
/* After apps.h and store.h: the gadgets read the same network and memory
 * state the system monitor does, and the jump lists read the recent-item
 * lists the apps push into. */
#include "shell.h"
#include "calc.h"
/* After calc.h; needs ac97_play from the driver kernel.c pulls in. */
#include "media.h"
#include "solid.h"
#include "chip8.h"
#include "chamber.h"

/*
 * Canvas app (WK_HELLO) content drawer.
 *
 * The pixels come from whichever surface is at the front — the process's
 * own, isolated from every other one — and from the shared fallback
 * canvas only when the pool was exhausted at spawn. Nothing else about
 * the window changed: it is the same rectangle in the same place, and a
 * program cannot tell which of the two it was given.
 *
 * Reported through app_composite_surface so that the compositor's
 * batched path and this one can never disagree about whose pixels the
 * application window is showing.
 */
static const uint32_t *app_composite_surface(void) {
    if (app_surf_front && app_surf_front->refs) return app_surf_front->px;
    return app_canvas;
}

static void hello_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       uint32_t tick, int focused) {
    (void)tick; (void)focused;
    const uint32_t *src = app_composite_surface();
    int32_t bw2 = cw < APP_CANVAS_W ? cw : APP_CANVAS_W;
    int32_t bh2 = chh < APP_CANVAS_H ? chh : APP_CANVAS_H;
    for (int32_t y = 0; y < bh2; y++) {
        int32_t dy = cy + y;
        if (dy < 0 || dy >= (int32_t)h) continue;
        for (int32_t x = 0; x < bw2; x++) {
            int32_t dx = cx + x;
            if (dx < 0 || dx >= (int32_t)w) continue;
            buf[(uint32_t)dy * w + (uint32_t)dx] =
                src[y * APP_CANVAS_W + x];
        }
    }
}

/* ===== 6. WINDOW MANAGER ===== */

/*
 * A window is a rectangle plus the memory of where it used to be.
 *
 * Minimizing and snapping are both "put it somewhere else and be able to
 * put it back", so they share one saved rectangle rather than keeping two
 * that could disagree. `snap` records which edge claimed the window so a
 * drag off that edge knows what to restore, and `have_rest` says whether
 * the saved rectangle means anything yet -- without it, restoring a window
 * that was never moved would snap it to a rect of zeroes.
 */
enum { SNAP_NONE = 0, SNAP_LEFT, SNAP_RIGHT, SNAP_MAX };

typedef struct {
    int     open;
    int32_t x, y, w, h;
    int     min;                  /* minimized to the taskbar */
    int     snap;                 /* SNAP_* */
    int     have_rest;
    int32_t rx, ry, rw, rh;       /* where to put it back */
} win_t;

static win_t wins[WK_COUNT];
static int wm_stack[WK_COUNT];
static int wm_stack_n = 0;
static int wm_focus = -1;
static int wm_drag = -1;
static int32_t wm_drag_ox = 0, wm_drag_oy = 0;

/*
 * ---- window motion ----
 *
 * There used to be one animation in this system: a gold outline growing
 * from a dock icon to a new window over twelve frames, linearly. Opening
 * was the only thing that moved. Minimising a window made it vanish;
 * maximising made it jump; a shake made four windows disappear at once
 * with nothing to say where they had gone.
 *
 * Now every geometry change a window makes is a flight from one
 * rectangle to another, driven by the spring in gfx.h. What is drawn
 * during the flight is a ghost -- the window's outline and title bar,
 * scaled and faded -- rather than the window's real contents, which
 * keeps the whole thing independent of what any application happens to
 * be drawing and costs a few hundred blended pixels a frame.
 *
 * One record per window kind, because a shake starts four of them at
 * once and they all have to run.
 */
typedef struct {
    int      active;
    int      hide;                 /* window is not drawn while flying */
    spring_t t;                    /* progress, 0 -> 1                 */
    float    x0, y0, w0, h0;
    float    x1, y1, w1, h1;
    float    a0, a1;               /* alpha at each end                */
} wm_anim_t;

static wm_anim_t wm_anim[WK_COUNT];

/* Needed to aim a minimise at the dock icon it collapses into; both are
 * defined much further down, with the dock. */
static void dock_bar_rect(uint32_t scr_w, int32_t *rx, int32_t *ry,
                          int32_t *rw, int32_t *rh);
static void dock_icon_rect(uint32_t scr_w, int idx,
                           int32_t *ix, int32_t *iy, int32_t *iw, int32_t *ih);
static int  dock_running_kind(int idx);

static void wm_anim_start(int kind,
                          float x0, float y0, float w0, float h0,
                          float x1, float y1, float w1, float h1,
                          float a0, float a1, int hide) {
    if (kind < 0 || kind >= WK_COUNT) return;
    wm_anim_t *a = &wm_anim[kind];
    a->active = 1;
    a->hide   = hide;
    a->t.p    = 0.0f;
    a->t.v    = 0.0f;
    a->x0 = x0; a->y0 = y0; a->w0 = w0; a->h0 = h0;
    a->x1 = x1; a->y1 = y1; a->w1 = w1; a->h1 = h1;
    a->a0 = a0; a->a1 = a1;
}

static int wm_anim_hides(int kind) {
    return kind >= 0 && kind < WK_COUNT &&
           wm_anim[kind].active && wm_anim[kind].hide;
}

/*
 * Where a window goes when it is minimised: the middle of its own dock
 * icon, if it has one. Falling back to the middle of the dock is not
 * merely a default -- a window whose program is not pinned genuinely has
 * nowhere more specific to go.
 */
static void wm_dock_target(int kind, float *tx, float *ty) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(scr_w_cache, &rx, &ry, &rw, &rh);
    *tx = (float)(rx + rw / 2);
    *ty = (float)(ry + rh / 2);

    for (int i = 0; i < dock_item_count; i++) {
        if (dock_running_kind(i) != kind) continue;
        int32_t ix, iy, iw, ih;
        dock_icon_rect(scr_w_cache, i, &ix, &iy, &iw, &ih);
        *tx = (float)(ix + iw / 2);
        *ty = (float)(iy + ih / 2);
        return;
    }
}

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
        /* Launching something already running means "show me it", which
         * for a minimized window is a restore, not just a raise. */
        wins[kind].min = 0;
        wm_raise(kind);
        return;
    }
    win_t *win = &wins[kind];
    win->open = 1;
    win->min = 0;
    win->snap = SNAP_NONE;
    win->have_rest = 0;
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
    if (kind == WK_MEDIA)
        media_scan();
    if (kind == WK_CHIP8) { c8_reset(); c8_running = 1; }

    wm_raise(kind);
}

static void wm_close(int kind) {
    if (!wins[kind].open) return;
    /* Cancel anything in flight. A window closed mid-animation would
     * otherwise leave a ghost finishing its journey to somewhere no
     * window exists any more -- which the old spawn animation avoided by
     * checking `open` on every frame, and which this has to do here
     * instead because several can be running at once. */
    wm_anim[kind].active = 0;
    wins[kind].open = 0;
    wm_stack_remove(kind);
    if (wm_drag == kind) wm_drag = -1;
    wm_focus = wm_stack_n > 0 ? wm_stack[wm_stack_n - 1] : -1;
}

/* --- minimize, snap, restore ---
 *
 * The work area is everything the menubar and the taskbar are not. Snap
 * measures against it rather than the screen, so a maximized window does
 * not slide under either of them.
 */
static void wm_work_area(int32_t *ax, int32_t *ay, int32_t *aw, int32_t *ah) {
    *ax = 0;
    *ay = MENUBAR_H;
    *aw = (int32_t)scr_w_cache;
    *ah = (int32_t)scr_h_cache - MENUBAR_H - dock_cfg.bar_h - 8;
    if (*ah < 120) *ah = 120;
}

#define WIN_MIN_W 240
#define WIN_MIN_H 140

static void wm_save_rect(int kind) {
    win_t *win = &wins[kind];
    if (win->snap != SNAP_NONE) return;   /* already saved by the first snap */
    win->rx = win->x; win->ry = win->y;
    win->rw = win->w; win->rh = win->h;
    win->have_rest = 1;
}

/* A window that changes shape does it visibly. The ghost flies from
 * where it was to where it now is, at full opacity, and the window
 * itself is held back until it lands -- so a maximise reads as the
 * window growing rather than as one window replacing another. */
static void wm_anim_reshape(int kind, int32_t ox, int32_t oy,
                            int32_t ow, int32_t oh) {
    win_t *win = &wins[kind];
    if (win->x == ox && win->y == oy && win->w == ow && win->h == oh) return;
    wm_anim_start(kind, (float)ox, (float)oy, (float)ow, (float)oh,
                  (float)win->x, (float)win->y,
                  (float)win->w, (float)win->h,
                  235.0f, 235.0f, 1);
}

static void wm_restore_rect(int kind) {
    win_t *win = &wins[kind];
    if (!win->have_rest) return;
    int32_t ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    win->x = win->rx; win->y = win->ry;
    win->w = win->rw; win->h = win->rh;
    win->snap = SNAP_NONE;
    wm_anim_reshape(kind, ox, oy, ow, oh);
}

static void wm_snap_to(int kind, int where) {
    win_t *win = &wins[kind];
    if (where == SNAP_NONE) { wm_restore_rect(kind); return; }
    const int32_t ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    int32_t ax, ay, aw, ah;
    wm_work_area(&ax, &ay, &aw, &ah);
    wm_save_rect(kind);
    win->snap = where;
    win->y = ay;
    win->h = ah;
    if (where == SNAP_MAX) { win->x = ax;            win->w = aw; }
    else if (where == SNAP_LEFT)  { win->x = ax;              win->w = aw / 2; }
    else                          { win->x = ax + aw / 2;     win->w = aw - aw / 2; }
    if (win->w < WIN_MIN_W) win->w = WIN_MIN_W;
    if (win->h < WIN_MIN_H) win->h = WIN_MIN_H;
    wm_anim_reshape(kind, ox, oy, ow, oh);
}

static void wm_minimize(int kind) {
    if (!wins[kind].open || wins[kind].min) return;

    /* Collapse into the dock icon it will reappear from. The window is
     * marked minimised immediately -- the animation is a picture of what
     * has already happened, not the thing itself, so a click that lands
     * mid-flight is answered by the new state and not the old. */
    float tx, ty;
    wm_dock_target(kind, &tx, &ty);
    wm_anim_start(kind,
                  (float)wins[kind].x, (float)wins[kind].y,
                  (float)wins[kind].w, (float)wins[kind].h,
                  tx - 8.0f, ty - 8.0f, 16.0f, 16.0f,
                  235.0f, 0.0f, 0);

    wins[kind].min = 1;
    if (wm_focus == kind) {
        wm_focus = -1;
        for (int i = wm_stack_n - 1; i >= 0; i--)
            if (!wins[wm_stack[i]].min) { wm_focus = wm_stack[i]; break; }
    }
}

static void wm_unminimize(int kind) {
    if (!wins[kind].open) return;
    if (wins[kind].min) {
        float tx, ty;
        wm_dock_target(kind, &tx, &ty);
        wm_anim_start(kind,
                      tx - 8.0f, ty - 8.0f, 16.0f, 16.0f,
                      (float)wins[kind].x, (float)wins[kind].y,
                      (float)wins[kind].w, (float)wins[kind].h,
                      0.0f, 235.0f, 1);
    }
    wins[kind].min = 0;
    wm_raise(kind);
}

/* Shake minimizes everything *but* the window being shaken. */
static void wm_minimize_others(int keep) {
    for (int i = 0; i < wm_stack_n; i++)
        if (wm_stack[i] != keep) wm_minimize(wm_stack[i]);
}

static int wm_any_minimized(void) {
    for (int i = 0; i < wm_stack_n; i++)
        if (wins[wm_stack[i]].min) return 1;
    return 0;
}

static void wm_unminimize_all(void) {
    for (int i = 0; i < wm_stack_n; i++)
        if (wins[wm_stack[i]].min) wm_unminimize(wm_stack[i]);
}

static void wm_content_rect(int kind, int32_t *cx, int32_t *cy,
                            int32_t *cw, int32_t *chh) {
    win_t *win = &wins[kind];
    *cx = win->x + WIN_BORDER;
    *cy = win->y + WIN_BORDER + WIN_TITLE_H;
    *cw = win->w - 2 * WIN_BORDER;
    *chh = win->h - 2 * WIN_BORDER - WIN_TITLE_H;
}

/*
 * Three title-bar buttons, right to left: close, maximize, minimize.
 *
 * They are indexed rather than named so the hit test and the drawing walk
 * the same arithmetic -- the old single button had its position written
 * out twice, which is exactly the kind of duplication that drifts.
 */
#define WM_BTN_CLOSE 0
#define WM_BTN_MAX   1
#define WM_BTN_MIN   2

static int32_t wm_btn_x(int kind, int which) {
    return wins[kind].x + wins[kind].w - 22 - which * 22;
}

static int32_t wm_btn_y(int kind) {
    return wins[kind].y + WIN_TITLE_H / 2 + WIN_BORDER;
}

static int wm_hit_btn(int kind, int which, int32_t mx, int32_t my) {
    int32_t dx = mx - wm_btn_x(kind, which);
    int32_t dy = my - wm_btn_y(kind);
    return dx * dx + dy * dy <= 81;   /* r=9 hit circle */
}


static int wm_hit_window(int kind, int32_t mx, int32_t my) {
    win_t *win = &wins[kind];
    if (win->min) return 0;        /* minimized: on the taskbar, not the desktop */
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

    /* Title, centred in the space the buttons leave rather than in the
     * whole title bar -- with three buttons instead of one, centring on
     * the window would run a long title straight under them. */
    {
        const char *title = wm_title_for(kind);
        const int32_t avail_l = win->x + 10;
        const int32_t avail_r = wm_btn_x(kind, WM_BTN_MIN) - 12;
        int tw = ttf_text_width(title, 13);
        int32_t tx = avail_l + (avail_r - avail_l - tw) / 2;
        if (tx < avail_l) tx = avail_l;
        ttf_draw_string_clip(buf, (int)w, (int)h, tx, win->y + 5, title,
                             focused ? C_TEXT : C_TEXT_DIM, 13, avail_r);
    }

    /* Close, maximize, minimize. The glyph inside each only appears on the
     * focused window, so an unfocused stack reads as a row of quiet dots
     * rather than a wall of controls competing for attention. */
    {
        const int32_t by = wm_btn_y(kind);
        for (int b = 0; b < 3; b++) {
            const int32_t bx = wm_btn_x(kind, b);
            uint32_t fill = 0x4A5060u;
            if (focused)
                fill = (b == WM_BTN_CLOSE) ? C_RED :
                       (b == WM_BTN_MAX)   ? 0x4C7A3Cu : 0x9A7A2Cu;
            gfx_circle(buf, w, h, bx, by, 7, fill);
            if (!focused) continue;

            if (b == WM_BTN_CLOSE) {
                gfx_line(buf, w, h, bx - 3, by - 3, bx + 3, by + 3, 1, 0x5A1616u);
                gfx_line(buf, w, h, bx - 3, by + 3, bx + 3, by - 3, 1, 0x5A1616u);
            } else if (b == WM_BTN_MAX) {
                /* an outline when it would maximize, a filled square when
                 * it would restore -- the glyph says what the click does */
                if (win->snap == SNAP_MAX)
                    gfx_rect(buf, w, h, bx - 3, by - 3, 6, 6, 0x1E3416u);
                else
                    gfx_rect_outline(buf, w, h, bx - 3, by - 3, 6, 6, 0x1E3416u);
            } else {
                gfx_rect(buf, w, h, bx - 3, by + 2, 7, 2, 0x3A2E10u);
            }
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
    case WK_CALC:
        calc_draw(buf, w, h, cx, cy, cw, chh, mouse_x, mouse_y);
        break;
    case WK_MEDIA:
        media_draw(buf, w, h, cx, cy, cw, chh, mouse_x, mouse_y);
        break;
    case WK_SOLID:
        solid_draw(buf, w, h, cx, cy, cw, chh, mouse_x, mouse_y);
        break;
    case WK_CHIP8:
        c8_app_draw(buf, w, h, cx, cy, cw, chh, mouse_x, mouse_y);
        c8_keys_decay();
        break;
    case WK_CHAMBER:
        chamber_draw(buf, w, h, cx, cy, cw, chh, mouse_x, mouse_y);
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

/* ===== AERO =====
 *
 * Snap, Shake and Peek are three readings of the same drag. None of them
 * needs a gesture recogniser: an edge is a comparison, a shake is a count
 * of direction changes, and a peek is a ramp on a latch.
 *
 * Peek used to be a hover: crossing the dock on the way to anything at
 * the bottom of the screen dissolved every window, which is a large
 * effect to trigger by accident and no way to decline. It is a click on
 * the Show Desktop tab now, and it latches -- clicking the tab again, or
 * touching any window, puts the stack back. Nothing fades on hover.
 */

#define PEEK_RAMP     8      /* frames to fade the windows out and back */
#define PEEK_ALPHA  216      /* how much wallpaper shows at full peek */
#define SNAP_EDGE     8      /* how close to an edge counts as snapping */
#define SHAKE_FLIPS   4      /* direction changes that mean "shake" */
#define SHAKE_WINDOW 28      /* ...within this many frames */
#define SHAKE_MIN_DX  9      /* travel that counts as a stroke, not a wobble */

static int32_t aero_peek = 0;          /* 0..PEEK_RAMP, the ramp */
static int     aero_peek_hold = 0;     /* the latch the ramp chases */
static int     aero_snap_hint = SNAP_NONE;

static int32_t shake_last_x = 0;
static int     shake_dir = 0, shake_flips = 0, shake_age = 0;

static void aero_shake_reset(void) {
    shake_dir = 0; shake_flips = 0; shake_age = 0;
}

/* Which edge, if any, the pointer is claiming right now. */
static int aero_snap_zone(int32_t mx, int32_t my) {
    if (my < MENUBAR_H + SNAP_EDGE)                  return SNAP_MAX;
    if (mx < SNAP_EDGE)                              return SNAP_LEFT;
    if (mx >= (int32_t)scr_w_cache - SNAP_EDGE)      return SNAP_RIGHT;
    return SNAP_NONE;
}

/* The translucent target the snap would fill, drawn under the window. */
static void aero_snap_preview(uint32_t *buf, uint32_t w, uint32_t h) {
    if (aero_snap_hint == SNAP_NONE) return;
    int32_t ax, ay, aw, ah;
    wm_work_area(&ax, &ay, &aw, &ah);
    int32_t px = ax, py = ay, pw = aw, ph = ah;
    if (aero_snap_hint == SNAP_LEFT)  pw = aw / 2;
    if (aero_snap_hint == SNAP_RIGHT) { px = ax + aw / 2; pw = aw - aw / 2; }
    gfx_rect_blend(buf, w, h, px, py, pw, ph, C_GOLD, 34);
    gfx_rect_outline(buf, w, h, px, py, pw, ph, C_GOLD_DIM);
    gfx_rect_outline(buf, w, h, px + 1, py + 1, pw - 2, ph - 2, 0x2A2618u);
}

/* aero_peek_draw is further down, next to desktop_render -- it reads the
 * wallpaper, which is not declared until after the window manager. */

static void wm_update(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                      int click_consumed) {
    int click = (lmb && !prev_lmb) && !click_consumed;

    /* A release has to be seen before the drag is forgotten -- that is
     * the event Snap acts on. */
    if (!lmb && wm_drag >= 0) {
        if (aero_snap_hint != SNAP_NONE)
            wm_snap_to(wm_drag, aero_snap_hint);
        aero_snap_hint = SNAP_NONE;
        aero_shake_reset();
    }
    if (!lmb)
        wm_drag = -1;

    if (click) {
        int hit = wm_top_at(mx, my);
        if (hit >= 0) {
            /* Reaching for a window is the other way out of Peek: the
             * stack is faded, not gone, so a click on one means the
             * user is done looking at the desktop. */
            aero_peek_hold = 0;
            wm_raise(hit);
            if (wm_hit_btn(hit, WM_BTN_CLOSE, mx, my)) {
                wm_close(hit);
            } else if (wm_hit_btn(hit, WM_BTN_MAX, mx, my)) {
                wm_snap_to(hit, wins[hit].snap == SNAP_MAX ? SNAP_NONE : SNAP_MAX);
            } else if (wm_hit_btn(hit, WM_BTN_MIN, mx, my)) {
                wm_minimize(hit);
            } else if (my < wins[hit].y + WIN_TITLE_H + WIN_BORDER) {
                /* Dragging a snapped window unsnaps it, and the restored
                 * window is hung off the cursor at the same proportion of
                 * its width -- grab it near the right edge and it stays
                 * near the right edge, which is where the hand expects it. */
                if (wins[hit].snap != SNAP_NONE && wins[hit].have_rest) {
                    const int32_t grip = wins[hit].w > 0
                        ? (mx - wins[hit].x) * wins[hit].rw / wins[hit].w
                        : wins[hit].rw / 2;
                    wm_restore_rect(hit);
                    wins[hit].x = mx - grip;
                    wins[hit].y = my - WIN_TITLE_H / 2;
                }
                wm_drag = hit;
                wm_drag_ox = mx - wins[hit].x;
                wm_drag_oy = my - wins[hit].y;
                shake_last_x = mx;
                aero_shake_reset();
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

        aero_snap_hint = aero_snap_zone(mx, my);

        /*
         * Shake. Only strokes longer than SHAKE_MIN_DX are counted, so
         * the pixel jitter of a hand holding still never accumulates,
         * and the count expires on a timer so that four slow direction
         * changes over several seconds are just someone moving a window.
         */
        if (++shake_age > SHAKE_WINDOW) {
            aero_shake_reset();
            shake_last_x = mx;
        }
        const int32_t dx = mx - shake_last_x;
        const int32_t adx = dx < 0 ? -dx : dx;
        if (adx >= SHAKE_MIN_DX) {
            const int dir = dx > 0 ? 1 : -1;
            if (shake_dir && dir != shake_dir) shake_flips++;
            shake_dir = dir;
            shake_last_x = mx;
            if (shake_flips >= SHAKE_FLIPS) {
                /* The gesture is its own undo: shake once to clear the
                 * desk, shake again to put it back the way it was. */
                if (wm_any_minimized()) wm_unminimize_all();
                else                    wm_minimize_others(wm_drag);
                wm_raise(wm_drag);
                aero_shake_reset();
                aero_snap_hint = SNAP_NONE;
            }
        }
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
        case WK_CALC:
            calc_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_MEDIA:
            media_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_SOLID:
            solid_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_CHIP8:
            c8_app_mouse(mx, my, eff_lmb, prev_lmb, cx, cy, cw, chh);
            break;
        case WK_CHAMBER:
            chamber_mouse(mx, my, eff_lmb, prev_lmb);
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

/* --- taskbar previews ---
 *
 * One thumbnail per window kind, filled from the back buffer at the point
 * in the walk where that window has just been drawn: at that instant its
 * rectangle holds itself and nothing above it, so a downscale is a
 * complete and correct capture with no offscreen re-render.
 *
 * Refreshed every THUMB_EVERY frames rather than every frame -- a preview
 * does not need 60 fps, and a window that gets minimized keeps the last
 * capture from just before it went, which is the picture the taskbar
 * wants to show anyway.
 */
#define THUMB_W     144
#define THUMB_H     90
#define THUMB_EVERY 6

static uint32_t wm_thumb[WK_COUNT][THUMB_W * THUMB_H];
static uint8_t  wm_thumb_valid[WK_COUNT];

static void wm_capture_thumb(const uint32_t *buf, uint32_t w, uint32_t h,
                             int kind) {
    const win_t *win = &wins[kind];
    if (win->w <= 0 || win->h <= 0) return;
    gfx_downscale(wm_thumb[kind], THUMB_W, THUMB_H, buf, w, h,
                  win->x, win->y, win->w, win->h);
    wm_thumb_valid[kind] = 1;
}

/* ===================================================================
 * 6b. THE COMPOSITING ENGINE
 * ===================================================================
 *
 * One routine owns the frame. It walks the window stack once, back to
 * front, and for each window puts down a soft shadow, a translucent
 * frame and the window's contents — where the contents of an
 * application window are the pixels of that process's own offscreen
 * surface and nothing else.
 *
 * What is new here is not the walk, which is the order the desktop has
 * always drawn in. It is that the expensive, opaque half of the work is
 * handed to the Gen9 blitter as batched ring commands, and the cheap,
 * blended half stays on the processor because the blitter has no alpha
 * term to give it. igpu.h argues that split at length; the short
 * version is that XY_SRC_COPY_BLT is a raster operation, source-over is
 * not one, and pretending otherwise would mean a 3D pipeline this
 * driver does not implement.
 *
 * ---- the ordering rule, and why it needs stating ----
 *
 * A batched command executes when the batch is submitted, not when it
 * is emitted. So a GPU copy queued while walking window 2, and a
 * processor-side blend performed while walking window 3, can reach the
 * same pixel in the wrong order: the blend lands first and the copy
 * overwrites it. Nothing about that is visible in the source, and it
 * would present as a window intermittently painting over the shadow of
 * the one above it.
 *
 * The rule that rules it out is one line: before the processor writes
 * anywhere, the pending batch is submitted if — and only if — that
 * write touches a rectangle the batch is going to write. Windows that
 * do not overlap therefore accumulate into one submission, and windows
 * that do cost a submission each, which is exactly the trade the
 * hardware imposes and not one chosen here.
 */

/* The destination rectangle the pending batch will write, as a bounding
 * box. A box rather than a list because the test below is conservative
 * in the safe direction: a false overlap costs one early submission, a
 * missed one costs a corrupt frame. */
static int32_t aero_gpu_x0, aero_gpu_y0, aero_gpu_x1, aero_gpu_y1;
static int     aero_gpu_pending = 0;
/* aero_gpu_batches and aero_gpu_ops are declared with the surfaces, in
 * section 4a, because the system monitor is included before this. */

static void aero_gpu_reset(void) {
    aero_gpu_pending = 0;
    aero_gpu_x0 = aero_gpu_y0 = 0;
    aero_gpu_x1 = aero_gpu_y1 = 0;
}

static void aero_gpu_note(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (!aero_gpu_pending) {
        aero_gpu_x0 = x; aero_gpu_y0 = y;
        aero_gpu_x1 = x + w; aero_gpu_y1 = y + h;
        aero_gpu_pending = 1;
        return;
    }
    if (x < aero_gpu_x0)         aero_gpu_x0 = x;
    if (y < aero_gpu_y0)         aero_gpu_y0 = y;
    if (x + w > aero_gpu_x1)     aero_gpu_x1 = x + w;
    if (y + h > aero_gpu_y1)     aero_gpu_y1 = y + h;
}

/* Submit whatever is queued, wait for the engine to retire it, and make
 * the processor's view of the affected rectangle agree with what was
 * written. Everything after this call may read those pixels. */
static void aero_gpu_flush(uint32_t *buf, uint32_t w) {
    if (!aero_gpu_pending) return;
    if (igpu_comp_submit() == 0) {
        igpu_comp_sync_cpu(buf, w * 4u, aero_gpu_x0, aero_gpu_y0,
                           aero_gpu_x1 - aero_gpu_x0,
                           aero_gpu_y1 - aero_gpu_y0);
        aero_gpu_batches++;
        aero_gpu_ops += igpu_comp.ops;
    }
    aero_gpu_reset();
}

/* The barrier. Called immediately before any processor-side write, with
 * the rectangle that write covers. */
static void aero_gpu_barrier(uint32_t *buf, uint32_t w,
                             int32_t x, int32_t y, int32_t rw, int32_t rh) {
    if (!aero_gpu_pending) return;
    if (x >= aero_gpu_x1 || x + rw <= aero_gpu_x0 ||
        y >= aero_gpu_y1 || y + rh <= aero_gpu_y0)
        return;                              /* disjoint: nothing to wait for */
    aero_gpu_flush(buf, w);
}

/*
 * ---- live previews, taken from the process rather than the screen ----
 *
 * The taskbar's thumbnails were captured out of the back buffer at the
 * moment a window had just been drawn, which is exact and cheap and
 * only works for a window that is on screen. An application that is
 * running behind another one has no pixels in the back buffer to
 * capture, and before private surfaces there was nothing else to read.
 *
 * There is now. Each process owns its window's pixels for the whole of
 * its life, on screen or not, so a preview is a downscale straight from
 * the surface — correct for every running application at once rather
 * than for whichever of them the window happened to be showing.
 */
static uint32_t aero_surf_thumb[APP_SURF_MAX][THUMB_W * THUMB_H];
static uint8_t  aero_surf_thumb_valid[APP_SURF_MAX];

static void aero_surface_previews(void) {
    if ((desktop_tick % THUMB_EVERY) != 0) return;
    for (int i = 0; i < APP_SURF_MAX; i++) {
        if (!app_surf[i].refs || !app_surf[i].gen) {
            aero_surf_thumb_valid[i] = 0;
            continue;
        }
        gfx_downscale(aero_surf_thumb[i], THUMB_W, THUMB_H,
                      app_surf[i].px, APP_CANVAS_W, APP_CANVAS_H,
                      0, 0, APP_CANVAS_W, APP_CANVAS_H);
        aero_surf_thumb_valid[i] = 1;
    }
}

/*
 * ---- the glass edge ----
 *
 * A one-pixel gold outline said "focused" and nothing else. What a
 * window is missing without a translucent border is the sense of being
 * a pane held above the desktop rather than a rectangle stamped onto
 * it, and that is entirely an alpha effect: a few concentric strips
 * whose colour is mixed with whatever is already underneath, brightest
 * at the edge the light would catch.
 *
 * Drawn outside the window's own rectangle so it costs the window
 * nothing and cannot be mistaken for content. Four strips rather than a
 * gradient loop because the falloff is short and the strips are one
 * pixel each — a loop would be the same arithmetic with an index.
 */
#define AERO_GLASS      4       /* strips of border outside the frame    */
#define AERO_GLASS_FOC  96      /* peak alpha, focused                   */
#define AERO_GLASS_UNF  40      /* and not                               */

static void aero_glass_border(uint32_t *buf, uint32_t w, uint32_t h,
                              const win_t *win, int focused) {
    const uint32_t tint = focused ? C_GOLD : 0x6E7890u;
    const uint32_t peak = focused ? AERO_GLASS_FOC : AERO_GLASS_UNF;

    for (int d = 1; d <= AERO_GLASS; d++) {
        /* Linear falloff from the frame outward. The +1 keeps the
         * outermost strip from vanishing entirely at low peak alpha,
         * which is what made an unfocused window's edge flicker in and
         * out as the shadow underneath it changed. */
        const uint32_t a = peak * (uint32_t)(AERO_GLASS - d + 1)
                                / (uint32_t)AERO_GLASS;
        if (!a) continue;
        const int32_t x = win->x - d, y = win->y - d;
        const int32_t bw = win->w + 2 * d, bh = win->h + 2 * d;
        gfx_rect_blend(buf, w, h, x,          y,          bw, 1,  tint, a);
        gfx_rect_blend(buf, w, h, x,          y + bh - 1, bw, 1,  tint, a);
        gfx_rect_blend(buf, w, h, x,          y,          1,  bh, tint, a);
        gfx_rect_blend(buf, w, h, x + bw - 1, y,          1,  bh, tint, a);
    }
}

/*
 * The application window's contents: this process's surface, copied by
 * the engine where it can reach both ends and by the processor where it
 * cannot.
 *
 * Returns 1 if the copy was queued on the engine, so that the caller
 * knows to record the rectangle with aero_gpu_note rather than treating
 * the pixels as already present.
 */
static int aero_blit_surface_gpu(int32_t cx, int32_t cy,
                                 int32_t cw, int32_t chh) {
    if (!igpu_comp_active()) return 0;
    if (!app_surf_front || !app_surf_front->refs) return 0;
    if (!app_surf_front->gpu_addr) return 0;

    int32_t bw2 = cw  < APP_CANVAS_W ? cw  : APP_CANVAS_W;
    int32_t bh2 = chh < APP_CANVAS_H ? chh : APP_CANVAS_H;
    if (bw2 <= 0 || bh2 <= 0) return 0;

    return igpu_comp_blit(app_surf_front->gpu_addr, APP_CANVAS_W * 4u,
                          0, 0, cx, cy, bw2, bh2) == 0;
}

/*
 * Every window, bottom of the stack upward.
 *
 * Shadow, glass, frame, content -- in that order and per window, not
 * shadows for all of them and then frames for all of them. Drawn in one
 * pass over the stack, a window's shadow lands on whatever is already
 * beneath it and is then covered by whatever is drawn after, which is
 * what makes the stack read as a stack. Hoisting the shadows into a
 * pass of their own would put the topmost window's shadow underneath
 * every other window, which is exactly the depth cue inverted -- and it
 * is also why the batch has a barrier rather than being submitted once
 * at the end.
 */
static void wm_draw_all(uint32_t *buf, uint32_t w, uint32_t h) {
    const int grab = (desktop_tick % THUMB_EVERY) == 0;

    for (int i = 0; i < wm_stack_n; i++) {
        int kind = wm_stack[i];
        if (wins[kind].min) continue;              /* it is on the taskbar */
        if (wm_anim_hides(kind))
            continue;   /* revealed when the animation lands */
        const win_t *win = &wins[kind];

        /* The shadow reaches outside the window on every side; the glass
         * reaches AERO_GLASS further still. One barrier covering both is
         * cheaper than two and no less exact. */
        const int32_t mx0 = win->x - GFX_SHADOW_R - AERO_GLASS;
        const int32_t my0 = win->y - GFX_SHADOW_R - AERO_GLASS;
        const int32_t mw  = win->w + 2 * (GFX_SHADOW_R + AERO_GLASS);
        const int32_t mh  = win->h + 2 * (GFX_SHADOW_R + AERO_GLASS);
        aero_gpu_barrier(buf, w, mx0, my0, mw, mh);

        gfx_shadow(buf, w, h, win->x, win->y, win->w, win->h);
        aero_glass_border(buf, w, h, win, wm_focus == kind);
        wm_draw_frame(buf, w, h, kind);

        /*
         * The application window is the one whose contents live outside
         * this file, in a page some ring-3 program owns, so it is the
         * one the engine can move without the processor reading a
         * single pixel. Everything else is drawn here by code that is
         * already holding the values it needs.
         */
        int32_t cx, cy, cw, chh;
        wm_content_rect(kind, &cx, &cy, &cw, &chh);
        if (kind == WK_HELLO && aero_blit_surface_gpu(cx, cy, cw, chh)) {
            aero_gpu_note(cx, cy,
                          cw  < APP_CANVAS_W ? cw  : APP_CANVAS_W,
                          chh < APP_CANVAS_H ? chh : APP_CANVAS_H);
        } else {
            wm_draw_content(buf, w, h, kind);
        }

        /* The capture reads the back buffer, so anything queued for this
         * window has to have landed in it first. */
        if (grab) {
            aero_gpu_barrier(buf, w, win->x, win->y, win->w, win->h);
            wm_capture_thumb(buf, w, h, kind);
        }
    }

    /* Nothing below this point belongs to a window, and the chrome is
     * drawn over all of it, so the frame's last batch retires here. */
    aero_gpu_flush(buf, w);
}

/* --- window motion, drawn --- */

/*
 * One ghost per running animation.
 *
 * The spring is stepped once per frame here rather than at the moment
 * something is clicked, which is what keeps the motion tied to the
 * display's clock: the compositor sleeps on the 60 Hz frame pulse, so
 * this runs exactly sixty times a second whatever else the machine is
 * doing.
 *
 * What is drawn is deliberately not the window. Scaling a live window's
 * contents would mean resampling whatever the application last painted,
 * every frame, for every window in flight; the outline and the title bar
 * carry the motion perfectly well and cost a few hundred blended pixels.
 */
static void wm_anim_draw(uint32_t *buf, uint32_t w, uint32_t h) {
    for (int kind = 0; kind < WK_COUNT; kind++) {
        wm_anim_t *a = &wm_anim[kind];
        if (!a->active) continue;

        spring_step(&a->t, 1.0f);
        if (spring_settled(&a->t, 1.0f)) { a->active = 0; continue; }

        float t = a->t.p;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        int32_t x = (int32_t)gfx_lerp(a->x0, a->x1, t);
        int32_t y = (int32_t)gfx_lerp(a->y0, a->y1, t);
        int32_t cw = (int32_t)gfx_lerp(a->w0, a->w1, t);
        int32_t ch = (int32_t)gfx_lerp(a->h0, a->h1, t);
        uint32_t al = (uint32_t)gfx_lerp(a->a0, a->a1, t);
        if (cw < 2 || ch < 2 || al == 0) continue;
        if (al > 255) al = 255;

        /* Its shadow shrinks with it, or the ghost looks like a
         * cardboard cutout sliding over the desktop. */
        int sr = (int)(GFX_SHADOW_R * (cw > 240 ? 1.0f : (float)cw / 240.0f));
        if (sr > 1)
            gfx_shadow_rect(buf, w, h, x, y, cw, ch, sr,
                            (int)(GFX_SHADOW_A * al / 255u),
                            GFX_SHADOW_DX, GFX_SHADOW_DY);

        int32_t th = WIN_TITLE_H;
        if (th > ch) th = ch;
        gfx_rect_blend(buf, w, h, x, y, cw, th, C_TITLE_FOC, al);
        if (ch > th)
            gfx_rect_blend(buf, w, h, x, y + th, cw, ch - th, C_WIN_BG,
                           al * 3u / 4u);
        gfx_rect_blend(buf, w, h, x, y, cw, 1, C_GOLD, al);
        gfx_rect_blend(buf, w, h, x, y + ch - 1, cw, 1, C_GOLD, al);
        gfx_rect_blend(buf, w, h, x, y, 1, ch, C_GOLD, al);
        gfx_rect_blend(buf, w, h, x + cw - 1, y, 1, ch, C_GOLD, al);
    }
}

/* Opening a window from the dock is the same motion as restoring one,
 * so it is the same code: a ghost growing out of the icon. */
static void spawn_anim_start(int kind, int32_t icon_cx, int32_t icon_cy) {
    if (kind < 0 || kind >= WK_COUNT) return;
    wm_anim_start(kind,
                  (float)icon_cx - 8.0f, (float)icon_cy - 8.0f, 16.0f, 16.0f,
                  (float)wins[kind].x, (float)wins[kind].y,
                  (float)wins[kind].w, (float)wins[kind].h,
                  0.0f, 235.0f, 1);
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

/*
 * ---- giving the engine everything it has to be able to address ----
 *
 * The wallpaper, the back buffer and all six window surfaces, mapped
 * into the compositor's GGTT window once and never again. All of them
 * are static kernel arrays for exactly this reason: the GPU's address
 * allocator is a bump pointer with no free, so anything mapped into it
 * has to be something that lives as long as the machine does.
 *
 * A failure anywhere here is not a failure of anything. Every user of
 * these addresses tests for zero and takes the processor's path, which
 * is the path that runs on every machine without an Intel Gen9 part —
 * including, notably, the emulator this is most often booted in.
 */
static uint32_t aero_wall_gpu = 0;

static void aero_gpu_init(void *back, uint32_t w, uint32_t h) {
    if (igpu_comp_init(back, w, h) != 0) return;

    aero_wall_gpu = igpu_comp_map_virt(wallpaper, sizeof(wallpaper));

    int mapped = 0;
    for (int i = 0; i < APP_SURF_MAX; i++) {
        app_surf[i].gpu_addr = igpu_comp_map_virt(app_surf_mem[i],
                                                  APP_SURF_BYTES);
        if (app_surf[i].gpu_addr) mapped++;
    }

    serial_puts("[compositor] blitter compositing enabled: wallpaper ");
    serial_puts(aero_wall_gpu ? "mapped" : "unmapped");
    serial_puts(", ");
    serial_put_dec((uint32_t)mapped);
    serial_puts(" of ");
    serial_put_dec((uint32_t)APP_SURF_MAX);
    serial_puts(" surfaces reachable by the engine\n");
}

/*
 * The frame's clear, as one blitter command.
 *
 * Returns 0 when there is no engine or the wallpaper never reached the
 * GGTT, and the caller does the copy itself.
 *
 * ---- the stride, which is not the array's width ----
 *
 * The obvious pitch is WALL_MAX_W, because that is how the array is
 * declared, and it is wrong at every resolution except the largest one.
 * wallpaper_regen passes the *panel's* width to every drawing call it
 * makes, so the cache is packed at stride w with the rest of the array
 * left untouched — which is also why the processor's version of this
 * clear is a flat `buf[i] = wallpaper[i]` and not a row loop. A blit
 * that stepped by WALL_MAX_W would read each row from further and
 * further into the one below it and smear the whole frame diagonally.
 *
 * There is no way for a test on this machine to have caught that: QEMU
 * has no Gen9 part, so this function returns 0 there and the processor
 * path is the only one that ever runs.
 *
 * The generated size is compared rather than assumed. desktop_render
 * regenerates the cache before it gets here, so they agree; if they ever
 * did not, the honest answer is the processor's copy rather than a blit
 * with a pitch that describes a different image.
 */
static int aero_wallpaper_gpu(uint32_t w, uint32_t h) {
    if (!igpu_comp_active() || !aero_wall_gpu) return 0;
    if (wall_gen_w != w || wall_gen_h != h) return 0;
    if (igpu_comp_blit(aero_wall_gpu, wall_gen_w * 4u,
                       0, 0, 0, 0, (int)w, (int)h) != 0)
        return 0;
    aero_gpu_note(0, 0, (int32_t)w, (int32_t)h);
    return 1;
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
#define MENU_ACT_HIT_BASE 400      /* + index into search_hits[]  */

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

/*
 * Everything in the Apps menu goes through here, so a query filters the
 * built-in entries and the installed packages by the same rule. A
 * separator only earns its row if something was drawn above it and
 * something ends up below it, which is why they are added lazily.
 */
static int menu_add(int n, const char *label, int action) {
    if (n >= MENU_APPS_MAX) return n;
    if (search_q_n > 0 && action >= 0 && !str_contains_ci(label, search_q))
        return n;
    menu_apps[n].label = label;
    menu_apps[n].action = action;
    return n + 1;
}

static void menu_rebuild(void) {
    int n = 0;
    n = menu_add(n, "Terminal",  WK_TERM);
    n = menu_add(n, "Browser",   WK_BROWSER);
    n = menu_add(n, "Files",     WK_FILES);
    n = menu_add(n, "App Store", WK_STORE);
    n = menu_add(n, "Photos",    WK_IMAGE);
    n = menu_add(n, "Wikipedia", WK_WIKI);
    n = menu_add(n, "Calculator", WK_CALC);
    n = menu_add(n, "Media Player", WK_MEDIA);
    n = menu_add(n, "Solid", WK_SOLID);
    n = menu_add(n, "CHIP-8", WK_CHIP8);
    const int after_docs = n;
    n = menu_add(n, "Goldsmith", WK_PAINT);
    n = menu_add(n, "Monolith",  WK_SYSMON);
    n = menu_add(n, "Matrix",    WK_MATRIX);
    n = menu_add(n, "hello.elf", WK_HELLO);
    if (after_docs > 0 && n > after_docs && search_q_n == 0) {
        /* reopen the gap the separator belongs in */
        for (int i = n; i > after_docs; i--) menu_apps[i] = menu_apps[i - 1];
        menu_apps[after_docs].label = "-";
        menu_apps[after_docs].action = -1;
        n++;
    }

    if (store_inst_count > 0) {
        const int before = n;
        for (int i = 0; i < store_inst_count && n < MENU_APPS_MAX; i++)
            n = menu_add(n, store_inst[i].name, MENU_ACT_APP_BASE + i);
        if (n > before && before > 0 && n < MENU_APPS_MAX) {
            for (int i = n; i > before; i--) menu_apps[i] = menu_apps[i - 1];
            menu_apps[before].label = "-";
            menu_apps[before].action = -1;
            n++;
        }
    }

    /* Files and folders found on the volume, below the applications. */
    if (search_q_n > 0 && search_hit_n > 0 && n < MENU_APPS_MAX) {
        if (n > 0) {
            menu_apps[n].label = "-";
            menu_apps[n].action = -1;
            n++;
        }
        for (int i = 0; i < search_hit_n && n < MENU_APPS_MAX; i++) {
            menu_apps[n].label = search_hits[i].label;
            menu_apps[n].action = MENU_ACT_HIT_BASE + i;
            n++;
        }
    }
    menu_apps_n = n;
    menu_item_count[1] = n;
}

static int menu_open_idx = -1;

/* menu_open_idx is only ever set from a loop bounded by MENU_COUNT, but
 * that is not visible to the compiler everywhere it is used to index the
 * per-menu tables. One guard states the invariant instead of leaving each
 * subscript to be proved separately. */
static int menu_open_valid(void) {
    return menu_open_idx >= 0 && menu_open_idx < MENU_COUNT;
}

#define MENU_ITEM_H   26
#define MENU_DD_W     170
#define MENU_SEARCH_H 32

/*
 * The Apps menu is wider than the system menu because it has to hold
 * search results -- an article title or a file path in 170px would be
 * ellipsis with a couple of letters attached.
 */
static int32_t menu_dd_w(int idx) { return idx == 1 ? 264 : MENU_DD_W; }

/* Rows start below the search field on the menu that has one. */
static int32_t menu_head_h(int idx) { return idx == 1 ? MENU_SEARCH_H : 0; }

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
    if (action >= MENU_ACT_HIT_BASE) {
        const int i = action - MENU_ACT_HIT_BASE;
        if (i < search_hit_n) {
            /* A folder opens in Files at that folder; a file opens in
             * whichever app claims its extension. */
            if (search_hits[i].is_dir) {
                wm_open(WK_FILES);
                str_copy(exp_path, search_hits[i].path, EXP_PATH_MAX);
                exp_scan();
            } else {
                desktop_open_recent(search_hits[i].kind, search_hits[i].path);
            }
        }
    } else if (action >= MENU_ACT_APP_BASE) {
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
                search_clear();
                return 1;
            }
        }
        menu_open_idx = -1;
        return 1;    /* clicks on the bar never fall through */
    }

    /* click inside an open dropdown */
    if (menu_open_valid()) {
        int32_t x0, x1;
        menu_label_rect(menu_open_idx, &x0, &x1);
        int n = menu_item_count[menu_open_idx];
        const int32_t ddw = menu_dd_w(menu_open_idx);
        int32_t dy = MENUBAR_H + menu_head_h(menu_open_idx);
        if (mx >= x0 && mx < x0 + ddw &&
            my >= dy && my < dy + n * MENU_ITEM_H) {
            int idx = (my - dy) / MENU_ITEM_H;
            if (idx >= 0 && idx < n && menu_items[menu_open_idx][idx].action >= 0) {
                menu_action(menu_items[menu_open_idx][idx].action);
            }
            menu_open_idx = -1;
            return 1;
        }
        /* A click on the search field is a click *in* the menu, so it
         * must not dismiss it -- that would make the field impossible to
         * aim at. */
        if (mx >= x0 && mx < x0 + ddw &&
            my >= MENUBAR_H && my < dy)
            return 1;
        menu_open_idx = -1;
        /* closing a menu consumes the click, like every other desktop */
        return 1;
    }
    return 0;
}

/* ===== ACTION CENTER =====
 *
 * A flag at the right of the menubar carrying the unread count, and a
 * panel that drops from it. The flag is always drawn, so its quiet state
 * is as legible as its loud one.
 */
#define AC_W       320
#define AC_ROW_H   34
#define AC_HEAD_H  28

static int ac_open = 0;

/*
 * The flag's x is measured, not guessed. The menubar lays its right-hand
 * cluster out right to left from the clock, so where the flag ends up
 * depends on how wide the clock and date happen to set -- a fixed offset
 * put it on top of the date. menubar_draw records the position it
 * actually used and everything else reads it from here.
 */
static int32_t ac_flag_px = 0;

static int32_t ac_flag_x(uint32_t w) {
    return ac_flag_px ? ac_flag_px : (int32_t)w - 200;
}

static int ac_hit_flag(uint32_t w, int32_t mx, int32_t my) {
    const int32_t fx = ac_flag_x(w);
    return my >= 0 && my < MENUBAR_H && mx >= fx - 12 && mx < fx + 12;
}

static int32_t ac_height(void) {
    const int rows = notify_n ? notify_n : 1;
    return AC_HEAD_H + rows * AC_ROW_H + 8;
}

static void ac_draw_flag(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t mx, int32_t my) {
    const int32_t fx = ac_flag_x(w), fy = MENUBAR_H / 2;
    const int hot = ac_hit_flag(w, mx, my) || ac_open;

    /* Highest category still unread decides the colour: an alert must
     * not read the same as a note. */
    uint32_t col = notify_unread ? C_GOLD : 0x555C6Eu;
    for (int i = 0; i < notify_unread && i < notify_n; i++) {
        const notify_t *e = notify_at(i);
        if (e && e->cat == NOTE_WARN) { col = C_RED; break; }
    }
    if (hot) gfx_rect(buf, w, h, fx - 12, 2, 24, MENUBAR_H - 5, 0x252B3Cu);

    /* a flag: staff, and a pennant that is filled only when unread */
    gfx_rect(buf, w, h, fx - 5, fy - 8, 1, 16, col);
    if (notify_unread)
        gfx_tri(buf, w, h, fx - 4, fy - 8, fx + 6, fy - 4, fx - 4, fy, col);
    else
        for (int i = 0; i < 5; i++)
            gfx_rect(buf, w, h, fx - 4 + i, fy - 8 + i / 2, 1, 8 - i, col);

    if (notify_unread) {
        char nb[8];
        uint_to_str((uint32_t)notify_unread, nb);
        ttf_draw_string(buf, (int)w, (int)h, fx + 8, 7, nb, col, 11);
    }
}

static void ac_draw_panel(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!ac_open) return;
    int32_t x = ac_flag_x(w) - AC_W + 60;
    if (x + AC_W > (int32_t)w - 6) x = (int32_t)w - 6 - AC_W;
    if (x < 6) x = 6;
    const int32_t y = MENUBAR_H;
    const int32_t hgt = ac_height();

    gfx_shadow_popup(buf, w, h, x, y, AC_W, hgt);
    gfx_rect_blend(buf, w, h, x, y, AC_W, hgt, 0x12151Fu, 248);
    gfx_rect_outline(buf, w, h, x, y, AC_W, hgt, C_GOLD_DIM);

    ttf_draw_string(buf, (int)w, (int)h, x + 12, y + 7, "ACTION CENTER",
                    C_GOLD_DIM, 10);
    ttf_draw_string(buf, (int)w, (int)h, x + AC_W - 60, y + 7,
                    "Clear", C_TEXT_DIM, 11);
    gfx_rect(buf, w, h, x + 10, y + AC_HEAD_H - 4, AC_W - 20, 1, 0x2A3142u);

    if (notify_n == 0) {
        ttf_draw_string(buf, (int)w, (int)h, x + 14, y + AC_HEAD_H + 8,
                        "Nothing needs your attention", C_TEXT_DIM, 12);
        return;
    }

    for (int i = 0; i < notify_n; i++) {
        const notify_t *e = notify_at(i);
        const int32_t ry = y + AC_HEAD_H + i * AC_ROW_H;
        const uint32_t dot = e->cat == NOTE_WARN ? C_RED :
                             e->cat == NOTE_GOOD ? C_GREEN : C_GOLD_DIM;
        gfx_circle(buf, w, h, x + 16, ry + 12, 3, dot);
        ttf_draw_string_clip(buf, (int)w, (int)h, x + 28, ry + 4, e->text,
                             i < notify_unread ? C_TEXT : C_TEXT_DIM, 12,
                             x + AC_W - 52);
        char ts[8], nb[6];
        uint_to_str((uint32_t)e->hh, nb);
        str_copy(ts, e->hh < 10 ? "0" : "", sizeof(ts));
        str_append(ts, nb, sizeof(ts));
        str_append(ts, ":", sizeof(ts));
        uint_to_str((uint32_t)e->mm, nb);
        if (e->mm < 10) str_append(ts, "0", sizeof(ts));
        str_append(ts, nb, sizeof(ts));
        ttf_draw_string(buf, (int)w, (int)h, x + AC_W - 44, ry + 5, ts,
                        0x606878u, 11);
        if (i + 1 < notify_n)
            gfx_rect(buf, w, h, x + 28, ry + AC_ROW_H - 1, AC_W - 44, 1,
                     0x1E2430u);
    }
}

/* Returns 1 if the click belonged to the Action Center. */
static int ac_mouse(uint32_t w, int32_t mx, int32_t my) {
    if (ac_hit_flag(w, mx, my)) {
        ac_open = !ac_open;
        if (ac_open) notify_unread = 0;   /* opening it is reading it */
        return 1;
    }
    if (!ac_open) return 0;

    int32_t x = ac_flag_x(w) - AC_W + 60;
    if (x + AC_W > (int32_t)w - 6) x = (int32_t)w - 6 - AC_W;
    if (x < 6) x = 6;
    const int32_t y = MENUBAR_H, hgt = ac_height();
    if (mx < x || mx >= x + AC_W || my < y || my >= y + hgt) {
        ac_open = 0;
        return 1;                         /* dismissing consumes the click */
    }
    if (my < y + AC_HEAD_H && mx >= x + AC_W - 64) {
        notify_clear();
        ac_open = 0;
    }
    return 1;
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

        x -= 26;
        ac_flag_px = x;
        ac_draw_flag(buf, w, h, mx, my);
    }
}

static void menu_dropdown_draw(uint32_t *buf, uint32_t w, uint32_t h,
                               int32_t mx, int32_t my) {
    if (!menu_open_valid()) return;

    int32_t x0, x1;
    menu_label_rect(menu_open_idx, &x0, &x1);
    int n = menu_item_count[menu_open_idx];
    int32_t dy = MENUBAR_H;

    /* The Apps menu carries a search field. It is always there rather
     * than appearing on the first keystroke, because a field that is
     * invisible until used cannot tell anyone it exists. */
    const int searchable = (menu_open_idx == 1);
    const int32_t head = menu_head_h(menu_open_idx);
    const int32_t ddw  = menu_dd_w(menu_open_idx);
    int32_t dh = head + n * MENU_ITEM_H;
    if (searchable && n == 0) dh = head + MENU_ITEM_H;

    /* A menu is held above the desktop, not painted onto it. This
     * used to be one offset rectangle at a flat alpha, which reads
     * as a second menu behind the first rather than as a shadow. */
    gfx_shadow_popup(buf, w, h, x0, dy, ddw, dh);
    gfx_rect(buf, w, h, x0, dy, ddw, dh, 0x1A1E2Au);
    gfx_rect_outline(buf, w, h, x0, dy, ddw, dh, C_GOLD_DIM);

    if (searchable) {
        gfx_rect(buf, w, h, x0 + 1, dy + 1, ddw - 2, head - 2, 0x12151Fu);
        gfx_rect(buf, w, h, x0 + 10, dy + head - 6, ddw - 20, 1, 0x2E3444u);
        if (search_q_n > 0) {
            ttf_draw_string_clip(buf, (int)w, (int)h, x0 + 14, dy + 6,
                                 search_q, C_TEXT, 13, x0 + ddw - 14);
            /* a caret, so it reads as a field being typed into */
            if ((desktop_tick / 20) & 1)
                gfx_rect(buf, w, h,
                         x0 + 15 + ttf_text_width(search_q, 13), dy + 7,
                         1, 14, C_GOLD);
        } else {
            ttf_draw_string(buf, (int)w, (int)h, x0 + 14, dy + 6,
                            "Search apps and files", 0x5A6070u, 13);
        }
        if (n == 0 && search_q_n > 0)
            ttf_draw_string(buf, (int)w, (int)h, x0 + 14, dy + head + 5,
                            "No matches", C_TEXT_DIM, 13);
    }
    dy += head;

    for (int i = 0; i < n; i++) {
        const menu_item_t *it = &menu_items[menu_open_idx][i];
        int32_t iy = dy + i * MENU_ITEM_H;
        if (it->action < 0) {
            gfx_rect(buf, w, h, x0 + 10, iy + MENU_ITEM_H / 2, ddw - 20,
                     1, 0x2E3444u);
            continue;
        }
        int hot = (mx >= x0 && mx < x0 + ddw &&
                   my >= iy && my < iy + MENU_ITEM_H);
        if (hot)
            gfx_rect(buf, w, h, x0 + 1, iy, ddw - 2, MENU_ITEM_H,
                     0x2A2410u);
        ttf_draw_string(buf, (int)w, (int)h, x0 + 14, iy + 5, it->label,
                        hot ? C_GOLD : C_TEXT, 13);
        /* open-window marker */
        if (it->action >= 0 && it->action < WK_COUNT && wm_is_open(it->action))
            gfx_circle(buf, w, h, x0 + ddw - 12, iy + MENU_ITEM_H / 2,
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
    WK_SYSMON, WK_MATRIX, WK_HELLO, WK_CALC, WK_MEDIA, WK_SOLID, WK_CHIP8,
    WK_CHAMBER, WK_SETTINGS,
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
    int32_t isz = dock_eff_isz;
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

/*
 * The Show Desktop tab: the far end of the bar, past every launcher.
 * dock_recalc has already reserved the length for it, so this is a slice
 * of the bar rather than something hanging off the end of it.
 */
static void dock_showdesk_rect(uint32_t scr_w, int32_t *ox, int32_t *oy,
                               int32_t *ow, int32_t *oh) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(scr_w, &rx, &ry, &rw, &rh);
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        *ox = rx + rw - DOCK_SHOWDESK_W - 3;
        *oy = ry + 4;
        *ow = DOCK_SHOWDESK_W - 3;
        *oh = rh - 8;
    } else {
        *ox = rx + 4;
        *oy = ry + rh - DOCK_SHOWDESK_W - 3;
        *ow = rw - 8;
        *oh = DOCK_SHOWDESK_W - 3;
    }
}

static int dock_hit_showdesk(int32_t mx, int32_t my) {
    int32_t x, y, w, h;
    dock_showdesk_rect(scr_w_cache, &x, &y, &w, &h);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* pictogram icons — everything scales off sz */
static void dock_draw_glyph(uint32_t *buf, uint32_t w, uint32_t h,
                            int kind, int32_t x, int32_t y, int32_t sz) {
    int32_t cx = x + sz / 2;
    int32_t cy = y + sz / 2;
    int32_t q = sz / 4;

    switch (kind) {
    case WK_CHAMBER: {
        /* a box inside a box: the guest, and what contains it */
        gfx_rect_outline(buf, w, h, cx - q - 2, cy - q - 2, 2 * q + 4,
                         2 * q + 4, C_GOLD_DIM);
        gfx_rect(buf, w, h, cx - q / 2, cy - q / 2, q, q, C_GOLD);
        gfx_rect(buf, w, h, cx - q - 2, cy - 1, 3, 2, C_GOLD_DIM);
        gfx_rect(buf, w, h, cx + q, cy - 1, 3, 2, C_GOLD_DIM);
        break;
    }
    case WK_CHIP8: {
        /* a 4x4 keypad, which is what the machine had */
        for (int r = 0; r < 4; r++)
            for (int c2 = 0; c2 < 4; c2++)
                gfx_rect(buf, w, h, cx - q + c2 * (q / 2), cy - q + r * (q / 2),
                         q / 2 - 2, q / 2 - 2,
                         ((r + c2) & 1) ? C_GOLD : C_GOLD_DIM);
        break;
    }
    case WK_SOLID: {
        /* a wireframe cube in two-point projection */
        const int32_t o = q / 2;
        gfx_rect_outline(buf, w, h, cx - q, cy - q + o, q * 2 - o, q * 2 - o, C_GOLD);
        gfx_rect_outline(buf, w, h, cx - q + o, cy - q, q * 2 - o, q * 2 - o, C_GOLD_DIM);
        gfx_line(buf, w, h, cx - q, cy - q + o, cx - q + o, cy - q, 1, C_GOLD_DIM);
        gfx_line(buf, w, h, cx + q - o, cy + q, cx + q, cy + q - o, 1, C_GOLD_DIM);
        break;
    }
    case WK_MEDIA: {
        /* a speaker: box, cone, and two arcs of sound */
        gfx_rect(buf, w, h, cx - q - 2, cy - q / 2, q, q, C_GOLD);
        gfx_tri(buf, w, h, cx - 2, cy - q, cx - 2, cy + q, cx - q - 2, cy, C_GOLD);
        for (int r = 1; r <= 2; r++)
            gfx_circle_outline(buf, w, h, cx - 1, cy, q / 2 + r * (q / 3),
                               C_GOLD_DIM);
        break;
    }
    case WK_CALC: {
        /* a keypad: the outline of the case and a four-by-four of keys */
        gfx_rect_outline(buf, w, h, cx - q - 2, cy - q * 2 + 2,
                         q * 2 + 4, q * 4 - 4, C_GOLD);
        gfx_rect(buf, w, h, cx - q, cy - q * 2 + 4, q * 2, q - 1, C_GOLD_DIM);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
                gfx_rect(buf, w, h, cx - q + c * (q * 2 / 3), cy + r * (q - 1),
                         q / 2, q / 2, C_GOLD);
        break;
    }
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
/* Which window a taskbar slot stands for, or -1 if nothing is running in
 * it. Canvas apps all share WK_HELLO, so the one actually loaded is
 * identified by the window title rather than by kind. */
static int dock_running_kind(int idx) {
    const int kind = dock_items[idx].kind;
    if (kind == WK_HELLO)
        return (wins[WK_HELLO].open && str_eq(app_win_title, dock_item_name(idx)))
               ? WK_HELLO : -1;
    return wm_is_open(kind) ? kind : -1;
}

static int dock_hit_item(int32_t mx, int32_t my) {
    for (int i = 0; i < dock_item_count; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(scr_w_cache, i, &ix, &iy, &iw, &ih);
        if (mx >= ix - 4 && mx < ix + iw + 4 &&
            my >= iy - 4 && my < iy + ih + 4)
            return i;
    }
    return -1;
}

static int dock_hit_bar(int32_t mx, int32_t my) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(scr_w_cache, &rx, &ry, &rw, &rh);
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

static int dock_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb) {
    int click = (lmb && !prev_lmb);
    if (!click) return 0;
    if (!dock_hit_bar(mx, my)) return 0;

    /* Show Desktop is a latch, so the second click is what puts the
     * stack back -- there is no hover to walk away from. */
    if (dock_hit_showdesk(mx, my)) {
        aero_peek_hold = !aero_peek_hold;
        return 1;
    }

    const int i = dock_hit_item(mx, my);
    if (i >= 0) {
        /*
         * A taskbar button is a toggle once its window exists: click the
         * focused one to put it away, click it again to bring it back.
         * Only an idle slot actually launches anything.
         */
        const int k = dock_running_kind(i);
        if (k < 0)                      dock_launch(i);
        else if (wins[k].min)           wm_unminimize(k);
        else if (wm_focus == k)         wm_minimize(k);
        else                            wm_raise(k);
        return 1;
    }
    return 1;   /* clicks on the bar background are still consumed */
}

/* ===== JUMP LISTS =====
 *
 * Right-click a taskbar button and it opens upward: the things that app
 * was last pointed at, then the actions that apply to it. Recent items
 * come from recents[] in shell.h, which the apps push into as they go, so
 * the list is what actually happened rather than a guess.
 *
 * One list is open at a time and it is identified by the taskbar slot,
 * not the window kind -- the canvas apps all share one kind, and a jump
 * list per app is the point of having one at all.
 */
#define JL_ROW_H    24
#define JL_HEAD_H   22
#define JL_W       248

static int jl_open = -1;        /* dock item index, or -1 */

static int jl_action_count(int idx) {
    return dock_running_kind(idx) >= 0 ? 2 : 1;   /* +close when running */
}

/*
 * dock_rebuild only ever stores valid window kinds, but that invariant is
 * not visible to the compiler through the inlined lookup, and an
 * unchecked recents[kind] indexed off it trips -Warray-bounds. One
 * accessor states the invariant once instead of four unchecked reads.
 */
static int jl_kind(int idx) {
    if (idx < 0 || idx >= dock_item_count) return -1;
    const int k = dock_items[idx].kind;
    return (k >= 0 && k < WK_COUNT) ? k : -1;
}

static int jl_recent_count(int idx) {
    const int k = jl_kind(idx);
    return k < 0 ? 0 : recent_n[k];
}

static void jl_rect(int idx, int32_t *ox, int32_t *oy,
                    int32_t *ow, int32_t *oh) {
    const int nrec = jl_recent_count(idx);
    const int nact = jl_action_count(idx);

    int32_t hgt = JL_HEAD_H + nact * JL_ROW_H + 8;
    if (nrec) hgt += JL_HEAD_H + nrec * JL_ROW_H;

    int32_t ix, iy, iw, ih;
    dock_icon_rect(scr_w_cache, idx, &ix, &iy, &iw, &ih);

    *ow = JL_W;
    *oh = hgt;
    if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
        *ox = ix + iw / 2 - JL_W / 2;
        *oy = iy - hgt - 10;
    } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
        *ox = ix + iw + 10;
        *oy = iy + ih / 2 - hgt / 2;
    } else {
        *ox = ix - JL_W - 10;
        *oy = iy + ih / 2 - hgt / 2;
    }
    if (*ox < 4) *ox = 4;
    if (*ox + *ow > (int32_t)scr_w_cache - 4) *ox = (int32_t)scr_w_cache - 4 - *ow;
    if (*oy < MENUBAR_H + 4) *oy = MENUBAR_H + 4;
}

/*
 * Rows are numbered top to bottom across both sections: 0..nrec-1 are the
 * recent items, then the actions. Returning one index for the whole list
 * keeps the hit test and the drawing walking the same arithmetic.
 */
static int jl_row_at(int idx, int32_t mx, int32_t my) {
    int32_t x, y, w2, h2;
    jl_rect(idx, &x, &y, &w2, &h2);
    if (mx < x || mx >= x + w2 || my < y || my >= y + h2) return -1;

    const int nrec = jl_recent_count(idx);
    int32_t ry2 = y;

    if (nrec) {
        ry2 += JL_HEAD_H;
        for (int i = 0; i < nrec; i++, ry2 += JL_ROW_H)
            if (my >= ry2 && my < ry2 + JL_ROW_H) return i;
    }
    ry2 += JL_HEAD_H;
    for (int a = 0; a < jl_action_count(idx); a++, ry2 += JL_ROW_H)
        if (my >= ry2 && my < ry2 + JL_ROW_H) return nrec + a;
    return -1;
}

static void jl_draw(uint32_t *buf, uint32_t w, uint32_t h,
                    int32_t mx, int32_t my) {
    if (jl_open < 0 || jl_open >= dock_item_count) return;

    int32_t x, y, w2, h2;
    jl_rect(jl_open, &x, &y, &w2, &h2);
    const int kind = jl_kind(jl_open);
    const int nrec = jl_recent_count(jl_open);
    const int hot  = jl_row_at(jl_open, mx, my);
    if (kind < 0) return;

    gfx_shadow_popup(buf, w, h, x, y, w2, h2);
    gfx_rect_blend(buf, w, h, x, y, w2, h2, 0x12151Fu, 246);
    gfx_rect_outline(buf, w, h, x, y, w2, h2, C_GOLD_DIM);

    int32_t ry2 = y;
    if (nrec) {
        ttf_draw_string(buf, (int)w, (int)h, x + 10, ry2 + 4, "RECENT",
                        C_GOLD_DIM, 10);
        ry2 += JL_HEAD_H;
        for (int i = 0; i < nrec; i++, ry2 += JL_ROW_H) {
            if (hot == i)
                gfx_rect(buf, w, h, x + 1, ry2, w2 - 2, JL_ROW_H, 0x232A3Cu);
            ttf_draw_string_clip(buf, (int)w, (int)h, x + 14, ry2 + 5,
                                 recents[kind][i].label, C_TEXT, 12,
                                 x + w2 - 10);
        }
        gfx_rect(buf, w, h, x + 8, ry2 + 2, w2 - 16, 1, 0x2A3142u);
    }

    ttf_draw_string(buf, (int)w, (int)h, x + 10, ry2 + 4,
                    dock_item_name(jl_open), C_GOLD_DIM, 10);
    ry2 += JL_HEAD_H;

    static const char *const act_open  = "Open";
    static const char *const act_close = "Close window";
    for (int a = 0; a < jl_action_count(jl_open); a++, ry2 += JL_ROW_H) {
        if (hot == nrec + a)
            gfx_rect(buf, w, h, x + 1, ry2, w2 - 2, JL_ROW_H, 0x232A3Cu);
        ttf_draw_string(buf, (int)w, (int)h, x + 14, ry2 + 5,
                        a == 0 ? act_open : act_close,
                        a == 0 ? C_TEXT : C_TEXT_DIM, 12);
    }
}

static int jl_click(int32_t mx, int32_t my) {
    if (jl_open < 0) return 0;
    const int idx = jl_open;
    const int row = jl_row_at(idx, mx, my);
    jl_open = -1;                     /* any click closes it */
    if (row < 0) return 1;            /* ...including one that misses */

    const int kind = jl_kind(idx);
    const int nrec = jl_recent_count(idx);
    if (kind >= 0 && row < nrec) {
        desktop_open_recent(kind, recents[kind][row].path);
    } else if (row == nrec) {
        dock_launch(idx);
    } else {
        const int k = dock_running_kind(idx);
        if (k >= 0) wm_close(k);
    }
    return 1;
}

static void dock_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t mx, int32_t my) {
    int32_t rx, ry, rw, rh;
    dock_bar_rect(w, &rx, &ry, &rw, &rh);

    /* translucent plate, over a shadow of its own -- it floats above
     * the wallpaper like everything else that is not the desktop. */
    gfx_shadow_popup(buf, w, h, rx, ry, rw, rh);
    gfx_rect_blend(buf, w, h, rx, ry, rw, rh, C_BG_PANEL, 215);
    gfx_rect_outline(buf, w, h, rx, ry, rw, rh, 0x2E3444u);
    gfx_rect(buf, w, h, rx, ry, rw, 1, 0x3A4254u);

    int hover = -1;
    for (int i = 0; i < dock_item_count; i++) {
        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, i, &ix, &iy, &iw, &ih);
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

        /* Running indicator: a full dot for a window on the desktop, a
         * hollow one for a window that has been put away. */
        const int rk = dock_running_kind(i);
        if (rk >= 0) {
            int32_t dx, dy;
            if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
                dx = ix + iw / 2;      dy = ry + rh - 4;
            } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
                dx = rx + rw - 4;      dy = iy + ih / 2;
            } else {
                dx = rx + 4;           dy = iy + ih / 2;
            }
            if (wins[rk].min) gfx_circle_outline(buf, w, h, dx, dy, 3, C_GOLD_DIM);
            else              gfx_circle(buf, w, h, dx, dy, 2, C_GOLD);
        }
    }

    /*
     * Show Desktop. Lit while the latch is set, so the state of Peek is
     * visible from the thing that controls it -- with no hover trigger,
     * a user who cannot see the latch has no way to know why the stack
     * went transparent.
     */
    {
        int32_t sx, sy, sw, sh;
        dock_showdesk_rect(w, &sx, &sy, &sw, &sh);
        const int shot = (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh);
        gfx_rect(buf, w, h, sx, sy, sw, sh,
                 aero_peek_hold ? 0x30301Cu : (shot ? 0x262C3Eu : 0x1C2130u));
        gfx_rect_outline(buf, w, h, sx, sy, sw, sh,
                         aero_peek_hold ? C_GOLD : 0x2A3040u);
        /* a screen, drawn small: the thing the click reveals */
        const int32_t gw = (sw > sh ? sh : sw) - 6;
        if (gw >= 5) {
            gfx_rect_outline(buf, w, h, sx + (sw - gw) / 2,
                             sy + (sh - gw) / 2, gw, gw,
                             aero_peek_hold ? C_GOLD : C_TEXT_DIM);
        }
    }

    /*
     * Hover shows a preview: the window as it actually looks if one is
     * running, and just the name if the slot is only a launcher. The
     * thumbnail is whatever the compositor last captured, which is why a
     * minimized window still has a picture to show.
     */
    if (hover >= 0 && hover != jl_open) {
        const char *name = dock_item_name(hover);
        const int rk = dock_running_kind(hover);

        /*
         * Where the picture comes from.
         *
         * For the application button it comes from the running process's
         * own surface, downscaled by the compositor a few frames ago,
         * and not from the back buffer -- an application running behind
         * another one has no pixels on screen to capture, and before
         * private surfaces there was nothing else to read. For every
         * other window it is still the screen capture, because those
         * windows are drawn by this file and have no surface of their
         * own to read instead.
         */
        const uint32_t *thumb = 0;
        if (rk == WK_HELLO) {
            for (int i = 0; i < APP_SURF_MAX; i++) {
                if (&app_surf[i] != app_surf_front) continue;
                if (aero_surf_thumb_valid[i]) thumb = aero_surf_thumb[i];
                break;
            }
        }
        if (!thumb && rk >= 0 && wm_thumb_valid[rk]) thumb = wm_thumb[rk];

        const int show_thumb = (thumb != 0);
        const int pad = 6;
        const int32_t pw = show_thumb ? THUMB_W + 2 * pad
                                      : ttf_text_width(name, 12) + 16;
        const int32_t ph = show_thumb ? THUMB_H + 2 * pad + 20 : 22;

        int32_t ix, iy, iw, ih;
        dock_icon_rect(w, hover, &ix, &iy, &iw, &ih);

        int32_t tx, ty;
        if (dock_cfg.edge == DOCK_EDGE_BOTTOM) {
            tx = ix + iw / 2 - pw / 2;
            ty = ry - ph - 8;
        } else if (dock_cfg.edge == DOCK_EDGE_LEFT) {
            tx = rx + rw + 8;
            ty = iy + ih / 2 - ph / 2;
        } else {
            tx = rx - pw - 8;
            ty = iy + ih / 2 - ph / 2;
        }
        if (tx < 4) tx = 4;
        if (tx + pw > (int32_t)w - 4) tx = (int32_t)w - 4 - pw;
        if (ty < MENUBAR_H + 4) ty = MENUBAR_H + 4;

        gfx_rect_blend(buf, w, h, tx, ty, pw, ph, 0x1A1E2Au, 240);
        gfx_rect_outline(buf, w, h, tx, ty, pw, ph, C_GOLD_DIM);

        if (show_thumb) {
            /* Blended rather than copied, and at an alpha just short of
             * opaque: the preview reads as a pane of the same glass the
             * window frames are made of rather than a photograph pasted
             * over the desktop. It is already at the size it is drawn,
             * so there is no resample here either way. */
            for (int32_t r = 0; r < THUMB_H; r++) {
                const int32_t yy = ty + pad + r;
                if (yy < 0 || yy >= (int32_t)h) continue;
                for (int32_t c = 0; c < THUMB_W; c++) {
                    const int32_t xx = tx + pad + c;
                    if (xx < 0 || xx >= (int32_t)w) continue;
                    uint32_t *d = &buf[(uint32_t)yy * w + (uint32_t)xx];
                    *d = gfx_mix(thumb[(uint32_t)r * THUMB_W + (uint32_t)c],
                                 *d, 232);
                }
            }
            gfx_rect_outline(buf, w, h, tx + pad, ty + pad,
                             THUMB_W, THUMB_H, 0x333A4Cu);
            ttf_draw_string_clip(buf, (int)w, (int)h, tx + pad,
                                 ty + pad + THUMB_H + 3, name, C_TEXT, 12,
                                 tx + pw - pad);
        } else {
            ttf_draw_string(buf, (int)w, (int)h, tx + 8, ty + 3,
                            name, C_TEXT, 12);
        }
    }
}

/*
 * Reopen something off a jump list.
 *
 * Each app already knows how to be pointed at a thing; this only picks
 * which one to ask, and raises its window so the result is visible. Kinds
 * with nothing meaningful to reopen just come to the front.
 */
static void desktop_open_recent(int kind, const char *path) {
    if (!path || !path[0]) return;
    switch (kind) {
    case WK_BROWSER: wm_open(WK_BROWSER); brw_navigate(path);      break;
    case WK_WIKI:    wm_open(WK_WIKI);    wiki_load(path, 0);      break;
    case WK_IMAGE:   wm_open(WK_IMAGE);   img_open_path(path);     break;
    case WK_FILES:
        wm_open(WK_FILES);
        str_copy(exp_path, path, EXP_PATH_MAX);
        exp_scan();
        break;
    default: wm_open(kind); break;
    }
}

/* ===== 10. RENDER GLUE ===== */

static uint8_t desk_prev_lmb = 0;
static uint8_t desk_prev_rmb = 0;

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
    else if (str_eq(name, "store") || str_eq(name, "ingot") ||
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
    dim_wake();

    /*
     * An elevation question takes the keyboard as completely as it takes
     * the pointer, and for the same reason: while it is up, nothing
     * behind it is live, and a keystroke that reached an application
     * would be a keystroke that reached a program which is supposed to
     * be frozen.
     *
     * Return allows and Escape refuses, which are the answers those two
     * keys give to every other dialog in this system. Anything else is
     * swallowed rather than passed on -- there is no third answer, and
     * letting a stray key through would be the one way to type into a
     * window that is not accepting input.
     */
    if (uac_req.pending) {
        if (ch == '\n' || ch == '\r') uac_answer(1);
        else if (ch == 27)            uac_answer(0);
        return;
    }

    if (menu_open_valid()) {
        /*
         * With the Apps menu open the keyboard belongs to its search
         * field. Escape backs out one step at a time -- it clears a query
         * first and only closes the menu once there is nothing to clear,
         * so a mistyped search does not cost the menu as well.
         */
        if (ch == 27) {
            if (menu_open_idx == 1 && search_q_n > 0) search_clear();
            else menu_open_idx = -1;
            return;
        }
        if (menu_open_idx != 1) return;

        if (ch == '\b') {
            if (search_q_n > 0) {
                search_q[--search_q_n] = '\0';
                search_run();
            }
            return;
        }
        if (ch == '\n') {
            /* Enter takes the first result, which is what a search box
             * is for -- type three letters and press return. */
            if (menu_apps_n > 0 && menu_apps[0].action >= 0) {
                const int a = menu_apps[0].action;
                menu_open_idx = -1;
                search_clear();
                menu_action(a);
            }
            return;
        }
        if (ch >= ' ' && ch < 127 && search_q_n < SEARCH_Q_MAX - 1) {
            search_q[search_q_n++] = ch;
            search_q[search_q_n] = '\0';
            search_run();
        }
        return;
    }
    if (wm_focus == WK_MEDIA) { media_key(ch); return; }
    if (wm_focus == WK_SOLID) { solid_key(ch); return; }
    if (wm_focus == WK_CHAMBER) { chamber_key(ch); return; }
    if (wm_focus == WK_CHIP8) { c8_app_key(ch); return; }
    if (wm_focus == WK_CALC) {
        /* The keypad and the keyboard are the same machine, so the
         * calculator takes the raw character and decides itself. */
        calc_key(ch);
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
                    "It loads a model from the volume at every boot. You can leave it off.",
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

/*
 * ===== THE ELEVATION PROMPT =====
 *
 * The other half of uac_guard: a question drawn over everything, while
 * the thread that asked it is frozen in a system call.
 *
 * It is drawn last in the frame and it takes every click, which is the
 * whole of what makes it a *secure* prompt rather than a dialog. A
 * program cannot draw over it, because a program draws into its own
 * offscreen surface and this is composited afterwards. A program cannot
 * click it, because there is no system call that moves the pointer or
 * synthesises a button. And a program cannot outlast it, because the
 * only thread that could ask a second question is already asleep
 * waiting for the answer to the first.
 *
 * The whole frame behind it is halved in brightness rather than
 * blurred — the same treatment the model opt-in uses, for the same
 * reason: it says unambiguously that nothing behind it is live.
 */
#define UAC_W 500
#define UAC_H 224

static int uac_prompt_hit(int32_t mx, int32_t my, uint32_t w, uint32_t h,
                          int which) {
    int32_t x0 = ((int32_t)w - UAC_W) / 2;
    int32_t y0 = ((int32_t)h - UAC_H) / 2;
    int32_t by = y0 + UAC_H - 52;
    int32_t bx = which == 0 ? x0 + UAC_W - 250 : x0 + UAC_W - 126;
    return mx >= bx && mx < bx + 110 && my >= by && my < by + 34;
}

static void uac_prompt_draw(uint32_t *buf, uint32_t w, uint32_t h,
                            int32_t mx, int32_t my) {
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t p = buf[i];
        buf[i] = ((p >> 1) & 0x7F7F7Fu);
    }

    int32_t x0 = ((int32_t)w - UAC_W) / 2;
    int32_t y0 = ((int32_t)h - UAC_H) / 2;

    gfx_shadow_popup(buf, w, h, x0, y0, UAC_W, UAC_H);
    gfx_rect(buf, w, h, x0, y0, UAC_W, UAC_H, 0x14161Eu);
    gfx_rect_outline(buf, w, h, x0, y0, UAC_W, UAC_H, C_GOLD);
    gfx_rect(buf, w, h, x0, y0, UAC_W, 2, C_GOLD);

    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 20,
                    "A program is asking for permission", C_GOLD, 17);

    /* The program, then what it wants, then the thing it wants it on.
     * In that order because it is the order the question is asked in,
     * and each line is clipped rather than wrapped -- a path that runs
     * off the end of the box is still readable at its start, which is
     * the part that says which directory it is in. */
    {
        char line[UAC_DETAIL_LEN + 48];
        str_copy(line, uac_req.program, sizeof(line));
        str_append(line, " wants to ", sizeof(line));
        str_append(line, uac_req.op, sizeof(line));
        ttf_draw_string_clip(buf, (int)w, (int)h, x0 + 24, y0 + 60, line,
                             C_TEXT, 14, x0 + UAC_W - 24);
    }
    ttf_draw_string_clip(buf, (int)w, (int)h, x0 + 24, y0 + 86,
                         uac_req.detail, C_GOLD_DIM, 13, x0 + UAC_W - 24);

    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 118,
                    "It was started from an administrator account, so this",
                    C_TEXT_DIM, 12);
    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 136,
                    "can be allowed. It has not been allowed anything yet.",
                    C_TEXT_DIM, 12);
    ttf_draw_string(buf, (int)w, (int)h, x0 + 24, y0 + 158,
                    "Allowing it lasts until the program exits. "
                    "Return allows, Escape refuses.",
                    0x707888u, 12);

    for (int i = 0; i < 2; i++) {
        int32_t by = y0 + UAC_H - 52;
        int32_t bx = i == 0 ? x0 + UAC_W - 250 : x0 + UAC_W - 126;
        int hot = uac_prompt_hit(mx, my, w, h, i);
        int yes = (i == 1);
        gfx_rect(buf, w, h, bx, by, 110, 34, yes ? 0x2A2410u : 0x1B1E26u);
        gfx_rect_outline(buf, w, h, bx, by, 110, 34,
                         hot ? C_GOLD : (yes ? C_GOLD_DIM : 0x3A4050u));
        const char *lbl = yes ? "Allow" : "Don't allow";
        int tw = ttf_text_width(lbl, 14);
        ttf_draw_string(buf, (int)w, (int)h, bx + (110 - tw) / 2, by + 9,
                        lbl, yes ? C_GOLD : C_TEXT_DIM, 14);
    }
}

/*
 * Peek: fade every window towards what is behind it, then draw their
 * outlines back on, so the desktop is visible but the stack is not lost.
 *
 * Blending the wallpaper over the whole frame is what makes this both
 * cheap and correct: over a window pixel it is the fade, and over a
 * wallpaper pixel it is the identity, so overlapping windows cannot fade
 * twice the way a per-window pass would let them. One full-screen blend,
 * and only while the pointer is actually on the taskbar.
 */
static void aero_peek_draw(uint32_t *buf, uint32_t w, uint32_t h) {
    if (aero_peek <= 0) return;
    const uint32_t a = (uint32_t)(aero_peek * PEEK_ALPHA / PEEK_RAMP);
    gfx_blend_region(buf, wallpaper, w, h, 0, 0, (int32_t)w, (int32_t)h, a);
    for (int i = 0; i < wm_stack_n; i++) {
        const int kind = wm_stack[i];
        if (wins[kind].min) continue;
        const win_t *win = &wins[kind];
        gfx_rect_blend(buf, w, h, win->x, win->y, win->w, 1, C_GOLD, a);
        gfx_rect_blend(buf, w, h, win->x, win->y + win->h - 1, win->w, 1, C_GOLD, a);
        gfx_rect_blend(buf, w, h, win->x, win->y, 1, win->h, C_GOLD, a);
        gfx_rect_blend(buf, w, h, win->x + win->w - 1, win->y, 1, win->h, C_GOLD, a);
    }
    /* The gadgets are desktop, not window: blending the bare wallpaper
     * over the frame had been erasing them along with the stack, so the
     * one thing Peek exists to show was the thing it hid. Redrawing them
     * here puts them back on top of the faded windows. */
    gadgets_draw(buf, w, h);
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

    /*
     * Wallpaper slideshow. desktop_tick counts frames at 60 Hz, so the
     * interval is in frames here; the comparison is unsigned subtraction
     * so it stays correct across the tick counter wrapping.
     */
    if (wall_slide > 0 && wall_slide < WALL_SLIDE_COUNT) {
        const uint32_t period = (uint32_t)wall_slide_secs[wall_slide] * 60u;
        if (desktop_tick - wall_slide_last >= period) {
            wall_slide_last = desktop_tick;
            wall_theme = (wall_theme + 1) % WALL_THEME_COUNT;
            wallpaper_set_theme(wall_theme);
        }
    }

    /*
     * Link state, watched here because nothing else notices it changing.
     * Only transitions are reported; the state itself is already on the
     * menubar and in the Network gadget.
     */
    {
        static int link_was = -1;
        const int link_now = e1000_found &&
            (e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0;
        if (link_was >= 0 && link_now != link_was)
            notify_push(link_now ? NOTE_GOOD : NOTE_WARN,
                        link_now ? "Network cable connected"
                                 : "Network cable disconnected");
        link_was = link_now;
    }

    /* async engines */
    term_async_poll();
    brw_poll();
    store_poll();
    ai_poll();
    wiki_poll();
    /*
     * Generation is background work and belongs in the frame loop, not
     * in a window's paint routine. It used to be driven from wiki_draw,
     * so an answer only advanced while the Wikipedia window was actually
     * being painted: ask from the shell and the window is never opened,
     * minimise it and the window stops being drawn, and either way the
     * question sat on "reading the question" forever with the desktop
     * idling at 60 fps beside it. Nothing about producing an answer
     * depends on it being on screen.
     */
    wiki_gen_poll();

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
                notify_push(NOTE_INFO, "Language model enabled; loading weights");
            } else if (ai_dialog_hit(mx, my, w, h, 0)) {
                ai_choice_save(0);
                serial_puts("[ai] declined; model not loaded\n");
                notify_push(NOTE_INFO, "Language model left off");
            }
        }
        return;                      /* nothing else runs while it is up */
    }

    /*
     * ---- an elevation question, while one is up ----
     *
     * Before any other input is looked at and instead of the rest of
     * the frame, exactly as the model opt-in above does. That is what
     * makes the prompt take every click: there is no path from here to
     * the window manager, the dock or an application while it is up, so
     * a click cannot land anywhere but on one of the two buttons.
     *
     * The desktop is still drawn underneath — halved in brightness by
     * the prompt itself — so the person can see which program the
     * question is about. It is drawn from the last frame's state and
     * nothing in it is live.
     */
    if (uac_req.pending) {
        uint8_t lmb0 = buttons & 1;
        int click = lmb0 && !desk_prev_lmb;
        desk_prev_lmb = lmb0;

        for (uint32_t i = 0; i < w * h; i++) buf[i] = wallpaper[i];
        gadgets_draw(buf, w, h);
        wm_draw_all(buf, w, h);
        menubar_draw(buf, w, h, mx, my);
        dock_draw(buf, w, h, mx, my);
        uac_prompt_draw(buf, w, h, mx, my);

        if (click) {
            if (uac_prompt_hit(mx, my, w, h, 1))      uac_answer(1);
            else if (uac_prompt_hit(mx, my, w, h, 0)) uac_answer(0);
        }
        return;
    }

    /* ---- input ---- */
    /* Anything the hand does counts as presence. Comparing against the
     * last frame rather than watching for events, because this is the
     * only place both the pointer and the buttons are visible. */
    {
        static int32_t last_mx = -1, last_my = -1;
        static uint8_t last_btn = 0;
        if (mx != last_mx || my != last_my || buttons != last_btn) {
            dim_wake();
            last_mx = mx; last_my = my; last_btn = buttons;
        }
    }

    uint8_t lmb = buttons & 1;
    uint8_t rmb = (buttons >> 1) & 1;

    /* Right-click on a taskbar button opens its jump list, and on the
     * same button again closes it. Anywhere else dismisses. */
    if (rmb && !desk_prev_rmb) {
        const int i = dock_hit_bar(mx, my) ? dock_hit_item(mx, my) : -1;
        jl_open = (i >= 0 && i != jl_open) ? i : -1;
    }
    desk_prev_rmb = rmb;

    /* An open jump list takes the click before anything underneath it. */
    int consumed = 0;
    if (jl_open >= 0 && lmb && !desk_prev_lmb)
        consumed = jl_click(mx, my);
    if (!consumed && lmb && !desk_prev_lmb)
        consumed = ac_mouse(w, mx, my);
    if (!consumed)
        consumed = menu_mouse(mx, my, lmb, desk_prev_lmb);
    if (!consumed)
        consumed = dock_mouse(mx, my, lmb, desk_prev_lmb);
    wm_update(mx, my, lmb, desk_prev_lmb, consumed);
    desk_prev_lmb = lmb;

    /*
     * Peek chases its latch, on a ramp in both directions so it fades
     * rather than snaps. Not while a drag is in progress: dragging a
     * window down to the taskbar is how you move it, and having the
     * desktop dissolve underneath at that moment would be the opposite
     * of helpful. Nothing here reads the pointer -- see aero_peek_hold.
     */
    {
        if (wm_stack_n == 0) aero_peek_hold = 0;
        const int want = aero_peek_hold && wm_drag < 0 && wm_stack_n > 0;
        aero_peek += want ? 1 : -1;
        if (aero_peek < 0)         aero_peek = 0;
        if (aero_peek > PEEK_RAMP) aero_peek = PEEK_RAMP;
    }

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
    /*
     * The clear, and the one place in the frame where handing work to
     * the engine needs no reasoning about order at all: it is the first
     * write to the back buffer this frame, so there is nothing it can
     * land on top of. It is also the largest single operation in the
     * frame by a wide margin — two million pixels at 1920x1080, against
     * a quarter of a million for the biggest window.
     *
     * The wallpaper is a static kernel array like the surfaces are, so
     * it is mapped into the compositor's GGTT window once at boot and
     * the copy is one command. Where there is no engine this is the
     * loop it always was.
     */
    aero_gpu_batches = 0;
    aero_gpu_ops     = 0;
    aero_gpu_reset();
    igpu_comp_begin();

    if (!aero_wallpaper_gpu(w, h)) {
        for (uint32_t i = 0; i < w * h; i++)
            buf[i] = wallpaper[i];
    }

    aero_surface_previews();          /* live, straight from each process */

    aero_gpu_barrier(buf, w, 0, 0, (int32_t)w, (int32_t)h);
    gadgets_draw(buf, w, h);          /* on the desktop, under everything */
    aero_snap_preview(buf, w, h);     /* under the windows: it is a target */
    wm_draw_all(buf, w, h);
    wm_anim_draw(buf, w, h);
    aero_peek_draw(buf, w, h);        /* over the stack, under the chrome */
    menubar_draw(buf, w, h, mx, my);
    menu_dropdown_draw(buf, w, h, mx, my);
    ac_draw_panel(buf, w, h);
    dock_draw(buf, w, h, mx, my);
    jl_draw(buf, w, h, mx, my);       /* over the taskbar it belongs to */

    /* Last of all, so it dims the finished frame including the chrome --
     * a menubar left at full brightness over a dimmed desktop would look
     * like a fault rather than a setting. */
    {
        const uint32_t d = dim_step();
        if (d) gfx_rect_blend(buf, w, h, 0, 0, (int32_t)w, (int32_t)h,
                              0x000000u, d);
    }
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
    /* every window shut, nothing focused, nothing remembered about where
     * it used to be */
    for (int k = 0; k < WK_COUNT; k++) {
        wins[k].open = 0;
        wins[k].min = 0;
        wins[k].snap = SNAP_NONE;
        wins[k].have_rest = 0;
    }
    wm_stack_n = 0;
    wm_focus = -1;
    wm_drag = -1;
    menu_open_idx = -1;

    /* The taskbar previews are pictures of the last person's screen --
     * a browser page, a document, a terminal. They do not survive a
     * logout any more than the scrollback does. */
    for (int k = 0; k < WK_COUNT; k++) {
        wm_thumb_valid[k] = 0;
        for (int p = 0; p < THUMB_W * THUMB_H; p++) wm_thumb[k][p] = 0;
    }
    aero_peek = 0;
    aero_peek_hold = 0;
    aero_snap_hint = SNAP_NONE;
    aero_shake_reset();
    jl_open = -1;
    ac_open = 0;
    notify_clear();
    recent_clear_all();
    calc_clear();

    /* terminal: history, scrollback and working directory */
    term_hist_count = 0;
    term_hist_pos = -1;
    term_input_len = 0;
    term_input[0] = '\0';
    term_clear();
    str_copy(term_cwd, "/", sizeof(term_cwd));

    /* The environment names the person who has just left -- their
     * account, their profile path, their Desktop. It goes with the
     * scrollback and the window thumbnails. */
    env_clear();

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

    /*
     * Built here rather than asked for, and that is the upgrade path for
     * every account made before profile.h existed: create_user_profile()
     * is idempotent, so an account that already has a tree costs four
     * stat calls and one that does not gets one on this login. Nothing
     * is copied and /home is left where it is -- the guard treats an
     * account's legacy directory as theirs too, so files written by an
     * older build stay private without being moved.
     */
    char home[PROFILE_PATH_MAX];
    profile_home(name, home, sizeof(home));
    create_user_profile(name);

    /*
     * Bind the session to that tree. The working directory and the file
     * browser were always set here; the environment is new, and it is
     * what a process started from this session inherits.
     */
    profile_bind_env(name);

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

    /*
     * Now that the answer is known, act on it.
     *
     * The boot-time autoload runs before anyone has logged in, when
     * ai_enabled is still -1, so it declines and returns -- correctly,
     * because loading 380 MB on the strength of an answer nobody has
     * given would be the wrong default. But nothing tried again once the
     * answer was read, so the model loaded on the *first* login, when the
     * dialog's Enable button started it, and never on any login after
     * that. The chat panel then sat there offering to answer questions
     * against weights that were never coming.
     */
    if (ai_enabled == 1) ai_autoload_start();
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
