/*
 * third_party/wpe-port/vxwpe.c — libwpe, on Vextro.
 *
 * A WPE backend is the piece that tells WebKit where its pixels go and
 * where its input comes from. Upstream there are two of them:
 * wpebackend-fdo, which talks Wayland to a compositor, and the DRM one,
 * which talks to a kernel mode-setting driver. Both are several thousand
 * lines, and almost all of it is machinery for moving a GPU buffer
 * between two processes without copying it.
 *
 * None of that machinery has anything to attach to here, and the reason
 * is worth stating precisely because it is also why this file is short.
 * A Vextro application's window is not a handle to be negotiated for —
 * it is a buffer of thirty-two-bit pixels mapped into the process's own
 * address space, which SYS_CANVAS hands back the address of, and which
 * the compositor reads every frame. The shortest path from a rendered
 * page to the screen is therefore a memory copy. The dmabuf-and-socket
 * apparatus exists to accomplish, expensively, something this system
 * gives away.
 *
 * ---- what is implemented, and why all of it has to be ----
 *
 * libwpe asks a backend for four things and WebKit will not start
 * without any of them:
 *
 *   a view backend      the thing a WebKitWebView is created around
 *   a renderer host     which normally owns the socket to the compositor
 *   an EGL backend      which normally owns the display connection
 *   an EGL target       which normally owns the window surface
 *
 * Three of those four describe resources this system does not have. They
 * are implemented anyway, and truthfully rather than as stubs — the EGL
 * backend reports EGL_DEFAULT_DISPLAY, the target reports no native
 * window, the renderer host reports no descriptor. That is the accurate
 * description of a build with no EGL, and it is what an engine
 * configured for software rasterisation expects to find. A backend that
 * returned garbage from these would not fail here; it would fail
 * thousands of lines into WebKit's compositor with a message about a
 * surface.
 *
 * The fourth is real, and vxwpe_present() is where the pixels arrive.
 */

#include "vxwpe.h"

#include <wpe/wpe.h>
#include <wpe/wpe-egl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/syscall.h>

/* ===== 1. THE WINDOW ===== */

/*
 * Where this process's pixels are.
 *
 * SYS_CANVAS answers with an address in this address space and the two
 * dimensions. The address is mapped writable and never executable, and
 * it is *this process's own* surface rather than one shared with
 * everything else running — which has not always been true, and the note
 * in src/desktop.h about the surface pool is worth reading before
 * assuming a window is a window.
 */
static uint32_t     *win_pixels = 0;
static uint32_t      win_width  = 0;
static uint32_t      win_height = 0;
static vxwpe_fit_t   win_fit    = VXWPE_FIT_CLIP;
static vxwpe_stats_t stats;

bool vxwpe_init(void) {
    uint64_t info[3] = { 0, 0, 0 };
    long rc = __syscall1(SYS_CANVAS, (long)(uintptr_t)info);
    if (rc == -1 || !info[0]) return false;

    win_pixels = (uint32_t *)(uintptr_t)info[0];
    win_width  = (uint32_t)info[1];
    win_height = (uint32_t)info[2];
    memset(&stats, 0, sizeof(stats));
    return win_width && win_height;
}

void vxwpe_shutdown(void) {
    /* The canvas is the kernel's mapping, not ours; there is nothing to
     * release and nothing that would be correct to unmap. */
    win_pixels = 0;
    win_width = win_height = 0;
}

void vxwpe_window_size(uint32_t *w, uint32_t *h) {
    if (w) *w = win_width;
    if (h) *h = win_height;
}

void vxwpe_set_fit(vxwpe_fit_t fit) { win_fit = fit; }

void vxwpe_clear(uint32_t argb) {
    if (!win_pixels) return;
    uint32_t n = win_width * win_height;
    for (uint32_t i = 0; i < n; i++) win_pixels[i] = argb;
}

void vxwpe_stats(vxwpe_stats_t *out) { if (out) *out = stats; }

/* ===== 2. THE PIXEL PATH =====
 *
 * One row at a time, because that is the only granularity at which the
 * source and the destination agree. The source has a stride the
 * rasteriser chose — its rows are padded to whatever alignment its
 * vector code wanted — and the destination's rows are exactly the
 * window's width. A single memcpy of the whole buffer would be correct
 * only when those happen to match, and would shear the image by a few
 * pixels per row when they do not, which looks like a corrupt decode
 * rather than a stride bug.
 */

