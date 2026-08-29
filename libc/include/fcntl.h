#ifndef _FCNTL_H
#define _FCNTL_H

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
 * fcntl.h — opening things.
 *
 * The numbers are Linux's, digit for digit, and that is a decision
 * rather than a coincidence. A port carries O_WRONLY|O_CREAT|O_TRUNC
 * folded into a constant by somebody else's preprocessor often enough
 * that choosing our own numbering would work perfectly for everything
 * compiled here and quietly break the things that were not.
 *
 * ---- what open() means on this volume ----
 *
 * Reading is ordinary: the descriptor resolves to an MFT record once and
 * reads a window at a time, so a file larger than memory is fine.
 *
 * Writing is not ordinary and the difference is worth knowing before
 * relying on it. The NTFS writer underneath replaces whole files rather
 * than updating them in place, so a descriptor opened for writing holds
 * the file's contents in the kernel and puts the whole image back at
 * close() or fsync(). Three consequences follow:
 *
 *   A file opened for writing is capped at four megabytes. Past that,
 *   write() reports ENOSPC rather than truncating.
 *
 *   Nothing reaches the disk until close() or fsync(). A program that
 *   wants a checkpoint has to ask for one.
 *
 *   Two programs writing one file each write their whole image, and the
 *   second wins entirely.
 *
 * ---- and the prompt ----
 *
 * Opening a file for writing is one of the things this system asks a
 * person about. The question is asked at open, once per program, and a
 * refusal is EPERM from open() rather than a failure at close where
 * there would be nothing to ask about and nobody to ask.
 */

#include <sys/types.h>

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_ACCMODE    3

#define O_CREAT      0100
#define O_EXCL       0200
#define O_NOCTTY     0400
#define O_TRUNC      01000
#define O_APPEND     02000
#define O_DIRECTORY  0200000
#define O_CLOEXEC    02000000

/*
 * Accepted and without effect, and listed separately from the ones above
 * so that is not something to be worked out from behaviour.
 *
 * O_NONBLOCK cannot be honoured: every descriptor here is blocking,
 * because the kernel's socket layer is one-thread-per-connection and its
 * file layer talks to a disk. A program that sets it and then relies on
 * EAGAIN would spin; one that sets it out of habit, which is most of
 * them, is unaffected.
 *
 * O_SYNC is redundant rather than ignored: a write here reaches the disk
 * at close or fsync and at no other time, so asking for it to be
 * synchronous changes nothing about when it happens.
 */
#define O_NONBLOCK   04000
#define O_SYNC       04010000
#define O_NOFOLLOW   0400000
#define O_LARGEFILE  0

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int close(int fd);

/*
 * fcntl, for the two commands that mean something here.
 *
 * F_GETFL and F_SETFL answer and accept the flags the descriptor was
 * opened with. F_GETFD and F_SETFD carry FD_CLOEXEC, which is recorded
 * and has nothing to act on it — there is no exec on this system, so a
 * descriptor cannot survive into another program however it is marked.
 * It is kept rather than refused because a port that sets it and gets an
 * error concludes something is badly wrong.
 *
 * F_DUPFD is not supported and answers EINVAL. See the note about dup in
 * <unistd.h>.
 */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

#define FD_CLOEXEC 1

int fcntl(int fd, int cmd, ...);


#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
