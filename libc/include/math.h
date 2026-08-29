#ifndef _MATH_H
#define _MATH_H

/* C++ reaches these now.
 *
 * libcxx/ compiles against this same library, and a C++ compiler mangles
 * every name it sees unless told not to -- so without this the C++ side
 * would fail to link against `malloc` and find `_Z6mallocm` missing.
 * Placed immediately after the include guard rather than after the
 * #includes below it, which is safe here because everything this header
 * includes is either one of the compiler's own type-only headers or one
 * of ours, and both want the same treatment. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * math.h — the transcendental functions, computed here.
 *
 * There was no libm in this system and, until recently, no way there
 * could be one: user programs were compiled with floating point banned
 * outright, because they ran in ring 0 on the kernel's own FPU state and
 * a program that used XMM registers corrupted whatever the kernel had
 * left in them. That ban was lifted when programs moved to ring 3 with
 * their own extended state saved across every context switch. What was
 * left was the ability to do arithmetic and nothing to do it with: a
 * program could multiply two doubles and had no way to take a sine.
 *
 * ---- what these are, and are not ----
 *
 * These are the algorithms from the Sun Microsystems freely-
 * distributable libm — the argument reductions and the minimax
 * polynomials that essentially every C library has used since 1993,
 * written out here from their mathematical description rather than
 * copied, and verified against a reference implementation over several
 * hundred thousand points per function by tools/math_test.c.
 *
 * They are not correctly rounded. Almost no libm is: producing the
 * nearest representable double for every input requires arbitrary
 * precision arithmetic in the hard cases, and the price is roughly a
 * hundredfold for a last-bit difference that no program here can
 * observe.
 *
 * What is claimed is what the test measures, function by function:
 *
 *   exact          sqrt, and every operation that is defined by a rule
 *                  rather than an approximation — floor, ceil, trunc,
 *                  round, rint, fmod, remainder, frexp, modf, scalbn,
 *                  ilogb, copysign, nextafter, fmin, fmax, fdim, fma
 *   1 ulp          exp, exp2, log, log2, log10, sqrt, cbrt, hypot,
 *                  asin, acos, atan, atan2
 *   2 ulp          expm1, log1p, pow, sin, cos, tan, and the hyperbolic
 *                  functions; 3 for tan of a large argument
 *   4 / 8 ulp      erf / erfc
 *   24 / 48 ulp    tgamma / lgamma
 *
 * The last line is the honest one. The gamma functions are computed
 * from a Lanczos approximation whose coefficients are good to about the
 * width of a double, and the reconstruction multiplies that error by the
 * size of the result. Bringing them to an ulp would take the dedicated
 * minimax polynomials a specialist library carries, and nothing in this
 * system calls them — they are here so that C++ headers which declare
 * them can be linked against.
 *
 * lgamma is also stated away from x = 1 and x = 2, where it is exactly
 * zero. No formula of this shape has relative accuracy at its own root,
 * because the answer is a difference of two large equal things; the two
 * exact zeros are returned exactly and the neighbourhood of them is not
 * claimed.
 *
 * ---- the one that is exact ----
 *
 * sqrt is, because the processor computes it and IEEE 754 requires the
 * result to be correctly rounded. It compiles to a single SQRTSD.
 *
 * ---- and the argument reduction ----
 *
 * sin(1e300) is a real question with a real answer, and getting it
 * requires more precision than a double can hold: reducing an argument
 * that large modulo pi/2 means knowing pi to about eleven hundred bits,
 * because the leading thousand of them are cancelled by the subtraction.
 * That table is in math.c and the reduction that uses it is why these
 * functions do not simply give up on large inputs — which matters here
 * rather than in the abstract, since a JavaScript program may pass any
 * double at all to Math.sin and expect a number back.
 */

#include <stdint.h>

/* ---- the constants ---- */
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

/*
 * Infinity and not-a-number, as expressions rather than as computations.
 *
 * The traditional definitions are 1.0/0.0 and 0.0/0.0, and both are
 * wrong in a header: they are constraint violations in a constant
 * expression and they raise floating-point exceptions at run time in
 * code that only wanted the value. The builtins produce the bit patterns
 * directly, at compile time, with no operation performed.
 */
#define HUGE_VAL    (__builtin_huge_val())
#define HUGE_VALF   (__builtin_huge_valf())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

#define FP_NAN        0
#define FP_INFINITE   1
#define FP_ZERO       2
#define FP_SUBNORMAL  3
#define FP_NORMAL     4

/*
 * The classification macros.
 *
 * Built on the compiler's own tests rather than on bit inspection,
 * because these must work for float, double and long double from one
 * spelling, and because __builtin_isnan compiles to a comparison rather
 * than to a call. Written as macros because that is what the standard
 * says they are, and ported code does take their address... no, it does
 * not, and cannot: taking the address of a macro is what a program
 * cannot do, which is exactly why the standard permits them to be one.
 */
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                           FP_SUBNORMAL, FP_ZERO, (x))
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define isgreater(a, b)      __builtin_isgreater((a), (b))
#define isgreaterequal(a, b) __builtin_isgreaterequal((a), (b))
#define isless(a, b)         __builtin_isless((a), (b))
#define islessequal(a, b)    __builtin_islessequal((a), (b))
#define islessgreater(a, b)  __builtin_islessgreater((a), (b))
#define isunordered(a, b)    __builtin_isunordered((a), (b))