/*
 * A pixel, from whatever the engine produced into what the compositor
 * reads.
 *
 * Vextro's framebuffer is 0xAARRGGBB in a native-endian 32-bit word,
 * which on this processor means the byte order in memory is B, G, R, A.
 * That is the same layout Cairo and Skia call BGRA8888 on a
 * little-endian machine — so the common case is genuinely a copy, and
 * the conversion below exists for the other two.
 *
 * Written as a switch on a value that is constant for the whole frame,
 * which the compiler hoists out of the loop; a function pointer per
 * pixel would not be.
 */
static inline uint32_t convert(uint32_t px, vxwpe_format_t fmt) {
    switch (fmt) {
    case VXWPE_FORMAT_BGRA8888:
    case VXWPE_FORMAT_ARGB8888:
        /* Already what the framebuffer wants. The two names describe the
         * same bits from opposite ends -- byte order versus word
         * layout -- and conflating them is the oldest mistake in this
         * area, so both are listed rather than one being assumed. */
        return px;
    case VXWPE_FORMAT_RGBA8888: {
        /* 0xRRGGBBAA in a word -> 0xAARRGGBB. */
        return (px >> 8) | (px << 24);
    }
    default:
        return px;
    }
}

static uint32_t present_clip(const uint8_t *src, uint32_t sw, uint32_t sh,
                             uint32_t stride, vxwpe_format_t fmt) {
    uint32_t rows = sh < win_height ? sh : win_height;
    uint32_t cols = sw < win_width ? sw : win_width;
    uint32_t written = 0;

    for (uint32_t y = 0; y < rows; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * stride);
        uint32_t *d = win_pixels + (size_t)y * win_width;
        if (fmt == VXWPE_FORMAT_RGBA8888) {
            for (uint32_t x = 0; x < cols; x++) d[x] = convert(s[x], fmt);
        } else {
            /* The fast path, and the one that runs: no per-pixel work at
             * all, just a row copy the compiler turns into vector moves. */
            memcpy(d, s, (size_t)cols * 4);
        }
        written += cols;
    }
    return written;
}

/*
 * Nearest neighbour, with the source step held in fixed point.
 *
 * Not bilinear, and that is a decision rather than laziness. A browser
 * that has been told its viewport size renders at that size, so this
 * path only runs when something has gone wrong or when a page has
 * deliberately asked for a device scale factor — and in both cases a
 * sharp wrong-sized image is easier to diagnose than a smooth one.
 * Interpolating would also cost four loads and three blends per pixel on
 * a processor that is already the only one drawing.
 *
 * 16.16 rather than floating point because the accumulator is exact:
 * a step computed once and added repeatedly in fixed point cannot drift,
 * and a double can, which shows as the last column of the image being
 * sampled from one pixel past the end.
 */
static uint32_t present_scale(const uint8_t *src, uint32_t sw, uint32_t sh,
                              uint32_t stride, vxwpe_format_t fmt,
                              uint32_t dx0, uint32_t dy0,
                              uint32_t dw, uint32_t dh) {
    if (!sw || !sh || !dw || !dh) return 0;

    const uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / dw);
    const uint32_t ystep = (uint32_t)(((uint64_t)sh << 16) / dh);
    uint32_t written = 0;
    uint32_t sy = 0;

    for (uint32_t y = 0; y < dh; y++, sy += ystep) {
        uint32_t syi = sy >> 16;
        if (syi >= sh) syi = sh - 1;
        const uint32_t *s = (const uint32_t *)(src + (size_t)syi * stride);
        uint32_t *d = win_pixels + (size_t)(dy0 + y) * win_width + dx0;

        uint32_t sx = 0;
        for (uint32_t x = 0; x < dw; x++, sx += xstep) {
            uint32_t sxi = sx >> 16;
            if (sxi >= sw) sxi = sw - 1;
            d[x] = convert(s[sxi], fmt);
        }
        written += dw;
    }
    return written;
}

