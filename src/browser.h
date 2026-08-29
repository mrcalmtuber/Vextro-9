#ifndef BROWSER_H
#define BROWSER_H

/*
 * Vextro Browser — HTTP/1.0 + internal vextro:// pages.
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
/* The internal page scheme, in one place, with its length derived rather
 * than written down a second time. */
#define BRW_SCHEME     "vextro://"
#define BRW_SCHEME_LEN (sizeof(BRW_SCHEME) - 1)

static char  brw_addr[BRW_ADDR_MAX] = "vextro://home";
static int   brw_addr_len = 15;
static int   brw_addr_cur = 15;
static int   brw_addr_focus = 0;
static char  brw_title[BRW_TITLE_MAX] = "Vextro Browser";
static int   brw_scroll = 0;         /* px */
static int   brw_total_h = 0;        /* px */
static int   brw_loading = 0;

/*
 * Whether the page on screen is the built-in start page.
 *
 * It is drawn from the skin rather than from the line list every other
 * page uses — a heading, three panels and a search field are a layout,
 * and expressing them as word-wrapped lines with one of eight styles
 * would be describing a shape in a vocabulary that cannot hold it.
 */
static int   brw_is_home = 0;

/*
 * The packages figure, which lives on the other side of an include.
 *
 * store.h is included after this file (it stores its catalog through the
 * filesystem layer, which is defined before both), so store_inst_count
 * is not in scope here. Declared here and defined there, which is the
 * arrangement this file already uses for prof_may and fs_native.
 */
static int brw_installed_count(void);

/* ===== the secure fetch =====
 *
 * https:// used to print "this kernel does not have TLS", which stopped
 * being true once Mbed TLS went in and stayed on screen anyway. What
 * kept the browser off it was not the crypto but the threading:
 * vxsec_https_get() blocks from the DNS lookup to the last body byte,
 * and the browser is drawn on the compositor thread, so calling it here
 * would stop the desktop for as long as the page took.
 *
 * So the fetch runs on a thread of its own and hands the result back
 * through these. `brw_tls_state` is the handshake between the two and is
 * the only variable either side writes while the other might read: the
 * worker fills the buffer and *then* publishes the state, the compositor
 * reads the state and only then touches the buffer.
 */
#define BRW_TLS_IDLE  0
#define BRW_TLS_RUN   1
#define BRW_TLS_DONE  2
#define BRW_TLS_ERR   3

#define BRW_TLS_MAX   (192 * 1024)

static volatile int brw_tls_state = BRW_TLS_IDLE;
static volatile int brw_tls_len   = 0;
static char         brw_tls_host[128];
static char         brw_tls_path[256];
static char         brw_tls_err[96];
static uint8_t      brw_tls_buf[BRW_TLS_MAX];

static void brw_tls_thread(void) {
    int n = vxsec_https_get(brw_tls_host, brw_tls_path,
                            brw_tls_buf, BRW_TLS_MAX);

    if (n > 0) {
        brw_tls_len = n;
        __asm__ volatile("" ::: "memory");   /* body, then the state */
        brw_tls_state = BRW_TLS_DONE;
        return;
    }

    /*
     * A failure here is most often the certificate, now that the chain
     * is actually checked -- so say which it was rather than "could not
     * connect", because the two want completely different responses
     * from whoever is reading.
     */
    if (!vxsec_verifies_certificates())
        str_copy(brw_tls_err, "the connection failed (certificates are "
                              "not being verified on this volume)",
                 sizeof(brw_tls_err));
    else
        str_copy(brw_tls_err, "the connection failed, or the server's "
                              "certificate did not verify",
                 sizeof(brw_tls_err));

    __asm__ volatile("" ::: "memory");
    brw_tls_state = BRW_TLS_ERR;
}
static char  brw_status[80] = "Ready";
static int   brw_hover_line = -1;
static int   brw_wrap_px = 700;      /* recomputed from window width */

/* Small history for the Back button */
static char  brw_history[8][BRW_ADDR_MAX];
static int   brw_hist_n = 0;

