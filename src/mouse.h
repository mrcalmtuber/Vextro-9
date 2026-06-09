#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "idt.h"

/* Sensitivity multiplier: applied to raw PS/2 deltas before smoothing */
#define MOUSE_SENSITIVITY 1

/* Global cursor position + button state — written by IRQ12, read by render loop */
volatile int32_t mouse_x = 0;
volatile int32_t mouse_y = 0;
volatile uint8_t mouse_buttons = 0;

static int32_t mouse_max_x = 1023;
static int32_t mouse_max_y = 767;

/* 3-byte packet assembly state */
static uint8_t mouse_buf[3];
static int     mouse_phase = 0;

/* Smoothing: sub-pixel accumulator (8-bit fractional) */
static int32_t accum_x_fp = 0;
static int32_t accum_y_fp = 0;

/* 2-tap FIR filter: previous post-sensitivity deltas */
static int32_t prev_fir_dx = 0;
static int32_t prev_fir_dy = 0;

/* ---- PS/2 controller I/O ---- */
static void ps2_wait_write(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++);
}
static void ps2_wait_read(void) {
    for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++);
}
static void ps2_cmd(uint8_t c)  { ps2_wait_write(); outb(0x64, c); }
static void ps2_data(uint8_t d) { ps2_wait_write(); outb(0x60, d); }
static uint8_t ps2_read(void)   { ps2_wait_read();  return inb(0x60); }

/* ---- IRQ12 handler: parse PS/2 3-byte packets ---- */
__attribute__((interrupt))
static void irq12_handler(interrupt_frame_t *frame) {
    (void)frame;

    /* Bit 5 of status port: output came from aux port (mouse) */
    if (!(inb(0x64) & 0x20)) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    uint8_t data = inb(0x60);

    /* Packet byte 0 must always have bit 3 set — resync if lost */
    if (mouse_phase == 0 && !(data & 0x08)) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    mouse_buf[mouse_phase++] = data;

    if (mouse_phase == 3) {
        mouse_phase = 0;
        uint8_t flags = mouse_buf[0];

        mouse_buttons = flags & 0x07;

        /* Discard packets that report coordinate overflow */
        if (!(flags & 0xC0)) {
            int32_t dx = (int32_t)(uint32_t)mouse_buf[1];
            int32_t dy = (int32_t)(uint32_t)mouse_buf[2];

            /* Sign-extend using the sign bits in the flags byte */
            if (flags & 0x10) dx |= (int32_t)0xFFFFFF00;
            if (flags & 0x20) dy |= (int32_t)0xFFFFFF00;

            /* Invert Y for screen coordinates */
            dy = -dy;

            /* 1:1 raw mapping — no acceleration, no sensitivity scaling */

            /* 2-tap FIR low-pass: 75% current + 25% previous */
            int32_t raw_dx = dx, raw_dy = dy;
            dx = (dx * 3 + prev_fir_dx) >> 2;
            dy = (dy * 3 + prev_fir_dy) >> 2;
            prev_fir_dx = raw_dx;
            prev_fir_dy = raw_dy;

            /* Scale into 8-bit fixed point for sub-pixel accumulation */
            int32_t dx_fp = dx << 8;
            int32_t dy_fp = dy << 8;

            /* Sub-pixel accumulator: prevents truncation jitter
             * by carrying fractional remainders across packets */
            accum_x_fp += dx_fp;
            accum_y_fp += dy_fp;

            int32_t move_x = accum_x_fp >> 8;
            int32_t move_y = accum_y_fp >> 8;
            accum_x_fp -= move_x << 8;
            accum_y_fp -= move_y << 8;

            mouse_x += move_x;
            mouse_y += move_y;

            if (mouse_x < 0)           mouse_x = 0;
            if (mouse_y < 0)           mouse_y = 0;
            if (mouse_x > mouse_max_x) mouse_x = mouse_max_x;
            if (mouse_y > mouse_max_y) mouse_y = mouse_max_y;
        }
    }

    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* ---- Initialize PS/2 mouse and hook IRQ12 ---- */
static void mouse_init(uint16_t cs, int32_t max_x, int32_t max_y) {
    mouse_max_x = max_x;
    mouse_max_y = max_y;
    mouse_x     = max_x / 2;
    mouse_y     = max_y / 2;

    ps2_cmd(0xA8);         /* Enable aux (mouse) port */

    ps2_cmd(0x20);         /* Read controller config byte */
    uint8_t cfg = ps2_read();
    cfg |=  (uint8_t)(1 << 1);   /* enable IRQ12 */
    cfg &= (uint8_t)~(1 << 5);   /* clear: enable aux clock */
    ps2_cmd(0x60);         /* Write config byte */
    ps2_data(cfg);

    ps2_cmd(0xD4);         /* Route next byte to mouse */
    ps2_data(0xF4);        /* Mouse command: enable packet reporting */
    ps2_read();            /* Consume ACK (0xFA) */

    /* Install the IRQ12 handler at vector 0x2C (slave IRQ4 = IRQ12) */
    idt_set_gate(0x2C, irq12_handler, cs);

    /* Unmask: IRQ2 (cascade) on master, IRQ4 (IRQ12) on slave */
    outb(PIC1_DATA, (uint8_t)~(1 << 2));
    outb(PIC2_DATA, (uint8_t)~(1 << 4));
}

#endif /* MOUSE_H */
