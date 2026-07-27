#ifndef BROWSER_H
#define BROWSER_H

/*
 * Socrates Browser — HTTP/1.0 + internal socrates:// pages.
 *
 * Pages are parsed into a flat list of styled, word-wrapped lines.
 * Lines can carry an href, making links clickable.  Loading is fully
 * asynchronous on top of the netstack HTTP client.
 */

#define BRW_ADDR_MAX   256
#define BRW_MAX_LINES  700
#define BRW_LINE_CHARS 200
#define BRW_HREF_MAX   120
#define BRW_TITLE_MAX  64

/* Line styles */
#define BS_BODY  0
#define BS_H1    1
#define BS_H2    2
#define BS_H3    3
#define BS_LINK  4
#define BS_PRE   5
#define BS_DIM   6
#define BS_RULE  7

typedef struct {
    char    text[BRW_LINE_CHARS];
    char    href[BRW_HREF_MAX];
    uint8_t style;
} brw_line_t;

static brw_line_t brw_lines[BRW_MAX_LINES];
static int   brw_line_count = 0;
static char  brw_addr[BRW_ADDR_MAX] = "socrates://home";
static int   brw_addr_len = 15;
static int   brw_addr_cur = 15;
static int   brw_addr_focus = 0;
static char  brw_title[BRW_TITLE_MAX] = "Socrates Browser";
static int   brw_scroll = 0;         /* px */
static int   brw_total_h = 0;        /* px */
static int   brw_loading = 0;
static char  brw_status[80] = "Ready";
static int   brw_hover_line = -1;
static int   brw_wrap_px = 700;      /* recomputed from window width */

/* Small history for the Back button */
static char  brw_history[8][BRW_ADDR_MAX];
static int   brw_hist_n = 0;

/* Layout constants */
#define BRW_TOOLBAR_H 36
#define BRW_STATUS_H  22
#define BRW_MARGIN    14
#define BRW_SCROLLW   10

static const int brw_style_size[8]   = { 15, 24, 19, 16, 15, 15, 14, 15 };
static const int brw_style_lineh[8]  = { 20, 32, 27, 22, 20, 12, 19, 14 };

static int brw_sb_drag = 0;
static int brw_sb_drag_off = 0;

static void brw_navigate(const char *url);

/* ===== LINE BUILDER ===== */

static char brw_cur_text[BRW_LINE_CHARS];
static int  brw_cur_len = 0;
static int  brw_cur_px = 0;
static char brw_cur_href[BRW_HREF_MAX];
static int  brw_cur_style = BS_BODY;
static int  brw_last_blank = 1;      /* suppress duplicate blank lines */

static void brw_line_flush(void) {
    if (brw_line_count >= BRW_MAX_LINES) return;
    brw_cur_text[brw_cur_len] = '\0';
    if (brw_cur_len == 0) {
        /* blank line */
        if (brw_last_blank) { brw_cur_href[0] = '\0'; return; }
        brw_last_blank = 1;
    } else {
        brw_last_blank = 0;
    }
    brw_line_t *l = &brw_lines[brw_line_count++];
    str_copy(l->text, brw_cur_text, BRW_LINE_CHARS);
    str_copy(l->href, brw_cur_href, BRW_HREF_MAX);
    l->style = (uint8_t)brw_cur_style;
    brw_cur_len = 0;
    brw_cur_px = 0;
    brw_cur_href[0] = '\0';
}

static void brw_add_word(const char *word, int wlen, int style,
                         const char *href) {
    if (wlen <= 0 || brw_line_count >= BRW_MAX_LINES) return;
    if (wlen > 60) wlen = 60;

    char wbuf[64];
    for (int i = 0; i < wlen; i++) wbuf[i] = word[i];
    wbuf[wlen] = '\0';

    int size = brw_style_size[style];
    int wpx = ttf_text_width(wbuf, size);
    int spx = ttf_text_width(" ", size);

    /* style change or overflow forces a wrap */
    if (brw_cur_len > 0 &&
        (style != brw_cur_style ||
         brw_cur_px + spx + wpx > brw_wrap_px))
        brw_line_flush();

    if (brw_cur_len == 0) {
        brw_cur_style = style;
    } else {
        if (brw_cur_len < BRW_LINE_CHARS - 2) {
            brw_cur_text[brw_cur_len++] = ' ';
            brw_cur_px += spx;
        }
    }
    for (int i = 0; i < wlen && brw_cur_len < BRW_LINE_CHARS - 1; i++)
        brw_cur_text[brw_cur_len++] = wbuf[i];
    brw_cur_px += wpx;

    if (href && href[0] && brw_cur_href[0] == '\0')
        str_copy(brw_cur_href, href, BRW_HREF_MAX);
}

static void brw_add_text(const char *s, int style, const char *href) {
    int i = 0;
    while (s[i]) {
        while (s[i] == ' ') i++;
        int start = i;
        while (s[i] && s[i] != ' ') i++;
        if (i > start) brw_add_word(s + start, i - start, style, href);
    }
}

static void brw_doc_reset(void) {
    brw_line_count = 0;
    brw_cur_len = 0;
    brw_cur_px = 0;
    brw_cur_href[0] = '\0';
    brw_cur_style = BS_BODY;
    brw_last_blank = 1;
    brw_scroll = 0;
    brw_hover_line = -1;
    str_copy(brw_title, "Socrates Browser", BRW_TITLE_MAX);
}

