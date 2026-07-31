#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "idt.h"
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
#include "boot_animation.h"

/*
 * Raw int 0x80 ISR stub — saves caller registers, forwards to C dispatch.
 * Stack after CPU push: [SS, RSP, RFLAGS, CS, RIP] (40 bytes above our SP).
 * We push 10 GPRs (80 bytes), align the stack, call syscall_dispatch, restore.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl int80_stub\n"
    ".type int80_stub, @function\n"
    "int80_stub:\n"
    "  push %rax\n"
    "  push %rcx\n"
    "  push %rdx\n"
    "  push %rsi\n"
    "  push %rdi\n"
    "  push %r8\n"
    "  push %r9\n"
    "  push %r10\n"
    "  push %r11\n"
    "  push %rbp\n"
    "  mov  %rsp, %rbp\n"
    "  and  $-16, %rsp\n"
    "  mov  72(%rbp), %rdi\n"    /* rax  = syscall number  → arg 1 */
    "  mov  40(%rbp), %rsi\n"    /* rdi  = user arg0       → arg 2 */
    "  mov  48(%rbp), %rdx\n"    /* rsi  = user arg1       → arg 3 */
    "  mov  56(%rbp), %rcx\n"    /* rdx  = user arg2       → arg 4 */
    "  call syscall_dispatch\n"
    "  mov  %rbp, %rsp\n"
    "  pop  %rbp\n"
    "  pop  %r11\n"
    "  pop  %r10\n"
    "  pop  %r9\n"
    "  pop  %r8\n"
    "  pop  %rdi\n"
    "  pop  %rsi\n"
    "  pop  %rdx\n"
    "  pop  %rcx\n"
    "  pop  %rax\n"
    "  iretq\n"
    ".popsection\n"
);

extern void int80_stub(void);

/*
 * 64-bit SYSCALL entry stub — target of IA32_LSTAR MSR.
 * On SYSCALL: CPU saves RIP→RCX, RFLAGS→R11, masks RFLAGS via SFMASK.
 * RSP is NOT switched (ring-0 → ring-0), so we run on the caller's stack.
 * We avoid SYSRETQ (forces CPL 3) and return via POPFQ + JMP *%RCX.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl syscall_entry\n"
    ".type syscall_entry, @function\n"
    "syscall_entry:\n"
    "  push %rax\n"
    "  push %rcx\n"          /* return RIP (saved by CPU) */
    "  push %rdx\n"
    "  push %rsi\n"
    "  push %rdi\n"
    "  push %r8\n"
    "  push %r9\n"
    "  push %r10\n"
    "  push %r11\n"          /* return RFLAGS (saved by CPU) */
    "  push %rbp\n"
    "  mov  %rsp, %rbp\n"
    "  and  $-16, %rsp\n"
    "  mov  72(%rbp), %rdi\n"   /* rax  = syscall number → arg 1 */
    "  mov  40(%rbp), %rsi\n"   /* rdi  = arg0          → arg 2 */
    "  mov  48(%rbp), %rdx\n"   /* rsi  = arg1          → arg 3 */
    "  mov  56(%rbp), %rcx\n"   /* rdx  = arg2          → arg 4 */
    "  call syscall_dispatch\n"
    "  mov  %rbp, %rsp\n"
    "  pop  %rbp\n"
    "  pop  %r11\n"
    "  pop  %r10\n"
    "  pop  %r9\n"
    "  pop  %r8\n"
    "  pop  %rdi\n"
    "  pop  %rsi\n"
    "  pop  %rdx\n"
    "  pop  %rcx\n"
    "  pop  %rax\n"
    "  push %r11\n"
    "  popfq\n"              /* restore RFLAGS (including IF) from R11 */
    "  jmp  *%rcx\n"         /* return to caller via saved RIP */
    ".popsection\n"
);

extern void syscall_entry(void);

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
static char pending_name[USER_NAME_MAX];
static char pending_pw[TEXT_BUF_SIZE];
static int  login_nav_up = 0, login_nav_down = 0;

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

/* HAL status flags */
static int hal_ps2_present = 1;

