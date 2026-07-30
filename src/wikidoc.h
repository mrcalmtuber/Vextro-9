#ifndef WIKIDOC_H
#define WIKIDOC_H

/*
 * A reading layout for encyclopedia articles.
 *
 * The browser's HTML engine already renders ZIM records, and articles came
 * out of it shredded. The cause is structural rather than a missing case:
 * `brw_add_word` ends the current line whenever the style changes, and a
 * Wikipedia sentence is mostly anchors, so entering and leaving each link
 * broke the line. "The Moon is Earth's only natural satellite" arrived as
 * four lines, one of them a single word — and, because every fragment
 * costs a line, the 700-line document cap was reached a few paragraphs in
 * and the rest of the article was silently dropped.
 *
 * The browser's model cannot express the fix. Its line carries exactly one
 * style and one href, so a sentence containing a link *cannot* be one
 * line. This is the smallest model that can: a line owns a span of runs,
 * and a run is the thing that carries style and link. Changing style now
 * starts a run, not a line.
 *
 * Font size is a property of the line, not the run. Headings therefore
 * size the whole line while bold, italic and links only change colour and
 * weight — which keeps every word on a line measured in one font, so
 * wrapping stays correct without a second measuring pass.
 *
 * Deliberately separate from browser.h rather than an extension of it: the
 * browser keeps its document in one set of globals, so sharing the engine
 * would mean the Wikipedia window and the browser window overwriting each
 * other's text whenever both are open.
 */

/* Line styles — these select the font size and line height. */
#define WD_BODY  0
#define WD_H1    1
#define WD_H2    2
#define WD_H3    3
#define WD_MONO  4
#define WD_DIM   5      /* captions, image placeholders */
#define WD_RULE  6      /* a horizontal rule; carries no text */
#define WD_NSTYLE 7

/* Run styles — these select colour and weight within a line. */
#define WR_NORM  0
#define WR_BOLD  1
#define WR_ITAL  2
#define WR_LINK  3
#define WR_DIM   4

/*
 * Capacities. An article runs to a few thousand lines once it is laid out
 * properly, so the old 700 would still have truncated. Roughly 730 KB of
 * .bss all told, against the 8.3 MB already spent on the frame buffers and
 * the 12 MB ZIM decompression window.
 */
#define WD_LINE_MAX  2400
#define WD_RUN_MAX   9000
#define WD_TEXT_MAX  (512 * 1024)
#define WD_HREF_MAX  512
#define WD_HREF_LEN  128
#define WD_TITLE_MAX 96

typedef struct {
    uint32_t start;          /* offset into wd_text            */
    uint16_t len;
    uint8_t  style;          /* WR_*                           */
    int16_t  href;           /* index into wd_hrefs, -1 = none */
} wd_run_t;

typedef struct {
    uint32_t run0;
    uint16_t nrun;
    uint8_t  style;          /* WD_*                           */
    int16_t  indent;         /* left inset in pixels           */
    uint8_t  pad_top;        /* extra space above, in pixels   */
} wd_line_t;

static wd_line_t wd_lines[WD_LINE_MAX];
static wd_run_t  wd_runs[WD_RUN_MAX];
static char      wd_text[WD_TEXT_MAX];
static char      wd_hrefs[WD_HREF_MAX][WD_HREF_LEN];
static char      wd_title[WD_TITLE_MAX];

static int wd_line_n, wd_run_n, wd_href_n;
static uint32_t wd_text_n;
static int wd_total_h;
static int wd_truncated;

static const int wd_size[WD_NSTYLE]  = { 15, 25, 20, 17, 14, 13, 15 };
static const int wd_lineh[WD_NSTYLE] = { 21, 36, 29, 24, 19, 19, 14 };

/* ===== builder state ===== */

static int      wdb_wrap_px;         /* usable width for text          */
static int      wdb_line_style;      /* style of the line being built  */
static int      wdb_indent;
static int      wdb_pad;             /* pending top padding            */
static int      wdb_px;              /* width used by the current line */
static int      wdb_run0, wdb_nrun;  /* runs belonging to it           */
static int      wdb_open_run;        /* index of the run being extended, -1 */
static int      wdb_cur_rstyle;
static int      wdb_cur_href;

