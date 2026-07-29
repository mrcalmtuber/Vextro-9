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

#define COLOR_BLACK  0x000000u
#define COLOR_WHITE  0xFFFFFFu
#define COLOR_GOLD   0xD4AF37u

/* Typed text buffer for keyboard display */
#define TEXT_BUF_SIZE 128
static char typed_text[TEXT_BUF_SIZE];
static uint32_t typed_len = 0;

/* First-boot password registration */
static int is_first_boot = 1;
static char system_password[TEXT_BUF_SIZE];
static uint32_t system_password_len = 0;

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

static void draw_cursor(uint32_t bw, uint32_t bh) {
    uint32_t cx = (uint32_t)mouse_x;
    uint32_t cy = (uint32_t)mouse_y;
    for (uint32_t row = 0; row < 18; row++) {
        const char *line = CURSOR_IMG[row];
        for (uint32_t col = 0; col < 12; col++) {
            char c = line[col];
            if (c == ' ') continue;
            uint32_t px = cx + col;
            uint32_t py = cy + row;
            if (px < bw && py < bh)
                backbuf[py * bw + px] = (c == 'X') ? 0x000000u : 0xFFFFFFu;
        }
    }
}

/*
 * Previously presented frame, so a row that did not change can be left
 * alone.  Comparing two rows in RAM is far cheaper than writing one to
 * the panel, and on a desktop that is mostly still, almost every row is
 * unchanged.
 */
static uint32_t prevbuf[BUF_MAX_W * BUF_MAX_H];
static int      prev_valid = 0;

static int row_same(const uint32_t *a, const uint32_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
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
static void vga_flip(volatile uint32_t *vram,
                     uint32_t w, uint32_t h, uint32_t pitch_px) {
    if (gfx_force_full_flip) {          /* someone else wrote the panel */
        gfx_force_full_flip = 0;
        prev_valid = 0;
    }

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;

        if (prev_valid && row_same(src, cmp, w)) continue;

        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = 0; col < w; col++) {
            dst[col] = src[col];
            cmp[col] = src[col];
        }
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

    /* Primary filesystem: exFAT on the ATA disk, with FAT32 and the tar
     * ramdisk as fallbacks (writable, persistent) */
    ata_init();
    fs_mount();

    /* App store: load the shipped catalog and the installed-app registry
     * so the dock and the Apps menu already know about installed apps */
    store_init();

    /* If a model is sitting on the volume, start pulling it in.  The
     * work itself happens in the render loop, so this only opens the
     * file — the desktop comes up while the weights are still arriving. */
    ai_autoload_start();

    /* Restore the master keycode saved on a previous boot */
    {
        uint64_t klen = 0;
        const void *kd = fs_read_file("/keycode.sys", &klen);
        if (kd && klen > 0 && klen < TEXT_BUF_SIZE) {
            const char *kp = (const char *)kd;
            for (uint64_t i = 0; i < klen; i++)
                system_password[i] = kp[i];
            system_password_len = (uint32_t)klen;
            system_password[system_password_len] = '\0';
            is_first_boot = 0;
        }
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

            desktop_render(backbuf, w, h, mouse_x, mouse_y, mouse_buttons);
            draw_cursor(w, h);
            vga_flip(vram, w, h, pitch_px);
            __asm__ volatile("hlt");
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
            } else if (ch >= 0x20 && ch < 0x7F &&
                       typed_len < TEXT_BUF_SIZE - 1) {
                typed_text[typed_len++] = ch;
            }
        }
        typed_text[typed_len] = '\0';

        if (enter_pressed && typed_len > 0) {
            if (is_first_boot) {
                /* Save the typed string as the master password */
                for (uint32_t i = 0; i < typed_len && i < TEXT_BUF_SIZE - 1; i++)
                    system_password[i] = typed_text[i];
                system_password_len = typed_len;
                system_password[system_password_len] = '\0';

                is_first_boot = 0;
                typed_len = 0;
                typed_text[0] = '\0';

                /* persist the keycode so it survives reboots */
                fs_write_file("/keycode.sys", system_password,
                              system_password_len);

                confirm_active = 1;
                confirm_tick = 0;

                vga_flip(vram, w, h, pitch_px);
                __asm__ volatile("hlt");
                continue;
            } else {
                /* Normal login: check against saved password */
                int match = (typed_len == system_password_len);
                if (match) {
                    for (uint32_t i = 0; i < system_password_len; i++) {
                        if (typed_text[i] != system_password[i]) { match = 0; break; }
                    }
                }
                if (!match) {
                    melt_active = 1;
                    melt_tick = 0;
                    vga_flip(vram, w, h, pitch_px);
                    __asm__ volatile("hlt");
                    continue;
                }
                /* Password matched — transition to desktop */
                typed_len = 0;
                typed_text[0] = '\0';
                desktop_mode = 1;
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                vga_flip(vram, w, h, pitch_px);
                __asm__ volatile("hlt");
                continue;
            }
        }

        /* Choose prompt based on boot state */
        const char *prompt;
        if (is_first_boot)
            prompt = "Socrates BSD 9 - Initialize System. Choose Master Keycode:";
        else
            prompt = "Socrates BSD 9 - Enter Keycode:";

        /* Mouse-reactive demoscene vortex + login interface */
        login_render(backbuf, w, h, mouse_x, mouse_y,
                     typed_text, mouse_buttons, prompt);

        /* 1-pixel metallic gold border (outermost frame) */
        fill_rect(w, 0,     0,     w, 1, COLOR_GOLD);
        fill_rect(w, 0,     h - 1, w, 1, COLOR_GOLD);
        fill_rect(w, 0,     0,     1, h, COLOR_GOLD);
        fill_rect(w, w - 1, 0,     1, h, COLOR_GOLD);

        draw_cursor(w, h);

        vga_flip(vram, w, h, pitch_px);

        __asm__ volatile("hlt");  /* sleep until next IRQ */
    }
}
