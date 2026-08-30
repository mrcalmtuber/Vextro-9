/*
 * third_party/libjpeg-port/jconfigint.h — the internal half of the same
 * configuration. Upstream generates it from jconfigint.h.in; see the
 * note in jconfig.h beside it for why it is written out here instead.
 */

#define BUILD  "vextro"

/*
 * Symbol visibility.
 *
 * Upstream sets this to __attribute__((visibility("default"))) so that a
 * shared object exports the API and hides everything else. There are no
 * shared objects on this target — no runtime linker, nothing to resolve
 * one — so the attribute would decorate every symbol in a static archive
 * with a property nothing reads. Empty is the honest value.
 */
#define HIDDEN

/* Compiler's inline keyword */
#undef inline

#define INLINE  __inline__ __attribute__((always_inline))

/*
 * Thread-local storage.
 *
 * `__thread` works here and is not a formality: libc/pthread.c sets a
 * per-thread FS base and SYS_SET_FSBASE restores it on every context
 * switch, which is what makes the storage per-thread rather than
 * per-machine. The compiler is also told -ftls-model=initial-exec
 * everywhere, because the general-dynamic sequence calls
 * __tls_get_addr through a module table that exists to support shared
 * libraries and there are none.
 */
#define THREAD_LOCAL  __thread

#define PACKAGE_NAME  "libjpeg-turbo"
#define VERSION  "3.0.4"
#define SIZEOF_SIZE_T  8

/* GCC has had it since 3.4, and sizeof(unsigned long) == sizeof(size_t)
 * on LP64. Used by the Huffman encoder's bit counter. */
#define HAVE_BUILTIN_CTZL

/* MSVC's; not this compiler's. */
/* #undef HAVE_INTRIN_H */

#if defined(_MSC_VER) && defined(HAVE_INTRIN_H)
#if (SIZEOF_SIZE_T == 8)
#define HAVE_BITSCANFORWARD64
#elif (SIZEOF_SIZE_T == 4)
#define HAVE_BITSCANFORWARD
#endif
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD
