/*
 * third_party/sqlite-port/vx_vfs.c — SQLite's operating system, here.
 *
 * SQLite is built with SQLITE_OS_OTHER=1, which excludes os_unix.c and
 * os_win.c entirely and leaves the engine with no way to touch a file at
 * all until something registers a sqlite3_vfs. This is that something:
 * about six hundred lines over `open`, `read`, `write`, `lseek`,
 * `fsync`, `close`, `unlink` and `stat` — the descriptor calls ring 3
 * gained in src/vfs.h — plus a clock, an entropy source and a sleep.
 *
 * That is the whole port. The 255,680 lines of engine above it are
 * vendored unmodified at 3.45.1; nothing in sqlite3.c was touched, and
 * the amalgamation's own checksum is pinned in the Makefile.
 *
 * ============================================================
 *  WHAT THIS FILESYSTEM IS, AND WHAT IT MEANS FOR A DATABASE
 * ============================================================
 *
 * The three properties below are not defects in the port. They are what
 * the storage underneath actually does, and a database that pretended
 * otherwise would be a database that lost data quietly.
 *
 * ---- 1. a file opened for writing is capped at four megabytes ----
 *
 * The NTFS writer replaces whole files rather than updating them in
 * place — see the long note in src/vfs.h — so a descriptor opened for
 * writing holds the file's image in the kernel and writes it back at
 * close or fsync. VFS_WBUF_MAX is four megabytes.
 *
 * A browser's history, its settings and its cookie table fit inside that
 * comfortably. A database that grows past it gets SQLITE_FULL, which is
 * the correct error and is reported rather than truncated.
 *
 * ---- 2. every sync rewrites the whole file ----
 *
 * xSync reaches fsync, and fsync on this system hands the entire image
 * to fs_write_file, which deletes the old MFT record and creates a new
 * one. SQLite syncs several times per transaction — journal, then
 * database, then journal again — so a committed write costs several
 * whole-file rewrites.
 *
 * That is genuinely slow and it is genuinely durable, which is the trade
 * this filesystem makes everywhere. It is stated here because somebody
 * benchmarking inserts will otherwise conclude the engine is at fault.
 *
 * ---- 3. one process at a time ----
 *
 * There is no file locking in this kernel, so the locking methods below
 * track state and enforce nothing between processes. Within one process
 * SQLite's own mutexes serialise access correctly and everything works.
 * Two processes with the same database open would each write back their
 * own whole image and the second would win entirely — losing the first's
 * transactions with no error anywhere.
 *
 * SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN is reported for the same family of
 * reason: fs_open resolves a path to an MFT record once and reads
 * through it thereafter, so deleting a file another descriptor holds
 * leaves that descriptor reading freed clusters.
 *
 * ============================================================
 *  WHAT IS COMPILED OUT, AND WHY EACH
 * ============================================================
 *
 * The Makefile defines these; they are listed here because the reasons
 * belong next to the code that would otherwise need them:
 *
 *   SQLITE_OMIT_WAL            write-ahead logging needs xShmMap, which
 *                              is shared memory between processes. There
 *                              is none. Rollback journalling — the
 *                              default — needs only ordinary files.
 *   SQLITE_TEMP_STORE=3        temporary tables live in memory. Against
 *                              a filesystem that rewrites a whole file
 *                              per sync, temp-file churn is pathological.
 *   SQLITE_OMIT_LOAD_EXTENSION there is no dynamic loader, so the four
 *                              xDl* entries below are null.
 *   SQLITE_OMIT_LOCALTIME      no timezone database. See the note on
 *                              xCurrentTimeInt64.
 *   SQLITE_OS_OTHER=1          this file is the operating system.
 */

#include "sqlite3.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* ============================================================
 *  a file
 * ============================================================ */

/*
 * SQLite allocates this itself and hands it back on every call, so it
 * must begin with the sqlite3_file base — that is how the engine finds
 * the method table.
 */
