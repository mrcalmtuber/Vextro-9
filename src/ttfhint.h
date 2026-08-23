#ifndef TTFHINT_H
#define TTFHINT_H

/*
 * src/ttfhint.h — the TrueType bytecode interpreter.
 *
 * A hinted font is a font with a *program* in it. Not metadata, not a
 * table of nudges: a stack machine with functions, loops, conditionals
 * and its own storage, which the rasteriser is expected to run once per
 * glyph per size, and whose job is to move the outline's points onto the
 * pixel grid before anything is filled.
 *
 * src/ttf.h skipped it. decode_simple() read the instruction length out
 * of the glyph header and stepped over the bytes:
 *
 *     uint16_t instr = be16(p); p += 2 + instr;
 *
 * That is 13,475 bytes of program in this face alone, plus a 3,605-byte
 * font program defining 141 functions and a 178-byte pre-program, all
 * discarded. This runs them.
 *
 * ---- why it matters at 9 to 13 px ----
 *
 * At 13 px an em is thirteen pixels and a horizontal bar is about one.
 * The unhinted outline puts the top of that bar wherever the maths lands
 * -- say 4.37 px below the cap line -- and the rasteriser, being honest,
 * splits it across two rows at 63% and 37%. Two grey rows instead of one
 * black one. Do that to every horizontal in a paragraph and the page is
 * uniformly soft.
 *
 * Grid-fitting moves that edge to exactly 4.0 before the fill, so the bar
 * lands inside one row of pixels and comes out solid. Nothing about the
 * anti-aliasing changed; what changed is where the edges are.
 *
 * ---- what this font actually contains, which bounds what this can do --
 *
 * Comic Neue is hinted by ttfautohint, and ttfautohint hints in *one
 * direction*. Disassembling all 187 instruction streams in the face:
 *
 *     SVTCA[y] (0x00)   6 uses        SVTCA[x] (0x01)   none
 *     IUP[y]   (0x30)   3 uses        IUP[x]   (0x31)   none
 *     MDRP     (0xC0+)  none          MIRP     (0xE0+)  none
 *
 * So the program sets the projection and freedom vectors to the y axis
 * and never moves a point horizontally. Horizontal strokes -- the
 * baseline, the x-height, the top and bottom of every bar and shoulder --
 * are snapped. Vertical stems are not, because there are no instructions
 * in the file that would snap them.
 *
 * That is a property of the font, not of this interpreter: the general
 * machinery for x is implemented and exercised by the same code paths,
 * and a face carrying x hints would get them. It is stated here because
 * the alternative is someone later reading "grid-fitting" and wondering
 * why the left edge of an 'l' is still grey.
 *
 * ---- integer only ----
 *
 * Every value here is F26Dot6 -- a signed integer counting sixty-fourths
 * of a pixel -- or F2Dot14 for the unit vectors. That is not a house
 * style imposed on the format; it is the format. The specification
 * defines rounding, projection and the cut-ins in terms of 26.6
 * arithmetic, and a float implementation would have to reproduce the
 * exact truncation behaviour of the integer one to get the same pixels.
 * There is no float in this file and no reason to want one.
 *
 * ---- failure is falling back, never garbage ----
 *
 * An opcode this does not implement, a stack that underflows, a jump out
 * of bounds, a program that will not terminate: each sets an error and
 * unwinds. The caller then uses the *unhinted* outline, which is exactly
 * what it drew before this file existed. A hinting bug can therefore
 * make text no worse than it used to be, which is the only safe design
 * for a bytecode interpreter reading data it did not produce.
 */

#include <stdint.h>

/* ----- limits -----
 *
 * maxp asks for 128 stack elements, 78 storage locations, 141 function
 * definitions and 36 twilight points. These are those, rounded up, with
 * the exception of the instruction budget, which is not in maxp because
 * the format has no notion of one.
 */
#define TT_MAX_STACK      256
#define TT_MAX_STORE      256
#define TT_MAX_CVT        512
#define TT_MAX_FUNCS      256
#define TT_MAX_TWILIGHT   64
#define TT_MAX_CALL       64
#define TT_MAX_PTS        (TTF_MAXPTS + 4)   /* four phantom points */
#define TT_MAX_CONT       64

/*
 * A runaway guard.
 *
 * The bytecode has unrestricted backward jumps and recursive calls, so a
 * malformed or hostile font can loop forever, and "the machine stopped
 * drawing text" is an unusually bad way to find out. Two million
 * instructions is several orders of magnitude past what a glyph needs --
 * the largest stream in this face is 312 bytes -- and it is a bound, not
 * a budget: hitting it means the program is wrong.
 */
#define TT_MAX_INSN       2000000

typedef int32_t f26;      /* 26.6: sixty-fourths of a pixel */
typedef int32_t f2d14;    /* 2.14: unit vector components   */

/* Point tags. Bit 0 is the on-curve flag the glyph format defines; the
 * other two are the interpreter's own record of which points it has
 * moved, and IUP reads them to decide what to drag along. */
#define TT_ON_CURVE   0x01
#define TT_TOUCH_X    0x02
#define TT_TOUCH_Y    0x04

/* ----- fixed-point helpers -----
 *
 * Every multiply goes through 64 bits before coming back down. A 26.6
 * coordinate at 13 px is small, but CVT arithmetic in a font program
 * routinely multiplies a scale by an em-sized number, and 32x32 there
 * overflows silently and moves a point half a screen.
 */
static inline f26 tt_mul(f26 a, f26 b) {          /* 26.6 x 26.6 -> 26.6 */
    int64_t v = (int64_t)a * b;
    return (f26)((v + (v >= 0 ? 32 : -32)) / 64);
}

static inline f26 tt_div(f26 a, f26 b) {          /* 26.6 / 26.6 -> 26.6 */
    if (b == 0) return 0;
    int64_t v = ((int64_t)a << 6);
    return (f26)(v / b);
}

static inline int32_t tt_muldiv(int32_t a, int32_t b, int32_t c) {
    if (c == 0) return 0;
    int64_t v = (int64_t)a * b;
    int64_t h = (v >= 0) ? (c / 2) : -(c / 2);
    return (int32_t)((v + h) / c);
}

/* Multiply without the rounding term. The specification distinguishes
 * the two and the cut-in comparisons notice. */
static inline int32_t tt_muldiv_no_round(int32_t a, int32_t b, int32_t c) {
    if (c == 0) return 0;
    return (int32_t)(((int64_t)a * b) / c);
}

static inline f26 tt_abs(f26 a) { return a < 0 ? -a : a; }

/* ----- graphics state ----- */
typedef struct {
    f2d14 pv_x, pv_y;         /* projection vector      */
    f2d14 fv_x, fv_y;         /* freedom vector         */
    f2d14 dv_x, dv_y;         /* dual projection vector */
    int32_t fdotp;            /* fv . pv, 2.14          */

    int   zp0, zp1, zp2;
    int   rp0, rp1, rp2;
    int   loop;

    /* Rounding, expressed as period/phase/threshold so that every state
     * -- to grid, to half grid, to double grid, up, down, off, and the
     * two programmable ones -- is the same three lines of arithmetic. */
    f26   round_period, round_phase, round_threshold;
    int   round_off;

    f26   min_distance;
    f26   cv_cutin;
    f26   sw_cutin;
    f26   sw_value;
    int   auto_flip;
    int   delta_base;
    int   delta_shift;
    int   instruct_control;
    int   scan_control;
    int   scan_type;
} tt_gstate;

