/*
 * winhello — a Windows executable, running on Vextro 9.
 *
 * Built by mingw-w64 into a genuine PE64 with a relocation table and an
 * import table naming vextro.dll, linked against an import library
 * generated from a .def file. Nothing about it is special-cased: the
 * loader relocates it to a randomised base, walks its imports, and
 * refuses it by name if any of them are unknown.
 *
 * It is written to the Microsoft calling convention, because that is
 * what the compiler producing it emits. The trampoline page shuffles
 * RCX/RDX/R8/R9 into the registers this kernel's syscalls expect, which
 * is the whole of what makes the two worlds meet.
 */

typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long long uint64_t;

__declspec(dllimport) void      VxPrint(const char *s);
__declspec(dllimport) void      VxDrawPixel(int x, int y, uint32_t colour);
__declspec(dllimport) uint32_t *VxCanvas(uint64_t *info);
__declspec(dllimport) void      VxYield(void);
__declspec(dllimport) void      VxExit(int code);
__declspec(dllimport) int       VxTextWidth(const char *s, int size);

#define CANVAS_W 598
#define CANVAS_H 402

/*
 * A table of pointers, in writable data.
 *
 * This is here to make the image need relocating. Ordinary code on
 * x86-64 is position independent without trying -- every reference is
 * relative to the instruction pointer -- so a small program can be
 * linked with no relocation table at all and can then only be loaded at
 * the address in its own header. An array of pointers cannot be
 * expressed that way: each element is an absolute address, so the linker
 * must record where they are and the loader must fix them up.
 *
 * Without this the loader's relocation path is never exercised by
 * anything that ships.
 */
/* volatile so the compiler cannot decide it knows what these
 * point at and fold the reads away, which is exactly what it
 * did the first time and why no relocations were emitted. */
static const char *volatile banner[] = {
    "winhello: a PE64 image, relocated and linked at load time\n",
    "winhello: writing straight into the mapped canvas\n",
    "winhello: VxTextWidth returned a width\n",
    "winhello: VxTextWidth measured nothing\n",
    "winhello: done\n",
};

void PeMain(void) {
    VxPrint(banner[0]);

    uint64_t info[3] = { 0, 0, 0 };
    uint32_t *canvas = VxCanvas(info);
    int w = canvas ? (int)info[1] : CANVAS_W;
    int h = canvas ? (int)info[2] : CANVAS_H;

    if (canvas)
        VxPrint(banner[1]);

    /*
     * Something obviously computed rather than obviously constant: a
     * radial interference pattern in double precision, which also
     * demonstrates that the floating-point unit is available to a
     * process loaded from a Windows image.
     */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = (double)(x - w / 2) / (double)w;
            double dy = (double)(y - h / 2) / (double)h;
            double r  = dx * dx + dy * dy;
            double v  = r * 900.0;
            /* a cheap triangle wave, so no maths library is needed */
            double t  = v - (double)(long long)v;
            if (((long long)v) & 1) t = 1.0 - t;

            uint32_t g = (uint32_t)(t * 200.0) + 20u;
            uint32_t c = (g << 16) | ((g * 7 / 8) << 8) | (g / 3);
            if (canvas) canvas[y * w + x] = c;
            else        VxDrawPixel(x, y, c);
        }
        if ((y & 31) == 0) VxYield();
    }

    int tw = VxTextWidth("measured through an import", 14);
    VxPrint(tw > 0 ? banner[2] : banner[3]);
    VxPrint(banner[4]);
    VxExit(0);
}
