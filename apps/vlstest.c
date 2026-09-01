/*
 * vlstest — the Vextro Linux Subset, asked to prove itself on the
 * machine.
 *
 * Everything here is a property that would be *silently* wrong if the
 * translation layer were subtly incorrect, which is why each one is
 * checked rather than demonstrated. A router that returns plausible
 * numbers for calls it has mistranslated is worse than one that refuses
 * them, and nothing but a program running in ring 3 on the real kernel
 * can tell the two apart.
 *
 * ---- how a native program makes Linux calls ----
 *
 * By adding the bias. include/vls.h explains why the bias exists as well
 * as the personality flag, and this file is half the answer: it is a
 * Vextro program, built against this tree's own C library, and it needs
 * to keep printing while it exercises a numbering in which 1 means
 * `write` rather than SYS_PRINT. Setting the personality would take
 * printf away on the first call.
 *
 * The other half is section 12, where a forked child *does* set the
 * personality — and from that instant speaks nothing but raw Linux
 * system calls, because its libc is now wrong about every number in it.
 * That child is deliberately written in inline assembly for exactly that
 * reason, and it reports by exiting with a status its parent reads.
 *
 * ---- what each section is for ----
 *
 *   1  The renumbering itself, on the calls where the answer is checkable
 *      against something else this program can see.
 *   2  Refusals. A subset is defined by what it says no to, and it has
 *      to say no in the way the asking program expects: ENOTTY for a
 *      terminal question and ENOSYS for a call that is not there.
 *   3  Files, through Linux numbers and Linux structures — struct stat
 *      is a hundred and forty-four bytes here and thirty-two on the
 *      other side of the translation.
 *   4  The device nodes, each checked for the behaviour that is the
 *      reason it exists.
 *   5  getdents64, which is the one call whose *records* are rebuilt
 *      rather than forwarded.
 *   6  dup and dup2, including the two cases this system refuses.
 *   7  Signals delivered to this process and returned from — the part
 *      that proves a frame was laid on the stack and taken off again
 *      with every register intact.
 *   8  fork, wait4 and SIGCHLD together, because none of the three is
 *      checkable without the other two.
 *   9  SIGKILL, which must not be catchable.
 *  10  SIGSEGV, delivered from a real page fault, with the faulting
 *      address in si_addr.
 *  11  execve, checked by executing this same program with an argument
 *      that makes it exit with a number nothing else would produce.
 *  12  The personality flag.
 *  13  clone(CLONE_VM), which is the one call native SYS_CLONE cannot
 *      express and which needed a new primitive in the scheduler.
 */

#include "vextro.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* ============================================================
 *  making a Linux call from a program that is not one
 * ============================================================ */

#define VLS_BIAS 0x40000000L

static inline long vsys6(long n, long a, long b, long c,
                         long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8v __asm__("r8")  = e;
    register long r9v __asm__("r9")  = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8v), "r"(r9v)
                     : "rcx", "r11", "memory", VX_CLOBBER_XMM);
    return r;
}

/* A Linux call, by its Linux number. */
static long lx(long n, long a, long b, long c, long d, long e, long f) {
    return vsys6(VLS_BIAS + n, a, b, c, d, e, f);
}
#define LX0(n)                   lx((n), 0, 0, 0, 0, 0, 0)
#define LX1(n, a)                lx((n), (long)(a), 0, 0, 0, 0, 0)
#define LX2(n, a, b)             lx((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define LX3(n, a, b, c)          lx((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define LX4(n, a, b, c, d)       lx((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define LX5(n, a, b, c, d, e)    lx((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define LX6(n, a, b, c, d, e, f) lx((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* And a native one, for the three that have no Linux equivalent worth
 * routing: the personality switch itself, and printing. */
static long nat3(long n, long a, long b, long c) {
    return vsys6(n, a, b, c, 0, 0, 0);
}

/*
 * Sleeping, as its own function.
 *
 * Not written inline at the three places that need it because a
 * compound literal — `(long[2]){ 0, 20000000 }` — carries a comma that
 * the preprocessor reads as an argument separator, so the macro above
 * sees three arguments where it wants two. A named local is clearer than
 * the extra parentheses that would hide it.
 */
static long lsleep_ns(long ns);

/* Linux numbers, named. Only the ones used below. */
#define L_read      0
#define L_write     1
#define L_open      2
#define L_close     3
#define L_stat      4
#define L_fstat     5
#define L_lseek     8
#define L_mmap      9
#define L_munmap   11
#define L_ioctl    16
#define L_writev   20
#define L_access   21
#define L_yield    24
#define L_dup      32
#define L_dup2     33
#define L_nanosleep 35
#define L_getpid   39
#define L_clone    56
#define L_fork     57
#define L_execve   59
#define L_exit     60
#define L_wait4    61
#define L_kill     62
#define L_uname    63
#define L_readlink 89
#define L_gettimeofday 96
#define L_getuid  102
#define L_getppid 110
#define L_sigaltstack 131
#define L_gettid  186
#define L_time    201
#define L_getdents64 217
#define L_clock_gettime 228
#define L_exit_group 231
#define L_openat  257
#define L_getrandom 318
#define L_rt_sigaction   13
#define L_rt_sigprocmask 14

#define L_AT_FDCWD (-100)

/*
 * The signal numbers, the sigaction flags and the errno values come from
 * <signal.h> and <errno.h> now, and that is a deliberate narrowing of
 * what this file duplicates.
 *
 * Section 14 exercises the C library, so those headers are included
 * anyway, and a second set of #defines beside them would be two things
 * to keep in step for no gain — the numbers are Linux's on both sides
 * because the header says so at the point it defines them.
 *
 * What is still written out below is the part where a duplicate is the
 * whole test: the Linux *call numbers*, and the *layouts* of the four
 * structures that cross the boundary. Those are what the kernel could
 * get wrong, and taking them from the kernel's own header would make
 * every check in this file circular.
 */

