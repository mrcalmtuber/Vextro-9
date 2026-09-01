/*
 * libc/process.c — starting a program, waiting for it, and signalling
 * it.
 *
 * Everything here is new because until the Linux subset landed there was
 * nothing underneath it. libc/posix.c has had `fork` since ring 3
 * existed, and it was half of a pair with no other half: a child could
 * only run the code its parent was already running, and when it stopped
 * nobody could be told.
 *
 * ---- why some of these use a biased call number ----
 *
 * Five of the calls below go to native numbers — fork, execve, wait4,
 * dup, dup2 — and five go to Linux numbers with VLS_CALL_BIAS added:
 * sigaction, sigprocmask, kill, getppid, tgkill. That is not
 * inconsistency, it is where the kernel actually put them.
 *
 * Signals are Linux's shape down to the byte order of the structures
 * they take. Giving them native numbers *as well* would have been a
 * second door into one room and two things to keep in step, so the
 * kernel gives them one door, in Linux's numbering, and the bias is how
 * a program that has not given up the native numbers reaches it. See
 * include/vls.h.
 *
 * ---- and the one conversion that matters ----
 *
 * `struct sigaction` is not the structure the system call takes. POSIX
 * orders its members handler, mask, flags, restorer; the kernel's
 * argument is handler, flags, restorer, mask. Both orders are fixed by
 * something older than this system and neither can move, so sigaction()
 * below copies four words rather than casting a pointer. A cast would
 * compile, link, run, and install a handler with the mask in the flags.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

/* ============================================================
 *  reaching the subset
 * ============================================================
 *
 * <sys/syscall.h> stops at four arguments because nothing before today
 * needed a fifth. rt_sigaction takes four and wait4 takes four, so the
 * existing helpers would have served — but a five-argument form is
 * written once here rather than added to a header every caller of which
 * would then have to be re-read.
 */
static long vls4(long nr, long a, long b, long c, long d) {
    return __syscall4(VLS_CALL_BIAS + nr, a, b, c, d);
}

/* ============================================================
 *  signal sets
 * ============================================================
 *
 * One word, because the kernel's mask is one word. Bit n-1 is signal n,
 * which is Linux's packing and the reason a set built here can be passed
 * straight to the call.
 */
static int sig_ok(int sig) {
    if (sig < 1 || sig >= NSIG) { errno = EINVAL; return 0; }
    return 1;
}

int sigemptyset(sigset_t *set) {
    if (!set) { errno = EINVAL; return -1; }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) { errno = EINVAL; return -1; }
    /* Every signal this header names, and not every bit in the word: a
     * mask with bits set for signals that do not exist is one the kernel
     * would have to decide what to do with. */
    *set = (NSIG >= 64) ? ~0UL : ((1UL << (NSIG - 1)) - 1UL);
    return 0;
}

int sigaddset(sigset_t *set, int sig) {
    if (!set || !sig_ok(sig)) { if (!set) errno = EINVAL; return -1; }
    *set |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig) {
    if (!set || !sig_ok(sig)) { if (!set) errno = EINVAL; return -1; }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig) {
    if (!set || !sig_ok(sig)) { if (!set) errno = EINVAL; return -1; }
    return (*set & (1UL << (sig - 1))) ? 1 : 0;
}

/* ============================================================
 *  dispositions
 * ============================================================ */

/* What the system call takes. Four words in the kernel's order, which is
 * not this header's order — see the note at the top. */
struct k_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!sig_ok(sig)) return -1;

    struct k_sigaction kact, kold;
    memset(&kact, 0, sizeof(kact));
    memset(&kold, 0, sizeof(kold));

    if (act) {
        kact.handler = (unsigned long)(void *)act->sa_handler;
        kact.flags   = (unsigned long)(unsigned int)act->sa_flags;
        kact.mask    = act->sa_mask;
        /*
         * SA_RESTORER is deliberately not set and sa_restorer is
         * deliberately not passed on.
         *
         * On Linux the kernel has no signal trampoline of its own, so
         * libc supplies one and sets that flag to say so. This kernel
         * has one, on the shared trampoline page, mapped at a
         * per-process address the program does not know — and it is the
         * one that issues the biased sigreturn, so it works whether or
         * not this process has given up the native numbering. Passing a
         * restorer would replace something that works with something
         * this library would then have to write in assembly.
         *
         * A program that set sa_restorer itself is honoured, because the
         * kernel checks SA_RESTORER; it simply is not this library's
         * default.
         */
    }

    const long rc = vls4(VLS_rt_sigaction, sig,
                         act ? (long)(void *)&kact : 0,
                         old ? (long)(void *)&kold : 0,
                         (long)sizeof(sigset_t));
    if (__syscall_ret(rc) < 0) return -1;

    if (old) {
        memset(old, 0, sizeof(*old));
        old->sa_handler  = (void (*)(int))(void *)kold.handler;
        old->sa_flags    = (int)kold.flags;
        old->sa_mask     = kold.mask;
        old->sa_restorer = (void (*)(void))(void *)kold.restorer;
    }
    return 0;
}

