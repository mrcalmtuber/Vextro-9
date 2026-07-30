/*
 * Host-side tests for src/wikidoc.h.
 *
 * The layout engine is pure computation over a byte buffer, so it can be
 * checked on the host without booting anything — which matters, because
 * the property that motivated it ("a link must not end the line") is
 * invisible in a screenshot and awkward to assert from inside the kernel.
 *
 * Build and run: make wikidoc-test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- the handful of kernel symbols wikidoc.h leans on ---- */

#define C_GOLD  0xD4AF37u
#define C_INK   0x20242Cu
#define C_LINK  0x8A6D1Fu

static int str_eq(const char *a, const char *b) {
    for (int i = 0;; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
}
static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* The real fold table, reduced to what the tests exercise. */
static char brw_fold_cp(uint32_t cp) {
    if (cp < 0x80) return (char)cp;
    switch (cp) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2010: case 0x2011: case 0x2012:
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    case 0x00A0: return ' ';
    case 0x00B7: case 0x2022: return '*';
    case 0x00D7: return 'x';
    case 0x00E9: return 'e';
    default: break;
    }
    return '?';
}

/* A proportional-ish metric: enough to exercise wrapping deterministically. */
static int ttf_text_width(const char *s, int size) {
    int n = 0;
    for (int i = 0; s[i]; i++) n++;
    return n * (size * 6 / 10);
}
static void ttf_draw_string(uint32_t *b, int w, int h, int x, int y,
                            const char *s, uint32_t c, int size) {
    (void)b;(void)w;(void)h;(void)x;(void)y;(void)s;(void)c;(void)size;
}
static void mono_text(uint32_t *b, uint32_t w, uint32_t h, int x, int y,
                      const char *s, uint32_t c, int sc) {
    (void)b;(void)w;(void)h;(void)x;(void)y;(void)s;(void)c;(void)sc;
}
static void gfx_rect(uint32_t *b, uint32_t w, uint32_t h, int x, int y,
                     int rw, int rh, uint32_t c) {
    (void)b;(void)w;(void)h;(void)x;(void)y;(void)rw;(void)rh;(void)c;
}

#include "../src/wikidoc.h"

/* ---- helpers ---- */

static int fails = 0, checks = 0;

static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  FAIL  %s\n", what); }
    else       { printf("  ok    %s\n", what); }
}

/* Reconstruct a laid-out line as plain text. */
static void line_text(int li, char *out, int max) {
    out[0] = '\0';
    int o = 0;
    wd_line_t *l = &wd_lines[li];
    for (uint16_t k = 0; k < l->nrun; k++) {
        wd_run_t *r = &wd_runs[l->run0 + k];
        for (uint16_t t = 0; t < r->len && o < max - 1; t++)
            out[o++] = wd_text[r->start + t];
    }
    out[o] = '\0';
}

static int doc_contains(const char *needle) {
    char buf[1024];
    for (int i = 0; i < wd_line_n; i++) {
        line_text(i, buf, sizeof(buf));
        if (strstr(buf, needle)) return 1;
    }
    return 0;
}

static int line_with(const char *needle) {
    char buf[1024];
    for (int i = 0; i < wd_line_n; i++) {
        line_text(i, buf, sizeof(buf));
        if (strstr(buf, needle)) return i;
    }
    return -1;
}

static void dump(const char *label) {
    char buf[1024];
    printf("--- %s: %d lines, %d runs ---\n", label, wd_line_n, wd_run_n);
    for (int i = 0; i < wd_line_n && i < 14; i++) {
        line_text(i, buf, sizeof(buf));
        printf("   [%d st=%d nrun=%d ind=%d] %s\n",
               i, wd_lines[i].style, wd_lines[i].nrun, wd_lines[i].indent, buf);
    }
}

/* ---- tests ---- */

static void t_inline_links(void) {
    /* The case that motivated the whole file. Under the browser's model
     * this became four lines, one of them the single word "satellite". */
    static const char html[] =
        "<html><body><p>The <a href=\"Moon\">Moon</a> is Earth's only "
        "natural <a href=\"Satellite\">satellite</a> and the fifth largest."
        "</p></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    dump("inline links");

    printf("\nTEST inline links do not break the line\n");
    ck(wd_line_n == 1, "the whole sentence is one line");
    ck(wd_lines[0].nrun >= 5, "that line has several runs");
    char buf[1024];
    line_text(0, buf, sizeof(buf));
    ck(strstr(buf, "The Moon is Earth's only natural satellite") != NULL,
       "words read in order with single spaces");

    int links = 0;
    for (uint16_t k = 0; k < wd_lines[0].nrun; k++)
        if (wd_runs[wd_lines[0].run0 + k].href >= 0) links++;
    ck(links == 2, "both anchors survive as linked runs");
}

static void t_comment_with_gt(void) {
    static const char html[] =
        "<html><body><!-- a => b, not text --><p>Visible.</p></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    printf("\nTEST comments containing '>' do not leak\n");
    ck(!doc_contains("not text"), "comment body is absent");
    ck(doc_contains("Visible"), "following paragraph still renders");
}