/* What rt_sigaction takes. Fixed layout, and the kernel's copy is in
 * include/vls.h — the two are the same four words in the same order,
 * which is the whole of the contract. */
struct k_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

/* Linux's struct stat on x86-64, by position. Written out rather than
 * included because the point of the test is that the *kernel* puts the
 * fields where Linux puts them, and a header shared with the kernel
 * would make the check circular. */
struct lstat {
    unsigned long st_dev, st_ino, st_nlink;
    unsigned int  st_mode, st_uid, st_gid, __pad0;
    unsigned long st_rdev, st_size, st_blksize;
    long          st_blocks;
    long          at_sec, at_nsec, mt_sec, mt_nsec, ct_sec, ct_nsec;
    long          __unused[3];
};

struct ldirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[1];
};

/* The three fields of siginfo_t anything actually reads. */
struct lsiginfo {
    int           si_signo, si_errno, si_code, __pad;
    unsigned long si_addr;
};

#define S_IFMT   0170000u
#define S_IFREG  0100000u
#define S_IFDIR  0040000u
#define S_IFCHR  0020000u



static long lsleep_ns(long ns) {
    long ts[2] = { 0, ns };
    return LX2(L_nanosleep, ts, 0);
}

/* ============================================================
 *  the ledger
 * ============================================================ */

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Reported alongside a failure so that a wrong number is diagnosable
 * without a second run. */
static void check_eq(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: got %ld, wanted %ld\n", what, got, want);
    }
}

/* ============================================================
 *  1. the renumbering
 * ============================================================ */

static void t_router(void) {
    char uts[65 * 6];
    memset(uts, 0xAA, sizeof(uts));
    check_eq("uname answers", LX1(L_uname, uts), 0);
    check("uname sysname is Linux", strcmp(uts, "Linux") == 0);
    check("uname machine is x86_64", strcmp(uts + 65 * 4, "x86_64") == 0);
    /* The field nothing branches on is where the truth goes. */
    check("uname version names the subset",
          strstr(uts + 65 * 3, "Vextro") != NULL);

    const long pid  = LX0(L_getpid);
    const long tid  = LX0(L_gettid);
    check("getpid answers", pid > 0);
    /* One thread, so the process identity and the thread identity are
     * the same number — which is exactly why the distinction between
     * them went unnoticed on this system for so long. */
    check_eq("getpid == gettid while single-threaded", tid, pid);

    /* Launched by the desktop rather than forked, so there is no
     * parent — unless this is the child of section 11, which never gets
     * here. */
    check("getppid answers something", LX0(L_getppid) >= 0);

    /*
     * Not zero, and that is the check rather than an implementation
     * detail: a great deal of ported code reads getuid() == 0 as "may do
     * anything", and nothing on this system may.
     */
    check("getuid is not root", LX0(L_getuid) != 0);

    check_eq("sched_yield answers", LX0(L_yield), 0);

    long tv[2] = { 0, 0 };
    check_eq("gettimeofday answers", LX2(L_gettimeofday, tv, 0), 0);
    /* Later than the release this was written for, which is the weakest
     * assertion that still catches a clock reading zero — the failure
     * the whole of SYS_WALLCLOCK exists to prevent. */
    check("gettimeofday is after 2020", tv[0] > 1577836800L);

    const long secs = LX1(L_time, 0);
    check("time agrees with gettimeofday",
          secs >= tv[0] && secs <= tv[0] + 4);

    long ts[2] = { 0, 0 };
    check_eq("clock_gettime(REALTIME)", LX2(L_clock_gettime, 0, ts), 0);
    check("REALTIME agrees with time", ts[0] >= secs && ts[0] <= secs + 4);

    long m1[2] = { 0, 0 }, m2[2] = { 0, 0 };
    LX2(L_clock_gettime, 1, m1);
    lsleep_ns(20000000L);
    LX2(L_clock_gettime, 1, m2);
    /*
     * The two clocks are different quantities here and are answered from
     * different places — the CMOS reading and the scheduler tick — so a
     * monotonic clock that had been wired to the wall clock by mistake
     * would pass every check above and fail this one.
     */
    check("CLOCK_MONOTONIC advances across a sleep",
          (m2[0] - m1[0]) * 1000000000L + (m2[1] - m1[1]) >= 10000000L);
    check("CLOCK_MONOTONIC is not the wall clock", m2[0] < tv[0]);
}

/* ============================================================
 *  2. refusals
 * ============================================================ */

static void t_refusals(void) {
    /*
     * A number with no row in the table. Refused with ENOSYS and, on the
     * serial line, reported once — the report is not checkable from
     * here, but the *stability* of the answer is, and a router that
     * refused only the first time would be worse than one that never
     * did.
     */
    check_eq("an unmapped vector is ENOSYS", LX1(499, 0), -ENOSYS);
    check_eq("and again, identically", LX1(499, 0), -ENOSYS);

    /*
     * ENOTTY and not ENOSYS, and the difference decides how a port
     * behaves: isatty() reads ENOTTY as "not a terminal, carry on" and
     * ENOSYS as "this system is broken".
     */
    check_eq("ioctl is ENOTTY", LX3(L_ioctl, 1, 0x5401, 0), -ENOTTY);

    /* An alternate signal stack cannot be given, because the frame is
     * laid on the interrupted stack and there is nowhere else to put
     * it. Refused rather than accepted and not honoured. */
    check_eq("sigaltstack is ENOSYS", LX2(L_sigaltstack, 0, 0), -ENOSYS);

    /* There are no symbolic links on this volume, so both answers are
     * exact: EINVAL for a file that is not one, ENOENT for a path that
     * is not there. */
    char lb[64];
    check_eq("readlink of a real file is EINVAL",
             LX3(L_readlink, "/about.txt", lb, sizeof(lb)), -EINVAL);
    check_eq("readlink of nothing is ENOENT",
             LX3(L_readlink, "/no/such/file", lb, sizeof(lb)), -ENOENT);
}

/* ============================================================
 *  3. files, in Linux's numbering and Linux's structures
 * ============================================================ */

