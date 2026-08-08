#ifndef VEXTRO_V3D_H
#define VEXTRO_V3D_H

/*
 * src/v3d.h — a software 3D rasteriser.
 *
 * Not a graphics API and not a driver: there is no shader compiler, no
 * command buffer and no GPU pipeline behind this, and the README says so
 * in those words. It is a triangle rasteriser that runs on the CPU, and
 * everything in it is integer arithmetic, because this kernel is built
 * with -mno-80387 -mno-sse and a single float would fail to link.
 *
 * Which is the interesting constraint, and it decides the whole design:
 *
 *   Positions are 16.16 fixed point. A cube two units wide is 131072.
 *
 *   Rotation comes from the same 360-entry integer sine table the rest of
 *   the system uses, at TRIG_SCALE 1024, so a rotate is a multiply and a
 *   shift.
 *
 *   The perspective divide is the one place a divide is unavoidable, and
 *   it is done once per vertex rather than once per pixel -- which is
 *   what makes the inner loop pure addition.
 *
 *   Depth is compared as 1/z in fixed point rather than z, so nearer is
 *   larger and the test is a plain >. Storing z itself would need a
 *   divide per pixel to interpolate correctly.
 */

#define V3D_FP      16                     /* fractional bits */
#define V3D_ONE     (1 << V3D_FP)

typedef int32_t v3d_fx;

typedef struct { v3d_fx x, y, z; } v3d_vec;

typedef struct {
    uint16_t a, b, c;      /* vertex indices */
    uint32_t colour;
} v3d_tri;

typedef struct {
    const v3d_vec *verts;
    int            nvert;
    const v3d_tri *tris;
    int            ntri;
} v3d_mesh;

/*
 * The depth buffer is sized for the largest window a mesh is drawn into,
 * not the screen: this is an application-level rasteriser and nothing
 * here composites full screen.
 */
#define V3D_MAX_W 640
#define V3D_MAX_H 480
static int32_t v3d_depth[V3D_MAX_W * V3D_MAX_H];

/* --- fixed-point helpers --- */

static inline v3d_fx v3d_mul(v3d_fx a, v3d_fx b) {
    return (v3d_fx)(((int64_t)a * (int64_t)b) >> V3D_FP);
}

static inline v3d_fx v3d_div(v3d_fx a, v3d_fx b) {
    if (b == 0) return 0;
    return (v3d_fx)((((int64_t)a) << V3D_FP) / b);
}

/* Sine and cosine of whole degrees, in 16.16. The table is 1024-scaled,
 * so shifting up by six turns 1024 into 65536 exactly. */
static inline v3d_fx v3d_sin(int deg) {
    int d = deg % 360; if (d < 0) d += 360;
    return (v3d_fx)int_sin[d] << (V3D_FP - 10);
}
static inline v3d_fx v3d_cos(int deg) {
    int d = deg % 360; if (d < 0) d += 360;
    return (v3d_fx)int_cos[d] << (V3D_FP - 10);
}

/* --- the pipeline --- */

typedef struct {
    int32_t sx, sy;        /* screen pixels */
    v3d_fx  invz;          /* 1/z, for the depth test */
    v3d_vec rot;           /* position after rotation, before projection */
    int     behind;        /* clipped: at or behind the eye */
} v3d_pt;

/*
 * Integer square root, by the bit-by-bit method: no FPU, no iteration
 * that might not converge, and exact for every input. Needed to
 * normalise a face normal -- the alternative is comparing squared
 * quantities, which cannot be done here because the dot product and the
 * magnitude have different dimensions.
 */
static uint32_t v3d_isqrt(uint64_t n) {
    uint64_t rem = 0, root = 0;
    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 62);
        n <<= 2;
        if (root < rem) { rem -= root | 1; root += 2; }
    }
    return (uint32_t)(root >> 1);
}

/*
 * Rotate about Y then X, translate away from the eye, and project.
 *
 * The near plane is a real test, not a clamp. A vertex at or behind the
 * eye has no projection at all, and letting it through produces a point
 * mirrored through the origin -- a triangle that flips inside out and
 * smears across the screen. Whole triangles with any vertex behind the
 * plane are dropped; this is a demo rasteriser and clipping a triangle
 * into two is more machinery than the picture needs.
 */
