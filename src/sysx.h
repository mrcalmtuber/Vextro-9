#ifndef SYSX_H
#define SYSX_H

/*
 * src/sysx.h — the small things a desktop is made of.
 *
 * None of what follows is a subsystem. Each is a detail that is invisible
 * when it is right and conspicuous when it is missing: a pointer that
 * survives a repaint, video that is the right colour, hidden windows
 * that do not cost anything to not draw, a clock that knows about summer,
 * a keyboard that can type an accent, a log of what went wrong, and a
 * shutdown that asks before it kills.
 */

#include <stdint.h>
#include "gfx.h"

/* ===== YUV TO RGB =====
 *
 * Every video codec worth the name produces YUV rather than RGB, because
 * the eye is far more sensitive to brightness than to colour and YUV
 * lets the colour be stored at half resolution or less for almost no
 * visible cost. Displaying it means converting, per pixel, at frame
 * rate -- which is exactly the sort of loop that has to be a table
 * rather than arithmetic.
 *
 * The coefficients are ITU-R BT.601, which is what everything that
 * predates high definition uses. The clamps are not optional: legal YUV
 * describes colours outside the RGB cube, and a value that wraps instead
 * of clamping turns a bright highlight into a dark blotch.
 */
static int16_t yuv_r_v[256];
static int16_t yuv_g_u[256], yuv_g_v[256];
static int16_t yuv_b_u[256];
static uint8_t yuv_clamp[1024];          /* indexed by value + 384 */