typedef struct vx_file {
    sqlite3_file base;
    int   fd;
    int   lock;                 /* SQLITE_LOCK_*, tracked not enforced */
    int   delete_on_close;
    char  path[256];
} vx_file;

/* One error number to one SQLite code. Only the distinctions the engine
 * acts on differently are made; inventing more would be inventing
 * detail the kernel did not report. */
static int vx_errno_to_sqlite(int e, int fallback) {
    switch (e) {
    case ENOENT:  return SQLITE_CANTOPEN;
    case EACCES:
    case EPERM:   return SQLITE_PERM;
    case ENOSPC:  return SQLITE_FULL;
    case EBUSY:   return SQLITE_BUSY;
    case EISDIR:
    case ENOTDIR:
    case EINVAL:  return SQLITE_CANTOPEN;
    default:      return fallback;
    }
}

static int vx_close(sqlite3_file *p) {
    vx_file *f = (vx_file *)p;
    if (f->fd >= 0) {
        /*
         * The close is where a written database reaches the disk, so its
         * result is the one that says whether the write happened at all.
         * Reporting it is the difference between a transaction that
         * committed and one that appeared to.
         */
        const int rc = close(f->fd);
        f->fd = -1;
        if (rc != 0) return SQLITE_IOERR_CLOSE;
    }
    if (f->delete_on_close && f->path[0]) unlink(f->path);
    return SQLITE_OK;
}

