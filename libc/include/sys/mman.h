#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

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
 * sys/mman.h — memory obtained by asking for a region rather than by
 * moving a line.
 *
 * The C library here has always had one source of memory: sbrk, which
 * moves a single break pointer upward and never gives anything back.
 * That is the oldest allocator interface there is and it is entirely
 * adequate for the programs it was written for — a Mandelbrot renderer
 * takes one buffer at startup and keeps it until it exits.
 *
 * It is inadequate for three things at once, and a browser engine is all
 * three:
 *
 *   A program with more than one thread cannot use a single break
 *   safely. Two threads calling sbrk race over one word in the kernel,
 *   and the loser's allocation overlaps the winner's.
 *
 *   A program that frees a large region cannot return it. sbrk only
 *   moves down from the top, so a hundred megabyte buffer freed while
 *   anything above it is live stays charged to the process forever.
 *
 *   A program cannot reserve without spending. A JavaScript heap wants a
 *   contiguous range of address space measured in gigabytes so that it
 *   can grow into it without moving, and expects to be charged only for
 *   the pages it touches. There is no way to say that with a break: the
 *   pages under a break are mapped, all of them, as soon as it moves.
 *
 * mmap says all three. See the note in src/vmm.h for how a reservation
 * that costs nothing is actually represented, and the note on SYS_MMAP
 * in src/syscall.h for the one thing this system refuses to map.
 */

#include <stddef.h>
#include <stdint.h>

/* What may be done with the pages. */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/*
 * How the mapping behaves.
 *
 * MAP_ANONYMOUS is not optional here and MAP_SHARED does not exist.
 * There is no file to map — a ring-3 program on this system has no way
 * to open one — and there is no second process to share with that is not
 * a fork, which shares by copy-on-write already. A port that passes
 * MAP_SHARED or a real descriptor is refused by the kernel rather than
 * quietly given private anonymous pages, because a program that believes
 * two processes are looking at one buffer and is wrong will not fail
 * here; it will fail much later, having silently computed on its own
 * copy.
 */
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
/* Accepted and ignored: every mapping here is already unreserved, in the
 * sense that no physical memory is committed until a page is touched.
 * Ported code passes it as an optimisation hint and gets what it wanted
 * without the flag. */
#define MAP_NORESERVE  0x4000

/* What mmap returns when it fails, which is not null — a mapping at
 * address zero would be a legitimate result on a system that allowed it,
 * so the failure value has to be one that never can be. */
#define MAP_FAILED  ((void *)-1)

/*
 * Reserve address space.
 *
 * `fd` and `offset` are part of the signature because ported code passes
 * them, and are required to be -1 and 0 respectively. Anything else is
 * EINVAL: see the note on MAP_ANONYMOUS above for why silently ignoring
 * them would be worse than refusing.
 *
 * The memory reads as zero on first touch. That is a guarantee and not
 * an artefact — the kernel clears each frame as it backs it, because an
 * anonymous mapping that returned whatever the previous owner left in a
 * frame would be a way to read another program's memory.
 *
 * PROT_WRITE and PROT_EXEC together are refused. See mprotect below.
 */
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset);

/* Release a range obtained from mmap. Address and length are rounded out
 * to whole pages, as POSIX specifies. Unmapping part of a mapping is
 * allowed and leaves the rest intact. */
int munmap(void *addr, size_t length);

/*
 * Change what may be done with a range.
 *
 * PROT_WRITE|PROT_EXEC is refused, and that refusal is the reason this
 * function is interesting rather than routine. Vextro maps every page of
 * every program either writable or executable and never both — the
 * linker scripts put text and data in separate segments, and the loader
 * maps each page with the protection its segment asked for. mprotect is
 * the call that could undo it, and the kernel is where the refusal
 * lives, so there is no sequence of calls from user space that arrives
 * at a page which is both.
 *
 * This is what makes a just-in-time compiler impossible here rather than
 * merely discouraged, and it is why the WebKit configuration in
 * third_party/wpe turns every JIT tier off: an engine that assumes it
 * can write code and then jump to it does not fail at the mprotect it
 * checked the return value of. It fails at the jump.
 */
int mprotect(void *addr, size_t length, int prot);

/*
 * Accepted, and honest about what it does.
 *
 * There is no file behind an anonymous mapping and therefore nothing to
 * flush to. Returning success is correct — the memory is as durable as
 * it is ever going to be — and returning an error would make ported code
 * take a failure path over a call that had nothing to do.
 */
#define MS_ASYNC       1
#define MS_INVALIDATE  2
#define MS_SYNC        4
int msync(void *addr, size_t length, int flags);

/*
 * Also accepted and also no-ops, for the same reason: they are advice,
 * and this system's answer to all of it is the same. MADV_DONTNEED is
 * the one that is a genuine simplification rather than a formality — a
 * real implementation would drop the frames and let them fault back in
 * as zeros, and here the pages simply stay. That costs memory a stricter
 * implementation would reclaim, and it cannot produce a wrong answer.
 */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
int madvise(void *addr, size_t length, int advice);

/* Locking pages into memory. There is a pager in this kernel and no way
 * for a program to exempt itself from it, so these report ENOSYS rather
 * than claiming a guarantee they cannot keep. */
int mlock(const void *addr, size_t length);
int munlock(const void *addr, size_t length);


#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
