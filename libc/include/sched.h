#ifndef _SCHED_H
#define _SCHED_H

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
 * sched.h — the two things a program may ask the scheduler.
 *
 * Small on purpose. Vextro's scheduler assigns priorities itself, and it
 * uses them for one thing that matters more than any program's opinion:
 * the compositor runs above applications, so a program in a tight loop
 * cannot cost the interface its frame. A sched_setscheduler that worked
 * would be a program's licence to take the machine, so the parameter
 * type exists for ported code to pass and nothing reads it.
 *
 * What is real is sched_yield, which has been a system call here since
 * before there was a C library to wrap it.
 */

struct sched_param { int sched_priority; };

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SCHED_RR     2

/* Hand the rest of this time slice back. */
int sched_yield(void);

/* Answers 1: user threads are pinned to processor zero, because the
 * system call entry keeps its kernel stack pointer in one global word.
 * See the note at the top of <pthread.h>. */
int sched_getcpu(void);
long sysconf(int name);

#define _SC_NPROCESSORS_ONLN  84
#define _SC_NPROCESSORS_CONF  83
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         30
#define _SC_PHYS_PAGES        85
#define _SC_AVPHYS_PAGES      86
#define _SC_OPEN_MAX          4
#define _SC_CLK_TCK           2


#ifdef __cplusplus
}
#endif

#endif /* _SCHED_H */
