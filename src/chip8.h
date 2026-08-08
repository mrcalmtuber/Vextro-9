#ifndef VEXTRO_CHIP8_H
#define VEXTRO_CHIP8_H

/*
 * src/chip8.h — a CHIP-8 interpreter.
 *
 * This is an emulator for a legacy machine, and it is called that rather
 * than "virtualisation" or "compatibility mode", because those words mean
 * running native code under a hypervisor and there is no hypervisor here.
 * Every instruction below is decoded and executed in software.
 *
 * CHIP-8 was an interpreted language on 1970s microcomputers: 4 KB of
 * memory, sixteen 8-bit registers, a 64x32 monochrome display, and
 * thirty-five instructions. Small enough to implement completely and
 * honestly, which is the point -- a partial x86 emulator would be a
 * larger lie than no emulator at all.
 *
 * The ROM is written in this file rather than downloaded, so nothing
 * external ships and there is no licence question about the classic
 * games.
 */

#define C8_MEM      4096
#define C8_START    0x200          /* where a ROM is loaded, by convention */
#define C8_W        64
#define C8_H        32
#define C8_STACK    16

static uint8_t  c8_mem[C8_MEM];
static uint8_t  c8_v[16];
static uint16_t c8_i;
static uint16_t c8_pc;
static uint16_t c8_stack[C8_STACK];
static uint8_t  c8_sp;
static uint8_t  c8_dt, c8_st;      /* delay and sound timers */
static uint8_t  c8_fb[C8_W * C8_H];
static uint8_t  c8_keys[16];
static int      c8_running = 0;
static int      c8_halted = 0;     /* waiting on a key */
static uint32_t c8_rng = 0x2545F491u;

/* The built-in font: sixteen 4x5 glyphs at 0x000, where every CHIP-8
 * program expects them because FX29 points I at one by digit. */
static const uint8_t c8_font[80] = {
    0xF0,0x90,0x90,0x90,0xF0, 0x20,0x60,0x20,0x20,0x70,
    0xF0,0x10,0xF0,0x80,0xF0, 0xF0,0x10,0xF0,0x10,0xF0,
    0x90,0x90,0xF0,0x10,0x10, 0xF0,0x80,0xF0,0x10,0xF0,
    0xF0,0x80,0xF0,0x90,0xF0, 0xF0,0x10,0x20,0x40,0x40,
    0xF0,0x90,0xF0,0x90,0xF0, 0xF0,0x90,0xF0,0x10,0xF0,
    0xF0,0x90,0xF0,0x90,0x90, 0xE0,0x90,0xE0,0x90,0xE0,
    0xF0,0x80,0x80,0x80,0xF0, 0xE0,0x90,0x90,0x90,0xE0,
    0xF0,0x80,0xF0,0x80,0xF0, 0xF0,0x80,0xF0,0x80,0x80,
};

/*
 * The bundled ROM, hand-assembled, with its addresses written out so the
 * jump targets can be checked against the listing rather than trusted.
 *
 *   200  6000      V0 = 0            x position
 *   202  610C      V1 = 12           y position
 *   204  6201      V2 = 1            x velocity
 *   206  A222      I  = 0x222        the sprite
 *   208  00E0      CLS               <- loop
 *   20A  D015      DRW V0, V1, 5
 *   20C  8024      V0 += V2          (8XY4, sets VF on carry)
 *   20E  4038      SNE V0, 0x38      at the right edge?
 *   210  62FF      V2 = 0xFF         then travel left (-1 as u8)
 *   212  4000      SNE V0, 0x00      at the left edge?
 *   214  6201      V2 = 0x01         then travel right
 *   216  7101      V1 += 1           drift downwards
 *   218  411B      SNE V1, 0x1B      past the bottom?
 *   21A  610C      V1 = 12           then back to the top
 *   21C  6304      V3 = 4            frames to wait
 *   21E  F315      DT = V3
 *   220  1208      JP 0x208
 *   222  sprite    5 rows
 *
 * The delay timer is what makes it move at a watchable speed: without it
 * the sprite crosses the screen as fast as the interpreter runs, which on
 * this machine is far faster than the hardware it was written for.
 */
