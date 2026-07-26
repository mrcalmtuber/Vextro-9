#ifndef STORE_H
#define STORE_H

/*
 * Agora — the Socrates BSD 9 app store.
 *
 * Two package sources feed one catalog:
 *
 *   1. the shipped repository cached on disk at /store/pkg (indexed by
 *      the table compiled in below, so the storefront exists even on a
 *      volume that was never seeded), and
 *   2. a remote repository fetched over the in-kernel HTTP client — an
 *      index of key:value blocks, with each payload downloaded on demand.
 *
 * "Installing" is a real operation: the ELF64 payload is validated,
 * copied into /apps, and recorded in the registry at /apps/apps.db, so
 * installed apps survive a reboot and show up in the dock and the Apps
 * menu.  Removing deletes the binary and the registry line.
 *
 * Included from desktop.h after term.h/browser.h — it uses the netstack
 * HTTP client and needs brw_loading / term_async to arbitrate for it.
 */

#define STORE_MAX_PKGS   24
#define STORE_MAX_INST   16

#define STORE_ID_MAX     12      /* 8.3 base name + NUL          */
#define STORE_NAME_MAX   28
#define STORE_VER_MAX    10
#define STORE_CAT_MAX    16
#define STORE_DESC_MAX   88
#define STORE_SRC_MAX    128
#define STORE_PATH_MAX   40

/* icon glyphs */
enum {
    SI_PKG = 0,
    SI_FRACTAL,
    SI_ORBIT,
    SI_GRID,
    SI_WAVE,
};

typedef struct {
    char     id[STORE_ID_MAX];
    char     name[STORE_NAME_MAX];
    char     ver[STORE_VER_MAX];
    char     cat[STORE_CAT_MAX];
    char     desc[STORE_DESC_MAX];
    char     src[STORE_SRC_MAX];   /* local path, or http:// url */
    uint32_t size;
    uint8_t  icon;
    uint8_t  remote;
    uint8_t  avail;                /* payload reachable right now */
} store_pkg_t;

typedef struct {
    char    id[STORE_ID_MAX];
    char    name[STORE_NAME_MAX];
    char    ver[STORE_VER_MAX];
    char    path[STORE_PATH_MAX];
    uint8_t icon;
} store_inst_t;

/* ===== SHIPPED CATALOG =====
 * Payload paths are all-lowercase because the FAT32 driver only writes
 * short names: a mixed-case 8.3 name has no room for the case flags. */

typedef struct {
    const char *id, *name, *ver, *cat, *desc, *src;
    uint8_t icon;
} store_shipped_t;

static const store_shipped_t store_shipped[] = {
    { "mandel", "Mandelbrot", "1.0", "Graphics",
      "Escape-time fractal drawn in 16.16 fixed point - no FPU.",
      "/store/pkg/mandel.elf", SI_FRACTAL },
    { "orbit", "Orbit", "1.0", "Simulation",
      "Five bodies under Newtonian gravity, integrated on integers.",
      "/store/pkg/orbit.elf", SI_ORBIT },
    { "life", "Game of Life", "1.1", "Simulation",
      "149x100 torus run for 160 generations over a heat map.",
      "/store/pkg/life.elf", SI_GRID },
    { "plasma", "Plasma", "1.0", "Graphics",
      "Interference field from a sine table built by an oscillator.",
      "/store/pkg/plasma.elf", SI_WAVE },
};

#define STORE_SHIPPED_COUNT \
    ((int)(sizeof(store_shipped) / sizeof(store_shipped[0])))

/* ===== STATE ===== */

static store_pkg_t  store_pkgs[STORE_MAX_PKGS];
static int          store_pkg_count = 0;
static store_inst_t store_inst[STORE_MAX_INST];
static int          store_inst_count = 0;

static int   store_scroll = 0;
static int   store_sel = 0;
static int   store_view_h = 200;
static int   store_sb_drag = 0;
static int   store_sb_off = 0;

static char  store_status[96] = "";
static int   store_status_col = 0;   /* 0 ink, 1 gold, 2 error, 3 ok */

static char  store_repo[STORE_SRC_MAX] = "http://10.0.2.2:8000/index.sr";

#define STORE_NET_IDLE    0
#define STORE_NET_INDEX   1
#define STORE_NET_PAYLOAD 2

static int      store_net = STORE_NET_IDLE;
static int      store_net_pkg = -1;
static int      store_net_term = 0;   /* transfer started from the shell */
static uint32_t store_net_t0 = 0;

static char     store_dbbuf[2560];

#define STORE_CARD_H  86
#define STORE_HEAD_H  60
#define STORE_BAR_H   24
#define STORE_SCROLLW 10

/* ===== SMALL HELPERS ===== */

static void store_say(const char *msg, int col) {
    str_copy(store_status, msg, sizeof(store_status));
    store_status_col = col;
}

static void store_say2(const char *a, const char *b, int col) {
    str_copy(store_status, a, sizeof(store_status));
    str_append(store_status, b, sizeof(store_status));
    store_status_col = col;
}

static void store_app_path(const char *id, char *out /* >= STORE_PATH_MAX */) {
    str_copy(out, "/apps/", STORE_PATH_MAX);
    str_append(out, id, STORE_PATH_MAX);
    str_append(out, ".elf", STORE_PATH_MAX);
}

/* Payload lookup that also sees the ustar ramdisk, so the storefront
 * still works on an ISO-only boot with no hard disk attached. */
static const void *store_read(const char *path, uint64_t *len) {
    if (fat_vol.mounted) {
        const void *d = fs_read_file(path, len);
        if (d) return d;
    }
    return tar_read_file(path, len);
}

static int store_stat(const char *path, uint32_t *size) {
    if (fat_vol.mounted) {
        fat_dirent_t e;
        if (fat_lookup(path, &e) && !(e.attr & FAT_ATTR_DIR)) {
            *size = e.size;
            return 1;
        }
    }
    uint64_t n = 0;
    if (tar_read_file(path, &n) && n > 0) {
        *size = (uint32_t)n;
        return 1;
    }
    return 0;
}