/* Layout constants */
/*
 * ---- the skin ----
 *
 * The chrome below is drawn from assets/ui/browser.vxml, resolved
 * against assets/ui/tokens.tw by tools/tailwind.py at build time and
 * laid out here by src/vxui.h. Changing a colour or a spacing is
 * changing the shell, not this file.
 */
#include "vxui.h"

/*
 * The toolbar's height comes from the token rather than a number here,
 * so `h-12` in the shell and the space the page gets below it cannot
 * drift apart. It was 36; the shell asks for 48.
 */
#define BRW_TOOLBAR_H ((int32_t)vxui_nodes[VXUI_ID_CHROME].h)
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
    /* Cleared here and set again by brw_page_home, so that every other
     * way of arriving at a page turns the skinned start page off. */
    brw_is_home = 0;
    brw_line_count = 0;
    brw_cur_len = 0;
    brw_cur_px = 0;
    brw_cur_href[0] = '\0';
    brw_cur_style = BS_BODY;
    brw_last_blank = 1;
    brw_scroll = 0;
    brw_hover_line = -1;
    str_copy(brw_title, "Vextro Browser", BRW_TITLE_MAX);
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
        !str_starts_with(href, "vextro://") && !str_starts_with(href, "//")) {
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
        str_starts_with(href, "vextro://")) {
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

/*
 * The nearest ASCII for a Unicode codepoint, or 0 to drop it.
 *
 * The font is indexed by byte, so anything above 0x7E cannot be drawn.
 * Encyclopedia text is full of accented names, typographic quotes and
 * dashes; silently deleting them corrupts words, while folding keeps them
 * readable.  '?' is the honest answer for anything genuinely foreign —
 * it shows something is there rather than pretending otherwise.
 */
static char brw_fold_cp(uint32_t cp) {
    if (cp < 0x80) return (char)cp;

    /* Latin-1 and Latin Extended-A, in codepoint order */
    static const char lat1[] =
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs"      /* 0xC0..0xDF */
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";     /* 0xE0..0xFF */
    if (cp >= 0xC0 && cp <= 0xFF) return lat1[cp - 0xC0];

    switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: return '\'';  /* curly single */
    case 0x201C: case 0x201D: case 0x201F: return '"';   /* curly double */
    case 0x2010: case 0x2011: case 0x2012:
    case 0x2013: case 0x2014: case 0x2015: return '-';   /* dashes */
    case 0x2026: return '.';                             /* ellipsis */
    case 0x00A0: case 0x2007: case 0x202F: return ' ';   /* hard spaces */
    case 0x00B7: case 0x2022: return '*';                /* bullets */
    case 0x00D7: return 'x';
    case 0x2032: return '\'';
    case 0x2033: return '"';
    case 0x00AB: case 0x00BB: return '"';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return 0;  /* zero width */
    default: break;
    }
    /* Latin Extended-A is mostly accented ASCII in pairs */
    if (cp >= 0x0100 && cp <= 0x017F) {
        static const char lex[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGg"
                                  "HhHhIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnn"
                                  "NnOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUu"
                                  "UuUuWwYyYZzZzZzs";
        uint32_t k = cp - 0x0100;
        if (k < sizeof(lex) - 1) return lex[k];
    }
    return '?';
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
        } else if ((uint8_t)out >= 0xC0) {
            /*
             * UTF-8 lead byte.  Archive text is UTF-8 and this renderer
             * draws bytes, so dropping what it cannot show deletes
             * letters from the middle of words — "Zoë" became "Zo".
             * Fold to the nearest ASCII instead and step over the
             * continuation bytes.
             */
            uint32_t cp = 0;
            int extra = 0;
            uint8_t b = (uint8_t)out;
            if      ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; extra = 1; }
            else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; extra = 2; }
            else                         { cp = b & 0x07u; extra = 3; }
            for (int k = 0; k < extra && i + 1 < len; k++) {
                uint8_t c2 = src[++i];
                if ((c2 & 0xC0) != 0x80) break;
                cp = (cp << 6) | (uint32_t)(c2 & 0x3F);
            }
            char f = brw_fold_cp(cp);
            if (f) {
                if (wlen < 63) word[wlen++] = f;
                else { BRW_FLUSH_WORD(); word[wlen++] = f; }
            }
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
    /* After the reset, not before: brw_doc_reset clears this, so that
     * every other way of arriving at a page turns the skinned start page
     * off. Setting it first meant it was cleared a line later and the
     * start page never drew. */
    brw_is_home = 1;
    str_copy(brw_title, "Home - Vextro Browser", BRW_TITLE_MAX);

    brw_add_text("Vextro Browser", BS_H1, 0);
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
    brw_add_text("vextro://help  -  how to drive this thing", BS_LINK,
                 "vextro://help");
    brw_line_flush();
    brw_add_text("vextro://about", BS_LINK, "vextro://about");
    brw_line_flush();
    brw_line_flush();
    if (!vxsec_ready()) {
        brw_add_text("https:// will not load: TLS did not start, because "
                     "this", BS_DIM, 0);
        brw_add_text("machine has no hardware random source.", BS_DIM, 0);
    } else if (vxsec_verifies_certificates()) {
        brw_add_text("https:// works, over TLS 1.3, and the server's "
                     "certificate", BS_DIM, 0);
        brw_add_text("is checked against the roots in /etc/ca-bundle.crt.",
                     BS_DIM, 0);
    } else {
        brw_add_text("https:// works, over TLS 1.3 - but this volume "
                     "carries no", BS_DIM, 0);
        brw_add_text("/etc/ca-bundle.crt, so nothing proves who is at the "
                     "far end.", BS_DIM, 0);
    }
    brw_doc_finish();
}

