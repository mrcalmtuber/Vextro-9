#ifndef VXWPE_H
#define VXWPE_H

/*
 * third_party/wpe-port/vxwpe.h — the seam between WPE WebKit and this
 * machine's window.
 *
 * WPE is WebKit with the platform taken out. Everything a browser engine
 * normally assumes — a window system, a compositor, an input stack, a
 * GPU — is behind one small C library, libwpe, and a *backend* supplies
 * it. On a Linux set-top box that backend talks to DRM and dmabuf; under
 * a desktop it talks to Wayland. This file is the backend for a system
 * that has none of those.
 *
 * ---- what the engine expects, and what is actually here ----
 *
 * The normal arrangement is that WebKit renders into an OpenGL texture,
 * exports it as a dmabuf file descriptor, and passes that descriptor
 * across a socket to a compositor that puts it on screen. Three of those
 * four things do not exist in Vextro: there is no GL driver a program can
 * reach from ring 3, there are no file descriptors in user space at all,
 * and there is no socket to pass one over.
 *
 * What there is, is better suited to the job than it sounds. A Vextro
 * application's window is a buffer of 32-bit pixels mapped directly into
 * its own address space — SYS_CANVAS hands back the address — and the
 * compositor reads those pixels every frame. So the shortest path from a
 * rendered page to the screen is a memory copy, and the whole of the
 * dmabuf-and-socket apparatus exists to accomplish something this
 * machine gets for free.
 *
 * The backend therefore does two things:
 *
 *   It implements libwpe's interfaces truthfully, so that WebKit can
 *   create a view, a renderer host and an EGL target, and so that every
 *   call it makes during setup is answered. An engine whose backend
 *   returns null from get_native_display does not fail there; it fails
 *   several thousand lines later.
 *
 *   It takes the finished pixels and puts them in the window, with the
 *   format conversion, the clipping and the scaling that the engine's
 *   idea of a surface and this machine's idea of a window differ by.
 *
 * ---- the software path ----
 *
 * vxwpe_present() below is the second half, and it is deliberately not
 * an EGL entry point. WebKit built without a GPU rasterises into an
 * ordinary image buffer, and that buffer is what arrives here. The EGL
 * interfaces are still implemented because libwpe requires them to
 * exist, and they report a display of EGL_DEFAULT_DISPLAY and a target
 * with no native window — which is the correct description of a build
 * that has no EGL, rather than a stub.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wpe_view_backend;

/* The pixel format the engine hands over. Both are 32 bits and differ
 * only in byte order; which one arrives depends on how WebKit's image
 * buffers were configured, so the caller states it rather than the
 * backend guessing. */
typedef enum {
    VXWPE_FORMAT_ARGB8888 = 0,   /* 0xAARRGGBB, native to this system   */
    VXWPE_FORMAT_BGRA8888 = 1,   /* what Cairo and Skia produce on x86  */
    VXWPE_FORMAT_RGBA8888 = 2
} vxwpe_format_t;

/* How a surface larger or smaller than the window is fitted to it. */
typedef enum {
    VXWPE_FIT_CLIP    = 0,   /* one pixel to one pixel, cropped         */
    VXWPE_FIT_SCALE   = 1,   /* nearest-neighbour, stretched to fill    */
    VXWPE_FIT_LETTER  = 2    /* scaled, aspect preserved, bars either side */
} vxwpe_fit_t;

/*
 * Start the backend.
 *
 * Claims the calling process's window through SYS_CANVAS and returns
 * false if it has none — which happens when more than six applications
 * are running, because the surface pool is that size and the seventh
 * falls back to a shared canvas the compositor does not treat as its
 * own. A browser drawing into that would be drawing over another
 * program's window.
 */
bool vxwpe_init(void);
void vxwpe_shutdown(void);

/* The window's dimensions, which are fixed by the system rather than
 * chosen by the program: see OS_CANVAS_W in apps/vextro.h. */
void vxwpe_window_size(uint32_t *width, uint32_t *height);

/* How a surface that does not match those dimensions is fitted.
 * Clipping by default, because a browser told the viewport size will
 * render at it and any scaling then is a bug being papered over. */
void vxwpe_set_fit(vxwpe_fit_t fit);

/*
 * The view backend, for WebKit.
 *
 * Handed to webkit_web_view_backend_new(). One per view; the engine owns
 * it after this returns and destroys it through libwpe.
 */
struct wpe_view_backend *vxwpe_view_backend_create(void);

/*
 * ---- the pixel path ----
 *
 * Copy a rendered frame into the window.
 *
 * `stride` is in bytes and may exceed width*4: a rasteriser aligns its
 * rows, and assuming otherwise shears the image by a few pixels per row
 * in a way that looks like a corrupted decode rather than a stride bug.
 *
 * Returns the number of pixels actually written, which is less than the
 * window when the surface is smaller and clipping is in force.
 */
uint32_t vxwpe_present(const void *pixels, uint32_t width, uint32_t height,
                       uint32_t stride, vxwpe_format_t format);

/* Fill the window with one colour. Used before the first frame arrives,
 * so that a view which has not painted yet shows a background rather
 * than whatever the last program left in the surface. */
void vxwpe_clear(uint32_t argb);

/*
 * ---- input ----
 *
 * Vextro reports the mouse through SYS_GET_MOUSE in screen coordinates
 * and the keyboard through the compositor. Neither is in the form
 * WebKit wants, and the translation is not only a matter of naming:
 * pointer coordinates have to be made relative to the window, button
 * state has to be turned into press and release *events* by comparing
 * against the last poll, and a key has to be given both a hardware code
 * and a keysym.
 *
 * Call vxwpe_pump() once per frame. It reads the current state, works
 * out what changed, and dispatches the events that follow — which is
 * where the edge detection lives, so that a held button produces one
 * press and not one per frame.
 */
void vxwpe_pump(struct wpe_view_backend *backend);

/* Feed a key that arrived from somewhere other than the poll — the
 * address bar, a menu, a synthesised accelerator. `pressed` is false for
 * a release, and both must be delivered or the engine believes the key
 * is still down. */
void vxwpe_key(struct wpe_view_backend *backend, uint32_t keysym,
               uint32_t hardware_code, bool pressed, uint32_t modifiers);

/* Scroll, in the engine's units: a positive `dy` scrolls the page down.
 * Separate from the pump because Vextro's mouse report has no wheel
 * field — a wheel event arrives from the window manager instead. */
void vxwpe_scroll(struct wpe_view_backend *backend, int32_t dx, int32_t dy);

/*
 * Tell the engine how large its viewport is.
 *
 * Sent once at startup and again whenever the window changes, which on
 * this system is never — the canvas is a fixed size. It is here because
 * an engine that was never told its size lays out against a default that
 * is not this window, and the result is a page that renders correctly at
 * the wrong dimensions.
 */
void vxwpe_set_viewport(struct wpe_view_backend *backend,
                        uint32_t width, uint32_t height);

/* What the backend has done so far, for the diagnostics panel: frames
 * presented, pixels copied, and how many frames were dropped because one
 * was still being copied. */
typedef struct {
    uint64_t frames;
    uint64_t pixels;
    uint64_t dropped;
    uint64_t input_events;
} vxwpe_stats_t;

void vxwpe_stats(vxwpe_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VXWPE_H */