static void t_files(void) {
    const long fd = LX3(L_open, "/about.txt", O_RDONLY, 0);
    check("open through the Linux number", fd >= 0);
    if (fd < 0) return;

    char buf[64];
    const long n = LX3(L_read, fd, buf, sizeof(buf));
    check("read returns bytes", n > 0);

    check_eq("lseek to the start", LX3(L_lseek, fd, 0, 0), 0);
    char again[64];
    const long n2 = LX3(L_read, fd, again, sizeof(again));
    check("the same bytes after seeking back",
          n2 == n && memcmp(buf, again, (size_t)n) == 0);

    /*
     * The structure the kernel fills is thirty-two bytes; this one is a
     * hundred and forty-four with the fields in Linux's places. The
     * translation is the thing under test, so the fields are read by
     * name out of a layout written from the ABI rather than from the
     * kernel's header.
     */
    struct lstat st;
    memset(&st, 0xAA, sizeof(st));
    check_eq("fstat answers", LX2(L_fstat, fd, &st), 0);
    check("fstat says it is a regular file",
          (st.st_mode & S_IFMT) == S_IFREG);
    check("fstat size is plausible", st.st_size > 0 && st.st_size < (1 << 20));
    check("fstat fills st_blksize", st.st_blksize == 4096);
    check("fstat fills st_nlink", st.st_nlink >= 1);

    LX1(L_close, fd);

    struct lstat ps;
    check_eq("stat by path", LX2(L_stat, "/about.txt", &ps), 0);
    check("stat and fstat agree about the size", ps.st_size == st.st_size);

    struct lstat ds;
    check_eq("stat of the root", LX2(L_stat, "/", &ds), 0);
    check("the root is a directory", (ds.st_mode & S_IFMT) == S_IFDIR);

    check_eq("openat with AT_FDCWD",
             (LX4(L_openat, L_AT_FDCWD, "/about.txt", O_RDONLY, 0) >= 0),
             1);
    /* A descriptor other than AT_FDCWD is refused rather than silently
     * resolved against the one working directory this system has. */
    check_eq("openat with a real descriptor is refused",
             LX4(L_openat, 4, "about.txt", O_RDONLY, 0), -EOPNOTSUPP);

    check_eq("access finds a file that exists",
             LX2(L_access, "/about.txt", 0), 0);
    check_eq("access does not find one that does not",
             LX2(L_access, "/no/such/file", 0), -ENOENT);
}

/* ============================================================
 *  4. the device nodes
 * ============================================================ */

static void t_devices(void) {
    char buf[64];

    long fd = LX3(L_open, "/dev/null", O_RDWR, 0);
    check("/dev/null opens", fd >= 0);
    if (fd >= 0) {
        check_eq("reading /dev/null is end of file",
                 LX3(L_read, fd, buf, sizeof(buf)), 0);
        /* The whole reason the node exists: a caller that loops until
         * everything is written has to terminate. */
        check_eq("writing to /dev/null consumes everything",
                 LX3(L_write, fd, "hello", 5), 5);
        LX1(L_close, fd);
    }

    fd = LX3(L_open, "/dev/zero", O_RDONLY, 0);
    check("/dev/zero opens", fd >= 0);
    if (fd >= 0) {
        memset(buf, 0x5A, sizeof(buf));
        check_eq("/dev/zero fills the buffer",
                 LX3(L_read, fd, buf, sizeof(buf)), (long)sizeof(buf));
        int allzero = 1;
        for (unsigned i = 0; i < sizeof(buf); i++) if (buf[i]) allzero = 0;
        check("/dev/zero reads zeroes", allzero);
        LX1(L_close, fd);
    }

    /* The one node whose purpose is to fail, which is the only way a
     * test suite can exercise its own error path. */
    fd = LX3(L_open, "/dev/full", O_WRONLY, 0);
    check("/dev/full opens", fd >= 0);
    if (fd >= 0) {
        check_eq("writing to /dev/full is ENOSPC",
                 LX3(L_write, fd, "x", 1), -ENOSPC);
        LX1(L_close, fd);
    }

    for (int pass = 0; pass < 2; pass++) {
        const char *path = pass ? "/dev/random" : "/dev/urandom";
        fd = LX3(L_open, path, O_RDONLY, 0);
        check(pass ? "/dev/random opens" : "/dev/urandom opens", fd >= 0);
        if (fd < 0) continue;
        unsigned char r[32];
        memset(r, 0, sizeof(r));
        const long got = LX3(L_read, fd, r, sizeof(r));
        check(pass ? "/dev/random gives bytes" : "/dev/urandom gives bytes",
              got > 0);
        int nonzero = 0;
        for (long i = 0; i < got; i++) if (r[i]) nonzero++;
        /* Thirty-two zero bytes from a hardware generator is not
         * entropy; it is a wire that is not connected. */
        check(pass ? "/dev/random is not all zeroes"
                   : "/dev/urandom is not all zeroes", nonzero > 0);
        LX1(L_close, fd);
    }

    /* getrandom, which is where a TLS library looks first. */
    unsigned char g[16];
    memset(g, 0, sizeof(g));
    check_eq("getrandom fills its buffer",
             LX3(L_getrandom, g, sizeof(g), 0), (long)sizeof(g));

    struct lstat st;
    check_eq("stat of /dev/null", LX2(L_stat, "/dev/null", &st), 0);
    check("/dev/null is a character device",
          (st.st_mode & S_IFMT) == S_IFCHR);
    check_eq("stat of /dev is a directory",
             (LX2(L_stat, "/dev", &st) == 0 &&
              (st.st_mode & S_IFMT) == S_IFDIR), 1);

    /*
     * The render node, which the specification this was built to asks to
     * be mapped down to the process's framebuffer — so what is checked
     * is that a write reaches pixels and a read brings them back.
     */
    fd = LX3(L_open, "/dev/dri/renderD128", O_RDWR, 0);
    check("/dev/dri/renderD128 opens", fd >= 0);
    if (fd >= 0) {
        check_eq("the render node has a length",
                 (LX2(L_fstat, fd, &st) == 0 && st.st_size > 0), 1);
        const unsigned int px[2] = { 0x00112233u, 0x44556677u };
        check_eq("writing pixels", LX3(L_write, fd, px, sizeof(px)),
                 (long)sizeof(px));
        check_eq("seeking back", LX3(L_lseek, fd, 0, 0), 0);
        unsigned int back[2] = { 0, 0 };
        check_eq("reading them", LX3(L_read, fd, back, sizeof(back)),
                 (long)sizeof(back));
        check("the framebuffer kept what was written",
              back[0] == px[0] && back[1] == px[1]);
        LX1(L_close, fd);
    }

    check("/dev/dri/card0 opens too",
          LX3(L_open, "/dev/dri/card0", O_RDONLY, 0) >= 0);
}

