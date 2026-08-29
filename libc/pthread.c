/*
 * libc/pthread.c — POSIX threads over Vextro's scheduler.
 *
 * Three kernel calls hold this up and nothing else does:
 *
 *   SYS_CLONE      a thread on this address space, not a copy of it
 *   SYS_FUTEX      somewhere to sleep, and a way to be woken
 *   SYS_MMAP       stacks, one per thread, unmapped when it ends
 *
 * Everything below is user-space arithmetic on top of those. That is the
 * point of the design rather than an economy: a lock that is not
 * contended never enters the kernel at all, so the cost of correctness
 * on the common path is one atomic instruction. The kernel is asked only
 * when a thread has genuinely nothing to do but wait.
 *
 * ---- the one thing to understand before reading any of it ----
 *
 * SYS_FUTEX's wait is *bounded*. It parks for at most FUTEX_PARK_MS and
 * then returns whether or not anybody woke it — see the long note in
 * src/desktop.h for why, which comes down to a page that may have been
 * evicted while a thread slept on its physical address. So every wait in
 * this file is written as a loop around a predicate, and a return from
 * the kernel is never taken as evidence that anything happened. That is
 * not defensive: a spurious wakeup is part of the futex contract on
 * every system that has one, and code written any other way is broken
 * everywhere, not merely here.
 */

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* ===== 1. THE FUTEX ===== */

static inline long futex(volatile uint32_t *w, int op, uint32_t val) {
    return __syscall3(SYS_FUTEX, (long)(uintptr_t)w, (long)op,
                      (long)(uint64_t)val);
}
static inline void futex_wait(volatile uint32_t *w, uint32_t expect) {
    futex(w, FUTEX_WAIT, expect);
}
static inline void futex_wake(volatile uint32_t *w, int all) {
    futex(w, FUTEX_WAKE, all ? 2 : 1);
}

