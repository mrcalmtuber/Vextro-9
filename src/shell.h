#ifndef VEXTRO_SHELL_H
#define VEXTRO_SHELL_H

/*
 * src/shell.h — the parts of the desktop that are not windows.
 *
 * Three things live here, and they are together because they all answer
 * the same question from different directions: what has this person been
 * doing?
 *
 *   Recent items   — what each app was last pointed at. The jump lists
 *                    read it, and so does the start menu.
 *   Gadgets        — what the machine is doing right now, on the desktop
 *                    rather than inside a window.
 *   The busy meter — the one number the gadgets cannot compute for
 *                    themselves, filled in by the render loop.
 *
 * No allocation, no FPU, no libc: fixed arrays and integer arithmetic,
 * the same as everything else in this tree.
 */

/* ===== 1. RECENT ITEMS =====
 *
 * A short move-to-front list per app. Re-opening something already in the
 * list moves it up rather than adding a second copy, so a list of eight
 * is eight distinct things rather than eight visits to the same one.
 */

#define RECENT_MAX   8
#define RECENT_LABEL 44
#define RECENT_PATH  160

typedef struct {
    char label[RECENT_LABEL];   /* what to show */
    char path[RECENT_PATH];     /* what to reopen */
} recent_t;

static recent_t recents[WK_COUNT][RECENT_MAX];
static int      recent_n[WK_COUNT];

static void recent_push(int kind, const char *label, const char *path) {
    if (kind < 0 || kind >= WK_COUNT || !label || !path) return;
    if (!label[0] || !path[0]) return;

    /* Already known? Move it to the front and keep the newer label --
     * a page's title changes more often than its address does. */
    int at = -1;
    for (int i = 0; i < recent_n[kind]; i++)
        if (str_eq(recents[kind][i].path, path)) { at = i; break; }

    if (at < 0) {
        at = recent_n[kind] < RECENT_MAX ? recent_n[kind]++ : RECENT_MAX - 1;
    }
    for (int i = at; i > 0; i--)
        recents[kind][i] = recents[kind][i - 1];

    str_copy(recents[kind][0].label, label, RECENT_LABEL);
    str_copy(recents[kind][0].path, path, RECENT_PATH);
}

static void recent_clear_all(void) {
    for (int k = 0; k < WK_COUNT; k++) {
        recent_n[k] = 0;
        for (int i = 0; i < RECENT_MAX; i++) {
            recents[k][i].label[0] = '\0';
            recents[k][i].path[0] = '\0';
        }
    }
}

/* ===== 2. SEARCH =====
 *
 * Type into the start menu and it looks in three places: the apps, the
 * things each app was recently pointed at, and the volume itself.
 *
 * The volume walk is breadth-first with two hard budgets -- directories
 * visited and hits collected -- because this runs inside the frame that
 * drew the menu. A search that finished eventually but dropped the frame
 * rate to nothing while it did would be worse than one that admits it
 * only looked at the first few hundred directories.
 *
 * fs_list takes a plain function pointer with no user argument, so the
 * walk state has to be file-scope. That is also why the walk cannot
 * recurse from inside the callback: the callback only queues directories,
 * and the loop below drains the queue.
 */

#define SEARCH_Q_MAX     28
#define SEARCH_HITS_MAX  10
#define SEARCH_DIRS_MAX  48      /* directories opened per search */
#define SEARCH_QUEUE_MAX 32

typedef struct {
    char label[64];
    char path[RECENT_PATH];
    int  kind;                   /* which app opens it */
    int  is_dir;
} search_hit_t;

static char search_q[SEARCH_Q_MAX];
static int  search_q_n = 0;

static search_hit_t search_hits[SEARCH_HITS_MAX];
static int          search_hit_n = 0;

/* walk state, file-scope because fs_list's callback carries no context */
static char sw_queue[SEARCH_QUEUE_MAX][RECENT_PATH];
static int  sw_queue_n = 0, sw_queue_head = 0;
static char sw_cur[RECENT_PATH];

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive substring test. Search that only matched exact case
 * would be a filter, not a search. */