/* ============================================================
 *  5. getdents64 — the one call whose records are rebuilt
 * ============================================================ */

static void t_getdents(void) {
    const long fd = LX3(L_open, "/dev", O_RDONLY, 0);
    check("/dev opens as a directory", fd >= 0);
    if (fd < 0) return;

    char buf[1024];
    const long n = LX3(L_getdents64, fd, buf, sizeof(buf));
    check("getdents64 returns bytes", n > 0);

    int saw_null = 0, saw_zero = 0, saw_dri = 0, count = 0, sane = 1;
    for (long off = 0; off < n; ) {
        const struct ldirent64 *d = (const struct ldirent64 *)(buf + off);
        /* The length the kernel wrote is what a program steps the buffer
         * with, so a record that does not advance or that runs past the
         * end is the failure this call has to be checked for. */
        if (d->d_reclen < 20 || off + d->d_reclen > n) { sane = 0; break; }
        if (strcmp(d->d_name, "null") == 0) saw_null = 1;
        if (strcmp(d->d_name, "zero") == 0) saw_zero = 1;
        if (strcmp(d->d_name, "dri") == 0 && d->d_type == 4) saw_dri = 1;
        off += d->d_reclen;
        count++;
    }
    check("every record has a usable length", sane);
    check("the listing has entries", count >= 6);
    check("null, zero and dri are all in /dev",
          saw_null && saw_zero && saw_dri);
    check_eq("a second call is at the end", LX3(L_getdents64, fd, buf,
                                                sizeof(buf)), 0);
    LX1(L_close, fd);
}

/* ============================================================
 *  6. dup and dup2
 * ============================================================ */

static void t_dup(void) {
    const long fd = LX3(L_open, "/dev/null", O_RDWR, 0);
    if (fd < 0) { check("dup: /dev/null opens", 0); return; }

    const long d = LX1(L_dup, fd);
    check("dup gives a second number", d >= 0 && d != fd);
    if (d >= 0) {
        char sink[8];
        check_eq("the duplicate behaves the same",
                 LX3(L_read, d, sink, sizeof(sink)), 0);
        LX1(L_close, d);
    }

    /* The idiom the whole call exists for: point a fixed descriptor
     * somewhere else. Number 9 rather than 1, because redirecting this
     * program's own output would take the report with it. */
    check_eq("dup2 onto a chosen number", LX2(L_dup2, fd, 9), 9);
    char sink9[8];
    check_eq("and it works there", LX3(L_read, 9, sink9, sizeof(sink9)), 0);
    LX1(L_close, 9);

    /*
     * The two cases this system refuses, by name rather than by
     * returning something almost right. A descriptor here *is* the open
     * file description, so two of them cannot share one write-back image
     * or one connection.
     */
    const long wf = LX3(L_open, "/vlsdup.tmp", O_WRONLY | O_CREAT | O_TRUNC,
                        0644);
    if (wf >= 0) {
        check_eq("dup of a file open for writing is refused",
                 LX1(L_dup, wf), -EOPNOTSUPP);
        LX1(L_close, wf);
    } else {
        /* Writing needs an answer at the keyboard, and a headless run
         * has nobody to give one. Not a failure of dup. */
        check("dup: a writable file could not be opened here", 1);
    }

    check_eq("dup of a closed descriptor is EBADF", LX1(L_dup, 55), -EBADF);
    LX1(L_close, fd);
}

/* ============================================================
 *  7. signals, delivered and returned from
 * ============================================================ */

static volatile int usr1_hits = 0, usr2_hits = 0, chld_hits = 0;
static volatile long usr1_sig = 0;

static void on_usr1(int sig) { usr1_hits++; usr1_sig = sig; }
static void on_usr2(int sig) { (void)sig; usr2_hits++; }
static void on_chld(int sig) { (void)sig; chld_hits++; }

static long set_handler(int sig, void (*fn)(int), unsigned long flags) {
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.handler = (unsigned long)fn;
    sa.flags   = flags;
    /* No SA_RESTORER: this system provides a trampoline on the shared
     * page, which is the whole reason the call bias exists as well as
     * the personality flag. */
    return LX4(L_rt_sigaction, sig, &sa, 0, 8);
}

