#ifndef _SYS_TIME_H
#define _SYS_TIME_H

/*
 * sys/time.h — the older half of the time interface.
 *
 * `struct timeval` and gettimeofday() are declared in <time.h> here,
 * because that is where this C library keeps everything to do with time
 * and splitting one subject across two headers to match a historical
 * accident helps nobody reading it. This header exists because portable
 * code includes it: gettimeofday and timeval came from BSD and were
 * never in ISO C, so a program that wants them has always had to reach
 * for sys/time.h, and every such program would otherwise stop at
 * "No such file or directory" over a declaration this library already
 * has.
 *
 * ICU 74's putil.cpp was the first to ask for it, on the very first
 * file compiled.
 *
 * ---- the timer macros ----
 *
 * timeradd, timersub and the three comparisons are the reason this is
 * not a one-line include. They are macros rather than functions in every
 * implementation, they are used by ported code, and they have one trap
 * worth knowing: `timercmp(a, b, >=)` is undefined on the historical
 * BSD version, which expanded to a form that only worked for < and >.
 * The versions below work for all six operators, which is what current
 * implementations do and what callers now assume.
 */

#include <time.h>

/* Microseconds and seconds, related. Kept as a macro so the arithmetic
 * happens in whatever type the caller's fields have. */
#define timerisset(tvp)     ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp)     ((tvp)->tv_sec = (tvp)->tv_usec = 0)

/*
 * Comparison. The subtraction form -- rather than the two-branch form
 * the original BSD header used -- is what makes >= and <= work: with
 * seconds equal, the result is decided entirely by the microseconds,
 * and with seconds different the microsecond term cannot change the
 * sign because it is bounded by a million.
 */
#define timercmp(a, b, CMP)                     \
    (((a)->tv_sec == (b)->tv_sec)               \
     ? ((a)->tv_usec CMP (b)->tv_usec)          \
     : ((a)->tv_sec  CMP (b)->tv_sec))

#define timeradd(a, b, res)                                 \
    do {                                                    \
        (res)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;        \
        (res)->tv_usec = (a)->tv_usec + (b)->tv_usec;       \
        if ((res)->tv_usec >= 1000000) {                    \
            (res)->tv_sec++;                                \
            (res)->tv_usec -= 1000000;                      \
        }                                                   \
    } while (0)

#define timersub(a, b, res)                                 \
    do {                                                    \
        (res)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;        \
        (res)->tv_usec = (a)->tv_usec - (b)->tv_usec;       \
        if ((res)->tv_usec < 0) {                           \
            (res)->tv_sec--;                                \
            (res)->tv_usec += 1000000;                      \
        }                                                   \
    } while (0)

#endif /* _SYS_TIME_H */