static int str_contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] &&
               lower_ch(hay[i + j]) == lower_ch(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void search_add(const char *label, const char *path, int kind, int is_dir) {
    if (search_hit_n >= SEARCH_HITS_MAX) return;
    for (int i = 0; i < search_hit_n; i++)
        if (str_eq(search_hits[i].path, path)) return;   /* already found */
    search_hit_t *h = &search_hits[search_hit_n++];
    str_copy(h->label, label, sizeof(h->label));
    str_copy(h->path, path, RECENT_PATH);
    h->kind = kind;
    h->is_dir = is_dir;
}

/* Which app should open a file, decided by its extension. */
static int search_kind_for(const char *name) {
    int n = str_len(name);
    if (n > 4 && str_eq(name + n - 4, ".sci")) return WK_IMAGE;
    if (n > 4 && str_eq(name + n - 4, ".zim")) return WK_WIKI;
    return WK_FILES;
}

static void search_join(char *out, int cap, const char *dir, const char *name) {
    str_copy(out, dir, cap);
    if (!(dir[0] == '/' && dir[1] == '\0')) str_append(out, "/", cap);
    str_append(out, name, cap);
}

static void search_walk_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (name[0] == '.') return;
    char full[RECENT_PATH];
    search_join(full, RECENT_PATH, sw_cur, name);

    if (str_contains_ci(name, search_q))
        search_add(name, full, is_dir ? WK_FILES : search_kind_for(name), is_dir);

    if (is_dir && sw_queue_n < SEARCH_QUEUE_MAX)
        str_copy(sw_queue[sw_queue_n++], full, RECENT_PATH);
}

static void search_run(void) {
    search_hit_n = 0;
    if (search_q_n == 0) return;

    /* Recently opened things first: they are what someone is most likely
     * to be reaching for, and they cost nothing to search. */
    for (int k = 0; k < WK_COUNT; k++)
        for (int i = 0; i < recent_n[k]; i++)
            if (str_contains_ci(recents[k][i].label, search_q) ||
                str_contains_ci(recents[k][i].path, search_q))
                search_add(recents[k][i].label, recents[k][i].path, k, 0);

    /* Then the volume, breadth-first and on a budget. */
    sw_queue_n = sw_queue_head = 0;
    str_copy(sw_queue[sw_queue_n++], "/", RECENT_PATH);

    int opened = 0;
    while (sw_queue_head < sw_queue_n &&
           opened < SEARCH_DIRS_MAX &&
           search_hit_n < SEARCH_HITS_MAX) {
        str_copy(sw_cur, sw_queue[sw_queue_head++], RECENT_PATH);
        opened++;
        fs_list(sw_cur, search_walk_cb);
    }
}

static void search_clear(void) {
    search_q[0] = '\0';
    search_q_n = 0;
    search_hit_n = 0;
}

/* ===== 3. THE ACTION CENTER =====
 *
 * One place where the system says what it has been doing, instead of
 * each subsystem inventing its own way to interrupt.
 *
 * A ring of the last NOTIFY_MAX events with a category on each, and an
 * unread count on the menubar. Nothing here steals focus or blocks: a
 * notification that has to be dismissed before work continues is a
 * dialog, and dialogs are for questions, not for news.
 *
 * The categories are the three the flag colours itself from -- an alert
 * has to look different from a note at a glance, or the flag says only
 * "something happened", which the timestamp already said.
 */

/* NOTIFY_TEXT and the NOTE_* categories are declared up in desktop.h,
 * where the subsystems that file notifications can see them. */
#define NOTIFY_MAX 16

typedef struct {
    char    text[NOTIFY_TEXT];
    uint8_t cat;
    int     hh, mm;          /* when, off the clock the menubar shows */
} notify_t;

static notify_t notify_ring[NOTIFY_MAX];
static int      notify_n = 0;        /* how many are held, <= NOTIFY_MAX */
static int      notify_head = 0;     /* next slot to write */
static int      notify_unread = 0;

