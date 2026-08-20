#ifndef VX_FREESTANDING_INTTYPES_H
#define VX_FREESTANDING_INTTYPES_H
/*
 * third_party/include/inttypes.h — the format-string macros only.
 *
 * On LP64, uint32_t is unsigned int and uint64_t is unsigned long, which
 * is what fixes "u" against "lu" below. Getting these the wrong way
 * round is not a compile error; it is a printf that reads the wrong
 * number of bytes off the stack.
 */
#include <stdint.h>

#define PRId8  "d"
#define PRIu8  "u"
#define PRIx8  "x"
#define PRId16 "d"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"

#endif
