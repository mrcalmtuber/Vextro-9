#ifndef VX_FREESTANDING_TIME_H
#define VX_FREESTANDING_TIME_H

/*
 * third_party/include/time.h
 *
 * Mbed TLS is built with MBEDTLS_HAVE_TIME off, so nothing in the
 * vendored set actually calls these -- they exist because a few files
 * include the header unconditionally and then use it only inside
 * #if defined(MBEDTLS_HAVE_TIME).
 *
 * The consequence is worth stating where TLS is described rather than
 * only here: with no clock the library cannot check a certificate's
 * notBefore and notAfter, so an expired certificate is not distinguished
 * from a current one. Which matters less than it sounds, because this
 * build does not verify certificates at all.
 */

#include <stddef.h>

typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t     time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);

#endif
