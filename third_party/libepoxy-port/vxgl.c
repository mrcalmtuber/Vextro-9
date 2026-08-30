/*
 * third_party/libepoxy-port/vxgl.c — the entry points libepoxy resolves
 * on this system, and the framebuffer they are bound to.
 *
 * ============================================================
 *  WHAT THIS IS, SAID PLAINLY BEFORE ANYTHING ELSE
 * ============================================================
 *
 * This is not an OpenGL implementation and nothing here should be read
 * as one. It is a *provider*: a table of a dozen named functions that
 * libepoxy can find with dlsym, each of which does the thing its name
 * means when the only device underneath is a linear framebuffer.
 *
 * The distinction matters because libepoxy's whole purpose is to hide
 * it. Epoxy is a dispatcher — it resolves some two thousand GL entry
 * points by name at run time and calls through the pointer — so a
 * program linked against it compiles and links whether or not a single
 * one of those names exists. What decides the outcome is what dlsym
 * finds, and on this machine dlsym finds twelve.
 *
 * ---- why twelve and not zero ----
 *
 * Because twelve of them have an honest meaning against a framebuffer,
 * and a graphics stack starting up asks for exactly those first:
 * glGetString to find out what it is talking to, glGetIntegerv to size
 * its buffers, glViewport and glClear to put something on the screen,
 * glReadPixels to get it back. A stack that gets truthful answers to
 * those and then discovers there is no glDrawArrays has learned
 * something. A stack that gets nothing has only learned that dlopen
 * failed.
 *
 * ---- why not thirteen ----
 *
 * Because the thirteenth would be a lie. Everything past clearing and
 * reading a rectangle — vertex arrays, shaders, textures, framebuffer
 * objects, anything with a pipeline behind it — needs a device that
 * *executes*, and /dev/dri/renderD128 on this system is a window's
 * pixels reached through a descriptor. src/devfs.h says so at the point
 * it is implemented: "What that is not is a DRM device. Real DRI is
 * almost entirely ioctl ... this kernel has no ioctl at all."
 *
 * So the unimplemented names are simply *absent from the table*, and
 * that is the design rather than an omission. libepoxy's own
 * do_dlsym() prints
 *
 *     glDrawArrays() not found: this system does not have that symbol
 *
 * and aborts — which is upstream's behaviour, unmodified, naming the
 * exact call that could not be served. A stub that returned quietly
 * would have produced a black window and no explanation.
 *
 * ---- and the honest summary ----
 *
 * A program can, through this, clear its window to a colour, set a
 * viewport, read its pixels back, and ask what it is running on. It
 * cannot draw a triangle. The rung that changes that is a software GL
 * over src/g3d.h's rasteriser, which is a separate project and not this
 * file pretending to be one.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * GL's types, spelled out rather than included.
 *
 * <epoxy/gl.h> would bring the generated dispatch table with it, and
 * that table is a wall of `#define glClear epoxy_glClear`. Including it
 * here would rename the functions below out from under the table that
 * is meant to export them — the provider would end up registering
 * epoxy's own dispatch stubs against their own names, which resolves to
 * infinite recursion on the first call.
 */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLuint;
typedef float          GLclampf;
typedef float          GLfloat;
typedef void           GLvoid;
typedef unsigned char  GLubyte;

#define GL_NO_ERROR          0x0000
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502

#define GL_COLOR_BUFFER_BIT  0x00004000
#define GL_DEPTH_BUFFER_BIT  0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400

#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_EXTENSIONS               0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

#define GL_VIEWPORT          0x0BA2
#define GL_MAX_TEXTURE_SIZE  0x0D33
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_MAJOR_VERSION     0x821B
#define GL_MINOR_VERSION     0x821C
#define GL_NUM_EXTENSIONS    0x821D

#define GL_RGBA              0x1908
#define GL_BGRA              0x80E1
#define GL_UNSIGNED_BYTE     0x1401

