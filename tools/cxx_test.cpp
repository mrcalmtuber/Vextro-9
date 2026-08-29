/*
 * tools/cxx_test.cpp — the C++ containers, checked on the host.
 *
 * The same headers the target builds against, compiled by the host's
 * compiler and run here. That buys two things the ring-3 test cannot:
 *
 *   A failure is a failure of the *library*, not of the machine. The
 *   containers are pure computation over memory; if a vector's insert
 *   shifts by one too many, that is wrong here and wrong there, and
 *   finding out here takes a second rather than a boot.
 *
 *   The cases nobody would put in a boot test. Growth across every
 *   reallocation boundary, a string at exactly the small-buffer limit,
 *   ten thousand insertions and erasures interleaved -- the kind of
 *   volume that would make a self-test noticeably slower on every boot
 *   and costs nothing on a workstation.
 *
 * What it cannot check is everything that is not computation: operator
 * new reaching the ring-3 allocator, static constructors, guard
 * variables under real threads, a vtable surviving the loader. That is
 * apps/cxxtest.cpp, on the machine, and the two are complementary
 * rather than redundant.
 *
 * <cmath>, <mutex> and <thread> are deliberately not included. The first
 * would collide with the host's own float overloads; the other two want
 * the pthreads in libc/, which is not this machine's. All three are
 * covered on the target instead.
 *
 * <chrono> *is* here, because its arithmetic is pure computation over
 * ratios and only `now()` touches the clock — and clock_gettime exists
 * on both. The duration algebra is exactly the kind of thing that is
 * cheap to check exhaustively here and expensive to check on a boot.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <limits>
#include <iterator>
#include <algorithm>
#include <memory>
#include <atomic>
#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <ratio>
#include <chrono>
#include <unordered_map>
#include <variant>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, bool good) {
    checks++;
    if (!good) {
        failures++;
        std::printf("  FAIL %s\n", what);
    }
}

/* An element that counts its own construction and destruction, which is
 * how a container is caught leaking or double-freeing without a
 * sanitiser. */
static int live = 0;
static int ctors = 0;
static int dtors = 0;

struct Tracked {
    int v;
    Tracked() : v(0) { live++; ctors++; }
    explicit Tracked(int x) : v(x) { live++; ctors++; }
    Tracked(const Tracked &o) : v(o.v) { live++; ctors++; }
    Tracked(Tracked &&o) noexcept : v(o.v) { o.v = -1; live++; ctors++; }
    Tracked &operator=(const Tracked &o) { v = o.v; return *this; }
    Tracked &operator=(Tracked &&o) noexcept { v = o.v; o.v = -1; return *this; }
    ~Tracked() { live--; dtors++; }
    bool operator==(const Tracked &o) const { return v == o.v; }
    bool operator<(const Tracked &o) const { return v < o.v; }
};

