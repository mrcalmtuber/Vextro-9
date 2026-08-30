/*
 * libc/calendar.c — turning a count of seconds into a date and back.
 *
 * See libc/include/time.h for why local time is UTC on this machine.
 *
 * ============================================================
 *  THE ALGORITHM, AND WHY IT HAS NO TABLE AND NO LOOP
 * ============================================================
 *
 * The obvious way to find a date from a day number is to subtract whole
 * years until what is left is less than one, then whole months, each
 * step asking whether the year is a leap year. It is a loop over
 * centuries for a date far from the epoch, and it is where the
 * off-by-one bugs live.
 *
 * Howard Hinnant's method removes both. Shift the year so that it starts
 * in March, which moves the leap day to the very end where it cannot
 * disturb anything before it; then the lengths of the twelve months from
 * March form a repeating 5-month pattern that a single linear expression
 * reproduces, and the leap rule becomes plain division inside a 400-year
 * era. Four multiplications, no branches, no table, and correct for
 * every date in the proleptic Gregorian calendar rather than only for
 * the range somebody thought to test.
 *
 * The kernel has the forward half of the same pair in src/gfx.h, where
 * it converts the CMOS reading into the seconds this file takes apart.
 * They are written out twice rather than shared because the kernel and
 * ring 3 do not share a translation unit, and the two are each ten lines.
 */

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* Local time is UTC here, so all three are constants. tzset has nothing
 * to read and nothing to change. */
static char tz_utc[] = "UTC";
char *tzname[2] = { tz_utc, tz_utc };
long  timezone = 0;
int   daylight = 0;

void tzset(void) {}

/* ---- civil <-> days ---- */

static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned  yoe = (unsigned)(y - era * 400);
    const unsigned  doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned  doe = (unsigned)(z - era * 146097);                /* [0, 146096] */
    const unsigned  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yr  = (long long)yoe + era * 400;
    const unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     /* [0, 365] */
    const unsigned  mp  = (5 * doy + 2) / 153;                         /* [0, 11], March = 0 */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yr + (*m <= 2);
}

/* ---- breaking a time_t apart ---- */

struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) { errno = EFAULT; return 0; }

    const long long secs = (long long)*t;

    /*
     * Floor division, not truncation. C's / rounds towards zero, so a
     * negative time_t -- any moment before 1970 -- would land on the day
     * *after* the right one and with a negative time of day. Dates
     * before the epoch are exactly the case nobody tests and exactly the
     * case a certificate's notBefore can contain.
     */
    long long days = secs / 86400;
    long long rem  = secs % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    long long y;
    unsigned  m, d;
    civil_from_days(days, &y, &m, &d);

    out->tm_sec  = (int)(rem % 60);
    out->tm_min  = (int)((rem / 60) % 60);
    out->tm_hour = (int)(rem / 3600);
    out->tm_mday = (int)d;
    out->tm_mon  = (int)m - 1;
    out->tm_year = (int)(y - 1900);

    /* 1970-01-01 was a Thursday, so day 0 is weekday 4; the modulo is
     * written to stay non-negative for days before the epoch. */
    out->tm_wday = (int)(((days % 7) + 11) % 7);
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));

    out->tm_isdst  = 0;
    out->tm_gmtoff = 0;
    out->tm_zone   = tz_utc;
    return out;
}

struct tm *localtime_r(const time_t *t, struct tm *out) {
    /* The whole of the difference between local time and UTC on this
     * machine. */
    return gmtime_r(t, out);
}

static struct tm shared_tm;

struct tm *gmtime(const time_t *t)    { return gmtime_r(t, &shared_tm); }
struct tm *localtime(const time_t *t) { return localtime_r(t, &shared_tm); }

/* ---- putting one back together ---- */

/*
 * timegm normalises in place, which is the property that makes it useful
 * for arithmetic: set tm_mday to 0 and you get the last day of the
 * previous month, add 400 to tm_yday's month and the year moves. The
 * normalisation falls out of the conversion rather than being done
 * separately -- every field is folded into a single second count, and
 * the count is then broken apart again.
 */