static void brw_doc_finish(void) {
    brw_line_flush();
    brw_total_h = 0;
    for (int i = 0; i < brw_line_count; i++)
        brw_total_h += brw_style_lineh[brw_lines[i].style];
}

/* ===== HTML → LINES ===== */

static int brw_ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

static int brw_tag_is(const char *tag, const char *name) {
    int i = 0;
    for (; name[i]; i++)
        if (!brw_ci_eq(tag[i], name[i])) return 0;
    return tag[i] == '\0';
}

/* decode one entity at s (after '&'), write to *out, return chars consumed */
static int brw_entity(const char *s, int max, char *out) {
    char name[10];
    int n = 0;
    while (n < max && n < 9 && s[n] != ';' && s[n] != '\0' &&
           s[n] != '&' && s[n] != '<' && n < 9) {
        name[n] = s[n];
        n++;
    }
    if (n >= max || s[n] != ';') { *out = '&'; return 0; }
    name[n] = '\0';

    if (str_eq(name, "amp"))  { *out = '&';  return n + 1; }
    if (str_eq(name, "lt"))   { *out = '<';  return n + 1; }
    if (str_eq(name, "gt"))   { *out = '>';  return n + 1; }
    if (str_eq(name, "quot")) { *out = '"';  return n + 1; }
    if (str_eq(name, "apos") || str_eq(name, "#39")) { *out = '\''; return n + 1; }
    if (str_eq(name, "nbsp")) { *out = ' ';  return n + 1; }
    if (str_eq(name, "mdash") || str_eq(name, "ndash")) { *out = '-'; return n + 1; }
    if (name[0] == '#') {
        int v = 0;
        for (int i = 1; name[i] >= '0' && name[i] <= '9'; i++)
            v = v * 10 + (name[i] - '0');
        *out = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
        return n + 1;
    }
    *out = '?';
    return n + 1;
}

/* Resolve href relative to current page host/path → absolute url string */
/* set while the displayed page came out of a ZIM archive */
static int brw_zim_mode = 0;