/* ============================================================
 *  the device
 * ============================================================ */

static int   vx_fd = -1;
static int   vx_width = 0, vx_height = 0;
static GLenum vx_error = GL_NO_ERROR;

static GLint vx_vp[4] = { 0, 0, 0, 0 };
static float vx_clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/*
 * How large the window is.
 *
 * Asked of the kernel rather than derived from the node's length,
 * because a length is a number of bytes and a framebuffer needs a
 * width: the same 961,536 bytes is 598 by 402 and also 402 by 598, and
 * guessing wrong turns every clear into diagonal stripes. SYS_CANVAS is
 * the one call that answers it and is what every program on this system
 * uses to find its own window.
 */
static void vx_geometry(void) {
    if (vx_width) return;
    unsigned long out[3] = { 0, 0, 0 };
    __syscall1(SYS_CANVAS, (long)(void *)out);
    vx_width  = (int)out[1];
    vx_height = (int)out[2];
    if (vx_vp[2] == 0) { vx_vp[2] = vx_width; vx_vp[3] = vx_height; }
}

static int vx_open(void) {
    if (vx_fd >= 0) return vx_fd;
    vx_geometry();
    /*
     * The render node, by the name a graphics stack expects — which is
     * the whole point of src/devfs.h having one. Read-write because
     * glClear writes and glReadPixels reads, and both go through the
     * same descriptor so that the offset is one thing rather than two.
     */
    vx_fd = open("/dev/dri/renderD128", O_RDWR);
    return vx_fd;
}

/* One scanline's worth of the clear colour, built once per clear and
 * written row by row. A whole-surface buffer would be a megabyte on the
 * stack; a pixel at a time would be a system call per pixel. */
static unsigned int vx_pack_clear(void) {
    unsigned int r = (unsigned int)(vx_clear[0] * 255.0f + 0.5f);
    unsigned int g = (unsigned int)(vx_clear[1] * 255.0f + 0.5f);
    unsigned int b = (unsigned int)(vx_clear[2] * 255.0f + 0.5f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    /* The compositor's surface is 0x00RRGGBB, which is what
     * src/gfx.h draws into and what the canvas mapping exposes. */
    return (r << 16) | (g << 8) | b;
}

/* ============================================================
 *  the twelve
 * ============================================================ */

static void vx_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    vx_clear[0] = r; vx_clear[1] = g; vx_clear[2] = b; vx_clear[3] = a;
}

static void vx_glClear(GLbitfield mask) {
    /*
     * Only the colour buffer, and the other two bits are accepted and
     * ignored rather than refused. There is no depth buffer and no
     * stencil buffer here, so "clear them" is a request that is already
     * satisfied — every program clears all three together and refusing
     * would break every one of them for no gain.
     */
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;

    const int fd = vx_open();
    if (fd < 0) { vx_error = GL_INVALID_OPERATION; return; }

    int x0 = vx_vp[0], y0 = vx_vp[1], w = vx_vp[2], h = vx_vp[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (w > vx_width - x0)  w = vx_width - x0;
    if (h > vx_height - y0) h = vx_height - y0;
    if (w <= 0 || h <= 0) return;

    static unsigned int row[4096];
    const unsigned int c = vx_pack_clear();
    const int rw = w > 4096 ? 4096 : w;
    for (int i = 0; i < rw; i++) row[i] = c;

    /*
     * GL's origin is the bottom-left and a framebuffer's is the
     * top-left, so the rows are written in the opposite order. It costs
     * nothing here and it is the difference between a viewport clear
     * that lands where the program asked and one that is mirrored.
     */
    for (int y = 0; y < h; y++) {
        const int fbrow = vx_height - 1 - (y0 + y);
        const long off = ((long)fbrow * vx_width + x0) * 4;
        if (lseek(fd, off, SEEK_SET) < 0) { vx_error = GL_INVALID_OPERATION; return; }
        int left = w;
        while (left > 0) {
            const int n = left > rw ? rw : left;
            if (write(fd, row, (unsigned)n * 4) < 0) {
                vx_error = GL_INVALID_OPERATION;
                return;
            }
            left -= n;
        }
    }
}

static void vx_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (w < 0 || h < 0) { vx_error = GL_INVALID_VALUE; return; }
    vx_geometry();
    vx_vp[0] = x; vx_vp[1] = y; vx_vp[2] = w; vx_vp[3] = h;
}