/* ----- the machine ----- */

/* Zone 1: the glyph. Four phantom points are appended past the real
 * ones -- left side bearing, advance, and the two vertical equivalents --
 * because the specification lets a program read and move them, and
 * ttfautohint's functions do read them. */
static f26     TH_cur_x[TT_MAX_PTS], TH_cur_y[TT_MAX_PTS];
static f26     TH_org_x[TT_MAX_PTS], TH_org_y[TT_MAX_PTS];
static uint8_t TH_tag[TT_MAX_PTS];
static uint16_t TH_ends[TT_MAX_CONT];
static int     TH_npts, TH_ncont;

/* Zone 0: the twilight zone -- scratch points with no outline meaning,
 * used by font programs to compute positions before applying them. */
static f26     TW_cur_x[TT_MAX_TWILIGHT], TW_cur_y[TT_MAX_TWILIGHT];
static f26     TW_org_x[TT_MAX_TWILIGHT], TW_org_y[TT_MAX_TWILIGHT];
static uint8_t TW_tag[TT_MAX_TWILIGHT];

static int32_t TT_stack[TT_MAX_STACK];
static int     TT_sp;
static int32_t TT_store[TT_MAX_STORE];
static f26     TT_cvt[TT_MAX_CVT];
static int     TT_cvt_n;

typedef struct { const uint8_t *code; uint32_t len; } tt_func;
static tt_func TT_func[TT_MAX_FUNCS];

static tt_gstate TT_gs;         /* live state          */
static tt_gstate TT_gs_default; /* what prep left      */

static int      TT_ppem;
static int32_t  TT_scale;       /* font units -> 26.6, as a 16.16 ratio */
static int      TT_error;
static uint32_t TT_insn_budget;

/* How the last run went, so a caller can tell "hinted" from "fell back
 * silently" -- which are indistinguishable from the pixels alone, and
 * the difference between a working interpreter and a decorative one. */
static uint32_t TT_n_hinted = 0, TT_n_failed = 0;

/* ----- zone access ----- */
static f26 *tt_zx(int z) { return z ? TH_cur_x : TW_cur_x; }
static f26 *tt_zy(int z) { return z ? TH_cur_y : TW_cur_y; }
static f26 *tt_ox(int z) { return z ? TH_org_x : TW_org_x; }
static f26 *tt_oy(int z) { return z ? TH_org_y : TW_org_y; }
static uint8_t *tt_zt(int z) { return z ? TH_tag : TW_tag; }
static int tt_zn(int z) { return z ? TH_npts : TT_MAX_TWILIGHT; }

static int tt_pt_ok(int z, int i) {
    if (i < 0 || i >= tt_zn(z)) { TT_error = 1; return 0; }
    return 1;
}

/* ----- stack ----- */
static void tt_push(int32_t v) {
    if (TT_sp >= TT_MAX_STACK) { TT_error = 1; return; }
    TT_stack[TT_sp++] = v;
}
static int32_t tt_pop(void) {
    if (TT_sp <= 0) { TT_error = 1; return 0; }
    return TT_stack[--TT_sp];
}

/* ----- projection and movement ----- */

static f26 tt_project(f26 x, f26 y) {
    return (f26)(((int64_t)x * TT_gs.pv_x + (int64_t)y * TT_gs.pv_y) >> 14);
}
static f26 tt_dual_project(f26 x, f26 y) {
    return (f26)(((int64_t)x * TT_gs.dv_x + (int64_t)y * TT_gs.dv_y) >> 14);
}

static void tt_compute_fdotp(void) {
    TT_gs.fdotp = (int32_t)(((int64_t)TT_gs.fv_x * TT_gs.pv_x +
                             (int64_t)TT_gs.fv_y * TT_gs.pv_y) >> 14);
    /* Perpendicular vectors mean a move along the freedom vector changes
     * the projected position not at all, so the distance to move is
     * undefined. The specification's answer is to treat it as the
     * identity rather than divide by zero. */
    if (TT_gs.fdotp == 0) TT_gs.fdotp = 0x4000;
}

/*
 * Move one point `dist` along the freedom vector.
 *
 * Both components are scaled by fv/(fv.pv), which for the axis-aligned
 * case this font uses -- fv = pv = y -- reduces to "add dist to y", and
 * for the general case is what keeps a diagonal move landing the right
 * projected distance away.
 */
static void tt_move(int z, int i, f26 dist) {
    if (!tt_pt_ok(z, i)) return;
    f26 *cx = tt_zx(z), *cy = tt_zy(z);
    uint8_t *tg = tt_zt(z);

    if (TT_gs.fv_x != 0) {
        cx[i] += tt_muldiv(dist, TT_gs.fv_x, TT_gs.fdotp);
        tg[i] |= TT_TOUCH_X;
    }
    if (TT_gs.fv_y != 0) {
        cy[i] += tt_muldiv(dist, TT_gs.fv_y, TT_gs.fdotp);
        tg[i] |= TT_TOUCH_Y;
    }
}

/* Move without marking the point touched -- what IUP needs internally,
 * and what SHPIX must not do. */
static void tt_move_untouched(int z, int i, f26 dx, f26 dy) {
    if (!tt_pt_ok(z, i)) return;
    tt_zx(z)[i] += dx;
    tt_zy(z)[i] += dy;
}

/* ----- rounding -----
 *
 * One expression serves every rounding state, because every state is a
 * choice of period, phase and threshold. Round-to-grid is period 64,
 * phase 0, threshold 32: add a half pixel, floor to a whole one. Round
 * to half grid moves the phase to 32 so results land on pixel centres.
 * Round down and round up move the threshold to 0 and 63.
 */
static f26 tt_round(f26 dist, f26 compensation) {
    if (TT_gs.round_off) {
        f26 v = dist + compensation;
        return v;
    }
    int neg = dist < 0;
    f26 d = neg ? -dist : dist;
    d += compensation;

    /* A compensated distance that has gone negative rounds to zero
     * rather than wrapping to the other side of the grid. */
    if (d < 0) d = 0;

    d += TT_gs.round_threshold - TT_gs.round_phase;
    d = (d / TT_gs.round_period) * TT_gs.round_period;
    d += TT_gs.round_phase;
    if (d < 0) d = TT_gs.round_phase;

    return neg ? -d : d;
}

static void tt_set_round(f26 period, f26 phase, f26 threshold) {
    TT_gs.round_off = 0;
    TT_gs.round_period = period;
    TT_gs.round_phase = phase;
    TT_gs.round_threshold = threshold;
}

