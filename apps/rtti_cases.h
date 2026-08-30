/*
 * apps/rtti_cases.h — dynamic_cast and typeid, asked the same questions
 * on two runtimes.
 *
 * ============================================================
 *  WHY THIS IS A SHARED HEADER AND NOT A TEST
 * ============================================================
 *
 * libcxx/src/typeinfo.cpp implements __dynamic_cast for this system: it
 * reads the type descriptors GCC emits, walks the base graph, and
 * applies the rule in [expr.dynamic.cast]. It is the kind of code whose
 * bugs are quiet — a cast that should succeed returning null looks
 * exactly like a cast that should fail, and multiple inheritance is
 * where the difference lives.
 *
 * So the questions are asked twice. This header is compiled into
 * tools/cxx_test.cpp, which runs on the host against the host's own
 * C++ runtime, and into apps/cxxtest.cpp, which runs in ring 3 against
 * the one in libcxx/. Every expectation below is written as an address
 * the compiler computes statically — `static_cast<Base *>(&obj)` — so
 * neither run is checking the implementation against itself.
 *
 * A disagreement between the two is a bug in libcxx/src/typeinfo.cpp.
 * Agreement on all of them, including the diamonds, is the strongest
 * statement available without a conformance suite: two independently
 * written implementations of the same specification, on the same
 * hierarchies, returning the same pointers.
 *
 * ============================================================
 *  WHAT THE HIERARCHIES ARE FOR
 * ============================================================
 *
 * Each shape below exercises a different descriptor kind or a different
 * clause of the rule:
 *
 *   Animal/Dog/Puppy      __si_class_type_info. The single-inheritance
 *                         chain, which is the fast path and most of ICU.
 *   Left/Right/Both       __vmi_class_type_info. Two bases, so one of
 *                         them sits at a non-zero offset — a cast to it
 *                         must *move* the pointer, and an implementation
 *                         that returns the address it was given passes
 *                         every single-inheritance test and fails here.
 *   Both -> Left/Right    the cross-cast. Neither base derives from the
 *                         other; the only way across is through the
 *                         complete object.
 *   VLeft/VRight/Diamond  virtual inheritance. The shared VBase is one
 *                         subobject reached by two paths, and its offset
 *                         lives in the object's vtable rather than in
 *                         any descriptor.
 *   Repeat                the *non*-virtual diamond: two distinct VBase
 *                         subobjects. From one of them the other side is
 *                         still reachable, but only through the second
 *                         clause of the rule — which is the clause an
 *                         implementation is most likely to have skipped.
 *   Holder                the same, plus a base that sees neither side,
 *                         so that a cast to the doubled base is resolved
 *                         at run time and must come back null. Returning
 *                         either copy is the failure this case catches.
 *   Secret                a private base. It is unreachable by
 *                         dynamic_cast even though it is physically
 *                         there, because accessibility is part of the
 *                         rule and not an afterthought.
 */

#ifndef VX_RTTI_CASES_H
#define VX_RTTI_CASES_H

#include <typeinfo>