static void wdoc_reset(void) {
    wd_line_n = wd_run_n = wd_href_n = 0;
    wd_text_n = 0;
    wd_total_h = 0;
    wd_truncated = 0;
    wd_title[0] = '\0';

    wdb_line_style = WD_BODY;
    wdb_indent = wdb_pad = wdb_px = 0;
    wdb_run0 = wdb_nrun = 0;
    wdb_open_run = -1;
    wdb_cur_rstyle = WR_NORM;
    wdb_cur_href = -1;
}

/* Intern an href, returning its index. Duplicates are shared: an article
 * links to the same target many times and the table is small. */
static int wd_href_intern(const char *s) {
    if (!s || !s[0]) return -1;
    for (int i = 0; i < wd_href_n; i++)
        if (str_eq(wd_hrefs[i], s)) return i;
    if (wd_href_n >= WD_HREF_MAX) return -1;
    str_copy(wd_hrefs[wd_href_n], s, WD_HREF_LEN);
    return wd_href_n++;
}

/* Close the line under construction. Empty lines are dropped unless they
 * are a rule, so stray block tags cost nothing. */
static void wd_line_flush(void) {
    if (wdb_nrun == 0 && wdb_line_style != WD_RULE) {
        /* Nothing on it, but remember any spacing it asked for. */
        wdb_px = 0;
        wdb_open_run = -1;
        return;
    }
    if (wd_line_n >= WD_LINE_MAX) { wd_truncated = 1; return; }

    wd_line_t *l = &wd_lines[wd_line_n++];
    l->run0    = (uint32_t)wdb_run0;
    l->nrun    = (uint16_t)wdb_nrun;
    l->style   = (uint8_t)wdb_line_style;
    l->indent  = (int16_t)wdb_indent;
    l->pad_top = (uint8_t)(wdb_pad > 255 ? 255 : wdb_pad);

    wdb_pad = 0;
    wdb_px = 0;
    wdb_run0 = wd_run_n;
    wdb_nrun = 0;
    wdb_open_run = -1;
}

/* Append raw characters to the current run, opening one if needed. */
static void wd_emit(const char *s, int n) {
    if (n <= 0) return;
    if (wd_text_n + (uint32_t)n >= WD_TEXT_MAX) { wd_truncated = 1; return; }

    if (wdb_open_run < 0) {
        if (wd_run_n >= WD_RUN_MAX) { wd_truncated = 1; return; }
        wd_run_t *r = &wd_runs[wd_run_n];
        r->start = wd_text_n;
        r->len   = 0;
        r->style = (uint8_t)wdb_cur_rstyle;
        r->href  = (int16_t)wdb_cur_href;
        wdb_open_run = wd_run_n++;
        if (wdb_nrun == 0) wdb_run0 = wdb_open_run;
        wdb_nrun++;
    }
    for (int i = 0; i < n; i++) wd_text[wd_text_n++] = s[i];
    wd_runs[wdb_open_run].len = (uint16_t)
        (wd_text_n - wd_runs[wdb_open_run].start);
}

/*
 * Place one word, wrapping if it does not fit.
 *
 * Everything on a line shares the line's font size, so a single
 * measurement per word is enough and the result cannot disagree with what
 * the draw pass computes.
 */
static void wd_add_word(const char *word, int wlen) {
    if (wlen <= 0) return;
    if (wlen > 120) wlen = 120;

    char tmp[128];
    for (int i = 0; i < wlen; i++) tmp[i] = word[i];
    tmp[wlen] = '\0';

    int size = wd_size[wdb_line_style];
    int wpx  = ttf_text_width(tmp, size);
    int spx  = (wdb_nrun > 0 || wdb_px > 0) ? ttf_text_width(" ", size) : 0;
    int avail = wdb_wrap_px - wdb_indent;
    if (avail < 60) avail = 60;

    if (wdb_px > 0 && wdb_px + spx + wpx > avail) {
        int keep_style = wdb_line_style, keep_indent = wdb_indent;
        wd_line_flush();
        wdb_line_style = keep_style;
        wdb_indent = keep_indent;
        spx = 0;
    }

    if (spx > 0) { wd_emit(" ", 1); wdb_px += spx; }
    wd_emit(tmp, wlen);
    wdb_px += wpx;
}