static void notify_push(int cat, const char *text) {
    if (!text || !text[0]) return;
    notify_t *e = &notify_ring[notify_head];
    str_copy(e->text, text, NOTIFY_TEXT);
    e->cat = (uint8_t)cat;

    int ss, day, mon, yr;
    rtc_read(&e->hh, &e->mm, &ss, &day, &mon, &yr);

    notify_head = (notify_head + 1) % NOTIFY_MAX;
    if (notify_n < NOTIFY_MAX) notify_n++;
    if (notify_unread < NOTIFY_MAX) notify_unread++;
}

/* Newest first, which is the order they are read in. */
static const notify_t *notify_at(int i) {
    if (i < 0 || i >= notify_n) return 0;
    int idx = notify_head - 1 - i;
    while (idx < 0) idx += NOTIFY_MAX;
    return &notify_ring[idx];
}

static void notify_clear(void) {
    notify_n = 0;
    notify_head = 0;
    notify_unread = 0;
}

/* ===== 4. IDLE DIMMING =====
 *
 * A screen nobody is looking at does not need to be at full brightness.
 * After DIM_IDLE seconds without a pointer move, a click or a keystroke
 * the frame fades down, and the first input brings it straight back.
 *
 * There is no backlight to turn down here -- this is a blend towards
 * black over the composited frame, which is the only lever a framebuffer
 * gives you. It still saves real work downstream: once the fade settles,
 * every frame is identical to the last, so the flip's row diff finds
 * nothing to send to the panel.
 *
 * The wake ramp is four times the sleep ramp. Fading away slowly is
 * calm; coming back slowly is the machine feeling unresponsive.
 */

#define DIM_IDLE_SECS 90
#define DIM_RAMP      48       /* frames to fade all the way down */
#define DIM_DEPTH     168      /* how black it gets, 0..255 */

static uint32_t dim_last_input = 0;
static int32_t  dim_t = 0;

static void dim_wake(void) {
    dim_last_input = desktop_tick;
}

/* Returns how dark the frame should be, 0..255. */
static uint32_t dim_step(void) {
    const uint32_t idle = desktop_tick - dim_last_input;
    if (idle > (uint32_t)DIM_IDLE_SECS * 60u) {
        if (dim_t < DIM_RAMP) dim_t++;
    } else {
        dim_t -= 4;
        if (dim_t < 0) dim_t = 0;
    }
    return (uint32_t)(dim_t * DIM_DEPTH / DIM_RAMP);
}

/* ===== 5. THE BUSY METER =====
 *
 * The render loop already counts cycles spent compositing, flipping and
 * idling, to report them on the serial line. Turning that into one
 * percentage and a short history costs nothing extra and gives the
 * gadgets something true to draw -- a meter fed by a counter that ticks
 * whether or not anyone is looking would be decoration, not
 * instrumentation.
 *
 * Written by frame_report() in kernel.c, which runs after this header is
 * included and so can see these.
 */

#define BUSY_HIST 48

static uint32_t sys_busy_pct = 0;
static uint8_t  sys_busy_hist[BUSY_HIST];
static int      sys_busy_pos = 0;

static void sys_busy_record(uint32_t busy_cy, uint32_t idle_cy) {
    const uint32_t total = busy_cy + idle_cy;
    sys_busy_pct = total ? (uint32_t)(((uint64_t)busy_cy * 100u) / total) : 0;
    if (sys_busy_pct > 100) sys_busy_pct = 100;
    sys_busy_hist[sys_busy_pos] = (uint8_t)sys_busy_pct;
    sys_busy_pos = (sys_busy_pos + 1) % BUSY_HIST;
}

/* ===== 3. DESKTOP GADGETS =====
 *
 * Mini-applications with no window and no chrome, drawn straight onto the
 * desktop under the window stack. They are stacked down the right-hand
 * side rather than pinned into a sidebar, so a maximized window covers
 * them completely and they cost nothing when they cannot be seen.
 */

#define GADGET_W      210
#define GADGET_GAP    12
#define GADGET_MARGIN 16

enum { GADGET_CLOCK = 0, GADGET_SYSTEM, GADGET_NETWORK, GADGET_COUNT };

static int gadgets_on = 1;
static int gadget_show[GADGET_COUNT] = { 1, 1, 1 };

