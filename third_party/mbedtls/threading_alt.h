#ifndef MBEDTLS_THREADING_ALT_H
#define MBEDTLS_THREADING_ALT_H

/*
 * The mutex Mbed TLS locks, described in terms this file can see.
 *
 * MBEDTLS_THREADING_ALT splits the job in two: the library asks us for
 * the *type* here, and for the four *functions* at run time through
 * mbedtls_threading_set_alt(). That split is why this header can be
 * plain integers with no kernel included -- it is pulled in from the
 * middle of rsa.h, long before anything of ours exists, and an include
 * of sched.h from this depth would be circular.
 *
 * The behaviour lives in src/mtls.h, where these fields are driven by
 * the scheduler's own mutex: `owner` holds a thread slot rather than a
 * bare flag so that a second lock attempt by the thread that already
 * holds it is a diagnosable bug instead of a machine that stops.
 */
typedef struct mbedtls_threading_mutex_t {
    volatile unsigned int locked;
    volatile int          owner;    /* scheduler slot, -1 when free */
    volatile unsigned int waiters;
    int                   ready;    /* set by _init, cleared by _free */
} mbedtls_threading_mutex_t;

#endif /* MBEDTLS_THREADING_ALT_H */