static const GLubyte *vx_glGetString(GLenum name) {
    /*
     * The four strings a stack reads at startup, and each says what is
     * actually here rather than what would get past a version check.
     *
     * The version is deliberately "1.1", which is the last OpenGL that
     * had no shaders and no vertex arrays — the honest description of a
     * device that can clear a rectangle and read it back. Claiming 3.3
     * would get a stack past its capability test and into the first
     * glCreateShader, which is not in the table, which aborts. A program
     * that reads 1.1 and gives up has been told the truth at the point
     * it asked.
     */
    switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"Vextro";
    case GL_RENDERER: return (const GLubyte *)"framebuffer over /dev/dri/renderD128";
    case GL_VERSION:  return (const GLubyte *)"1.1 Vextro framebuffer";
    case GL_SHADING_LANGUAGE_VERSION:
        /* Null, which is what GL 1.1 returns and what it means: there is
         * no shading language, so there is no version of one. */
        return (const GLubyte *)0;
    case GL_EXTENSIONS: return (const GLubyte *)"";
    default:
        vx_error = GL_INVALID_ENUM;
        return (const GLubyte *)0;
    }
}

static GLenum vx_glGetError(void) {
    const GLenum e = vx_error;
    vx_error = GL_NO_ERROR;    /* reading clears, as GL requires */
    return e;
}

static void vx_glGetIntegerv(GLenum name, GLint *out) {
    if (!out) { vx_error = GL_INVALID_VALUE; return; }
    vx_geometry();
    switch (name) {
    case GL_VIEWPORT:
        out[0] = vx_vp[0]; out[1] = vx_vp[1];
        out[2] = vx_vp[2]; out[3] = vx_vp[3];
        break;
    case GL_MAX_VIEWPORT_DIMS:
        out[0] = vx_width; out[1] = vx_height;
        break;
    case GL_MAX_TEXTURE_SIZE:
        /* Zero, and it is not a placeholder: there are no textures, and
         * a maximum size of zero is the arithmetic truth about how large
         * the largest one may be. */
        out[0] = 0;
        break;
    case GL_MAJOR_VERSION: out[0] = 1; break;
    case GL_MINOR_VERSION: out[0] = 1; break;
    case GL_NUM_EXTENSIONS: out[0] = 0; break;
    default:
        vx_error = GL_INVALID_ENUM;
        break;
    }
}

static void vx_glFlush(void) {
    /* Nothing is queued. A write to the render node reaches the pixels
     * inside the system call that made it — src/devfs.h bumps the
     * surface generation there — so there is never anything in flight
     * for a flush to push. */
}

static void vx_glFinish(void) { vx_glFlush(); }