time_t timegm(struct tm *tm) {
    if (!tm) { errno = EFAULT; return (time_t)-1; }

    long long year  = (long long)tm->tm_year + 1900;
    long long month = tm->tm_mon;

    /* Months first, because a month outside 0..11 changes the year and
     * therefore which days_from_civil to ask about. */
    year  += month / 12;
    month %= 12;
    if (month < 0) { month += 12; year -= 1; }

    const long long days = days_from_civil(year, (unsigned)month + 1, 1)
                           + (long long)tm->tm_mday - 1;

    const long long secs = days * 86400
                         + (long long)tm->tm_hour * 3600
                         + (long long)tm->tm_min * 60
                         + (long long)tm->tm_sec;

    const time_t result = (time_t)secs;
    gmtime_r(&result, tm);      /* hand the caller back a normalised tm */
    return result;
}

time_t mktime(struct tm *tm) { return timegm(tm); }

double difftime(time_t end, time_t start) {
    return (double)end - (double)start;
}

/* ---- formatting ---- */

static const char *const wday_short[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const wday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};
static const char *const mon_short[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const mon_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* Appends to out if it fits; returns 0 and stops the whole format if it
 * does not, which is what strftime is specified to do -- a truncated
 * result is not returned, the call fails. */
static int put(char *out, size_t max, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= max) return 0;
        out[(*n)++] = *s++;
    }
    return 1;
}

