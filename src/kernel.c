#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "idt.h"
#include "pmm.h"
#include "gdt.h"
#include "vmm.h"
#include "kheap.h"
#include "syscall.h"
#include "sched.h"
#include "trap.h"
#include "mouse.h"
#include "keyboard.h"
#include "xhci.h"
#include "ttf.h"
#include "login.h"
#include "e1000.h"
#include "ac97.h"
#include "netstack.h"
#include "igpu.h"
#include "llm.h"
#include "desktop.h"
#include "bootanim.h"

/* Freestanding C runtime helpers — GCC may emit calls to these */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

/* Limine requests */
__attribute__((used, section(".limine_reqs_start")))
static volatile uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_reqs")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_module_request mod_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs_end")))
static volatile uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

/*
 * Software double-buffer.  The bound is a build option because it costs
 * static memory three times over — back buffer, previous frame, and the
 * wallpaper cache — so a machine that only ever runs 1280x800 should not
 * pay for a 2560x1600 panel it does not have.
 */
#ifndef BUF_MAX_W
#define BUF_MAX_W 1920
#endif
#ifndef BUF_MAX_H
#define BUF_MAX_H 1080
#endif
static uint32_t backbuf[BUF_MAX_W * BUF_MAX_H];
/* The previous frame, so a flip can tell what actually changed. */
static uint32_t prevbuf[BUF_MAX_W * BUF_MAX_H];
static int      prev_valid = 0;

#define COLOR_BLACK  0x000000u
#define COLOR_WHITE  0xFFFFFFu
#define COLOR_GOLD   0xD4AF37u

/* Typed text buffer for keyboard display */
#define TEXT_BUF_SIZE 128
static char typed_text[TEXT_BUF_SIZE];
static uint32_t typed_len = 0;

/*
 * Where the login screen is in its sequence.
 *
 * The machine used to have one anonymous passcode and two states: set it,
 * or type it. With accounts there is a first run that asks for a name and
 * a password twice, and a normal path that picks an account and asks for
 * its password.
 */
enum {
    LOGIN_PASSWORD = 0,   /* pick an account, type its password */
    LOGIN_NEW_NAME,       /* first run: choose a username        */
    LOGIN_NEW_PW,
    LOGIN_NEW_CONFIRM
};
static int  login_stage = LOGIN_PASSWORD;
static int  login_sel = 0;               /* which account is highlighted */
static char login_msg[96] = "";          /* replaces the prompt when set */
static char login_notice[112] = "";      /* shown under the box, additive */
static char pending_name[USER_NAME_MAX];
static char pending_pw[TEXT_BUF_SIZE];
static int  login_nav_up = 0, login_nav_down = 0;

/*
 * Whether the account being created is an administrator.
 *
 * Defaults to yes: on a machine with one user that is what they want, and
 * it is what the first account used to be given with no say in it.
 *
 * Declining does not leave the machine without one. Somebody has to be
 * able to create and remove accounts, so a separate `admin` account is
 * made alongside -- with the same password, because the alternative is a
 * fixed default one, and a known password on an administrator account is
 * a hole rather than a convenience. The login screen says so plainly
 * rather than leaving it to be discovered.
 */
#ifdef NO_ADMIN_DEFAULT
static int  login_want_admin = 0;   /* test hook: start with the box clear */
#else
static int  login_want_admin = 1;
#endif

/* Confirmation message state: shows gold text for ~1 second */
static int confirm_active = 0;
static uint32_t confirm_tick = 0;
#define CONFIRM_DURATION 60  /* ~1 second at 60 Hz */

/* Melt animation state */
static int melt_active = 0;
static uint32_t melt_tick = 0;
#define MELT_DURATION 120  /* ~2 seconds at 60 Hz */

/* Desktop mode: set after successful authentication */
static int desktop_mode = 0;
#ifdef AUTO_ASK
static int auto_ask_pending = 0;
#endif

#ifdef AUTO_LOGIN
/*
 * Skip the login screen.
 *
 * A headless harness cannot get past a password it does not know, and the
 * volume carries whatever keycode was set on it. Off by default and never
 * in a release build; this only exists so screenshots of the desktop can
 * be taken automatically.
 */
static int auto_login = 1;
#else
static int auto_login = 0;
#endif


/*
 * The accounts on this machine, above the login box.
 *
 * Lives here rather than in login.h because login.h is included before
 * the filesystem layer that users.h needs, and login.h is byte-identical
 * across the two architecture trees -- a property worth keeping.
 *
 * Only drawn when there is a choice to make: one account and the box on
 * its own is the whole interface, exactly as before.
 */
#define ACCT_W   150
#define ACCT_H   34
#define ACCT_GAP 10

static void login_draw_users(uint32_t *buf, uint32_t w, uint32_t h,
                             int32_t mx, int32_t my, uint8_t lmb) {
    static uint8_t prev = 0;
    int click = (lmb & 1) && !prev;
    prev = lmb & 1;

    int32_t total = user_count * ACCT_W + (user_count - 1) * ACCT_GAP;
    int32_t x0 = ((int32_t)w - total) / 2;
    int32_t y0 = ((int32_t)h - LOGIN_BOX_H) / 2 - ACCT_H - 26;
    if (y0 < 8) y0 = 8;

    for (int i = 0; i < user_count; i++) {
        int32_t x = x0 + i * (ACCT_W + ACCT_GAP);
        int sel = (i == login_sel);
        int hot = (mx >= x && mx < x + ACCT_W && my >= y0 && my < y0 + ACCT_H);

        if (click && hot) login_sel = i;

        gfx_rect(buf, w, h, x, y0, ACCT_W, ACCT_H,
                 sel ? 0x2A2410u : 0x0E1017u);
        gfx_rect_outline(buf, w, h, x, y0, ACCT_W, ACCT_H,
                         sel ? COLOR_GOLD : (hot ? 0x6A5A20u : 0x2A3040u));

        const char *nm = user_name_of(i);
        int tw = ttf_text_width(nm, 14);
        ttf_draw_string(buf, (int)w, (int)h, x + (ACCT_W - tw) / 2, y0 + 7,
                        nm, sel ? COLOR_GOLD : 0x9098A8u, 14);

        if (user_is_admin(i))
            gfx_rect(buf, w, h, x + 6, y0 + 6, 4, 4, COLOR_GOLD);
    }
}

/*
 * The administrator checkbox, and any notice under the login box.
 *
 * Sits below the box during the first-run sequence, where the account is
 * being created and the choice still means something. Same clickable
 * shape as login.h's Show/Hide button.
 */
#define CHK_BOX 18

static void login_draw_admin_check(uint32_t *buf, uint32_t w, uint32_t h,
                                   int32_t mx, int32_t my, uint8_t lmb) {
    static uint8_t prev = 0;
    int click = (lmb & 1) && !prev;
    prev = lmb & 1;

    int32_t by = ((int32_t)h - LOGIN_BOX_H) / 2 + LOGIN_BOX_H + 22;
    const char *label = "Make this an administrator account";
    int32_t tw = ttf_text_width(label, 14);
    int32_t total = CHK_BOX + 10 + tw;
    int32_t x0 = ((int32_t)w - total) / 2;

    int hot = (mx >= x0 && mx < x0 + total &&
               my >= by - 2 && my < by + CHK_BOX + 2);
    if (click && hot) login_want_admin = !login_want_admin;

    gfx_rect(buf, w, h, x0, by, CHK_BOX, CHK_BOX,
             login_want_admin ? 0x2A2410u : 0x0E1017u);
    gfx_rect_outline(buf, w, h, x0, by, CHK_BOX, CHK_BOX,
                     hot ? COLOR_GOLD : (login_want_admin ? COLOR_GOLD
                                                          : 0x3A4050u));
    if (login_want_admin) {
        /* a tick, drawn as two strokes */
        for (int k = 0; k < 4; k++)
            gfx_rect(buf, w, h, x0 + 4 + k, by + 8 + k, 2, 2, COLOR_GOLD);
        for (int k = 0; k < 6; k++)
            gfx_rect(buf, w, h, x0 + 8 + k, by + 12 - k, 2, 2, COLOR_GOLD);
    }

    ttf_draw_string(buf, (int)w, (int)h, x0 + CHK_BOX + 10, by + 1, label,
                    login_want_admin ? 0xD8DCE6u : 0x8891A0u, 14);

    if (!login_want_admin)
        ttf_draw_string(buf, (int)w, (int)h, x0, by + CHK_BOX + 8,
                        "An 'admin' account will be created, same password",
                        0x8A8F9Cu, 12);
}

