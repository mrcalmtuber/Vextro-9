#include "socrates.h"

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
}
