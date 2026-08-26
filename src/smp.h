#ifndef SMP_H
#define SMP_H

/*
 * src/smp.h — the other processors.
 *
 * This machine has been running on one core since it first booted. Not
 * because the others were hidden: acpi.h has parsed the MADT since the
 * firmware tables were first read, and `acpi.cpu[]` has held every
 * processor's APIC identifier, its package, its core and whether it is a
 * hyperthread, all correctly, all along. What was missing was the part
 * that is not discovery — actually starting them.
 *
 * ---- how a processor is started, and why it is this strange ----
 *
 * An application processor comes out of firmware halted in a state the
 * architecture calls "wait-for-SIPI". It is not running code; it is not
 * even in a mode that could run this kernel's code. Waking it is a
 * sequence of inter-processor interrupts sent through the boot
 * processor's local APIC:
 *
 *     INIT           put it into a known reset state
 *     wait 10 ms     the interval the specification asks for
 *     STARTUP        with a vector that *is* an address
 *     wait 200 us
 *     STARTUP        again, because the first is permitted to be lost
 *
 * The second startup is not superstition. Intel's own multiprocessor
 * initialisation algorithm sends two, and a processor that has already
 * started ignores the second, so the cost of sending it is nothing and
 * the cost of not sending it is a core that sometimes does not come up.
 *
 * The vector in a STARTUP message is eight bits and the target begins
 * executing at `vector << 12`, in *real mode*, with CS set to match. So
 * the first instruction an application processor executes in this
 * kernel has to live in the first megabyte of physical memory, on a page
 * boundary, and has to be sixteen-bit code. There is no way to ask for
 * anything else; the encoding cannot express it.
 *
 * What follows below is therefore a small program that exists only to
 * climb: real mode to protected mode to long mode, in that order,
 * because each is only reachable from the one before it. It is copied
 * into a page under a megabyte at boot and every processor runs the same
 * copy, one at a time.
 *
 * ---- what an application processor is allowed to do here ----
 *
 * It runs kernel code, in ring 0, and nothing else. It does not run
 * threads, take system calls, or touch the scheduler's tables.
 *
 * That is a deliberate limit and it is worth being precise about why,
 * because the alternative looks tempting and would corrupt this kernel
 * quietly. Several things that are correct on one processor are simply
 * wrong on two:
 *
 *   syscall_user_rsp_slot is a single global that the SYSCALL entry stub
 *     parks the caller's stack pointer in. Two processors entering the
 *     kernel at once would overwrite each other's, and the second to
 *     leave would return onto the first one's stack.
 *
 *   syscall_cur_frame and syscall_kstack are single globals for the same
 *     reason, and fork reads the first of them.
 *
 *   there is one TSS, and tss.rsp0 says which kernel stack a trap from
 *     ring 3 lands on. One value cannot be right for two processors
 *     running two different threads.
 *
 *   sched_block_on_locked closes the lost-wakeup race by disabling
 *     interrupts. That is airtight with one processor and no protection
 *     at all with two, because the waker is not on this one.
 *
 * Every one of those is fixable and none of them is fixable *here*. So
 * the boundary is drawn where it can be drawn honestly: the application
 * processors are a pool of kernel workers, they are handed bounded
 * compute that touches only memory the submitter owns, and the desktop
 * keeps the boot processor to itself. That is exactly the shape of the
 * problem the specification names — inference on the auxiliary cores,
 * core zero clear for the interface — and it is reached without
 * pretending the rest of the kernel is ready for symmetric execution.
 *
 * ---- the include rule ----
 *
 * This file owns mutable static state — per-processor blocks, the job
 * queue, the trampoline's data area — and reaches into src/apic.h for
 * the interrupt command register. It must therefore appear in exactly
 * one translation unit's include closure, which is src/core/main.c's.
 * See the invariant at the top of include/kernel_shared.h; what the
 * scheduler needs from here crosses through that header and not through
 * this one.
 */

#include <stdint.h>
#include "kernel_shared.h"
#include "apic.h"
#include "acpi.h"

/*
 * How many processors this kernel will actually use.
 *
 * ACPI is allowed to report sixty-four and the tables are read in full;
 * this is the number that get a stack, a descriptor table and a place in
 * the work queue. Sixteen is well past any machine this is likely to
 * boot on and each one costs a kernel stack, so the bound is here rather
 * than in the parser.
 */
#define SMP_MAX_CPUS   16
#define SMP_AP_STACK   (32 * 1024)
#define SMP_AP_EMERG   (16 * 1024)