/* A line under the box that is not the prompt, so both can be shown. */
static void login_draw_notice(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!login_notice[0]) return;
    int32_t by = ((int32_t)h - LOGIN_BOX_H) / 2 + LOGIN_BOX_H + 24;
    int32_t tw = ttf_text_width(login_notice, 13);
    ttf_draw_string(buf, (int)w, (int)h, ((int32_t)w - tw) / 2, by,
                    login_notice, C_GOLD_DIM, 13);
}

/* HAL status flags */
static int hal_ps2_present = 1;

/* ---- PIT timer (IRQ0) — keeps render loop alive when mouse is idle ---- */
__attribute__((interrupt))
static void irq0_handler(interrupt_frame_t *f) {
    (void)f;
    sys_ticks++;
    /* This is the frame clock, and the compositor sleeps on it. Waking
     * it here rather than letting it poll is what keeps the interface
     * locked to 60 Hz while the scheduler runs at a thousand. */
    sched_frame_pulse();
    outb(PIC1_CMD, PIC_EOI);
}

/*
 * Where each frame's time goes.
 *
 * The ARM tree has printed a frame rate since its first milestone and this
 * one never did, which meant "the pointer feels choppy" was a complaint
 * with no number attached to it — and the pointer's smoothness *is* the
 * frame rate, because the cursor is composited into the back buffer like
 * everything else.
 *
 * The split matters as much as the total. Compositing the desktop and
 * pushing the result to the panel are separately expensive and the fix for
 * one is not the fix for the other, so they are timed separately. TSC
 * rather than the PIT: at 60 Hz a tick is coarser than an entire frame.
 */
/*
 * Cycles per millisecond, measured against a clock that is known to be
 * right.
 *
 * The TSC's frequency is not discoverable portably — it is not the core
 * clock on anything modern, and under emulation it is whatever the host
 * decided. The PIT's frequency, on the other hand, is a fixed property of
 * the hardware: 1,193,182 Hz. So one is counted against the other.
 *
 * Channel 2 in one-shot mode, spun on directly, rather than counting IRQ0
 * ticks — and that choice is not stylistic. Interrupts are still masked
 * here; `sti` is a hundred lines further down, after the framebuffer and
 * every driver is up. A version of this that waited for `sys_ticks` to
 * advance waited for an interrupt that could not arrive, and hung the
 * kernel before its first line of output. Channel 2 signals completion
 * through a port bit instead, which the boot animation already relies on
 * for exactly this reason.
 */
#define PIT_HZ 1193182u

static void tsc_calibrate(void) {
    const uint32_t ms = 50;
    uint32_t count = PIT_HZ * ms / 1000;          /* 59659 for 50 ms */

    outb(0x43, 0xB0);                             /* ch2, mode 0, 16-bit */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)(gate & 0xFE));           /* gate low: reset */
    uint64_t c0 = cycle_now();
    outb(0x61, (uint8_t)((gate | 1) & 0xFD));     /* gate high: count down */

    /* Bounded: a machine with no working channel 2 must not hang here.
     * Falling out leaves cycles_per_ms at zero, which every budget reads as
     * "unknown" and degrades to one unit of work per frame. */
    for (uint32_t guard = 0; guard < 200000000u; guard++)
        if (inb(0x61) & 0x20) {                   /* OUT2 high: expired */
            cycles_per_ms = (cycle_now() - c0) / ms;
            break;
        }

    if (!cycles_per_ms) {
        serial_puts("[tsc] could not calibrate - pacing one step per frame\n");
        return;
    }
    serial_puts("[tsc] ");
    serial_put_dec((uint32_t)(cycles_per_ms / 1000));
    serial_puts(" MHz apparent\n");
}

static uint32_t frame_n = 0;
static uint64_t frame_render_cy = 0, frame_flip_cy = 0, frame_idle_cy = 0;
static uint32_t frame_t0_ticks = 0;

/*
 * Pixels actually pushed into live scanout per frame.
 *
 * Cycles are the wrong instrument for the tearing near the pointer: the
 * flip spends nearly all of its time *comparing* a million pixels and
 * almost none writing the handful that changed, so the cycle count barely
 * moves however much the written area shrinks. What is visible on the
 * panel is the writing, because it races the host's sampling of it.
 */
static uint64_t frame_wr_px = 0;      /* written by the flip    */

/* Short accumulators for the desktop busy meter, reset four times a
 * second -- the serial report's own counters run for two seconds, which
 * is far too coarse for something being watched on screen. */
static uint64_t busy_acc = 0, idle_acc = 0;
static uint32_t busy_frames = 0;

static void frame_report(void) {
    if (++frame_n < 120) return;

    uint32_t elapsed = sys_ticks - frame_t0_ticks;    /* 60 Hz ticks */
    uint32_t ms = elapsed * 1000u / 60u;
    serial_puts("[frame] 120 in ");
    serial_put_dec(ms);
    serial_puts(" ms (");
    serial_put_dec(ms ? 120000u / ms : 0);
    serial_puts(" fps)  composite ");
    serial_put_dec((uint32_t)(frame_render_cy / 120 / 1000));
    serial_puts("k cyc  flip ");
    serial_put_dec((uint32_t)(frame_flip_cy / 120 / 1000));
    serial_puts("k cyc  idle ");
    serial_put_dec((uint32_t)(frame_idle_cy / 120 / 1000));
    serial_puts("k cyc  scanout ");
    serial_put_dec((uint32_t)(frame_wr_px / 120));
    serial_puts(" px\n");

    frame_n = 0;
    frame_render_cy = frame_flip_cy = frame_idle_cy = 0;
    frame_wr_px = 0;
    frame_t0_ticks = sys_ticks;
}

/*
 * Bring up the x87/SSE unit.  The kernel proper is compiled -mno-sse and
 * never touches these registers; only the inference translation unit
 * does, and it runs with interrupts enabled but never inside one, so no
 * ISR can clobber XMM state.
 *
 *   CR0.EM clear  - do not trap SSE as "no coprocessor"
 *   CR0.MP set    - WAIT/FWAIT honours TS
 *   CR4.OSFXSR    - FXSAVE/FXRSTOR available, SSE instructions legal
 *   CR4.OSXMMEXCPT- unmasked SSE exceptions raise #XF rather than #UD
 */
/*
 * Sleep out the rest of the frame, and let everything else run while we
 * do.
 *
 * This was a bare HLT, which is still what parks the processor. What is
 * new either side of it is the preemption count: the compositor holds it
 * raised for the whole of a frame so that no application thread can
 * reach the interface's shared state halfway through a redraw, and drops
 * it here, which is the one moment in the frame when nothing is
 * half-updated. An application therefore runs in the gap between the
 * last pixel of one frame and the first of the next — on this machine,
 * most of every frame.
 */