static void t_signals(void) {
    check_eq("installing a SIGUSR1 handler",
             set_handler(SIGUSR1, on_usr1, 0), 0);

    const long pid = LX0(L_getpid);

    /*
     * The strongest single check in this file.
     *
     * If the handler runs, a frame was built on this thread's own stack,
     * the registers were saved into it, and RIP was redirected. If the
     * line after the kill also runs, sigreturn took the frame off again
     * and restored every one of them — because `pid` and the return
     * address of this function were in registers or on the stack across
     * the whole of it. A layer that delivered but could not return would
     * pass the first half and crash on the second.
     */
    usr1_hits = 0;
    check_eq("kill(self, SIGUSR1)", LX2(L_kill, pid, SIGUSR1), 0);
    check_eq("the handler ran", usr1_hits, 1);
    check_eq("and was told which signal", usr1_sig, SIGUSR1);
    check_eq("and execution continued past it", LX0(L_getpid), pid);

    /*
     * Blocked, then unblocked. The delivery happens on the way out of
     * the unblocking call itself, which is the only boundary between the
     * two lines.
     */
    check_eq("installing a SIGUSR2 handler",
             set_handler(SIGUSR2, on_usr2, 0), 0);
    unsigned long mask = 1UL << (SIGUSR2 - 1);
    check_eq("blocking SIGUSR2",
             LX4(L_rt_sigprocmask, SIG_BLOCK, &mask, 0, 8), 0);
    usr2_hits = 0;
    LX2(L_kill, pid, SIGUSR2);
    check_eq("a blocked signal is not delivered", usr2_hits, 0);
    check_eq("unblocking it",
             LX4(L_rt_sigprocmask, SIG_UNBLOCK, &mask, 0, 8), 0);
    check_eq("and then it is", usr2_hits, 1);

    /* The mask can be read back. */
    unsigned long now = ~0UL;
    check_eq("reading the mask back",
             LX4(L_rt_sigprocmask, 0, 0, &now, 8), 0);
    check_eq("SIGUSR2 is no longer blocked", (long)(now & mask), 0);

    /* SIG_IGN, and the POSIX rule that a signal pending when it is
     * ignored is discarded rather than delivered later. */
    struct k_sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.handler = (unsigned long)(void *)SIG_IGN;
    check_eq("ignoring SIGUSR1", LX4(L_rt_sigaction, SIGUSR1, &ign, 0, 8), 0);
    usr1_hits = 0;
    LX2(L_kill, pid, SIGUSR1);
    LX0(L_getpid);                       /* a delivery point, if any */
    check_eq("an ignored signal is not delivered", usr1_hits, 0);

    /* And it cannot be caught at all if it is one of the two. */
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.handler = (unsigned long)on_usr1;
    check_eq("SIGKILL cannot be caught",
             LX4(L_rt_sigaction, SIGKILL, &sa, 0, 8), -EINVAL);

    /* Signalling a process that does not exist. */
    check_eq("kill of a pid nobody has is ESRCH",
             LX2(L_kill, 60000, SIGUSR1), -3);
    /* And the existence probe, which sends nothing. */
    check_eq("kill(self, 0) is the existence probe",
             LX2(L_kill, pid, 0), 0);
}

/* ============================================================
 *  8-10. children
 * ============================================================ */

/* The address the SIGSEGV child touches: below the lowest page any
 * image, stack or heap is placed at, so it is unmapped in every process
 * this system builds. */
#define BAD_ADDR 0x12340UL

static volatile unsigned long segv_seen_addr = 0;
static volatile int segv_hits = 0;

static void on_segv(int sig, struct lsiginfo *si, void *uc) {
    (void)uc;
    segv_hits++;
    segv_seen_addr = si ? si->si_addr : 0;
    /*
     * Ends the process from inside the handler rather than returning.
     *
     * Returning would resume the instruction that faulted, which would
     * fault again, forever — that is what a synchronous signal means and
     * it is why Linux forces the disposition back to default when one is
     * blocked. The status carries the verdict out to the parent, which
     * is the only place it can be read from: this is a forked child and
     * its memory is its own.
     */
    const int ok = (sig == SIGSEGV && si != 0 &&
                    si->si_addr == BAD_ADDR && si->si_signo == SIGSEGV);
    LX1(L_exit_group, ok ? 42 : 43);
    for (;;) LX0(L_yield);
}

static long wait_one(long *status) {
    *status = 0;
    return LX4(L_wait4, -1, status, 0, 0);
}

static void t_children(void) {
    check_eq("installing a SIGCHLD handler",
             set_handler(SIGCHLD, on_chld, 0), 0);
    chld_hits = 0;

    /* ---- an ordinary child, an ordinary status ---- */
    long kid = LX0(L_fork);
    check("fork answers", kid >= 0);
    if (kid == 0) LX1(L_exit_group, 7);

    long st = 0;
    const long got = wait_one(&st);
    check_eq("wait4 returns the child", got, kid);
    check_eq("and its exit status", WEXITSTATUS(st), 7);
    /* Posted by the same code that recorded the status, so a SIGCHLD
     * that never arrived would mean the two had been separated. */
    check("SIGCHLD arrived", chld_hits >= 1);

    /* ---- and one that is killed ---- */
    kid = LX0(L_fork);
    if (kid == 0) {
        /* Sleeps rather than spins, so the parent's kill has to reach a
         * thread that is parked in the kernel — which is the case that
         * cannot wait for a system call boundary and is why a fatal
         * signal acts immediately. */
        for (;;) lsleep_ns(50000000L);
    }
    check_eq("kill(child, SIGKILL)", LX2(L_kill, kid, SIGKILL), 0);
    st = 0;
    check_eq("wait4 collects the killed child", wait_one(&st), kid);
    check_eq("and says which signal killed it", WTERMSIG(st), SIGKILL);

    /* ---- and one that faults ---- */
    kid = LX0(L_fork);
    if (kid == 0) {
        struct k_sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.handler = (unsigned long)on_segv;
        sa.flags   = SA_SIGINFO;
        LX4(L_rt_sigaction, SIGSEGV, &sa, 0, 8);
        *(volatile int *)BAD_ADDR = 1;     /* a real page fault */
        LX1(L_exit_group, 44);             /* the handler should not return */
    }
    st = 0;
    check_eq("wait4 collects the faulting child", wait_one(&st), kid);
    /*
     * 42 means the handler ran on a real page fault, was given the
     * faulting address in si_addr, and got the signal number right. 43
     * means it ran with the wrong information; 44 means the fault was
     * never delivered at all and the child fell through.
     */
    check_eq("SIGSEGV reached the handler with the right address",
             WEXITSTATUS(st), 42);
}

/* ============================================================
 *  11. execve
 * ============================================================ */

