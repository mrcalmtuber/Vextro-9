#ifndef VX_FREESTANDING_ERRNO_H
#define VX_FREESTANDING_ERRNO_H
/*
 * third_party/include/errno.h
 *
 * A single global, not a per-thread one, and that is a real limitation
 * rather than a simplification: two threads failing at the same moment
 * overwrite each other's code.
 *
 * It is acceptable here only because almost nothing reads it. lwIP is
 * built with LWIP_PROVIDE_ERRNO, so the whole socket layer carries its
 * own; Mbed TLS returns its errors in the return value the way its API
 * is designed to. What is left is a couple of places that set errno and
 * never look at it.
 */
extern int errno;

#define EPERM        1
#define ENOENT       2
#define EINTR        4
#define EIO          5
#define EBADF        9
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define EINVAL      22
#define ENOSPC      28
#define EPIPE       32
#define ERANGE      34
#define ENOSYS      38

#endif
