#ifndef _PTHREAD_H
#define _PTHREAD_H

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
 * pthread.h — POSIX threads, on Vextro's own scheduler.
 *
 * The kernel here has had threads since long before this file existed.
 * What it did not have was a way for a *program* to make one. A ring-3
 * process could fork — which gives the child a copy-on-write duplicate
 * of the address space, so the two halves share nothing they write — and
 * that is the opposite of what a thread is. A thread that could not see
 * its creator's heap would be useless to every library ever written
 * against this interface.
 *
 * So the kernel gained SYS_CLONE, which is fork's mirror image: a new
 * thread on *the* address space rather than on a copy of it. Everything
 * below is built on that, on SYS_FUTEX, which was already here, and on
 * mmap for stacks.
 *
 * ---- what is real and what is not ----
 *
 * Real: threads, joining, detaching, mutexes of all three kinds,
 * condition variables, read-write locks, barriers, spin locks,
 * thread-specific data with destructors, once-initialisation, and
 * semaphores. All of them block in the kernel rather than spinning, and
 * all of them cost nothing but an atomic instruction when uncontended.
 *
 * Not real, and stated here rather than discovered later:
 *
 *   Scheduling policy and priority. pthread_setschedparam accepts and
 *   ignores. Vextro's scheduler has priorities — the compositor runs
 *   above applications, which is what keeps the interface responsive
 *   under a runaway program — but they are the kernel's to assign, and
 *   letting a program raise itself above the compositor would hand it
 *   the machine.
 *
 *   Cancellation is deferred-only and has few cancellation points; see
 *   pthread_cancel below.
 *
 *   Process-shared attributes. Two processes here share memory only
 *   through fork, and the objects below are not placed in it.
 *
 * ---- one processor ----
 *
 * Threads created here are pinned to processor zero, along with every
 * other ring-3 thread in the system. That is not a property of this
 * library but of the system call boundary: src/syscall.h keeps the
 * kernel stack for the next entry from user mode in a single global
 * word, and two user threads entering it simultaneously on two
 * processors would overwrite each other. The kernel enforces the
 * pinning; this note exists so that the performance is not a surprise.
 * Threads here buy concurrency and structure, not parallelism.
 */

#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>

/* ---- limits ---- */
#define PTHREAD_KEYS_MAX              128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_STACK_MIN             (16 * 1024)
#define PTHREAD_THREADS_MAX           64

/* ---- attributes ---- */
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2
#define PTHREAD_MUTEX_DEFAULT     PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE  0
#define PTHREAD_PROCESS_SHARED   1

#define PTHREAD_CANCEL_ENABLE     0
#define PTHREAD_CANCEL_DISABLE    1
#define PTHREAD_CANCEL_DEFERRED   0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED         ((void *)-1)

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

#define PTHREAD_INHERIT_SCHED    0
#define PTHREAD_EXPLICIT_SCHED   1

/*
 * ---- the types ----
 *
 * A pthread_t is a pointer to a control block this library allocates,
 * not an index or a kernel identifier. That is what lets pthread_equal
 * be a comparison and what lets a joiner read the exit value out of a
 * thread that has already ended.
 *
 * The synchronisation types are laid out here rather than hidden behind
 * a pointer because the static initialisers below have to produce a
 * complete object, and because a mutex that required allocation could
 * fail to be created — which PTHREAD_MUTEX_INITIALIZER has no way to
 * report.
 */
typedef struct pthread_impl *pthread_t;

typedef struct {
    size_t   stacksize;
    int      detachstate;
    int      guardsize;
    void    *stackaddr;
} pthread_attr_t;

/*
 * The mutex.
 *
 * `word` is the futex: 0 free, 1 held, 2 held with a sleeper. It is the
 * only field the uncontended path touches, which is why it is first and
 * why the whole fast path is one compare-and-exchange against it — see
 * <vxmutex.h>, whose three-state design this is.
 *
 * The other fields exist only for the kinds of mutex that are not the
 * default. A recursive mutex needs to know whose it is and how deep; an
 * error-checking one needs the owner to notice a self-deadlock. Both
 * read `owner`, which is a thread id and not a pointer so that it can be
 * compared against SYS_GETTID without a dereference.
 */
typedef struct {
    volatile uint32_t word;
    int               kind;
    volatile uint32_t owner;      /* thread id, for recursive/errorcheck */
    volatile int      depth;
} pthread_mutex_t;

typedef struct { int kind; int pshared; } pthread_mutexattr_t;

/*
 * The condition variable.
 *
 * `seq` counts signals ever issued. A waiter reads it before releasing
 * the mutex and sleeps until it changes, which is what closes the race
 * that makes a naive condition variable lose wakeups: between unlocking
 * the mutex and going to sleep, the signal can arrive, and a design that
 * slept on "has a signal happened" rather than "has the count moved"
 * would sleep through it.
 */
typedef struct {
    volatile uint32_t seq;
    volatile uint32_t waiters;
} pthread_cond_t;

typedef struct { int pshared; int clock; } pthread_condattr_t;

