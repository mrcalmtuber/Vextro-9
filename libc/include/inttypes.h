#ifndef _INTTYPES_H
#define _INTTYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * inttypes.h — how to print a fixed-width integer.
 *
 * <stdint.h> gives the types; this gives the format strings for them,
 * and the reason both exist is that neither `%d` nor `%ld` is portably
 * correct for an int64_t. On this target int64_t is `long`, so PRId64 is
 * "ld" — but a library that wrote "%ld" directly would be wrong on every
 * 32-bit machine, and the code being ported is written to run on both.
 *
 * Nothing in this system needed it until HarfBuzz did. That is worth
 * recording: the amalgamation compiled against this C library with
 * exactly one error, and it was the absence of this file rather than
 * anything about the shaping engine.
 *
 * ---- the widths, on this machine ----
 *
 * LP64: int is 32 bits, long and pointers are 64. So the 64-bit macros
 * are "l"-prefixed and the 32-bit ones are bare, which is what the
 * definitions below say and is the only reason they are short enough to
 * read.
 */

#include <stdint.h>
#include <stddef.h>

/* ===== printing ===== */

#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "ld"

#define PRIi8    "i"
#define PRIi16   "i"
#define PRIi32   "i"
#define PRIi64   "li"

#define PRIo8    "o"
#define PRIo16   "o"
#define PRIo32   "o"
#define PRIo64   "lo"

#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "lu"

#define PRIx8    "x"
#define PRIx16   "x"
#define PRIx32   "x"
#define PRIx64   "lx"

#define PRIX8    "X"
#define PRIX16   "X"
#define PRIX32   "X"
#define PRIX64   "lX"

/* The least- and fast- variants are the same types here: there is one
 * integer of each width on this machine and no reason for a "fast" form
 * to differ from it. */
#define PRIdLEAST8   PRId8
#define PRIdLEAST16  PRId16
#define PRIdLEAST32  PRId32
#define PRIdLEAST64  PRId64
#define PRIuLEAST8   PRIu8
#define PRIuLEAST16  PRIu16
#define PRIuLEAST32  PRIu32
#define PRIuLEAST64  PRIu64
#define PRIxLEAST8   PRIx8
#define PRIxLEAST16  PRIx16
#define PRIxLEAST32  PRIx32
#define PRIxLEAST64  PRIx64
#define PRIoLEAST8   PRIo8
#define PRIoLEAST16  PRIo16
#define PRIoLEAST32  PRIo32
#define PRIoLEAST64  PRIo64
#define PRIiLEAST8   PRIi8
#define PRIiLEAST16  PRIi16
#define PRIiLEAST32  PRIi32
#define PRIiLEAST64  PRIi64
#define PRIXLEAST8   PRIX8
#define PRIXLEAST16  PRIX16
#define PRIXLEAST32  PRIX32
#define PRIXLEAST64  PRIX64

#define PRIdFAST8    PRId8
#define PRIdFAST16   PRId64
#define PRIdFAST32   PRId64
#define PRIdFAST64   PRId64
#define PRIuFAST8    PRIu8
#define PRIuFAST16   PRIu64
#define PRIuFAST32   PRIu64
#define PRIuFAST64   PRIu64
#define PRIxFAST8    PRIx8
#define PRIxFAST16   PRIx64
#define PRIxFAST32   PRIx64
#define PRIxFAST64   PRIx64
#define PRIoFAST8    PRIo8
#define PRIoFAST16   PRIo64
#define PRIoFAST32   PRIo64
#define PRIoFAST64   PRIo64
#define PRIiFAST8    PRIi8
#define PRIiFAST16   PRIi64
#define PRIiFAST32   PRIi64
#define PRIiFAST64   PRIi64
#define PRIXFAST8    PRIX8
#define PRIXFAST16   PRIX64
#define PRIXFAST32   PRIX64
#define PRIXFAST64   PRIX64

#define PRIdMAX  "ld"
#define PRIiMAX  "li"
#define PRIoMAX  "lo"
#define PRIuMAX  "lu"
#define PRIxMAX  "lx"
#define PRIXMAX  "lX"

#define PRIdPTR  "ld"
#define PRIiPTR  "li"
#define PRIoPTR  "lo"
#define PRIuPTR  "lu"
#define PRIxPTR  "lx"
#define PRIXPTR  "lX"

/* ===== reading =====
 *
 * The scanf family reads these; libc/stdio.c understands the `l` length
 * modifier they expand to.
 */

#define SCNd8    "hhd"
#define SCNd16   "hd"
#define SCNd32   "d"
#define SCNd64   "ld"

#define SCNi8    "hhi"
#define SCNi16   "hi"
#define SCNi32   "i"
#define SCNi64   "li"

#define SCNo8    "hho"
#define SCNo16   "ho"
#define SCNo32   "o"
#define SCNo64   "lo"

#define SCNu8    "hhu"
#define SCNu16   "hu"
#define SCNu32   "u"
#define SCNu64   "lu"

#define SCNx8    "hhx"
#define SCNx16   "hx"
#define SCNx32   "x"
#define SCNx64   "lx"

#define SCNdMAX  "ld"
#define SCNiMAX  "li"
#define SCNoMAX  "lo"
#define SCNuMAX  "lu"
#define SCNxMAX  "lx"

#define SCNdPTR  "ld"
#define SCNiPTR  "li"
#define SCNoPTR  "lo"
#define SCNuPTR  "lu"
#define SCNxPTR  "lx"

/* ===== the four functions ===== */

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t  imaxabs(intmax_t v);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);

intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif

#endif /* _INTTYPES_H */
