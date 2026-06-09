#ifndef LOGIN_H
#define LOGIN_H

#include <stdint.h>
#include "sincos_lut.h"

static inline uint32_t isqrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

static inline uint32_t clamp255(uint32_t v) { return v > 255 ? 255 : v; }

/* ===== GRAVITY WELL FLUID VORTEX ===== */
#define NUM_PARTICLES 2048
#define PARTICLE_FP   8

typedef struct {
    int32_t  x, y;
    int32_t  vx, vy;
    uint16_t angle;
    uint16_t orbit_r;
    uint8_t  bright;
    uint8_t  life;
} particle_t;

static particle_t particles[NUM_PARTICLES];
static uint32_t vtick = 0;
static uint32_t rng = 0xDEADBEEF;

static uint32_t xorshift32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void particle_spawn(particle_t *p, uint32_t w, uint32_t h) {
    uint32_t edge = xorshift32() & 3;
    if (edge == 0) {
        p->x = (int32_t)(xorshift32() % w) << PARTICLE_FP;
        p->y = 0;
    } else if (edge == 1) {
        p->x = (int32_t)(xorshift32() % w) << PARTICLE_FP;
        p->y = (int32_t)(h - 1) << PARTICLE_FP;
    } else if (edge == 2) {
        p->x = 0;
        p->y = (int32_t)(xorshift32() % h) << PARTICLE_FP;
    } else {
        p->x = (int32_t)(w - 1) << PARTICLE_FP;
        p->y = (int32_t)(xorshift32() % h) << PARTICLE_FP;
    }
    p->vx = (int32_t)(xorshift32() % 200) - 100;
    p->vy = (int32_t)(xorshift32() % 200) - 100;
    p->angle = (uint16_t)(xorshift32() % 360);
    p->orbit_r = (uint16_t)(20 + (xorshift32() % 180));
    p->bright = (uint8_t)(30 + (xorshift32() % 40));
    p->life = (uint8_t)(180 + (xorshift32() % 76));
}

static void particles_init(uint32_t w, uint32_t h) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particle_spawn(&particles[i], w, h);
        if (i & 1) {
            particles[i].x = (int32_t)(xorshift32() % w) << PARTICLE_FP;
            particles[i].y = (int32_t)(xorshift32() % h) << PARTICLE_FP;
        }
    }
}

static void particles_update(uint32_t w, uint32_t h, int32_t mx, int32_t my) {
    vtick++;

    for (int i = 0; i < NUM_PARTICLES; i++) {
        particle_t *p = &particles[i];

        int32_t px = p->x >> PARTICLE_FP;
        int32_t py = p->y >> PARTICLE_FP;

        int32_t dx = mx - px;
        int32_t dy = my - py;
        uint32_t dist = isqrt((uint32_t)(dx * dx + dy * dy));
        if (dist < 1) dist = 1;

        if (dist < 4) {
            particle_spawn(p, w, h);
            continue;
        }

        /* Inverse-square gravity toward mouse */
        uint32_t d_soft = dist + 6;
        int32_t grav = (int32_t)(500000 / (d_soft * d_soft));
        if (grav > 250) grav = 250;

        int32_t gx = (dx * grav) / (int32_t)dist;
        int32_t gy = (dy * grav) / (int32_t)dist;

        /* Tangential spin: perpendicular to gravity, faster when close */
        int32_t spin = 80 + (int32_t)(40000 / (dist + 15));
        int32_t tx = ((-dy) * spin) / ((int32_t)dist * 12);
        int32_t ty = (( dx) * spin) / ((int32_t)dist * 12);

        /* Archimedean spiral modulation */
        uint16_t phase = (uint16_t)((p->angle + (uint16_t)(vtick * 2)) % 360);
        int32_t osc = (int_sin[phase] * (int32_t)p->orbit_r) / (TRIG_SCALE * 6);

        uint16_t phase2 = (uint16_t)((p->angle * 3 + (uint16_t)(vtick * 5)) % 360);
        int32_t cross = (int_cos[phase2] * (int32_t)(p->orbit_r / 3)) / (TRIG_SCALE * 8);

        /* Combine with damping */
        p->vx = (p->vx * 245) / 256 + (gx + tx) / 5 + osc / 16 + cross / 16;
        p->vy = (p->vy * 245) / 256 + (gy + ty) / 5 + osc / 16 + cross / 16;

        if (p->vx >  900) p->vx =  900;
        if (p->vx < -900) p->vx = -900;
        if (p->vy >  900) p->vy =  900;
        if (p->vy < -900) p->vy = -900;

        p->x += p->vx;
        p->y += p->vy;

        int32_t wfp = (int32_t)w << PARTICLE_FP;
        int32_t hfp = (int32_t)h << PARTICLE_FP;
        if (p->x < 0 || p->x >= wfp || p->y < 0 || p->y >= hfp) {
            particle_spawn(p, w, h);
            continue;
        }

        p->angle = (uint16_t)((p->angle + 2 + 300 / (dist + 8)) % 360);

        /* Brightness: radial gradient from dim amber to white-hot */
        if (dist < 30)
            p->bright = (uint8_t)(235 + (xorshift32() % 21));
        else if (dist < 60)
            p->bright = (uint8_t)(190 + (uint8_t)(45 * (60 - dist) / 30));
        else if (dist < 140)
            p->bright = (uint8_t)(110 + (uint8_t)((int_sin[(vtick * 3 + i * 7) % 360] + TRIG_SCALE) * 40 / (TRIG_SCALE * 2)));
        else if (dist < 300)
            p->bright = (uint8_t)(55 + (uint8_t)((int_sin[(vtick * 2 + i * 11) % 360] + TRIG_SCALE) * 30 / (TRIG_SCALE * 2)));
        else
            p->bright = (uint8_t)(25 + (uint8_t)((int_sin[(vtick + i * 17) % 360] + TRIG_SCALE) * 20 / (TRIG_SCALE * 2)));

        p->life--;
        if (p->life == 0)
            particle_spawn(p, w, h);
    }
}

