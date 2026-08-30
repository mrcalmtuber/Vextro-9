/*
 * libc/dlfcn.c — dlopen and dlsym over a table, because there is no
 * dynamic linker to put underneath them.
 *
 * The reasoning is at the top of libc/include/dlfcn.h and the short of
 * it is: libepoxy is a dispatcher, it resolves entry points by name at
 * run time, WebKit requires it, and the choice was between patching
 * upstream and answering the question upstream actually asks. This
 * answers it — truthfully, which for most of the two thousand OpenGL
 * names it asks about means "no".
 *
 * Everything here is bounded and static. No allocation, no locking: a
 * table is registered before threads exist (a provider does it from a
 * constructor or from the top of main) and is read-only afterwards, so
 * the only concurrent access is concurrent reads.
 */

#include <dlfcn.h>
#include <stddef.h>

#define DL_MAX_OBJECTS 8

typedef struct {
    const char           *name;
    const vx_dl_symbol_t *symbols;
} dl_object_t;

static dl_object_t dl_objects[DL_MAX_OBJECTS];
static int         dl_object_count = 0;

static const char *dl_error_text = NULL;
static unsigned long dl_hit_count = 0;
static unsigned long dl_miss_count = 0;
static const char *dl_miss_name = NULL;

/*
 * The handle a program is given.
 *
 * The index plus one, cast to a pointer, rather than the address of the
 * table entry. Two reasons and both are about what a caller can do with
 * it: an index cannot be dereferenced into this file's memory by a
 * program that treats the handle as a struct pointer, and one is not
 * null, which matters because null is dlopen's failure value and is also
 * RTLD_DEFAULT.
 */
static void *dl_handle_for(int index) {
    return (void *)(unsigned long)(index + 1);
}

static int dl_index_of(void *handle) {
    const unsigned long v = (unsigned long)handle;
    if (v == 0 || v > (unsigned long)dl_object_count) return -1;
    return (int)v - 1;
}

static int dl_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int vx_dl_register(const char *object, const vx_dl_symbol_t *symbols) {
    if (!object || !symbols) return -1;
    for (int i = 0; i < dl_object_count; i++) {
        if (dl_streq(dl_objects[i].name, object)) {
            /* Re-registering replaces, which is what a provider that is
             * initialised twice should get: one table, not two entries
             * where the first shadows the second. */
            dl_objects[i].symbols = symbols;
            return 0;
        }
    }
    if (dl_object_count >= DL_MAX_OBJECTS) return -1;
    dl_objects[dl_object_count].name    = object;
    dl_objects[dl_object_count].symbols = symbols;
    dl_object_count++;
    return 0;
}

void *dlopen(const char *file, int mode) {
    (void)mode;   /* see the note in the header: nothing is ever loaded,
                   * so binding time and scope describe nothing */

    /*
     * A null name means this program, which POSIX defines and which is
     * the one object that always exists. It is given index zero's
     * handle only if something registered a table for it; otherwise it
     * still succeeds, with an empty table, because "the program has no
     * dynamic symbols" is true rather than an error.
     */
    if (!file) {
        dl_error_text = NULL;
        return dl_handle_for(DL_MAX_OBJECTS);   /* the empty self-handle */
    }

    for (int i = 0; i < dl_object_count; i++) {
        if (dl_streq(dl_objects[i].name, file)) {
            dl_error_text = NULL;
            return dl_handle_for(i);
        }
    }

    dl_error_text = "no such object on this system; nothing is loadable "
                    "here and nothing registered that name";
    return NULL;
}

int dlclose(void *handle) {
    (void)handle;
    /* Nothing was opened, so nothing is closed. Zero is success, which
     * is the truthful answer: the caller's obligation is discharged. */
    dl_error_text = NULL;
    return 0;
}

void *dlsym(void *handle, const char *name) {
    if (!name) {
        dl_error_text = "dlsym: null name";
        return NULL;
    }

    const int idx = dl_index_of(handle);

    /*
     * RTLD_DEFAULT and the self-handle both mean "search everything",
     * which here is every registered table in registration order. A
     * handle naming one object searches only that one, which is what
     * RTLD_LOCAL scoping would have meant.
     */
    const int first = (idx < 0) ? 0 : idx;
    const int last  = (idx < 0) ? dl_object_count - 1 : idx;

    for (int i = first; i <= last && i < dl_object_count; i++) {
        const vx_dl_symbol_t *s = dl_objects[i].symbols;
        if (!s) continue;
        for (; s->name; s++) {
            if (dl_streq(s->name, name)) {
                dl_hit_count++;
                dl_error_text = NULL;
                return s->addr;
            }
        }
    }

    /*
     * Not found, and this is the ordinary outcome rather than a failure
     * of this file. A dispatcher asks about every entry point its
     * generated table knows and this system has a few of them.
     */
    dl_miss_count++;
    dl_miss_name  = name;
    dl_error_text = "this system does not have that symbol";
    return NULL;
}

char *dlerror(void) {
    /* Cleared by reading, as POSIX requires: two calls in a row give the
     * message and then null. */
    const char *e = dl_error_text;
    dl_error_text = NULL;
    return (char *)e;
}

unsigned long vx_dl_hits(void)   { return dl_hit_count; }
unsigned long vx_dl_misses(void) { return dl_miss_count; }
const char   *vx_dl_last_miss(void) { return dl_miss_name; }
