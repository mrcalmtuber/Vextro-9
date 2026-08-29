/*
 * assert.h — a claim the program makes about itself.
 *
 * No include guard, deliberately, and this is the one header in the
 * standard where that is correct rather than a mistake: NDEBUG is
 * allowed to change between two inclusions of this file in one
 * translation unit, and the definition of assert has to change with it.
 * A guard would freeze whichever meaning was seen first.
 */

#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#ifdef __cplusplus
extern "C"
#endif
void __assert_fail(const char *expr, const char *file, int line,
                   const char *fn) __attribute__((noreturn));
#define assert(e) \
    ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

/*
 * static_assert is a keyword in C++ and has been since C++11, so
 * defining a macro over it there is at best redundant and at worst a
 * redefinition the compiler complains about. In C it is _Static_assert
 * until C23, which is why the macro exists at all.
 */
#if !defined(__cplusplus) && !defined(_ASSERT_H_STATIC)
#define _ASSERT_H_STATIC
#define static_assert _Static_assert
#endif