static void v3d_project(const v3d_vec *v, int ry, int rx, v3d_fx dist,
                        int32_t cx, int32_t cy, v3d_fx scale, v3d_pt *out) {
    const v3d_fx sy = v3d_sin(ry), cy_ = v3d_cos(ry);
    const v3d_fx sx = v3d_sin(rx), cx_ = v3d_cos(rx);

    /* rotate around Y */
    v3d_fx x1 =  v3d_mul(v->x, cy_) + v3d_mul(v->z, sy);
    v3d_fx z1 = -v3d_mul(v->x, sy)  + v3d_mul(v->z, cy_);
    v3d_fx y1 =  v->y;

    /* then around X */
    v3d_fx y2 = v3d_mul(y1, cx_) - v3d_mul(z1, sx);
    v3d_fx z2 = v3d_mul(y1, sx)  + v3d_mul(z1, cx_);

    out->rot.x = x1; out->rot.y = y2; out->rot.z = z2;

    z2 += dist;

    const v3d_fx near = V3D_ONE / 4;
    if (z2 <= near) { out->behind = 1; out->sx = out->sy = 0; out->invz = 0; return; }
    out->behind = 0;

    /* one divide per vertex, and none per pixel */
    const v3d_fx inv = v3d_div(V3D_ONE, z2);
    out->invz = inv;
    out->sx = cx + (int32_t)(v3d_mul(v3d_mul(x1, inv), scale) >> V3D_FP);
    out->sy = cy - (int32_t)(v3d_mul(v3d_mul(y2, inv), scale) >> V3D_FP);
}

/* Signed area of the screen-space triangle, doubled. Sign gives winding,
 * which is the backface test; magnitude is the barycentric denominator. */
static inline int64_t v3d_area2(const v3d_pt *a, const v3d_pt *b,
                                const v3d_pt *c) {
    return (int64_t)(b->sx - a->sx) * (c->sy - a->sy) -
           (int64_t)(b->sy - a->sy) * (c->sx - a->sx);
}

static void v3d_clear_depth(int32_t w, int32_t h) {
    const int32_t n = (w > V3D_MAX_W ? V3D_MAX_W : w) *
                      (h > V3D_MAX_H ? V3D_MAX_H : h);
    for (int32_t i = 0; i < n; i++) v3d_depth[i] = 0;   /* 1/z = 0 is infinity */
}

/*
 * Fill one triangle with a depth test.
 *
 * Edge functions are evaluated once at the top-left of the bounding box
 * and then stepped by a constant per pixel, so the inner loop is three
 * adds and a compare. That is the whole reason to compute them this way
 * rather than testing barycentrics from scratch at every pixel.
 */