uint32_t vxwpe_present(const void *pixels, uint32_t width, uint32_t height,
                       uint32_t stride, vxwpe_format_t format) {
    if (!win_pixels || !pixels || !width || !height) {
        stats.dropped++;
        return 0;
    }
    /* A stride that cannot hold a row means the caller has given us the
     * wrong number, and copying against it would read past the end of
     * every row. Refused rather than clamped: clamping would produce an
     * image that looked nearly right. */
    if (stride < width * 4u) {
        stats.dropped++;
        return 0;
    }

    const uint8_t *src = (const uint8_t *)pixels;
    uint32_t written;

    switch (win_fit) {
    case VXWPE_FIT_SCALE:
        written = present_scale(src, width, height, stride, format,
                                0, 0, win_width, win_height);
        break;

    case VXWPE_FIT_LETTER: {
        /*
         * The largest rectangle with the source's aspect ratio that fits
         * in the window, centred, with the remainder cleared.
         *
         * Clearing first and every frame, rather than once: the bars
         * move when the source's shape changes, and a bar that was drawn
         * over last frame and is not this frame would keep showing the
         * old image.
         */
        uint64_t by_width  = (uint64_t)win_width * height;
        uint64_t by_height = (uint64_t)win_height * width;
        uint32_t dw, dh;
        if (by_width <= by_height) {
            dw = win_width;
            dh = (uint32_t)(by_width / width);
        } else {
            dh = win_height;
            dw = (uint32_t)(by_height / height);
        }
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        vxwpe_clear(0xFF000000u);
        written = present_scale(src, width, height, stride, format,
                                (win_width - dw) / 2, (win_height - dh) / 2,
                                dw, dh);
        break;
    }

    case VXWPE_FIT_CLIP:
    default:
        written = present_clip(src, width, height, stride, format);
        break;
    }

    stats.frames++;
    stats.pixels += written;
    return written;
}

/* ===== 3. THE VIEW BACKEND =====
 *
 * What a WebKitWebView is built around. libwpe calls create() when the
 * view is made and initialize() when it is ready to be told its size;
 * the size is what the engine lays out against, so sending it here
 * rather than waiting for a resize that will never come is what stops a
 * page rendering correctly at the wrong dimensions.
 */

struct vx_view {
    struct wpe_view_backend *backend;
    uint32_t width, height;

    /* The last state the pointer was seen in, so that a held button
     * produces one press rather than one per frame. */
    int32_t  last_x, last_y;
    uint32_t last_buttons;
    int      have_last;
};

/*
 * From a wpe_view_backend back to our own record.
 *
 * libwpe keeps what create() returned in `base.interface_data`, and
 * struct wpe_view_backend is opaque — the definition is inside
 * view-backend.c and there is no accessor. Reaching into it by guessing
 * the layout would work today and break on the first upstream release
 * that added a field, which is exactly the coupling vendoring an
 * unmodified tree is meant to avoid.
 *
 * So the mapping is kept here instead. Four entries because a browser
 * with tabs has one view per tab and this is a machine with a 598-pixel
 * window; a linear scan over four is not worth a data structure.
 */
#define VX_VIEWS_MAX 4
static struct { struct wpe_view_backend *backend; struct vx_view *view; }
       view_registry[VX_VIEWS_MAX];

static struct vx_view *view_lookup(struct wpe_view_backend *backend) {
    for (int i = 0; i < VX_VIEWS_MAX; i++)
        if (view_registry[i].backend == backend) return view_registry[i].view;
    return 0;
}

static void *view_create(void *data, struct wpe_view_backend *backend) {
    (void)data;
    struct vx_view *v = (struct vx_view *)calloc(1, sizeof(*v));
    if (!v) return 0;
    v->backend = backend;
    v->width   = win_width;
    v->height  = win_height;

    for (int i = 0; i < VX_VIEWS_MAX; i++) {
        if (view_registry[i].backend) continue;
        view_registry[i].backend = backend;
        view_registry[i].view    = v;
        return v;
    }
    /* More views than this backend can track. Refusing is better than
     * succeeding: a view that is not in the registry gets no input at
     * all, which is a much harder thing to explain than a failed
     * creation. */
    fprintf(stderr, "vxwpe: refusing a fifth view\n");
    free(v);
    return 0;
}

