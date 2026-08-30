#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sys/wait.h — collecting a child.
 *
 * New, and it could not have existed before: waiting for a child
 * requires that a process *have* a parent, which requires that a process
 * have an identity, and until the Linux subset landed the only number
 * anywhere in this kernel was a thread's. Two threads of one program had
 * two of them and neither was the program's.
 *
 * `addr_space_t` now carries the distinction Linux calls tgid and tid.
 * getpid() answers the process; gettid() answers the thread; they are
 * the same number for a single-threaded program, which is exactly why
 * the difference went unnoticed here for so long.
 */

#include <sys/types.h>

/* Return immediately if no child has ended. The only option this system
 * implements, and the only one most programs use — WUNTRACED and
 * WCONTINUED are about *stopped* children, and nothing here can stop a
 * process without ending it. */
#define WNOHANG    1
#define WUNTRACED  2

/*
 * Taking a status apart.
 *
 * Linux's encoding, exactly: the low seven bits are the signal that
 * killed the process and the next eight are the value it exited with.
 * A program that was compiled against these macros somewhere else and
 * handed a status word from here gets the same answer, which is the
 * whole reason not to invent an encoding.
 */
#define WEXITSTATUS(s)  (((s) & 0xff00) >> 8)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WCOREDUMP(s)    ((s) & 0x80)

/* Nothing here stops a process without ending it, so these are always
 * false. Defined rather than omitted because a port tests them in an
 * `if` and would not compile without them; answering false is the
 * truthful result, not a stub. */
#define WIFSTOPPED(s)   (0)
#define WSTOPSIG(s)     (0)
#define WIFCONTINUED(s) (0)

/*
 * Wait for a child to end.
 *
 * `pid` above zero names one child; -1 and 0 both mean any child, since
 * this system has no process groups and the group forms degrade to the
 * same answer rather than to a refusal.
 *
 * Returns the child's identifier, or 0 if WNOHANG was given and nothing
 * had ended, or -1 with ECHILD if there is no such child to wait for.
 */
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

/*
 * The four-argument form. `rusage` must be null: nothing on this system
 * accounts processor time per *process* — the scheduler counts slices
 * per thread and nothing sums them — so a request for it is refused with
 * EINVAL rather than answered with zeros. A zeroed rusage is the kind of
 * wrong answer a program builds a report out of.
 */
pid_t wait4(pid_t pid, int *status, int options, void *rusage);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
