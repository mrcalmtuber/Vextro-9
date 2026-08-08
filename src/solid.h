#ifndef VEXTRO_SOLID_H
#define VEXTRO_SOLID_H

#include "g3d.h"

/*
 * src/solid.h — the Solid app: a client of the g3d API.
 *
 * It is deliberately written the way a program outside the kernel would
 * have to write it -- build buffers, set state, record a command buffer,
 * submit -- so that the API is load-bearing rather than decorative. If
 * g3d only existed to serve one caller with one hard-coded path, it
 * would not be an API, and this is the thing that keeps it honest.
 *
 * The window shows what is usually invisible: which backend is live,
 * how many triangles survived culling, how many fragments were shaded,
 * and the shader source itself with its compile status. A shader that
 * fails to compile reports its line and message here rather than
 * silently drawing nothing.
 */

/* ===== meshes ===== */

#define SU (G3D_ONE)              /* one unit */

static const g3d_vec3 solid_cube_v[8] = {
    { -SU, -SU, -SU }, {  SU, -SU, -SU }, {  SU,  SU, -SU }, { -SU,  SU, -SU },
    { -SU, -SU,  SU }, {  SU, -SU,  SU }, {  SU,  SU,  SU }, { -SU,  SU,  SU },
};

/* Wound counter-clockwise seen from outside, so backface culling can
 * use the sign of the screen-space area and nothing else. */
static const uint16_t solid_cube_i[36] = {
    0, 2, 1,  0, 3, 2,      /* back   */
    4, 5, 6,  4, 6, 7,      /* front  */
    0, 4, 7,  0, 7, 3,      /* left   */
    1, 2, 6,  1, 6, 5,      /* right  */
    3, 7, 6,  3, 6, 2,      /* top    */
    0, 1, 5,  0, 5, 4,      /* bottom */
};

#define SO (G3D_ONE * 3 / 2)

static const g3d_vec3 solid_oct_v[6] = {
    {  SO, 0, 0 }, { -SO, 0, 0 },
    { 0,  SO, 0 }, { 0, -SO, 0 },
    { 0, 0,  SO }, { 0, 0, -SO },
};

static const uint16_t solid_oct_i[24] = {
    0, 2, 4,  2, 1, 4,  1, 3, 4,  3, 0, 4,
    2, 0, 5,  1, 2, 5,  3, 1, 5,  0, 3, 5,
};

/*
 * The octahedron carries vertex normals, and the cube does not. That is
 * not an oversight in either direction: a cube's faces are flat, and
 * averaging normals at its corners would round off the edges that make
 * it a cube. Per-vertex normals on the octahedron point straight out
 * from the centre, so per-pixel shading rounds it into a sphere -- which
 * is the visible difference between the two shading modes.
 */
static const g3d_vec3 solid_oct_n[6] = {
    {  G3D_ONE, 0, 0 }, { -G3D_ONE, 0, 0 },
    { 0,  G3D_ONE, 0 }, { 0, -G3D_ONE, 0 },
    { 0, 0,  G3D_ONE }, { 0, 0, -G3D_ONE },
};

static const g3d_vbuf_t solid_cube_vb = { solid_cube_v, 0, 8 };
static const g3d_ibuf_t solid_cube_ib = { solid_cube_i, 36 };
static const g3d_vbuf_t solid_oct_vb  = { solid_oct_v, solid_oct_n, 6 };
static const g3d_ibuf_t solid_oct_ib  = { solid_oct_i, 24 };

/* ===== shaders ===== */

/*
 * Two programs, compiled from this text at run time. The second one
 * exists to be visibly different, so that "the shader is running" is
 * something the eye can confirm rather than something the code claims.
 */
static const char *const solid_shader_src[2] = {
    "# lambert, with a rim light on the silhouette\n"
    "d   = sat(dot(n, l));\n"
    "rim = sat(1.0 - dot(n, e));\n"
    "color = base * (0.18 + 0.82 * d) + gold * (rim * rim * 0.55);\n",

    "# banded, to show the shader is really running per fragment\n"
    "d = sat(dot(n, l));\n"
    "b = sat(d * 3.0);\n"
    "s = max(max(b - 0.66, 0.0) * 3.0, max(b - 0.33, 0.0));\n"
    "color = mix(base * 0.25, gold, sat(s));\n",
};