static void test_vector() {
    /* ---- growth, across every boundary ---- */
    {
        std::vector<int> v;
        for (int i = 0; i < 5000; i++) {
            v.push_back(i);
            if (v.size() != (size_t)i + 1) { ok("vector size tracks pushes", false); return; }
            if (v[(size_t)i] != i) { ok("vector keeps what was pushed", false); return; }
            if (v[0] != 0) { ok("vector keeps the first element across growth", false); return; }
        }
        ok("five thousand pushes keep every element", true);
        ok("and the capacity is at least the size", v.capacity() >= v.size());
    }

    /* ---- insert and erase, interleaved ---- */
    {
        std::vector<int> v;
        for (int i = 0; i < 100; i++) v.push_back(i);

        v.insert(v.begin(), -1);
        ok("insert at the front", v.size() == 101 && v[0] == -1 && v[1] == 0);

        v.insert(v.end(), 999);
        ok("insert at the end", v.size() == 102 && v.back() == 999);

        v.insert(v.begin() + 50, 555);
        ok("insert in the middle", v[50] == 555 && v[51] == 49);

        /* 100 pushed, then three inserted: 103. */
        ok("three inserts make it 103", v.size() == 103);

        v.erase(v.begin());
        ok("erase from the front", v.size() == 102 && v[0] == 0);

        v.erase(v.end() - 1);
        ok("erase from the end", v.size() == 101 && v.back() == 99);

        std::vector<int> w = { 1, 2, 3, 4, 5, 6 };
        w.erase(w.begin() + 1, w.begin() + 4);
        ok("erase a range", w.size() == 3 && w[0] == 1 && w[1] == 5 && w[2] == 6);
    }

    /* ---- the self-referential push ---- */
    {
        std::vector<int> v;
        v.reserve(2);
        v.push_back(11);
        v.push_back(22);
        v.push_back(v[0]);            /* reallocates, and v[0] is inside */
        ok("push_back of one's own element", v.size() == 3 && v[2] == 11);

        std::vector<int> u;
        u.reserve(2);
        u.push_back(7);
        u.push_back(8);
        u.insert(u.begin(), u[1]);    /* likewise, through insert */
        ok("insert of one's own element", u.size() == 3 && u[0] == 8);
    }

    /* ---- nothing leaks and nothing is destroyed twice ---- */
    {
        ctors = dtors = live = 0;
        {
            std::vector<Tracked> v;
            for (int i = 0; i < 200; i++) v.emplace_back(i);
            ok("emplace_back constructs", v.size() == 200 && v[199].v == 199);

            std::vector<Tracked> copy = v;
            ok("copy construction duplicates", copy.size() == 200);

            v.erase(v.begin(), v.begin() + 100);
            ok("erasing a range destroys what it removed", v.size() == 100);

            v.resize(10);
            ok("shrinking destroys the tail", v.size() == 10);

            v.clear();
            ok("clear destroys everything", v.empty());
        }
        ok("every element constructed was destroyed", live == 0);
        ok("and the counts agree", ctors == dtors);
    }

    /* ---- move semantics ---- */
    {
        std::vector<int> a = { 1, 2, 3 };
        const int *before = a.data();
        std::vector<int> b = std::move(a);
        ok("a moved vector takes the storage", b.data() == before);
        ok("and the source is empty", a.size() == 0 && a.data() == nullptr);
    }
}

static void test_string() {
    /* ---- every length across the small-buffer boundary ---- */
    {
        for (size_t n = 0; n < 200; n++) {
            std::string s;
            for (size_t i = 0; i < n; i++) s.push_back((char)('a' + (i % 26)));
            if (s.size() != n) { ok("string length across the boundary", false); return; }
            if (std::strlen(s.c_str()) != n) {
                ok("string stays terminated across the boundary", false);
                return;
            }
            for (size_t i = 0; i < n; i++)
                if (s[i] != (char)('a' + (i % 26))) {
                    ok("string content across the boundary", false);
                    return;
                }
        }
        ok("every length from 0 to 199 is stored and terminated", true);
    }

    /* ---- self-append at every length ---- */
    {
        for (size_t n = 1; n < 60; n++) {
            std::string s(n, 'x');
            s += s;
            if (s.size() != n * 2) { ok("self-append length", false); return; }
            for (size_t i = 0; i < n * 2; i++)
                if (s[i] != 'x') { ok("self-append content", false); return; }
        }
        ok("appending a string to itself works at every length", true);
    }

    /* ---- the operations ---- */
    {
        std::string s = "hello, world";
        ok("substr", s.substr(7) == "world");
        ok("substr with a length", s.substr(0, 5) == "hello");
        ok("substr past the end is empty", s.substr(100) == "");
        ok("find", s.find("world") == 7);
        ok("find of a character", s.find(',') == 5);
        ok("find that fails", s.find("zebra") == std::string::npos);
        ok("rfind", s.rfind('o') == 8);
        ok("starts_with", s.starts_with("hello"));
        ok("ends_with", s.ends_with("world"));

        s.insert(5, "!!!");
        ok("insert", s == "hello!!!, world");

        s.erase(5, 3);
        ok("erase", s == "hello, world");

        std::string t = s;
        ok("copy is equal", t == s);
        t.push_back('!');
        ok("and independent", t != s && s == "hello, world");

        std::string m = std::move(t);
        ok("move takes the content", m == "hello, world!");
        ok("and leaves an empty string", t.empty());

        ok("concatenation", (std::string("ab") + "cd" + 'e') == "abcde");
        ok("comparison orders", std::string("abc") < std::string("abd"));
    }

    /* ---- a long string moved and swapped ---- */
    {
        std::string big(500, 'q');
        std::string small = "tiny";
        big.swap(small);
        ok("swap exchanges a heap string with a small one",
           big == "tiny" && small.size() == 500 && small[499] == 'q');
    }

    /* ---- numbers ---- */
    {
        ok("to_string of a negative", std::to_string(-42) == "-42");
        ok("to_string of a large unsigned",
           std::to_string(4000000000u) == "4000000000");
        ok("stoi", std::stoi("123") == 123);
        ok("stol of a negative", std::stol("-9999") == -9999);
        ok("stod", std::stod("0.5") == 0.5);
    }

    /* ---- sorting a container of strings ---- */
    {
        std::vector<std::string> v = { "delta", "alpha", "charlie", "bravo" };
        std::sort(v.begin(), v.end());
        ok("strings sort", v[0] == "alpha" && v[3] == "delta");
    }
}

