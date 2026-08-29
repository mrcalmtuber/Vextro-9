/*
 * tools/math_test.c — is libc/math.c right?
 *
 * There is no way to answer that from inside this system. A kernel that
 * has just written its own exp() cannot check exp() against anything,
 * because the only other implementation of exp() available to it is the
 * one being checked. So the check happens on the host, where a reference
 * libm exists, and libc/math.c is compiled a second time with every
 * function renamed (see tools/math_rename.h) so that both can be linked
 * into one program and called side by side.
 *
 * ---- what "correct" means here ----
 *
 * Not "equal". Two libms that both round correctly still disagree,
 * because neither is required to be correctly rounded and both are
 * allowed a fraction of an ulp — a unit in the last place, the distance
 * between adjacent representable doubles at that magnitude. So the test
 * measures the difference in ulps and asserts a bound.
 *
 * The bound is one ulp for the elementary functions, which is what glibc
 * claims and what these algorithms deliver, and a handful for the gamma
 * and error functions, which are computed here by series rather than by
 * the minimax tables a specialist implementation would use. Each bound
 * is stated at its call site rather than applied globally, so that a
 * function which quietly gets worse is caught by the number next to it.
 *
 * ---- where the arguments come from ----
 *
 * Three sources, and the third is the one that finds things. Uniform
 * samples across the range check the ordinary case. Exact values at the
 * boundaries check the special cases the standard specifies. And samples
 * clustered at every point where a reduction changes branch — 0.5*ln2
 * for exp, sqrt(2)/2 for log, each multiple of pi/4 for the
 * trigonometric functions, 7/16 and 11/16 and 19/16 for atan — check the
 * seams, which is where an argument reduction is wrong if it is wrong
 * anywhere.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

int vx_math_errno;

/* The implementation under test. Declared by hand rather than by
 * including our own math.h, which would rename these back. */
double vx_exp(double), vx_exp2(double), vx_expm1(double);
double vx_log(double), vx_log2(double), vx_log10(double), vx_log1p(double);
double vx_pow(double, double);
double vx_sqrt(double), vx_cbrt(double), vx_hypot(double, double);
double vx_sin(double), vx_cos(double), vx_tan(double);
double vx_asin(double), vx_acos(double), vx_atan(double);
double vx_atan2(double, double);
double vx_sinh(double), vx_cosh(double), vx_tanh(double);
double vx_asinh(double), vx_acosh(double), vx_atanh(double);
double vx_floor(double), vx_ceil(double), vx_trunc(double), vx_round(double);
double vx_rint(double), vx_fmod(double, double), vx_remainder(double, double);
double vx_modf(double, double *), vx_frexp(double, int *);
double vx_scalbn(double, int), vx_copysign(double, double);
double vx_nextafter(double, double), vx_fdim(double, double);
double vx_fmin(double, double), vx_fmax(double, double);
double vx_fma(double, double, double);
double vx_tgamma(double), vx_lgamma(double), vx_erf(double), vx_erfc(double);
double vx_logb(double);
int    vx_ilogb(double);
void   vx_sincos(double, double *, double *);
float  vx_sinf(float), vx_expf(float), vx_sqrtf(float);

static int failures = 0;
static int checks = 0;

/* ---- ulp distance ----
 *
 * The number of representable doubles between a and b. Computed on the
 * bit patterns, which for IEEE 754 are ordered the same way the values
 * are within a sign — that ordering is a design property of the format
 * and is what makes this two subtractions rather than a logarithm.
 */
static double ulp_diff(double a, double b) {
    if (a == b) return 0.0;
    if (isnan(a) && isnan(b)) return 0.0;
    if (isnan(a) || isnan(b)) return 1e18;
    if (isinf(a) || isinf(b)) return 1e18;

    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    /* Map the sign-magnitude layout onto a monotonic integer, so that
     * the two sides of zero are adjacent rather than a whole exponent
     * range apart. */
    if (ia < 0) ia = (int64_t)0x8000000000000000ll - ia;
    if (ib < 0) ib = (int64_t)0x8000000000000000ll - ib;
    int64_t d = ia - ib;
    return (double)(d < 0 ? -d : d);
}

static double worst;
static double worst_at;

