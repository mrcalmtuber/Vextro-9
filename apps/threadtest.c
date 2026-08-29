/*
 * threadtest — does any of the new C library actually work on the
 * machine?
 *
 * tools/math_test.c checks the arithmetic on the host, where a reference
 * libm exists to check it against. Nothing about threads, mmap or
 * thread-local storage can be checked that way: they are not
 * computations, they are the behaviour of the kernel underneath, and the
 * only place that exists is here.
 *
 * So this runs in ring 3 on the real system and asserts the properties
 * that would be silently wrong if the kernel work were subtly incorrect.
 * Each one is chosen because it fails differently when the corresponding
 * piece is broken:
 *
 *   Threads share memory. If SYS_CLONE copied the address space
 *   instead of sharing it — which is what fork does and what the
 *   obvious implementation would have done — every thread would
 *   increment its own private counter and the total would come out at
 *   one per thread instead of the sum.
 *
 *   The address space outlives its first thread. Before the refcount,
 *   the first thread to exit destroyed the page tables the others were
 *   running on. That does not produce a wrong answer; it produces a
 *   triple fault, so a run that reaches the end at all is the check.
 *
 *   Thread-local storage is per thread. If the FS base were written
 *   once for the machine, as the GS base is, every thread would read
 *   the same variable and the last writer would win.
 *
 *   mmap reserves without committing. A gigabyte on a machine with less
 *   than that must succeed and must not consume it, or the reservation
 *   is being backed eagerly.
 *
 *   A mutex under contention is actually exclusive. A lock that is
 *   merely usually right is indistinguishable from a correct one at low
 *   thread counts, so the counter loop below is deliberately tight.
 */

#include "vextro.h"
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>

static int failures = 0;
static int checks   = 0;

/*
 * A comparison function at file scope, and not a nested one.
 *
 * GCC will happily accept a function defined inside another and pass a
 * pointer to it — and it implements that by writing a trampoline onto
 * the *stack* and pointing at that. The stack here is mapped
 * no-execute, as every writable page in this system is, so the call
 * would fault. That is W^X working exactly as intended, and it is worth
 * knowing that this is one of the constructs it rules out.
 */
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* ===== shared counter, under a mutex ===== */

#define WORKERS   4
#define PER_WORKER 20000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long            counter      = 0;

/* Deliberately unlocked, to show the difference. If threads did not
 * share memory this would equal the locked count; if they do share it
 * and the lock works, this one loses updates and the locked one does
 * not. */
static volatile long   counter_racy = 0;

static void *counter_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < PER_WORKER; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
        counter_racy++;
    }
    return (void *)(long)PER_WORKER;
}

/* ===== thread-local storage ===== */

static __thread int tls_value = 0xA5;
static __thread char tls_buf[32];

static void *tls_worker(void *arg) {
    int mine = (int)(long)arg;

    /* The initialised template must have been copied, not shared: this
     * reads the value from .tdata even though another thread has
     * already overwritten its own copy. */
    if (tls_value != 0xA5) return (void *)1;

    tls_value = mine;
    snprintf(tls_buf, sizeof(tls_buf), "thread-%d", mine);

    /* Give the others a chance to write theirs before checking that ours
     * survived — the whole point is that they cannot reach it. */
    for (int i = 0; i < 200; i++) sched_yield();

    if (tls_value != mine) return (void *)2;

    char want[32];
    snprintf(want, sizeof(want), "thread-%d", mine);
    if (strcmp(tls_buf, want) != 0) return (void *)3;
    return 0;
}

/* ===== a producer and a consumer over a condition variable ===== */

#define RING 8
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  ring_space = PTHREAD_COND_INITIALIZER;
static int             ring[RING];
static int             ring_count = 0, ring_head = 0, ring_tail = 0;
static long            consumed_sum = 0;

#define MESSAGES 5000

static void *producer(void *arg) {
    (void)arg;
    for (int i = 1; i <= MESSAGES; i++) {
        pthread_mutex_lock(&ring_lock);
        while (ring_count == RING) pthread_cond_wait(&ring_space, &ring_lock);
        ring[ring_tail] = i;
        ring_tail = (ring_tail + 1) % RING;
        ring_count++;
        pthread_cond_signal(&ring_ready);
        pthread_mutex_unlock(&ring_lock);
    }
    return 0;
}