static void test_algorithm() {
    /* ---- sort, on the inputs that break a naive quicksort ---- */
    {
        const int N = 20000;
        std::vector<int> v;

        v.clear();
        for (int i = 0; i < N; i++) v.push_back(i);
        std::sort(v.begin(), v.end());
        ok("sorting an already-sorted range", std::is_sorted(v.begin(), v.end()));

        v.clear();
        for (int i = N; i > 0; i--) v.push_back(i);
        std::sort(v.begin(), v.end());
        ok("sorting a reversed range", std::is_sorted(v.begin(), v.end()));

        v.clear();
        for (int i = 0; i < N; i++) v.push_back(0);
        std::sort(v.begin(), v.end());
        ok("sorting all-equal elements", std::is_sorted(v.begin(), v.end()));

        v.clear();
        unsigned seed = 12345;
        for (int i = 0; i < N; i++) {
            seed = seed * 1664525u + 1013904223u;
            v.push_back((int)(seed >> 8));
        }
        std::sort(v.begin(), v.end());
        ok("sorting a random range", std::is_sorted(v.begin(), v.end()));
        ok("and keeping every element", v.size() == (size_t)N);

        /* Organ pipe: up then down, which is the shape a median-of-three
         * pivot handles and a first-element pivot does not. */
        v.clear();
        for (int i = 0; i < N / 2; i++) v.push_back(i);
        for (int i = N / 2; i > 0; i--) v.push_back(i);
        std::sort(v.begin(), v.end());
        ok("sorting an organ-pipe range", std::is_sorted(v.begin(), v.end()));
    }

    /* ---- stable_sort really is stable ---- */
    {
        struct Item { int key; int seq; };
        std::vector<Item> v;
        for (int i = 0; i < 500; i++) v.push_back({ i % 7, i });
        std::stable_sort(v.begin(), v.end(),
                         [](const Item &a, const Item &b) { return a.key < b.key; });

        bool stable = true;
        for (size_t i = 1; i < v.size(); i++)
            if (v[i].key == v[i - 1].key && v[i].seq < v[i - 1].seq) stable = false;
        ok("stable_sort keeps equal elements in order", stable);
    }

    /* ---- the searches ---- */
    {
        std::vector<int> v = { 1, 3, 5, 7, 9, 11 };
        ok("lower_bound finds", std::lower_bound(v.begin(), v.end(), 7) == v.begin() + 3);
        ok("lower_bound between", std::lower_bound(v.begin(), v.end(), 6) == v.begin() + 3);
        ok("upper_bound", std::upper_bound(v.begin(), v.end(), 7) == v.begin() + 4);
        ok("binary_search finds", std::binary_search(v.begin(), v.end(), 9));
        ok("and does not find", !std::binary_search(v.begin(), v.end(), 8));

        ok("find", *std::find(v.begin(), v.end(), 5) == 5);
        ok("find_if", *std::find_if(v.begin(), v.end(),
                                    [](int x) { return x > 6; }) == 7);
        ok("count_if", std::count_if(v.begin(), v.end(),
                                     [](int x) { return x % 3 == 0; }) == 2);
        ok("max_element", *std::max_element(v.begin(), v.end()) == 11);
        ok("min_element", *std::min_element(v.begin(), v.end()) == 1);
        ok("all_of", std::all_of(v.begin(), v.end(), [](int x) { return x % 2; }));
        ok("none_of", std::none_of(v.begin(), v.end(), [](int x) { return x == 4; }));
    }

    /* ---- copying and moving ranges ---- */
    {
        std::vector<int> src = { 1, 2, 3, 4, 5 };
        std::vector<int> dst(5, 0);
        std::copy(src.begin(), src.end(), dst.begin());
        ok("copy", dst == src);

        std::reverse(dst.begin(), dst.end());
        ok("reverse", dst[0] == 5 && dst[4] == 1);

        std::rotate(dst.begin(), dst.begin() + 2, dst.end());
        ok("rotate", dst[0] == 3);

        std::fill(dst.begin(), dst.end(), 7);
        ok("fill", std::count(dst.begin(), dst.end(), 7) == 5);

        std::vector<int> out;
        std::transform(src.begin(), src.end(), std::back_inserter(out),
                       [](int x) { return x * 2; });
        ok("transform into a back_inserter",
           out.size() == 5 && out[4] == 10);
    }
}