/* ===================================================================
 * 1. THE TRAMPOLINE
 * ===================================================================
 *
 * Sixteen-bit code that turns into thirty-two-bit code that turns into
 * sixty-four-bit code, in about sixty instructions.
 *
 * The awkward part is addressing. This runs at a physical address that
 * is not known until boot, and the two far jumps it has to make take
 * *absolute* addresses — so a blob assembled at one address and copied
 * to another would jump into whatever happens to be at the address it
 * was linked for.
 *
 * The way out is that the processor already knows where it is. A STARTUP
 * message leaves CS holding the page number, so `CS << 4` is the linear
 * address of the first byte of this code, and every absolute address the
 * blob needs is that plus a fixed offset. The two far jumps are
 * therefore *indirect*, through a pair of words in the data area that
 * the code computes and fills in for itself just before using them. That
 * keeps every write to data rather than to instructions, which is worth
 * something on a processor whose prefetch queue has already read past
 * where a patch would land.
 *
 * Everything else — the page tables to load, the stack to run on, the
 * function to call — is written into the data area by the boot processor
 * before the message is sent.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".balign 16\n"
    ".globl smp_tramp_start\n"
    "smp_tramp_start:\n"
    ".code16\n"
    "  cli\n"
    "  cld\n"
    /* DS = CS, so a bare offset within this page addresses this page. */
    "  movw %cs, %ax\n"
    "  movw %ax, %ds\n"
    /* EBX = the linear address of smp_tramp_start, for the rest of the
     * climb. It survives into protected and long mode untouched, which
     * is what lets the later stages find the data area. */
    "  xorl %ebx, %ebx\n"
    "  movw %cs, %bx\n"
    "  shll $4, %ebx\n"

    /* The GDT pointer's base field: this page's linear address plus the
     * table's offset within it. */
    "  movl %ebx, %eax\n"
    "  addl $(smp_tramp_gdt - smp_tramp_start), %eax\n"
    "  movw $(smp_tramp_gdtr - smp_tramp_start + 2), %si\n"
    "  movl %eax, (%si)\n"
    "  movw $(smp_tramp_gdtr - smp_tramp_start), %si\n"
    "  lgdtl (%si)\n"

    /* Where the thirty-two-bit half begins, written into the indirect
     * far-jump slot before the jump reads it. */
    "  movl %ebx, %eax\n"
    "  addl $(smp_pm32 - smp_tramp_start), %eax\n"
    "  movw $(smp_far32 - smp_tramp_start), %si\n"
    "  movl %eax, (%si)\n"

    "  movl %cr0, %eax\n"
    "  orl $1, %eax\n"
    "  movl %eax, %cr0\n"
    "  ljmpl *(%si)\n"

    ".code32\n"
    "smp_pm32:\n"
    "  movw $0x10, %ax\n"
    "  movw %ax, %ds\n"
    "  movw %ax, %es\n"
    "  movw %ax, %fs\n"
    "  movw %ax, %gs\n"
    "  movw %ax, %ss\n"

    /* Physical address extension, without which long mode cannot be
     * entered at all -- IA32_EFER.LME is simply refused while CR4.PAE is
     * clear, and the refusal is silent. */
    "  movl %cr4, %eax\n"
    "  orl $(1 << 5), %eax\n"
    "  movl %eax, %cr4\n"

    /* The kernel's own page tables, which the boot processor left here.
     * The trampoline's page is identity-mapped in them for exactly the
     * next three instructions -- see smp_boot. */
    "  movl (smp_data_cr3 - smp_tramp_start)(%ebx), %eax\n"
    "  movl %eax, %cr3\n"

    /* LME turns long mode on; NXE is what makes bit 63 of a page table
     * entry mean no-execute rather than reserved, and this kernel's
     * mappings use it -- a processor without it set faults on the first
     * instruction fetch from any page the loader marked. SCE follows the
     * boot processor's setting so the two agree. */
    "  movl $0xC0000080, %ecx\n"
    "  rdmsr\n"
    "  orl $((1 << 8) | (1 << 11) | (1 << 0)), %eax\n"
    "  wrmsr\n"

    "  movl %cr0, %eax\n"
    "  orl $(1 << 31), %eax\n"
    "  movl %eax, %cr0\n"

    "  leal (smp_lm64 - smp_tramp_start)(%ebx), %ecx\n"
    "  leal (smp_far64 - smp_tramp_start)(%ebx), %eax\n"
    "  movl %ecx, (%eax)\n"
    "  ljmpl *(%eax)\n"

    ".code64\n"
    "smp_lm64:\n"
    /*
     * Zero-extend the page base before using it as one.
     *
     * EBX has held this page's linear address since the sixteen-bit
     * stage, and it still does -- but only the low half of it. The upper
     * thirty-two bits of a general register are *architecturally
     * undefined* across the transition out of legacy mode: nothing in
     * protected mode can write them and nothing promises what long mode
     * finds there. A thirty-two bit write in sixty-four bit mode zeroes
     * the upper half, which is precisely what this instruction is for
     * and why it looks like it does nothing.
     *
     * Leaving it out is not a subtle bug. The next instruction reads a
     * stack pointer from a non-canonical address, which faults with no
     * interrupt table loaded -- the IDTR is still whatever reset left --
     * and a fault that cannot be delivered is a triple fault. The
     * machine resets, silently, halfway through bringing up its second
     * processor, and the boot log simply starts again.
     */
    "  movl %ebx, %ebx\n"
    "  movq (smp_data_stack - smp_tramp_start)(%rbx), %rsp\n"
    "  movq (smp_data_entry - smp_tramp_start)(%rbx), %rax\n"
    /* The argument: which processor this is, decided by the boot
     * processor rather than derived here, so that the index and the
     * table entry cannot disagree. */
    "  movl (smp_data_index - smp_tramp_start)(%rbx), %edi\n"
    /* Say so before calling, so that a fault inside the entry point is
     * distinguishable from never having arrived. */
    "  movl $1, (smp_data_flag - smp_tramp_start)(%rbx)\n"
    "  xorq %rbp, %rbp\n"
    "  callq *%rax\n"
    "smp_lm64_halt:\n"
    "  cli\n"
    "  hlt\n"
    "  jmp smp_lm64_halt\n"

    /* ---- the data area ---- */
    ".balign 16\n"
    ".globl smp_tramp_gdt\n"
    "smp_tramp_gdt:\n"
    "  .quad 0x0000000000000000\n"      /* null                          */
    "  .quad 0x00CF9A000000FFFF\n"      /* 0x08  32-bit code, DPL 0      */
    "  .quad 0x00CF92000000FFFF\n"      /* 0x10  32-bit data, DPL 0      */
    "  .quad 0x00AF9A000000FFFF\n"      /* 0x18  64-bit code, DPL 0      */
    "  .quad 0x00AF92000000FFFF\n"      /* 0x20  64-bit data, DPL 0      */
    ".globl smp_tramp_gdtr\n"
    "smp_tramp_gdtr:\n"
    "  .word 39\n"                      /* five descriptors, less one    */
    "  .long 0\n"                       /* base: filled in by the code   */
    ".balign 8\n"
    ".globl smp_far32\n"
    "smp_far32:\n"
    "  .long 0\n"
    "  .word 0x08\n"
    ".balign 8\n"
    ".globl smp_far64\n"
    "smp_far64:\n"
    "  .long 0\n"
    "  .word 0x18\n"
    ".balign 8\n"
    ".globl smp_data_cr3\n"   "smp_data_cr3:   .quad 0\n"
    ".globl smp_data_stack\n" "smp_data_stack: .quad 0\n"
    ".globl smp_data_entry\n" "smp_data_entry: .quad 0\n"
    ".globl smp_data_index\n" "smp_data_index: .long 0\n"
    ".globl smp_data_flag\n"  "smp_data_flag:  .long 0\n"
    ".globl smp_tramp_end\n"
    "smp_tramp_end:\n"
    ".code64\n"
    ".popsection\n"
);