static void v3d_triangle(uint32_t *buf, uint32_t bw, uint32_t bh,
                         int32_t clip_x, int32_t clip_y,
                         int32_t clip_w, int32_t clip_h,
                         const v3d_pt *a, const v3d_pt *b, const v3d_pt *c,
                         uint32_t colour) {
    if (a->behind || b->behind || c->behind) return;

    const int64_t area = v3d_area2(a, b, c);
    if (area <= 0) return;                  /* backface, or degenerate */

    int32_t minx = a->sx < b->sx ? (a->sx < c->sx ? a->sx : c->sx)
                                 : (b->sx < c->sx ? b->sx : c->sx);
    int32_t maxx = a->sx > b->sx ? (a->sx > c->sx ? a->sx : c->sx)
                                 : (b->sx > c->sx ? b->sx : c->sx);
    int32_t miny = a->sy < b->sy ? (a->sy < c->sy ? a->sy : c->sy)
                                 : (b->sy < c->sy ? b->sy : c->sy);
    int32_t maxy = a->sy > b->sy ? (a->sy > c->sy ? a->sy : c->sy)
                                 : (b->sy > c->sy ? b->sy : c->sy);

    if (minx < clip_x) minx = clip_x;
    if (miny < clip_y) miny = clip_y;
    if (maxx > clip_x + clip_w - 1) maxx = clip_x + clip_w - 1;
    if (maxy > clip_y + clip_h - 1) maxy = clip_y + clip_h - 1;
    if (minx > maxx || miny > maxy) return;

    /* Edge steps: how each edge function changes per pixel of x and y. */
    const int32_t A01 = a->sy - b->sy, B01 = b->sx - a->sx;
    const int32_t A12 = b->sy - c->sy, B12 = c->sx - b->sx;
    const int32_t A20 = c->sy - a->sy, B20 = a->sx - c->sx;

    int64_t w0_row = (int64_t)(c->sx - b->sx) * (miny - b->sy) -
                     (int64_t)(c->sy - b->sy) * (minx - b->sx);
    int64_t w1_row = (int64_t)(a->sx - c->sx) * (miny - c->sy) -
                     (int64_t)(a->sy - c->sy) * (minx - c->sx);
    int64_t w2_row = (int64_t)(b->sx - a->sx) * (miny - a->sy) -
                     (int64_t)(b->sy - a->sy) * (minx - a->sx);

    for (int32_t y = miny; y <= maxy; y++) {
        int64_t w0 = w0_row, w1 = w1_row, w2 = w2_row;
        for (int32_t x = minx; x <= maxx; x++) {
            if ((w0 | w1 | w2) >= 0) {
                /* Interpolate 1/z, which is linear in screen space --
                 * z itself is not, which is why the buffer holds 1/z. */
                const int64_t iz = ((int64_t)a->invz * w0 +
                                    (int64_t)b->invz * w1 +
                                    (int64_t)c->invz * w2) / area;
                const int32_t di = (y - clip_y) * (clip_w > V3D_MAX_W
                                                   ? V3D_MAX_W : clip_w)
                                 + (x - clip_x);
                if (di >= 0 && di < V3D_MAX_W * V3D_MAX_H &&
                    (int32_t)iz > v3d_depth[di]) {
                    v3d_depth[di] = (int32_t)iz;
                    if ((uint32_t)x < bw && (uint32_t)y < bh)
                        buf[(uint32_t)y * bw + (uint32_t)x] = colour;
                }
            }
            w0 += A12; w1 += A20; w2 += A01;
        }
        w0_row += B12; w1_row += B20; w2_row += B01;
    }
}

/*
 * Flat shading from the face normal.
 *
 * The normal is the cross product of two edges of the triangle in
 * rotated object space -- not screen space. That distinction matters:
 * a screen-space normal is cheap but its magnitude depends on how large
 * the triangle happens to appear, so faces darken as they shrink instead
 * of as they turn away. This darkens them only when they turn.
 *
 * The light is fixed at the eye, so the shade is the z component of the
 * normalised normal. One square root per triangle, not per pixel.
 */
static uint32_t v3d_shade(uint32_t base, const v3d_pt *a, const v3d_pt *b,
                          const v3d_pt *c) {
    const int64_t ux = b->rot.x - a->rot.x, uy = b->rot.y - a->rot.y,
                  uz = b->rot.z - a->rot.z;
    const int64_t vx = c->rot.x - a->rot.x, vy = c->rot.y - a->rot.y,
                  vz = c->rot.z - a->rot.z;

    /* Shifted down before multiplying: these are 16.16 values and their
     * products would otherwise need more than 64 bits on a large mesh. */
    const int64_t nx = ((uy >> 8) * (vz >> 8)) - ((uz >> 8) * (vy >> 8));
    const int64_t ny = ((uz >> 8) * (vx >> 8)) - ((ux >> 8) * (vz >> 8));
    const int64_t nz = ((ux >> 8) * (vy >> 8)) - ((uy >> 8) * (vx >> 8));

    const uint64_t sq = (uint64_t)(nx * nx + ny * ny + nz * nz);
    const uint32_t len = v3d_isqrt(sq);
    if (!len) return base;

    /* Facing the eye is -z here, because the projection looks down +z. */
    int64_t d = (-nz * 255) / (int64_t)len;
    if (d < 0) d = -d;                 /* two-sided: never a black face */
    if (d > 255) d = 255;

    /* Floor at 70 so a face turned nearly edge-on is still a face and not
     * a hole in the object. */
    uint32_t lit = (uint32_t)(70 + (d * 185) / 255);
    return gfx_mix(base, 0x000000u, lit);
}

