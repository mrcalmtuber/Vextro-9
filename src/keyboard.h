#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include "idt.h"

#define KB_BUF_SIZE 256

/* Special (non-ASCII) keys are delivered through kb_getchar() as control
 * codes in the 0x11..0x19 range, which no printable path ever emits. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18
#define KEY_DEL   0x19

static volatile char kb_buffer[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

static volatile int kb_shift = 0;
static volatile int kb_ext = 0;      /* saw E0 prefix */

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

__attribute__((target("general-regs-only")))
static void kb_push(char ch) {
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buffer[kb_head] = ch;
        kb_head = next;
    }
}

__attribute__((interrupt, target("general-regs-only")))
static void irq1_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        kb_ext = 1;
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    if (kb_ext) {
        kb_ext = 0;
        if (!(sc & 0x80)) {
            switch (sc) {
            case 0x48: kb_push(KEY_UP);    break;
            case 0x50: kb_push(KEY_DOWN);  break;
            case 0x4B: kb_push(KEY_LEFT);  break;
            case 0x4D: kb_push(KEY_RIGHT); break;
            case 0x49: kb_push(KEY_PGUP);  break;
            case 0x51: kb_push(KEY_PGDN);  break;
            case 0x47: kb_push(KEY_HOME);  break;
            case 0x4F: kb_push(KEY_END);   break;
            case 0x53: kb_push(KEY_DEL);   break;
            default: break;
            }
        }
    } else if (sc & 0x80) {
        uint8_t released = sc & 0x7F;
        if (released == 0x2A || released == 0x36)
            kb_shift = 0;
    } else {
        if (sc == 0x2A || sc == 0x36) {
            kb_shift = 1;
        } else {
            char ch = kb_shift ? scancode_upper[sc] : scancode_lower[sc];
            if (ch) kb_push(ch);
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
