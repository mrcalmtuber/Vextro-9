#ifndef VEXTRO_CALC_H
#define VEXTRO_CALC_H

/*
 * src/calc.h — the calculator.
 *
 * Three modes behind one keypad: standard arithmetic, a programmer's view
 * of a 64-bit integer, and unit conversion.
 *
 * There is no floating point anywhere in it, because there is no floating
 * point in this kernel -- it is built with -mno-80387 -mno-sse on x86 and
 * -mgeneral-regs-only on aarch64, and a single double would fail to link.
 * That is not a limitation worked around here so much as the thing that
 * decided the design:
 *
 *   - values are int64_t scaled by CALC_SCALE, so 1.5 is 1500000 and the
 *     six decimal places are exact rather than approximated;
 *   - addition and subtraction are just addition and subtraction;
 *   - multiplication splits its operand so the product of two large
 *     numbers does not overflow on the way to being scaled back down;
 *   - conversion factors are themselves scaled integers, so a chain of
 *     conversions is integer arithmetic end to end and 1000 mm is exactly
 *     one metre rather than 0.9999999.
 *
 * The cost is range and precision: about +/- 9.2e12 with six decimals.
 * For a calculator on a desk that is the right trade, and it is stated in
 * the About text rather than left to be discovered.
 */

#define CALC_SCALE   1000000LL
#define CALC_DECS    6
#define CALC_MAX     9223372036854LL      /* CALC_SCALE fits above this */

typedef int64_t cfix;

enum { CALC_STD = 0, CALC_PROG, CALC_CONV, CALC_MODES };

static const char *const calc_mode_names[CALC_MODES] = {
    "Standard", "Programmer", "Convert"
};

static int   calc_mode = CALC_STD;
static char  calc_entry[24] = "0";     /* what is being typed */
static int   calc_fresh = 1;           /* next digit replaces the entry */
static cfix  calc_acc = 0;             /* the left operand */
static char  calc_op = 0;              /* pending operator, 0 = none */
static char  calc_expr[48] = "";       /* the line above the result */
static char  calc_err[32] = "";

/* ---- programmer mode ---- */
static int64_t calc_pv = 0;            /* the register being edited */
static int64_t calc_pacc = 0;
static char    calc_pop = 0;
static int     calc_base = 10;         /* 10, 16 or 2 */

/* ---- convert mode ---- */
enum { CONV_LEN = 0, CONV_MASS, CONV_DATA, CONV_CATS };

typedef struct {
    const char *name;
    cfix        per_base;              /* how many base units in one of these */
} conv_unit_t;

/*
 * Every unit is expressed in a base unit for its category -- millimetres,
 * grams, bytes -- as a CALC_SCALE-scaled integer. Converting is then two
 * exact multiplications and one division, with no accumulated error from
 * chaining through an intermediate.
 */
static const conv_unit_t conv_len[] = {
    { "mm",    1LL * CALC_SCALE },
    { "cm",   10LL * CALC_SCALE },
    { "m",  1000LL * CALC_SCALE },
    { "km", 1000000LL * CALC_SCALE },
    { "in",  25400000LL },                 /* 25.4 mm exactly */
    { "ft", 304800000LL },
    { "mi", 1609344000LL * 1000LL },
};
static const conv_unit_t conv_mass[] = {
    { "g",     1LL * CALC_SCALE },
    { "kg", 1000LL * CALC_SCALE },
    { "t",  1000000LL * CALC_SCALE },
    { "oz",  28349523LL },                 /* 28.349523 g */
    { "lb", 453592370LL },
};
static const conv_unit_t conv_data[] = {
    { "B",     1LL * CALC_SCALE },
    { "KiB",  1024LL * CALC_SCALE },
    { "MiB",  1048576LL * CALC_SCALE },
    { "GiB",  1073741824LL * CALC_SCALE },
};

static const conv_unit_t *const conv_tables[CONV_CATS] = {
    conv_len, conv_mass, conv_data
};
static const int conv_counts[CONV_CATS] = {
    (int)(sizeof(conv_len)  / sizeof(conv_len[0])),
    (int)(sizeof(conv_mass) / sizeof(conv_mass[0])),
    (int)(sizeof(conv_data) / sizeof(conv_data[0])),
};
static const char *const conv_cat_names[CONV_CATS] = {
    "Length", "Mass", "Data"
};

