#ifndef GDT_H
#define GDT_H

/*
 * src/gdt.h — descriptor tables, and the two selectors that make ring 3
 * possible.
 *
 * This kernel used to run on the table Limine left behind. That is
 * enough to execute code and take interrupts, and it is exactly not
 * enough to leave ring 0: the bootloader's table has no user code or
 * data descriptor, and no task state segment, so there is nothing for
 * IRET to load on the way down and nowhere for the processor to find a
 * kernel stack on the way back up.
 *
 * The order of the entries below is not a matter of taste. SYSRET does
 * not take selectors; it computes them, as IA32_STAR[63:48] + 8 for SS
 * and IA32_STAR[63:48] + 16 for CS. So user data must sit exactly eight
 * bytes after the SYSRET base and 64-bit user code exactly sixteen, or
 * the first return to user space loads a code segment as a stack and
 * faults. The 32-bit user code descriptor at the base is never loaded by
 * anything here; it exists to make that arithmetic land where it should,
 * which is also why Linux's table has one in the same place.
 */

#include <stdint.h>
#include "idt.h"
#include "pci.h"          /* serial_puts */

#define GDT_NULL     0x00
#define GDT_KCODE    0x08
#define GDT_KDATA    0x10
#define GDT_UCODE32  0x18        /* SYSRET base — never loaded directly */
#define GDT_UDATA    0x20
#define GDT_UCODE    0x28
#define GDT_TSS      0x30        /* 16 bytes: occupies 0x30 and 0x38 */

#define GDT_ENTRIES  8

/* Selectors as a user-mode thread sees them, RPL 3 included. */
#define SEL_UCODE    (GDT_UCODE | 3)
#define SEL_UDATA    (GDT_UDATA | 3)

/* What IA32_STAR wants: kernel CS in [47:32], SYSRET base in [63:48].
 * The base carries RPL 3 so that the selectors SYSRET derives from it
 * already describe a ring-3 load. */
#define STAR_SYSRET_BASE ((uint64_t)(GDT_UCODE32 | 3))
#define STAR_SYSCALL_CS  ((uint64_t)GDT_KCODE)

typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;          /* limit[19:16] in the low nibble, flags high */
    uint8_t  base_hi;
} __attribute__((packed)) gdt_entry_t;

/* A system descriptor — the TSS — is twice as wide in long mode, because
 * its base is 64 bits. It is written as two ordinary entries so the
 * table stays one flat array. */
typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

/*
 * The task state segment.
 *
 * Long mode threw away hardware task switching and kept this structure
 * for one reason: RSP0. When an interrupt or a SYSCALL-free trap arrives
 * while the processor is in ring 3, the stack it switches to comes from
 * here and nowhere else. The scheduler rewrites rsp0 on every context
 * switch, so the field is the single point where "which kernel stack"
 * is decided.
 *
 * ist[0] is the double-fault stack. A fault taken on a stack that is
 * itself unusable — a thread that overran its guard page, say — cannot
 * push an exception frame, and the processor's answer to that is a
 * triple fault and a reset with nothing on the wire. An IST entry gives
 * vector 8 a stack that is known good no matter what the faulting thread
 * did to its own.
 */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static gdt_entry_t gdt_table[GDT_ENTRIES] __attribute__((aligned(16)));
static tss_t       tss __attribute__((aligned(16)));

/*
 * Stacks the processor switches to on its own. None is ever the stack of
 * a running thread, so all can be static.
 *
 * Four interrupt stack table entries, because four exceptions can arrive
 * while the current stack is exactly what cannot be trusted:
 *
 *   IST1  #DF  a fault taken while handling a fault -- by definition the
 *              stack in hand did not work
 *   IST2  NMI  arrives at any instruction boundary including the middle
 *              of a stack switch, and cannot be masked to avoid it
 *   IST3  #SS  the stack segment itself faulted; pushing to it is what
 *              just failed
 *   IST4  #MC  a machine check may follow memory that is physically bad,
 *              and the report is the only thing of value left
 *
 * The first was already here. The other three were sharing the faulting
 * thread's stack, which means a stack overflow -- the single most common
 * way to get #SS -- produced a triple fault and a reset instead of a
 * report naming the thread that overflowed.
 */
