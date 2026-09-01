#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sys/random.h — where a modern port looks for entropy.
 *
 * The two functions are declared in <unistd.h> as well, which is where
 * this library defined them and where the older code looks. This header
 * exists because glibc has it and because libgcrypt's rndgetentropy.c
 * includes it by name: a port that finds the header missing takes a
 * different and worse backend — in libgcrypt's case one that reads
 * /dev/random by hand, or one that times its own instruction stream to
 * manufacture entropy out of jitter.
 *
 * Both go to SYS_RANDOM, which is RDSEED and RDRAND. The difference
 * between them is only the contract: getentropy fills the buffer
 * completely or fails, and getrandom may return fewer bytes than asked
 * and says how many. Neither ever blocks — there is no pool here to
 * drain and nothing to wait for.
 */

#include <stddef.h>

/* Accepted and ignored, because neither describes anything this source
 * can do: there is no blocking pool for GRND_NONBLOCK to skip and no
 * second source for GRND_RANDOM to select. Defined because callers pass
 * them. */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

int     getentropy(void *buf, size_t len);
ssize_t getrandom(void *buf, size_t len, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RANDOM_H */
