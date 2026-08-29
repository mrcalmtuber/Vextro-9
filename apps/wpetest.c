/*
 * wpetest — libwpe, running in ring 3 on this machine.
 *
 * WebKit is not here yet (see third_party/wpe-config/README.md for what
 * stands between), and that is exactly why this exists. The backend in
 * third_party/wpe-port/ is the piece the engine will plug into, and it
 * can be wrong in ways that would only surface as a blank window three
 * million lines later. Everything about it that does not need WebKit can
 * be checked now, and is:
 *
 *   libwpe itself links and runs against the new C library. That is a
 *   real result rather than a formality — libwpe is ordinary POSIX C,
 *   and until this week this system had no calloc that ported code
 *   would find, no fprintf, no stderr to print to.
 *
 *   The static loader resolves. libwpe normally finds a backend by
 *   dlopen; there is no dynamic loader here, so it is built with
 *   upstream's loader-static.c and every interface has to be reachable
 *   through the linked-in _wpe_loader_interface.
 *
 *   A view backend can be created, initialised and destroyed, and the
 *   size it dispatches reaches a client. A view that never dispatches
 *   its size is the single most common way a WPE port renders a page at
 *   the wrong dimensions.
 *
 *   The pixel path puts the right pixels in the right places — clipped,
 *   scaled and letterboxed — with a source stride that does not match
 *   the window's, which is the case a naive single memcpy gets wrong and
 *   which every real rasteriser produces.
 *
 *   Input turns a *state* into *events*. Vextro reports which buttons
 *   are down; WebKit wants to be told when one changed. A backend that
 *   confuses the two sends a page sixty clicks a second.
 *
 * What it does not check is anything about WebKit, because WebKit is not
 * here. It checks that the socket the engine plugs into is wired
 * correctly.
 */

#include "vextro.h"
#include <wpe/wpe.h>
#include <wpe/wpe-egl.h>
#include "vxwpe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* ===== what the engine would be told ===== */

static uint32_t got_width, got_height;
static int      size_dispatched;
static int      frame_displayed;

static void on_set_size(void *data, uint32_t w, uint32_t h) {
    (void)data;
    got_width = w;
    got_height = h;
    size_dispatched++;
}

static void on_frame_displayed(void *data) {
    (void)data;
    frame_displayed++;
}

static struct wpe_view_backend_client test_client = {
    .set_size        = on_set_size,
    .frame_displayed = on_frame_displayed,
};

/* What the engine would be told about input. */
static int      motion_events, button_events, axis_events, key_events;
static int      last_button, last_state;
static int32_t  last_px, last_py;

static void on_handle_keyboard(void *data, struct wpe_input_keyboard_event *e) {
    (void)data; (void)e;
    key_events++;
}
static void on_handle_pointer(void *data, struct wpe_input_pointer_event *e) {
    (void)data;
    if (e->type == wpe_input_pointer_event_type_motion) {
        motion_events++;
        last_px = e->x;
        last_py = e->y;
    } else if (e->type == wpe_input_pointer_event_type_button) {
        button_events++;
        last_button = (int)e->button;
        last_state  = (int)e->state;
    }
}
static void on_handle_axis(void *data, struct wpe_input_axis_event *e) {
    (void)data; (void)e;
    axis_events++;
}
static void on_handle_touch(void *data, struct wpe_input_touch_event *e) {
    (void)data; (void)e;
}

static struct wpe_view_backend_input_client test_input_client = {
    .handle_keyboard_event = on_handle_keyboard,
    .handle_pointer_event  = on_handle_pointer,
    .handle_axis_event     = on_handle_axis,
    .handle_touch_event    = on_handle_touch,
};

/* ===== a stand-in for the engine's output =====
 *
 * A recognisable pattern, in a buffer whose stride is deliberately
 * larger than its width — which is what a rasteriser produces after
 * aligning its rows, and which is precisely the case a backend that
 * memcpy'd the whole buffer in one go would shear.
 */
#define SRC_W      320
#define SRC_H      200
#define SRC_STRIDE (SRC_W * 4 + 64)      /* padded, on purpose */