/* SROUND and S45ROUND encode period, phase and threshold into one byte.
 * S45ROUND's period is the diagonal of a pixel, which is where the
 * square root of two comes from -- as 0x5A82 in 16.16, not as a float. */
static void tt_set_super(int32_t sel, int is45) {
    f26 period;
    if (is45) period = (f26)(((int64_t)0x5A82 * 64) >> 16);   /* sqrt(2) px */
    else      period = 64;

    switch ((sel >> 6) & 3) {
        case 0: period /= 2; break;
        case 1: break;
        case 2: period *= 2; break;
        default: TT_error = 1; return;
    }
    if (period <= 0) period = 1;

    f26 phase;
    switch ((sel >> 4) & 3) {
        case 0: phase = 0; break;
        case 1: phase = period / 4; break;
        case 2: phase = period / 2; break;
        default: phase = period * 3 / 4; break;
    }

    f26 threshold;
    int t = sel & 0x0F;
    if (t == 0) threshold = period - 1;
    else        threshold = (t - 4) * period / 8;

    tt_set_round(period, phase, threshold);
}

/* ----- IUP: drag the untouched points along -----
 *
 * The program moves a handful of points per contour and leaves the rest
 * where the outline put them. IUP is what reconnects them: between two
 * touched points, every untouched point is interpolated by where it sat
 * relative to them originally, so a curve keeps its shape while its ends
 * move onto the grid. Outside the touched pair it is shifted rather than
 * stretched.
 *
 * Doing this wrong is not subtle -- the glyph comes apart -- but it is
 * easy to get the wraparound wrong, because a contour is a ring and the
 * "between" of the last touched point and the first is the segment that
 * crosses the end of the array.
 */
static void tt_iup_range(f26 *cur, const f26 *org, int p1, int p2,
                         int ref1, int ref2) {
    if (p1 > p2) return;

    f26 org1 = org[ref1], org2 = org[ref2];
    f26 cur1 = cur[ref1], cur2 = cur[ref2];

    /* Order the two anchors by their original position, so "inside" is
     * well defined however the contour is wound. */
    if (org1 > org2) {
        f26 t;
        t = org1; org1 = org2; org2 = t;
        t = cur1; cur1 = cur2; cur2 = t;
    }

    f26 span_org = org2 - org1;
    f26 span_cur = cur2 - cur1;

    for (int i = p1; i <= p2; i++) {
        f26 o = org[i];
        if (o <= org1)      cur[i] = o + (cur1 - org1);   /* shift  */
        else if (o >= org2) cur[i] = o + (cur2 - org2);   /* shift  */
        else if (span_org > 0)
            cur[i] = cur1 + tt_muldiv(o - org1, span_cur, span_org);
        else
            cur[i] = cur1;
    }
}

static void tt_iup(int axis_y) {
    f26 *cur = axis_y ? TH_cur_y : TH_cur_x;
    const f26 *org = axis_y ? TH_org_y : TH_org_x;
    uint8_t mask = axis_y ? TT_TOUCH_Y : TT_TOUCH_X;

    int start = 0;
    for (int c = 0; c < TH_ncont; c++) {
        int end = TH_ends[c];
        if (end >= TH_npts) break;

        int first_touched = -1, prev = -1;
        for (int i = start; i <= end; i++) {
            if (!(TH_tag[i] & mask)) continue;
            if (first_touched < 0) first_touched = i;
            else                   tt_iup_range(cur, org, prev + 1, i - 1, prev, i);
            prev = i;
        }

        if (first_touched >= 0) {
            if (prev == first_touched) {
                /* Exactly one touched point: the whole contour shifts
                 * with it, which is the degenerate case of the loop
                 * below and would otherwise interpolate against itself. */
                f26 d = cur[first_touched] - org[first_touched];
                for (int i = start; i <= end; i++)
                    if (i != first_touched) cur[i] = org[i] + d;
            } else {
                /* The wraparound segment: from the last touched point,
                 * over the end of the contour, to the first. */
                tt_iup_range(cur, org, prev + 1, end, prev, first_touched);
                tt_iup_range(cur, org, start, first_touched - 1,
                             prev, first_touched);
            }
        }
        start = end + 1;
    }
}

/* ----- delta exceptions -----
 *
 * A list of (ppem, amount) corrections a designer added by hand for the
 * sizes where the algorithm still gets it wrong. The encoding packs the
 * size and a signed nudge into one byte: high nibble is the ppem
 * relative to the delta base, low nibble is the step, and steps of zero
 * through seven are negative while eight through fifteen are positive
 * with no zero in the middle.
 */
static void tt_deltap(int range)
{
    int32_t count = tt_pop();
    if (TT_error || count < 0) { TT_error = 1; return; }

    for (int32_t k = 0; k < count && !TT_error; k++) {
        int32_t pt  = tt_pop();
        int32_t arg = tt_pop();
        if (TT_error) return;

        int ppem_of = ((arg >> 4) & 0x0F) + TT_gs.delta_base + range * 16;
        if (ppem_of != TT_ppem) continue;

        int step = arg & 0x0F;
        step = (step < 8) ? step - 8 : step - 7;      /* no zero step */

        /* delta_shift is a power-of-two divisor: shift 3 means the step
         * is in eighths of a pixel. */
        f26 amount = (f26)(((int64_t)step * 64) >> TT_gs.delta_shift);
        tt_move(TT_gs.zp0, pt, amount);
    }
}

static void tt_deltac(int range)
{
    int32_t count = tt_pop();
    if (TT_error || count < 0) { TT_error = 1; return; }

    for (int32_t k = 0; k < count && !TT_error; k++) {
        int32_t idx = tt_pop();
        int32_t arg = tt_pop();
        if (TT_error) return;

        int ppem_of = ((arg >> 4) & 0x0F) + TT_gs.delta_base + range * 16;
        if (ppem_of != TT_ppem) continue;
        if (idx < 0 || idx >= TT_cvt_n) { TT_error = 1; return; }

        int step = arg & 0x0F;
        step = (step < 8) ? step - 8 : step - 7;
        TT_cvt[idx] += (f26)(((int64_t)step * 64) >> TT_gs.delta_shift);
    }
}

/* ----- the interpreter ----- */

/*
 * Run one instruction stream.
 *
 * `depth` bounds recursion through CALL and LOOPCALL. The budget is
 * shared across the whole run rather than per stream, so a program that
 * spends it inside a function cannot get more by returning.
 */
static void tt_run(const uint8_t *code, uint32_t len, int depth);

static void tt_exec_call(int32_t fn, int depth) {
    if (fn < 0 || fn >= TT_MAX_FUNCS || !TT_func[fn].code) { TT_error = 1; return; }
    if (depth >= TT_MAX_CALL) { TT_error = 1; return; }
    tt_run(TT_func[fn].code, TT_func[fn].len, depth + 1);
}