static void test_memory() {
    {
        ctors = dtors = live = 0;
        {
            auto p = std::make_unique<Tracked>(5);
            ok("make_unique constructs one", live == 1 && p->v == 5);
        }
        ok("and destroys it at the end of the scope", live == 0);
    }

    {
        ctors = dtors = live = 0;
        {
            auto s = std::make_shared<Tracked>(9);
            ok("make_shared constructs one", live == 1);
            ok("with one owner", s.use_count() == 1);
            {
                auto t = s;
                auto u = t;
                ok("copies are owners", s.use_count() == 3);
            }
            ok("and the count comes back", s.use_count() == 1);
            ok("with the object still alive", live == 1);

            std::weak_ptr<Tracked> w = s;
            ok("a weak reference does not own", s.use_count() == 1);
            ok("and locks while it lives", w.lock() != nullptr);
            s.reset();
            ok("expires when the last owner goes", w.expired());
            ok("and the object is destroyed", live == 0);
            ok("and locking gives nothing", w.lock() == nullptr);
        }
    }

    /* A cycle broken by a weak reference: two objects pointing at each
     * other, one of them weakly, is the case shared_ptr alone leaks. */
    {
        ctors = dtors = live = 0;
        struct Node {
            std::shared_ptr<Node> next;
            std::weak_ptr<Node>   prev;
            Tracked t;
        };
        {
            auto a = std::make_shared<Node>();
            auto b = std::make_shared<Node>();
            a->next = b;
            b->prev = a;
        }
        ok("a cycle broken by a weak pointer does not leak", live == 0);
    }

    {
        std::vector<std::unique_ptr<Tracked>> v;
        ctors = dtors = live = 0;
        for (int i = 0; i < 100; i++) v.push_back(std::make_unique<Tracked>(i));
        ok("a hundred owned objects", live == 100);
        v.clear();
        ok("released by the container", live == 0);
    }
}

