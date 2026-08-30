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
 * time.h — how long, how long ago, and what day it is.
 *
 * The last of those is new. This header used to say there was no
 * calendar: the machine had a real-time clock, the kernel read it for
 * the taskbar, and nothing carried the reading across the system call
 * boundary, so time() answered from the monotonic tick and a program
 * that formatted the result printed a date in January 1970. SYS_WALLCLOCK
 * closed that, and the two quantities are now genuinely two:
 *
 *   CLOCK_MONOTONIC   the scheduler tick. Starts at zero when the
 *                     machine boots, never goes backwards, and is what
 *                     every timeout in this library is measured with.
 *   CLOCK_REALTIME    the CMOS clock, as seconds since 1970. Can jump,
 *                     can go backwards, and is the only one whose value
 *                     means anything to a person.
 *
 * ---- the timezone is UTC and cannot be anything else ----
 *
 * Nothing in this system records a timezone. The CMOS clock holds
 * whatever the firmware put in it and there is no second place saying
 * which zone that was, so localtime() is gmtime() here, tzname is
 * { "UTC", "UTC" }, and daylight is zero. That is stated as a fact
 * rather than hidden behind a function that returns a plausible offset
 * it made up.
 *
 * A program that needs real timezones wants ICU, which carries the
 * whole IANA database in its data archive and is not limited by this.
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


/* ===== the calendar ===== */

/*
 * struct tm, with the two fields everybody gets wrong: tm_year counts
 * from 1900, and tm_mon counts from zero. Both are the standard's and
 * neither is negotiable, because ported code does the arithmetic itself.
 *
 * tm_gmtoff and tm_zone are the BSD extensions. They are here because
 * ported code tests for them -- WebKit's own configure probes for both
 * by name -- and because a struct that has them and reports zero and
 * "UTC" is more useful than one that makes the probe fail.
 */
struct tm {
    int tm_sec;     /* 0..60, the 60 for a leap second        */
    int tm_min;     /* 0..59                                   */
    int tm_hour;    /* 0..23                                   */
    int tm_mday;    /* 1..31                                   */
    int tm_mon;     /* 0..11                                   */
    int tm_year;    /* years since 1900                        */
    int tm_wday;    /* 0..6, Sunday is 0                       */
    int tm_yday;    /* 0..365                                  */
    int tm_isdst;   /* always 0 here: there is no zone to have a rule */

    long        tm_gmtoff;   /* always 0: the clock is treated as UTC */
    const char *tm_zone;     /* always "UTC"                          */
};

/*
 * The reentrant forms take the destination; the others return a pointer
 * into storage this library owns, which two threads formatting a time at
 * once will fight over. The _r forms are the ones to use and the plain
 * ones are here because ported code calls them.
 */
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);

/*
 * mktime and timegm.
 *
 * Both turn a struct tm back into a time_t and both normalise the input
 * in place -- a tm_mday of 32 in January becomes the 1st of February,
 * which is what makes them useful for date arithmetic and is a
 * requirement rather than a convenience.
 *
 * They are the same function here, because local time is UTC here.
 * timegm is the non-standard one and is the one that says what it means.
 */
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);

double difftime(time_t end, time_t start);

/*
 * strftime, with the conversions this system can answer honestly.
 * %Z is "UTC" and %z is "+0000" for the reason above.
 */
size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm);

char *asctime_r(const struct tm *tm, char *buf);   /* buf: at least 26 */
char *ctime_r(const time_t *t, char *buf);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);

/*
 * The three variables the C library is expected to export, and their
 * honest values here.
 *
 * tzset() exists and does nothing, which is the correct behaviour for a
 * system with no TZ to read: it is specified to set these three from the
 * environment, there is no environment, and they are already right.
 */
extern char *tzname[2];
extern long  timezone;
extern int   daylight;

void tzset(void);


/* Milliseconds since boot, which is what the monotonic half of this
 * system actually keeps and what every interval above is derived from.
 * Not standard; present because it is the honest primitive. */
uint64_t vx_millis(void);


#ifdef __cplusplus
}
#endif

#endif /* _TIME_H */
