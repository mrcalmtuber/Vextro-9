#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include "idt.h"

#define KB_BUF_SIZE 256

static volatile char kb_buffer[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

static volatile int kb_shift = 0;

static const char scancode_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancode_upper[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

__attribute__((interrupt))
static void irq1_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t sc = inb(0x60);

    if (sc & 0x80) {
        uint8_t released = sc & 0x7F;
        if (released == 0x2A || released == 0x36)
            kb_shift = 0;
    } else {
        if (sc == 0x2A || sc == 0x36) {
            kb_shift = 1;
        } else {
            char ch = kb_shift ? scancode_upper[sc] : scancode_lower[sc];
            if (ch) {
                uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
                if (next != kb_tail) {
                    kb_buffer[kb_head] = ch;
                    kb_head = next;
                }
            }
        }
    }

    outb(PIC1_CMD, PIC_EOI);
}

static void keyboard_init(uint16_t cs) {
    idt_set_gate(0x21, irq1_handler, cs);

    /* Unmask IRQ1 on master PIC (preserve other bits) */
    uint8_t mask = inb(PIC1_DATA);
    mask &= (uint8_t)~(1 << 1);
    outb(PIC1_DATA, mask);
}

static char kb_getchar(void) {
    if (kb_tail == kb_head) return 0;
    char ch = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return ch;
}

#endif /* KEYBOARD_H */
