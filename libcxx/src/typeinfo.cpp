/*
 * libcxx/src/typeinfo.cpp — run-time type identification.
 *
 * Two things live here. The first is a set of destructors that look
 * pointless and are not: defining each one out of line is what makes the
 * compiler emit that class's *vtable* in this object file, and the type
 * descriptors GCC writes into every -frtti translation unit point at
 * those vtables by name. Without them a link ends in
 *
 *     undefined reference to `vtable for __cxxabiv1::__si_class_type_info'
 *
 * naming a symbol nobody wrote and no source file mentions.
 *
 * The second is __dynamic_cast.
 *
 * ============================================================
 *  WHAT dynamic_cast ACTUALLY HAS TO DO
 * ============================================================
 *
 * The compiler knows the *static* type of the pointer it is given and
 * the type asked for. It cannot know the type of the complete object,
 * which is the whole point, so it emits a call and the answer is found
 * at run time by reading the object.
 *
 * Every polymorphic object begins with a pointer to its class's vtable,
 * and the two words *before* a vtable's function pointers are, by the
 * Itanium ABI, the offset from this subobject to the top of the complete
 * object and a pointer to the complete object's type descriptor. So
 * three loads turn "some pointer" into "an object of exactly this type,
 * starting here", and everything after that is a walk over base classes.
 *
 * ============================================================
 *  THE RULE, WHICH IS NOT "SEARCH FOR THE TYPE"
 * ============================================================
 *
 * [expr.dynamic.cast] is precise, and the precision matters because the
 * obvious implementation is wrong for multiple inheritance:
 *
 *   1. If, within the complete object, the source subobject is a public
 *      base class subobject of exactly one object of the target type,
 *      the result is that object.
 *   2. Otherwise, if the source is a public base subobject of the
 *      complete object, and the complete object has an unambiguous
 *      public base of the target type, the result is that base.
 *   3. Otherwise the cast fails and yields null.
 *
 * Rule 1 is the downcast and the cross-cast. Rule 2 is what makes
 * `dynamic_cast<Base*>` work from a sibling that does not itself derive
 * from Base. "Unambiguous" is doing real work in both: a target type
 * that appears twice in the object is not an answer, it is a failure,
 * and returning either one would be a silently wrong pointer rather
 * than a null anybody could test.
 *
 * ============================================================
 *  WHY THIS IS A PLAIN WALK
 * ============================================================
 *
 * libsupc++ implements the same rules as one incremental pass that
 * threads a partial result through every recursion, with a contract per
 * function about what may already be known. It is faster and it is very
 * hard to read, and its correctness is not local to any one function.
 *
 * This does the rules literally: collect every target subobject, then
 * ask of each whether the source sits publicly inside it. Two small
 * recursions, each of which can be checked on its own. The cost is
 * revisiting shared bases in a diamond, and the depth of a real class
 * hierarchy makes that irrelevant -- ICU's deepest is six.
 *
 * The one shortcut taken is the case that is nearly all of them: a
 * single-inheritance downcast, where the compiler's own hint says the
 * target contains the source at a fixed offset and the complete object
 * turns out to be the target. Two comparisons, no walk.
 */

#include <typeinfo>
#include <cxxabi.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace std {

/* Out of line for the same reason as everything else in this file: one
 * home for the vtable. */
type_info::~type_info() {}

/*
 * A hash consistent with operator==, which is the only requirement.
 *
 * Names marked unique with a leading '*' compare by address, so their
 * hash is derived from the address; everything else compares by string,
 * so its hash must come from the string. Getting this backwards would
 * put equal types in different buckets of an unordered_map and lose
 * them.
 */