static void test_misc() {
    {
        std::array<int, 4> a = { 4, 3, 2, 1 };
        ok("array size is a constant", a.size() == 4);
        std::sort(a.begin(), a.end());
        ok("array sorts", a[0] == 1 && a[3] == 4);
        ok("array data is contiguous", &a[1] == &a[0] + 1);
    }

    {
        std::optional<std::string> o;
        ok("an empty optional", !o.has_value());
        ok("value_or", o.value_or("fallback") == "fallback");
        o = std::string("here");
        ok("once engaged", o.has_value() && *o == "here");

        ctors = dtors = live = 0;
        {
            std::optional<Tracked> t;
            ok("an empty optional constructs nothing", live == 0);
            t.emplace(3);
            ok("emplace constructs", live == 1);
            t.reset();
            ok("reset destroys", live == 0);
        }
    }

    {
        std::string_view v = "the quick brown fox";
        ok("view size", v.size() == 19);
        ok("view substr", v.substr(4, 5) == std::string_view("quick"));
        ok("view find", v.find("brown") == 10);
        ok("view starts_with", v.starts_with("the"));
        ok("view of a std::string", std::string_view(std::string("abc")).size() == 3);
    }

    {
        std::function<int(int, int)> f = [](int a, int b) { return a + b; };
        ok("std::function", f(2, 3) == 5);
        int base = 100;
        f = [base](int a, int b) { return base + a + b; };
        ok("with a capture", f(2, 3) == 105);
        std::function<int(int, int)> g = f;
        ok("copied", g(1, 1) == 102);
        ok("truthiness", (bool)g && !std::function<void()>());

        std::hash<std::string> h;
        ok("hash is stable", h("abc") == h("abc"));
        ok("and spreads", h("abc") != h("abd") && h("") != h("a"));
    }

    {
        std::atomic<int> a{0};
        for (int i = 0; i < 1000; i++) a.fetch_add(1);
        ok("atomic add", a.load() == 1000);
        int want = 1000;
        ok("compare exchange", a.compare_exchange_strong(want, 0) && a.load() == 0);
    }

    {
        ok("numeric_limits<int>::max", std::numeric_limits<int>::max() == 2147483647);
        ok("numeric_limits<unsigned char>::max",
           std::numeric_limits<unsigned char>::max() == 255);
        ok("numeric_limits<double> has infinity",
           std::numeric_limits<double>::has_infinity);
        ok("is_signed", std::numeric_limits<int>::is_signed &&
                        !std::numeric_limits<unsigned>::is_signed);
    }

    {
        ok("is_same", (std::is_same<int, int>::value) &&
                      !(std::is_same<int, long>::value));
        ok("remove_cv", (std::is_same<std::remove_cv_t<const volatile int>,
                                      int>::value));
        ok("decay of an array",
           (std::is_same<std::decay_t<int[5]>, int *>::value));
        ok("is_trivially_copyable",
           std::is_trivially_copyable<int>::value &&
           !std::is_trivially_copyable<std::string>::value);
        ok("conditional",
           (std::is_same<std::conditional_t<true, int, long>, int>::value));

        std::pair<int, std::string> p = std::make_pair(1, std::string("one"));
        ok("pair", p.first == 1 && p.second == "one");

        int x = 1, y = 2;
        std::swap(x, y);
        ok("swap", x == 2 && y == 1);
        ok("exchange", std::exchange(x, 9) == 2 && x == 9);
    }
}