static void *consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < MESSAGES; i++) {
        pthread_mutex_lock(&ring_lock);
        while (ring_count == 0) pthread_cond_wait(&ring_ready, &ring_lock);
        int v = ring[ring_head];
        ring_head = (ring_head + 1) % RING;
        ring_count--;
        consumed_sum += v;
        pthread_cond_signal(&ring_space);
        pthread_mutex_unlock(&ring_lock);
    }
    return 0;
}

/* ===== once, keys, barriers ===== */

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static volatile int   once_ran = 0;
static void once_fn(void) { once_ran++; }

static void *once_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) pthread_once(&once_control, once_fn);
    return 0;
}

static pthread_key_t  the_key;
static volatile int   destructor_calls = 0;
static void key_destructor(void *v) { (void)v; destructor_calls++; }

static void *key_worker(void *arg) {
    pthread_setspecific(the_key, arg);
    for (int i = 0; i < 50; i++) sched_yield();
    return pthread_getspecific(the_key);
}

static pthread_barrier_t the_barrier;
static volatile int      barrier_before = 0;
static volatile int      barrier_after  = 0;

static void *barrier_worker(void *arg) {
    (void)arg;
    __atomic_add_fetch(&barrier_before, 1, __ATOMIC_SEQ_CST);
    pthread_barrier_wait(&the_barrier);
    /* Every thread must have arrived before any leaves, so nobody can
     * see a `before` count below the total. */
    int seen = __atomic_load_n(&barrier_before, __ATOMIC_SEQ_CST);
    if (seen == WORKERS) __atomic_add_fetch(&barrier_after, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* ===== the detached thread, which must clean up after itself ===== */

static sem_t detached_done;
static void *detached_worker(void *arg) {
    (void)arg;
    sem_post(&detached_done);
    return 0;
}

void _start(void) {
    /* No crt0 here: this program keeps the old entry convention so it
     * runs on a loader that knows nothing about the new one. That means
     * the thread pointer has to be installed by hand before the first
     * thread-local access. */
    __libc_init_tls();

    printf("threadtest: the C library, on the machine\n");

    /* ---- 1. threads share memory, and the lock is exclusive ---- */
    {
        pthread_t t[WORKERS];
        int made = 0;
        for (int i = 0; i < WORKERS; i++)
            if (pthread_create(&t[i], 0, counter_worker, 0) == 0) made++;
        ok("four threads started", made == WORKERS);

        long joined = 0;
        for (int i = 0; i < made; i++) {
            void *r = 0;
            pthread_join(t[i], &r);
            joined += (long)r;
        }
        ok("each thread returned its own count",
           joined == (long)made * PER_WORKER);
        ok("the locked counter is exact",
           counter == (long)made * PER_WORKER);
        /* Not asserted as a failure: on one processor an unlocked
         * increment may well come out right, because a preemption in the
         * middle of one is unlikely rather than impossible. Printed
         * because when it does differ, it is the clearest possible
         * evidence that the memory really is shared. */
        printf("       (the unlocked counter reached %ld of %ld)\n",
               counter_racy, (long)made * PER_WORKER);
    }

    /* ---- 2. thread-local storage is per thread ---- */
    {
        tls_value = 0x5A;                 /* the main thread's own copy */
        pthread_t t[WORKERS];
        int made = 0;
        for (int i = 0; i < WORKERS; i++)
            if (pthread_create(&t[i], 0, tls_worker, (void *)(long)(i + 1)) == 0)
                made++;
        long bad = 0;
        for (int i = 0; i < made; i++) {
            void *r = 0;
            pthread_join(t[i], &r);
            if (r) bad++;
        }
        ok("each thread has its own __thread variables", bad == 0);
        ok("the main thread's copy is untouched", tls_value == 0x5A);
    }

    /* ---- 3. a condition variable does not lose a wakeup ---- */
    {
        pthread_t p, c;
        int a = pthread_create(&p, 0, producer, 0);
        int b = pthread_create(&c, 0, consumer, 0);
        ok("producer and consumer started", a == 0 && b == 0);
        pthread_join(p, 0);
        pthread_join(c, 0);
        long want = (long)MESSAGES * (MESSAGES + 1) / 2;
        ok("every message crossed the ring exactly once",
           consumed_sum == want);
    }

    /* ---- 4. once, keys, barrier ---- */
    {
        pthread_t t[WORKERS];
        for (int i = 0; i < WORKERS; i++) pthread_create(&t[i], 0, once_worker, 0);
        for (int i = 0; i < WORKERS; i++) pthread_join(t[i], 0);
        ok("pthread_once ran the function exactly once", once_ran == 1);

        pthread_key_create(&the_key, key_destructor);
        for (int i = 0; i < WORKERS; i++)
            pthread_create(&t[i], 0, key_worker, (void *)(long)(i + 100));
        int keys_ok = 1;
        for (int i = 0; i < WORKERS; i++) {
            void *r = 0;
            pthread_join(t[i], &r);
            if ((long)r != (long)(i + 100)) keys_ok = 0;
        }
        ok("thread-specific data stayed with its thread", keys_ok);
        ok("the key destructor ran for every thread",
           destructor_calls == WORKERS);

        pthread_barrier_init(&the_barrier, 0, WORKERS);
        for (int i = 0; i < WORKERS; i++)
            pthread_create(&t[i], 0, barrier_worker, 0);
        for (int i = 0; i < WORKERS; i++) pthread_join(t[i], 0);
        ok("nobody left the barrier before everybody arrived",
           barrier_after == WORKERS);
        pthread_barrier_destroy(&the_barrier);
    }

    /* ---- 5. a detached thread ---- */
    {
        sem_init(&detached_done, 0, 0);
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_t d;
        int r = pthread_create(&d, &a, detached_worker, 0);
        ok("a detached thread started", r == 0);
        ok("and signalled the semaphore", sem_wait(&detached_done) == 0);
        pthread_attr_destroy(&a);
    }

    /* ---- 6. recursive and error-checking mutexes ---- */
    {
        pthread_mutexattr_t a;
        pthread_mutex_t m;

        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&m, &a);
        int deep = 1;
        for (int i = 0; i < 8; i++) if (pthread_mutex_lock(&m) != 0) deep = 0;
        for (int i = 0; i < 8; i++) if (pthread_mutex_unlock(&m) != 0) deep = 0;
        ok("a recursive mutex nests and unwinds", deep);
        /* Eight locks and eight unlocks leave it free, so a ninth unlock
         * is an error and must say so — a recursive mutex that let the
         * depth go negative would release a lock it still held. */
        ok("and refuses to unwind past zero",
           pthread_mutex_unlock(&m) == EPERM);
        pthread_mutex_destroy(&m);

        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&m, &a);
        pthread_mutex_lock(&m);
        ok("an error-checking mutex refuses to deadlock on itself",
           pthread_mutex_lock(&m) == EDEADLK);
        pthread_mutex_unlock(&m);
        ok("and reports an unlock it does not own",
           pthread_mutex_unlock(&m) == EPERM);
        pthread_mutexattr_destroy(&a);
    }

    /* ---- 7. mmap reserves without committing ---- */
    {
        uint64_t before[2] = { 0, 0 }, after[2] = { 0, 0 };
        __syscall1(SYS_MEMINFO, (long)(uintptr_t)before);

        /* A gigabyte, which is very likely more than this machine has
         * free. If it succeeds and the free count barely moves, the
         * reservation is genuinely lazy. */
        const size_t huge = 1024ull * 1024 * 1024;
        char *big = (char *)mmap(0, huge, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok("a one-gigabyte reservation succeeds", big != MAP_FAILED);

        __syscall1(SYS_MEMINFO, (long)(uintptr_t)after);
        long spent = (long)before[0] - (long)after[0];
        ok("and costs almost no physical memory", spent < 4096);
        printf("       (free memory moved by %ld kB)\n", spent);

        if (big != MAP_FAILED) {
            /* Touching it must work, at both ends and in the middle —
             * this is what proves the fault handler finds the
             * reservation wherever the address falls in it. */
            big[0] = 'a';
            big[huge / 2] = 'b';
            big[huge - 1] = 'c';
            ok("the first, middle and last pages fault in",
               big[0] == 'a' && big[huge / 2] == 'b' && big[huge - 1] == 'c');

            /* Fresh memory reads as zero, which is a promise and not an
             * accident: anything else would be another program's data. */
            int zeroed = 1;
            for (int i = 1; i < 4096; i++) if (big[huge / 2 + i]) zeroed = 0;
            ok("an untouched page reads as zero", zeroed);

            ok("and it unmaps", munmap(big, huge) == 0);
        }

        /* A modest mapping, written and read back. */
        size_t n = 256 * 1024;
        int *p = (int *)mmap(0, n, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        /*
         * Scalar stores first, then the vectorised loop below, and the
         * two are separate checks because they failed separately.
         *
         * A page fault taken in the middle of a vectorised store returns
         * to the *same instruction*, which the processor re-executes —
         * so if the fault handler used vector registers and did not put
         * them back, the retried store writes whatever the handler left
         * there. Scalar stores are immune, because a general-purpose
         * register is saved by the exception stub.
         *
         * That is exactly what happened, and this pair is what told
         * the two apart: these volatile writes came back correct while
         * the loop below did not.
         */
        if (p != MAP_FAILED) {
            volatile int *vp = p;
            int scalar_ok = 1;
            vp[0] = 11; vp[1] = 22; vp[2] = 33; vp[3] = 44;
            vp[4] = 55; vp[5] = 66; vp[6] = 77; vp[7] = 88;
            vp[1000] = 777; vp[20000] = 888; vp[65535] = 999;
            if (vp[0] != 11 || vp[3] != 44 || vp[7] != 88) scalar_ok = 0;
            if (vp[1000] != 777 || vp[20000] != 888 || vp[65535] != 999)
                scalar_ok = 0;
            ok("scalar stores across several pages survive", scalar_ok);
        }
        int good = (p != MAP_FAILED);
        if (!good) {
            printf("       (mmap of %lu bytes failed, errno %d)\n",
                   (unsigned long)n, errno);
        } else {
            for (size_t i = 0; i < n / sizeof(int); i++) p[i] = (int)i * 3;
            for (size_t i = 0; i < n / sizeof(int); i++)
                if (p[i] != (int)i * 3) {
                    printf("       (p[%lu] is %d, want %d; p = %p)\n",
                           (unsigned long)i, p[i], (int)i * 3, (void *)p);
                    good = 0;
                    break;
                }
            munmap(p, n);
        }
        ok("and so does a vectorised loop over the whole mapping", good);

        /* W^X, which is the whole reason the JIT is off. */
        void *wx = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok("a writable-and-executable mapping is refused", wx == MAP_FAILED);

        void *rw = mmap(0, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (rw != MAP_FAILED) {
            ok("and mprotect will not make one either",
               mprotect(rw, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1);
            ok("but read-only is allowed", mprotect(rw, 4096, PROT_READ) == 0);
            munmap(rw, 4096);
        }
    }

    /* ---- 8. the arithmetic, on the machine that will run it ---- */
    {
        /* Not an accuracy check — tools/math_test.c does that against a
         * reference. This is a check that the code executes correctly in
         * ring 3 at all: that the FPU state survives a system call and a
         * context switch, which the XMM clobber lists exist to
         * guarantee and which nothing else here would notice. */
        double s = 0;
        for (int i = 0; i < 1000; i++) {
            s += sin((double)i) * cos((double)i);
            if ((i & 63) == 0) sched_yield();     /* force a switch */
        }
        /* sum of sin(i)cos(i) = 0.5 * sum sin(2i), a bounded oscillation */
        ok("floating point survives context switches", fabs(s) < 10.0);

        ok("sqrt is exact", sqrt(1024.0) == 32.0);
        ok("pow handles a negative base and an odd integer",
           pow(-2.0, 3.0) == -8.0);
        ok("log2 of a power of two is exact", log2(65536.0) == 16.0);
        ok("a huge argument still reduces",
           fabs(sin(1e18)) <= 1.0 && sin(1e18) == sin(1e18));
        ok("strtod round-trips a simple decimal", strtod("2.5", 0) == 2.5);

        int v[9] = { 5, 3, 9, 1, 7, 2, 8, 4, 6 };
        qsort(v, 9, sizeof(int), cmp_int);
        int sorted = 1;
        for (int i = 0; i < 9; i++) if (v[i] != i + 1) sorted = 0;
        ok("qsort sorts", sorted);

        int want = 7;
        int *found = (int *)bsearch(&want, v, 9, sizeof(int), cmp_int);
        ok("bsearch finds", found && *found == 7);
    }

    /* ---- 9. time moves forward ---- */
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        struct timespec nap = { 0, 50 * 1000 * 1000 };     /* 50 ms */
        nanosleep(&nap, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long dms = (b.tv_sec - a.tv_sec) * 1000 +
                   (b.tv_nsec - a.tv_nsec) / 1000000;
        ok("nanosleep sleeps for about as long as asked",
           dms >= 40 && dms < 500);
        printf("       (asked for 50 ms, slept %ld)\n", dms);
    }

    printf("threadtest: %d checks, %d failures\n", checks, failures);
    printf(failures ? "threadtest: FAILED\n" : "threadtest: all passed\n");
}