static const char *const solid_shader_name[2] = { "lambert + rim", "banded" };

static g3sl_prog_t solid_prog;
static int  solid_shader = 0;
static int  solid_compiled = -1;      /* which source is in solid_prog */

static void solid_compile(void) {
    g3sl_compile(&solid_prog, solid_shader_src[solid_shader],
                 g3d_in_names, g3d_in_types, G3D_N_INPUTS);
    solid_compiled = solid_shader;
}

/* ===== app state ===== */

static int solid_shape = 0;          /* 0 cube, 1 octahedron */
static int solid_spin  = 1;
static int solid_ry = 30, solid_rx = 20;
static int solid_dist_units = 5;
static int solid_pixel = 0;          /* per-pixel shading */
static int solid_ready = 0;

static g3d_pipeline_t solid_pipe = {
    .depth_test = 1, .depth_write = 1,
    .cull = G3D_CULL_BACK, .shade = G3D_SHADE_FLAT,
    .fs = &solid_prog,
};

static void solid_init(void) {
    if (solid_ready) return;
    g3d_init();
    solid_compile();
    solid_ready = 1;
}

/* ===== the window ===== */

#define SOLID_PANEL 132      /* the status strip down the right side */

static void solid_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       int32_t mx, int32_t my) {
    (void)mx; (void)my;
    solid_init();
    if (solid_compiled != solid_shader) solid_compile();

    int32_t vw = cw - SOLID_PANEL;
    int32_t vh = chh;
    if (vw > G3D_MAX_W) vw = G3D_MAX_W;
    if (vh > G3D_MAX_H) vh = G3D_MAX_H;
    if (vw < 16 || vh < 16) return;

    if (solid_spin) {
        solid_ry = (solid_ry + 1) % 360;
        if ((solid_ry % 3) == 0) solid_rx = (solid_rx + 1) % 360;
    }

    /* ---- record a frame ---- */
    g3d_target_t target;
    target.color = buf;
    target.w = (int32_t)w;
    target.h = (int32_t)h;
    target.vx = cx;
    target.vy = cy;
    target.vw = vw;
    target.vh = vh;
    target.screen = 0;        /* a window back-buffer, not the scanout */

    g3d_begin(&target);
    g3d_cmd_clear(0x0C1018u);

    solid_pipe.shade = solid_pixel ? G3D_SHADE_PIXEL : G3D_SHADE_FLAT;
    g3d_cmd_pipeline(&solid_pipe);

    {
        g3d_mat4 ry = g3d_rotate_y(solid_ry);
        g3d_mat4 rx = g3d_rotate_x(solid_rx);
        g3d_mat4 model = g3d_mat_mul(&ry, &rx);
        g3d_cmd_matrix(0, &model);

        g3d_mat4 view = g3d_translate(0, 0, -(g3f)solid_dist_units * G3D_ONE);
        g3d_cmd_matrix(1, &view);

        const g3f aspect = g3d_div(vw * G3D_ONE, vh * G3D_ONE);
        g3d_mat4 proj = g3d_perspective(60, aspect, G3D_ONE / 2,
                                        G3D_ONE * 40);
        g3d_cmd_matrix(2, &proj);
    }

    g3d_cmd_uniform(G3D_U_LIGHT,
                    g3d_normalise(g3d_v3(G3D_ONE / 2, G3D_ONE, G3D_ONE * 3 / 4)));
    g3d_cmd_uniform(G3D_U_BASE, g3d_v3(G3D_ONE / 5, G3D_ONE * 2 / 5,
                                       G3D_ONE * 3 / 5));
    g3d_cmd_uniform(G3D_U_ACCENT, g3d_v3((g3f)(0.85 * G3D_ONE),
                                         (g3f)(0.72 * G3D_ONE),
                                         (g3f)(0.35 * G3D_ONE)));
    g3d_cmd_uniform(G3D_U_EYE, g3d_v3(0, 0, G3D_ONE));

    if (solid_shape == 0) g3d_cmd_draw(&solid_cube_vb, &solid_cube_ib);
    else                  g3d_cmd_draw(&solid_oct_vb, &solid_oct_ib);

    const uint32_t frag0 = g3d_stats.fragments;
    const uint32_t run0 = g3d_stats.shader_runs;
    const uint32_t drawn0 = g3d_stats.tris_drawn;
    const uint32_t cull0 = g3d_stats.tris_culled;

    g3d_submit();

    /* ---- the status strip ---- */
    const int32_t px = cx + vw;
    gfx_rect(buf, w, h, px, cy, cw - vw, chh, 0x14171Fu);
    gfx_rect(buf, w, h, px, cy, 1, chh, 0x2A3040u);

    int32_t ty = cy + 8;
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, "g3d", C_GOLD, 15);
    ty += 20;

    char line[64], nb[16];

    str_copy(line, "backend", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT_DIM, 10);
    ty += 12;
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, g3d_backend_name(),
                    C_TEXT, 11);
    ty += 18;

    str_copy(line, "shader", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT_DIM, 10);
    ty += 12;
    if (solid_prog.ok) {
        ttf_draw_string(buf, (int)w, (int)h, px + 10, ty,
                        solid_shader_name[solid_shader], C_TEXT, 11);
        ty += 14;
        uint_to_str((uint32_t)solid_prog.ncode, nb);
        str_copy(line, nb, sizeof(line));
        str_append(line, " ops, ", sizeof(line));
        uint_to_str((uint32_t)solid_prog.nvars, nb);
        str_append(line, nb, sizeof(line));
        str_append(line, " regs", sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT_DIM, 10);
    } else {
        str_copy(line, "line ", sizeof(line));
        uint_to_str((uint32_t)solid_prog.err_line, nb);
        str_append(line, nb, sizeof(line));
        ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, 0xC06060u, 11);
        ty += 14;
        ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, solid_prog.err,
                        0xC06060u, 10);
    }
    ty += 22;

    str_copy(line, "this frame", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT_DIM, 10);
    ty += 13;

    uint_to_str(g3d_stats.tris_drawn - drawn0, nb);
    str_copy(line, nb, sizeof(line));
    str_append(line, " tris, ", sizeof(line));
    uint_to_str(g3d_stats.tris_culled - cull0, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " culled", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT, 10);
    ty += 13;

    uint_to_str(g3d_stats.fragments - frag0, nb);
    str_copy(line, nb, sizeof(line));
    str_append(line, " fragments", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT, 10);
    ty += 13;

    uint_to_str(g3d_stats.shader_runs - run0, nb);
    str_copy(line, nb, sizeof(line));
    str_append(line, " shader runs", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT, 10);
    ty += 20;

    str_copy(line, "clears: ", sizeof(line));
    uint_to_str(g3d_stats.gpu_ops, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " gpu / ", sizeof(line));
    uint_to_str(g3d_stats.cpu_ops, nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " cpu", sizeof(line));
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, line, C_TEXT_DIM, 10);
    ty += 24;

    /* controls */
    ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, "keys", C_TEXT_DIM, 10);
    ty += 13;
    const char *const keys[] = {
        "space  spin", "s  shape", "f  shader", "p  per-pixel",
        "+ -  distance",
    };
    for (int i = 0; i < 5; i++) {
        ttf_draw_string(buf, (int)w, (int)h, px + 10, ty, keys[i],
                        C_TEXT_DIM, 10);
        ty += 12;
    }
}

static void solid_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    (void)cy; (void)chh;
    if (lmb && !prev_lmb && mx >= cx && mx < cx + cw - SOLID_PANEL)
        solid_spin = !solid_spin;
    (void)my;
}

static void solid_key(char ch) {
    if (ch == ' ')                     solid_spin = !solid_spin;
    else if (ch == 's' || ch == 'S')   solid_shape = !solid_shape;
    else if (ch == 'f' || ch == 'F')   solid_shader = !solid_shader;
    else if (ch == 'p' || ch == 'P')   solid_pixel = !solid_pixel;
    else if (ch == '+' || ch == '=')   { if (solid_dist_units > 3) solid_dist_units--; }
    else if (ch == '-' || ch == '_')   { if (solid_dist_units < 14) solid_dist_units++; }
    else if (ch == KEY_LEFT)           solid_ry = (solid_ry + 355) % 360;
    else if (ch == KEY_RIGHT)          solid_ry = (solid_ry + 5) % 360;
    else if (ch == KEY_UP)             solid_rx = (solid_rx + 355) % 360;
    else if (ch == KEY_DOWN)           solid_rx = (solid_rx + 5) % 360;
}

#endif /* VEXTRO_SOLID_H */
