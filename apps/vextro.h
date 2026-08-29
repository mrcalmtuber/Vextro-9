#ifndef VEXTRO_H
#define VEXTRO_H

/*
 * Vextro 9 — the application header.
 *
 * Programs built against this run in ring 3, in an address space of
 * their own, with no way to reach the kernel except through the calls
 * below. That was not true when this file was written; it is now, and
 * almost nothing in here had to change for it, because a syscall is a
 * syscall whichever side of a privilege boundary it is issued from.
 *
 * ---- the two doors ----
 *
 * The os_* wrappers below still use `int 0x80`, and they will keep using
 * it. Binaries compiled against older copies of this header are
 * installed on real disks and emit that instruction, and the kernel's
 * legacy gate preserves every general-purpose register for them —
 * including RAX, which is why none of these wrappers can return a value.
 * That is not an oversight: GCC compiles two calls in a row to one
 * `mov eax, N` and two interrupts, so a gate that clobbered RAX would
 * turn the second call into a silent no-op.
 *
 * Anything that needs an answer back goes through the SYSCALL
 * instruction instead. <sys/syscall.h> in the C library has those, and
 * that is where new code should look.
 *
 * ---- and the vector registers ----
 *
 * Every wrapper declares all sixteen XMM registers clobbered. The kernel
 * saves floating-point state across a context *switch*, not across a
 * syscall: a handler that fills a rectangle runs on the calling thread's
 * own FPU state and is compiled with SSE available, so it uses those
 * registers and does not put them back. Without saying so, GCC would
 * keep a live double in XMM across the instruction and read back
 * whatever the kernel left there.
 */

#include <stdint.h>

/* Dimensions of the canvas that sys_draw_pixel writes into.  The kernel
 * clips anything outside, so these are the app's usable bounds. */
#define OS_CANVAS_W 598
#define OS_CANVAS_H 402

/* ---- Syscall 1: sys_print ----
 * Print a null-terminated string to the terminal canvas. */
static inline void os_print(const char *str) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)1), "D"(str)
        : "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
}

/* ---- Syscall 2: sys_draw_pixel ----
 * Draw a single pixel at (x, y) relative to the terminal client area
 * origin (top-left of the black canvas).  Color is 0xRRGGBB. */
static inline void os_draw_pixel(int x, int y, uint32_t color) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)2),
          "D"((uint64_t)(unsigned)x),
          "S"((uint64_t)(unsigned)y),
          "d"((uint64_t)color)
        : "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
}

/* ---- Syscall 3: sys_get_mouse ----
 * Fill a 4-element int32_t buffer with the current mouse state:
 *   out[0] = screen X    out[1] = screen Y
 *   out[2] = button mask  out[3] = reserved (0) */
static inline void os_get_mouse(int32_t *out) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)3), "D"(out)
        : "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
}

/* ---- Syscall 7: sys_yield ----
 * Hand the rest of this time slice back. A program that draws for a long
 * time and calls this occasionally stays responsive to the interface
 * around it; one that never calls it is preempted anyway, a thousand
 * times a second. */
static inline void os_yield(void) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)7)
        : "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
}

/* ---- Syscall 4: sys_exit ----
 * Ends the program. Returning from _start does the same thing — the
 * loader puts the address of an exit stub on the stack for exactly that
 * — so this is only for ending early. */
static inline void os_exit(int code) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)4), "D"((uint64_t)(unsigned)code)
        : "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
    for (;;) { }
}

/*
 * ---- Syscall 9: sys_canvas ----
 * Where this program's window actually lives.
 *
 * The canvas is mapped into the address space, so a program that fills
 * it can write to it directly instead of paying a syscall for every
 * pixel — which for a full 598x402 repaint is a quarter of a million
 * kernel entries. Returns a pointer and the dimensions; the pointer is
 * writable and never executable.
 *
 * Uses SYSCALL rather than the legacy gate because it has to fill in a
 * result. Requires nothing of the caller beyond a 24-byte buffer.
 */
static inline uint32_t *os_canvas(int *out_w, int *out_h) {
    uint64_t info[3] = { 0, 0, 0 };
    long rc;
    __asm__ volatile(
        "syscall"
        : "=a"(rc)
        : "a"((uint64_t)9), "D"(info)
        : "rcx", "r11", "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
    /*
     * Only -1 is a refusal.
     *
     * This used to read `if (rc != 0) return 0;`, and it was correct
     * when the kernel answered zero for success. It does not: the
     * service routine returns the canvas address in RAX as well as in
     * the buffer, so that a caller which treats this as a function
     * returning a pointer -- which is the natural way to write it --
     * gets one without reading the buffer at all.
     *
     * The two changes together meant every success was read as a
     * failure and this returned null, always. Nothing crashed, because
     * every caller checks: mandel falls back to a syscall per pixel, and
     * so quietly paid a quarter of a million kernel entries per frame
     * for the optimisation its own comment says it makes. That is the
     * shape of this class of bug -- a fallback that works is a bug that
     * does not announce itself.
     */
    if (rc == -1) return 0;
    if (out_w) *out_w = (int)info[1];
    if (out_h) *out_h = (int)info[2];
    return (uint32_t *)(uintptr_t)info[0];
}

/*
 * ---- Syscall 25: sys_random ----
 * Hardware entropy, without touching supervisor memory.
 *
 * Returns the number of bytes actually written, which is not always the
 * number asked for: the generator behind this can fail under load, and
 * a short read is reported rather than padded with something that looks
 * random and is not. A caller that needs all of it must check.
 *
 *     uint8_t key[32];
 *     if (os_random(key, sizeof key) != sizeof key)
 *         give_up();          // do NOT proceed with a partial key
 *
 * Uses SYSCALL rather than the legacy gate because it has to return a
 * count; see the note on syscall_dispatch_legacy in src/syscall.h for
 * why `int 0x80` cannot.
 */
static inline long os_random(void *buf, unsigned long len) {
    long rc;
    __asm__ volatile(
        "syscall"
        : "=a"(rc)
        : "a"((unsigned long)25), "D"(buf), "S"(len)
        : "rcx", "r11", "memory",
          "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    );
    return rc;
}

#endif /* VEXTRO_H */