static void yuv_init(void) {
    for (int i = 0; i < 256; i++) {
        int c = i - 128;
        yuv_r_v[i] = (int16_t)((91881 * c) >> 16);
        yuv_g_u[i] = (int16_t)((22554 * c) >> 16);
        yuv_g_v[i] = (int16_t)((46802 * c) >> 16);
        yuv_b_u[i] = (int16_t)((116130 * c) >> 16);
    }
    for (int i = 0; i < 1024; i++) {
        int v = i - 384;
        yuv_clamp[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static inline uint32_t yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v) {
    int c = y;
    int r = c + yuv_r_v[v];
    int g = c - yuv_g_u[u] - yuv_g_v[v];
    int b = c + yuv_b_u[u];
    return ((uint32_t)yuv_clamp[r + 384] << 16) |
           ((uint32_t)yuv_clamp[g + 384] << 8) |
            (uint32_t)yuv_clamp[b + 384];
}

/*
 * A whole plane at once, for 4:2:0 -- one chroma sample per two-by-two
 * block of luma, which is what almost every codec emits. Two output rows
 * are produced per chroma row, which is why the loop is shaped this way
 * rather than as one pass per pixel.
 */
static void yuv420_to_rgb(uint32_t *dst, int dst_pitch,
                          const uint8_t *yp, int y_pitch,
                          const uint8_t *up, const uint8_t *vp, int c_pitch,
                          int w, int h) {
    for (int row = 0; row < h; row += 2) {
        const uint8_t *y0 = yp + (size_t)row * y_pitch;
        const uint8_t *y1 = y0 + y_pitch;
        const uint8_t *u  = up + (size_t)(row / 2) * c_pitch;
        const uint8_t *v  = vp + (size_t)(row / 2) * c_pitch;
        uint32_t *d0 = dst + (size_t)row * dst_pitch;
        uint32_t *d1 = d0 + dst_pitch;

        for (int col = 0; col < w; col += 2) {
            uint8_t cu = u[col / 2], cv = v[col / 2];
            d0[col]     = yuv_to_rgb(y0[col], cu, cv);
            if (col + 1 < w) d0[col + 1] = yuv_to_rgb(y0[col + 1], cu, cv);
            if (row + 1 < h) {
                d1[col] = yuv_to_rgb(y1[col], cu, cv);
                if (col + 1 < w) d1[col + 1] = yuv_to_rgb(y1[col + 1], cu, cv);
            }
        }
    }
}

/* ===== POINTERS =====
 *
 * The cursor was one shape, drawn from a character-art table, in two
 * colours. What a desktop needs is several shapes -- the shape is how a
 * window edge says it can be dragged -- and alpha, because a hard-edged
 * pointer over a busy background is hard to see and an antialiased one
 * is not.
 *
 * Each cursor is a small ARGB bitmap with a hot spot. Compositing it is
 * one alpha blend per pixel over whatever is underneath, which is what
 * makes it look like it is above the desktop rather than punched into
 * it.
 */
#define CUR_W 16
#define CUR_H 24

typedef enum {
    CUR_ARROW = 0,
    CUR_TEXT,
    CUR_RESIZE_H,
    CUR_RESIZE_V,
    CUR_RESIZE_D,
    CUR_HAND,
    CUR_WAIT,
    CUR_COUNT
} cursor_id_t;

typedef struct {
    uint8_t  hot_x, hot_y;
    uint8_t  w, h;
    uint32_t pix[CUR_W * CUR_H];       /* 0xAARRGGBB */
} cursor_t;

static cursor_t cursors[CUR_COUNT];
static int cursor_current = CUR_ARROW;
static int cursor_ready = 0;

static void cur_set(cursor_t *c, int x, int y, uint32_t argb) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->pix[y * CUR_W + x] = argb;
}

/*
 * The shapes are drawn rather than stored, so that changing one is
 * arithmetic rather than editing a table of characters -- and so the
 * antialiasing on the diagonals can be computed instead of guessed.
 */
static void cursors_init(void) {
    for (int i = 0; i < CUR_COUNT; i++) {
        cursors[i].w = CUR_W;
        cursors[i].h = CUR_H;
        cursors[i].hot_x = 0;
        cursors[i].hot_y = 0;
        for (int k = 0; k < CUR_W * CUR_H; k++) cursors[i].pix[k] = 0;
    }

    /* Arrow: a filled triangle with an outline and a tail. */
    {
        cursor_t *c = &cursors[CUR_ARROW];
        for (int y = 0; y < 18; y++) {
            int span = y < 12 ? y : (y < 16 ? 11 - (y - 12) * 2 : 0);
            for (int x = 0; x <= span; x++) cur_set(c, x, y, 0xFFF4F4F8u);
            if (span >= 0) {
                cur_set(c, 0, y, 0xFF101018u);
                cur_set(c, span + 1, y, 0xFF101018u);
                /* One softened pixel past the edge, so the diagonal is
                 * not a staircase. */
                cur_set(c, span + 2, y, 0x60101018u);
            }
        }
        for (int y = 12; y < 22; y++) {
            int x = 6 + (y - 12) / 2;
            cur_set(c, x, y, 0xFFF4F4F8u);
            cur_set(c, x + 1, y, 0xFFF4F4F8u);
            cur_set(c, x - 1, y, 0xFF101018u);
            cur_set(c, x + 2, y, 0xFF101018u);
        }
    }

    /* Text: an I-beam. */
    {
        cursor_t *c = &cursors[CUR_TEXT];
        c->hot_x = 4; c->hot_y = 9;
        for (int y = 2; y < 18; y++) cur_set(c, 4, y, 0xFFF0F0F4u);
        for (int x = 2; x <= 6; x++) {
            cur_set(c, x, 2, 0xFFF0F0F4u);
            cur_set(c, x, 17, 0xFFF0F0F4u);
        }
    }

    /* Resize handles: double-headed arrows. */
    {
        cursor_t *c = &cursors[CUR_RESIZE_H];
        c->hot_x = 8; c->hot_y = 8;
        for (int x = 1; x < 15; x++) cur_set(c, x, 8, 0xFFF0F0F4u);
        for (int i = 0; i < 4; i++) {
            cur_set(c, 1 + i, 8 - i, 0xFFF0F0F4u);
            cur_set(c, 1 + i, 8 + i, 0xFFF0F0F4u);
            cur_set(c, 14 - i, 8 - i, 0xFFF0F0F4u);
            cur_set(c, 14 - i, 8 + i, 0xFFF0F0F4u);
        }
    }
    {
        cursor_t *c = &cursors[CUR_RESIZE_V];
        c->hot_x = 8; c->hot_y = 11;
        for (int y = 3; y < 20; y++) cur_set(c, 8, y, 0xFFF0F0F4u);
        for (int i = 0; i < 4; i++) {
            cur_set(c, 8 - i, 3 + i, 0xFFF0F0F4u);
            cur_set(c, 8 + i, 3 + i, 0xFFF0F0F4u);
            cur_set(c, 8 - i, 19 - i, 0xFFF0F0F4u);
            cur_set(c, 8 + i, 19 - i, 0xFFF0F0F4u);
        }
    }
    {
        cursor_t *c = &cursors[CUR_RESIZE_D];
        c->hot_x = 8; c->hot_y = 8;
        for (int i = 0; i < 12; i++) cur_set(c, 2 + i, 2 + i, 0xFFF0F0F4u);
        for (int i = 0; i < 4; i++) {
            cur_set(c, 2 + i, 2, 0xFFF0F0F4u);
            cur_set(c, 2, 2 + i, 0xFFF0F0F4u);
            cur_set(c, 13 - i, 13, 0xFFF0F0F4u);
            cur_set(c, 13, 13 - i, 0xFFF0F0F4u);
        }
    }

    /* Hand: a blob with fingers, for links. */
    {
        cursor_t *c = &cursors[CUR_HAND];
        c->hot_x = 5; c->hot_y = 1;
        for (int y = 4; y < 16; y++)
            for (int x = 3; x < 12; x++) cur_set(c, x, y, 0xFFF0F0F4u);
        for (int y = 1; y < 6; y++) cur_set(c, 5, y, 0xFFF0F0F4u);
        for (int y = 2; y < 5; y++) { cur_set(c, 7, y, 0xFFF0F0F4u);
                                      cur_set(c, 9, y, 0xFFF0F0F4u); }
        for (int y = 3; y < 17; y++) { cur_set(c, 2, y, 0xFF101018u);
                                       cur_set(c, 12, y, 0xFF101018u); }
    }

    /* Wait: an hourglass. */
    {
        cursor_t *c = &cursors[CUR_WAIT];
        c->hot_x = 8; c->hot_y = 10;
        for (int x = 3; x < 14; x++) { cur_set(c, x, 2, 0xFFF0F0F4u);
                                       cur_set(c, x, 19, 0xFFF0F0F4u); }
        for (int i = 0; i < 8; i++) {
            cur_set(c, 3 + i, 3 + i, 0xFFF0F0F4u);
            cur_set(c, 13 - i, 3 + i, 0xFFF0F0F4u);
            cur_set(c, 3 + i, 18 - i, 0xFFF0F0F4u);
            cur_set(c, 13 - i, 18 - i, 0xFFF0F0F4u);
        }
        for (int y = 14; y < 19; y++)
            for (int x = 5; x < 12; x++) cur_set(c, x, y, 0xFFD4AF37u);
    }

    cursor_ready = 1;
}

static void cursor_select(int which) {
    if (which >= 0 && which < CUR_COUNT) cursor_current = which;
}

/* Composite the current pointer at (mx, my), alpha blended. */
static void cursor_draw(uint32_t *buf, uint32_t bw, uint32_t bh,
                        int32_t mx, int32_t my) {
    if (!cursor_ready) return;
    const cursor_t *c = &cursors[cursor_current];
    int32_t ox = mx - c->hot_x, oy = my - c->hot_y;

    for (int y = 0; y < c->h; y++) {
        int32_t py = oy + y;
        if (py < 0 || py >= (int32_t)bh) continue;
        uint32_t *row = buf + (uint32_t)py * bw;
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->pix[y * CUR_W + x];
            uint32_t a = p >> 24;
            if (!a) continue;
            int32_t px = ox + x;
            if (px < 0 || px >= (int32_t)bw) continue;
            row[px] = (a == 255) ? (p & 0x00FFFFFFu)
                                 : gfx_mix(p & 0x00FFFFFFu, row[px], a);
        }
    }
}