static const uint8_t c8_rom[] = {
    0x60,0x00, 0x61,0x0C, 0x62,0x01, 0xA2,0x22,
    0x00,0xE0, 0xD0,0x15, 0x80,0x24, 0x40,0x38,
    0x62,0xFF, 0x40,0x00, 0x62,0x01, 0x71,0x01,
    0x41,0x1B, 0x61,0x0C, 0x63,0x04, 0xF3,0x15,
    0x12,0x08,
    0x00,                                   /* pad to an even address */
    /* 0x222: a diamond, so the XOR draw is obvious against the grid */
    0x20, 0x70, 0xF8, 0x70, 0x20,
};

static uint32_t c8_rand(void) {
    c8_rng ^= c8_rng << 13; c8_rng ^= c8_rng >> 17; c8_rng ^= c8_rng << 5;
    return c8_rng;
}

static void c8_reset(void) {
    for (int i = 0; i < C8_MEM; i++) c8_mem[i] = 0;
    for (int i = 0; i < 80; i++) c8_mem[i] = c8_font[i];
    for (unsigned i = 0; i < sizeof(c8_rom) && C8_START + i < C8_MEM; i++)
        c8_mem[C8_START + i] = c8_rom[i];
    for (int i = 0; i < 16; i++) { c8_v[i] = 0; c8_keys[i] = 0; }
    for (int i = 0; i < C8_W * C8_H; i++) c8_fb[i] = 0;
    c8_i = 0; c8_pc = C8_START; c8_sp = 0; c8_dt = 0; c8_st = 0;
    c8_halted = 0;
}

/*
 * One instruction.
 *
 * Every memory access is masked to the 4 KB address space rather than
 * trusted: a ROM is data, and an interpreter that indexes its own arrays
 * with a value out of one is a memory-safety bug wearing a costume. The
 * same goes for register numbers, which come out of the opcode nibbles
 * and are therefore already 0..15, and for the stack pointer, which is
 * the one place a program could otherwise push past the end.
 */