static int store_find_inst(const char *id) {
    for (int i = 0; i < store_inst_count; i++)
        if (str_eq(store_inst[i].id, id)) return i;
    return -1;
}

static int store_icon_from_name(const char *s) {
    if (str_eq(s, "fractal")) return SI_FRACTAL;
    if (str_eq(s, "orbit"))   return SI_ORBIT;
    if (str_eq(s, "grid"))    return SI_GRID;
    if (str_eq(s, "wave"))    return SI_WAVE;
    return SI_PKG;
}

/* An id becomes /apps/<id>.elf, so it has to survive 8.3 encoding. */
static int store_id_ok(const char *id) {
    int n = str_len(id);
    if (n < 1 || n > 8) return 0;
    for (int i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static void store_size_str(uint32_t bytes, char *out /* >= 16 */) {
    char nb[12];
    if (bytes == 0) {
        str_copy(out, "-", 16);
        return;
    }
    if (bytes < 1024) {
        uint_to_str(bytes, nb);
        str_copy(out, nb, 16);
        str_append(out, " B", 16);
        return;
    }
    uint_to_str((bytes + 512) / 1024, nb);
    str_copy(out, nb, 16);
    str_append(out, " KB", 16);
}

/* ===== REGISTRY (/apps/apps.db) ===== */

static void store_registry_save(void) {
    if (!fs_writable()) return;
    int p = 0;
    store_dbbuf[0] = '\0';

    const char *hdr = "# socrates installed apps - id|name|version|icon|path\n";
    while (hdr[p]) { store_dbbuf[p] = hdr[p]; p++; }

    for (int i = 0; i < store_inst_count && p < (int)sizeof(store_dbbuf) - 160;
         i++) {
        char line[160];
        char nb[8];
        str_copy(line, store_inst[i].id, sizeof(line));
        str_append(line, "|", sizeof(line));
        str_append(line, store_inst[i].name, sizeof(line));
        str_append(line, "|", sizeof(line));
        str_append(line, store_inst[i].ver, sizeof(line));
        str_append(line, "|", sizeof(line));
        uint_to_str(store_inst[i].icon, nb);
        str_append(line, nb, sizeof(line));
        str_append(line, "|", sizeof(line));
        str_append(line, store_inst[i].path, sizeof(line));
        str_append(line, "\n", sizeof(line));
        for (int j = 0; line[j]; j++) store_dbbuf[p++] = line[j];
    }
    store_dbbuf[p] = '\0';
    fs_write_file("/apps/apps.db", store_dbbuf, (uint32_t)p);
}

static void store_registry_load(void) {
    store_inst_count = 0;
    uint64_t len = 0;
    const void *d = store_read("/apps/apps.db", &len);
    if (!d || len == 0) return;

    const char *p = (const char *)d;
    uint64_t i = 0;
    while (i < len && store_inst_count < STORE_MAX_INST) {
        char line[200];
        int n = 0;
        while (i < len && p[i] != '\n') {
            if (p[i] != '\r' && n < (int)sizeof(line) - 1) line[n++] = p[i];
            i++;
        }
        if (i < len) i++;           /* consume the newline */
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        /* id|name|ver|icon|path */
        char *field[5];
        int nf = 0;
        field[nf++] = line;
        for (int j = 0; line[j] && nf < 5; j++)
            if (line[j] == '|') {
                line[j] = '\0';
                field[nf++] = line + j + 1;
            }
        if (nf < 5) continue;

        store_inst_t *e = &store_inst[store_inst_count];
        str_copy(e->id, field[0], STORE_ID_MAX);
        str_copy(e->name, field[1], STORE_NAME_MAX);
        str_copy(e->ver, field[2], STORE_VER_MAX);
        e->icon = (uint8_t)store_icon_from_name(field[3]);
        if (field[3][0] >= '0' && field[3][0] <= '9')
            e->icon = (uint8_t)(field[3][0] - '0');
        str_copy(e->path, field[4], STORE_PATH_MAX);
        if (e->id[0]) store_inst_count++;
    }
}

/* ===== CATALOG ===== */

static void store_catalog_local(void) {
    store_pkg_count = 0;
    for (int i = 0; i < STORE_SHIPPED_COUNT && i < STORE_MAX_PKGS; i++) {
        const store_shipped_t *s = &store_shipped[i];
        store_pkg_t *p = &store_pkgs[store_pkg_count++];
        str_copy(p->id, s->id, STORE_ID_MAX);
        str_copy(p->name, s->name, STORE_NAME_MAX);
        str_copy(p->ver, s->ver, STORE_VER_MAX);
        str_copy(p->cat, s->cat, STORE_CAT_MAX);
        str_copy(p->desc, s->desc, STORE_DESC_MAX);
        str_copy(p->src, s->src, STORE_SRC_MAX);
        p->icon = s->icon;
        p->remote = 0;
        p->size = 0;
        p->avail = (uint8_t)store_stat(p->src, &p->size);
    }
}

/* Re-check the disk-backed payloads without disturbing anything the
 * network repository added to the catalog. */
static void store_restat(void) {
    for (int i = 0; i < store_pkg_count; i++)
        if (!store_pkgs[i].remote)
            store_pkgs[i].avail =
                (uint8_t)store_stat(store_pkgs[i].src, &store_pkgs[i].size);
}

/* Resolve a possibly-relative payload reference against the repo URL. */
static void store_resolve_url(const char *ref, char *out, int max) {
    if (str_starts_with(ref, "http://") || str_starts_with(ref, "https://")) {
        str_copy(out, ref, max);
        return;
    }
    /* strip the last path component of the repo url */
    str_copy(out, store_repo, max);
    int cut = -1;
    int slashes = 0;
    for (int i = 0; out[i]; i++) {
        if (out[i] == '/') {
            slashes++;
            if (slashes > 2) cut = i;
        }
    }
    if (cut < 0) {
        int n = str_len(out);
        out[n] = '/';
        out[n + 1] = '\0';
    } else {
        out[cut + 1] = '\0';
    }
    if (ref[0] == '/') ref++;
    str_append(out, ref, max);
}

/* Parse a key:value index into the catalog, keeping the shipped
 * entries.  Returns the number of remote packages accepted. */
static int store_parse_index(const uint8_t *data, int len) {
    store_catalog_local();

    int added = 0;
    store_pkg_t *cur = 0;
    int i = 0;

    while (i < len) {
        char line[256];
        int n = 0;
        while (i < len && data[i] != '\n') {
            if (data[i] != '\r' && n < (int)sizeof(line) - 1)
                line[n++] = (char)data[i];
            i++;
        }
        if (i < len) i++;
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        int colon = -1;
        for (int j = 0; line[j]; j++)
            if (line[j] == ':') { colon = j; break; }
        if (colon <= 0) continue;
        line[colon] = '\0';
        const char *key = line;
        const char *val = line + colon + 1;
        while (*val == ' ') val++;

        if (str_eq(key, "pkg")) {
            cur = 0;
            if (!store_id_ok(val) || store_pkg_count >= STORE_MAX_PKGS)
                continue;
            /* a remote entry with a shipped id updates that entry */
            store_pkg_t *slot = 0;
            for (int k = 0; k < store_pkg_count; k++)
                if (str_eq(store_pkgs[k].id, val)) { slot = &store_pkgs[k]; break; }
            if (!slot) {
                slot = &store_pkgs[store_pkg_count++];
                str_copy(slot->name, val, STORE_NAME_MAX);
                str_copy(slot->ver, "-", STORE_VER_MAX);
                str_copy(slot->cat, "Apps", STORE_CAT_MAX);
                slot->desc[0] = '\0';
                slot->icon = SI_PKG;
                slot->size = 0;
                added++;
            }
            str_copy(slot->id, val, STORE_ID_MAX);
            slot->src[0] = '\0';
            slot->remote = 1;
            slot->avail = 1;
            cur = slot;
            continue;
        }

        if (!cur) continue;

        if (str_eq(key, "name"))      str_copy(cur->name, val, STORE_NAME_MAX);
        else if (str_eq(key, "ver"))  str_copy(cur->ver, val, STORE_VER_MAX);
        else if (str_eq(key, "cat"))  str_copy(cur->cat, val, STORE_CAT_MAX);
        else if (str_eq(key, "desc")) str_copy(cur->desc, val, STORE_DESC_MAX);
        else if (str_eq(key, "icon")) cur->icon = (uint8_t)store_icon_from_name(val);
        else if (str_eq(key, "size")) {
            uint32_t v = 0;
            for (const char *q = val; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (uint32_t)(*q - '0');
            cur->size = v;
        } else if (str_eq(key, "url") || str_eq(key, "file")) {
            if (str_eq(key, "file") && val[0] == '/') {
                str_copy(cur->src, val, STORE_SRC_MAX);
                cur->remote = 0;
                cur->avail = (uint8_t)store_stat(cur->src, &cur->size);
            } else {
                store_resolve_url(val, cur->src, STORE_SRC_MAX);
                cur->remote = 1;
            }
        }
    }

    /* a remote block that never named a payload is not installable */
    for (int k = 0; k < store_pkg_count; k++)
        if (store_pkgs[k].remote && store_pkgs[k].src[0] == '\0')
            store_pkgs[k].avail = 0;

    return added;
}

/* ===== INSTALL / REMOVE / LAUNCH ===== */

static int store_is_elf64(const uint8_t *d, uint32_t len) {
    return len >= 64 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' &&
           d[3] == 'F' && d[4] == 2;
}

static int store_commit(store_pkg_t *p, const uint8_t *data, uint32_t len) {
    if (!store_is_elf64(data, len)) {
        store_say("payload is not a 64-bit ELF application", 2);
        return -1;
    }
    if (!fs_writable()) {
        store_say("read-only volume - attach a disk to install", 2);
        return -1;
    }

    fat_dirent_t d;
    if (!fat_lookup("/apps", &d) && fs_mkdir("/apps") != 0) {
        store_say2("cannot create /apps: ", fs_errstr, 2);
        return -1;
    }

    char path[STORE_PATH_MAX];
    store_app_path(p->id, path);
    if (fs_write_file(path, data, len) != 0) {
        store_say2("install failed: ", fs_errstr, 2);
        return -1;
    }

    int slot = store_find_inst(p->id);
    if (slot < 0) {
        if (store_inst_count >= STORE_MAX_INST) {
            store_say("app registry is full", 2);
            return -1;
        }
        slot = store_inst_count++;
    }
    store_inst_t *e = &store_inst[slot];
    str_copy(e->id, p->id, STORE_ID_MAX);
    str_copy(e->name, p->name, STORE_NAME_MAX);
    str_copy(e->ver, p->ver, STORE_VER_MAX);
    str_copy(e->path, path, STORE_PATH_MAX);
    e->icon = p->icon;
    store_registry_save();

    char sz[16];
    store_size_str(len, sz);
    store_say2("Installed ", p->name, 3);
    str_append(store_status, " (", sizeof(store_status));
    str_append(store_status, sz, sizeof(store_status));
    str_append(store_status, ") to ", sizeof(store_status));
    str_append(store_status, path, sizeof(store_status));
    return 0;
}

/* Arbitration for the single in-kernel TCP connection. */
static int store_net_busy(void) {
    return brw_loading || term_async == TERM_ASYNC_FETCH ||
           http_state == HTTP_RESOLVING || http_state == HTTP_CONNECTING ||
           http_state == HTTP_REQUESTING || http_state == HTTP_RECEIVING;
}

static int store_http_start(const char *url) {
    char host[128], path[256];
    uint16_t port;
    if (str_starts_with(url, "https://")) {
        store_say("https needs TLS, which this kernel does not have", 2);
        return -1;
    }
    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        store_say2("bad repository URL: ", url, 2);
        return -1;
    }
    if (!e1000_found) {
        store_say("no network adapter detected", 2);
        return -1;
    }
    if (store_net_busy()) {
        store_say("network is busy - try again when the transfer finishes", 2);
        return -1;
    }
    http_get(host, port, path);
    http_owner = HTTP_OWNER_STORE;
    store_net_t0 = net_ticks;
    return 0;
}

static void store_install(int idx) {
    if (idx < 0 || idx >= store_pkg_count) return;
    store_pkg_t *p = &store_pkgs[idx];

    if (store_net != STORE_NET_IDLE) {
        store_say("a download is already in progress", 2);
        return;
    }

    if (p->remote) {
        if (p->src[0] == '\0') {
            store_say2("no payload URL for ", p->id, 2);
            return;
        }
        if (store_http_start(p->src) != 0) return;
        store_net = STORE_NET_PAYLOAD;
        store_net_pkg = idx;
        store_say2("Downloading ", p->name, 1);
        return;
    }

    uint64_t len = 0;
    const void *data = store_read(p->src, &len);
    if (!data || len == 0) {
        store_say2("payload missing: ", p->src, 2);
        return;
    }
    store_commit(p, (const uint8_t *)data, (uint32_t)len);
}

static void store_remove(const char *id) {
    int slot = store_find_inst(id);
    if (slot < 0) {
        store_say2("not installed: ", id, 2);
        return;
    }
    char path[STORE_PATH_MAX];
    str_copy(path, store_inst[slot].path, sizeof(path));
    char name[STORE_NAME_MAX];
    str_copy(name, store_inst[slot].name, sizeof(name));

    if (fs_writable() && fs_delete(path) != 0) {
        store_say2("could not delete the binary: ", fs_errstr, 2);
        /* still drop the registry entry so the state stays consistent */
    }
    for (int i = slot; i < store_inst_count - 1; i++)
        store_inst[i] = store_inst[i + 1];
    store_inst_count--;
    store_registry_save();
    store_say2("Removed ", name, 1);
}

static void store_launch_inst(int idx) {
    if (idx < 0 || idx >= store_inst_count) return;
    char path[STORE_PATH_MAX];
    char name[STORE_NAME_MAX];
    str_copy(path, store_inst[idx].path, sizeof(path));
    str_copy(name, store_inst[idx].name, sizeof(name));

    silent_launch = 1;
    int rc = execute_bin_internal(path, 0);
    silent_launch = 0;

    if (rc != 0) {
        store_say2("could not run ", name, 2);
        return;
    }
    str_copy(app_win_title, name, sizeof(app_win_title));
    wm_open(WK_HELLO);
    store_say2("Launched ", name, 3);
}

static void store_launch_id(const char *id) {
    store_launch_inst(store_find_inst(id));
}

/* ===== NETWORK POLL (called once per frame) ===== */

static void store_refresh(void) {
    if (store_net != STORE_NET_IDLE) {
        store_say("already talking to the repository", 2);
        return;
    }
    if (store_http_start(store_repo) != 0) return;
    store_net = STORE_NET_INDEX;
    store_net_pkg = -1;
    store_say2("Fetching ", store_repo, 1);
}

/* A transfer kicked off from the shell finishes long after the command
 * returned, so echo the outcome into the scrollback when it lands. */
static void store_net_report(void) {
    if (!store_net_term) return;
    store_net_term = 0;
    term_print("  ");
    term_print_c(store_status, store_status_col == 2 ? 2 : 4);
    term_putc('\n');
}

static void store_poll(void) {
    if (store_net == STORE_NET_IDLE) return;

    if (http_owner != HTTP_OWNER_STORE) {
        store_net = STORE_NET_IDLE;
        store_net_pkg = -1;
        store_say("transfer cancelled - another app took the connection", 2);
        store_net_report();
        return;
    }

    if (http_state == HTTP_ERROR) {
        store_say2("repository error: ", http_err, 2);
        store_net = STORE_NET_IDLE;
        store_net_pkg = -1;
        http_owner = HTTP_OWNER_NONE;
        store_net_report();
        return;
    }
    if (http_state != HTTP_DONE) return;

    int what = store_net;
    int pkg = store_net_pkg;
    store_net = STORE_NET_IDLE;
    store_net_pkg = -1;
    http_owner = HTTP_OWNER_NONE;

    if (http_status_code != 200) {
        char nb[12];
        uint_to_str((uint32_t)http_status_code, nb);
        store_say2("repository returned HTTP ", nb, 2);
        store_net_report();
        return;
    }

    if (what == STORE_NET_INDEX) {
        int added = store_parse_index(http_body, http_body_len);
        char nb[12];
        uint_to_str((uint32_t)added, nb);
        store_say2("Repository index loaded - ", nb, 3);
        str_append(store_status, " remote package(s)", sizeof(store_status));
        if (store_scroll > 0) store_scroll = 0;
        store_net_report();
        return;
    }

    if (what == STORE_NET_PAYLOAD && pkg >= 0 && pkg < store_pkg_count)
        store_commit(&store_pkgs[pkg], http_body, (uint32_t)http_body_len);
    store_net_report();
}

/* ===== STARTUP ===== */

static void store_init(void) {
    store_catalog_local();
    store_registry_load();

    /* an optional saved repository URL */
    uint64_t len = 0;
    const void *d = store_read("/apps/repo.cfg", &len);
    if (d && len > 0 && len < STORE_SRC_MAX) {
        const char *p = (const char *)d;
        int n = 0;
        while (n < (int)len && p[n] != '\n' && p[n] != '\r') n++;
        if (n > 0) {
            for (int i = 0; i < n; i++) store_repo[i] = p[i];
            store_repo[n] = '\0';
        }
    }

    if (store_inst_count > 0) {
        char nb[12];
        uint_to_str((uint32_t)store_inst_count, nb);
        store_say2("", nb, 0);
        str_append(store_status, " app(s) installed. Refresh to check the"
                                 " repository for more.", sizeof(store_status));
    } else {
        store_say("Pick an app and press Install. Refresh checks the network"
                  " repository.", 0);
    }
}

/* ===== ICONS ===== */

/* Just the pictogram — the dock draws its own tile behind it. */
static void store_icon_glyph(uint32_t *buf, uint32_t w, uint32_t h,
                             int32_t x, int32_t y, int32_t sz, int kind) {
    int32_t cx = x + sz / 2;
    int32_t cy = y + sz / 2;
    int32_t q = sz / 4;

    if (kind == SI_FRACTAL) {
        gfx_circle(buf, w, h, cx + q / 2, cy, q, C_GOLD_DIM);
        gfx_circle(buf, w, h, cx - q, cy, q / 2 + 1, C_GOLD);
        gfx_circle(buf, w, h, cx - q - q / 2 - 1, cy, 2, C_GOLD);
        gfx_circle(buf, w, h, cx + q / 2, cy - q - 1, 2, C_GOLD_DIM);
        gfx_circle(buf, w, h, cx + q / 2, cy + q + 1, 2, C_GOLD_DIM);
    } else if (kind == SI_ORBIT) {
        gfx_circle_outline(buf, w, h, cx, cy, q + q / 2, 0x6C7590u);
        gfx_circle_outline(buf, w, h, cx, cy, q, 0x4E556Bu);
        gfx_circle(buf, w, h, cx, cy, 3, C_GOLD);
        gfx_circle(buf, w, h, cx + q + q / 2, cy, 2, 0x8FD0F0u);
        gfx_circle(buf, w, h, cx - q, cy - 1, 2, 0xE07A5Fu);
    } else if (kind == SI_GRID) {
        int32_t c = sz / 8;
        if (c < 2) c = 2;
        int32_t ox = cx - 2 * c - c / 2;
        int32_t oy = cy - 2 * c - c / 2;
        /* a glider, plus a couple of still cells */
        for (int gy = 0; gy < 5; gy++)
            for (int gx = 0; gx < 5; gx++) {
                int on = (gy == 0 && gx == 1) || (gy == 1 && gx == 2) ||
                         (gy == 2 && gx == 0) || (gy == 2 && gx == 1) ||
                         (gy == 2 && gx == 2) || (gy == 4 && gx == 4);
                gfx_rect(buf, w, h, ox + gx * c, oy + gy * c, c - 1, c - 1,
                         on ? C_GOLD : 0x2C3244u);
            }
    } else if (kind == SI_WAVE) {
        for (int k = 0; k < 3; k++) {
            uint32_t col = k == 1 ? C_GOLD : C_GOLD_DIM;
            int32_t base = cy - q + k * q;
            int32_t prev_y = base;
            for (int32_t px = 0; px < sz - 8; px++) {
                /* a cheap triangle wave keeps this dependency-free */
                int32_t t = (px + k * 5) % 16;
                int32_t amp = t < 8 ? t - 4 : 12 - t;
                int32_t yy = base + amp / 2;
                gfx_line(buf, w, h, x + 4 + px - 1, prev_y, x + 4 + px, yy,
                         1, col);
                prev_y = yy;
            }
        }
    } else {
        /* generic parcel */
        gfx_rect(buf, w, h, cx - q - 2, cy - q, 2 * q + 4, 2 * q, 0x2C3244u);
        gfx_rect_outline(buf, w, h, cx - q - 2, cy - q, 2 * q + 4, 2 * q,
                         C_GOLD_DIM);
        gfx_rect(buf, w, h, cx - 2, cy - q, 4, 2 * q, C_GOLD);
        gfx_rect(buf, w, h, cx - q - 2, cy - 2, 2 * q + 4, 4, C_GOLD);
    }
}

/* Storefront icon: dark tile plus the pictogram. */
static void store_icon(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t x, int32_t y, int32_t sz, int kind) {
    gfx_vgrad(buf, w, h, x, y, sz, sz, 0x232838u, 0x12151Eu);
    gfx_rect_outline(buf, w, h, x, y, sz, sz, 0x39405Au);
    store_icon_glyph(buf, w, h, x, y, sz, kind);
}

/* ===== LAYOUT ===== */

static int store_list_y(int32_t cy) {
    return (int)cy + STORE_HEAD_H + STORE_BAR_H;
}

static int store_total_h(void) {
    return store_pkg_count * STORE_CARD_H + 8;
}

static void store_scroll_by(int dy) {
    store_scroll += dy;
    int max_s = store_total_h() - store_view_h;
    if (max_s < 0) max_s = 0;
    if (store_scroll > max_s) store_scroll = max_s;
    if (store_scroll < 0) store_scroll = 0;
}

static void store_reveal(int idx) {
    int top = idx * STORE_CARD_H;
    if (top < store_scroll) store_scroll = top;
    else if (top + STORE_CARD_H > store_scroll + store_view_h)
        store_scroll = top + STORE_CARD_H - store_view_h;
    store_scroll_by(0);
}

/* Button rectangles inside a card, laid out from the right edge. */
static void store_btn_rect(int32_t cx, int32_t cw, int32_t card_y, int which,
                           int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    int32_t right = cx + cw - STORE_SCROLLW - 18;
    *bh = 26;
    *by = card_y + 30;
    if (which == 0) {           /* Install / Open */
        *bw = 82;
        *bx = right - 82;
    } else {                    /* Remove */
        *bw = 78;
        *bx = right - 82 - 8 - 78;
    }
}

/* ===== INPUT ===== */

static void store_primary(int idx) {
    if (idx < 0 || idx >= store_pkg_count) return;
    int inst = store_find_inst(store_pkgs[idx].id);
    if (inst >= 0) store_launch_inst(inst);
    else if (store_pkgs[idx].avail) store_install(idx);
    else store_say2("payload not available: ", store_pkgs[idx].src, 2);
}

static void store_key(char ch) {
    if (ch == KEY_UP) {
        if (store_sel > 0) store_sel--;
        store_reveal(store_sel);
    } else if (ch == KEY_DOWN) {
        if (store_sel < store_pkg_count - 1) store_sel++;
        store_reveal(store_sel);
    } else if (ch == KEY_PGUP) {
        store_scroll_by(-store_view_h + STORE_CARD_H);
    } else if (ch == KEY_PGDN) {
        store_scroll_by(store_view_h - STORE_CARD_H);
    } else if (ch == KEY_HOME) {
        store_sel = 0;
        store_scroll = 0;
    } else if (ch == KEY_END) {
        store_sel = store_pkg_count - 1;
        store_reveal(store_sel);
    } else if (ch == '\n') {
        store_primary(store_sel);
    } else if (ch == KEY_DEL) {
        if (store_sel >= 0 && store_sel < store_pkg_count)
            store_remove(store_pkgs[store_sel].id);
    } else if (ch == 'r' || ch == 'R') {
        store_refresh();
    } else if (ch == 27) {
        wm_close(WK_STORE);
    }
}

static void store_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    int click = (lmb && !prev_lmb);
    int32_t ly = store_list_y(cy);
    int32_t vh = cy + chh - ly;
    store_view_h = vh > 0 ? vh : 1;

    if (!lmb) store_sb_drag = 0;

    /* The window manager hands the focused window every event, wherever
     * the pointer is, so presses outside the content area are ours to
     * discard.  An in-progress scrollbar drag still tracks off-window. */
    if (click && (mx < cx || mx >= cx + cw || my < cy || my >= cy + chh))
        return;

    /* scrollbar */
    int total = store_total_h();
    int32_t sb_x = cx + cw - STORE_SCROLLW - 2;
    if (total > vh && vh > 40) {
        int knob_h = vh * vh / total;
        if (knob_h < 24) knob_h = 24;
        int max_s = total - vh;
        int knob_y = ly + (vh - knob_h) * store_scroll / (max_s > 0 ? max_s : 1);

        if (click && mx >= sb_x && mx < sb_x + STORE_SCROLLW + 2 &&
            my >= ly && my < ly + vh) {
            if (my >= knob_y && my < knob_y + knob_h) {
                store_sb_drag = 1;
                store_sb_off = my - knob_y;
            } else if (my < knob_y) {
                store_scroll_by(-vh + STORE_CARD_H);
            } else {
                store_scroll_by(vh - STORE_CARD_H);
            }
            return;
        }
        if (store_sb_drag && lmb) {
            int span = vh - knob_h;
            if (span > 0) {
                store_scroll = (my - store_sb_off - ly) * max_s / span;
                store_scroll_by(0);
            }
            return;
        }
    }

    if (!click) return;

    /* header: Refresh button */
    {
        int32_t bx = cx + cw - 108, by = cy + 18;
        if (mx >= bx && mx < bx + 92 && my >= by && my < by + 28) {
            store_refresh();
            return;
        }
    }

    if (my < ly) return;

    /* cards */
    for (int i = 0; i < store_pkg_count; i++) {
        int32_t card_y = ly + 4 + i * STORE_CARD_H - store_scroll;
        if (card_y + STORE_CARD_H - 8 < ly || card_y > cy + chh) continue;
        if (my < card_y || my >= card_y + STORE_CARD_H - 8) continue;

        store_sel = i;
        int inst = store_find_inst(store_pkgs[i].id);

        int32_t bx, by, bw, bh;
        store_btn_rect(cx, cw, card_y, 0, &bx, &by, &bw, &bh);
        /* buttons that were clipped away are not clickable either */
        if (by < ly || by + bh > cy + chh) return;
        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
            store_primary(i);
            return;
        }
        if (inst >= 0) {
            store_btn_rect(cx, cw, card_y, 1, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                store_remove(store_pkgs[i].id);
                return;
            }
        }
        return;
    }
}

