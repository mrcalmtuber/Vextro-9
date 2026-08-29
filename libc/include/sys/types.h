#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

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
 * sys/types.h — the handful of names every POSIX header assumes.
 *
 * They are here rather than in whichever header first needed one
 * because several headers need the same ones and C, before C11, made a
 * repeated typedef an error rather than a no-op. The guards below are
 * the arrangement every C library that predates that rule uses, and are
 * kept for the same reason: a port may well define one of these itself
 * after testing for the macro, and a port that does should find its test
 * answered rather than its definition rejected.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef __DEFINED_ssize_t
typedef long ssize_t;
#define __DEFINED_ssize_t
#endif

#ifndef __DEFINED_off_t
/* Sixty-four bits, which on this machine is what `long` already is.
 * Named separately anyway, because ported code writes off_t and expects
 * a file offset rather than an int. */
typedef long off_t;
#define __DEFINED_off_t
#endif

#ifndef __DEFINED_pid_t
typedef int pid_t;
#define __DEFINED_pid_t
#endif

#ifndef __DEFINED_mode_t
typedef unsigned int mode_t;
#define __DEFINED_mode_t
#endif

#ifndef __DEFINED_uid_t
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#define __DEFINED_uid_t
#endif

#ifndef __DEFINED_ino_t
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
#define __DEFINED_ino_t
#endif

#ifndef __DEFINED_socklen_t
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
#define __DEFINED_socklen_t
#endif

#ifndef __DEFINED_useconds_t
typedef unsigned long useconds_t;
#define __DEFINED_useconds_t
#endif

/* The BSD spellings, which a surprising amount of portable code still
 * writes and which cost nothing to provide. */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;


#ifdef __cplusplus
}
#endif

#endif /* _SYS_TYPES_H */