/* ===== OCCLUSION =====
 *
 * A window entirely behind another does not need drawing, and on a
 * desktop with several open that is most of them. The compositor drew
 * every window bottom to top regardless, so the cost of having windows
 * open was linear in how many there were rather than in how much of them
 * could be seen.
 *
 * Full occlusion only -- a window is skipped if a single window above it
 * covers it completely. Partial occlusion needs a region algebra to
 * express what is left, and the gain over this is small: the common case
 * is one maximised window over everything else, and that is exactly what
 * this catches.
 */
typedef struct { int32_t x, y, w, h; } rect_t;

static inline int rect_contains(const rect_t *outer, const rect_t *inner) {
    return inner->x >= outer->x && inner->y >= outer->y &&
           inner->x + inner->w <= outer->x + outer->w &&
           inner->y + inner->h <= outer->y + outer->h;
}

/*
 * `stack` is bottom to top. Returns a bitmask of which entries can be
 * skipped -- so the caller keeps its own ordering and only asks what to
 * leave out.
 */
static uint32_t occlusion_mask(const rect_t *stack, int n) {
    uint32_t hidden = 0;
    for (int i = 0; i < n && i < 32; i++) {
        if (stack[i].w <= 0 || stack[i].h <= 0) { hidden |= 1u << i; continue; }
        for (int j = i + 1; j < n && j < 32; j++) {
            if (hidden & (1u << j)) continue;
            if (rect_contains(&stack[j], &stack[i])) {
                hidden |= 1u << i;
                break;
            }
        }
    }
    return hidden;
}