static inline uint32_t load_acq(volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void store_rel(volatile uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline int cas(volatile uint32_t *p, uint32_t want, uint32_t to) {
    return __atomic_compare_exchange_n(p, &want, to, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int sched_yield(void) { __syscall0(SYS_YIELD); return 0; }
int sched_getcpu(void) { return 0; }

/* ===== 2. THE THREAD CONTROL BLOCK =====
 *
 * Two structures, and the split between them is the x86-64 ABI's rather
 * than a choice. `vx_tcb` sits at the thread pointer — the address in
 * IA32_FS_BASE — because the ABI requires the word at %fs:0 to be the
 * thread pointer itself, and because thread-local variables are
 * addressed at *negative* offsets from it. So the block a thread's
 * `__thread` variables live in has to be immediately below this, which
 * means the control block cannot simply contain them.
 *
 * `pthread_impl` is this library's own bookkeeping and is reached
 * through a pointer in the TCB. It could have been laid out inside the
 * TCB, above the ABI's two mandatory words; keeping it separate means
 * the TCB is exactly what the ABI describes and nothing else, which is
 * one less thing to get wrong when a compiler generates a TLS access.
 */
typedef struct vx_tcb {
    struct vx_tcb       *self;      /* %fs:0 — required by the ABI      */
    void                *dtv;       /* %fs:8 — unused; no dynamic TLS   */
    struct pthread_impl *thr;
    int                  err;       /* errno, per thread                */
} vx_tcb_t;

struct pthread_impl {
    uint32_t   tid;                 /* the kernel's identifier          */
    volatile uint32_t done;         /* 1 while running, 0 when finished */
    void    *(*start)(void *);
    void      *arg;
    void      *retval;

    void      *stack;               /* the mmap, for release            */
    size_t     stacksize;
    void      *tlsblock;            /* the TLS image and the TCB        */
    size_t     tlssize;

    int        detached;
    int        is_main;

    volatile int cancel_requested;
    int        cancel_state;
    int        cancel_type;

    void      *specific[PTHREAD_KEYS_MAX];

    struct pthread_impl *retired_next;
    char       name[24];
};

/*
 * The main thread's control block, in .bss.
 *
 * Statically allocated rather than obtained from malloc, and that is
 * load-bearing: this has to exist before anything else does, including
 * the allocator. errno is a field of a thread control block, and malloc
 * sets errno.
 */
static struct pthread_impl main_thread;
static vx_tcb_t            main_tcb;

/*
 * Has a thread pointer been installed yet?
 *
 * Until it has, %fs:0 reads through a base of zero and faults. Every
 * accessor below checks this word first and falls back to the static
 * main thread — which is correct rather than a stopgap, because a
 * program is single-threaded right up until it calls pthread_create, and
 * pthread_create is one of the things that sets this.
 */
static int tls_ready = 0;

static inline vx_tcb_t *tcb_self(void) {
    if (!tls_ready) return &main_tcb;
    vx_tcb_t *p;
    __asm__("movq %%fs:0, %0" : "=r"(p));
    return p;
}

/*
 * errno, per thread.
 *
 * The function rather than the variable, which is what <errno.h>'s macro
 * expands to and what every C++ runtime looks for by this exact name. It
 * has to be a function because the storage moves with the thread, and it
 * has to work before thread-local storage is set up, which is why it
 * goes through tcb_self rather than being a `__thread int`.
 */
int *__errno_location(void) { return &tcb_self()->err; }

/* ---- the TLS image, as the linker laid it out ----
 *
 * These come from the linker script (apps/app.ld and vxfmt/vx.ld). A
 * program with no thread-local variables still has them, both equal, so
 * the size below is zero and no image is copied.
 */
extern char __tdata_start[];
extern char __tdata_end[];
extern char __tbss_end[];

static size_t tls_image_size(void) {
    return (size_t)(__tbss_end - __tdata_start);
}
static size_t tls_data_size(void) {
    return (size_t)(__tdata_end - __tdata_start);
}

/*
 * Build a thread pointer for one thread.
 *
 * The layout is the x86-64 "variant II" one, which is upside down
 * compared to the obvious design and has to be:
 *
 *     [ .tdata copy | .tbss zeros ] [ TCB ]
 *                                   ^ tp, and IA32_FS_BASE
 *
 * A `__thread` variable compiles to a load at a *negative* offset from
 * FS, with the offset chosen by the linker from the layout of the PT_TLS
 * segment. So the variables must sit immediately below the thread
 * pointer, in the same order and with the same alignment the linker
 * used, and the control block must sit at or above it. Getting the
 * direction wrong produces a program that reads plausible garbage rather
 * than one that crashes.
 */
static vx_tcb_t *tls_alloc(struct pthread_impl *t) {
    size_t img = tls_image_size();
    /* Rounded so the TCB is sixteen-aligned however large the image is;
     * SSE loads against a field of it would fault otherwise. */
    size_t below = (img + 15u) & ~(size_t)15u;
    size_t total = below + sizeof(vx_tcb_t);

    char *block = (char *)malloc(total);
    if (!block) return 0;

    char *tp = block + below;
    if (img) {
        memcpy(tp - img, __tdata_start, tls_data_size());
        memset(tp - img + tls_data_size(), 0, img - tls_data_size());
    }

    vx_tcb_t *tcb = (vx_tcb_t *)tp;
    tcb->self = tcb;
    tcb->dtv  = 0;
    tcb->thr  = t;
    tcb->err  = 0;

    t->tlsblock = block;
    t->tlssize  = total;
    return tcb;
}

static void tls_install(vx_tcb_t *tcb) {
    __syscall1(SYS_SET_FSBASE, (long)(uintptr_t)tcb);
}

void __libc_init_tls(void) {
    if (tls_ready) return;

    main_thread.tid      = (uint32_t)__syscall0(SYS_GETTID);
    main_thread.done     = 1;
    main_thread.is_main  = 1;
    main_thread.detached = 1;
    memcpy(main_thread.name, "main", 5);

    vx_tcb_t *tcb = tls_alloc(&main_thread);
    if (!tcb) {
        /* No memory for a TLS image. The static fallback keeps errno and
         * pthread_self working; only the program's own `__thread`
         * variables are unavailable, and a program that cannot allocate
         * a hundred bytes has larger problems. */
        main_tcb.self = &main_tcb;
        main_tcb.thr  = &main_thread;
        return;
    }
    tls_install(tcb);
    /* Set last. Until this store, every accessor uses the static block;
     * after it, they read through FS — and FS is only valid because the
     * line above ran first. */
    __atomic_store_n(&tls_ready, 1, __ATOMIC_RELEASE);
}

static struct pthread_impl *self_impl(void) {
    if (!tls_ready) {
        /* Still the main thread, whether or not it has announced itself.
         * Filling in the tid here means pthread_self() is usable from a
         * program that never calls anything else in this library. */
        if (!main_thread.tid)
            main_thread.tid = (uint32_t)__syscall0(SYS_GETTID);
        if (!main_tcb.thr) { main_tcb.self = &main_tcb; main_tcb.thr = &main_thread; }
        main_thread.is_main = 1;
        return &main_thread;
    }
    return tcb_self()->thr;
}

pthread_t pthread_self(void)          { return self_impl(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int pthread_gettid(void)              { return (int)self_impl()->tid; }

/* ===== 3. RETIRED THREADS =====
 *
 * A thread cannot free the stack it is standing on, and a detached
 * thread has no joiner to do it afterwards. So a detached thread that
 * finishes puts its own record on this list and the *next* call into
 * pthread_create collects it.
 *
 * What makes that safe is the order of the last two things a dying
 * thread does. It publishes itself here, and then — in one block of
 * assembly, with no stack access between them — it clears `done` and
 * issues the exit call, which never returns. So from the moment `done`
 * reads zero, the thread has executed no further instruction against its
 * stack and never will. Anything the reaper sees with `done` clear is
 * genuinely finished.
 *
 * Written as inline assembly for exactly that reason. In C, the compiler
 * is free to spill a register to the stack between the store and the
 * call, and the whole argument collapses.
 */
static pthread_mutex_t retired_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pthread_impl *retired_list = 0;

static void reap_retired(void) {
    struct pthread_impl *take = 0;

    pthread_mutex_lock(&retired_lock);
    struct pthread_impl **pp = &retired_list;
    while (*pp) {
        struct pthread_impl *t = *pp;
        if (load_acq(&t->done) == 0) {
            *pp = t->retired_next;
            t->retired_next = take;
            take = t;
        } else {
            pp = &t->retired_next;
        }
    }
    pthread_mutex_unlock(&retired_lock);

    while (take) {
        struct pthread_impl *t = take;
        take = t->retired_next;
        if (t->stack) munmap(t->stack, t->stacksize);
        if (t->tlsblock) free(t->tlsblock);
        free(t);
    }
}

/* ===== 4. THREAD-SPECIFIC DATA ===== */

static struct {
    volatile uint32_t used;
    void (*destructor)(void *);
} keys[PTHREAD_KEYS_MAX];

static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    __libc_init_tls();
    pthread_mutex_lock(&key_lock);
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (keys[i].used) continue;
        keys[i].used = 1;
        keys[i].destructor = destructor;
        *key = i;
        pthread_mutex_unlock(&key_lock);
        return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    pthread_mutex_lock(&key_lock);
    keys[key].used = 0;
    keys[key].destructor = 0;
    pthread_mutex_unlock(&key_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return 0;
    return self_impl()->specific[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    self_impl()->specific[key] = (void *)value;
    return 0;
}

/*
 * Run the destructors for one thread that is ending.
 *
 * Repeated, because a destructor may set the value of another key — or
 * of its own — and POSIX requires those to be run too. Bounded, because
 * it may do so forever; four passes is what the standard names and what
 * every implementation uses.
 */
static void run_key_destructors(struct pthread_impl *t) {
    for (int pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; pass++) {
        int any = 0;
        for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
            void *v = t->specific[i];
            if (!v || !keys[i].used || !keys[i].destructor) continue;
            /* Cleared before the call, so a destructor that sets it
             * again is asking for another pass rather than looping
             * inside this one. */
            t->specific[i] = 0;
            keys[i].destructor(v);
            any = 1;
        }
        if (!any) break;
    }
}

/* ===== 5. MUTEXES ===== */

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
    if (!a) return EINVAL;
    a->kind = PTHREAD_MUTEX_DEFAULT; a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int kind) {
    if (!a || kind < 0 || kind > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    a->kind = kind;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *kind) {
    if (!a || !kind) return EINVAL;
    *kind = a->kind;
    return 0;
}
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared) {
    if (!a) return EINVAL;
    /* Accepted only in the form this system can keep. */
    if (pshared != PTHREAD_PROCESS_PRIVATE) return EINVAL;
    a->pshared = pshared;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    if (!m) return EINVAL;
    m->word  = 0;
    m->kind  = a ? a->kind : PTHREAD_MUTEX_DEFAULT;
    m->owner = 0;
    m->depth = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (load_acq(&m->word) != 0) return EBUSY;
    return 0;
}

/*
 * The three-state lock from <vxmutex.h>, with the two kinds that need to
 * know who holds it layered on top.
 *
 * 0 free, 1 held, 2 held with somebody asleep. The third state is what
 * keeps unlock off the system call path: a two-state lock cannot tell
 * whether anybody is listening, so it must wake on every release, which
 * puts a syscall on the *uncontended* path and gives back everything
 * this design exists to save.
 */
int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) return EINVAL;

    if (m->kind != PTHREAD_MUTEX_NORMAL) {
        uint32_t me = (uint32_t)__syscall0(SYS_GETTID);
        if (load_acq(&m->word) != 0 && m->owner == me) {
            if (m->kind == PTHREAD_MUTEX_RECURSIVE) {
                if (m->depth == 0x7FFFFFFF) return EAGAIN;
                m->depth++;
                return 0;
            }
            return EDEADLK;               /* error-checking */
        }
        if (cas(&m->word, 0, 1)) { m->owner = me; m->depth = 1; return 0; }
        while (__atomic_exchange_n(&m->word, 2u, __ATOMIC_ACQUIRE) != 0)
            futex_wait(&m->word, 2);
        m->owner = me;
        m->depth = 1;
        return 0;
    }

    if (cas(&m->word, 0, 1)) return 0;
    /*
     * Contended. The exchange both takes the lock if it happens to be
     * free and marks it contested if it is not, in one operation — which
     * is what makes the announcement impossible to lose. Sleeping only
     * when the word is *observed* to be contested closes the race with
     * an unlock that lands between the exchange and the system call: the
     * kernel re-reads the word with interrupts masked and returns at
     * once if it has changed.
     */
    while (__atomic_exchange_n(&m->word, 2u, __ATOMIC_ACQUIRE) != 0)
        futex_wait(&m->word, 2);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->kind != PTHREAD_MUTEX_NORMAL) {
        uint32_t me = (uint32_t)__syscall0(SYS_GETTID);
        if (load_acq(&m->word) != 0 && m->owner == me) {
            if (m->kind == PTHREAD_MUTEX_RECURSIVE) { m->depth++; return 0; }
            return EBUSY;
        }
        if (cas(&m->word, 0, 1)) { m->owner = me; m->depth = 1; return 0; }
        return EBUSY;
    }
    return cas(&m->word, 0, 1) ? 0 : EBUSY;
}

static uint64_t millis_now(void) {
    return (uint64_t)__syscall0(SYS_TICKS);
}

/* An absolute deadline in the caller's terms, as a count of our own
 * milliseconds. The two clocks share an origin, so this is a unit
 * conversion and not a translation. */
static uint64_t abs_to_millis(const struct timespec *abs) {
    if (!abs) return 0;
    int64_t ms = (int64_t)abs->tv_sec * 1000 + abs->tv_nsec / 1000000;
    return ms < 0 ? 0 : (uint64_t)ms;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs) {
    if (!m) return EINVAL;
    if (!abs) return pthread_mutex_lock(m);

    uint64_t deadline = abs_to_millis(abs);
    for (;;) {
        int r = pthread_mutex_trylock(m);
        if (r != EBUSY) return r;
        if (millis_now() >= deadline) return ETIMEDOUT;
        /* The park is bounded by the kernel anyway, so the deadline is
         * re-tested at least five times a second whatever happens. */
        futex_wait(&m->word, 2);
    }
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;

    if (m->kind != PTHREAD_MUTEX_NORMAL) {
        uint32_t me = (uint32_t)__syscall0(SYS_GETTID);
        if (load_acq(&m->word) == 0 || m->owner != me) return EPERM;
        if (m->kind == PTHREAD_MUTEX_RECURSIVE && --m->depth > 0) return 0;
        m->depth = 0;
        m->owner = 0;
    }

    /* One store and a comparison for a lock nobody slept on, which is
     * nearly every unlock. */
    if (__atomic_exchange_n(&m->word, 0u, __ATOMIC_RELEASE) == 2)
        futex_wake(&m->word, 0);
    return 0;
}

/* ===== 6. CONDITION VARIABLES ===== */

int pthread_condattr_init(pthread_condattr_t *a) {
    if (!a) return EINVAL;
    a->pshared = PTHREAD_PROCESS_PRIVATE;
    a->clock = CLOCK_MONOTONIC;
    return 0;
}
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, int clock) {
    if (!a) return EINVAL;
    a->clock = clock;   /* both clocks are the same count here */
    return 0;
}
int pthread_condattr_setpshared(pthread_condattr_t *a, int pshared) {
    if (!a) return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE) return EINVAL;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    if (!c) return EINVAL;
    c->seq = 0;
    c->waiters = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) {
    if (!c) return EINVAL;
    return load_acq(&c->waiters) ? EBUSY : 0;
}