static void t_execve(void) {
    const long kid = LX0(L_fork);
    if (kid == 0) {
        char *av[3];
        char *ev[2];
        av[0] = (char *)"vlstest";
        av[1] = (char *)"exec-child";
        av[2] = 0;
        ev[0] = (char *)"VLS=1";
        ev[1] = 0;
        LX3(L_execve, "/vlstest", av, ev);
        /* Only reached if the exec failed, which is the answer POSIX
         * gives and the reason exec returns at all. */
        LX1(L_exit_group, 90);
    }
    long st = 0;
    check_eq("wait4 collects the executed child", wait_one(&st), kid);
    /*
     * 77 is written by the branch at the top of _start below, reached
     * only when argv[1] and envp[0] both arrived intact. So one number
     * covers the exec, the argument vector and the environment; 78 means
     * the image ran but its arguments did not survive, and 90 means the
     * exec itself was refused.
     */
    check_eq("execve ran the new image with its arguments",
             WEXITSTATUS(st), 77);
}

/* ============================================================
 *  12. the personality
 * ============================================================ */

#define SYS_PERSONALITY_NATIVE_NR 64

/*
 * A child that has given up the native numbering.
 *
 * From the personality call onwards nothing in the C library is usable:
 * printf would issue SYS_PRINT, which is 1, which is now `write`, and
 * memcpy would be fine but is not worth the risk of a call it makes
 * inside itself. So this is inline assembly and nothing else, and it
 * reports the only way it still can — by exiting with a number.
 *
 * Marked noinline so that nothing from the caller is left half-done in a
 * register across the switch.
 */
static void __attribute__((noinline)) linux_only_child(void) {
    /* Native call 64: which numbering this process speaks. */
    nat3(SYS_PERSONALITY_NATIVE_NR, 1, 0, 0);

    /* From here, *unbiased* Linux numbers. Linux write is 1. */
    static const char msg[] = "vlstest: the child speaks linux\n";
    __asm__ volatile("syscall"
                     :
                     : "a"(1L), "D"(1L), "S"(msg), "d"((long)sizeof(msg) - 1)
                     : "rcx", "r11", "memory");
    /* And Linux exit_group is 231. */
    __asm__ volatile("syscall"
                     :
                     : "a"(231L), "D"(88L)
                     : "rcx", "r11", "memory");
    for (;;) __asm__ volatile("pause");
}

static void t_personality(void) {
    const long kid = LX0(L_fork);
    if (kid == 0) linux_only_child();
    long st = 0;
    check_eq("wait4 collects the linux-personality child", wait_one(&st), kid);
    /*
     * 88 could only have been produced by two unbiased Linux calls made
     * after the switch: a `write` that had to mean write rather than
     * print, and an `exit_group` that had to mean exit_group rather than
     * nothing at all. If the personality had not taken, call 231 would
     * have fallen off the end of the native table and the child would
     * still be spinning.
     */
    check_eq("it ran on unbiased linux numbers", WEXITSTATUS(st), 88);
}

/* ============================================================
 *  13. clone, which native SYS_CLONE cannot express
 * ============================================================ */

#define CLONE_VM        0x00000100u
#define CLONE_FS        0x00000200u
#define CLONE_FILES     0x00000400u
#define CLONE_SIGHAND   0x00000800u
#define CLONE_THREAD    0x00010000u
#define CLONE_SYSVSEM   0x00040000u

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define PROT_READ     1
#define PROT_WRITE    2

static volatile long clone_mark = 0;

static void __attribute__((noinline)) clone_child_body(void) {
    /*
     * Running on a stack this function was handed, in the caller's
     * address space. Touching only a global is deliberate: anything on
     * the *caller's* stack is at an address this thread's own stack
     * pointer no longer bears any relation to.
     */
    clone_mark = 0x5A;
    __asm__ volatile("syscall" :: "a"(VLS_BIAS + 60L), "D"(0L)
                     : "rcx", "r11", "memory");
    for (;;) __asm__ volatile("pause");
}

