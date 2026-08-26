#ifndef _VXMUTEX_H
#define _VXMUTEX_H

/*
 * A mutex that is not usually a system call.
 *
 * This is the user-space half of SYS_FUTEX, and the division of labour
 * is the whole point of the design. Taking an uncontended lock is one
 * atomic compare-and-exchange on a word in this program's own memory:
 * no interrupt, no privilege change, no kernel stack, nothing to
 * schedule. The kernel is only involved when two threads actually want
 * the same lock at the same time, and what it is asked for then is not
 * the lock -- it is somewhere to sleep until the lock is free.
 *
 * The alternative available before this was to spin on sys_yield, which
 * costs a whole scheduling slice per attempt and, for a thread at
 * PRIO_NORMAL competing with a compositor at PRIO_UI, can spin for a
 * very long time waiting for a thread that is perfectly ready to run.
 *
 * ---- the three states, and why two would not do ----
 *
 *     0  free
 *     1  held, and nobody is waiting
 *     2  held, and at least one thread is asleep on it
 *
 * A two-state lock has to issue a wake on every unlock, because it
 * cannot tell whether anybody is listening -- which puts a system call
 * on the *uncontended* path and gives back everything this exists to
 * save. The third state is what lets unlock be a single store and a
 * branch that is almost never taken.
 *
 * The lock is a plain uint32_t with no other fields, so it may be
 * placed in memory shared across a fork and locked from either side:
 * the kernel keys its wait channel on the word's physical address
 * precisely so that this works.
 */

#include <stdint.h>
#include <sys/syscall.h>

#define VX_MUTEX_FREE     0u
#define VX_MUTEX_HELD     1u
#define VX_MUTEX_CONTESTED 2u

typedef uint32_t vx_mutex_t;

#define VX_MUTEX_INIT 0u

static inline long vx_futex(uint32_t *uaddr, int op, uint32_t val) {
    return __syscall3(SYS_FUTEX, (long)(uintptr_t)uaddr, (long)op,
                      (long)(uint64_t)val);
}

static inline void vx_mutex_init(vx_mutex_t *m) {
    __atomic_store_n(m, VX_MUTEX_FREE, __ATOMIC_RELEASE);
}

/* Returns 1 if the lock was taken, 0 if it was already held. Never
 * enters the kernel. */
static inline int vx_mutex_trylock(vx_mutex_t *m) {
    uint32_t expected = VX_MUTEX_FREE;
    return __atomic_compare_exchange_n(m, &expected, VX_MUTEX_HELD, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void vx_mutex_lock(vx_mutex_t *m) {
    uint32_t expected = VX_MUTEX_FREE;

    /* The fast path, and the only path most of the time. */
    if (__atomic_compare_exchange_n(m, &expected, VX_MUTEX_HELD, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;

    /*
     * Contended. Announce that somebody is waiting before sleeping, and
     * keep announcing it on every pass: the exchange below both takes
     * the lock if it happens to be free and marks it contested if it is
     * not, in one operation, which is what makes the announcement
     * impossible to lose.
     *
     * Sleeping only when the word is *observed* to be contested closes
     * the race with an unlock that happens between the exchange and the
     * system call -- the kernel re-reads the word with interrupts
     * masked and returns immediately if it has changed, so a wake that
     * arrives in that window cannot be missed.
     */
    while (__atomic_exchange_n(m, VX_MUTEX_CONTESTED, __ATOMIC_ACQUIRE)
           != VX_MUTEX_FREE)
        vx_futex(m, FUTEX_WAIT, VX_MUTEX_CONTESTED);
}

static inline void vx_mutex_unlock(vx_mutex_t *m) {
    /*
     * A free lock nobody was waiting on costs one store and one
     * comparison. Only a lock somebody slept on pays for a wake, which
     * is the trade the third state exists to make.
     */
    if (__atomic_exchange_n(m, VX_MUTEX_FREE, __ATOMIC_RELEASE)
        == VX_MUTEX_CONTESTED)
        vx_futex(m, FUTEX_WAKE, 1);
}

#endif /* _VXMUTEX_H */
