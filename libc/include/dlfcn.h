#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dlfcn.h — resolving a symbol by name, on a system with no dynamic
 * linker.
 *
 * ---- what this is not ----
 *
 * It is not a loader. There are no shared objects on this target and
 * there is nothing to load one with: every program here is statically
 * linked, `CMAKE_FIND_LIBRARY_SUFFIXES` is `.a` and nothing else, and
 * the kernel's ELF loader reads PT_LOAD segments and stops. `dlopen`
 * below will never bring code into a process that was not already in it.
 *
 * ---- what it is, and who asked ----
 *
 * A *name table*. Several ported libraries are written as dispatchers:
 * they do not call an implementation, they look one up by name at run
 * time and call through the pointer. libepoxy is the one that forced
 * this — its whole purpose is to resolve some two thousand OpenGL entry
 * points through `dlsym`, and it is a REQUIRED dependency of WebKit's
 * WPE port, so a build gets no further without it.
 *
 * The alternative was to patch libepoxy, and the standing rule in this
 * tree is not to patch upstream. A `dlsym` that answers from a table the
 * program filled in is the smaller and more honest change: the library
 * asks the question it was written to ask, and gets a truthful answer —
 * including, for the great majority of those two thousand names, the
 * truthful answer *no*.
 *
 * ---- so a miss is the normal case, and is reported ----
 *
 * dlsym() returning null is not a failure of this file; it is this
 * system saying it does not have that function. The count is kept and
 * can be asked for, because "the program resolved four entry points and
 * missed nine hundred" is the single most useful sentence about a
 * graphics stack running here, and it is not visible any other way.
 */

#include <stddef.h>

/* Accepted and ignored, every one of them. They describe binding time
 * and symbol visibility for a loader that does not exist; a table has no
 * lazy mode and no local scope. Defined because callers pass them. */
#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_LOCAL   0x00000
#define RTLD_GLOBAL  0x00100
#define RTLD_NOLOAD  0x00004
#define RTLD_NODELETE 0x01000

/* Handles for the two pseudo-objects POSIX names. Both mean "this
 * program", which here is the only object there is. */
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1L)

/*
 * Open a named object.
 *
 * Answers a handle for a name some part of this program has registered a
 * table under, and null otherwise. A null `file` means the calling
 * program itself, which POSIX defines and which is the one case that is
 * always available.
 *
 * RTLD_NOLOAD asks whether an object is *already* loaded without loading
 * it. Since nothing is ever loaded, that question and the ordinary one
 * have the same answer here, which is the correct behaviour rather than
 * a simplification.
 */
void *dlopen(const char *file, int mode);
int   dlclose(void *handle);

/* Look a symbol up. Null if this system does not have it — which is the
 * common outcome and not an error. */
void *dlsym(void *handle, const char *name);

/* The last failure, or null if the last call succeeded. Cleared by
 * reading it, as POSIX requires. */
char *dlerror(void);

/*
 * ===== registering a table =====
 *
 * Not POSIX. This is the other half of the design and the reason the
 * file works at all: since nothing can be loaded, everything that can be
 * found has to be put here by the program before it is asked for.
 *
 * `vx_dl_register` associates a name — "libGL.so.1", say — with an array
 * of symbols, terminated by an entry whose name is null. The array is
 * borrowed, not copied, so it must outlive every dlopen of that object;
 * a static array in the provider is the intended shape.
 */
typedef struct {
    const char *name;
    void       *addr;
} vx_dl_symbol_t;

int vx_dl_register(const char *object, const vx_dl_symbol_t *symbols);

/* How many lookups have hit and missed since the program started. A
 * graphics stack that resolved four entry points out of nine hundred is
 * worth being able to say so about. */
unsigned long vx_dl_hits(void);
unsigned long vx_dl_misses(void);

/* The most recent name that was not found, or null. One slot rather than
 * a log: the first miss is nearly always the informative one, and a
 * dispatcher probes in a loop. */
const char *vx_dl_last_miss(void);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */
