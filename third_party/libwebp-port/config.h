/*
 * third_party/libwebp-port/config.h — libwebp's build configuration,
 * decided here instead of by ./configure.
 *
 * The Makefile copies this to build/webp/src/webp/config.h and compiles
 * with -DHAVE_CONFIG_H -Ibuild/webp, which is exactly the shape
 * upstream's own build has: every source that wants it says
 * `#include "src/webp/config.h"` and autotools passes
 * -I$(top_builddir). The path component matters — the include is
 * *quoted*, so it is looked for beside the includer first, finds
 * nothing there (no file called src/webp/config.h sits inside
 * src/utils/), and falls through to -I.
 *
 * ================================================================
 * the part that is not optional: the SIMD gate
 * ================================================================
 *
 * This is the only file in the port with a decision in it that changes
 * what code runs, and getting it half right is worse than turning it
 * off, so it is worth reading src/dsp/cpu.h:69-90 alongside.
 *
 * libwebp has two families of macro. WEBP_USE_SSE2 says "this
 * translation unit may contain SSE2 intrinsics", and is derived from
 * the compiler's own __SSE2__. WEBP_HAVE_SSE2 says "somewhere in this
 * *archive* there is an SSE2 implementation, so the dispatcher may call
 * VP8DspInitSSE2()". Upstream's rule is:
 *
 *     #if (defined(__SSE2__) || ...) && \
 *         (!defined(HAVE_CONFIG_H) || defined(WEBP_HAVE_SSE2))
 *     #define WEBP_USE_SSE2
 *     #endif
 *
 * — so the moment HAVE_CONFIG_H is defined, this file is *responsible*
 * for the answer. Leaving WEBP_HAVE_SSE2 out would silently compile
 * every SIMD kernel away even though the compiler was told -msse2, and
 * nothing would fail: the decoder would produce identical pixels, more
 * slowly, and the only evidence would be an archive that is smaller
 * than it should be.
 *
 * The three below are each paired with a compiler flag in the Makefile,
 * because a WEBP_HAVE_ without the matching -m is the failure that does
 * *not* stay quiet — the dispatcher would call an Init function whose
 * translation unit compiled to nothing, and the function would not be
 * in the archive:
 *
 *   WEBP_HAVE_SSE2    every object here is built -msse2, because all of
 *                     ring 3 is: APP_CFLAGS has carried -msse -msse2
 *                     -mfpmath=sse since the no-FPU rule was repealed.
 *   WEBP_HAVE_SSE41   the seven *_sse41.c files get -msse4.1.
 *   WEBP_HAVE_AVX2    the two *_avx2.c files get -mavx2.
 *
 * Which of the three actually runs is decided at run time, by CPUID,
 * in src/dsp/cpu.c — and CPUID is not a privileged instruction, so a
 * ring-3 process asks the hardware directly. AVX2 additionally requires
 * the *operating system* to have enabled the YMM state, which libwebp
 * checks with XGETBV. This kernel does not do that today, so the AVX2
 * kernels are compiled, shipped, and correctly not used; apps/webptest.c
 * reports what was detected rather than requiring an answer, because
 * either answer is correct and the report is the useful part.
 *
 * ================================================================
 * threads
 * ================================================================
 *
 * WEBP_USE_THREAD is on. src/utils/thread_utils.c wants
 * pthread_create, pthread_join, and the mutex and condition variable
 * pairs — eleven functions, all of which this C library has and
 * apps/threadtest.c exercises on every boot.
 *
 * It is worth knowing that WebKit cannot reach it: WEBPImageDecoder.cpp
 * drives the *incremental* decoder through WebPINewDecoder, which takes
 * no WebPDecoderConfig and therefore cannot set use_threads. So the
 * only thing in this tree that makes the setting real is section 7 of
 * apps/webptest.c, which decodes the same image with use_threads 0 and
 * 1 and requires the two results to be identical. A flag that is
 * compiled in and never exercised is not a flag that works.
 *
 * ================================================================
 * the rest
 * ================================================================
 */

