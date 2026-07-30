#include "socrates.h"

/*
 * Symbols borrowed from the kernel.
 *
 * A `.bsd` image carries no relocations and cannot reference anything
 * outside itself, so an application that wanted anti-aliased text used to
 * have to contain a TrueType rasteriser. This structure is the whole of
 * the alternative: a tag the loader searches the image for, a count, and
 * names with their addresses left zero. The loader fills them in before
 * the entry point runs.
 *
 * `volatile` so the compiler cannot decide the zeroes are the final word
 * and fold the calls away — the addresses are written by something it
 * cannot see.
 */
#define IMPORT_MAGIC 0x53524D50495F4253ULL

static volatile struct {
    unsigned long long magic;
    unsigned int count, reserved;
    struct { char name[24]; unsigned long long addr; } e[2];
} imports = {
    IMPORT_MAGIC, 2, 0,
    { { "ttf_text_width", 0 },
      { "ttf_draw_string", 0 } }
};

typedef int  (*ttf_text_width_fn)(const char *, int);
typedef void (*ttf_draw_string_fn)(unsigned int *, int, int, int, int,
                                   const char *, unsigned int, int);

void _start(void) {
    os_print("Hello from Socrates BSD 9!\n");
    os_print("Hybrid syscall framework active.\n");

    uint32_t gold = 0xD4AF37;
    int sx = 80, sy = 120, sz = 60;

    for (int x = sx; x < sx + sz; x++) {
        os_draw_pixel(x, sy, gold);
        os_draw_pixel(x, sy + sz - 1, gold);
    }
    for (int y = sy; y < sy + sz; y++) {
        os_draw_pixel(sx, y, gold);
        os_draw_pixel(sx + sz - 1, y, gold);
    }

    os_print("Gold square drawn via sys_draw_pixel.\n");

    /* And the borrowed rasteriser, if the loader found it. */
    if (imports.e[0].addr && imports.e[1].addr) {
        ttf_text_width_fn width = (ttf_text_width_fn)(unsigned long)imports.e[0].addr;
        int w = width("borrowed", 14);
        os_print(w > 0 ? "ttf_text_width resolved and returned a width\n"
                       : "ttf_text_width resolved but measured nothing\n");
    } else {
        os_print("no imports resolved - running standalone\n");
    }
}