extern uint8_t smp_tramp_start[], smp_tramp_end[];
extern uint8_t smp_data_cr3[], smp_data_stack[], smp_data_entry[];
extern uint8_t smp_data_index[], smp_data_flag[];

/* Where a symbol in the blob lands once the blob has been copied. The
 * difference of two labels in one section is a link-time constant, which
 * is what keeps this free of relocations the position-independent link
 * would refuse. */
#define SMP_TOFF(sym) ((uint64_t)((sym) - smp_tramp_start))

/* ===================================================================
 * 2. PER-PROCESSOR STATE
 * =================================================================== */

/*
 * Each application processor gets its own descriptor table and its own
 * task state segment, and the reason is one bit in a descriptor.
 *
 * A TSS descriptor has a "busy" flag that LTR sets. Two processors
 * loading the *same* descriptor means the second finds it already busy
 * and takes a general protection fault at the instruction that was
 * supposed to make it able to handle faults. So the table is copied per
 * processor and the copy's TSS descriptor names that processor's own
 * structure.
 *
 * An application processor never leaves ring 0, so rsp0 is never
 * consulted and is filled in only for completeness. What is genuinely
 * needed is the interrupt stack table: a double fault, a non-maskable
 * interrupt, a stack fault or a machine check arrives on a stack that is
 * by definition not to be trusted, and without an IST entry the
 * processor tries to push onto it anyway and triple faults. One
 * emergency stack per processor serves all four, which is a compromise
 * -- two of them nesting would overwrite the first one's frame -- and
 * the alternative is four times the memory to improve a case in which
 * the machine is already lost. What the single stack buys is a legible
 * panic instead of a silent reset, which is the whole value of an IST
 * here.
 */
typedef struct {
    uint32_t apic_id;
    /*
     * Volatile, and it is load-bearing rather than decorative.
     *
     * This is written by the processor it describes and read by the one
     * that started it, in a loop that does nothing else. Without the
     * qualifier the compiler is entitled to notice that the loop body
     * cannot change the value, hoist the load out, and spin on a
     * register — so the wait always runs to its full deadline and always
     * reports failure, for a processor that came up correctly and is
     * sitting there waiting for work.
     *
     * That is exactly what happened, and it is the same mistake
     * include/kernel_shared.h records against sched_ticks, from the same
     * cause and with the same shape: a value written by something the
     * compiler cannot see.
     */
    volatile int online;
    int      is_hyperthread;
    uint64_t stack_top;
    uint64_t emerg_top;
    volatile uint64_t chunks;      /* work items this processor retired */
    volatile uint64_t wakes;       /* times it was pulled out of HLT    */
    volatile int      parked;      /* in HLT, waiting for a wake IPI    */
} smp_cpu_t;

static smp_cpu_t   smp_cpu[SMP_MAX_CPUS];
static int         smp_cpu_count = 1;    /* the boot processor is index 0 */
static int         smp_online    = 1;
static const char *smp_status    = "not started";

static gdt_entry_t smp_ap_gdt[SMP_MAX_CPUS][GDT_ENTRIES]
                       __attribute__((aligned(16)));
static tss_t       smp_ap_tss[SMP_MAX_CPUS] __attribute__((aligned(16)));

/*
 * There is deliberately no smp_this_cpu() here.
 *
 * The obvious way to answer "which processor am I" is to read the local
 * APIC's own identifier and look it up, and that works -- but nothing
 * below needs to ask. A worker is told its index once, by the processor
 * that started it, through the trampoline's data area, and carries it as
 * an argument from there. An index that is passed cannot disagree with
 * the table entry it names; an index that is derived can, the first time
 * the firmware reports an APIC identifier that is not the one the table
 * was built from.
 */

/* ===================================================================
 * 3. THE WORK QUEUE
 * ===================================================================
 *
 * One job at a time, split into chunks, claimed by whoever is free.
 *
 * The shape is a parallel loop rather than a general task queue, and
 * that is a decision about what the auxiliary cores are for. The work
 * that actually wants them here is a matrix multiplication: thousands of
 * output rows, each independent of every other, each reading shared
 * constant weights and writing one element of an output array. There is
 * no ordering between chunks, no allocation inside them, and no shared
 * mutable state at all — which is exactly why it is safe to run on a
 * processor that has no scheduler, no address space of its own and no
 * business taking a lock.
 *
 * Claiming is a single atomic increment on the chunk cursor, so no
 * processor ever waits for another to hand it work: the queue is
 * lock-free in the strict sense, and the only spinlock in this file
 * guards the *submission* path, where one job must finish before the
 * next is posted.
 *
 * Chunks rather than a row each, because an atomic increment that is
 * contended by eight processors for every row costs more than the row.
 */
typedef void (*smp_work_fn)(void *ctx, uint32_t first, uint32_t last);

static struct {
    smp_work_fn       fn;
    void             *ctx;
    uint32_t          items;
    uint32_t          chunk;         /* items per claim                  */
    uint32_t          chunks;        /* how many claims there are        */
    volatile uint32_t claimed;       /* next unclaimed, atomically bumped */
    volatile uint32_t retired;       /* chunks finished                  */
    volatile uint32_t epoch;         /* bumped per job; workers watch it */
} smp_job;