static void test_chrono() {
    using namespace std::chrono;

    /* ---- conversions, in both directions ---- */
    {
        ok("seconds to milliseconds is exact",
           duration_cast<milliseconds>(seconds(3)).count() == 3000);
        ok("milliseconds to seconds truncates",
           duration_cast<seconds>(milliseconds(3999)).count() == 3);
        ok("and truncates toward zero for negatives",
           duration_cast<seconds>(milliseconds(-3999)).count() == -3);
        ok("hours to nanoseconds does not overflow",
           duration_cast<nanoseconds>(hours(2)).count() == 7200000000000LL);
        ok("minutes to seconds", duration_cast<seconds>(minutes(5)).count() == 300);
    }

    /* ---- the implicit widening, and the conversion that is refused ---- */
    {
        nanoseconds n = milliseconds(7);          /* widening: implicit */
        ok("a coarser duration converts implicitly", n.count() == 7000000);
        static_assert(!std::is_convertible<nanoseconds, milliseconds>::value,
                      "a lossy conversion must not be implicit");
        ok("and a lossy one does not", true);
    }

    /* ---- mixed arithmetic lands on the finer period ---- */
    {
        auto sum = milliseconds(2) + microseconds(500);
        static_assert(std::is_same<decltype(sum), microseconds>::value,
                      "the common type of ms and us is us");
        ok("adding across periods uses the finer one", sum.count() == 2500);

        auto diff = seconds(1) - milliseconds(250);
        ok("subtracting across periods", diff.count() == 750);

        ok("duration divided by duration is a count",
           minutes(3) / seconds(30) == 6);
        ok("duration times a scalar", (milliseconds(5) * 4).count() == 20);
        ok("a scalar times a duration", (3 * seconds(2)).count() == 6);
        ok("modulo", (milliseconds(2500) % seconds(1)).count() == 500);
    }

    /* ---- comparison across periods ---- */
    {
        ok("equal across periods", seconds(1) == milliseconds(1000));
        ok("less across periods", milliseconds(999) < seconds(1));
        ok("greater across periods", minutes(1) > seconds(59));
        ok("not equal", seconds(1) != milliseconds(1001));
    }

    /* ---- time points ---- */
    {
        steady_clock::time_point base{};
        auto later = base + milliseconds(1500);
        ok("a point plus a duration", (later - base).count() == 1500000000);
        ok("and the difference is a duration",
           duration_cast<milliseconds>(later - base).count() == 1500);
        ok("points order", base < later);

        auto rounded = time_point_cast<seconds>(later);
        ok("time_point_cast truncates", rounded.time_since_epoch().count() == 1);
    }

    /* ---- the clock moves forward and never back ---- */
    {
        auto a = steady_clock::now();
        long long spins = 0;
        for (int i = 0; i < 2000000; i++) spins += i;
        auto b = steady_clock::now();
        ok("the steady clock does not go backwards", b >= a);
        ok("and it is steady", steady_clock::is_steady);
        (void)spins;
    }

    /* ---- ratio, which nobody asks for and chrono cannot do without ---- */
    {
        ok("a ratio reduces", std::ratio<4, 8>::num == 1 && std::ratio<4, 8>::den == 2);
        ok("the sign lands on the numerator",
           std::ratio<1, -2>::num == -1 && std::ratio<1, -2>::den == 2);
        ok("multiply reduces to identity",
           (std::is_same<std::ratio_multiply<std::milli, std::kilo>,
                         std::ratio<1, 1>>::value));
        ok("divide", std::ratio_divide<std::milli, std::nano>::num == 1000000);
        ok("add", (std::ratio_add<std::ratio<1,2>, std::ratio<1,3>>::num == 5 &&
                   std::ratio_add<std::ratio<1,2>, std::ratio<1,3>>::den == 6));
        ok("compare", (std::ratio_less<std::ratio<1,3>, std::ratio<1,2>>::value));
    }

    /* ---- the literals ---- */
    {
        using namespace std::chrono_literals;
        ok("ms literal", (250ms).count() == 250);
        ok("s literal", (2s).count() == 2);
        ok("h literal", (3h).count() == 3);
        ok("literals mix with arithmetic", (1s + 500ms).count() == 1500);
    }
}