static inline void blend_add(uint32_t *buf, uint32_t idx,
                             uint32_t r, uint32_t g, uint32_t b) {
    uint32_t ex = buf[idx];
    uint32_t er = (ex >> 16) & 0xFF;
    uint32_t eg = (ex >> 8)  & 0xFF;
    uint32_t eb =  ex        & 0xFF;
    buf[idx] = (clamp255(er + r) << 16) |
               (clamp255(eg + g) << 8)  |
                clamp255(eb + b);
}

static void particles_draw(uint32_t *buf, uint32_t w, uint32_t h,
                           int32_t mx, int32_t my) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particle_t *p = &particles[i];
        int32_t px = p->x >> PARTICLE_FP;
        int32_t py = p->y >> PARTICLE_FP;

        if (px < 3 || py < 3 || px >= (int32_t)w - 3 || py >= (int32_t)h - 3)
            continue;

        uint32_t br = p->bright;

        /* Color temperature: dark amber -> gold -> bright gold -> white-hot */
        uint32_t r = clamp255((0xD4 * br) / 160 + (br > 200 ? (br - 200) * 2 : 0));
        uint32_t g = clamp255((0xAF * br) / 200 + (br > 180 ? (br - 180)     : 0));
        uint32_t b = clamp255((0x20 * br) / 200 + (br > 220 ? (br - 220) * 5 : 0));

        int32_t ddx = mx - px;
        int32_t ddy = my - py;
        uint32_t dist = isqrt((uint32_t)(ddx * ddx + ddy * ddy));

        int size;
        if      (dist < 25)  size = 4;
        else if (dist < 70)  size = 3;
        else if (dist < 180) size = 2;
        else                 size = 1;

        /* Core pixel */
        blend_add(buf, (uint32_t)py * w + (uint32_t)px, r, g, b);

        if (size >= 2) {
            uint32_t dr = r * 3 / 4, dg = g * 3 / 4, db = b * 3 / 4;
            blend_add(buf, (uint32_t)py * w + (uint32_t)(px + 1), dr, dg, db);
            blend_add(buf, (uint32_t)(py + 1) * w + (uint32_t)px, dr, dg, db);
            blend_add(buf, (uint32_t)(py + 1) * w + (uint32_t)(px + 1), dr, dg, db);
        }

        if (size >= 3) {
            uint32_t hr = r / 3, hg = g / 3, hb = b / 3;
            for (int hy = -1; hy <= 1; hy++)
                for (int hx = -1; hx <= 1; hx++) {
                    if (hx == 0 && hy == 0) continue;
                    blend_add(buf, (uint32_t)(py + hy) * w + (uint32_t)(px + hx),
                              hr, hg, hb);
                }
        }

        if (size >= 4) {
            uint32_t or_ = r / 6, og = g / 6, ob = b / 6;
            for (int gy = -2; gy <= 2; gy++)
                for (int gx = -2; gx <= 2; gx++) {
                    if (gy > -2 && gy < 2 && gx > -2 && gx < 2) continue;
                    blend_add(buf, (uint32_t)(py + gy) * w + (uint32_t)(px + gx),
                              or_, og, ob);
                }
        }
    }
}

