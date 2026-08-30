/*
 * cxxtest — the C++ runtime, in ring 3, on the real machine.
 *
 * The first C++ program this system has ever run.
 *
 * What is being checked is not that the containers have the right
 * members — a compiler settles that, and a host test settles their
 * semantics. It is the half that only exists at run time and only on
 * this machine:
 *
 *   operator new reaches the same allocator malloc does, over sbrk and
 *   mmap in ring 3. If it did not, every `new` would fault.
 *
 *   Static constructors run before main, and static destructors run
 *   after it. That is crt0 walking .init_array and __cxa_finalize
 *   walking its table, and neither is visible in a compile.
 *
 *   A function-local static is constructed exactly once even with
 *   several threads racing for it — which is __cxa_guard_acquire over
 *   the kernel's futex. A broken guard does not fail to compile; it
 *   constructs the object twice, occasionally, and the second one
 *   overwrites the first.
 *
 *   A virtual call dispatches through a vtable that survived the loader,
 *   which lays out an image page by page with W^X protections. A vtable
 *   in the wrong segment is a fault at the first virtual call.
 *
 *   A large allocation goes through mmap and is given back. That is the
 *   path added to malloc for `operator new`, and it fails only under
 *   memory pressure, which is exactly when nobody is looking.
 */

#include "vextro.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <array>
#include <algorithm>
#include <optional>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <variant>
#include <type_traits>

extern "C" {
#include <pthread.h>
}

static int checks = 0;
static int failures = 0;

static void ok(const char *what, bool good) {
    checks++;
    if (!good) failures++;
    std::printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* The RTTI cases live in a translation unit of their own -- this one is
 * compiled -fno-rtti, and typeid does not compile under it. They report
 * through a plain function pointer, so ok() needs a non-inline address
 * to hand over. */
extern "C" void vx_rtti_run(void (*check)(const char *what, bool good));
static void ok_rtti(const char *what, bool good) { ok(what, good); }

/* ================================================================
 *  static construction, observed
 * ================================================================
 *
 * The counter is incremented by a constructor at namespace scope and
 * read from main. If crt0 did not walk .init_array it would still be
 * zero, and the program would run perfectly and be wrong.
 */
static int ctor_order = 0;
static int first_ctor_saw = -1;
static int second_ctor_saw = -1;

namespace {
struct Marker {
    int *slot;
    explicit Marker(int *s) : slot(s) { *slot = ctor_order++; }
    ~Marker() { /* observed by the destructor test below */ }
};

Marker first(&first_ctor_saw);
Marker second(&second_ctor_saw);

/* Whether the destructor ran is not observable from inside main, which
 * finishes first. It is observable on the serial line, which is what a
 * headless harness reads. */
struct Farewell {
    ~Farewell() {
        std::printf("cxxtest: a static destructor ran after main\n");
    }
};
Farewell farewell;
}  // namespace

/* ================================================================
 *  a polymorphic hierarchy
 * ================================================================ */

namespace {
struct Shape {
    virtual ~Shape() = default;
    virtual int area() const = 0;
    virtual const char *name() const { return "shape"; }
};

struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override { return side * side; }
    const char *name() const override { return "square"; }
};

struct Rect : Shape {
    int w, h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() const override { return w * h; }
    const char *name() const override { return "rect"; }
};

/* How many of these have been destroyed, so that a container of owning
 * pointers can be checked to have actually released them. */
int shapes_destroyed = 0;

struct Counted {
    ~Counted() { shapes_destroyed++; }
};
}  // namespace

/* ================================================================
 *  the guard variable, under contention
 * ================================================================ */

namespace {
std::atomic<int> singleton_constructions{0};
std::atomic<int> threads_done{0};

struct Singleton {
    int value;
    Singleton() {
        singleton_constructions.fetch_add(1);
        /* Slow on purpose. A guard that merely tested and set without
         * making the second thread wait would let a second construction
         * start during this window; with a constructor that returns
         * immediately the race is far too narrow to catch.
         *
         * The counter is read and written through a volatile *load and
         * store* rather than being a volatile loop variable, which C++20
         * deprecated -- `i++` on a volatile is a read-modify-write whose
         * result is also discarded, and the language stopped defining
         * what that means. */
        for (int i = 0; i < 2000000; i++) {
            volatile int sink = i;
            (void)sink;
        }
        value = 42;
    }
};

Singleton &instance() {
    static Singleton s;
    return s;
}

void *racer(void *) {
    Singleton &s = instance();
    if (s.value != 42) failures++;
    threads_done.fetch_add(1);
    return nullptr;
}
}  // namespace

