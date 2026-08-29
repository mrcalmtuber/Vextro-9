/*
 * libc/math.c — the elementary functions.
 *
 * The algorithms are the ones from the Sun freely-distributable libm:
 * Cody-Waite and Payne-Hanek argument reduction, and minimax rational
 * approximations on the reduced range. They are here because there is
 * nothing to link against — this system has no libm and no host C
 * library beneath it — and because a browser engine calls essentially
 * all of them.
 *
 * ---- how these are organised ----
 *
 * Every function that is not trivially a bit operation reduces its
 * argument to a small interval, evaluates a polynomial there, and
 * reconstructs. The reductions are shared: exp, exp2 and pow all end at
 * `exp_scaled`; sin, cos and tan all begin at `rem_pio2`. That sharing
 * is not only economy — it is what makes the accuracy claim checkable,
 * because there are four hard pieces to verify rather than twenty.
 *
 * ---- what is verified, and where ----
 *
 * tools/math_test.c runs every function in this file against the host's
 * own libm over several hundred thousand arguments each, including the
 * places where the reductions change branch, and reports the worst error
 * in units of the last place. It runs as part of `make test`. That is
 * the only meaningful check available: nothing in this repository can
 * evaluate a transcendental function independently, so the reference has
 * to come from outside, and the host is where outside is.
 */

#include <math.h>
#include <errno.h>
#include <stdint.h>

/* ===== 1. GETTING AT THE BITS =====
 *
 * Through a union rather than through a cast between pointer types. The
 * cast is what every textbook writes and it is undefined behaviour that
 * modern compilers act on: with strict aliasing enabled, GCC is entitled
 * to assume a double* and a uint64_t* never refer to the same storage,
 * and to reorder a store through one past a load through the other. The
 * union is the spelling the standard blesses and compiles to the same
 * single instruction.
 */
typedef union { double d; uint64_t u; } dbits_t;
typedef union { float  f; uint32_t u; } fbits_t;

static inline uint64_t dtou(double x) { dbits_t v; v.d = x; return v.u; }
static inline double   utod(uint64_t u) { dbits_t v; v.u = u; return v.d; }
static inline uint32_t hi32(double x) { return (uint32_t)(dtou(x) >> 32); }
static inline uint32_t lo32(double x) { return (uint32_t)dtou(x); }

/* Rebuild a double from a replaced high word, keeping the low word. Used
 * by every reduction that needs to truncate a value to a known number of
 * leading bits. */
static inline double set_hi(double x, uint32_t h) {
    return utod(((uint64_t)h << 32) | (uint32_t)dtou(x));
}
static inline double from_words(uint32_t h, uint32_t l) {
    return utod(((uint64_t)h << 32) | l);
}

/* ===== 2. THE OPERATIONS THAT ARE ONLY BITS ===== */

double fabs(double x) { return utod(dtou(x) & 0x7FFFFFFFFFFFFFFFull); }

double copysign(double x, double y) {
    return utod((dtou(x) & 0x7FFFFFFFFFFFFFFFull) |
                (dtou(y) & 0x8000000000000000ull));
}

double nan(const char *tag) { (void)tag; return utod(0x7FF8000000000000ull); }

double fmin(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    /* -0 is smaller than +0, which a plain comparison cannot see because
     * the two compare equal. Ported code that sorts by fmin and then
     * divides would otherwise get an infinity of the wrong sign. */
    if (x == 0.0 && y == 0.0) return signbit(x) ? x : y;
    return x < y ? x : y;
}

double fmax(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == 0.0 && y == 0.0) return signbit(x) ? y : x;
    return x > y ? x : y;
}

double fdim(double x, double y) {
    if (isnan(x) || isnan(y)) return x + y;      /* propagates the NaN */
    return x > y ? x - y : 0.0;
}

/* ===== 3. ROUNDING =====
 *
 * By exponent inspection rather than by converting to an integer and
 * back. The conversion is shorter and wrong for anything a long cannot
 * hold, which for a double is most of its range — floor(1e300) is a
 * perfectly good question and the answer is 1e300.
 */
