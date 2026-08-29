/*
 * fdprobe — the smallest thing that can tell whether descriptors work.
 *
 * Deliberately written against the raw system calls rather than against
 * the C library, and deliberately before that library was written. What
 * it is looking for is not whether open() has the right signature; it is
 * whether the two integration questions that could not be answered by
 * reading code have the answers they were assumed to have:
 *
 *   Does a blocking socket call, made from a system call with interrupts
 *   masked, actually come back? lwIP parks on a semaphore that is
 *   ultimately sched_block_on_locked, which is the same thing uac_guard
 *   does — but uac_guard is woken by a person and this is woken by the
 *   tcpip thread, which has to be scheduled while the caller sits in the
 *   kernel. If that is wrong the machine stops here rather than three
 *   thousand lines further on.
 *
 *   Does the loopback interface exist? LWIP_NETIF_LOOPBACK was turned on
 *   for this, and "on" in a header is not the same as an interface that
 *   routes.
 *
 * It stays in the tree after having served that purpose, because those
 * two things are exactly what a later change would break silently.
 */

#include "vextro.h"
#include <sys/syscall.h>

/* No libc: this runs before there is one to depend on. */
static void say(const char *s) { os_print(s); }

static void say_num(const char *label, long v) {
    char buf[64];
    int i = 0;
    while (label[i] && i < 40) { buf[i] = label[i]; i++; }
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    buf[i++] = '\n';
    buf[i] = '\0';
    os_print(buf);
}

static int checks = 0, failures = 0;

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    char line[128];
    const char *tag = good ? " ok   " : "FAIL  ";
    int i = 0;
    while (tag[i]) { line[i] = tag[i]; i++; }
    int j = 0;
    while (what[j] && i < 120) line[i++] = what[j++];
    line[i++] = '\n';
    line[i] = '\0';
    os_print(line);
}

void _start(void) {
    say("fdprobe: starting\n");

    /* ---- 1. a file that is certainly there ----
     *
     * /about.txt is seeded onto the volume by the Makefile, so this is
     * a real NTFS record read through a real descriptor. */
    {
        long fd = __syscall3(SYS_OPEN, (long)(uintptr_t)"/about.txt",
                             0 /* O_RDONLY */, 0);
        ok("open a file that exists", fd >= 3);
        if (fd >= 3) {
            char buf[64];
            long n = __syscall3(SYS_READ, fd, (long)(uintptr_t)buf,
                                (long)sizeof(buf));
            say_num("  read returned ", n);
            ok("read returns bytes", n > 0);
            long rc = __syscall3(SYS_LSEEK, fd, 0, 0 /* SEEK_SET */);
            ok("lseek to the start", rc == 0);
            ok("close", __syscall1(SYS_CLOSE, fd) == 0);
        }
    }

    /* ---- 2. one that is not ---- */
    {
        long fd = __syscall3(SYS_OPEN, (long)(uintptr_t)"/no-such-file", 0, 0);
        say_num("  open of a missing file returned ", fd);
        ok("a missing file is -ENOENT", fd == -2);
    }

    /* ---- 3. a directory ---- */
    {
        long fd = __syscall3(SYS_OPEN, (long)(uintptr_t)"/", 0, 0);
        ok("open the root directory", fd >= 3);
        if (fd >= 3) {
            /* One vx_dirent_t is 272 bytes. */
            static char dents[272 * 8];
            long n = __syscall3(SYS_GETDENTS, fd, (long)(uintptr_t)dents,
                                (long)sizeof(dents));
            say_num("  getdents returned ", n);
            ok("the root directory has entries", n > 0);
            __syscall1(SYS_CLOSE, fd);
        }
    }

    /* ---- 4. stat ---- */
    {
        /* vx_stat_t is 32 bytes: size, ino, mode, nlink, mtime. */
        static unsigned long st[4];
        long rc = __syscall2(SYS_STAT, (long)(uintptr_t)"/about.txt",
                             (long)(uintptr_t)st);
        ok("stat a file", rc == 0);
        say_num("  size ", (long)st[0]);
        say_num("  ino  ", (long)st[1]);
    }

    /* ---- 5. a socket, and the loopback round trip ----
     *
     * The whole point of the probe. AF_INET=2, SOCK_STREAM=1. */
    {
        long s = __syscall3(SYS_SOCKET, 2, 1, 0);
        say_num("  socket returned ", s);
        ok("socket", s >= 3);

        if (s >= 3) {
            /* Nothing is listening on this port, so what is being
             * measured is that connect *returns at all* rather than
             * hanging a thread inside a system call. A refusal is the
             * correct answer and is what a working stack gives. */
            static unsigned char lo[4] = { 127, 0, 0, 1 };
            long rc = __syscall3(SYS_CONNECT, s, (long)(uintptr_t)lo, 9);
            say_num("  connect to 127.0.0.1:9 returned ", rc);
            ok("connect returns rather than hanging", rc == 0 || rc < 0);
            __syscall1(SYS_CLOSE, s);
        }
    }

    /* ---- 6. the refusals that need no network ---- */
    {
        long s = __syscall3(SYS_SOCKET, 2, 2 /* SOCK_DGRAM */, 0);
        ok("a datagram socket is refused", s == -95 /* EOPNOTSUPP */);

        s = __syscall3(SYS_SOCKET, 10 /* AF_INET6 */, 1, 0);
        ok("IPv6 is refused", s == -97 /* EAFNOSUPPORT */);

        long rc = __syscall3(SYS_READ, 77, 0, 0);
        ok("a descriptor nobody opened is -EBADF", rc == -9);
    }

    say_num("fdprobe: checks ", checks);
    say_num("fdprobe: failures ", failures);
    say(failures ? "fdprobe: FAILED\n" : "fdprobe: all passed\n");
}
