/*
 * third_party/libjpeg-port/jconfig.h — libjpeg-turbo's build
 * configuration, written by hand instead of generated.
 *
 * Upstream produces this from jconfig.h.in with configure_file() during
 * its own CMake run. This system does not run that CMake: the library is
 * compiled by the main Makefile, with the same cross compiler and the
 * same flags as everything else in ring 3, so the sixteen substitutions
 * CMake would have made are made here once.
 *
 * The same arrangement FreeType is under, and for the same reason —
 * `make webkit-sysroot` copies these over the upstream tree's own copies
 * so that the headers in the sysroot describe the archive beside them.
 * Nothing under third_party/libjpeg/ is modified.
 *
 * Each value below is a decision, not a transcription:
 */

/* Version ID for the JPEG library. FindJPEG.cmake greps this file and
 * jpeglib.h for exactly this line and fails the package without it, so
 * it is load-bearing twice over: the API level the headers describe,
 * and the string a configure run reads back. */
#define JPEG_LIB_VERSION  62

/* libjpeg-turbo version */
#define LIBJPEG_TURBO_VERSION  3.0.4

/* libjpeg-turbo version in integer form */
#define LIBJPEG_TURBO_VERSION_NUMBER  3000004

/*
 * Arithmetic coding, off.
 *
 * It is patent-free now and it is also not what JPEG files on the web
 * use — Huffman is universal and arithmetic is vanishingly rare. Leaving
 * it out drops two source files and, more to the point, keeps the
 * decoder's entropy path down to the one that will actually run.
 */
/* #undef C_ARITH_CODING_SUPPORTED */
/* #undef D_ARITH_CODING_SUPPORTED */

/*
 * In-memory source and destination managers, on.
 *
 * This is the one a browser needs and the reason it is not left at the
 * default: WebKit hands the decoder a buffer it already has in memory
 * rather than a FILE*, through jpeg_mem_src(). Without this the only
 * source manager is the stdio one.
 */
#define MEM_SRCDST_SUPPORTED  1

/*
 * SIMD, off, and this is the substantive decision in the file.
 *
 * libjpeg-turbo's reason for existing is its hand-written SSE2/AVX2
 * assembly, and turning it off gives up most of the speed the name
 * promises. It is off because the SIMD build is driven by NASM through
 * upstream's own CMake, assembles into objects this Makefile has no rule
 * for, and selects between kernels at run time by reading CPUID and
 * getenv — none of which is reachable from here. jsimd_none.c is the
 * portable path upstream ships for exactly this case, and it is the
 * whole of the difference: correctness is identical, throughput is not.
 */
/* #undef WITH_SIMD */

/* Run-time selection of data precision means this no longer chooses the
 * build's precision — it says what the unmodified libjpeg API behaves
 * as, which is 8-bit, and some downstream software still reads it. */
#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

/* x86-64 shifts signed values arithmetically, which is what the library
 * wants; the workaround is for compilers that do not. */
/* #undef RIGHT_SHIFT_IS_UNSIGNED */
