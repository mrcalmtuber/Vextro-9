#ifndef _TIME_H
#define _TIME_H

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
 * time.h — how long, and how long ago.
 *
 * There is no calendar here. This machine has a real-time clock and the
 * kernel reads it at boot, but nothing carries that reading up to ring 3,
 * so time() answers from the same monotonic count everything else does
 * and the epoch is when the machine started rather than 1970. That is
 * stated plainly because a program that formats it will print a date in
 * January 1970 and the reason should not be a mystery.
 *
 * What is real is elapsed time, to a millisecond, from SYS_CLOCK — and
 * that is what every timeout in every ported library actually needs.
 */

#include <stddef.h>
#include <stdint.h>

typedef long time_t;
typedef long suseconds_t;
typedef long clock_t;

struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval  { time_t tv_sec; suseconds_t tv_usec; };

/*
 * The clocks.
 *
 * Both of them are the same count, and saying so is more useful than
 * pretending otherwise: there is one time source in this system and it
 * starts at zero when the machine boots. CLOCK_MONOTONIC is therefore
 * exactly right and CLOCK_REALTIME is a monotonic clock with the wrong
 * name, which for measuring an interval — which is what almost every
 * caller does — makes no difference at all.
 */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_REALTIME_COARSE    5

#define CLOCKS_PER_SEC 1000L

typedef int clockid_t;

int    clock_gettime(clockid_t id, struct timespec *ts);
int    clock_getres(clockid_t id, struct timespec *ts);
time_t time(time_t *out);
clock_t clock(void);
int    gettimeofday(struct timeval *tv, void *tz);

/* Sleep. Interruption is not something this system can do to a sleeping
 * thread, so the remainder is always zero and the return is always
 * zero — but the parameter is here because ported code passes it. */
int    nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int    usleep(unsigned long usec);

/* Milliseconds since boot, which is what this system actually keeps and
 * what everything above is derived from. Not standard; present because
 * it is the honest primitive. */
uint64_t vx_millis(void);


#ifdef __cplusplus
}
#endif

#endif /* _TIME_H */
