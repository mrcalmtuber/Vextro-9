/*
 * life — Conway's Game of Life for Vextro 9.
 *
 * A 149x100 torus seeded from a linear congruential soup, run for 160
 * generations.  The final board is drawn over a heat map of everywhere
 * life has ever been, so the still lifes and oscillators that survive
 * stand out against the burnt-out region they grew from.
 */
#include "../vextro.h"

#define GW    149
#define GH    100
#define CELL  4
#define GENS  160

static unsigned char cur[GW * GH];
static unsigned char nxt[GW * GH];
static unsigned char age[GW * GH];   /* generations this cell was alive */
static unsigned char seen[GW * GH];  /* peak age, for the heat map      */

static uint32_t lcg_state = 0x1BADB002u;

static uint32_t lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state >> 8;
}

static void put_uint(char *out, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = '\0';
}

static void cell_fill(int gx, int gy, uint32_t color) {
    int x0 = 1 + gx * CELL;
    int y0 = 1 + gy * CELL;
    for (int y = 0; y < CELL - 1; y++)
        for (int x = 0; x < CELL - 1; x++)
            os_draw_pixel(x0 + x, y0 + y, color);
}

static uint32_t ramp(uint32_t a, uint32_t b, int t /* 0..255 */) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((((ar * (255 - t) + br * t) / 255) << 16) |
            (((ag * (255 - t) + bg * t) / 255) << 8) |
            (((ab * (255 - t) + bb * t) / 255)));
}

void _start(void) {
    os_print("life: 149x100 torus, 160 generations\n");

    /* Soup in the middle third, quiet at the edges. */
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int inside = (x > GW / 5 && x < GW * 4 / 5 &&
                          y > GH / 6 && y < GH * 5 / 6);
            cur[y * GW + x] = (unsigned char)(inside && (lcg() % 100) < 34);
        }

    for (int gen = 0; gen < GENS; gen++) {
        for (int y = 0; y < GH; y++) {
            int up = ((y + GH - 1) % GH) * GW;
            int mid = y * GW;
            int dn = ((y + 1) % GH) * GW;
            for (int x = 0; x < GW; x++) {
                int xl = (x + GW - 1) % GW;
                int xr = (x + 1) % GW;
                int n = cur[up + xl] + cur[up + x] + cur[up + xr] +
                        cur[mid + xl] +                cur[mid + xr] +
                        cur[dn + xl] + cur[dn + x] + cur[dn + xr];
                int alive = cur[mid + x];
                nxt[mid + x] = (unsigned char)((n == 3) || (alive && n == 2));
            }
        }
        for (int i = 0; i < GW * GH; i++) {
            cur[i] = nxt[i];
            if (cur[i]) {
                if (age[i] < 255) age[i]++;
                if (age[i] > seen[i]) seen[i] = age[i];
            } else {
                age[i] = 0;
            }
        }
    }

    uint32_t population = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int i = y * GW + x;
            uint32_t color;
            if (cur[i]) {
                /* Long-lived cells burn from gold up to white. */
                int t = age[i] > 60 ? 255 : age[i] * 255 / 60;
                color = ramp(0xD4AF37u, 0xFFF6DCu, t);
                population++;
            } else if (seen[i]) {
                int t = seen[i] > 40 ? 255 : seen[i] * 255 / 40;
                color = ramp(0x0B0E16u, 0x2A2438u, t);
            } else {
                color = 0x05070Cu;
            }
            cell_fill(x, y, color);
        }

    char msg[48];
    char nb[12];
    int p = 0;
    const char *pre = "life: final population ";
    while (pre[p]) { msg[p] = pre[p]; p++; }
    put_uint(nb, population);
    for (int i = 0; nb[i]; i++) msg[p++] = nb[i];
    msg[p++] = '\n';
    msg[p] = '\0';
    os_print(msg);
}