static void c8_step(void) {
    if (c8_halted) return;

    const uint16_t pc = (uint16_t)(c8_pc & (C8_MEM - 1));
    const uint16_t op = (uint16_t)((c8_mem[pc] << 8) |
                                    c8_mem[(pc + 1) & (C8_MEM - 1)]);
    c8_pc = (uint16_t)((pc + 2) & (C8_MEM - 1));

    const uint8_t  x   = (uint8_t)((op >> 8) & 0x0F);
    const uint8_t  y   = (uint8_t)((op >> 4) & 0x0F);
    const uint8_t  n   = (uint8_t)(op & 0x0F);
    const uint8_t  kk  = (uint8_t)(op & 0xFF);
    const uint16_t nnn = (uint16_t)(op & 0x0FFF);

    switch (op >> 12) {
    case 0x0:
        if (op == 0x00E0) {
            for (int i = 0; i < C8_W * C8_H; i++) c8_fb[i] = 0;
        } else if (op == 0x00EE) {
            if (c8_sp > 0) c8_pc = c8_stack[--c8_sp];
        }
        break;
    case 0x1: c8_pc = nnn; break;
    case 0x2:
        if (c8_sp < C8_STACK) { c8_stack[c8_sp++] = c8_pc; c8_pc = nnn; }
        break;
    case 0x3: if (c8_v[x] == kk)      c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1)); break;
    case 0x4: if (c8_v[x] != kk)      c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1)); break;
    case 0x5: if (c8_v[x] == c8_v[y]) c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1)); break;
    case 0x6: c8_v[x] = kk; break;
    case 0x7: c8_v[x] = (uint8_t)(c8_v[x] + kk); break;
    case 0x8:
        switch (n) {
        case 0x0: c8_v[x] = c8_v[y]; break;
        case 0x1: c8_v[x] |= c8_v[y]; break;
        case 0x2: c8_v[x] &= c8_v[y]; break;
        case 0x3: c8_v[x] ^= c8_v[y]; break;
        case 0x4: {
            const uint16_t s = (uint16_t)(c8_v[x] + c8_v[y]);
            c8_v[0xF] = s > 0xFF; c8_v[x] = (uint8_t)s;
        } break;
        case 0x5: {
            const uint8_t b = c8_v[x] >= c8_v[y];
            c8_v[x] = (uint8_t)(c8_v[x] - c8_v[y]); c8_v[0xF] = b;
        } break;
        case 0x6: { const uint8_t b = c8_v[x] & 1; c8_v[x] >>= 1; c8_v[0xF] = b; } break;
        case 0x7: {
            const uint8_t b = c8_v[y] >= c8_v[x];
            c8_v[x] = (uint8_t)(c8_v[y] - c8_v[x]); c8_v[0xF] = b;
        } break;
        case 0xE: { const uint8_t b = (c8_v[x] >> 7) & 1; c8_v[x] = (uint8_t)(c8_v[x] << 1); c8_v[0xF] = b; } break;
        default: break;
        }
        break;
    case 0x9: if (c8_v[x] != c8_v[y]) c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1)); break;
    case 0xA: c8_i = nnn; break;
    case 0xB: c8_pc = (uint16_t)((nnn + c8_v[0]) & (C8_MEM - 1)); break;
    case 0xC: c8_v[x] = (uint8_t)(c8_rand() & kk); break;
    case 0xD: {
        /* Sprites are 8 pixels wide, XORed in, and VF reports whether any
         * lit pixel was turned off -- which is how CHIP-8 games do
         * collision detection, so getting it wrong breaks the game rather
         * than the picture. Rows wrap; that is the documented behaviour. */
        c8_v[0xF] = 0;
        for (int row = 0; row < n; row++) {
            const uint8_t bits = c8_mem[(c8_i + row) & (C8_MEM - 1)];
            const int py = (c8_v[y] + row) % C8_H;
            for (int col = 0; col < 8; col++) {
                if (!((bits >> (7 - col)) & 1)) continue;
                const int px = (c8_v[x] + col) % C8_W;
                uint8_t *p = &c8_fb[py * C8_W + px];
                if (*p) c8_v[0xF] = 1;
                *p ^= 1;
            }
        }
    } break;
    case 0xE:
        if (kk == 0x9E && c8_keys[c8_v[x] & 0xF]) c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1));
        if (kk == 0xA1 && !c8_keys[c8_v[x] & 0xF]) c8_pc = (uint16_t)((c8_pc + 2) & (C8_MEM - 1));
        break;
    case 0xF:
        switch (kk) {
        case 0x07: c8_v[x] = c8_dt; break;
        case 0x0A: {
            int got = -1;
            for (int k = 0; k < 16; k++) if (c8_keys[k]) { got = k; break; }
            if (got < 0) c8_pc = (uint16_t)((c8_pc - 2) & (C8_MEM - 1));  /* wait */
            else c8_v[x] = (uint8_t)got;
        } break;
        case 0x15: c8_dt = c8_v[x]; break;
        case 0x18: c8_st = c8_v[x]; break;
        case 0x1E: c8_i = (uint16_t)((c8_i + c8_v[x]) & (C8_MEM - 1)); break;
        case 0x29: c8_i = (uint16_t)((c8_v[x] & 0xF) * 5); break;
        case 0x33: {
            const uint8_t v = c8_v[x];
            c8_mem[(c8_i + 0) & (C8_MEM - 1)] = (uint8_t)(v / 100);
            c8_mem[(c8_i + 1) & (C8_MEM - 1)] = (uint8_t)((v / 10) % 10);
            c8_mem[(c8_i + 2) & (C8_MEM - 1)] = (uint8_t)(v % 10);
        } break;
        case 0x55: for (int k = 0; k <= x; k++) c8_mem[(c8_i + k) & (C8_MEM - 1)] = c8_v[k]; break;
        case 0x65: for (int k = 0; k <= x; k++) c8_v[k] = c8_mem[(c8_i + k) & (C8_MEM - 1)]; break;
        default: break;
        }
        break;
    default: break;
    }
}