static inline void frame_idle(void) {
    preempt_enable();
    sched_wait_frame();
    preempt_disable();
}

static void fpu_init(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);          /* EM */
    cr0 |=  (1ULL << 1);          /* MP */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    __asm__ volatile("fninit");
}

static void pit_init(uint16_t cs) {
    idt_set_gate(0x20, irq0_handler, cs);

    /* PIT channel 0, mode 3 (square wave), 60 Hz ≈ divisor 19886 */
    uint16_t div = 19886;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)(div >> 8));

    /* Unmask IRQ0 (timer) + IRQ2 (cascade) — preserve other unmasks (keyboard) */
    uint8_t mask = inb(PIC1_DATA);
    mask &= (uint8_t)~((1 << 0) | (1 << 2));
    outb(PIC1_DATA, mask);
}

static void fill_rect(uint32_t w, uint32_t x, uint32_t y,
                      uint32_t rw, uint32_t rh, uint32_t color) {
    for (uint32_t row = y; row < y + rh; row++)
        for (uint32_t col = x; col < x + rw; col++)
            backbuf[row * w + col] = color;
}

/*
 * 12x18 arrow cursor: 'X' = black outline, '.' = white fill, ' ' = clear.
 * Upper-left hot spot.
 */
static const char *CURSOR_IMG[18] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      XXX   ",
};

#define CURSOR_W 12
#define CURSOR_H 18

/*
 * The pointer is composited by the flip, not drawn after it.
 *
 * It used to be stamped into the back buffer, which meant moving it
 * changed the frame and dragged a band of unchanged desktop across the
 * bus every time. Taking it out of the back buffer fixed that, but
 * drawing it into scanout *after* the flip introduced a worse problem:
 * every frame the flip wrote desktop pixels over the sprite and the
 * overlay put it back, so on any animating screen -- the login vortex
 * above all -- the pointer was erased and redrawn sixty times a second.
 * There is no vblank to hide that in, so the host could sample between
 * the two halves. That is the flicker, and it is also why the pointer
 * looked like it was sliding *underneath* things.
 *
 * Compositing inside the flip writes every pixel exactly once with its
 * final value, so the sprite is always on top and there is no window in
 * which it is missing.
 *
 * prevbuf still records the clean desktop pixel rather than the
 * composited one, so change detection stays honest -- the next frame
 * compares like with like.
 */
static int32_t cur_prev_x = 0, cur_prev_y = 0;
static int     cur_valid = 0;      /* cur_prev_* describe a real position */

/* The sprite's colour at a screen position, or 0 where it does not cover. */
static int cursor_at(int32_t px, int32_t py, int32_t cx, int32_t cy,
                     uint32_t *out) {
    int32_t dx = px - cx, dy = py - cy;
    if (dx < 0 || dx >= CURSOR_W || dy < 0 || dy >= CURSOR_H) return 0;
    char c = CURSOR_IMG[dy][dx];
    if (c == ' ') return 0;
    *out = (c == 'X') ? 0x000000u : 0xFFFFFFu;
    return 1;
}

/*
 * The fling trail.
 *
 * A hard flick can carry the pointer most of the way across the screen in
 * a couple of frames, which reads as the sprite disappearing and
 * reappearing somewhere else. A few fading ghosts along the path make the
 * movement legible -- you can see where it went rather than inferring it.
 *
 * Deliberately not always on. Below the fling threshold the trail is
 * empty and the pointer costs exactly what it did before, which matters
 * because slow movement is the precise mode and the last thing it wants
 * is decoration.
 */
#define TRAIL_N 5

static struct { int32_t x, y; uint8_t life; } trail[TRAIL_N];
static int trail_used = 0;

/* Weight of a ghost, 0-255, falling off with age. */
static uint32_t trail_weight(uint8_t life) {
    return (uint32_t)life * 40u;          /* life 4 -> 160, life 1 -> 40 */
}

/* Blend towards `c` by w/256, per channel, in integer arithmetic. */
static uint32_t trail_blend(uint32_t bg, uint32_t c, uint32_t w) {
    uint32_t br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t cr = (c  >> 16) & 0xFF, cg  = (c  >> 8) & 0xFF, cb = c  & 0xFF;
    uint32_t r = br + ((cr - br) * w >> 8);
    uint32_t g = bgn + ((cg - bgn) * w >> 8);
    uint32_t b = bb + ((cb - bb) * w >> 8);
    return (r << 16) | (g << 8) | b;
}

/* The strongest ghost covering a pixel, if any. */
static int trail_at(int32_t px, int32_t py, uint32_t *colour, uint32_t *weight) {
    uint32_t best = 0, bc = 0;
    for (int i = 0; i < trail_used; i++) {
        if (!trail[i].life) continue;
        uint32_t c;
        if (!cursor_at(px, py, trail[i].x, trail[i].y, &c)) continue;
        uint32_t w = trail_weight(trail[i].life);
        if (w > best) { best = w; bc = c; }
    }
    if (!best) return 0;
    *colour = bc;
    *weight = best;
    return 1;
}

/*
 * Age the trail by one frame, and record where the pointer was if it was
 * moving fast enough to be worth showing.
 */
static void trail_step(int32_t cx, int32_t cy, int moved_fast) {
    for (int i = 0; i < trail_used; i++)
        if (trail[i].life) trail[i].life--;

    /* compact out the dead, so the search above stays short */
    int k = 0;
    for (int i = 0; i < trail_used; i++)
        if (trail[i].life) trail[k++] = trail[i];
    trail_used = k;

    if (!moved_fast) return;
    if (trail_used == TRAIL_N) {
        for (int i = 1; i < TRAIL_N; i++) trail[i-1] = trail[i];
        trail_used = TRAIL_N - 1;
    }
    trail[trail_used].x = cx;
    trail[trail_used].y = cy;
    trail[trail_used].life = 4;
    trail_used++;
}
static void vga_flip(volatile uint32_t *vram,
                     uint32_t w, uint32_t h, uint32_t pitch_px) {
    if (gfx_force_full_flip) {          /* someone else wrote the panel */
        gfx_force_full_flip = 0;
        prev_valid = 0;
        cur_valid = 0;
    }

    int32_t cx = mouse_x, cy = mouse_y;

    /*
     * The rectangle the pointer occupies now, unioned with the one it
     * occupied last frame. Both have to be rewritten: the new one to
     * paint the sprite, the old one to put the desktop back underneath
     * it. prevbuf holds the clean pixels for both, so the row scan below
     * would otherwise decide nothing had changed and skip them -- which
     * is exactly what would leave a trail behind the pointer.
     */
    int32_t ux0 = cx, uy0 = cy, ux1 = cx + CURSOR_W, uy1 = cy + CURSOR_H;
    if (cur_valid) {
        if (cur_prev_x < ux0) ux0 = cur_prev_x;
        if (cur_prev_y < uy0) uy0 = cur_prev_y;
        if (cur_prev_x + CURSOR_W > ux1) ux1 = cur_prev_x + CURSOR_W;
        if (cur_prev_y + CURSOR_H > uy1) uy1 = cur_prev_y + CURSOR_H;
    }

    /*
     * Age the trail and fold every live ghost into the same rectangle.
     * They have to be in it whether they are being drawn or erased --
     * prevbuf holds the clean desktop underneath them, so a ghost left
     * out of the union is a ghost that never goes away.
     */
    trail_step(cx, cy, paccel_is_fling());
    for (int i = 0; i < trail_used; i++) {
        if (trail[i].x < ux0) ux0 = trail[i].x;
        if (trail[i].y < uy0) uy0 = trail[i].y;
        if (trail[i].x + CURSOR_W > ux1) ux1 = trail[i].x + CURSOR_W;
        if (trail[i].y + CURSOR_H > uy1) uy1 = trail[i].y + CURSOR_H;
    }
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 > (int32_t)w) ux1 = (int32_t)w;
    if (uy1 > (int32_t)h) uy1 = (int32_t)h;

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;

        int in_cur = ((int32_t)row >= uy0 && (int32_t)row < uy1 && ux1 > ux0);

        uint32_t c0 = 0, c1 = w;
        if (prev_valid) {
            while (c0 < w && src[c0] == cmp[c0]) c0++;
            if (c0 == w) {
                if (!in_cur) continue;              /* row unchanged */
                c0 = (uint32_t)ux0;                 /* but the pointer is here */
                c1 = (uint32_t)ux1;
            } else {
                while (c1 > c0 && src[c1 - 1] == cmp[c1 - 1]) c1--;
                if (in_cur) {
                    if ((uint32_t)ux0 < c0) c0 = (uint32_t)ux0;
                    if ((uint32_t)ux1 > c1) c1 = (uint32_t)ux1;
                }
            }
        }

        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = c0; col < c1; col++) {
            uint32_t px = src[col];
            cmp[col] = px;                    /* record the clean pixel */
            uint32_t sprite, gw;
            if (in_cur) {
                /* ghosts first, so the live sprite draws over them */
                if (trail_used &&
                    trail_at((int32_t)col, (int32_t)row, &sprite, &gw))
                    px = trail_blend(px, sprite, gw);
                if (cursor_at((int32_t)col, (int32_t)row, cx, cy, &sprite))
                    px = sprite;              /* the pointer wins, always */
            }
            dst[col] = px;
        }
        frame_wr_px += c1 - c0;
    }
    prev_valid = 1;
    cur_prev_x = cx;
    cur_prev_y = cy;
    cur_valid = 1;
}