static void t_entities(void) {
    static const char html[] =
        "<html><body><p>a&#x2014;b &#8212; c&nbsp;d &amp; e&#233;f "
        "&mdash; &hellip;</p></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    char buf[1024];
    line_text(0, buf, sizeof(buf));
    printf("\nTEST entities  [%s]\n", buf);
    ck(strstr(buf, "a-b") != NULL, "hex numeric entity decodes (&#x2014;)");
    ck(strstr(buf, "- c") != NULL, "decimal numeric entity decodes (&#8212;)");
    ck(strstr(buf, "& e") != NULL, "&amp; decodes");
    ck(strchr(buf, '?') == NULL, "nothing fell back to '?'");
}

static void t_infobox(void) {
    static const char html[] =
        "<html><body><table class=\"wikitable\">"
        "<tr><th>Mass</th><td>7.34 kg</td></tr>"
        "<tr><th>Radius</th><td>1737 km</td></tr>"
        "</table></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    dump("infobox");
    printf("\nTEST table cells become readable rows\n");
    int a = line_with("Mass"), b = line_with("Radius");
    ck(a >= 0 && b >= 0, "both rows are present");
    ck(a != b, "each row is its own line");
    char buf[1024];
    if (a >= 0) {
        line_text(a, buf, sizeof(buf));
        ck(strstr(buf, "7.34") != NULL, "value sits on the same line as its key");
        ck(strstr(buf, ":") != NULL, "key and value are separated");
    }
}

static void t_chrome_removed(void) {
    static const char html[] =
        "<html><body><h2>Origin"
        "<span class=\"mw-editsection\">[edit]</span></h2>"
        "<p>Text<sup class=\"reference\">[1]</sup> continues.</p>"
        "<div class=\"navbox\">Navigation junk here</div>"
        "</body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    dump("chrome");
    printf("\nTEST Wikipedia chrome is dropped\n");
    ck(!doc_contains("edit"), "edit-section link removed");
    ck(!doc_contains("[1]"), "reference marker removed");
    ck(!doc_contains("Navigation junk"), "navbox removed");
    ck(doc_contains("Origin"), "the heading itself survives");
    ck(doc_contains("Text continues"), "prose is unbroken by the removal");
}

static void t_headings(void) {
    static const char html[] =
        "<html><body><h1>Moon</h1><p>Body.</p><h2>Orbit</h2>"
        "<p>More.</p></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    printf("\nTEST headings carry heading styles\n");
    int h1 = line_with("Moon"), h2 = line_with("Orbit");
    ck(h1 >= 0 && wd_lines[h1].style == WD_H1, "h1 gets WD_H1");
    ck(h2 >= 0 && wd_lines[h2].style == WD_H2, "h2 gets WD_H2");
    int body = line_with("Body");
    ck(body >= 0 && wd_lines[body].style == WD_BODY, "paragraph stays body");
}

static void t_lists(void) {
    static const char html[] =
        "<html><body><ul><li>Alpha</li><li>Beta</li></ul>"
        "<ol><li>One</li><li>Two</li></ol></body></html>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 900);
    dump("lists");
    printf("\nTEST lists get markers and indent\n");
    int a = line_with("Alpha");
    ck(a >= 0 && wd_lines[a].indent > 0, "bulleted item is indented");
    ck(a >= 0 && doc_contains("* Alpha"), "unordered item gets a bullet");
    ck(doc_contains("1. One") && doc_contains("2. Two"),
       "ordered items are numbered in sequence");
}

static void t_wrapping(void) {
    static char html[200000];
    int o = 0;
    o += sprintf(html + o, "<html><body>");
    for (int p = 0; p < 110; p++) {
        o += sprintf(html + o, "<p>");
        for (int wds = 0; wds < 60; wds++)
            o += sprintf(html + o, "word%d ", wds);
        o += sprintf(html + o, "</p>");
    }
    o += sprintf(html + o, "</body></html>");

    wdoc_parse((const uint8_t *)html, o, 400);
    printf("\nTEST wrapping and capacity  (%d lines)\n", wd_line_n);
    ck(wd_line_n > 700, "a long article exceeds the old 700-line cap");
    ck(!wd_truncated, "and is not truncated");

    /* No laid-out line may exceed the width it was given. */
    int worst = 0;
    for (int i = 0; i < wd_line_n; i++) {
        char buf[1024];
        line_text(i, buf, sizeof(buf));
        int px = ttf_text_width(buf, wd_size[wd_lines[i].style]) +
                 wd_lines[i].indent;
        if (px > worst) worst = px;
    }
    printf("        widest laid-out line: %d px (limit 400)\n", worst);
    ck(worst <= 400, "every line fits the wrap width");
}

static void t_pathological(void) {
    /* Truncated tags, stray '<', unterminated quotes: a parser that walks
     * off the end here would fault in the kernel. */
    static const char html[] =
        "<html><body><p>a < b</p><div class=\"unterminated><p>x</p>"
        "<a href=\"</body>";
    wdoc_parse((const uint8_t *)html, (int)sizeof(html) - 1, 500);
    printf("\nTEST malformed input is survivable\n");
    ck(1, "parser returned without faulting");
    ck(wd_text_n < WD_TEXT_MAX, "text pool within bounds");
    ck(wd_run_n <= WD_RUN_MAX && wd_line_n <= WD_LINE_MAX, "counts in range");
}

int main(void) {
    t_inline_links();
    t_comment_with_gt();
    t_entities();
    t_infobox();
    t_chrome_removed();
    t_headings();
    t_lists();
    t_wrapping();
    t_pathological();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