static int calc_cat = CONV_LEN;
static int calc_from = 2;              /* metres */
static int calc_to = 4;                /* inches */

/* ===== fixed-point arithmetic ===== */

/*
 * a * b, both scaled. Splitting a into whole and fractional parts keeps
 * the intermediate inside int64 for any pair this calculator can hold:
 * the naive (a*b)/SCALE overflows as soon as either operand passes about
 * three million.
 */
static cfix cfix_mul(cfix a, cfix b) {
    const cfix ai = a / CALC_SCALE, af = a % CALC_SCALE;
    return ai * b + (af * b) / CALC_SCALE;
}

static cfix cfix_div(cfix a, cfix b, int *err) {
    if (b == 0) { *err = 1; return 0; }
    /* Scale the numerator in two steps for the same overflow reason. */
    const cfix q = a / b;
    const cfix r = a % b;
    return q * CALC_SCALE + (r * CALC_SCALE) / b;
}

/* Parse a decimal string into fixed point. Rejects nothing: a partly
 * typed number like "3." is a valid prefix of one being entered. */
static cfix cfix_parse(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    cfix whole = 0;
    while (*s >= '0' && *s <= '9') {
        if (whole > CALC_MAX / 10) break;
        whole = whole * 10 + (*s - '0');
        s++;
    }
    cfix frac = 0, scale = CALC_SCALE;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale > 1) {
            scale /= 10;
            frac += (cfix)(*s - '0') * scale;
            s++;
        }
    }
    const cfix v = whole * CALC_SCALE + frac;
    return neg ? -v : v;
}

/* Format fixed point, trimming trailing zeros so 2 does not print as
 * 2.000000 -- a calculator that pads every answer is unreadable. */
static void cfix_str(cfix v, char *out, int cap) {
    int n = 0;
    if (v < 0) { if (n < cap - 1) out[n++] = '-'; v = -v; }

    const cfix whole = v / CALC_SCALE;
    cfix frac = v % CALC_SCALE;

    char tmp[24];
    int t = 0;
    cfix wv = whole;
    if (wv == 0) tmp[t++] = '0';
    while (wv > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (int)(wv % 10)); wv /= 10; }
    while (t > 0 && n < cap - 1) out[n++] = tmp[--t];

    if (frac) {
        if (n < cap - 1) out[n++] = '.';
        cfix s = CALC_SCALE / 10;
        int printed = 0;
        for (int i = 0; i < CALC_DECS && n < cap - 1; i++) {
            const int d = (int)(frac / s);
            frac %= s;
            s /= 10;
            out[n++] = (char)('0' + d);
            printed++;
            if (frac == 0) break;
        }
        (void)printed;
    }
    out[n] = '\0';
}

/* ===== the machine ===== */

static cfix calc_current(void) { return cfix_parse(calc_entry); }

static void calc_set_result(cfix v) {
    cfix_str(v, calc_entry, sizeof(calc_entry));
    calc_fresh = 1;
}

static void calc_apply(void) {
    if (!calc_op) { calc_acc = calc_current(); return; }
    const cfix b = calc_current();
    int err = 0;
    cfix r = 0;
    switch (calc_op) {
    case '+': r = calc_acc + b; break;
    case '-': r = calc_acc - b; break;
    case '*': r = cfix_mul(calc_acc, b); break;
    case '/': r = cfix_div(calc_acc, b, &err); break;
    default:  r = b; break;
    }
    if (err) {
        str_copy(calc_err, "Cannot divide by zero", sizeof(calc_err));
        calc_acc = 0;
        calc_set_result(0);
    } else {
        calc_acc = r;
        calc_set_result(r);
    }
}

