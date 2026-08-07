/*
 * voronoi — nearest-site partition of the canvas, for Vextro 9.
 *
 * This one is deliberately absent from the disk-seeded repository: it
 * only exists in the network repository, so installing it exercises the
 * store's HTTP download path end to end.
 *
 * Squared distances are compared directly, which keeps the whole thing
 * on 32-bit integers with no square root anywhere.
 */
#include "../vextro.h"

#define SITES 28

static int site_x[SITES];
static int site_y[SITES];
static int site_c[SITES];

static int prev_row[OS_CANVAS_W];

static uint32_t lcg_state = 0x5EEDF00Du;

static uint32_t lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state >> 8;
}

static const uint32_t base_color[8] = {
    0xD4AF37u, 0x4E93C8u, 0xC8664Eu, 0x5FA87Au,
    0x9A6FC0u, 0xC9A24Bu, 0x4EA0A0u, 0xB0526Fu,
};

static uint32_t scale_color(uint32_t c, int num, int den) {
    if (den <= 0) den = 1;
    if (num > den) num = den;
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0xFF) * (uint32_t)num / (uint32_t)den;
    return (r << 16) | (g << 8) | b;
}

void _start(void) {
    os_print("voronoi: 28-site nearest-neighbour partition\n");

    for (int i = 0; i < SITES; i++) {
        site_x[i] = 12 + (int)(lcg() % (OS_CANVAS_W - 24));
        site_y[i] = 12 + (int)(lcg() % (OS_CANVAS_H - 24));
        site_c[i] = (int)(lcg() % 8);
    }

    for (int y = 0; y < OS_CANVAS_H; y++) {
        int left = -1;
        for (int x = 0; x < OS_CANVAS_W; x++) {
            int best = 0;
            int best_d = 0x7FFFFFFF;
            for (int i = 0; i < SITES; i++) {
                int dx = x - site_x[i];
                int dy = y - site_y[i];
                int d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best = i; }
            }

            /* Cell edges: the nearest site changed from the neighbour. */
            int edge = (left >= 0 && left != best) ||
                       (y > 0 && prev_row[x] != best);

            uint32_t color;
            if (edge) {
                color = 0x0B0E16u;
            } else {
                /* fade towards the rim of each cell */
                int shade = 255 - best_d / 90;
                if (shade < 70) shade = 70;
                color = scale_color(base_color[site_c[best]], shade, 255);
            }
            os_draw_pixel(x, y, color);

            left = best;
            prev_row[x] = best;
        }
    }

    /* Site markers. */
    for (int i = 0; i < SITES; i++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                if (dx * dx + dy * dy > 4) continue;
                int px = site_x[i] + dx, py = site_y[i] + dy;
                if (px < 0 || px >= OS_CANVAS_W ||
                    py < 0 || py >= OS_CANVAS_H) continue;
                os_draw_pixel(px, py, 0xFFF6DCu);
            }

    os_print("voronoi: done - integer squared distances only\n");
}