/* ===== DRAW ===== */

/* Copy src into dst, truncating with an ellipsis to fit budget pixels. */
static void store_fit(char *dst, int dst_max, const char *src,
                      int budget, int font) {
    str_copy(dst, src, dst_max);
    if (ttf_text_width(dst, font) <= budget) return;
    int n = str_len(dst);
    while (n > 1) {
        dst[n - 1] = '\0';
        dst[n - 2] = '.';
        if (n >= 3) dst[n - 3] = '.';
        if (ttf_text_width(dst, font) <= budget) return;
        n--;
    }
}

static void store_button(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t x, int32_t y, int32_t bw, int32_t bh,
                         const char *label, int style) {
    /* style: 0 primary (gold), 1 secondary, 2 disabled */
    uint32_t fill = style == 0 ? 0x2A2410u : 0xE7E9EEu;
    uint32_t edge = style == 0 ? C_GOLD : (style == 2 ? 0xD0D3DAu : 0xB8BCC8u);
    uint32_t ink  = style == 0 ? C_GOLD : (style == 2 ? 0xA7ABB5u : 0x3C414Bu);
    if (style == 2) fill = 0xF0F1F4u;

    gfx_rect(buf, w, h, x, y, bw, bh, fill);
    gfx_rect_outline(buf, w, h, x, y, bw, bh, edge);
    int tw = ttf_text_width(label, 13);
    ttf_draw_string(buf, (int)w, (int)h, x + (bw - tw) / 2, y + (bh - 17) / 2,
                    label, ink, 13);
}

