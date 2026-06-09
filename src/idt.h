#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* ---- I/O port helpers ---- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- MSR access helpers ---- */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)) : "memory");
}

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

/* ---- CPU-pushed interrupt frame (no error code) ---- */
typedef struct {
    uint64_t ip, cs, flags, sp, ss;
} interrupt_frame_t;

/* ---- 64-bit interrupt gate descriptor (16 bytes) ---- */
typedef struct {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;   /* 0x8E = present | DPL=0 | 64-bit interrupt gate */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

#define IDT_SIZE 256
static idt_entry_t idt_table[IDT_SIZE];

static void idt_set_gate(int vec, void (*fn)(interrupt_frame_t *), uint16_t sel) {
    uint64_t addr = (uint64_t)(uintptr_t)fn;
    idt_table[vec].offset_lo  = (uint16_t)(addr & 0xFFFF);
    idt_table[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt_table[vec].offset_hi  = (uint32_t)(addr >> 32);
    idt_table[vec].selector   = sel;
    idt_table[vec].ist        = 0;
    idt_table[vec].type_attr  = 0x8E;
    idt_table[vec].reserved   = 0;
}

/* ---- 8259 PIC constants ---- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

/* Remap PIC: master IRQ0-7 → 0x20-0x27, slave IRQ8-15 → 0x28-0x2F */
static void pic_remap(void) {
    outb(PIC1_CMD, 0x11); io_wait();   /* ICW1: begin init, ICW4 needed */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();  /* ICW2: master base vector */
    outb(PIC2_DATA, 0x28); io_wait();  /* ICW2: slave base vector */
    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: master has slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* ICW3: slave cascade id = 2 */
    outb(PIC1_DATA, 0x01); io_wait();  /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    /* Mask all IRQs — individual drivers unmask their own lines */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ---- Catch-all: silently return from any unexpected vector ---- */
__attribute__((interrupt))
static void isr_noop(interrupt_frame_t *f) { (void)f; }

/* Handler for exceptions that push an error code (vectors 8,10-14,17,21,29,30).
 * Without this, iretq pops the stale error code as RIP → cascade → triple fault. */
__attribute__((interrupt))
static void isr_noop_err(interrupt_frame_t *f, uint64_t err) {
    (void)f; (void)err;
    while (1) __asm__ volatile("hlt");
}

/* Remap PIC + fill all 256 gates with the no-op stub */
static void idt_init(uint16_t cs) {
    pic_remap();
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, isr_noop, cs);

    /* Override vectors that push an error code with the correct signature */
    void (*eh)(interrupt_frame_t *) =
        (void (*)(interrupt_frame_t *))(uintptr_t)isr_noop_err;
    idt_set_gate(8,  eh, cs);   /* #DF Double Fault        */
    idt_set_gate(10, eh, cs);   /* #TS Invalid TSS         */
    idt_set_gate(11, eh, cs);   /* #NP Segment Not Present */
    idt_set_gate(12, eh, cs);   /* #SS Stack-Segment Fault */
    idt_set_gate(13, eh, cs);   /* #GP General Protection  */
    idt_set_gate(14, eh, cs);   /* #PF Page Fault          */
    idt_set_gate(17, eh, cs);   /* #AC Alignment Check     */
    idt_set_gate(21, eh, cs);   /* #CP Control Protection  */
    idt_set_gate(29, eh, cs);   /* #VC VMM Communication   */
    idt_set_gate(30, eh, cs);   /* #SX Security Exception  */
}

static void idt_load(void) {
    idtr_t r = { (uint16_t)(sizeof(idt_table) - 1), (uint64_t)idt_table };
    __asm__ volatile("lidt %0" :: "m"(r) : "memory");
}

#endif /* IDT_H */
