/*
 * gltest — libepoxy resolving entry points on a machine with no OpenGL,
 * and finding nine of them.
 *
 * ---- what this proves, link by link ----
 *
 * Every GL call below goes through epoxy's generated dispatch, which is
 * a `#define glClear epoxy_glClear` over a function pointer that starts
 * out pointing at a resolver. So one call exercises the whole chain:
 *
 *     glClear -> epoxy's dispatch stub
 *             -> epoxy_gl_dlsym
 *             -> dlopen("libGL.so.1")        libc/dlfcn.c
 *             -> dlsym("glClear")            the table in vxgl.c
 *             -> vx_glClear
 *             -> write() to /dev/dri/renderD128
 *             -> src/devfs.h
 *             -> this process's window surface
 *
 * and the last link is checkable from here: the pixels are read back
 * through a *different* path — glReadPixels, and then the canvas mapping
 * the kernel handed this program at startup — so a clear that wrote to
 * the wrong place, or to nowhere, fails rather than passes quietly.
 *
 * ---- and what it proves about the refusals ----
 *
 * The last section asks for an entry point that is deliberately not in
 * the table. libepoxy's own do_dlsym prints the name and calls abort(),
 * which is upstream's behaviour and not something this port chose — so
 * the check has to be made from a forked child, and the parent reads the
 * verdict out of the exit status. A test that called it directly would
 * end the program.
 *
 * That is the honest shape of graphics on this system and the reason it
 * is worth a test of its own: nine entry points are real, everything
 * past clearing and reading a rectangle is absent, and a program that
 * asks for one is told which one by name rather than shown a black
 * window.
 */

#include "vextro.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <epoxy/gl.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Reported by the provider, which counts what dlsym found and missed.
 * Declared here rather than in a header because it is not an API — it
 * exists so that this number can be printed at all. */
void vxgl_report(unsigned long *hits, unsigned long *misses,
                 const char **last_miss);