static void tt_run(const uint8_t *code, uint32_t len, int depth) {
    uint32_t ip = 0;

    while (ip < len && !TT_error) {
        if (TT_insn_budget-- == 0) { TT_error = 1; return; }

        uint8_t op = code[ip++];

        switch (op) {

        /* ---- pushing ---- */
        case 0x40: {                                   /* NPUSHB */
            if (ip >= len) { TT_error = 1; return; }
            int n = code[ip++];
            for (int i = 0; i < n; i++) {
                if (ip >= len) { TT_error = 1; return; }
                tt_push(code[ip++]);
            }
            break;
        }
        case 0x41: {                                   /* NPUSHW */
            if (ip >= len) { TT_error = 1; return; }
            int n = code[ip++];
            for (int i = 0; i < n; i++) {
                if (ip + 1 >= len) { TT_error = 1; return; }
                tt_push((int16_t)((code[ip] << 8) | code[ip + 1]));
                ip += 2;
            }
            break;
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {  /* PUSHB[n] */
            int n = (op - 0xB0) + 1;
            for (int i = 0; i < n; i++) {
                if (ip >= len) { TT_error = 1; return; }
                tt_push(code[ip++]);
            }
            break;
        }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {  /* PUSHW[n] */
            int n = (op - 0xB8) + 1;
            for (int i = 0; i < n; i++) {
                if (ip + 1 >= len) { TT_error = 1; return; }
                tt_push((int16_t)((code[ip] << 8) | code[ip + 1]));
                ip += 2;
            }
            break;
        }

        /* ---- stack ---- */
        case 0x20: {                                   /* DUP */
            int32_t v = tt_pop(); tt_push(v); tt_push(v); break;
        }
        case 0x21: tt_pop(); break;                    /* POP */
        case 0x22: TT_sp = 0; break;                   /* CLEAR */
        case 0x23: {                                   /* SWAP */
            int32_t a = tt_pop(), b = tt_pop(); tt_push(a); tt_push(b); break;
        }
        case 0x24: tt_push(TT_sp); break;              /* DEPTH */
        case 0x25: {                                   /* CINDEX */
            int32_t k = tt_pop();
            if (k <= 0 || k > TT_sp) { TT_error = 1; return; }
            tt_push(TT_stack[TT_sp - k]);
            break;
        }
        case 0x26: {                                   /* MINDEX */
            int32_t k = tt_pop();
            if (k <= 0 || k > TT_sp) { TT_error = 1; return; }
            int32_t v = TT_stack[TT_sp - k];
            for (int i = TT_sp - k; i < TT_sp - 1; i++) TT_stack[i] = TT_stack[i + 1];
            TT_stack[TT_sp - 1] = v;
            tt_pop();
            tt_push(v);
            break;
        }
        case 0x8A: {                                   /* ROLL */
            int32_t a = tt_pop(), b = tt_pop(), c = tt_pop();
            tt_push(b); tt_push(a); tt_push(c);
            break;
        }

        /* ---- arithmetic ---- */
        case 0x60: { f26 b = tt_pop(), a = tt_pop(); tt_push(a + b); break; }  /* ADD */
        case 0x61: { f26 b = tt_pop(), a = tt_pop(); tt_push(a - b); break; }  /* SUB */
        case 0x62: { f26 b = tt_pop(), a = tt_pop(); tt_push(tt_div(a, b)); break; }
        case 0x63: { f26 b = tt_pop(), a = tt_pop(); tt_push(tt_mul(a, b)); break; }
        case 0x64: { f26 a = tt_pop(); tt_push(tt_abs(a)); break; }            /* ABS */
        case 0x65: { f26 a = tt_pop(); tt_push(-a); break; }                   /* NEG */
        case 0x66: { f26 a = tt_pop(); tt_push(a & ~63); break; }              /* FLOOR */
        case 0x67: { f26 a = tt_pop(); tt_push((a + 63) & ~63); break; }       /* CEILING */
        case 0x8B: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a > b ? a : b); break; }
        case 0x8C: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a < b ? a : b); break; }

        /* ---- comparison and logic ---- */
        case 0x50: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a <  b); break; }
        case 0x51: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a <= b); break; }
        case 0x52: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a >  b); break; }
        case 0x53: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a >= b); break; }
        case 0x54: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a == b); break; }
        case 0x55: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a != b); break; }
        case 0x56: { f26 a = tt_pop(); tt_push((tt_round(a, 0) & 127) == 64); break; }
        case 0x57: { f26 a = tt_pop(); tt_push((tt_round(a, 0) & 127) == 0); break; }
        case 0x5A: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a && b); break; }
        case 0x5B: { int32_t b = tt_pop(), a = tt_pop(); tt_push(a || b); break; }
        case 0x5C: { int32_t a = tt_pop(); tt_push(!a); break; }

        /* ---- rounding ---- */
        case 0x68: case 0x69: case 0x6A: case 0x6B: {  /* ROUND[ab] */
            f26 a = tt_pop(); tt_push(tt_round(a, 0)); break;
        }
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {  /* NROUND[ab] */
            break;                                     /* no engine compensation here */
        }
        case 0x18: tt_set_round(64, 0, 32); break;     /* RTG  */
        case 0x19: tt_set_round(64, 32, 32); break;    /* RTHG */
        case 0x3D: tt_set_round(32, 0, 16); break;     /* RTDG */
        case 0x7C: tt_set_round(64, 0, 63); break;     /* RUTG */
        case 0x7D: tt_set_round(64, 0, 0);  break;     /* RDTG */
        case 0x7A: TT_gs.round_off = 1; break;         /* ROFF */
        case 0x76: { int32_t s = tt_pop(); tt_set_super(s, 0); break; }  /* SROUND   */
        case 0x77: { int32_t s = tt_pop(); tt_set_super(s, 1); break; }  /* S45ROUND */

        /* ---- vectors ---- */
        case 0x00:                                     /* SVTCA[y] */
            TT_gs.pv_x = TT_gs.fv_x = TT_gs.dv_x = 0;
            TT_gs.pv_y = TT_gs.fv_y = TT_gs.dv_y = 0x4000;
            tt_compute_fdotp();
            break;
        case 0x01:                                     /* SVTCA[x] */
            TT_gs.pv_x = TT_gs.fv_x = TT_gs.dv_x = 0x4000;
            TT_gs.pv_y = TT_gs.fv_y = TT_gs.dv_y = 0;
            tt_compute_fdotp();
            break;
        case 0x02: TT_gs.pv_x = TT_gs.dv_x = 0; TT_gs.pv_y = TT_gs.dv_y = 0x4000;
                   tt_compute_fdotp(); break;          /* SPVTCA[y] */
        case 0x03: TT_gs.pv_x = TT_gs.dv_x = 0x4000; TT_gs.pv_y = TT_gs.dv_y = 0;
                   tt_compute_fdotp(); break;          /* SPVTCA[x] */
        case 0x04: TT_gs.fv_x = 0; TT_gs.fv_y = 0x4000;
                   tt_compute_fdotp(); break;          /* SFVTCA[y] */
        case 0x05: TT_gs.fv_x = 0x4000; TT_gs.fv_y = 0;
                   tt_compute_fdotp(); break;          /* SFVTCA[x] */
        case 0x0E: TT_gs.fv_x = TT_gs.pv_x; TT_gs.fv_y = TT_gs.pv_y;
                   tt_compute_fdotp(); break;          /* SFVTPV */
        case 0x0C: tt_push(TT_gs.pv_x); tt_push(TT_gs.pv_y); break;   /* GPV */
        case 0x0D: tt_push(TT_gs.fv_x); tt_push(TT_gs.fv_y); break;   /* GFV */

        /* ---- graphics state setters ---- */
        case 0x10: TT_gs.rp0 = tt_pop(); break;        /* SRP0 */
        case 0x11: TT_gs.rp1 = tt_pop(); break;        /* SRP1 */
        case 0x12: TT_gs.rp2 = tt_pop(); break;        /* SRP2 */
        case 0x13: TT_gs.zp0 = tt_pop() ? 1 : 0; break;   /* SZP0 */
        case 0x14: TT_gs.zp1 = tt_pop() ? 1 : 0; break;   /* SZP1 */
        case 0x15: TT_gs.zp2 = tt_pop() ? 1 : 0; break;   /* SZP2 */
        case 0x16: {                                    /* SZPS */
            int z = tt_pop() ? 1 : 0;
            TT_gs.zp0 = TT_gs.zp1 = TT_gs.zp2 = z;
            break;
        }
        case 0x17: TT_gs.loop = tt_pop(); break;       /* SLOOP */
        case 0x1A: TT_gs.min_distance = tt_pop(); break;  /* SMD */
        case 0x1D: TT_gs.cv_cutin = tt_pop(); break;   /* SCVTCI */
        case 0x1E: TT_gs.sw_cutin = tt_pop(); break;   /* SSWCI */
        case 0x1F: TT_gs.sw_value = tt_pop(); break;   /* SSW */
        case 0x4D: TT_gs.auto_flip = 1; break;         /* FLIPON  */
        case 0x4E: TT_gs.auto_flip = 0; break;         /* FLIPOFF */
        case 0x5E: TT_gs.delta_base = tt_pop(); break; /* SDB */
        case 0x5F: TT_gs.delta_shift = tt_pop(); break;/* SDS */
        case 0x85: TT_gs.scan_control = tt_pop(); break;  /* SCANCTRL */
        case 0x8D: TT_gs.scan_type = tt_pop(); break;  /* SCANTYPE */
        case 0x8E: {                                   /* INSTCTRL */
            int32_t v = tt_pop(), sel = tt_pop();
            if (sel >= 1 && sel <= 2) {
                if (v) TT_gs.instruct_control |= sel;
                else   TT_gs.instruct_control &= ~sel;
            }
            break;
        }

        /* ---- storage and control values ---- */
        case 0x42: {                                   /* WS */
            int32_t v = tt_pop(), a = tt_pop();
            if (a < 0 || a >= TT_MAX_STORE) { TT_error = 1; return; }
            TT_store[a] = v;
            break;
        }
        case 0x43: {                                   /* RS */
            int32_t a = tt_pop();
            if (a < 0 || a >= TT_MAX_STORE) { TT_error = 1; return; }
            tt_push(TT_store[a]);
            break;
        }
        case 0x44: {                                   /* WCVTP: already pixels */
            f26 v = tt_pop(); int32_t a = tt_pop();
            if (a < 0 || a >= TT_cvt_n) { TT_error = 1; return; }
            TT_cvt[a] = v;
            break;
        }
        case 0x70: {                                   /* WCVTF: font units in */
            int32_t v = tt_pop(), a = tt_pop();
            if (a < 0 || a >= TT_cvt_n) { TT_error = 1; return; }
            TT_cvt[a] = (f26)(((int64_t)v * TT_scale) >> 16);
            break;
        }
        case 0x45: {                                   /* RCVT */
            int32_t a = tt_pop();
            if (a < 0 || a >= TT_cvt_n) { TT_error = 1; return; }
            tt_push(TT_cvt[a]);
            break;
        }

        /* ---- measurement ---- */
        case 0x4B: tt_push(TT_ppem); break;            /* MPPEM */
        case 0x4C: tt_push(TT_ppem); break;            /* MPS: no point size here */
        case 0x46: case 0x47: {                        /* GC[a] */
            int32_t i = tt_pop();
            int z = TT_gs.zp2;
            if (!tt_pt_ok(z, i)) return;
            if (op == 0x46) tt_push(tt_project(tt_zx(z)[i], tt_zy(z)[i]));
            else            tt_push(tt_dual_project(tt_ox(z)[i], tt_oy(z)[i]));
            break;
        }
        case 0x48: {                                   /* SCFS */
            f26 v = tt_pop(); int32_t i = tt_pop();
            int z = TT_gs.zp2;
            if (!tt_pt_ok(z, i)) return;
            f26 cur = tt_project(tt_zx(z)[i], tt_zy(z)[i]);
            tt_move(z, i, v - cur);
            break;
        }
        case 0x49: case 0x4A: {                        /* MD[a] */
            int32_t i2 = tt_pop(), i1 = tt_pop();
            int za = TT_gs.zp1, zb = TT_gs.zp0;
            if (!tt_pt_ok(za, i2) || !tt_pt_ok(zb, i1)) return;
            f26 d;
            if (op == 0x49)
                d = tt_project(tt_zx(zb)[i1] - tt_zx(za)[i2],
                               tt_zy(zb)[i1] - tt_zy(za)[i2]);
            else
                d = tt_dual_project(tt_ox(zb)[i1] - tt_ox(za)[i2],
                                    tt_oy(zb)[i1] - tt_oy(za)[i2]);
            tt_push(d);
            break;
        }

        /* ---- control flow ---- */
        case 0x58: {                                   /* IF */
            int32_t c = tt_pop();
            if (c) break;
            /* Skip to the matching ELSE or EIF, counting nesting and
             * stepping over inline push data -- a PUSHB's operands can
             * contain the byte 0x59 and must not be read as EIF. */
            int nest = 0;
            while (ip < len) {
                uint8_t o = code[ip++];
                if (o == 0x58) nest++;
                else if (o == 0x59) { if (nest == 0) break; nest--; }
                else if (o == 0x1B && nest == 0) break;
                else if (o == 0x40 || o == 0x41) {
                    if (ip >= len) { TT_error = 1; return; }
                    int n = code[ip++];
                    ip += (o == 0x40) ? (uint32_t)n : (uint32_t)n * 2;
                } else if (o >= 0xB0 && o <= 0xB7) ip += (uint32_t)(o - 0xB0) + 1;
                else if (o >= 0xB8 && o <= 0xBF) ip += ((uint32_t)(o - 0xB8) + 1) * 2;
            }
            break;
        }
        case 0x1B: {                                   /* ELSE */
            /* Reached only by falling out of a taken branch: skip to EIF. */
            int nest = 0;
            while (ip < len) {
                uint8_t o = code[ip++];
                if (o == 0x58) nest++;
                else if (o == 0x59) { if (nest == 0) break; nest--; }
                else if (o == 0x40 || o == 0x41) {
                    if (ip >= len) { TT_error = 1; return; }
                    int n = code[ip++];
                    ip += (o == 0x40) ? (uint32_t)n : (uint32_t)n * 2;
                } else if (o >= 0xB0 && o <= 0xB7) ip += (uint32_t)(o - 0xB0) + 1;
                else if (o >= 0xB8 && o <= 0xBF) ip += ((uint32_t)(o - 0xB8) + 1) * 2;
            }
            break;
        }
        case 0x59: break;                              /* EIF */

        case 0x1C: {                                   /* JMPR */
            int32_t off = tt_pop();
            int64_t t = (int64_t)ip - 1 + off;
            if (t < 0 || t > (int64_t)len) { TT_error = 1; return; }
            ip = (uint32_t)t;
            break;
        }
        case 0x78: case 0x79: {                        /* JROT / JROF */
            int32_t c = tt_pop(), off = tt_pop();
            int take = (op == 0x78) ? (c != 0) : (c == 0);
            if (take) {
                int64_t t = (int64_t)ip - 1 + off;
                if (t < 0 || t > (int64_t)len) { TT_error = 1; return; }
                ip = (uint32_t)t;
            }
            break;
        }

        /* ---- functions ---- */
        case 0x2C: {                                   /* FDEF */
            int32_t fn = tt_pop();
            if (fn < 0 || fn >= TT_MAX_FUNCS) { TT_error = 1; return; }
            uint32_t body = ip;
            int nest = 0;
            while (ip < len) {
                uint8_t o = code[ip++];
                if (o == 0x2C) nest++;
                else if (o == 0x2D) { if (nest == 0) break; nest--; }
                else if (o == 0x40 || o == 0x41) {
                    if (ip >= len) { TT_error = 1; return; }
                    int n = code[ip++];
                    ip += (o == 0x40) ? (uint32_t)n : (uint32_t)n * 2;
                } else if (o >= 0xB0 && o <= 0xB7) ip += (uint32_t)(o - 0xB0) + 1;
                else if (o >= 0xB8 && o <= 0xBF) ip += ((uint32_t)(o - 0xB8) + 1) * 2;
            }
            TT_func[fn].code = code + body;
            TT_func[fn].len  = (ip > body) ? (ip - body - 1) : 0;
            break;
        }
        case 0x2D: return;                             /* ENDF */
        case 0x2B: {                                   /* CALL */
            int32_t fn = tt_pop();
            tt_exec_call(fn, depth);
            break;
        }
        case 0x2A: {                                   /* LOOPCALL */
            int32_t fn = tt_pop();
            int32_t n  = tt_pop();
            if (n < 0 || n > 65535) { TT_error = 1; return; }
            for (int32_t k = 0; k < n && !TT_error; k++) tt_exec_call(fn, depth);
            break;
        }

        /* ---- point movement ---- */
        case 0x2E: case 0x2F: {                        /* MDAP[a] */
            int32_t i = tt_pop();
            int z = TT_gs.zp0;
            if (!tt_pt_ok(z, i)) return;
            if (op & 1) {
                f26 cur = tt_project(tt_zx(z)[i], tt_zy(z)[i]);
                tt_move(z, i, tt_round(cur, 0) - cur);
            } else {
                /* MDAP[0] moves nothing. Its entire purpose is to mark
                 * the point touched so IUP treats it as an anchor --
                 * which is most of how this font works. */
                tt_zt(z)[i] |= (TT_gs.fv_x ? TT_TOUCH_X : 0) |
                               (TT_gs.fv_y ? TT_TOUCH_Y : 0);
            }
            TT_gs.rp0 = TT_gs.rp1 = i;
            break;
        }
        case 0x3E: case 0x3F: {                        /* MIAP[a] */
            int32_t c = tt_pop(), i = tt_pop();
            int z = TT_gs.zp0;
            if (!tt_pt_ok(z, i)) return;
            if (c < 0 || c >= TT_cvt_n) { TT_error = 1; return; }
            f26 want = TT_cvt[c];
            f26 cur  = tt_project(tt_zx(z)[i], tt_zy(z)[i]);

            /* In the twilight zone the point has no outline position, so
             * the control value *becomes* its position rather than a
             * target to move toward. */
            if (z == 0) {
                tt_ox(z)[i] = tt_muldiv(want, TT_gs.fv_x, 0x4000);
                tt_oy(z)[i] = tt_muldiv(want, TT_gs.fv_y, 0x4000);
                tt_zx(z)[i] = tt_ox(z)[i];
                tt_zy(z)[i] = tt_oy(z)[i];
                cur = want;
            }
            if (op & 1) {
                if (tt_abs(want - cur) > TT_gs.cv_cutin) want = cur;
                want = tt_round(want, 0);
            }
            tt_move(z, i, want - cur);
            TT_gs.rp0 = TT_gs.rp1 = i;
            break;
        }
        case 0x38: {                                   /* SHPIX */
            f26 amount = tt_pop();
            int z = TT_gs.zp2;
            for (int k = 0; k < TT_gs.loop && !TT_error; k++) {
                int32_t i = tt_pop();
                if (!tt_pt_ok(z, i)) return;
                /* SHPIX moves along the freedom vector by a distance
                 * given directly in pixels -- no projection, no
                 * rounding. It is how a font program applies a
                 * correction it has already computed. */
                f26 dx = tt_muldiv(amount, TT_gs.fv_x, 0x4000);
                f26 dy = tt_muldiv(amount, TT_gs.fv_y, 0x4000);
                tt_zx(z)[i] += dx;
                tt_zy(z)[i] += dy;
                tt_zt(z)[i] |= (TT_gs.fv_x ? TT_TOUCH_X : 0) |
                               (TT_gs.fv_y ? TT_TOUCH_Y : 0);
            }
            TT_gs.loop = 1;
            break;
        }
        case 0x32: case 0x33: {                        /* SHP[a] */
            int zref = (op & 1) ? TT_gs.zp0 : TT_gs.zp1;
            int iref = (op & 1) ? TT_gs.rp1 : TT_gs.rp2;
            if (!tt_pt_ok(zref, iref)) return;
            f26 dx = tt_zx(zref)[iref] - tt_ox(zref)[iref];
            f26 dy = tt_zy(zref)[iref] - tt_oy(zref)[iref];
            int z = TT_gs.zp2;
            for (int k = 0; k < TT_gs.loop && !TT_error; k++) {
                int32_t i = tt_pop();
                if (!tt_pt_ok(z, i)) return;
                tt_move_untouched(z, i, dx, dy);
                tt_zt(z)[i] |= (dx ? TT_TOUCH_X : 0) | (dy ? TT_TOUCH_Y : 0);
            }
            TT_gs.loop = 1;
            break;
        }
        case 0x34: case 0x35: {                        /* SHC[a] */
            int32_t ci = tt_pop();
            int zref = (op & 1) ? TT_gs.zp0 : TT_gs.zp1;
            int iref = (op & 1) ? TT_gs.rp1 : TT_gs.rp2;
            if (!tt_pt_ok(zref, iref)) return;
            if (ci < 0 || ci >= TH_ncont) { TT_error = 1; return; }

            f26 dx = tt_zx(zref)[iref] - tt_ox(zref)[iref];
            f26 dy = tt_zy(zref)[iref] - tt_oy(zref)[iref];

            int s = ci ? TH_ends[ci - 1] + 1 : 0;
            int e = TH_ends[ci];
            for (int i = s; i <= e && i < TH_npts; i++) {
                /* The reference point is not moved by its own shift. */
                if (TT_gs.zp2 == 1 && i == iref && zref == 1) continue;
                TH_cur_x[i] += dx;
                TH_cur_y[i] += dy;
                TH_tag[i] |= (dx ? TT_TOUCH_X : 0) | (dy ? TT_TOUCH_Y : 0);
            }
            break;
        }
        case 0x36: case 0x37: {                        /* SHZ[a] */
            int32_t zi = tt_pop();
            int zref = (op & 1) ? TT_gs.zp0 : TT_gs.zp1;
            int iref = (op & 1) ? TT_gs.rp1 : TT_gs.rp2;
            if (!tt_pt_ok(zref, iref)) return;
            f26 dx = tt_zx(zref)[iref] - tt_ox(zref)[iref];
            f26 dy = tt_zy(zref)[iref] - tt_oy(zref)[iref];
            int z = zi ? 1 : 0;
            int n = (z == 1) ? TH_npts : TT_MAX_TWILIGHT;
            for (int i = 0; i < n; i++) { tt_zx(z)[i] += dx; tt_zy(z)[i] += dy; }
            break;
        }
        case 0x3C: {                                   /* ALIGNRP */
            int zr = TT_gs.zp0, ir = TT_gs.rp0;
            if (!tt_pt_ok(zr, ir)) return;
            f26 ref = tt_project(tt_zx(zr)[ir], tt_zy(zr)[ir]);
            int z = TT_gs.zp1;
            for (int k = 0; k < TT_gs.loop && !TT_error; k++) {
                int32_t i = tt_pop();
                if (!tt_pt_ok(z, i)) return;
                f26 cur = tt_project(tt_zx(z)[i], tt_zy(z)[i]);
                tt_move(z, i, ref - cur);
            }
            TT_gs.loop = 1;
            break;
        }
        case 0x27: {                                   /* ALIGNPTS */
            int32_t i2 = tt_pop(), i1 = tt_pop();
            int z1 = TT_gs.zp0, z2 = TT_gs.zp1;
            if (!tt_pt_ok(z1, i1) || !tt_pt_ok(z2, i2)) return;
            f26 p1 = tt_project(tt_zx(z1)[i1], tt_zy(z1)[i1]);
            f26 p2 = tt_project(tt_zx(z2)[i2], tt_zy(z2)[i2]);
            f26 mid = (p1 + p2) / 2;
            tt_move(z1, i1, mid - p1);
            tt_move(z2, i2, mid - p2);
            break;
        }
        case 0x39: {                                   /* IP */
            int zr = TT_gs.zp0;
            if (!tt_pt_ok(zr, TT_gs.rp1) || !tt_pt_ok(TT_gs.zp1, TT_gs.rp2)) return;
            f26 o1 = tt_dual_project(tt_ox(zr)[TT_gs.rp1], tt_oy(zr)[TT_gs.rp1]);
            f26 c1 = tt_project(tt_zx(zr)[TT_gs.rp1], tt_zy(zr)[TT_gs.rp1]);
            int zq = TT_gs.zp1;
            f26 o2 = tt_dual_project(tt_ox(zq)[TT_gs.rp2], tt_oy(zq)[TT_gs.rp2]);
            f26 c2 = tt_project(tt_zx(zq)[TT_gs.rp2], tt_zy(zq)[TT_gs.rp2]);

            int z = TT_gs.zp2;
            for (int k = 0; k < TT_gs.loop && !TT_error; k++) {
                int32_t i = tt_pop();
                if (!tt_pt_ok(z, i)) return;
                f26 o = tt_dual_project(tt_ox(z)[i], tt_oy(z)[i]);
                f26 c = tt_project(tt_zx(z)[i], tt_zy(z)[i]);
                f26 want;
                if (o2 != o1) want = c1 + tt_muldiv(o - o1, c2 - c1, o2 - o1);
                else          want = c1 + (o - o1);
                tt_move(z, i, want - c);
            }
            TT_gs.loop = 1;
            break;
        }
        case 0x29: {                                   /* UTP */
            int32_t i = tt_pop();
            int z = TT_gs.zp0;
            if (!tt_pt_ok(z, i)) return;
            uint8_t clear = 0;
            if (TT_gs.fv_x) clear |= TT_TOUCH_X;
            if (TT_gs.fv_y) clear |= TT_TOUCH_Y;
            tt_zt(z)[i] &= (uint8_t)~clear;
            break;
        }
        case 0x30: tt_iup(1); break;                   /* IUP[y] */
        case 0x31: tt_iup(0); break;                   /* IUP[x] */

        /* ---- relative point movement ---- */
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: case 0xD0: case 0xD1:
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD:
        case 0xDE: case 0xDF: {                        /* MDRP */
            int32_t i = tt_pop();
            int zr = TT_gs.zp0, z = TT_gs.zp1;
            if (!tt_pt_ok(zr, TT_gs.rp0) || !tt_pt_ok(z, i)) return;

            f26 org = tt_dual_project(tt_ox(z)[i] - tt_ox(zr)[TT_gs.rp0],
                                      tt_oy(z)[i] - tt_oy(zr)[TT_gs.rp0]);
            f26 dist = org;
            if (op & 0x04) dist = tt_round(dist, 0);
            if (op & 0x08) {                            /* min distance */
                if (org >= 0) { if (dist < TT_gs.min_distance) dist = TT_gs.min_distance; }
                else          { if (dist > -TT_gs.min_distance) dist = -TT_gs.min_distance; }
            }
            f26 cur = tt_project(tt_zx(z)[i] - tt_zx(zr)[TT_gs.rp0],
                                 tt_zy(z)[i] - tt_zy(zr)[TT_gs.rp0]);
            tt_move(z, i, dist - cur);
            TT_gs.rp1 = TT_gs.rp0;
            TT_gs.rp2 = i;
            if (op & 0x10) TT_gs.rp0 = i;
            break;
        }
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0xF0: case 0xF1:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
        case 0xFE: case 0xFF: {                        /* MIRP */
            int32_t c = tt_pop(), i = tt_pop();
            int zr = TT_gs.zp0, z = TT_gs.zp1;
            if (!tt_pt_ok(zr, TT_gs.rp0) || !tt_pt_ok(z, i)) return;
            if (c < 0 || c >= TT_cvt_n) { TT_error = 1; return; }

            f26 want = TT_cvt[c];
            f26 org = tt_dual_project(tt_ox(z)[i] - tt_ox(zr)[TT_gs.rp0],
                                      tt_oy(z)[i] - tt_oy(zr)[TT_gs.rp0]);

            /* The control-value cut-in: trust the CVT only when it is
             * close to what the outline itself says, otherwise the
             * design's own distance wins. */
            if (tt_abs(want - org) > TT_gs.cv_cutin) want = org;
            if (op & 0x04) want = tt_round(want, 0);
            if (op & 0x08) {
                if (org >= 0) { if (want < TT_gs.min_distance) want = TT_gs.min_distance; }
                else          { if (want > -TT_gs.min_distance) want = -TT_gs.min_distance; }
            }
            f26 cur = tt_project(tt_zx(z)[i] - tt_zx(zr)[TT_gs.rp0],
                                 tt_zy(z)[i] - tt_zy(zr)[TT_gs.rp0]);
            tt_move(z, i, want - cur);
            TT_gs.rp1 = TT_gs.rp0;
            TT_gs.rp2 = i;
            if (op & 0x10) TT_gs.rp0 = i;
            break;
        }

        /* ---- deltas ---- */
        case 0x5D: tt_deltap(0); break;                /* DELTAP1 */
        case 0x71: tt_deltap(1); break;                /* DELTAP2 */
        case 0x72: tt_deltap(2); break;                /* DELTAP3 */
        case 0x73: tt_deltac(0); break;                /* DELTAC1 */
        case 0x74: tt_deltac(1); break;                /* DELTAC2 */
        case 0x75: tt_deltac(2); break;                /* DELTAC3 */

        /* ---- environment ---- */
        case 0x88: {                                   /* GETINFO */
            int32_t sel = tt_pop();
            int32_t r = 0;
            /* Interpreter version 35: the classic, non-subpixel one.
             * Claiming a later version invites the font program down a
             * ClearType path whose assumptions -- horizontal subpixel
             * filtering, and a freedom vector restricted to y -- this
             * rasteriser does not meet. */
            if (sel & 0x0001) r |= 35;
            /* Not rotated, not stretched: bits 8 and 9 stay clear. */
            if (sel & 0x0020) r |= 1 << 12;            /* greyscale, which we are */
            tt_push(r);
            break;
        }
        case 0x4F: tt_pop(); break;                    /* DEBUG */

        default:
            /* An opcode this does not implement. Rather than guess at a
             * stack effect and corrupt everything after it, give up on
             * this glyph: the caller falls back to the unhinted outline. */
            TT_error = 1;
            return;
        }
    }
}

