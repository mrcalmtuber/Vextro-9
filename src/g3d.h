#ifndef VEXTRO_G3D_H
#define VEXTRO_G3D_H

#include "sincos_lut.h"

/*
 * src/g3d.h — the 3D graphics API.
 *
 * src/v3d.h is a triangle rasteriser: you hand it a mesh and it draws.
 * That is a renderer, not an API, and the difference is the thing this
 * file exists to close. What an API owes a caller is:
 *
 *   a device it can ask about, and more than one implementation behind
 *     it -- here a CPU rasteriser and a Gen9 blitter path, chosen at
 *     run time from what the machine actually has;
 *   state it sets rather than arguments it repeats -- pipelines, a
 *     viewport, matrices, uniforms;
 *   buffers it fills once and draws many times;
 *   commands recorded into a buffer and submitted together, so the
 *     backend sees the whole frame and can order the work;
 *   and programmable shading, which means a language, a compiler and
 *     something to run the result on.
 *
 * All of it is integer. The kernel is built -mno-sse -mno-80387 and a
 * single float would fail to link, so positions, matrices, colours and
 * every value inside a shader are 16.16 fixed point.
 *
 * What this is not: a driver that runs shaders on the GPU. No hardware
 * reachable from this kernel can be programmed to rasterise -- the Gen9
 * driver in src/igpu.h is a blitter, deliberately, and QEMU's emulated
 * display has no 3D engine at all. So the geometry and fragment stages
 * run on the CPU, and the backend dispatches to the GPU the operations
 * the GPU actually has: clears and solid fills. g3d_backend_name()
 * reports which path is live, and the Solid app displays it, because a
 * claim of acceleration that cannot be checked is worth nothing.
 */

/* ===== fixed point ===== */

#define G3D_FP   16
#define G3D_ONE  (1 << G3D_FP)

typedef int32_t g3f;

static inline g3f g3d_mul(g3f a, g3f b) {
    return (g3f)(((int64_t)a * (int64_t)b) >> G3D_FP);
}

static inline g3f g3d_div(g3f a, g3f b) {
    if (b == 0) return 0;
    return (g3f)((((int64_t)a) << G3D_FP) / b);
}

static inline g3f g3d_sin(int deg) {
    int d = deg % 360; if (d < 0) d += 360;
    return (g3f)int_sin[d] << (G3D_FP - 10);
}
static inline g3f g3d_cos(int deg) { return g3d_sin(deg + 90); }

/* Integer square root of a 32.32 value, bit by bit. No FPU, and no
 * Newton iteration either -- this is exact and terminates in 32 steps. */
static uint32_t g3d_isqrt(uint64_t n) {
    uint64_t rem = 0, root = 0;
    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 62);
        n <<= 2;
        if (root < rem) { rem -= root | 1; root |= 2; }
    }
    return (uint32_t)(root >> 1);
}

typedef struct { g3f x, y, z; } g3d_vec3;
typedef struct { g3f x, y, z, w; } g3d_vec4;

static inline g3d_vec3 g3d_v3(g3f x, g3f y, g3f z) {
    g3d_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static inline g3f g3d_dot3(g3d_vec3 a, g3d_vec3 b) {
    return g3d_mul(a.x, b.x) + g3d_mul(a.y, b.y) + g3d_mul(a.z, b.z);
}

static inline g3d_vec3 g3d_cross(g3d_vec3 a, g3d_vec3 b) {
    return g3d_v3(g3d_mul(a.y, b.z) - g3d_mul(a.z, b.y),
                  g3d_mul(a.z, b.x) - g3d_mul(a.x, b.z),
                  g3d_mul(a.x, b.y) - g3d_mul(a.y, b.x));
}

static g3d_vec3 g3d_normalise(g3d_vec3 v) {
    const int64_t sq = (int64_t)v.x * v.x + (int64_t)v.y * v.y +
                       (int64_t)v.z * v.z;
    if (sq <= 0) return g3d_v3(0, 0, 0);
    const uint32_t len = g3d_isqrt((uint64_t)sq);
    if (len == 0) return g3d_v3(0, 0, 0);
    return g3d_v3((g3f)(((int64_t)v.x << G3D_FP) / len),
                  (g3f)(((int64_t)v.y << G3D_FP) / len),
                  (g3f)(((int64_t)v.z << G3D_FP) / len));
}

/* ===== matrices =====
 *
 * Row major: m[row * 4 + col], so a transform is a row of dot products
 * and the translation lives in the fourth column. Stated because the
 * other convention is equally common and silently transposes everything.
 */

typedef struct { g3f m[16]; } g3d_mat4;

static g3d_mat4 g3d_identity(void) {
    g3d_mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = G3D_ONE;
    return r;
}

static g3d_mat4 g3d_mat_mul(const g3d_mat4 *a, const g3d_mat4 *b) {
    g3d_mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++)
                s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
            r.m[i * 4 + j] = (g3f)(s >> G3D_FP);
        }
    return r;
}

static g3d_mat4 g3d_translate(g3f x, g3f y, g3f z) {
    g3d_mat4 r = g3d_identity();
    r.m[3] = x; r.m[7] = y; r.m[11] = z;
    return r;
}

static g3d_mat4 g3d_rotate_x(int deg) {
    g3d_mat4 r = g3d_identity();
    const g3f s = g3d_sin(deg), c = g3d_cos(deg);
    r.m[5] = c; r.m[6] = -s;
    r.m[9] = s; r.m[10] = c;
    return r;
}

static g3d_mat4 g3d_rotate_y(int deg) {
    g3d_mat4 r = g3d_identity();
    const g3f s = g3d_sin(deg), c = g3d_cos(deg);
    r.m[0] = c;  r.m[2] = s;
    r.m[8] = -s; r.m[10] = c;
    return r;
}

/*
 * A perspective projection, from the field of view and the clip planes.
 *
 * The one subtlety in fixed point: 1/tan(fov/2) at a narrow field of
 * view grows without bound, and (2*far*near)/(near-far) is large and
 * negative. Both stay inside 16.16 for any sane camera, and the caller
 * is clamped to one below.
 */