static int vx_read(sqlite3_file *p, void *buf, int amt, sqlite3_int64 ofst) {
    vx_file *f = (vx_file *)p;
    if (lseek(f->fd, (off_t)ofst, SEEK_SET) < 0) return SQLITE_IOERR_READ;

    unsigned char *out = (unsigned char *)buf;
    int got = 0;
    while (got < amt) {
        const ssize_t n = read(f->fd, out + got, (size_t)(amt - got));
        if (n < 0) return SQLITE_IOERR_READ;
        if (n == 0) break;
        got += (int)n;
    }

    if (got < amt) {
        /*
         * Short of what was asked for: past the end of the file. SQLite
         * requires the remainder to be zeroed and treats the distinct
         * code as "this page does not exist yet" rather than as an
         * error — a fresh database is read before it is written.
         */
        memset(out + got, 0, (size_t)(amt - got));
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int vx_write(sqlite3_file *p, const void *buf, int amt,
                    sqlite3_int64 ofst) {
    vx_file *f = (vx_file *)p;
    if (lseek(f->fd, (off_t)ofst, SEEK_SET) < 0) return SQLITE_IOERR_WRITE;

    const unsigned char *in = (const unsigned char *)buf;
    int done = 0;
    while (done < amt) {
        const ssize_t n = write(f->fd, in + done, (size_t)(amt - done));
        if (n <= 0) {
            /* ENOSPC here is the four-megabyte ceiling on a written
             * file, and SQLITE_FULL is exactly what it means. */
            return vx_errno_to_sqlite(errno, SQLITE_IOERR_WRITE);
        }
        done += (int)n;
    }
    return SQLITE_OK;
}

static int vx_truncate(sqlite3_file *p, sqlite3_int64 size) {
    vx_file *f = (vx_file *)p;
    return ftruncate(f->fd, (off_t)size) == 0 ? SQLITE_OK
                                              : SQLITE_IOERR_TRUNCATE;
}

static int vx_sync(sqlite3_file *p, int flags) {
    vx_file *f = (vx_file *)p;
    (void)flags;
    /*
     * fsync here is not a hint. On this system nothing written through a
     * descriptor is on the volume until fsync or close: the kernel holds
     * the image and hands the whole of it to the NTFS writer, which
     * journals it. So a successful return really does mean the bytes are
     * durable — which is a stronger guarantee than fsync gives on most
     * systems, and a much more expensive one.
     */
    return fsync(f->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int vx_file_size(sqlite3_file *p, sqlite3_int64 *out) {
    vx_file *f = (vx_file *)p;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return SQLITE_IOERR_FSTAT;
    *out = (sqlite3_int64)st.st_size;
    return SQLITE_OK;
}

/*
 * ---- locking ----
 *
 * Tracked, not enforced. There is no advisory locking in this kernel and
 * nothing to build one on: no shared memory between processes, no
 * fcntl(F_SETLK).
 *
 * Recording the level anyway is not theatre. SQLite asserts its own
 * transitions — SHARED before RESERVED, RESERVED before EXCLUSIVE — and
 * a VFS that answered nothing would let a bug in the engine's state
 * machine pass unnoticed here and be discovered somewhere far less
 * convenient. Within one process this is *sufficient*: the engine's
 * mutexes serialise access, and the lock level is exactly what it
 * believes it is.
 *
 * Between processes it is not sufficient and there is no way to make it
 * so from here. The note at the head of this file says what that costs.
 */
static int vx_lock(sqlite3_file *p, int level) {
    ((vx_file *)p)->lock = level;
    return SQLITE_OK;
}

static int vx_unlock(sqlite3_file *p, int level) {
    ((vx_file *)p)->lock = level;
    return SQLITE_OK;
}

static int vx_check_reserved_lock(sqlite3_file *p, int *out) {
    *out = ((vx_file *)p)->lock >= SQLITE_LOCK_RESERVED;
    return SQLITE_OK;
}

static int vx_file_control(sqlite3_file *p, int op, void *arg) {
    vx_file *f = (vx_file *)p;
    switch (op) {
    case SQLITE_FCNTL_SIZE_HINT:
        /* A promise about how large the file will get. Nothing here can
         * preallocate — the image grows by doubling in the kernel — so
         * this is accepted and used for nothing, which is what the
         * interface allows. */
        return SQLITE_OK;
    case SQLITE_FCNTL_VFSNAME:
        *(char **)arg = sqlite3_mprintf("vextro");
        return SQLITE_OK;
    case SQLITE_FCNTL_LOCKSTATE:
        *(int *)arg = f->lock;
        return SQLITE_OK;
    default:
        return SQLITE_NOTFOUND;
    }
}

static int vx_sector_size(sqlite3_file *p) {
    (void)p;
    /* The cluster this volume is formatted with, which is the unit the
     * layer underneath actually reads and writes in. */
    return 4096;
}

static int vx_device_characteristics(sqlite3_file *p) {
    (void)p;
    /*
     * One flag, and it is a warning rather than a capability: a file
     * this VFS has open must not be deleted, because fs_open resolves a
     * path to an MFT record once and keeps reading through it.
     *
     * Nothing else is claimed. SQLITE_IOCAP_POWERSAFE_OVERWRITE and its
     * neighbours are assertions about what the *hardware* guarantees
     * across a power cut, and claiming one that is not true is how a
     * database is corrupted by an optimisation it invited.
     */
    return SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static const sqlite3_io_methods vx_io_methods = {
    1,                          /* iVersion: no shm, no mmap  */
    vx_close,
    vx_read,
    vx_write,
    vx_truncate,
    vx_sync,
    vx_file_size,
    vx_lock,
    vx_unlock,
    vx_check_reserved_lock,
    vx_file_control,
    vx_sector_size,
    vx_device_characteristics,
    0, 0, 0, 0,                 /* xShm*: SQLITE_OMIT_WAL     */
    0, 0                        /* xFetch/xUnfetch: no mmap   */
};

/* ============================================================
 *  the VFS
 * ============================================================ */

static int vx_full_pathname(sqlite3_vfs *vfs, const char *in, int nOut,
                            char *out);

static int vx_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *file,
                   int flags, int *out_flags) {
    (void)vfs;
    vx_file *f = (vx_file *)file;
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    f->lock = SQLITE_LOCK_NONE;

    /*
     * A null name means a temporary file, and there is nowhere to put
     * one that would not be pathological: every sync rewrites a whole
     * file, and a temp file is written constantly.
     *
     * SQLITE_TEMP_STORE=3 keeps temporary *tables* in memory so this is
     * almost never reached. When it is — a sorter spilling, say — the
     * honest answer is that the operation cannot be done here rather
     * than a file that will be rewritten a thousand times.
     */
    if (!name) return SQLITE_CANTOPEN;

    int oflags = 0;
    if (flags & SQLITE_OPEN_READWRITE) oflags = O_RDWR;
    else                               oflags = O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE)    oflags |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) oflags |= O_EXCL;

    if (vx_full_pathname(vfs, name, (int)sizeof(f->path), f->path) != SQLITE_OK)
        return SQLITE_CANTOPEN;

    int fd = open(f->path, oflags, 0644);
    if (fd < 0 && (flags & SQLITE_OPEN_READWRITE) && errno == EPERM) {
        /*
         * Opening for writing was refused, which on this system means a
         * person declined the prompt or nobody is signed in to be asked.
         * Falling back to read-only is the useful behaviour and is what
         * SQLite's own unix VFS does for a file it may read and not
         * write: the caller is told through out_flags and a later write
         * gets SQLITE_READONLY rather than a failure to open at all.
         */
        if (!(flags & SQLITE_OPEN_EXCLUSIVE)) {
            fd = open(f->path, O_RDONLY);
            if (fd >= 0) flags = (flags & ~SQLITE_OPEN_READWRITE) |
                                 SQLITE_OPEN_READONLY;
        }
    }
    if (fd < 0) return vx_errno_to_sqlite(errno, SQLITE_CANTOPEN);

    f->fd = fd;
    f->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) ? 1 : 0;
    f->base.pMethods = &vx_io_methods;

    if (out_flags) {
        *out_flags = (flags & SQLITE_OPEN_READWRITE) ? SQLITE_OPEN_READWRITE
                                                     : SQLITE_OPEN_READONLY;
    }
    return SQLITE_OK;
}

static int vx_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    (void)vfs;
    (void)sync_dir;             /* no directory to sync; see xSync */
    if (unlink(name) != 0) {
        if (errno == ENOENT) return SQLITE_OK;   /* already gone */
        return vx_errno_to_sqlite(errno, SQLITE_IOERR_DELETE);
    }
    return SQLITE_OK;
}

