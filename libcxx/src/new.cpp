/*
 * libcxx/src/new.cpp — operator new and operator delete.
 *
 * Every allocation a C++ program makes that is not an explicit malloc
 * comes through here: every `new`, every container that grows, every
 * std::string longer than its small buffer.
 *
 * The whole file is twenty lines of substance and a great deal of
 * signature, and that ratio is the point — the compiler picks between
 * twelve overloads on its own, by rules that depend on the type being
 * allocated, and a runtime that implements eleven of them fails at link
 * time with a mangled name rather than at compile time with a sentence.
 */

#include <new>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace std {

const nothrow_t nothrow{};

/* Out of line so that the vtable and the type information for these
 * have exactly one home; a class with virtual functions defined entirely
 * inline emits both into every translation unit that sees it. */
bad_alloc::~bad_alloc() {}
const char *bad_alloc::what() const noexcept { return "std::bad_alloc"; }

bad_array_new_length::~bad_array_new_length() {}
const char *bad_array_new_length::what() const noexcept {
    return "std::bad_array_new_length";
}

/*
 * The new handler.
 *
 * Kept and called, because the interface promises it and because it is
 * the one hook a program has for "release a cache and let me try again".
 * The loop below is the one the standard describes: call the handler,
 * try again, and give up only when there is no handler left to call.
 */
static new_handler vx_new_handler = nullptr;

new_handler set_new_handler(new_handler h) noexcept {
    new_handler old = vx_new_handler;
    vx_new_handler = h;
    return old;
}

new_handler get_new_handler() noexcept { return vx_new_handler; }

}  // namespace std

/*
 * The one place that decides what running out of memory means.
 *
 * See the long note in <new> for why this stops the program rather than
 * returning null: code compiled against a throwing `new` does not check
 * the result, so null would surface as a fault inside the constructor
 * rather than as an allocation failure, and the report would name the
 * wrong thing entirely.
 */
[[noreturn]] static void vx_new_failed(std::size_t n) {
    char msg[96];
    std::snprintf(msg, sizeof(msg),
                  "out of memory: operator new(%lu) failed\n",
                  (unsigned long)n);
    std::fputs_stream(msg, stderr);
    std::abort();
}

static void *vx_alloc(std::size_t n) {
    /* Zero bytes still gets a distinct address, because two objects must
     * not compare equal for having no members between them. */
    if (n == 0) n = 1;
    for (;;) {
        void *p = std::malloc(n);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) return nullptr;
        h();
    }
}

/*
 * Over-aligned allocation, by hand.
 *
 * aligned_alloc exists in this C library and is used; what is done here
 * on top of it is the check the standard requires — an alignment must be
 * a power of two — because a caller that passes six gets an allocator
 * walking off the end of a block rather than an error.
 */
static void *vx_alloc_aligned(std::size_t n, std::size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (n == 0) n = align;
    /* aligned_alloc requires a size that is a multiple of the alignment,
     * which the caller has no reason to have arranged. */
    std::size_t bytes = (n + align - 1) & ~(align - 1);
    for (;;) {
        void *p = std::aligned_alloc(align, bytes);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) return nullptr;
        h();
    }
}

/* ---- allocation ---- */

void *operator new(std::size_t n) {
    void *p = vx_alloc(n);
    if (!p) vx_new_failed(n);
    return p;
}

void *operator new[](std::size_t n) { return ::operator new(n); }

void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    return vx_alloc(n);
}

void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
    return vx_alloc(n);
}

void *operator new(std::size_t n, std::align_val_t a) {
    void *p = vx_alloc_aligned(n, static_cast<std::size_t>(a));
    if (!p) vx_new_failed(n);
    return p;
}

void *operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void *operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept {
    return vx_alloc_aligned(n, static_cast<std::size_t>(a));
}

void *operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t &) noexcept {
    return vx_alloc_aligned(n, static_cast<std::size_t>(a));
}

/* ---- release ----
 *
 * The size and the alignment are accepted and discarded. They are there
 * for allocators that keep objects in size classes and would otherwise
 * have to look the size up; this one records it in a header ahead of
 * every block, so it already knows. Accepting them anyway is not
 * optional — the compiler emits calls to the sized forms by default and
 * to the aligned forms for any over-aligned type, and a missing overload
 * is an undefined symbol at the end of the build.
 */

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