static void store_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       uint32_t tick, int focused) {
    (void)focused;
    gfx_rect(buf, w, h, cx, cy, cw, chh, C_WIN_BG);

    /* ---- header ---- */
    gfx_vgrad(buf, w, h, cx, cy, cw, STORE_HEAD_H, 0x1B2030u, 0x11141Cu);
    gfx_rect(buf, w, h, cx, cy + STORE_HEAD_H - 1, cw, 1, C_GOLD_DIM);
    {
        int32_t lx = cx + 26, ly = cy + 28;
        gfx_tri(buf, w, h, lx, ly - 11, lx - 9, ly, lx, ly + 11, C_GOLD);
        gfx_tri(buf, w, h, lx, ly - 11, lx + 9, ly, lx, ly + 11, C_GOLD_DIM);
    }
    ttf_draw_string(buf, (int)w, (int)h, cx + 46, cy + 10, "Agora", C_GOLD, 21);
    ttf_draw_string(buf, (int)w, (int)h, cx + 46, cy + 36,
                    "Socrates App Store", C_TEXT_DIM, 12);
    {
        char sum[48], nb[12];
        uint_to_str((uint32_t)store_pkg_count, nb);
        str_copy(sum, nb, sizeof(sum));
        str_append(sum, " packages, ", sizeof(sum));
        uint_to_str((uint32_t)store_inst_count, nb);
        str_append(sum, nb, sizeof(sum));
        str_append(sum, " installed", sizeof(sum));
        int tw = ttf_text_width(sum, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + cw - 118 - tw, cy + 36,
                        sum, C_TEXT_DIM, 12);
    }

    /* Refresh button */
    {
        int32_t bx = cx + cw - 108, by = cy + 18;
        int busy = (store_net != STORE_NET_IDLE);
        gfx_rect(buf, w, h, bx, by, 92, 28, busy ? 0x232838u : 0x2A2410u);
        gfx_rect_outline(buf, w, h, bx, by, 92, 28,
                         busy ? 0x4A5060u : C_GOLD);
        const char *lbl = busy ? "Working" : "Refresh";
        int tw = ttf_text_width(lbl, 13);
        ttf_draw_string(buf, (int)w, (int)h, bx + (92 - tw) / 2, by + 6, lbl,
                        busy ? C_TEXT_DIM : C_GOLD, 13);
        if (busy) {
            /* progress ticker so a slow repo does not look hung */
            int32_t px = bx + 6 + (int32_t)((tick / 4) % 80);
            gfx_rect(buf, w, h, px, by + 24, 8, 2, C_GOLD);
        }
    }

    /* ---- status bar ---- */
    gfx_rect(buf, w, h, cx, cy + STORE_HEAD_H, cw, STORE_BAR_H, 0xE8E9EEu);
    gfx_rect(buf, w, h, cx, cy + STORE_HEAD_H + STORE_BAR_H - 1, cw, 1,
             0xD5D8E0u);
    {
        uint32_t col = store_status_col == 2 ? 0xB0322Eu :
                       store_status_col == 3 ? 0x2E7D4Fu :
                       store_status_col == 1 ? C_LINK : 0x50555Fu;
        char line[96];
        store_fit(line, sizeof(line), store_status, cw - 28, 12);
        ttf_draw_string(buf, (int)w, (int)h, cx + 14, cy + STORE_HEAD_H + 4,
                        line, col, 12);
    }

    /* ---- list ---- */
    int32_t ly = store_list_y(cy);
    int32_t vh = cy + chh - ly;
    store_view_h = vh > 0 ? vh : 1;
    store_scroll_by(0);

    int32_t list_w = cw - STORE_SCROLLW - 4;

    for (int i = 0; i < store_pkg_count; i++) {
        int32_t card_y = ly + 4 + i * STORE_CARD_H - store_scroll;
        int32_t card_h = STORE_CARD_H - 8;
        if (card_y + card_h < ly || card_y > cy + chh) continue;

        store_pkg_t *p = &store_pkgs[i];
        int inst = store_find_inst(p->id);
        int sel = (i == store_sel);

        /* Clip to the viewport by hand: the gfx primitives clip to the
         * backbuffer, not to the window, and the text renderer cannot
         * clip at all — so each element is drawn only if it fits whole,
         * which makes a half-scrolled card reveal itself progressively. */
        int32_t clip_top = ly;
        int32_t clip_bot = cy + chh;
        int32_t top = card_y < clip_top ? clip_top : card_y;
        int32_t bot = card_y + card_h;
        if (bot > clip_bot) bot = clip_bot;
        if (bot <= top) continue;

#define STORE_FITS(y0, hgt) \
    ((y0) >= clip_top && (y0) + (hgt) <= clip_bot)

        uint32_t edge = sel ? C_GOLD : 0xDCDEE5u;
        gfx_rect(buf, w, h, cx + 10, top, list_w - 12, bot - top, 0xFFFFFFu);
        if (card_y >= clip_top && card_y < clip_bot)
            gfx_rect(buf, w, h, cx + 10, card_y, list_w - 12, 1, edge);
        if (card_y + card_h - 1 >= clip_top && card_y + card_h - 1 < clip_bot)
            gfx_rect(buf, w, h, cx + 10, card_y + card_h - 1, list_w - 12, 1,
                     edge);
        gfx_rect(buf, w, h, cx + 10, top, 1, bot - top, edge);
        gfx_rect(buf, w, h, cx + 10 + list_w - 13, top, 1, bot - top, edge);
        if (sel)
            gfx_rect(buf, w, h, cx + 10, top, 3, bot - top, C_GOLD);

        if (STORE_FITS(card_y + 11, 56))
            store_icon(buf, w, h, cx + 24, card_y + 11, 56, p->icon);

        if (STORE_FITS(card_y + 12, 18)) {
            char nm[STORE_NAME_MAX + 4];
            store_fit(nm, sizeof(nm), p->name, list_w - 300, 15);
            ttf_draw_string(buf, (int)w, (int)h, cx + 92, card_y + 12, nm,
                            0x1A1E28u, 15);

            /* installed / unavailable badge sits beside the name */
            if (inst >= 0)
                gfx_circle(buf, w, h, cx + 84, card_y + 20, 3, 0x2E7D4Fu);
            else if (!p->avail)
                gfx_circle(buf, w, h, cx + 84, card_y + 20, 3, 0xC06060u);
        }

        /* version - category - size - source */
        if (STORE_FITS(card_y + 33, 14)) {
            char meta[72], sz[16];
            str_copy(meta, "v", sizeof(meta));
            str_append(meta, p->ver, sizeof(meta));
            str_append(meta, "   ", sizeof(meta));
            str_append(meta, p->cat, sizeof(meta));
            str_append(meta, "   ", sizeof(meta));
            store_size_str(p->size, sz);
            str_append(meta, sz, sizeof(meta));
            str_append(meta, p->remote ? "   network" : "   local", sizeof(meta));
            char fit[72];
            store_fit(fit, sizeof(fit), meta, list_w - 300, 11);
            ttf_draw_string(buf, (int)w, (int)h, cx + 92, card_y + 33, fit,
                            0x848A96u, 11);
        }

        if (STORE_FITS(card_y + 50, 16)) {
            char ds[STORE_DESC_MAX + 4];
            store_fit(ds, sizeof(ds), p->desc, list_w - 300, 12);
            ttf_draw_string(buf, (int)w, (int)h, cx + 92, card_y + 50, ds,
                            0x555B66u, 12);
        }

        int32_t bx, by, bw, bh;
        store_btn_rect(cx, cw, card_y, 0, &bx, &by, &bw, &bh);
        if (STORE_FITS(by, bh)) {
            if (inst >= 0)
                store_button(buf, w, h, bx, by, bw, bh, "Open", 0);
            else if (p->avail)
                store_button(buf, w, h, bx, by, bw, bh, "Install", 0);
            else
                store_button(buf, w, h, bx, by, bw, bh, "Unavailable", 2);

            if (inst >= 0) {
                store_btn_rect(cx, cw, card_y, 1, &bx, &by, &bw, &bh);
                store_button(buf, w, h, bx, by, bw, bh, "Remove", 1);
            }
        }
#undef STORE_FITS
    }

    if (store_pkg_count == 0) {
        const char *msg = "No packages. Press Refresh to query the repository.";
        int tw = ttf_text_width(msg, 13);
        ttf_draw_string(buf, (int)w, (int)h, cx + (cw - tw) / 2, ly + 40, msg,
                        0x8A8F9Cu, 13);
    }

    /* scrollbar */
    {
        int total = store_total_h();
        int32_t sb_x = cx + cw - STORE_SCROLLW - 2;
        gfx_rect(buf, w, h, sb_x, ly, STORE_SCROLLW, vh, 0xE2E3E8u);
        if (total > vh && vh > 40) {
            int knob_h = vh * vh / total;
            if (knob_h < 24) knob_h = 24;
            int max_s = total - vh;
            int knob_y = ly + (vh - knob_h) * store_scroll /
                         (max_s > 0 ? max_s : 1);
            gfx_rect(buf, w, h, sb_x + 1, knob_y, STORE_SCROLLW - 2, knob_h,
                     store_sb_drag ? C_GOLD_DIM : 0xB6BAC4u);
        }
    }
}

