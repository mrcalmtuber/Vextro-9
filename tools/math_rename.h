/*
 * tools/math_rename.h — how libc/math.c is compiled for the host.
 *
 * The problem this solves is that a test which checks our exp() against
 * the host's exp() has to be able to name both, and in one program there
 * is only one symbol called exp. Forced into the compilation of math.c
 * with -include, this renames every function it defines, so the object
 * exports vx_exp, vx_log and so on, and the test can link against the
 * host's libm without a collision.
 *
 * Two other things have to be neutralised for the same compilation to
 * succeed away from its target:
 *
 * errno, because libc/errno.h declares it as a thread-local this
 * library defines and the host already has one of its own. Defining the
 * include guard up front makes our header expand to nothing and leaves
 * the two constants math.c actually uses.
 *
 * The include guard for math.h is *not* defined, deliberately: our
 * math.h is what declares the prototypes, and the renames have to apply
 * to the declaration and the definition alike or they will not match.
 */
#ifndef VX_MATH_RENAME_H
#define VX_MATH_RENAME_H

/* Our errno.h, skipped; the two values math.c sets. */
#define _ERRNO_H 1
extern int vx_math_errno;
#define errno vx_math_errno
#define EDOM   33
#define ERANGE 34

#define exp        vx_exp
#define exp2       vx_exp2
#define expm1      vx_expm1
#define log        vx_log
#define log2       vx_log2
#define log10      vx_log10
#define log1p      vx_log1p
#define pow        vx_pow
#define sqrt       vx_sqrt
#define cbrt       vx_cbrt
#define hypot      vx_hypot
#define sin        vx_sin
#define cos        vx_cos
#define tan        vx_tan
#define sincos     vx_sincos
#define asin       vx_asin
#define acos       vx_acos
#define atan       vx_atan
#define atan2      vx_atan2
#define sinh       vx_sinh
#define cosh       vx_cosh
#define tanh       vx_tanh
#define asinh      vx_asinh
#define acosh      vx_acosh
#define atanh      vx_atanh
#define fabs       vx_fabs
#define floor      vx_floor
#define ceil       vx_ceil
#define trunc      vx_trunc
#define round      vx_round
#define rint       vx_rint
#define nearbyint  vx_nearbyint
#define lround     vx_lround
#define lrint      vx_lrint
#define llround    vx_llround
#define llrint     vx_llrint
#define fmod       vx_fmod
#define remainder  vx_remainder
#define modf       vx_modf
#define frexp      vx_frexp
#define ldexp      vx_ldexp
#define scalbn     vx_scalbn
#define scalbln    vx_scalbln
#define ilogb      vx_ilogb
#define logb       vx_logb
#define copysign   vx_copysign
#define nan        vx_nan
#define nextafter  vx_nextafter
#define fdim       vx_fdim
#define fmin       vx_fmin
#define fmax       vx_fmax
#define fma        vx_fma
#define tgamma     vx_tgamma
#define lgamma     vx_lgamma
#define erf        vx_erf
#define erfc       vx_erfc

/*
 * The float and long double forms are renamed too, even though the test
 * only exercises a few of them. A partial rename would leave, say,
 * expf() defined here and colliding with the host's, and the failure
 * would be a duplicate symbol at link time with nothing to say which
 * half of the file caused it.
 */
#define expf vx_expf
#define exp2f vx_exp2f
#define expm1f vx_expm1f
#define logf vx_logf
#define log2f vx_log2f
#define log10f vx_log10f
#define log1pf vx_log1pf
#define powf vx_powf
#define sqrtf vx_sqrtf
#define cbrtf vx_cbrtf
#define hypotf vx_hypotf
#define sinf vx_sinf
#define cosf vx_cosf
#define tanf vx_tanf
#define asinf vx_asinf
#define acosf vx_acosf
#define atanf vx_atanf
#define atan2f vx_atan2f
#define sinhf vx_sinhf
#define coshf vx_coshf
#define tanhf vx_tanhf
#define asinhf vx_asinhf
#define acoshf vx_acoshf
#define atanhf vx_atanhf
#define fabsf vx_fabsf
#define floorf vx_floorf
#define ceilf vx_ceilf
#define truncf vx_truncf
#define roundf vx_roundf
#define rintf vx_rintf
#define nearbyintf vx_nearbyintf
#define fmodf vx_fmodf
#define remainderf vx_remainderf
#define modff vx_modff
#define frexpf vx_frexpf
#define ldexpf vx_ldexpf
#define scalbnf vx_scalbnf
#define copysignf vx_copysignf
#define nanf vx_nanf
#define nextafterf vx_nextafterf
#define fdimf vx_fdimf
#define fminf vx_fminf
#define fmaxf vx_fmaxf
#define fmaf vx_fmaf
#define tgammaf vx_tgammaf
#define lgammaf vx_lgammaf
#define erff vx_erff
#define erfcf vx_erfcf
#define ilogbf vx_ilogbf
#define logbf vx_logbf

#define fabsl vx_fabsl
#define sqrtl vx_sqrtl
#define floorl vx_floorl
#define ceill vx_ceill
#define truncl vx_truncl
#define roundl vx_roundl
#define rintl vx_rintl
#define nearbyintl vx_nearbyintl
#define fmodl vx_fmodl
#define remainderl vx_remainderl
#define expl vx_expl
#define expm1l vx_expm1l
#define logl vx_logl
#define log1pl vx_log1pl
#define log2l vx_log2l
#define log10l vx_log10l
#define powl vx_powl
#define sinl vx_sinl
#define cosl vx_cosl
#define tanl vx_tanl
#define asinl vx_asinl
#define acosl vx_acosl
#define atanl vx_atanl
#define atan2l vx_atan2l
#define sinhl vx_sinhl
#define coshl vx_coshl
#define tanhl vx_tanhl
#define asinhl vx_asinhl
#define acoshl vx_acoshl
#define atanhl vx_atanhl
#define cbrtl vx_cbrtl
#define hypotl vx_hypotl
#define tgammal vx_tgammal
#define lgammal vx_lgammal
#define ldexpl vx_ldexpl
#define scalbnl vx_scalbnl
#define frexpl vx_frexpl
#define modfl vx_modfl
#define copysignl vx_copysignl
#define nextafterl vx_nextafterl
#define fminl vx_fminl
#define fmaxl vx_fmaxl
#define fdiml vx_fdiml
#define fmal vx_fmal
#define ilogbl vx_ilogbl
#define logbl vx_logbl

#endif /* VX_MATH_RENAME_H */
