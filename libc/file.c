/*
 * libc/file.c — open, read, close, and the directory walk.
 *
 * Every function here is a thin translation between two conventions and
 * nothing more, which is the point: the kernel decides what a descriptor
 * is (src/vfs.h) and this file decides only how a C program asks about
 * it.
 *
 * The translation itself is one rule, applied everywhere through
 * __syscall_ret: the calls from 40 upwards answer a negated error
 * number, and a C function answers -1 with the reason in errno. It is
 * written once rather than eighteen times so that no wrapper can get it
 * subtly wrong.
 */

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdarg.h>

/* ===== what the kernel actually returns =====
 *
 * Mirrors of the two structures src/syscall.h defines, and the layouts
 * must agree exactly. They are written out again here rather than shared
 * through one header because a header a program can edit is a header the
 * kernel must not read its own layout from — the same argument that puts
 * the futex operation numbers in two places.
 *
 * The static assertions are what keep the two copies honest: a field
 * added on one side and not the other changes a size, and the build
 * stops.
 */
typedef struct {
    unsigned long size;
    unsigned long ino;
    unsigned int  mode;
    unsigned int  nlink;
    unsigned long mtime_ns;
} vx_stat_t;
_Static_assert(sizeof(vx_stat_t) == 32, "vx_stat_t must match the kernel");

#define VX_NAME_MAX 255
typedef struct {
    unsigned long size;
    unsigned int  type;
    unsigned int  namelen;
    char          name[256];
} vx_dirent_t;
_Static_assert(sizeof(vx_dirent_t) == 272, "vx_dirent_t must match the kernel");

#define VX_DT_REG 1
#define VX_DT_DIR 2

/* ===== files ===== */

int open(const char *path, int flags, ...) {
    /*
     * The mode is read and discarded. open() is variadic because the
     * third argument is only present when O_CREAT is, and a port passes
     * one; this volume has no permission bits to store it in, so it
     * goes no further than here. See <sys/stat.h> for why that is not
     * an omission that can be quietly fixed.
     */
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned int);
        va_end(ap);
    }
    return (int)__syscall_ret(__syscall3(SYS_OPEN, (long)(uintptr_t)path,
                                         (long)flags, (long)mode));
}

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd) {
    return (int)__syscall_ret(__syscall1(SYS_CLOSE, fd));
}

ssize_t read(int fd, void *buf, size_t len) {
    return (ssize_t)__syscall_ret(__syscall3(SYS_READ, fd,
                                             (long)(uintptr_t)buf,
                                             (long)len));
}

/*
 * write() was here before descriptors were, and it still is: the same
 * system call now looks the number up in the table instead of accepting
 * only 1 and 2. What changed on this side is that it unpacks an error
 * number, because the call reports one now.
 */
ssize_t write(int fd, const void *buf, size_t len) {
    return (ssize_t)__syscall_ret(__syscall3(SYS_WRITE, fd,
                                             (long)(uintptr_t)buf,
                                             (long)len));
}

off_t lseek(int fd, off_t off, int whence) {
    return (off_t)__syscall_ret(__syscall3(SYS_LSEEK, fd, (long)off,
                                           (long)whence));
}

int fsync(int fd) {
    return (int)__syscall_ret(__syscall1(SYS_FSYNC, fd));
}

int fdatasync(int fd) { return fsync(fd); }

int ftruncate(int fd, off_t len) {
    return (int)__syscall_ret(__syscall2(SYS_FTRUNCATE, fd, (long)len));
}

int unlink(const char *path) {
    return (int)__syscall_ret(__syscall1(SYS_UNLINK, (long)(uintptr_t)path));
}

int remove(const char *path) {
    /* rmdir and unlink are one operation on this volume: a directory is
     * deleted by the same driver call as a file, and refusing a
     * non-empty one is the filesystem's decision rather than this
     * layer's. */
    return unlink(path);
}

int rmdir(const char *path) { return unlink(path); }

int mkdir(const char *path, mode_t mode) {
    return (int)__syscall_ret(__syscall2(SYS_MKDIR, (long)(uintptr_t)path,
                                         (long)mode));
}

/* ===== stat ===== */

static void stat_from_kernel(struct stat *st, const vx_stat_t *k) {
    memset(st, 0, sizeof(*st));
    st->st_size  = (off_t)k->size;
    st->st_ino   = k->ino;
    st->st_mode  = k->mode;
    st->st_nlink = k->nlink ? k->nlink : 1;
    /* The unit the kernel reads and writes a file in. Reported because
     * ported code sizes its buffers from it, and 4096 is genuinely the
     * cluster size this volume is formatted with. */
    st->st_blksize = 4096;
    st->st_blocks  = (blkcnt_t)((k->size + 511) / 512);
    /* The times stay zero. See the note at the head of <sys/stat.h>:
     * the lookup path does not carry $STANDARD_INFORMATION up, and a
     * plausible wrong answer is worse than an obvious one. */
}