/* ===== TERMINAL COMMAND ===== */

static void store_cmd(int argc, char **argv) {
    if (argc < 2 || str_eq(argv[1], "list")) {
        char nb[16];
        for (int i = 0; i < store_pkg_count; i++) {
            store_pkg_t *p = &store_pkgs[i];
            int inst = store_find_inst(p->id);
            term_print("  ");
            term_print_c(p->id, inst >= 0 ? 4 : 0);
            int pad = 10 - str_len(p->id);
            for (int j = 0; j < pad; j++) term_putc(' ');
            term_print(p->ver);
            pad = 8 - str_len(p->ver);
            for (int j = 0; j < pad; j++) term_putc(' ');
            term_print_c(p->cat, 3);
            pad = 13 - str_len(p->cat);
            for (int j = 0; j < pad; j++) term_putc(' ');
            if (inst >= 0)        term_print_c("installed\n", 4);
            else if (!p->avail)   term_print_c("unavailable\n", 2);
            else if (p->remote) { store_size_str(p->size, nb);
                                  term_print_c("network\n", 5); }
            else                  term_print_c("available\n", 3);
        }
        if (store_pkg_count == 0) term_print_c("  (catalog empty)\n", 3);
        term_print_c("  repo: ", 3);
        term_print_c(store_repo, 3);
        term_putc('\n');
        return;
    }

    if (str_eq(argv[1], "install") || str_eq(argv[1], "add")) {
        if (argc < 3) { term_print_c("usage: store install <id>\n", 2); return; }
        for (int i = 0; i < store_pkg_count; i++)
            if (str_eq(store_pkgs[i].id, argv[2])) {
                store_install(i);
                term_print("  ");
                term_print_c(store_status, store_status_col == 2 ? 2 : 4);
                term_putc('\n');
                /* a network install lands a few frames from now */
                if (store_net != STORE_NET_IDLE) store_net_term = 1;
                return;
            }
        term_print_c("no such package: ", 2);
        term_print_c(argv[2], 2);
        term_putc('\n');
        return;
    }

    if (str_eq(argv[1], "remove") || str_eq(argv[1], "rm")) {
        if (argc < 3) { term_print_c("usage: store remove <id>\n", 2); return; }
        store_remove(argv[2]);
        term_print("  ");
        term_print_c(store_status, store_status_col == 2 ? 2 : 4);
        term_putc('\n');
        return;
    }

    if (str_eq(argv[1], "run") || str_eq(argv[1], "open")) {
        if (argc < 3) { term_print_c("usage: store run <id>\n", 2); return; }
        if (store_find_inst(argv[2]) < 0) {
            term_print_c("not installed: ", 2);
            term_print_c(argv[2], 2);
            term_putc('\n');
            return;
        }
        store_launch_id(argv[2]);
        return;
    }

    if (str_eq(argv[1], "refresh") || str_eq(argv[1], "update")) {
        store_refresh();
        term_print("  ");
        term_print_c(store_status, store_status_col == 2 ? 2 : 3);
        term_putc('\n');
        if (store_net != STORE_NET_IDLE) store_net_term = 1;
        return;
    }

    if (str_eq(argv[1], "repo")) {
        if (argc < 3) {
            term_print("  repository: ");
            term_print_c(store_repo, 1);
            term_putc('\n');
            return;
        }
        str_copy(store_repo, argv[2], STORE_SRC_MAX);
        if (fs_writable())
            fs_write_file("/apps/repo.cfg", store_repo,
                          (uint32_t)str_len(store_repo));
        term_print("  repository set to ");
        term_print_c(store_repo, 1);
        term_putc('\n');
        return;
    }

    if (str_eq(argv[1], "installed")) {
        for (int i = 0; i < store_inst_count; i++) {
            term_print("  ");
            term_print_c(store_inst[i].id, 4);
            int pad = 10 - str_len(store_inst[i].id);
            for (int j = 0; j < pad; j++) term_putc(' ');
            term_print(store_inst[i].name);
            term_print("  ");
            term_print_c(store_inst[i].path, 3);
            term_putc('\n');
        }
        if (store_inst_count == 0) term_print_c("  (nothing installed)\n", 3);
        return;
    }

    term_print_c("usage: store [list|installed|install <id>|remove <id>|"
                 "run <id>|refresh|repo [url]]\n", 2);
}

#endif /* STORE_H */