static const char *const gadget_names[GADGET_COUNT] = {
    "Clock", "System", "Network"
};

static const int32_t gadget_heights[GADGET_COUNT] = { 96, 104, 96 };

/* Where gadget `g` sits, or 0 if it is switched off. */
static int gadget_rect(int g, uint32_t w, int32_t *ox, int32_t *oy,
                       int32_t *ow, int32_t *oh) {
    if (!gadgets_on || g < 0 || g >= GADGET_COUNT || !gadget_show[g]) return 0;
    int32_t y = MENUBAR_H + GADGET_GAP;
    for (int i = 0; i < g; i++)
        if (gadget_show[i]) y += gadget_heights[i] + GADGET_GAP;
    *ox = (int32_t)w - GADGET_W - GADGET_MARGIN;
    *oy = y;
    *ow = GADGET_W;
    *oh = gadget_heights[g];
    return 1;
}

static void gadget_plate(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t x, int32_t y, int32_t gw, int32_t gh,
                         const char *title) {
    gfx_rect_blend(buf, w, h, x, y, gw, gh, 0x0E1118u, 176);
    gfx_rect_outline(buf, w, h, x, y, gw, gh, 0x2A3142u);
    gfx_rect(buf, w, h, x, y, gw, 1, gfx_mix(C_GOLD, 0x0E1118u, 70));
    ttf_draw_string(buf, (int)w, (int)h, x + 12, y + 7, title, C_GOLD_DIM, 11);
}

/* A meter drawn as a row of cells rather than a smooth bar: at this size
 * a filled rectangle two pixels longer than last frame reads as noise,
 * where a cell lighting up reads as a step. */
static void gadget_meter(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t x, int32_t y, int32_t mw, int32_t mh,
                         uint32_t pct, uint32_t col) {
    const int32_t cells = 20;
    const int32_t cw = mw / cells;
    const int32_t lit = (int32_t)(pct * (uint32_t)cells / 100u);
    for (int32_t i = 0; i < cells; i++)
        gfx_rect(buf, w, h, x + i * cw, y, cw - 2, mh,
                 i < lit ? col : 0x1C2130u);
}

static void gadget_draw_clock(uint32_t *buf, uint32_t w, uint32_t h,
                              int32_t x, int32_t y, int32_t gw, int32_t gh) {
    gadget_plate(buf, w, h, x, y, gw, gh, "CLOCK");

    int hh, mm, ss, day, mon, yr;
    rtc_read(&hh, &mm, &ss, &day, &mon, &yr);

    char t[16], nb[12];
    uint_to_str((uint32_t)hh, nb);
    if (hh < 10) { str_copy(t, "0", sizeof(t)); str_append(t, nb, sizeof(t)); }
    else           str_copy(t, nb, sizeof(t));
    str_append(t, ":", sizeof(t));
    uint_to_str((uint32_t)mm, nb);
    if (mm < 10) str_append(t, "0", sizeof(t));
    str_append(t, nb, sizeof(t));

    const int tw = ttf_text_width(t, 34);
    ttf_draw_string(buf, (int)w, (int)h, x + (gw - tw) / 2, y + 26, t, C_TEXT, 34);

    char sec[8];
    uint_to_str((uint32_t)ss, nb);
    str_copy(sec, ss < 10 ? "0" : "", sizeof(sec));
    str_append(sec, nb, sizeof(sec));
    ttf_draw_string(buf, (int)w, (int)h, x + (gw + tw) / 2 + 6, y + 44,
                    sec, C_GOLD_DIM, 13);

    /* A second hand that sweeps: the one moving thing on an otherwise
     * still desktop, and the cheapest possible proof the clock is live. */
    const int32_t bx = x + 12, by = y + gh - 12, bw = gw - 24;
    gfx_rect(buf, w, h, bx, by, bw, 2, 0x1C2130u);
    gfx_rect(buf, w, h, bx, by, bw * ss / 59, 2, C_GOLD_DIM);
}

