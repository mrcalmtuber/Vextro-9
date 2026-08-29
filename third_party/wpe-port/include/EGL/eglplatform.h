#ifndef __eglplatform_h_
#define __eglplatform_h_

/*
 * EGL/eglplatform.h — two typedefs, and nothing else.
 *
 * libwpe includes this header for exactly one reason: its renderer
 * interface has to name the types of a native display and a native
 * window, and those are platform-defined. It does not call EGL, link
 * against it, or dereference either type — it passes them through from
 * the backend to WebKit.
 *
 * That is why supplying the header rather than an EGL implementation is
 * enough, and it is not a workaround: upstream does the same thing for
 * Windows, where getting a real EGL means building ANGLE. The comment in
 * renderer-backend-egl.h says so, and this file is the same answer for a
 * platform whose EGL is not merely inconvenient but absent.
 *
 * There is no GL or EGL a ring-3 program on this system can reach. The
 * kernel drives the integrated graphics — src/igpu.h runs the blitter
 * the compositor composites with — but that is the kernel's, and nothing
 * exports it across the system-call boundary. WebKit is therefore built
 * for software rasterisation, and these two types are carried but never
 * used.
 *
 * The definitions match the Khronos header for a platform with no window
 * system, which is what EGL_PLATFORM_DEVICE and headless builds use.
 */

#include <stdint.h>

typedef intptr_t EGLNativeDisplayType;
typedef intptr_t EGLNativePixmapType;
typedef intptr_t EGLNativeWindowType;

/* The names EGL 1.4 used, kept because some code still spells them this
 * way and the two are required to be the same type. */
typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativePixmapType  NativePixmapType;
typedef EGLNativeWindowType  NativeWindowType;

typedef int32_t EGLint;

#endif /* __eglplatform_h_ */