/* Switching inline style ends the run, never the line. This is the whole
 * point of the run model. */
static void wd_set_run_style(int style, int href) {
    if (style == wdb_cur_rstyle && href == wdb_cur_href) return;
    wdb_cur_rstyle = style;
    wdb_cur_href   = href;
    wdb_open_run   = -1;        /* next emit opens a fresh run */
}

/* Start a block: end the current line and remember the spacing wanted. */
static void wd_block(int style, int indent, int pad) {
    wd_line_flush();
    wdb_line_style = style;
    wdb_indent = indent;
    if (pad > wdb_pad) wdb_pad = pad;
}

/* ===== entities ===== */

static const struct { const char *name; uint32_t cp; } wd_ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0x00A0 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "times", 0x00D7 }, { "middot", 0x00B7 }, { "bull", 0x2022 },
    { "deg", 0x00B0 }, { "prime", 0x2032 }, { "Prime", 0x2033 },
    { "minus", 0x2212 }, { "plusmn", 0x00B1 }, { "frac12", 0x00BD },
    { "eacute", 0x00E9 }, { "egrave", 0x00E8 }, { "uuml", 0x00FC },
    { "ouml", 0x00F6 }, { "auml", 0x00E4 }, { "ccedil", 0x00E7 },
    { "szlig", 0x00DF }, { "aring", 0x00E5 }, { "oslash", 0x00F8 },
    { 0, 0 }
};

static int wd_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decode one entity beginning at src[i] (which is '&').
 *
 * Returns the folded ASCII character, or 0 to emit nothing, and advances
 * *pi past the entity. If it does not look like an entity the '&' is
 * returned literally — encyclopedia text contains bare ampersands.
 *
 * Hexadecimal forms are handled here; the browser's decoder rejects them,
 * which is why `&#x2014;` used to come out as '?'.
 */
static char wd_entity(const uint8_t *src, int len, int *pi) {
    int i = *pi + 1, j = i;
    while (j < len && j < i + 12 && src[j] != ';' && src[j] != ' ' &&
           src[j] != '<' && src[j] != '&')
        j++;
    if (j >= len || src[j] != ';') { (*pi)++; return '&'; }

    uint32_t cp = 0;
    if (src[i] == '#') {
        int k = i + 1;
        if (k < j && (src[k] == 'x' || src[k] == 'X')) {
            k++;
            if (k >= j) { (*pi)++; return '&'; }
            for (; k < j; k++) {
                int v = wd_hexval((char)src[k]);
                if (v < 0) { (*pi)++; return '&'; }
                cp = cp * 16 + (uint32_t)v;
            }
        } else {
            if (k >= j) { (*pi)++; return '&'; }
            for (; k < j; k++) {
                if (src[k] < '0' || src[k] > '9') { (*pi)++; return '&'; }
                cp = cp * 10 + (uint32_t)(src[k] - '0');
            }
        }
    } else {
        char nm[16];
        int n = 0;
        for (int k = i; k < j && n < 15; k++) nm[n++] = (char)src[k];
        nm[n] = '\0';
        int found = 0;
        for (int e = 0; wd_ents[e].name; e++)
            if (str_eq(nm, wd_ents[e].name)) { cp = wd_ents[e].cp; found = 1; break; }
        if (!found) { (*pi)++; return '&'; }
    }

    *pi = j + 1;
    return brw_fold_cp(cp);
}

/* ===== tag helpers ===== */