static g3d_mat4 g3d_perspective(int fov_deg, g3f aspect, g3f near, g3f far) {
    g3d_mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    if (fov_deg < 10) fov_deg = 10;
    if (fov_deg > 150) fov_deg = 150;

    const g3f t = g3d_div(g3d_sin(fov_deg / 2), g3d_cos(fov_deg / 2));
    const g3f f = g3d_div(G3D_ONE, t == 0 ? 1 : t);

    r.m[0]  = aspect ? g3d_div(f, aspect) : f;
    r.m[5]  = f;
    r.m[10] = g3d_div(far + near, near - far);
    r.m[11] = g3d_div(2 * g3d_mul(far, near), near - far);
    r.m[14] = -G3D_ONE;
    return r;
}

static g3d_vec4 g3d_transform(const g3d_mat4 *m, g3d_vec3 v) {
    g3d_vec4 o;
    const int64_t x = v.x, y = v.y, z = v.z;
    o.x = (g3f)(((int64_t)m->m[0] * x + (int64_t)m->m[1] * y +
                 (int64_t)m->m[2] * z) >> G3D_FP) + m->m[3];
    o.y = (g3f)(((int64_t)m->m[4] * x + (int64_t)m->m[5] * y +
                 (int64_t)m->m[6] * z) >> G3D_FP) + m->m[7];
    o.z = (g3f)(((int64_t)m->m[8] * x + (int64_t)m->m[9] * y +
                 (int64_t)m->m[10] * z) >> G3D_FP) + m->m[11];
    o.w = (g3f)(((int64_t)m->m[12] * x + (int64_t)m->m[13] * y +
                 (int64_t)m->m[14] * z) >> G3D_FP) + m->m[15];
    return o;
}

/* Rotate a direction: the upper 3x3 only, so translation is ignored. */
static g3d_vec3 g3d_transform_dir(const g3d_mat4 *m, g3d_vec3 v) {
    const int64_t x = v.x, y = v.y, z = v.z;
    return g3d_v3(
        (g3f)(((int64_t)m->m[0] * x + (int64_t)m->m[1] * y +
               (int64_t)m->m[2] * z) >> G3D_FP),
        (g3f)(((int64_t)m->m[4] * x + (int64_t)m->m[5] * y +
               (int64_t)m->m[6] * z) >> G3D_FP),
        (g3f)(((int64_t)m->m[8] * x + (int64_t)m->m[9] * y +
               (int64_t)m->m[10] * z) >> G3D_FP));
}

/* =====================================================================
 * G3SL — the shader language
 *
 * A small language, a real compiler: tokeniser, recursive-descent parser
 * with operator precedence, static type checking over two types
 * (scalar and vec3), and a stack machine to run the result.
 *
 *     # a lambert term, and a rim light on the silhouette
 *     d   = sat(dot(n, l));
 *     rim = sat(1.0 - dot(n, e));
 *     color = base * (0.2 + 0.8 * d) + gold * (rim * rim * 0.5);
 *
 * Types are inferred and checked: `dot` takes two vec3 and yields a
 * scalar, `*` accepts scalar*scalar, vec3*scalar or vec3*vec3, and a
 * mismatch is a compile error with a line number rather than something
 * strange on screen.
 *
 * Programs are compiled at run time from text the app holds, which is
 * the point -- editing the shader in the Solid window recompiles it and
 * the next frame is different.
 * ===================================================================== */

#define G3SL_MAX_CODE   256
#define G3SL_MAX_VARS    32
#define G3SL_MAX_CONST   64
#define G3SL_NAME_MAX    16
#define G3SL_ERR_MAX     72

enum {
    G3SL_T_SCALAR = 1,
    G3SL_T_VEC3   = 3,
};

enum {
    OP_PUSHC = 1,   /* push constant[a]            */
    OP_PUSHV,       /* push variable[a]            */
    OP_STORE,       /* pop into variable[a]        */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_DOT,         /* vec3 vec3 -> scalar         */
    OP_CROSS,
    OP_SAT,         /* clamp to 0..1               */
    OP_MIN, OP_MAX,
    OP_MIX,         /* mix(a, b, t)                */
    OP_NEG,
    OP_ABS,
    OP_SQRT,
    OP_NORM,        /* normalise a vec3            */
    OP_MKVEC,       /* three scalars -> vec3       */
    OP_SPLAT,       /* scalar -> vec3              */
};

typedef struct {
    uint8_t op;
    uint8_t a;
} g3sl_ins_t;

typedef struct {
    char    name[G3SL_NAME_MAX];
    uint8_t type;
    uint8_t is_input;
} g3sl_var_t;

typedef struct {
    g3sl_ins_t code[G3SL_MAX_CODE];
    int        ncode;
    g3f        konst[G3SL_MAX_CONST];
    int        nconst;
    g3sl_var_t vars[G3SL_MAX_VARS];
    int        nvars;
    int        ok;
    int        err_line;
    char       err[G3SL_ERR_MAX];
} g3sl_prog_t;

/* --- the values a program computes --- */

typedef struct {
    g3f     v[3];
    uint8_t type;
} g3sl_val_t;

/* --- tokeniser --- */

enum {
    TK_END = 0, TK_NAME, TK_NUM, TK_PUNCT
};

typedef struct {
    const char *src;
    int         pos;
    int         line;
    int         kind;
    char        text[G3SL_NAME_MAX];
    g3f         num;
    char        punct;
} g3sl_lex_t;

static int g3sl_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int g3sl_is_digit(char c) { return c >= '0' && c <= '9'; }