#define KSTACK_SIZE  (32 * 1024)
#define IST_SIZE     (16 * 1024)
static uint8_t kernel_stack[KSTACK_SIZE] __attribute__((aligned(16)));
static uint8_t df_stack[IST_SIZE]        __attribute__((aligned(16)));
static uint8_t nmi_stack[IST_SIZE]       __attribute__((aligned(16)));
static uint8_t ss_stack[IST_SIZE]        __attribute__((aligned(16)));
static uint8_t mc_stack[IST_SIZE]        __attribute__((aligned(16)));

#define IST_DF  1
#define IST_NMI 2
#define IST_SS  3
#define IST_MC  4

static void gdt_set(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags) {
    gdt_table[i].limit_lo = (uint16_t)(limit & 0xFFFF);
    gdt_table[i].base_lo  = (uint16_t)(base & 0xFFFF);
    gdt_table[i].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt_table[i].access   = access;
    gdt_table[i].gran     = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt_table[i].base_hi  = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss(int i, uint64_t base, uint32_t limit) {
    gdt_tss_entry_t *e = (gdt_tss_entry_t *)&gdt_table[i];
    e->limit_lo   = (uint16_t)(limit & 0xFFFF);
    e->base_lo    = (uint16_t)(base & 0xFFFF);
    e->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    e->access     = 0x89;                 /* present, available 64-bit TSS */
    e->gran       = (uint8_t)((limit >> 16) & 0x0F);
    e->base_hi    = (uint8_t)((base >> 24) & 0xFF);
    e->base_upper = (uint32_t)(base >> 32);
    e->reserved   = 0;
}

/* Where the processor should land when a ring-3 thread traps. */
static inline void tss_set_rsp0(uint64_t rsp) { tss.rsp0 = rsp; }

/*
 * Install the table and start using it.
 *
 * Loading a GDT does not change CS — the selector the processor is
 * already running under keeps its cached descriptor until something
 * reloads it, and in long mode no instruction moves a value into CS. A
 * far return does: push the selector we want and an address, then let
 * LRETQ pop both. The data selectors follow with ordinary moves, and LTR
 * finally tells the processor where the task state segment is.
 */
static void gdt_init(void) {
    for (int i = 0; i < GDT_ENTRIES; i++) gdt_set(i, 0, 0, 0, 0);

    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xA0);   /* kernel code, L=1, DPL 0     */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC0);   /* kernel data,      DPL 0     */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xC0);   /* user code 32-bit, DPL 3     */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xC0);   /* user data,        DPL 3     */
    gdt_set(5, 0, 0xFFFFF, 0xFA, 0xA0);   /* user code 64-bit, DPL 3     */

    uint8_t *t = (uint8_t *)&tss;
    for (uint64_t i = 0; i < sizeof(tss); i++) t[i] = 0;
    tss.rsp0   = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
    tss.ist[IST_DF  - 1] = (uint64_t)(uintptr_t)(df_stack  + IST_SIZE);
    tss.ist[IST_NMI - 1] = (uint64_t)(uintptr_t)(nmi_stack + IST_SIZE);
    tss.ist[IST_SS  - 1] = (uint64_t)(uintptr_t)(ss_stack  + IST_SIZE);
    tss.ist[IST_MC  - 1] = (uint64_t)(uintptr_t)(mc_stack  + IST_SIZE);
    /* No I/O permission bitmap. Pointing the base past the segment limit
     * is how the architecture says "there isn't one", which denies every
     * port to ring 3 — IN and OUT from user space then raise #GP rather
     * than reading whatever byte followed the structure. */
    tss.iomap_base = (uint16_t)sizeof(tss);

    gdt_set_tss(6, (uint64_t)(uintptr_t)&tss, (uint32_t)(sizeof(tss) - 1));

    gdtr_t gdtr = { (uint16_t)(sizeof(gdt_table) - 1),
                    (uint64_t)(uintptr_t)gdt_table };
    __asm__ volatile(
        "lgdt %[g]\n\t"
        "pushq %[kcode]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %[kdata], %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %[tsssel], %%ax\n\t"
        "ltr %%ax\n"
        :
        : [g] "m"(gdtr),
          [kcode] "i"((uint64_t)GDT_KCODE),
          [kdata] "i"((uint16_t)GDT_KDATA),
          [tsssel] "i"((uint16_t)GDT_TSS)
        : "rax", "memory");

    serial_puts("[gdt] ring 0/3 descriptors installed, TSS loaded\n");
}

#endif /* GDT_H */