static int wd_ci_eq(const char *a, const char *b) {
    for (int i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/*
 * Pull an attribute value out of a tag's attribute region.
 *
 * Good enough for the two attributes that matter — href and class — and
 * tolerant of unquoted values, which mwoffliner output does contain.
 */
static int wd_attr(const uint8_t *s, int n, const char *want,
                   char *out, int outmax) {
    int wl = 0;
    while (want[wl]) wl++;
    out[0] = '\0';

    for (int i = 0; i + wl < n; i++) {
        if (i > 0 && s[i - 1] != ' ' && s[i - 1] != '\t' &&
            s[i - 1] != '\n' && s[i - 1] != '\r') continue;
        int k = 0;
        while (k < wl) {
            char c = (char)s[i + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != want[k]) break;
            k++;
        }
        if (k != wl) continue;

        int j = i + wl;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j >= n || s[j] != '=') continue;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j >= n) return 0;

        char q = 0;
        if (s[j] == '"' || s[j] == '\'') { q = (char)s[j]; j++; }

        int o = 0;
        while (j < n && o < outmax - 1) {
            char c = (char)s[j];
            if (q && c == q) break;
            if (!q && (c == ' ' || c == '>' || c == '\t')) break;
            out[o++] = c;
            j++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* Does a class attribute contain one of Wikipedia's chrome markers? */
static int wd_class_is_chrome(const char *cls) {
    static const char *junk[] = {
        "mw-editsection", "navbox", "metadata", "reference", "reflist",
        "noprint", "mw-jump-link", "toc", "sistersitebox", "ambox",
        "hatnote", "mbox", "portal", "catlinks", "printfooter", 0
    };
    for (int j = 0; junk[j]; j++) {
        const char *needle = junk[j];
        for (int i = 0; cls[i]; i++) {
            int k = 0;
            while (needle[k] && cls[i + k] == needle[k]) k++;
            if (!needle[k]) return 1;
        }
    }
    return 0;
}

/* ===== the parser ===== */

/*
 * Lay out an HTML article.
 *
 * `wrap_px` is passed in rather than read from a global, so layout can
 * never run against a width left over from a previous frame — a bug the
 * browser still has.
 */
static void wdoc_parse(const uint8_t *src, int len, int wrap_px) {
    wdoc_reset();
    wdb_wrap_px = wrap_px > 80 ? wrap_px : 80;

    /* element whose content is being discarded, and its nesting depth */
    char skip_name[24];
    int  skip_nest = 0;
    skip_name[0] = '\0';

    int list_depth = 0;
    int ol_counter[6];
    int ol_is_num[6];
    for (int i = 0; i < 6; i++) { ol_counter[i] = 0; ol_is_num[i] = 0; }

    int in_pre = 0;
    int in_body = 0;          /* ignore everything before <body> */
    int want_title = 0;
    int row_had_header = 0;   /* for th/td separators */
    int cell_open = 0;

    char word[128];
    int  wlen = 0;

    #define WD_FLUSH_WORD() do { if (wlen > 0) { wd_add_word(word, wlen); wlen = 0; } } while (0)

    for (int i = 0; i < len; ) {
        /* ---- comments and declarations ---- */
        if (src[i] == '<' && i + 1 < len && src[i + 1] == '!') {
            if (i + 3 < len && src[i + 2] == '-' && src[i + 3] == '-') {
                /* Scan for the real terminator. Stopping at the first '>'
                 * (as the browser does) spills any comment containing one
                 * into the text. */
                int j = i + 4;
                while (j + 2 < len &&
                       !(src[j] == '-' && src[j + 1] == '-' && src[j + 2] == '>'))
                    j++;
                i = (j + 2 < len) ? j + 3 : len;
            } else {
                int j = i + 2;
                while (j < len && src[j] != '>') j++;
                i = j + 1;
            }
            continue;
        }

        /* ---- tags ---- */
        if (src[i] == '<') {
            int j = i + 1;
            int closing = 0;
            if (j < len && src[j] == '/') { closing = 1; j++; }

            char name[24];
            int nl = 0;
            while (j < len && nl < 23 &&
                   ((src[j] >= 'a' && src[j] <= 'z') ||
                    (src[j] >= 'A' && src[j] <= 'Z') ||
                    (src[j] >= '0' && src[j] <= '9'))) {
                char c = (char)src[j];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                name[nl++] = c;
                j++;
            }
            name[nl] = '\0';

            int attr0 = j;
            while (j < len && src[j] != '>') {
                /* step over quoted attribute values so a '>' inside one
                 * does not end the tag early */
                if (src[j] == '"' || src[j] == '\'') {
                    char q = (char)src[j++];
                    while (j < len && src[j] != q) j++;
                }
                if (j < len) j++;
            }
            int attrn = j - attr0;
            int self_close = (attrn > 0 && src[j - 1] == '/');
            int next = (j < len) ? j + 1 : len;

            /* --- discarding a subtree --- */
            if (skip_nest > 0) {
                if (nl && wd_ci_eq(name, skip_name)) {
                    if (closing) skip_nest--;
                    else if (!self_close) skip_nest++;
                }
                i = next;
                continue;
            }

            if (nl == 0) { i = next; continue; }

            /* --- elements whose content is never shown --- */
            if (!closing &&
                (wd_ci_eq(name, "script") || wd_ci_eq(name, "style") ||
                 wd_ci_eq(name, "noscript") || wd_ci_eq(name, "svg") ||
                 wd_ci_eq(name, "template") || wd_ci_eq(name, "head"))) {
                if (!self_close) {
                    str_copy(skip_name, name, sizeof(skip_name));
                    skip_nest = 1;
                }
                i = next;
                continue;
            }

            /* --- Wikipedia chrome: edit links, navboxes, reference marks --- */
            if (!closing && !self_close && attrn > 0) {
                char cls[160];
                if (wd_attr(src + attr0, attrn, "class", cls, sizeof(cls)) &&
                    wd_class_is_chrome(cls)) {
                    str_copy(skip_name, name, sizeof(skip_name));
                    skip_nest = 1;
                    i = next;
                    continue;
                }
            }
            if (!closing && wd_ci_eq(name, "sup")) {
                /* Reference markers are noise in running prose even when
                 * they carry no class. */
                if (!self_close) {
                    str_copy(skip_name, name, sizeof(skip_name));
                    skip_nest = 1;
                }
                i = next;
                continue;
            }

            if (wd_ci_eq(name, "title")) {
                want_title = !closing;
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "body")) { in_body = 1; i = next; continue; }

            /* --- inline --- */
            if (wd_ci_eq(name, "a")) {
                WD_FLUSH_WORD();
                if (closing) {
                    wd_set_run_style(WR_NORM, -1);
                } else {
                    char href[WD_HREF_LEN];
                    if (wd_attr(src + attr0, attrn, "href", href, sizeof(href)) &&
                        href[0] && href[0] != '#')
                        wd_set_run_style(WR_LINK, wd_href_intern(href));
                }
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "b") || wd_ci_eq(name, "strong")) {
                WD_FLUSH_WORD();
                wd_set_run_style(closing ? WR_NORM : WR_BOLD, wdb_cur_href);
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "i") || wd_ci_eq(name, "em")) {
                WD_FLUSH_WORD();
                wd_set_run_style(closing ? WR_NORM : WR_ITAL, wdb_cur_href);
                i = next;
                continue;
            }

            /* --- images: say something is there --- */
            if (!closing && wd_ci_eq(name, "img")) {
                WD_FLUSH_WORD();
                char alt[80];
                int save = wdb_cur_rstyle;
                wd_set_run_style(WR_DIM, -1);
                if (wd_attr(src + attr0, attrn, "alt", alt, sizeof(alt)) && alt[0]) {
                    wd_add_word("[img:", 5);
                    int a = 0, s0 = 0;
                    while (alt[a]) {
                        if (alt[a] == ' ') { wd_add_word(alt + s0, a - s0); s0 = a + 1; }
                        a++;
                    }
                    if (a > s0) wd_add_word(alt + s0, a - s0);
                    wd_add_word("]", 1);
                } else {
                    wd_add_word("[image]", 7);
                }
                wd_set_run_style(save, wdb_cur_href);
                i = next;
                continue;
            }

            /* --- breaks and rules --- */
            if (wd_ci_eq(name, "br")) {
                WD_FLUSH_WORD();
                wd_block(wdb_line_style, wdb_indent, 0);
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "hr")) {
                WD_FLUSH_WORD();
                wd_line_flush();
                wdb_line_style = WD_RULE;
                wd_line_flush();
                wdb_line_style = WD_BODY;
                i = next;
                continue;
            }

            /* --- headings --- */
            if (nl == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
                WD_FLUSH_WORD();
                int lv = name[1] - '0';
                int st = lv == 1 ? WD_H1 : lv == 2 ? WD_H2 : WD_H3;
                if (closing) {
                    wd_block(WD_BODY, 0, 6);
                } else {
                    wd_set_run_style(WR_NORM, -1);
                    wd_block(st, 0, lv == 1 ? 10 : 14);
                }
                i = next;
                continue;
            }

            /* --- preformatted --- */
            if (wd_ci_eq(name, "pre")) {
                WD_FLUSH_WORD();
                in_pre = !closing;
                wd_block(closing ? WD_BODY : WD_MONO, closing ? 0 : 12, 8);
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "code") || wd_ci_eq(name, "tt") ||
                wd_ci_eq(name, "kbd")) {
                WD_FLUSH_WORD();
                i = next;
                continue;
            }

            /* --- lists --- */
            if (wd_ci_eq(name, "ul") || wd_ci_eq(name, "ol")) {
                WD_FLUSH_WORD();
                if (closing) {
                    if (list_depth > 0) list_depth--;
                    wd_block(WD_BODY, list_depth * 18, 4);
                } else {
                    if (list_depth < 6) {
                        ol_is_num[list_depth] = wd_ci_eq(name, "ol");
                        ol_counter[list_depth] = 0;
                        list_depth++;
                    }
                    wd_block(WD_BODY, list_depth * 18, 4);
                }
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "li")) {
                WD_FLUSH_WORD();
                if (!closing) {
                    int d = list_depth > 0 ? list_depth - 1 : 0;
                    wd_block(WD_BODY, (d + 1) * 18, 2);
                    int save = wdb_cur_rstyle;
                    wd_set_run_style(WR_DIM, -1);
                    if (ol_is_num[d]) {
                        ol_counter[d]++;
                        char num[8];
                        int v = ol_counter[d], n2 = 0;
                        char rev[8];
                        if (v == 0) rev[n2++] = '0';
                        while (v > 0 && n2 < 6) { rev[n2++] = (char)('0' + v % 10); v /= 10; }
                        int o = 0;
                        while (n2 > 0) num[o++] = rev[--n2];
                        num[o++] = '.';
                        num[o] = '\0';
                        wd_add_word(num, o);
                    } else {
                        wd_add_word("*", 1);
                    }
                    wd_set_run_style(save, wdb_cur_href);
                }
                i = next;
                continue;
            }

            /* --- definition lists --- */
            if (wd_ci_eq(name, "dt")) {
                WD_FLUSH_WORD();
                if (!closing) { wd_block(WD_BODY, 8, 6); wd_set_run_style(WR_BOLD, -1); }
                else wd_set_run_style(WR_NORM, -1);
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "dd")) {
                WD_FLUSH_WORD();
                if (!closing) wd_block(WD_BODY, 28, 2);
                i = next;
                continue;
            }

            /* --- tables ---
             * Cells were not block elements in the browser, so every field
             * of an infobox ran together into one paragraph. A row is a
             * line; a header cell is followed by ": " so infoboxes read as
             * "Key: value", and sibling data cells by " - ". */
            if (wd_ci_eq(name, "tr")) {
                WD_FLUSH_WORD();
                wd_block(WD_BODY, 8, 2);
                row_had_header = 0;
                cell_open = 0;
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "th") || wd_ci_eq(name, "td")) {
                WD_FLUSH_WORD();
                if (closing) {
                    cell_open = 0;
                    wd_set_run_style(WR_NORM, wdb_cur_href);
                } else {
                    if (cell_open || wdb_nrun > 0) {
                        int save = wdb_cur_rstyle;
                        wd_set_run_style(WR_DIM, -1);
                        wd_add_word(row_had_header ? ":" : "-", 1);
                        wd_set_run_style(save, wdb_cur_href);
                    }
                    if (wd_ci_eq(name, "th")) {
                        row_had_header = 1;
                        wd_set_run_style(WR_BOLD, -1);
                    }
                    cell_open = 1;
                }
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "caption") || wd_ci_eq(name, "figcaption")) {
                WD_FLUSH_WORD();
                wd_block(closing ? WD_BODY : WD_DIM, closing ? 0 : 12, 4);
                i = next;
                continue;
            }

            /* --- generic blocks --- */
            if (wd_ci_eq(name, "p") || wd_ci_eq(name, "div") ||
                wd_ci_eq(name, "section") || wd_ci_eq(name, "article") ||
                wd_ci_eq(name, "table") || wd_ci_eq(name, "dl") ||
                wd_ci_eq(name, "header") || wd_ci_eq(name, "footer") ||
                wd_ci_eq(name, "nav") || wd_ci_eq(name, "main") ||
                wd_ci_eq(name, "figure") || wd_ci_eq(name, "form")) {
                WD_FLUSH_WORD();
                wd_block(WD_BODY, 0, wd_ci_eq(name, "p") ? 8 : 4);
                i = next;
                continue;
            }
            if (wd_ci_eq(name, "blockquote")) {
                WD_FLUSH_WORD();
                wd_block(WD_BODY, closing ? 0 : 24, 6);
                i = next;
                continue;
            }

            i = next;
            continue;
        }

        /* ---- text ---- */
        {
            char c;
            if (src[i] == '&') {
                c = wd_entity(src, len, &i);
                if (!c) continue;
            } else if (src[i] >= 0x80) {
                /* decode UTF-8 and fold to the nearest ASCII the font has */
                uint32_t cp = 0;
                int extra = 0;
                uint8_t b = src[i];
                if ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; extra = 1; }
                else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; extra = 2; }
                else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; extra = 3; }
                else { i++; continue; }
                if (i + extra >= len) { i = len; continue; }
                for (int k = 1; k <= extra; k++)
                    cp = (cp << 6) | (uint32_t)(src[i + k] & 0x3Fu);
                i += extra + 1;
                c = brw_fold_cp(cp);
                if (!c) continue;
            } else {
                c = (char)src[i];
                i++;
            }

            /*
             * Inside a discarded element.
             *
             * The tag scanner has its own `skip_nest` gate, but tags were
             * never the problem: dropping <span class="mw-editsection">
             * and its closing tag while still emitting what sat between
             * them left the literal "[edit]" in the heading. The character
             * has already been decoded, so this only has to refuse to
             * store it.
             */
            if (skip_nest > 0) continue;

            if (!in_body && !want_title) continue;

            if (want_title) {
                int n = 0;
                while (wd_title[n]) n++;
                if (c != '\n' && c != '\r' && n < WD_TITLE_MAX - 1) {
                    wd_title[n] = c;
                    wd_title[n + 1] = '\0';
                }
                continue;
            }

            if (in_pre) {
                if (c == '\n') { WD_FLUSH_WORD(); wd_block(WD_MONO, 12, 0); continue; }
                if (c == '\t') c = ' ';
            }

            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                WD_FLUSH_WORD();
            } else if (wlen < 120) {
                word[wlen++] = c;
            }
        }
    }

    WD_FLUSH_WORD();
    wd_line_flush();
    #undef WD_FLUSH_WORD

    /* Total height, so the caller can size a scrollbar. */
    wd_total_h = 0;
    for (int i = 0; i < wd_line_n; i++)
        wd_total_h += wd_lines[i].pad_top + wd_lineh[wd_lines[i].style];
}

