# wpe-port

The Vextro backend for [libwpe](https://github.com/WebPlatformForEmbedded/libwpe),
beside `../libwpe/`, which is vendored unmodified.

## What a WPE backend is

WPE is WebKit with the platform removed. Everything an engine normally
assumes — a window system, an input stack, a compositor, a GPU — sits
behind one small C library, and a *backend* supplies it. Upstream ships
two: `wpebackend-fdo`, which speaks Wayland, and a DRM one, which speaks
to a kernel mode-setting driver. Both are several thousand lines, and
nearly all of it is machinery for handing a GPU buffer between two
processes without copying it.

None of that machinery has anything to attach to here, and the reason
is also why this port is short. A Vextro application's window is not a
handle to be negotiated for — it is a buffer of 32-bit pixels mapped
into the process's own address space, whose address `SYS_CANVAS` returns
and which the compositor reads every frame. The shortest path from a
rendered page to the screen is a memory copy.

## The files

    vxwpe.h     what the browser application calls
    vxwpe.c     the four libwpe interfaces, the blit, and the input
    pasteboard-noop.c
                libwpe's clipboard hook, in C

`pasteboard-noop.c` exists because upstream's is C++ — `pasteboard-noop.cpp`
— and there is no C++ runtime for this target. It is a rewrite of a file
that does nothing, not a patch to one that does something.

## What is implemented

libwpe asks a backend for four things, and WebKit will not start without
any of them:

| interface | upstream | here |
|---|---|---|
| view backend | the thing a `WebKitWebView` is built around | real |
| renderer host | owns the socket to the compositor | no descriptor |
| EGL backend | owns the display connection | `EGL_DEFAULT_DISPLAY` |
| EGL target | owns the window surface | no native window |

The last three are implemented *truthfully* rather than stubbed. An
engine configured for software rasterisation asks for a display, gets the
default, asks for a window, gets none, and takes its image-buffer path —
which is the path that ends at `vxwpe_present()`. A backend that returned
garbage from these would not fail there; it would fail some thousands of
lines into WebKit's compositor with a message about a surface.

## The loader

libwpe normally finds a backend by `dlopen`-ing a library named at run
time and looking up `_wpe_loader_interface` inside it. There is no
dynamic loader in this system.

That needs no patch: upstream ships `src/loader-static.c` for exactly
this case. It does no loading and expects `_wpe_loader_interface` to be
linked in, which `vxwpe.c` defines. `../libwpe/` is unmodified.

## What is not here

**No GPU.** There is no GL or EGL a ring-3 program on this system can
reach. The kernel drives the integrated graphics — `src/igpu.h` runs the
blitter the compositor composites with — but that is the kernel's, and
nothing exports it across the system-call boundary. WebKit must therefore
be built for software rasterisation, which is also why the JIT is off:
see `../wpe-config/`.

**No window origin from the compositor.** `SYS_GET_MOUSE` answers in
screen coordinates and the engine wants them relative to its viewport.
The compositor knows the difference and does not report it, so
`vxwpe_set_window_origin()` is how the application supplies it. Left at
zero, the pointer is correct only for a full-screen window — a pointer
consistently offset by the height of a title bar is the symptom.

**No clipboard, no gamepads, no touch.** The pasteboard is a no-op and
`vx_load_object` answers null for anything else, with a line on stderr
saying which — a null returned to WebKit becomes a feature that is
quietly missing, and knowing which one is the whole difference when a
page misbehaves.