/*
 * Wait, and the lost-wakeup problem it exists to solve.
 *
 * The naive implementation unlocks the mutex, sleeps, and relocks. Its
 * bug is the gap: between the unlock and the sleep, another thread can
 * take the mutex, change the condition, and signal — and the signal
 * arrives at a thread that is not yet asleep, so it goes nowhere and the
 * waiter then sleeps on a condition that has already happened.
 *
 * The sequence number closes it. It is read *before* the mutex is
 * released, so it is a snapshot of the world as the waiter last saw it
 * while holding the lock. A signal increments it. Sleeping only while
 * the number is still the one that was read means a signal in the gap is
 * not missed but merely already accounted for: the futex wait returns
 * immediately because the kernel re-reads the word and finds it changed.
 */
static int cond_wait_common(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abs) {
    if (!c || !m) return EINVAL;

    uint32_t seen = load_acq(&c->seq);
    __atomic_add_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);

    int kind  = m->kind;
    int depth = m->depth;
    pthread_mutex_unlock(m);

    uint64_t deadline = abs ? abs_to_millis(abs) : 0;
    int rc = 0;

    while (load_acq(&c->seq) == seen) {
        if (abs && millis_now() >= deadline) { rc = ETIMEDOUT; break; }
        struct pthread_impl *me = self_impl();
        if (me->cancel_requested && me->cancel_state == PTHREAD_CANCEL_ENABLE) {
            rc = ECANCELED;
            break;
        }
        futex_wait(&c->seq, seen);
    }

    __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(m);
    /* A recursive mutex held several times deep must come back at the
     * same depth. The unlock above took it to zero whatever it was. */
    if (kind == PTHREAD_MUTEX_RECURSIVE) m->depth = depth;

    if (rc == ECANCELED) pthread_exit(PTHREAD_CANCELED);
    return rc;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return cond_wait_common(c, m, 0);
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abs) {
    return cond_wait_common(c, m, abs);
}