__sighandler_t signal(int sig, __sighandler_t handler) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    /*
     * No flags, which gives System V semantics: the handler stays
     * installed across deliveries and the signal is blocked for the
     * duration of its own handler. That is what `signal` has meant to
     * every program written since the eighties, and it is what BSD's
     * `signal` gave; the original System V behaviour of resetting to
     * SIG_DFL on entry is the one nobody wanted and SA_RESETHAND is how
     * a program asks for it explicitly.
     */
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, &old) != 0) return SIG_ERR;
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        errno = EINVAL;
        return -1;
    }
    const long rc = vls4(VLS_rt_sigprocmask, how,
                         set ? (long)(void *)set : 0,
                         old ? (long)(void *)old : 0,
                         (long)sizeof(sigset_t));
    return (int)__syscall_ret(rc);
}

int kill(int pid, int sig) {
    if (sig != 0 && !sig_ok(sig)) return -1;
    const long rc = __syscall2(VLS_CALL_BIAS + VLS_kill, pid, sig);
    return (int)__syscall_ret(rc);
}

int raise(int sig) {
    /*
     * Through kill() now rather than straight to abort(), and the change
     * is visible to any program that installs a handler: raise(SIGUSR1)
     * used to end the process because there was nowhere for a handler to
     * be. It now calls the handler and returns.
     *
     * raise(SIGABRT) with no handler still ends the process, because
     * that is the default action for it — and the kernel does that, not
     * this function, which is the difference between an implementation
     * and a special case.
     */
    return kill((int)getpid(), sig);
}

/* ============================================================
 *  starting a program, and waiting for it
 * ============================================================ */

pid_t getppid(void) {
    return (pid_t)__syscall0(VLS_CALL_BIAS + VLS_getppid);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    if (!path) { errno = EFAULT; return -1; }
    const long rc = __syscall3(SYS_EXECVE, (long)(void *)path,
                               (long)(void *)argv, (long)(void *)envp);
    /*
     * Only reached when it failed, which is the whole shape of exec: a
     * call that succeeded is not a call that returned. Every caller in
     * the world is written as `execve(...); _exit(127);` for that
     * reason.
     */
    return (int)__syscall_ret(rc);
}

int execv(const char *path, char *const argv[]) {
    /* No environment of its own, which is not the same as no
     * environment: the child gets an empty one rather than this
     * process's, because nothing in this library keeps `environ`. A
     * program that wants variables passed on builds the vector and calls
     * execve, which is what execv exists to save it from doing when it
     * does not. */
    static char *const empty[] = { NULL };
    return execve(path, argv, empty);
}

int execvp(const char *file, char *const argv[]) {
    /*
     * Named for the search it does not do, and that is deliberate rather
     * than an omission. There is no PATH on this system — no environment
     * to hold one and no convention about where programs live — so the
     * only honest search is the empty one, and a file given without a
     * separator is looked for at the root, which is where every program
     * on this volume actually is.
     */
    if (file && file[0] != '/' && file[0] != '\\') {
        char rooted[256];
        rooted[0] = '/';
        size_t n = strlen(file);
        if (n > sizeof(rooted) - 2) { errno = ENAMETOOLONG; return -1; }
        memcpy(rooted + 1, file, n + 1);
        return execv(rooted, argv);
    }
    return execv(file, argv);
}

pid_t wait4(pid_t pid, int *status, int options, void *rusage) {
    const long rc = __syscall4(SYS_WAIT4, (long)pid, (long)(void *)status,
                               (long)options, (long)rusage);
    return (pid_t)__syscall_ret(rc);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    /*
     * Resource usage is refused rather than answered with zeros — see
     * the kernel's wait4 — so nothing here ever asks for it. A zeroed
     * rusage is the kind of wrong answer a program builds a report out
     * of.
     */
    return wait4(pid, status, options, NULL);
}

pid_t wait(int *status) {
    return wait4((pid_t)-1, status, 0, NULL);
}

/* ============================================================
 *  a second name for a descriptor
 * ============================================================
 *
 * The limits are the kernel's and are refused there by name: a
 * descriptor on this system *is* the open file description, so a file
 * open for writing cannot be duplicated (two write-back images, and
 * whichever closed last would silently win) and neither can a socket
 * (two holders of one connection get alternating halves of the
 * response). Everything else — a device node, a console stream, a
 * directory, a file open for reading — duplicates exactly, and that is
 * what `dup2(fd, 1)` is nearly always used on.
 */
