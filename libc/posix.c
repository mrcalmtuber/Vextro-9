/*
 * libc/posix.c — the rest of what a ported program expects to find.
 *
 * Time, character classes, the case-insensitive comparisons, setjmp, and
 * the handful of process calls that have an honest answer here. None of
 * it is difficult; all of it is missing until somebody writes it, and a
 * link failure on `tolower` is as fatal to a port as a missing kernel
 * feature.
 *
 * What is *not* here is as deliberate as what is. There is no open(),
 * no read(), no socket: ring 3 on this system has no file descriptors
 * at all, and a stub that returned -1 would let a library get several
 * layers into an operation before discovering it. A missing symbol fails
 * at the link, where it can be seen.
 */

#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <setjmp.h>
#include <sched.h>
#include <stdint.h>
#include <sys/syscall.h>

/* ===== TIME ===== */

uint64_t vx_millis(void) {
    return (uint64_t)__syscall0(SYS_TICKS);
}

int clock_gettime(clockid_t id, struct timespec *ts) {
    (void)id;
    if (!ts) { errno = EFAULT; return -1; }
    /*
     * Nanoseconds from the kernel rather than milliseconds converted
     * here, so that there is one place that knows the tick rate. A
     * program that did the conversion itself would carry a copy of a
     * constant the kernel is free to change.
     */
    uint64_t ns = (uint64_t)__syscall0(SYS_CLOCK);
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

int clock_getres(clockid_t id, struct timespec *ts) {
    (void)id;
    if (!ts) { errno = EFAULT; return -1; }
    /* One millisecond, which is the scheduler tick and therefore the
     * real resolution — reporting a nanosecond because that is the unit
     * would be a promise the clock cannot keep. */
    ts->tv_sec  = 0;
    ts->tv_nsec = 1000000;
    return 0;
}

time_t time(time_t *out) {
    /*
     * Seconds since boot, not since 1970.
     *
     * The machine has a real-time clock and the kernel reads it, but
     * nothing carries that reading across the system call boundary, so
     * there is no calendar available up here. Answering from the
     * monotonic count is correct for every use that measures an interval
     * and wrong for every use that formats a date — and a program that
     * formats this will print a day in January 1970, which is at least a
     * recognisable symptom rather than a plausible wrong date.
     */
    time_t t = (time_t)(vx_millis() / 1000);
    if (out) *out = t;
    return t;
}

clock_t clock(void) {
    /* CLOCKS_PER_SEC is 1000 here, so this is the tick count unchanged.
     * It measures elapsed time rather than processor time, which for a
     * thread that is mostly running is the same number and for one that
     * blocks is not. */
    return (clock_t)vx_millis();
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) { errno = EFAULT; return -1; }
    uint64_t ms = vx_millis();
    tv->tv_sec  = (time_t)(ms / 1000);
    tv->tv_usec = (suseconds_t)((ms % 1000) * 1000);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ms = (uint64_t)req->tv_sec * 1000 +
                  (uint64_t)(req->tv_nsec / 1000000);
    /* A request under a millisecond still costs a yield rather than
     * nothing: a caller that sleeps for a microsecond in a loop and gets
     * an immediate return has written a spin. */
    __syscall1(SYS_NANOSLEEP, (long)ms);
    /* Nothing can interrupt a sleeping thread here — there are no
     * signals — so the remainder is always zero and this always
     * succeeds. */
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { (time_t)seconds, 0 };
    nanosleep(&ts, 0);
    return 0;
}

int usleep(unsigned long usec) {
    struct timespec ts = { (time_t)(usec / 1000000ul),
                           (long)((usec % 1000000ul) * 1000ul) };
    return nanosleep(&ts, 0);
}

/* ===== PROCESS ===== */

pid_t fork(void)   { return (pid_t)__syscall0(SYS_FORK); }
pid_t getpid(void) { return (pid_t)__syscall0(SYS_GETTID); }
pid_t gettid(void) { return (pid_t)__syscall0(SYS_GETTID); }

void _exit(int status) {
    __syscall1(SYS_EXIT_GROUP, (long)status);
    for (;;) { }
}

void *sbrk(long delta) {
    long r = __syscall1(SYS_SBRK, delta);
    if (r == -1) { errno = ENOMEM; return (void *)-1; }
    return (void *)(uintptr_t)r;
}

int getpagesize(void) { return 4096; }

/*
 * ===== entropy =====
 *
 * Straight through to SYS_RANDOM, which answers from the same source TLS
 * uses. The kernel returns a *count* and is allowed to answer short —
 * see the note in src/syscall.h about why a partial read is reported
 * rather than padded.
 */