static void view_destroy(void *data) {
    struct vx_view *v = (struct vx_view *)data;
    if (!v) return;
    for (int i = 0; i < VX_VIEWS_MAX; i++)
        if (view_registry[i].view == v) {
            view_registry[i].backend = 0;
            view_registry[i].view    = 0;
        }
    free(v);
}

static void view_initialize(void *data) {
    struct vx_view *v = (struct vx_view *)data;
    if (!v) return;
    /*
     * The engine is told its viewport here and nowhere else.
     *
     * This is the one call in the whole interface that has to happen for
     * the page to be laid out at all — WebKit starts with a default
     * size, and a view that is never told otherwise renders against that
     * default and then paints it into a window of a different shape.
     */
    wpe_view_backend_dispatch_set_size(v->backend, v->width, v->height);
}

static int view_get_renderer_host_fd(void *data) {
    (void)data;
    /*
     * There is no descriptor, and -1 is the honest answer rather than a
     * failure.
     *
     * Upstream this is one end of a socket pair: the web process sends
     * buffer handles down it to the UI process's compositor. Both ends
     * of that arrangement are absent here — there is one process, and
     * its pixels are already in memory both halves can see — so there is
     * nothing for a descriptor to name. WebKit treats -1 as "this
     * backend does not use the renderer host", which is exactly true.
     */
    return -1;
}

static struct wpe_view_backend_interface vx_view_backend_interface = {
    .create               = view_create,
    .destroy              = view_destroy,
    .initialize           = view_initialize,
    .get_renderer_host_fd = view_get_renderer_host_fd,
};

struct wpe_view_backend *vxwpe_view_backend_create(void) {
    return wpe_view_backend_create_with_backend_interface(
        &vx_view_backend_interface, 0);
}

void vxwpe_set_viewport(struct wpe_view_backend *backend,
                        uint32_t width, uint32_t height) {
    if (!backend) return;
    wpe_view_backend_dispatch_set_size(backend, width, height);
}

/* ===== 4. THE RENDERER HOST =====
 *
 * Upstream this owns the socket the web process sends its rendered
 * buffers over, and create_client() hands back the other end. There is
 * one process here and its pixels are already where they need to be, so
 * there is no channel to create and -1 says so.
 */
/*
 * create() and destroy() are not optional, even though there is nothing
 * to create.
 *
 * libwpe calls `interface->create()` unconditionally and stores what it
 * returns; there is no null check, and a null pointer here is a call to
 * address zero — which on this system is an instruction fetch from an
 * unmapped page and a dead process. That is not a hypothetical: it is
 * what the first run of wpetest did, and the fault named an address of
 * zero with nothing to say which vtable it came from.
 *
 * So both are real functions that do nothing, and create() answers a
 * non-null token because the value is passed straight back into
 * create_client() below. It is never dereferenced.
 */
static void *host_create(void) { return (void *)1; }
static void  host_destroy(void *data) { (void)data; }

static int host_create_client(void *data) {
    (void)data;
    /*
     * There is no descriptor to hand back.
     *
     * Upstream this is one end of a socket pair, and the web process
     * sends rendered buffer handles down it to the UI process. Both ends
     * are absent here — one process, and its pixels are already in
     * memory that the compositor reads — so -1 is the accurate answer
     * and the one WebKit reads as "this backend does not use a renderer
     * host".
     */
    return -1;
}

static struct wpe_renderer_host_interface vx_renderer_host_interface = {
    .create        = host_create,
    .destroy       = host_destroy,
    .create_client = host_create_client,
};

/* ===== 5. THE EGL BACKEND AND TARGET =====
 *
 * Implemented, and reporting nothing — which is not the same as being a
 * stub, and the difference is the whole reason these are here.
 *
 * WebKit built for software rasterisation still constructs its renderer
 * through libwpe, because the construction path does not branch on
 * whether a GPU is present. It asks for a display, gets
 * EGL_DEFAULT_DISPLAY, asks for a window, gets none, and falls back to
 * its image-buffer path — which is the path that ends at
 * vxwpe_present(). An implementation that refused to be created, or
 * returned an invalid pointer, would take the engine down the GPU path
 * and fail somewhere unrecognisable.
 *
 * There is no GL driver a ring-3 program on this system can reach. The
 * kernel has one for the integrated graphics — src/igpu.h drives the
 * blitter the compositor uses — but it is the kernel's, and nothing
 * exports it across the system call boundary. That is on the list at the
 * end of this file.
 */