static void v3d_draw_mesh(uint32_t *buf, uint32_t bw, uint32_t bh,
                          int32_t cx0, int32_t cy0, int32_t cw, int32_t chh,
                          const v3d_mesh *m, int ry, int rx,
                          v3d_fx dist, v3d_fx scale) {
    static v3d_pt pts[256];
    if (m->nvert > 256) return;

    v3d_clear_depth(cw, chh);

    const int32_t cx = cx0 + cw / 2, cy = cy0 + chh / 2;
    for (int i = 0; i < m->nvert; i++)
        v3d_project(&m->verts[i], ry, rx, dist, cx, cy, scale, &pts[i]);

    for (int i = 0; i < m->ntri; i++) {
        const v3d_tri *t = &m->tris[i];
        const v3d_pt *a = &pts[t->a], *b = &pts[t->b], *c = &pts[t->c];
        if (v3d_area2(a, b, c) <= 0) continue;
        v3d_triangle(buf, bw, bh, cx0, cy0, cw, chh, a, b, c,
                     v3d_shade(t->colour, a, b, c));
    }
}


/* ===== the demo window =====
 *
 * A cube and an octahedron, because two solids with different face counts
 * exercise the depth test against each other in a way one convex solid
 * never can: a single convex object looks correct even with the depth
 * test switched off, since backface culling alone sorts it.
 */

#define V3D_C (V3D_ONE)          /* cube half-extent: one unit */

static const v3d_vec v3d_cube_v[8] = {
    { -V3D_C, -V3D_C, -V3D_C }, {  V3D_C, -V3D_C, -V3D_C },
    {  V3D_C,  V3D_C, -V3D_C }, { -V3D_C,  V3D_C, -V3D_C },
    { -V3D_C, -V3D_C,  V3D_C }, {  V3D_C, -V3D_C,  V3D_C },
    {  V3D_C,  V3D_C,  V3D_C }, { -V3D_C,  V3D_C,  V3D_C },
};

/* Wound counter-clockwise seen from outside, which is what makes the
 * backface test a sign check on the screen-space area. */
static const v3d_tri v3d_cube_t[12] = {
    { 0, 2, 1, 0xD4AF37u }, { 0, 3, 2, 0xD4AF37u },   /* back   */
    { 4, 5, 6, 0xC89B2Fu }, { 4, 6, 7, 0xC89B2Fu },   /* front  */
    { 0, 4, 7, 0xB8860Bu }, { 0, 7, 3, 0xB8860Bu },   /* left   */
    { 1, 2, 6, 0xE8C55Fu }, { 1, 6, 5, 0xE8C55Fu },   /* right  */
    { 3, 7, 6, 0xF0D68Au }, { 3, 6, 2, 0xF0D68Au },   /* top    */
    { 0, 1, 5, 0x8B6914u }, { 0, 5, 4, 0x8B6914u },   /* bottom */
};

static const v3d_mesh v3d_cube = { v3d_cube_v, 8, v3d_cube_t, 12 };

#define V3D_O (V3D_ONE * 3 / 2)

static const v3d_vec v3d_oct_v[6] = {
    {  V3D_O, 0, 0 }, { -V3D_O, 0, 0 },
    { 0,  V3D_O, 0 }, { 0, -V3D_O, 0 },
    { 0, 0,  V3D_O }, { 0, 0, -V3D_O },
};

static const v3d_tri v3d_oct_t[8] = {
    { 0, 2, 4, 0x4A9BD4u }, { 2, 1, 4, 0x3E86BAu },
    { 1, 3, 4, 0x5AAEE8u }, { 3, 0, 4, 0x2F6E99u },
    { 2, 0, 5, 0x4A9BD4u }, { 1, 2, 5, 0x3E86BAu },
    { 3, 1, 5, 0x5AAEE8u }, { 0, 3, 5, 0x2F6E99u },
};

static const v3d_mesh v3d_oct = { v3d_oct_v, 6, v3d_oct_t, 8 };

static int v3d_shape = 0;        /* 0 cube, 1 octahedron */
static int v3d_spin  = 1;
static int v3d_ry = 30, v3d_rx = 20;
static int v3d_dist_units = 5;