int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    /*
     * Wakes every waiter, not one, and that is a deliberate imprecision
     * rather than an oversight.
     *
     * The futex channel is the sequence word, and all waiters are parked
     * on it — there is no per-waiter channel to aim at. Waking one would
     * mean waking an *arbitrary* one, and since every waiter re-tests
     * `seq` and any of them may find it already advanced by a different
     * signal, the one woken could go straight back to sleep while
     * another that could have proceeded stays parked.
     *
     * So this is a broadcast, which is always correct and sometimes
     * wasteful: n waiters wake, one proceeds, the rest see the sequence
     * they already saw and park again. The cost is n-1 spurious wakeups
     * per signal. The alternative — a wait queue with per-waiter words —
     * is the right answer for a system where threads run on several
     * processors, and buys nothing here, where they do not.
     */
    if (load_acq(&c->waiters)) futex_wake(&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    if (load_acq(&c->waiters)) futex_wake(&c->seq, 1);
    return 0;
}

/* ===== 7. READ-WRITE LOCKS =====
 *
 * One word: zero is free, all-ones is held for writing, anything else is
 * a count of readers. A reader takes the lock with a compare-and-exchange
 * that increments; a writer with one that moves it from zero to all-ones.
 *
 * Writers are not given priority, which means a steady stream of readers
 * can starve one indefinitely. That is the honest trade for a lock this
 * small — preventing it needs a second word to hold writers' intent and
 * a rule that readers respect it, and every caller in this system takes
 * a read lock briefly and infrequently.
 */