#ifndef VEXTRO_LIBWEBP_CONFIG_H
#define VEXTRO_LIBWEBP_CONFIG_H

#define PACKAGE         "libwebp"
#define PACKAGE_NAME    "libwebp"
#define PACKAGE_TARNAME "libwebp"
#define PACKAGE_VERSION "1.6.0"
#define PACKAGE_STRING  "libwebp 1.6.0"
#define PACKAGE_URL     "https://developers.google.com/speed/webp"
#define PACKAGE_BUGREPORT "https://issues.webmproject.org"
#define VERSION         "1.6.0"

/* ---- the SIMD gate; see the long note above ---- */
#define WEBP_HAVE_SSE2  1
#define WEBP_HAVE_SSE41 1
#define WEBP_HAVE_AVX2  1

/* ---- threads ---- */
#define WEBP_USE_THREAD 1

/*
 * Byte swapping. src/utils/endian_inl_utils.h uses these to turn a
 * big-endian load into one instruction; without them it falls back to
 * shifts and ORs, which is correct and slower. GCC 16 has all three.
 */
#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1

/* ---- headers this C library has ---- */
#define HAVE_STDIO_H    1
#define HAVE_STDLIB_H   1
#define HAVE_STRING_H   1
#define HAVE_STRINGS_H  1
#define HAVE_STDINT_H   1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H   1
#define STDC_HEADERS    1

/*
 * Near-lossless encoding, which is upstream's default and is on here
 * for the same reason every other feature is: it is a quality mode of
 * the encoder rather than a dependency on anything, and turning it off
 * would be a divergence from the tarball to maintain for no gain.
 */
#define WEBP_NEAR_LOSSLESS 1

/*
 * ---- not defined, and each for its own reason ----
 *
 *   WORDS_BIGENDIAN
 *       x86-64 is little-endian. Left *undefined* rather than defined
 *       to 0, because src/dsp/dsp.h and six other files test it with
 *       #ifdef and not with #if — `#define WORDS_BIGENDIAN 0` would
 *       select every big-endian path in the library.
 *
 *   WEBP_HAVE_NEON, WEBP_HAVE_NEON_RTCD
 *       ARM. See [[arm-tree-frozen]] — this tree is x86 only.
 *
 *   HAVE_CPU_FEATURES_H
 *       Android's <cpu-features.h>, which is how libwebp does run-time
 *       NEON detection there. Not this machine, and not this
 *       architecture.
 *
 *   WEBP_HAVE_PNG, WEBP_HAVE_JPEG, WEBP_HAVE_TIFF, WEBP_HAVE_GIF,
 *   WEBP_HAVE_SDL, WEBP_HAVE_GL, HAVE_GLUT_GLUT_H, HAVE_GL_GLUT_H,
 *   HAVE_OPENGL_GLUT_H
 *       read only by examples/ — cwebp, dwebp, vwebp and friends —
 *       none of which is built here. libpng *is* ported and staged, so
 *       WEBP_HAVE_PNG could be answered truthfully; it is left out
 *       because the thing that reads it is not compiled, and a define
 *       that describes a program nobody builds is a claim with nothing
 *       behind it.
 *
 *   HAVE_DLFCN_H
 *       there is no dynamic linker here; libc/dlfcn.c is a name table.
 *       Nothing in src/ reads it — it is libtool's.
 *
 *   HAVE_SHLWAPI_H, HAVE_WINCODEC_H, HAVE_WINDOWS_H
 *       Windows.
 *
 *   HAVE_PTHREAD_PRIO_INHERIT
 *       configure probes for it and nothing in src/ reads the answer.
 *
 *   LT_OBJDIR
 *       libtool's, and there is no libtool in this build.
 */

#endif /* VEXTRO_LIBWEBP_CONFIG_H */
