#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * signal.h — the names exist; delivery does not.
 *
 * ---- what this system actually has ----
 *
 * The kernel traps every processor exception -- src/trap.h installs
 * twenty-four vectors, four of them on their own stacks -- and a ring-3
 * program that divides by zero or touches an unmapped page is stopped
 * with a line on the wire saying which fault and where. What does not
 * exist is the machinery that turns that into a *call back into the
 * faulting program*: a signal frame pushed onto the user stack, a
 * handler entered with the interrupted state saved, and a sigreturn that
 * restores it. Nor is there any way for one process to send a signal to
 * another, because there is no kill.
 *
 * So the honest shape of this header is: the constants, which are real
 * and worth having, and two functions that do what a system with no
 * handlers can do.
 *
 * ---- and why that is worth a header rather than nothing ----
 *
 * signal() returns SIG_ERR and sets errno to ENOSYS. That is the
 * standard's own way of saying a handler could not be installed, and a
 * caller that checks -- which is most of them, because installing a
 * handler is the sort of thing people check -- learns the truth and
 * takes its other path. A header that pretended to install one and
 * silently never called it would be far worse: the program would run its
 * whole cleanup path only on the assumption that a signal could arrive.
 *
 * raise() does the real default action, which for every signal named
 * here is to end the process. That is not a refusal; it is what a
 * correct implementation does when no handler is installed, and it is
 * exactly what raise(SIGABRT) means.
 *
 * ---- who asked for it ----
 *
 * ICU's decContext.h includes <signal.h> "for traps" and uses nothing
 * from it -- decNumber's trap mechanism is compiled out. WebKit's
 * configure probes for SIGTRAP by name. Both are satisfied by the
 * constants alone.
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

#define NSIG      32

typedef void (*__sighandler_t)(int);
typedef __sighandler_t sig_t;

#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)

/*
 * Always SIG_ERR, with errno set to ENOSYS. See the note above: this is
 * a refusal a caller can detect, and the alternative -- accepting the
 * handler and never calling it -- is a lie a caller cannot.
 */
__sighandler_t signal(int sig, __sighandler_t handler);

/*
 * The default action for every signal above, which is to end the
 * process. Prints which signal it was first, because a program that
 * disappears without a word is the hardest kind to diagnose on a serial
 * console.
 */
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
