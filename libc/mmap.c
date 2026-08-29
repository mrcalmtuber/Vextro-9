/*
 * libc/mmap.c — the user-space half of the memory calls.
 *
 * Thin, and deliberately so. Everything that decides anything is in the
 * kernel: where an unhinted mapping lands, whether a range was obtained
 * from mmap at all, and the refusal of writable-and-executable. What is
 * here is argument validation that can be done without entering the
 * kernel, and the translation between a system call's single return
 * register and the two channels — value and errno — that C expects.
 *
 * That translation is the reason this file exists rather than a header
 * of inline wrappers. A syscall answers -1 for every failure; ported
 * code wants to know which failure, and mapping one onto the other is a
 * decision per call rather than a rule.
 */

#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define VX_PAGE 4096u

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset) {
    /*
     * The three refusals that do not need the kernel.
     *
     * A zero length is meaningless rather than merely unsupported. A
     * descriptor is something this system has no concept of above the
     * kernel's own file layer, and offset follows it. Checking here
     * rather than there is not about speed — it is so the error is
     * EINVAL and not the generic refusal every kernel-side check
     * produces, which ported code cannot distinguish from ENOMEM.
     */
    if (length == 0) { errno = EINVAL; return MAP_FAILED; }

    if (fd != -1 || offset != 0 || !(flags & MAP_ANONYMOUS)) {
        /* A file mapping. There is nothing to map: see <sys/mman.h>. */
        errno = ENODEV;
        return MAP_FAILED;
    }
    if (flags & MAP_SHARED) { errno = EINVAL; return MAP_FAILED; }

    /* Overflow before the kernel is asked to round it. length is size_t
     * and the rounding adds to it, so a length within a page of the top
     * of the address space becomes a small one if this is not caught. */
    if (length > (size_t)-1 - VX_PAGE) { errno = ENOMEM; return MAP_FAILED; }

    long r = __syscall4(SYS_MMAP, (long)(uintptr_t)addr, (long)length,
                        (long)prot, (long)flags);

    /*
     * Distinguishing an address from a refusal.
     *
     * The kernel answers -1 for failure and an address otherwise, and an
     * address is never in the top half of the space for a user mapping —
     * user_range_ok refuses anything at or above USER_SPACE_END, so a
     * legitimate result always has its high bits clear. Testing for a
     * small unsigned value rather than for exactly -1 catches every
     * negative errno-style return as well, which costs nothing and means
     * this line does not have to be revisited if the kernel ever starts
     * returning them.
     */
    if (r == -1 || (uint64_t)r >= 0x0000800000000000ull) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)r;
}

int munmap(void *addr, size_t length) {
    if (!length) return 0;
    if (__syscall2(SYS_MUNMAP, (long)(uintptr_t)addr, (long)length) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    if (!length) return 0;
    /*
     * Refused here as well as in the kernel, and the duplication is
     * intentional. The kernel's refusal is the one that matters — it is
     * the one a program cannot go around — but it reports through the
     * serial log and a -1, which a port would surface as a generic
     * failure. EACCES is the answer POSIX specifies for a protection a
     * process is not permitted, and it is the one that gives the caller
     * something to print.
     */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        errno = EACCES;
        return -1;
    }
    if (__syscall3(SYS_MPROTECT, (long)(uintptr_t)addr, (long)length,
                   (long)prot) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Nothing to flush to, so nothing to do. See the header. */
int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    return 0;
}

int madvise(void *addr, size_t length, int advice) {
    (void)addr; (void)length; (void)advice;
    return 0;
}

int mlock(const void *addr, size_t length) {
    (void)addr; (void)length;
    errno = ENOSYS;
    return -1;
}

int munlock(const void *addr, size_t length) {
    (void)addr; (void)length;
    errno = ENOSYS;
    return -1;
}