#define RW_WRITER 0xFFFFFFFFu

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    if (a) a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    (void)a;
    if (!l) return EINVAL;
    l->state = 0; l->waiters = 0;
    return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    return load_acq(&l->state) ? EBUSY : 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    for (;;) {
        uint32_t s = load_acq(&l->state);
        if (s == RW_WRITER) return EBUSY;
        if (s == RW_WRITER - 1) return EAGAIN;      /* too many readers */
        if (cas(&l->state, s, s + 1)) return 0;
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    for (;;) {
        int r = pthread_rwlock_tryrdlock(l);
        if (r != EBUSY) return r;
        __atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
        futex_wait(&l->state, RW_WRITER);
        __atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    return cas(&l->state, 0, RW_WRITER) ? 0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    for (;;) {
        if (cas(&l->state, 0, RW_WRITER)) return 0;
        uint32_t s = load_acq(&l->state);
        if (s == 0) continue;                  /* it just went free */
        __atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
        futex_wait(&l->state, s);
        __atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    uint32_t s = load_acq(&l->state);
    if (s == RW_WRITER) {
        store_rel(&l->state, 0);
    } else {
        if (s == 0) return EPERM;
        __atomic_sub_fetch(&l->state, 1, __ATOMIC_ACQ_REL);
        if (load_acq(&l->state) != 0) return 0;   /* readers remain */
    }
    if (load_acq(&l->waiters)) futex_wake(&l->state, 1);
    return 0;
}

/* ===== 8. SPIN LOCKS ===== */

int pthread_spin_init(pthread_spinlock_t *s, int pshared) {
    (void)pshared;
    if (!s) return EINVAL;
    *s = 0;
    return 0;
}
int pthread_spin_destroy(pthread_spinlock_t *s) { (void)s; return 0; }
int pthread_spin_trylock(pthread_spinlock_t *s) {
    if (!s) return EINVAL;
    return cas(s, 0, 1) ? 0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *s) {
    if (!s) return EINVAL;
    for (;;) {
        if (cas(s, 0, 1)) return 0;
        /*
         * A short spin, and then the processor is handed back.
         *
         * A true spin lock never yields, and on a machine where all user
         * threads share one processor that is a guaranteed deadlock
         * rather than a performance question: the holder cannot run
         * while the spinner is holding the only processor there is. So
         * this is a spin lock in interface and an adaptive one in
         * behaviour, and the burst below exists only for the case where
         * the holder is about to be preempted anyway.
         */
        for (int i = 0; i < 64; i++) {
            if (load_acq(s) == 0) break;
            __asm__ volatile("pause" ::: "memory");
        }
        if (load_acq(s) != 0) sched_yield();
    }
}

int pthread_spin_unlock(pthread_spinlock_t *s) {
    if (!s) return EINVAL;
    store_rel(s, 0);
    return 0;
}

/* ===== 9. BARRIERS ===== */

int pthread_barrierattr_init(pthread_barrierattr_t *a) {
    if (a) a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}
int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { (void)a; return 0; }

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a,
                         unsigned int count) {
    (void)a;
    if (!b || count == 0) return EINVAL;
    b->count = 0;
    b->seq   = 0;
    b->total = count;
    return 0;
}
int pthread_barrier_destroy(pthread_barrier_t *b) {
    if (!b) return EINVAL;
    return 0;
}

/*
 * Arrive, and wait for everyone else.
 *
 * `seq` is a generation counter and it is what makes a barrier reusable.
 * Without it, a fast thread could pass the barrier, loop, and arrive
 * again before a slow one had left — and be counted into the round it
 * has already completed. Waiting on the generation rather than on the
 * count means a thread from the next round bumps a number the previous
 * round's sleepers do not care about.
 */
int pthread_barrier_wait(pthread_barrier_t *b) {
    if (!b) return EINVAL;

    uint32_t gen = load_acq(&b->seq);
    uint32_t n = __atomic_add_fetch(&b->count, 1, __ATOMIC_ACQ_REL);

    if (n >= b->total) {
        store_rel(&b->count, 0);
        __atomic_add_fetch(&b->seq, 1, __ATOMIC_ACQ_REL);
        futex_wake(&b->seq, 1);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (load_acq(&b->seq) == gen)
        futex_wait(&b->seq, gen);
    return 0;
}

/* ===== 10. ONCE ===== */

#define ONCE_NEW     0
#define ONCE_RUNNING 1
#define ONCE_DONE    2

/*
 * Run this exactly once, and make every other caller wait for it.
 *
 * Waiting is the part that is easy to get wrong. An implementation that
 * returned immediately when it saw ONCE_RUNNING would let a second
 * thread proceed as though the initialisation had happened, which is the
 * whole thing pthread_once exists to prevent — the object is not built
 * yet.
 */
int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (!once || !fn) return EINVAL;

    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == ONCE_DONE) return 0;

    int expect = ONCE_NEW;
    if (__atomic_compare_exchange_n(once, &expect, ONCE_RUNNING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(once, ONCE_DONE, __ATOMIC_RELEASE);
        futex_wake((volatile uint32_t *)once, 1);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != ONCE_DONE)
        futex_wait((volatile uint32_t *)once, ONCE_RUNNING);
    return 0;
}

/* ===== 11. SEMAPHORES ===== */

int sem_init(sem_t *s, int pshared, unsigned int value) {
    if (!s) return EINVAL;
    if (pshared) { errno = ENOSYS; return -1; }
    s->count = value;
    s->waiters = 0;
    return 0;
}
int sem_destroy(sem_t *s) { (void)s; return 0; }

int sem_trywait(sem_t *s) {
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        uint32_t v = load_acq(&s->count);
        if (v == 0) { errno = EAGAIN; return -1; }
        if (cas(&s->count, v, v - 1)) return 0;
    }
}