static void test_unordered_map() {
    /* ---- volume, and every rehash boundary crossed ---- */
    {
        std::unordered_map<int, int> m;
        for (int i = 0; i < 20000; i++) {
            m[i] = i * 3;
            if (m.size() != (size_t)i + 1) { ok("map grows", false); return; }
        }
        ok("twenty thousand insertions", m.size() == 20000);

        bool all = true;
        for (int i = 0; i < 20000; i++) if (m[i] != i * 3) all = false;
        ok("every value survived the rehashes", all);

        size_t walked = 0;
        for (const auto &kv : m) { (void)kv; walked++; }
        ok("iteration visits each element exactly once", walked == m.size());
    }

    /* ---- the guarantee chaining exists for ---- */
    {
        std::unordered_map<int, int> m;
        m[1] = 100;
        int *held = &m[1];
        for (int i = 2; i < 5000; i++) m[i] = i;
        ok("a pointer into the map survives rehashing", *held == 100);
        ok("and still refers to the same element", held == &m[1]);
    }

    /* ---- string keys ---- */
    {
        std::unordered_map<std::string, int> counts;
        const char *words[] = { "the", "quick", "brown", "the", "fox", "the" };
        for (const char *w : words) counts[w]++;
        ok("counting with operator[]", counts["the"] == 3);
        ok("a key seen once", counts["fox"] == 1);
        ok("size counts distinct keys", counts.size() == 4);
        ok("contains", counts.contains("brown") && !counts.contains("zebra"));
        ok("find", counts.find("quick")->second == 1);
        ok("find of a missing key", counts.find("zebra") == counts.end());
        ok("at", counts.at("the") == 3);
    }

    /* ---- erase ---- */
    {
        std::unordered_map<int, int> m;
        for (int i = 0; i < 1000; i++) m[i] = i;
        for (int i = 0; i < 1000; i += 2) m.erase(i);
        ok("erasing every other one", m.size() == 500);
        ok("the odd ones remain", m.contains(999) && !m.contains(998));
        ok("erasing what is not there answers zero", m.erase(998) == 0);
        m.clear();
        ok("clear", m.empty() && m.size() == 0);
    }

    /* ---- elements with destructors ---- */
    {
        ctors = dtors = live = 0;
        {
            std::unordered_map<int, Tracked> m;
            for (int i = 0; i < 500; i++) m.emplace(i, Tracked(i));
            ok("emplace constructs", m.size() == 500);
            m.erase(3);
            ok("erase destroys what it removed", m.size() == 499);
        }
        ok("and the map destroys the rest", live == 0);
    }

    /* ---- copy, move, compare ---- */
    {
        std::unordered_map<int, int> a;
        for (int i = 0; i < 100; i++) a[i] = i;
        std::unordered_map<int, int> b = a;
        ok("copy is equal", a == b);
        b[500] = 500;
        ok("and independent", !(a == b) && a.size() == 100);

        std::unordered_map<int, int> c = std::move(b);
        ok("move takes the contents", c.size() == 101);
        ok("and empties the source", b.size() == 0);
    }

    /* ---- the set ---- */
    {
        std::unordered_set<std::string> s;
        ok("first insert reports true", s.insert("a"));
        ok("second reports false", !s.insert("a"));
        s.insert("b");
        ok("size counts distinct", s.size() == 2);
        ok("contains", s.contains("b") && !s.contains("c"));
        size_t n = 0;
        for (const auto &k : s) { (void)k; n++; }
        ok("iterates its keys", n == 2);
    }
}