/* ---- PIT timer (IRQ0) — keeps render loop alive when mouse is idle ---- */
__attribute__((interrupt))
static void irq0_handler(interrupt_frame_t *f) {
    (void)f;
    sys_ticks++;
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
static uint64_t frame_cur_px = 0;     /* written by the overlay */

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
    serial_puts(" px + cursor ");
    serial_put_dec((uint32_t)(frame_cur_px / 120));
    serial_puts(" px\n");

    frame_n = 0;
    frame_render_cy = frame_flip_cy = frame_idle_cy = 0;
    frame_wr_px = frame_cur_px = 0;
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
 * The pointer is an overlay, not part of the picture.
 *
 * It used to be stamped into the back buffer along with everything else,
 * which meant moving it *changed the frame*. The flip compares against
 * the previous frame and writes the span from the leftmost change to the
 * rightmost, so a pointer crossing the screen dirtied the rectangle it
 * left, the one it arrived at, and everything between — and those writes
 * go straight into live scanout with no vblank to hide behind. On an
 * otherwise still desktop that band was the only thing being rewritten,
 * every frame, which is exactly the shimmer that follows the cursor.
 *
 * Keeping it out of `backbuf` means cursor motion dirties nothing at all:
 * the flip no longer sees the pointer, so `desktop_render`'s output is
 * identical whether the mouse moved or not.
 *
 * There is no hardware cursor plane to hand this to. QEMU's `-vga std`
 * has none, and igpu.h is deliberately blitter-only, so the sprite is
 * composited into scanout directly, after the flip.
 *
 * Nothing needs saving from underneath. `backbuf` *is* the clean desktop
 * — by construction it never contains the pointer — so erasing means
 * copying those pixels back out of it.
 */
static int32_t cur_prev_x = 0, cur_prev_y = 0;
static int     cur_drawn  = 0;

/* Repaint a rectangle of the true desktop over whatever is in scanout. */
static void cursor_restore(volatile uint32_t *vram, uint32_t w, uint32_t h,
                           uint32_t pitch_px, int32_t rx, int32_t ry) {
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    for (uint32_t row = 0; row < CURSOR_H; row++) {
        uint32_t py = (uint32_t)ry + row;
        if (py >= h) break;
        const uint32_t   *src = backbuf + py * w;
        volatile uint32_t *dst = vram + py * pitch_px;
        for (uint32_t col = 0; col < CURSOR_W; col++) {
            uint32_t px = (uint32_t)rx + col;
            if (px >= w) break;
            dst[px] = src[px];
            frame_cur_px++;
        }
    }
}

/*
 * Erase the pointer where it was, draw it where it is.
 *
 * Called after the flip. The redraw is unconditional because the flip may
 * have just written desktop content over part of the sprite; the erase is
 * not, because when the pointer has not moved there is nothing stale to
 * clean up.
 */
static void cursor_overlay_flush(volatile uint32_t *vram, uint32_t w,
                                 uint32_t h, uint32_t pitch_px) {
    int32_t cx = mouse_x, cy = mouse_y;

    if (cur_drawn && (cur_prev_x != cx || cur_prev_y != cy))
        cursor_restore(vram, w, h, pitch_px, cur_prev_x, cur_prev_y);

    for (uint32_t row = 0; row < CURSOR_H; row++) {
        uint32_t py = (uint32_t)(cy + (int32_t)row);
        if (py >= h) break;
        const char        *line = CURSOR_IMG[row];
        volatile uint32_t *dst  = vram + py * pitch_px;
        for (uint32_t col = 0; col < CURSOR_W; col++) {
            char c = line[col];
            if (c == ' ') continue;
            uint32_t px = (uint32_t)(cx + (int32_t)col);
            if (px >= w) continue;
            dst[px] = (c == 'X') ? 0x000000u : 0xFFFFFFu;
            frame_cur_px++;
        }
    }

    cur_prev_x = cx;
    cur_prev_y = cy;
    cur_drawn  = 1;
}


/*
 * Copy the back buffer to the panel.
 *
 * This writes straight into live scanout with nothing to synchronise
 * against, so the host can sample a row that has already been updated
 * next to one that has not — the seam that shows up as tearing.  There
 * is no vblank to wait for here, but skipping unchanged rows shrinks
 * that window to whatever actually moved, and when the screen is
 * completely still it writes nothing at all.
 */
/*
 * Push the back buffer at the panel, touching as little as possible.
 *
 * Three things happen in one pass, and folding them together is the
 * point: finding what changed, copying it, and recording where it was.
 *
 * The row scan used to answer only "is this row identical?" and then
 * copy the whole row if not. A moving pointer changes twelve pixels of a
 * 1280-wide row, so that copied a hundred times more than it needed to —
 * and, worse, it learned nothing it could pass on. Scanning inward from
 * both ends of the row costs the same comparisons, copies only the span
 * between them, and yields a bounding box for free.
 *
 * That box is what makes the difference on virtio-gpu, where presenting
 * is an explicit transfer of guest pixels into the host's copy of the
 * resource followed by a flush. Handing it the whole screen every frame
 * means moving four megabytes to show a cursor that moved four pixels.
 */
static void vga_flip(volatile uint32_t *vram,
                     uint32_t w, uint32_t h, uint32_t pitch_px) {
    if (gfx_force_full_flip) {          /* someone else wrote the panel */
        gfx_force_full_flip = 0;
        prev_valid = 0;
        /* Whatever was on the panel is gone, pointer included, so there
         * is no stale sprite to erase — and erasing would write the
         * desktop over a rectangle the full flip is about to repaint. */
        cur_drawn = 0;
    }

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;

        uint32_t c0 = 0, c1 = w;
        if (prev_valid) {
            while (c0 < w && src[c0] == cmp[c0]) c0++;
            if (c0 == w) continue;                  /* row unchanged */
            while (c1 > c0 && src[c1 - 1] == cmp[c1 - 1]) c1--;
        }

        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = c0; col < c1; col++) {
            dst[col] = src[col];
            cmp[col] = src[col];
        }
        frame_wr_px += c1 - c0;
    }
    prev_valid = 1;
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

