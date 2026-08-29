/*
 * third_party/wpe-port/pasteboard-noop.c — the clipboard, which is not
 * here.
 *
 * libwpe requires a pasteboard: wpe_pasteboard_get_singleton() calls
 * initialize() on whatever it finds and dereferences the result, so a
 * backend without one crashes the first time a page reads the clipboard
 * — which happens on any page with a text field, not only on a paste.
 *
 * Upstream ships exactly this file as `pasteboard-noop.cpp`, and the
 * only reason it is rewritten here rather than compiled is the
 * extension: it is C++, and there is no C++ runtime for this target.
 * Rewriting a file that does nothing is not a patch to libwpe, which is
 * why third_party/libwpe stays unmodified and this lives beside it.
 *
 * `generic_pasteboard_interface` is the symbol pasteboard.c falls back
 * to when the loader has no clipboard to offer, so defining it here is
 * what makes the fallback resolve.
 */

#include "../libwpe/src/pasteboard-private.h"

#include <stddef.h>

static void *noop_initialize(struct wpe_pasteboard *pasteboard) {
    (void)pasteboard;
    /* Non-null, because pasteboard.c stores this and passes it to every
     * call below without checking it. Nothing dereferences it. */
    return (void *)1;
}

static void noop_get_types(void *data,
                           struct wpe_pasteboard_string_vector *types) {
    (void)data;
    /*
     * An empty vector, and emptied rather than left alone.
     *
     * The caller allocates this on its stack and does not clear it, so a
     * function that returned without writing would leave WebKit reading
     * a length and a pointer that are whatever was on the stack. "There
     * are no types on the clipboard" has to be *said*.
     */
    if (!types) return;
    types->strings = NULL;
    types->length  = 0;
}

static void noop_get_string(void *data, const char *type,
                            struct wpe_pasteboard_string *out) {
    (void)data; (void)type;
    if (!out) return;
    out->data   = NULL;
    out->length = 0;
}

static void noop_write(void *data, struct wpe_pasteboard_string_map *map) {
    (void)data; (void)map;
    /* A copy that goes nowhere. The page believes it succeeded, which is
     * the same thing that happens on a real system when the clipboard
     * manager has gone away, and is better than an error a page would
     * surface to the person using it. */
}

struct wpe_pasteboard_interface generic_pasteboard_interface = {
    .initialize = noop_initialize,
    .get_types  = noop_get_types,
    .get_string = noop_get_string,
    .write      = noop_write,
};

struct wpe_pasteboard_interface noop_pasteboard_interface = {
    .initialize = noop_initialize,
    .get_types  = noop_get_types,
    .get_string = noop_get_string,
    .write      = noop_write,
};