/* Pixel-decay trail: fade every pixel toward black */
static void screen_fade(uint32_t *buf, uint32_t total) {
    for (uint32_t i = 0; i < total; i++) {
        uint32_t p = buf[i];
        if (p == 0) continue;
        uint32_t rb = ((p & 0xFF00FFu) * 240u >> 8) & 0xFF00FFu;
        uint32_t g  = ((p & 0x00FF00u) * 240u >> 8) & 0x00FF00u;
        buf[i] = rb | g;
    }
}

/* ===== LOGIN BOX INTERFACE ===== */
#define LOGIN_BOX_W  740
#define LOGIN_BOX_H  120
#define LOGIN_BORDER 2
#define GLOW_RADIUS  6

/* Show/Hide toggle button dimensions */
#define TOGGLE_W  58
#define TOGGLE_H  20
#define TOGGLE_PAD_R 16
#define TOGGLE_PAD_B 14

static int login_show_plain = 0;
static uint8_t login_prev_lmb = 0;

static void login_draw_box(uint32_t *buf, uint32_t w, uint32_t h,
                           const char *typed, uint32_t tick,
                           int32_t mx, int32_t my, uint8_t lmb,
                           const char *prompt_str) {
    int32_t bx = ((int32_t)w - LOGIN_BOX_W) / 2;
    int32_t by = ((int32_t)h - LOGIN_BOX_H) / 2;

    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    /* --- Toggle button hit test (rising edge of left click) --- */
    int32_t btn_x = bx + LOGIN_BOX_W - TOGGLE_PAD_R - TOGGLE_W;
    int32_t btn_y = by + LOGIN_BOX_H - TOGGLE_PAD_B - TOGGLE_H;

    int hover = (mx >= btn_x && mx < btn_x + TOGGLE_W &&
                 my >= btn_y && my < btn_y + TOGGLE_H);

    if (lmb && !login_prev_lmb && hover)
        login_show_plain ^= 1;
    login_prev_lmb = lmb;

    /* Outer glow: soft gold halo around the box */
    for (int32_t row = by - GLOW_RADIUS; row < by + LOGIN_BOX_H + GLOW_RADIUS; row++) {
        if (row < 0 || row >= (int32_t)h) continue;
        for (int32_t col = bx - GLOW_RADIUS; col < bx + LOGIN_BOX_W + GLOW_RADIUS; col++) {
            if (col < 0 || col >= (int32_t)w) continue;
            if (row >= by && row < by + LOGIN_BOX_H &&
                col >= bx && col < bx + LOGIN_BOX_W)
                continue;

            int32_t dx = 0, dy = 0;
            if (col < bx)                   dx = bx - col;
            if (col >= bx + LOGIN_BOX_W)    dx = col - (bx + LOGIN_BOX_W - 1);
            if (row < by)                   dy = by - row;
            if (row >= by + LOGIN_BOX_H)    dy = row - (by + LOGIN_BOX_H - 1);
            uint32_t dist = isqrt((uint32_t)(dx * dx + dy * dy));
            if (dist >= (uint32_t)GLOW_RADIUS) continue;

            uint16_t pulse_phase = (uint16_t)((tick * 3) % 360);
            int32_t pulse = (int_sin[pulse_phase] + TRIG_SCALE) / 2;
            uint32_t base_alpha = (uint32_t)((GLOW_RADIUS - (int32_t)dist) * 18);
            uint32_t alpha = (base_alpha * (uint32_t)(512 + pulse)) / (uint32_t)(TRIG_SCALE + 512);
            if (alpha > 255) alpha = 255;

            blend_add(buf, (uint32_t)row * w + (uint32_t)col,
                      (0xD4 * alpha) / 512,
                      (0xAF * alpha) / 512,
                      (0x37 * alpha) / 512);
        }
    }

    /* Box fill + border */
    for (int32_t row = by; row < by + LOGIN_BOX_H && row < (int32_t)h; row++) {
        for (int32_t col = bx; col < bx + LOGIN_BOX_W && col < (int32_t)w; col++) {
            int is_border = (row < by + LOGIN_BORDER ||
                             row >= by + LOGIN_BOX_H - LOGIN_BORDER ||
                             col < bx + LOGIN_BORDER ||
                             col >= bx + LOGIN_BOX_W - LOGIN_BORDER);
            if (is_border)
                buf[(uint32_t)row * w + (uint32_t)col] = 0xD4AF37u;
            else
                buf[(uint32_t)row * w + (uint32_t)col] = 0x080808u;
        }
    }

    /* Prompt text */
    int font_size = 20;
    int plen = 0;
    const char *t = prompt_str;
    while (*t++) plen++;
    int text_w = plen * (font_size * 6 / 10);
    int tx = bx + (LOGIN_BOX_W - text_w) / 2;
    int ty = by + 14;

    ttf_draw_string(buf, (int)w, (int)h, tx, ty, prompt_str, 0xD4AF37u, font_size);

    /* Separator line below prompt */
    int sep_y = by + 46;
    int sep_x0 = bx + 20;
    int sep_x1 = bx + LOGIN_BOX_W - 20;
    if (sep_y >= 0 && sep_y < (int32_t)h) {
        for (int32_t col = sep_x0; col < sep_x1 && col < (int32_t)w; col++)
            buf[(uint32_t)sep_y * w + (uint32_t)col] = 0x3A2F10u;
    }

    /* Input field */
    int input_y = by + 58;
    int input_size = 22;

    int tlen = 0;
    if (typed) { while (typed[tlen]) tlen++; }

    /* Build display string: plain text or asterisks */
    char display[130];
    if (login_show_plain) {
        for (int i = 0; i < tlen && i < 128; i++) display[i] = typed[i];
    } else {
        for (int i = 0; i < tlen && i < 128; i++) display[i] = '*';
    }
    display[tlen] = '\0';

    int input_w = tlen * (input_size * 6 / 10);
    int ix = bx + (LOGIN_BOX_W - input_w) / 2 - 6;
    if (ix < bx + 20) ix = bx + 20;

    if (tlen > 0)
        ttf_draw_string(buf, (int)w, (int)h, ix, input_y,
                        display, 0xFFFFFFu, input_size);

    /* Blinking caret */
    int caret_on = ((tick / 30) & 1) == 0;
    if (caret_on) {
        int cx = ix + input_w + 4;
        int cy = input_y + 2;
        int ch = input_size - 2;
        if (cx > bx + 4 && cx < bx + LOGIN_BOX_W - 6) {
            for (int r = 0; r < ch && (cy + r) < (int32_t)h; r++) {
                if (cy + r >= 0)
                    buf[(uint32_t)(cy + r) * w + (uint32_t)cx] = 0xD4AF37u;
            }
        }
    }

    /* --- Show/Hide toggle button --- */
    uint32_t btn_bg    = hover ? 0x1A1508u : 0x101010u;
    uint32_t btn_border = hover ? 0xE8C545u : 0x8A7228u;
    uint32_t btn_text  = hover ? 0xFFFFFFu : 0xD4AF37u;

    for (int32_t row = btn_y; row < btn_y + TOGGLE_H && row < (int32_t)h; row++) {
        for (int32_t col = btn_x; col < btn_x + TOGGLE_W && col < (int32_t)w; col++) {
            if (row < 0 || col < 0) continue;
            int is_edge = (row == btn_y || row == btn_y + TOGGLE_H - 1 ||
                           col == btn_x || col == btn_x + TOGGLE_W - 1);
            buf[(uint32_t)row * w + (uint32_t)col] = is_edge ? btn_border : btn_bg;
        }
    }

    const char *btn_label = login_show_plain ? "Hide" : "Show";
    int bl = 0;
    const char *bp = btn_label;
    while (*bp++) bl++;
    int btn_fs = 12;
    int btn_tw = bl * (btn_fs * 6 / 10);
    int btn_tx = btn_x + (TOGGLE_W - btn_tw) / 2;
    int btn_ty = btn_y + (TOGGLE_H - btn_fs) / 2 - 1;
    ttf_draw_string(buf, (int)w, (int)h, btn_tx, btn_ty,
                    btn_label, btn_text, btn_fs);
}