namespace rtti_cases {

/* ---- 1. single inheritance ---- */

struct Animal {
    virtual ~Animal() {}
    virtual int legs() const { return 4; }
};
struct Dog : Animal {
    int bark = 1;
};
struct Puppy : Dog {
    int wag = 2;
};
struct Cat : Animal {
    int meow = 3;
};

/* ---- 2. multiple inheritance, and the cross-cast ---- */

struct Left {
    virtual ~Left() {}
    int l = 10;
};
struct Right {
    virtual ~Right() {}
    int r = 20;
};
struct Both : Left, Right {
    int b = 30;
};

/* ---- 3. virtual inheritance ---- */

struct VBase {
    virtual ~VBase() {}
    int v = 40;
};
struct VLeft : virtual VBase {
    int vl = 50;
};
struct VRight : virtual VBase {
    int vr = 60;
};
struct Diamond : VLeft, VRight {
    int d = 70;
};

/* ---- 4. the non-virtual diamond: two of the same base ---- */

struct RLeft : VBase {
    int rl = 80;
};
struct RRight : VBase {
    int rr = 90;
};
struct Repeat : RLeft, RRight {
    int rp = 100;
};

/* The same shape with a third base that sees neither side, so that a
 * cast to the repeated base has to be resolved at run time rather than
 * rejected at compile time. */
struct Unrelated {
    virtual ~Unrelated() {}
    int u = 105;
};
struct Holder : Unrelated, RLeft, RRight {
    int hd = 106;
};

/* ---- 5. a base the program may not name ---- */

struct Secret {
    virtual ~Secret() {}
    int s = 110;
};
struct Hides : private Secret {
    virtual ~Hides() {}
    int h = 120;
};

/*
 * Every check is `expression == expected`, where expected is either a
 * statically computed address or nullptr. `check` is the caller's
 * assertion function so that the two harnesses can report in their own
 * formats.
 */
using check_fn = void (*)(const char *what, bool good);

/*
 * An optimisation barrier, and it is load-bearing.
 *
 * Every object below is a local, so the compiler can see its complete
 * type and fold most of these casts at compile time -- it warned about
 * two of them ("can never succeed") and silently constant-folded
 * several more, which would have left a test that checks the compiler
 * rather than the runtime. Passing each source pointer through an empty
 * asm with the value in a register destroys that knowledge without
 * emitting an instruction, so every dynamic_cast below is a real call
 * into libcxx/src/typeinfo.cpp.
 *
 * The count is checked rather than assumed: after this, the object file
 * contains one call to __dynamic_cast per cast in the file.
 */
template <class T>
inline T *hide(T *p) {
    asm volatile("" : "+r"(p));
    return p;
}

inline void run(check_fn check) {
    /* ---- single inheritance: down, up, and sideways ---- */
    {
        Puppy   p;
        Animal *a = hide<Animal>(&p);

        check("downcast to the exact type",
              dynamic_cast<Puppy *>(a) == &p);
        check("downcast to an intermediate type",
              dynamic_cast<Dog *>(a) == static_cast<Dog *>(&p));
        check("downcast to a sibling fails",
              dynamic_cast<Cat *>(a) == nullptr);

        Dog d;
        Animal *ad = hide<Animal>(&d);
        check("a Dog is not a Puppy",
              dynamic_cast<Puppy *>(ad) == nullptr);
        check("but it is a Dog",
              dynamic_cast<Dog *>(ad) == &d);

        /* Through a middle pointer rather than the top one, so the
         * source subobject is not at offset zero of the walk. */
        Dog *middle = hide<Dog>(&p);
        check("downcast from the middle of the chain",
              dynamic_cast<Puppy *>(middle) == &p);
        check("upcast from the middle of the chain",
              dynamic_cast<Animal *>(middle) == static_cast<Animal *>(&p));
    }

    /* ---- multiple inheritance: the pointer has to move ---- */
    {
        Both  b;
        Left *l = hide<Left>(&b);
        Right *r = hide<Right>(&b);

        /* Right does not start where Both does; if these two addresses
         * were equal the case would prove nothing, so it is asserted. */
        check("the two bases are at different addresses",
              (void *)static_cast<Left *>(&b) != (void *)static_cast<Right *>(&b));

        check("downcast from the first base",
              dynamic_cast<Both *>(l) == &b);
        check("downcast from the second base, which is offset",
              dynamic_cast<Both *>(r) == &b);

        check("cross-cast left to right",
              dynamic_cast<Right *>(l) == static_cast<Right *>(&b));
        check("cross-cast right to left",
              dynamic_cast<Left *>(r) == static_cast<Left *>(&b));

        /* A standalone Left is not part of a Both. */
        Left alone;
        check("a plain Left does not cross-cast",
              dynamic_cast<Right *>(hide(&alone)) == nullptr);
        check("and does not downcast",
              dynamic_cast<Both *>(hide(&alone)) == nullptr);
    }

    /* ---- virtual inheritance: one shared base, two paths ---- */
    {
        Diamond dm;
        VBase  *vb = hide<VBase>(&dm);

        check("the virtual base downcasts to the complete object",
              dynamic_cast<Diamond *>(vb) == &dm);
        check("and to each side of the diamond",
              dynamic_cast<VLeft *>(vb) == static_cast<VLeft *>(&dm) &&
              dynamic_cast<VRight *>(vb) == static_cast<VRight *>(&dm));

        VLeft *vl = hide<VLeft>(&dm);
        check("across the diamond, left to right",
              dynamic_cast<VRight *>(vl) == static_cast<VRight *>(&dm));

        /* Both paths reach the same VBase subobject -- that is what
         * virtual inheritance means, and it is why the cast above is
         * unambiguous where the next block's is not. */
        check("both paths reach one virtual base",
              static_cast<VBase *>(static_cast<VLeft *>(&dm)) ==
              static_cast<VBase *>(static_cast<VRight *>(&dm)));

        VLeft standalone;
        check("a plain VLeft has its own virtual base",
              dynamic_cast<Diamond *>(hide<VBase>(&standalone)) ==
              nullptr);
    }

    /* ---- the repeated base: ambiguity is a null, not a choice ---- */
    {
        Repeat rp;

        check("the two copies of the base are distinct",
              (void *)static_cast<VBase *>(static_cast<RLeft *>(&rp)) !=
              (void *)static_cast<VBase *>(static_cast<RRight *>(&rp)));

        /* From one copy, the complete object is reachable and unique. */
        VBase *via_left  = hide<VBase>(static_cast<RLeft *>(&rp));
        VBase *via_right = hide<VBase>(static_cast<RRight *>(&rp));
        check("the left copy downcasts to the whole",
              dynamic_cast<Repeat *>(via_left) == &rp);
        check("the right copy downcasts to the whole",
              dynamic_cast<Repeat *>(via_right) == &rp);

        check("from the left copy, RLeft is found",
              dynamic_cast<RLeft *>(via_left) == static_cast<RLeft *>(&rp));

        /*
         * And RRight is found too, which is the case that catches a
         * plausible wrong implementation.
         *
         * via_left is not inside an RRight, so the first clause of the
         * rule finds nothing and an implementation that stops there
         * returns null. The second clause applies instead: via_left is
         * a public base subobject of the complete object, and the
         * complete object has exactly one public RRight base — so the
         * answer is that RRight.
         */
        check("and RRight is reached through the complete object",
              dynamic_cast<RRight *>(via_left) == static_cast<RRight *>(&rp));
    }

    /* ---- ambiguity, which must be a null ----
     *
     * Holder contains two VBase subobjects, one down each side. A cast
     * to VBase from a base that cannot see either is the case where the
     * rule says the cast fails: there is no unambiguous answer, and
     * picking one would be a pointer to the wrong half of the object.
     *
     * The source has to be Unrelated rather than one of the sides,
     * because from a side the compiler resolves the conversion
     * statically and no run-time search happens at all.
     */
    {
        Holder    hd;
        Unrelated *u = hide<Unrelated>(&hd);

        check("the ambiguous target really is present twice",
              (void *)static_cast<VBase *>(static_cast<RLeft *>(&hd)) !=
              (void *)static_cast<VBase *>(static_cast<RRight *>(&hd)));
        check("an unrelated base still finds the complete object",
              dynamic_cast<Holder *>(u) == &hd);
        check("and each side individually",
              dynamic_cast<RLeft *>(u) == static_cast<RLeft *>(&hd) &&
              dynamic_cast<RRight *>(u) == static_cast<RRight *>(&hd));
        check("but a base present twice is ambiguous and yields null",
              dynamic_cast<VBase *>(u) == nullptr);
    }

    /* ---- a private base is not reachable ---- */
    {
        Hides h;
        /* Reaching the Secret subobject at all requires the cast to be
         * made from inside, which the program cannot do here; what is
         * checked is the other direction, from a Secret that is really
         * part of a Hides. The address is obtained by a C-style cast,
         * which is permitted where static_cast is not. */
        Secret *s = hide((Secret *)&h);
        check("a private base does not downcast",
              dynamic_cast<Hides *>(s) == nullptr);
    }

    /* ---- typeid ---- */
    {
        Puppy   p;
        Dog     d;
        Animal *ap = &p;
        Animal *ad = &d;

        check("typeid sees the complete type, not the static one",
              typeid(*ap) == typeid(Puppy));
        check("and tells two complete types apart",
              typeid(*ap) != typeid(*ad));
        check("typeid on the static type is the static type",
              typeid(ap) == typeid(Animal *));
        check("equality is reflexive",
              typeid(Puppy) == typeid(Puppy));
        check("a name is not empty",
              typeid(Puppy).name() != nullptr && typeid(Puppy).name()[0] != '\0');
        check("equal types hash equally",
              typeid(Puppy).hash_code() == typeid(*ap).hash_code());
        check("different types usually hash differently",
              typeid(Puppy).hash_code() != typeid(Cat).hash_code());
        check("before() is a strict order",
              (typeid(Puppy).before(typeid(Cat)) !=
               typeid(Cat).before(typeid(Puppy))) ||
              typeid(Puppy) == typeid(Cat));

        /* Fundamental and pointer type descriptors are different
         * descriptor kinds again, and they must still compare. */
        check("fundamental types compare",
              typeid(int) == typeid(int) && typeid(int) != typeid(long));
        check("pointer types compare",
              typeid(int *) != typeid(int) && typeid(int *) == typeid(int *));
    }

    /* ---- null and self ---- */
    {
        Animal *nothing = nullptr;
        check("a null pointer casts to null",
              dynamic_cast<Puppy *>(nothing) == nullptr);

        Puppy p;
        Puppy *self = hide<Puppy>(&p);
        check("a cast to the same type is the same pointer",
              dynamic_cast<Puppy *>(self) == &p);
        check("and to void* is the complete object",
              dynamic_cast<void *>(hide<Animal>(&p)) == (void *)&p);

        Both b;
        check("to void* from an offset base is still the complete object",
              dynamic_cast<void *>(hide<Right>(&b)) == (void *)&b);
    }
}

}  // namespace rtti_cases

#endif