static void gadget_draw_system(uint32_t *buf, uint32_t w, uint32_t h,
                               int32_t x, int32_t y, int32_t gw, int32_t gh) {
    gadget_plate(buf, w, h, x, y, gw, gh, "SYSTEM");

    char nb[16], line[40];

    ttf_draw_string(buf, (int)w, (int)h, x + 12, y + 26, "cpu", C_TEXT_DIM, 11);
    uint_to_str(sys_busy_pct, nb);
    str_copy(line, nb, sizeof(line));
    str_append(line, "%", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, x + gw - 12 - ttf_text_width(line, 11),
                    y + 26, line, C_TEXT, 11);
    gadget_meter(buf, w, h, x + 12, y + 42, gw - 24, 6, sys_busy_pct,
                 sys_busy_pct > 80 ? C_RED : C_GOLD);

    /* the history, oldest at the left */
    const int32_t hx = x + 12, hy = y + 56, hw = gw - 24, hh2 = 20;
    gfx_rect(buf, w, h, hx, hy, hw, hh2, 0x141824u);
    for (int i = 0; i < BUSY_HIST; i++) {
        const int idx = (sys_busy_pos + i) % BUSY_HIST;
        const int32_t bh = (int32_t)sys_busy_hist[idx] * hh2 / 100;
        const int32_t bx = hx + i * hw / BUSY_HIST;
        if (bh > 0)
            gfx_rect(buf, w, h, bx, hy + hh2 - bh, hw / BUSY_HIST - 1, bh,
                     gfx_mix(C_GOLD, 0x141824u, 150));
    }

    const uint32_t secs = desktop_tick / 60;
    str_copy(line, "up ", sizeof(line));
    uint_to_str(secs / 3600, nb); str_append(line, nb, sizeof(line));
    str_append(line, "h ", sizeof(line));
    uint_to_str((secs / 60) % 60, nb); str_append(line, nb, sizeof(line));
    str_append(line, "m", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, x + 12, y + gh - 20, line,
                    C_TEXT_DIM, 11);

    uint_to_str((uint32_t)system_total_memory_mb, nb);
    str_copy(line, nb, sizeof(line));
    str_append(line, " MB", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h,
                    x + gw - 12 - ttf_text_width(line, 11), y + gh - 20,
                    line, C_TEXT_DIM, 11);
}

static void gadget_draw_network(uint32_t *buf, uint32_t w, uint32_t h,
                                int32_t x, int32_t y, int32_t gw, int32_t gh) {
    gadget_plate(buf, w, h, x, y, gw, gh, "NETWORK");

    /* Same test the system monitor makes, so the gadget and the window
     * can never disagree about whether the cable is in. */
    const int up = e1000_found &&
                   (e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0;
    ttf_draw_string(buf, (int)w, (int)h, x + 12, y + 28,
                    up ? "link up" : "no link", up ? C_GREEN : C_RED, 12);

    if (up) {
        char ipb[24];
        ip_to_str(net_our_ip, ipb);
        ttf_draw_string_clip(buf, (int)w, (int)h, x + 12, y + 48, ipb,
                             C_TEXT, 12, x + gw - 12);
        ttf_draw_string_clip(buf, (int)w, (int)h, x + 12, y + 66,
                             tcp_state_names[tcp_state], C_TEXT_DIM, 11,
                             x + gw - 12);
    } else {
        ttf_draw_string(buf, (int)w, (int)h, x + 12, y + 48,
                        "no adapter found", C_TEXT_DIM, 11);
    }
}

static void gadgets_draw(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!gadgets_on) return;
    int32_t x, y, gw, gh;
    if (gadget_rect(GADGET_CLOCK, w, &x, &y, &gw, &gh))
        gadget_draw_clock(buf, w, h, x, y, gw, gh);
    if (gadget_rect(GADGET_SYSTEM, w, &x, &y, &gw, &gh))
        gadget_draw_system(buf, w, h, x, y, gw, gh);
    if (gadget_rect(GADGET_NETWORK, w, &x, &y, &gw, &gh))
        gadget_draw_network(buf, w, h, x, y, gw, gh);
}

#endif /* VEXTRO_SHELL_H */