/* ===== SCREEN MELT DESTRUCTION ANIMATION ===== */
#define MELT_COLS 160

static int16_t melt_offsets[MELT_COLS];
static int melt_inited = 0;

static void melt_init(void) {
    for (int i = 0; i < MELT_COLS; i++)
        melt_offsets[i] = (int16_t)(xorshift32() % 40);
    melt_inited = 1;
}

static void screen_melt(uint32_t *buf, uint32_t w, uint32_t h, uint32_t tick) {
    if (!melt_inited) melt_init();

    uint32_t col_width = w / MELT_COLS;
    if (col_width < 1) col_width = 1;

    uint32_t passes = 3 + tick / 8;
    if (passes > 20) passes = 20;

    for (uint32_t pass = 0; pass < passes; pass++) {
        uint32_t ci = xorshift32() % MELT_COLS;
        uint32_t col_start = ci * col_width;
        uint32_t col_end = col_start + col_width;
        if (col_end > w) col_end = w;

        /* Accelerate this column's descent over time */
        int32_t speed = (int32_t)(melt_offsets[ci]) + (int32_t)(tick / 4);
        if (speed > (int32_t)h) speed = (int32_t)h;

        /* Shift pixels downward — cascade from bottom to avoid overwrite */
        int32_t drop = 2 + (int32_t)(xorshift32() % (uint32_t)(speed + 1));
        if (drop > (int32_t)h - 1) drop = (int32_t)h - 1;

        for (int32_t row = (int32_t)h - 1; row >= drop; row--) {
            for (uint32_t col = col_start; col < col_end; col++) {
                buf[(uint32_t)row * w + col] = buf[(uint32_t)(row - drop) * w + col];
            }
        }

        /* Leave dark smudge trail where pixels were pulled from */
        uint32_t smudge = 0x030201u;
        if (tick > 60) smudge = 0x000000u;
        for (int32_t row = 0; row < drop && row < (int32_t)h; row++) {
            for (uint32_t col = col_start; col < col_end; col++) {
                uint32_t existing = buf[(uint32_t)row * w + col];
                uint32_t er = (existing >> 16) & 0xFF;
                uint32_t eg = (existing >> 8)  & 0xFF;
                uint32_t eb =  existing        & 0xFF;
                er = er / 3;
                eg = eg / 3;
                eb = eb / 3;
                uint32_t sr = (smudge >> 16) & 0xFF;
                uint32_t sg = (smudge >> 8)  & 0xFF;
                uint32_t sb =  smudge        & 0xFF;
                buf[(uint32_t)row * w + col] = ((er + sr) << 16) |
                                               ((eg + sg) << 8)  |
                                                (eb + sb);
            }
        }

        /* Random pixel corruption: scatter dark artifacts */
        if (tick > 20) {
            for (uint32_t k = 0; k < 8; k++) {
                uint32_t rx = xorshift32() % w;
                uint32_t ry = xorshift32() % h;
                uint32_t idx = ry * w + rx;
                uint32_t p = buf[idx];
                buf[idx] = ((p >> 2) & 0x3F3F3Fu);
            }
        }
    }

    /* Global darken: entire screen progressively dims */
    if (tick > 40) {
        uint32_t decay = (tick > 90) ? 200u : 230u;
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t p = buf[i];
            if (p == 0) continue;
            uint32_t rb = ((p & 0xFF00FFu) * decay >> 8) & 0xFF00FFu;
            uint32_t g  = ((p & 0x00FF00u) * decay >> 8) & 0x00FF00u;
            buf[i] = rb | g;
        }
    }
}

/* ===== MAIN LOGIN RENDER (called each frame) ===== */
static int login_initialized = 0;

static void login_render(uint32_t *buf, uint32_t w, uint32_t h,
                         int32_t mx, int32_t my,
                         const char *typed, uint8_t buttons,
                         const char *prompt_str) {
    if (!login_initialized) {
        particles_init(w, h);
        login_initialized = 1;
    }

    screen_fade(buf, w * h);
    particles_update(w, h, mx, my);
    particles_draw(buf, w, h, mx, my);
    login_draw_box(buf, w, h, typed, vtick, mx, my, buttons & 1, prompt_str);
}

#endif /* LOGIN_H */