/* ----- entry points ----- */

static void tt_reset_gstate(tt_gstate *g) {
    g->pv_x = g->fv_x = g->dv_x = 0x4000;
    g->pv_y = g->fv_y = g->dv_y = 0;
    g->fdotp = 0x4000;
    g->zp0 = g->zp1 = g->zp2 = 1;
    g->rp0 = g->rp1 = g->rp2 = 0;
    g->loop = 1;
    g->round_off = 0;
    g->round_period = 64; g->round_phase = 0; g->round_threshold = 32;
    g->min_distance = 64;
    g->cv_cutin = 68;             /* 17/16 px, the specified default */
    g->sw_cutin = 0;
    g->sw_value = 0;
    g->auto_flip = 1;
    g->delta_base = 9;
    g->delta_shift = 3;
    g->instruct_control = 0;
    g->scan_control = 0;
    g->scan_type = 0;
}

/*
 * Run the font program: the one that defines the functions.
 *
 * Once per boot, not once per size. It contains only FDEFs and the
 * arithmetic to build them, and the definitions it leaves behind are
 * pointers into the font blob -- which is a static array with the
 * lifetime of the kernel, so nothing has to be copied.
 */
static int tt_run_fpgm(const uint8_t *code, uint32_t len) {
    for (int i = 0; i < TT_MAX_FUNCS; i++) { TT_func[i].code = 0; TT_func[i].len = 0; }
    for (int i = 0; i < TT_MAX_STORE; i++) TT_store[i] = 0;

    TT_sp = 0;
    TT_error = 0;
    TT_insn_budget = TT_MAX_INSN;
    tt_reset_gstate(&TT_gs);
    TH_npts = 0; TH_ncont = 0;

    tt_run(code, len, 0);
    return !TT_error;
}