/* ===== MULTIPLE DISPLAYS =====
 *
 * One framebuffer was assumed everywhere. A machine with two panels has
 * two, at different resolutions, and a window can be on either or across
 * both -- which means "the screen" stops being a single rectangle and
 * becomes a coordinate space with holes in it.
 *
 * What is recorded here is the topology: where each panel sits in that
 * space, so that a window's position can be resolved to a panel and
 * clipped to it. Limine reports every framebuffer it found; this is
 * what turns a list of them into a desktop.
 */
#define DISPLAY_MAX 4

typedef struct {
    volatile uint32_t *fb;
    uint32_t w, h, pitch_px;
    int32_t  origin_x, origin_y;      /* position in the desktop space */
    int      primary;
} display_t;

static display_t displays[DISPLAY_MAX];
static int display_count = 0;
static int32_t desktop_w = 0, desktop_h = 0;

static void display_add(volatile uint32_t *fb, uint32_t w, uint32_t h,
                        uint32_t pitch_px) {
    if (display_count >= DISPLAY_MAX) return;
    display_t *d = &displays[display_count];
    d->fb = fb;
    d->w = w;
    d->h = h;
    d->pitch_px = pitch_px;
    /* Laid out left to right in the order the firmware reported them,
     * which is the only ordering available without asking a person. */
    d->origin_x = desktop_w;
    d->origin_y = 0;
    d->primary = (display_count == 0);
    display_count++;

    desktop_w += (int32_t)w;
    if ((int32_t)h > desktop_h) desktop_h = (int32_t)h;
}

/* Which panel is this point on? -1 if it is in a gap between them. */
static int display_at(int32_t x, int32_t y) {
    for (int i = 0; i < display_count; i++) {
        display_t *d = &displays[i];
        if (x >= d->origin_x && x < d->origin_x + (int32_t)d->w &&
            y >= d->origin_y && y < d->origin_y + (int32_t)d->h)
            return i;
    }
    return -1;
}

/* ===== SUMMER TIME =====
 *
 * The CMOS clock keeps one time and does not know what to call it. The
 * rules below are the European and North American ones, which between
 * them cover where this is likely to be run; a machine elsewhere can set
 * the offset by hand and the rules are ignored.
 *
 * The interesting part is that the rule is stated in local time -- "the
 * last Sunday in March" -- so applying it requires knowing the date,
 * which requires knowing the offset. The circularity is resolved the way
 * everyone resolves it: evaluate the rule against standard time.
 */
#define DST_NONE   0
#define DST_EUROPE 1
#define DST_US     2

static int dst_rule = DST_EUROPE;
static int dst_base_offset_min = 0;      /* standard time from UTC */
static int dst_active = 0;