static void test_variant() {
    using V = std::variant<int, std::string, double>;

    /* ---- which alternative is live ---- */
    {
        V v;
        ok("a fresh variant holds the first alternative", v.index() == 0);
        ok("and it is value-initialised", std::get<int>(v) == 0);
        ok("valueless is false and stays false", !v.valueless_by_exception());

        v = 42;
        ok("assigning an int selects it", v.index() == 0 && std::get<0>(v) == 42);
        ok("holds_alternative agrees", std::holds_alternative<int>(v));

        v = std::string("hello");
        ok("assigning a string selects it", v.index() == 1);
        ok("and the value is right", std::get<std::string>(v) == "hello");
        ok("holds_alternative for the old one is false",
           !std::holds_alternative<int>(v));

        v = 1.5;
        ok("and a double", v.index() == 2 && std::get<double>(v) == 1.5);
    }

    /* ---- the conversion that picks an alternative ---- */
    {
        V v = "a literal";      /* const char* converts uniquely to string */
        ok("a unique conversion selects that alternative", v.index() == 1);
        ok("and converts", std::get<std::string>(v) == "a literal");
    }

    /* ---- get_if answers rather than stopping ---- */
    {
        V v = 7;
        ok("get_if on the live alternative", std::get_if<int>(&v) != nullptr);
        ok("and its value", *std::get_if<int>(&v) == 7);
        ok("get_if on another answers null", std::get_if<std::string>(&v) == nullptr);
        ok("get_if by index", *std::get_if<0>(&v) == 7);
        ok("get_if on a null variant", std::get_if<0>((V *)nullptr) == nullptr);
    }

    /* ---- in_place, which is how an alternative is built rather than copied ---- */
    {
        V v(std::in_place_index<1>, 4, 'z');
        ok("in_place_index constructs in place", std::get<1>(v) == "zzzz");

        V w(std::in_place_type<std::string>, "direct");
        ok("in_place_type", std::get<std::string>(w) == "direct");

        w.emplace<int>(9);
        ok("emplace replaces the alternative", w.index() == 0 && std::get<0>(w) == 9);
    }

    /* ---- visiting ---- */
    {
        auto describe = [](const auto &x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same<T, int>::value) return "int";
            else if constexpr (std::is_same<T, std::string>::value) return "string";
            else return "double";
        };
        V a = 1, b = std::string("s"), c = 2.0;
        ok("visit dispatches on the live alternative",
           std::visit(describe, a) == "int");
        ok("and again", std::visit(describe, b) == "string");
        ok("and again", std::visit(describe, c) == "double");

        /* A visitor that returns a value derived from the alternative,
         * which is the case that catches a dispatch table built in the
         * wrong order. */
        V n = 21;
        int doubled = std::visit([](const auto &x) -> int {
            if constexpr (std::is_same<std::decay_t<decltype(x)>, int>::value)
                return x * 2;
            else return -1;
        }, n);
        ok("a visitor's return value comes back", doubled == 42);
    }

    /* ---- copying, moving and comparing ---- */
    {
        V a = std::string("shared");
        V b = a;
        ok("copy", std::get<std::string>(b) == "shared");
        ok("and they compare equal", a == b);

        V c = std::move(b);
        ok("move", std::get<std::string>(c) == "shared");

        V i = 1, j = 2;
        ok("same alternative compares by value", i < j);
        V s = std::string("");
        ok("different alternatives compare by index", i < s);
        ok("not equal across alternatives", !(i == s));
    }

    /* ---- destructors run, including on replacement ---- */
    {
        ctors = dtors = live = 0;
        {
            std::variant<int, Tracked> v;
            ok("holding the int constructs no Tracked", live == 0);
            v.emplace<Tracked>(1);
            ok("emplacing constructs one", live == 1);
            v = 5;                         /* replaces: must destroy it */
            ok("replacing the alternative destroys the old one", live == 0);
            v.emplace<Tracked>(2);
        }
        ok("and going out of scope destroys the last", live == 0);
        ok("with the counts agreeing", ctors == dtors);
    }

    /* ---- variants in a container ---- */
    {
        std::vector<V> vs;
        for (int i = 0; i < 500; i++) {
            if (i % 3 == 0)      vs.push_back(V(i));
            else if (i % 3 == 1) vs.push_back(V(std::string("x")));
            else                 vs.push_back(V((double)i));
        }
        size_t ints = 0, strs = 0, dbls = 0;
        for (const auto &e : vs) {
            if (e.index() == 0) ints++;
            else if (e.index() == 1) strs++;
            else dbls++;
        }
        ok("a vector of variants keeps its alternatives",
           ints + strs + dbls == 500 && strs > 0 && ints > 0 && dbls > 0);
    }

    /* ---- monostate, and the sizes ---- */
    {
        std::variant<std::monostate, std::string> v;
        ok("monostate is default constructible", v.index() == 0);
        ok("variant_size", std::variant_size_v<V> == 3);
        ok("variant_alternative",
           (std::is_same<std::variant_alternative_t<1, V>, std::string>::value));
    }
}

int main() {
    test_vector();
    test_string();
    test_algorithm();
    test_memory();
    test_misc();
    test_chrono();
    test_unordered_map();
    test_variant();

    std::printf("  ok   libcxx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