static void chk1(const char *name, double (*mine)(double),
                 double (*ref)(double), double x, double bound) {
    checks++;
    double a = mine(x), b = ref(x);
    double u = ulp_diff(a, b);
    if (u > worst) { worst = u; worst_at = x; }
    if (u > bound) {
        if (failures < 12)
            printf("  FAIL %-10s(%.17g): got %.17g want %.17g  (%.1f ulp)\n",
                   name, x, a, b, u);
        failures++;
    }
}

static void chk2(const char *name, double (*mine)(double, double),
                 double (*ref)(double, double), double x, double y,
                 double bound) {
    checks++;
    double a = mine(x, y), b = ref(x, y);
    double u = ulp_diff(a, b);
    if (u > worst) { worst = u; worst_at = x; }
    if (u > bound) {
        if (failures < 12)
            printf("  FAIL %-10s(%.17g, %.17g): got %.17g want %.17g  (%.1f ulp)\n",
                   name, x, y, a, b, u);
        failures++;
    }
}

/* A deterministic generator, so a failure found on one machine is
 * reproducible on another. */
static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* A uniform double in [lo, hi]. */
static double runif(double lo, double hi) {
    double t = (double)(rnd() >> 11) * 0x1p-53;
    return lo + t * (hi - lo);
}

/* A double drawn uniformly in the *exponent*, which is how a numerical
 * argument is actually distributed: sampling linearly in [0, 1e300]
 * produces nothing below 1e280. */
static double rlog(double lo, double hi) {
    double l = log(lo), h = log(hi);
    return exp(l + (h - l) * ((double)(rnd() >> 11) * 0x1p-53));
}

static void section(const char *s) {
    printf("%s\n", s);
    worst = 0; worst_at = 0;
}

static void report(void) {
    printf("    worst %.1f ulp at %.17g\n", worst, worst_at);
}

