#ifndef _DIRENT_H
#define _DIRENT_H

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
 * dirent.h — reading a directory.
 *
 * One property is worth stating because it is stronger than what POSIX
 * promises and code may reasonably want to rely on it: the listing is
 * taken *whole* when opendir() returns, not a block at a time as
 * readdir() is called. A directory on this volume is a B-tree and
 * walking it means reading from the disk, so doing it once makes
 * readdir() free and — the part that matters more — makes the sequence
 * stable. A program enumerating a directory while something else
 * creates a file in it sees a consistent snapshot rather than a name
 * repeated or skipped.
 *
 * The cost is that the snapshot can be stale, which is the same trade
 * every implementation makes and is invisible at the scale anything
 * here works at.
 */

#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct __vx_dir DIR;

DIR *opendir(const char *path);

/*
 * The next entry, or null at the end.
 *
 * The pointer is into storage owned by the DIR and is valid until the
 * next call on the same DIR, which is what readdir has always promised
 * and what makes it safe to use two directories at once and unsafe to
 * keep the result of one call across the next.
 */
struct dirent *readdir(DIR *d);

int   closedir(DIR *d);
void  rewinddir(DIR *d);
long  telldir(DIR *d);
void  seekdir(DIR *d, long pos);

/* The descriptor underneath, for a caller that wants to fstat it. */
int   dirfd(DIR *d);


#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