static int vx_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    (void)vfs;
    struct stat st;
    *out = 0;
    if (stat(name, &st) != 0) return SQLITE_OK;   /* absent, not an error */

    switch (flags) {
    case SQLITE_ACCESS_EXISTS:
        *out = 1;
        break;
    case SQLITE_ACCESS_READ:
        *out = 1;
        break;
    case SQLITE_ACCESS_READWRITE:
        /*
         * Answered yes for anything that exists, and that is the honest
         * answer rather than an optimistic one. Whether a file may be
         * written here does not depend on the file: it depends on the
         * profile the path is in and on somebody answering a prompt, and
         * neither is knowable in advance. <unistd.h> says the same thing
         * about access(2) for the same reason.
         */
        *out = 1;
        break;
    default:
        break;
    }
    return SQLITE_OK;
}

static int vx_full_pathname(sqlite3_vfs *vfs, const char *in, int nOut,
                            char *out) {
    (void)vfs;
    if (!in || !out || nOut <= 1) return SQLITE_ERROR;

    /*
     * There is no per-process working directory on this system — every
     * path is resolved from the root, which is what getcwd() answering
     * "/" means. So a name that is already absolute is already full, and
     * one that is not gets a leading slash rather than a directory
     * prefix that does not exist.
     */
    int i = 0;
    if (in[0] != '/') out[i++] = '/';
    for (int k = 0; in[k] && i < nOut - 1; k++) out[i++] = in[k];
    out[i] = '\0';
    return SQLITE_OK;
}

/* ---- no dynamic loading ----
 *
 * SQLITE_OMIT_LOAD_EXTENSION is defined, so the engine never calls
 * these; they are null in the structure below rather than stubs that
 * would have to decide what to return.
 */