int main(void) {
    printf("math: libc/math.c against the host libm\n\n");

    /* ===== the exponential family ===== */
    section("  exp");
    for (int i = 0; i < 60000; i++) chk1("exp", vx_exp, exp, runif(-700, 700), 1.0);
    for (int i = 0; i < 20000; i++) chk1("exp", vx_exp, exp, runif(-1, 1), 1.0);
    /* The seam: |x| = 0.5*ln2 is where the reduction starts happening. */
    for (int i = 0; i < 20000; i++)
        chk1("exp", vx_exp, exp, 0.34657359027997264 + runif(-1e-9, 1e-9), 1.0);
    chk1("exp", vx_exp, exp, 0.0, 0.0);
    chk1("exp", vx_exp, exp, 709.78, 1.0);
    chk1("exp", vx_exp, exp, -745.0, 1.0);
    chk1("exp", vx_exp, exp, -1000.0, 0.0);
    report();

    section("  exp2, expm1");
    for (int i = 0; i < 40000; i++) chk1("exp2", vx_exp2, exp2, runif(-1020, 1020), 1.0);
    for (int i = 0; i < 20000; i++) chk1("exp2", vx_exp2, exp2, runif(-1, 1), 1.0);
    for (int i = 0; i < 40000; i++) chk1("expm1", vx_expm1, expm1, runif(-50, 50), 2.0);
    for (int i = 0; i < 40000; i++) chk1("expm1", vx_expm1, expm1, runif(-1e-8, 1e-8), 1.0);
    for (int i = 0; i < 20000; i++) chk1("expm1", vx_expm1, expm1, runif(-0.3, 0.3), 2.0);
    report();

    /* ===== the logarithms ===== */
    section("  log");
    for (int i = 0; i < 60000; i++) chk1("log", vx_log, log, rlog(1e-300, 1e300), 1.0);
    for (int i = 0; i < 20000; i++) chk1("log", vx_log, log, runif(0.5, 2.0), 1.0);
    /* The seam: sqrt(2)/2 and sqrt(2), where the mantissa interval is
     * split so that |f| stays small. */
    for (int i = 0; i < 20000; i++)
        chk1("log", vx_log, log, 0.70710678118654752 + runif(-1e-9, 1e-9), 1.0);
    for (int i = 0; i < 20000; i++)
        chk1("log", vx_log, log, 1.0 + runif(-1e-9, 1e-9), 1.0);
    chk1("log", vx_log, log, 1.0, 0.0);
    /* Subnormals, which have to be scaled up before the exponent is
     * taken or every one of them reduces to the same value. */
    for (int i = 0; i < 5000; i++) chk1("log", vx_log, log, rlog(1e-320, 1e-308), 1.0);
    report();

    section("  log2, log10, log1p");
    for (int i = 0; i < 40000; i++) chk1("log2", vx_log2, log2, rlog(1e-300, 1e300), 1.0);
    for (int i = 0; i < 40000; i++) chk1("log10", vx_log10, log10, rlog(1e-300, 1e300), 2.0);
    /* Exact powers must come out exact, which is the entire reason log2
     * and log10 are not log(x)*constant. */
    for (int k = -300; k <= 300; k++) {
        double want = (double)k;
        double got = vx_log2(ldexp(1.0, k));
        checks++;
        if (got != want) {
            printf("  FAIL log2(2^%d) = %.17g, not exact\n", k, got);
            failures++;
        }
    }
    for (int k = -300; k <= 300; k++) {
        double got = vx_log10(pow(10.0, (double)k));
        checks++;
        if (ulp_diff(got, (double)k) > 1.0) {
            printf("  FAIL log10(1e%d) = %.17g\n", k, got);
            failures++;
        }
    }
    for (int i = 0; i < 40000; i++) chk1("log1p", vx_log1p, log1p, runif(-0.9, 10.0), 2.0);
    for (int i = 0; i < 40000; i++) chk1("log1p", vx_log1p, log1p, runif(-1e-10, 1e-10), 1.0);
    report();

    /* ===== pow, which has more special cases than arithmetic ===== */
    section("  pow");
    for (int i = 0; i < 60000; i++)
        chk2("pow", vx_pow, pow, rlog(1e-40, 1e40), runif(-40, 40), 2.0);
    for (int i = 0; i < 20000; i++)
        chk2("pow", vx_pow, pow, runif(0.1, 10.0), runif(-100, 100), 2.0);
    for (int i = 0; i < 20000; i++)
        chk2("pow", vx_pow, pow, runif(-10, -0.1), (double)(int)runif(-20, 20), 2.0);
    /* Integer powers, which the sign rules make interesting for a
     * negative base and which JavaScript exercises constantly. */
    for (int b = -20; b <= 20; b++)
        for (int e = -8; e <= 8; e++)
            chk2("pow", vx_pow, pow, (double)b, (double)e, 2.0);
    /* And every clause the standard spells out. */
    struct { double x, y; } sp[] = {
        {1.0, NAN}, {NAN, 0.0}, {-1.0, INFINITY}, {-1.0, -INFINITY},
        {0.0, -1.0}, {-0.0, -1.0}, {0.0, -2.0}, {-0.0, -3.0},
        {0.0, 3.0}, {-0.0, 3.0}, {0.0, 2.0}, {-0.0, 2.0},
        {INFINITY, 2.0}, {-INFINITY, 3.0}, {-INFINITY, 2.0},
        {-INFINITY, -3.0}, {2.0, INFINITY}, {0.5, INFINITY},
        {2.0, -INFINITY}, {0.5, -INFINITY}, {-2.0, 0.5},
        {4.0, 0.5}, {2.0, 1.0}, {2.0, -1.0}, {2.0, 2.0},
        {1e308, 2.0}, {1e-308, 2.0}, {0.99999, 1e9}, {1.00001, 1e9},
    };
    for (unsigned i = 0; i < sizeof(sp) / sizeof(sp[0]); i++)
        chk2("pow", vx_pow, pow, sp[i].x, sp[i].y, 2.0);
    report();

    /* ===== roots ===== */
    section("  sqrt, cbrt, hypot");
    for (int i = 0; i < 40000; i++) chk1("sqrt", vx_sqrt, sqrt, rlog(1e-300, 1e300), 0.0);
    for (int i = 0; i < 40000; i++) chk1("cbrt", vx_cbrt, cbrt, rlog(1e-300, 1e300), 1.0);
    for (int i = 0; i < 10000; i++) chk1("cbrt", vx_cbrt, cbrt, -rlog(1e-10, 1e10), 1.0);
    chk1("cbrt", vx_cbrt, cbrt, 0.0, 0.0);
    chk1("cbrt", vx_cbrt, cbrt, 8.0, 0.0);
    chk1("cbrt", vx_cbrt, cbrt, 27.0, 0.0);
    for (int i = 0; i < 40000; i++)
        chk2("hypot", vx_hypot, hypot, rlog(1e-150, 1e150), rlog(1e-150, 1e150), 1.0);
    /* The whole reason hypot exists: these overflow if squared directly. */
    chk2("hypot", vx_hypot, hypot, 1e300, 1e300, 1.0);
    chk2("hypot", vx_hypot, hypot, 1e-300, 1e-300, 1.0);
    chk2("hypot", vx_hypot, hypot, 3.0, 4.0, 0.0);
    report();

    /* ===== the trigonometric functions ===== */
    section("  sin, cos, tan (small and medium)");
    for (int i = 0; i < 60000; i++) chk1("sin", vx_sin, sin, runif(-M_PI, M_PI), 1.0);
    for (int i = 0; i < 60000; i++) chk1("cos", vx_cos, cos, runif(-M_PI, M_PI), 1.0);
    for (int i = 0; i < 60000; i++) chk1("tan", vx_tan, tan, runif(-M_PI/2, M_PI/2), 2.0);
    for (int i = 0; i < 40000; i++) chk1("sin", vx_sin, sin, runif(-1000, 1000), 1.0);
    for (int i = 0; i < 40000; i++) chk1("cos", vx_cos, cos, runif(-1000, 1000), 1.0);
    for (int i = 0; i < 40000; i++) chk1("tan", vx_tan, tan, runif(-1000, 1000), 2.0);
    /* Every seam in the reduction: the multiples of pi/4 where the
     * quadrant changes and the kernels swap over. */
    for (int q = -40; q <= 40; q++) {
        double c = (double)q * M_PI_4;
        for (int i = 0; i < 400; i++) {
            double x = c + runif(-1e-8, 1e-8);
            chk1("sin", vx_sin, sin, x, 1.0);
            chk1("cos", vx_cos, cos, x, 1.0);
        }
    }
    /* And the two branch points of the reduction itself: 3*pi/4, where
     * one-step Cody-Waite gives way to the general form, and 2^20*pi/2,
     * where Cody-Waite gives way to Payne-Hanek. */
    for (int i = 0; i < 20000; i++) {
        double x = 2.356194490192345 + runif(-1e-9, 1e-9);
        chk1("sin", vx_sin, sin, x, 1.0);
        chk1("cos", vx_cos, cos, x, 1.0);
    }
    report();

    section("  sin, cos (large: Payne-Hanek)");
    for (int i = 0; i < 40000; i++) {
        double x = rlog(1e7, 1e300);
        chk1("sin", vx_sin, sin, x, 2.0);
        chk1("cos", vx_cos, cos, x, 2.0);
    }
    for (int i = 0; i < 20000; i++) {
        double x = -rlog(1e7, 1e300);
        chk1("sin", vx_sin, sin, x, 2.0);
        chk1("cos", vx_cos, cos, x, 2.0);
    }
    /* The classic hard cases: arguments that sit extraordinarily close
     * to a multiple of pi/2, where almost every bit cancels and the
     * answer depends on the far tail of the 2/pi expansion. */
    double hard[] = {
        1e22, 1e18, 1e100, 1e200, 1e300,
        6381956970095103.0 * 0x1p797,      /* the canonical worst case */
        2.2250738585072014e-308,
        5.31937264832654141671e+255,
        1.0e17, 123456789.0, 1e15 + 0.5,
        0x1p1023, 0x1.fffffffffffffp1023,
    };
    for (unsigned i = 0; i < sizeof(hard) / sizeof(hard[0]); i++) {
        chk1("sin", vx_sin, sin, hard[i], 2.0);
        chk1("cos", vx_cos, cos, hard[i], 2.0);
        chk1("sin", vx_sin, sin, -hard[i], 2.0);
    }
    /* The boundary between the two reduction methods, walked densely. */
    for (int i = 0; i < 20000; i++) {
        double x = 1647099.0 + runif(-40.0, 40.0);
        chk1("sin", vx_sin, sin, x, 2.0);
        chk1("cos", vx_cos, cos, x, 2.0);
        chk1("tan", vx_tan, tan, x, 3.0);
    }
    report();

    section("  sincos agrees with sin and cos");
    for (int i = 0; i < 40000; i++) {
        double x = runif(-1e6, 1e6);
        double s, c;
        vx_sincos(x, &s, &c);
        checks += 2;
        if (s != vx_sin(x) || c != vx_cos(x)) {
            if (failures < 12)
                printf("  FAIL sincos(%.17g) disagrees with sin/cos\n", x);
            failures++;
        }
    }
    report();

    /* ===== the inverse trigonometric functions ===== */
    section("  asin, acos, atan, atan2");
    for (int i = 0; i < 60000; i++) chk1("asin", vx_asin, asin, runif(-1, 1), 1.0);
    for (int i = 0; i < 60000; i++) chk1("acos", vx_acos, acos, runif(-1, 1), 1.0);
    /* The seams: 0.5 where the identity kicks in, and 0.975 where the
     * two forms of the reconstruction swap. */
    for (int i = 0; i < 20000; i++) {
        chk1("asin", vx_asin, asin, 0.5 + runif(-1e-9, 1e-9), 1.0);
        chk1("asin", vx_asin, asin, 0.975 + runif(-1e-9, 1e-9), 1.0);
        chk1("acos", vx_acos, acos, 0.5 + runif(-1e-9, 1e-9), 1.0);
        chk1("acos", vx_acos, acos, -0.5 + runif(-1e-9, 1e-9), 1.0);
    }
    chk1("asin", vx_asin, asin, 1.0, 0.0);
    chk1("asin", vx_asin, asin, -1.0, 0.0);
    chk1("acos", vx_acos, acos, 1.0, 0.0);
    chk1("acos", vx_acos, acos, -1.0, 0.0);
    for (int i = 0; i < 60000; i++) chk1("atan", vx_atan, atan, rlog(1e-20, 1e20), 1.0);
    for (int i = 0; i < 20000; i++) chk1("atan", vx_atan, atan, -rlog(1e-20, 1e20), 1.0);
    /* atan's four anchor points. */
    double anchors[] = { 0.4375, 0.6875, 1.1875, 2.4375 };
    for (unsigned a = 0; a < 4; a++)
        for (int i = 0; i < 5000; i++)
            chk1("atan", vx_atan, atan, anchors[a] + runif(-1e-9, 1e-9), 1.0);
    for (int i = 0; i < 60000; i++)
        chk2("atan2", vx_atan2, atan2, runif(-100, 100), runif(-100, 100), 1.0);
    for (int i = 0; i < 20000; i++)
        chk2("atan2", vx_atan2, atan2, rlog(1e-200, 1e200) * (rnd() & 1 ? 1 : -1),
             rlog(1e-200, 1e200) * (rnd() & 1 ? 1 : -1), 1.0);
    /* The eight quadrant and axis cases, plus the infinities. */
    double q[] = { 0.0, -0.0, 1.0, -1.0, INFINITY, -INFINITY };
    for (unsigned i = 0; i < 6; i++)
        for (unsigned j = 0; j < 6; j++)
            chk2("atan2", vx_atan2, atan2, q[i], q[j], 1.0);
    report();

    /* ===== the hyperbolic functions ===== */
    section("  sinh, cosh, tanh, asinh, acosh, atanh");
    for (int i = 0; i < 40000; i++) chk1("sinh", vx_sinh, sinh, runif(-710, 710), 2.0);
    for (int i = 0; i < 20000; i++) chk1("sinh", vx_sinh, sinh, runif(-1, 1), 2.0);
    for (int i = 0; i < 40000; i++) chk1("cosh", vx_cosh, cosh, runif(-710, 710), 2.0);
    for (int i = 0; i < 40000; i++) chk1("tanh", vx_tanh, tanh, runif(-30, 30), 2.0);
    for (int i = 0; i < 20000; i++) chk1("tanh", vx_tanh, tanh, runif(-1e-8, 1e-8), 1.0);
    for (int i = 0; i < 40000; i++) chk1("asinh", vx_asinh, asinh, rlog(1e-20, 1e20), 2.0);
    for (int i = 0; i < 20000; i++) chk1("asinh", vx_asinh, asinh, -rlog(1e-20, 1e20), 2.0);
    for (int i = 0; i < 40000; i++) chk1("acosh", vx_acosh, acosh, 1.0 + rlog(1e-15, 1e15), 2.0);
    for (int i = 0; i < 40000; i++) chk1("atanh", vx_atanh, atanh, runif(-0.999999, 0.999999), 2.0);
    chk1("acosh", vx_acosh, acosh, 1.0, 0.0);
    chk1("atanh", vx_atanh, atanh, 0.0, 0.0);
    report();

    /* ===== rounding and decomposition =====
     *
     * These must be bit-exact, not merely close: they are defined by
     * exact rules rather than by approximation, and a floor() that is
     * out by one ulp is out by a whole integer.
     */
    section("  floor, ceil, trunc, round, rint (exact)");
    for (int i = 0; i < 100000; i++) {
        double x = runif(-1e17, 1e17);
        chk1("floor", vx_floor, floor, x, 0.0);
        chk1("ceil", vx_ceil, ceil, x, 0.0);
        chk1("trunc", vx_trunc, trunc, x, 0.0);
        chk1("round", vx_round, round, x, 0.0);
        chk1("rint", vx_rint, rint, x, 0.0);
    }
    for (int i = 0; i < 40000; i++) {
        double x = runif(-4, 4);
        chk1("floor", vx_floor, floor, x, 0.0);
        chk1("ceil", vx_ceil, ceil, x, 0.0);
        chk1("round", vx_round, round, x, 0.0);
        chk1("rint", vx_rint, rint, x, 0.0);
    }
    /* Halfway cases, where round and rint deliberately disagree. */
    double half[] = { 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5,
                      0.49999999999999994, -0.49999999999999994,
                      4503599627370495.5, -4503599627370495.5 };
    for (unsigned i = 0; i < sizeof(half) / sizeof(half[0]); i++) {
        chk1("round", vx_round, round, half[i], 0.0);
        chk1("rint", vx_rint, rint, half[i], 0.0);
        chk1("floor", vx_floor, floor, half[i], 0.0);
        chk1("ceil", vx_ceil, ceil, half[i], 0.0);
    }
    /* Zeros keep their sign, which a plain comparison cannot see. */
    checks += 4;
    if (!signbit(vx_floor(-0.0)) || !signbit(vx_ceil(-0.0)) ||
        !signbit(vx_trunc(-0.0)) || !signbit(vx_rint(-0.0))) {
        printf("  FAIL rounding lost the sign of a negative zero\n");
        failures++;
    }
    checks++;
    if (!signbit(vx_ceil(-0.3))) {
        printf("  FAIL ceil(-0.3) should be -0.0\n");
        failures++;
    }
    report();

    section("  fmod, remainder (exact)");
    for (int i = 0; i < 100000; i++) {
        double x = rlog(1e-100, 1e100) * (rnd() & 1 ? 1 : -1);
        double y = rlog(1e-100, 1e100) * (rnd() & 1 ? 1 : -1);
        chk2("fmod", vx_fmod, fmod, x, y, 0.0);
    }
    for (int i = 0; i < 40000; i++)
        chk2("fmod", vx_fmod, fmod, runif(-100, 100), runif(-10, 10), 0.0);
    for (int i = 0; i < 40000; i++)
        chk2("remainder", vx_remainder, remainder, runif(-100, 100), runif(-10, 10), 0.0);
    /* Subnormal divisors, which need the renormalising loop. */
    for (int i = 0; i < 5000; i++)
        chk2("fmod", vx_fmod, fmod, runif(-1e-300, 1e-300), rlog(1e-320, 1e-310), 0.0);
    chk2("fmod", vx_fmod, fmod, 5.0, 3.0, 0.0);
    chk2("fmod", vx_fmod, fmod, -5.0, 3.0, 0.0);
    chk2("fmod", vx_fmod, fmod, 1e300, 3.0, 0.0);
    report();

    section("  frexp, modf, scalbn, ilogb, logb, nextafter");
    for (int i = 0; i < 60000; i++) {
        double x = rlog(1e-300, 1e300) * (rnd() & 1 ? 1 : -1);
        int ea, eb;
        double a = vx_frexp(x, &ea), b = frexp(x, &eb);
        checks++;
        if (a != b || ea != eb) {
            if (failures < 12)
                printf("  FAIL frexp(%.17g): %.17g/%d vs %.17g/%d\n", x, a, ea, b, eb);
            failures++;
        }
        double ia, ib;
        a = vx_modf(x, &ia); b = modf(x, &ib);
        checks++;
        if (a != b || ia != ib) {
            if (failures < 12) printf("  FAIL modf(%.17g)\n", x);
            failures++;
        }
        checks++;
        if (vx_ilogb(x) != ilogb(x)) {
            if (failures < 12) printf("  FAIL ilogb(%.17g)\n", x);
            failures++;
        }
        chk1("logb", vx_logb, logb, x, 0.0);
    }
    /* Subnormals: frexp and ilogb both have to normalise first. */
    for (int i = 0; i < 5000; i++) {
        double x = rlog(1e-320, 1e-308);
        int ea, eb;
        double a = vx_frexp(x, &ea), b = frexp(x, &eb);
        checks++;
        if (a != b || ea != eb) {
            if (failures < 12)
                printf("  FAIL frexp subnormal(%.17g): %.17g/%d vs %.17g/%d\n",
                       x, a, ea, b, eb);
            failures++;
        }
        checks++;
        if (vx_ilogb(x) != ilogb(x)) {
            if (failures < 12) printf("  FAIL ilogb subnormal(%.17g)\n", x);
            failures++;
        }
    }
    /* scalbn across the whole range, including the steps that go into
     * and back out of the subnormals. */
    for (int i = 0; i < 40000; i++) {
        double x = rlog(1e-30, 1e30) * (rnd() & 1 ? 1 : -1);
        int n = (int)runif(-1200, 1200);
        double a = vx_scalbn(x, n), b = scalbn(x, n);
        checks++;
        if (ulp_diff(a, b) > 0.0) {
            if (failures < 12)
                printf("  FAIL scalbn(%.17g, %d): %.17g vs %.17g\n", x, n, a, b);
            failures++;
        }
    }
    for (int i = 0; i < 20000; i++)
        chk2("nextafter", vx_nextafter, nextafter,
             rlog(1e-300, 1e300) * (rnd() & 1 ? 1 : -1),
             rlog(1e-300, 1e300) * (rnd() & 1 ? 1 : -1), 0.0);
    chk2("nextafter", vx_nextafter, nextafter, 0.0, 1.0, 0.0);
    chk2("nextafter", vx_nextafter, nextafter, 0.0, -1.0, 0.0);
    chk2("nextafter", vx_nextafter, nextafter, 1.0, 1.0, 0.0);
    report();

    section("  fmin, fmax, fdim, copysign, fma");
    for (int i = 0; i < 40000; i++) {
        double x = runif(-100, 100), y = runif(-100, 100);
        chk2("fmin", vx_fmin, fmin, x, y, 0.0);
        chk2("fmax", vx_fmax, fmax, x, y, 0.0);
        chk2("fdim", vx_fdim, fdim, x, y, 0.0);
        chk2("copysign", vx_copysign, copysign, x, y, 0.0);
    }
    /* fmin and fmax on the two zeros and on NaN, which is where every
     * naive implementation is wrong. */
    checks += 4;
    if (!signbit(vx_fmin(0.0, -0.0)) || signbit(vx_fmax(0.0, -0.0)) ||
        vx_fmin(NAN, 3.0) != 3.0 || vx_fmax(NAN, 3.0) != 3.0) {
        printf("  FAIL fmin/fmax on signed zero or NaN\n");
        failures++;
    }
    for (int i = 0; i < 60000; i++) {
        double x = runif(-1e10, 1e10), y = runif(-1e10, 1e10), z = runif(-1e20, 1e20);
        double a = vx_fma(x, y, z), b = fma(x, y, z);
        checks++;
        if (ulp_diff(a, b) > 0.0) {
            if (failures < 12)
                printf("  FAIL fma(%.17g, %.17g, %.17g): %.17g vs %.17g\n",
                       x, y, z, a, b);
            failures++;
        }
    }
    report();

    /* ===== gamma and error, to a looser bound ===== */
    section("  tgamma, lgamma, erf, erfc");
    for (int i = 0; i < 20000; i++) chk1("tgamma", vx_tgamma, tgamma, runif(0.1, 170.0), 24.0);
    for (int i = 0; i < 20000; i++) chk1("lgamma", vx_lgamma, lgamma, runif(2.5, 1e5), 48.0);
    for (int i = 0; i < 20000; i++) chk1("erf", vx_erf, erf, runif(-6, 6), 4.0);
    for (int i = 0; i < 20000; i++) chk1("erfc", vx_erfc, erfc, runif(-6, 26), 8.0);
    /*
     * The small factorials, exactly.
     *
     * Only up to 19, and the bound is not arbitrary: 18! is 6.4e15,
     * which fits in a double's 53-bit mantissa, and 19! does not. Above
     * that "exact" is not a property the format can hold, so the check
     * becomes an ulp bound like everything else rather than a comparison
     * that would fail for a correct implementation.
     */
    for (int n = 1; n <= 19; n++) {
        double want = 1.0;
        for (int k = 2; k < n; k++) want *= (double)k;
        checks++;
        if (vx_tgamma((double)n) != want) {
            printf("  FAIL tgamma(%d) = %.17g, want %.17g (must be exact)\n",
                   n, vx_tgamma((double)n), want);
            failures++;
        }
    }
    for (int n = 20; n <= 170; n++)
        chk1("tgamma", vx_tgamma, tgamma, (double)n, 24.0);
    report();

    /* ===== the single-precision forms ===== */
    section("  the float forms");
    for (int i = 0; i < 40000; i++) {
        float x = (float)runif(-100, 100);
        checks += 2;
        if (ulp_diff((double)vx_sinf(x), (double)sinf(x)) > 4e9) failures++;
        if (vx_sqrtf(fabsf(x)) != sqrtf(fabsf(x))) {
            if (failures < 12) printf("  FAIL sqrtf(%g)\n", (double)x);
            failures++;
        }
    }
    for (int i = 0; i < 20000; i++) {
        float x = (float)runif(-80, 80);
        float a = vx_expf(x), b = expf(x);
        checks++;
        /* One ulp of a float, expressed as a relative difference. */
        if (!(fabs((double)a - (double)b) <= fabs((double)b) * 1.2e-7)) {
            if (failures < 12)
                printf("  FAIL expf(%g): %g vs %g\n", (double)x, (double)a, (double)b);
            failures++;
        }
    }
    report();

    /* ===== the domain edges =====
     *
     * Every one of these was a bug at some point in writing this file,
     * and all of them the same bug: a high word read as unsigned rather
     * than signed, so that "is this argument negative" became a question
     * that is always answered no. log(-1) returned a number. pow(2, -1)
     * returned 2. They are cheap to check and they do not announce
     * themselves — a wrong sign test produces a plausible number rather
     * than a crash.
     */
    section("  domain and range edges");
    struct { const char *name; double (*f)(double); double x; int want_nan; }
    edge[] = {
        { "log",   vx_log,   -1.0,   1 }, { "log",   vx_log,   -1e-300, 1 },
        { "log",   vx_log,   -1e300, 1 }, { "log2",  vx_log2,  -1.0,   1 },
        { "log2",  vx_log2,  -1e-15, 1 }, { "log10", vx_log10, -1.0,   1 },
        { "log10", vx_log10, -1e-15, 1 }, { "log1p", vx_log1p, -2.0,   1 },
        { "sqrt",  vx_sqrt,  -1.0,   1 }, { "asin",  vx_asin,   1.5,   1 },
        { "acos",  vx_acos,  -1.5,   1 }, { "acosh", vx_acosh,  0.5,   1 },
        { "atanh", vx_atanh,  1.5,   1 },
        { "log",   vx_log,    0.0,   0 }, { "log2",  vx_log2,   0.0,   0 },
        { "log10", vx_log10,  0.0,   0 }, { "log1p", vx_log1p, -1.0,   0 },
    };
    for (unsigned i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        double got = edge[i].f(edge[i].x);
        checks++;
        int ok = edge[i].want_nan ? isnan(got) : (isinf(got) && got < 0);
        if (!ok) {
            printf("  FAIL %s(%.17g) = %.17g, want %s\n", edge[i].name,
                   edge[i].x, got, edge[i].want_nan ? "NaN" : "-inf");
            failures++;
        }
    }
    /* pow's sign rules for a negative base, which depend on whether the
     * exponent is an integer and whether that integer is odd. */
    checks += 6;
    if (vx_pow(2.0, -1.0) != 0.5 || vx_pow(-2.0, 3.0) != -8.0 ||
        vx_pow(-2.0, 2.0) != 4.0 || !isnan(vx_pow(-2.0, 0.5)) ||
        vx_pow(-2.0, -3.0) != -0.125 || vx_pow(-8.0, 1.0 / 3.0) == vx_pow(-8.0, 1.0 / 3.0) * 0 + 1) {
        if (!(vx_pow(2.0, -1.0) == 0.5 && vx_pow(-2.0, 3.0) == -8.0 &&
              vx_pow(-2.0, 2.0) == 4.0 && isnan(vx_pow(-2.0, 0.5)) &&
              vx_pow(-2.0, -3.0) == -0.125)) {
            printf("  FAIL pow sign or integer-exponent rules\n");
            failures++;
        }
    }
    /* cbrt keeps the sign; sqrt does not accept one. */
    checks += 2;
    if (vx_cbrt(-8.0) != -2.0 || !signbit(vx_cbrt(-0.0))) {
        printf("  FAIL cbrt on a negative argument\n");
        failures++;
    }
    report();

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("math: FAILED\n");
        return 1;
    }
    printf("math: all %d checks passed\n", checks);
    return 0;
}