size_t type_info::hash_code() const noexcept {
    if (__name[0] == '*') {
        /* Fibonacci hashing of the pointer: multiply by 2^64/phi and
         * take the high bits, which mixes the low-entropy alignment
         * bits away. */
        size_t v = (size_t)(const void *)__name;
        return (size_t)(v * 0x9E3779B97F4A7C15ULL);
    }
    /* FNV-1a over the name. */
    size_t h = 1469598103934665603ULL;
    for (const char *p = __name; *p; ++p) {
        h ^= (size_t)(unsigned char)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

bad_cast::~bad_cast() {}
const char *bad_cast::what() const noexcept { return "std::bad_cast"; }

bad_typeid::~bad_typeid() {}
const char *bad_typeid::what() const noexcept { return "std::bad_typeid"; }

}  // namespace std

namespace __cxxabiv1 {

/* ============================================================
 *  the destructors that exist to place vtables
 * ============================================================ */

__fundamental_type_info::~__fundamental_type_info() {}
__array_type_info::~__array_type_info() {}
__function_type_info::~__function_type_info() {}
__enum_type_info::~__enum_type_info() {}
__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
__pbase_type_info::~__pbase_type_info() {}
__pointer_type_info::~__pointer_type_info() {}
__pointer_to_member_type_info::~__pointer_to_member_type_info() {}

/* ============================================================
 *  describing base classes uniformly
 * ============================================================
 *
 * The three class descriptor kinds store their bases in three different
 * shapes -- none, one implicit, or a table -- and the search should not
 * have to know which it is looking at. These two functions are the only
 * place that difference exists.
 */

unsigned __class_type_info::__vx_nbases() const { return 0; }

const __class_type_info *__class_type_info::__vx_base(unsigned, const void *,
                                                      ptrdiff_t *, bool *) const {
    return nullptr;
}

unsigned __si_class_type_info::__vx_nbases() const { return 1; }

const __class_type_info *__si_class_type_info::__vx_base(unsigned i,
                                                         const void *,
                                                         ptrdiff_t *offset,
                                                         bool *is_public) const {
    if (i != 0) return nullptr;
    /* This descriptor kind exists precisely because both answers are
     * known without looking anything up: offset zero, public. */
    *offset    = 0;
    *is_public = true;
    return __base_type;
}

unsigned __vmi_class_type_info::__vx_nbases() const { return __base_count; }

const __class_type_info *__vmi_class_type_info::__vx_base(unsigned i,
                                                          const void *obj,
                                                          ptrdiff_t *offset,
                                                          bool *is_public) const {
    if (i >= __base_count) return nullptr;
    const __base_class_type_info &b = __base_info[i];
    /* __base_offset reads the object's vtable when the base is virtual,
     * which is why obj is a parameter here and not in the si case. */
    *offset    = b.__base_offset(obj);
    *is_public = b.__is_public();
    return b.__base_type;
}

}  // namespace __cxxabiv1

/* ============================================================
 *  the search
 * ============================================================ */

namespace {

using __cxxabiv1::__class_type_info;

/*
 * A recursion bound rather than a cycle check.
 *
 * The base graph of a well-formed program is acyclic, so this can only
 * be reached by a corrupt descriptor -- a wild pointer that happens to
 * land in something shaped like one. Failing the cast is the right
 * answer to that; recursing until the stack is gone is not, and on this
 * system a stack overflow in a library is a page fault with no
 * explanation attached.
 */
constexpr unsigned kMaxDepth = 64;

/*
 * At most this many distinct subobjects of the target type. Two is
 * already a failed cast by the ambiguity rule, so the only thing a
 * larger number buys is the ability to *report* ambiguity in a class
 * with a great many repeated bases -- and on overflow the search stops
 * counting and the cast fails, which is the conservative direction.
 */
constexpr unsigned kMaxHits = 16;

struct hit {
    const void *addr;
    bool        public_path;
};

struct finder {
    const __class_type_info *target;
    hit      hits[kMaxHits];
    unsigned n;
    bool     overflowed;

    finder(const __class_type_info *t) : target(t), n(0), overflowed(false) {}

    void add(const void *addr, bool is_public) {
        /* A virtual base is one subobject however many paths reach it,
         * so identity is the address. It is public if any path to it is
         * public, which is what accessibility means. */
        for (unsigned i = 0; i < n; i++)
            if (hits[i].addr == addr) {
                if (is_public) hits[i].public_path = true;
                return;
            }
        if (n == kMaxHits) { overflowed = true; return; }
        hits[n].addr = addr;
        hits[n].public_path = is_public;
        n++;
    }

    void walk(const __class_type_info *type, const void *obj,
              bool is_public, unsigned depth) {
        if (depth >= kMaxDepth) { overflowed = true; return; }
        if (*type == *target) add(obj, is_public);

        const unsigned nb = type->__vx_nbases();
        for (unsigned i = 0; i < nb; i++) {
            ptrdiff_t off = 0;
            bool      base_public = false;
            const __class_type_info *b =
                type->__vx_base(i, obj, &off, &base_public);
            if (!b) continue;
            /* A path is public only if every step of it is. */
            walk(b, (const char *)obj + off, is_public && base_public,
                 depth + 1);
        }
    }
};

/*
 * Is the object of type `want` at `want_addr` a public base class
 * subobject of the object of type `type` at `obj` -- or that object
 * itself?
 *
 * Only public edges are followed, so a target reachable solely through
 * a private base is correctly not found: dynamic_cast is not permitted
 * to cross an inheritance the program cannot name.
 */
bool public_contains(const __class_type_info *type, const void *obj,
                     const __class_type_info *want, const void *want_addr,
                     unsigned depth) {
    if (depth >= kMaxDepth) return false;
    if (obj == want_addr && *type == *want) return true;

    const unsigned nb = type->__vx_nbases();
    for (unsigned i = 0; i < nb; i++) {
        ptrdiff_t off = 0;
        bool      base_public = false;
        const __class_type_info *b = type->__vx_base(i, obj, &off, &base_public);
        if (!b || !base_public) continue;
        if (public_contains(b, (const char *)obj + off, want, want_addr,
                            depth + 1))
            return true;
    }
    return false;
}

}  // namespace

extern "C" void *__dynamic_cast(const void *sub,
                                const __class_type_info *src,
                                const __class_type_info *dst,
                                ptrdiff_t src2dst) {
    if (!sub || !src || !dst) return nullptr;

    /* ---- read the complete object out of the vtable ---- */
    const void *const *const vtable = *(const void *const *const *)sub;
    if (!vtable) return nullptr;

    const ptrdiff_t offset_to_top = ((const ptrdiff_t *)vtable)[-2];
    const __class_type_info *const whole_type =
        ((const __class_type_info *const *)vtable)[-1];
    const void *const whole = (const char *)sub + offset_to_top;

    /*
     * A null descriptor is not corruption. Everything else in this
     * runtime is compiled -fno-rtti, and GCC emits a vtable with a null
     * type-information slot for such a class -- so this is what a
     * dynamic_cast on an object from a -fno-rtti translation unit looks
     * like. Failing the cast is the only honest answer: the type of the
     * complete object genuinely was not recorded.
     */
    if (!whole_type) return nullptr;

    /*
     * The fast path, and the shape of nearly every cast in practice: a
     * downcast in a single-inheritance chain. src2dst >= 0 means the
     * compiler determined that dst contains src as a unique public
     * non-virtual base at that offset, so if the complete object starts
     * where that puts dst and is itself a dst, it is the answer -- and
     * it is unambiguous, because a class cannot contain itself as a
     * base.
     */
    if (src2dst >= 0 && (const char *)sub - src2dst == (const char *)whole &&
        *whole_type == *dst)
        return const_cast<void *>(whole);

    /* ---- rule 1: a target object that contains the source publicly ---- */
    finder found(dst);
    found.walk(whole_type, whole, true, 0);
    if (found.overflowed) return nullptr;

    const void *answer  = nullptr;
    unsigned    matches = 0;
    for (unsigned i = 0; i < found.n; i++)
        if (public_contains(dst, found.hits[i].addr, src, sub, 0)) {
            answer = found.hits[i].addr;
            matches++;
        }

    if (matches == 1) return const_cast<void *>(answer);
    if (matches > 1) return nullptr;      /* ambiguous: a failure, not a pick */

    /* ---- rule 2: an unambiguous public target base of the whole ---- */
    if (public_contains(whole_type, whole, src, sub, 0)) {
        const void *pub   = nullptr;
        unsigned    npubs = 0;
        for (unsigned i = 0; i < found.n; i++)
            if (found.hits[i].public_path) {
                pub = found.hits[i].addr;
                npubs++;
            }
        if (npubs == 1) return const_cast<void *>(pub);
    }

    return nullptr;
}

/* ============================================================
 *  the two that cannot do what their names say
 * ============================================================
 *
 * A dynamic_cast to a *reference* has no null to return, so the ABI
 * requires it to throw std::bad_cast; typeid on a null pointer must
 * throw std::bad_typeid. This runtime has no unwinder, so neither can
 * happen -- and a runtime that returned instead would carry on with a
 * reference bound to nothing, which is worse than stopping.
 *
 * They are here rather than absent because the compiler emits calls to
 * them whenever it sees either construct, so their absence is a link
 * error rather than a compile error, and the message names a symbol
 * rather than a line. With these present the failure is at run time and
 * says what happened.
 */

extern "C" [[noreturn]] void __cxa_bad_cast(void) {
    std::fputs_stream("libcxx: a dynamic_cast to a reference type failed, "
                      "and there is no exception to throw\n", stderr);
    std::abort();
}

extern "C" [[noreturn]] void __cxa_bad_typeid(void) {
    std::fputs_stream("libcxx: typeid was applied to a null pointer, "
                      "and there is no exception to throw\n", stderr);
    std::abort();
}