struct vx_egl_target {
    struct wpe_renderer_backend_egl_target *target;
    uint32_t width, height;
};

static void *egl_backend_create(int host_fd) { (void)host_fd; return (void *)1; }
static void  egl_backend_destroy(void *data) { (void)data; }

static EGLNativeDisplayType egl_get_native_display(void *data) {
    (void)data;
    /* EGL_DEFAULT_DISPLAY, which is zero, and which means "whatever
     * display this platform has" rather than "no display". A software
     * build never dereferences it. */
    return (EGLNativeDisplayType)0;
}

static uint32_t egl_get_platform(void *data) {
    (void)data;
    /* Zero is EGL_NONE: no platform extension applies. Naming a platform
     * this system does not have -- EGL_PLATFORM_GBM_KHR, say -- would
     * send an engine that *did* find an EGL library looking for a device
     * node. */
    return 0;
}

static struct wpe_renderer_backend_egl_interface vx_egl_backend_interface = {
    .create              = egl_backend_create,
    .destroy             = egl_backend_destroy,
    .get_native_display  = egl_get_native_display,
    .get_platform        = egl_get_platform,
};

static void *egl_target_create(struct wpe_renderer_backend_egl_target *target,
                               int host_fd) {
    (void)host_fd;
    struct vx_egl_target *t = (struct vx_egl_target *)calloc(1, sizeof(*t));
    if (!t) return 0;
    t->target = target;
    return t;
}

static void egl_target_destroy(void *data) { free(data); }

static void egl_target_initialize(void *data, void *backend,
                                  uint32_t width, uint32_t height) {
    (void)backend;
    struct vx_egl_target *t = (struct vx_egl_target *)data;
    if (!t) return;
    t->width = width;
    t->height = height;
}

static EGLNativeWindowType egl_target_get_native_window(void *data) {
    (void)data;
    return (EGLNativeWindowType)0;
}

static void egl_target_resize(void *data, uint32_t width, uint32_t height) {
    struct vx_egl_target *t = (struct vx_egl_target *)data;
    if (!t) return;
    t->width = width;
    t->height = height;
}

static void egl_target_frame_will_render(void *data) { (void)data; }

static void egl_target_frame_rendered(void *data) {
    struct vx_egl_target *t = (struct vx_egl_target *)data;
    if (!t || !t->target) return;
    /*
     * The engine has finished a frame and is waiting to be released.
     *
     * This acknowledgement is not optional and is the single easiest
     * thing to leave out of a backend: WebKit will not begin the next
     * frame until it arrives, so a backend that never calls this renders
     * exactly one frame and then appears to hang. It is called here
     * rather than after the copy because the copy is synchronous — by
     * the time the engine reaches this point the pixels are already in
     * the window.
     */
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->target);
}

static void egl_target_deinitialize(void *data) { (void)data; }

static struct wpe_renderer_backend_egl_target_interface
vx_egl_target_interface = {
    .create            = egl_target_create,
    .destroy           = egl_target_destroy,
    .initialize        = egl_target_initialize,
    .get_native_window = egl_target_get_native_window,
    .resize            = egl_target_resize,
    .frame_will_render = egl_target_frame_will_render,
    .frame_rendered    = egl_target_frame_rendered,
    .deinitialize      = egl_target_deinitialize,
};

static void *egl_offscreen_create(void) { return (void *)1; }
static void  egl_offscreen_destroy(void *data) { (void)data; }
static void  egl_offscreen_initialize(void *data, void *backend) {
    (void)data; (void)backend;
}
static EGLNativeWindowType egl_offscreen_get_native_window(void *data) {
    (void)data;
    return (EGLNativeWindowType)0;
}

static struct wpe_renderer_backend_egl_offscreen_target_interface
vx_egl_offscreen_interface = {
    .create            = egl_offscreen_create,
    .destroy           = egl_offscreen_destroy,
    .initialize        = egl_offscreen_initialize,
    .get_native_window = egl_offscreen_get_native_window,
};