int sem_wait(sem_t *s) {
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        if (sem_trywait(s) == 0) return 0;
        struct pthread_impl *me = self_impl();
        if (me->cancel_requested && me->cancel_state == PTHREAD_CANCEL_ENABLE)
            pthread_exit(PTHREAD_CANCELED);
        __atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        futex_wait(&s->count, 0);
        __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int sem_timedwait(sem_t *s, const struct timespec *abs) {
    if (!s) { errno = EINVAL; return -1; }
    if (!abs) return sem_wait(s);
    uint64_t deadline = abs_to_millis(abs);
    for (;;) {
        if (sem_trywait(s) == 0) return 0;
        if (millis_now() >= deadline) { errno = ETIMEDOUT; return -1; }
        __atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        futex_wait(&s->count, 0);
        __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int sem_post(sem_t *s) {
    if (!s) { errno = EINVAL; return -1; }
    __atomic_add_fetch(&s->count, 1, __ATOMIC_ACQ_REL);
    if (load_acq(&s->waiters)) futex_wake(&s->count, 1);
    return 0;
}

int sem_getvalue(sem_t *s, int *out) {
    if (!s || !out) { errno = EINVAL; return -1; }
    *out = (int)load_acq(&s->count);
    return 0;
}

/* ===== 12. ATTRIBUTES ===== */

#define DEFAULT_STACK (256 * 1024)

int pthread_attr_init(pthread_attr_t *a) {
    if (!a) return EINVAL;
    a->stacksize   = DEFAULT_STACK;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->guardsize   = 4096;
    a->stackaddr   = 0;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    if (!a || (state != PTHREAD_CREATE_JOINABLE &&
               state != PTHREAD_CREATE_DETACHED)) return EINVAL;
    a->detachstate = state;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state) {
    if (!a || !state) return EINVAL;
    *state = a->detachstate;
    return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (!a || size < PTHREAD_STACK_MIN) return EINVAL;
    a->stacksize = size;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size) {
    if (!a || !size) return EINVAL;
    *size = a->stacksize;
    return 0;
}
int pthread_attr_setguardsize(pthread_attr_t *a, size_t size) {
    if (!a) return EINVAL;
    a->guardsize = (int)size;
    return 0;
}
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size) {
    if (!a || !size) return EINVAL;
    *size = (size_t)a->guardsize;
    return 0;
}
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t size) {
    if (!a || size < PTHREAD_STACK_MIN) return EINVAL;
    a->stackaddr = addr;
    a->stacksize = size;
    return 0;
}
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *size) {
    if (!a || !addr || !size) return EINVAL;
    *addr = a->stackaddr;
    *size = a->stacksize;
    return 0;
}
int pthread_attr_setscope(pthread_attr_t *a, int scope) { (void)a; (void)scope; return 0; }
int pthread_attr_setinheritsched(pthread_attr_t *a, int i) { (void)a; (void)i; return 0; }
int pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { (void)a; (void)p; return 0; }
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p) {
    (void)a; (void)p; return 0;
}