/* ===== drawing =====
 *
 * Run x-positions are computed here rather than stored, because they are
 * only needed for the handful of lines actually on screen, and the same
 * walk answers hit-testing. `hover` is a run index or -1.
 */

static uint32_t wd_run_colour(int style) {
    switch (style) {
    case WR_LINK: return C_LINK;
    case WR_DIM:  return 0x8A8F9Cu;
    case WR_BOLD: return 0x101319u;
    case WR_ITAL: return 0x33383Fu;
    default:      return C_INK;
    }
}

/*
 * Walk the visible lines. For each run this calls back with its rectangle,
 * which both the painter and the hit-tester use so they cannot disagree.
 */
typedef void (*wd_run_cb)(void *ctx, int run, int x, int y, int rw,
                          int size, int lstyle);

static void wd_walk(int x0, int y0, int scroll, int view_h,
                    wd_run_cb cb, void *ctx) {
    int y = y0 - scroll;
    for (int i = 0; i < wd_line_n; i++) {
        wd_line_t *l = &wd_lines[i];
        int size = wd_size[l->style];
        int lh   = wd_lineh[l->style];
        y += l->pad_top;

        if (y + lh >= y0 - 40 && y <= y0 + view_h + 40) {
            int x = x0 + l->indent;
            for (uint16_t k = 0; k < l->nrun; k++) {
                int ri = (int)l->run0 + k;
                wd_run_t *r = &wd_runs[ri];
                char tmp[256];
                int n = r->len < 255 ? r->len : 255;
                for (int t = 0; t < n; t++) tmp[t] = wd_text[r->start + t];
                tmp[n] = '\0';
                int rw = ttf_text_width(tmp, size);
                cb(ctx, ri, x, y, rw, size, l->style);
                x += rw;
            }
        }
        y += lh;
        if (y > y0 + view_h + 60) break;
    }
}

