#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include "kernel_shared.h"

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

/* ---- time budgets ----
 *
 * Several things in this kernel are too big to do in one frame and have to
 * be handed out in pieces: reading a 400 MB model off the disk, running a
 * transformer over a prompt, decompressing an archive. Each of them used
 * to be paced by a *count* — four chunks per frame, two layers per frame —
 * and a count is the wrong unit, for two reasons that pull in opposite
 * directions.
 *
 * Too many, and the frame blows its deadline. The model loader read four
 * 1 MB chunks per frame; on an emulated machine one of those chunks is
 * already twice a frame's worth of work, so the desktop ran at 1 fps for
 * the whole load and the pointer was unusable for a minute after login.
 *
 * Too few, and the work never finishes. The same pacing applied to
 * inference limited it to two layers per frame, which throttled a
 * three-second prompt evaluation to over two minutes on hardware that
 * could have done it immediately.
 *
 * A count cannot be right for both, because the correct count depends on
 * how fast the machine is — which is exactly what a count cannot express.
 * Time can. `tsc_budget_ms` lets a caller spend a stated slice of the
 * frame and stop, so the same code adapts from an emulator to bare metal
 * without a tuning constant anywhere.
 */
/*
 * The names are architecture-neutral because the code that uses them is
 * shared verbatim with the aarch64 tree, where the counter is a system
 * register rather than an instruction. Keeping apps.h and term.h
 * identical between the two is worth more than calling this rdtsc().
 */
static inline uint64_t cycle_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Zero until calibrated against the PIT; a budget test then always
 * expires, which degrades to exactly one unit of work per frame — slow,
 * but never a stall. */
static uint64_t cycles_per_ms = 0;

static inline int budget_expired_ms(uint64_t start, uint32_t ms) {
    if (!cycles_per_ms) return 1;
    return (cycle_now() - start) >= (uint64_t)ms * cycles_per_ms;
}

/* Microseconds, for spans a millisecond cannot resolve. */
static inline uint32_t cycles_to_us(uint64_t cycles) {
    uint64_t per_us = cycles_per_ms / 1000;
    return per_us ? (uint32_t)(cycles / per_us) : 0;
}

/* For reporting an elapsed span; 0 if the counter was never calibrated. */
static inline uint32_t cycles_to_ms(uint64_t cycles) {
    return cycles_per_ms ? (uint32_t)(cycles / cycles_per_ms) : 0;
}

/* ---- MSR access helpers ----
 * rdmsr and wrmsr moved to include/kernel_shared.h: scheduler.o needs
 * them and they are pure instructions with no state to share. */

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

/*
 * The general form. Two fields beyond the address matter once ring 3
 * exists:
 *
 *   dpl  — the privilege a caller needs to reach the vector through INT.
 *          Every hardware vector stays at 0, because a user program that
 *          can forge a page fault can convince the kernel it faulted on
 *          an address it never touched. The syscall gate is the sole
 *          exception and must be 3, or `int 0x80` from user space raises
 *          #GP instead of entering the kernel.
 *
 *   ist  — an index, 1-7, into the TSS's interrupt stack table, or 0 for
 *          "keep using whatever stack we are on". Used for the double
 *          fault, where the stack in hand is exactly what cannot be
 *          trusted.
 */
void idt_set_gate_ex(int vec, void *fn, uint16_t sel,
                            uint8_t ist, uint8_t dpl) {
    uint64_t addr = (uint64_t)(uintptr_t)fn;
    idt_table[vec].offset_lo  = (uint16_t)(addr & 0xFFFF);
    idt_table[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt_table[vec].offset_hi  = (uint32_t)(addr >> 32);
    idt_table[vec].selector   = sel;
    idt_table[vec].ist        = (uint8_t)(ist & 7);
    idt_table[vec].type_attr  = (uint8_t)(0x8E | ((dpl & 3) << 5));
    idt_table[vec].reserved   = 0;
}

static void idt_set_gate(int vec, void (*fn)(interrupt_frame_t *), uint16_t sel) {
    idt_set_gate_ex(vec, (void *)(uintptr_t)fn, sel, 0, 0);
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

/*
 * ---- Catch-all: silently return from any unexpected vector ----
 *
 * target("general-regs-only") on every handler in this kernel, and it is
 * not optional now that the rest of it is compiled with SSE available.
 * GCC's interrupt attribute saves and restores the general-purpose
 * registers; it does not save XMM, so a handler that used one would hand
 * the interrupted thread back a register it never wrote. Threads carry
 * their own extended state through the scheduler — handlers simply do
 * not get to touch it.
 */
__attribute__((interrupt, target("general-regs-only")))
static void isr_noop(interrupt_frame_t *f) { (void)f; }

/*
 * Fill all 256 gates with the no-op stub, and remap the PIC.
 *
 * The first thirty-two vectors are overwritten immediately afterwards by
 * trap_install() in trap.h, which is where the exceptions are actually
 * handled — a page fault has to be diagnosed and a faulting thread has
 * to be killed, and neither is something a no-op can do. What this
 * leaves behind is the floor: every vector points somewhere valid, so an
 * interrupt nothing claimed returns instead of triple-faulting.
 */
static void idt_init(uint16_t cs) {
    pic_remap();
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, isr_noop, cs);
}

static void idt_load(void) {
    idtr_t r = { (uint16_t)(sizeof(idt_table) - 1), (uint64_t)idt_table };
    __asm__ volatile("lidt %0" :: "m"(r) : "memory");
}

#endif /* IDT_H */