/* Taken with an atomic exchange rather than a lock; see the note in
 * smp_parallel_for about why a second caller runs its own work instead
 * of waiting for the first. */
static volatile int smp_job_busy = 0;

/*
 * Take chunks until there are none left.
 *
 * Run by the boot processor and by every worker, which is what makes the
 * submitter a participant rather than a spectator: on a machine that
 * reports two cores there is one worker, and a scheme where the
 * submitter only waited would leave half the machine idle.
 */
static void smp_job_drain(int cpu) {
    for (;;) {
        uint32_t c = __atomic_fetch_add(&smp_job.claimed, 1u,
                                        __ATOMIC_ACQ_REL);
        if (c >= smp_job.chunks) return;

        uint32_t first = c * smp_job.chunk;
        uint32_t last  = first + smp_job.chunk;
        if (last > smp_job.items) last = smp_job.items;

        smp_job.fn(smp_job.ctx, first, last);

        if (cpu >= 0 && cpu < SMP_MAX_CPUS) smp_cpu[cpu].chunks++;
        __atomic_add_fetch(&smp_job.retired, 1u, __ATOMIC_ACQ_REL);
    }
}

/* ===================================================================
 * 4. WHERE AN APPLICATION PROCESSOR LIVES
 * =================================================================== */

/*
 * The wake vector.
 *
 * A worker with nothing to do halts, because a processor spinning on a
 * flag is a processor drawing full power and, on a machine that is
 * itself a guest, one that is stealing a whole host core to do nothing.
 * Halting means it has to be woken by an interrupt, which means an
 * interrupt table, a vector and something in the vector that
 * acknowledges and returns. This is that something: it does nothing on
 * purpose. The wake is the arrival, not the handler.
 */
__attribute__((interrupt, target("general-regs-only")))
static void smp_wake_isr(interrupt_frame_t *f) {
    (void)f;
    lapic_eoi();
}

/*
 * The idle loop, and everything an application processor ever runs.
 *
 * It watches the job epoch rather than the chunk cursor: a job with
 * fewer chunks than there are processors would otherwise leave the ones
 * that found nothing spinning through the whole job. Waiting on the
 * epoch means a worker wakes once per job, takes what is left, and goes
 * back to sleep.
 *
 * STI before HLT and the order is not negotiable -- the two are a single
 * instruction pair to the processor precisely so that a wake arriving
 * between them cannot be lost. Written the other way round, a job posted
 * in that window would find the worker about to halt with nothing left
 * to wake it.
 */
static void smp_ap_idle(int cpu) {
    uint32_t seen = smp_job.epoch;

    for (;;) {
        uint32_t now = __atomic_load_n(&smp_job.epoch, __ATOMIC_ACQUIRE);
        if (now != seen) {
            seen = now;
            smp_job_drain(cpu);
            continue;
        }
        smp_cpu[cpu].parked = 1;
        __asm__ volatile("sti; hlt" ::: "memory");
        smp_cpu[cpu].parked = 0;
        smp_cpu[cpu].wakes++;
    }
}

/*
 * The first C this processor executes.
 *
 * Everything here is per-processor state that the boot processor's own
 * initialisation could not have done on its behalf: its descriptor
 * tables, its floating-point unit, its local APIC. The order matters
 * only in that the interrupt table must be in place before interrupts
 * are enabled, which happens in the idle loop and not here.
 */
static void fpu_init(void);          /* src/core/main.c, above this      */

static void smp_ap_main(int cpu) {
    /*
     * The extended state unit, before anything else at all — and this
     * is not where it would naturally go.
     *
     * It comes out of reset with CR0.EM set, which makes every SSE
     * instruction an invalid opcode, and this processor has no interrupt
     * table yet, so an invalid opcode is a triple fault and a silent
     * machine reset. The trap is that nothing below *looks* like it uses
     * SSE: the two loops that follow copy a descriptor table and zero a
     * task state segment, in plain C, a structure and a byte at a time.
     * GCC vectorises both. The first instruction of the first loop
     * becomes `pxor %xmm0, %xmm0`, and the machine is gone before it has
     * said anything.
     *
     * kmain has the identical note at its own first line, for the
     * identical reason, found the identical way. It is the same lesson
     * and it has to be learned once per processor.
     */
    fpu_init();

    /* Its own descriptor table, with its own task state segment in it.
     * A copy of the boot processor's, because everything except the TSS
     * descriptor is identical and a second definition would be a second
     * thing to keep in step. */
    for (int i = 0; i < GDT_ENTRIES; i++)
        smp_ap_gdt[cpu][i] = gdt_table[i];

    tss_t *t = &smp_ap_tss[cpu];
    for (uint64_t i = 0; i < sizeof(tss_t); i++) ((uint8_t *)t)[i] = 0;
    t->rsp0 = smp_cpu[cpu].stack_top;
    for (int i = 0; i < 4; i++) t->ist[i] = smp_cpu[cpu].emerg_top;
    t->iomap_base = (uint16_t)sizeof(tss_t);

    {
        gdt_tss_entry_t *e = (gdt_tss_entry_t *)&smp_ap_gdt[cpu][6];
        uint64_t base = (uint64_t)(uintptr_t)t;
        uint32_t limit = (uint32_t)(sizeof(tss_t) - 1);
        e->limit_lo   = (uint16_t)(limit & 0xFFFF);
        e->base_lo    = (uint16_t)(base & 0xFFFF);
        e->base_mid   = (uint8_t)((base >> 16) & 0xFF);
        e->access     = 0x89;
        e->gran       = (uint8_t)((limit >> 16) & 0x0F);
        e->base_hi    = (uint8_t)((base >> 24) & 0xFF);
        e->base_upper = (uint32_t)(base >> 32);
        e->reserved   = 0;
    }

    {
        gdtr_t g = { (uint16_t)(sizeof(smp_ap_gdt[cpu]) - 1),
                     (uint64_t)(uintptr_t)smp_ap_gdt[cpu] };
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
            "mov %[tsssel], %%ax\n\t"
            "ltr %%ax\n"
            :
            : [g] "m"(g),
              [kcode] "i"((uint64_t)GDT_KCODE),
              [kdata] "i"((uint16_t)GDT_KDATA),
              [tsssel] "i"((uint16_t)GDT_TSS)
            : "rax", "memory");
    }

    /* The same interrupt table the boot processor uses. Sharing it is
     * correct and intended: a vector means the same thing on every
     * processor, and the only entry an application processor ever takes
     * is the wake. */
    idt_load();

    lapic_init_ap();

    smp_cpu[cpu].online = 1;
    __atomic_add_fetch(&smp_online, 1, __ATOMIC_ACQ_REL);

    smp_ap_idle(cpu);
}

