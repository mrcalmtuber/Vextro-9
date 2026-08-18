#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/*
 * The system call interface, as user space sees it.
 *
 * One header, shared by everything in libc, so that the register
 * assignment is written down once. It matches src/syscall.h on the other
 * side of the boundary:
 *
 *   RAX  number        RDI  arg0   RSI  arg1   RDX  arg2
 *                      R10  arg3   R8   arg4   R9   arg5
 *
 * R10 rather than RCX for the fourth argument, because the SYSCALL
 * instruction puts the return address in RCX and there is nowhere else
 * for it to go. That is why the kernel reads it from R10 too, and why a
 * program that used the ordinary C convention here would find its fourth
 * argument replaced by its own return address.
 *
 * SYSCALL rather than `int 0x80`: both doors lead to the same dispatcher
 * and always will, because apps/vextro.h has emitted the interrupt since
 * the beginning and binaries that use it are still installed. This is
 * simply the cheaper of the two.
 */

/*
 * ---- and the vector registers ----
 *
 * Every wrapper below declares all sixteen XMM registers clobbered, and
 * that is load-bearing rather than defensive.
 *
 * The kernel does not save floating-point state across a syscall. It
 * saves it across a *context switch*, which is a different thing: a
 * handler that fills a rectangle or measures a string runs on the
 * calling thread's own FPU state and is compiled with SSE available, so
 * it will use those registers and will not put them back. Without this
 * clobber list GCC would keep a live double in XMM across the
 * instruction and read back whatever the kernel left there — a loop that
 * computes a value and prints it would be correct on its first
 * iteration and wrong on every one after.
 *
 * The alternative is an FXSAVE and an FXRSTOR around every syscall,
 * which is 1024 bytes of copying to protect registers the caller usually
 * has nothing in. Declaring the truth is cheaper and just as correct.
 */
#include <stdint.h>

#define VX_CLOBBER_XMM \
    "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"

#define SYS_PRINT            1
#define SYS_DRAW_PIXEL       2
#define SYS_GET_MOUSE        3
#define SYS_EXIT             4
#define SYS_SBRK             5
#define SYS_WRITE            6
#define SYS_YIELD            7
#define SYS_TICKS            8
#define SYS_CANVAS           9
#define SYS_TTF_TEXT_WIDTH  20
#define SYS_TTF_DRAW_STRING 21
#define SYS_GFX_RECT        22
#define SYS_FORK            23
#define SYS_MEMINFO         24

static inline long __syscall0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

static inline long __syscall1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

static inline long __syscall2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

static inline long __syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

static inline long __syscall4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

#endif /* _SYS_SYSCALL_H */