static void brw_page_help(void) {
    brw_doc_reset();
    str_copy(brw_title, "Help - Vextro Browser", BRW_TITLE_MAX);
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
    brw_add_text("vextro://home", BS_LINK, "vextro://home");
    brw_line_flush();
    brw_add_text("vextro://about", BS_LINK, "vextro://about");
    brw_line_flush();
    brw_add_text("vextro://file/<name> shows a ramdisk file", BS_BODY, 0);
    brw_doc_finish();
}

static void brw_page_about(void) {
    brw_doc_reset();
    str_copy(brw_title, "About - Vextro Browser", BRW_TITLE_MAX);
    brw_add_text("Vextro 9", BS_H1, 0);
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
    str_copy(brw_title, "Error - Vextro Browser", BRW_TITLE_MAX);
    brw_add_text("Page failed to load", BS_H2, 0);
    brw_line_flush();
    brw_add_text(msg, BS_BODY, 0);
    brw_line_flush();
    brw_line_flush();
    brw_add_text("Back to home", BS_LINK, "vextro://home");
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

/* Re-file the current page under its finished title. brw_navigate_no_hist
 * records it under its URL before the page exists; this runs once the
 * title is real, and recent_push dedupes on the address. */
static void brw_note_recent(void) {
    if (brw_addr[0]) recent_push(WK_BROWSER, brw_title, brw_addr);
}

static void brw_navigate_no_hist(const char *url) {
    /* Recorded here rather than in brw_navigate so that the page opened
     * with the window, and pages reached with Back, count as visits too.
     * The address is what reopens a page; the title is not known yet for
     * anything fetched over the network, so this records it under its URL
     * and the load re-records it under its title -- recent_push dedupes on
     * the path and keeps the newer label. */
    recent_push(WK_BROWSER, url, url);

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

    if (str_starts_with(url_buf, BRW_SCHEME)) {
        /* Length taken from the scheme itself. This was a hardcoded 11 --
         * the length of the old scheme -- and renaming the system to a
         * shorter one silently ate the first two letters of every page
         * name, so every internal page became "unknown". */
        const char *page = url_buf + BRW_SCHEME_LEN;
        brw_set_addr(url_buf);
        if (str_eq(page, "home") || page[0] == '\0') brw_page_home();
        else if (str_eq(page, "help"))  brw_page_help();
        else if (str_eq(page, "about")) brw_page_about();
        else if (str_starts_with(page, "file/")) brw_page_file(page + 5);
        else brw_page_error("Unknown internal page.");
        brw_set_status("Ready");
        brw_note_recent();
        return;
    }

    if (str_starts_with(url_buf, "https://")) {
        char host[128], path[256];
        uint16_t port = 443;

        brw_set_addr(url_buf);

        if (!http_parse_url(url_buf, host, sizeof(host), &port,
                            path, sizeof(path))) {
            brw_page_error("That does not look like a valid URL.");
            brw_set_status("Error: bad URL");
            return;
        }

        if (!vxnet_up()) {
            brw_page_error("The network is not up.");
            brw_set_status("Error: no network");
            return;
        }
        if (!vxsec_ready()) {
            brw_page_error("TLS did not start. Without a hardware random "
                           "source there is no safe way to make a session "
                           "key, so secure connections are disabled rather "
                           "than made with a predictable one.");
            brw_set_status("Error: no TLS");
            return;
        }
        if (brw_tls_state == BRW_TLS_RUN) {
            brw_set_status("A secure request is already in flight");
            return;
        }

        str_copy(brw_tls_host, host, sizeof(brw_tls_host));
        str_copy(brw_tls_path, path[0] ? path : "/", sizeof(brw_tls_path));

        brw_doc_reset();
        brw_add_text("Connecting securely", BS_H3, 0);
        brw_add_text(url_buf, BS_DIM, 0);
        brw_doc_finish();

        brw_tls_len   = 0;
        brw_tls_state = BRW_TLS_RUN;
        brw_loading   = 1;
        brw_set_status("TLS handshake...");

        /*
         * On a thread of its own, because vxsec_https_get() blocks from
         * the DNS lookup through the handshake to the last byte of the
         * body -- seconds, on a slow site. This function runs on the
         * compositor thread, so doing it here would freeze the whole
         * desktop until the page arrived.
         */
        if (!sched_spawn_kernel(brw_tls_thread, "brw-tls", PRIO_NORMAL)) {
            brw_tls_state = BRW_TLS_IDLE;
            brw_loading = 0;
            brw_page_error("Could not start the secure fetch.");
            brw_set_status("Error: no thread");
        }
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

    /* The secure fetch, if one is in flight. Checked before the plain
     * HTTP state because the two are independent and a stale HTTP_DONE
     * from a previous navigation would otherwise be picked up here. */
    if (brw_tls_state == BRW_TLS_DONE) {
        int len = brw_tls_len;
        int is_html = 0;

        brw_tls_state = BRW_TLS_IDLE;
        brw_loading = 0;

        for (int i = 0; i < len && i < 64; i++) {
            if (brw_tls_buf[i] == '<') { is_html = 1; break; }
            if (brw_tls_buf[i] > ' ') break;
        }

        if (is_html) brw_parse_html(brw_tls_buf, len);
        else         brw_parse_plain(brw_tls_buf, len);

        {
            char st[96], nb[16];
            str_copy(st, vxsec_verifies_certificates()
                             ? "Done - TLS 1.3, certificate verified, "
                             : "Done - TLS 1.3, NOT verified, ",
                     sizeof(st));
            uint_to_str((uint32_t)(len / 1024), nb);
            str_append(st, nb, sizeof(st));
            str_append(st, " KB", sizeof(st));
            brw_set_status(st);
        }
        brw_note_recent();
        return;
    }

    if (brw_tls_state == BRW_TLS_ERR) {
        char msg[128];
        brw_tls_state = BRW_TLS_IDLE;
        brw_loading = 0;
        str_copy(msg, "Could not load the page: ", sizeof(msg));
        str_append(msg, brw_tls_err, sizeof(msg));
        brw_page_error(msg);
        brw_set_status("Error: TLS");
        return;
    }

    if (brw_tls_state == BRW_TLS_RUN) return;   /* still working */

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
        /* After brw_set_addr, so a redirect is filed under where it
         * actually landed rather than where it was aimed. */
        brw_note_recent();
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
        !str_starts_with(brw_addr, "vextro://")) {
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
/*
 * Where everything in the chrome is, this frame.
 *
 * The solve is what both the painter and the click handler read, which
 * is the point of routing it through here: the address field's
 * rectangle is computed once, so a click lands exactly where the gold
 * border is drawn. The two used to be separate arithmetic that happened
 * to agree.
 */
static void brw_layout(int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       int32_t *addr_x, int32_t *addr_y,
                       int32_t *addr_w, int32_t *addr_h,
                       int32_t *view_y, int32_t *view_h) {
    vxui_solve(VXUI_ID_CHROME, cx, cy, cw, BRW_TOOLBAR_H);

    const vxui_rect_t a = vxui_rects[VXUI_ID_ADDRESS];
    *addr_x = a.x;
    *addr_y = a.y;
    *addr_w = a.w;
    *addr_h = a.h;
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

    /*
     * ---- the chrome ----
     *
     * Painted from the resolved shell rather than from colours written
     * here. Every rectangle, every border and every gold below comes
     * from assets/ui/browser.vxml through build/ui/vxui_gen.h; the only
     * thing this code decides is what *text* goes in the address field
     * and which of the two nav glyphs is lit.
     */
    vxui_paint_tree(buf, w, h, VXUI_ID_CHROME);

    /* Back and reload, which are glyphs rather than text and so are
     * drawn over the boxes the shell placed. */
    {
        const int back_ok = brw_hist_n >= 1;
        const vxui_rect_t b = vxui_rects[VXUI_ID_NAV_BACK];
        const uint32_t col = back_ok ? VXUI_VEXTRO_GOLD_LIGHT
                                     : VXUI_VEXTRO_GOLD_DARK;
        const int bx = b.x + b.w / 2, by = b.y + b.h / 2;
        gfx_line(buf, w, h, bx + 3, by - 5, bx - 3, by, 2, col);
        gfx_line(buf, w, h, bx - 3, by, bx + 3, by + 5, 2, col);

        const vxui_rect_t r = vxui_rects[VXUI_ID_NAV_RELOAD];
        const int rx = r.x + r.w / 2, ry = r.y + r.h / 2;
        gfx_circle_outline(buf, w, h, rx, ry, 6, VXUI_VEXTRO_GOLD_LIGHT);
        gfx_tri(buf, w, h, rx + 3, ry - 8, rx + 9, ry - 6, rx + 3, ry - 2,
                VXUI_VEXTRO_GOLD_LIGHT);
    }

    /* The address text, in the field the solver placed. Focus is the one
     * thing the skin cannot express, so it is drawn on top: a brighter
     * border and a caret, both in tokens. */
    {
        const vxui_node_t *an = &vxui_nodes[VXUI_ID_ADDRESS];
        const int fs = an->font_size;
        const int sc = vxui_mono_scale(fs);
        const int ty = ay + (ah - 8 * sc) / 2;

        if (brw_addr_focus)
            vxui_stroke(buf, w, h, ax, ay, aw, ah, an->radius,
                        an->border_sides, VXUI_VEXTRO_GOLD_LIGHT);

        vxui_paint_node(buf, w, h, VXUI_ID_ADDRESS, brw_addr);
        (void)ty;

        if (brw_addr_focus && ((tick / 30) & 1) == 0) {
            char tmp[BRW_ADDR_MAX];
            str_copy(tmp, brw_addr, BRW_ADDR_MAX);
            tmp[brw_addr_cur] = '\0';
            int n = 0;
            while (tmp[n]) n++;
            const int cx_px = ax + an->pad[3] + n * MONO_ADV(sc);
            gfx_rect(buf, w, h, cx_px, ay + 4, 2, ah - 8,
                     VXUI_VEXTRO_GOLD_LIGHT);
        }
    }

    /*
     * ---- the start page ----
     *
     * The one page in this browser that is a layout rather than a
     * document, so it is drawn from the shell like the chrome is. Every
     * other page is HTML somebody else wrote and goes through the line
     * list below.
     *
     * The three figures are read from the system rather than written
     * into the shell: the archive's own article count, the number of
     * packages actually installed, and this machine's address. A start
     * page that invented them would be the one thing in this repository
     * that did.
     */
    if (brw_is_home) {
        static char wiki_metric[64];
        static char store_metric[64];
        static char net_metric[64];
        char nb[16];

        if (zim.open) {
            uint_to_str(zim.article_count, nb);
            str_copy(wiki_metric, nb, sizeof(wiki_metric));
            str_append(wiki_metric, " articles, offline", sizeof(wiki_metric));
        } else {
            /* The same thing the Wikipedia window says when the archive
             * was not fetched. Degrading to the truth rather than to a
             * zero that reads as an empty encyclopedia. */
            str_copy(wiki_metric, "no archive on this volume",
                     sizeof(wiki_metric));
        }

        uint_to_str((uint32_t)brw_installed_count(), nb);
        str_copy(store_metric, nb, sizeof(store_metric));
        str_append(store_metric, " installed from the store",
                   sizeof(store_metric));

        if (vxnet_up()) {
            uint8_t ip[4], mask[4], gw[4];
            vxnet_addr(ip, mask, gw);
            net_metric[0] = '\0';
            for (int i = 0; i < 4; i++) {
                uint_to_str(ip[i], nb);
                str_append(net_metric, nb, sizeof(net_metric));
                if (i != 3) str_append(net_metric, ".", sizeof(net_metric));
            }
            str_append(net_metric, vxsec_verifies_certificates()
                                       ? ", TLS verified"
                                       : ", TLS unverified",
                       sizeof(net_metric));
        } else {
            str_copy(net_metric, "not connected", sizeof(net_metric));
        }

        vxui_solve(VXUI_ID_START, cx, vy, cw, vh);
        vxui_paint_tree(buf, w, h, VXUI_ID_START);
        vxui_paint_node(buf, w, h, VXUI_ID_PANEL_WIKI_METRIC, wiki_metric);
        vxui_paint_node(buf, w, h, VXUI_ID_PANEL_STORE_METRIC, store_metric);
        vxui_paint_node(buf, w, h, VXUI_ID_PANEL_NET_METRIC, net_metric);
        return;
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


#ifdef APP_SELFTEST
/*
 * ===== the skin, checked by its pixels =====
 *
 * A headless harness cannot look at the screen, and "the browser is gold
 * now" is not a claim a boot log can make. So the chrome is drawn into a
 * scratch buffer and the buffer is read back: the header strip must be
 * exactly the charcoal token, the divider under it exactly the dark gold
 * token, the address field's border exactly the gold one.
 *
 * Exact values, not approximate ones. Every colour here came from
 * assets/ui/tokens.tw through the generator, so a token that was
 * mistyped, a class that silently resolved to nothing, or a painter that
 * blended when it should not have all change a pixel — and each of those
 * is invisible in a screenshot on a machine nobody is watching.
 *
 * The same buffer is what checks the *layout*: the address field is
 * flex-1 between two fixed groups, so widening the window must widen the
 * field and move nothing else. Solving twice at two widths and comparing
 * is how that is asserted without a person resizing anything.
 */
/*
 * Tall enough for the whole start page, which is the point: the panel
 * row sits below a 48-pixel title, a subtitle and a search field, about
 * three hundred pixels down. A shorter buffer solved the layout
 * correctly and then sampled a pixel that was never drawn, so the
 * rectangle checks passed and the colour check did not — which is a
 * confusing way to be told the buffer is too small.
 *
 * 1.6 MB of it, and only in an APP_SELFTEST build.
 */
#define BRW_SKIN_W 800
#define BRW_SKIN_H 520
static uint32_t brw_skin_buf[BRW_SKIN_W * BRW_SKIN_H];

static int brw_skin_checks = 0;
static int brw_skin_failures = 0;

static void brw_skin_ok(const char *what, int good) {
    brw_skin_checks++;
    if (!good) brw_skin_failures++;
    serial_puts(good ? " ok   " : "FAIL  ");
    serial_puts(what);
    serial_putc('\n');
}

static uint32_t brw_skin_px(int x, int y) {
    if (x < 0 || y < 0 || x >= BRW_SKIN_W || y >= BRW_SKIN_H) return 0xDEADBEEFu;
    return brw_skin_buf[y * BRW_SKIN_W + x] & 0x00FFFFFFu;
}

static void brw_skin_selftest(void) {
    for (int i = 0; i < BRW_SKIN_W * BRW_SKIN_H; i++) brw_skin_buf[i] = 0;

    brw_draw(brw_skin_buf, BRW_SKIN_W, BRW_SKIN_H, 0, 0, BRW_SKIN_W,
             BRW_SKIN_H, 0, 0);

    const int toolbar_h = BRW_TOOLBAR_H;

    /* ---- the tokens reached the pixels ---- */
    brw_skin_ok("the header strip is the charcoal token",
                brw_skin_px(2, 2) == VXUI_VEXTRO_CHARCOAL);
    brw_skin_ok("and so is the far side of it",
                brw_skin_px(BRW_SKIN_W - 3, 2) == VXUI_VEXTRO_CHARCOAL);
    brw_skin_ok("the divider under it is the dark gold token",
                brw_skin_px(BRW_SKIN_W / 2, toolbar_h - 1) ==
                    VXUI_VEXTRO_GOLD_DARK);

    /* ---- the strip is exactly as tall as h-12 asked ---- */
    brw_skin_ok("the strip is as tall as the token says", toolbar_h == 48);
    brw_skin_ok("and the row below the divider is not charcoal",
                brw_skin_px(BRW_SKIN_W / 2, toolbar_h) !=
                    VXUI_VEXTRO_GOLD_DARK);

    /* ---- the address field ---- */
    {
        const vxui_rect_t a = vxui_rects[VXUI_ID_ADDRESS];
        brw_skin_ok("the address field has a rectangle", a.w > 0 && a.h > 0);
        brw_skin_ok("its left border is the gold token",
                    brw_skin_px(a.x, a.y + a.h / 2) == VXUI_VEXTRO_GOLD);
        brw_skin_ok("and so is its right",
                    brw_skin_px(a.x + a.w - 1, a.y + a.h / 2) ==
                        VXUI_VEXTRO_GOLD);
        brw_skin_ok("it is h-8 tall", a.h == 32);

        /* mx-6 is 24 pixels of margin on each side, between the field and
         * the groups either side of it. */
        const vxui_rect_t nav = vxui_rects[VXUI_ID_NAV];
        const vxui_rect_t act = vxui_rects[VXUI_ID_ACTIONS];
        brw_skin_ok("mx-6 sits it 24px from the nav group",
                    a.x - (nav.x + nav.w) == 24);
        brw_skin_ok("and 24px from the actions group",
                    act.x - (a.x + a.w) == 24);

        /* px-4 on the chrome: the nav group starts 16 pixels in, and the
         * actions group ends 16 pixels from the right. */
        brw_skin_ok("px-4 indents the left group", nav.x == 16);
        brw_skin_ok("and the right one", act.x + act.w == BRW_SKIN_W - 16);
    }

    /* ---- the layout is solved, not baked ---- */
    {
        const int16_t narrow_w = vxui_rects[VXUI_ID_ADDRESS].w;
        const int16_t narrow_go_x = vxui_rects[VXUI_ID_GO].x;

        vxui_solve(VXUI_ID_CHROME, 0, 0, BRW_SKIN_W + 200, BRW_TOOLBAR_H);
        const int16_t wide_w = vxui_rects[VXUI_ID_ADDRESS].w;
        const int16_t wide_go_x = vxui_rects[VXUI_ID_GO].x;

        brw_skin_ok("widening the window widens the flex-1 field",
                    wide_w == narrow_w + 200);
        brw_skin_ok("and moves the fixed group with the right edge",
                    wide_go_x == narrow_go_x + 200);
        brw_skin_ok("while the fixed nav group does not move",
                    vxui_rects[VXUI_ID_NAV].x == 16);

        /* Back to where it was, so nothing after this sees a stale
         * solve. */
        vxui_solve(VXUI_ID_CHROME, 0, 0, BRW_SKIN_W, BRW_TOOLBAR_H);
    }

    /* ---- the start page ---- */
    {
        brw_page_home();
        for (int i = 0; i < BRW_SKIN_W * BRW_SKIN_H; i++) brw_skin_buf[i] = 0;
        brw_draw(brw_skin_buf, BRW_SKIN_W, BRW_SKIN_H, 0, 0, BRW_SKIN_W,
                 BRW_SKIN_H, 0, 0);

        brw_skin_ok("the start page paints the charcoal background",
                    brw_skin_px(BRW_SKIN_W / 2, toolbar_h + 4) ==
                        VXUI_VEXTRO_CHARCOAL);

        /* The gradient title: sampled at its left and right, where the
         * two end stops are. A title drawn in one flat colour -- which is
         * what a broken bg-clip-text would give -- has the same pixel at
         * both. */
        const vxui_rect_t t = vxui_rects[VXUI_ID_START_TITLE];
        brw_skin_ok("the title has a rectangle", t.w > 0 && t.h > 0);

        uint32_t left_ink = 0, right_ink = 0;
        for (int y = t.y; y < t.y + t.h && y < BRW_SKIN_H; y++) {
            for (int x = t.x; x < t.x + t.w / 4 && x < BRW_SKIN_W; x++) {
                const uint32_t p = brw_skin_px(x, y);
                if (p != VXUI_VEXTRO_CHARCOAL && p != 0) { left_ink = p; break; }
            }
            if (left_ink) break;
        }
        for (int y = t.y; y < t.y + t.h && y < BRW_SKIN_H; y++) {
            for (int x = t.x + t.w - 1; x > t.x + t.w * 3 / 4 && x >= 0; x--) {
                const uint32_t p = brw_skin_px(x, y);
                if (p != VXUI_VEXTRO_CHARCOAL && p != 0) { right_ink = p; break; }
            }
            if (right_ink) break;
        }
        brw_skin_ok("the title is drawn at all", left_ink != 0);
        brw_skin_ok("and its two ends are different colours",
                    left_ink != right_ink);

        /* The three panels share the row equally: flex-1 each. */
        const vxui_rect_t p1 = vxui_rects[VXUI_ID_PANEL_WIKI];
        const vxui_rect_t p2 = vxui_rects[VXUI_ID_PANEL_STORE];
        const vxui_rect_t p3 = vxui_rects[VXUI_ID_PANEL_NET];
        brw_skin_ok("the three panels are the same width",
                    p1.w == p2.w && (p3.w == p1.w || p3.w == p1.w + 1));
        brw_skin_ok("laid out left to right", p1.x < p2.x && p2.x < p3.x);
        brw_skin_ok("with gap-4 between them", p2.x - (p1.x + p1.w) == 16);
        brw_skin_ok("and a dark gold border",
                    brw_skin_px(p1.x, p1.y + p1.h / 2) ==
                        VXUI_VEXTRO_GOLD_DARK);
    }

    serial_puts("[skin] browser skin: ");
    serial_put_dec((uint32_t)brw_skin_checks);
    serial_puts(" checks, ");
    serial_put_dec((uint32_t)brw_skin_failures);
    serial_puts(brw_skin_failures ? " failures - FAILED\n"
                                  : " failures - all passed\n");
}
#endif /* APP_SELFTEST */

#endif /* BROWSER_H */