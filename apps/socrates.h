#ifndef SOCRATES_H
#define SOCRATES_H

/*
 * Socrates BSD 9 — Native Hybrid Application Header
 *
 * Provides inline assembly wrappers around the kernel's int 0x80
 * syscall gateway.  Include this in any standalone ELF64 app to
 * access OS services without linking against kernel internals.
 *
 * Syscall ABI:
 *   RAX = syscall number
 *   RDI = arg0   RSI = arg1   RDX = arg2
 *   Invoked via:  int $0x80
 *   All GPRs preserved across the call (kernel ISR saves/restores).
 */

typedef unsigned int      uint32_t;
typedef int               int32_t;
typedef unsigned long     uint64_t;
typedef unsigned long     uintptr_t;

/* ---- Syscall 1: sys_print ----
 * Print a null-terminated string to the terminal canvas. */
static inline void os_print(const char *str) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((uint64_t)1), "D"(str)
        : "memory"
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
        : "memory"
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
        : "memory"
    );
}

#endif /* SOCRATES_H */