typedef struct {
    uint32_t *buf;
    uint32_t  w, h;
    int       hover;
    int       clip_y0, clip_y1;
} wd_paint_ctx;

static void wd_paint_run(void *vctx, int run, int x, int y, int rw,
                         int size, int lstyle) {
    wd_paint_ctx *c = (wd_paint_ctx *)vctx;
    wd_run_t *r = &wd_runs[run];
    if (y + size < c->clip_y0 || y > c->clip_y1) return;

    char tmp[256];
    int n = r->len < 255 ? r->len : 255;
    for (int t = 0; t < n; t++) tmp[t] = wd_text[r->start + t];
    tmp[n] = '\0';

    uint32_t col;
    if (lstyle == WD_H1 || lstyle == WD_H2 || lstyle == WD_H3)
        col = 0x1A1E28u;
    else if (lstyle == WD_DIM)
        col = 0x8A8F9Cu;
    else
        col = wd_run_colour(r->style);
    if (r->href >= 0) col = C_LINK;

    if (lstyle == WD_MONO)
        mono_text(c->buf, c->w, c->h, x, y, tmp, 0x30343Eu, 1);
    else
        ttf_draw_string(c->buf, (int)c->w, (int)c->h, x, y, tmp, col, size);

    /* Bold has no separate face in this font, so it is drawn twice a
     * pixel apart — the same trick the rest of the UI uses. */
    if (r->style == WR_BOLD && lstyle != WD_MONO)
        ttf_draw_string(c->buf, (int)c->w, (int)c->h, x + 1, y, tmp, col, size);

    if (r->href >= 0) {
        int uy = y + size + 2;
        if (uy >= c->clip_y0 && uy < c->clip_y1)
            gfx_rect(c->buf, c->w, c->h, x, uy, rw, 1,
                     run == c->hover ? C_GOLD : 0xC9B678u);
    }
}

