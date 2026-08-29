/*
 * fdtest — files and sockets in ring 3, through the C library.
 *
 * apps/fdprobe.c checks the same ground through the raw system calls and
 * runs first; this is the layer above, and the two are not redundant. A
 * probe failure means the kernel is wrong. A failure here with the probe
 * passing means the *library* is wrong — an errno unpacked backwards, a
 * sockaddr taken apart in the wrong order, a stream buffer flushed at
 * the wrong moment — and knowing which of the two it is without a
 * debugger is worth the second program.
 *
 * Every check below is chosen because it fails differently when the
 * corresponding piece is broken:
 *
 *   A file read back equals what was written. If the write-back image in
 *   src/vfs.h were flushed at the wrong point, or not at all, the file
 *   would exist and be empty — which is exactly what a program that
 *   "wrote successfully" and lost everything looks like.
 *
 *   A seek in the middle of a buffered stream lands where it says. The
 *   FILE layer reads ahead, so the descriptor's own offset runs ahead of
 *   the caller's; ftell that did not subtract the difference would be
 *   wrong by up to a buffer and right for short files.
 *
 *   errno says which failure it was. A port that cannot tell ENOENT from
 *   EACCES cannot decide whether to create the file or give up.
 *
 *   Bytes actually move over a socket. Everything else about the network
 *   path can be right while nothing is carried, and the loopback round
 *   trip is the only check that distinguishes them.
 *
 * The socket half runs entirely inside the machine, over 127.0.0.1. A
 * test that needed a server somewhere else would fail in a room with no
 * network, which is the room a headless harness runs in.
 */

#include "vextro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

static int checks = 0, failures = 0;

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* Somewhere this account may certainly write. */
#define SCRATCH "/fdtest.tmp"

/* Where the kernel's echo thread is listening. The same number as
 * VFS_ECHO_PORT in src/vfs.h, which is where it is chosen; two copies,
 * because a constant a program could change is not one the kernel may
 * read its own configuration from. */
#define ECHO_PORT 7777

int main(void);

void _start(void) {
    main();
}

