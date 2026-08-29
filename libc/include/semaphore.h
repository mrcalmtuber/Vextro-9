#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

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
 * semaphore.h — a counted permit, on the same futex everything else here
 * uses.
 *
 * The count is the whole object. Taking a permit when one is available
 * is a compare-and-exchange and nothing else; only a thread that finds
 * the count at zero enters the kernel, and what it asks for there is
 * somewhere to sleep until the count moves.
 *
 * Named semaphores are not here. They would need a namespace shared
 * between processes, and this system has no such namespace above the
 * file system — which ring 3 cannot open.
 */

#include <stdint.h>
#include <time.h>

typedef struct {
    volatile uint32_t count;
    volatile uint32_t waiters;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

int sem_init(sem_t *s, int pshared, unsigned int value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abs);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *out);


#ifdef __cplusplus
}
#endif

#endif /* _SEMAPHORE_H */
