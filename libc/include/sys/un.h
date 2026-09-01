#ifndef _SYS_UN_H
#define _SYS_UN_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sys/un.h — the address of a socket in the filesystem.
 *
 * ---- what is here and what is not ----
 *
 * The *type* is here and is exactly the one every Unix defines, because
 * a structure layout is a fact that can be stated correctly whether or
 * not anything implements the protocol behind it. What is not here is a
 * socket that will accept one: this system's sockets are AF_INET and
 * SOCK_STREAM over lwIP, and src/syscall.h says so at the point it
 * refuses everything else — "a datagram socket is refused rather than
 * accepted and made to fail later".
 *
 * So `socket(AF_UNIX, ...)` is answered with EAFNOSUPPORT by the kernel,
 * at the call, naming the family. That is a refusal a program can act
 * on, and it is why this header does not need to pretend.
 *
 * ---- who asked ----
 *
 * libgpg-error's logging.c includes it unconditionally on every
 * non-Windows target and uses `struct sockaddr_un` in one function:
 * the one that redirects the log to a socket, which a program has to ask
 * for explicitly and libgcrypt never does. The include has to succeed;
 * the code behind it is never reached, and if it ever were it would be
 * refused at the socket() call rather than here.
 */

#include <sys/socket.h>

/* 108 bytes of path, which is not a round number and is the one every
 * Unix chose: it makes the whole structure 110 bytes, which fit in the
 * 128-byte sockaddr_storage of the day. Code that sizes a buffer from
 * sizeof(sun_path) gets the number it expects. */
#define UNIX_PATH_MAX 108

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[UNIX_PATH_MAX];
};

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UN_H */
