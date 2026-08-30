#ifndef _ERRNO_H
#define _ERRNO_H

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
 * errno.h — why the last call failed.
 *
 * Nothing in this system produced an errno until now, and that was not
 * an omission so much as a consequence: every system call here answers
 * -1 and writes a line to the serial port saying what it refused and
 * why. A person watching the wire gets a better diagnosis than a number
 * could give them, and the programs written against it were small enough
 * to check the return value and stop.
 *
 * Ported code is not written that way. It is written to distinguish
 * "there is no more memory" from "you asked for something that does not
 * exist", to retry on EINTR and to give up on EACCES, and it does so by
 * reading this variable. A library that finds errno permanently zero
 * concludes that every failure was a success with an odd return value.
 *
 * ---- why it is per-thread ----
 *
 * The declaration below is a function call, not a variable, which looks
 * like ceremony for something that could be an `extern int`. It is not.
 * Two threads failing two different calls at the same moment would
 * otherwise overwrite each other's answer, and the one that lost would
 * act on a reason belonging to the other. That is precisely the bug
 * errno had on real systems before threads, and precisely why every
 * modern C library defines it this way.
 *
 * A function and not a `__thread int`, and the difference matters here
 * more than it does on a hosted system. A thread-local variable is
 * addressed through the FS segment, which is only meaningful once a
 * thread pointer has been installed — and errno is set by malloc, which
 * runs before any of that. The function checks first and falls back to
 * the main thread's block, so errno works from the first instruction of
 * a program rather than from the first call to the threading library.
 *
 * The name is the one every C++ runtime and every ported library looks
 * for, which is why it is spelled this way rather than something local.
 */

int *__errno_location(void);

#define errno (*__errno_location())

/*
 * The numbers.
 *
 * Chosen to match Linux's, digit for digit, and that is worth stating as
 * a decision rather than leaving as a coincidence. Ported code contains
 * comparisons against these constants that survived a preprocessor pass
 * on some other machine -- a table of numeric error codes in a test
 * fixture, a switch compiled into a library, a value that crossed a
 * boundary as an integer. Choosing our own would work perfectly for
 * everything compiled here and break exactly the things that were not.
 */
#define EPERM            1  /* not permitted                            */
#define ENOENT           2  /* no such file or directory                */
#define ESRCH            3  /* no such process                          */
#define EINTR            4  /* interrupted                              */
#define EIO              5  /* input/output error                       */
#define ENXIO            6  /* no such device or address                */
#define E2BIG            7  /* argument list too long                   */
#define ENOEXEC          8  /* not an executable                        */
#define EBADF            9  /* bad file descriptor                      */
#define ECHILD          10  /* no child processes                       */
#define EAGAIN          11  /* try again                                */
#define ENOMEM          12  /* out of memory                            */
#define EACCES          13  /* permission denied                        */
#define EFAULT          14  /* bad address                              */
#define EBUSY           16  /* device or resource busy                  */
#define EEXIST          17  /* already exists                           */
#define EXDEV           18  /* cross-device link                        */
#define ENODEV          19  /* no such device                           */
#define ENOTDIR         20  /* not a directory                          */
#define EISDIR          21  /* is a directory                           */
#define EINVAL          22  /* invalid argument                         */
#define ENFILE          23  /* too many open files in the system        */
#define EMFILE          24  /* too many open files                      */
#define ENOTTY          25  /* not a terminal                           */
#define ENOSPC          28  /* no space left                            */
#define ESPIPE          29  /* illegal seek                             */
#define EROFS           30  /* read-only file system                    */
#define EMLINK          31  /* too many links                           */
#define EPIPE           32  /* broken pipe                              */
#define EDOM            33  /* argument outside a function's domain     */
#define ERANGE          34  /* result outside the representable range   */
#define EDEADLK         35  /* a deadlock would occur                   */
#define ENAMETOOLONG    36  /* name too long                            */
#define ENOSYS          38  /* not implemented                          */
#define ENOTEMPTY       39  /* directory not empty                      */
#define ELOOP           40  /* too many levels of symbolic links        */
#define ENOMSG          42  /* no message of the desired type           */
#define EOVERFLOW       75  /* value too large for its type             */
/* The bytes are not a valid character in the current encoding. Raised by
 * the UTF-8 codec in libc/wchar.c, which is the only place in this
 * system where a byte sequence can be malformed rather than merely
 * unexpected. */
#define EILSEQ          84  /* illegal byte sequence                    */
#define ENOTSOCK        88  /* not a socket                             */
#define ENOPROTOOPT     92  /* no such socket option                    */
#define EOPNOTSUPP      95  /* not supported                            */
#define EAFNOSUPPORT    97  /* address family not supported             */
#define EADDRINUSE      98  /* address already in use                   */
#define ENETDOWN       100  /* the network is not up                    */
#define ENETUNREACH    101  /* no route                                 */
#define ECONNABORTED   103  /* connection aborted                       */
#define ECONNRESET     104  /* connection reset by peer                 */
#define EISCONN        106  /* already connected                        */
#define ENOTCONN       107  /* not connected                            */
#define ETIMEDOUT      110  /* timed out                                */
#define ECONNREFUSED   111  /* connection refused                       */
#define EHOSTUNREACH   113  /* no route to host                         */
#define EALREADY       114  /* already in progress                      */
#define EINPROGRESS    115  /* in progress                              */
#define ECANCELED      125  /* cancelled                                */
#define ENOTRECOVERABLE 131 /* state not recoverable                    */

/* Two names for one number, as POSIX allows and as ported code assumes
 * in both directions. */
#define EWOULDBLOCK EAGAIN

/* A sentence for a number. Returns a pointer into static storage that
 * the caller must not modify, which is what strerror has always meant. */
char *strerror(int e);


#ifdef __cplusplus
}
#endif

#endif /* _ERRNO_H */