static void g3sl_next(g3sl_lex_t *lx) {
    /* whitespace and comments */
    for (;;) {
        char c = lx->src[lx->pos];
        if (c == '\n') { lx->line++; lx->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; continue; }
        if (c == '#') {
            while (lx->src[lx->pos] && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        break;
    }

    const char c = lx->src[lx->pos];
    if (!c) { lx->kind = TK_END; return; }

    if (g3sl_is_alpha(c)) {
        int n = 0;
        while (g3sl_is_alpha(lx->src[lx->pos]) ||
               g3sl_is_digit(lx->src[lx->pos])) {
            if (n < G3SL_NAME_MAX - 1) lx->text[n++] = lx->src[lx->pos];
            lx->pos++;
        }
        lx->text[n] = '\0';
        lx->kind = TK_NAME;
        return;
    }

    if (g3sl_is_digit(c) || (c == '.' && g3sl_is_digit(lx->src[lx->pos + 1]))) {
        /* Decimal into 16.16, accumulating the fraction as a scaled
         * integer so there is never a division by a power of ten that
         * loses the low bits. */
        int64_t whole = 0;
        while (g3sl_is_digit(lx->src[lx->pos]))
            whole = whole * 10 + (lx->src[lx->pos++] - '0');
        g3f frac = 0;
        if (lx->src[lx->pos] == '.') {
            lx->pos++;
            int64_t num = 0, den = 1;
            while (g3sl_is_digit(lx->src[lx->pos]) && den < 1000000) {
                num = num * 10 + (lx->src[lx->pos++] - '0');
                den *= 10;
            }
            while (g3sl_is_digit(lx->src[lx->pos])) lx->pos++;   /* excess */
            frac = (g3f)((num << G3D_FP) / den);
        }
        lx->num = (g3f)((whole << G3D_FP) + frac);
        lx->kind = TK_NUM;
        return;
    }

    lx->punct = c;
    lx->pos++;
    lx->kind = TK_PUNCT;
}

static void g3sl_lex_init(g3sl_lex_t *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    g3sl_next(lx);
}

/* --- compiler --- */

typedef struct {
    g3sl_lex_t  lx;
    g3sl_prog_t *p;
    int          failed;
} g3sl_ctx_t;

static void g3sl_fail(g3sl_ctx_t *c, const char *msg) {
    if (c->failed) return;
    c->failed = 1;
    c->p->ok = 0;
    c->p->err_line = c->lx.line;
    int i = 0;
    while (msg[i] && i < G3SL_ERR_MAX - 1) { c->p->err[i] = msg[i]; i++; }
    c->p->err[i] = '\0';
}

static int g3sl_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void g3sl_emit(g3sl_ctx_t *c, uint8_t op, uint8_t a) {
    if (c->p->ncode >= G3SL_MAX_CODE) { g3sl_fail(c, "program too long"); return; }
    c->p->code[c->p->ncode].op = op;
    c->p->code[c->p->ncode].a = a;
    c->p->ncode++;
}

static int g3sl_const(g3sl_ctx_t *c, g3f v) {
    for (int i = 0; i < c->p->nconst; i++)
        if (c->p->konst[i] == v) return i;
    if (c->p->nconst >= G3SL_MAX_CONST) { g3sl_fail(c, "too many constants"); return 0; }
    c->p->konst[c->p->nconst] = v;
    return c->p->nconst++;
}

static int g3sl_var_find(const g3sl_prog_t *p, const char *name) {
    for (int i = 0; i < p->nvars; i++)
        if (g3sl_streq(p->vars[i].name, name)) return i;
    return -1;
}

static int g3sl_var_add(g3sl_ctx_t *c, const char *name, uint8_t type,
                        int is_input) {
    if (c->p->nvars >= G3SL_MAX_VARS) { g3sl_fail(c, "too many variables"); return 0; }
    g3sl_var_t *v = &c->p->vars[c->p->nvars];
    int i = 0;
    while (name[i] && i < G3SL_NAME_MAX - 1) { v->name[i] = name[i]; i++; }
    v->name[i] = '\0';
    v->type = type;
    v->is_input = (uint8_t)is_input;
    return c->p->nvars++;
}

static uint8_t g3sl_expr(g3sl_ctx_t *c);

/* A call's arguments, checked for count and type as they are compiled. */
static uint8_t g3sl_call(g3sl_ctx_t *c, const char *fn) {
    struct { const char *name; int argc; } table[] = {
        { "dot", 2 }, { "cross", 2 }, { "sat", 1 }, { "min", 2 },
        { "max", 2 }, { "mix", 3 }, { "abs", 1 }, { "sqrt", 1 },
        { "norm", 1 }, { "vec3", 3 },
    };
    int which = -1;
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++)
        if (g3sl_streq(table[i].name, fn)) { which = i; break; }
    if (which < 0) { g3sl_fail(c, "unknown function"); return G3SL_T_SCALAR; }

    if (!(c->lx.kind == TK_PUNCT && c->lx.punct == '(')) {
        g3sl_fail(c, "expected ( after a function name");
        return G3SL_T_SCALAR;
    }
    g3sl_next(&c->lx);

    uint8_t at[3] = { 0, 0, 0 };
    for (int i = 0; i < table[which].argc; i++) {
        at[i] = g3sl_expr(c);
        if (c->failed) return G3SL_T_SCALAR;
        if (i + 1 < table[which].argc) {
            if (!(c->lx.kind == TK_PUNCT && c->lx.punct == ',')) {
                g3sl_fail(c, "expected , between arguments");
                return G3SL_T_SCALAR;
            }
            g3sl_next(&c->lx);
        }
    }
    if (!(c->lx.kind == TK_PUNCT && c->lx.punct == ')')) {
        g3sl_fail(c, "expected ) after the arguments");
        return G3SL_T_SCALAR;
    }
    g3sl_next(&c->lx);

    switch (which) {
    case 0:   /* dot */
        if (at[0] != G3SL_T_VEC3 || at[1] != G3SL_T_VEC3)
            { g3sl_fail(c, "dot takes two vec3"); return G3SL_T_SCALAR; }
        g3sl_emit(c, OP_DOT, 0);
        return G3SL_T_SCALAR;
    case 1:   /* cross */
        if (at[0] != G3SL_T_VEC3 || at[1] != G3SL_T_VEC3)
            { g3sl_fail(c, "cross takes two vec3"); return G3SL_T_SCALAR; }
        g3sl_emit(c, OP_CROSS, 0);
        return G3SL_T_VEC3;
    case 2:   /* sat */
        g3sl_emit(c, OP_SAT, 0);
        return at[0];
    case 3: case 4:   /* min, max */
        if (at[0] != at[1])
            { g3sl_fail(c, "min and max need matching types"); return at[0]; }
        g3sl_emit(c, which == 3 ? OP_MIN : OP_MAX, 0);
        return at[0];
    case 5:   /* mix(a, b, t) */
        if (at[0] != at[1])
            { g3sl_fail(c, "mix needs its first two arguments to match"); return at[0]; }
        if (at[2] != G3SL_T_SCALAR)
            { g3sl_fail(c, "the third argument to mix is a scalar"); return at[0]; }
        g3sl_emit(c, OP_MIX, 0);
        return at[0];
    case 6:
        g3sl_emit(c, OP_ABS, 0);
        return at[0];
    case 7:
        if (at[0] != G3SL_T_SCALAR)
            { g3sl_fail(c, "sqrt takes a scalar"); return G3SL_T_SCALAR; }
        g3sl_emit(c, OP_SQRT, 0);
        return G3SL_T_SCALAR;
    case 8:   /* norm */
        if (at[0] != G3SL_T_VEC3)
            { g3sl_fail(c, "norm takes a vec3"); return G3SL_T_VEC3; }
        g3sl_emit(c, OP_NORM, 0);
        return G3SL_T_VEC3;
    default:  /* vec3 */
        if (at[0] != G3SL_T_SCALAR || at[1] != G3SL_T_SCALAR ||
            at[2] != G3SL_T_SCALAR)
            { g3sl_fail(c, "vec3 takes three scalars"); return G3SL_T_VEC3; }
        g3sl_emit(c, OP_MKVEC, 0);
        return G3SL_T_VEC3;
    }
}

static uint8_t g3sl_primary(g3sl_ctx_t *c) {
    if (c->lx.kind == TK_NUM) {
        const int k = g3sl_const(c, c->lx.num);
        g3sl_emit(c, OP_PUSHC, (uint8_t)k);
        g3sl_next(&c->lx);
        return G3SL_T_SCALAR;
    }
    if (c->lx.kind == TK_PUNCT && c->lx.punct == '(') {
        g3sl_next(&c->lx);
        const uint8_t t = g3sl_expr(c);
        if (!(c->lx.kind == TK_PUNCT && c->lx.punct == ')')) {
            g3sl_fail(c, "expected )");
            return t;
        }
        g3sl_next(&c->lx);
        return t;
    }
    if (c->lx.kind == TK_PUNCT && c->lx.punct == '-') {
        g3sl_next(&c->lx);
        const uint8_t t = g3sl_primary(c);
        g3sl_emit(c, OP_NEG, 0);
        return t;
    }
    if (c->lx.kind == TK_NAME) {
        char name[G3SL_NAME_MAX];
        for (int i = 0; i < G3SL_NAME_MAX; i++) name[i] = c->lx.text[i];
        g3sl_next(&c->lx);
        if (c->lx.kind == TK_PUNCT && c->lx.punct == '(')
            return g3sl_call(c, name);
        const int v = g3sl_var_find(c->p, name);
        if (v < 0) { g3sl_fail(c, "unknown name"); return G3SL_T_SCALAR; }
        g3sl_emit(c, OP_PUSHV, (uint8_t)v);
        return c->p->vars[v].type;
    }
    g3sl_fail(c, "expected a value");
    return G3SL_T_SCALAR;
}

static uint8_t g3sl_term(g3sl_ctx_t *c) {
    uint8_t lt = g3sl_primary(c);
    while (!c->failed && c->lx.kind == TK_PUNCT &&
           (c->lx.punct == '*' || c->lx.punct == '/')) {
        const char op = c->lx.punct;
        g3sl_next(&c->lx);
        const uint8_t rt = g3sl_primary(c);
        /*
         * Mixed arithmetic is allowed in one direction only: a vec3
         * scaled by a scalar. The scalar is broadcast to three lanes
         * here so the machine itself only ever sees matching operands.
         */
        if (lt == G3SL_T_VEC3 && rt == G3SL_T_SCALAR) {
            g3sl_emit(c, OP_SPLAT, 0);
        } else if (lt == G3SL_T_SCALAR && rt == G3SL_T_VEC3) {
            if (op == '/') { g3sl_fail(c, "cannot divide a scalar by a vec3"); return lt; }
            g3sl_emit(c, OP_SPLAT, 1);      /* splat the deeper operand */
            lt = G3SL_T_VEC3;
        } else if (lt != rt) {
            g3sl_fail(c, "type mismatch in a product");
            return lt;
        }
        g3sl_emit(c, op == '*' ? OP_MUL : OP_DIV, 0);
    }
    return lt;
}

static uint8_t g3sl_expr(g3sl_ctx_t *c) {
    uint8_t lt = g3sl_term(c);
    while (!c->failed && c->lx.kind == TK_PUNCT &&
           (c->lx.punct == '+' || c->lx.punct == '-')) {
        const char op = c->lx.punct;
        g3sl_next(&c->lx);
        const uint8_t rt = g3sl_term(c);
        if (lt == G3SL_T_VEC3 && rt == G3SL_T_SCALAR) {
            g3sl_emit(c, OP_SPLAT, 0);
        } else if (lt == G3SL_T_SCALAR && rt == G3SL_T_VEC3) {
            g3sl_emit(c, OP_SPLAT, 1);
            lt = G3SL_T_VEC3;
        } else if (lt != rt) {
            g3sl_fail(c, "type mismatch in a sum");
            return lt;
        }
        g3sl_emit(c, op == '+' ? OP_ADD : OP_SUB, 0);
    }
    return lt;
}

/*
 * Compile a program. `inputs` names the variables the pipeline binds
 * before each run, in order, with their types; everything else the
 * program mentions must be assigned before it is read.
 */
static void g3sl_compile(g3sl_prog_t *p, const char *src,
                         const char *const *in_names,
                         const uint8_t *in_types, int n_in) {
    p->ncode = 0;
    p->nconst = 0;
    p->nvars = 0;
    p->ok = 1;
    p->err[0] = '\0';
    p->err_line = 0;

    g3sl_ctx_t c;
    c.p = p;
    c.failed = 0;
    for (int i = 0; i < n_in; i++)
        g3sl_var_add(&c, in_names[i], in_types[i], 1);

    g3sl_lex_init(&c.lx, src);

    while (!c.failed && c.lx.kind != TK_END) {
        if (c.lx.kind != TK_NAME) { g3sl_fail(&c, "expected a name"); break; }
        char name[G3SL_NAME_MAX];
        for (int i = 0; i < G3SL_NAME_MAX; i++) name[i] = c.lx.text[i];
        g3sl_next(&c.lx);
        if (!(c.lx.kind == TK_PUNCT && c.lx.punct == '=')) {
            g3sl_fail(&c, "expected = after a name");
            break;
        }
        g3sl_next(&c.lx);

        const uint8_t t = g3sl_expr(&c);
        if (c.failed) break;

        int v = g3sl_var_find(p, name);
        if (v < 0) {
            v = g3sl_var_add(&c, name, t, 0);
        } else if (p->vars[v].type != t) {
            g3sl_fail(&c, "a variable cannot change type");
            break;
        }
        g3sl_emit(&c, OP_STORE, (uint8_t)v);

        if (c.lx.kind == TK_PUNCT && c.lx.punct == ';') g3sl_next(&c.lx);
    }

    if (p->ok && g3sl_var_find(p, "color") < 0) {
        c.lx.line = 0;
        g3sl_fail(&c, "the program never assigns color");
    }
}

/* --- the machine --- */

#define G3SL_STACK 16

static void g3sl_run(const g3sl_prog_t *p, g3sl_val_t *vars) {
    g3sl_val_t st[G3SL_STACK];
    int sp = 0;

    for (int pc = 0; pc < p->ncode; pc++) {
        const uint8_t op = p->code[pc].op;
        const uint8_t a = p->code[pc].a;

        /* Every op below either pushes one value or consumes a known
         * number, and the compiler bounds the depth; the guards are here
         * so a malformed program cannot walk off the stack. */
        switch (op) {
        case OP_PUSHC:
            if (sp >= G3SL_STACK) return;
            st[sp].v[0] = p->konst[a];
            st[sp].type = G3SL_T_SCALAR;
            sp++;
            break;
        case OP_PUSHV:
            if (sp >= G3SL_STACK) return;
            st[sp++] = vars[a];
            break;
        case OP_STORE:
            if (sp < 1) return;
            vars[a] = st[--sp];
            break;
        case OP_SPLAT: {
            /* a == 0: the value on top; a == 1: the one below it */
            const int i = sp - 1 - (a ? 1 : 0);
            if (i < 0) return;
            st[i].v[1] = st[i].v[2] = st[i].v[0];
            st[i].type = G3SL_T_VEC3;
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_MIN: case OP_MAX: {
            if (sp < 2) return;
            g3sl_val_t b = st[--sp];
            g3sl_val_t *r = &st[sp - 1];
            const int n = r->type == G3SL_T_VEC3 ? 3 : 1;
            for (int i = 0; i < n; i++) {
                switch (op) {
                case OP_ADD: r->v[i] += b.v[i]; break;
                case OP_SUB: r->v[i] -= b.v[i]; break;
                case OP_MUL: r->v[i] = g3d_mul(r->v[i], b.v[i]); break;
                case OP_DIV: r->v[i] = g3d_div(r->v[i], b.v[i]); break;
                case OP_MIN: if (b.v[i] < r->v[i]) r->v[i] = b.v[i]; break;
                default:     if (b.v[i] > r->v[i]) r->v[i] = b.v[i]; break;
                }
            }
            break;
        }
        case OP_DOT: {
            if (sp < 2) return;
            g3sl_val_t b = st[--sp];
            g3sl_val_t *r = &st[sp - 1];
            const g3f d = g3d_mul(r->v[0], b.v[0]) + g3d_mul(r->v[1], b.v[1]) +
                          g3d_mul(r->v[2], b.v[2]);
            r->v[0] = d;
            r->type = G3SL_T_SCALAR;
            break;
        }
        case OP_CROSS: {
            if (sp < 2) return;
            g3sl_val_t b = st[--sp];
            g3sl_val_t *r = &st[sp - 1];
            const g3d_vec3 cr = g3d_cross(g3d_v3(r->v[0], r->v[1], r->v[2]),
                                          g3d_v3(b.v[0], b.v[1], b.v[2]));
            r->v[0] = cr.x; r->v[1] = cr.y; r->v[2] = cr.z;
            break;
        }
        case OP_MIX: {
            if (sp < 3) return;
            g3sl_val_t t = st[--sp];
            g3sl_val_t b = st[--sp];
            g3sl_val_t *r = &st[sp - 1];
            const int n = r->type == G3SL_T_VEC3 ? 3 : 1;
            for (int i = 0; i < n; i++)
                r->v[i] = r->v[i] + g3d_mul(b.v[i] - r->v[i], t.v[0]);
            break;
        }
        case OP_SAT: {
            if (sp < 1) return;
            g3sl_val_t *r = &st[sp - 1];
            const int n = r->type == G3SL_T_VEC3 ? 3 : 1;
            for (int i = 0; i < n; i++) {
                if (r->v[i] < 0) r->v[i] = 0;
                else if (r->v[i] > G3D_ONE) r->v[i] = G3D_ONE;
            }
            break;
        }
        case OP_NEG: case OP_ABS: {
            if (sp < 1) return;
            g3sl_val_t *r = &st[sp - 1];
            const int n = r->type == G3SL_T_VEC3 ? 3 : 1;
            for (int i = 0; i < n; i++)
                r->v[i] = op == OP_NEG ? -r->v[i]
                                       : (r->v[i] < 0 ? -r->v[i] : r->v[i]);
            break;
        }
        case OP_SQRT: {
            if (sp < 1) return;
            g3sl_val_t *r = &st[sp - 1];
            r->v[0] = r->v[0] <= 0 ? 0
                    : (g3f)g3d_isqrt((uint64_t)r->v[0] << G3D_FP);
            break;
        }
        case OP_NORM: {
            if (sp < 1) return;
            g3sl_val_t *r = &st[sp - 1];
            const g3d_vec3 u = g3d_normalise(g3d_v3(r->v[0], r->v[1], r->v[2]));
            r->v[0] = u.x; r->v[1] = u.y; r->v[2] = u.z;
            break;
        }
        case OP_MKVEC: {
            if (sp < 3) return;
            const g3f z = st[--sp].v[0];
            const g3f y = st[--sp].v[0];
            g3sl_val_t *r = &st[sp - 1];
            r->v[1] = y; r->v[2] = z;
            r->type = G3SL_T_VEC3;
            break;
        }
        default:
            return;
        }
    }
}

/* =====================================================================
 * The device
 * ===================================================================== */

enum { G3D_BACKEND_CPU = 0, G3D_BACKEND_GEN9 = 1 };

enum { G3D_CULL_NONE = 0, G3D_CULL_BACK, G3D_CULL_FRONT };
enum { G3D_SHADE_FLAT = 0, G3D_SHADE_PIXEL };

typedef struct {
    uint8_t depth_test;
    uint8_t depth_write;
    uint8_t cull;
    uint8_t shade;
    const g3sl_prog_t *fs;
} g3d_pipeline_t;

typedef struct {
    uint32_t *color;
    int32_t   w, h;            /* the buffer this draws into */
    int32_t   vx, vy, vw, vh;  /* viewport within it */
    /* Set only when `color` is the scanned-out framebuffer. The blitter
     * writes through the GGTT, which maps that and nothing else, so this
     * is what decides whether the clear can go to hardware. Comparing
     * pointers would not do: the kernel sees a virtual address and the
     * GPU a physical one. */
    uint8_t   screen;
} g3d_target_t;

typedef struct {
    const g3d_vec3 *pos;
    const g3d_vec3 *normal;    /* per vertex; may be null */
    int             count;
} g3d_vbuf_t;

typedef struct {
    const uint16_t *idx;
    int             count;     /* a multiple of three */
} g3d_ibuf_t;

/* Depth buffer, sized for the largest viewport an app draws into. */
#define G3D_MAX_W 640
#define G3D_MAX_H 480
static int32_t g3d_depth[G3D_MAX_W * G3D_MAX_H];

static struct {
    uint32_t frames;
    uint32_t tris_in, tris_drawn, tris_culled, tris_clipped;
    uint32_t fragments;
    uint32_t shader_runs;
    uint32_t gpu_ops;          /* operations dispatched to hardware */
    uint32_t cpu_ops;          /* ...and those that fell back */
} g3d_stats;

static int g3d_backend = G3D_BACKEND_CPU;

static const char *g3d_backend_name(void) {
    return g3d_backend == G3D_BACKEND_GEN9 ? "Gen9 blitter + CPU raster"
                                           : "CPU";
}

/*
 * Pick a backend.
 *
 * The Gen9 path is chosen only when the driver actually came up and
 * reported the framebuffer reachable through the GGTT -- igpu.active is
 * set by a self-test that writes a pattern with the blitter and reads it
 * back with the CPU, so this is a checked capability rather than a
 * device ID lookup. On anything else, including every emulator this is
 * developed on, the answer is CPU and the app says so.
 */
static void g3d_select_backend(void) {
#if defined(IGPU_H)
    if (igpu.active && igpu.fb_blittable) {
        g3d_backend = G3D_BACKEND_GEN9;
        return;
    }
#endif
    g3d_backend = G3D_BACKEND_CPU;
}

/* ===== the command buffer ===== */

enum {
    G3D_CMD_CLEAR = 1,
    G3D_CMD_PIPELINE,
    G3D_CMD_MATRIX,
    G3D_CMD_UNIFORM,
    G3D_CMD_DRAW,
};

#define G3D_MAX_CMDS 64

typedef struct {
    uint8_t op;
    uint8_t slot;
    uint32_t colour;
    g3d_vec3 vec;
    const g3d_pipeline_t *pipe;
    const g3d_vbuf_t *vb;
    const g3d_ibuf_t *ib;
    g3d_mat4 mat;
} g3d_cmd_t;

/* Uniform slots the pipeline binds for a program. */
enum {
    G3D_U_LIGHT = 0,
    G3D_U_BASE,
    G3D_U_ACCENT,
    G3D_U_EYE,
    G3D_U_COUNT
};

static struct {
    g3d_cmd_t cmd[G3D_MAX_CMDS];
    int       n;
    int       recording;
    g3d_target_t target;
    g3d_mat4  model, view, proj;
    g3d_vec3  uniform[G3D_U_COUNT];
    const g3d_pipeline_t *pipe;
} g3d_cb;

static void g3d_begin(const g3d_target_t *t) {
    g3d_cb.n = 0;
    g3d_cb.recording = 1;
    g3d_cb.target = *t;
}

static void g3d_cmd_clear(uint32_t colour) {
    if (!g3d_cb.recording || g3d_cb.n >= G3D_MAX_CMDS) return;
    g3d_cmd_t *c = &g3d_cb.cmd[g3d_cb.n++];
    c->op = G3D_CMD_CLEAR;
    c->colour = colour;
}

static void g3d_cmd_pipeline(const g3d_pipeline_t *p) {
    if (!g3d_cb.recording || g3d_cb.n >= G3D_MAX_CMDS) return;
    g3d_cmd_t *c = &g3d_cb.cmd[g3d_cb.n++];
    c->op = G3D_CMD_PIPELINE;
    c->pipe = p;
}

/* slot: 0 model, 1 view, 2 projection */
static void g3d_cmd_matrix(int slot, const g3d_mat4 *m) {
    if (!g3d_cb.recording || g3d_cb.n >= G3D_MAX_CMDS) return;
    g3d_cmd_t *c = &g3d_cb.cmd[g3d_cb.n++];
    c->op = G3D_CMD_MATRIX;
    c->slot = (uint8_t)slot;
    c->mat = *m;
}

static void g3d_cmd_uniform(int slot, g3d_vec3 v) {
    if (!g3d_cb.recording || g3d_cb.n >= G3D_MAX_CMDS) return;
    g3d_cmd_t *c = &g3d_cb.cmd[g3d_cb.n++];
    c->op = G3D_CMD_UNIFORM;
    c->slot = (uint8_t)slot;
    c->vec = v;
}

static void g3d_cmd_draw(const g3d_vbuf_t *vb, const g3d_ibuf_t *ib) {
    if (!g3d_cb.recording || g3d_cb.n >= G3D_MAX_CMDS) return;
    g3d_cmd_t *c = &g3d_cb.cmd[g3d_cb.n++];
    c->op = G3D_CMD_DRAW;
    c->vb = vb;
    c->ib = ib;
}

/* ===== execution ===== */

static void g3d_clear_depth(void) {
    const int32_t n = g3d_cb.target.vw * g3d_cb.target.vh;
    const int32_t lim = G3D_MAX_W * G3D_MAX_H;
    for (int32_t i = 0; i < n && i < lim; i++) g3d_depth[i] = -0x7FFFFFFF;
}

/*
 * Clear the viewport.
 *
 * This is the one stage with a hardware path: XY_COLOR_BLT fills a
 * rectangle, which is exactly a clear, and igpu_screen_fill already
 * wraps it with the ring submission and the hang recovery. It applies
 * only when the target is the visible framebuffer -- the blitter writes
 * through the GGTT, and an arbitrary window back-buffer is not mapped
 * there.
 */
static void g3d_exec_clear(uint32_t colour) {
    const g3d_target_t *t = &g3d_cb.target;

#if defined(IGPU_H)
    if (g3d_backend == G3D_BACKEND_GEN9 && t->screen &&
        igpu_screen_fill(t->vx, t->vy, t->vw, t->vh, colour) == 0) {
        g3d_stats.gpu_ops++;
        g3d_clear_depth();
        return;
    }
#endif

    g3d_stats.cpu_ops++;
    for (int32_t y = 0; y < t->vh; y++) {
        const int32_t py = t->vy + y;
        if (py < 0 || py >= t->h) continue;
        uint32_t *row = t->color + (int64_t)py * t->w;
        for (int32_t x = 0; x < t->vw; x++) {
            const int32_t px = t->vx + x;
            if (px >= 0 && px < t->w) row[px] = colour;
        }
    }
    g3d_clear_depth();
}

/* A vertex after the geometry stage. */
typedef struct {
    int32_t  sx, sy;      /* screen pixels */
    g3f      invw;        /* 1/w, which is what interpolates linearly */
    g3d_vec3 normal;      /* in world space, for the shader */
    int      behind;
} g3d_sv_t;

static inline int64_t g3d_edge(const g3d_sv_t *a, const g3d_sv_t *b,
                               int32_t px, int32_t py) {
    return (int64_t)(b->sx - a->sx) * (py - a->sy) -
           (int64_t)(b->sy - a->sy) * (px - a->sx);
}

/* Pack a shader's vec3 result into a pixel. */
static inline uint32_t g3d_pack(const g3f *v) {
    int32_t r = v[0], g = v[1], b = v[2];
    if (r < 0) r = 0; else if (r > G3D_ONE) r = G3D_ONE;
    if (g < 0) g = 0; else if (g > G3D_ONE) g = G3D_ONE;
    if (b < 0) b = 0; else if (b > G3D_ONE) b = G3D_ONE;
    return ((uint32_t)(r * 255 >> G3D_FP) << 16) |
           ((uint32_t)(g * 255 >> G3D_FP) << 8) |
            (uint32_t)(b * 255 >> G3D_FP);
}

/*
 * Rasterise one triangle.
 *
 * Edge functions, evaluated once at the bounding-box corner and stepped
 * by a constant per pixel, so the inner loop is three adds and three
 * compares. Depth is 1/w, which is linear in screen space; storing w
 * itself would need a divide per pixel to interpolate honestly.
 */
static void g3d_raster(const g3d_pipeline_t *pipe, const g3d_sv_t *a,
                       const g3d_sv_t *b, const g3d_sv_t *c,
                       g3sl_val_t *vars, int n_in) {
    const g3d_target_t *t = &g3d_cb.target;

    int32_t minx = a->sx < b->sx ? (a->sx < c->sx ? a->sx : c->sx)
                                 : (b->sx < c->sx ? b->sx : c->sx);
    int32_t maxx = a->sx > b->sx ? (a->sx > c->sx ? a->sx : c->sx)
                                 : (b->sx > c->sx ? b->sx : c->sx);
    int32_t miny = a->sy < b->sy ? (a->sy < c->sy ? a->sy : c->sy)
                                 : (b->sy < c->sy ? b->sy : c->sy);
    int32_t maxy = a->sy > b->sy ? (a->sy > c->sy ? a->sy : c->sy)
                                 : (b->sy > c->sy ? b->sy : c->sy);

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > t->vw - 1) maxx = t->vw - 1;
    if (maxy > t->vh - 1) maxy = t->vh - 1;
    if (minx > maxx || miny > maxy) return;

    const int64_t area = g3d_edge(a, b, c->sx, c->sy);
    if (area == 0) return;

    /* Flat shading runs the program once for the whole primitive, which
     * is what "flat" means -- and it is the difference between a shader
     * that costs microseconds and one that costs a frame. */
    uint32_t flat = 0;
    if (pipe->shade == G3D_SHADE_FLAT && pipe->fs && pipe->fs->ok) {
        g3sl_val_t local[G3SL_MAX_VARS];
        for (int i = 0; i < pipe->fs->nvars; i++) {
            local[i] = (i < n_in) ? vars[i] : (g3sl_val_t){ { 0, 0, 0 }, 1 };
        }
        g3sl_run(pipe->fs, local);
        const int out = g3sl_var_find(pipe->fs, "color");
        flat = out >= 0 ? g3d_pack(local[out].v) : 0x808080u;
        g3d_stats.shader_runs++;
    }

    for (int32_t py = miny; py <= maxy; py++) {
        int64_t w0 = g3d_edge(b, c, minx, py);
        int64_t w1 = g3d_edge(c, a, minx, py);
        int64_t w2 = g3d_edge(a, b, minx, py);
        const int64_t s0 = -(int64_t)(c->sy - b->sy);
        const int64_t s1 = -(int64_t)(a->sy - c->sy);
        const int64_t s2 = -(int64_t)(b->sy - a->sy);

        uint32_t *row = t->color + (int64_t)(t->vy + py) * t->w + t->vx;
        int32_t *drow = g3d_depth + (int64_t)py * G3D_MAX_W;

        for (int32_t px = minx; px <= maxx; px++, w0 += s0, w1 += s1, w2 += s2) {
            const int inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                        : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;

            /* barycentric, as fractions of the signed area */
            const int64_t l0 = w0, l1 = w1, l2 = w2;
            const int32_t z = (int32_t)(((int64_t)a->invw * l0 +
                                         (int64_t)b->invw * l1 +
                                         (int64_t)c->invw * l2) / area);

            if (pipe->depth_test && px < G3D_MAX_W && py < G3D_MAX_H) {
                if (z <= drow[px]) continue;
            }
            if (pipe->depth_write && px < G3D_MAX_W && py < G3D_MAX_H)
                drow[px] = z;

            uint32_t colour = flat;
            if (pipe->shade == G3D_SHADE_PIXEL && pipe->fs && pipe->fs->ok) {
                g3sl_val_t local[G3SL_MAX_VARS];
                for (int i = 0; i < pipe->fs->nvars; i++)
                    local[i] = (i < n_in) ? vars[i]
                                          : (g3sl_val_t){ { 0, 0, 0 }, 1 };
                /* interpolate the normal across the face */
                if (n_in > 0) {
                    const g3f nx = (g3f)(((int64_t)a->normal.x * l0 +
                                          (int64_t)b->normal.x * l1 +
                                          (int64_t)c->normal.x * l2) / area);
                    const g3f ny = (g3f)(((int64_t)a->normal.y * l0 +
                                          (int64_t)b->normal.y * l1 +
                                          (int64_t)c->normal.y * l2) / area);
                    const g3f nz = (g3f)(((int64_t)a->normal.z * l0 +
                                          (int64_t)b->normal.z * l1 +
                                          (int64_t)c->normal.z * l2) / area);
                    const g3d_vec3 nn = g3d_normalise(g3d_v3(nx, ny, nz));
                    local[0].v[0] = nn.x;
                    local[0].v[1] = nn.y;
                    local[0].v[2] = nn.z;
                    local[0].type = G3SL_T_VEC3;
                }
                g3sl_run(pipe->fs, local);
                const int out = g3sl_var_find(pipe->fs, "color");
                colour = out >= 0 ? g3d_pack(local[out].v) : 0x808080u;
                g3d_stats.shader_runs++;
            }
            row[px] = colour;
            g3d_stats.fragments++;
        }
    }
}

/* Shader input names and types, in the order the pipeline binds them. */
static const char *const g3d_in_names[] = { "n", "l", "base", "gold", "e" };
static const uint8_t g3d_in_types[] = {
    G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3, G3SL_T_VEC3
};
#define G3D_N_INPUTS ((int)(sizeof(g3d_in_types) / sizeof(g3d_in_types[0])))

static void g3d_exec_draw(const g3d_pipeline_t *pipe,
                          const g3d_vbuf_t *vb, const g3d_ibuf_t *ib) {
    if (!pipe || !vb || !ib) return;
    const g3d_target_t *t = &g3d_cb.target;

    g3d_mat4 mv = g3d_mat_mul(&g3d_cb.view, &g3d_cb.model);
    g3d_mat4 mvp = g3d_mat_mul(&g3d_cb.proj, &mv);

    for (int i = 0; i + 2 < ib->count; i += 3) {
        g3d_stats.tris_in++;

        /*
         * The face normal, from the cross product of two edges in model
         * space. Computed before the vertices because it is also the
         * fallback for a mesh that carries no per-vertex normals -- a
         * cube has none, and should not: its faces are flat and sharing
         * a normal at a corner would round it off.
         *
         * Taking the normal from the *projected* triangle instead --
         * which is cheaper and tempting -- makes faces darken as they
         * shrink rather than as they turn away, and that is visible.
         */
        g3d_vec3 fn;
        {
            const int i0 = ib->idx[i], i1 = ib->idx[i + 1], i2 = ib->idx[i + 2];
            if (i0 >= vb->count || i1 >= vb->count || i2 >= vb->count) return;
            const g3d_vec3 p0 = vb->pos[i0], p1 = vb->pos[i1], p2 = vb->pos[i2];
            const g3d_vec3 e1 = g3d_v3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
            const g3d_vec3 e2 = g3d_v3(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
            fn = g3d_normalise(g3d_transform_dir(&g3d_cb.model,
                                                 g3d_cross(e1, e2)));
        }

        g3d_sv_t sv[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            const int vi = ib->idx[i + k];
            if (vi >= vb->count) return;
            const g3d_vec4 clip = g3d_transform(&mvp, vb->pos[vi]);

            /*
             * The near plane, as a rejection rather than a clip. A
             * proper clipper would split the triangle; this refuses it,
             * which is honest for a model that stays in front of the
             * camera and avoids dividing by a w at or through zero.
             */
            if (clip.w <= (G3D_ONE / 64)) { behind = 1; break; }

            const g3f ndc_x = g3d_div(clip.x, clip.w);
            const g3f ndc_y = g3d_div(clip.y, clip.w);

            sv[k].sx = (int32_t)(((int64_t)(ndc_x + G3D_ONE) * t->vw) >>
                                 (G3D_FP + 1));
            sv[k].sy = (int32_t)(((int64_t)(G3D_ONE - ndc_y) * t->vh) >>
                                 (G3D_FP + 1));
            sv[k].invw = g3d_div(G3D_ONE, clip.w);
            sv[k].normal = vb->normal
                ? g3d_normalise(g3d_transform_dir(&g3d_cb.model,
                                                  vb->normal[vi]))
                : fn;
            sv[k].behind = 0;
        }
        if (behind) { g3d_stats.tris_clipped++; continue; }

        /* Backface culling, from the sign of the screen-space area. */
        const int64_t area = g3d_edge(&sv[0], &sv[1], sv[2].sx, sv[2].sy);
        if (pipe->cull == G3D_CULL_BACK && area >= 0) {
            g3d_stats.tris_culled++;
            continue;
        }
        if (pipe->cull == G3D_CULL_FRONT && area <= 0) {
            g3d_stats.tris_culled++;
            continue;
        }

        g3sl_val_t in[G3D_N_INPUTS];
        in[0].v[0] = fn.x; in[0].v[1] = fn.y; in[0].v[2] = fn.z;
        in[0].type = G3SL_T_VEC3;
        for (int u = 0; u < G3D_U_COUNT && u + 1 < G3D_N_INPUTS; u++) {
            in[u + 1].v[0] = g3d_cb.uniform[u].x;
            in[u + 1].v[1] = g3d_cb.uniform[u].y;
            in[u + 1].v[2] = g3d_cb.uniform[u].z;
            in[u + 1].type = G3SL_T_VEC3;
        }

        g3d_raster(pipe, &sv[0], &sv[1], &sv[2], in, G3D_N_INPUTS);
        g3d_stats.tris_drawn++;
    }
}

/*
 * Submit the recorded frame.
 *
 * Everything happens here rather than at record time, which is what
 * makes this a command buffer and not a pile of function calls: the
 * backend sees the whole frame before any of it runs.
 */
static void g3d_submit(void) {
    g3d_cb.recording = 0;
    g3d_stats.frames++;

    for (int i = 0; i < g3d_cb.n; i++) {
        const g3d_cmd_t *c = &g3d_cb.cmd[i];
        switch (c->op) {
        case G3D_CMD_CLEAR:
            g3d_exec_clear(c->colour);
            break;
        case G3D_CMD_PIPELINE:
            g3d_cb.pipe = c->pipe;
            break;
        case G3D_CMD_MATRIX:
            if (c->slot == 0)      g3d_cb.model = c->mat;
            else if (c->slot == 1) g3d_cb.view = c->mat;
            else                   g3d_cb.proj = c->mat;
            break;
        case G3D_CMD_UNIFORM:
            if (c->slot < G3D_U_COUNT) g3d_cb.uniform[c->slot] = c->vec;
            break;
        case G3D_CMD_DRAW:
            g3d_exec_draw(g3d_cb.pipe, c->vb, c->ib);
            break;
        default:
            break;
        }
    }
    g3d_cb.n = 0;
}

static void g3d_init(void) {
    g3d_select_backend();
    g3d_cb.model = g3d_identity();
    g3d_cb.view = g3d_identity();
    g3d_cb.proj = g3d_identity();
    g3d_cb.n = 0;
    g3d_cb.recording = 0;
}

#endif /* VEXTRO_G3D_H */
