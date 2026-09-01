#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/* C++ reaches these now.
 *
 * libcxx/ compiles against this same library, and a C++ compiler mangles
 * every name it sees unless told not to -- so without this the C++ side
 * would fail to link against `malloc` and find `_Z6mallocm` missing.
 * Placed immediately after the include guard rather than after the
 * #includes below it, which is safe here because everything this header
 * includes is either one of the compiler's own type-only headers or one
 * of ours, and both want the same treatment. */
#ifdef __cplusplus
extern "C" {
#endif

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
#include <errno.h>

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
#define SYS_RANDOM          25
#define SYS_FUTEX           26
#define SYS_FS_WRITE        27
#define SYS_REG_SET         28
#define SYS_BLK_WRITE       29

/*
 * 30-39: memory that is not the break, and threads that are not forks.
 *
 * The kernel's copy of these numbers and the reasoning behind each of
 * them is in src/syscall.h. What matters on this side is which of them
 * return a value, because that decides which door they may use: all of
 * them do, so all of them go through SYSCALL. None of these may ever be
 * called through `int 0x80` -- the legacy gate preserves RAX by an old
 * promise to binaries already on disk, so mmap through it would return
 * the number 30 rather than an address.
 */
#define SYS_MMAP            30
#define SYS_MUNMAP          31
#define SYS_MPROTECT        32
#define SYS_CLONE           33
#define SYS_THREAD_EXIT     34
#define SYS_GETTID          35
#define SYS_SET_FSBASE      36
#define SYS_CLOCK           37
#define SYS_EXIT_GROUP      38
#define SYS_NANOSLEEP       39

/*
 * 40-58: descriptors, and the two things one can name.
 *
 * The kernel's copy of these numbers, and the argument for every one of
 * them, is in src/syscall.h. What matters on this side is the *return
 * convention*, because it is not the one the calls above use.
 *
 * Everything below 40 answers -1 and nothing else. These answer the
 * negated error number — -ENOENT, -EACCES, -EISDIR — and a non-negative
 * value on success, which is what Linux's system calls do and therefore
 * what the code being ported already expects its C library to unpack.
 * __syscall_ret below is where the unpacking happens, once, so that no
 * wrapper has to remember.
 */
#define SYS_OPEN            40
#define SYS_READ            41
#define SYS_CLOSE           42
#define SYS_LSEEK           43
#define SYS_STAT            44
#define SYS_FSTAT           45
#define SYS_GETDENTS        46
#define SYS_UNLINK          47
#define SYS_MKDIR           48
#define SYS_FSYNC           49
#define SYS_FTRUNCATE       50
#define SYS_SOCKET          51
#define SYS_CONNECT         52
#define SYS_CONNECT_HOST    53
#define SYS_SEND            54
#define SYS_RECV            55
#define SYS_SOCKOPT         56
#define SYS_SHUTDOWN        57
#define SYS_RESOLVE         58

/*
 * ===== 59: the calendar =====
 *
 * Seconds since 1970-01-01, read from the CMOS clock.
 *
 * Its own call rather than a clock id on SYS_CLOCK, because it is a
 * different quantity from the one that call answers. SYS_CLOCK returns
 * the scheduler tick: monotonic, starting at zero when the machine
 * boots, and the right thing to measure an interval with. This returns
 * a point on a calendar, which can be earlier than the last one it
 * returned if somebody sets the clock, and which is only as accurate as
 * the firmware that set it. Two quantities with different guarantees
 * should not share a system call number and be told apart by an
 * argument.
 *
 * Free of any permission check, which is worth saying out loud: the
 * time of day is not a secret, every process can already see it in the
 * taskbar, and a call that fails would only push programs back onto the
 * monotonic count they were using before.
 */
#define SYS_WALLCLOCK       59

/*
 * ===== 60-64: a second process =====
 *
 * SYS_FORK has been number 23 since ring 3 existed and was half of a
 * pair with no other half — a child could only run the code its parent
 * was already running, and when it stopped nobody could be told. These
 * are the other half. libc/process.c is what a program calls instead.
 */
#define SYS_EXECVE          60
#define SYS_WAIT4           61
#define SYS_DUP             62
#define SYS_DUP2            63
#define SYS_PERSONALITY     64
#define SYS_PIPE2           65
#define SYS_POLL            66
#define SYS_SOCKETPAIR      67

/*
 * ===== and the calls that only exist in Linux's numbering =====
 *
 * Signals are Linux's shape down to the layout of the structures they
 * take, so the kernel gives them Linux numbers and no native ones — a
 * second door into one room would be two things to keep in step. A
 * program reaches them by adding the bias, which is what the kernel
 * reads as "this number is written in Linux's numbering", and which
 * works from a process that has not given up the native numbers.
 * include/vls.h is the kernel's side and explains why there are two
 * mechanisms rather than one.
 *
 * The bias is a bit rather than an offset so the arithmetic is exact in
 * both directions and a Linux number can never be mistaken for a native
 * one by an off-by-one.
 */
#define VLS_CALL_BIAS       0x40000000L

#define VLS_rt_sigaction    13
#define VLS_rt_sigprocmask  14
#define VLS_kill            62
#define VLS_wait4           61
#define VLS_tgkill         234
#define VLS_getppid        110

/* futex operations. See <vxmutex.h> for what to build on them. */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1

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

/*
 * ---- turning a kernel answer into a C answer ----
 *
 * The calls from 40 upwards report failure as a negated error number.
 * Every C function built on them reports it as -1 with the reason in
 * errno. This is that conversion, and it exists exactly once so that
 * eighteen wrappers do not each carry their own copy of the range test.
 *
 * The range is what makes it unambiguous. A value in [-4095, -1] is an
 * error; everything else is a result. That is why an mmap-style call
 * — which can legitimately return an address whose top bit is set —
 * could not use this convention and does not, and why nothing that does
 * use it returns a pointer.
 */
static inline long __syscall_ret(long r) {
    if (r < 0 && r >= -4095) {
        *__errno_location() = (int)-r;
        return -1;
    }
    return r;
}


#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSCALL_H */