int main(void) {
    printf("gltest: libepoxy over /dev/dri/renderD128\n");

    /* ============================================================
     *  1. what is this running on?
     * ============================================================
     *
     * The four strings a graphics stack reads before it does anything
     * else. Each is answered truthfully by vxgl.c, and the version in
     * particular is chosen not to get past a capability test it would
     * then fail: 1.1 is the last OpenGL with no shaders and no vertex
     * arrays, which is the honest description of a device that can clear
     * a rectangle and read it back.
     */
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *rend   = (const char *)glGetString(GL_RENDERER);
    const char *ver    = (const char *)glGetString(GL_VERSION);

    check("glGetString resolved and answered", vendor != NULL);
    check("the vendor is this system", vendor && strcmp(vendor, "Vextro") == 0);
    check("the renderer names the device it actually is",
          rend && strstr(rend, "renderD128") != NULL);
    check("the version does not claim shaders it does not have",
          ver && ver[0] == '1');

    /* ============================================================
     *  2. the viewport, and what the device says it can do
     * ============================================================ */
    GLint vp[4] = { -1, -1, -1, -1 };
    glGetIntegerv(GL_VIEWPORT, vp);
    check("the viewport starts as the whole window", vp[2] > 0 && vp[3] > 0);

    const GLint W = vp[2], H = vp[3];

    GLint maxtex = -1;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    /* Zero, and it is arithmetic rather than a placeholder: there are no
     * textures, so the largest one is zero across. */
    check("the maximum texture size is honestly zero", maxtex == 0);

    GLint major = -1;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    check("and the major version agrees with the string", major == 1);

    /* ============================================================
     *  3. a clear that has to reach real pixels
     * ============================================================
     *
     * Read back two ways. glReadPixels goes down the same descriptor the
     * clear went up, so on its own it would prove only that the file
     * remembered what was written to it. The canvas mapping is the other
     * path — the kernel handed this program the surface's pages at
     * startup — and a clear that landed in a scratch buffer rather than
     * in the window passes the first check and fails the second.
     */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check("glClear resolved and ran", glGetError() == GL_NO_ERROR);

    /* A colour with three different bytes, so a channel swap cannot
     * hide: 0x33, 0x77, 0xBB. */
    glClearColor(0x33 / 255.0f, 0x77 / 255.0f, 0xBB / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check("the second clear ran too", glGetError() == GL_NO_ERROR);

    {
        static unsigned char back[16 * 4];
        glReadPixels(0, 0, 4, 1, GL_BGRA, GL_UNSIGNED_BYTE, back);
        check("glReadPixels resolved and ran", glGetError() == GL_NO_ERROR);
        /* The surface holds 0x00RRGGBB, which little-endian is B,G,R,0. */
        check("the pixels read back are the colour that was set",
              back[0] == 0xBB && back[1] == 0x77 && back[2] == 0x33);

        /* And the same pixel through the mapping, which is a different
         * road to the same memory. */
        int cwi = 0, chi = 0;
        const unsigned int *canvas = (const unsigned int *)os_canvas(&cwi, &chi);
        const unsigned int cw = (unsigned int)cwi, ch = (unsigned int)chi;
        check("the canvas mapping is there", canvas != NULL);
        check("and agrees about the geometry",
              (GLint)cw == W && (GLint)ch == H);
        if (!canvas) goto after_canvas;
        /* GL's origin is bottom-left, the framebuffer's is top-left, so
         * the pixel read at (0,0) is the *last* row of the mapping. */
        const unsigned int corner = canvas[(ch - 1) * cw];
        check("and the window itself holds that colour",
              (corner & 0x00FFFFFFu) == 0x003377BBu);
after_canvas: ;
    }

    /* A viewport smaller than the window, cleared to something else, so
     * that the rectangle arithmetic is exercised rather than assumed. */
    if (W >= 32 && H >= 32) {
        glViewport(0, 0, 8, 8);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        check("a viewport clear ran", glGetError() == GL_NO_ERROR);

        int cwi = 0, chi = 0;
        const unsigned int *canvas = (const unsigned int *)os_canvas(&cwi, &chi);
        const unsigned int cw = (unsigned int)cwi, ch = (unsigned int)chi;
        /* Inside the viewport: white. Outside it: still the previous
         * colour, which is what makes this a check on the *bounds*
         * rather than on the clear. */
        if (canvas)
        check("the viewport rectangle is white",
              (canvas[(ch - 1) * cw + 1] & 0x00FFFFFFu) == 0x00FFFFFFu);
        if (canvas)
        check("and the pixel beyond it was left alone",
              (canvas[(ch - 1) * cw + 20] & 0x00FFFFFFu) == 0x003377BBu);
        glViewport(0, 0, W, H);
    }

    /* ============================================================
     *  4. what happens when it asks for something that is not there
     * ============================================================
     *
     * In a child, because libepoxy's answer is to print the name and
     * abort — upstream's behaviour, deliberately not patched, because a
     * stub that returned quietly would produce a black window and no
     * explanation. The child cannot report; its death *is* the report,
     * and the parent reads it out of the status.
     */
    {
        unsigned long hits = 0, misses = 0;
        const char *last = NULL;
        vxgl_report(&hits, &misses, &last);
        check("dlsym found the entry points this system has", hits >= 6);
        printf("gltest: %lu entry points resolved, %lu not present\n",
               hits, misses);

        const pid_t kid = fork();
        if (kid == 0) {
            /* Not in the table, and chosen because it is the first call
             * any modern GL program makes: everything with a pipeline
             * behind it needs a device that executes, and this one is a
             * framebuffer. */
            glDrawArrays(GL_TRIANGLES, 0, 3);
            /* Only reached if it somehow resolved, which would mean the
             * table had grown something it should not have. */
            _exit(70);
        }
        int st = 0;
        waitpid(kid, &st, 0);
        check("an unimplemented entry point does not silently succeed",
              WEXITSTATUS(st) != 70);
    }

    printf("gltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/*
 * No _start here. This program is linked with libc/crt0.o, which is what
 * runs static constructors — and vxgl.c registers its symbol table from
 * one, because libepoxy resolves lazily but not late: a program whose
 * first statement is a GL call would otherwise find nothing registered.
 * crt0 calls main().
 */