/* Zeller, for the day of the week: 0 is Saturday. */
static int dst_dow(int y, int m, int d) {
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    return (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
}

/* The date of the last given weekday in a month. */
static int dst_last_dow(int y, int m, int want_dow) {
    static const int len[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    int days = len[m];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days = 29;
    for (int d = days; d > days - 7; d--)
        if (dst_dow(y, m, d) == want_dow) return d;
    return days;
}

static int dst_nth_dow(int y, int m, int want_dow, int nth) {
    int found = 0;
    for (int d = 1; d <= 31; d++) {
        if (dst_dow(y, m, d) != want_dow) continue;
        if (++found == nth) return d;
    }
    return 1;
}

/*
 * Is summer time in force? `hour` is standard local time.
 *
 * Europe: last Sunday in March to last Sunday in October.
 * North America: second Sunday in March to first Sunday in November.
 */
static int dst_in_effect(int y, int mo, int d, int hour) {
    if (dst_rule == DST_NONE) return 0;
    const int SUNDAY = 1;                /* Zeller: 0 Saturday, 1 Sunday */

    if (dst_rule == DST_EUROPE) {
        if (mo < 3 || mo > 10) return 0;
        if (mo > 3 && mo < 10) return 1;
        if (mo == 3) {
            int start = dst_last_dow(y, 3, SUNDAY);
            return d > start || (d == start && hour >= 1);
        }
        int end = dst_last_dow(y, 10, SUNDAY);
        return d < end || (d == end && hour < 1);
    }

    if (mo < 3 || mo > 11) return 0;
    if (mo > 3 && mo < 11) return 1;
    if (mo == 3) {
        int start = dst_nth_dow(y, 3, SUNDAY, 2);
        return d > start || (d == start && hour >= 2);
    }
    int end = dst_nth_dow(y, 11, SUNDAY, 1);
    return d < end || (d == end && hour < 2);
}

/* Minutes to add to the hardware clock to get local time. */
static int dst_offset(int y, int mo, int d, int hour) {
    dst_active = dst_in_effect(y, mo, d, hour);
    return dst_base_offset_min + (dst_active ? 60 : 0);
}

/* ===== MULTILINGUAL INPUT =====
 *
 * A keyboard produces one key at a time and many languages need more
 * characters than a keyboard has keys. The oldest answer, and the one
 * that needs no window of candidates, is the dead key: a key that
 * produces nothing on its own and modifies the next one.
 *
 * This is a two-state machine and that is the whole of it. Press the
 * accent, then the letter, get the accented letter; press the accent
 * twice and get the accent itself, which is how the accent is typed at
 * all.
 */
#define IME_LAYOUT_US       0
#define IME_LAYOUT_INTL     1
#define IME_LAYOUT_UK       2

static int  ime_layout = IME_LAYOUT_US;
static char ime_pending = 0;

typedef struct {
    char dead;
    char base;
    char result;      /* the code page 437 byte, which is what the font has */
} ime_compose_t;

static const ime_compose_t ime_table[] = {
    { '\'', 'a', (char)0xA0 }, { '\'', 'e', (char)0x82 },
    { '\'', 'i', (char)0xA1 }, { '\'', 'o', (char)0xA2 },
    { '\'', 'u', (char)0xA3 }, { '\'', 'y', (char)0x98 },
    { '`',  'a', (char)0x85 }, { '`',  'e', (char)0x8A },
    { '`',  'i', (char)0x8D }, { '`',  'o', (char)0x95 },
    { '`',  'u', (char)0x97 },
    { '^',  'a', (char)0x83 }, { '^',  'e', (char)0x88 },
    { '^',  'i', (char)0x8C }, { '^',  'o', (char)0x93 },
    { '^',  'u', (char)0x96 },
    { '"',  'a', (char)0x84 }, { '"',  'e', (char)0x89 },
    { '"',  'i', (char)0x8B }, { '"',  'o', (char)0x94 },
    { '"',  'u', (char)0x81 }, { '"',  'y', (char)0x98 },
    { '~',  'n', (char)0xA4 }, { '~',  'N', (char)0xA5 },
    { '\'', 'A', (char)0xB5 }, { '\'', 'E', (char)0x90 },
    { '"',  'A', (char)0x8E }, { '"',  'O', (char)0x99 },
    { '"',  'U', (char)0x9A },
    { 0, 0, 0 }
};

static int ime_is_dead(char c) {
    if (ime_layout != IME_LAYOUT_INTL) return 0;
    return c == '\'' || c == '`' || c == '^' || c == '"' || c == '~';
}

/*
 * Feed a key in, get zero or one character out.
 *
 * Returns 0 when the key was swallowed -- which is what a dead key does,
 * and is why this returns a character rather than writing one: a caller
 * that assumed one key meant one character would emit the accent twice.
 */
static char ime_feed(char c) {
    if (ime_pending) {
        char dead = ime_pending;
        ime_pending = 0;
        if (c == dead) return dead;          /* doubled: the accent itself */
        for (int i = 0; ime_table[i].dead; i++)
            if (ime_table[i].dead == dead && ime_table[i].base == c)
                return ime_table[i].result;
        /* No combination: the accent stands alone and the letter
         * follows. The letter is returned and the accent is lost, which
         * is what every implementation does and what people expect. */
        return c;
    }
    if (ime_is_dead(c)) { ime_pending = c; return 0; }
    return c;
}

static const char *ime_layout_name(void) {
    switch (ime_layout) {
    case IME_LAYOUT_INTL: return "US International";
    case IME_LAYOUT_UK:   return "United Kingdom";
    default:              return "US";
    }
}

/* ===== EVENT LOG =====
 *
 * Everything that goes wrong goes to the serial port, which is perfect
 * for somebody with a cable and useless for somebody with a laptop. A
 * ring of recent events that the interface can show is what makes a
 * driver failure something a person can find out about.
 */
#define EVLOG_ENTRIES 64
#define EVLOG_TEXT    96

#define EV_INFO  0
#define EV_WARN  1
#define EV_ERROR 2

typedef struct {
    uint32_t seq;
    uint32_t tick;
    uint8_t  level;
    char     source[16];
    char     text[EVLOG_TEXT];
} evlog_entry_t;

static evlog_entry_t evlog[EVLOG_ENTRIES];
static uint32_t evlog_next = 0;
static uint32_t evlog_seq = 0;
static uint32_t evlog_errors = 0;

static void evlog_add(uint8_t level, const char *source, const char *text) {
    evlog_entry_t *e = &evlog[evlog_next % EVLOG_ENTRIES];
    evlog_next++;
    e->seq   = ++evlog_seq;
    e->tick  = sys_ticks;
    e->level = level;
    int i = 0;
    while (source[i] && i < 15) { e->source[i] = source[i]; i++; }
    e->source[i] = '\0';
    i = 0;
    while (text[i] && i < EVLOG_TEXT - 1) { e->text[i] = text[i]; i++; }
    e->text[i] = '\0';
    if (level == EV_ERROR) evlog_errors++;

    serial_puts(level == EV_ERROR ? "[event] error: "
              : level == EV_WARN  ? "[event] warning: "
                                  : "[event] ");
    serial_puts(source);
    serial_puts(": ");
    serial_puts(text);
    serial_putc('\n');
}

/* Most recent first, which is the order anybody reads a log in. */
static const evlog_entry_t *evlog_get(int back) {
    if (back < 0 || back >= EVLOG_ENTRIES) return 0;
    if ((uint32_t)back >= evlog_next) return 0;
    uint32_t idx = (evlog_next - 1 - (uint32_t)back) % EVLOG_ENTRIES;
    return &evlog[idx];
}

static int evlog_count(void) {
    return evlog_next < EVLOG_ENTRIES ? (int)evlog_next : EVLOG_ENTRIES;
}

/* ===== SHUTDOWN =====
 *
 * Killing every process and cutting the power loses whatever they had
 * not written. Asking first is a handshake: every process is told, given
 * a bounded time to finish, and only then stopped -- and the bound is
 * what stops one stuck program holding the machine on forever.
 */
#define SHUTDOWN_GRACE_TICKS 180        /* three seconds of frames */

typedef enum {
    SHUT_NONE = 0,
    SHUT_ASKING,
    SHUT_FORCING,
    SHUT_DONE
} shutdown_state_t;

static shutdown_state_t shutdown_state = SHUT_NONE;
static uint32_t shutdown_started = 0;
static int      shutdown_reboot = 0;
static int      shutdown_waiting = 0;

static void shutdown_begin(int reboot) {
    if (shutdown_state != SHUT_NONE) return;
    shutdown_state = SHUT_ASKING;
    shutdown_started = sys_ticks;
    shutdown_reboot = reboot;
    evlog_add(EV_INFO, "shutdown",
              reboot ? "restart requested; asking programs to finish"
                     : "shutdown requested; asking programs to finish");
}

static const char *shutdown_message(void) {
    switch (shutdown_state) {
    case SHUT_ASKING:
        return shutdown_waiting
             ? "Waiting for programs to finish..."
             : "Closing down...";
    case SHUT_FORCING: return "Some programs did not finish. Stopping them.";
    case SHUT_DONE:    return "It is now safe to turn off this machine.";
    default:           return "";
    }
}

#endif /* SYSX_H */
