#include <stdint.h>
#include <stddef.h>
#include "../../include/hal.h"
#include "../idt.h"
#include "../mouse.h"
#include "../keyboard.h"

int hal_ps2_mouse_present = 0;
int hal_ps2_keyboard_present = 0;

static int32_t hal_screen_w = 1024;
static int32_t hal_screen_h = 768;

static int ps2_controller_exists(void) {
    /* Read PS/2 status port — if the controller is absent,
     * the bus floats high (0xFF). */
    uint8_t status = inb(0x64);
    if (status == 0xFF)
        return 0;
    return 1;
}

static int ps2_keyboard_probe(void) {
    /* Send "echo" command (0xEE) to keyboard, expect 0xEE back */
    ps2_wait_write();
    outb(0x60, 0xEE);
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) {
            uint8_t resp = inb(0x60);
            if (resp == 0xEE)
                return 1;
            if (resp == 0xFE) /* resend = device present but busy */
                return 1;
            break;
        }
    }
    return 1; /* Assume present if we got this far without 0xFF on status */
}

static int ps2_mouse_probe(void) {
    /* Enable aux port and check for ACK on enable-reporting command */
    ps2_cmd(0xA8);
    ps2_cmd(0xD4);
    ps2_wait_write();
    outb(0x60, 0xF2); /* Request device ID */
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) {
            uint8_t resp = inb(0x60);
            if (resp == 0xFA || resp == 0x00)
                return 1;
            break;
        }
    }
    return 1; /* Assume present — worst case we get no packets */
}

int hal_init(uint16_t code_selector, int32_t screen_w, int32_t screen_h) {
    hal_screen_w = screen_w;
    hal_screen_h = screen_h;

    if (!ps2_controller_exists()) {
        hal_ps2_mouse_present = 0;
        hal_ps2_keyboard_present = 0;
        return 0; /* Graceful: no PS/2 controller, no crash */
    }

    /* Probe and init keyboard */
    if (ps2_keyboard_probe()) {
        hal_ps2_keyboard_present = 1;
        keyboard_init(code_selector);
    }

    /* Probe and init mouse */
    if (ps2_mouse_probe()) {
        hal_ps2_mouse_present = 1;
        mouse_init(code_selector, screen_w - 1, screen_h - 1);
    }

    return 0;
}

void hal_poll_mouse(hal_mouse_state_t *state) {
    if (!hal_ps2_mouse_present) {
        state->x = hal_screen_w / 2;
        state->y = hal_screen_h / 2;
        state->buttons = 0;
        return;
    }
    state->x = mouse_x;
    state->y = mouse_y;
    state->buttons = mouse_buttons;
}

char hal_poll_keyboard(void) {
    if (!hal_ps2_keyboard_present)
        return 0;
    return kb_getchar();
}
