// -*- C++ -*-
#ifndef _VXCXX_CXXABI_H
#define _VXCXX_CXXABI_H

/*
 * cxxabi.h — the parts of the Itanium C++ ABI this runtime implements.
 *
 * ============================================================
 *  WHAT IS FIXED HERE AND WHAT IS NOT
 * ============================================================
 *
 * When a translation unit compiled -frtti mentions a type in a typeid or
 * a dynamic_cast, the compiler writes a type descriptor object into that
 * unit: one of the classes below, filled in by the compiler, with its
 * vtable pointer pointing at that class's vtable.
 *
 * So two different things meet in this file, and only one of them is
 * negotiable.
 *
 * **The data layouts are the ABI's.** GCC has already written those
 * objects; the field order and the bit packing below are how they must
 * be read. A field inserted in the wrong place does not fail to
 * compile -- it reads a base-class pointer out of the middle of a
 * string.
 *
 * **The virtual functions are ours.** Nothing outside this runtime ever
 * calls through these vtables: the compiler emits the descriptors as
 * data and calls __dynamic_cast, which is an ordinary extern "C"
 * function. libsupc++ puts __do_dyncast, __do_find_public_src and
 * __do_upcast here, each with an intricate contract, because its search
 * is a single incremental pass that accumulates a result as it goes.
 * The two virtuals below are enough for the search in
 * libcxx/src/typeinfo.cpp, which walks the base graph plainly instead --
 * see the note there about why plainly was the right choice.
 *
 * **The vtable symbols must exist.** The descriptors GCC emitted point
 * at _ZTVN10__cxxabiv120__si_class_type_infoE and its siblings by name.
 * Each destructor below is declared here and defined out of line in
 * typeinfo.cpp, which is what makes the compiler emit those vtables
 * there and exactly once.
 *
 * ============================================================
 *  WHAT THE THREE CLASS KINDS MEAN
 * ============================================================
 *
 *   __class_type_info      no base classes at all
 *   __si_class_type_info   exactly one base: public, non-virtual, at
 *                          offset zero. Nearly every class in an
 *                          ordinary hierarchy, and a separate kind so
 *                          that the common case costs one pointer
 *                          rather than a table.
 *   __vmi_class_type_info  everything else -- several bases, a private
 *                          or protected base, a virtual base, or a base
 *                          at a non-zero offset.
 *
 * ICU reaches all three: single-inheritance chains throughout, and also
 * classes declared `: public Iterator, public UnicodeFunctor`. An
 * implementation that handled only the first two would compile, link,
 * and quietly return null from casts that should succeed.
 */

#include <cstddef>
#include <typeinfo>

namespace __cxxabiv1 {

/* ---- leaf kinds: a name and nothing to search ---- */

class __fundamental_type_info : public std::type_info {
public:
    explicit __fundamental_type_info(const char *n) : std::type_info(n) {}
    ~__fundamental_type_info() override;
};

class __array_type_info : public std::type_info {
public:
    explicit __array_type_info(const char *n) : std::type_info(n) {}
    ~__array_type_info() override;
};

class __function_type_info : public std::type_info {
public:
    explicit __function_type_info(const char *n) : std::type_info(n) {}
    ~__function_type_info() override;
};

class __enum_type_info : public std::type_info {
public:
    explicit __enum_type_info(const char *n) : std::type_info(n) {}
    ~__enum_type_info() override;
};

/* ---- class kinds ---- */

class __class_type_info;

/*
 * One entry of a __vmi_class_type_info's base table.
 *
 * __offset_flags packs three things into one word, and the packing is
 * the ABI's: bit 0 says the base is virtual, bit 1 says it is public,
 * and the rest shifted down by 8 is the offset. For a virtual base that
 * "offset" is not an offset -- it is a displacement into the virtual
 * base offset table hanging off the object's own vtable, which is why
 * resolving it needs the object and not just the descriptor.
 */
struct __base_class_type_info {
    const __class_type_info *__base_type;
    long                     __offset_flags;

    enum __offset_flags_masks {
        __virtual_mask = 0x1,
        __public_mask  = 0x2,
        __offset_shift = 8
    };

    bool __is_virtual() const { return (__offset_flags & __virtual_mask) != 0; }
    bool __is_public()  const { return (__offset_flags & __public_mask) != 0; }
    long __offset()     const { return __offset_flags >> __offset_shift; }