/* The trampoline calls this with the processor index in EDI. Not static:
 * its address is written into the trampoline's data area, and a function
 * whose address is taken from assembly has to survive into the object. */
__attribute__((used))
void smp_ap_entry(int cpu);

void smp_ap_entry(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) for (;;) __asm__ volatile("hlt");
    smp_ap_main(cpu);
}

/* ===================================================================
 * 5. BRINGING THEM UP
 * =================================================================== */

/*
 * Somewhere under a megabyte to put the trampoline.
 *
 * The constraint is the STARTUP message's, not ours: the vector is eight
 * bits of a page number, so the address has to be page-aligned and below
 * 0x100000. There is exactly one way to get such a page here, and it is
 * not the obvious one.
 *
 * pmm_alloc cannot be asked, because the frame allocator deliberately
 * never hands out the first megabyte at all -- it marks every frame
 * below it used at init, on the grounds that the real-mode interrupt
 * vector table, the BIOS data area and the VGA window live down there
 * and a driver that pokes one of them should find it rather than a page
 * table. So every low frame reads as busy no matter what the memory map
 * says, and a bitmap test is guaranteed to reject all of them.
 *
 * That same reservation is what makes taking one safe. A page that the
 * allocator will never give to anybody cannot be given to anybody else
 * while these processors are executing out of it; there is nothing to
 * claim it *from*. What the map is still consulted for is which low
 * pages are memory at all, since the ones that are not -- the video
 * window, the option ROMs, the BIOS itself -- are not marked usable.
 *
 * The first two pages are skipped whatever the map says about them: the
 * vector table and the BIOS data area are real structures at real
 * addresses, and a null pointer dereference that lands on a mapped page
 * is a bug that does not fault.
 *
 * ---- and the page the frame allocator is standing on ----
 *
 * "Usable" is what the firmware thought before this kernel started. It
 * is not the same question as "unused now", and the difference has one
 * concrete instance that this got wrong: the frame bitmap itself. It is
 * placed by hand at boot, in the first usable region large enough to
 * hold it, and on a machine with a couple of gigabytes of memory it is
 * sixty kilobytes -- which fits comfortably in conventional low memory
 * and is therefore put there. The map still says those pages are usable,
 * because the map has no idea.
 *
 * Overlapping it is not a crash. It is far worse than a crash: the
 * bitmap is written on every allocation and every free, so the
 * trampoline's *instructions* change underneath the processor executing
 * them. What that looked like was a machine that reset in a loop with
 * an invalid-opcode fault on `mov %eax, %cr3` -- an instruction which
 * cannot raise one, because by the time it executed the byte selecting
 * CR3 had been overwritten by a word of allocation bitmap and it was no
 * longer that instruction at all.
 *
 * So the bitmap's extent is excluded explicitly, and the search runs
 * from the top of low memory downward rather than the bottom up. Either
 * would be correct on its own; the exclusion is the part that is
 * *known* to be correct, and starting at the far end is what keeps this
 * away from wherever the next thing to want low memory decides to sit.
 */
#define SMP_LOW_FLOOR 0x2000ULL

static int smp_page_is_free(uint64_t p) {
    uint64_t b0 = pmm_bitmap_phys;
    uint64_t b1 = b0 + pmm_bitmap_words * 8;
    if (b0 && p < b1 && p + PAGE_SIZE > b0) return 0;
    return 1;
}

static uint64_t smp_low_page(struct limine_memmap_response *mm) {
    uint64_t best = 0;
    if (!mm) return 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = PAGE_ALIGN_UP(e->base);
        uint64_t end  = (e->base + e->length) & ~PAGE_MASK;
        if (base < SMP_LOW_FLOOR) base = SMP_LOW_FLOOR;
        if (end > 0x100000ULL) end = 0x100000ULL;

        for (uint64_t p = base; p + PAGE_SIZE <= end; p += PAGE_SIZE)
            if (smp_page_is_free(p) && p > best) best = p;
    }
    return best;
}

static uint64_t smp_tramp_phys = 0;

static void smp_write64(uint64_t off, uint64_t v) {
    *(volatile uint64_t *)(uintptr_t)phys_to_virt(smp_tramp_phys + off) = v;
}
static void smp_write32(uint64_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)phys_to_virt(smp_tramp_phys + off) = v;
}
static uint32_t smp_read32(uint64_t off) {
    return *(volatile uint32_t *)(uintptr_t)phys_to_virt(smp_tramp_phys + off);
}

/* Spin for a while, measured against the time stamp counter, which
 * tsc_calibrate has already pinned to the PIT. The intervals the startup
 * sequence asks for are ten milliseconds and two hundred microseconds,
 * and neither can be waited out with the scheduler -- this runs before
 * anything else is allowed on the machine and with the other processor
 * already halfway through resetting. */