static int vx_randomness(sqlite3_vfs *vfs, int nByte, char *out) {
    (void)vfs;
    /*
     * The kernel's hardware source, through getentropy — which fills the
     * buffer completely or fails, rather than returning what it managed.
     * SQLite uses this to salt its rollback journal headers, where a
     * partly-filled buffer would be a quiet weakness rather than a
     * visible failure.
     *
     * getentropy takes at most 256 bytes at a time, so this loops.
     */
    int done = 0;
    while (done < nByte) {
        int chunk = nByte - done;
        if (chunk > 256) chunk = 256;
        if (getentropy(out + done, (size_t)chunk) != 0) {
            /* No entropy at all is a real possibility — RDRAND can fail
             * under load. Reporting how much was obtained lets SQLite
             * fall back to its own mixing rather than trusting zeroes. */
            return done;
        }
        done += chunk;
    }
    return done;
}

static int vx_sleep(sqlite3_vfs *vfs, int microseconds) {
    (void)vfs;
    usleep((unsigned long)microseconds);
    return microseconds;
}

static int vx_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out) {
    (void)vfs;
    /*
     * Milliseconds of a Julian day, which is what SQLite wants.
     *
     * The epoch here is when the machine started, not 1970 — nothing
     * carries the real-time clock across the system call boundary, and
     * <time.h> has said so since it was written. So a timestamp stored
     * by CURRENT_TIMESTAMP reads as a date in January 1970 plus the
     * uptime.
     *
     * That is wrong and it is *honestly* wrong: the interval between two
     * rows is exact, which is what a history table is usually asked, and
     * the absolute date is visibly implausible rather than plausibly
     * incorrect. The day something carries the RTC up, this line is
     * where it lands.
     */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return SQLITE_ERROR;

    static const sqlite3_int64 unix_epoch_as_julian_ms = 24405875LL * 8640000LL;
    *out = unix_epoch_as_julian_ms +
           (sqlite3_int64)ts.tv_sec * 1000 +
           (sqlite3_int64)ts.tv_nsec / 1000000;
    return SQLITE_OK;
}

static int vx_current_time(sqlite3_vfs *vfs, double *out) {
    sqlite3_int64 ms = 0;
    const int rc = vx_current_time_int64(vfs, &ms);
    *out = (double)ms / 86400000.0;
    return rc;
}

static int vx_get_last_error(sqlite3_vfs *vfs, int nBuf, char *buf) {
    (void)vfs;
    /* The reason the last call failed, as a sentence. SQLite only ever
     * shows this to a person. */
    if (nBuf > 0 && buf) {
        const char *msg = strerror(errno);
        int i = 0;
        while (msg[i] && i < nBuf - 1) { buf[i] = msg[i]; i++; }
        buf[i] = '\0';
    }
    return errno;
}

static sqlite3_vfs vx_vfs = {
    2,                          /* iVersion                        */
    (int)sizeof(vx_file),       /* szOsFile                        */
    256,                        /* mxPathname: FS_PATH_MAX         */
    0,                          /* pNext, filled in by SQLite      */
    "vextro",
    0,                          /* pAppData                        */
    vx_open,
    vx_delete,
    vx_access,
    vx_full_pathname,
    0, 0, 0, 0,                 /* xDl*: no dynamic loader         */
    vx_randomness,
    vx_sleep,
    vx_current_time,
    vx_get_last_error,
    vx_current_time_int64
};

/* ============================================================
 *  the two hooks SQLITE_OS_OTHER demands
 * ============================================================
 *
 * With no built-in operating system layer, the engine calls these on the
 * first sqlite3_initialize() and expects a default VFS to exist
 * afterwards. Registering with makeDflt=1 is what makes sqlite3_open()
 * work without every caller naming "vextro".
 */

int sqlite3_os_init(void) {
    return sqlite3_vfs_register(&vx_vfs, 1);
}

int sqlite3_os_end(void) {
    return SQLITE_OK;
}