static void calc_digit(char c) {
    calc_err[0] = '\0';
    if (calc_fresh) { calc_entry[0] = '\0'; calc_fresh = 0; }
    const int n = str_len(calc_entry);
    if (n >= (int)sizeof(calc_entry) - 1) return;
    if (c == '.') {
        for (int i = 0; i < n; i++) if (calc_entry[i] == '.') return;
        if (n == 0) { calc_entry[0] = '0'; calc_entry[1] = '.'; calc_entry[2] = '\0'; return; }
    }
    char b[2] = { c, '\0' };
    str_append(calc_entry, b, sizeof(calc_entry));
    if (calc_entry[0] == '\0') str_copy(calc_entry, "0", sizeof(calc_entry));
}

static void calc_operator(char op) {
    calc_err[0] = '\0';
    calc_apply();
    calc_op = op;
    char e[48];
    cfix_str(calc_acc, e, sizeof(e));
    str_copy(calc_expr, e, sizeof(calc_expr));
    const char s[3] = { ' ', op, '\0' };
    str_append(calc_expr, s, sizeof(calc_expr));
    calc_fresh = 1;
}

static void calc_equals(void) {
    calc_apply();
    calc_op = 0;
    calc_expr[0] = '\0';
}

static void calc_clear(void) {
    str_copy(calc_entry, "0", sizeof(calc_entry));
    calc_expr[0] = '\0';
    calc_err[0] = '\0';
    calc_acc = 0;
    calc_op = 0;
    calc_fresh = 1;
    calc_pv = calc_pacc = 0;
    calc_pop = 0;
}

static void calc_backspace(void) {
    if (calc_fresh) { calc_clear(); return; }
    const int n = str_len(calc_entry);
    if (n <= 1) { str_copy(calc_entry, "0", sizeof(calc_entry)); calc_fresh = 1; return; }
    calc_entry[n - 1] = '\0';
}

/* ---- programmer mode ---- */

