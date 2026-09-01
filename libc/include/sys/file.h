#ifndef _SYS_FILE_H
#define _SYS_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sys/file.h — advisory whole-file locking, which this system does not
 * have.
 *
 * ---- why the header exists at all ----
 *
 * Because ports include it whether or not they lock. libgpg-error's
 * estream.c has `#include <sys/file.h>` at the top, outside every
 * conditional, and never calls flock() once — the `_gpgrt_flockfile` in
 * it is its own function over its own mutex. A header that is required
 * to *exist* and not to *work* is exactly the case where the smallest
 * honest thing is to describe the interface properly.
 *
 * ---- and why flock() is declared and not defined ----
 *
 * Deliberately, and it is the rule libc/include/unistd.h states for
 * this library: what this system cannot do is absent rather than
 * stubbed, so a port fails to link against a name that does not work
 * instead of calling one that silently does nothing.
 *
 * There is no file locking here to build one on. A descriptor *is* the
 * open file description (src/vfs.h says so at the point a fork declines
 * to duplicate a socket), there is no table of held locks anywhere, and
 * a flock() that returned success would tell two writers they had
 * exclusive access to the same write-back image. The failure mode of
 * that is silent data loss; the failure mode of an undefined symbol is
 * a link error naming flock.
 *
 * The constants are real and are the values every Unix uses, so code
 * that only *mentions* LOCK_EX in a branch it does not take compiles.
 */

#define LOCK_SH 1   /* shared lock       */
#define LOCK_EX 2   /* exclusive lock    */
#define LOCK_NB 4   /* do not block      */
#define LOCK_UN 8   /* unlock            */

int flock(int fd, int operation);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FILE_H */