static void smp_delay_us(uint32_t us) {
    if (!cycles_per_ms) {
        for (volatile uint32_t i = 0; i < us * 400u; i++) { }
        return;
    }
    uint64_t want  = (cycles_per_ms * us) / 1000u;
    uint64_t start = cycle_now();
    while (cycle_now() - start < want) __asm__ volatile("pause");
}

/*
 * INIT, then STARTUP twice.
 *
 * Three outcomes, and the difference between the last two is what
 * decides whether it is safe to start anything else:
 *
 *    1  the processor is up and initialised
 *    0  it acknowledged the trampoline but has not finished; it has
 *       already read the data area, so the area may be reprogrammed
 *   -1  it never acknowledged at all
 *
 * The flag the trampoline sets is written immediately before it calls
 * into C, which is *after* it has copied the stack pointer, the entry
 * point and its own index out of the data area. That ordering is the
 * whole reason the flag exists: there is one data area shared by every
 * processor, and overwriting it for the next one while a previous one
 * has yet to read it would hand two processors the same kernel stack.
 * Once the flag is up, the area is free.
 *
 * A processor that never acknowledged might still be about to. There is
 * no way to withdraw a startup message, so the only safe response is to
 * stop starting processors and leave its stack allocated — a few tens of
 * kilobytes given up in exchange for never having a late arrival run on
 * memory that has since been handed to somebody else.
 */
static int smp_start_one(int idx) {
    const uint32_t apic = smp_cpu[idx].apic_id;
    const uint8_t  vec  = (uint8_t)(smp_tramp_phys >> 12);

    smp_write64(SMP_TOFF(smp_data_stack), smp_cpu[idx].stack_top);
    smp_write32(SMP_TOFF(smp_data_index), (uint32_t)idx);
    smp_write32(SMP_TOFF(smp_data_flag),  0);
    __asm__ volatile("mfence" ::: "memory");

    if (lapic_send_init(apic) != 0) return -1;
    smp_delay_us(10000);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (lapic_send_startup(apic, vec) != 0) return -1;
        smp_delay_us(200);
        for (int i = 0; i < 200; i++) {
            if (smp_read32(SMP_TOFF(smp_data_flag))) {
                for (int k = 0; k < 2000; k++) {
                    if (smp_cpu[idx].online) return 1;
                    smp_delay_us(500);
                }
                return 0;               /* arrived, still initialising */
            }
            smp_delay_us(500);
        }
    }
    return -1;
}

/*
 * Discover, start, and report.
 *
 * The processor list comes from acpi.h, which has had it since the
 * firmware tables were read; what is chosen here is which of them to
 * use. One worker per *physical core* rather than per thread: two
 * hyperthreads share one set of execution units, so a second worker on
 * the same core roughly halves the throughput of the first and adds
 * nothing. acpi_topology() has already worked out which is which.
 */
static void smp_init(struct limine_memmap_response *mm) {
    /* Index zero is this processor, whatever the tables say the order
     * is. Everything else is described relative to it. */
    smp_cpu[0].apic_id = lapic_id();
    smp_cpu[0].online  = 1;
    smp_cpu_count      = 1;

    if (!lapic_present) {
        smp_status = "no local APIC: one processor";
        serial_puts("[smp] no local APIC; staying on one processor\n");
        return;
    }
    if (acpi.cpu_count <= 1) {
        smp_status = "the firmware reports one processor";
        serial_puts("[smp] ACPI reports a single processor\n");
        return;
    }

    if ((uint64_t)(smp_tramp_end - smp_tramp_start) > PAGE_SIZE) {
        smp_status = "the trampoline does not fit in a page";
        serial_puts("[smp] trampoline larger than a page; not starting\n");
        return;
    }

    /* The vector a parked worker is woken on, installed before any
     * processor is in a position to take it. Interrupt gate at DPL 0:
     * nothing in ring 3 has any business raising it. */
    idt_set_gate_ex(APIC_VEC_WAKE, (void *)(uintptr_t)smp_wake_isr,
                    GDT_KCODE, 0, 0);

    smp_tramp_phys = smp_low_page(mm);
    if (!smp_tramp_phys) {
        smp_status = "no page below 1 MB for the trampoline";
        serial_puts("[smp] no usable page under a megabyte\n");
        return;
    }

    /*
     * The trampoline has to be executable at its own physical address,
     * because that is where the STARTUP message sends the processor and
     * the first thing it does after loading CR3 is fetch the next
     * instruction from there. The kernel's page tables have no reason to
     * map the first megabyte, so the mapping is made here and taken away
     * again once every processor is past it.
     */
    if (vmm_map(&vmm_kernel_as, smp_tramp_phys, smp_tramp_phys,
                PTE_PRESENT | PTE_WRITE) != 0) {
        smp_status = "could not identity-map the trampoline";
        serial_puts("[smp] could not identity-map the trampoline page\n");
        return;
    }

    {
        uint8_t *dst = (uint8_t *)(uintptr_t)phys_to_virt(smp_tramp_phys);
        uint64_t n = (uint64_t)(smp_tramp_end - smp_tramp_start);
        for (uint64_t i = 0; i < n; i++) dst[i] = smp_tramp_start[i];
    }
    smp_write64(SMP_TOFF(smp_data_cr3),   vmm_kernel_pml4_phys);
    smp_write64(SMP_TOFF(smp_data_entry),
                (uint64_t)(uintptr_t)smp_ap_entry);

    serial_puts("[smp] trampoline at ");
    serial_put_hex32((uint32_t)smp_tramp_phys);
    serial_puts(", ");
    serial_put_dec((uint32_t)(smp_tramp_end - smp_tramp_start));
    serial_puts(" bytes, startup vector ");
    serial_put_hex32((uint32_t)(smp_tramp_phys >> 12));
    serial_puts("\n");

    /* One worker per physical core, skipping this one and every
     * hyperthread sibling. */
    for (int i = 0; i < acpi.cpu_count && smp_cpu_count < SMP_MAX_CPUS; i++) {
        const acpi_cpu_t *c = &acpi.cpu[i];
        if (!c->enabled && !c->online_capable) continue;
        if (c->apic_id == smp_cpu[0].apic_id) continue;
        if (c->is_hyperthread) continue;

        int idx = smp_cpu_count;
        void *stack = kstack_alloc(SMP_AP_STACK);
        void *emerg = kstack_alloc(SMP_AP_EMERG);
        if (!stack || !emerg) {
            if (stack) kstack_free(stack, SMP_AP_STACK);
            if (emerg) kstack_free(emerg, SMP_AP_EMERG);
            serial_puts("[smp] out of kernel stacks; stopping here\n");
            break;
        }

        smp_cpu[idx].apic_id        = c->apic_id;
        smp_cpu[idx].is_hyperthread = c->is_hyperthread;
        smp_cpu[idx].online         = 0;
        smp_cpu[idx].stack_top      =
            ((uint64_t)(uintptr_t)stack + SMP_AP_STACK) & ~15ULL;
        smp_cpu[idx].emerg_top      =
            ((uint64_t)(uintptr_t)emerg + SMP_AP_EMERG) & ~15ULL;
        smp_cpu_count++;

        int rc = smp_start_one(idx);
        serial_puts("[smp]   APIC ");
        serial_put_dec(c->apic_id);
        if (rc > 0) {
            serial_puts(" online\n");
        } else if (rc == 0) {
            /* It has the trampoline and its own stack; it simply took
             * longer than the deadline to finish. It will join the pool
             * when it does, and until then it is skipped by everything
             * that tests `online`. */
            serial_puts(" started, still initialising\n");
        } else {
            serial_puts(" did not answer; not starting any more\n");
            break;
        }
    }

    /* Nothing runs from the low page again, and leaving it mapped would
     * leave a writable, executable page at a physical address anything
     * can guess. */
    {
        uint64_t *pte = vmm_walk(&vmm_kernel_as, smp_tramp_phys, 0);
        if (pte) { *pte = 0; flush_tlb_page(smp_tramp_phys); }
    }

    if (smp_online > 1) smp_status = "worker pool running";
    else                smp_status = "no processor answered the startup";

    serial_puts("[smp] ");
    serial_put_dec((uint32_t)smp_online);
    serial_puts(" of ");
    serial_put_dec((uint32_t)acpi.core_count);
    serial_puts(" cores running (");
    serial_put_dec((uint32_t)(smp_online - 1));
    serial_puts(" kernel workers, core 0 reserved for the desktop)\n");
}