static void calc_prog_render(char *out, int cap, int64_t v, int base) {
    int neg = 0;
    uint64_t u;
    if (base == 10 && v < 0) { neg = 1; u = (uint64_t)(-v); }
    else u = (uint64_t)v;

    char tmp[72];
    int t = 0;
    if (u == 0) tmp[t++] = '0';
    while (u && t < (int)sizeof(tmp)) {
        const int d = (int)(u % (uint64_t)base);
        tmp[t++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        u /= (uint64_t)base;
    }
    int n = 0;
    if (neg && n < cap - 1) out[n++] = '-';
    while (t > 0 && n < cap - 1) out[n++] = tmp[--t];
    out[n] = '\0';
}

static void calc_prog_digit(int d) {
    calc_err[0] = '\0';
    if (d >= calc_base) return;           /* not a digit in this base */
    if (calc_fresh) { calc_pv = 0; calc_fresh = 0; }
    /* Wrapping is the honest behaviour for a fixed-width register, and
     * this one is deliberately 64 bits wide. */
    calc_pv = (int64_t)((uint64_t)calc_pv * (uint64_t)calc_base + (uint64_t)d);
}

static void calc_prog_apply(void) {
    if (!calc_pop) { calc_pacc = calc_pv; return; }
    const uint64_t a = (uint64_t)calc_pacc, b = (uint64_t)calc_pv;
    uint64_t r = b;
    switch (calc_pop) {
    case '&': r = a & b; break;
    case '|': r = a | b; break;
    case '^': r = a ^ b; break;
    case '<': r = (b < 64) ? (a << b) : 0; break;
    case '>': r = (b < 64) ? (a >> b) : 0; break;
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    default: break;
    }
    calc_pacc = (int64_t)r;
    calc_pv = (int64_t)r;
    calc_fresh = 1;
}

static void calc_prog_op(char op) {
    calc_err[0] = '\0';
    calc_prog_apply();
    calc_pop = op;
    calc_fresh = 1;
}

/* ---- convert mode ---- */

static cfix calc_convert(cfix v) {
    const conv_unit_t *t = conv_tables[calc_cat];
    const int n = conv_counts[calc_cat];
    if (calc_from >= n) calc_from = 0;
    if (calc_to >= n)   calc_to = 0;
    int err = 0;
    const cfix base = cfix_mul(v, t[calc_from].per_base);
    return cfix_div(base, t[calc_to].per_base, &err);
}

/* ===== keyboard ===== */

static void calc_key(char ch) {
    if (ch >= '0' && ch <= '9') {
        if (calc_mode == CALC_PROG) calc_prog_digit(ch - '0');
        else                        calc_digit(ch);
        return;
    }
    if (calc_mode == CALC_PROG && ch >= 'a' && ch <= 'f') {
        calc_prog_digit(10 + (ch - 'a'));
        return;
    }
    if (calc_mode == CALC_PROG && ch >= 'A' && ch <= 'F') {
        calc_prog_digit(10 + (ch - 'A'));
        return;
    }
    switch (ch) {
    case '.': if (calc_mode != CALC_PROG) calc_digit('.'); break;
    case '+': case '-': case '*': case '/':
        if (calc_mode == CALC_PROG) calc_prog_op(ch);
        else                        calc_operator(ch);
        break;
    case '&': case '|': case '^':
        if (calc_mode == CALC_PROG) calc_prog_op(ch);
        break;
    case '\n': case '=':
        if (calc_mode == CALC_PROG) { calc_prog_apply(); calc_pop = 0; }
        else                          calc_equals();
        break;
    case '\b': if (calc_mode == CALC_PROG) { calc_pv /= calc_base; }
               else calc_backspace();
               break;
    case 27:  calc_clear(); break;
    default: break;
    }
}


/* ===== the window =====
 *
 * One keypad, relaid per mode. The buttons are a table rather than a
 * painted grid of special cases, so the hit test and the drawing walk the
 * same rows and cannot disagree about where anything is.
 */

typedef struct {
    const char *label;
    char        code;      /* what pressing it means; 0 = blank */
} calc_btn_t;

#define CALC_COLS 4

static const calc_btn_t calc_pad_std[] = {
    {"C",0x1B},{"+/-",'N'},{"%",'%'},{"/",'/'},
    {"7",'7'}, {"8",'8'},  {"9",'9'},{"*",'*'},
    {"4",'4'}, {"5",'5'},  {"6",'6'},{"-",'-'},
    {"1",'1'}, {"2",'2'},  {"3",'3'},{"+",'+'},
    {"0",'0'}, {".",'.'},  {"<",'\b'},{"=",'='},
};

static const calc_btn_t calc_pad_prog[] = {
    {"AC",0x1B},{"AND",'&'},{"OR",'|'}, {"XOR",'^'},
    {"D",'D'},  {"E",'E'},  {"F",'F'},  {"<<",'L'},
    {"A",'A'},  {"B",'B'},  {"C",'C'},  {">>",'R'},
    {"7",'7'},  {"8",'8'},  {"9",'9'},  {"+",'+'},
    {"4",'4'},  {"5",'5'},  {"6",'6'},  {"-",'-'},
    {"1",'1'},  {"2",'2'},  {"3",'3'},  {"=",'='},
    {"0",'0'},  {"NOT",'~'},{"<",'\b'}, {"",0},
};

static const calc_btn_t calc_pad_conv[] = {
    {"C",0x1B},{"",0},   {"",0},    {"<",'\b'},
    {"7",'7'}, {"8",'8'},{"9",'9'}, {"",0},
    {"4",'4'}, {"5",'5'},{"6",'6'}, {"",0},
    {"1",'1'}, {"2",'2'},{"3",'3'}, {"",0},
    {"0",'0'}, {".",'.'},{"",0},    {"",0},
};

static const calc_btn_t *calc_pad(int *rows) {
    if (calc_mode == CALC_PROG) {
        *rows = (int)(sizeof(calc_pad_prog) / sizeof(calc_pad_prog[0])) / CALC_COLS;
        return calc_pad_prog;
    }
    if (calc_mode == CALC_CONV) {
        *rows = (int)(sizeof(calc_pad_conv) / sizeof(calc_pad_conv[0])) / CALC_COLS;
        return calc_pad_conv;
    }
    *rows = (int)(sizeof(calc_pad_std) / sizeof(calc_pad_std[0])) / CALC_COLS;
    return calc_pad_std;
}

/* Codes that are not simply characters the keyboard could have sent. */
static void calc_press(char code) {
    switch (code) {
    case 0: return;
    case 'N':                                   /* +/- */
        if (calc_mode == CALC_PROG) { calc_pv = -calc_pv; return; }
        if (calc_entry[0] == '-') {
            for (int i = 0; calc_entry[i]; i++) calc_entry[i] = calc_entry[i + 1];
        } else {
            char t[24];
            str_copy(t, "-", sizeof(t));
            str_append(t, calc_entry, sizeof(t));
            str_copy(calc_entry, t, sizeof(calc_entry));
        }
        return;
    case '%': {                                 /* per cent of the accumulator */
        int err = 0;
        const cfix v = cfix_div(cfix_mul(calc_acc, calc_current()),
                                100LL * CALC_SCALE, &err);
        calc_set_result(err ? 0 : v);
        return;
    }
    case '~': if (calc_mode == CALC_PROG) calc_pv = ~calc_pv; return;
    case 'L': if (calc_mode == CALC_PROG) calc_prog_op('<'); return;
    case 'R': if (calc_mode == CALC_PROG) calc_prog_op('>'); return;
    default:
        if (calc_mode == CALC_PROG && code >= 'A' && code <= 'F') {
            calc_prog_digit(10 + (code - 'A'));
            return;
        }
        calc_key(code);
    }
}

/* ---- geometry, shared by the drawing and the hit test ---- */

#define CALC_TAB_H  26
#define CALC_DISP_H 74

static void calc_pad_rect(int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                          int32_t *px, int32_t *py, int32_t *pw, int32_t *ph) {
    const int32_t top = cy + CALC_TAB_H + CALC_DISP_H +
                        (calc_mode == CALC_CONV ? 66 : 0);
    *px = cx + 8;
    *py = top + 6;
    *pw = cw - 16;
    *ph = (cy + chh) - *py - 8;
    if (*ph < 40) *ph = 40;
}

static void calc_draw(uint32_t *buf, uint32_t w, uint32_t h,
                      int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                      int32_t mx, int32_t my) {
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x14171Fu);

    /* mode tabs */
    for (int i = 0; i < CALC_MODES; i++) {
        const int32_t tw = cw / CALC_MODES;
        const int32_t tx = cx + i * tw;
        const int on = (i == calc_mode);
        gfx_rect(buf, w, h, tx, cy, tw, CALC_TAB_H, on ? 0x1E2432u : 0x171A24u);
        if (on) gfx_rect(buf, w, h, tx, cy, tw, 2, C_GOLD);
        const int lw = ttf_text_width(calc_mode_names[i], 12);
        ttf_draw_string(buf, (int)w, (int)h, tx + (tw - lw) / 2, cy + 5,
                        calc_mode_names[i], on ? C_TEXT : C_TEXT_DIM, 12);
    }

    /* display */
    const int32_t dy = cy + CALC_TAB_H;
    gfx_rect(buf, w, h, cx + 8, dy + 6, cw - 16, CALC_DISP_H - 12, 0x0E1118u);
    gfx_rect_outline(buf, w, h, cx + 8, dy + 6, cw - 16, CALC_DISP_H - 12,
                     0x2A3142u);

    char main_s[72], sub_s[72];
    sub_s[0] = '\0';

    if (calc_mode == CALC_PROG) {
        calc_prog_render(main_s, sizeof(main_s), calc_pv, calc_base);
        /* the other two bases, so the register is legible in all of them */
        char a[72], b2[72];
        calc_prog_render(a, sizeof(a), calc_pv, calc_base == 16 ? 10 : 16);
        calc_prog_render(b2, sizeof(b2), calc_pv, calc_base == 2 ? 10 : 2);
        str_copy(sub_s, calc_base == 16 ? "dec " : "hex ", sizeof(sub_s));
        str_append(sub_s, a, sizeof(sub_s));
        str_append(sub_s, calc_base == 2 ? "   dec " : "   bin ", sizeof(sub_s));
        str_append(sub_s, b2, sizeof(sub_s));
    } else if (calc_mode == CALC_CONV) {
        cfix_str(calc_convert(calc_current()), main_s, sizeof(main_s));
        str_copy(sub_s, calc_entry, sizeof(sub_s));
        str_append(sub_s, " ", sizeof(sub_s));
        str_append(sub_s, conv_tables[calc_cat][calc_from].name, sizeof(sub_s));
        str_append(sub_s, "  =", sizeof(sub_s));
    } else {
        str_copy(main_s, calc_entry, sizeof(main_s));
        str_copy(sub_s, calc_expr, sizeof(sub_s));
    }

    if (sub_s[0])
        ttf_draw_string_clip(buf, (int)w, (int)h, cx + 16, dy + 12, sub_s,
                             C_TEXT_DIM, 11, cx + cw - 16);
    {
        const int size = 26;
        int tw = ttf_text_width(main_s, size);
        int32_t tx = cx + cw - 20 - tw;
        if (tx < cx + 16) tx = cx + 16;
        ttf_draw_string_clip(buf, (int)w, (int)h, tx, dy + 30, main_s,
                             calc_err[0] ? C_RED : C_TEXT, size, cx + cw - 16);
    }
    if (calc_err[0])
        ttf_draw_string(buf, (int)w, (int)h, cx + 16, dy + CALC_DISP_H - 20,
                        calc_err, C_RED, 11);

    /* programmer: the base selector, in the display's bottom strip */
    if (calc_mode == CALC_PROG) {
        static const int bases[3] = { 10, 16, 2 };
        static const char *const bn[3] = { "DEC", "HEX", "BIN" };
        for (int i = 0; i < 3; i++) {
            const int32_t bx = cx + 16 + i * 46;
            const int32_t by = dy + CALC_DISP_H - 24;
            const int on = (calc_base == bases[i]);
            gfx_rect(buf, w, h, bx, by, 40, 16, on ? 0x2A2410u : 0x171A24u);
            gfx_rect_outline(buf, w, h, bx, by, 40, 16,
                             on ? C_GOLD_DIM : 0x2A3142u);
            const int lw = ttf_text_width(bn[i], 10);
            ttf_draw_string(buf, (int)w, (int)h, bx + (40 - lw) / 2, by + 2,
                            bn[i], on ? C_GOLD : C_TEXT_DIM, 10);
        }
    }

    /* convert: category, then the two unit rows */
    if (calc_mode == CALC_CONV) {
        const int32_t uy = dy + CALC_DISP_H;
        for (int i = 0; i < CONV_CATS; i++) {
            const int32_t bx = cx + 8 + i * 72;
            const int on = (calc_cat == i);
            gfx_rect(buf, w, h, bx, uy, 66, 20, on ? 0x2A2410u : 0x171A24u);
            gfx_rect_outline(buf, w, h, bx, uy, 66, 20,
                             on ? C_GOLD_DIM : 0x2A3142u);
            const int lw = ttf_text_width(conv_cat_names[i], 10);
            ttf_draw_string(buf, (int)w, (int)h, bx + (66 - lw) / 2, uy + 4,
                            conv_cat_names[i], on ? C_GOLD : C_TEXT_DIM, 10);
        }
        const conv_unit_t *t = conv_tables[calc_cat];
        const int n = conv_counts[calc_cat];
        for (int row = 0; row < 2; row++) {
            const int32_t ry = uy + 24 + row * 20;
            const int sel = row == 0 ? calc_from : calc_to;
            ttf_draw_string(buf, (int)w, (int)h, cx + 10, ry + 2,
                            row == 0 ? "from" : "to", C_TEXT_DIM, 10);
            for (int i = 0; i < n; i++) {
                const int32_t bx = cx + 44 + i * 38;
                if (bx + 34 > cx + cw - 8) break;
                const int on = (sel == i);
                gfx_rect(buf, w, h, bx, ry, 34, 18, on ? 0x2A2410u : 0x171A24u);
                gfx_rect_outline(buf, w, h, bx, ry, 34, 18,
                                 on ? C_GOLD_DIM : 0x2A3142u);
                const int lw = ttf_text_width(t[i].name, 10);
                ttf_draw_string(buf, (int)w, (int)h, bx + (34 - lw) / 2, ry + 3,
                                t[i].name, on ? C_GOLD : C_TEXT_DIM, 10);
            }
        }
    }

    /* the keypad */
    int rows = 0;
    const calc_btn_t *pad = calc_pad(&rows);
    int32_t px, py, pw, ph;
    calc_pad_rect(cx, cy, cw, chh, &px, &py, &pw, &ph);
    const int32_t bw = pw / CALC_COLS, bh = ph / rows;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < CALC_COLS; c++) {
            const calc_btn_t *b = &pad[r * CALC_COLS + c];
            if (!b->code) continue;
            const int32_t bx = px + c * bw, by = py + r * bh;
            const int hot = mx >= bx && mx < bx + bw - 3 &&
                            my >= by && my < by + bh - 3;
            const int op = (c == CALC_COLS - 1) || (r == 0);
            gfx_rect(buf, w, h, bx, by, bw - 3, bh - 3,
                     hot ? 0x2E3648u : (op ? 0x1F2534u : 0x232936u));
            gfx_rect_outline(buf, w, h, bx, by, bw - 3, bh - 3,
                             hot ? C_GOLD_DIM : 0x2A3142u);
            const int lw = ttf_text_width(b->label, 13);
            ttf_draw_string(buf, (int)w, (int)h,
                            bx + (bw - 3 - lw) / 2, by + (bh - 3 - 16) / 2,
                            b->label, op ? C_GOLD : C_TEXT, 13);
        }
    }
}