    ptrdiff_t __base_offset(const void *obj) const {
        if (!__is_virtual()) return (ptrdiff_t)__offset();
        /* The vtable pointer is the first word of any object with a
         * virtual base; the offset sits at __offset() bytes from it,
         * which the ABI specifies as a negative displacement. */
        const char *const vtable = *(const char *const *)obj;
        return *(const ptrdiff_t *)(vtable + __offset());
    }
};

class __class_type_info : public std::type_info {
public:
    explicit __class_type_info(const char *n) : std::type_info(n) {}
    ~__class_type_info() override;

    /* How many direct base classes this type has. */
    virtual unsigned __vx_nbases() const;

    /*
     * Describe direct base i of an object of this type located at `obj`.
     *
     * Returns the base's descriptor and, through the out-parameters,
     * where that base subobject sits relative to `obj` and whether the
     * inheritance is public. `obj` is needed rather than optional
     * because a virtual base's offset lives in the object's vtable, not
     * in the descriptor.
     */
    virtual const __class_type_info *__vx_base(unsigned i, const void *obj,
                                               ptrdiff_t *offset,
                                               bool *is_public) const;
};

class __si_class_type_info : public __class_type_info {
public:
    const __class_type_info *__base_type;

    __si_class_type_info(const char *n, const __class_type_info *b)
        : __class_type_info(n), __base_type(b) {}
    ~__si_class_type_info() override;

    unsigned __vx_nbases() const override;
    const __class_type_info *__vx_base(unsigned i, const void *obj,
                                       ptrdiff_t *offset,
                                       bool *is_public) const override;
};

class __vmi_class_type_info : public __class_type_info {
public:
    unsigned int           __flags;
    unsigned int           __base_count;
    __base_class_type_info __base_info[1];   /* __base_count of them */

    enum __flags_masks {
        __non_diamond_repeat_mask = 0x1,   /* a repeated base, non-virtually */
        __diamond_shaped_mask     = 0x2,   /* a repeated base, virtually     */
        __flags_unknown_mask      = 0x10
    };

    __vmi_class_type_info(const char *n, int flags)
        : __class_type_info(n), __flags(flags), __base_count(0) {}
    ~__vmi_class_type_info() override;

    unsigned __vx_nbases() const override;
    const __class_type_info *__vx_base(unsigned i, const void *obj,
                                       ptrdiff_t *offset,
                                       bool *is_public) const override;
};

/* ---- pointer kinds ---- */

class __pbase_type_info : public std::type_info {
public:
    unsigned int          __flags;
    const std::type_info *__pointee;

    enum __masks {
        __const_mask            = 0x1,
        __volatile_mask         = 0x2,
        __restrict_mask         = 0x4,
        __incomplete_mask       = 0x8,
        __incomplete_class_mask = 0x10,
        __transaction_safe_mask = 0x20,
        __noexcept_mask         = 0x40
    };

    __pbase_type_info(const char *n, int f, const std::type_info *p)
        : std::type_info(n), __flags(f), __pointee(p) {}
    ~__pbase_type_info() override;
};

class __pointer_type_info : public __pbase_type_info {
public:
    __pointer_type_info(const char *n, int f, const std::type_info *p)
        : __pbase_type_info(n, f, p) {}
    ~__pointer_type_info() override;
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    const __class_type_info *__context;

    __pointer_to_member_type_info(const char *n, int f,
                                  const std::type_info *p,
                                  const __class_type_info *c)
        : __pbase_type_info(n, f, p), __context(c) {}
    ~__pointer_to_member_type_info() override;
};

}  // namespace __cxxabiv1

namespace abi = __cxxabiv1;

extern "C" {

/*
 * The function the compiler emits a call to for every pointer
 * dynamic_cast.
 *
 * `sub` points at a subobject of static type `src`; the result is the
 * `dst` subobject of the same complete object, or null.
 *
 * src2dst is a hint: >= 0 means dst contains src as a unique public
 * non-virtual base at that offset, -1 means no information, -2 means
 * src is not a public base of dst, -3 means src is a public base of dst
 * more than once but never virtually. It is never required -- the
 * search is correct without it -- and typeinfo.cpp says which of these
 * it takes advantage of and why.
 */
void *__dynamic_cast(const void *sub,
                     const __cxxabiv1::__class_type_info *src,
                     const __cxxabiv1::__class_type_info *dst,
                     ptrdiff_t src2dst);

[[noreturn]] void __cxa_bad_cast(void);
[[noreturn]] void __cxa_bad_typeid(void);

}

#endif
