#ifndef VX_FREESTANDING_ASSERT_H
#define VX_FREESTANDING_ASSERT_H
/*
 * third_party/include/assert.h
 *
 * assert() names the failure on the serial line and returns. It does not
 * stop the machine, and that is the deliberate part.
 *
 * Mbed TLS asserts on internal invariants, and some of those invariants
 * are reachable from a malformed certificate -- which arrives from
 * whoever this system just connected to. An abort() there would let a
 * stranger halt the desktop by serving a bad chain, which is a denial of
 * service with a one-line exploit. Carrying on means the caller's own
 * error path handles the input as invalid, which is what it is.
 *
 * NDEBUG compiles it away entirely, as the standard requires.
 */
void vx_log(const char *s);

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) \
    do { if (!(x)) { vx_log("[tls] assertion failed: " #x "\n"); } } while (0)
#endif

#endif