static void t_clone(void) {
    const long stack = LX6(L_mmap, 0, 64 * 1024, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("a stack for the new thread", stack > 0);
    if (stack <= 0) return;

    /* Touched so the reservation is real memory before the thread stands
     * on it: a thread does not start by calling anything, so its first
     * instruction pushes. */
    memset((void *)stack, 0, 64 * 1024);
    const unsigned long top = ((unsigned long)stack + 64 * 1024) & ~15UL;

    clone_mark = 0;
    const long tid = LX5(L_clone,
                         CLONE_VM | CLONE_FS | CLONE_FILES |
                         CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
                         top, 0, 0, 0);
    if (tid == 0) clone_child_body();

    check("clone(CLONE_VM) makes a thread", tid > 0);
    if (tid > 0) {
        /* Bounded, because a test that hangs tells nobody anything. */
        for (int i = 0; i < 200 && clone_mark != 0x5A; i++)
            lsleep_ns(5000000L);
        /*
         * The mark is what says the address space was *shared* rather
         * than copied. A clone that fell through to fork would set it in
         * a private copy and this would still be zero — which is exactly
         * how the translation could have been wrong without failing.
         */
        check_eq("the thread ran in this address space", clone_mark, 0x5A);
    }

    /* A flag combination outside the subset is refused rather than
     * half-honoured. CLONE_NEWPID is not something this system has. */
    check_eq("clone flags outside the subset are refused",
             LX5(L_clone, CLONE_VM | 0x20000000u, top, 0, 0, 0), -ENOSYS);

    LX2(L_munmap, stack, 64 * 1024);
}

/* ============================================================
 *  14. the same things again, through the C library
 * ============================================================
 *
 * Everything above speaks to the kernel directly, which is the right way
 * to test a system call and the wrong way to find out whether a *program*
 * can use it. libc/process.c is the layer in between and it is where the
 * mistakes of this kind live: `struct sigaction` is not the structure
 * the system call takes — POSIX orders the members handler, mask, flags,
 * restorer and the kernel's argument is handler, flags, restorer, mask —
 * so a wrapper that cast a pointer instead of copying four words would
 * install a handler with the mask in the flags and pass every check in
 * section 7.
 *
 * These are also the names WebKit's configure looks for. It runs
 * check_function_exists on fork and execve and friends, and under this
 * toolchain that check cannot fail — nothing links, so every function
 * "exists". The answers are seeded by hand in
 * third_party/wpe-config/vextro-project-inject.cmake, and this section
 * is what makes the seeded answer true rather than merely asserted.
 */

static volatile int libc_usr1 = 0;
static volatile pid_t parent_pid = 0;
static void libc_on_usr1(int sig) { (void)sig; libc_usr1++; }

static void t_libc(void) {
    /* signal(), which used to be a documented refusal: it answered
     * SIG_ERR with ENOSYS because there was nowhere for a handler to be. */
    libc_usr1 = 0;
    check("signal() installs a handler", signal(SIGUSR1, libc_on_usr1) != SIG_ERR);
    check_eq("kill through libc", kill((int)getpid(), SIGUSR1), 0);
    check_eq("and the handler ran", libc_usr1, 1);

    /* raise(), which used to call abort() unconditionally because there
     * was no other thing it could mean. */
    libc_usr1 = 0;
    check_eq("raise() goes through kill now", raise(SIGUSR1), 0);
    check_eq("and returns to its caller", libc_usr1, 1);

    /* sigaction's four-word reorder, checked by reading back what was
     * written: a wrapper that cast rather than copied would hand back
     * the flags where the mask went. */
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = libc_on_usr1;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    check_eq("sigaction installs", sigaction(SIGUSR1, &sa, NULL), 0);
    memset(&old, 0xAA, sizeof(old));
    check_eq("and reads back", sigaction(SIGUSR1, NULL, &old), 0);
    check("the handler survived the round trip", old.sa_handler == libc_on_usr1);
    check_eq("the flags did not become the mask", (long)old.sa_flags, SA_RESTART);
    check_eq("and the mask did not become the flags",
             sigismember(&old.sa_mask, SIGUSR2), 1);

    /* sigprocmask through the wrapper. */
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    check_eq("sigprocmask blocks", sigprocmask(SIG_BLOCK, &block, &prev), 0);
    sigset_t now;
    check_eq("and reads back", sigprocmask(SIG_BLOCK, NULL, &now), 0);
    check_eq("SIGUSR2 is blocked", sigismember(&now, SIGUSR2), 1);
    check_eq("unblocking it", sigprocmask(SIG_SETMASK, &prev, NULL), 0);

    /* fork, waitpid and the status macros. */
    const pid_t kid = fork();
    check("fork() through libc", kid >= 0);
    if (kid == 0) _exit(11);
    int st = 0;
    check_eq("waitpid returns the child", (long)waitpid(kid, &st, 0), (long)kid);
    check("WIFEXITED says it exited", WIFEXITED(st));
    check_eq("WEXITSTATUS is what it passed", WEXITSTATUS(st), 11);
    check("WIFSIGNALED is false for a clean exit", !WIFSIGNALED(st));

    /*
     * getppid, which needs a parent to have been recorded.
     *
     * The parent's own identifier is stashed in a global *before* the
     * fork, because that is the only way the child can know what answer
     * to expect: it inherits the variable through the copy-on-write
     * duplicate, and its own getpid() is its own, not its parent's.
     * Comparing getppid() against getpid() in the child — which is what
     * this check did at first — is a test that can only fail.
     */
    parent_pid = getpid();
    const pid_t kid2 = fork();
    if (kid2 == 0) {
        _exit(getppid() == parent_pid ? 21 : 22);
    }
    st = 0;
    waitpid(kid2, &st, 0);
    check_eq("a child sees its parent's pid", WEXITSTATUS(st), 21);

    /* execv, and the argument vector arriving in the new image. */
    const pid_t kid3 = fork();
    if (kid3 == 0) {
        char *av[3];
        av[0] = (char *)"vlstest";
        av[1] = (char *)"exec-child";
        av[2] = 0;
        execv("/vlstest", av);
        _exit(90);            /* only reached if the exec failed */
    }
    st = 0;
    waitpid(kid3, &st, 0);
    /* 78 rather than 77: execv passes an empty environment on purpose —
     * nothing in this library keeps `environ` — so the child finds its
     * argument and no VLS=1, which is exactly what it is written to
     * distinguish. */
    check_eq("execv ran the image with its arguments", WEXITSTATUS(st), 78);

    /* dup2 through libc, onto a number the report does not use. */
    const int devnull = open("/dev/null", O_RDWR);
    check("open(/dev/null) through libc", devnull >= 0);
    if (devnull >= 0) {
        check_eq("dup2 to a chosen number", dup2(devnull, 11), 11);
        char sink[4];
        check_eq("and the duplicate reads as /dev/null",
                 (long)read(11, sink, sizeof(sink)), 0L);
        close(11);
        close(devnull);
    }
    /* And the refusal, by name rather than by a number that is almost
     * right: a socket cannot be held twice on this system. */
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        check_eq("dup of a socket is refused", dup(sock), -1);
        check_eq("with EOPNOTSUPP", (long)errno, (long)EOPNOTSUPP);
        close(sock);
    } else {
        check("dup: a socket could not be opened here", 1);
    }
}

/* ============================================================
 *  15. a channel between two processes
 * ============================================================
 *
 * Pipes are the newest thing in this kernel and the one that had no test
 * until now, so these checks are chosen for the ways a ring buffer with
 * two counted ends actually goes wrong:
 *
 *   The empty ring answering the wrong question. A reader that finds
 *   nothing must be able to tell "wait, somebody may still write" from
 *   "stop, nobody ever will", and a single reference count cannot
 *   express that. The end-of-file check below is the one that fails if
 *   readers and writers are counted together.
 *
 *   The fork *sharing* rather than duplicating. A child with a private
 *   copy of the ring is not at the other end of anything — the program
 *   runs, the write succeeds, the parent reads nothing, and the failure
 *   looks like a lost message rather than a wrong reference count.
 *
 *   Blocking, which is the substance of the thing. The parent's read
 *   below happens before the child has written, so it must park and be
 *   woken rather than answer zero.
 */
