#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * signal.h — and now delivery exists.
 *
 * ---- what this header used to say ----
 *
 * That the names were real and the machinery was not: the kernel trapped
 * every processor exception and stopped a faulting program with a line
 * on the wire, and what did not exist was "a signal frame pushed onto
 * the user stack, a handler entered with the interrupted state saved,
 * and a sigreturn that restores it. Nor is there any way for one process
 * to send a signal to another, because there is no kill."
 *
 * All four of those exist now. src/sched/vls_core.c lays the frame,
 * src/trap.h delivers a fault into it, the trampoline on the shared page
 * returns from it, and kill is a system call. So `signal()` no longer
 * answers SIG_ERR — it installs a handler and the handler runs.
 *
 * ---- what is still worth knowing before you rely on it ----
 *
 * Delivery happens at two points and only two: on the way out of a
 * system call, and on the way out of a ring-3 fault. It is deliberately
 * not in the timer interrupt, because that path is hand-tuned and
 * compiled general-regs-only and a check inside it would be new
 * instructions in the most delicate function in the kernel.
 *
 * Two consequences follow and neither is hidden:
 *
 *   **No call returns EINTR.** A system call that has begun runs to
 *   completion and the handler runs after it. A program that loops on
 *   EINTR will simply never take that branch, which is safe; a program
 *   that *depends* on a signal cutting a long read short will wait.
 *
 *   A thread asleep in the kernel sees a caught signal when it next
 *   wakes rather than at the instant it is sent. A signal that *kills*
 *   does not wait — the process ends immediately — so what is delayed is
 *   the case where the program asked to be told and is not in a hurry.
 *
 * And the mask is per-process rather than per-thread, which POSIX makes
 * per-thread. Two threads of one program cannot have different masks
 * here. That is written down in include/vls.h beside the structure that
 * holds it rather than discovered.
 */

#include <stddef.h>

/*
 * The type an asynchronous handler may safely touch. `int` here, and
 * volatile-qualified at the point of use rather than in the typedef,
 * which is how the standard defines it.
 */
typedef int sig_atomic_t;

/*
 * Linux's numbers, for the same reason errno.h uses Linux's numbers: a
 * signal number that crossed a boundary as an integer -- in a test
 * fixture, in a table compiled elsewhere -- should mean here what it
 * meant there.
 */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     SIGABRT
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGSYS    31

/* One past the highest signal a program may name. The kernel carries
 * sixty-four; thirty-two is what this header names and what the numbers
 * above stop at, and a program that asks for 40 gets EINVAL rather than
 * a silent nothing. */
#define NSIG      32

typedef void (*__sighandler_t)(int);
typedef __sighandler_t sig_t;

#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)

/*
 * A set of signals, as one word.
 *
 * glibc makes this a hundred and twenty-eight bytes so that the type
 * would not have to change if the kernel ever grew past a thousand
 * signals. This kernel carries sixty-four and says so, and the system
 * call takes an eight-byte mask — so a word is the whole of it, and a
 * structure whose first word was the only one ever read would be a
 * hundred and twenty bytes of pretence.
 */
typedef unsigned long sigset_t;

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

/* sa_flags. SA_RESTART is accepted and means nothing here, and that is
 * the truthful outcome rather than a lie: no call returns EINTR, so
 * there is never anything to restart. */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

/* sigprocmask's `how`. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* si_code, for the two that a handler here can actually be told apart
 * by. A crash reporter prints "address not mapped" rather than
 * "permissions" by reading this, and the page-fault error code already
 * knew which it was. */
#define SI_USER      0
#define SI_KERNEL    0x80
#define SEGV_MAPERR  1
#define SEGV_ACCERR  2
#define CLD_EXITED   1
#define CLD_KILLED   2

/*
 * What a three-argument handler is given.
 *
 * A hundred and twenty-eight bytes with the fields where Linux puts
 * them, because this is a structure a *program* reads by name and the
 * names have to land on the offsets it was compiled against. Only three
 * of them are ever filled — si_signo, si_code and si_addr — and the rest
 * is zero, which is a truthful siginfo rather than a partial one: every
 * field a program can read has the value it should have.
 */
typedef struct {
    int           si_signo;
    int           si_errno;
    int           si_code;
    int           __pad0;
    /* For a fault, the address. For a SIGCHLD this is where Linux packs
     * si_pid and si_uid; nothing here fills that, and it reads zero. */
    void         *si_addr;
    int           si_status;
    int           __pad1;
    unsigned char __pad2[96];
} siginfo_t;

/*
 * The disposition of one signal.
 *
 * This is *not* the structure the system call takes, and the difference
 * is the whole reason libc/process.c has a conversion in it rather than
 * a cast. POSIX orders the members handler, mask, flags, restorer; the
 * kernel's argument is handler, flags, restorer, mask. Both orders are
 * correct for their own side and neither is free to change, so the
 * wrapper moves four words.
 */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};

/*
 * Install a handler, and run it when the signal arrives.
 *
 * SIGKILL and SIGSTOP are refused with EINVAL, which is the standard's
 * answer and this kernel's: they are the two a program is not permitted
 * to be between.
 */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);

/*
 * The older, simpler form, and it is a real one now.
 *
 * Implemented over sigaction with no flags, which gives the System V
 * semantics: the handler stays installed across deliveries and the
 * signal is blocked for the duration of its own handler. That is what
 * every program written since the eighties expects `signal` to mean.
 */
__sighandler_t signal(int sig, __sighandler_t handler);

int sigprocmask(int how, const sigset_t *set, sigset_t *old);

/* Send one. `sig` of zero sends nothing and answers whether the process
 * exists, which is what it has meant since the seventh edition. */
int kill(int pid, int sig);

/*
 * Send one to this process.
 *
 * Over kill() now rather than straight to abort(). The difference is
 * visible to any program that installs a handler: raise(SIGUSR1) used to
 * end the process because there was nowhere for a handler to be, and now
 * it calls the handler and returns. raise(SIGABRT) with no handler still
 * ends the process, because that is the default action and not a
 * refusal.
 */
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