int dup(int fd) {
    return (int)__syscall_ret(__syscall1(SYS_DUP, fd));
}

int dup2(int oldfd, int newfd) {
    return (int)__syscall_ret(__syscall2(SYS_DUP2, oldfd, newfd));
}

/* ============================================================
 *  a channel, and asking whether it is ready
 * ============================================================
 *
 * unistd.h listed pipe among the things this system did not have, with
 * the reason that "there is no way to start a program from a program
 * here ... so a pipe would have nobody at the other end". fork has
 * existed since ring 3 did and exec and wait are in this file, so the
 * reason expired.
 */
int pipe2(int fds[2], int flags) {
    return (int)__syscall_ret(__syscall2(SYS_PIPE2, (long)(void *)fds,
                                         (long)flags));
}

int pipe(int fds[2]) {
    return pipe2(fds, 0);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return (int)__syscall_ret(__syscall3(SYS_POLL, (long)(void *)fds,
                                         (long)nfds, (long)timeout));
}

/*
 * A connected pair that never leaves the machine.
 *
 * The same object a pipe is with both directions filled in, which is why
 * it lives beside pipe() rather than beside socket(): nothing here
 * reaches lwIP, no address is bound, and nothing is sent anywhere.
 * AF_UNIX and SOCK_STREAM only, refused at the call otherwise.
 */
int socketpair(int domain, int type, int protocol, int sv[2]) {
    return (int)__syscall_ret(__syscall4(SYS_SOCKETPAIR, (long)domain,
                                         (long)type, (long)protocol,
                                         (long)(void *)sv));
}

/*
 * ---- who this process is running as ----
 *
 * Never zero, and that is the substance rather than a detail. A great
 * deal of ported code reads uid zero as permission to skip a check, and
 * none of it is true here: every process holds UAC_TOKEN_RESTRICTED
 * whoever started it, and what decides access is the account identifier
 * and the profile boundary. With nobody signed in the answer is 65534 —
 * nobody — which the kernel chooses; see the note beside vlsh_uid.
 *
 * There is no separate effective identifier because there is no setuid:
 * nothing on this system changes the account a running program belongs
 * to, so the real and effective answers are the same by construction.
 */
uid_t getuid(void)  { return (uid_t)__syscall0(VLS_CALL_BIAS + 102); }
uid_t geteuid(void) { return getuid(); }
gid_t getgid(void)  { return (gid_t)__syscall0(VLS_CALL_BIAS + 104); }
gid_t getegid(void) { return getgid(); }

/*
 * A new session, which this system does not have.
 *
 * Sessions and process groups are how Unix decides which processes a
 * terminal signal reaches. There is one console here, it belongs to the
 * compositor's window rather than to any process, and kill() already
 * refuses the negative pids that name a group — so there is no structure
 * for a session to be a partition of.
 *
 * ENOSYS rather than a pretended success: a program calls setsid() to
 * detach from a terminal it will then stop receiving signals from, and
 * one told it had succeeded would believe something about its own
 * lifetime that is not true.
 */
pid_t setsid(void) {
    errno = ENOSYS;
    return (pid_t)-1;
}

/*
 * ---- the environment, which is empty and is not null ----
 *
 * POSIX says `environ` points at a NULL-terminated array of "NAME=value"
 * strings. This system has no environment: nothing sets a variable,
 * nothing inherits one, and execve's third argument is built by whoever
 * calls it rather than taken from anywhere.
 *
 * So the honest value is a vector with nothing in it, and *not* a null
 * pointer. The difference is the whole reason this has a comment: code
 * that walks the environment writes `for (p = environ; *p; p++)`, which
 * on a null pointer is a fault and on an empty vector is a loop that
 * runs zero times. libgpg-error's spawn-posix.c passes environ straight
 * to execv, where a null would become the child's argv terminator.
 */
static char *vx_environ_empty[1] = { NULL };
char **environ = vx_environ_empty;

/*
 * Becoming another account, which a running program does not get to do.
 *
 * EPERM rather than ENOSYS: the operation is understood and refused.
 * There is one way to be another account here and it is to sign in as
 * one — the account is fixed at spawn from the session (see
 * app_map_image, which sets as->sid from user_current) and nothing
 * reachable from ring 3 moves it.
 */
int setuid(uid_t uid) { (void)uid; errno = EPERM; return -1; }
int setgid(gid_t gid) { (void)gid; errno = EPERM; return -1; }