double floor(double x) {
    uint64_t u = dtou(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;

    if (e >= 52) return x;                 /* already integral, or inf/nan */
    if (e < 0) {
        /* |x| < 1: the answer is 0 or -1, and the sign of the zero
         * matters — floor(-0.0) is -0.0, not 0.0. */
        if (u & 0x7FFFFFFFFFFFFFFFull) return (u >> 63) ? -1.0 : 0.0;
        return x;
    }
    uint64_t mask = 0x000FFFFFFFFFFFFFull >> e;
    if (!(u & mask)) return x;             /* nothing below the point */
    if (u >> 63) u += 0x0010000000000000ull >> e;   /* negative: round away */
    return utod(u & ~mask);
}

double ceil(double x) {
    uint64_t u = dtou(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;

    if (e >= 52) return x;
    if (e < 0) {
        if (u & 0x7FFFFFFFFFFFFFFFull) return (u >> 63) ? -0.0 : 1.0;
        return x;
    }
    uint64_t mask = 0x000FFFFFFFFFFFFFull >> e;
    if (!(u & mask)) return x;
    if (!(u >> 63)) u += 0x0010000000000000ull >> e;
    return utod(u & ~mask);
}

double trunc(double x) {
    uint64_t u = dtou(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;
    if (e < 0) return (u >> 63) ? -0.0 : 0.0;
    return utod(u & ~(0x000FFFFFFFFFFFFFull >> e));
}

/* Halfway cases away from zero, which is what round() means and what
 * rint() specifically does not. */
double round(double x) {
    double t = trunc(x);
    double f = x - t;
    if (f >= 0.5) return t + 1.0;
    if (f <= -0.5) return t - 1.0;
    return t;
}

/*
 * To the nearest integer, halfway cases to even.
 *
 * Done by adding and subtracting 2^52, which forces the rounding the
 * hardware is already configured for and is exact for every input where
 * it does anything at all: adding 2^52 to a value below it pushes the
 * fractional bits off the end of the mantissa, and subtracting it back
 * leaves the integer. The volatile is load-bearing — without it the
 * compiler folds `(x + C) - C` to `x` and the function becomes the
 * identity.
 */
double rint(double x) {
    static const double toint = 4503599627370496.0;    /* 2^52 */
    uint64_t u = dtou(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;
    volatile double y;
    if (u >> 63) y = (x - toint) + toint;
    else         y = (x + toint) - toint;
    if (y == 0.0) return (u >> 63) ? -0.0 : 0.0;
    return y;
}

/* No exception flags are raised anywhere in this library, so these are
 * the same function. Named separately because ported code names both. */
double nearbyint(double x) { return rint(x); }

long      lround(double x)  { return (long)round(x); }
long      lrint(double x)   { return (long)rint(x); }
long long llround(double x) { return (long long)round(x); }
long long llrint(double x)  { return (long long)rint(x); }

/* ===== 4. EXPONENTS ===== */

/*
 * x * 2^n, without computing 2^n.
 *
 * The direct route overflows for large n and underflows for small n even
 * when the *result* is perfectly representable, so the scaling is split
 * into at most three steps of at most 1023 each. The subnormal case
 * needs the third: coming up from 2^-1074 to a normal number is more
 * than two steps of 1023.
 */
double scalbn(double x, int n) {
    double y = x;
    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {
        /* Two steps down of 969 rather than 1022, because 2^-1022 is the
         * smallest *normal* and multiplying by it loses bits that a
         * larger step keeps: 969 = 1022 - 53 leaves the mantissa whole. */
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    return y * utod(((uint64_t)(0x3FF + n)) << 52);
}

double ldexp(double x, int n)      { return scalbn(x, n); }
double scalbln(double x, long n) {
    if (n > 100000) n = 100000;
    if (n < -100000) n = -100000;
    return scalbn(x, (int)n);
}

double frexp(double x, int *e) {
    uint64_t u = dtou(x);
    int ex = (int)((u >> 52) & 0x7FF);

    if (ex == 0) {                          /* zero or subnormal */
        if (x == 0.0) { *e = 0; return x; }
        /* Normalise first, then take the exponent, or a subnormal would
         * report an exponent of -1022 with a mantissa below 0.5. */
        x = scalbn(x, 64);
        u = dtou(x);
        ex = (int)((u >> 52) & 0x7FF) - 64;
    } else if (ex == 0x7FF) {               /* infinity or NaN */
        *e = 0;
        return x;
    }
    *e = ex - 1022;
    /* Force the exponent to 2^-1, putting the mantissa in [0.5, 1). */
    return utod((u & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
}

int ilogb(double x) {
    uint64_t u = dtou(x) & 0x7FFFFFFFFFFFFFFFull;
    int ex = (int)(u >> 52);
    if (!u) return -2147483647 - 1;                    /* FP_ILOGB0 */
    if (ex == 0x7FF) return 2147483647;                /* FP_ILOGBNAN */
    if (ex) return ex - 1023;
    /* Subnormal: the leading bit is somewhere inside the mantissa. */
    return -1022 - (int)(__builtin_clzll(u) - 11);
}

double logb(double x) {
    if (x == 0.0) return -HUGE_VAL;
    if (isnan(x)) return x;
    if (isinf(x)) return HUGE_VAL;
    return (double)ilogb(x);
}

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    if (isinf(x)) return copysign(0.0, x);
    return copysign(x - t, x);
}

/* ===== 5. SQUARE ROOT =====
 *
 * One instruction. IEEE 754 requires square root to be correctly
 * rounded, and the processor obeys, so this is the only function in the
 * file that is exact — every other one carries a fraction of an ulp of
 * approximation error that no amount of care removes.
 */
double sqrt(double x) {
#if defined(__x86_64__)
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    /*
     * Only ever taken by tools/math_test.c, which compiles this file for
     * the machine the build is running on so that it can be checked
     * against a reference. The builtin is the same instruction on any
     * architecture that has one, and the point of the branch is that
     * this file can be compiled at all somewhere other than its target.
     */
    return __builtin_sqrt(x);
#endif
}

/* ===== 6. REMAINDER ===== */

/*
 * The remainder of a division that was never performed.
 *
 * Bitwise long division on the mantissas, which is the only way to get
 * this exactly right: computing x - trunc(x/y)*y loses every bit of the
 * answer as soon as x/y exceeds 2^53, and fmod is defined to be exact
 * for all finite arguments.
 */
double fmod(double x, double y) {
    uint64_t ux = dtou(x), uy = dtou(y);
    int ex = (int)((ux >> 52) & 0x7FF);
    int ey = (int)((uy >> 52) & 0x7FF);
    uint64_t sx = ux & 0x8000000000000000ull;

    if (uy << 1 == 0 || isnan(y) || ex == 0x7FF)
        return (x * y) / (x * y);            /* NaN, and the right one */
    if ((ux << 1) <= (uy << 1)) {
        if ((ux << 1) == (uy << 1)) return 0.0 * x;
        return x;
    }

    /* Mantissas with the implicit bit made explicit, and the exponents
     * they belong to. A subnormal has no implicit bit and its exponent
     * is one more than the field says. */
    uint64_t mx, my;
    if (!ex) { mx = ux << 12 >> 12; ex = 1; while (!(mx >> 52)) { mx <<= 1; ex--; } }
    else       mx = (ux << 12 >> 12) | (1ull << 52);
    if (!ey) { my = uy << 12 >> 12; ey = 1; while (!(my >> 52)) { my <<= 1; ey--; } }
    else       my = (uy << 12 >> 12) | (1ull << 52);

    for (int i = ex - ey; i; i--) {
        if (mx >= my) {
            if (mx == my) return 0.0 * x;
            mx -= my;
        }
        mx <<= 1;
    }
    if (mx >= my) {
        if (mx == my) return 0.0 * x;
        mx -= my;
    }
    /* Renormalise: shift the leading bit back into place, tracking how
     * far the exponent has to come down with it. */
    while (!(mx >> 52)) { mx <<= 1; ey--; }
    if (ey > 0) {
        mx -= 1ull << 52;
        mx |= (uint64_t)ey << 52;
    } else {
        mx >>= -ey + 1;
    }
    return utod(mx | sx);
}

/* IEEE remainder: the same magnitude reduction, but to the *nearest*
 * multiple rather than truncating, so the result may be negative when x
 * is positive. */
double remainder(double x, double y) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) return (x * y) / (x * y);
    double r = fmod(x, y);
    double ay = fabs(y);
    double ar = fabs(r);
    if (ar * 2.0 > ay || (ar * 2.0 == ay && fmod(fabs(x / y), 2.0) >= 1.0))
        r -= copysign(ay, r);
    return r;
}

/* ===== 7. EXPONENTIAL AND LOGARITHM ===== */

static const double
    ln2_hi   =  6.93147180369123816490e-01,
    ln2_lo   =  1.90821492927058770002e-10,
    invln2   =  1.44269504088896338700e+00,
    Ep1      =  1.66666666666666019037e-01,
    Ep2      = -2.77777777770155933842e-03,
    Ep3      =  6.61375632143793436117e-05,
    Ep4      = -1.65339022054652515390e-06,
    Ep5      =  4.13813679705723846039e-08;

/*
 * exp(hi - lo) * 2^k, for |hi - lo| at most half a natural logarithm of
 * two.
 *
 * The shared tail of exp, exp2 and pow. Splitting it out is not tidiness:
 * each of the three arrives at a *different* decomposition of the same
 * quantity, and forcing them through one entry point would mean
 * recombining hi and lo into a single double first — which throws away
 * exactly the extra precision the split exists to carry, and shows up as
 * a lost bit in pow(x, y) for large y.
 *
 * The rational form rather than a plain polynomial is what buys the
 * accuracy: r*c/(2-c) is an odd function of r whose error is symmetric,
 * so the two halves of the interval do not have to be approximated
 * separately.
 */
static double exp_scaled(double hi, double lo, int k) {
    double x = hi - lo;
    double t = x * x;
    double c = x - t * (Ep1 + t * (Ep2 + t * (Ep3 + t * (Ep4 + t * Ep5))));
    if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
    double y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
    return scalbn(y, k);
}

double exp(double x) {
    uint32_t hx = hi32(x) & 0x7FFFFFFF;

    if (hx >= 0x7FF00000) {                       /* infinity or NaN */
        if (hx > 0x7FF00000 || lo32(x)) return x + x;   /* NaN */
        return (hi32(x) & 0x80000000) ? 0.0 : x;        /* -inf -> 0 */
    }
    if (x > 709.782712893383973096) { errno = ERANGE; return HUGE_VAL * HUGE_VAL; }
    if (x < -745.133219101941108420) { errno = ERANGE; return 0x1p-1000 * 0x1p-1000; }

    /* |x| below 2^-28: the polynomial and the identity agree to more
     * places than a double has, and the addition is what rounds. */
    if (hx < 0x3E300000) return 1.0 + x;

    int k;
    double hi, lo;
    if (hx > 0x3FD62E42) {                        /* |x| > 0.5*ln2 */
        k = (int)(invln2 * x + copysign(0.5, x));
        hi = x - (double)k * ln2_hi;
        lo = (double)k * ln2_lo;
    } else {
        k = 0; hi = x; lo = 0.0;
    }
    return exp_scaled(hi, lo, k);
}

double exp2(double x) {
    if (isnan(x)) return x;
    if (x > 1024.0) { errno = ERANGE; return HUGE_VAL * HUGE_VAL; }
    if (x < -1075.0) { errno = ERANGE; return 0x1p-1000 * 0x1p-1000; }

    /*
     * 2^x = 2^k * e^(r ln2), with k the nearest integer to x.
     *
     * Multiplying x by ln2 and calling exp() would be one line and would
     * lose a bit: the product is inexact, and the error is amplified by
     * the exponential. Splitting ln2 into a head with trailing zeros and
     * a tail keeps the product exact to twice the precision, which is
     * what the two arguments to exp_scaled are for.
     */
    double k = rint(x);
    double r = x - k;
    return exp_scaled(r * ln2_hi, -(r * ln2_lo), (int)k);
}

static const double
    Lg1 = 6.666666666666735130e-01,
    Lg2 = 3.999999999940941908e-01,
    Lg3 = 2.857142874366239149e-01,
    Lg4 = 2.222219843214978396e-01,
    Lg5 = 1.818357216161805012e-01,
    Lg6 = 1.531383769920937332e-01,
    Lg7 = 1.479819860511658591e-01;

/*
 * The natural logarithm.
 *
 * x is written as 2^k (1+f) with 1+f between sqrt(2)/2 and sqrt(2), so
 * that f is small and centred on zero. The polynomial is then in
 * s = f/(2+f), whose odd powers alone approximate log((1+s)/(1-s)) —
 * which is exactly log(1+f) — and an odd series converges twice as fast
 * as the direct one for the same number of terms.
 */
double log(double x) {
    /*
     * Signed, and that is the whole guard against a negative argument.
     * The test below is `hx < 0x00100000`, which for a signed high word
     * catches zero, the subnormals *and* every negative number in one
     * comparison -- a negative double has its top bit set, so its high
     * word read as a signed integer is less than any of them. Read as
     * unsigned the same comparison is false for every negative x, and
     * log(-1) walks into the reduction and returns a number.
     */
    int32_t  hx = (int32_t)hi32(x);
    uint32_t lx = lo32(x);
    int k = 0;

    if (hx < 0x00100000) {                        /* zero, subnormal, or < 0 */
        if (((hx & 0x7FFFFFFF) | (int32_t)lx) == 0) { errno = ERANGE; return -1.0 / 0.0 * 1.0; }
        if (hx < 0) { errno = EDOM; return (x - x) / 0.0; }
        /* Scale a subnormal up into the normal range and pay for it in
         * the exponent, or every one of them would reduce to the same
         * value and log would be flat near zero. */
        k -= 54;
        x *= 0x1p54;
        hx = (int32_t)hi32(x);
    }
    if (hx >= 0x7FF00000) return x + x;           /* inf, NaN */
    if (hx == 0x3FF00000 && lx == 0) return 0.0;  /* exactly 1 */

    k += (int)(hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    /* 0x95F64 is the high word of sqrt(2) less one, which is where the
     * interval is split so that |f| is smallest. */
    int i = (hx + 0x95F64) & 0x100000;
    x = set_hi(x, hx | (i ^ 0x3FF00000));
    k += (i >> 20);

    double f = x - 1.0;

    /* |f| under 2^-20: the series is dominated by its first two terms
     * and the polynomial below would evaluate to noise. */
    if ((0x000FFFFF & (2 + hx)) < 3) {
        if (f == 0.0) return (double)k * ln2_hi + (double)k * ln2_lo;
        double R = f * f * (0.5 - 0.33333333333333333 * f);
        return (double)k * ln2_hi - ((R - (double)k * ln2_lo) - f);
    }

    double s  = f / (2.0 + f);
    double z  = s * s;
    double w  = z * z;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    double R  = t2 + t1;
    double hfsq = 0.5 * f * f;

    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return (double)k * ln2_hi -
           ((hfsq - (s * (hfsq + R) + (double)k * ln2_lo)) - f);
}

/*
 * log(1+x), for x so small that computing 1+x first would throw the
 * answer away.
 *
 * The same reduction as log, with one addition. The `c` term is what
 * makes it worth having: when 1+x rounds, c records exactly how much was
 * lost, and adding it back at the end recovers the precision that the
 * naive log(1.0+x) simply does not have.
 */
double log1p(double x) {
    int32_t hx = (int32_t)hi32(x);
    int32_t ax = hx & 0x7FFFFFFF;

    double c = 0.0, f = 0.0, u;
    int k = 1;                 /* 1 means "the reduction is still needed" */
    uint32_t hu = 0;

    if (hx < 0x3FDA827A) {                        /* x < 0.41422 */
        if (ax >= 0x3FF00000) {                   /* |x| >= 1 */
            if (x == -1.0) { errno = ERANGE; return -1.0 / 0.0 * 1.0; }
            errno = EDOM;
            return (x - x) / 0.0;                 /* x < -1 */
        }
        if (ax < 0x3E200000) {                    /* |x| < 2^-29 */
            if (ax < 0x3C900000) return x;        /* |x| < 2^-54 */
            return x - x * x * 0.5;
        }
        /*
         * Between -0.2929 and 0.41422 the argument is already inside the
         * interval the series converges on, so 1+x is never formed and
         * nothing is lost to its rounding. That is the entire point of
         * having log1p at all: for a small x, 1+x throws away every bit
         * of x below the fifty-third from the leading one, and log() can
         * only ever see what survived.
         *
         * Outside that window the reduction runs, and then `c` carries
         * exactly what 1+x lost so it can be added back at the end.
         */
        if (hx > 0 || hx <= (int32_t)0xBFD2BEC3) { k = 0; f = x; hu = 1; }
    }
    if (hx >= 0x7FF00000) return x + x;

    if (k != 0) {
        if (hx < 0x43400000) {                    /* x < 2^53 */
            u  = 1.0 + x;
            hu = hi32(u);
            k  = (int)(hu >> 20) - 1023;
            /* Which operand was the larger decides which subtraction is
             * exact. Both forms compute the same quantity; only one of
             * them computes it without error. */
            c  = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
            c /= u;
        } else {
            /* Beyond 2^53, adding one changes nothing at all. */
            u  = x;
            hu = hi32(u);
            k  = (int)(hu >> 20) - 1023;
            c  = 0.0;
        }
        hu &= 0x000FFFFF;
        /* 0x6A09E is the high mantissa of sqrt(2), the point at which
         * folding into the half-interval below gives a smaller |f|. */
        if (hu < 0x6A09E) {
            u = set_hi(u, hu | 0x3FF00000);
        } else {
            k += 1;
            u = set_hi(u, hu | 0x3FE00000);
            hu = (0x00100000 - hu) >> 2;
        }
        f = u - 1.0;
    }

    double hfsq = 0.5 * f * f;

    if (hu == 0) {                                /* |f| < 2^-20 */
        if (f == 0.0) {
            if (k == 0) return 0.0;
            c += (double)k * ln2_lo;
            return (double)k * ln2_hi + c;
        }
        double R = hfsq * (1.0 - 0.66666666666666666 * f);
        if (k == 0) return f - R;
        return (double)k * ln2_hi - ((R - ((double)k * ln2_lo + c)) - f);
    }

    double s = f / (2.0 + f);
    double z = s * s;
    double R = z * (Lg1 + z * (Lg2 + z * (Lg3 + z * (Lg4 + z * (Lg5 +
                                z * (Lg6 + z * Lg7))))));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return (double)k * ln2_hi -
           ((hfsq - (s * (hfsq + R) + ((double)k * ln2_lo + c))) - f);
}

double log10(double x) {
    /*
     * log(x) * log10(e), with the constant split.
     *
     * The single-constant form loses about a bit for arguments near a
     * power of ten, which is precisely where a program formatting a
     * number will land: log10(1000) coming back as 2.9999999999999996
     * makes a digit-count computation short by one. Splitting the
     * multiplier into a head with trailing zeros and a tail keeps the
     * product to twice the precision, and the exact-power-of-ten cases
     * come out exact.
     */
    static const double
        ivln10_hi = 4.34294481878168880939e-01,
        ivln10_lo = 2.50829467116452752298e-11,
        log10_2hi = 3.01029995663611771306e-01,
        log10_2lo = 3.69423907715893089906e-13;

    int32_t  hx = (int32_t)hi32(x);
    uint32_t lx = lo32(x);
    int k = 0;

    if (hx < 0x00100000) {
        if (((hx & 0x7FFFFFFF) | (int32_t)lx) == 0) { errno = ERANGE; return -1.0 / 0.0 * 1.0; }
        if (hx < 0) { errno = EDOM; return (x - x) / 0.0; }
        k -= 54; x *= 0x1p54; hx = (int32_t)hi32(x);
    }
    if (hx >= 0x7FF00000) return x + x;

    k += (int)(hx >> 20) - 1023;
    /* The exponent as a double, and the mantissa put back at 2^0. */
    int i = (int)(((uint32_t)k & 0x80000000) >> 31);
    hx = (hx & 0x000FFFFF) | (int32_t)((0x3FF - i) << 20);
    double y = (double)(k + i);
    x = set_hi(x, hx);

    double z = y * log10_2lo + ivln10_lo * log(x);
    return z + y * log10_2hi + ivln10_hi * log(x);
}

double log2(double x) {
    /*
     * The same shape as log10 and for the same reason: log2 of an exact
     * power of two must be exact, and log(x)/ln2 is not.
     *
     * Here the reduction does more than accuracy — after pulling the
     * exponent out, what is left is the log of a mantissa in [1,2), and
     * the exponent contributes an exact integer. A power of two has a
     * mantissa of exactly one, whose log is exactly zero, so the answer
     * is the exponent and nothing else.
     */
    static const double
        ivln2_hi = 1.44269504072144627571e+00,
        ivln2_lo = 1.67517131648865118353e-10;

    int32_t  hx = (int32_t)hi32(x);
    uint32_t lx = lo32(x);
    int k = 0;

    if (hx < 0x00100000) {
        if (((hx & 0x7FFFFFFF) | (int32_t)lx) == 0) { errno = ERANGE; return -1.0 / 0.0 * 1.0; }
        if (hx < 0) { errno = EDOM; return (x - x) / 0.0; }
        k -= 54; x *= 0x1p54; hx = (int32_t)hi32(x);
    }
    if (hx >= 0x7FF00000) return x + x;

    k += (int)(hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    if (hx == 0 && lx == 0) return (double)k;    /* an exact power of two */

    /*
     * The mantissa is centred on one, not merely normalised to [1, 2),
     * and the difference is the accuracy of this function.
     *
     * Leaving it in [1, 2) makes log2 of the mantissa lie in [0, 1), so
     * for an x just below a power of two -- 0.96, say -- the answer is
     * (-1) + 0.9415, and that subtraction cancels four bits. The
     * mantissa's own error, a fraction of an ulp at 0.94, survives the
     * cancellation unchanged and becomes a dozen ulps at 0.058.
     *
     * Splitting at sqrt(2) instead puts log2 of the mantissa in
     * [-0.5, 0.5] and makes k the *nearest* exponent rather than the one
     * below. Then k and the mantissa term never have opposite signs with
     * comparable magnitudes: when the result is small, k is zero and
     * there is nothing to cancel against.
     *
     * 0x95F64 is the high mantissa word of sqrt(2) less one, which is
     * where the two halves meet.
     */
    int i = ((int)hx + 0x95F64) & 0x100000;
    x = set_hi(x, (uint32_t)hx | (uint32_t)(i ^ 0x3FF00000));
    k += (i >> 20);

    double f = log(x);
    return (double)k + (f * ivln2_hi + f * ivln2_lo);
}

double expm1(double x) {
    /*
     * exp(x) - 1, computed without ever forming exp(x).
     *
     * That is the entire reason the function exists. For a small x,
     * exp(x) is a number just above one, and subtracting one from it
     * discards every bit of the answer that lay below the leading bit of
     * exp(x) -- which for x near 2^-30 is all of them.
     *
     * The reduction is the same as exp's: x becomes k*ln2 + r with |r|
     * at most half a logarithm of two. The reconstruction is not, and
     * cannot be. exp(k*ln2 + r) - 1 = 2^k * (exp(r) - 1) + (2^k - 1),
     * and both halves have to be assembled without letting the second
     * one round away the first, which is what the branching on k below
     * is for.
     */
    static const double
        Q1 = -3.33333333333331316428e-02,
        Q2 =  1.58730158725481460165e-03,
        Q3 = -7.93650757867487942473e-05,
        Q4 =  4.00821782732936239552e-06,
        Q5 = -2.01099218183624371326e-07;

    int32_t hx = (int32_t)hi32(x);
    int32_t xsb = hx & 0x80000000;                /* the sign, kept apart */
    int32_t ix = hx & 0x7FFFFFFF;

    if (ix >= 0x4043687A) {                       /* |x| >= 56*ln2 */
        if (ix >= 0x40862E42) {                   /* |x| >= 709.78 */
            if (ix >= 0x7FF00000) {
                if (((ix & 0x000FFFFF) | lo32(x)) != 0) return x + x;   /* NaN */
                return xsb ? -1.0 : x;            /* exp(-inf)-1 = -1 */
            }
            if (x > 709.782712893383973096) { errno = ERANGE; return HUGE_VAL * HUGE_VAL; }
        }
        /* Below -56*ln2, exp(x) is under 2^-56 and the answer rounds to
         * exactly -1. Returning the computed value would be the same
         * number reached more slowly. */
        if (xsb) return -1.0;
    }

    double c = 0.0, t, y, hi, lo;
    int k;

    if (ix > 0x3FD62E42) {                        /* |x| > 0.5*ln2 */
        if (ix < 0x3FF0A2B2) {                    /* |x| < 1.5*ln2 */
            if (xsb == 0) { hi = x - ln2_hi; lo = ln2_lo; k = 1; }
            else          { hi = x + ln2_hi; lo = -ln2_lo; k = -1; }
        } else {
            k = (int)(invln2 * x + (xsb == 0 ? 0.5 : -0.5));
            t = (double)k;
            hi = x - t * ln2_hi;
            lo = t * ln2_lo;
        }
        x = hi - lo;
        c = (hi - x) - lo;                        /* what the subtraction lost */
    } else if (ix < 0x3C900000) {                 /* |x| < 2^-54 */
        return x;
    } else {
        k = 0;
    }

    /* The rational approximation to (exp(r)-1) on the reduced range.
     * Same odd-symmetric form as exp's, which is what keeps the error
     * even across the interval rather than piled at one end. */
    double hfx = 0.5 * x;
    double hxs = x * hfx;
    double rr  = 1.0 + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    t = 3.0 - rr * hfx;
    double e = hxs * ((rr - t) / (6.0 - x * t));

    if (k == 0) return x - (x * e - hxs);         /* no reduction happened */

    e = x * (e - c) - c;
    e -= hxs;

    if (k == -1) return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return 1.0 + 2.0 * (x - e);
    }
    if (k <= -2 || k > 56) {                      /* 2^k is far from one */
        y = 1.0 - (e - x);
        if (k == 1024) y = y * 2.0 * 0x1p1023;
        else           y = scalbn(y, k);
        return y - 1.0;
    }

    if (k < 20) {
        /* 1 - 2^-k, formed exactly by masking rather than by
         * subtracting: the subtraction would round for k near 20 and the
         * whole point here is that this term is exact. */
        t = utod(((uint64_t)(0x3FF00000 - (0x200000 >> k))) << 32);
        y = t - (e - x);
        y = scalbn(y, k);
    } else {
        t = utod(((uint64_t)((0x3FF - k) << 20)) << 32);   /* 2^-k */
        y = x - (e + t);
        y += 1.0;
        y = scalbn(y, k);
    }
    return y;
}

/* ===== 8. POWER ===== */

static const double
    bp0 = 1.0,                bp1 = 1.5,
    dp_h0 = 0.0,              dp_h1 = 5.84962487220764160156e-01,
    dp_l0 = 0.0,              dp_l1 = 1.35003920212974897128e-08,
    PL1 = 5.99999999999994648725e-01,
    PL2 = 4.28571428578550184252e-01,
    PL3 = 3.33333329818377432918e-01,
    PL4 = 2.72728123808534006489e-01,
    PL5 = 2.30660745775561754067e-01,
    PL6 = 2.06975017800338417784e-01,
    lg2   =  6.93147180559945286227e-01,
    lg2_h =  6.93147182464599609375e-01,
    lg2_l = -1.90465429995776804525e-09,
    ovt   =  8.0085662595372944372e-17,
    cp    =  9.61796693925975554329e-01,
    cp_h  =  9.61796700954437255859e-01,
    cp_l  = -7.02846165095275826516e-09,
    ivln2   = 1.44269504088896338700e+00,
    ivln2_h = 1.44269502162933349609e+00,
    ivln2_l = 1.92596299112661746887e-08;

/*
 * x raised to y.
 *
 * Twenty lines of arithmetic behind eighty of special cases, and that
 * ratio is right rather than embarrassing. The C standard specifies
 * pow's behaviour for infinities, zeros, negative bases and integral
 * exponents in about thirty separate clauses, most of which disagree
 * with what the arithmetic would produce on its own — pow(-1, inf) is 1,
 * not NaN, and pow(0, -3) is -inf, not inf. JavaScript's Math.pow
 * inherits every one of them, so a browser engine is precisely the caller
 * that will find any of these that is wrong.
 *
 * The computation itself is 2^(y*log2(x)) carried out in double-double
 * arithmetic throughout, because y multiplies the *error* in log2(x) as
 * well as its value: an error of one ulp in the logarithm becomes an
 * error of y ulps in the result, and y can be a thousand.
 */
double pow(double x, double y) {
    /*
     * The high words are signed, and that is load-bearing rather than
     * incidental: half the special cases below turn on `hy < 0`, which
     * asks whether the exponent is negative. Read as unsigned it is a
     * question that is always answered no, and pow(2, -1) comes back as
     * 2 rather than 0.5 with no diagnostic anywhere.
     */
    int32_t  hx = (int32_t)hi32(x); uint32_t lx = lo32(x);
    int32_t  hy = (int32_t)hi32(y); uint32_t ly = lo32(y);
    int32_t  ix = hx & 0x7FFFFFFF;
    int32_t  iy = hy & 0x7FFFFFFF;

    /* y is zero: the answer is one, for every x including NaN. */
    if ((iy | (int32_t)ly) == 0) return 1.0;

    /* x is one: the answer is one, for every y including NaN. */
    if (hx == 0x3FF00000 && lx == 0) return 1.0;

    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0) ||
        iy > 0x7FF00000 || (iy == 0x7FF00000 && ly != 0))
        return x + y;                          /* either is NaN */

    /*
     * Is y an integer, and if so is it odd?
     *
     * yisint: 0 not an integer, 1 an odd integer, 2 an even one. It
     * decides the sign of the result for negative x, and whether a
     * negative x is legal at all — (-2)^0.5 is NaN and (-2)^3 is -8.
     */
    int yisint = 0;
    if (hx & 0x80000000) {
        if (iy >= 0x43400000) yisint = 2;      /* 2^53 and up: all even */
        else if (iy >= 0x3FF00000) {
            int k = (iy >> 20) - 0x3FF;
            if (k > 20) {
                uint32_t j = ly >> (52 - k);
                if ((j << (52 - k)) == ly) yisint = 2 - (int)(j & 1);
            } else if (ly == 0) {
                uint32_t j = (uint32_t)iy >> (20 - k);
                if ((int32_t)(j << (20 - k)) == iy) yisint = 2 - (int)(j & 1);
            }
        }
    }

    /* y is an infinity. */
    if (ly == 0) {
        if (iy == 0x7FF00000) {
            if (((ix - 0x3FF00000) | (int32_t)lx) == 0)
                return 1.0;                    /* (-1)^inf is 1, by fiat */
            if (ix >= 0x3FF00000)
                return (hy >= 0) ? y : 0.0;
            return (hy < 0) ? -y : 0.0;
        }
        if (iy == 0x3FF00000) return (hy < 0) ? 1.0 / x : x;
        if (hy == 0x40000000) return x * x;
        if (hy == 0x3FE00000 && hx >= 0) return sqrt(x);
    }

    double ax = fabs(x);

    /* x is zero, an infinity, or exactly one in magnitude. */
    if (lx == 0 && (ix == 0x7FF00000 || ix == 0 || ix == 0x3FF00000)) {
        double z = ax;
        if (hy < 0) z = 1.0 / z;
        if (hx & 0x80000000) {
            if (((ix - 0x3FF00000) | yisint) == 0) {
                /* A negative base to a non-integer power. */
                errno = EDOM;
                return (z - z) / (z - z);
            }
            if (yisint == 1) z = -z;
        }
        return z;
    }

    /* The sign of the result: negative only for a negative base raised
     * to an odd integer power. Everything below computes a magnitude. */
    double sn = 1.0;
    if (hx & 0x80000000) {
        if (yisint == 0) { errno = EDOM; return (x - x) / (x - x); }
        if (yisint == 1) sn = -1.0;
    }

    double t1, t2;

    if (iy > 0x41E00000) {
        /*
         * |y| beyond 2^31: the result can only be 0, 1 or infinity,
         * because log2(x) is at least 2^-20 in magnitude for any x that
         * is not extremely close to one, and the product overflows.
         *
         * The two thresholds either side of one are what separate "close
         * enough to one that the product is finite" from "not".
         */
        if (iy > 0x43F00000) {
            if (ix <= 0x3FEFFFFF) return (hy < 0) ? HUGE_VAL * HUGE_VAL : 0.0;
            if (ix >= 0x3FF00000) return (hy > 0) ? HUGE_VAL * HUGE_VAL : 0.0;
        }
        if (ix < 0x3FEFFFFF) return (hy < 0) ? sn * HUGE_VAL * HUGE_VAL : sn * 0.0;
        if (ix > 0x3FF00000) return (hy > 0) ? sn * HUGE_VAL * HUGE_VAL : sn * 0.0;

        /* x is within 2^-20 of one: log(x) is essentially x-1 and the
         * whole computation collapses to a series. */
        double t = ax - 1.0;
        double w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        double u = ivln2_h * t;
        double v = t * ivln2_l - w * ivln2;
        t1 = u + v;
        t1 = set_hi(t1, hi32(t1));
        t1 = utod(dtou(t1) & 0xFFFFFFFF00000000ull);
        t2 = v - (t1 - u);
    } else {
        /*
         * The general case: log2(ax) to double-double precision.
         *
         * ax is written as 2^n * s, with s in [1, 2), and then s is
         * compared against 1.5 to choose which of two anchor points to
         * expand around. Two anchors rather than one halves the interval
         * the polynomial has to cover, and it is what lets six terms
         * reach the precision that would otherwise need eleven.
         */
        int n = 0;
        if (ix < 0x00100000) { ax *= 0x1p53; n -= 53; ix = (int32_t)hi32(ax); }
        n += ((ix) >> 20) - 0x3FF;
        int j = ix & 0x000FFFFF;
        ix = j | 0x3FF00000;

        int k;
        if (j <= 0x3988E) k = 0;              /* |s - 1|   smallest */
        else if (j < 0xBB67A) k = 1;          /* |s - 1.5| smallest */
        else { k = 0; n += 1; ix -= 0x00100000; }
        ax = set_hi(ax, (uint32_t)ix);

        double bp = k ? bp1 : bp0;
        double u = ax - bp;
        double v = 1.0 / (ax + bp);
        double ss = u * v;
        double s_h = utod(dtou(ss) & 0xFFFFFFFF00000000ull);

        /* t_h and t_l reconstruct (ax + bp) to twice the precision, so
         * that s_l below is the exact residual of s_h rather than an
         * estimate of it. */
        double t_h = utod(((uint64_t)((uint32_t)(((ix >> 1) | 0x20000000) +
                                                 0x00080000 + (k << 18)))) << 32);
        double t_l = ax - (t_h - bp);
        double s_l = v * ((u - s_h * t_h) - s_h * t_l);

        double s2 = ss * ss;
        double r = s2 * s2 * (PL1 + s2 * (PL2 + s2 * (PL3 + s2 *
                   (PL4 + s2 * (PL5 + s2 * PL6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = 3.0 + s2 + r;
        t_h = utod(dtou(t_h) & 0xFFFFFFFF00000000ull);
        t_l = r - ((t_h - 3.0) - s2);

        /* u + v = ss * (t_h + t_l), computed so the product's own
         * rounding error is carried rather than discarded. */
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        double p_h = u + v;
        p_h = utod(dtou(p_h) & 0xFFFFFFFF00000000ull);
        double p_l = v - (p_h - u);
        double z_h = cp_h * p_h;
        double z_l = cp_l * p_h + p_l * cp + (k ? dp_l1 : dp_l0);

        double t = (double)n;
        t1 = ((z_h + z_l) + (k ? dp_h1 : dp_h0)) + t;
        t1 = utod(dtou(t1) & 0xFFFFFFFF00000000ull);
        t2 = z_l - (((t1 - t) - (k ? dp_h1 : dp_h0)) - z_h);
    }

    /* y * log2(ax), again in two pieces. */
    double y1 = utod(dtou(y) & 0xFFFFFFFF00000000ull);
    double p_l = (y - y1) * t1 + y * t2;
    double p_h = y1 * t1;
    double z = p_l + p_h;
    uint32_t j = hi32(z);
    uint32_t i = lo32(z);

    if ((int32_t)j >= 0x40900000) {                       /* z >= 1024 */
        if ((((uint32_t)j - 0x40900000) | i) != 0) { errno = ERANGE; return sn * HUGE_VAL * HUGE_VAL; }
        if (p_l + ovt > z - p_h) { errno = ERANGE; return sn * HUGE_VAL * HUGE_VAL; }
    } else if ((j & 0x7FFFFFFF) >= 0x4090CC00) {          /* z <= -1075 */
        if (((j - 0xC090CC00u) | i) != 0) { errno = ERANGE; return sn * 0x1p-1000 * 0x1p-1000; }
        if (p_l <= z - p_h) { errno = ERANGE; return sn * 0x1p-1000 * 0x1p-1000; }
    }

    /*
     * 2^z, with the integer part taken out first.
     *
     * n is the nearest integer to z, and it is found by *adding* half an
     * ulp at z's own exponent and then truncating — which is a rounding
     * carried out in the exponent field rather than by arithmetic, and
     * so is exact. Doing it by truncation alone, as though the fraction
     * were always below a half, is wrong for every z whose fraction is
     * above one: the residue p_h - t handed to the exponential below
     * would then exceed the interval the polynomial covers, and the
     * answer would be out by whole ulps rather than a fraction of one.
     */
    int32_t n = 0;
    uint32_t iz = j & 0x7FFFFFFF;
    int32_t  kz = (int32_t)(iz >> 20) - 0x3FF;
    if (iz > 0x3FE00000) {                       /* |z| > 0.5 */
        uint32_t nw = j + (0x00100000u >> (kz + 1));
        kz = (int32_t)((nw & 0x7FFFFFFF) >> 20) - 0x3FF;   /* n's own exponent */
        double t = utod(((uint64_t)(nw & ~(0x000FFFFFu >> kz))) << 32);
        n = (int32_t)(((nw & 0x000FFFFF) | 0x00100000) >> (20 - kz));
        if ((int32_t)j < 0) n = -n;
        p_h -= t;
    }
    int32_t k = n;
    double t = p_l + p_h;
    t = utod(dtou(t) & 0xFFFFFFFF00000000ull);
    double u = t * lg2_h;
    double v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    double w = v - (z - u);
    t = z * z;
    double t3 = z - t * (Ep1 + t * (Ep2 + t * (Ep3 + t * (Ep4 + t * Ep5))));
    double r = (z * t3) / (t3 - 2.0) - (w + z * w);
    z = 1.0 - (r - z);
    return sn * scalbn(z, (int)k);
}

/* ===== 9. ARGUMENT REDUCTION FOR THE TRIGONOMETRIC FUNCTIONS =====
 *
 * Every one of sin, cos and tan begins by writing x as n*(pi/2) + y with
 * |y| at most pi/4, and everything difficult about them is here rather
 * than in the polynomials that follow.
 *
 * The difficulty is cancellation. Subtracting n*(pi/2) from a large x
 * cancels every leading bit the two have in common, so the accuracy of
 * the *result* depends on knowing pi to as many bits as were cancelled —
 * not to the precision of a double. sin(1e22) needs about seventy bits
 * of pi beyond the double's own fifty-three; the worst case over the
 * whole double range needs about eleven hundred.
 *
 * So there are three paths, and which one runs is decided by how much
 * precision the argument's magnitude will destroy:
 *
 *   No reduction at all, when |x| is already inside pi/4.
 *
 *   Cody-Waite, for |x| below 2^20 * pi/2: subtract n*(pi/2) in three
 *   pieces, each with trailing zero bits, so that the products are exact
 *   and the cancellation happens against pi represented to about 160
 *   bits.
 *
 *   Payne-Hanek beyond that, which computes x * (2/pi) against a stored
 *   1280-bit expansion of 2/pi and keeps only the part that survives.
 *   This is the path that makes sin(1e300) an answer rather than a
 *   shrug, and it is the reason for the table below.
 */

static const double
    invpio2 = 6.36619772367581382433e-01,
    pio2_1  = 1.57079632673412561417e+00,
    pio2_1t = 6.07710050650619224932e-11,
    pio2_2  = 6.07710050630396597660e-11,
    pio2_2t = 2.02226624879595063154e-21,
    pio2_3  = 2.02226624871116645580e-21,
    pio2_3t = 8.47842766036889956997e-32;

/*
 * The fractional bits of 2/pi, most significant first: 1280 of them,
 * which is enough to reduce any finite double.
 *
 * The bound is not a guess. An argument x = m * 2^e loses e bits to
 * cancellation, e reaches 971 for the largest finite double, and the
 * result needs 53 significant bits back with a margin for the worst-case
 * near-multiple-of-pi/2 argument — which for double precision is known
 * to be about 61 bits. 971 + 53 + 61 rounds up to twenty 64-bit words.
 *
 * Generated rather than transcribed, and checked against the published
 * hexadecimal expansion of 2/pi.
 */
#define TWOPI_WORDS 20
static const uint64_t twopi_bits_tab[TWOPI_WORDS] = {
    0xA2F9836E4E441529ull, 0xFC2757D1F534DDC0ull,
    0xDB6295993C439041ull, 0xFE5163ABDEBBC561ull,
    0xB7246E3A424DD2E0ull, 0x06492EEA09D1921Cull,
    0xFE1DEB1CB129A73Eull, 0xE88235F52EBB4484ull,
    0xE99C7026B45F7E41ull, 0x3991D639835339F4ull,
    0x9C845F8BBDF9283Bull, 0x1FF897FFDE05980Full,
    0xEF2F118B5A0A6D1Full, 0x6D367ECF27CB09B7ull,
    0x4F463F669E5FEA2Dull, 0x7527BAC7EBE5F17Bull,
    0x3D0739F78A5292EAull, 0x6BFB5FB11F8D5D08ull,
    0x56033046FC7B6BABull, 0xF0CFBC209AF4361Dull,
};

/* Sixty-four bits of 2/pi starting at one-based bit index `idx`. Indices
 * at or below zero read as zero, which is correct: 2/pi is less than one,
 * so every bit before its binary point is a zero. */
static uint64_t twopi_window(int idx) {
    uint64_t r = 0;
    for (int b = 0; b < 64; b++) {
        int i = idx + b;
        uint64_t bit = 0;
        if (i >= 1) {
            int w = (i - 1) >> 6;
            int o = (i - 1) & 63;
            if (w < TWOPI_WORDS) bit = (twopi_bits_tab[w] >> (63 - o)) & 1u;
        }
        r = (r << 1) | bit;
    }
    return r;
}

static const double
    pio2_hi = 1.57079632679489655800e+00,     /* pi/2 rounded to double  */
    pio2_lo = 6.12323399573676603587e-17;     /* and what that rounding
                                               * left over               */

/*
 * Payne-Hanek: x = n*(pi/2) + y, computed against the bit table.
 *
 * The method is to form x * (2/pi) in fixed point and keep only what
 * matters — the fractional part, which gives y, and the low two bits of
 * the integer part, which give the quadrant. Everything above those two
 * bits is a multiple of four and cancels out of both, which is what makes
 * a 192-bit window of the table sufficient no matter how large x is: the
 * window simply starts further along.
 */
static int rem_pio2_large(double x, double *y) {
    uint64_t u = dtou(x);
    int sign = (int)(u >> 63);
    int be   = (int)((u >> 52) & 0x7FF);

    /* x = m * 2^e, with m a 53-bit integer. */
    uint64_t m = (u & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    int e = be - 1075;

    /*
     * The window starts one bit *before* the position that would give a
     * purely fractional product, so that the two integer bits of weight
     * 1 and 2 come out with it. Those are the quadrant. Bits of weight 4
     * and above multiply an integer mantissa and so contribute nothing
     * to either the quadrant modulo four or the fraction.
     */
    int idx = e - 1;
    uint64_t w0 = twopi_window(idx);
    uint64_t w1 = twopi_window(idx + 64);
    uint64_t w2 = twopi_window(idx + 128);

    /* The 192-bit product, truncated — the discarded high bits are the
     * multiples of four that do not matter. */
    __uint128_t p2 = (__uint128_t)m * w2;
    __uint128_t p1 = (__uint128_t)m * w1 + (uint64_t)(p2 >> 64);
    __uint128_t p0 = (__uint128_t)m * w0 + (uint64_t)(p1 >> 64);

    uint64_t r0 = (uint64_t)p0;
    uint64_t r1 = (uint64_t)p1;

    int n = (int)(r0 >> 62);                        /* the quadrant */

    /* The fraction, as 126 bits: bits 189 down to 64 of the product. */
    __uint128_t F = ((__uint128_t)(r0 & 0x3FFFFFFFFFFFFFFFull) << 64) | r1;

    int neg = 0;
    if (F >> 125) {                                 /* fraction over a half */
        n++;
        F = ((__uint128_t)1 << 126) - F;
        neg = 1;
    }

    if (F == 0) { y[0] = 0.0; y[1] = 0.0; }
    else {
        /*
         * Normalise, then take two consecutive 53-bit slices. Together
         * they hold the fraction to 106 significant bits *relative to its
         * own magnitude* — which is the point, because a fraction that
         * came out tiny is exactly the case where an absolute error
         * bound would be worthless. Shifting until the leading bit is in
         * a known place is what turns an absolute 126 bits into a
         * relative 106.
         */
        int lz = 0;
        while (!((F >> 125) & 1u)) { F <<= 1; lz++; }

        uint64_t s0 = (uint64_t)(F >> 73) & 0x1FFFFFFFFFFFFFull;   /* 53 bits */
        uint64_t s1 = (uint64_t)(F >> 20) & 0x1FFFFFFFFFFFFFull;   /* next 53 */

        double fhi = scalbn((double)s0, -(126 + lz) + 73);
        double flo = scalbn((double)s1, -(126 + lz) + 20);

        /*
         * The fraction times pi/2, in double-double.
         *
         * fhi * pio2_hi is not the answer even to first order: the
         * product of two doubles needs 106 bits and a double holds 53,
         * so half of it is thrown away by the multiplication itself.
         * Dekker's splitting recovers exactly what was thrown away —
         * split each factor into two 26-bit halves whose pairwise
         * products are exact, and the residual falls out of their sum.
         *
         * Getting this wrong is invisible for ordinary arguments and
         * catastrophic here, because the residue is the *whole* answer
         * for an argument that reduced almost to nothing.
         */
        const double split = 134217729.0;         /* 2^27 + 1 */
        double ah = fhi * split; ah = ah - (ah - fhi);
        double al = fhi - ah;
        double bh = pio2_hi * split; bh = bh - (bh - pio2_hi);
        double bl = pio2_hi - bh;

        double ph = fhi * pio2_hi;
        double pl = ((ah * bh - ph) + ah * bl + al * bh) + al * bl;

        double t = pl + fhi * pio2_lo + flo * pio2_hi;
        y[0] = ph + t;
        y[1] = (ph - y[0]) + t;
    }

    if (neg) { y[0] = -y[0]; y[1] = -y[1]; }
    if (sign) { y[0] = -y[0]; y[1] = -y[1]; n = -n; }
    return n & 3;
}

/*
 * The reduction proper. Returns the quadrant and fills y with the
 * residue as a head and a tail.
 */
static int rem_pio2(double x, double *y) {
    /* Signed, because the branches below ask which side of zero x is on
     * and an unsigned high word can only ever answer "positive". */
    int32_t hx = (int32_t)hi32(x);
    int32_t ix = hx & 0x7FFFFFFF;

    if (ix <= 0x3FE921FB) {                    /* |x| <= pi/4: nothing to do */
        y[0] = x; y[1] = 0.0;
        return 0;
    }

    if (ix < 0x4002D97C) {                     /* |x| < 3*pi/4: one step */
        if (hx > 0) {
            double z = x - pio2_1;
            /* The high word of pi/2 to 33 bits: below that the second
             * correction term is not yet needed and adding it would only
             * introduce rounding. */
            if (ix != 0x3FF921FB) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        double z = x + pio2_1;
        if (ix != 0x3FF921FB) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        } else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1 & 3;
    }

    if (ix <= 0x413921FB) {                    /* |x| <= 2^20 * pi/2 */
        double t = fabs(x);
        int n = (int)(t * invpio2 + 0.5);
        double fn = (double)n;
        double r = t - fn * pio2_1;
        double w = fn * pio2_1t;

        /*
         * A second and sometimes a third correction.
         *
         * The condition is not about the size of n but about how much
         * cancellation r - w has suffered: if the result is more than
         * sixteen binary orders of magnitude below the input, the first
         * correction term has itself been rounded away and a finer one
         * is needed. Testing the exponents directly is what makes this
         * exact rather than heuristic.
         */
        int j = ix >> 20;
        y[0] = r - w;
        uint32_t high = hi32(y[0]);
        int i = j - (int)((high >> 20) & 0x7FF);
        if (i > 16) {
            double t2 = r;
            w = fn * pio2_2;
            r = t2 - w;
            w = fn * pio2_2t - ((t2 - r) - w);
            y[0] = r - w;
            high = hi32(y[0]);
            i = j - (int)((high >> 20) & 0x7FF);
            if (i > 49) {
                double t3 = r;
                w = fn * pio2_3;
                r = t3 - w;
                w = fn * pio2_3t - ((t3 - r) - w);
                y[0] = r - w;
            }
        }
        y[1] = (r - y[0]) - w;
        if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; return -n & 3; }
        return n & 3;
    }

    if (ix >= 0x7FF00000) {                    /* infinity or NaN */
        y[0] = y[1] = x - x;
        return 0;
    }

    return rem_pio2_large(x, y);
}

/* ===== 10. THE TRIGONOMETRIC KERNELS =====
 *
 * Each takes a reduced argument in [-pi/4, pi/4] and its tail, and each
 * is a minimax polynomial of the appropriate parity: sine is odd, cosine
 * is even, and using the parity halves the number of terms for a given
 * accuracy because every coefficient the parity forbids would have been
 * zero anyway.
 */
static const double
    S1 = -1.66666666666666324348e-01,
    S2 =  8.33333333332248946124e-03,
    S3 = -1.98412698298579493134e-04,
    S4 =  2.75573137070700676789e-06,
    S5 = -2.50507602534068634195e-08,
    S6 =  1.58969099521155010221e-10;

static double kernel_sin(double x, double y, int iy) {
    if (fabs(x) < 0x1p-27) return x;           /* x is its own sine here */

    double z = x * x;
    double v = z * x;
    double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    /* With a tail: the correction is not simply added, because v*(...) and
     * the tail are of wildly different magnitudes and the grouping below
     * is what keeps the small one from being lost. */
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static const double
    C1 =  4.16666666666666019037e-02,
    C2 = -1.38888888888741095749e-03,
    C3 =  2.48015872894767294178e-05,
    C4 = -2.75573143513906633035e-07,
    C5 =  2.08757232129817482790e-09,
    C6 = -1.13596475577881948265e-11;

static double kernel_cos(double x, double y) {
    double z = x * x;
    double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));

    if (fabs(x) < 0.3) return 1.0 - (0.5 * z - (z * r - x * y));

    /*
     * Near the ends of the interval, 1 - z/2 cancels badly and the
     * subtraction has to be staged.
     *
     * qx is a small constant chosen to sit just below the rounding of
     * z/2, so that (1 - qx) is exact and the remaining subtraction is
     * between quantities of comparable size. Without it, cos(0.7) loses
     * about two bits.
     */
    double qx;
    if (fabs(x) > 0.78125) qx = 0.28125;
    else qx = set_hi(0.0, (uint32_t)(hi32(x) - 0x00200000));
    double hz = 0.5 * z - qx;
    return (1.0 - qx) - (hz - (z * r - x * y));
}

static const double
    T0  =  3.33333333333334091986e-01,
    T1  =  1.33333333333201242699e-01,
    T2  =  5.39682539762260521377e-02,
    T3  =  2.18694882948595424599e-02,
    T4  =  8.86323982359930005737e-03,
    T5  =  3.59207910759131235356e-03,
    T6  =  1.45620945432529025516e-03,
    T7  =  5.88041240820264096874e-04,
    T8  =  2.46463134818469906812e-04,
    T9  =  7.81794442939557092300e-05,
    T10 =  7.14072491382608190305e-05,
    T11 = -1.85586374855275456654e-05,
    T12 =  2.59073051863633712884e-05,
    pio4    = 7.85398163397448278999e-01,
    pio4lo  = 3.06161699786838301793e-17;

/* iy is 1 for tan and -1 for cotangent, which is what lets tan(x) for a
 * reduced argument in an odd quadrant be computed as -1/tan(y) using the
 * same polynomial. */
static double kernel_tan(double x, double y, int iy) {
    uint32_t hx = hi32(x);
    int32_t ix = (int32_t)(hx & 0x7FFFFFFF);

    if (ix < 0x3E300000) {                     /* |x| < 2^-28 */
        if ((int)x == 0) {
            if (((uint32_t)ix | lo32(x) | (uint32_t)(iy + 1)) == 0)
                return 1.0 / fabs(x);
            if (iy == 1) return x;
            return -1.0 / x;
        }
    }

    double z, w;
    if (ix >= 0x3FE59428) {                    /* |x| >= 0.6744 */
        /* Reflect about pi/4, where the polynomial is accurate, and undo
         * the reflection at the end. Without this the series would have
         * to cover a range over which tan grows without bound. */
        if (hx & 0x80000000) { x = -x; y = -y; }
        z = pio4 - x;
        w = pio4lo - y;
        x = z + w;
        y = 0.0;
    }
    z = x * x;
    w = z * z;

    double r = T1 + w * (T3 + w * (T5 + w * (T7 + w * (T9 + w * T11))));
    double v = z * (T2 + w * (T4 + w * (T6 + w * (T8 + w * (T10 + w * T12)))));
    double s = z * x;
    r = y + z * (s * (r + v) + y);
    r += T0 * s;
    w = x + r;

    if (ix >= 0x3FE59428) {
        double vv = (double)iy;
        return (double)(1 - ((int)hx >> 30 & 2)) *
               (vv - 2.0 * (x - (w * w / (w + vv) - r)));
    }
    if (iy == 1) return w;

    /*
     * -1/(x+r), computed so that the division's rounding error is
     * corrected rather than accepted.
     *
     * The straightforward -1.0/w carries a full ulp of error, and a
     * cotangent is exactly where that shows: near a zero of the tangent,
     * the reciprocal is enormous and the relative error is preserved into
     * a result the caller is about to subtract from something.
     */
    double a = utod(dtou(w) & 0xFFFFFFFF00000000ull);
    double vv = r - (a - x);
    double t = -1.0 / w;
    double aa = utod(dtou(t) & 0xFFFFFFFF00000000ull);
    s = 1.0 + aa * a;
    return aa + t * (s + aa * vv);
}

/* ===== 11. THE TRIGONOMETRIC FUNCTIONS ===== */

double sin(double x) {
    uint32_t ix = hi32(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return kernel_sin(x, 0.0, 0);
    if (ix >= 0x7FF00000) return x - x;

    double y[2];
    int n = rem_pio2(x, y);
    switch (n) {
    case 0:  return  kernel_sin(y[0], y[1], 1);
    case 1:  return  kernel_cos(y[0], y[1]);
    case 2:  return -kernel_sin(y[0], y[1], 1);
    default: return -kernel_cos(y[0], y[1]);
    }
}

double cos(double x) {
    uint32_t ix = hi32(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return kernel_cos(x, 0.0);
    if (ix >= 0x7FF00000) return x - x;

    double y[2];
    int n = rem_pio2(x, y);
    switch (n) {
    case 0:  return  kernel_cos(y[0], y[1]);
    case 1:  return -kernel_sin(y[0], y[1], 1);
    case 2:  return -kernel_cos(y[0], y[1]);
    default: return  kernel_sin(y[0], y[1], 1);
    }
}

void sincos(double x, double *s, double *c) {
    uint32_t ix = hi32(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) {
        *s = kernel_sin(x, 0.0, 0);
        *c = kernel_cos(x, 0.0);
        return;
    }
    if (ix >= 0x7FF00000) { *s = *c = x - x; return; }

    double y[2];
    int n = rem_pio2(x, y);
    double sn = kernel_sin(y[0], y[1], 1);
    double cs = kernel_cos(y[0], y[1]);
    switch (n) {
    case 0:  *s =  sn; *c =  cs; break;
    case 1:  *s =  cs; *c = -sn; break;
    case 2:  *s = -sn; *c = -cs; break;
    default: *s = -cs; *c =  sn; break;
    }
}

double tan(double x) {
    uint32_t ix = hi32(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB) return kernel_tan(x, 0.0, 1);
    if (ix >= 0x7FF00000) return x - x;

    double y[2];
    int n = rem_pio2(x, y);
    /* An odd quadrant gives the cotangent of the residue, which the
     * kernel produces from the same polynomial when told to. */
    return kernel_tan(y[0], y[1], 1 - ((n & 1) << 1));
}

/* ===== 12. THE INVERSE TRIGONOMETRIC FUNCTIONS ===== */

static const double
    pio2_hi_a =  1.57079632679489655800e+00,
    pio2_lo_a =  6.12323399573676603587e-17,
    pio4_hi_a =  7.85398163397448278999e-01,
    aS0 =  1.66666666666666657415e-01,
    aS1 = -3.25565818622400915405e-01,
    aS2 =  2.01212532134862925881e-01,
    aS3 = -4.00555345006794114027e-02,
    aS4 =  7.91534994289814532176e-04,
    aS5 =  3.47933107596021167570e-05,
    aQ1 = -2.40339491173441421878e+00,
    aQ2 =  2.02094576023350569471e+00,
    aQ3 = -6.88283971605453293030e-01,
    aQ4 =  7.70381505559019352791e-02;

double asin(double x) {
    int32_t hx = (int32_t)hi32(x);
    int32_t ix = hx & 0x7FFFFFFF;

    if (ix >= 0x3FF00000) {
        if (((uint32_t)(ix - 0x3FF00000) | lo32(x)) == 0)
            return x * pio2_hi_a + x * pio2_lo_a;   /* asin(+-1) */
        errno = EDOM;
        return (x - x) / (x - x);
    }

    if (ix < 0x3FE00000) {                     /* |x| < 0.5 */
        if (ix < 0x3E400000) return x;         /* below 2^-27: asin(x)=x */
        double t = x * x;
        double p = t * (aS0 + t * (aS1 + t * (aS2 + t * (aS3 + t * (aS4 + t * aS5)))));
        double q = 1.0 + t * (aQ1 + t * (aQ2 + t * (aQ3 + t * aQ4)));
        return x + x * (p / q);
    }

    /*
     * |x| at or above a half: asin(x) = pi/2 - 2*asin(sqrt((1-x)/2)).
     *
     * The identity moves the argument back into the range where the
     * rational approximation is accurate, and — more importantly — it
     * removes the infinite derivative at 1, which no polynomial in x can
     * follow.
     */
    double w = 1.0 - fabs(x);
    double t = w * 0.5;
    double p = t * (aS0 + t * (aS1 + t * (aS2 + t * (aS3 + t * (aS4 + t * aS5)))));
    double q = 1.0 + t * (aQ1 + t * (aQ2 + t * (aQ3 + t * aQ4)));
    double s = sqrt(t);
    double r, c;

    if (ix >= 0x3FEF3333) {                    /* |x| > 0.975 */
        w = p / q;
        t = pio2_hi_a - (2.0 * (s + s * w) - pio2_lo_a);
    } else {
        /* Split the square root so the subtraction below is exact: w has
         * the low half of its mantissa cleared, and c holds precisely
         * what that removed. */
        w = utod(dtou(s) & 0xFFFFFFFF00000000ull);
        c = (t - w * w) / (s + w);
        r = p / q;
        p = 2.0 * s * r - (pio2_lo_a - 2.0 * c);
        q = pio4_hi_a - 2.0 * w;
        t = pio4_hi_a - (p - q);
    }
    return (hx > 0) ? t : -t;
}

double acos(double x) {
    int32_t hx = (int32_t)hi32(x);
    int32_t ix = hx & 0x7FFFFFFF;

    if (ix >= 0x3FF00000) {
        if (((uint32_t)(ix - 0x3FF00000) | lo32(x)) == 0)
            return (hx >> 31) ? 2.0 * pio2_hi_a + 0x1p-1000 : 0.0;
        errno = EDOM;
        return (x - x) / (x - x);
    }

    if (ix < 0x3FE00000) {                     /* |x| < 0.5 */
        if (ix <= 0x3C600000) return pio2_hi_a + pio2_lo_a;
        double z = x * x;
        double p = z * (aS0 + z * (aS1 + z * (aS2 + z * (aS3 + z * (aS4 + z * aS5)))));
        double q = 1.0 + z * (aQ1 + z * (aQ2 + z * (aQ3 + z * aQ4)));
        double r = p / q;
        return pio2_hi_a - (x - (pio2_lo_a - x * r));
    }

    if (hx >> 31) {                            /* x <= -0.5 */
        double z = (1.0 + x) * 0.5;
        double p = z * (aS0 + z * (aS1 + z * (aS2 + z * (aS3 + z * (aS4 + z * aS5)))));
        double q = 1.0 + z * (aQ1 + z * (aQ2 + z * (aQ3 + z * aQ4)));
        double s = sqrt(z);
        double r = p / q;
        double w = r * s - pio2_lo_a;
        return 2.0 * pio2_hi_a - 2.0 * (s + w);
    }

    /* x >= 0.5 */
    double z = (1.0 - x) * 0.5;
    double s = sqrt(z);
    double df = utod(dtou(s) & 0xFFFFFFFF00000000ull);
    double c  = (z - df * df) / (s + df);
    double p = z * (aS0 + z * (aS1 + z * (aS2 + z * (aS3 + z * (aS4 + z * aS5)))));
    double q = 1.0 + z * (aQ1 + z * (aQ2 + z * (aQ3 + z * aQ4)));
    double r = p / q;
    double w = r * s + c;
    return 2.0 * (df + w);
}

static const double
    atanhi0 = 4.63647609000806093515e-01,      /* atan(0.5)  */
    atanhi1 = 7.85398163397448278999e-01,      /* atan(1)    */
    atanhi2 = 9.82793723247329054082e-01,      /* atan(1.5)  */
    atanhi3 = 1.57079632679489655800e+00,      /* atan(inf)  */
    atanlo0 = 2.26987774529616870924e-17,
    atanlo1 = 3.06161699786838301793e-17,
    atanlo2 = 1.39033110312309984516e-17,
    atanlo3 = 6.12323399573676603587e-17,
    aT0  =  3.33333333333329318027e-01,
    aT1  = -1.99999999998764832476e-01,
    aT2  =  1.42857142725034663711e-01,
    aT3  = -1.11111104054623557880e-01,
    aT4  =  9.09088713343650656196e-02,
    aT5  = -7.69187620504482999495e-02,
    aT6  =  6.66107313738753120669e-02,
    aT7  = -5.83357013379057348645e-02,
    aT8  =  4.97687799461593236017e-02,
    aT9  = -3.65315727442169155270e-02,
    aT10 =  1.62858201153657823623e-02;

static double atanhi(int i) {
    switch (i) { case 0: return atanhi0; case 1: return atanhi1;
                 case 2: return atanhi2; default: return atanhi3; }
}
static double atanlo(int i) {
    switch (i) { case 0: return atanlo0; case 1: return atanlo1;
                 case 2: return atanlo2; default: return atanlo3; }
}

/*
 * The arctangent.
 *
 * Four anchor points rather than one: the interval is split at 7/16,
 * 11/16, 19/16 and 39/16, and on each piece the argument is transformed
 * to (x - a)/(1 + a*x) before the series is applied. That keeps |t|
 * under about 0.1 everywhere, which is what lets eleven terms reach full
 * precision over an unbounded range.
 */
double atan(double x) {
    int32_t hx = (int32_t)hi32(x);
    int32_t ix = hx & 0x7FFFFFFF;

    if (ix >= 0x44100000) {                    /* |x| >= 2^66: atan is pi/2 */
        if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lo32(x) != 0)) return x + x;
        return (hx > 0) ? atanhi3 + 0x1p-1000 : -atanhi3 - 0x1p-1000;
    }

    int id;
    if (ix < 0x3FDC0000) {                     /* |x| < 7/16 */
        if (ix < 0x3E200000) {                 /* |x| < 2^-29 */
            if (ix < 0x00100000) {
                /* Force an underflow so a subnormal argument does not
                 * silently return itself with the flag unset -- the
                 * result is the argument either way. */
                volatile double t = x * x;
                (void)t;
            }
            return x;
        }
        id = -1;
    } else {
        x = fabs(x);
        if (ix < 0x3FF30000) {
            if (ix < 0x3FE60000) { id = 0; x = (2.0 * x - 1.0) / (2.0 + x); }
            else                 { id = 1; x = (x - 1.0) / (x + 1.0); }
        } else if (ix < 0x40038000) {
            id = 2; x = (x - 1.5) / (1.0 + 1.5 * x);
        } else {
            id = 3; x = -1.0 / x;
        }
    }

    double z = x * x;
    double w = z * z;
    double s1 = z * (aT0 + w * (aT2 + w * (aT4 + w * (aT6 + w * (aT8 + w * aT10)))));
    double s2 = w * (aT1 + w * (aT3 + w * (aT5 + w * (aT7 + w * aT9))));

    if (id < 0) return x - x * (s1 + s2);
    z = atanhi(id) - ((x * (s1 + s2) - atanlo(id)) - x);
    return (hx < 0) ? -z : z;
}

double atan2(double y, double x) {
    int32_t hx = (int32_t)hi32(x); uint32_t lx = lo32(x);
    int32_t hy = (int32_t)hi32(y); uint32_t ly = lo32(y);
    int32_t ix = hx & 0x7FFFFFFF;
    int32_t iy = hy & 0x7FFFFFFF;

    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0) ||
        iy > 0x7FF00000 || (iy == 0x7FF00000 && ly != 0))
        return x + y;                          /* either is NaN */

    if (hx == 0x3FF00000 && lx == 0) return atan(y);   /* x is exactly 1 */

    /* Two bits: the sign of y, and the sign of x. Together they name the
     * quadrant, which is the only thing atan2 knows that atan does not. */
    int m = (int)(((hy >> 31) & 1) | ((hx >> 30) & 2));

    if ((iy | (int32_t)ly) == 0) {             /* y is zero */
        switch (m) {
        case 0: case 1: return y;              /* +-0 for x > 0 */
        case 2: return  2.0 * pio2_hi_a + 0x1p-1000;   /* +pi for x < 0 */
        default: return -2.0 * pio2_hi_a - 0x1p-1000;
        }
    }
    if ((ix | (int32_t)lx) == 0)               /* x is zero */
        return (hy < 0) ? -pio2_hi_a - 0x1p-1000 : pio2_hi_a + 0x1p-1000;

    if (ix == 0x7FF00000) {                    /* x is infinite */
        if (iy == 0x7FF00000) {
            switch (m) {
            case 0: return  pio4_hi_a + 0x1p-1000;
            case 1: return -pio4_hi_a - 0x1p-1000;
            case 2: return  3.0 * pio4_hi_a + 0x1p-1000;
            default: return -3.0 * pio4_hi_a - 0x1p-1000;
            }
        }
        switch (m) {
        case 0: return  0.0;
        case 1: return -0.0;
        case 2: return  2.0 * pio2_hi_a + 0x1p-1000;
        default: return -2.0 * pio2_hi_a - 0x1p-1000;
        }
    }
    if (iy == 0x7FF00000)                      /* y is infinite, x is not */
        return (hy < 0) ? -pio2_hi_a - 0x1p-1000 : pio2_hi_a + 0x1p-1000;

    /* The ratio's own exponent decides whether it is worth computing:
     * beyond 2^60 the arctangent is pi/2 to every bit a double has, and
     * the division would only introduce error. */
    int32_t k = (iy - ix) >> 20;
    double z;
    if (k > 60) z = pio2_hi_a + 0.5 * pio2_lo_a;
    else if (hx < 0 && k < -60) z = 0.0;
    else z = atan(fabs(y / x));

    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return 2.0 * pio2_hi_a - (z - 2.0 * pio2_lo_a);
    default: return (z - 2.0 * pio2_lo_a) - 2.0 * pio2_hi_a;
    }
}

/* ===== 13. THE HYPERBOLIC FUNCTIONS ===== */

double sinh(double x) {
    if (isnan(x) || isinf(x)) return x;
    double ax = fabs(x);

    if (ax < 22.0) {
        if (ax < 0x1p-28) return x;
        double t = expm1(ax);
        /* (t + t/(t+1))/2 rather than (e^x - e^-x)/2, because for small
         * x the second form subtracts two nearly equal numbers and loses
         * every significant bit. */
        if (ax < 1.0) return copysign(0.5 * (2.0 * t - t * t / (t + 1.0)), x);
        return copysign(0.5 * (t + t / (t + 1.0)), x);
    }
    if (ax < 709.7822265625) return copysign(0.5 * exp(ax), x);
    /* Beyond that, halving after the exponential would overflow, so the
     * halving is folded into the argument. */
    if (ax <= 710.4758600739439) {
        double w = exp(0.5 * ax);
        return copysign(0.5 * w * w, x);
    }
    errno = ERANGE;
    return copysign(HUGE_VAL * HUGE_VAL, x);
}

double cosh(double x) {
    double ax = fabs(x);
    if (isnan(x)) return x;
    if (isinf(x)) return ax;

    if (ax < 0.5 * M_LN2) {
        double t = expm1(ax);
        return 1.0 + (t * t) / (2.0 * (1.0 + t));
    }
    if (ax < 22.0) {
        double t = exp(ax);
        return 0.5 * t + 0.5 / t;
    }
    if (ax < 709.7822265625) return 0.5 * exp(ax);
    if (ax <= 710.4758600739439) {
        double w = exp(0.5 * ax);
        return 0.5 * w * w;
    }
    errno = ERANGE;
    return HUGE_VAL * HUGE_VAL;
}

double tanh(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(1.0, x);

    double ax = fabs(x);
    double z;
    if (ax < 22.0) {
        if (ax < 0x1p-55) return x * (1.0 + x);
        if (ax >= 1.0) {
            double t = expm1(2.0 * ax);
            z = 1.0 - 2.0 / (t + 2.0);
        } else {
            double t = expm1(-2.0 * ax);
            z = -t / (t + 2.0);
        }
    } else {
        z = 1.0;
    }
    return copysign(z, x);
}

double asinh(double x) {
    double ax = fabs(x);
    if (isnan(x) || isinf(x)) return x + x;

    if (ax < 0x1p-28) return x;
    double w;
    if (ax > 0x1p28) {
        /* log(2x) — the square root under the radical is x to every bit
         * a double holds. */
        w = log(ax) + M_LN2;
    } else if (ax > 2.0) {
        w = log(2.0 * ax + 1.0 / (sqrt(ax * ax + 1.0) + ax));
    } else {
        double t = x * x;
        w = log1p(ax + t / (1.0 + sqrt(1.0 + t)));
    }
    return copysign(w, x);
}

double acosh(double x) {
    if (isnan(x)) return x;
    if (x < 1.0) { errno = EDOM; return (x - x) / (x - x); }
    if (x == 1.0) return 0.0;
    if (isinf(x)) return x;

    if (x > 0x1p28) return log(x) + M_LN2;
    if (x > 2.0) return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
    /* Near one, log(x + sqrt(x^2-1)) loses everything; log1p of the
     * displacement from one does not. */
    double t = x - 1.0;
    return log1p(t + sqrt(2.0 * t + t * t));
}

double atanh(double x) {
    double ax = fabs(x);
    if (isnan(x)) return x;
    if (ax > 1.0) { errno = EDOM; return (x - x) / (x - x); }
    if (ax == 1.0) { errno = ERANGE; return copysign(1.0, x) / 0.0; }
    if (ax < 0x1p-28) return x;

    double t;
    if (ax < 0.5) {
        t = ax + ax;
        t = 0.5 * log1p(t + t * ax / (1.0 - ax));
    } else {
        t = 0.5 * log1p((ax + ax) / (1.0 - ax));
    }
    return copysign(t, x);
}

/* ===== 14. ROOTS AND MAGNITUDES ===== */

/*
 * The cube root.
 *
 * A rough approximation obtained by dividing the exponent by three
 * directly in the bit pattern, then two Newton steps. The bit trick is
 * exact enough to give about five significant bits at no cost, which is
 * what lets two iterations finish the job — starting from nothing would
 * need five.
 */
double cbrt(double x) {
    /*
     * B1 and B2 are (1023 - 1023/3) shifted into the exponent field,
     * with a small bias that minimises the worst-case error of the
     * estimate. Dividing the biased exponent by three is division by
     * three of the true exponent plus a constant, and the constant is
     * what these absorb.
     */
    static const uint32_t B1 = 715094163, B2 = 696219795;
    static const double
        P0 =  1.87595182427177009643,
        P1 = -1.88497979543377169875,
        P2 =  1.621429720105354466140,
        P3 = -0.758397934778766047437,
        P4 =  0.145996192886612446982;

    uint64_t ux = dtou(x);
    uint32_t hx = (uint32_t)(ux >> 32) & 0x7FFFFFFF;

    if (hx >= 0x7FF00000) return x + x;           /* inf or NaN */

    /*
     * The rough estimate: divide the exponent by three in the bit
     * pattern and leave the mantissa alone. That is worth about five
     * significant bits for no arithmetic at all, which is what makes two
     * refinement steps enough where starting from nothing would need
     * five.
     */
    double t;
    if (hx < 0x00100000) {                        /* zero or subnormal */
        t = 0x1p54 * x;
        uint64_t ut = dtou(t);
        if (((uint32_t)(ut >> 32) & 0x7FFFFFFF) == 0) return x;   /* zero */
        hx = (uint32_t)(ut >> 32) & 0x7FFFFFFF;
        t = utod((ux & 0x8000000000000000ull) |
                 ((uint64_t)(hx / 3 + B2) << 32));
    } else {
        t = utod((ux & 0x8000000000000000ull) |
                 ((uint64_t)(hx / 3 + B1) << 32));
    }

    /*
     * One rational step to about twenty-three bits.
     *
     * r is t^3/x, which is one when t is right -- so the polynomial is
     * evaluated around one, where a short series is accurate. Written as
     * (t*t)*(t/x) rather than (t*t*t)/x because the first form cannot
     * overflow for a large x or underflow for a small one: each factor
     * stays within range even when their product's numerator would not.
     */
    double r = (t * t) * (t / x);
    t = t * ((P0 + r * (P1 + r * P2)) + ((r * r) * r) * (P3 + r * P4));

    /* Round the estimate to twenty-three bits, away from zero. That
     * makes t*t below exact, which is what the error bound on the final
     * step depends on. */
    t = utod((dtou(t) + 0x80000000ull) & 0xFFFFFFFFC0000000ull);

    /*
     * One Newton step, arranged so nothing cancels.
     *
     * The textbook form is t - (t^3 - x)/(3t^2), whose numerator is a
     * difference of two nearly equal numbers and loses most of its
     * significance. This form computes the same correction as a ratio of
     * quantities that are not close, and the result is good to about
     * two-thirds of an ulp.
     */
    double sq = t * t;                            /* exact */
    r = x / sq;
    double w = t + t;                             /* exact */
    r = (r - t) / (w + r);
    return t + t * r;
}

/*
 * The Euclidean distance, without the overflow.
 *
 * sqrt(x*x + y*y) is wrong for any x above about 1e154: the square
 * overflows to infinity and the answer becomes infinity for a result
 * that is perfectly representable. Scaling both arguments by a power of
 * two before squaring and scaling the result back is exact — a power of
 * two multiplies without error — and moves the whole computation into
 * the range where it is safe.
 */
double hypot(double x, double y) {
    uint32_t ha = hi32(x) & 0x7FFFFFFF;
    uint32_t hb = hi32(y) & 0x7FFFFFFF;

    if (hb > ha) { uint32_t t = ha; ha = hb; hb = t; double u = x; x = y; y = u; }
    x = fabs(x);
    y = fabs(y);

    if (ha >= 0x7FF00000) {
        /* An infinity wins over a NaN, which is what the standard says
         * and what a distance ought to mean. */
        if (ha == 0x7FF00000 && lo32(x) == 0) return x;
        if (hb == 0x7FF00000 && lo32(y) == 0) return y;
        return x + y;
    }
    if (hb == 0) return x;

    /* More than 2^60 apart: the smaller cannot change the answer. */
    if (ha - hb > 0x3C00000) return x + y;

    int k = 0;
    if (ha > 0x5F300000) {                     /* x > 2^500 */
        x *= 0x1p-600; y *= 0x1p-600; k = 600;
    } else if (hb < 0x20B00000) {              /* y < 2^-500 */
        if (hb <= 0x000FFFFF) {                /* subnormal */
            x *= 0x1p1022; y *= 0x1p1022; k = -1022;
        } else {
            x *= 0x1p600; y *= 0x1p600; k = -600;
        }
    }

    double w = sqrt(x * x + y * y);
    return k ? scalbn(w, k) : w;
}

double nextafter(double x, double y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;

    uint64_t ux = dtou(x);
    if (x == 0.0) return copysign(utod(1), y);   /* the smallest subnormal */

    /* Toward y means away from zero when |y| > |x| and toward it
     * otherwise, and in the bit pattern of a float those are exactly one
     * increment and one decrement of the magnitude. That is the whole
     * reason IEEE 754 orders the exponent above the mantissa. */
    if ((x < y) == (x > 0.0)) ux++;
    else ux--;
    return utod(ux);
}

/* ===== 15. FUSED MULTIPLY-ADD =====
 *
 * x*y + z with a single rounding, which is observably different from
 * computing the two operations separately -- that difference is the
 * whole reason the function is in the standard, so a version that simply
 * wrote `x*y+z` would be a lie that numerical code eventually catches.
 *
 * Two implementations, chosen once by asking the processor what it has.
 * The instruction is exact by definition. The fallback computes the
 * product exactly as a pair of doubles by Dekker's splitting -- 53 bits
 * times 53 bits is 106 bits, and two doubles hold 106 bits -- then sums
 * the three terms in decreasing order of magnitude. That is accurate to
 * well under an ulp but is not, in the last-bit sense, a single
 * rounding; on a processor made in this century it is not the path
 * taken.
 */
#if defined(__x86_64__)
__attribute__((target("fma")))
static double fma_hw(double x, double y, double z) {
    double r;
    __asm__("vfmadd213sd %2, %1, %0" : "=x"(r) : "x"(y), "x"(z), "0"(x));
    return r;
}

/* Asked once and remembered. CPUID is a serialising instruction and this
 * function is called from inner loops. */
static int fma_have = -1;

static int cpu_has_fma(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(1), "c"(0));
    return (c >> 12) & 1u;
}
#endif

double fma(double x, double y, double z) {
#if defined(__x86_64__)
    if (fma_have < 0) fma_have = cpu_has_fma();
    if (fma_have) return fma_hw(x, y, z);
#endif

    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return x * y + z;

    /* Dekker's split: 2^27 + 1 breaks a double into two halves whose
     * product with each other is exact. */
    const double split = 134217729.0;
    double xh = (x * split); xh = xh - (xh - x);
    double xl = x - xh;
    double yh = (y * split); yh = yh - (yh - y);
    double yl = y - yh;

    double p  = x * y;
    double pe = ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;

    double s = p + z;
    double bv = s - p;
    double se = (p - (s - bv)) + (z - bv);

    return s + (se + pe);
}

/* ===== 16. THE GAMMA AND ERROR FUNCTIONS =====
 *
 * Present because C++ headers declare them and a program that never
 * calls one still will not link without it. Implemented by the standard
 * series rather than by minimax tables: they are accurate to a few units
 * in the last place rather than under one, which is the honest
 * description and is adequate for every caller in this system, none of
 * which is doing statistics.
 */
/*
 * The Lanczos approximation, with g = 607/128 and fifteen coefficients.
 *
 * The parameter choice is not free. The obvious set — g = 7 with nine
 * terms — is the one that appears in every textbook and it is about two
 * digits short: it reaches roughly 1e-15 relative, which for a gamma
 * value of 1e173 is a thousand units in the last place. This set reaches
 * the full width of a double, and costs six more divisions.
 *
 *   gamma(z+1) = sqrt(2pi) * (z+g+1/2)^(z+1/2) * e^-(z+g+1/2) * A(z)
 */
#define LANCZOS_G (607.0 / 128.0)

static const double lanczos_c[15] = {
     0.99999999999999709182,
    57.156235665862923517,
   -59.597960355475491248,
    14.136097974741747174,
    -0.49191381609762019978,
     0.33994649984811888699e-4,
     0.46523628927048575665e-4,
    -0.98374475304879564677e-4,
     0.15808870322491248884e-3,
    -0.21026444172410488319e-3,
     0.21743961811521264320e-3,
    -0.16431810653676389022e-3,
     0.84418223983852743293e-4,
    -0.26190838401581408670e-4,
     0.36899182659531622704e-5
};

static double lanczos_a(double z) {
    double a = lanczos_c[0];
    for (int k = 1; k < 15; k++) a += lanczos_c[k] / (z + (double)k);
    return a;
}

/* log gamma for a positive argument, straight from the formula. */
static double lgamma_pos(double x) {
    double z = x - 1.0;
    double t = z + LANCZOS_G + 0.5;
    return 0.5 * log(2.0 * M_PI) + (z + 0.5) * log(t) - t + log(lanczos_a(z));
}

double lgamma(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return HUGE_VAL;
    /* The two exact zeros, which no approximation reaches by accident
     * and which callers do compare against. */
    if (x == 1.0 || x == 2.0) return 0.0;

    if (x < 0.5) {
        /* The reflection formula. The sine is zero exactly at the
         * non-positive integers, which are the poles. */
        double s = sin(M_PI * x);
        if (s == 0.0) { errno = ERANGE; return HUGE_VAL; }
        return log(M_PI / fabs(s)) - lgamma_pos(1.0 - x);
    }
    return lgamma_pos(x);
}

/*
 * The gamma function, computed directly rather than as exp(lgamma).
 *
 * Those are not the same computation and the difference is two orders of
 * magnitude in the result. exp() turns an *absolute* error in its
 * argument into a *relative* error in its answer, and lgamma(170) is
 * about 700 — so even a perfectly rounded logarithm, wrong by half an
 * ulp at 700, produces a gamma wrong by four hundred ulps. Nothing about
 * the quality of the logarithm can fix that; the exponential is where
 * the accuracy goes.
 *
 * Evaluating the power directly avoids it, because pow() carries its own
 * internal double-double logarithm and rounds once at the end. What it
 * costs is range: (z+g+1/2)^(z+1/2) alone overflows for z past about
 * 140, long before gamma itself does. So the power is taken in two
 * halves with the decaying exponential between them, which keeps every
 * intermediate inside the representable range while multiplying out to
 * the same value.
 */
double tgamma(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) {
        if (x > 0) return x;
        errno = EDOM;
        return (x - x) / (x - x);
    }
    if (x == 0.0) { errno = ERANGE; return copysign(HUGE_VAL, x); }
    if (x < 0.0 && x == floor(x)) { errno = EDOM; return (x - x) / (x - x); }

    if (x < 0.5) {
        double s = sin(M_PI * x);
        return M_PI / (s * tgamma(1.0 - x));
    }
    if (x > 171.61447887182298) { errno = ERANGE; return HUGE_VAL * HUGE_VAL; }

    /* Small factorials exactly. Every multiplication below 2^53 is
     * exact, so this is not merely more accurate than the general path
     * -- it is the true value, which is what a caller who wrote
     * tgamma(6) and expects 120 is entitled to. */
    if (x == floor(x) && x <= 19.0) {
        double r = 1.0;
        for (double k = 2.0; k < x; k += 1.0) r *= k;
        return r;
    }

    double z = x - 1.0;
    double t = z + LANCZOS_G + 0.5;
    double h = pow(t, (z + 0.5) * 0.5);
    return sqrt(2.0 * M_PI) * lanczos_a(z) * h * exp(-t) * h;
}

/*
 * The error function, by an all-positive series.
 *
 * The obvious series is the Maclaurin one, and it alternates: for x = 3
 * its largest term is exp(9), about eight thousand, and the sum is under
 * one. Thirteen bits of the answer are cancelled away before it appears.
 *
 * This is the confluent form,
 *
 *     erf(x) = (2x/sqrt(pi)) * exp(-x^2) * SUM (2x^2)^n / (1*3*5*...*(2n+1))
 *
 * in which every term is positive. Nothing cancels, so the accuracy is
 * the accuracy of the terms rather than of their difference, and the
 * series may be pushed out to where the continued fraction below takes
 * over cleanly.
 */
double erf(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(1.0, x);

    double ax = fabs(x);
    if (ax == 0.0) return x;

    /*
     * Below one, the series. Above it, one minus the complement — which
     * looks like the cancelling subtraction this function exists to
     * avoid and is the opposite of it. erfc is *small* up there, so
     * subtracting it from one loses nothing; it is erfc computed as
     * 1 - erf that would lose everything, which is why the two functions
     * cross over at the same point in opposite directions.
     */
    if (ax < 1.0) {
        double z    = ax * ax;
        double term = 1.0;
        double sum  = 1.0;
        /*
         * Compensated summation.
         *
         * The series is thirty-odd terms and every one of them rounds,
         * so a plain accumulation drifts by about the square root of
         * that many ulps — five or six, which is what erfc inherits when
         * it takes one minus this. Carrying the running remainder in
         * `comp` and folding it into the next addition costs one
         * subtraction per term and brings the whole sum back to a single
         * rounding.
         */
        double comp = 0.0;
        for (int n = 1; n < 200; n++) {
            term *= (2.0 * z) / (2.0 * (double)n + 1.0);
            double t = sum + term;
            /* Which operand is larger decides which of the two forms
             * recovers the lost bits exactly; sum dominates throughout. */
            comp += (sum - t) + term;
            sum = t;
            if (term < sum * 0x1p-60) break;
        }
        sum += comp;
        double r = M_2_SQRTPI * ax * exp(-z) * sum;
        return copysign(r, x);
    }
    return copysign(1.0 - erfc(ax), x);
}

/*
 * The complementary error function.
 *
 * Below four, computed as 1 - erf(x): the subtraction loses nothing
 * there because erf is not yet close to one. Above four it is, and the
 * subtraction would leave nothing at all -- erfc(6) is 2e-17, which is
 * smaller than the ulp of the 1 it would be subtracted from. So the
 * large branch uses the continued fraction, which computes erfc
 * directly and converges quickly once x is past about three.
 */
double erfc(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return x > 0 ? 0.0 : 2.0;

    /*
     * The crossover is at one, not at four, and the reason is
     * cancellation rather than convergence.
     *
     * 1 - erf(x) is exact enough only while erf(x) is comfortably below
     * one. By x = 4, erf is 1 - 1.5e-8: the subtraction leaves eight
     * significant digits out of sixteen, and by x = 6 it leaves none at
     * all, because erfc(6) is smaller than the ulp of the 1 it is being
     * taken from. At x = 1 the loss is about two bits, which is where
     * the continued fraction becomes the better of the two.
     */
    if (x < 1.0) {
        if (x < -1.0) return 2.0 - erfc(-x);
        return 1.0 - erf(x);
    }
    /* Beyond this the result is below the smallest subnormal. */
    if (x > 27.25) return 0.0;

    /*
     * erfc(x) = exp(-x^2)/sqrt(pi) * 1/(x + (1/2)/(x + 1/(x + (3/2)/(x + ...))))
     *
     * Evaluated from the bottom upward, which is unconditionally stable;
     * the forward recurrence for the same fraction is not.
     *
     * The depth is where the measured convergence actually is: past
     * about two and a half the fraction is at full precision within
     * forty terms, and down at one it needs three hundred. Using the
     * deep count everywhere would be simpler and would put two hundred
     * and fifty pointless divisions on the common path.
     */
    const int depth = (x < 3.0) ? 340 : 60;
    double f = 0.0;
    for (int k = depth; k >= 1; k--)
        f = (double)k * 0.5 / (x + f);

    /*
     * exp(-x^2), with x^2 computed exactly.
     *
     * Writing exp(-x*x) is one character shorter and wrong by hundreds
     * of ulps out here. x*x rounds, and at x = 23 the square is 530, so
     * half an ulp of it is about 6e-14 — an *absolute* error in the
     * argument, which the exponential converts into a *relative* error
     * of the same size in its answer. Two hundred and sixty units in the
     * last place, from a single rounding.
     *
     * Dekker's splitting gives the square as an exact pair: the high
     * half squared is exact because each factor has only 26 significant
     * bits, and the remainder is small enough that exp() of it is
     * accurate to its own last bit. Multiplying the two exponentials
     * back together costs one rounding and recovers all of it.
     *
     * This is the same failure the gamma function has when it is
     * computed as exp(lgamma), and the same answer: never hand an
     * exponential an argument that is large and inexact.
     */
    const double split = 134217729.0;             /* 2^27 + 1 */
    double xh = x * split; xh = xh - (xh - x);
    double xl = x - xh;
    double zh = xh * xh;                          /* exact */
    double zl = (xh + xh) * xl + xl * xl;

    return (exp(-zh) * exp(-zl)) / (sqrt(M_PI) * (x + f));
}

/* ===== 17. THE SINGLE-PRECISION FORMS =====
 *
 * Every one computed in double and rounded once. See the note in
 * <math.h> for why that is more accurate rather than less, and why it
 * cannot suffer double rounding here.
 */
float expf(float x)   { return (float)exp((double)x); }
float exp2f(float x)  { return (float)exp2((double)x); }
float expm1f(float x) { return (float)expm1((double)x); }
float logf(float x)   { return (float)log((double)x); }
float log2f(float x)  { return (float)log2((double)x); }
float log10f(float x) { return (float)log10((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float cbrtf(float x)  { return (float)cbrt((double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
float sinf(float x)   { return (float)sin((double)x); }
float cosf(float x)   { return (float)cos((double)x); }
float tanf(float x)   { return (float)tan((double)x); }
float asinf(float x)  { return (float)asin((double)x); }
float acosf(float x)  { return (float)acos((double)x); }
float atanf(float x)  { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float sinhf(float x)  { return (float)sinh((double)x); }
float coshf(float x)  { return (float)cosh((double)x); }
float tanhf(float x)  { return (float)tanh((double)x); }
float asinhf(float x) { return (float)asinh((double)x); }
float acoshf(float x) { return (float)acosh((double)x); }
float atanhf(float x) { return (float)atanh((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float remainderf(float x, float y) { return (float)remainder((double)x, (double)y); }
float tgammaf(float x) { return (float)tgamma((double)x); }
float lgammaf(float x) { return (float)lgamma((double)x); }
float erff(float x)    { return (float)erf((double)x); }
float erfcf(float x)   { return (float)erfc((double)x); }
float nextafterf(float x, float y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    fbits_t v; v.f = x;
    if (x == 0.0f) { v.u = 1; return copysignf(v.f, y); }
    if ((x < y) == (x > 0.0f)) v.u++;
    else v.u--;
    return v.f;
}

/* The bit operations are done in single precision directly: promoting
 * would work but costs two conversions to move bits that are already
 * where they need to be. */
float fabsf(float x) { fbits_t v; v.f = x; v.u &= 0x7FFFFFFFu; return v.f; }
float copysignf(float x, float y) {
    fbits_t a, b; a.f = x; b.f = y;
    a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u);
    return a.f;
}
float nanf(const char *tag) { (void)tag; fbits_t v; v.u = 0x7FC00000u; return v.f; }
float sqrtf(float x) {
#if defined(__x86_64__)
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    return __builtin_sqrtf(x);
#endif
}
float floorf(float x)  { return (float)floor((double)x); }
float ceilf(float x)   { return (float)ceil((double)x); }
float truncf(float x)  { return (float)trunc((double)x); }
float roundf(float x)  { return (float)round((double)x); }
float rintf(float x)   { return (float)rint((double)x); }
float nearbyintf(float x) { return (float)rint((double)x); }
float modff(float x, float *ip) {
    double di;
    float r = (float)modf((double)x, &di);
    *ip = (float)di;
    return r;
}
float frexpf(float x, int *e)  { return (float)frexp((double)x, e); }
float ldexpf(float x, int n)   { return (float)scalbn((double)x, n); }
float scalbnf(float x, int n)  { return (float)scalbn((double)x, n); }
float fdimf(float x, float y)  { return (float)fdim((double)x, (double)y); }
float fminf(float x, float y)  { return (float)fmin((double)x, (double)y); }
float fmaxf(float x, float y)  { return (float)fmax((double)x, (double)y); }
/* The one place where promoting is not merely adequate but exactly
 * right: a double multiply-add of two floats is already exact, because
 * 24 bits times 24 bits is 48 and a double holds 53. */
float fmaf(float x, float y, float z) {
    return (float)((double)x * (double)y + (double)z);
}
int   ilogbf(float x) { return ilogb((double)x); }
float logbf(float x)  { return (float)logb((double)x); }

/* ===== 18. THE EXTENDED-PRECISION FORMS =====
 *
 * Computed in double, which loses eleven mantissa bits and most of the
 * exponent range. <math.h> says so plainly and says why; these exist so
 * that C++ code which names them links, not so that it gets the extra
 * precision it may believe it is asking for.
 */
long double fabsl(long double x) { return (long double)fabs((double)x); }
long double sqrtl(long double x) { return (long double)sqrt((double)x); }
long double floorl(long double x) { return (long double)floor((double)x); }
long double ceill(long double x) { return (long double)ceil((double)x); }
long double truncl(long double x) { return (long double)trunc((double)x); }
long double roundl(long double x) { return (long double)round((double)x); }
long double rintl(long double x) { return (long double)rint((double)x); }
long double nearbyintl(long double x) { return (long double)rint((double)x); }
long double fmodl(long double x, long double y) {
    return (long double)fmod((double)x, (double)y);
}
long double remainderl(long double x, long double y) {
    return (long double)remainder((double)x, (double)y);
}
long double expl(long double x) { return (long double)exp((double)x); }
long double expm1l(long double x) { return (long double)expm1((double)x); }
long double logl(long double x) { return (long double)log((double)x); }
long double log1pl(long double x) { return (long double)log1p((double)x); }
long double log2l(long double x) { return (long double)log2((double)x); }
long double log10l(long double x) { return (long double)log10((double)x); }
long double powl(long double x, long double y) {
    return (long double)pow((double)x, (double)y);
}
long double sinl(long double x) { return (long double)sin((double)x); }
long double cosl(long double x) { return (long double)cos((double)x); }
long double tanl(long double x) { return (long double)tan((double)x); }
long double asinl(long double x) { return (long double)asin((double)x); }
long double acosl(long double x) { return (long double)acos((double)x); }
long double atanl(long double x) { return (long double)atan((double)x); }
long double atan2l(long double y, long double x) {
    return (long double)atan2((double)y, (double)x);
}
long double sinhl(long double x) { return (long double)sinh((double)x); }
long double coshl(long double x) { return (long double)cosh((double)x); }
long double tanhl(long double x) { return (long double)tanh((double)x); }
long double asinhl(long double x) { return (long double)asinh((double)x); }
long double acoshl(long double x) { return (long double)acosh((double)x); }
long double atanhl(long double x) { return (long double)atanh((double)x); }
long double cbrtl(long double x) { return (long double)cbrt((double)x); }
long double hypotl(long double x, long double y) {
    return (long double)hypot((double)x, (double)y);
}
long double tgammal(long double x) { return (long double)tgamma((double)x); }
long double lgammal(long double x) { return (long double)lgamma((double)x); }
long double ldexpl(long double x, int n) { return (long double)scalbn((double)x, n); }
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }
long double frexpl(long double x, int *e) { return (long double)frexp((double)x, e); }
long double modfl(long double x, long double *ip) {
    double di;
    long double r = (long double)modf((double)x, &di);
    *ip = (long double)di;
    return r;
}
long double copysignl(long double x, long double y) {
    return (long double)copysign((double)x, (double)y);
}
long double nextafterl(long double x, long double y) {
    return (long double)nextafter((double)x, (double)y);
}
long double fminl(long double x, long double y) {
    return (long double)fmin((double)x, (double)y);
}
long double fmaxl(long double x, long double y) {
    return (long double)fmax((double)x, (double)y);
}
long double fdiml(long double x, long double y) {
    return (long double)fdim((double)x, (double)y);
}
long double fmal(long double x, long double y, long double z) {
    return (long double)fma((double)x, (double)y, (double)z);
}
int         ilogbl(long double x) { return ilogb((double)x); }
long double logbl(long double x) { return (long double)logb((double)x); }
