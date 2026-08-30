/*
 * libcxx/src/cxa.cpp — the parts of C++ the compiler calls by name.
 *
 * Nothing in this file is ever written by a programmer. Every symbol
 * here is emitted by the compiler into object files as a call, and a
 * runtime that is missing one produces a link failure at the end of a
 * long build with a mangled name and no explanation. That is the whole
 * reason it exists as a file rather than as three functions somewhere
 * convenient: this is the list, and it is worth being able to read it.
 *
 * ============================================================
 *  1. STATIC OBJECTS, AND WHEN THEY ARE DESTROYED
 * ============================================================
 *
 * A destructor for an object with static storage duration has to run at
 * exit, in reverse order of construction. The compiler arranges that by
 * calling __cxa_atexit at the point of construction, with the
 * destructor, the object, and a token for the code that owns it.
 *
 * The reverse order is not a nicety. A logger constructed before a cache
 * is destroyed after it, and the cache's destructor is allowed to log.
 * Destroying in construction order would have it write through a logger
 * that no longer exists.
 *
 * ============================================================
 *  2. FUNCTION-LOCAL STATICS, AND THE RACE THEY HAVE
 * ============================================================
 *
 *     Thing &instance() { static Thing t; return t; }
 *
 * Two threads reaching that for the first time at once must not both
 * construct it. Since C++11 the language *requires* that the second one
 * waits, and the compiler implements the requirement by calling
 * __cxa_guard_acquire before the constructor and __cxa_guard_release
 * after it.
 *
 * The guard is eight bytes the compiler allocates next to the object.
 * The first byte is the "already constructed" flag it tests inline
 * before it calls anything — so the fast path, which is every call after
 * the first, is a load and a branch and never reaches this file. What is
 * here is the slow path, and it is built on the same futex the C
 * library's mutex is, so a thread that has to wait is descheduled rather
 * than spinning.
 *
 * Note that -fno-threadsafe-statics is deliberately *not* used, which is
 * the shortcut this could have been. It would remove the calls and leave
 * the race.
 *
 * ============================================================
 *  3. THE ONES THAT MEAN SOMETHING WENT WRONG
 * ============================================================
 *
 * __cxa_pure_virtual, __cxa_deleted_virtual, and
 * __cxa_throw_bad_array_new_length. Each is emitted for a case the
 * compiler cannot rule out statically and each means the program is
 * already broken by the time it is reached; all three stop with a
 * message naming which it was.
 *
 * The last is the one that surprises people building with exceptions
 * off: `new T[n]` has to check that n * sizeof(T) has not overflowed,
 * and GCC emits a call to it for the failure whether or not exceptions
 * are available. Without it, a build fails to link the moment anything
 * allocates an array with a computed length.
 */

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <new>

extern "C" {
#include <sys/syscall.h>
}

/* ============================================================
 *  static destruction
 * ============================================================ */

/*
 * Ninety-six of them.
 *
 * A fixed table rather than a list allocated as it grows, and the reason
 * is the order of events at the end of a program: exit() runs these
 * *and* whatever the allocator has been asked to release, and a table
 * that had to be freed would be one more thing to get wrong in that
 * window. The bound is generous for the programs this runs -- a hundred
 * distinct namespace-scope objects with destructors is a large program
 * -- and running out is reported rather than silently dropped, because a
 * destructor that never runs is a file never flushed.
 */
#define VX_ATEXIT_MAX 96

namespace {

struct cxa_entry {
    void (*fn)(void *);
    void *arg;
    void *dso;
};

cxa_entry vx_atexit[VX_ATEXIT_MAX];
int       vx_atexit_n = 0;
bool      vx_finalizing = false;

}  // namespace

/*
 * The token identifying "this program's own statics".
 *
 * On a system with shared libraries each of them has its own, so that
 * unloading one runs only its destructors. There is no dynamic loader
 * here and never will be — the linker scripts produce one static image —
 * so there is exactly one of these and its value is never examined. It
 * has to *exist*, because the compiler passes its address.
 */
extern "C" { void *__dso_handle = &__dso_handle; }

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    if (!fn) return 0;
    if (vx_atexit_n >= VX_ATEXIT_MAX) {
        std::fputs_stream("libcxx: too many static destructors registered; "
                          "some will not run\n", stderr);
        return -1;
    }
    vx_atexit[vx_atexit_n].fn  = fn;
    vx_atexit[vx_atexit_n].arg = arg;
    vx_atexit[vx_atexit_n].dso = dso;
    vx_atexit_n++;
    return 0;
}