static void vx_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
                            GLenum format, GLenum type, GLvoid *pixels) {
    if (!pixels || w < 0 || h < 0) { vx_error = GL_INVALID_VALUE; return; }
    if (type != GL_UNSIGNED_BYTE ||
        (format != GL_RGBA && format != GL_BGRA)) {
        /* One layout is what the surface holds, and converting between
         * the others is work this file should not silently do wrong.
         * INVALID_ENUM is GL's own answer for a format it cannot serve. */
        vx_error = GL_INVALID_ENUM;
        return;
    }
    const int fd = vx_open();
    if (fd < 0) { vx_error = GL_INVALID_OPERATION; return; }
    if (x < 0 || y < 0 || x + w > vx_width || y + h > vx_height) {
        vx_error = GL_INVALID_VALUE;
        return;
    }

    unsigned char *out = (unsigned char *)pixels;
    for (int row = 0; row < h; row++) {
        /* Bottom-up, for the reason glClear writes bottom-up. */
        const int fbrow = vx_height - 1 - (y + row);
        const long off = ((long)fbrow * vx_width + x) * 4;
        if (lseek(fd, off, SEEK_SET) < 0) { vx_error = GL_INVALID_OPERATION; return; }
        unsigned char *dst = out + (size_t)row * (size_t)w * 4;
        long left = (long)w * 4;
        while (left > 0) {
            const long got = read(fd, dst, (unsigned long)left);
            if (got <= 0) { vx_error = GL_INVALID_OPERATION; return; }
            dst += got;
            left -= got;
        }
        if (format == GL_RGBA) {
            /* The surface is 0x00RRGGBB in memory, which read back
             * little-endian is B,G,R,0. A caller asking for RGBA gets
             * the bytes reordered here rather than a silently swapped
             * picture. */
            unsigned char *p = out + (size_t)row * (size_t)w * 4;
            for (int i = 0; i < w; i++) {
                const unsigned char b = p[i * 4 + 0];
                const unsigned char r = p[i * 4 + 2];
                p[i * 4 + 0] = r;
                p[i * 4 + 2] = b;
                p[i * 4 + 3] = 0xFF;
            }
        }
    }
}

/* ============================================================
 *  the table
 * ============================================================
 *
 * Registered under both names libepoxy tries on a generic Unix, in the
 * order it tries them: GLX_LIB is "libGL.so.1" and OPENGL_LIB is
 * "libOpenGL.so.0". Registering both means the answer does not depend on
 * which one it reaches for first, which is a detail of upstream's
 * loading order that this file should not be sensitive to.
 */
static const vx_dl_symbol_t vx_gl_table[] = {
    { "glClearColor",  (void *)vx_glClearColor  },
    { "glClear",       (void *)vx_glClear       },
    { "glViewport",    (void *)vx_glViewport    },
    { "glGetString",   (void *)vx_glGetString   },
    { "glGetError",    (void *)vx_glGetError    },
    { "glGetIntegerv", (void *)vx_glGetIntegerv },
    { "glFlush",       (void *)vx_glFlush       },
    { "glFinish",      (void *)vx_glFinish      },
    { "glReadPixels",  (void *)vx_glReadPixels  },
    { 0, 0 }
};

/*
 * Registered from a constructor, which libc/crt0.c runs before main.
 *
 * It has to be before main because libepoxy's own dispatch is lazy but
 * not deferred: the first GL call a program makes resolves through
 * dlopen, and a program whose first statement is glClearColor would
 * otherwise find nothing registered. A constructor is the only hook that
 * runs earlier than the program's own first line.
 */
__attribute__((constructor))
static void vx_gl_register(void) {
    vx_dl_register("libGL.so.1", vx_gl_table);
    vx_dl_register("libOpenGL.so.0", vx_gl_table);
    /* libGLX.so.1 deliberately not registered. It is the GLX half of
     * glvnd — OpenGL over the X11 protocol — and there is no X server
     * here. Epoxy tries it first and getting nothing is the correct
     * answer; it then falls through to libGL.so.1. */
}

/*
 * How much of GL this program actually found.
 *
 * Not part of any API. It is here because "the stack resolved nine entry
 * points and missed four hundred" is the single most useful sentence
 * about graphics running on this machine, and there is no other way to
 * obtain it — libepoxy aborts on the first miss, so the count is taken
 * before that happens or not at all.
 */
void vxgl_report(unsigned long *hits, unsigned long *misses,
                 const char **last_miss);
void vxgl_report(unsigned long *hits, unsigned long *misses,
                 const char **last_miss) {
    if (hits) *hits = vx_dl_hits();
    if (misses) *misses = vx_dl_misses();
    if (last_miss) *last_miss = vx_dl_last_miss();
}