/*
 * Run the pre-program, once per size.
 *
 * This is where a font decides what its stems should measure at this
 * particular ppem, and it writes those decisions into the control value
 * table -- so the CVT has to be reloaded from the file and rescaled
 * before it runs, or the second size to be rendered inherits the first
 * size's answers.
 */
static int tt_run_prep(const uint8_t *cvt_raw, uint32_t cvt_len,
                       const uint8_t *code, uint32_t len, int ppem,
                       int32_t scale16) {
    TT_ppem = ppem;
    TT_scale = scale16;

    TT_cvt_n = (int)(cvt_len / 2);
    if (TT_cvt_n > TT_MAX_CVT) TT_cvt_n = TT_MAX_CVT;
    for (int i = 0; i < TT_cvt_n; i++) {
        int16_t v = (int16_t)((cvt_raw[i * 2] << 8) | cvt_raw[i * 2 + 1]);
        TT_cvt[i] = (f26)(((int64_t)v * scale16) >> 16);
    }

    TT_sp = 0;
    TT_error = 0;
    TT_insn_budget = TT_MAX_INSN;
    tt_reset_gstate(&TT_gs);
    TH_npts = 0; TH_ncont = 0;

    for (int i = 0; i < TT_MAX_TWILIGHT; i++) {
        TW_cur_x[i] = TW_cur_y[i] = TW_org_x[i] = TW_org_y[i] = 0;
        TW_tag[i] = 0;
    }

    if (code && len) tt_run(code, len, 0);

    /* Whatever prep left behind is the starting state for every glyph at
     * this size -- rounding mode, cut-ins, scan control. Glyphs inherit
     * it rather than the specification's defaults. */
    TT_gs_default = TT_gs;
    return !TT_error;
}

/*
 * Run one glyph's instructions over the points already staged in TH_*.
 *
 * Returns 1 if the outline in TH_cur_* is now hinted and usable, 0 if
 * anything went wrong -- in which case the caller must use the outline
 * it had before, not the half-modified one here.
 */
static int tt_run_glyph(const uint8_t *code, uint32_t len) {
    TT_sp = 0;
    TT_error = 0;
    TT_insn_budget = TT_MAX_INSN;
    TT_gs = TT_gs_default;
    TT_gs.loop = 1;
    TT_gs.rp0 = TT_gs.rp1 = TT_gs.rp2 = 0;
    TT_gs.zp0 = TT_gs.zp1 = TT_gs.zp2 = 1;

    for (int i = 0; i < TT_MAX_TWILIGHT; i++) {
        TW_cur_x[i] = TW_cur_y[i] = TW_org_x[i] = TW_org_y[i] = 0;
        TW_tag[i] = 0;
    }

    tt_run(code, len, 0);
    if (TT_error) TT_n_failed++; else TT_n_hinted++;
    return !TT_error;
}

#endif /* TTFHINT_H */