int main(void) {
    printf("fdtest: starting\n");

    /* ================================================================
     *  1. reading a file that was put there by the build
     * ================================================================ */
    {
        int fd = open("/about.txt", O_RDONLY);
        ok("open a file the build seeded", fd >= 0);

        if (fd >= 0) {
            char buf[64];
            ssize_t n = read(fd, buf, sizeof(buf));
            ok("read returns bytes", n > 0);

            off_t back = lseek(fd, 0, SEEK_SET);
            ok("lseek to the start answers 0", back == 0);

            char again[64];
            ssize_t m = read(fd, again, sizeof(again));
            ok("reading again after a seek gives the same bytes",
               m == n && memcmp(buf, again, (size_t)n) == 0);

            off_t end = lseek(fd, 0, SEEK_END);
            ok("seeking to the end gives the size", end > 0);

            ok("close", close(fd) == 0);
        }
    }

    /* ================================================================
     *  1b. a fork inherits the open files
     * ================================================================
     *
     * The child gets a *copy* of the table, so the two halves read the
     * same file at independent offsets. That is a deliberate difference
     * from Unix, which shares one open file description and therefore
     * one offset — there is nowhere here to put a shared description,
     * because the table is the description. Checking it means checking
     * that the child can read at all (the table was duplicated) and
     * that the parent's position did not move (they are independent).
     */
    {
        int fd = open("/about.txt", O_RDONLY);
        if (fd >= 0) {
            char head[8];
            read(fd, head, sizeof(head));      /* parent now at 8 */

            pid_t kid = fork();
            if (kid == 0) {
                /* The child reads from where the parent was, which is
                 * what "a copy of the table" means, and then leaves. Its
                 * result reaches the parent through the file it writes
                 * nothing to -- so it simply exits, and the parent's
                 * checks are the ones that matter. */
                char mine[8];
                ssize_t n = read(fd, mine, sizeof(mine));
                _exit(n == (ssize_t)sizeof(mine) ? 0 : 1);
            }

            ok("fork with a file open", kid > 0);

            /* The parent's own position is untouched by whatever the
             * child did with its copy. */
            off_t at = lseek(fd, 0, SEEK_CUR);
            ok("and the parent's offset is its own", at == 8);
            close(fd);
        }
    }

    /* ================================================================
     *  2. the failures, and which one each is
     * ================================================================ */
    {
        errno = 0;
        int fd = open("/no-such-file-at-all", O_RDONLY);
        ok("opening a missing file fails", fd < 0);
        ok("and errno is ENOENT", errno == ENOENT);

        errno = 0;
        char c;
        ssize_t n = read(77, &c, 1);
        ok("reading a descriptor nobody opened fails", n < 0);
        ok("and errno is EBADF", errno == EBADF);

        errno = 0;
        ok("closing one too fails", close(77) < 0 && errno == EBADF);
    }

    /* ================================================================
     *  3. writing, and what it costs to be allowed to
     * ================================================================
     *
     * Writing a file is one of the things this system asks a person
     * about, and the question is asked at open() rather than at close()
     * — by then the program may be gone and there would be nobody to ask
     * about. So there are two correct outcomes here and the test asserts
     * whichever one applies:
     *
     *   With somebody signed in who grants it, the file is created,
     *   written, closed, read back, and removed.
     *
     *   With nobody signed in — which is the state a boot self-test runs
     *   in, and also the state a machine sitting at its login screen is
     *   in — open() is refused with EPERM before anything is written.
     *   That refusal is the security property, and asserting it is worth
     *   as much as asserting the write.
     *
     * The refusal is the *only* thing that happens in a headless run, so
     * the write-back path itself is checked from the kernel side
     * instead: vfs_selftest() in src/vfs.h drives the same code with no
     * ring-3 program to ask about.
     */
    int may_write = 0;
    {
        errno = 0;
        int fd = open(SCRATCH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0) {
            ok("with nobody signed in, creating a file is refused",
               errno == EPERM);
            printf("       (no session: the write half is checked from "
                   "the kernel side)\n");
        } else {
            may_write = 1;
            static const char payload[] =
                "vextro fdtest\nline two\nand a third line\n";
            const size_t len = sizeof(payload) - 1;

            ssize_t w = write(fd, payload, len);
            ok("write reports every byte", w == (ssize_t)len);
            ok("close", close(fd) == 0);

            struct stat st;
            ok("stat finds it", stat(SCRATCH, &st) == 0);
            ok("with the size that was written", (size_t)st.st_size == len);
            ok("and it is a regular file", S_ISREG(st.st_mode));

            int rd = open(SCRATCH, O_RDONLY);
            ok("open it again for reading", rd >= 0);
            if (rd >= 0) {
                char back[128];
                memset(back, 0, sizeof(back));
                ssize_t r = read(rd, back, sizeof(back));
                ok("read back the same length", r == (ssize_t)len);
                ok("read back the same bytes",
                   r == (ssize_t)len && memcmp(back, payload, len) == 0);
                close(rd);
            }
        }
    }

    /* ================================================================
     *  4. streams
     * ================================================================
     *
     * Over the file the build seeded, so that this runs whether or not
     * writing was permitted. What is being checked is the buffering, and
     * a stream over a file somebody else wrote checks it exactly as well.
     */
    {
        FILE *f = fopen("/about.txt", "r");
        ok("fopen", f != 0);

        if (f) {
            char line[256];
            char *got = fgets(line, sizeof(line), f);
            ok("fgets reads a line", got != 0);
            ok("and stops at the newline",
               got && line[strlen(line) - 1] == '\n');

            /*
             * The buffered-position check, and the reason this test
             * exists at all. The stream has read four kilobytes from the
             * descriptor by now, so the descriptor's own offset is far
             * ahead of the caller's; ftell has to answer where the
             * *caller* is. One that forgot to subtract the unread
             * remainder would be right for a file shorter than the
             * buffer and wrong for everything else.
             */
            long at = ftell(f);
            ok("ftell answers the caller's position, not the descriptor's",
               at == (long)strlen(line));

            ok("rewind and re-read", (rewind(f), 1));
            char again[256];
            got = fgets(again, sizeof(again), f);
            ok("gives the first line again",
               got && strcmp(again, line) == 0);

            int c = fgetc(f);
            ok("fgetc reads the next character", c != EOF);
            ok("ungetc puts it back", ungetc(c, f) == c);
            ok("and it comes out again", fgetc(f) == c);

            while (fgets(line, sizeof(line), f)) { }
            ok("feof is set at the end", feof(f) != 0);
            ok("and ferror is not", ferror(f) == 0);

            ok("fclose", fclose(f) == 0);
        }
    }

    /* ================================================================
     *  5. formatted input
     * ================================================================ */
    {
        int a = 0, b = 0;
        char word[16] = { 0 };
        int n = sscanf("42 -7 hello", "%d %d %15s", &a, &b, word);
        ok("sscanf assigns three fields", n == 3);
        ok("and gets them right", a == 42 && b == -7 && strcmp(word, "hello") == 0);

        double d = 0;
        n = sscanf("  3.5e2", "%lf", &d);
        ok("sscanf reads a float in exponent form", n == 1 && d == 350.0);

        unsigned int h = 0;
        n = sscanf("0xff", "%x", &h);
        ok("and hexadecimal", n == 1 && h == 255);

        /* A field that does not match stops the scan where it is, and
         * the count says how far it got. */
        a = b = 0;
        n = sscanf("12 xyz", "%d %d", &a, &b);
        ok("a field that does not match stops the scan", n == 1 && a == 12);
    }

    /* ================================================================
     *  6. directories
     * ================================================================ */
    {
        DIR *d = opendir("/");
        ok("opendir the root", d != 0);

        if (d) {
            int count = 0, found_about = 0, found_scratch = 0;
            struct dirent *e;
            while ((e = readdir(d)) != 0) {
                count++;
                if (strcmp(e->d_name, "about.txt") == 0)  found_about = 1;
                if (strcmp(e->d_name, "fdtest.tmp") == 0) found_scratch = 1;
            }
            ok("the root has entries", count > 0);
            ok("including one the build put there", found_about);
            if (may_write)
                ok("and the file just written", found_scratch);
            printf("       (%d entries)\n", count);

            rewinddir(d);
            int again = 0;
            while (readdir(d)) again++;
            ok("rewinddir gives the same listing again", again == count);

            ok("closedir", closedir(d) == 0);
        }

        errno = 0;
        ok("opendir on a file is ENOTDIR",
           opendir("/about.txt") == 0 && errno == ENOTDIR);
    }

    /* ================================================================
     *  7. removing
     * ================================================================ */
    {
        if (may_write) {
            ok("unlink", unlink(SCRATCH) == 0);
            struct stat st;
            ok("and it is gone", stat(SCRATCH, &st) != 0);
        } else {
            errno = 0;
            ok("with nobody signed in, deleting is refused too",
               unlink("/about.txt") < 0 && errno == EPERM);
            struct stat st;
            ok("and the file is still there", stat("/about.txt", &st) == 0);
        }
    }

    /* ================================================================
     *  8. addresses, without any network
     * ================================================================ */
    {
        struct in_addr a;
        ok("inet_aton parses a dotted quad", inet_aton("10.0.2.15", &a) == 1);
        unsigned char *b = (unsigned char *)&a.s_addr;
        ok("in the right order", b[0] == 10 && b[1] == 0 && b[2] == 2 && b[3] == 15);

        ok("and rejects nonsense", inet_aton("10.0.2.256", &a) == 0);
        ok("and rejects a trailing name", inet_aton("10.0.2.1x", &a) == 0);

        char text[INET_ADDRSTRLEN];
        ok("inet_ntop round-trips",
           inet_ntop(AF_INET, &a.s_addr, text, sizeof(text)) != 0);

        ok("htons swaps", htons(0x1234) == 0x3412);
        ok("and ntohs swaps back", ntohs(htons(443)) == 443);
    }

    /* ================================================================
     *  9. the refusals a socket makes before any packet
     * ================================================================ */
    {
        errno = 0;
        ok("a datagram socket is refused",
           socket(AF_INET, SOCK_DGRAM, 0) < 0 && errno == EOPNOTSUPP);

        errno = 0;
        ok("IPv6 is refused",
           socket(AF_INET6, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);

        errno = 0;
        char c;
        ok("recv on something that is not a socket is ENOTSOCK",
           recv(1, &c, 1, 0) < 0 && errno == ENOTSOCK);
    }

    /* ================================================================
     *  10. bytes over a socket, to ourselves
     * ================================================================
     *
     * The check this whole file exists for.
     *
     * Everything else about the network path can be right while nothing
     * is carried: the descriptor allocated, the address unpacked, the
     * connection made, and not one byte moved. And moving bytes is the
     * most intricate code on the kernel side — the payload is staged
     * through a bounce buffer, the calling thread parks, and the result
     * is copied out with the user range checked *again*, because every
     * other thread on the machine ran while this one waited and one of
     * them may have unmapped the buffer.
     *
     * The far end is a kernel thread echoing on 127.0.0.1:7777; ring 3
     * cannot be a server here, because bind, listen and accept are
     * deliberately not exported. Nothing leaves the machine, so this
     * works in a room with no network.
     */
    {
        static const char msg[] = "the quick brown fox jumps over it";
        const size_t mlen = sizeof(msg) - 1;

        int s = socket(AF_INET, SOCK_STREAM, 0);
        ok("socket", s >= 0);

        if (s >= 0) {
            struct timeval tv = { 5, 0 };
            ok("setsockopt SO_RCVTIMEO",
               setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

            int on = 1;
            ok("setsockopt TCP_NODELAY",
               setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0);

            errno = 0;
            ok("an option this system cannot honour is refused",
               setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0 &&
               errno == ENOPROTOOPT);

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(ECHO_PORT);
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            /* Retried, because the server is a thread that may not have
             * finished binding. Bounded, so a server that never comes up
             * is a failure rather than a hang. */
            int rc = -1;
            long before = (long)vx_millis();
            for (int attempt = 0; attempt < 20; attempt++) {
                rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
                if (rc == 0) break;
                struct timespec nap = { 0, 50 * 1000 * 1000 };
                nanosleep(&nap, 0);
                close(s);
                s = socket(AF_INET, SOCK_STREAM, 0);
                if (s < 0) break;
            }
            long after = (long)vx_millis();
            ok("connect to the echo server on loopback", rc == 0);
            ok("and it did not hang", after - before < 5000);

            if (rc == 0) {
                ssize_t sent = send(s, msg, mlen, 0);
                ok("send reports every byte", sent == (ssize_t)mlen);

                /* Looped, because a short read is what a stream socket
                 * is allowed to give and what every correct caller
                 * handles. */
                char back[128];
                memset(back, 0, sizeof(back));
                size_t got = 0;
                while (got < mlen) {
                    ssize_t n = recv(s, back + got, sizeof(back) - got, 0);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                ok("recv brings back as many as were sent", got == mlen);
                ok("and they are the same bytes",
                   got == mlen && memcmp(back, msg, mlen) == 0);

                /* The same socket through write() and read(), which is
                 * the other door into the same two service routines: a
                 * descriptor is a descriptor, and SYS_WRITE looks it up
                 * in the table exactly as SYS_SEND does. */
                static const char again[] = "and again, through write";
                const size_t alen = sizeof(again) - 1;
                ok("write() to a socket", write(s, again, alen) == (ssize_t)alen);

                memset(back, 0, sizeof(back));
                got = 0;
                while (got < alen) {
                    ssize_t n = read(s, back + got, sizeof(back) - got);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                ok("read() from a socket", got == alen);
                ok("with the same bytes", got == alen &&
                                           memcmp(back, again, alen) == 0);

                /* Half-close: finished sending, still listening. The
                 * server sees the end of its stream and closes, which is
                 * the zero this then reads. */
                ok("shutdown the write side", shutdown(s, SHUT_WR) == 0);
                ok("and the peer's close reads as end of stream",
                   recv(s, back, sizeof(back), 0) == 0);

                errno = 0;
                ok("a message flag this system cannot honour is refused",
                   recv(s, back, 1, MSG_OOB) < 0 && errno == EOPNOTSUPP);
            }
            close(s);
        }
    }

    /* ================================================================
     *  10b. the same, by name rather than by address
     * ================================================================
     *
     * vx_connect_host is the only way to open a TLS session, because a
     * handshake carries the host's name and resolution, connection and
     * handshake are one operation rather than three. Checking it on a
     * plain socket is what proves the door works; the TLS side of it
     * needs a TLS server, and there is none inside this machine.
     */
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ok("a second socket", s >= 0);
        if (s >= 0) {
            int rc = vx_connect_host(s, "localhost", ECHO_PORT);
            ok("connect by name, resolved without a query", rc == 0);
            if (rc == 0) {
                static const char ping[] = "by name";
                ok("send over it",
                   send(s, ping, sizeof(ping) - 1, 0) == (ssize_t)(sizeof(ping) - 1));
                char back[32];
                memset(back, 0, sizeof(back));
                ssize_t n = recv(s, back, sizeof(back), 0);
                ok("and it echoes back",
                   n == (ssize_t)(sizeof(ping) - 1) &&
                   memcmp(back, ping, sizeof(ping) - 1) == 0);
            }
            close(s);
        }
    }

    /* ================================================================
     *  11. resolving, without leaving the machine
     * ================================================================ */
    {
        struct hostent *he = gethostbyname("127.0.0.1");
        ok("gethostbyname on a literal answers without a query", he != 0);
        if (he) {
            unsigned char *ip = (unsigned char *)he->h_addr_list[0];
            ok("with the right address", ip[0] == 127 && ip[3] == 1);
        }

        he = gethostbyname("localhost");
        ok("and localhost resolves locally", he != 0);

        struct addrinfo hints, *res = 0;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo("127.0.0.1", "https", &hints, &res);
        ok("getaddrinfo answers", rc == 0 && res != 0);
        if (rc == 0 && res) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            ok("with the service turned into a port", ntohs(sa->sin_port) == 443);
            freeaddrinfo(res);
        }
    }

    printf("fdtest: %d checks, %d failures\n", checks, failures);
    printf(failures ? "fdtest: FAILED\n" : "fdtest: all passed\n");
    return failures ? 1 : 0;
}
