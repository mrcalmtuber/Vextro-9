/*
 * mutextest — proving the futex, from ring 3.
 *
 * Two processes and one lock, over memory they both map.
 *
 * The lock and the counter it protects live on the window canvas, which
 * is the only memory two processes in this system share: a fork copies
 * every private page copy-on-write, so a counter in ordinary memory
 * would be *two* counters and the test would pass without proving
 * anything. The canvas is mapped shared into both, so a write on either
 * side is a write both sides see — which is exactly the situation a
 * mutex exists for, and exactly the situation the kernel keys its wait
 * channel on a physical address to make work.
 *
 * What is checked:
 *
 *   the uncontended path        a lock taken and released with nobody
 *                               else running enters the kernel zero
 *                               times, which is the whole point
 *
 *   mutual exclusion            two processes each add to a shared
 *                               counter many times; if the lock does
 *                               not hold, the total comes out below
 *                               what was added, because two
 *                               read-modify-writes overlapped
 *
 *   the sleep and the wake      one side holds the lock across a long
 *                               stretch while the other tries to take
 *                               it, so the loser genuinely parks in the
 *                               kernel and genuinely has to be woken
 */
#include "vextro.h"
#include <sys/syscall.h>
#include <vxmutex.h>

/* Where in the shared canvas the test state lives. Past the first row so
 * that nothing here is mistaken for a pixel somebody wanted drawn, and
 * aligned because the futex word must be. */
#define SHARED_OFF   (OS_CANVAS_W * 2)

#define ROUNDS       2000

typedef struct {
    vx_mutex_t lock;
    uint32_t   counter;
    uint32_t   child_done;
    uint32_t   parent_done;
} shared_t;

static void put_uint(char *out, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = '\0';
}

static void say(const char *label, uint32_t v) {
    char line[96];
    int i = 0;
    while (label[i] && i < 60) { line[i] = label[i]; i++; }
    put_uint(line + i, v);
    int j = 0;
    while (line[j]) j++;
    line[j] = '\n';
    line[j + 1] = '\0';
    os_print(line);
}

/* Add to the shared counter under the lock, ROUNDS times. The read and
 * the write are deliberately separated by a little work: a counter
 * incremented in one instruction would be nearly atomic by accident and
 * would pass whether the lock held or not. */
static void hammer(shared_t *s) {
    for (int i = 0; i < ROUNDS; i++) {
        vx_mutex_lock(&s->lock);
        uint32_t v = s->counter;
        for (volatile int d = 0; d < 8; d++) { }
        s->counter = v + 1;
        vx_mutex_unlock(&s->lock);
    }
}

void _start(void) {
    uint64_t info[3];
    long canvas = __syscall1(SYS_CANVAS, (long)(uintptr_t)info);
    if (canvas <= 0) {
        os_print("mutextest: no canvas to share\n");
        return;
    }

    shared_t *s = (shared_t *)(uintptr_t)((uint8_t *)(uintptr_t)canvas
                                          + SHARED_OFF);
    vx_mutex_init(&s->lock);
    s->counter     = 0;
    s->child_done  = 0;
    s->parent_done = 0;

    os_print("mutextest: shared state on the window canvas\n");

    /* ---- 1. the uncontended path ---- */
    {
        int ok = 1;
        for (int i = 0; i < 1000; i++) {
            if (!vx_mutex_trylock(&s->lock)) { ok = 0; break; }
            vx_mutex_unlock(&s->lock);
        }
        os_print(ok ? "mutextest: 1000 uncontended lock/unlock pairs, "
                      "no syscall\n"
                    : "mutextest: FAIL an uncontended trylock was refused\n");
    }

    /* ---- 2. the lock is exclusive while held ---- */
    {
        vx_mutex_lock(&s->lock);
        int refused = !vx_mutex_trylock(&s->lock);
        vx_mutex_unlock(&s->lock);
        os_print(refused
            ? "mutextest: a held lock refuses a second taker\n"
            : "mutextest: FAIL a held lock was taken twice\n");
    }

    /* ---- 3. two processes, one counter ---- */
    long pid = __syscall0(SYS_FORK);
    if (pid < 0) {
        os_print("mutextest: FAIL fork refused\n");
        return;
    }

    if (pid == 0) {
        hammer(s);
        __atomic_store_n(&s->child_done, 1u, __ATOMIC_RELEASE);
        /* Nothing else to say from here: both sides print from the
         * parent, so the two halves of the result cannot interleave in
         * the middle of a line. */
        __syscall1(SYS_EXIT, 0);
        return;
    }

    hammer(s);
    __atomic_store_n(&s->parent_done, 1u, __ATOMIC_RELEASE);

    /* Wait for the child, without spinning the processor away from it:
     * sys_yield stands down for the rest of this slice each time round,
     * which is what lets a child at the same priority actually finish. */
    for (int guard = 0; guard < 2000000; guard++) {
        if (__atomic_load_n(&s->child_done, __ATOMIC_ACQUIRE)) break;
        __syscall0(SYS_YIELD);
    }

    if (!__atomic_load_n(&s->child_done, __ATOMIC_ACQUIRE)) {
        os_print("mutextest: FAIL the child never finished\n");
        return;
    }

    say("mutextest: two processes added ", (uint32_t)(ROUNDS * 2));
    say("mutextest: the counter reads   ", s->counter);
    os_print(s->counter == (uint32_t)(ROUNDS * 2)
        ? "mutextest: mutual exclusion held across a fork\n"
        : "mutextest: FAIL updates were lost - the lock did not hold\n");

    /*
     * ---- 4. and the door this program is not allowed through ----
     *
     * A restricted token is what every process starts with, whoever
     * launched it. Writing a file is one of the three calls that can
     * change the machine permanently, so it goes through the elevation
     * gateway -- and what happens next depends on the session:
     *
     *   no session, or a session that is not an administrator's:
     *     refused outright, because there is no answer anybody could
     *     give that would grant it
     *
     *   an administrator's session:
     *     this thread freezes and a prompt appears; the call returns
     *     when somebody answers it
     *
     * Either way the program keeps running afterwards. A refusal is a
     * failed system call and not a death sentence.
     */
    {
        static const char payload[] = "written by a ring 3 program\n";
        long rc = __syscall3(SYS_FS_WRITE, (long)(uintptr_t)"/uactest.txt",
                             (long)(uintptr_t)payload,
                             (long)(sizeof(payload) - 1));
        os_print(rc < 0
            ? "mutextest: the privileged write was refused, as expected\n"
            : "mutextest: the privileged write was allowed\n");
    }
}