/* ===== 13. CREATING AND ENDING A THREAD ===== */

/*
 * Where a new thread begins.
 *
 * Reached by IRETQ from the kernel with RSP already loaded and RDI
 * holding this argument — there is no caller and no return address, so
 * this function must never return. It ends by calling pthread_exit,
 * which is the same path a thread that returns from its start routine
 * takes.
 */
static void thread_entry(void *arg) {
    struct pthread_impl *t = (struct pthread_impl *)arg;

    /* The base was installed by the kernel from the clone call's fourth
     * argument, so `__thread` variables already work here. What is not
     * yet true is that this library knows it. */
    t->tid = (uint32_t)__syscall0(SYS_GETTID);

    void *r = t->start(t->arg);
    pthread_exit(r);
}

static volatile uint32_t live_threads = 1;      /* the main thread counts */

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (!start) return EINVAL;

    __libc_init_tls();
    reap_retired();

    pthread_attr_t def;
    if (!attr) { pthread_attr_init(&def); attr = &def; }

    struct pthread_impl *t = (struct pthread_impl *)calloc(1, sizeof(*t));
    if (!t) return EAGAIN;

    t->start        = start;
    t->arg          = arg;
    t->detached     = (attr->detachstate == PTHREAD_CREATE_DETACHED);
    t->done         = 1;
    t->cancel_state = PTHREAD_CANCEL_ENABLE;
    t->cancel_type  = PTHREAD_CANCEL_DEFERRED;
    memcpy(t->name, "thread", 7);

    /*
     * The stack, page-aligned and from mmap rather than malloc.
     *
     * Not because the heap could not hold it, but because mmap leaves a
     * page unmapped between consecutive mappings — so a thread that runs
     * off the end of its stack faults on nothing instead of quietly
     * writing into whatever the allocator handed out next. That is the
     * same argument the kernel makes for its own stacks, for the same
     * price.
     */
    size_t ssize = (attr->stacksize + 4095u) & ~(size_t)4095u;
    void *stack = mmap(0, ssize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) { free(t); return EAGAIN; }
    t->stack     = stack;
    t->stacksize = ssize;

    vx_tcb_t *tcb = tls_alloc(t);
    if (!tcb) { munmap(stack, ssize); free(t); return EAGAIN; }

    /* Sixteen-aligned, as the ABI requires of a function's stack on
     * entry. The kernel checks this and refuses otherwise, because a
     * misaligned thread runs correctly until its first MOVAPS against a
     * local — a fault a long way from its cause. */
    uint64_t sp = ((uint64_t)(uintptr_t)stack + ssize) & ~(uint64_t)15u;

    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);

    long tid = __syscall4(SYS_CLONE, (long)(uintptr_t)thread_entry,
                          (long)sp, (long)(uintptr_t)t,
                          (long)(uintptr_t)tcb);
    if (tid < 0) {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        free(t->tlsblock);
        munmap(stack, ssize);
        free(t);
        return EAGAIN;
    }
    t->tid = (uint32_t)tid;
    if (out) *out = t;
    return 0;
}

/*
 * End this thread.
 *
 * The last two operations are one block of assembly and they have to be.
 * `done` going to zero is the signal that this thread's stack is free
 * for reuse; the exit call is what makes that true. Between them the
 * thread must touch nothing — and in C the compiler is entitled to spill
 * a register to the stack in exactly that gap, which would be a write to
 * memory another thread has just been told it may take.
 *
 * The futex wake is inside the same block for the same reason: a joiner
 * parked on `done` must be released, and doing it before the store would
 * wake it to a value that has not changed yet.
 */
void pthread_exit(void *retval) {
    struct pthread_impl *t = self_impl();

    run_key_destructors(t);
    t->retval = retval;

    /*
     * The last thread out ends the process, and this is where "the
     * program is finished" is actually decided.
     *
     * main() returning is not that decision — POSIX is explicit that a
     * process outlives its initial thread if others are running, and a
     * library that starts a worker and then returns from main expects
     * the worker to keep going. So the count is what ends it, and the
     * whole-process exit is a different system call from the
     * single-thread one precisely so that this line can choose.
     */
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0)
        __syscall1(SYS_EXIT_GROUP, 0);

    if (t->detached && !t->is_main) {
        pthread_mutex_lock(&retired_lock);
        t->retired_next = retired_list;
        retired_list = t;
        pthread_mutex_unlock(&retired_lock);
    }

    if (t->is_main) {
        /* The main thread's stack belongs to the loader, not to us, and
         * its control block is static. Nothing to hand back. */
        store_rel(&t->done, 0);
        futex_wake(&t->done, 1);
        __syscall1(SYS_THREAD_EXIT, 0);
        for (;;) { }
    }

    __asm__ volatile(
        /* done = 0, with the store visible before anything below it */
        "movl $0, (%0)\n"
        /* futex(&done, FUTEX_WAKE, 2) — release every joiner */
        "movq %0, %%rdi\n"
        "movl $1, %%esi\n"           /* FUTEX_WAKE */
        "movl $2, %%edx\n"           /* all of them */
        "movl $26, %%eax\n"          /* SYS_FUTEX */
        "syscall\n"
        /* and out, which does not return */
        "xorl %%edi, %%edi\n"
        "movl $34, %%eax\n"          /* SYS_THREAD_EXIT */
        "syscall\n"
        :
        : "r"(&t->done)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r11", "memory");

    for (;;) { }                     /* unreachable; the kernel took it */
}