/* ZIM paths are percent-encoded in article markup */
static void brw_pct_decode(const char *in, char *out, int max) {
    int o = 0;
    for (int i = 0; in[i] && o < max - 1; i++) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            int hi = in[i + 1], lo = in[i + 2], v = 0, ok = 1;
            for (int k = 0; k < 2; k++) {
                int c = k ? lo : hi, d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { ok = 0; break; }
                v = v * 16 + d;
            }
            if (ok) { out[o++] = (char)v; i += 2; continue; }
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static void brw_resolve_href(const char *href, char *out, int out_max) {
    if (str_starts_with(href, "zim://")) {
        str_copy(out, href, out_max);
        return;
    }
    /*
     * Inside an article every link is relative, and points at a sibling
     * entry rather than a directory: "../A/Moon" and "Moon" both mean the
     * entry named Moon.  Strip the walk-ups and the namespace prefix, drop
     * any fragment, and hand back a zim:// address.
     */
    if (brw_zim_mode &&
        !str_starts_with(href, "http://") && !str_starts_with(href, "https://") &&
        !str_starts_with(href, "socrates://") && !str_starts_with(href, "//")) {
        const char *q = href;
        while (str_starts_with(q, "./")) q += 2;
        while (str_starts_with(q, "../")) q += 3;
        if (q[0] && q[1] == '/' &&
            (q[0] == 'A' || q[0] == 'C' || q[0] == 'I' || q[0] == 'M'))
            q += 2;
        char dec[BRW_ADDR_MAX];
        brw_pct_decode(q, dec, sizeof(dec));
        for (int i = 0; dec[i]; i++) if (dec[i] == '#') { dec[i] = '\0'; break; }
        str_copy(out, "zim://", out_max);
        str_append(out, dec, out_max);
        return;
    }
    if (str_starts_with(href, "http://") ||
        str_starts_with(href, "https://") ||
        str_starts_with(href, "socrates://")) {
        str_copy(out, href, out_max);
        return;
    }
    if (str_starts_with(href, "//")) {
        str_copy(out, "http:", out_max);
        str_append(out, href, out_max);
        return;
    }
    /* build from current http host/path */
    str_copy(out, "http://", out_max);
    str_append(out, http_host, out_max);
    if (href[0] == '/') {
        str_append(out, href, out_max);
        return;
    }
    /* relative to current directory */
    char dir[256];
    str_copy(dir, http_path, sizeof(dir));
    int dlen = str_len(dir);
    while (dlen > 0 && dir[dlen - 1] != '/') dir[--dlen] = '\0';
    if (dlen == 0) str_copy(dir, "/", sizeof(dir));
    str_append(out, dir, out_max);
    str_append(out, href, out_max);
}

static void brw_parse_html(const uint8_t *src, int len) {
    brw_doc_reset();

    char word[64];
    int  wlen = 0;
    int  style = BS_BODY;
    int  heading = 0;          /* 0 none, 1..3 */
    int  in_anchor = 0;
    char anchor_href[BRW_HREF_MAX];
    anchor_href[0] = '\0';
    int  pre_mode = 0;
    int  in_title = 0;
    int  title_len = 0;

    #define BRW_FLUSH_WORD() do { \
        if (wlen > 0) { \
            int st = pre_mode ? BS_PRE : \
                     (heading == 1 ? BS_H1 : heading == 2 ? BS_H2 : \
                      heading == 3 ? BS_H3 : in_anchor ? BS_LINK : style); \
            brw_add_word(word, wlen, st, in_anchor ? anchor_href : 0); \
            wlen = 0; \
        } \
    } while (0)

    int i = 0;
    while (i < len && brw_line_count < BRW_MAX_LINES - 1) {
        uint8_t c = src[i];

        if (c == '<') {
            /* ---- parse tag ---- */
            BRW_FLUSH_WORD();
            int j = i + 1;
            int closing = 0;
            if (j < len && src[j] == '/') { closing = 1; j++; }
            char tag[14];
            int tl = 0;
            while (j < len && tl < 13) {
                uint8_t tc = src[j];
                if ((tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z') ||
                    (tc >= '0' && tc <= '9')) {
                    tag[tl++] = (char)((tc >= 'A' && tc <= 'Z') ? tc + 32 : tc);
                    j++;
                } else break;
            }
            tag[tl] = '\0';

            /* capture href attribute inside the tag */
            char href_raw[BRW_HREF_MAX];
            href_raw[0] = '\0';
            int tag_end = j;
            while (tag_end < len && src[tag_end] != '>') tag_end++;
            if (!closing && brw_tag_is(tag, "a")) {
                for (int k = j; k + 6 < tag_end; k++) {
                    if (brw_ci_eq((char)src[k], 'h') &&
                        brw_ci_eq((char)src[k+1], 'r') &&
                        brw_ci_eq((char)src[k+2], 'e') &&
                        brw_ci_eq((char)src[k+3], 'f') ) {
                        int p = k + 4;
                        while (p < tag_end && (src[p] == ' ' || src[p] == '=')) p++;
                        char q = 0;
                        if (p < tag_end && (src[p] == '"' || src[p] == '\'')) {
                            q = (char)src[p];
                            p++;
                        }
                        int hl = 0;
                        while (p < tag_end && hl < BRW_HREF_MAX - 1) {
                            char hc = (char)src[p];
                            if (q && hc == q) break;
                            if (!q && (hc == ' ' || hc == '>')) break;
                            href_raw[hl++] = hc;
                            p++;
                        }
                        href_raw[hl] = '\0';
                        break;
                    }
                }
            }

            /* ---- skip whole invisible blocks ---- */
            if (!closing && (brw_tag_is(tag, "script") || brw_tag_is(tag, "style") ||
                             brw_tag_is(tag, "noscript") || brw_tag_is(tag, "svg") ||
                             brw_tag_is(tag, "template"))) {
                /* scan for matching close tag */
                int k = tag_end;
                while (k + tl + 2 < len) {
                    if (src[k] == '<' && src[k+1] == '/') {
                        int m = 0;
                        while (m < tl && k + 2 + m < len &&
                               brw_ci_eq((char)src[k + 2 + m], tag[m])) m++;
                        if (m == tl) break;
                    }
                    k++;
                }
                while (k < len && src[k] != '>') k++;
                i = k + 1;
                continue;
            }

            /* ---- tag effects ---- */
            if (brw_tag_is(tag, "title")) {
                in_title = !closing;
                if (!closing) title_len = 0;
                else brw_title[title_len] = '\0';
            } else if (brw_tag_is(tag, "br")) {
                brw_line_flush();
            } else if (brw_tag_is(tag, "p") || brw_tag_is(tag, "div") ||
                       brw_tag_is(tag, "section") || brw_tag_is(tag, "article") ||
                       brw_tag_is(tag, "table") || brw_tag_is(tag, "tr") ||
                       brw_tag_is(tag, "ul") || brw_tag_is(tag, "ol") ||
                       brw_tag_is(tag, "blockquote") || brw_tag_is(tag, "header") ||
                       brw_tag_is(tag, "footer") || brw_tag_is(tag, "nav") ||
                       brw_tag_is(tag, "form") || brw_tag_is(tag, "main")) {
                brw_line_flush();
                if (!closing && brw_tag_is(tag, "p")) brw_line_flush();
            } else if (brw_tag_is(tag, "li")) {
                brw_line_flush();
                if (!closing) brw_add_word("-", 1, BS_BODY, 0);
            } else if (brw_tag_is(tag, "h1")) {
                brw_line_flush(); heading = closing ? 0 : 1;
                if (closing) brw_line_flush();
            } else if (brw_tag_is(tag, "h2")) {
                brw_line_flush(); heading = closing ? 0 : 2;
                if (closing) brw_line_flush();
            } else if (brw_tag_is(tag, "h3") || brw_tag_is(tag, "h4")) {
                brw_line_flush(); heading = closing ? 0 : 3;
                if (closing) brw_line_flush();
            } else if (brw_tag_is(tag, "pre")) {
                brw_line_flush();
                pre_mode = !closing;
            } else if (brw_tag_is(tag, "hr")) {
                brw_line_flush();
                brw_add_word("----------------------------------------", 40,
                             BS_RULE, 0);
                brw_line_flush();
            } else if (brw_tag_is(tag, "a")) {
                if (closing) {
                    in_anchor = 0;
                } else if (href_raw[0] &&
                           !str_starts_with(href_raw, "#") &&
                           !str_starts_with(href_raw, "mailto:") &&
                           !str_starts_with(href_raw, "javascript:")) {
                    brw_resolve_href(href_raw, anchor_href, BRW_HREF_MAX);
                    in_anchor = 1;
                }
            }

            i = tag_end + 1;
            continue;
        }

        /* ---- character data ---- */
        char out = (char)c;
        if (c == '&') {
            int adv = brw_entity((const char *)src + i + 1, len - i - 1, &out);
            i += adv;   /* consumed entity body; '&' consumed below */
        }

        if (in_title) {
            if (out >= 0x20 && out < 0x7F && title_len < BRW_TITLE_MAX - 1)
                brw_title[title_len++] = out;
            i++;
            continue;
        }

        if (pre_mode) {
            /* preserve layout: emit raw chars into mono lines */
            if (out == '\n') {
                BRW_FLUSH_WORD();
                brw_line_flush();
            } else if (out == '\r') {
                /* skip */
            } else if (out == '\t') {
                BRW_FLUSH_WORD();
                brw_add_word("    ", 4, BS_PRE, 0);
            } else if (out >= 0x20 && out < 0x7F) {
                /* accumulate pre text verbatim including spaces */
                if (out == ' ') {
                    BRW_FLUSH_WORD();
                    /* represent spaces via direct append to current line */
                    if (brw_cur_len < BRW_LINE_CHARS - 1 &&
                        brw_cur_len < brw_wrap_px / 8) {
                        brw_cur_style = BS_PRE;
                        brw_cur_text[brw_cur_len++] = ' ';
                    }
                } else if (wlen < 63) {
                    word[wlen++] = out;
                }
            }
            i++;
            continue;
        }

        if (out == ' ' || out == '\n' || out == '\r' || out == '\t') {
            BRW_FLUSH_WORD();
        } else if (out >= 0x20 && out < 0x7F) {
            if (wlen < 63) word[wlen++] = out;
            else { BRW_FLUSH_WORD(); word[wlen++] = out; }
        }
        i++;
    }

    BRW_FLUSH_WORD();
    brw_doc_finish();
    #undef BRW_FLUSH_WORD
}

static void brw_parse_plain(const uint8_t *src, int len) {
    brw_doc_reset();
    int i = 0;
    while (i < len && brw_line_count < BRW_MAX_LINES - 1) {
        char c = (char)src[i];
        if (c == '\n') {
            brw_line_flush();
            brw_last_blank = 0;   /* keep blank lines in plain text */
        } else if (c >= 0x20 && c < 0x7F) {
            if (brw_cur_len < BRW_LINE_CHARS - 1 &&
                brw_cur_len < brw_wrap_px / 8) {
                brw_cur_style = BS_PRE;
                brw_cur_text[brw_cur_len++] = c;
            }
        }
        i++;
    }
    brw_doc_finish();
}

/* ===== INTERNAL PAGES ===== */

static void brw_page_home(void) {
    brw_doc_reset();
    str_copy(brw_title, "Home - Socrates Browser", BRW_TITLE_MAX);

    brw_add_text("Socrates Browser", BS_H1, 0);
    brw_line_flush();
    brw_add_text("A tiny HTTP/1.0 browser running on a homemade TCP/IP stack,", BS_BODY, 0);
    brw_add_text("straight on the metal. No libc, no TLS, no fear.", BS_BODY, 0);
    brw_line_flush();
    brw_line_flush();
    brw_add_text("Try these:", BS_H3, 0);
    brw_line_flush();
    brw_add_text("http://example.com", BS_LINK, "http://example.com");
    brw_line_flush();
    brw_add_text("http://info.cern.ch  -  the first website", BS_LINK,
                 "http://info.cern.ch");
    brw_line_flush();
    brw_add_text("http://neverssl.com", BS_LINK, "http://neverssl.com");
    brw_line_flush();
    brw_add_text("socrates://help  -  how to drive this thing", BS_LINK,
                 "socrates://help");
    brw_line_flush();
    brw_add_text("socrates://about", BS_LINK, "socrates://about");
    brw_line_flush();
    brw_line_flush();
    brw_add_text("https:// sites will not load - there is no TLS on bare", BS_DIM, 0);
    brw_add_text("metal (yet). Plain http only.", BS_DIM, 0);
    brw_doc_finish();
}

static void brw_page_help(void) {
    brw_doc_reset();
    str_copy(brw_title, "Help - Socrates Browser", BRW_TITLE_MAX);
    brw_add_text("Using the browser", BS_H1, 0);
    brw_line_flush();
    brw_add_text("Click the address bar, type a URL, press Enter.", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("Gold underlined lines are links - click them.", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("Scroll with Up/Down, PgUp/PgDn, or drag the scrollbar.", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("The Back button returns to the previous page.", BS_BODY, 0);
    brw_line_flush();
    brw_line_flush();
    brw_add_text("Pages:", BS_H3, 0);
    brw_line_flush();
    brw_add_text("socrates://home", BS_LINK, "socrates://home");
    brw_line_flush();
    brw_add_text("socrates://about", BS_LINK, "socrates://about");
    brw_line_flush();
    brw_add_text("socrates://file/<name> shows a ramdisk file", BS_BODY, 0);
    brw_doc_finish();
}

static void brw_page_about(void) {
    brw_doc_reset();
    str_copy(brw_title, "About - Socrates Browser", BRW_TITLE_MAX);
    brw_add_text("Socrates BSD 9", BS_H1, 0);
    brw_line_flush();
    brw_add_text("Bare-metal x86_64 hobby operating system.", BS_BODY, 0);
    brw_line_flush();
    brw_line_flush();
    brw_add_text("Integer-only TrueType rasterizer", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("Window manager with focus and z-order", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("IPv4 / ICMP / UDP / DNS / TCP / HTTP stack", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("Intel e1000 NIC + AC97 audio + PS/2 HAL", BS_BODY, 0);
    brw_line_flush();
    brw_add_text("ustar ramdisk + ELF64 loader + int 0x80 syscalls", BS_BODY, 0);
    brw_doc_finish();
}

static void brw_page_error(const char *msg) {
    brw_doc_reset();
    str_copy(brw_title, "Error - Socrates Browser", BRW_TITLE_MAX);
    brw_add_text("Page failed to load", BS_H2, 0);
    brw_line_flush();
    brw_add_text(msg, BS_BODY, 0);
    brw_line_flush();
    brw_line_flush();
    brw_add_text("Back to home", BS_LINK, "socrates://home");
    brw_doc_finish();
}

static void brw_page_file(const char *name) {
    uint64_t fsize = 0;
    const void *data = fs_read_file(name, &fsize);
    if (!data) {
        brw_page_error("File not found on the ramdisk.");
        return;
    }
    brw_parse_plain((const uint8_t *)data, (int)fsize);
    str_copy(brw_title, name, BRW_TITLE_MAX);
}

static void brw_page_zim(const char *path) {
    if (!zim.open) {
        brw_page_error("No archive is open.  Open the Wikipedia app, or run"
                       " 'zim open <file>' in the terminal.");
        return;
    }
    if (path[0] == '\0') path = "";

    uint32_t idx;
    if (!zim_find('C', path, &idx)) {
        brw_doc_reset();
        str_copy(brw_title, "Not found", BRW_TITLE_MAX);
        brw_add_text("No such article", BS_H2, 0);
        brw_line_flush();
        brw_add_text(path, BS_DIM, 0);
        brw_line_flush();
        brw_line_flush();
        brw_add_text("Entry names are case sensitive.  Use the Wikipedia app"
                     " to search.", BS_BODY, 0);
        brw_doc_finish();
        return;
    }

    const uint8_t *data;
    uint32_t len;
    zim_dirent_t e;
    if (zim_content(idx, &data, &len, &e) != 0) {
        brw_page_error(zim_err);
        return;
    }

    int is_html = 0;
    const char *m = zim_mime_name(e.mime);
    for (int i = 0; m[i]; i++)
        if (m[i] == 'h' && m[i+1] == 't' && m[i+2] == 'm' && m[i+3] == 'l') {
            is_html = 1;
            break;
        }

    brw_zim_mode = 1;
    if (is_html) brw_parse_html(data, (int)len);
    else         brw_parse_plain(data, (int)len);
    brw_zim_mode = 0;

    str_copy(brw_title, e.title, BRW_TITLE_MAX);
}

/* ===== NAVIGATION ===== */

static void brw_set_status(const char *s) {
    str_copy(brw_status, s, sizeof(brw_status));
}

static void brw_push_history(void) {
    if (brw_hist_n == 8) {
        for (int i = 0; i < 7; i++)
            str_copy(brw_history[i], brw_history[i + 1], BRW_ADDR_MAX);
        brw_hist_n = 7;
    }
    str_copy(brw_history[brw_hist_n++], brw_addr, BRW_ADDR_MAX);
}

static void brw_set_addr(const char *url) {
    str_copy(brw_addr, url, BRW_ADDR_MAX);
    brw_addr_len = str_len(brw_addr);
    brw_addr_cur = brw_addr_len;
}

static void brw_navigate_no_hist(const char *url) {
    brw_loading = 0;
    brw_addr_focus = 0;

    char url_buf[BRW_ADDR_MAX];
    str_copy(url_buf, url, BRW_ADDR_MAX);

    if (str_starts_with(url_buf, "zim://")) {
        brw_set_addr(url_buf);
        brw_set_status("Reading the archive...");
        brw_page_zim(url_buf + 6);
        brw_set_status(zim.open ? "Ready" : "No archive open");
        return;
    }

    if (str_starts_with(url_buf, "socrates://")) {
        const char *page = url_buf + 11;
        brw_set_addr(url_buf);
        if (str_eq(page, "home") || page[0] == '\0') brw_page_home();
        else if (str_eq(page, "help"))  brw_page_help();
        else if (str_eq(page, "about")) brw_page_about();
        else if (str_starts_with(page, "file/")) brw_page_file(page + 5);
        else brw_page_error("Unknown internal page.");
        brw_set_status("Ready");
        return;
    }

    if (str_starts_with(url_buf, "https://")) {
        brw_set_addr(url_buf);
        brw_page_error("https:// needs TLS, which this kernel does not have. Try the http:// version.");
        brw_set_status("Error: no TLS");
        return;
    }

    char host[128], path[256];
    uint16_t port;
    if (!http_parse_url(url_buf, host, sizeof(host), &port,
                        path, sizeof(path))) {
        brw_set_addr(url_buf);
        brw_page_error("That does not look like a valid URL.");
        brw_set_status("Error: bad URL");
        return;
    }

    /* canonical form in the address bar */
    char canon[BRW_ADDR_MAX];
    str_copy(canon, "http://", BRW_ADDR_MAX);
    str_append(canon, host, BRW_ADDR_MAX);
    if (port != 80) {
        char pb[8];
        str_append(canon, ":", BRW_ADDR_MAX);
        uint_to_str(port, pb);
        str_append(canon, pb, BRW_ADDR_MAX);
    }
    str_append(canon, path, BRW_ADDR_MAX);
    brw_set_addr(canon);

    if (!e1000_found) {
        brw_page_error("No network adapter detected.");
        brw_set_status("Error: no NIC");
        return;
    }

    brw_doc_reset();
    brw_add_text("Loading", BS_H3, 0);
    brw_add_text(canon, BS_DIM, 0);
    brw_doc_finish();

    brw_loading = 1;
    brw_set_status("Resolving host...");
    http_get(host, port, path);
    http_owner = HTTP_OWNER_BROWSER;
}

static void brw_navigate(const char *url) {
    brw_push_history();
    brw_navigate_no_hist(url);
}

static void brw_back(void) {
    /* history top = page we came from; pop it and go there */
    if (brw_hist_n < 1) return;
    char prev[BRW_ADDR_MAX];
    str_copy(prev, brw_history[--brw_hist_n], BRW_ADDR_MAX);
    brw_navigate_no_hist(prev);
}

/* ===== ASYNC POLL (each frame) ===== */

static void brw_poll(void) {
    if (!brw_loading) return;

    if (http_state == HTTP_DONE) {
        brw_loading = 0;

        /* content type sniffing */
        char ctype[64];
        int is_html = 0;
        if (http_find_header("Content-Type", ctype, sizeof(ctype))) {
            for (int i = 0; ctype[i]; i++) {
                if (ctype[i] == 'h' && ctype[i+1] == 't' &&
                    ctype[i+2] == 'm' && ctype[i+3] == 'l') {
                    is_html = 1;
                    break;
                }
            }
        } else {
            for (int i = 0; i < http_body_len && i < 64; i++) {
                if (http_body[i] == '<') { is_html = 1; break; }
                if (http_body[i] > ' ') break;
            }
        }

        if (is_html) brw_parse_html(http_body, http_body_len);
        else         brw_parse_plain(http_body, http_body_len);

        char st[64], nb[16];
        str_copy(st, "Done - HTTP ", sizeof(st));
        uint_to_str((uint32_t)http_status_code, nb);
        str_append(st, nb, sizeof(st));
        str_append(st, ", ", sizeof(st));
        uint_to_str((uint32_t)(http_body_len / 1024), nb);
        str_append(st, nb, sizeof(st));
        str_append(st, " KB", sizeof(st));
        brw_set_status(st);

        /* keep the canonical address of where we ended up (redirects) */
        char canon[BRW_ADDR_MAX];
        str_copy(canon, "http://", BRW_ADDR_MAX);
        str_append(canon, http_host, BRW_ADDR_MAX);
        str_append(canon, http_path, BRW_ADDR_MAX);
        brw_set_addr(canon);
        return;
    }
    if (http_state == HTTP_ERROR) {
        brw_loading = 0;
        char msg[96];
        str_copy(msg, "Could not load the page: ", sizeof(msg));
        str_append(msg, http_err, sizeof(msg));
        brw_page_error(msg);
        char st[80];
        str_copy(st, "Error: ", sizeof(st));
        str_append(st, http_err, sizeof(st));
        brw_set_status(st);
        return;
    }

    /* progress feedback */
    if (http_state == HTTP_RESOLVING)   brw_set_status("Resolving host...");
    else if (http_state == HTTP_CONNECTING) brw_set_status("Connecting...");
    else if (http_state == HTTP_REQUESTING) brw_set_status("Requesting...");
    else if (http_state == HTTP_RECEIVING) {
        char st[64], nb[16];
        str_copy(st, "Receiving... ", sizeof(st));
        uint_to_str((uint32_t)(tcp_rx_len / 1024), nb);
        str_append(st, nb, sizeof(st));
        str_append(st, " KB", sizeof(st));
        brw_set_status(st);
    }
}

/* ===== KEY INPUT ===== */

static void brw_go(void) {
    if (brw_addr_len == 0) return;
    /* bare hostname convenience: add http:// */
    char url[BRW_ADDR_MAX];
    if (!str_starts_with(brw_addr, "http://") &&
        !str_starts_with(brw_addr, "https://") &&
        !str_starts_with(brw_addr, "socrates://")) {
        str_copy(url, "http://", BRW_ADDR_MAX);
        str_append(url, brw_addr, BRW_ADDR_MAX);
    } else {
        str_copy(url, brw_addr, BRW_ADDR_MAX);
    }
    brw_navigate(url);
}

static void brw_scroll_by(int dy, int view_h) {
    brw_scroll += dy;
    int max_s = brw_total_h - view_h;
    if (max_s < 0) max_s = 0;
    if (brw_scroll > max_s) brw_scroll = max_s;
    if (brw_scroll < 0) brw_scroll = 0;
}

static int brw_view_h_cache = 300;

static void brw_key(char ch) {
    if (brw_addr_focus) {
        if (ch == '\n') { brw_go(); return; }
        if (ch == 27)   { brw_addr_focus = 0; return; }
        if (ch == '\b') {
            if (brw_addr_cur > 0) {
                for (int i = brw_addr_cur - 1; i < brw_addr_len; i++)
                    brw_addr[i] = brw_addr[i + 1];
                brw_addr_len--;
                brw_addr_cur--;
            }
            return;
        }
        if (ch == KEY_DEL) {
            if (brw_addr_cur < brw_addr_len) {
                for (int i = brw_addr_cur; i < brw_addr_len; i++)
                    brw_addr[i] = brw_addr[i + 1];
                brw_addr_len--;
            }
            return;
        }
        if (ch == KEY_LEFT)  { if (brw_addr_cur > 0) brw_addr_cur--; return; }
        if (ch == KEY_RIGHT) { if (brw_addr_cur < brw_addr_len) brw_addr_cur++; return; }
        if (ch == KEY_HOME)  { brw_addr_cur = 0; return; }
        if (ch == KEY_END)   { brw_addr_cur = brw_addr_len; return; }
        if (ch >= 0x20 && ch < 0x7F && brw_addr_len < BRW_ADDR_MAX - 1) {
            for (int i = brw_addr_len; i > brw_addr_cur; i--)
                brw_addr[i] = brw_addr[i - 1];
            brw_addr[brw_addr_cur++] = ch;
            brw_addr_len++;
            brw_addr[brw_addr_len] = '\0';
        }
        return;
    }

    /* page scrolling */
    if (ch == KEY_UP)   brw_scroll_by(-40, brw_view_h_cache);
    if (ch == KEY_DOWN) brw_scroll_by(40, brw_view_h_cache);
    if (ch == KEY_PGUP) brw_scroll_by(-brw_view_h_cache + 30, brw_view_h_cache);
    if (ch == KEY_PGDN || ch == ' ')
        brw_scroll_by(brw_view_h_cache - 30, brw_view_h_cache);
    if (ch == KEY_HOME) brw_scroll = 0;
    if (ch == KEY_END)  brw_scroll_by(brw_total_h, brw_view_h_cache);
}

/* ===== MOUSE + DRAW ===== */

/* geometry helpers shared by draw + mouse */
static void brw_layout(int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       int32_t *addr_x, int32_t *addr_y,
                       int32_t *addr_w, int32_t *addr_h,
                       int32_t *view_y, int32_t *view_h) {
    *addr_x = cx + 78;
    *addr_y = cy + 6;
    *addr_w = cw - 78 - 56;
    *addr_h = BRW_TOOLBAR_H - 12;
    *view_y = cy + BRW_TOOLBAR_H;
    *view_h = chh - BRW_TOOLBAR_H - BRW_STATUS_H;
}

static void brw_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    int click = (lmb && !prev_lmb);
    int32_t ax, ay, aw, ah, vy, vh;
    brw_layout(cx, cy, cw, chh, &ax, &ay, &aw, &ah, &vy, &vh);
    brw_view_h_cache = vh;
    brw_wrap_px = cw - 2 * BRW_MARGIN - BRW_SCROLLW - 8;

    /* hover link detection */
    brw_hover_line = -1;
    if (mx >= cx + BRW_MARGIN && mx < cx + cw - BRW_SCROLLW - 4 &&
        my >= vy && my < vy + vh) {
        int y = vy + 6 - brw_scroll;
        for (int i = 0; i < brw_line_count; i++) {
            int lh = brw_style_lineh[brw_lines[i].style];
            if (my >= y && my < y + lh && brw_lines[i].href[0]) {
                brw_hover_line = i;
                break;
            }
            y += lh;
            if (y > vy + vh) break;
        }
    }

    if (!click && !lmb) brw_sb_drag = 0;

    /* scrollbar */
    int32_t sb_x = cx + cw - BRW_SCROLLW - 2;
    if (brw_total_h > vh && vh > 40) {
        int knob_h = vh * vh / brw_total_h;
        if (knob_h < 24) knob_h = 24;
        int max_s = brw_total_h - vh;
        int knob_y = vy + (vh - knob_h) * brw_scroll / (max_s > 0 ? max_s : 1);

        if (click && mx >= sb_x && mx < sb_x + BRW_SCROLLW + 2 &&
            my >= vy && my < vy + vh) {
            if (my >= knob_y && my < knob_y + knob_h) {
                brw_sb_drag = 1;
                brw_sb_drag_off = my - knob_y;
            } else if (my < knob_y) {
                brw_scroll_by(-vh + 30, vh);
            } else {
                brw_scroll_by(vh - 30, vh);
            }
        }
        if (brw_sb_drag && lmb) {
            int new_ky = my - brw_sb_drag_off - vy;
            int span = vh - knob_h;
            if (span > 0) {
                brw_scroll = new_ky * max_s / span;
                if (brw_scroll < 0) brw_scroll = 0;
                if (brw_scroll > max_s) brw_scroll = max_s;
            }
        }
    }

    if (!click) return;

    /* Back button */
    if (mx >= cx + 8 && mx < cx + 38 && my >= ay && my < ay + ah) {
        brw_back();
        return;
    }
    /* Reload button */
    if (mx >= cx + 42 && mx < cx + 72 && my >= ay && my < ay + ah) {
        brw_navigate_no_hist(brw_addr);
        return;
    }
    /* Address bar */
    if (mx >= ax && mx < ax + aw && my >= ay && my < ay + ah) {
        brw_addr_focus = 1;
        brw_addr_cur = brw_addr_len;
        return;
    }
    brw_addr_focus = 0;

    /* Link click */
    if (brw_hover_line >= 0 && !brw_sb_drag) {
        char url[BRW_HREF_MAX];
        str_copy(url, brw_lines[brw_hover_line].href, BRW_HREF_MAX);
        brw_navigate(url);
    }
}

static void brw_draw(uint32_t *buf, uint32_t w, uint32_t h,
                     int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                     uint32_t tick, int focused) {
    int32_t ax, ay, aw, ah, vy, vh;
    brw_layout(cx, cy, cw, chh, &ax, &ay, &aw, &ah, &vy, &vh);
    brw_view_h_cache = vh;
    brw_wrap_px = cw - 2 * BRW_MARGIN - BRW_SCROLLW - 8;

    /* toolbar */
    gfx_rect(buf, w, h, cx, cy, cw, BRW_TOOLBAR_H, C_BG_PANEL);
    gfx_rect(buf, w, h, cx, cy + BRW_TOOLBAR_H - 1, cw, 1, 0x2A3040u);

    /* back + reload buttons */
    int back_ok = brw_hist_n >= 1;
    gfx_rect(buf, w, h, cx + 8, ay, 30, ah, 0x202535u);
    gfx_rect_outline(buf, w, h, cx + 8, ay, 30, ah, 0x323A4Eu);
    {
        uint32_t col = back_ok ? C_TEXT : 0x555C6Eu;
        int bx = cx + 23, by = ay + ah / 2;
        gfx_line(buf, w, h, bx + 4, by - 5, bx - 3, by, 2, col);
        gfx_line(buf, w, h, bx - 3, by, bx + 4, by + 5, 2, col);
    }
    gfx_rect(buf, w, h, cx + 42, ay, 30, ah, 0x202535u);
    gfx_rect_outline(buf, w, h, cx + 42, ay, 30, ah, 0x323A4Eu);
    gfx_circle_outline(buf, w, h, cx + 57, ay + ah / 2, 6, C_TEXT);
    gfx_tri(buf, w, h, cx + 60, ay + ah / 2 - 8, cx + 66, ay + ah / 2 - 6,
            cx + 60, ay + ah / 2 - 2, C_TEXT);

    /* address field */
    uint32_t field_bg = brw_addr_focus ? 0x0D1017u : 0x191E2Bu;
    gfx_rect(buf, w, h, ax, ay, aw, ah, field_bg);
    gfx_rect_outline(buf, w, h, ax, ay, aw, ah,
                     brw_addr_focus ? C_GOLD : 0x323A4Eu);
    {
        int fs = 14;
        int ty = ay + (ah - fs) / 2 - 2;
        ttf_draw_string(buf, (int)w, (int)h, ax + 8, ty, brw_addr,
                        brw_addr_focus ? C_TEXT : C_TEXT_DIM, fs);
        if (brw_addr_focus && ((tick / 30) & 1) == 0) {
            char tmp[BRW_ADDR_MAX];
            str_copy(tmp, brw_addr, BRW_ADDR_MAX);
            tmp[brw_addr_cur] = '\0';
            int cx_px = ax + 8 + ttf_text_width(tmp, fs);
            gfx_rect(buf, w, h, cx_px, ay + 4, 2, ah - 8, C_GOLD);
        }
    }

    /* Go button */
    {
        int32_t gx = ax + aw + 6;
        gfx_rect(buf, w, h, gx, ay, 44, ah, 0x2A2410u);
        gfx_rect_outline(buf, w, h, gx, ay, 44, ah, C_GOLD_DIM);
        ttf_draw_string(buf, (int)w, (int)h, gx + 12, ay + (ah - 13) / 2 - 2,
                        "Go", C_GOLD, 13);
    }

    /* page background */
    gfx_rect(buf, w, h, cx, vy, cw, vh, C_WIN_BG);

    /* content lines */
    int y = vy + 6 - brw_scroll;
    for (int i = 0; i < brw_line_count; i++) {
        brw_line_t *l = &brw_lines[i];
        int lh = brw_style_lineh[l->style];
        if (y + lh >= vy && y < vy + vh) {
            uint32_t col;
            int size = brw_style_size[l->style];
            switch (l->style) {
            case BS_H1: case BS_H2: case BS_H3: col = 0x1A1E28u; break;
            case BS_LINK: col = C_LINK; break;
            case BS_DIM:  col = 0x8A8F9Cu; break;
            case BS_RULE: col = 0xC5C9D2u; break;
            default: col = C_INK; break;
            }
            if (l->href[0]) col = C_LINK;

            if (l->style == BS_PRE) {
                if (y >= vy - 12)
                    mono_text(buf, w, h, cx + BRW_MARGIN, y, l->text,
                              0x30343Eu, 1);
            } else {
                if (y >= vy - size)
                    ttf_draw_string(buf, (int)w, (int)h, cx + BRW_MARGIN, y,
                                    l->text, col, size);
            }
            /* link underline + hover highlight */
            if (l->href[0]) {
                int tw = ttf_text_width(l->text, size);
                int uy = y + size + 3;
                if (uy >= vy && uy < vy + vh)
                    gfx_rect(buf, w, h, cx + BRW_MARGIN, uy, tw, 1,
                             i == brw_hover_line ? C_GOLD : 0xC9B678u);
            }
        }
        y += lh;
        if (y > vy + vh) break;
    }

    /* scrollbar */
    if (brw_total_h > vh && vh > 40) {
        int32_t sb_x = cx + cw - BRW_SCROLLW - 2;
        gfx_rect(buf, w, h, sb_x, vy, BRW_SCROLLW, vh, 0xE2E3E8u);
        int knob_h = vh * vh / brw_total_h;
        if (knob_h < 24) knob_h = 24;
        int max_s = brw_total_h - vh;
        int knob_y = vy + (vh - knob_h) * brw_scroll / (max_s > 0 ? max_s : 1);
        gfx_rect(buf, w, h, sb_x + 1, knob_y, BRW_SCROLLW - 2, knob_h,
                 0xA8ACB8u);
    }

    /* status bar */
    int32_t sy = cy + chh - BRW_STATUS_H;
    gfx_rect(buf, w, h, cx, sy, cw, BRW_STATUS_H, C_BG_PANEL);
    gfx_rect(buf, w, h, cx, sy, cw, 1, 0x2A3040u);
    {
        const char *st = brw_status;
        if (brw_hover_line >= 0) st = brw_lines[brw_hover_line].href;
        ttf_draw_string(buf, (int)w, (int)h, cx + 10, sy + 3, st,
                        C_TEXT_DIM, 12);
    }
    (void)focused;
}

#endif /* BROWSER_H */