static void boot_frame_delay(void) {
    /* ~41.67 ms per frame (24 fps) via PIT channel 2 one-shot, no IRQ needed.
       PIT freq = 1,193,182 Hz → 1193182/24 ≈ 49716 ticks. */
    outb(0x43, 0xB0);          /* ch2, lobyte/hibyte, mode 0 (one-shot) */
    outb(0x42, 0x34);          /* low byte of 49716 */
    outb(0x42, 0xC2);          /* high byte */
    uint8_t gate = inb(0x61);
    outb(0x61, gate & 0xFE);   /* gate low — reset */
    outb(0x61, (gate | 1) & 0xFD); /* gate high, speaker off — start count */
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
    uint32_t scale_x = scr_w / BOOT_ANIM_W;
    uint32_t scale_y = scr_h / BOOT_ANIM_H;
    uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale == 0) scale = 1;

    uint32_t dst_w = BOOT_ANIM_W * scale;
    uint32_t dst_h = BOOT_ANIM_H * scale;
    uint32_t off_x = (scr_w - dst_w) / 2;
    uint32_t off_y = (scr_h - dst_h) / 2;

    const uint16_t *frames = (const uint16_t *)boot_anim_data;

    for (uint32_t f = 0; f < BOOT_ANIM_FRAME_COUNT; f++) {
        const uint16_t *src = frames + f * BOOT_ANIM_W * BOOT_ANIM_H;

        for (uint32_t sy = 0; sy < BOOT_ANIM_H; sy++) {
            for (uint32_t sx = 0; sx < BOOT_ANIM_W; sx++) {
                uint16_t c = src[sy * BOOT_ANIM_W + sx];
                uint32_t r = (c >> 11) & 0x1F;
                uint32_t g = (c >> 5)  & 0x3F;
                uint32_t b = c & 0x1F;
                uint32_t pixel = (r << 19) | (g << 10) | (b << 3);

                for (uint32_t dy = 0; dy < scale; dy++)
                    for (uint32_t dx = 0; dx < scale; dx++)
                        vram[(off_y + sy * scale + dy) * pitch_px +
                             (off_x + sx * scale + dx)] = pixel;
            }
        }
        boot_frame_delay();
        if (boot_key_pressed()) break;   /* any key skips the rest */
    }

    /* Clear screen to black after animation finishes */
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

    /* Read the kernel code segment selector */
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));

    /* Floating point, before anything that might use it */
    fpu_init();

    /* Build IDT: remap PIC, fill all 256 gates with no-op stubs */
    idt_init(cs);

    /* Register int 0x80 syscall gateway for native hybrid apps */
    idt_set_gate(0x80,
                 (void (*)(interrupt_frame_t *))(uintptr_t)int80_stub, cs);

    idt_load();

    /* ---- Initialize 64-bit SYSCALL/SYSRET MSRs ----
     * IA32_EFER.SCE  — enable the SYSCALL instruction
     * IA32_STAR       — kernel CS/SS selectors for privilege transition
     * IA32_LSTAR      — 64-bit syscall entry point (our assembly stub)
     * IA32_FMASK      — RFLAGS bits cleared on SYSCALL entry (mask IF)
     */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);         /* set SCE bit          */
    wrmsr(MSR_STAR, (uint64_t)cs << 32);           /* kernel CS in [47:32] */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);                      /* mask IF on entry     */

    /* HAL: safely probe and initialize PS/2 devices */
    hal_init_devices(cs, (int32_t)w, (int32_t)h);

    /* Start PIT at ~60 Hz so the render loop runs even when mouse is idle */
    pit_init(cs);

    /* The TSC frequency is not discoverable, but the PIT was just set to a
     * known rate — so measure one against the other while nothing else is
     * competing for the machine. Everything that paces itself by time
     * rather than by a fixed count depends on this. */
    tsc_calibrate();

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
    serial_puts("[socrates] app selftest: running /hello\n");
    execute_bin_internal("/hello", 0);
    serial_puts("[socrates] app selftest: done\n");
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
    if (user_count == 0 && users_migrate_keycode())
        serial_puts("[socrates] users: migrated /keycode.sys to 'admin'\n");

    if (user_count == 0) {
        login_stage = LOGIN_NEW_NAME;
        serial_puts("[socrates] users: no accounts, first-run setup\n");
    } else {
        login_stage = LOGIN_PASSWORD;
        login_sel = 0;
        serial_puts("[socrates] users: ");
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

        /* The model is far larger than any static buffer, so give the
         * inference arena the biggest usable region Limine reports. */
        if (hhdm_request.response != NULL) {
            uint64_t best_base = 0, best_len = 0;
            for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
                struct limine_memmap_entry *e = memmap_request.response->entries[i];
                if (e->type != LIMINE_MEMMAP_USABLE) continue;
                if (e->length > best_len) { best_len = e->length; best_base = e->base; }
            }
            if (best_len > (16ull << 20))
                llm_arena_init((void *)(uintptr_t)(hhdm_request.response->offset
                                                   + best_base), best_len);
        }
    }

    /* Unmask hardware interrupts */
    __asm__ volatile("sti" ::: "memory");

    /* Render loop — wakes on each interrupt, redraws, sleeps again */
    while (1) {
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
            __asm__ volatile("hlt");
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
            __asm__ volatile("hlt");
            continue;
        }

        if (auto_login && !desktop_mode) {
            auto_login = 0;
            desktop_mode = 1;
            serial_puts("[socrates] AUTO_LOGIN: skipped the login screen\n");
            for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
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
                serial_puts("[socrates] AUTO_WIKI: article open\n");
            else
                serial_puts("[socrates] AUTO_WIKI: load failed\n");
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
            serial_puts("[socrates] logout: ");
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
            __asm__ volatile("hlt");
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

            uint64_t t0 = cycle_now();
            desktop_render(backbuf, w, h, mouse_x, mouse_y, mouse_buttons);
            uint64_t t1 = cycle_now();
            vga_flip(vram, w, h, pitch_px);
            cursor_overlay_flush(vram, w, h, pitch_px);
            uint64_t t2 = cycle_now();
            frame_render_cy += t1 - t0;
            frame_flip_cy   += t2 - t1;
            frame_report();
            __asm__ volatile("hlt");
            frame_idle_cy += cycle_now() - t2;
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
                } else if (user_add(pending_name, pending_pw,
                                    user_count == 0) < 0) {
                    str_copy(login_msg, user_err, sizeof(login_msg));
                    login_stage = LOGIN_NEW_PW;
                } else {
                    /* First account on the machine is the administrator. */
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
                    __asm__ volatile("hlt");
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
                wm_open(WK_TERM);
                {
                    static const char *cmds[] = {
                        "whoami", "pwd", "users",
                        "useradd bob hunter2",
                        "users",
                        "userdel kairav",         /* refused: in use      */
                        "useradd bob other",      /* refused: name taken  */
                        "useradd BOB x",          /* refused: bad name    */
                        "users",
                        0
                    };
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
                serial_puts("[socrates] login: ");
                serial_puts(user_name_of(user_current));
                serial_puts(user_is_admin(user_current) ? " (admin)\n" : "\n");
                vga_flip(vram, w, h, pitch_px);
                __asm__ volatile("hlt");
                continue;
            }
            vga_flip(vram, w, h, pitch_px);
            __asm__ volatile("hlt");
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
            prompt = "Socrates BSD 9 - Create an account. Username:";
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

        /* 1-pixel metallic gold border (outermost frame) */
        fill_rect(w, 0,     0,     w, 1, COLOR_GOLD);
        fill_rect(w, 0,     h - 1, w, 1, COLOR_GOLD);
        fill_rect(w, 0,     0,     1, h, COLOR_GOLD);
        fill_rect(w, w - 1, 0,     1, h, COLOR_GOLD);

        vga_flip(vram, w, h, pitch_px);
        cursor_overlay_flush(vram, w, h, pitch_px);

        __asm__ volatile("hlt");  /* sleep until next IRQ */
    }
}