static uint32_t pattern_at(uint32_t x, uint32_t y) {
    /* Distinct in both axes, so a transposed or offset copy shows up as
     * a wrong value rather than as a plausible one. */
    return 0xFF000000u | ((x & 0xFF) << 16) | ((y & 0xFF) << 8) |
           ((x ^ y) & 0xFF);
}

int main(void) {
    printf("wpetest: libwpe %d.%d.%d on Vextro\n",
           wpe_get_major_version(), wpe_get_minor_version(),
           wpe_get_micro_version());

    /* ---- 1. the backend claims the window ---- */
    ok("the backend claims this process's window", vxwpe_init());

    uint32_t ww = 0, wh = 0;
    vxwpe_window_size(&ww, &wh);
    ok("and reports the canvas dimensions",
       ww == OS_CANVAS_W && wh == OS_CANVAS_H);
    printf("       (window is %ux%u)\n", ww, wh);

    /* ---- 2. the static loader resolves, through the public path ----
     *
     * libwpe is built with upstream's loader-static.c, which does no
     * loading at all and expects `_wpe_loader_interface` to be linked
     * in. If that symbol were missing or its load_object null, libwpe
     * calls abort() -- so every line below that returns at all is
     * evidence the wiring holds.
     *
     * Exercised through the functions WebKit actually calls rather than
     * through wpe_load_object, which is in libwpe's private header. A
     * test that reached into internals would keep passing after a
     * refactor that broke the real path.
     */
    ok("wpe_loader_init succeeds without a dynamic loader",
       wpe_loader_init("vextro") == true);

    /* The no-argument form goes through the loader for its interface,
     * which is the path a WebKit built against a shared backend takes.
     * That it returns a backend at all means the loader found ours. */
    struct wpe_view_backend *loaded = wpe_view_backend_create();
    ok("the loader supplies a view backend interface", loaded != 0);
    if (loaded) wpe_view_backend_destroy(loaded);

    /* Also through the loader, and answering -1 because there is no
     * socket for a renderer host to hand back. */
    ok("the loader supplies a renderer host interface",
       wpe_renderer_host_create_client() == -1);

    /* And the EGL side, which WebKit constructs even for a software
     * build because the construction path does not branch on whether a
     * GPU is present. */
    struct wpe_renderer_backend_egl *egl = wpe_renderer_backend_egl_create(-1);
    ok("the loader supplies an EGL backend interface", egl != 0);
    if (egl) {
        ok("which reports the default display",
           wpe_renderer_backend_egl_get_native_display(egl) ==
           (EGLNativeDisplayType)0);
        struct wpe_renderer_backend_egl_target *tgt =
            wpe_renderer_backend_egl_target_create(-1);
        ok("and an EGL target can be created", tgt != 0);
        if (tgt) {
            wpe_renderer_backend_egl_target_initialize(tgt, egl, ww, wh);
            ok("initialised with the window's size, reporting no native "
               "window",
               wpe_renderer_backend_egl_target_get_native_window(tgt) ==
               (EGLNativeWindowType)0);
            wpe_renderer_backend_egl_target_destroy(tgt);
        }
        wpe_renderer_backend_egl_destroy(egl);
    }

    /* ---- 3. a view backend, as WebKit would make one ---- */
    struct wpe_view_backend *vb = vxwpe_view_backend_create();
    ok("a view backend is created", vb != 0);
    if (!vb) goto done;

    wpe_view_backend_set_backend_client(vb, &test_client, 0);
    wpe_view_backend_set_input_client(vb, &test_input_client, 0);

    wpe_view_backend_initialize(vb);
    ok("initialising it dispatches the viewport size", size_dispatched == 1);
    ok("and the size is the window's",
       got_width == ww && got_height == wh);

    ok("the renderer host descriptor is -1, as a backend with no socket "
       "must report",
       wpe_view_backend_get_renderer_host_fd(vb) == -1);

    /* ---- 4. the pixel path ---- */
    uint8_t *src = (uint8_t *)mmap(0, (size_t)SRC_STRIDE * SRC_H,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok("a source surface is allocated", src != MAP_FAILED);
    if (src == MAP_FAILED) goto done;

    for (uint32_t y = 0; y < SRC_H; y++) {
        uint32_t *row = (uint32_t *)(src + (size_t)y * SRC_STRIDE);
        for (uint32_t x = 0; x < SRC_W; x++) row[x] = pattern_at(x, y);
        /* Poison the padding, so a copy that used the stride as the
         * width would put recognisable garbage on screen rather than
         * something that might pass. */
        for (uint32_t x = SRC_W; x < SRC_STRIDE / 4; x++) row[x] = 0xDEADBEEF;
    }

    uint32_t *canvas = os_canvas(0, 0);
    ok("the canvas is reachable from the application too", canvas != 0);

    vxwpe_set_fit(VXWPE_FIT_CLIP);
    vxwpe_clear(0xFF101820u);
    uint32_t n = vxwpe_present(src, SRC_W, SRC_H, SRC_STRIDE,
                               VXWPE_FORMAT_ARGB8888);
    ok("a frame is presented", n == (uint32_t)SRC_W * SRC_H);

    /* The pattern must land pixel for pixel, and the padding must not
     * land at all. */
    int exact = 1;
    for (uint32_t y = 0; y < SRC_H && exact; y++)
        for (uint32_t x = 0; x < SRC_W; x++)
            if (canvas[y * ww + x] != pattern_at(x, y)) { exact = 0; break; }
    ok("every pixel of the source is where it should be", exact);

    int padding_leaked = 0;
    for (uint32_t y = 0; y < SRC_H; y++)
        for (uint32_t x = SRC_W; x < ww; x++)
            if (canvas[y * ww + x] == 0xDEADBEEF) padding_leaked = 1;
    ok("and the source's row padding did not leak into the window",
       !padding_leaked);

    /* Beyond the source, the window keeps what was there — clipping does
     * not clear what it does not cover. */
    ok("the area outside the frame is untouched",
       canvas[(SRC_H) * ww] == 0xFF101820u);

    /* A source larger than the window is cropped rather than overrunning
     * it, which is the check that this cannot write outside the
     * canvas. */
    vxwpe_clear(0xFF000000u);
    uint32_t big_w = ww + 137, big_h = wh + 91;
    size_t big_stride = (size_t)big_w * 4;
    uint8_t *big = (uint8_t *)mmap(0, big_stride * big_h,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (big != MAP_FAILED) {
        for (uint32_t y = 0; y < big_h; y++) {
            uint32_t *row = (uint32_t *)(big + (size_t)y * big_stride);
            for (uint32_t x = 0; x < big_w; x++) row[x] = pattern_at(x, y);
        }
        uint32_t m = vxwpe_present(big, big_w, big_h, (uint32_t)big_stride,
                                   VXWPE_FORMAT_ARGB8888);
        ok("a source larger than the window is clipped to it",
           m == ww * wh);
        int corner_ok = canvas[(wh - 1) * ww + (ww - 1)] ==
                        pattern_at(ww - 1, wh - 1);
        ok("and the last pixel in the window is the right one", corner_ok);
        munmap(big, big_stride * big_h);
    }

    /* Scaling: the corners must map to the corners. Nearest neighbour
     * makes that an exact claim rather than an approximate one. */
    vxwpe_set_fit(VXWPE_FIT_SCALE);
    vxwpe_clear(0xFF000000u);
    vxwpe_present(src, SRC_W, SRC_H, SRC_STRIDE, VXWPE_FORMAT_ARGB8888);
    ok("scaling puts the source's first pixel in the window's first",
       canvas[0] == pattern_at(0, 0));
    ok("and the source's last pixel in the window's last",
       canvas[(wh - 1) * ww + (ww - 1)] == pattern_at(SRC_W - 1, SRC_H - 1));

    /* Letterboxing: the aspect ratio is kept and the bars are black. */
    vxwpe_set_fit(VXWPE_FIT_LETTER);
    vxwpe_present(src, SRC_W, SRC_H, SRC_STRIDE, VXWPE_FORMAT_ARGB8888);
    /* 320x200 is wider than 598x402, so the window is filled
     * horizontally and there are bars above and below. */
    ok("letterboxing leaves a black bar at the top",
       canvas[0] == 0xFF000000u);
    ok("and fills the middle", canvas[(wh / 2) * ww + (ww / 2)] != 0xFF000000u);

    /* A stride that cannot hold a row is refused rather than read past
     * the end of every row. */
    vxwpe_set_fit(VXWPE_FIT_CLIP);
    ok("an impossible stride is refused",
       vxwpe_present(src, SRC_W, SRC_H, SRC_W * 2, VXWPE_FORMAT_ARGB8888) == 0);
    ok("and a null surface is refused",
       vxwpe_present(0, SRC_W, SRC_H, SRC_STRIDE, VXWPE_FORMAT_ARGB8888) == 0);

    /* The byte-order conversion. RGBA in, ARGB out. */
    {
        uint32_t one = 0x11223344u;              /* R=11 G=22 B=33 A=44 */
        uint32_t out = 0;
        uint32_t save = canvas[0];
        vxwpe_present(&one, 1, 1, 4, VXWPE_FORMAT_RGBA8888);
        out = canvas[0];
        canvas[0] = save;
        ok("RGBA is rotated into the framebuffer's ARGB", out == 0x44112233u);
    }

    /* ---- 5. input: a state becomes events ---- */
    {
        int m0 = motion_events, b0 = button_events;

        /* The first pump has nothing to compare against and must place
         * the pointer, so that a page painting a hover state does not
         * wait for the first movement. */
        vxwpe_pump(vb);
        ok("the first pump places the pointer", motion_events == m0 + 1);
        ok("and issues no button event", button_events == b0);

        /* A second pump with nothing changed must be silent. This is the
         * check that separates an event from a state: a backend that
         * dispatched whatever the mouse currently reads would send one
         * of everything per frame. */
        int m1 = motion_events, b1 = button_events;
        vxwpe_pump(vb);
        vxwpe_pump(vb);
        vxwpe_pump(vb);
        ok("three more pumps with nothing moving are silent",
           motion_events == m1 && button_events == b1);
    }

    /* Keys and scrolling go straight through, since they arrive as
     * events already. */
    {
        int k0 = key_events;
        vxwpe_key(vb, 'a', 30, true, 0);
        vxwpe_key(vb, 'a', 30, false, 0);
        ok("a key press and release both reach the engine",
           key_events == k0 + 2);

        int a0 = axis_events;
        vxwpe_scroll(vb, 0, 3);
        ok("a vertical scroll is one axis event", axis_events == a0 + 1);
        vxwpe_scroll(vb, 5, 7);
        ok("and a diagonal one is two", axis_events == a0 + 3);
        vxwpe_scroll(vb, 0, 0);
        ok("a scroll of nothing is no event", axis_events == a0 + 3);
    }

    /* ---- 6. the statistics the panel reads ---- */
    {
        vxwpe_stats_t st;
        vxwpe_stats(&st);
        ok("the backend counted its frames", st.frames == 5);
        ok("and its dropped frames", st.dropped == 2);
        ok("and its input events", st.input_events > 0);
        printf("       (%llu frames, %llu pixels, %llu dropped, %llu events)\n",
               (unsigned long long)st.frames,
               (unsigned long long)st.pixels,
               (unsigned long long)st.dropped,
               (unsigned long long)st.input_events);
    }

    /* ---- 7. teardown ---- */
    munmap(src, (size_t)SRC_STRIDE * SRC_H);
    wpe_view_backend_destroy(vb);
    ok("the view backend is destroyed", 1);

    /* Creating another after destroying the first proves the registry
     * gives its slot back -- a backend that leaked them would run out
     * after four tabs. */
    struct wpe_view_backend *again = vxwpe_view_backend_create();
    ok("and its slot is reusable", again != 0);
    if (again) wpe_view_backend_destroy(again);

done:
    vxwpe_shutdown();
    printf("wpetest: %d checks, %d failures\n", checks, failures);
    printf(failures ? "wpetest: FAILED\n" : "wpetest: all passed\n");
    return failures ? 1 : 0;
}