/* HAL initialization: probe PS/2 controller safely */
static int hal_init_devices(uint16_t cs, int32_t w, int32_t h) {
    /* Check if PS/2 controller is present (bus floats 0xFF if absent) */
    uint8_t status = inb(0x64);
    if (status == 0xFF) {
        hal_ps2_present = 0;
        return -1;
    }

    /* PS/2 controller exists — init mouse and keyboard */
    mouse_init(cs, w - 1, h - 1);
    keyboard_init(cs);

    /*
     * USB input, for machines that have no PS/2 controller to emulate one.
     *
     * Brought up after the PS/2 path rather than instead of it. Under a
     * hypervisor the emulated PS/2 devices and the VMware backdoor are
     * present and better — the backdoor pointer is absolute, which a USB
     * boot-protocol mouse cannot be. On real hardware there is usually no
     * PS/2 controller at all and this is the only way in. Both feed the
     * same ring and the same pointer variables, so whichever answers,
     * the desktop above cannot tell.
     */
#ifdef ENABLE_XHCI
    /*
     * The MMIO mapper walks the page tables through Limine's direct map,
     * so it needs the HHDM offset before it can resolve anything. That
     * used to be set as a side effect of e1000_init(), which runs much
     * later — so every walk here used offset zero, dereferenced a
     * physical address that is not mapped, and faulted into a handler
     * that halts without a word. Setting it explicitly makes the
     * dependency visible instead of incidental.
     */
    if (hhdm_request.response) hal_hhdm_offset = hhdm_request.response->offset;
    if (xhci_init()) xhci_enumerate();
#endif
    return 0;
}

/*
 * Frame pacing for the boot animation, in two halves.
 *
 * ~41.67 ms per frame (24 fps) via PIT channel 2 one-shot, no IRQ needed:
 * PIT freq = 1,193,182 Hz, and 1193182/24 is about 49716 ticks.
 *
 * Split into "start the clock" and "wait for it" because a single call
 * that does both can only sleep 41 ms *after* the frame is drawn, which
 * makes every frame cost the drawing plus the wait. Rendering the
 * animation takes about 18 ms under emulation, so a five second sequence
 * ran for seven. Arming the timer first and waiting for what is left of
 * it afterwards is the same two port writes and gives the frame rate that
 * was asked for.
 */
static void boot_frame_start(void) {
    outb(0x43, 0xB0);          /* ch2, lobyte/hibyte, mode 0 (one-shot) */
    outb(0x42, 0x34);          /* low byte of 49716 */
    outb(0x42, 0xC2);          /* high byte */
    uint8_t gate = inb(0x61);
    outb(0x61, gate & 0xFE);   /* gate low — reset */
    outb(0x61, (gate | 1) & 0xFD); /* gate high, speaker off — start count */
}

static void boot_frame_wait(void) {
    while (!(inb(0x61) & 0x20)) {} /* spin until OUT2 goes high */
}

/*
 * Has someone hit a key?  The animation runs before the IDT is loaded,
 * so there is no interrupt handler to ask — read the PS/2 controller
 * directly.  Consuming the press here also means it does not turn up
 * later as a stray character in the keycode field.
 */
static int boot_key_pressed(void) {
    uint8_t st = inb(0x64);
    if (st == 0xFF || !(st & 1)) return 0;   /* no controller, or nothing */
    if (st & 0x20) { (void)inb(0x60); return 0; }  /* mouse byte, discard */
    return !(inb(0x60) & 0x80);              /* a press, not a release */
}

static void display_boot_animation(volatile uint32_t *vram,
                                   uint32_t scr_w, uint32_t scr_h,
                                   uint32_t pitch_px) {
    ba_init((int)scr_w, (int)scr_h);

    /*
     * Composed in the back buffer and blitted, rather than drawn straight
     * into the framebuffer.
     *
     * Video memory is uncached, so a store into it costs many times what
     * the same store costs in RAM -- and the upscale writes scale x scale
     * of them per simulated pixel, which is where nearly all of the time
     * would go. Building the frame in RAM and copying it out once is both
     * faster and free of the tearing that comes from a panel being scanned
     * while it is still being written.
     */
    for (uint32_t i = 0; i < scr_w * scr_h; i++) backbuf[i] = 0;

    for (int f = 0; f < BA_FRAMES; f++) {
        boot_frame_start();
        ba_render(backbuf, (int)scr_w, f);
        for (uint32_t row = 0; row < scr_h; row++) {
            const uint32_t *src = backbuf + (size_t)row * scr_w;
            volatile uint32_t *dst = vram + (size_t)row * pitch_px;
            for (uint32_t col = 0; col < scr_w; col++) dst[col] = src[col];
        }
        boot_frame_wait();
        if (boot_key_pressed()) break;   /* any key skips the rest */
    }

    /* The desktop's own diff has no idea the panel was just repainted. */
    prev_valid = 0;

    for (uint32_t row = 0; row < scr_h; row++)
        for (uint32_t col = 0; col < scr_w; col++)
            vram[row * pitch_px + col] = 0;
}