ssize_t getrandom(void *buf, size_t len, unsigned int flags) {
    (void)flags;
    if (!buf) { errno = EFAULT; return -1; }
    if (!len) return 0;
    const long n = __syscall2(SYS_RANDOM, (long)(uintptr_t)buf, (long)len);
    if (n < 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}

int getentropy(void *buf, size_t len) {
    if (!buf) { errno = EFAULT; return -1; }
    if (len > 256) { errno = EIO; return -1; }

    /*
     * All of it or none of it. Looping because the generator behind
     * SYS_RANDOM can fail under load, and bounded because a source that
     * keeps answering zero is a source that is not going to start.
     * Returning a partly filled buffer would be the dangerous answer: a
     * caller seeding a key from it has no way to notice.
     */
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    for (int tries = 0; tries < 16 && got < len; tries++) {
        const ssize_t n = getrandom(p + got, len - got, 0);
        if (n < 0) return -1;
        got += (size_t)n;
    }
    if (got != len) { errno = EIO; return -1; }
    return 0;
}

/* ===== CHARACTER CLASSES =====
 *
 * By comparison rather than by table. The table is one load faster and
 * is also where this header's oldest bug lives: the index is an int that
 * may be EOF or a sign-extended char, and the lookup then reads before
 * the start of the array. A comparison cannot.
 */
int isascii(int c) { return (unsigned)c < 128u; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (unsigned)c < 32u || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

/* ===== THE CASE-INSENSITIVE COMPARISONS ===== */

int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

void bzero(void *p, size_t n) { memset(p, 0, n); }
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
int  bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

int ffs(int v) {
    if (!v) return 0;
    return __builtin_ctz((unsigned)v) + 1;
}

/* ===== ERROR STRINGS ===== */

char *strerror(int e) {
    /* Not static-per-call and not thread-local: every string below is a
     * literal, so there is nothing to share and nothing to overwrite.
     * Only the unknown case needs storage, and it is formatted into a
     * buffer that a second caller may overwrite -- which is exactly the
     * contract strerror has always had. */
    switch (e) {
    case 0:             return "no error";
    case EPERM:         return "operation not permitted";
    case ENOENT:        return "no such file or directory";
    case ESRCH:         return "no such thread";
    case EINTR:         return "interrupted";
    case EIO:           return "input/output error";
    case ENXIO:         return "no such device or address";
    case E2BIG:         return "argument list too long";
    case ENOEXEC:       return "not an executable";
    case EBADF:         return "bad file descriptor";
    case ECHILD:        return "no child processes";
    case EAGAIN:        return "resource temporarily unavailable";
    case ENOMEM:        return "out of memory";
    case EACCES:        return "permission denied";
    case EFAULT:        return "bad address";
    case EBUSY:         return "device or resource busy";
    case EEXIST:        return "already exists";
    case ENODEV:        return "no such device";
    case ENOTDIR:       return "not a directory";
    case EISDIR:        return "is a directory";
    case EINVAL:        return "invalid argument";
    case ENFILE:        return "too many open files in the system";
    case EMFILE:        return "too many open files";
    case ENOSPC:        return "no space left on device";
    case EROFS:         return "read-only file system";
    case EPIPE:         return "broken pipe";
    case EDOM:          return "argument outside the function's domain";
    case ERANGE:        return "result outside the representable range";
    case EDEADLK:       return "a deadlock would occur";
    case ENAMETOOLONG:  return "name too long";
    case ENOSYS:        return "not implemented on this system";
    case ENOTEMPTY:     return "directory not empty";
    case EOVERFLOW:     return "value too large for its type";
    case ENOTSOCK:      return "not a socket";
    case EOPNOTSUPP:    return "not supported";
    case ETIMEDOUT:     return "timed out";
    case ECANCELED:     return "cancelled";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "unknown error %d", e);
        return buf;
    }
    }
}

void __assert_fail(const char *expr, const char *file, int line,
                   const char *fn) {
    printf("assertion failed: %s\n  at %s:%d in %s\n", expr, file, line, fn);
    abort();
}

/* ===== NON-LOCAL JUMPS =====
 *
 * The callee-saved set and nothing else, which is the whole contract:
 * anything the compiler kept in a caller-saved register is not in the
 * buffer, and that is exactly why the standard says a non-volatile local
 * modified between the two calls has an indeterminate value.
 *
 * The return address is taken from the stack rather than from a
 * register, because on entry to setjmp it is the word the CALL pushed —
 * which is where longjmp has to put it back for the RET to land in the
 * right place.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".globl setjmp\n"
    ".type setjmp, @function\n"
    "setjmp:\n"
    "  movq %rbx,  0(%rdi)\n"
    "  movq %rbp,  8(%rdi)\n"
    "  movq %r12, 16(%rdi)\n"
    "  movq %r13, 24(%rdi)\n"
    "  movq %r14, 32(%rdi)\n"
    "  movq %r15, 40(%rdi)\n"
    /* The stack pointer as it will be *after* this function returns, so
     * that longjmp restores the caller's frame and not setjmp's own. */
    "  leaq 8(%rsp), %rax\n"
    "  movq %rax,  48(%rdi)\n"
    "  movq (%rsp), %rax\n"
    "  movq %rax,  56(%rdi)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".size setjmp, . - setjmp\n"

    ".globl longjmp\n"
    ".type longjmp, @function\n"
    "longjmp:\n"
    "  movq  0(%rdi), %rbx\n"
    "  movq  8(%rdi), %rbp\n"
    "  movq 16(%rdi), %r12\n"
    "  movq 24(%rdi), %r13\n"
    "  movq 32(%rdi), %r14\n"
    "  movq 40(%rdi), %r15\n"
    "  movq 48(%rdi), %rsp\n"
    "  movl %esi, %eax\n"
    /* setjmp must never appear to return zero from a longjmp, or the
     * caller's `if (setjmp(...))` takes the wrong branch and runs the
     * protected code a second time. */
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  movl $1, %eax\n"
    "1:\n"
    "  jmp *56(%rdi)\n"
    ".size longjmp, . - longjmp\n"

    /* No signals here, so the mask has nothing to save and these are the
     * same two functions. Defined as separate symbols rather than as
     * aliases so that a debugger's backtrace names the one the program
     * called. */
    ".globl sigsetjmp\n"
    "sigsetjmp:\n"
    "  jmp setjmp\n"
    ".globl siglongjmp\n"
    "siglongjmp:\n"
    "  jmp longjmp\n"
    ".popsection\n"
);