/* ===================================================================
 * 6. RUNNING SOMETHING ON ALL OF THEM
 * ===================================================================
 *
 * The whole point of the file, in one call.
 *
 * `items` independent units of work, `fn` applied to a half-open range
 * of them. The caller's loop body must be safe to run on a processor
 * that holds no locks and owns no address space: read what the submitter
 * owns, write only within its own range, allocate nothing, and block on
 * nothing. Everything the inference path hands over satisfies that by
 * construction, and anything that did not would be a bug that showed up
 * as corruption rather than as a deadlock.
 *
 * On a machine with one processor this is a plain loop, executed inline,
 * with no atomics and no waiting -- which is not a fallback so much as
 * the same program with one participant.
 */
static void smp_parallel_for(smp_work_fn fn, void *ctx, uint32_t items,
                             uint32_t min_chunk) {
    if (!fn || !items) return;

    if (smp_online <= 1) {
        fn(ctx, 0, items);
        return;
    }

    /*
     * One job may be in flight at a time, and a second caller does the
     * work itself rather than waiting for the first.
     *
     * A lock would be the obvious thing and it is the wrong thing twice
     * over. Waiting for a job that runs for seconds -- and a matrix
     * multiplication over a language model's weights does -- means the
     * second caller is blocked for seconds; and spin_lock_irq waits with
     * interrupts disabled, so on the compositor's thread that would stop
     * the frame clock for the duration and freeze the desktop. Running
     * the range inline is slower for that one caller and correct for
     * everybody, which is the right way round.
     */
    if (__atomic_exchange_n(&smp_job_busy, 1, __ATOMIC_ACQ_REL)) {
        fn(ctx, 0, items);
        return;
    }

    /*
     * Enough chunks that a processor which finishes early has something
     * else to take, and few enough that claiming them is not the work.
     * Four per processor is the usual answer to that trade and it is the
     * one used here; min_chunk lets a caller whose unit of work is very
     * small raise the floor.
     */
    uint32_t want = (uint32_t)(smp_online * 4);
    uint32_t chunk = (items + want - 1) / want;
    if (chunk < min_chunk) chunk = min_chunk;
    if (chunk == 0) chunk = 1;

    smp_job.fn      = fn;
    smp_job.ctx     = ctx;
    smp_job.items   = items;
    smp_job.chunk   = chunk;
    smp_job.chunks  = (items + chunk - 1) / chunk;
    smp_job.claimed = 0;
    smp_job.retired = 0;

    /* Published last, and with a release, so that a worker which sees
     * the new epoch is guaranteed to see every field above it. */
    __atomic_add_fetch(&smp_job.epoch, 1u, __ATOMIC_RELEASE);

    /*
     * Every worker, not only the ones that look parked.
     *
     * Testing `parked` first and skipping the ones that are busy is the
     * obvious saving and it loses wakeups. A worker checks the epoch,
     * finds nothing, and *then* halts; a job posted between those two
     * steps is seen by neither -- the worker's check was too early and
     * the submitter's read of `parked` was too. Sending unconditionally
     * closes the window, and an interrupt delivered to a processor that
     * is already working costs it an acknowledgement and nothing else.
     */
    for (int i = 1; i < smp_cpu_count; i++)
        if (smp_cpu[i].online) lapic_send_wake(smp_cpu[i].apic_id);

    /* The submitter is a worker too. */
    smp_job_drain(0);

    /*
     * Wait for the stragglers.
     *
     * Bounded, and the bound is not decoration: a worker that faulted
     * inside a chunk never retires it, and an unbounded wait here would
     * take the desktop down with it. Falling out early means the results
     * for some range are missing rather than wrong, which the caller
     * cannot distinguish -- so it is reported, loudly, once.
     */
    uint64_t start = cycle_now();
    uint64_t limit = cycles_per_ms ? cycles_per_ms * 5000u : 0;
    while (__atomic_load_n(&smp_job.retired, __ATOMIC_ACQUIRE)
           < smp_job.chunks) {
        __asm__ volatile("pause");
        if (limit && cycle_now() - start > limit) {
            static int said = 0;
            if (!said) {
                said = 1;
                serial_puts("[smp] a worker did not retire its chunk; "
                            "results may be incomplete\n");
            }
            break;
        }
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __asm__ volatile("" ::: "memory");

    /* Release the submission slot for the next job. */
    __atomic_store_n(&smp_job_busy, 0, __ATOMIC_RELEASE);
}

/* How many processors are carrying work, for the system monitor. */
static int smp_worker_count(void) { return smp_online > 1 ? smp_online - 1 : 0; }

/* ===================================================================
 * 7. PROVING IT
 * ===================================================================
 *
 * A worker pool that comes up and reports itself online has proved
 * exactly one thing: that a processor reached the end of its own
 * initialisation. It has not proved that a job reaches it, that the
 * chunk it claims is a chunk no other processor also claimed, that the
 * result is visible to the submitter afterwards, or that the barrier
 * actually waits. Every one of those can be wrong in a way that shows up
 * later as a wrong answer from the language model, which is the single
 * hardest place in this system to notice a wrong answer.
 *
 * So the pool is checked the way the blitter is checked: by giving it
 * work whose result is known, and reading back every element.
 *
 * Each element is written with a value derived from its own index, and
 * with the *sequence number of the write*, so the test distinguishes the
 * three failures that matter and would otherwise look alike:
 *
 *   an element still holding its initial value   never dispatched
 *   an element holding the wrong index           a chunk ran over its
 *                                                neighbour's range
 *   an element written twice                     two processors claimed
 *                                                the same chunk
 *
 * The array is deliberately larger than the chunk count so that some
 * processors take several chunks and the claim cursor is genuinely
 * contended, and it is checked *after* the barrier returns, which is
 * what makes a barrier that returns early a failure here rather than a
 * corruption somewhere else.
 */
#define SMP_TEST_N 4096

static uint32_t smp_test_val[SMP_TEST_N];
static uint32_t smp_test_hits[SMP_TEST_N];

/*
 * Enough arithmetic per element that the job outlives an interrupt.
 *
 * This began as a single store per element and the test passed while
 * proving less than it looked like it did: the submitting processor
 * drained all sixteen chunks before the wake messages had even been
 * taken, so the pool was verified to be *correct* without a single
 * chunk having run anywhere but processor zero. A job that finishes
 * before its workers wake up is not a test of the workers.
 *
 * Sixty-four rounds of mixing per element is still under a millisecond
 * of real work in total, and it is enough that every processor arrives
 * and takes a share -- which is the property that the distribution
 * printed at the end is there to show. It is also the honest shape of
 * the only caller this pool has: a chunk of matrix rows is thousands of
 * multiply-adds, not a store.
 */
static uint32_t smp_test_mix(uint32_t x) {
    for (int r = 0; r < 64; r++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x;
}

static void smp_test_fn(void *ctx, uint32_t first, uint32_t last) {
    (void)ctx;
    for (uint32_t i = first; i < last; i++) {
        /* A value only this index can produce, and one that a truncated
         * or overlapping range cannot accidentally reproduce. */
        smp_test_val[i] = smp_test_mix(i * 2654435761u + 0x9E3779B9u);
        __atomic_add_fetch(&smp_test_hits[i], 1u, __ATOMIC_RELAXED);
    }
}

static void smp_selftest(void) {
    if (smp_online <= 1) return;

    for (uint32_t i = 0; i < SMP_TEST_N; i++) {
        smp_test_val[i]  = 0;
        smp_test_hits[i] = 0;
    }

    uint64_t before[SMP_MAX_CPUS];
    for (int i = 0; i < SMP_MAX_CPUS; i++) before[i] = smp_cpu[i].chunks;

    smp_parallel_for(smp_test_fn, 0, SMP_TEST_N, 1);

    uint32_t missing = 0, wrong = 0, twice = 0;
    for (uint32_t i = 0; i < SMP_TEST_N; i++) {
        if (smp_test_hits[i] == 0)      { missing++; continue; }
        if (smp_test_hits[i] > 1)       twice++;
        if (smp_test_val[i] !=
            smp_test_mix(i * 2654435761u + 0x9E3779B9u)) wrong++;
    }

    if (missing || wrong || twice) {
        serial_puts("[smp] SELFTEST FAILED: ");
        serial_put_dec(missing); serial_puts(" never written, ");
        serial_put_dec(wrong);   serial_puts(" wrong, ");
        serial_put_dec(twice);   serial_puts(" written twice\n");
        return;
    }

    serial_puts("[smp] selftest passed: ");
    serial_put_dec(SMP_TEST_N);
    serial_puts(" elements, one write each, across");
    for (int i = 0; i < smp_cpu_count; i++) {
        uint64_t took = smp_cpu[i].chunks - before[i];
        if (!took && i) continue;
        serial_puts(" cpu");
        serial_put_dec((uint32_t)i);
        serial_puts(":");
        serial_put_dec((uint32_t)took);
    }
    serial_puts(" chunks\n");
}

#endif /* SMP_H */