static void t_pipe(void) {
    int fds[2] = { -1, -1 };
    check_eq("pipe() answers", pipe(fds), 0);
    check("and gives two different descriptors",
          fds[0] >= 0 && fds[1] >= 0 && fds[0] != fds[1]);

    /* Within one process first, which needs no scheduling at all. */
    char buf[32];
    check_eq("a write goes in", (long)write(fds[1], "hello", 5), 5L);
    check_eq("and comes back out", (long)read(fds[0], buf, sizeof(buf)), 5L);
    check("with the same bytes", memcmp(buf, "hello", 5) == 0);

    /* fstat says what kind of thing it is, which is how a program
     * decides whether its input can be seeked. */
    struct lstat st;
    check_eq("fstat on a pipe", LX2(L_fstat, fds[0], &st), 0);
    check("says it is a FIFO", (st.st_mode & S_IFMT) == 0010000u);

    /*
     * Readiness. The ring is empty and the write end is open, so the
     * read end is *not* ready and poll must say so rather than
     * optimistically claiming everything is always ready.
     */
    struct { int fd; short events; short revents; } pf[2];
    pf[0].fd = fds[0]; pf[0].events = POLLIN;  pf[0].revents = 0;
    pf[1].fd = fds[1]; pf[1].events = POLLOUT; pf[1].revents = 0;
    check_eq("poll with a timeout of zero returns immediately",
             poll((struct pollfd *)pf, 2, 0), 1);
    check("the empty read end is not readable", !(pf[0].revents & POLLIN));
    check("the write end is writable", (pf[1].revents & POLLOUT) != 0);

    write(fds[1], "x", 1);
    pf[0].revents = pf[1].revents = 0;
    check_eq("after a write, two ends are ready",
             poll((struct pollfd *)pf, 2, 0), 2);
    check("and the read end is one of them", (pf[0].revents & POLLIN) != 0);
    read(fds[0], buf, 1);

    /*
     * The end-of-file rule. With the write end closed and the ring
     * empty, a read must answer zero rather than wait — and this is the
     * check that fails if the two counts were folded into one.
     */
    close(fds[1]);
    check_eq("reading a pipe whose writer has gone is end of file",
             (long)read(fds[0], buf, sizeof(buf)), 0L);
    pf[0].revents = 0;
    pf[0].fd = fds[0]; pf[0].events = POLLIN;
    poll((struct pollfd *)pf, 1, 0);
    check("and poll reports the hangup", (pf[0].revents & POLLHUP) != 0);
    close(fds[0]);

    /* ---- and now across a fork, which is what it is for ---- */
    check_eq("a second pipe", pipe(fds), 0);
    const pid_t kid = fork();
    if (kid == 0) {
        close(fds[0]);
        /* Slept first on purpose, so the parent's read below is reached
         * with the ring still empty and has to block. A read that
         * answered zero here would pass a test written the other way
         * round. */
        lsleep_ns(60000000L);
        write(fds[1], "from the child", 14);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    char got[32];
    memset(got, 0, sizeof(got));
    const long n = read(fds[0], got, sizeof(got));
    check_eq("the parent's blocking read returns the child's bytes", n, 14L);
    check("and they are the right bytes",
          memcmp(got, "from the child", 14) == 0);
    /* The child closed its end, so this must now be end of file rather
     * than a second wait. */
    check_eq("then end of file", (long)read(fds[0], got, sizeof(got)), 0L);
    close(fds[0]);
    int st2 = 0;
    waitpid(kid, &st2, 0);
    check_eq("and the child exited cleanly", WEXITSTATUS(st2), 0);

    /*
     * A socketpair, which is the same object with both directions
     * filled in. The check that matters is that it is *bidirectional* —
     * a pipe pretending to be one would pass the first write and fail
     * the reply.
     */
    int sv[2] = { -1, -1 };
    check_eq("socketpair() answers", socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    check_eq("one end writes", (long)write(sv[0], "ping", 4), 4L);
    check_eq("the other reads", (long)read(sv[1], buf, 4), 4L);
    check("the right bytes", memcmp(buf, "ping", 4) == 0);
    check_eq("and the reply goes the other way",
             (long)write(sv[1], "pong", 4), 4L);
    check_eq("and arrives", (long)read(sv[0], buf, 4), 4L);
    check("intact", memcmp(buf, "pong", 4) == 0);
    close(sv[0]);
    close(sv[1]);

    /* A family this system does not have is refused at the call. */
    check_eq("an AF_INET socketpair is refused",
             socketpair(AF_INET, SOCK_STREAM, 0, sv), -1);
}

/* ============================================================
 *  the program
 * ============================================================ */

/*
 * Three parameters, and the first launch of this program passes none of
 * them.
 *
 * sched_build_frame zeroes every register of a new thread's frame, so a
 * program the desktop starts arrives with argc zero and argv null. An
 * exec fills RDI, RSI and RDX — the ordinary C calling convention — so
 * the same entry point serves both, which is what lets section 11 test
 * exec by executing this file.
 */
void _start(int argc, char **argv, char **envp) {
    /*
     * The other side of the exec test. Reached only when this image was
     * started by execve with the argument vector section 11 builds, and
     * it exits with a number nothing else in this program produces.
     */
    if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "exec-child") == 0) {
        const int env_ok = envp && envp[0] &&
                           strcmp(envp[0], "VLS=1") == 0 && envp[1] == 0;
        LX1(L_exit_group, env_ok ? 77 : 78);
        for (;;) LX0(L_yield);
    }

    printf("vlstest: the vextro linux subset\n");

    t_router();
    t_refusals();
    t_files();
    t_devices();
    t_getdents();
    t_dup();
    t_signals();
    t_children();
    t_execve();
    t_personality();
    t_clone();
    t_libc();
    t_pipe();

    printf("vlstest: %d checks, %d failures\n", checks, failures);
}