extern "C" void __cxa_finalize(void *dso) {
    /*
     * Reverse order, and re-entrancy guarded.
     *
     * A destructor is allowed to call exit(), and a second pass over a
     * table already half-run would destroy objects twice. The flag turns
     * that into a no-op, which is what every implementation does and
     * what the standard's "no more than once" requires.
     */
    if (vx_finalizing) return;
    vx_finalizing = true;

    for (int i = vx_atexit_n - 1; i >= 0; i--) {
        if (dso && vx_atexit[i].dso != dso) continue;
        void (*fn)(void *) = vx_atexit[i].fn;
        if (!fn) continue;
        vx_atexit[i].fn = nullptr;      /* before the call, not after */
        fn(vx_atexit[i].arg);
    }
    vx_finalizing = false;
}

/* ============================================================
 *  guard variables
 * ============================================================
 *
 * The eight bytes the compiler puts beside a function-local static, as
 * the Itanium C++ ABI lays them out on a little-endian machine:
 *
 *     byte 0   non-zero once the object is constructed
 *     byte 1   this implementation's "somebody is constructing it"
 *
 * Byte 0 is the one the compiler tests inline, so the common path never
 * arrives here at all. Byte 1 is ours to choose; the ABI leaves the rest
 * of the word to the implementation, and using a byte rather than a bit
 * of byte 0 keeps the compiler's own test unambiguous.
 */

namespace {

/* The futex, reached directly rather than through <pthread.h>. This
 * file is below the threading library in the same sense malloc is: a
 * lock that allocated, or that asked which thread it was on through
 * machinery that allocated, could not be used to protect the first
 * allocation a program makes. */
/*
 * The argument order is (address, operation, value), and it is written
 * out here rather than left to look obvious because it was wrong.
 *
 * This passed `op` where the kernel reads the address and the address
 * where it reads the operation. The kernel refused every call -- a
 * futex address of 0 or 1 is not four-byte aligned, which is the first
 * thing SYS_FUTEX checks -- and the refusal was invisible from up here,
 * because both callers ignore the return value.
 *
 * Nothing produced a wrong answer, which is why it survived: the wait
 * below is inside a loop that re-tests its condition, so a wait that
 * never happens is a spin rather than a hang, and the wake that never
 * happened had nobody asleep to miss it. What it cost was the entire
 * point of using a futex -- a thread waiting for another to finish
 * constructing a function-local static burned a core doing it.
 */
inline long vx_futex(uint32_t *addr, int op, uint32_t val) {
    return __syscall3(SYS_FUTEX, (long)(uintptr_t)addr, (long)op, (long)val);
}

/* One channel for every guard in the program.
 *
 * A per-guard channel would be the obvious thing and is not available:
 * the futex here keys a wait on the address of a *four-byte aligned*
 * word, and a guard variable is eight bytes at whatever alignment the
 * compiler chose. Sharing one word means a thread waiting on one static
 * is woken when any other is constructed, which costs a re-test of a
 * byte it was going to re-test anyway. Contention on this path happens
 * once per static per program run.
 */
uint32_t vx_guard_chan = 0;

}  // namespace

extern "C" int __cxa_guard_acquire(uint64_t *guard) {
    volatile uint8_t *done   = reinterpret_cast<volatile uint8_t *>(guard);
    volatile uint8_t *busy   = done + 1;

    for (;;) {
        if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) return 0;  /* already */

        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(busy, &expected, (uint8_t)1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            /* Won it. Re-test, because the winner of the exchange is not
             * necessarily the first to have looked. */
            if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(busy, (uint8_t)0, __ATOMIC_RELEASE);
                return 0;
            }
            return 1;                       /* construct it */
        }

        /*
         * Somebody else is constructing it. Park, bounded — every wait in
         * this system is bounded, because a wake that arrives between the
         * test above and the wait below is a wake that is lost, and
         * re-testing on a timer turns a hang into a delay.
         */
        vx_futex(&vx_guard_chan, FUTEX_WAIT,
                 __atomic_load_n(&vx_guard_chan, __ATOMIC_RELAXED));
    }
}