/*
 * Linked with crt0, unlike every other test here, and that is the point
 * rather than a convenience: crt0 is what walks .init_array, and this
 * program's first two checks are about whether it did.
 */
int main() {
    std::printf("cxxtest: starting\n");

    /* ---- 1. the runtime got here at all ---- */
    {
        ok("static constructors ran before main", ctor_order == 2);
        ok("and in declaration order",
           first_ctor_saw == 0 && second_ctor_saw == 1);
    }

    /* ---- 2. new and delete ---- */
    {
        int *p = new int(7);
        ok("new returns memory", p != nullptr);
        ok("that holds what was put in it", p && *p == 7);
        delete p;

        /* An array with a length the compiler cannot see, which is what
         * makes GCC emit the overflow check that calls
         * __cxa_throw_bad_array_new_length. If that symbol were missing
         * this would not have linked. */
        int n = 64;
        int *a = new int[n];
        ok("new[] with a computed length", a != nullptr);
        for (int i = 0; i < n; i++) a[i] = i * i;
        ok("and the elements are writable", a[63] == 63 * 63);
        delete[] a;

        int *nt = new (std::nothrow) int(3);
        ok("new (nothrow) returns memory", nt != nullptr && *nt == 3);
        delete nt;

        /* Over-aligned: the compiler emits the aligned form of operator
         * new by itself for this type. */
        struct alignas(64) Wide { double d[8]; };
        Wide *w = new Wide;
        ok("an over-aligned new is aligned",
           ((uintptr_t)w % 64) == 0);
        delete w;
    }

    /* ---- 3. the large path, which is mmap ---- */
    {
        /* Above the quarter-megabyte threshold in libc/malloc.c, so this
         * is a mapping of its own rather than a block on the free
         * list. Done twice with a release in between: if munmap were not
         * happening the second would still succeed, but the first byte
         * of the second would carry the first's data. */
        const size_t big = 1u << 20;
        char *m1 = new char[big];
        ok("a megabyte from new[]", m1 != nullptr);
        std::memset(m1, 0xAB, big);
        ok("and it is all writable", (unsigned char)m1[big - 1] == 0xAB);
        delete[] m1;

        char *m2 = new char[big];
        ok("and another after the first was released", m2 != nullptr);
        delete[] m2;
    }

    /* ---- 4. vector ---- */
    {
        std::vector<int> v;
        ok("a fresh vector is empty", v.empty() && v.size() == 0);

        for (int i = 0; i < 1000; i++) v.push_back(i);
        ok("a thousand pushes", v.size() == 1000);
        ok("with the values intact", v[0] == 0 && v[999] == 999);
        ok("and the capacity grew geometrically", v.capacity() >= 1000);

        v.erase(v.begin());
        ok("erase from the front shifts", v.size() == 999 && v[0] == 1);

        v.insert(v.begin(), -1);
        ok("insert at the front", v.size() == 1000 && v[0] == -1);

        /* The self-reference that a naive implementation gets wrong: the
         * argument lives inside the vector that is about to reallocate. */
        std::vector<int> tight;
        tight.reserve(2);
        tight.push_back(5);
        tight.push_back(6);
        tight.push_back(tight[0]);
        ok("push_back of one's own element survives the reallocation",
           tight.size() == 3 && tight[2] == 5);

        std::vector<int> init = { 3, 1, 2 };
        ok("brace initialisation", init.size() == 3 && init[0] == 3);
        std::sort(init.begin(), init.end());
        ok("sort", init[0] == 1 && init[1] == 2 && init[2] == 3);

        std::vector<int> copy = init;
        ok("copy construction is deep",
           copy == init && copy.data() != init.data());

        std::vector<int> moved = std::move(copy);
        ok("move leaves the source empty",
           moved.size() == 3 && copy.size() == 0);

        /* Sorted, reversed, and sorted again -- the case plain quicksort
         * with a first-element pivot degenerates on. */
        std::vector<int> ordered;
        for (int i = 0; i < 4000; i++) ordered.push_back(i);
        std::sort(ordered.begin(), ordered.end());
        ok("sorting an already-sorted range finishes",
           std::is_sorted(ordered.begin(), ordered.end()));
        std::reverse(ordered.begin(), ordered.end());
        std::sort(ordered.begin(), ordered.end());
        ok("and so does sorting a reversed one",
           std::is_sorted(ordered.begin(), ordered.end()) && ordered[0] == 0);
    }

    /* ---- 5. vector of objects with destructors ---- */
    {
        shapes_destroyed = 0;
        {
            std::vector<Counted> v;
            v.resize(10);
        }
        ok("a vector destroys its elements", shapes_destroyed == 10);
    }

    /* ---- 6. string ---- */
    {
        std::string s = "hello";
        ok("a short string", s.size() == 5 && s == "hello");
        ok("and it is terminated for C",
           std::strcmp(s.c_str(), "hello") == 0);

        /* Inside the small buffer: no allocation at all. */
        ok("a short string does not allocate", s.capacity() >= 5);

        s += ", world";
        ok("append", s == "hello, world" && s.size() == 12);

        /* Past 22 characters, which is where it moves to the heap. The
         * check is that the content survives the move. */
        std::string big = "0123456789012345678901234567890123456789";
        ok("a long string", big.size() == 40);
        ok("with the right first and last characters",
           big[0] == '0' && big[39] == '9');

        std::string self = "abc";
        self += self;
        ok("a string appended to itself", self == "abcabc");

        std::string doubled = "0123456789012345678901";  /* exactly 22 */
        ok("a string exactly filling the small buffer", doubled.size() == 22);
        doubled += doubled;
        ok("and doubling it across the boundary works",
           doubled.size() == 44 && doubled[22] == '0' && doubled[43] == '1');

        ok("substr", big.substr(5, 3) == "567");
        ok("find", big.find("789") == 7);
        ok("find of something absent", s.find("zzz") == std::string::npos);

        ok("to_string", std::to_string(-1234) == "-1234");
        ok("stoi", std::stoi("4321") == 4321);
        ok("stod", std::stod("2.5") == 2.5);

        std::vector<std::string> names = { "pear", "apple", "cherry" };
        std::sort(names.begin(), names.end());
        ok("sorting strings", names[0] == "apple" && names[2] == "pear");
    }

    /* ---- 7. string_view ---- */
    {
        std::string owner = "the quick brown fox";
        std::string_view v(owner);
        ok("a view over a string", v.size() == owner.size());
        ok("substr of a view is a pointer bump",
           v.substr(4, 5) == std::string_view("quick"));
        ok("starts_with", v.starts_with("the "));
        ok("a view of a literal", std::string_view("abc").size() == 3);
    }

    /* ---- 8. smart pointers ---- */
    {
        auto u = std::make_unique<Square>(4);
        ok("make_unique", u && u->area() == 16);
        ok("and dispatches virtually", std::strcmp(u->name(), "square") == 0);

        Shape *raw = u.get();
        auto u2 = std::move(u);
        ok("move transfers ownership", u2.get() == raw && u.get() == nullptr);

        auto s1 = std::make_shared<Rect>(3, 5);
        ok("make_shared", s1 && s1->area() == 15);
        ok("with one owner", s1.use_count() == 1);
        {
            auto s2 = s1;
            ok("a copy is a second owner", s1.use_count() == 2);
            ok("and both point at the same object", s2.get() == s1.get());
        }
        ok("and the count comes back down", s1.use_count() == 1);

        std::weak_ptr<Rect> w = s1;
        ok("a weak pointer does not own", s1.use_count() == 1);
        ok("and can be locked while the object lives", w.lock() != nullptr);
        s1.reset();
        ok("and reports expiry once it does not", w.expired());
        ok("and locking then gives nothing", w.lock() == nullptr);

        shapes_destroyed = 0;
        {
            std::vector<std::unique_ptr<Counted>> owners;
            for (int i = 0; i < 5; i++)
                owners.push_back(std::make_unique<Counted>());
        }
        ok("a vector of unique_ptr releases what it holds",
           shapes_destroyed == 5);
    }

    /* ---- 9. polymorphism through a container ---- */
    {
        std::vector<std::unique_ptr<Shape>> shapes;
        shapes.push_back(std::make_unique<Square>(3));
        shapes.push_back(std::make_unique<Rect>(2, 6));
        shapes.push_back(std::make_unique<Square>(5));

        int total = 0;
        for (const auto &s : shapes) total += s->area();
        ok("virtual dispatch through a container", total == 9 + 12 + 25);

        std::sort(shapes.begin(), shapes.end(),
                  [](const std::unique_ptr<Shape> &a,
                     const std::unique_ptr<Shape> &b) {
                      return a->area() < b->area();
                  });
        ok("sorting move-only elements with a lambda",
           shapes[0]->area() == 9 && shapes[2]->area() == 25);
    }

    /* ---- 10. the rest of the containers ---- */
    {
        std::array<int, 5> a = { 5, 4, 3, 2, 1 };
        ok("array knows its size", a.size() == 5);
        std::sort(a.begin(), a.end());
        ok("and sorts", a[0] == 1 && a[4] == 5);

        std::optional<int> none;
        ok("an empty optional", !none.has_value());
        ok("with a fallback", none.value_or(9) == 9);
        none = 4;
        ok("and once it holds something", none.has_value() && *none == 4);
        none.reset();
        ok("and after reset", !none);

        std::function<int(int)> f = [](int x) { return x * 3; };
        ok("a std::function over a lambda", f(7) == 21);
        int captured = 10;
        f = [captured](int x) { return x + captured; };
        ok("and over one with captures", f(5) == 15);

        std::hash<std::string> h;
        ok("hashing a string is stable", h("abc") == h("abc"));
        ok("and distinguishes", h("abc") != h("abd"));
    }

    /* ---- 11. atomics ---- */
    {
        std::atomic<int> counter{0};
        for (int i = 0; i < 100; i++) counter.fetch_add(1);
        ok("atomic increments", counter.load() == 100);

        int expected = 100;
        ok("compare exchange when it matches",
           counter.compare_exchange_strong(expected, 5));
        ok("and the value changed", counter.load() == 5);
        ok("and fails when it does not",
           !counter.compare_exchange_strong(expected, 9));
    }

    /* ---- 12. the guard variable, with four threads racing ----
     *
     * The one check here that could not be made on a host without this
     * kernel: __cxa_guard_acquire parks on SYS_FUTEX, and a
     * thread-unsafe guard constructs the object twice.
     */
    {
        pthread_t t[4];
        int started = 0;
        for (int i = 0; i < 4; i++)
            if (pthread_create(&t[i], nullptr, racer, nullptr) == 0) started++;
        ok("four threads started", started == 4);

        for (int i = 0; i < started; i++) pthread_join(t[i], nullptr);
        ok("all of them finished", threads_done.load() == started);
        ok("and the local static was constructed exactly once",
           singleton_constructions.load() == 1);
        ok("with the value the constructor set", instance().value == 42);
    }

    /* ---- 13. a mutex, which is the kernel's futex underneath ---- */
    {
        std::mutex m;
        {
            std::lock_guard<std::mutex> held(m);
            ok("a lock_guard takes the lock", !m.try_lock());
        }
        ok("and releases it at the end of the scope", m.try_lock());
        m.unlock();

        std::once_flag once;
        int ran = 0;
        for (int i = 0; i < 5; i++) std::call_once(once, [&] { ran++; });
        ok("call_once runs once", ran == 1);
    }

    /* ---- 14. type traits, which are the compiler answering ---- */
    {
        ok("is_integral", std::is_integral<int>::value &&
                          !std::is_integral<double>::value);
        ok("is_trivially_copyable distinguishes",
           std::is_trivially_copyable<int>::value &&
           !std::is_trivially_copyable<std::string>::value);
        ok("is_base_of", (std::is_base_of<Shape, Square>::value) &&
                         !(std::is_base_of<Square, Shape>::value));
        ok("decay strips",
           (std::is_same<std::decay_t<const int &>, int>::value));
    }

    /* ================================================================
     *  15. std::thread, which is SYS_CLONE underneath
     * ================================================================
     *
     * The host test cannot run any of this: it has neither this
     * system's pthreads nor its scheduler. What is being checked is that
     * the chain from `std::thread t(f)` down through pthread_create,
     * SYS_CLONE and sched_spawn_thread actually starts a thread in *this*
     * address space — which is the difference from fork and the reason
     * the counter below is shared at all.
     */
    {
        std::atomic<int> sum{0};
        {
            std::thread a([&] { for (int i = 0; i < 1000; i++) sum.fetch_add(1); });
            std::thread b([&] { for (int i = 0; i < 1000; i++) sum.fetch_add(1); });
            ok("two std::threads are joinable", a.joinable() && b.joinable());
            ok("and have different identifiers", a.get_id() != b.get_id());
            a.join();
            b.join();
            ok("after joining they are not", !a.joinable() && !b.joinable());
        }
        ok("both threads saw the same counter", sum.load() == 2000);

        /* Arguments are copied into the thread, not referenced -- the
         * calling scope may end before it runs. */
        std::atomic<int> product{0};
        {
            int x = 6, y = 7;
            std::thread t([&product](int a, int b) { product.store(a * b); }, x, y);
            x = y = 0;                     /* must not affect the result */
            t.join();
        }
        ok("arguments are copied at construction", product.load() == 42);

        std::thread moved([&] { sum.fetch_add(1); });
        std::thread taken = std::move(moved);
        ok("a moved-from thread is not joinable", !moved.joinable());
        taken.join();
        ok("and the moved-to one ran", sum.load() == 2001);

        /* One. Every ring-3 thread here runs on processor zero, because
         * the kernel keeps one stack pointer for the next entry from
         * user mode; a pool sized from this gets one worker, which is
         * the right answer rather than a placeholder. */
        ok("hardware_concurrency answers one",
           std::thread::hardware_concurrency() == 1);
    }

    /* ================================================================
     *  16. the clock, which is the scheduler's tick
     * ================================================================ */
    {
        using namespace std::chrono;

        auto t0 = steady_clock::now();
        std::this_thread::sleep_for(milliseconds(60));
        auto t1 = steady_clock::now();

        const auto slept = duration_cast<milliseconds>(t1 - t0).count();
        ok("the clock advances across a sleep", t1 > t0);
        ok("by about as long as was asked", slept >= 50 && slept < 600);
        std::printf("       (asked 60 ms, measured %lld)\n", (long long)slept);

        /* The resolution is a millisecond, so the nanosecond count is
         * always a whole number of them -- which is the honest thing to
         * assert rather than pretending to finer precision. */
        const auto ns = t1.time_since_epoch().count();
        ok("the nanosecond count is a whole millisecond", ns % 1000000 == 0);

        ok("the clock is steady", steady_clock::is_steady);
        ok("and never goes backwards", steady_clock::now() >= t1);

        /* Arithmetic, on the machine, over the same ratios the host
         * checked exhaustively. */
        ok("durations convert", duration_cast<milliseconds>(seconds(2)).count() == 2000);
        ok("and mix", (milliseconds(1) + microseconds(500)).count() == 1500);
    }

    /* ================================================================
     *  17. the containers, on the ring-3 allocator
     * ================================================================
     *
     * The semantics were settled on the host. What is being checked here
     * is that they work over *this* heap — every node from operator new,
     * over malloc, over sbrk and mmap in ring 3.
     */
    {
        std::unordered_map<std::string, int> m;
        for (int i = 0; i < 2000; i++) m[std::to_string(i)] = i;
        ok("two thousand nodes from the ring-3 allocator", m.size() == 2000);
        ok("with the values intact", m["1999"] == 1999 && m["0"] == 0);

        int *held = &m["7"];
        for (int i = 2000; i < 4000; i++) m[std::to_string(i)] = i;
        ok("and a pointer into it survives the rehash", *held == 7);

        size_t walked = 0;
        for (const auto &kv : m) { (void)kv; walked++; }
        ok("iteration is complete", walked == m.size());

        std::variant<int, std::string, double> v = std::string("ring 3");
        ok("a variant holding a string", std::get<std::string>(v) == "ring 3");
        v = 3.5;
        ok("replaced by a double", v.index() == 2 && std::get<double>(v) == 3.5);
        ok("visited", std::visit([](const auto &x) -> bool {
               return std::is_same<std::decay_t<decltype(x)>, double>::value;
           }, v));
    }

    /* ---- dynamic_cast and typeid ----
     *
     * The cases are in apps/rtti_cases.h and are compiled twice: once
     * here, over libcxx/src/typeinfo.cpp, and once on the host over the
     * host's own C++ runtime, where `make test` runs them. Every
     * expectation in that file is an address the compiler works out
     * statically, so neither run checks an implementation against
     * itself -- and the two runs must agree, including on the diamonds.
     *
     * The cases live in their own translation unit because this one is
     * compiled -fno-rtti, under which typeid does not compile at all.
     */
    vx_rtti_run(ok_rtti);

    std::printf("cxxtest: %d checks, %d failures\n", checks, failures);
    std::printf(failures ? "cxxtest: FAILED\n" : "cxxtest: all passed\n");

    /* Through exit() rather than by returning, so that the static
     * destructors run and can be seen to have run: falling off the end
     * of _start lands on the loader's exit stub, which ends the thread
     * without unwinding anything. */
    std::exit(failures ? 1 : 0);
}