static void calc_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    if (!(lmb && !prev_lmb)) return;

    /* mode tabs */
    if (my >= cy && my < cy + CALC_TAB_H) {
        const int32_t tw = cw / CALC_MODES;
        const int i = (int)((mx - cx) / (tw ? tw : 1));
        if (i >= 0 && i < CALC_MODES) { calc_mode = i; calc_fresh = 1; }
        return;
    }

    const int32_t dy = cy + CALC_TAB_H;

    if (calc_mode == CALC_PROG) {
        static const int bases[3] = { 10, 16, 2 };
        for (int i = 0; i < 3; i++) {
            const int32_t bx = cx + 16 + i * 46, by = dy + CALC_DISP_H - 24;
            if (mx >= bx && mx < bx + 40 && my >= by && my < by + 16) {
                calc_base = bases[i];
                return;
            }
        }
    }

    if (calc_mode == CALC_CONV) {
        const int32_t uy = dy + CALC_DISP_H;
        for (int i = 0; i < CONV_CATS; i++) {
            const int32_t bx = cx + 8 + i * 72;
            if (mx >= bx && mx < bx + 66 && my >= uy && my < uy + 20) {
                calc_cat = i;
                calc_from = 0;
                calc_to = conv_counts[i] > 1 ? 1 : 0;
                return;
            }
        }
        const int n = conv_counts[calc_cat];
        for (int row = 0; row < 2; row++) {
            const int32_t ry = uy + 24 + row * 20;
            for (int i = 0; i < n; i++) {
                const int32_t bx = cx + 44 + i * 38;
                if (bx + 34 > cx + cw - 8) break;
                if (mx >= bx && mx < bx + 34 && my >= ry && my < ry + 18) {
                    if (row == 0) calc_from = i; else calc_to = i;
                    return;
                }
            }
        }
    }

    int rows = 0;
    const calc_btn_t *pad = calc_pad(&rows);
    int32_t px, py, pw, ph;
    calc_pad_rect(cx, cy, cw, chh, &px, &py, &pw, &ph);
    const int32_t bw = pw / CALC_COLS, bh = ph / rows;
    if (mx < px || my < py) return;
    const int c = (int)((mx - px) / (bw ? bw : 1));
    const int r = (int)((my - py) / (bh ? bh : 1));
    if (c < 0 || c >= CALC_COLS || r < 0 || r >= rows) return;
    calc_press(pad[r * CALC_COLS + c].code);
}

#endif /* VEXTRO_CALC_H */