/* ============================================================
 *  mutexes
 * ============================================================
 *
 * ---- why these are installed at run time rather than compiled in ----
 *
 * The amalgamation chooses its mutex implementation like this:
 *
 *     #if SQLITE_THREADSAFE && !defined(SQLITE_MUTEX_NOOP)
 *     #  if SQLITE_OS_UNIX
 *     #    define SQLITE_MUTEX_PTHREADS
 *     #  elif SQLITE_OS_WIN
 *     #    define SQLITE_MUTEX_W32
 *     #  else
 *     #    define SQLITE_MUTEX_NOOP
 *     #  endif
 *     #endif
 *
 * With SQLITE_OS_OTHER neither branch matches, so SQLITE_MUTEX_NOOP is
 * defined whatever the command line says — and defining
 * SQLITE_MUTEX_PTHREADS as well compiles *both* implementations, which
 * is two definitions of sqlite3DefaultMutex and a sqlite3MemoryBarrier
 * that is a macro and a function at once. That was tried; it does not
 * build, and it cannot be made to.
 *
 * SQLite's own answer for a custom operating system is
 * sqlite3_config(SQLITE_CONFIG_MUTEX), which replaces the methods before
 * anything uses them. That is what vx_sqlite_init() below does. It has
 * to run before sqlite3_initialize(), because the engine brings the
 * mutex subsystem up first — so a program that calls sqlite3_open()
 * without it gets the no-op mutexes and a connection that is only safe
 * on one thread.
 *
 * The alternative was SQLITE_THREADSAFE=0, which is honest and gives up
 * the engine's serialisation entirely. This keeps it.
 */

#include <pthread.h>

/*
 * The static mutexes SQLite asks for by number — the main one, the
 * allocator's, the page cache's and so on. Twelve covers every
 * SQLITE_MUTEX_STATIC_* in 3.45; a request outside the range is refused
 * rather than wrapped, because a mutex that is silently shared with
 * another subsystem is a deadlock nobody will find.
 */
#define VX_STATIC_MUTEX_COUNT 12

typedef struct vx_mutex {
    pthread_mutex_t m;
    int             id;
    volatile int    depth;      /* for the assert-only held/notheld */
    volatile unsigned long owner;
    int             in_use;
} vx_mutex;

static vx_mutex vx_static_mutex[VX_STATIC_MUTEX_COUNT];
static int      vx_mutex_ready = 0;

static int vx_mutex_init(void) {
    if (vx_mutex_ready) return SQLITE_OK;
    for (int i = 0; i < VX_STATIC_MUTEX_COUNT; i++) {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        /* Recursive throughout: SQLITE_MUTEX_RECURSIVE is a real
         * request the engine makes, and a static mutex that is entered
         * twice by one thread must not deadlock. */
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&vx_static_mutex[i].m, &a);
        pthread_mutexattr_destroy(&a);
        vx_static_mutex[i].id = i;
        vx_static_mutex[i].depth = 0;
        vx_static_mutex[i].owner = 0;
        vx_static_mutex[i].in_use = 1;
    }
    vx_mutex_ready = 1;
    return SQLITE_OK;
}

static int vx_mutex_end(void) {
    if (!vx_mutex_ready) return SQLITE_OK;
    for (int i = 0; i < VX_STATIC_MUTEX_COUNT; i++)
        pthread_mutex_destroy(&vx_static_mutex[i].m);
    vx_mutex_ready = 0;
    return SQLITE_OK;
}

/*
 * ---- the allocator here is libc's, and that is load-bearing ----
 *
 * Not sqlite3_malloc. The public allocator begins with
 *
 *     if( sqlite3_initialize() ) return 0;
 *
 * and a mutex can be allocated *during* sqlite3_initialize, before
 * isInit is set — so xMutexAlloc calling sqlite3_malloc re-enters
 * initialize, which allocates another mutex, which re-enters again.
 *
 * That is not hypothetical. It was the first thing this port did on the
 * machine: a page fault on a write 696 kilobytes below the bottom of a
 * 256-kilobyte stack, three lines into the test, with nothing in the
 * trace to say why. Unbounded recursion looks exactly like that and
 * nothing else does.
 *
 * SQLite's own pthreads implementation avoids it by using the *internal*
 * sqlite3MallocZero, which does not auto-initialise. That symbol is
 * SQLITE_PRIVATE and not visible from here, so this uses the C library's
 * malloc — which has no opinion about SQLite's initialisation state and
 * cannot re-enter it.
 */
