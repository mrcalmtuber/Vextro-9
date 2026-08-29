/*
 * tools/cxx_hostshim.h — what libcxx/ needs from a C library that the
 * host's does not have.
 *
 * Included with -include, so it arrives before anything else and can
 * declare a name the headers below then pull into namespace std.
 *
 * There is exactly one name. libc/include/stdio.h calls the stream form
 * of fputs `fputs_stream`, because in that library `fputs` has taken a
 * *descriptor* since before there were streams and renaming it would
 * silently break code already on disk. The host has no such history and
 * no such function, so it is defined here as what it is everywhere else.
 *
 * This is the same arrangement src/fs/ntfs/ntfs_hostshim.h uses to run
 * the filesystem driver against a file instead of a disk: the shim is
 * how a piece of this system is tested on a machine that is not it.
 */

#ifndef VX_CXX_HOSTSHIM_H
#define VX_CXX_HOSTSHIM_H

/* The C header, not <cstdio>: this file is included before everything
 * and <cstdio> is one of the headers under test, which would find this
 * declaration missing and fail on the `using` that imports it. */
#include <stdio.h>

static inline int fputs_stream(const char *s, FILE *f) {
    return fputs(s, f);
}

#endif