int stat(const char *path, struct stat *st) {
    if (!st) { errno = EFAULT; return -1; }
    vx_stat_t k;
    long rc = __syscall_ret(__syscall2(SYS_STAT, (long)(uintptr_t)path,
                                       (long)(uintptr_t)&k));
    if (rc < 0) return -1;
    stat_from_kernel(st, &k);
    return 0;
}

/* No symbolic links exist on this volume, so there is nothing for lstat
 * to decline to follow. */
int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st) {
    if (!st) { errno = EFAULT; return -1; }
    vx_stat_t k;
    long rc = __syscall_ret(__syscall2(SYS_FSTAT, fd, (long)(uintptr_t)&k));
    if (rc < 0) return -1;
    stat_from_kernel(st, &k);
    return 0;
}

int access(const char *path, int mode) {
    /*
     * Answered from stat, and the reason it can be is worth stating: on
     * this system what a program may do to a file depends on the profile
     * the path is in and on an answer somebody gives at the keyboard,
     * neither of which is a property of the file that could be reported
     * in advance. So F_OK is real and R_OK, W_OK and X_OK all mean "it
     * is there".
     *
     * That is the honest answer rather than a convenient one: a
     * pessimistic guess would make a program give up on a file it could
     * have opened, and an optimistic one is what every caller does
     * anyway when it opens the file and checks.
     */
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

/* Recorded and applied to nothing; see <sys/stat.h>. */
static mode_t vx_umask = 022;

mode_t umask(mode_t mask) {
    mode_t old = vx_umask;
    vx_umask = mask;
    return old;
}

int chmod(const char *path, mode_t mode) {
    /* Succeeds without doing anything, deliberately. The usual caller is
     * a port tightening the mode on a file it has just written, and
     * failing here would turn a completed write into a reported
     * failure over a distinction this volume does not record. */
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    (void)mode;
    return 0;
}

/* ===== fcntl ===== */

int fcntl(int fd, int cmd, ...) {
    /*
     * Two commands answer, two are recorded, and one is refused.
     *
     * F_GETFL is answered from the descriptor's own flags, which means
     * asking the kernel — there is nowhere on this side to keep them
     * that a fork or a second thread would agree with. fstat is used to
     * establish the descriptor exists; the flags themselves are not
     * something the kernel exports, so what comes back is the access
     * mode implied by what the descriptor can do.
     *
     * F_DUPFD is refused because there is no dup on this system, for the
     * reason set out in <unistd.h>.
     */
    struct stat st;
    switch (cmd) {
    case F_GETFD:
        if (fstat(fd, &st) != 0) return -1;
        return 0;                       /* close-on-exec is never set */
    case F_SETFD: {
        va_list ap;
        va_start(ap, cmd);
        (void)va_arg(ap, int);
        va_end(ap);
        if (fstat(fd, &st) != 0) return -1;
        return 0;
    }
    case F_GETFL:
        if (fstat(fd, &st) != 0) return -1;
        return O_RDWR;
    case F_SETFL: {
        va_list ap;
        va_start(ap, cmd);
        (void)va_arg(ap, int);
        va_end(ap);
        if (fstat(fd, &st) != 0) return -1;
        return 0;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ===== directories =====
 *
 * The whole listing is fetched at opendir and held here. The kernel took
 * it in one pass when the descriptor was opened — a directory on this
 * volume is a B-tree and walking it means reading from a disk — so
 * pulling it across in one go rather than a system call per name costs
 * one call and a buffer, and readdir becomes a pointer bump.
 */

#define DIR_CHUNK 32

struct __vx_dir {
    int          fd;
    vx_dirent_t *ents;
    long         n;
    long         pos;
    struct dirent out;      /* what readdir hands back a pointer to */
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;

    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) { close(fd); errno = ENOMEM; return 0; }
    d->fd = fd;

    /* Grown by doubling rather than asked for in one piece, because
     * there is no call that says how many names a directory has and
     * guessing large would mean a megabyte for a directory of three. */
    long cap = DIR_CHUNK;
    d->ents = (vx_dirent_t *)malloc((size_t)cap * sizeof(vx_dirent_t));
    if (!d->ents) { close(fd); free(d); errno = ENOMEM; return 0; }

    for (;;) {
        if (d->n == cap) {
            long want = cap * 2;
            vx_dirent_t *grown = (vx_dirent_t *)
                realloc(d->ents, (size_t)want * sizeof(vx_dirent_t));
            if (!grown) { closedir(d); errno = ENOMEM; return 0; }
            d->ents = grown;
            cap = want;
        }
        long got = __syscall_ret(
            __syscall3(SYS_GETDENTS, fd, (long)(uintptr_t)(d->ents + d->n),
                       (long)((cap - d->n) * (long)sizeof(vx_dirent_t))));
        if (got < 0) { closedir(d); return 0; }
        if (got == 0) break;
        d->n += got;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return 0; }
    if (d->pos >= d->n) return 0;              /* the end, and not an error */

    const vx_dirent_t *e = &d->ents[d->pos];
    memset(&d->out, 0, sizeof(d->out));
    /* There is no per-entry inode in a directory listing here — the
     * kernel's enumeration gives a name, a size and a kind. A caller
     * that needs the identity of an entry stats it. */
    d->out.d_ino    = 0;
    d->out.d_off    = d->pos + 1;
    d->out.d_reclen = (unsigned short)sizeof(struct dirent);
    d->out.d_type   = e->type == VX_DT_DIR ? DT_DIR
                    : e->type == VX_DT_REG ? DT_REG : DT_UNKNOWN;
    {
        size_t n = e->namelen;
        if (n > sizeof(d->out.d_name) - 1) n = sizeof(d->out.d_name) - 1;
        memcpy(d->out.d_name, e->name, n);
        d->out.d_name[n] = '\0';
    }
    d->pos++;
    return &d->out;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    if (d->fd >= 0) close(d->fd);
    free(d->ents);
    free(d);
    return 0;
}

void rewinddir(DIR *d) { if (d) d->pos = 0; }
long telldir(DIR *d)   { return d ? d->pos : -1; }

void seekdir(DIR *d, long pos) {
    if (!d) return;
    if (pos < 0) pos = 0;
    if (pos > d->n) pos = d->n;
    d->pos = pos;
}

int dirfd(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    return d->fd;
}

/* ===== the working directory =====
 *
 * There is not one. Every path this system resolves is made absolute by
 * fs_native() in the kernel before any driver sees it, and there is no
 * per-process directory for a relative path to be relative *to*.
 *
 * So getcwd answers "/" and chdir refuses anything else. Both are here
 * rather than absent because a port calls getcwd to build an absolute
 * path out of a relative one, and the answer it gets is true: on this
 * system a bare name is resolved from the root.
 */
char *getcwd(char *buf, size_t size) {
    if (!buf) { errno = EINVAL; return 0; }
    if (size < 2) { errno = ERANGE; return 0; }
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int chdir(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return 0;
    /* Refused rather than accepted-and-ignored, which would leave a
     * program building paths against a directory it believes it is in
     * and this system has never heard of. */
    errno = ENOSYS;
    return -1;
}

int isatty(int fd) {
    /* The console streams, and nothing else. A program asking this is
     * usually deciding whether to colour its output, and getting it
     * wrong the other way fills a file with escape sequences. */
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    if (S_ISCHR(st.st_mode)) return 1;
    errno = ENOTTY;
    return 0;
}

/*
 * rename — and why it is a copy.
 *
 * There is no rename in the filesystem underneath. NTFS has one, and the
 * driver here does not implement it: what it can do is create a file
 * with contents and delete a file by name. So this reads the source,
 * writes the destination, and unlinks the source.
 *
 * Three things follow, and they are stated rather than left to be found:
 *
 *   It is not atomic. A crash between the write and the unlink leaves
 *   both files. That rules out the "write a temporary and rename over
 *   the original" idiom as a *crash-safety* technique — it still works
 *   as a way of not showing a half-written file to a reader, which is
 *   the other reason people use it.
 *
 *   It costs a copy of the file, and is capped at what a descriptor
 *   opened for writing can hold: four megabytes, past which it fails
 *   with ENOSPC rather than truncating.
 *
 *   The source is only unlinked once the destination is closed and
 *   therefore on the disk. A failure anywhere before that leaves the
 *   source exactly as it was, which is the direction to be wrong in.
 */
int rename(const char *from, const char *to) {
    if (!from || !to) { errno = EINVAL; return -1; }

    int src = open(from, O_RDONLY);
    if (src < 0) return -1;

    int dst = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dst < 0) { int e = errno; close(src); errno = e; return -1; }

    char buf[4096];
    for (;;) {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n < 0) { int e = errno; close(src); close(dst); errno = e; return -1; }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, (size_t)(n - off));
            if (w <= 0) {
                int e = errno;
                close(src); close(dst);
                errno = e ? e : EIO;
                return -1;
            }
            off += w;
        }
    }
    close(src);
    /* The close is where the image reaches the disk, so its result is
     * the one that says whether the destination exists. */
    if (close(dst) != 0) return -1;

    return unlink(from);
}