void kmain(void) {
    if (fb_request.response == NULL ||
        fb_request.response->framebuffer_count < 1)
        while (1) __asm__ volatile("hlt");

    struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
    uint32_t panel_w  = (uint32_t)fb->width;
    uint32_t panel_h  = (uint32_t)fb->height;
    uint32_t pitch_px = (uint32_t)(fb->pitch / (fb->bpp / 8));
    volatile uint32_t *vram = (volatile uint32_t *)fb->address;

    /*
     * Everything after this point draws through a fixed back buffer, so
     * a mode larger than that only ever reaches the panel's top-left
     * corner.  Blank the whole panel once here, so the margin is black
     * rather than whatever the firmware left in video memory.
     */
    for (uint32_t row = 0; row < panel_h; row++)
        for (uint32_t col = 0; col < panel_w; col++)
            vram[row * pitch_px + col] = 0;

    uint32_t w = panel_w > BUF_MAX_W ? BUF_MAX_W : panel_w;
    uint32_t h = panel_h > BUF_MAX_H ? BUF_MAX_H : panel_h;

    /* The animation is centred on the real panel, not the back buffer */
    display_boot_animation(vram, panel_w, panel_h, pitch_px);

    /*
     * ---- the machine's own descriptors ----
     *
     * Everything up to here has been running on the tables Limine left
     * behind. They are correct and they are not sufficient: no user
     * descriptors, no task state segment, and therefore no way down to
     * ring 3 and no way back. From this call onward the kernel code
     * selector is GDT_KCODE and not whatever the bootloader chose, which
     * is why the IDT is built after it and not before.
     */
    gdt_init();
    const uint16_t cs = GDT_KCODE;

    /* Floating point, before anything that might use it */
    fpu_init();

    /* Build IDT: remap PIC, install fault handlers, fill the rest with
     * no-op stubs */
    idt_init(cs);
    trap_install();
    idt_load();

    /*
     * ---- physical and virtual memory ----
     *
     * The frame allocator has to exist before the address-space code can
     * ask it for a page table, and both have to exist before anything
     * calls kmalloc. They are also the first things in this function
     * that can fail on a machine rather than in the abstract, so they go
     * as early as the firmware's own data allows.
     */
    if (memmap_request.response != NULL && hhdm_request.response != NULL) {
        pmm_init(memmap_request.response, hhdm_request.response->offset);
        pmm_install_mmio_hook();
        vmm_init();
    } else {
        serial_puts("[vextro] no memory map: paging and heap unavailable\n");
    }

    /* SYSCALL/SYSRET, the int 0x80 gate at DPL 3, and EFER.NXE */
    syscall_init();
    kernel_exports_init();

    /* HAL: safely probe and initialize PS/2 devices */
    hal_init_devices(cs, (int32_t)w, (int32_t)h);

    /* Start PIT at ~60 Hz so the render loop runs even when mouse is idle */
    pit_init(cs);

    /* The TSC frequency is not discoverable, but the PIT was just set to a
     * known rate — so measure one against the other while nothing else is
     * competing for the machine. Everything that paces itself by time
     * rather than by a fixed count depends on this. */
    tsc_calibrate();

    /*
     * ---- threads ----
     *
     * The local APIC's timer is the scheduler's clock, at a millisecond,
     * and the PIT stays where it was at 60 Hz because that is the frame
     * clock. Two timers because they are answering two different
     * questions; sharing one would mean either a scheduler that reacts a
     * frame late or an animation that ticks a thousand times a second.
     *
     * The context this is called from becomes thread 1 — the compositor
     * — because it is already running and cannot be created.
     */
    lapic_init(1000);
    sched_init();
    sched_reap_hook = app_reaped;
    sched_start();

    /* Initialize Intel e1000 NIC via PCI discovery */
    if (hhdm_request.response != NULL) {
        e1000_init(hhdm_request.response->offset);
    }

    /* Initialize Intel AC97 audio via PCI discovery */
    ac97_init();

    /* Initialize Layer 2 network stack (Ethernet + ARP) */
    netstack_init();

    /* Intel Gen9 iGPU blitter — optional acceleration; on machines
     * without a supported iGPU we stay on the portable CPU renderer */
    if (hhdm_request.response != NULL) {
        hal_hhdm_offset = hhdm_request.response->offset;
        uint64_t fb_phys = kern_virt_to_phys((void *)(uintptr_t)vram);
        if (fb_phys)
            igpu_init(fb_phys, w, h, pitch_px);
    }

    /*
     * AMD-V. This has to run after the HHDM offset is known, because
     * every structure the processor is handed -- the VMCB, the host
     * save area, the nested page tables -- is given to it as a physical
     * address, and kern_virt_to_phys is what produces one. On a machine
     * without SVM this probes, records why, and changes nothing.
     */
    hv_init();
    serial_puts("[hv] ");
    serial_puts(hv.status);
    serial_puts("\n");

    /* Initialize Tarfs from Limine boot module (initrd.tar) — read-only
     * fallback for ISO-only boots without a hard disk */
    if (mod_request.response != NULL &&
        mod_request.response->module_count > 0) {
        struct limine_file *mod = mod_request.response->modules[0];
        tarfs_init(mod->address, mod->size);
    }

    /* Storage: probe NVMe, then SATA, then the legacy IDE ports, and
     * mount the first volume any of them turns out to be carrying.  The
     * tar ramdisk stays as the read-only fallback for a machine where
     * none of the three finds a disk. */
    if (hhdm_request.response != NULL)
        hal_hhdm_offset = hhdm_request.response->offset;
    blk_init();
#ifdef STORAGE_SELFTEST
    blk_selftest();
#endif
    fs_mount();

    /* App store: load the shipped catalog and the installed-app registry
     * so the dock and the Apps menu already know about installed apps */
    store_init();

#ifdef APP_SELFTEST
    /*
     * Run the shipped app straight from boot, before the login screen can
     * get in the way. The loader is otherwise only reachable by typing at
     * a terminal behind a password prompt, which is not something a
     * headless harness can do; this makes import resolution testable.
     */
    serial_puts("[vextro] app selftest: running /hello\n");
    execute_bin_blocking("/hello", 0);
    serial_puts("[vextro] app selftest: done\n");
#endif

#ifdef WIKI_SELFTEST
    /*
     * Lay out a real article and report what came out.
     *
     * The property the layout engine exists for -- that a link does not end
     * the line it is in -- is not visible in a screenshot, and the article
     * that exercises it is the one on the volume rather than a fixture. So
     * the numbers go to serial where a harness can assert on them.
     */
    {
        serial_puts("[wikitest] opening archive\n");
        wiki_autoopen();
        if (!zim.open) {
            serial_puts("[wikitest] FAIL: no archive\n");
        } else {
            const char *want = WIKI_SELFTEST;
            serial_puts("[wikitest] article: ");
            serial_puts(want);
            serial_puts("\n");

            wiki_last_cw = 780;
            if (wiki_load(want, 0) != 0) {
                serial_puts("[wikitest] FAIL: could not load\n");
            } else {
                serial_puts("[wikitest] title: ");
                serial_puts(wiki_art_title);
                serial_puts("\n[wikitest] lines=");
                serial_put_dec((uint32_t)wd_line_n);
                serial_puts(" runs=");
                serial_put_dec((uint32_t)wd_run_n);
                serial_puts(" chars=");
                serial_put_dec((uint32_t)wd_text_n);
                serial_puts(" height=");
                serial_put_dec((uint32_t)wd_total_h);
                serial_puts(" truncated=");
                serial_put_dec((uint32_t)wd_truncated);

                /* How many laid-out lines carry a link *and* other text?
                 * Under the browser's model this count is necessarily
                 * zero, because a link always ended its line. */
                uint32_t mixed = 0, linked = 0, headings = 0;
                for (int i = 0; i < wd_line_n; i++) {
                    int has_link = 0, has_plain = 0;
                    for (uint16_t k = 0; k < wd_lines[i].nrun; k++) {
                        wd_run_t *r = &wd_runs[wd_lines[i].run0 + k];
                        if (r->href >= 0) has_link = 1; else has_plain = 1;
                    }
                    if (has_link) linked++;
                    if (has_link && has_plain) mixed++;
                    if (wd_lines[i].style == WD_H1 ||
                        wd_lines[i].style == WD_H2 ||
                        wd_lines[i].style == WD_H3) headings++;
                }
                serial_puts("\n[wikitest] linked_lines=");
                serial_put_dec(linked);
                serial_puts(" mixed_lines=");
                serial_put_dec(mixed);
                serial_puts(" headings=");
                serial_put_dec(headings);
                serial_puts("\n");

                /* The first few lines, so the text can be eyeballed. */
                for (int i = 0; i < wd_line_n && i < 12; i++) {
                    char out[220];
                    int o = 0;
                    for (uint16_t k = 0; k < wd_lines[i].nrun && o < 200; k++) {
                        wd_run_t *r = &wd_runs[wd_lines[i].run0 + k];
                        for (uint16_t t = 0; t < r->len && o < 200; t++)
                            out[o++] = wd_text[r->start + t];
                    }
                    out[o] = '\0';
                    serial_puts("[wikitest] | ");
                    serial_puts(out);
                    serial_puts("\n");
                }
            }
        }
        serial_puts("[wikitest] done\n");
    }
#endif

    /* If a model is sitting on the volume, start pulling it in.  The
     * work itself happens in the render loop, so this only opens the
     * file — the desktop comes up while the weights are still arriving. */
    ai_autoload_start();

    /*
     * Accounts.
     *
     * A machine that predates them has a plaintext /keycode.sys and no
     * name attached to it; rather than lock its owner out, that becomes
     * an administrator account called "admin" with the same password.
     * A machine with neither goes to the first-run sequence.
     */
    users_load();
    /* Policy lives beside the accounts and is enforced in the loader, so
     * it has to be in memory before anything can be launched. */
    policy_load();
    if (user_count == 0 && users_migrate_keycode())
        serial_puts("[vextro] users: migrated /keycode.sys to 'admin'\n");

    if (user_count == 0) {
        login_stage = LOGIN_NEW_NAME;
        serial_puts("[vextro] users: no accounts, first-run setup\n");
    } else {
        login_stage = LOGIN_PASSWORD;
        login_sel = 0;
        serial_puts("[vextro] users: ");
        serial_put_dec((uint32_t)user_count);
        serial_puts(" account(s)\n");
    }

    /* Compute total system memory from Limine memory map */
    if (memmap_request.response != NULL) {
        uint64_t total_bytes = 0;
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE ||
                e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
                e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
                total_bytes += e->length;
        }
        system_total_memory_mb = total_bytes / (1024 * 1024);

        /*
         * The model is far larger than any static buffer, so the
         * inference arena is still most of the machine — but it is now
         * *allocated* rather than assumed.
         *
         * It used to take the largest usable region straight out of the
         * firmware map, which was correct while nothing else owned
         * physical memory. It is not correct any more: the frame
         * allocator hands out that same region for page tables, kernel
         * stacks and process images, and two owners of one region is
         * silent corruption that would show up as a model that answers
         * nonsense after a program has run. So it asks for a contiguous
         * run instead, and leaves the kernel enough to work with.
         */
        if (pmm_ready) {
            const uint64_t keep  = (64ull << 20) / PAGE_SIZE;   /* 64 MB */
            const uint64_t floor = (16ull << 20) / PAGE_SIZE;   /* 16 MB */
            uint64_t frames = 0;
            uint64_t phys = pmm_alloc_largest(keep, floor, &frames);
            if (phys) {
                void *arena = (void *)(uintptr_t)phys_to_virt(phys);
                llm_arena_init(arena, frames * PAGE_SIZE);
                /* Remembered so a different model can be loaded later
                 * without rebooting: switching resets the arena, which
                 * needs the region it was carved from. */
                ai_arena_base = arena;
                ai_arena_size = frames * PAGE_SIZE;
                serial_puts("[vextro] inference arena ");
                serial_put_dec((uint32_t)(frames * 4 / 1024));
                serial_puts(" MB, ");
                serial_put_dec((uint32_t)(pmm_free_frames * 4 / 1024));
                serial_puts(" MB left for the kernel\n");
            } else {
                serial_puts("[vextro] no contiguous run for the "
                            "inference arena\n");
            }
        }
    }

    /* Unmask hardware interrupts */
    __asm__ volatile("sti" ::: "memory");

    /*
     * The compositor is a thread now, and the frame is its critical
     * section.
     *
     * Everything it touches between waking and sleeping — the window
     * list, the terminal ring, the notification queue, the back buffer —
     * is reachable from a syscall made by an application thread, and
     * locking each of those separately would be a great deal of code to
     * protect a machine with one processor. Raising the preemption count
     * instead says the whole frame is indivisible; frame_idle() drops it
     * for exactly as long as this thread is asleep, which is when
     * applications run. On this hardware that is most of every frame.
     */
    preempt_disable();

    /* Render loop — wakes on each interrupt, redraws, sleeps again */
    while (1) {
        sched_reap();
        net_poll();
#ifdef ENABLE_XHCI
        xhci_poll();
#endif

        /* --- Confirmation message overlay --- */
        if (confirm_active) {
            char discard;
            while ((discard = kb_getchar()) != 0) (void)discard;

            confirm_tick++;

            for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;

            const char *msg = "Master Keycode Confirmed. Rebooting Interface...";
            int msg_len = 0;
            const char *mp = msg;
            while (*mp++) msg_len++;
            int msg_fs = 22;
            int msg_w = msg_len * (msg_fs * 6 / 10);
            int msg_x = ((int)w - msg_w) / 2;
            int msg_y = (int)h / 2 - msg_fs / 2;
            ttf_draw_string(backbuf, (int)w, (int)h, msg_x, msg_y,
                            msg, COLOR_GOLD, msg_fs);

            if (confirm_tick >= CONFIRM_DURATION) {
                confirm_active = 0;
                confirm_tick = 0;
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                login_initialized = 0;
            }

            vga_flip(vram, w, h, pitch_px);
            frame_idle();
            continue;
        }

        /* --- Melt animation --- */
        if (melt_active) {
            char discard;
            while ((discard = kb_getchar()) != 0) (void)discard;

            melt_tick++;
            screen_melt(backbuf, w, h, melt_tick);

            if (melt_tick >= MELT_DURATION) {
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                typed_len = 0;
                typed_text[0] = '\0';
                melt_active = 0;
                melt_tick = 0;
                melt_inited = 0;
                login_initialized = 0;
            }

            vga_flip(vram, w, h, pitch_px);
            frame_idle();
            continue;
        }

#ifdef TRAIL_DEMO
        /* Sweep the pointer fast enough to trail. The pointer cannot be
         * driven headlessly here -- query-mice reports one relative PS/2
         * device and qemu feeds the absolute path from the host cursor,
         * which does not exist under -display none. */
        if (desktop_mode) {
            static int td = 0, tdx = 34;
            if (++td > 90) {
                paccel_speed = 40;            /* as if flicked */
                mouse_x += tdx;
                if (mouse_x > 1100 || mouse_x < 150) tdx = -tdx;
            }
        }
#endif
        if (auto_login && !desktop_mode) {
            auto_login = 0;
            desktop_mode = 1;
            serial_puts("[vextro] AUTO_LOGIN: skipped the login screen\n");
            for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
#ifdef AI_ACCEPT
            /* AUTO_LOGIN skips the dialog, so the answer has to be given
             * here -- otherwise the loader refuses on an unanswered
             * question, which is exactly what it is meant to do. */
            ai_choice_save(1);
            ai_autoload_start();
            serial_puts("[vextro] AI_ACCEPT: model enabled\n");
#endif
#ifdef AUTO_ASK
            /*
             * Open the chat panel on a question, for a screenshot. The
             * model has to be loaded first, so this waits on ai_poll
             * from the render loop rather than firing here.
             */
            wm_open(WK_WIKI);
            wiki_autoopen();
            wiki_want_main = 0;
            auto_ask_pending = 1;
            serial_puts("[autoask] queued: " AUTO_ASK "\n");
#endif
#ifdef AUTO_WIKI
            /*
             * Open the encyclopedia on a named article.
             *
             * The pointer cannot be driven headlessly on this machine:
             * `query-mice` reports one relative PS/2 device, because the
             * guest's absolute path is the VMware backdoor and QEMU feeds
             * that from the *host* cursor, which does not exist with
             * `-display none`. So the window is opened directly rather
             * than by clicking the dock.
             */
            wm_open(WK_WIKI);
            wiki_autoopen();
            wiki_want_main = 0;      /* else the poll replaces this article
                                      * with the archive's front page */
            wiki_last_cw = wk_meta[WK_WIKI].w;
            if (wiki_load(AUTO_WIKI, 0) == 0)
                serial_puts("[vextro] AUTO_WIKI: article open\n");
            else
                serial_puts("[vextro] AUTO_WIKI: load failed\n");
#endif
        }

        /*
         * Logging out.
         *
         * Handled here rather than where it is requested, because both
         * the menu and the shell ask for it from inside a draw or a
         * command, and session_end() closes every window -- pulling the
         * list out from under whatever is walking it.
         */
        if (want_logout) {
            want_logout = 0;
            serial_puts("[vextro] logout: ");
            serial_puts(user_name_of(user_current));
            serial_puts("\n");
            session_end();
#ifdef USER_SELFTEST
            /* What the next person would inherit, if anything did leak. */
            serial_puts("[usertest] after logout: history=");
            serial_put_dec((uint32_t)term_hist_count);
            serial_puts(" cwd=");
            serial_puts(term_cwd);
            serial_puts(" browser=");
            serial_puts(brw_addr);
            serial_puts(" wiki_view=");
            serial_put_dec((uint32_t)wiki_view);
            serial_puts(" windows=");
            serial_put_dec((uint32_t)wm_stack_n);
            serial_puts("\n");
#endif
            user_current = -1;
            desktop_mode = 0;
            login_stage = LOGIN_PASSWORD;
            login_initialized = 0;      /* the vortex starts over */
            prev_valid = 0;             /* whole panel is about to change */
            typed_len = 0;
            typed_text[0] = '\0';
            login_msg[0] = '\0';
            for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
            vga_flip(vram, w, h, pitch_px);
            frame_idle();
            continue;
        }

        /* === DESKTOP MODE === */
        if (desktop_mode) {
            char dch;
            while ((dch = kb_getchar()) != 0)
                desktop_key_input(dch);

            /* Read then subtract, rather than read then zero, so a notch
             * that lands between the two is carried into the next frame
             * instead of being dropped. */
            int32_t wheel = mouse_wheel;
            if (wheel) {
                mouse_wheel -= wheel;
                desktop_wheel_input(wheel);
            }

#ifdef AUTO_ASK
            if (auto_ask_pending && llm_weights_loaded()) {
                auto_ask_pending = 0;
                serial_puts("[autoask] asking\n");
                wiki_ask(AUTO_ASK);
            }
#endif
            uint64_t t0 = cycle_now();
            desktop_render(backbuf, w, h, mouse_x, mouse_y, mouse_buttons);
            uint64_t t1 = cycle_now();
            vga_flip(vram, w, h, pitch_px);
            uint64_t t2 = cycle_now();
            frame_render_cy += t1 - t0;
            frame_flip_cy   += t2 - t1;
            frame_report();
            /*
             * hlt parks the core until the next interrupt. The PIT is
             * running at 60 Hz, so this sleeps out whatever is left of
             * the frame instead of spinning through it -- and the cycles
             * it does not spend are what the System gadget reports as
             * idle.
             */
            frame_idle();
            uint64_t t3 = cycle_now();
            frame_idle_cy += t3 - t2;

            /* Feed the desktop meter four times a second: often enough to
             * respond to something starting, slow enough that the bar is
             * readable rather than a flicker. */
            busy_acc += t2 - t0;
            idle_acc += t3 - t2;
            if (++busy_frames >= 15) {
                sys_busy_record((uint32_t)(busy_acc / busy_frames),
                                (uint32_t)(idle_acc / busy_frames));
                busy_acc = idle_acc = 0;
                busy_frames = 0;
            }
            continue;
        }

        /* Drain keyboard buffer into our typed text */
        int enter_pressed = 0;
        char ch;
        while ((ch = kb_getchar()) != 0) {
            if (ch == '\n') {
                enter_pressed = 1;
            } else if (ch == '\b') {
                if (typed_len > 0) typed_len--;
            } else if (ch == KEY_UP) {
                login_nav_up = 1;
            } else if (ch == KEY_DOWN) {
                login_nav_down = 1;
            } else if (ch >= 0x20 && ch < 0x7F &&
                       typed_len < TEXT_BUF_SIZE - 1) {
                typed_text[typed_len++] = ch;
            }
        }
        typed_text[typed_len] = '\0';

        if (enter_pressed && typed_len > 0) {
            switch (login_stage) {

            case LOGIN_NEW_NAME:
                /* First run: no accounts exist yet. */
                if (!user_name_ok(typed_text)) {
                    str_copy(login_msg, user_err, sizeof(login_msg));
                } else {
                    str_copy(pending_name, typed_text, sizeof(pending_name));
                    login_stage = LOGIN_NEW_PW;
                    login_msg[0] = '\0';
                }
                typed_len = 0;
                typed_text[0] = '\0';
                break;

            case LOGIN_NEW_PW:
                str_copy(pending_pw, typed_text, sizeof(pending_pw));
                login_stage = LOGIN_NEW_CONFIRM;
                typed_len = 0;
                typed_text[0] = '\0';
                break;

            case LOGIN_NEW_CONFIRM:
                if (!str_eq(pending_pw, typed_text)) {
                    str_copy(login_msg, "Those did not match. Try again.",
                             sizeof(login_msg));
                    login_stage = LOGIN_NEW_PW;
                } else if (!login_want_admin && str_eq(pending_name, "admin")) {
                    /* The fallback account would collide with theirs. */
                    str_copy(login_msg, "'admin' is the fallback name - tick "
                             "the box or pick another.", sizeof(login_msg));
                    login_stage = LOGIN_NEW_NAME;
                } else if (user_add(pending_name, pending_pw,
                                    login_want_admin) < 0) {
                    str_copy(login_msg, user_err, sizeof(login_msg));
                    login_stage = LOGIN_NEW_PW;
                } else {
                    /*
                     * Someone has to be able to create and remove
                     * accounts. If they did not want that to be them, a
                     * separate administrator is made alongside.
                     */
                    if (!login_want_admin &&
                        user_add("admin", pending_pw, 1) >= 0) {
                        str_copy(login_notice,
                                 "Administrator account 'admin' created, "
                                 "same password", sizeof(login_notice));
                        serial_puts("[vextro] users: created 'admin' "
                                    "alongside a standard account\n");
                    }
                    login_sel = user_find(pending_name);
                    login_stage = LOGIN_PASSWORD;
                    confirm_active = 1;
                    confirm_tick = 0;
                    login_msg[0] = '\0';
                }
                for (uint32_t i = 0; i < sizeof(pending_pw); i++)
                    pending_pw[i] = '\0';
                typed_len = 0;
                typed_text[0] = '\0';
                break;

            case LOGIN_PASSWORD:
            default:
                if (!user_check(login_sel, typed_text)) {
                    typed_len = 0;
                    typed_text[0] = '\0';
                    melt_active = 1;
                    melt_tick = 0;
                    vga_flip(vram, w, h, pitch_px);
                    frame_idle();
                    continue;
                }
                typed_len = 0;
                typed_text[0] = '\0';
                user_current = login_sel;
                session_begin(user_name_of(user_current));
                desktop_mode = 1;
#ifdef USER_SELFTEST
                /*
                 * Exercise the account commands from the shell and report
                 * to serial. Typing at the desktop cannot open a window --
                 * keys go to whichever window has focus, and none does --
                 * and the pointer cannot be driven headlessly here, so the
                 * terminal is opened directly.
                 */
#ifdef AI_DECLINE
                /* The pointer cannot be driven headlessly here, so the
                 * dialog's two outcomes are reachable by build flag. */
                ai_choice_save(0);
                serial_puts("[aitest] declined via hook\n");
#endif
#ifdef AI_ACCEPT
                ai_choice_save(1);
                ai_autoload_start();
                serial_puts("[aitest] accepted via hook\n");
#endif
                wm_open(WK_TERM);
                {
                    static const char *cmds[] = {
#ifdef CU_SELFTEST
                        "ls /", "wc /t.txt", "wc -l /t.txt",
                        "head -n 2 /t.txt", "tail -n 1 /t.txt",
                        "grep alpha /t.txt", "grep -c alpha /t.txt",
                        "grep -n -i ALPHA /t.txt",
                        "sort /t.txt", "sort -r /t.txt", "nl /t.txt",
                        "tac /t.txt", "rev /t.txt",
                        "cut -d ' ' -f 1 /t.txt",
                        "tr a-z A-Z /t.txt",
                        "file /t.txt", "stat /t.txt",
                        "sha256sum /t.txt", "cksum /t.txt",
                        "base64 /t.txt",
                        "hexdump -n 32 /t.txt",
                        "basename /a/b/c.txt", "dirname /a/b/c.txt",
                        "realpath t.txt",
                        "touch /t2.txt", "cp /t.txt /t3.txt",
                        "mv /t3.txt /t4.txt", "cmp /t.txt /t4.txt",
                        "diff /t.txt /t2.txt",
                        "find / -name t*.txt", "du /", "tree /",
                        "strings /t.txt", "tr a-z A-Z /t.txt",
                        "sed s/alpha/OMEGA/ /t.txt",
                        "sed s/o/0/g /t.txt",
                        "sed /delta/d /t.txt",
                        "comm /t.txt /t4.txt",
                        "paste /t.txt /t.txt",
                        "column -s ' ' /t.txt",
                        "fmt -w 30 /t.txt",
                        "dd if=/t.txt skip=6 count=13",
                        "dos2unix /t2.txt",
                        "printf %s=%d\\n answer 42",
                        "seq 3", "seq 2 2 8",
                        "test -f /t.txt", "test -d /etc", "test 3 -lt 5",
                        "test abc = abc", "test -e /nope",
                        "true", "false",
                        "hostname", "arch", "nproc", "free",
                        "lscpu", "lsmem", "lsblk", "lspci",
                        "cal 7 2026", "sync",
                        "ip", "route",
                        "tar -t /initrd.tar",
                        "id", "groups", "umask", "chmod 755 /t.txt",
                        "man grep", "whatis sort", "apropos compress",
                        "unzstd /nothere.zst",
#endif
                        "ai", "whoami", "pwd", "users",
                        "useradd bob hunter2",
                        "users",
                        "userdel kairav",         /* refused: in use      */
                        "useradd bob other",      /* refused: name taken  */
                        "useradd BOB x",          /* refused: bad name    */
                        "users",
                        0
                    };
#ifdef CU_SELFTEST
                    fs_write_file("/t.txt",
                        "alpha bravo charlie\n"
                        "delta echo foxtrot\n"
                        "alpha again\n"
                        "delta echo foxtrot\n", 70);
#endif
                    for (int c = 0; cmds[c]; c++) {
                        serial_puts("[usertest] $ ");
                        serial_puts(cmds[c]);
                        serial_puts("\n");
                        char line[64];
                        str_copy(line, cmds[c], sizeof(line));
                        term_exec(line);
                    }
                    /* Leave a mark on the session, so what survives a
                     * logout can be checked rather than assumed. */
                    char mk[64];
                    str_copy(mk, "echo leak-canary > /tmp-canary.txt",
                             sizeof(mk));
                    term_exec(mk);
                    serial_puts("[usertest] admin=");
                    serial_put_dec((uint32_t)(user_is_admin(user_current) ? 1 : 0));
                    serial_puts("\n[usertest] shell history now holds ");
                    serial_put_dec((uint32_t)term_hist_count);
                    serial_puts(" entries, cwd ");
                    serial_puts(term_cwd);
                    serial_puts("\n[usertest] done\n");
                    want_logout = 1;
                }
#endif
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                serial_puts("[vextro] login: ");
                serial_puts(user_name_of(user_current));
                serial_puts(user_is_admin(user_current) ? " (admin)\n" : "\n");
                {
                    char note[NOTIFY_TEXT];
                    str_copy(note, "Signed in as ", sizeof(note));
                    str_append(note, user_name_of(user_current), sizeof(note));
                    if (user_is_admin(user_current))
                        str_append(note, " (administrator)", sizeof(note));
                    notify_push(NOTE_GOOD, note);
                }
                vga_flip(vram, w, h, pitch_px);
                frame_idle();
                continue;
            }
            vga_flip(vram, w, h, pitch_px);
            frame_idle();
            continue;
        }

        /* Up and down pick an account when there is more than one. */
        if (login_stage == LOGIN_PASSWORD && user_count > 1) {
            if (login_nav_up   && login_sel > 0) login_sel--;
            if (login_nav_down && login_sel + 1 < user_count) login_sel++;
        }
        login_nav_up = login_nav_down = 0;

        /* Choose prompt based on where in the sequence we are */
        static char prompt_buf[96];
        const char *prompt;
        switch (login_stage) {
        case LOGIN_NEW_NAME:
            prompt = "Vextro 9 - Create an account. Username:";
            break;
        case LOGIN_NEW_PW:
            prompt = "Choose a password:";
            break;
        case LOGIN_NEW_CONFIRM:
            prompt = "Type it once more:";
            break;
        default:
            str_copy(prompt_buf, "Password for ", sizeof(prompt_buf));
            str_append(prompt_buf, user_name_of(login_sel), sizeof(prompt_buf));
            str_append(prompt_buf, ":", sizeof(prompt_buf));
            prompt = prompt_buf;
            break;
        }
        if (login_msg[0]) prompt = login_msg;

        /* Mouse-reactive demoscene vortex + login interface */
        login_render(backbuf, w, h, mouse_x, mouse_y,
                     typed_text, mouse_buttons, prompt);

        /* The accounts on this machine, so one can be picked. */
        if (login_stage == LOGIN_PASSWORD && user_count > 1)
            login_draw_users(backbuf, w, h, mouse_x, mouse_y,
                             mouse_buttons);

        /* The administrator choice, while the account is being made. */
        if (login_stage != LOGIN_PASSWORD)
            login_draw_admin_check(backbuf, w, h, mouse_x, mouse_y,
                                   mouse_buttons);
        else
            login_draw_notice(backbuf, w, h);

        /* 1-pixel metallic gold border (outermost frame) */
        fill_rect(w, 0,     0,     w, 1, COLOR_GOLD);
        fill_rect(w, 0,     h - 1, w, 1, COLOR_GOLD);
        fill_rect(w, 0,     0,     1, h, COLOR_GOLD);
        fill_rect(w, w - 1, 0,     1, h, COLOR_GOLD);

        vga_flip(vram, w, h, pitch_px);

        frame_idle();  /* sleep until next IRQ */
    }
}