extern "C" void __cxa_guard_release(uint64_t *guard) {
    volatile uint8_t *done = reinterpret_cast<volatile uint8_t *>(guard);
    volatile uint8_t *busy = done + 1;

    /* Constructed first, then not-busy: a waiter that sees `busy` clear
     * must find `done` set, or it will construct the object a second
     * time. */
    __atomic_store_n(done, (uint8_t)1, __ATOMIC_RELEASE);
    __atomic_store_n(busy, (uint8_t)0, __ATOMIC_RELEASE);

    __atomic_add_fetch(&vx_guard_chan, 1u, __ATOMIC_RELEASE);
    vx_futex(&vx_guard_chan, FUTEX_WAKE, 0);
}

extern "C" void __cxa_guard_abort(uint64_t *guard) {
    /* The constructor did not complete. `done` stays clear, so the next
     * thread through will try again -- which is what the ABI specifies
     * and is only reachable here through a constructor that called exit,
     * since there are no exceptions to unwind. */
    volatile uint8_t *busy = reinterpret_cast<volatile uint8_t *>(guard) + 1;
    __atomic_store_n(busy, (uint8_t)0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&vx_guard_chan, 1u, __ATOMIC_RELEASE);
    vx_futex(&vx_guard_chan, FUTEX_WAKE, 0);
}

/* ============================================================
 *  the ones that mean the program is already wrong
 * ============================================================ */

[[noreturn]] static void vx_cxx_fatal(const char *what) {
    std::fputs_stream("libcxx: ", stderr);
    std::fputs_stream(what, stderr);
    std::fputs_stream("\n", stderr);
    std::abort();
}

extern "C" void __cxa_pure_virtual() {
    /* A virtual call resolved to a pure declaration, which means it was
     * made from a base's constructor or destructor while the derived
     * part did not exist. */
    vx_cxx_fatal("a pure virtual function was called");
}

extern "C" void __cxa_deleted_virtual() {
    vx_cxx_fatal("a deleted virtual function was called");
}

extern "C" void __cxa_throw_bad_array_new_length() {
    /* `new T[n]` where n * sizeof(T) overflowed. Emitted by GCC even
     * with exceptions off, which is why this is here at all. */
    vx_cxx_fatal("the length of an array being allocated is not "
                 "representable");
}

/* ============================================================
 *  std::terminate
 * ============================================================ */

namespace std {

/* Out of line so the vtable and the type information have exactly one
 * home. A polymorphic class defined entirely inline emits both into
 * every translation unit that sees it, and with -fno-rtti the type
 * information is not emitted at all -- which is fine until two
 * translation units disagree about whether it was. */
exception::~exception() {}
const char *exception::what() const noexcept { return "std::exception"; }

bad_exception::~bad_exception() {}
const char *bad_exception::what() const noexcept { return "std::bad_exception"; }

namespace {
terminate_handler vx_terminate_handler = nullptr;
}

terminate_handler set_terminate(terminate_handler h) noexcept {
    terminate_handler old = vx_terminate_handler;
    vx_terminate_handler = h;
    return old;
}

terminate_handler get_terminate() noexcept { return vx_terminate_handler; }

[[noreturn]] void terminate() noexcept {
    if (vx_terminate_handler) {
        terminate_handler h = vx_terminate_handler;
        /* Cleared before the call: a handler that itself terminates must
         * not loop through this forever. */
        vx_terminate_handler = nullptr;
        h();
    }
    vx_cxx_fatal("std::terminate was called");
}

}  // namespace std

/*
 * The C++ runtime's hook into exit().
 *
 * libc/crt0.c walks .init_array on the way in; on the way out, exit()
 * calls whatever atexit handlers were registered. This is the one that
 * runs the static destructors, and it is registered from a constructor
 * of its own so that no change to crt0 is needed and the C library stays
 * free of any knowledge that C++ exists.
 *
 * The priority puts it first in .init_array, so it is registered before
 * any other static object's constructor has had a chance to register a
 * destructor -- which is what makes the reverse ordering in
 * __cxa_finalize cover all of them.
 */
namespace {

struct vx_cxx_init {
    vx_cxx_init() { std::atexit([]() { __cxa_finalize(nullptr); }); }
};

__attribute__((init_priority(101))) vx_cxx_init vx_cxx_init_instance;

}  // namespace