int pthread_join(pthread_t t, void **retval) {
    if (!t) return ESRCH;
    if (t == self_impl()) return EDEADLK;
    if (t->detached) return EINVAL;

    while (load_acq(&t->done) != 0)
        futex_wait(&t->done, 1);

    if (retval) *retval = t->retval;

    /*
     * Safe to release here and only here. `done` reading zero means the
     * thread has issued its exit call and executed nothing since — see
     * the assembly in pthread_exit — so its stack and its thread-local
     * block have no user left.
     */
    if (t->stack) munmap(t->stack, t->stacksize);
    if (t->tlsblock) free(t->tlsblock);
    free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    if (!t) return ESRCH;
    if (t->detached) return EINVAL;
    t->detached = 1;
    /* Already finished, and nobody is coming to join it. Put it where
     * the reaper will find it rather than leaking the stack. */
    if (load_acq(&t->done) == 0) {
        pthread_mutex_lock(&retired_lock);
        t->retired_next = retired_list;
        retired_list = t;
        pthread_mutex_unlock(&retired_lock);
    }
    return 0;
}

/* ===== 14. CANCELLATION ===== */

int pthread_cancel(pthread_t t) {
    if (!t) return ESRCH;
    t->cancel_requested = 1;
    /* Nudge anything parked, so a thread waiting on a condition notices
     * within one park rather than whenever it happens to wake. */
    futex_wake(&t->done, 1);
    return 0;
}

int pthread_setcancelstate(int state, int *old) {
    struct pthread_impl *t = self_impl();
    if (old) *old = t->cancel_state;
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
        return EINVAL;
    t->cancel_state = state;
    return 0;
}

int pthread_setcanceltype(int type, int *old) {
    struct pthread_impl *t = self_impl();
    if (old) *old = t->cancel_type;
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
        return EINVAL;
    /* Recorded and not honoured; asynchronous behaves as deferred. See
     * the note in <pthread.h> for why stopping a thread at an arbitrary
     * instruction is not something this system can make safe. */
    t->cancel_type = type;
    return 0;
}

void pthread_testcancel(void) {
    struct pthread_impl *t = self_impl();
    if (t->cancel_requested && t->cancel_state == PTHREAD_CANCEL_ENABLE)
        pthread_exit(PTHREAD_CANCELED);
}

/* ===== 15. THE REST ===== */

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p) {
    (void)t; (void)policy; (void)p;
    return 0;                        /* accepted and ignored; see the header */
}
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p) {
    (void)t;
    if (policy) *policy = SCHED_OTHER;
    if (p) p->sched_priority = 0;
    return 0;
}
int pthread_setname_np(pthread_t t, const char *name) {
    if (!t || !name) return EINVAL;
    size_t n = strnlen(name, sizeof(t->name) - 1);
    memcpy(t->name, name, n);
    t->name[n] = '\0';
    return 0;
}
int pthread_getname_np(pthread_t t, char *name, size_t len) {
    if (!t || !name || len == 0) return EINVAL;
    size_t n = strnlen(t->name, len - 1);
    memcpy(name, t->name, n);
    name[n] = '\0';
    return 0;
}
int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void)) {
    (void)prepare; (void)parent; (void)child;
    /*
     * Accepted and never called, because fork and threads are not
     * combined anywhere in this system. A fork here duplicates the
     * calling thread and nothing else, which leaves every lock the other
     * threads held locked forever in the child — the exact hazard these
     * handlers exist to manage. Registering them would suggest the
     * combination is supported.
     */
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF: return 1;   /* user threads share one */
    case _SC_PAGESIZE:         return 4096;
    case _SC_OPEN_MAX:         return 3;
    case _SC_CLK_TCK:          return 1000;
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
        /* SYS_MEMINFO answers in kilobytes: free, then total. */
        uint64_t info[2] = { 0, 0 };
        if (__syscall1(SYS_MEMINFO, (long)(uintptr_t)info) != 0) return -1;
        return (long)((name == _SC_PHYS_PAGES ? info[1] : info[0]) / 4);
    }
    default: return -1;
    }
}