static void wdoc_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int x0, int y0, int view_w, int view_h,
                      int scroll, int hover) {
    (void)view_w;
    wd_paint_ctx c;
    c.buf = buf; c.w = w; c.h = h;
    c.hover = hover;
    c.clip_y0 = y0;
    c.clip_y1 = y0 + view_h;
    wd_walk(x0, y0 + 4, scroll, view_h, wd_paint_run, &c);
}

typedef struct { int mx, my, hit; } wd_hit_ctx;

static void wd_hit_run(void *vctx, int run, int x, int y, int rw,
                       int size, int lstyle) {
    (void)lstyle;
    wd_hit_ctx *c = (wd_hit_ctx *)vctx;
    if (wd_runs[run].href < 0) return;
    if (c->mx >= x && c->mx < x + rw && c->my >= y && c->my < y + size + 4)
        c->hit = run;
}

/* Which link is under the pointer, or -1. */
static int wdoc_hit(int x0, int y0, int view_w, int view_h,
                    int scroll, int mx, int my) {
    (void)view_w;
    wd_hit_ctx c;
    c.mx = mx; c.my = my; c.hit = -1;
    wd_walk(x0, y0 + 4, scroll, view_h, wd_hit_run, &c);
    return c.hit;
}

static const char *wdoc_href_of(int run) {
    if (run < 0 || run >= wd_run_n) return "";
    int hi = wd_runs[run].href;
    if (hi < 0 || hi >= wd_href_n) return "";
    return wd_hrefs[hi];
}

#endif /* WIKIDOC_H */
