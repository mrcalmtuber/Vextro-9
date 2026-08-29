#ifndef _SYS_STAT_H
#define _SYS_STAT_H

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
 * sys/stat.h — what a file is.
 *
 * The structure has the fields a port reads and the fields it can be
 * given honestly, and the difference between those two sets is stated
 * here rather than left to be discovered:
 *
 *   st_size, st_mode and st_ino are real. On NTFS the inode is the MFT
 *   record, which is a genuine identity — two paths with the same
 *   st_ino are the same file.
 *
 *   st_mtime, st_atime and st_ctime are always zero. The volume keeps
 *   timestamps in $STANDARD_INFORMATION and the lookup path this is
 *   built on does not carry them up; inventing a plausible number would
 *   be worse than an obviously wrong one. A make(1) built against this
 *   would rebuild everything, which is the safe direction to be wrong
 *   in.
 *
 *   st_uid, st_gid and the permission bits in st_mode are zero. This
 *   system does have accounts and does enforce a boundary between them
 *   — see the profile check every path goes through — but it does not
 *   express that as nine bits per file, so there are no nine bits to
 *   report. A program that tests S_IWUSR to decide whether it may write
 *   should open the file and read the answer instead.
 */

#include <sys/types.h>
#include <time.h>

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    time_t    st_atime;
    long      st_atime_nsec;
    time_t    st_mtime;
    long      st_mtime_nsec;
    time_t    st_ctime;
    long      st_ctime_nsec;
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* The permission bits, so that code which builds a mode to pass to
 * open() or mkdir() compiles. Nothing on this volume stores them; see
 * the note above. */
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

/* There are no symbolic links on this volume, so this is stat. Present
 * because portable code reaches for it when it means "do not follow",
 * and there is nothing here to follow. */
int lstat(const char *path, struct stat *st);

int mkdir(const char *path, mode_t mode);

/* Accepted and without effect: there are no permission bits to change.
 * Returns 0 rather than an error, because the common caller is a port
 * tightening the mode on a file it has just written, and failing that
 * turns a successful write into a reported failure. */
int chmod(const char *path, mode_t mode);

/* The process-wide file creation mask, which is kept and applied to
 * nothing, for the same reason. */
mode_t umask(mode_t mask);


#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */
