#ifndef _STDIO_H
#define _STDIO_H

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
 * stdio.h — formatting, streams, and files.
 *
 * This header used to open with "there are no files and no FILE objects
 * here", and it was true: a FILE was a name for one of two destinations
 * and fopen was deliberately not declared, so that a parser could not
 * conclude it had reached the end of a file it never opened.
 *
 * There are files now. A FILE is a real object over a real descriptor,
 * with a buffer, a position, and the end-of-file and error conditions
 * kept apart — and fread, fgets and the scanf family are here because
 * there is finally something for them to read.
 *
 * Two things about them are worth knowing before relying on them, and
 * both are consequences of the filesystem rather than of this library:
 *
 *   fflush does not mean the bytes are on the disk. It means this
 *   library is no longer holding any. The kernel keeps a file's image
 *   until the descriptor is closed, because the writer underneath
 *   replaces whole files rather than updating them in place — see
 *   <fcntl.h>. fclose, or fsync, is what puts it on the disk.
 *
 *   stdout and stderr are unbuffered. The reason a program writes to the
 *   terminal is usually to say what it is doing while it does it, and a
 *   buffered stderr is a diagnostic that arrives after the crash it was
 *   describing. setvbuf can change it.
 */

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)

/* The two descriptors every process starts with that reach the console.
 * A third, fd 0, exists and cannot be read from — there is no console
 * input in ring 3, and read() says so with EIO rather than answering
 * zero, which a parser would take for an empty file. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define BUFSIZ       4096
#define FILENAME_MAX 256
#define FOPEN_MAX    64
#define L_tmpnam     32

int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintf(const char *fmt, va_list ap);

int  sprintf(char *out, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
int  snprintf(char *out, size_t cap, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);

int  puts(const char *s);
int  putchar(int c);

/* ===== streams ===== */

typedef struct _VX_FILE FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *f);
int    fflush(FILE *f);
int    fileno(FILE *f);

size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);

int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *out, int cap, FILE *f);

int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);

/*
 * ---- fputs used to take a descriptor, and no longer does ----
 *
 * It was `int fputs(const char *, int fd)` in this library from before
 * there were streams, with the stream form under the name fputs_stream
 * beside it. The note that used to be here defended that: renaming it
 * "would break that code silently, since both a descriptor and a
 * pointer are integers to a compiler that has been given no prototype".
 *
 * That defence was measured and does not hold. There is always a
 * prototype — this header is the only way to reach the function — and
 * GCC 14 and later make int-from-pointer a hard **error**, not a
 * warning. Passing a descriptor to the standard form does not compile,
 * which is the opposite of silent.
 *
 * What the old signature did cost was every port that calls the
 * standard one. libepoxy is where it was found: `fputs(msg, stderr)` in
 * its dispatcher, which is plain C89, and which failed to compile
 * against a library claiming to provide fputs. That is a failure mode
 * with no bottom — it would recur for each of the remaining
 * dependencies, one port at a time.
 *
 * So fputs is the standard function now. fputs_stream is kept as an
 * alias of it because libcxx/ and tools/cxx_hostshim.h call it by that
 * name, and a rename there would be churn with nothing behind it.
 */
int    fputs(const char *s, FILE *f);
int    fputs_stream(const char *s, FILE *f);

int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
int    fseeko(FILE *f, off_t off, int whence);
off_t  ftello(FILE *f);
void   rewind(FILE *f);

int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);

int    setvbuf(FILE *f, char *buf, int mode, size_t size);
void   setbuf(FILE *f, char *buf);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int  fprintf(FILE *f, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
int  vfprintf(FILE *f, const char *fmt, va_list ap);

/* ===== reading formatted input ===== */

int  scanf(const char *fmt, ...)  __attribute__((format(scanf, 1, 2)));
int  fscanf(FILE *f, const char *fmt, ...)
        __attribute__((format(scanf, 2, 3)));
int  sscanf(const char *s, const char *fmt, ...)
        __attribute__((format(scanf, 2, 3)));
int  vscanf(const char *fmt, va_list ap);
int  vfscanf(FILE *f, const char *fmt, va_list ap);
int  vsscanf(const char *s, const char *fmt, va_list ap);

/* ===== files by name ===== */

int  remove(const char *path);
int  rename(const char *from, const char *to);

/* Raw, unformatted, straight to a descriptor. */
long write(int fd, const void *buf, size_t len);

int  vsprintf(char *out, const char *fmt, va_list ap);
int  vasprintf(char **out, const char *fmt, va_list ap);
int  asprintf(char **out, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));

int  perror_prefix(const char *s);
void perror(const char *s);


#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