/*
 * The read-write lock.
 *
 * `state` is the whole lock: 0 free, 0xFFFFFFFF held for writing, and
 * anything else a count of readers. One word rather than a mutex and two
 * counters, so that taking a read lock when uncontended is a single
 * compare-and-exchange like everything else here.
 */
typedef struct {
    volatile uint32_t state;
    volatile uint32_t waiters;
} pthread_rwlock_t;

typedef struct { int pshared; } pthread_rwlockattr_t;

typedef volatile uint32_t pthread_spinlock_t;

typedef struct {
    volatile uint32_t count;      /* how many have arrived           */
    volatile uint32_t seq;        /* which generation of the barrier */
    uint32_t          total;
} pthread_barrier_t;

typedef struct { int pshared; } pthread_barrierattr_t;

typedef volatile int pthread_once_t;
typedef unsigned int pthread_key_t;

/* ---- static initialisers ----
 *
 * Every one of these is all-zero except where a non-default kind is
 * meant, which is deliberate: an object in .bss is correctly initialised
 * before anything runs, so a program that forgets pthread_mutex_init on
 * a static mutex still works. That is not a licence to forget it — an
 * automatic or heap-allocated mutex holds whatever was there — but it
 * removes one whole class of failure that only appears under load.
 */
#define PTHREAD_MUTEX_INITIALIZER            { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER  { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0 }
#define PTHREAD_COND_INITIALIZER             { 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER           { 0, 0 }
#define PTHREAD_ONCE_INIT                    0

/* ===== threads ===== */

int       pthread_create(pthread_t *out, const pthread_attr_t *attr,
                         void *(*start)(void *), void *arg);
int       pthread_join(pthread_t t, void **retval);
int       pthread_detach(pthread_t t);
void      pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int       pthread_equal(pthread_t a, pthread_t b);

/* The kernel's own identifier for this thread — what shows in the
 * process list, and what the recursive mutex compares against. Not
 * POSIX; present because it is the only way to correlate a thread here
 * with a line on the serial console. */
int       pthread_gettid(void);

/*
 * Cancellation, deferred only.
 *
 * pthread_cancel sets a flag. The flag is examined at pthread_testcancel
 * and on return from pthread_cond_wait, pthread_cond_timedwait,
 * sem_wait, and nanosleep — and nowhere else. That is a much smaller set
 * of cancellation points than POSIX lists, and the difference matters:
 * a thread blocked in a read that will never complete cannot be
 * cancelled here, because there is no read to interrupt.
 *
 * Asynchronous cancellation is accepted and behaves as deferred.
 * Honouring it would mean stopping a thread at an arbitrary instruction,
 * which is only safe if every lock, every allocation and every partially
 * built structure in the program is unwindable — and nothing here is.
 */
int       pthread_cancel(pthread_t t);
int       pthread_setcancelstate(int state, int *old);
int       pthread_setcanceltype(int type, int *old);
void      pthread_testcancel(void);

int       pthread_once(pthread_once_t *once, void (*fn)(void));

/* Accepted and ignored; see the note at the top of this file. */
int       pthread_setschedparam(pthread_t t, int policy,
                                const struct sched_param *p);
int       pthread_getschedparam(pthread_t t, int *policy,
                                struct sched_param *p);
int       pthread_setname_np(pthread_t t, const char *name);
int       pthread_getname_np(pthread_t t, char *name, size_t len);
int       pthread_atfork(void (*prepare)(void), void (*parent)(void),
                         void (*child)(void));

/* ===== attributes ===== */

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t size);
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *size);
int pthread_attr_setscope(pthread_attr_t *a, int scope);
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit);
int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy);
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p);

/* ===== mutexes ===== */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int kind);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *kind);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared);

/* ===== condition variables ===== */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abs);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, int clock);
int pthread_condattr_setpshared(pthread_condattr_t *a, int pshared);

/* ===== read-write locks ===== */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

/* ===== spin locks =====
 *
 * The one primitive here that genuinely spins, and on a system where all
 * user threads share one processor that is almost always the wrong
 * choice: the thread holding the lock cannot make progress while the
 * spinner is burning the slice it needs. So this yields after a short
 * burst rather than spinning indefinitely — which is not what a spin
 * lock is for, and is the only way for one to terminate here.
 */
int pthread_spin_init(pthread_spinlock_t *s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);

/* ===== barriers ===== */

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);

/* ===== thread-specific data ===== */

int   pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int   pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int   pthread_setspecific(pthread_key_t key, const void *value);

/*
 * Set up this thread's own storage.
 *
 * Called automatically by everything in this library that needs it, so a
 * program that only uses the functions above never has to think about
 * it. It is declared because a program that uses `__thread` or C++
 * `thread_local` variables of its own has to call it before touching
 * one: those compile to a load through the FS segment, which the
 * compiler emits directly and no library call can intercept, so the
 * segment base has to be set before the first such access rather than
 * on demand at it.
 *
 * Programs linked against libc/crt0.c get this for free — that is what
 * crt0 is for. Programs that define their own _start, which is every
 * application written for this system before threads existed, must call
 * it themselves or not use thread-local variables.
 */
void __libc_init_tls(void);


#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