static sqlite3_mutex *vx_mutex_alloc(int id) {
    if (id == SQLITE_MUTEX_FAST || id == SQLITE_MUTEX_RECURSIVE) {
        vx_mutex *p = (vx_mutex *)malloc(sizeof(vx_mutex));
        if (!p) return 0;
        memset(p, 0, sizeof(*p));
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&p->m, &a);
        pthread_mutexattr_destroy(&a);
        p->id = id;
        p->in_use = 1;
        return (sqlite3_mutex *)p;
    }
    if (id >= 0 && id < VX_STATIC_MUTEX_COUNT) {
        if (!vx_mutex_ready) vx_mutex_init();
        return (sqlite3_mutex *)&vx_static_mutex[id];
    }
    return 0;
}

static void vx_mutex_free(sqlite3_mutex *m) {
    vx_mutex *p = (vx_mutex *)m;
    if (!p) return;
    /* A static one is never freed: it is not this allocation's to give
     * back, and SQLite does not ask. */
    if (p >= &vx_static_mutex[0] && p < &vx_static_mutex[VX_STATIC_MUTEX_COUNT])
        return;
    pthread_mutex_destroy(&p->m);
    free(p);
}

static void vx_mutex_enter(sqlite3_mutex *m) {
    vx_mutex *p = (vx_mutex *)m;
    if (!p) return;
    pthread_mutex_lock(&p->m);
    p->owner = (unsigned long)pthread_self();
    p->depth++;
}

static int vx_mutex_try(sqlite3_mutex *m) {
    vx_mutex *p = (vx_mutex *)m;
    if (!p) return SQLITE_OK;
    if (pthread_mutex_trylock(&p->m) != 0) return SQLITE_BUSY;
    p->owner = (unsigned long)pthread_self();
    p->depth++;
    return SQLITE_OK;
}

static void vx_mutex_leave(sqlite3_mutex *m) {
    vx_mutex *p = (vx_mutex *)m;
    if (!p) return;
    p->depth--;
    if (p->depth == 0) p->owner = 0;
    pthread_mutex_unlock(&p->m);
}

/* Both of these exist for assert() and are compiled out with NDEBUG.
 * Answering truthfully anyway costs nothing and makes a debug build of
 * the engine actually check what it thinks it is checking. */
static int vx_mutex_held(sqlite3_mutex *m) {
    vx_mutex *p = (vx_mutex *)m;
    return !p || (p->depth > 0 &&
                  p->owner == (unsigned long)pthread_self());
}

static int vx_mutex_notheld(sqlite3_mutex *m) {
    return !vx_mutex_held(m);
}

static const sqlite3_mutex_methods vx_mutex_methods = {
    vx_mutex_init,
    vx_mutex_end,
    vx_mutex_alloc,
    vx_mutex_free,
    vx_mutex_enter,
    vx_mutex_try,
    vx_mutex_leave,
    vx_mutex_held,
    vx_mutex_notheld
};

/*
 * Bring the engine up on this system's mutexes.
 *
 * Must be called before the first sqlite3_open(). sqlite3_initialize()
 * starts the mutex subsystem before it calls sqlite3_os_init(), so
 * installing the methods from inside the VFS registration would be one
 * step too late — this is the only place they can go.
 *
 * Idempotent, and safe to call from several threads: sqlite3_config
 * refuses once the library is initialised, which is exactly the
 * condition that makes a second call unnecessary.
 */
int vx_sqlite_init(void) {
    static int done = 0;
    if (done) return SQLITE_OK;

    int rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &vx_mutex_methods);
    if (rc != SQLITE_OK && rc != SQLITE_MISUSE) return rc;

    rc = sqlite3_initialize();
    if (rc == SQLITE_OK) done = 1;
    return rc;
}