static void v3d_app_draw(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                         int32_t mx, int32_t my) {
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x0B0E14u);

    if (v3d_spin) { v3d_ry = (v3d_ry + 2) % 360; v3d_rx = (v3d_rx + 1) % 360; }

    const int32_t vy = cy + 34;
    const int32_t vh = chh - 34;
    /* Scaled by the viewing distance so the solid keeps its apparent
     * size as the camera moves: at distance d, a unit at the origin
     * projects to scale/d pixels, so scale must carry d to stay put. */
    /* /6, not /3: a unit cube's longest diagonal is sqrt(3) units, so
     * a solid scaled to a third of the viewport spans most of it when a
     * corner turns towards the camera and one face then fills the view. */
    const int32_t base_px = (cw < vh ? cw : vh) / 6;
    const v3d_fx scale = (v3d_fx)(base_px * v3d_dist_units) << V3D_FP;

    v3d_draw_mesh(buf, w, h, cx, vy, cw, vh,
                  v3d_shape ? &v3d_oct : &v3d_cube,
                  v3d_ry, v3d_rx,
                  (v3d_fx)v3d_dist_units << V3D_FP, scale);

    /* chrome last, so it is never depth-tested against the model */
    gfx_rect(buf, w, h, cx, cy, cw, 34, 0x14171Fu);
    gfx_rect(buf, w, h, cx, cy + 33, cw, 1, 0x2A3142u);

    static const char *const btn[3] = { "Shape", "Spin", "Reset" };
    for (int i = 0; i < 3; i++) {
        const int32_t bx = cx + 10 + i * 62;
        const int hot = mx >= bx && mx < bx + 56 && my >= cy + 5 && my < cy + 29;
        gfx_rect(buf, w, h, bx, cy + 5, 56, 24, hot ? 0x2A2410u : 0x1C2130u);
        gfx_rect_outline(buf, w, h, bx, cy + 5, 56, 24, hot ? C_GOLD : 0x2A3142u);
        const int tw = ttf_text_width(btn[i], 11);
        ttf_draw_string(buf, (int)w, (int)h, bx + (56 - tw) / 2, cy + 10,
                        btn[i], hot ? C_GOLD : C_TEXT, 11);
    }

    char line[64], nb[12];
    str_copy(line, v3d_shape ? "octahedron  " : "cube  ", sizeof(line));
    uint_to_str((uint32_t)(v3d_shape ? 8 : 12), nb);
    str_append(line, nb, sizeof(line));
    str_append(line, " triangles, software, no FPU", sizeof(line));
    ttf_draw_string_clip(buf, (int)w, (int)h, cx + 200, cy + 11, line,
                         C_TEXT_DIM, 11, cx + cw - 10);
}

static void v3d_app_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                          int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    (void)cw; (void)chh;
    if (!(lmb && !prev_lmb)) return;
    for (int i = 0; i < 3; i++) {
        const int32_t bx = cx + 10 + i * 62;
        if (mx >= bx && mx < bx + 56 && my >= cy + 5 && my < cy + 29) {
            if (i == 0) v3d_shape = !v3d_shape;
            else if (i == 1) v3d_spin = !v3d_spin;
            else { v3d_ry = 30; v3d_rx = 20; v3d_dist_units = 5; }
            return;
        }
    }
}

static void v3d_app_key(char ch) {
    switch (ch) {
    case KEY_LEFT:  v3d_ry = (v3d_ry + 355) % 360; v3d_spin = 0; break;
    case KEY_RIGHT: v3d_ry = (v3d_ry + 5) % 360;   v3d_spin = 0; break;
    case KEY_UP:    v3d_rx = (v3d_rx + 355) % 360; v3d_spin = 0; break;
    case KEY_DOWN:  v3d_rx = (v3d_rx + 5) % 360;   v3d_spin = 0; break;
    case ' ':       v3d_spin = !v3d_spin; break;
    case '\t':      v3d_shape = !v3d_shape; break;
    case '+': case '=': if (v3d_dist_units > 3) v3d_dist_units--; break;
    case '-':           if (v3d_dist_units < 20) v3d_dist_units++; break;
    default: break;
    }
}

#endif /* VEXTRO_V3D_H */