/* ===== 6. THE LOADER =====
 *
 * libwpe normally finds a backend by dlopen-ing a shared library named
 * at run time and looking up this symbol in it. There is no dynamic
 * loader in this system and no shared libraries, so libwpe is compiled
 * with src/loader-static.c instead — which does no loading at all and
 * expects `_wpe_loader_interface` to be *linked in*.
 *
 * That is the whole of the change needed to make libwpe work without
 * dlopen, and it is upstream's own answer rather than a patch: the file
 * exists precisely for static builds. Nothing in third_party/libwpe is
 * modified.
 */
static void *vx_load_object(const char *name) {
    if (!name) return 0;

    if (!strcmp(name, "_wpe_view_backend_interface"))
        return &vx_view_backend_interface;
    if (!strcmp(name, "_wpe_renderer_host_interface"))
        return &vx_renderer_host_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_interface"))
        return &vx_egl_backend_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_target_interface"))
        return &vx_egl_target_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_offscreen_target_interface"))
        return &vx_egl_offscreen_interface;

    /*
     * Anything else is a request for something this backend does not
     * provide -- a gamepad provider, an audio sink -- and null is the
     * documented answer. Said on the wire rather than silently, because
     * a null returned to WebKit becomes a feature that is quietly
     * missing rather than an error, and knowing which one is the whole
     * difference when a page does not behave.
     */
    fprintf(stderr, "vxwpe: no such backend object: %s\n", name);
    return 0;
}

__attribute__((visibility("default")))
struct wpe_loader_interface _wpe_loader_interface = {
    .load_object = vx_load_object,
};

/* ===== 7. INPUT =====
 *
 * Vextro reports the mouse as a state — where it is and which buttons
 * are down — and WebKit wants events: this button went down, the pointer
 * moved from there to here. The difference between a state and an event
 * is memory, which is what `last_*` in struct vx_view is for.
 *
 * Getting that wrong is not subtle in its effects but is very easy to do:
 * a backend that dispatches a button event whenever the button is down
 * sends one per frame, and a page sees sixty clicks a second.
 */

/* Vextro's button mask, from SYS_GET_MOUSE. */
#define VX_MOUSE_LEFT   1u
#define VX_MOUSE_RIGHT  2u
#define VX_MOUSE_MIDDLE 4u

/* WPE numbers its buttons from one, in the X11 order. */
#define WPE_BUTTON_LEFT   1u
#define WPE_BUTTON_MIDDLE 2u
#define WPE_BUTTON_RIGHT  3u

static uint32_t now_ms(void) {
    return (uint32_t)__syscall0(SYS_TICKS);
}

/* Where the window's top-left corner is on screen.
 *
 * SYS_GET_MOUSE answers in screen coordinates and the engine wants them
 * relative to its viewport, so the difference has to come from
 * somewhere. The compositor knows it and does not report it, so the
 * value is set by the application, which is told where its window is
 * when it is placed. Zero until it is, which puts the pointer in the
 * right place only for a full-screen window -- stated rather than
 * hidden, because a pointer that is consistently offset by the title bar
 * height is exactly the symptom. */
static int32_t win_origin_x = 0;
static int32_t win_origin_y = 0;

void vxwpe_set_window_origin(int32_t x, int32_t y) {
    win_origin_x = x;
    win_origin_y = y;
}

static void dispatch_pointer(struct wpe_view_backend *backend,
                             enum wpe_input_pointer_event_type type,
                             int x, int y, uint32_t button, uint32_t state,
                             uint32_t modifiers) {
    struct wpe_input_pointer_event ev = {
        .type      = type,
        .time      = now_ms(),
        .x         = x,
        .y         = y,
        .button    = button,
        .state     = state,
        .modifiers = modifiers,
    };
    wpe_view_backend_dispatch_pointer_event(backend, &ev);
    stats.input_events++;
}