/* The original ran roughly 500 instructions and 60 timer ticks a second,
 * so one frame is about eight instructions and one tick. */
#define C8_IPF 8

static void c8_frame(void) {
    if (!c8_running) return;
    for (int i = 0; i < C8_IPF; i++) c8_step();
    if (c8_dt) c8_dt--;
    if (c8_st) c8_st--;
}

/* ===== the window ===== */

static void c8_app_draw(uint32_t *buf, uint32_t w, uint32_t h,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                        int32_t mx, int32_t my) {
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x0B0E14u);
    c8_frame();

    gfx_rect(buf, w, h, cx, cy, cw, 34, 0x14171Fu);
    gfx_rect(buf, w, h, cx, cy + 33, cw, 1, 0x2A3142u);
    static const char *const btn[2] = { "Run", "Reset" };
    for (int i = 0; i < 2; i++) {
        const int32_t bx = cx + 10 + i * 62;
        const int hot = mx >= bx && mx < bx + 56 && my >= cy + 5 && my < cy + 29;
        const int on = (i == 0 && c8_running);
        gfx_rect(buf, w, h, bx, cy + 5, 56, 24,
                 hot ? 0x2A2410u : (on ? 0x232A16u : 0x1C2130u));
        gfx_rect_outline(buf, w, h, bx, cy + 5, 56, 24,
                         hot ? C_GOLD : (on ? C_GOLD_DIM : 0x2A3142u));
        const int tw = ttf_text_width(btn[i], 11);
        ttf_draw_string(buf, (int)w, (int)h, bx + (56 - tw) / 2, cy + 10,
                        btn[i], hot || on ? C_GOLD : C_TEXT, 11);
    }
    ttf_draw_string(buf, (int)w, (int)h, cx + 140, cy + 11,
                    "CHIP-8 interpreter, 35 opcodes, bundled ROM",
                    C_TEXT_DIM, 11);

    /* The 64x32 framebuffer, scaled by whole pixels: a fractional scale
     * would make some CHIP-8 pixels wider than others, which on a 64-wide
     * display is immediately visible. */
    const int32_t vy = cy + 40;
    const int32_t vh = chh - 48;
    int32_t s = cw / C8_W;
    if (vh / C8_H < s) s = vh / C8_H;
    if (s < 1) s = 1;
    const int32_t ox = cx + (cw - C8_W * s) / 2;
    const int32_t oy = vy + (vh - C8_H * s) / 2;

    gfx_rect_outline(buf, w, h, ox - 1, oy - 1, C8_W * s + 2, C8_H * s + 2,
                     0x2A3142u);
    for (int py = 0; py < C8_H; py++)
        for (int px = 0; px < C8_W; px++)
            gfx_rect(buf, w, h, ox + px * s, oy + py * s, s, s,
                     c8_fb[py * C8_W + px] ? C_GOLD : 0x11141Cu);
}

static void c8_app_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                         int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    (void)cw; (void)chh;
    if (!(lmb && !prev_lmb)) return;
    for (int i = 0; i < 2; i++) {
        const int32_t bx = cx + 10 + i * 62;
        if (mx >= bx && mx < bx + 56 && my >= cy + 5 && my < cy + 29) {
            if (i == 0) c8_running = !c8_running;
            else { c8_reset(); c8_running = 1; }
            return;
        }
    }
}

/* The original keypad was 4x4 hexadecimal; this maps the usual
 * 1234/QWER/ASDF/ZXCV block onto it, which is what every emulator does
 * and therefore what anyone's fingers expect. */
static void c8_app_key(char ch) {
    static const char map[17] = "x123qweasdzc4rfv";
    if (ch == ' ') { c8_running = !c8_running; return; }
    for (int i = 0; i < 16; i++)
        if (map[i] == ch) { c8_keys[i] = 1; return; }
}

/* Keys are released a frame later: there is no key-up event reaching the
 * desktop, so holding one is indistinguishable from tapping it. */
static void c8_keys_decay(void) {
    for (int i = 0; i < 16; i++) if (c8_keys[i]) c8_keys[i] = 0;
}

#endif /* VEXTRO_CHIP8_H */