static int put_num(char *out, size_t max, size_t *n, long long v,
                   int width, char pad) {
    char buf[32];
    int  i = 0;
    int  neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;

    do { buf[i++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (i < width) buf[i++] = pad;
    if (neg) buf[i++] = '-';

    while (i-- > 0) {
        if (*n + 1 >= max) return 0;
        out[(*n)++] = buf[i];
    }
    return 1;
}

size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm) {
    if (!out || !fmt || !tm || max == 0) return 0;

    size_t n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (n + 1 >= max) return 0;
            out[n++] = *fmt;
            continue;
        }

        fmt++;
        if (*fmt == 'E' || *fmt == 'O') fmt++;   /* locale modifiers: none here */

        const int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
        const int mo = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon : 0;
        int ok = 1;

        switch (*fmt) {
        case 'a': ok = put(out, max, &n, wday_short[wd]); break;
        case 'A': ok = put(out, max, &n, wday_long[wd]);  break;
        case 'b':
        case 'h': ok = put(out, max, &n, mon_short[mo]);  break;
        case 'B': ok = put(out, max, &n, mon_long[mo]);   break;

        case 'C': ok = put_num(out, max, &n, (tm->tm_year + 1900) / 100, 2, '0'); break;
        case 'd': ok = put_num(out, max, &n, tm->tm_mday, 2, '0'); break;
        case 'e': ok = put_num(out, max, &n, tm->tm_mday, 2, ' '); break;
        case 'H': ok = put_num(out, max, &n, tm->tm_hour, 2, '0'); break;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            ok = put_num(out, max, &n, h, 2, '0');
            break;
        }
        case 'j': ok = put_num(out, max, &n, tm->tm_yday + 1, 3, '0'); break;
        case 'm': ok = put_num(out, max, &n, tm->tm_mon + 1, 2, '0'); break;
        case 'M': ok = put_num(out, max, &n, tm->tm_min, 2, '0'); break;
        case 'n': ok = put(out, max, &n, "\n"); break;
        case 'p': ok = put(out, max, &n, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'S': ok = put_num(out, max, &n, tm->tm_sec, 2, '0'); break;
        case 't': ok = put(out, max, &n, "\t"); break;
        case 'u': ok = put_num(out, max, &n, wd == 0 ? 7 : wd, 1, '0'); break;
        case 'w': ok = put_num(out, max, &n, wd, 1, '0'); break;
        case 'y': ok = put_num(out, max, &n, (tm->tm_year + 1900) % 100, 2, '0'); break;
        case 'Y': ok = put_num(out, max, &n, tm->tm_year + 1900, 4, '0'); break;

        /* Both constants, for the reason given in time.h. */
        case 'Z': ok = put(out, max, &n, "UTC"); break;
        case 'z': ok = put(out, max, &n, "+0000"); break;

        case 'D': {
            char sub[16];
            snprintf(sub, sizeof sub, "%02d/%02d/%02d",
                     tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100);
            ok = put(out, max, &n, sub);
            break;
        }
        case 'F': {
            char sub[24];
            snprintf(sub, sizeof sub, "%04d-%02d-%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
            ok = put(out, max, &n, sub);
            break;
        }
        case 'R': {
            char sub[8];
            snprintf(sub, sizeof sub, "%02d:%02d", tm->tm_hour, tm->tm_min);
            ok = put(out, max, &n, sub);
            break;
        }
        case 'T': {
            char sub[12];
            snprintf(sub, sizeof sub, "%02d:%02d:%02d",
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            ok = put(out, max, &n, sub);
            break;
        }
        case 'c': {
            char sub[40];
            snprintf(sub, sizeof sub, "%s %s %2d %02d:%02d:%02d %d",
                     wday_short[wd], mon_short[mo], tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
            ok = put(out, max, &n, sub);
            break;
        }
        case 's': {
            struct tm copy = *tm;
            ok = put_num(out, max, &n, (long long)timegm(&copy), 1, '0');
            break;
        }

        case '%': if (n + 1 >= max) return 0; out[n++] = '%'; break;

        case '\0':
            /* A trailing '%' with nothing after it. Emit it and stop
             * rather than reading past the end of the format. */
            if (n + 1 >= max) return 0;
            out[n++] = '%';
            fmt--;
            break;

        default:
            /* An unknown conversion is copied through verbatim, which is
             * what lets a caller tell "this library does not know %G"
             * from "the value was empty". */
            if (n + 2 >= max) return 0;
            out[n++] = '%';
            out[n++] = *fmt;
            break;
        }

        if (!ok) return 0;
    }

    out[n] = '\0';
    return n;
}

/*
 * asctime's format is fixed by the standard, down to the column: exactly
 * 26 bytes including the newline and the terminator. The width of the
 * year is the reason it is not simply snprintf'd -- a year outside four
 * digits breaks the promise, and the standard leaves that undefined
 * rather than defining a wider field.
 */
char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) { errno = EFAULT; return 0; }
    const int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    const int mo = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon : 0;
    snprintf(buf, 26, "%s %s %2d %02d:%02d:%02d %4d\n",
             wday_short[wd], mon_short[mo], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime_r(const time_t *t, char *buf) {
    struct tm tmp;
    if (!localtime_r(t, &tmp)) return 0;
    return asctime_r(&tmp, buf);
}

static char shared_asctime[32];

char *asctime(const struct tm *tm) { return asctime_r(tm, shared_asctime); }
char *ctime(const time_t *t)       { return ctime_r(t, shared_asctime); }

/* ============================================================
 *  signals, which this system does not deliver
 * ============================================================
 *
 * The reasoning is in libc/include/signal.h. The two functions live here
 * rather than in a file of their own because between them they are
 * twenty lines and neither will grow until the kernel can push a signal
 * frame onto a user stack.
 */

#include <signal.h>
#include <stdlib.h>

__sighandler_t signal(int sig, __sighandler_t handler) {
    (void)sig;
    (void)handler;
    errno = ENOSYS;
    return SIG_ERR;
}

int raise(int sig) {
    static const char *const names[] = {
        [SIGHUP] = "SIGHUP",   [SIGINT] = "SIGINT",   [SIGQUIT] = "SIGQUIT",
        [SIGILL] = "SIGILL",   [SIGTRAP] = "SIGTRAP", [SIGABRT] = "SIGABRT",
        [SIGBUS] = "SIGBUS",   [SIGFPE] = "SIGFPE",   [SIGKILL] = "SIGKILL",
        [SIGSEGV] = "SIGSEGV", [SIGPIPE] = "SIGPIPE", [SIGALRM] = "SIGALRM",
        [SIGTERM] = "SIGTERM", [SIGSYS] = "SIGSYS",
    };

    const char *name = (sig > 0 && sig < NSIG && names[sig]) ? names[sig] : "signal";
    fprintf(stderr, "raise: %s, and there is no handler to run\n", name);

    /* The default action for every signal this header names. abort()
     * ends the process through the same path a failed assertion does. */
    abort();
    return 0;   /* not reached */
}
