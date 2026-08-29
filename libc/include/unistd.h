#ifndef _UNISTD_H
#define _UNISTD_H

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
 * unistd.h — the system calls, as C.
 *
 * This header used to begin by saying what was missing, and the list was
 * most of it: "There are no file descriptors in ring 3. There is no
 * open(), no read(), no close(), no directory, no socket." That was the
 * largest single thing standing between this system and a ported
 * application, and it is no longer true.
 *
 * What is here now is the ordinary set — open, read, write, close,
 * lseek, stat, unlink, mkdir, the directory walk in <dirent.h>, and
 * sockets in <sys/socket.h>. The rule the old header stated is kept,
 * because it is what made the gap legible: anything this system cannot
 * do is *absent* rather than stubbed, so a port fails to link against a
 * name that does not work instead of calling one that silently does
 * nothing.
 *
 * ---- what is still absent, and why ----
 *
 *   dup, dup2. On Unix a descriptor is a name for an open file
 *   description and dup makes a second name for one — which is why two
 *   duplicated descriptors share an offset. Here the descriptor *is* the
 *   description; there is no second layer for two names to point at, so
 *   dup could only be a copy, with two independent offsets, two
 *   write-back images of one file, and two closes of one socket. Every
 *   one of those is wrong in a way a program would discover as data
 *   loss.
 *
 *   pipe, exec, wait. There is no way to start a program from a program
 *   here — the loader is reached by typing at a terminal or clicking
 *   something — so a pipe would have nobody at the other end and exec
 *   would have nothing to replace this image with.
 *
 *   symlink, readlink, link. There are no links on this volume.
 *
 *   chown, getuid, setuid. There are accounts and there is a boundary
 *   between them, enforced on every path; it is not expressed as a
 *   numeric owner per file, so there is no number to report or change.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ===== descriptors ===== */

/*
 * Both 1 and 2 reach the terminal window; there is one console. Any
 * other descriptor is looked up in this process's table, so this is also
 * how a file and a socket are written to.
 */
ssize_t write(int fd, const void *buf, size_t len);

/*
 * Reading fd 0 fails with EIO and does not return zero.
 *
 * There is no console input in ring 3: the keyboard belongs to the
 * terminal window, and handing a background program a claim on it is a
 * feature with a design rather than a return value. Zero would mean end
 * of file, and a parser told it has reached the end of input it never
 * had concludes the input was empty and carries on.
 */
ssize_t read(int fd, void *buf, size_t len);

int   close(int fd);
off_t lseek(int fd, off_t off, int whence);

/*
 * fsync is what puts a written file on the disk, and on this system that
 * is a stronger statement than usual: nothing written through a
 * descriptor reaches the volume until this or close(). See <fcntl.h>.
 */
int   fsync(int fd);
int   fdatasync(int fd);
int   ftruncate(int fd, off_t len);

int   unlink(const char *path);
int   rmdir(const char *path);
int   isatty(int fd);

/* R_OK, W_OK and X_OK all answer the same question as F_OK here: whether
 * the file is there. What a program may do to it depends on an answer
 * somebody gives at the keyboard, which is not a property of the file
 * that could be reported in advance. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int   access(const char *path, int mode);

/* There is no per-process working directory: every path is resolved
 * from the root. getcwd answers "/" — which is true — and chdir accepts
 * only that, rather than accepting anything and leaving a program
 * building paths against a directory this system has never heard of. */
char *getcwd(char *buf, size_t size);
int   chdir(const char *path);

/* ===== the process ===== */

/* A copy of this process over a copy-on-write duplicate of its address
 * space. Returns 0 in the child and the child's identifier in the
 * parent, as it has since the seventh edition. The child gets a copy of
 * the open files and does *not* inherit sockets — one connection cannot
 * be read by two processes without each getting half of it. Do not
 * combine with threads: a fork duplicates the calling thread only, and
 * every lock the others held stays locked in the child forever. */
pid_t fork(void);
pid_t getpid(void);
pid_t gettid(void);

void  _exit(int status) __attribute__((noreturn));

unsigned int sleep(unsigned int seconds);
int          usleep(unsigned long usec);

/* Move the break. malloc's small path is built on this; a program that
 * calls it directly and then calls malloc will confuse the allocator,
 * which believes it owns everything above the break it last asked for. */
void *sbrk(long delta);

long sysconf(int name);
int  getpagesize(void);

/*
 * ===== entropy =====
 *
 * The kernel has had a hardware source since SYS_RANDOM was added and
 * ring 3 could reach it only through apps/vextro.h — which is the
 * *application* header, not the C library, so a ported library that
 * wanted a random number had nowhere to ask. SQLite's VFS is the first
 * thing to need one.
 *
 * getentropy fills the buffer completely or fails; that is its whole
 * contract and the reason to prefer it. The kernel is allowed to answer
 * short — RDRAND can fail under load and a short read is reported rather
 * than padded with something that looks random — so this loops, and
 * gives up rather than returning a partly-filled buffer somebody would
 * use as a key.
 *
 * 256 bytes maximum, as on every system that has it.
 */
int getentropy(void *buf, size_t len);

/* The Linux spelling, which returns a count and may legitimately return
 * fewer bytes than asked for. `flags` is accepted and ignored: there is
 * one source here and no blocking/non-blocking distinction to make. */
ssize_t getrandom(void *buf, size_t len, unsigned int flags);


#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