void vxwpe_pump(struct wpe_view_backend *backend) {
    if (!backend) return;

    struct vx_view *v = view_lookup(backend);
    if (!v) return;

    int32_t m[4] = { 0, 0, 0, 0 };
    __syscall1(SYS_GET_MOUSE, (long)(uintptr_t)m);

    int32_t x = m[0] - win_origin_x;
    int32_t y = m[1] - win_origin_y;
    uint32_t buttons = (uint32_t)m[2];

    /*
     * Modifiers, as WPE expects them on a pointer event: which buttons
     * are held *now*. Not the keyboard modifiers, which are a separate
     * set of bits in the same field and are tracked by the keyboard
     * path -- a page that reads event.buttons is reading these.
     */
    uint32_t mods = 0;
    if (buttons & VX_MOUSE_LEFT)   mods |= wpe_input_pointer_modifier_button1;
    if (buttons & VX_MOUSE_MIDDLE) mods |= wpe_input_pointer_modifier_button2;
    if (buttons & VX_MOUSE_RIGHT)  mods |= wpe_input_pointer_modifier_button3;

    if (!v->have_last) {
        v->last_x = x;
        v->last_y = y;
        v->last_buttons = buttons;
        v->have_last = 1;
        /* One motion event to place the pointer, so that a page which
         * paints a hover state does not wait for the first movement. */
        dispatch_pointer(backend, wpe_input_pointer_event_type_motion,
                         x, y, 0, 0, mods);
        return;
    }

    if (x != v->last_x || y != v->last_y) {
        dispatch_pointer(backend, wpe_input_pointer_event_type_motion,
                         x, y, 0, 0, mods);
        v->last_x = x;
        v->last_y = y;
    }

    /*
     * The edges, one button at a time.
     *
     * Comparing the whole mask and dispatching once would lose the case
     * where one button goes down in the same frame another comes up --
     * which a person operating a mouse does more often than it sounds,
     * and which produces a stuck button when it is missed.
     */
    uint32_t changed = buttons ^ v->last_buttons;
    if (changed) {
        static const struct { uint32_t vx, wpe; } map[] = {
            { VX_MOUSE_LEFT,   WPE_BUTTON_LEFT   },
            { VX_MOUSE_MIDDLE, WPE_BUTTON_MIDDLE },
            { VX_MOUSE_RIGHT,  WPE_BUTTON_RIGHT  },
        };
        for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (!(changed & map[i].vx)) continue;
            dispatch_pointer(backend, wpe_input_pointer_event_type_button,
                             x, y, map[i].wpe,
                             (buttons & map[i].vx) ? 1 : 0, mods);
        }
        v->last_buttons = buttons;
    }
}

void vxwpe_key(struct wpe_view_backend *backend, uint32_t keysym,
               uint32_t hardware_code, bool pressed, uint32_t modifiers) {
    if (!backend) return;
    struct wpe_input_keyboard_event ev = {
        .time              = now_ms(),
        .key_code          = keysym,
        .hardware_key_code = hardware_code,
        .pressed           = pressed,
        .modifiers         = modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event(backend, &ev);
    stats.input_events++;
}

void vxwpe_scroll(struct wpe_view_backend *backend, int32_t dx, int32_t dy) {
    if (!backend) return;
    struct vx_view *v = view_lookup(backend);
    int x = v ? v->last_x : 0;
    int y = v ? v->last_y : 0;

    /*
     * Two events, one per axis, because that is what the non-smooth
     * interface carries: `axis` names which one and `value` how far.
     * Sending only the vertical one -- which is all a wheel usually
     * produces -- would silently drop horizontal scrolling on a page
     * that has it.
     *
     * Axis 0 is vertical and axis 1 is horizontal, which is the reverse
     * of what the names suggest and is what WebKit reads.
     */
    if (dy) {
        struct wpe_input_axis_event ev = {
            .type = wpe_input_axis_event_type_motion,
            .time = now_ms(), .x = x, .y = y,
            .axis = 0, .value = dy, .modifiers = 0,
        };
        wpe_view_backend_dispatch_axis_event(backend, &ev);
        stats.input_events++;
    }
    if (dx) {
        struct wpe_input_axis_event ev = {
            .type = wpe_input_axis_event_type_motion,
            .time = now_ms(), .x = x, .y = y,
            .axis = 1, .value = dx, .modifiers = 0,
        };
        wpe_view_backend_dispatch_axis_event(backend, &ev);
        stats.input_events++;
    }
}