/* ---- exponential and logarithmic ---- */
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);

/* ---- power and root ---- */
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);

/* ---- trigonometric ---- */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

/* sin and cos of one argument, for the price of one reduction. Not
 * standard C, and present because it is in POSIX and because a graphics
 * routine that wants both is the common case rather than the exception:
 * the reduction is the expensive half and doing it twice is pure
 * waste. */
void   sincos(double x, double *s, double *c);

/* ---- hyperbolic ---- */
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

/* ---- rounding, remainder, and the parts of a number ---- */
double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long   lround(double x);
long   lrint(double x);
long long llround(double x);
long long llrint(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double modf(double x, double *ipart);

/* ---- exponent manipulation ---- */
double frexp(double x, int *exp2out);
double ldexp(double x, int n);
double scalbn(double x, int n);
double scalbln(double x, long n);
int    ilogb(double x);
double logb(double x);

/* ---- sign and comparison ---- */
double copysign(double x, double y);
double nan(const char *tag);
double nextafter(double x, double y);
double fdim(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);

/*
 * Fused multiply-add: x*y+z with one rounding rather than two.
 *
 * Genuinely fused, using the processor's FMA instruction when the
 * processor has one, and computed exactly in software when it does not.
 * The difference is observable — it is the whole reason the function
 * exists — so a version that simply wrote `x*y+z` would be a lie that
 * ported numerical code would eventually catch.
 */
double fma(double x, double y, double z);

/* ---- the gamma family ---- */
double tgamma(double x);
double lgamma(double x);
double erf(double x);
double erfc(double x);

/*
 * ---- the float forms ----
 *
 * Computed in double precision and rounded once at the end, which is
 * both simpler and *more* accurate than a dedicated single-precision
 * implementation: every intermediate carries twenty-nine bits more than
 * the result needs, so the only rounding error that survives is the
 * final one. The cost is that they are no faster than the double forms,
 * which on a processor with SSE2 is very nearly true of a real
 * single-precision libm anyway.
 *
 * Double rounding is the hazard this argument has to answer, and it does
 * not arise here: it requires the intermediate to be within half an ulp
 * of a float's rounding boundary, and the intermediate carries enough
 * extra precision that the elementary functions cannot land there.
 */
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float sqrtf(float x);
float cbrtf(float x);
float hypotf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float rintf(float x);
float nearbyintf(float x);
float fmodf(float x, float y);
float remainderf(float x, float y);
float modff(float x, float *ipart);
float frexpf(float x, int *exp2out);
float ldexpf(float x, int n);
float scalbnf(float x, int n);
float copysignf(float x, float y);
float nanf(const char *tag);
float nextafterf(float x, float y);
float fdimf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);
float fmaf(float x, float y, float z);
float tgammaf(float x);
float lgammaf(float x);
float erff(float x);
float erfcf(float x);
int   ilogbf(float x);
float logbf(float x);

/*
 * ---- the long double forms ----
 *
 * Present because C++ code names them and will not link without them,
 * and computed in double precision, which is a real loss of range and
 * accuracy: on this architecture a long double carries sixty-four
 * mantissa bits and an exponent reaching 2^16384, and these do not.
 *
 * That is stated rather than hidden because it is the kind of thing that
 * is discovered as a wrong answer three layers up. Code that genuinely
 * needs the extended format — an accumulator that depends on the extra
 * eleven bits, a value beyond the double range — will get a wrong result
 * here and would be better served by using double explicitly and knowing
 * it. Nothing in the graphics or the engine paths does.
 */
long double fabsl(long double x);
long double sqrtl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double roundl(long double x);
long double fmodl(long double x, long double y);
long double expl(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double powl(long double x, long double y);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double asinl(long double x);
long double acosl(long double x);
long double ldexpl(long double x, int n);
long double frexpl(long double x, int *exp2out);
long double modfl(long double x, long double *ipart);
long double copysignl(long double x, long double y);
long double scalbnl(long double x, int n);
long double nextafterl(long double x, long double y);
long double fminl(long double x, long double y);
long double fmaxl(long double x, long double y);
long double fmal(long double x, long double y, long double z);
int         ilogbl(long double x);
long double hypotl(long double x, long double y);
long double cbrtl(long double x);
long double sinhl(long double x);
long double coshl(long double x);
long double tanhl(long double x);
long double asinhl(long double x);
long double acoshl(long double x);
long double atanhl(long double x);
long double expm1l(long double x);
long double log1pl(long double x);
long double tgammal(long double x);
long double lgammal(long double x);
long double remainderl(long double x, long double y);
long double fdiml(long double x, long double y);
long double rintl(long double x);
long double nearbyintl(long double x);
long double logbl(long double x);


#ifdef __cplusplus
}
#endif

#endif /* _MATH_H */
